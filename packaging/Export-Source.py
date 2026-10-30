"""Export an allowlisted source folder and reproducible ZIP inside _verify."""
from __future__ import annotations

import hashlib
import argparse
import json
from pathlib import Path
import re
import stat
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


ROOT_FILES = (
    ".editorconfig", ".gitattributes", ".gitignore", "AnimBridge.cmd",
    "CMakeLists.txt", "CMakePresets.json", "CONTRIBUTING.md", "SECURITY.md", "LICENSE", "pyproject.toml", "README.md", "VERSION",
    "native/app-config.json", "native/app.manifest",
    "native/resources/resource.h", "native/resources/animbridge.rc", "native/resources/animbridge.ico", "native/ui/app-icon.png",
    "packaging/AnimBridge.iss", "packaging/Build-Distribution.ps1",
    "packaging/dependencies.lock.json", "packaging/Export-Source.py", "packaging/INSTALL.txt",
    "packaging/Prepare-Portable.cmd", "packaging/SOURCE-AVAILABILITY.txt",
    "packaging/THIRD_PARTY_NOTICES.txt", "packaging/Verify-Distribution.py", "packaging/stage.py",
    ".github/workflows/cross-platform.yml",
    "assets/readme/animbridge-readme-banner.png",
)
SOURCE_PATTERNS = (
    "readmes/README.*.md", ".github/**/*.yml", ".github/*.md", ".github/CODEOWNERS",
    "native/**/*.cpp", "native/**/*.mm", "native/**/*.hpp", "native/**/*.h", "native/**/*.hpp.in",
    "native/**/*.cmake", "native/**/CMakeLists.txt", "native/**/*.plist", "native/**/*.plist.in", "native/**/*.entitlements",
    "native/**/*.desktop", "native/**/*.sh", "native/**/*.rc",
    "native/ui/*.html", "native/ui/*.css", "native/ui/*.js", "native/ui/locales/*.js",
    "src/unityanimbridge/*.py", "tests/*.py", "tests/*.cs", "tests/*.mjs", "blender/*.py",
    "unity/Editor/*.cs", "unreal/*.py", "packaging/licenses/*.txt",
    "packaging/*.py", "packaging/*.sh", "packaging/*.txt", "packaging/*.json", "packaging/targets/*.json", "packaging/targets/*.txt",
)
EXCLUDED_PARTS = frozenset({
    "_verify", "verify", "runtime", "third_party", "build", "dist", "node_modules",
    "__pycache__", ".git", ".venv", "venv", "assets",
})
LOCAL_PATH = re.compile(r"[A-Za-z]:\\(?:Users|osk-vault|LLMWIKI)\\|/Users/(?!Shared/)[A-Za-z0-9_.-]+/|/home/[A-Za-z0-9_.-]+/")
TEXT_SUFFIXES = frozenset({".py", ".md", ".txt", ".json", ".yml", ".sh", ".cpp", ".mm", ".hpp", ".h", ".in",
                           ".cmake", ".plist", ".entitlements", ".desktop", ".cs", ".js", ".mjs", ".css", ".html",
                           ".ps1", ".cmd", ".iss", ".toml", ".rc"})


def checked_path(root: Path, relative: Path | str) -> Path:
    path = root / relative
    if not path.resolve().is_relative_to(root):
        raise ValueError(f"Path leaves the repository: {relative}")
    current = path
    while current != root:
        attributes = getattr(current.lstat(), "st_file_attributes", 0) if current.exists() else 0
        if current.is_symlink() or attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError(f"Linked paths are not allowed: {relative}")
        current = current.parent
    return path


def source_files(root: Path) -> list[Path]:
    files = {Path(name) for name in ROOT_FILES}
    for pattern in SOURCE_PATTERNS:
        for path in root.glob(pattern):
            relative = path.relative_to(root)
            if path.is_file() and not EXCLUDED_PARTS.intersection(relative.parts[:-1]):
                files.add(relative)
    for relative in files:
        if not checked_path(root, relative).is_file():
            raise FileNotFoundError(f"Missing source file: {relative}")
    return sorted(files, key=lambda path: path.as_posix())


def is_executable(path: Path, relative: Path) -> bool:
    return relative.suffix == ".sh" or bool(path.stat().st_mode & 0o111)


def leaked_path(data: bytes, relative: Path) -> bool:
    return relative.suffix.lower() in TEXT_SUFFIXES and bool(LOCAL_PATH.search(data.decode("utf-8", "replace")))


def export_source(root: Path, label: str = "") -> dict:
    root = root.resolve()
    version = checked_path(root, "VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9]+)*", version):
        raise ValueError("VERSION must contain a release version")
    if label and not re.fullmatch(r"[a-z0-9][a-z0-9-]*", label):
        raise ValueError("Archive label must contain lowercase letters, digits and hyphens")
    files = source_files(root)
    contents = {relative: checked_path(root, relative).read_bytes() for relative in files}
    leaks = [relative.as_posix() for relative, data in contents.items() if leaked_path(data, relative)]
    if leaks:
        raise ValueError(f"Local paths found in source files: {', '.join(leaks)}")
    name = f"AnimBridge-{version}-source" + (f"-{label}" if label else "")
    release = checked_path(root, Path("_verify") / "source-release")
    folder = checked_path(root, release.relative_to(root) / name)
    archive = checked_path(root, release.relative_to(root) / f"{name}.zip")
    if folder.exists() or archive.exists():
        raise FileExistsError(f"Source release already exists: {name}")
    folder.mkdir(parents=True)
    with ZipFile(archive, "x") as output:
        for relative in files:
            data = contents[relative]
            mode = 0o755 if is_executable(root / relative, relative) else 0o644
            destination = folder / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            destination.chmod(mode)
            info = ZipInfo(f"{name}/{relative.as_posix()}", date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (0o100000 | mode) << 16
            output.writestr(info, data, compress_type=ZIP_DEFLATED, compresslevel=9)
    return {"folder": str(folder), "zip": str(archive), "files": len(files),
            "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="", help="Optional archive suffix; existing exports are never overwritten")
    args = parser.parse_args()
    print(json.dumps(export_source(Path(__file__).resolve().parents[1], args.label), indent=2))
