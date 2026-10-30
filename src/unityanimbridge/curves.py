"""Evaluate scalar Unity AnimationCurves, including weighted tangents and wrap modes."""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

# Unity's serialized m_PreInfinity / m_PostInfinity (internal wrap mode).
WRAP_PINGPONG = 0
WRAP_LOOP = 1
WRAP_CLAMP = 2

WEIGHT_IN = 1
WEIGHT_OUT = 2


@dataclass
class Key:
    time: float
    value: float
    in_slope: float = 0.0
    out_slope: float = 0.0
    weighted_mode: int = 0
    in_weight: float = 1.0 / 3.0
    out_weight: float = 1.0 / 3.0


@dataclass
class Curve:
    keys: list[Key]
    pre_infinity: int = WRAP_CLAMP
    post_infinity: int = WRAP_CLAMP
    warnings: list[str] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.keys = _clean_keys(self.keys, self.warnings)

    @property
    def empty(self) -> bool:
        return not self.keys

    @property
    def start(self) -> float:
        return self.keys[0].time

    @property
    def end(self) -> float:
        return self.keys[-1].time

    def evaluate(self, times) -> np.ndarray:
        t = np.atleast_1d(np.asarray(times, dtype=np.float64))
        if not self.keys:
            raise ValueError("cannot evaluate an empty curve")
        if len(self.keys) == 1:
            return np.full(t.shape, self.keys[0].value)
        t = self._wrap(t)
        times_arr = np.array([k.time for k in self.keys])
        # Find i where keys[i].time <= t < keys[i + 1].time.
        idx = np.clip(np.searchsorted(times_arr, t, side="right") - 1, 0, len(self.keys) - 2)
        out = np.empty(t.shape)
        for seg in np.unique(idx):
            m = idx == seg
            out[m] = _eval_segment(self.keys[seg], self.keys[seg + 1], t[m])
        return out

    def _wrap(self, t: np.ndarray) -> np.ndarray:
        start, end = self.start, self.end
        length = end - start
        t = t.copy()
        if length <= 0:
            return np.full(t.shape, start)
        before = t < start
        after = t > end
        if before.any():
            t[before] = _wrap_one(t[before], start, length, self.pre_infinity)
        if after.any():
            t[after] = _wrap_one(t[after], start, length, self.post_infinity)
        return t


def _wrap_one(t: np.ndarray, start: float, length: float, mode: int) -> np.ndarray:
    if mode == WRAP_LOOP:
        return start + np.mod(t - start, length)
    if mode == WRAP_PINGPONG:
        p = np.mod(t - start, 2 * length)
        return start + np.where(p > length, 2 * length - p, p)
    return np.clip(t, start, start + length)


def _eval_segment(k0: Key, k1: Key, t: np.ndarray) -> np.ndarray:
    dt = k1.time - k0.time
    if dt <= 0:
        return np.full(t.shape, k1.value)
    if math.isinf(k0.out_slope) or math.isinf(k1.in_slope):
        # Stepped tangent: hold the left value until the next key.
        return np.where(t >= k1.time, k1.value, k0.value)
    w_out = bool(k0.weighted_mode & WEIGHT_OUT)
    w_in = bool(k1.weighted_mode & WEIGHT_IN)
    if not (w_out or w_in):
        s = (t - k0.time) / dt
        s2 = s * s
        s3 = s2 * s
        h00 = 2 * s3 - 3 * s2 + 1
        h10 = s3 - 2 * s2 + s
        h01 = -2 * s3 + 3 * s2
        h11 = s3 - s2
        return h00 * k0.value + h10 * dt * k0.out_slope + h01 * k1.value + h11 * dt * k1.in_slope
    w0 = k0.out_weight if w_out else 1.0 / 3.0
    w1 = k1.in_weight if w_in else 1.0 / 3.0
    return _bezier(k0, k1, w0, w1, t)


def _bezier(k0: Key, k1: Key, w0: float, w1: float, t: np.ndarray) -> np.ndarray:
    dt = k1.time - k0.time
    x1, x2 = w0, 1.0 - w1
    y0, y3 = k0.value, k1.value
    y1 = k0.value + k0.out_slope * w0 * dt
    y2 = k1.value - k1.in_slope * w1 * dt
    target = (t - k0.time) / dt

    def bx(u):
        v = 1 - u
        return 3 * v * v * u * x1 + 3 * v * u * u * x2 + u * u * u

    def dbx(u):
        v = 1 - u
        return 3 * v * v * x1 + 6 * v * u * (x2 - x1) + 3 * u * u * (1 - x2)

    # Bisection bracket + Newton refinement on x(u) = target.
    lo = np.zeros_like(target)
    hi = np.ones_like(target)
    u = target.copy()
    for _ in range(40):
        x = bx(u) - target
        if np.all(np.abs(x) < 1e-10):
            break
        d = dbx(u)
        lo = np.where(x < 0, u, lo)
        hi = np.where(x >= 0, u, hi)
        safe_d = np.where(np.abs(d) > 1e-12, d, 1.0)
        newton = np.where(np.abs(d) > 1e-12, u - x / safe_d, np.nan)
        ok = np.isfinite(newton) & (newton > lo) & (newton < hi)
        u = np.where(ok, newton, 0.5 * (lo + hi))
    v = 1 - u
    return v * v * v * y0 + 3 * v * v * u * y1 + 3 * v * u * u * y2 + u * u * u * y3


def _clean_keys(keys: list[Key], warnings: list[str]) -> list[Key]:
    good = []
    for k in keys:
        if not (math.isfinite(k.time) and math.isfinite(k.value)):
            warnings.append(f"dropped key with non-finite time/value ({k.time}, {k.value})")
            continue
        if math.isnan(k.in_slope) or math.isnan(k.out_slope):
            warnings.append(f"NaN tangent at t={k.time}; treated as 0")
            k.in_slope = 0.0 if math.isnan(k.in_slope) else k.in_slope
            k.out_slope = 0.0 if math.isnan(k.out_slope) else k.out_slope
        k.in_weight = min(1.0, max(0.0, k.in_weight))
        k.out_weight = min(1.0, max(0.0, k.out_weight))
        good.append(k)
    if any(good[i].time > good[i + 1].time for i in range(len(good) - 1)):
        warnings.append("keys were not in time order; sorted")
    good = sorted(good, key=lambda k: k.time)
    dedup: list[Key] = []
    for k in good:
        if dedup and dedup[-1].time == k.time:
            warnings.append(f"duplicate key time {k.time}; kept the later key")
            dedup[-1] = k
        else:
            dedup.append(k)
    return dedup
