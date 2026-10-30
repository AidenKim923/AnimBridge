"""Read the ModelImporter settings of an FBX's .meta file."""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .unity_yaml import load_plain, to_float


@dataclass
class ClipInfo:
    name: str
    take_name: str
    first_frame: float
    last_frame: float
    loop: bool
    mirror: bool
    events: list[dict] = field(default_factory=list)  # time is normalized 0..1 in the .meta


@dataclass
class ModelMeta:
    path: str
    global_scale: float = 1.0
    use_file_scale: bool = True
    bake_axis_conversion: bool = False
    animation_type: int = 2  # 1 Legacy, 2 Generic, 3 Humanoid
    import_animation: bool = True
    optimize_game_objects: bool = False
    clips: list[ClipInfo] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


def meta_path_for(asset: Path) -> Path:
    return Path(str(asset) + ".meta")


def _find(d: Any, key: str):
    """Depth-first search for a key anywhere in nested dicts/lists."""
    if isinstance(d, dict):
        if key in d:
            return d[key]
        for v in d.values():
            r = _find(v, key)
            if r is not None:
                return r
    elif isinstance(d, list):
        for v in d:
            r = _find(v, key)
            if r is not None:
                return r
    return None


def read_model_meta(path: Path) -> ModelMeta:
    data = load_plain(path) or {}
    imp = data.get("ModelImporter")
    if not isinstance(imp, dict):
        raise ValueError(f"{path}: not a ModelImporter .meta file")
    m = ModelMeta(path=str(path))

    def num(key, default):
        v = _find(imp, key)
        return default if v is None else to_float(v)

    m.global_scale = num("globalScale", 1.0)
    m.use_file_scale = bool(num("useFileScale", 1))
    m.bake_axis_conversion = bool(num("bakeAxisConversion", 0))
    m.animation_type = int(num("animationType", 2))
    m.import_animation = bool(num("importAnimation", 1))
    m.optimize_game_objects = bool(num("optimizeGameObjects", 0))
    if m.global_scale <= 0:
        raise ValueError(f"{path}: globalScale must be > 0")
    if m.bake_axis_conversion:
        m.warnings.append("bakeAxisConversion is on; analytic conversion may be wrong, use --unity-rest")

    anims = imp.get("animations") or {}
    for c in anims.get("clipAnimations") or []:
        info = ClipInfo(
            name=str(c.get("name") or c.get("takeName") or "Clip"),
            take_name=str(c.get("takeName") or ""),
            first_frame=to_float(c.get("firstFrame", 0)),
            last_frame=to_float(c.get("lastFrame", 0)),
            loop=bool(int(c.get("loopTime", 0) or 0)),
            mirror=bool(int(c.get("mirror", 0) or 0)),
        )
        for ev in c.get("events") or []:
            info.events.append(
                {
                    "time": to_float(ev.get("time", 0)),
                    "name": str(ev.get("functionName") or ""),
                    "string": str(ev.get("data") or ""),
                    "float": to_float(ev.get("floatParameter", 0)),
                    "int": int(ev.get("intParameter", 0) or 0),
                }
            )
        if info.mirror:
            m.warnings.append(f"clip {info.name!r}: mirror is not applied (v1)")
        if info.last_frame < info.first_frame:
            m.warnings.append(f"clip {info.name!r}: lastFrame < firstFrame; skipped")
            continue
        m.clips.append(info)
    if m.animation_type == 3:
        m.warnings.append("model is Humanoid; path C exports the source FBX motion, not Unity's retargeted result")
    return m


def unit_k(meta: ModelMeta | None, fbx_unit_scale_factor: float) -> float:
    """Unity position scale k: Unity units per FBX unit."""
    if meta is None:
        return fbx_unit_scale_factor / 100.0
    file_scale = fbx_unit_scale_factor / 100.0 if meta.use_file_scale else 1.0
    return file_scale * meta.global_scale
