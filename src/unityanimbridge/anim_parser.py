"""Parse a text-serialized Unity AnimationClip (.anim) into channel curves."""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .curves import WRAP_CLAMP, Curve, Key
from .unity_yaml import UnityYamlError, load_documents, to_float

ANIMATION_CLIP_CLASS_ID = 74
TRANSFORM_CLASS_ID = 4
ANIMATOR_CLASS_ID = 95

CHANNELS = {"pos": 3, "rot": 4, "euler": 3, "scale": 3}

_EDITOR_ATTR = {
    "m_LocalPosition": ("pos", "xyz"),
    "m_LocalRotation": ("rot", "xyzw"),
    "m_LocalScale": ("scale", "xyz"),
    "localEulerAnglesRaw": ("euler", "xyz"),
    "localEulerAngles": ("euler", "xyz"),
    "localEulerAnglesBaked": ("euler", "xyz"),
}

_HUMANOID_GOALS = re.compile(r"^(Root|Motion|LeftFoot|RightFoot|LeftHand|RightHand)[TQ]\.[xyzw]$")
_HUMANOID_MUSCLE = re.compile(
    r"^((Left|Right) )?(Spine|Chest|UpperChest|Neck|Head|Eye|Jaw|Upper Leg|Lower Leg|Foot|Toes|"
    r"Shoulder|Arm|Forearm|Hand) .+$"
    r"|^(Left|Right)Hand\.(Thumb|Index|Middle|Ring|Little)\..+$"
)


class AnimParseError(Exception):
    pass


@dataclass
class AnimEvent:
    time: float
    name: str
    string: str = ""
    float: float = 0.0
    int: int = 0


@dataclass
class ScalarTrack:
    path: str
    attribute: str
    class_id: int
    curve: Curve


@dataclass
class AnimClip:
    name: str
    source: str
    sample_rate: float
    start_time: float | None
    stop_time: float | None
    loop: bool
    legacy: bool
    mirror: bool
    # path -> channel -> list of per-component Curve or None
    channels: dict[str, dict[str, list[Curve | None]]] = field(default_factory=dict)
    float_tracks: list[ScalarTrack] = field(default_factory=list)
    humanoid_attributes: list[str] = field(default_factory=list)
    events: list[AnimEvent] = field(default_factory=list)
    # path -> Unity RotationOrder (0 XYZ, 1 XZY, 2 YZX, 3 YXZ, 4 ZXY, 5 ZYX) for euler channels
    rotation_order: dict[str, int] = field(default_factory=dict)
    unsupported: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    @property
    def is_humanoid(self) -> bool:
        return bool(self.humanoid_attributes)

    def key_range(self) -> tuple[float, float] | None:
        lo = hi = None
        for chans in self.channels.values():
            for comps in chans.values():
                for c in comps:
                    if c is None or c.empty:
                        continue
                    lo = c.start if lo is None else min(lo, c.start)
                    hi = c.end if hi is None else max(hi, c.end)
        return None if lo is None else (lo, hi)


def is_humanoid_attribute(attr: str) -> bool:
    return bool(_HUMANOID_GOALS.match(attr) or _HUMANOID_MUSCLE.match(attr))


def parse_anim(path: Path) -> AnimClip:
    path = Path(path)
    try:
        docs = load_documents(path)
    except UnityYamlError as e:
        raise AnimParseError(str(e)) from e
    clip_doc = next(
        (d for d in docs if d.class_id == ANIMATION_CLIP_CLASS_ID or d.type_name == "AnimationClip"), None
    )
    if clip_doc is None or not isinstance(clip_doc.body, dict):
        raise AnimParseError(f"{path}: no AnimationClip document found")
    return parse_clip_body(clip_doc.body, str(path))


def parse_clip_body(body: dict[str, Any], source: str) -> AnimClip:
    settings = body.get("m_AnimationClipSettings") or {}
    start = settings.get("m_StartTime")
    stop = settings.get("m_StopTime")
    clip = AnimClip(
        name=str(body.get("m_Name") or Path(source).stem),
        source=source,
        sample_rate=to_float(body.get("m_SampleRate", 60)),
        start_time=None if start is None else to_float(start),
        stop_time=None if stop is None else to_float(stop),
        loop=bool(int(settings.get("m_LoopTime", 0) or 0)),
        legacy=bool(int(body.get("m_Legacy", 0) or 0)),
        mirror=bool(int(settings.get("m_Mirror", 0) or 0)),
    )
    if clip.mirror:
        clip.warnings.append("clip has Mirror enabled; mirroring is not applied (v1)")
    if int(settings.get("m_HasAdditiveReferencePose", 0) or 0):
        clip.warnings.append("clip has an additive reference pose; it is ignored (v1)")

    primary_found: set[tuple[str, str]] = set()
    for key, chan in (
        ("m_PositionCurves", "pos"),
        ("m_RotationCurves", "rot"),
        ("m_EulerCurves", "euler"),
        ("m_ScaleCurves", "scale"),
    ):
        for entry in body.get(key) or []:
            p = str(entry.get("path") or "")
            curves = _vector_curves(entry.get("curve") or {}, CHANNELS[chan], clip.warnings, f"{p}:{chan}")
            if curves is None:
                continue
            clip.channels.setdefault(p, {})[chan] = curves
            primary_found.add((p, chan))
            if chan == "euler":
                order = int((entry.get("curve") or {}).get("m_RotationOrder", 4) or 0)
                if order not in range(6):
                    clip.warnings.append(f"{p!r}: unknown m_RotationOrder {order}; using ZXY")
                    order = 4
                clip.rotation_order[p] = order

    for entry in body.get("m_FloatCurves") or []:
        _add_scalar(clip, entry, primary_found)
    for entry in body.get("m_EditorCurves") or []:
        _add_scalar(clip, entry, primary_found)

    for p, chans in clip.channels.items():
        if "rot" in chans and "euler" in chans:
            clip.warnings.append(f"{p!r}: both quaternion and euler rotation curves; using quaternion")
            del chans["euler"]

    if body.get("m_PPtrCurves"):
        clip.unsupported.append(f"{len(body['m_PPtrCurves'])} object-reference (PPtr) curve(s)")

    for ev in body.get("m_Events") or []:
        clip.events.append(
            AnimEvent(
                time=to_float(ev.get("time", 0)),
                name=str(ev.get("functionName") or ""),
                string=str(ev.get("data") or ""),
                float=to_float(ev.get("floatParameter", 0)),
                int=int(ev.get("intParameter", 0) or 0),
            )
        )
    clip.humanoid_attributes = sorted(set(clip.humanoid_attributes))
    clip.unsupported = sorted(set(clip.unsupported))
    return clip


