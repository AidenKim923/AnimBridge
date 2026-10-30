"""Bake an AnimClip into per-frame local TRS values (still in Unity space)."""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .anim_parser import AnimClip


@dataclass
class Track:
    # NaN marks a component that has no curve (filled from the rest pose later).
    t: np.ndarray | None = None  # [N,3]
    r: np.ndarray | None = None  # [N,4] x,y,z,w
    s: np.ndarray | None = None  # [N,3]


@dataclass
class SampledClip:
    name: str
    fps: float
    start: float
    duration: float
    frames: int
    loop: bool
    tracks: dict[str, Track] = field(default_factory=dict)
    times: np.ndarray | None = None  # output times starting at 0
    events: list[dict] = field(default_factory=list)
    curves: dict[str, np.ndarray] = field(default_factory=dict)  # name -> [N,2] (time, value)
    warnings: list[str] = field(default_factory=list)

    def to_json(self) -> dict:
        return {
            "name": self.name,
            "fps": self.fps,
            "frames": self.frames,
            "duration": self.duration,
            "loop": self.loop,
            "times": None if self.times is None else self.times.tolist(),
            "events": self.events,
            "tracks": {p: {"t": arr_json(tr.t), "r": arr_json(tr.r), "s": arr_json(tr.s)} for p, tr in self.tracks.items()},
        }


def arr_json(a):
    if a is None:
        return None
    return [[None if np.isnan(v) else float(v) for v in row] for row in a]


ROTATION_ORDERS = {0: "XYZ", 1: "XZY", 2: "YZX", 3: "YXZ", 4: "ZXY", 5: "ZYX"}


def euler_to_quat_unity(deg: np.ndarray, order: int = 4) -> np.ndarray:
    """Unity euler (degrees) -> quaternion x,y,z,w.

    The order name lists the order the axes are applied in; ZXY (default, = Quaternion.Euler)
    applies Z first, so q = qY * qX * qZ.
    """
    names = ROTATION_ORDERS.get(order, "ZXY")
    h = np.radians(deg) * 0.5
    z0 = np.zeros(len(deg))
    axis_q = {
        "X": np.stack([np.sin(h[:, 0]), z0, z0, np.cos(h[:, 0])], 1),
        "Y": np.stack([z0, np.sin(h[:, 1]), z0, np.cos(h[:, 1])], 1),
        "Z": np.stack([z0, z0, np.sin(h[:, 2]), np.cos(h[:, 2])], 1),
    }
    first, second, third = (axis_q[a] for a in names)
    return quat_mul(quat_mul(third, second), first)


def quat_mul(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    ax, ay, az, aw = a[:, 0], a[:, 1], a[:, 2], a[:, 3]
    bx, by, bz, bw = b[:, 0], b[:, 1], b[:, 2], b[:, 3]
    return np.stack(
        [
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz,
        ],
        1,
    )


def make_quats_continuous(q: np.ndarray) -> np.ndarray:
    q = q.copy()
    for i in range(1, len(q)):
        if np.dot(q[i], q[i - 1]) < 0:
            q[i] = -q[i]
    return q


def sample_clip(clip: AnimClip, fps: float | None = None) -> SampledClip:
    warnings: list[str] = []
    rate = fps if fps and fps > 0 else clip.sample_rate
    if not rate or not (1 <= rate <= 1000):
        warnings.append(f"sample rate {rate} out of range 1..1000; using 30")
        rate = 30.0
    start, stop = clip.start_time, clip.stop_time
    key_range = clip.key_range()
    if start is None or stop is None or (start == 0 and stop == 0):
        if key_range is None:
            start, stop = 0.0, 0.0
        else:
            start, stop = key_range
    elif stop < start:
        warnings.append(f"m_StopTime ({stop}) < m_StartTime ({start}); using key range")
        start, stop = key_range if key_range else (0.0, 0.0)
    times, out_duration = time_grid(start, stop, rate, warnings)
    n = len(times)

    out = SampledClip(clip.name, rate, start, out_duration, n, clip.loop, warnings=warnings)
    out.times = times - start
    for ev in clip.events:
        t = ev.time - start
        if t < -1e-6 or t > (stop - start) + 1e-6:
            warnings.append(f"event {ev.name!r} at {ev.time}s is outside the clip range; dropped")
            continue
        if t > out_duration + 1e-9:
            warnings.append(f"event {ev.name!r} at {t:.4f}s moved to the output end {out_duration:.4f}s")
            t = out_duration
        out.events.append({"time": max(0.0, t), "name": ev.name, "string": ev.string, "float": ev.float, "int": ev.int})
    for ft in clip.float_tracks:
        key = ft.attribute if not ft.path else f"{ft.path}:{ft.attribute}"
        out.curves[key] = np.stack([out.times, ft.curve.evaluate(times)], 1)
    for path, chans in clip.channels.items():
        tr = Track()
        if "pos" in chans:
            tr.t = _eval_components(chans["pos"], times)
        if "scale" in chans:
            tr.s = _eval_components(chans["scale"], times)
        if "rot" in chans:
            comps = chans["rot"]
            if any(c is None for c in comps):
                warnings.append(f"{path!r}: quaternion curve missing components; rotation dropped")
            else:
                tr.r = _normalize_quats(_eval_components(comps, times), path, warnings)
        elif "euler" in chans:
            comps = chans["euler"]
            if any(c is None for c in comps):
                warnings.append(f"{path!r}: euler curve missing components; rotation dropped")
            else:
                order = clip.rotation_order.get(path, 4)
                tr.r = make_quats_continuous(euler_to_quat_unity(_eval_components(comps, times), order))
        if tr.t is not None or tr.r is not None or tr.s is not None:
            out.tracks[path] = tr
    return out


def time_grid(start: float, stop: float, fps: float, warnings: list[str]) -> tuple[np.ndarray, float]:
    """Return absolute sample times on a uniform grid and the output duration."""
    d = max(0.0, stop - start)
    if d == 0:
        return np.array([start]), 0.0
    n = max(2, int(round(d * fps)) + 1)
    times = start + np.minimum(np.arange(n) / fps, d)
    out_d = (n - 1) / fps
    if abs(out_d - d) > 1e-4:
        warnings.append(f"clip length {d:.6f}s is not a whole number of frames at {fps} fps; output length {out_d:.6f}s")
    return times, out_d


def _eval_components(comps, times: np.ndarray) -> np.ndarray:
    res = np.full((len(times), len(comps)), np.nan)
    for i, c in enumerate(comps):
        if c is not None:
            res[:, i] = c.evaluate(times)
    return res


def _normalize_quats(q: np.ndarray, path: str, warnings: list[str]) -> np.ndarray:
    norms = np.linalg.norm(q, axis=1)
    bad = norms < 1e-8
    if bad.any():
        warnings.append(f"{path!r}: {int(bad.sum())} zero-length quaternion sample(s); held previous value")
    if bad.all():
        warnings.append(f"{path!r}: no valid quaternion samples; rotation dropped")
        return None
    q = q / np.where(bad, 1.0, norms)[:, None]
    first_good = int(np.flatnonzero(~bad)[0])
    for i in np.flatnonzero(bad):
        q[i] = q[i - 1] if i > 0 else q[first_good]
    return make_quats_continuous(q)
