"""Loading Unity's flavour of YAML (.anim, .meta, .prefab ...)."""
from __future__ import annotations

import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

try:
    _Loader = yaml.CSafeLoader
except AttributeError:  # pragma: no cover - pure python fallback
    _Loader = yaml.SafeLoader

# "--- !u!74 &7400000" or "--- !u!1 &123 stripped"
_DOC_HEADER = re.compile(r"^--- !u!(\d+) &(-?\d+)( stripped)?[ \t]*$", re.MULTILINE)


class UnityYamlError(Exception):
    pass


@dataclass
class UnityDocument:
    class_id: int
    file_id: int
    stripped: bool
    type_name: str
    body: Any


def is_text_yaml(path: Path) -> bool:
    with open(path, "rb") as f:
        head = f.read(5)
    return head == b"%YAML"


def load_documents(path: Path) -> list[UnityDocument]:
    """Split a Unity YAML file into documents and parse each body."""
    path = Path(path)
    if not is_text_yaml(path):
        raise UnityYamlError(
            f"{path}: not a text-serialized Unity asset (missing %YAML header). "
            "Use the Unity baker helper (path B) or set Asset Serialization to Force Text."
        )
    text = path.read_text(encoding="utf-8-sig")
    headers = list(_DOC_HEADER.finditer(text))
    if not headers:
        return [UnityDocument(-1, 0, False, "", _parse(text, path, 1))]
    docs: list[UnityDocument] = []
    for i, m in enumerate(headers):
        start = m.end()
        end = headers[i + 1].start() if i + 1 < len(headers) else len(text)
        line_no = text.count("\n", 0, start) + 1
        body = _parse(text[start:end], path, line_no)
        type_name = ""
        if isinstance(body, dict) and len(body) == 1:
            type_name, body = next(iter(body.items()))
        docs.append(UnityDocument(int(m.group(1)), int(m.group(2)), bool(m.group(3)), type_name, body))
    return docs


def load_plain(path: Path) -> Any:
    """Load a .meta (plain YAML) file."""
    text = Path(path).read_text(encoding="utf-8-sig")
    return _parse(text, Path(path), 1)


def _parse(text: str, path: Path, line_offset: int) -> Any:
    try:
        return yaml.load(text, Loader=_Loader)
    except yaml.YAMLError as e:
        mark = getattr(e, "problem_mark", None)
        where = f"line {mark.line + line_offset}" if mark is not None else "unknown line"
        raise UnityYamlError(f"{path}: YAML parse error at {where}: {e}") from e


def to_float(v: Any) -> float:
    """Unity writes Infinity/-Infinity/NaN as bare words, which YAML 1.1 leaves as strings."""
    if isinstance(v, bool):
        return float(v)
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, str):
        low = v.strip().lower()
        if low in ("infinity", "+infinity", "inf", ".inf"):
            return math.inf
        if low in ("-infinity", "-inf", "-.inf"):
            return -math.inf
        if low in ("nan", ".nan"):
            return math.nan
        return float(low)
    if v is None:
        return 0.0
    raise UnityYamlError(f"expected number, got {v!r}")


def vec(d: Any, keys: str) -> tuple[float, ...]:
    """{x: 1, y: 2, z: 3} -> (1.0, 2.0, 3.0) for keys 'xyz'."""
    if not isinstance(d, dict):
        raise UnityYamlError(f"expected vector mapping, got {d!r}")
    return tuple(to_float(d.get(k, 0.0)) for k in keys)