def _add_scalar(clip: AnimClip, entry: dict, primary_found: set) -> None:
    attr = str(entry.get("attribute") or "")
    p = str(entry.get("path") or "")
    class_id = int(entry.get("classID", 0) or 0)
    if class_id == ANIMATOR_CLASS_ID and is_humanoid_attribute(attr):
        clip.humanoid_attributes.append(attr)
        return
    if class_id == TRANSFORM_CLASS_ID and "." in attr:
        prop, comp = attr.rsplit(".", 1)
        mapping = _EDITOR_ATTR.get(prop)
        if mapping is None or comp not in mapping[1]:
            clip.unsupported.append(f"{p!r}: Transform attribute {attr}")
            return
        chan, comps = mapping
        if (p, chan) in primary_found or (chan == "euler" and (p, "rot") in primary_found):
            return  # already provided by the vector curve lists
        curve = _scalar_curve(entry.get("curve") or {}, clip.warnings, f"{p}:{attr}")
        if curve is None:
            return
        slot = clip.channels.setdefault(p, {}).setdefault(chan, [None] * CHANNELS[chan])
        slot[comps.index(comp)] = curve
        return
    if any(t.path == p and t.attribute == attr and t.class_id == class_id for t in clip.float_tracks):
        return  # m_EditorCurves duplicates m_FloatCurves
    curve = _scalar_curve(entry.get("curve") or {}, clip.warnings, f"{p}:{attr}")
    if curve is None:
        return
    clip.float_tracks.append(ScalarTrack(p, attr, class_id, curve))
    if class_id != ANIMATOR_CLASS_ID:
        clip.unsupported.append(f"{p!r}: classID {class_id} attribute {attr} (kept in sidecar only)")


def _wrap_modes(curve_body: dict) -> tuple[int, int]:
    pre = curve_body.get("m_PreInfinity", WRAP_CLAMP)
    post = curve_body.get("m_PostInfinity", WRAP_CLAMP)
    return int(WRAP_CLAMP if pre is None else pre), int(WRAP_CLAMP if post is None else post)


def _vector_curves(curve_body: dict, n: int, warnings: list[str], label: str) -> list[Curve | None] | None:
    raw = curve_body.get("m_Curve") or []
    if not raw:
        warnings.append(f"{label}: curve has no keys; ignored")
        return None
    comps = "xyzw"[:n]
    pre, post = _wrap_modes(curve_body)
    per: list[list[Key]] = [[] for _ in range(n)]
    for k in raw:
        t = to_float(k.get("time", 0))
        val, ins, outs = k.get("value") or {}, k.get("inSlope") or {}, k.get("outSlope") or {}
        inw, outw = k.get("inWeight") or {}, k.get("outWeight") or {}
        wm = int(k.get("weightedMode", 0) or 0)
        for i, c in enumerate(comps):
            per[i].append(
                Key(
                    time=t,
                    value=to_float(val.get(c, 0)),
                    in_slope=to_float(ins.get(c, 0)),
                    out_slope=to_float(outs.get(c, 0)),
                    weighted_mode=wm,
                    in_weight=to_float(inw.get(c, 1 / 3)),
                    out_weight=to_float(outw.get(c, 1 / 3)),
                )
            )
    out: list[Curve | None] = []
    for i in range(n):
        cv = Curve(per[i], pre, post)
        warnings.extend(f"{label}.{comps[i]}: {w}" for w in cv.warnings)
        out.append(None if cv.empty else cv)
    return out


def _scalar_curve(curve_body: dict, warnings: list[str], label: str) -> Curve | None:
    raw = curve_body.get("m_Curve") or []
    if not raw:
        warnings.append(f"{label}: curve has no keys; ignored")
        return None
    pre, post = _wrap_modes(curve_body)
    keys = [
        Key(
            time=to_float(k.get("time", 0)),
            value=to_float(k.get("value", 0)),
            in_slope=to_float(k.get("inSlope", 0)),
            out_slope=to_float(k.get("outSlope", 0)),
            weighted_mode=int(k.get("weightedMode", 0) or 0),
            in_weight=to_float(k.get("inWeight", 1 / 3)),
            out_weight=to_float(k.get("outWeight", 1 / 3)),
        )
        for k in raw
    ]
    cv = Curve(keys, pre, post)
    warnings.extend(f"{label}: {w}" for w in cv.warnings)
    return None if cv.empty else cv
