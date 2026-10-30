"""UnityAnimBridge: Unity animation to Unreal FBX converter."""
from importlib import metadata as _metadata
from pathlib import Path as _Path

try:
    __version__ = _metadata.version("unityanimbridge")
except _metadata.PackageNotFoundError:
    try:
        __version__ = (_Path(__file__).resolve().parents[2] / "VERSION").read_text(encoding="utf-8").strip()
    except OSError:
        __version__ = "0+unknown"
