"""Common AnimBridge staging, runtime-manifest and distribution-manifest tool.

Standard library only, so it also runs with the Windows embeddable Python.
It never fabricates completion: missing binaries, wrong CPU architecture,
escaping archive members and unresolved lock entries all fail closed.

Subcommands:
  check-lock   validate a per-target lock (--require-resolved for releases)
  stage        assemble a target tree from build output and extracted runtimes
  finalize     complete an already assembled tree (Windows PS1 pipeline):
               bootstrap copy, runtime-manifest.json, layout/arch validation
  manifest     write distribution-manifest.json (run LAST, after signing)
  verify       re-check layout, architectures and the distribution manifest
  extract      safely extract a verified archive (path-escape checks)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile
import time
import urllib.request
import plistlib
import stat
import struct
import sys
import tarfile
import zipfile

PACKAGING = Path(__file__).resolve().parent
REPO = PACKAGING.parent
SHA256 = re.compile(r"[0-9a-f]{64}")
UI_ASSETS = ("index.html", "app.js", "host-transport.js", "styles.css", "icons.js", "viewer.js", "preview-queue.js", "i18n.js", "app-icon.png") + tuple(
    f"locales/{locale}.js" for locale in ("en", "ko", "es", "ja", "zh-CN", "zh-TW", "pl", "hi", "ru", "ar", "pt-BR", "pt-PT", "fr", "th", "tr", "native-en", "native-ko"))

TARGETS = {
    "windows": {"os": "windows", "id": "windows-x64", "arch": "x64", "lock": "dependencies.lock.json", "exe": ".exe"},
    "macarm64": {"os": "macos", "id": "macos-arm64", "arch": "arm64", "lock": "targets/macarm64.lock.json", "exe": ""},
    "ubuntu22.04": {"os": "linux", "id": "linux-x64-ubuntu22.04", "arch": "x64", "lock": "targets/ubuntu22.04.lock.json", "exe": ""},
    "ubuntu24.04": {"os": "linux", "id": "linux-x64-ubuntu24.04", "arch": "x64", "lock": "targets/ubuntu24.04.lock.json", "exe": ""},
}
ALIASES = {"windows-x64": "windows", "macos-arm64": "macarm64", "ubuntu-22.04": "ubuntu22.04",
           "ubuntu-24.04": "ubuntu24.04", "linux-x64-ubuntu22.04": "ubuntu22.04", "linux-x64-ubuntu24.04": "ubuntu24.04"}


class StageError(RuntimeError):
    pass


def canonical(target: str) -> str:
    target = ALIASES.get(target, target)
    if target not in TARGETS:
        raise StageError(f"Unknown target {target!r}; expected one of {sorted(TARGETS)}")
    return target


def layout(target: str, profile: str = "production") -> dict:
    """Relative locations. Everything is relative to the bundle root:
    Windows install root, macOS Contents, Linux /opt/animbridge."""
    target = canonical(target)
    if profile not in ("production", "harness", "development"):
        raise StageError("Unknown build profile")
    kind, ext = TARGETS[target]["os"], TARGETS[target]["exe"]
    main = "AnimBridgeHarness" if profile == "harness" else "AnimBridge"
    if kind == "windows":
        return {"manifest": "runtime-manifest.json", "main": f"{main}{ext}", "cli": f"AnimBridgeCli{ext}",
                "supervisor": f"AnimBridgeProcessSupervisor{ext}", "ui": "ui", "backend": "backend",
                "python": "runtime/python/python.exe", "python_dir": "runtime/python",
                "blender": "runtime/blender/blender.exe", "blender_dir": "runtime/blender",
                "webview2": "runtime/webview2", "bootstrap": "backend/bootstrap.py"}
    if kind == "macos":
        return {"manifest": "Resources/runtime-manifest.json", "main": f"MacOS/{main}",
                "cli": "MacOS/AnimBridgeCli", "supervisor": "MacOS/AnimBridgeProcessSupervisor",
                "ui": "Resources/ui", "backend": "Resources/backend",
                "python": "Resources/runtime/python/bin/python3", "python_dir": "Resources/runtime/python",
                "blender": "Resources/runtime/blender/Blender.app/Contents/MacOS/Blender",
                "blender_dir": "Resources/runtime/blender",
                "bootstrap": "Resources/backend/bootstrap.py"}
    return {"manifest": "share/runtime-manifest.json", "main": f"bin/{main}", "cli": "bin/AnimBridgeCli",
            "supervisor": "bin/AnimBridgeProcessSupervisor", "ui": "share/ui", "backend": "share/backend",
            "python": "runtime/python/bin/python3", "python_dir": "runtime/python",
            "blender": "runtime/blender/blender", "blender_dir": "runtime/blender",
            "bootstrap": "share/backend/bootstrap.py"}


# ---------------------------------------------------------------- locks
def lock_path(target: str) -> Path:
    return PACKAGING / TARGETS[canonical(target)]["lock"]


def load_lock(target: str) -> dict:
    return json.loads(lock_path(target).read_text(encoding="utf-8-sig"))


def lock_problems(lock: dict, require_resolved: bool) -> list[str]:
    problems = []
    if not isinstance(lock, dict) or not isinstance(lock.get("assets"), list) or not lock["assets"]:
        return ["lock.assets must be a nonempty list"]
    names = set()
    for asset in lock.get("assets", []):
        if not isinstance(asset, dict):
            problems.append("asset must be an object")
            continue
        name = asset.get("name", "?")
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            problems.append("asset.name must be a simple identifier")
            continue
        filename = asset.get("filename", "")
        if not isinstance(filename, str) or not filename or Path(filename).name != filename or any(c in filename for c in ("/", "\\", "\0")):
            problems.append(f"{name}: filename must be a plain basename")
        if not isinstance(asset.get("version"), str) or not asset["version"]:
            problems.append(f"{name}: version is required")
        if name in names:
            problems.append(f"{name}: duplicate asset")
        names.add(name)
        resolved = asset.get("status", "resolved") == "resolved"
        digest = asset.get("sha256")
        if resolved:
            if not isinstance(digest, str) or not SHA256.fullmatch(digest):
                problems.append(f"{name}: resolved asset needs a 64-hex sha256")
            if not str(asset.get("url", "")).startswith("https://"):
                problems.append(f"{name}: resolved asset needs an https url")
            if lock.get("schema_version", 1) >= 2 and not asset.get("license"):
                problems.append(f"{name}: license is required")
        elif digest is not None:
            problems.append(f"{name}: unresolved asset must not carry a sha256")
        elif require_resolved:
            problems.append(f"{name}: unresolved ({asset.get('resolve_via', 'no instruction')})")
        if resolved:
            for field in ("source", "build_recipe_source", "license_bundle"):
                descriptor = asset.get(field)
                if descriptor is None:
                    continue
                if not isinstance(descriptor, dict) or not isinstance(descriptor.get("filename"), str) or Path(descriptor["filename"]).name != descriptor["filename"] or any(c in descriptor["filename"] for c in ("/", "\\", "\0")):
                    problems.append(f"{name}.{field}: filename must be a plain basename")
                    continue
                if not str(descriptor.get("url", "")).startswith("https://") or not isinstance(descriptor.get("sha256"), str) or not SHA256.fullmatch(descriptor["sha256"]):
                    problems.append(f"{name}.{field}: verified HTTPS URL and 64-hex sha256 are required")
                if field == "license_bundle":
                    if not isinstance(descriptor.get("files"), list) or not descriptor["files"]:
                        problems.append(f"{name}.license_bundle: license file inventory is required")
                    for entry in descriptor.get("files", []):
                        if not isinstance(entry, dict) or not str(entry.get("path", "")).startswith(str(descriptor.get("archive_directory", "")) + "/") or not SHA256.fullmatch(str(entry.get("sha256", ""))):
                            problems.append(f"{name}.license_bundle: invalid license file inventory")
            for descriptor in (asset, asset.get("build_recipe_source", {})):
                if descriptor.get("license_url") or descriptor.get("license_sha256"):
                    if not str(descriptor.get("license_url", "")).startswith("https://") or not SHA256.fullmatch(str(descriptor.get("license_sha256", ""))):
                        problems.append(f"{name}: upstream license needs HTTPS and a 64-hex sha256")
    return problems


def check_lock(target: str, require_resolved: bool) -> list[str]:
    lock = load_lock(target)
    problems = lock_problems(lock, require_resolved)
    expected = canonical(target)
    if lock.get("schema_version", 1) >= 2 and lock.get("target") != expected:
        problems.append(f"lock target {lock.get('target')!r} != {expected!r}")
    return problems


# ---------------------------------------------------------------- hashing
def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


# ---------------------------------------------------------------- architecture
PE_MACHINES = {0x8664: "x64", 0xAA64: "arm64", 0x14C: "x86"}
ELF_MACHINES = {0x3E: "x64", 0xB7: "arm64", 0x03: "x86"}
MACHO_CPUS = {0x01000007: "x64", 0x0100000C: "arm64", 7: "x86", 12: "arm"}


def binary_architectures(path: Path) -> set[str]:
    """CPU architectures of a PE, ELF or Mach-O (thin or fat) binary."""
    with path.open("rb") as stream:
        head = stream.read(4096)
    if head[:2] == b"MZ":
        if len(head) < 0x40:
            raise StageError(f"{path}: truncated PE header")
        offset = struct.unpack_from("<I", head, 0x3C)[0]
        with path.open("rb") as stream:
            stream.seek(offset)
            pe = stream.read(6)
        if len(pe) < 6 or pe[:4] != b"PE\0\0":
            raise StageError(f"{path}: invalid PE signature")
        machine = struct.unpack_from("<H", pe, 4)[0]
        return {PE_MACHINES.get(machine, f"pe:{machine:#x}")}
    if head[:4] == b"\x7fELF":
        if len(head) < 20:
            raise StageError(f"{path}: truncated ELF header")
        machine = struct.unpack_from("<H" if head[5] == 1 else ">H", head, 18)[0]
        return {ELF_MACHINES.get(machine, f"elf:{machine:#x}")}
    magic = head[:4]
    if magic in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe"):
        value = struct.unpack_from("<I", head, 4)[0]
        return {MACHO_CPUS.get(value, f"macho:{value:#x}")}
    if magic in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
        count = struct.unpack_from(">I", head, 4)[0]
        size = 32 if magic[3] == 0xBF else 20
        if not 0 < count <= 16 or 8 + count * size > len(head):
            raise StageError(f"{path}: invalid fat Mach-O header")
        return {MACHO_CPUS.get(struct.unpack_from(">I", head, 8 + i * size)[0], "unknown-slice")
                for i in range(count)}
    raise StageError(f"{path}: not a recognised native executable")


def require_architecture(path: Path, arch: str) -> None:
    found = binary_architectures(path)
    if found != {arch}:
        raise StageError(f"{path.name}: architecture {sorted(found)} does not match target {arch}")


# ---------------------------------------------------------------- safe paths/extraction
def safe_join(root: Path, relative: str) -> Path:
    if not isinstance(relative, str) or "\0" in relative:
        raise StageError("Invalid path encoding")
    try:
        relative.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise StageError("Invalid archive path encoding") from error
    if ":" in relative:
        raise StageError(f"Path escapes the destination: {relative!r}")
    posix = PurePosixPath(relative.replace("\\", "/"))
    if posix.is_absolute() or ".." in posix.parts or re.match(r"^[A-Za-z]:", relative):
        raise StageError(f"Path escapes the destination: {relative!r}")
    base = root.resolve()
    target = (base / Path(*posix.parts)).resolve()
    if target != base and not target.is_relative_to(base):
        raise StageError(f"Path escapes the destination: {relative!r}")
    return target


def _check_link(root: Path, member: str, link: str) -> None:
    if PurePosixPath(link).is_absolute() or re.match(r"^[A-Za-z]:", link):
        raise StageError(f"Link target escapes the destination: {member} -> {link}")
    resolved = os.path.normpath((PurePosixPath(member).parent / link).as_posix())
    safe_join(root, resolved)


def safe_extract(archive: Path, destination: Path, expected_sha256: str | None = None,
                 member_paths: set[str] | None = None) -> None:
    if expected_sha256 is not None and sha256_file(archive) != expected_sha256:
        raise StageError(f"SHA-256 mismatch for {archive.name}")
    destination.mkdir(parents=True, exist_ok=True)
    if archive.name.endswith(".zst"):
        zstd = shutil.which("zstd")
        if not zstd:
            raise StageError("Zstandard archives require the build-only zstd tool")
        with tempfile.TemporaryDirectory(prefix="animbridge-zstd-", dir=destination.parent) as temporary:
            tar_path = Path(temporary) / "uncompressed.tar"
            process = subprocess.Popen([zstd, "--decompress", "--stdout", "--quiet", str(archive)], stdout=subprocess.PIPE)
            try:
                total = 0
                with tar_path.open("xb") as output:
                    while block := process.stdout.read(1 << 20):
                        total += len(block)
                        if total > 20 * 1024**3:
                            raise StageError("Archive exceeds the expanded size limit")
                        output.write(block)
                if process.wait() != 0:
                    raise StageError("Zstandard decompression failed")
            finally:
                if process.poll() is None:
                    process.kill(); process.wait()
                process.stdout.close()
            safe_extract(tar_path, destination, member_paths=member_paths)
        return
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as zipped:
            infos = zipped.infolist()
            if member_paths is not None:
                infos = [info for info in infos if info.filename in member_paths]
                if {info.filename for info in infos} != member_paths:
                    raise StageError("Requested license archive members are missing")
            seen = set()
            if sum(info.file_size for info in infos) > 20 * 1024**3:
                raise StageError("Archive exceeds the expanded size limit")
            for info in infos:
                key = PurePosixPath(info.filename.replace("\\", "/")).as_posix()
                if key in seen:
                    raise StageError(f"Duplicate archive member: {info.filename}")
                seen.add(key)
                target = safe_join(destination, info.filename)
                if target.exists() and not (info.is_dir() and target.is_dir() and not target.is_symlink()):
                    raise StageError(f"Archive destination exists: {info.filename}")
                if stat.S_ISLNK(info.external_attr >> 16):
                    raise StageError(f"Symlink members are not accepted in ZIP archives: {info.filename}")
            for info in infos:
                target = safe_join(destination, info.filename)
                if info.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with zipped.open(info) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)
                target.chmod(0o755 if (info.external_attr >> 16) & 0o111 else 0o644)
                stamp = time.mktime((*info.date_time, 0, 0, -1))
                os.utime(target, (stamp, stamp))
        return
    with tarfile.open(archive) as tarred:
        members = tarred.getmembers()
        if member_paths is not None:
            members = [member for member in members if member.name in member_paths]
            if {member.name for member in members} != member_paths or any(not member.isfile() for member in members):
                raise StageError("Requested license archive members must exist as regular files")
        seen = set()
        if sum(member.size for member in members if member.isfile()) > 20 * 1024**3:
            raise StageError("Archive exceeds the expanded size limit")
        for member in members:
            key = PurePosixPath(member.name).as_posix()
            if key in seen:
                raise StageError(f"Duplicate archive member: {member.name}")
            seen.add(key)
            target = safe_join(destination, member.name)
            if target.exists() and not (member.isdir() and target.is_dir() and not target.is_symlink()):
                raise StageError(f"Archive destination exists: {member.name}")
            if member.issym():
                _check_link(destination, member.name, member.linkname)
            elif member.islnk():
                safe_join(destination, member.linkname)
            elif not (member.isfile() or member.isdir()):
                raise StageError(f"Special archive member refused: {member.name}")
        for member in members:
            target = safe_join(destination, member.name)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.issym():
                target.parent.mkdir(parents=True, exist_ok=True)
                os.symlink(member.linkname, target)
            elif member.islnk():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(safe_join(destination, member.linkname), target)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with tarred.extractfile(member) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)
                target.chmod(0o755 if member.mode & 0o111 else 0o644)
                os.utime(target, (member.mtime, member.mtime))


def validate_links(root: Path) -> None:
    for path in root.rglob("*"):
        if path.is_symlink():
            relative = path.relative_to(root).as_posix()
            _check_link(root, relative, os.readlink(path))
            try:
                resolved = path.resolve(strict=True)
            except (OSError, RuntimeError) as error:
                raise StageError(f"Broken or cyclic payload link: {relative}") from error
            if not resolved.is_relative_to(root.resolve()):
                raise StageError(f"Payload link escapes bundle: {relative}")


def source_snapshot(repo: Path) -> str:
    # Matches the expanded native CMake fingerprint. Root literals are separate
    # so generated _verify/CMakeLists.txt files can never enter this snapshot.
    trees = {
        "native": ("*.cpp", "*.mm", "*.h", "*.hpp", "*.in", "*.manifest", "*.json", "*.rc", "*.ico"),
        "native/ui": ("*.js", "*.mjs", "*.html", "*.css", "*.png"),
        "src/unityanimbridge": ("*.py",), "blender": ("*.py",), "unity": ("*.cs",), "unreal": ("*.py",),
        "assets/characters": ("*.fbx",),
        "packaging": ("*.py", "*.ps1", "*.sh", "*.json", "*.txt", "*.iss", "*.in", "*.plist", "*.entitlements", "*.desktop"),
        ".github/workflows": ("*.yml", "*.yaml"), "tests": ("*.py", "*.mjs"),
    }
    paths = {path for folder, patterns in trees.items() for pattern in patterns for path in (repo / folder).rglob(pattern) if path.is_file()}
    paths.update(repo / name for name in ("CMakeLists.txt", "CMakePresets.json", "VERSION", "pyproject.toml", "LICENSE") if (repo / name).is_file())
    ordered = sorted(paths, key=lambda path: path.relative_to(repo).as_posix())
    text = "".join(f"{path.relative_to(repo).as_posix()}:{sha256_file(path)}\n" for path in ordered)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def build_provenance(build: Path, repo: Path, profile: str) -> dict:
    cache_path = _require_file(build / "CMakeCache.txt", "native CMake build cache")
    cache = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        match = re.match(r"([^:#/][^:=]*):[^=]+=(.*)", line)
        if match:
            cache[match[1]] = match[2]
    expected_hook = "ON" if profile == "harness" else "OFF"
    for key, expected in (("ANIMBRIDGE_ENABLE_SOURCE_FALLBACK", "OFF"),
                          ("ANIMBRIDGE_ENABLE_UI_TEST_HOOKS", expected_hook),
                          ("ANIMBRIDGE_BUILD_DESKTOP", "ON")):
        if cache.get(key) != expected:
            raise StageError(f"{key} must be {expected} for {profile} staging")
    if cache.get("CMAKE_PROJECT_VERSION") != app_version(repo):
        raise StageError("Native build version differs from VERSION")
    evidence = [build / "animbridge_desktop.vcxproj",
                build / "CMakeFiles/animbridge_desktop.dir/flags.make",
                build / "build.ninja"]
    fingerprints = set()
    for path in evidence:
        if path.is_file():
            fingerprints.update(re.findall(r"ANIMBRIDGE_SOURCE_SNAPSHOT_SHA[^a-f0-9]+([a-f0-9]{64})", path.read_text(encoding="utf-8")))
    expected = source_snapshot(repo)
    if fingerprints != {expected}:
        raise StageError("Native source snapshot differs or is absent; freeze sources, configure and rebuild")
    config = _require_file(build / "generated/animbridge/app_config.hpp", "generated app configuration")
    return {"validated": True, "sourceSnapshot": expected, "appConfigSha256": sha256_file(config), "sourceFallback": False,
            "testHooks": profile == "harness", "profile": profile,
            "generator": cache.get("CMAKE_GENERATOR", "unknown"),
            "compiler": cache.get("CMAKE_CXX_COMPILER", "unknown"),
            "configuration": cache.get("CMAKE_BUILD_TYPE") or "Release", "buildTesting": cache.get("BUILD_TESTING", "unknown"),
            "cxxFlags": cache.get("CMAKE_CXX_FLAGS", ""), "cxxFlagsRelease": cache.get("CMAKE_CXX_FLAGS_RELEASE", "")}


def verify_embedded_build(path: Path, proof: dict) -> None:
    expected = proof["sourceSnapshot"].encode("ascii")
    forbidden = [value.encode(encoding) for value in ("--ui-test", "--automation", "--remote-debugging-port") for encoding in ("ascii", "utf-16-le")]
    found = False; debug = False; tail = b""
    with path.open("rb") as stream:
        while block := stream.read(1 << 20):
            data = tail + block
            found = found or expected in data
            if proof.get("profile") == "production":
                debug = debug or any(value in data for value in forbidden)
            tail = data[-512:]
    if not found:
        raise StageError("The native desktop executable predates its configured source snapshot; rebuild it")
    if debug:
        raise StageError("Production executable still contains UI automation/debug arguments")


def compare_provenance(production: dict, harness: dict) -> None:
    for proof, profile in ((production, "production"), (harness, "harness")):
        if not proof.get("validated") or proof.get("sourceFallback") is not False or proof.get("profile") != profile:
            raise StageError("Both artifacts must have validated distribution-mode build provenance")
    for key in ("sourceSnapshot", "runtimeLockSha256", "targetId", "appConfigSha256"):
        if not production.get(key) or production.get(key) != harness.get(key):
            raise StageError(f"Production and harness differ in {key}")
    for key in ("generator", "compiler", "configuration", "buildTesting", "cxxFlags", "cxxFlagsRelease"):
        if production.get(key) != harness.get(key):
            raise StageError(f"Production and harness differ in {key}")
    if production.get("testHooks") is not False or harness.get("testHooks") is not True:
        raise StageError("Only the harness may enable test hooks")


def fetch_asset(asset: dict, cache: Path, offline: bool = False) -> Path:
    name, expected = asset["filename"], asset["sha256"]
    if not isinstance(expected, str) or not SHA256.fullmatch(expected):
        raise StageError(f"Unresolved asset: {name}")
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / name
    if path.exists():
        if not path.is_file() or sha256_file(path) != expected:
            raise StageError(f"Cached SHA-256 mismatch: {name}; restore the pinned original")
        return path
    if offline:
        raise StageError(f"Offline cache is missing {name}")
    if not asset["url"].startswith("https://"):
        raise StageError("Downloads must use HTTPS")
    partial = cache / (name + ".partial")
    try:
        request = urllib.request.Request(asset["url"], headers={"User-Agent": "AnimBridge-stage/1"})
        with urllib.request.urlopen(request, timeout=120) as source, partial.open("xb") as output:
            if not source.url.startswith("https://"):
                raise StageError("Download redirected away from HTTPS")
            shutil.copyfileobj(source, output, 1 << 20)
        if sha256_file(partial) != expected:
            raise StageError(f"Downloaded SHA-256 mismatch: {name}")
        partial.replace(path)
    finally:
        partial.unlink(missing_ok=True)
    return path


def license_inputs(lock: dict):
    aliases = {"nlohmann-json": "json", "python-build-recipe": "pbs"}
    seen = set()
    for asset in lock["assets"]:
        candidates = [(asset["name"], asset)]
        if asset.get("build_recipe_source"):
            candidates.append((asset["name"] + "-build-recipe", asset["build_recipe_source"]))
        for name, descriptor in candidates:
            if descriptor.get("license_url") and descriptor.get("license_sha256"):
                identity = (descriptor["license_url"], descriptor["license_sha256"])
                if identity in seen:
                    continue
                seen.add(identity)
                yield {"name": name + "-license", "filename": aliases.get(name, name) + "-license.txt",
                       "url": descriptor["license_url"], "sha256": descriptor["license_sha256"]}


def fetch(target: str, cache: Path, offline: bool = False) -> dict:
    problems = check_lock(target, True)
    if problems:
        raise StageError("Lock is not releasable: " + "; ".join(problems))
    assets = {}
    lock = load_lock(target)
    for asset in lock["assets"]:
        assets[asset["name"]] = str(fetch_asset(asset, cache, offline))
        if asset.get("license_bundle"):
            assets[asset["name"] + "-license-bundle"] = str(fetch_asset(asset["license_bundle"], cache, offline))
    for asset in license_inputs(lock):
        assets[asset["name"]] = str(fetch_asset(asset, cache, offline))
    return {"target": canonical(target), "assets": assets}


def _flatten_extraction(root: Path, executable: str) -> Path:
    if (root / executable).is_file():
        return root
    candidates = [entry for entry in root.iterdir() if entry.is_dir() and not entry.is_symlink() and (entry / executable).is_file()]
    if len(candidates) != 1:
        raise StageError(f"Archive must contain one runtime root with {executable}")
    return candidates[0]


def install_wheel(wheel: Path, site: Path) -> None:
    site.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="animbridge-wheel-", dir=site.parent) as temporary:
        unpacked = Path(temporary)
        safe_extract(wheel, unpacked)
        for entry in unpacked.iterdir():
            if entry.name.endswith(".data"):
                for section in entry.iterdir():
                    if section.name in ("purelib", "platlib"):
                        shutil.copytree(section, site, dirs_exist_ok=True)
                    elif section.name not in ("scripts", "headers", "data"):
                        raise StageError(f"Unsupported wheel data section: {section.name}")
            else:
                if entry.suffix == ".pth":
                    raise StageError("Wheel import hooks are not accepted")
                destination = site / entry.name
                if destination.exists():
                    raise StageError(f"Wheel payload collision: {entry.name}")
                if entry.is_dir():
                    shutil.copytree(entry, destination)
                else:
                    shutil.copy2(entry, destination)


def prepare_runtimes(target: str, cache: Path, destination: Path, offline: bool = False,
                     mac_blender_app: Path | None = None) -> dict:
    target = canonical(target)
    if TARGETS[target]["os"] == "linux" and sys.platform != "linux":
        raise StageError("Linux runtime preparation requires a native Linux filesystem; use a Docker volume, not an NTFS bind mount")
    if destination.exists() and any(destination.iterdir()):
        raise StageError("Runtime preparation output is not empty")
    result = fetch(target, cache, offline)
    assets = {asset["name"]: asset for asset in load_lock(target)["assets"]}
    paths = {name: Path(value) for name, value in result["assets"].items()}
    for required in ("python", "numpy", "pyyaml", "blender"):
        if required not in assets:
            raise StageError(f"Missing locked runtime asset {required}")
    destination.mkdir(parents=True, exist_ok=True)
    if TARGETS[target]["os"] == "linux":
        if sys.platform != "linux":
            raise StageError("Linux runtime preparation requires a native Linux filesystem; use a Docker volume, not an NTFS bind mount")
        probe = destination / ".animbridge-CaseProbe"
        alternate = destination / ".animbridge-caseprobe"
        probe.mkdir()
        try:
            if alternate.exists():
                raise StageError("Linux runtime archives need a case-sensitive filesystem; use a native Docker volume")
        finally:
            probe.rmdir()
    python_unpack = destination / "python-unpacked"
    safe_extract(paths["python"], python_unpack, assets["python"]["sha256"])
    python_executable = "python.exe" if target == "windows" else "bin/python3"
    python_source = _flatten_extraction(python_unpack, python_executable)
    python_dir = destination / "python"
    shutil.copytree(python_source, python_dir, symlinks=True)
    version = assets["python"]["version"].split("+")[0].split(".")
    site = python_dir / ("Lib/site-packages" if target == "windows" else f"lib/python{version[0]}.{version[1]}/site-packages")
    for name in ("numpy", "pyyaml"):
        install_wheel(paths[name], site)
    blender_dir = destination / "blender"
    if target == "macarm64":
        if mac_blender_app is None or mac_blender_app.name != "Blender.app":
            raise StageError("macOS Blender DMG must be mounted read-only by Build-Mac.sh; pass its Blender.app")
        blender_dir.mkdir()
        shutil.copytree(mac_blender_app, blender_dir / "Blender.app", symlinks=True)
    else:
        blender_unpack = destination / "blender-unpacked"
        safe_extract(paths["blender"], blender_unpack, assets["blender"]["sha256"])
        blender_source = _flatten_extraction(blender_unpack, "blender.exe" if target == "windows" else "blender")
        shutil.copytree(blender_source, blender_dir, symlinks=True)
    if target == "windows":
        (python_dir / "python312._pth").write_text("python312.zip\n.\nLib/site-packages\n../../backend/src\nimport site\n", encoding="ascii")
    license_dir = destination / "licenses"
    for name, asset in assets.items():
        descriptor = asset.get("license_bundle")
        if descriptor:
            unpacked = destination / (name + "-license-unpacked")
            selected = {entry["path"] for entry in descriptor.get("files", [])}
            if descriptor.get("metadata_path"):
                selected.add(descriptor["metadata_path"])
            safe_extract(paths[name + "-license-bundle"], unpacked, descriptor["sha256"], member_paths=selected or None)
            source = safe_join(unpacked, descriptor["archive_directory"])
            if not source.is_dir():
                raise StageError(f"Missing corresponding license directory for {name}")
            for entry in descriptor.get("files", []):
                member = safe_join(unpacked, entry["path"])
                if sha256_file(member) != entry["sha256"]:
                    raise StageError(f"Runtime license checksum mismatch: {entry['path']}")
            metadata_path = descriptor.get("metadata_path")
            if metadata_path and sha256_file(safe_join(unpacked, metadata_path)) != descriptor["metadata_sha256"]:
                raise StageError("Private Python metadata checksum mismatch")
            shutil.copytree(source, license_dir / (name + "-private-runtime"), symlinks=False)
    validate_links(destination)
    receipt = {"schemaVersion": 1, "targetId": TARGETS[target]["id"], "runtimeLockSha256": sha256_file(lock_path(target)),
               "assets": [{"name": value["name"], "filename": value["filename"], "sha256": value["sha256"]} for value in assets.values()],
               "pythonInventory": distribution_files(python_dir), "blenderInventory": distribution_files(blender_dir),
               "licenseInventory": distribution_files(license_dir) if license_dir.exists() else []}
    receipt_path = destination / "runtime-receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return {"target": target, "python_dir": str(python_dir), "blender_dir": str(blender_dir), "receipt": str(receipt_path)}


def record_native_runtimes(target: str, cache: Path, python_dir: Path, blender_dir: Path,
                           license_dir: Path, evidence_path: Path, output: Path) -> dict:
    target = canonical(target)
    if not sys.platform.startswith("linux") or TARGETS[target]["os"] != "linux":
        raise StageError("Native extraction receipts require Linux")
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    if evidence.get("sourceChecksumsVerified") is not True or evidence.get("pythonLicenseInventoryMatched") is not True:
        raise StageError("Native extraction evidence is incomplete")
    if Path(evidence["pythonRoot"]).resolve() != python_dir.resolve() or Path(evidence["blenderRoot"]).resolve() != blender_dir.resolve() or Path(evidence["pythonLicenses"]).resolve() != license_dir.resolve():
        raise StageError("Native extraction evidence names different runtime roots")
    fetch(target, cache, offline=True)
    lock = load_lock(target)
    python_asset = next(asset for asset in lock["assets"] if asset["name"] == "python")
    descriptor = python_asset.get("license_bundle", {})
    prefix = PurePosixPath(descriptor["archive_directory"])
    for entry in descriptor["files"]:
        relative = PurePosixPath(entry["path"]).relative_to(prefix).as_posix()
        if sha256_file(safe_join(license_dir, relative)) != entry["sha256"]:
            raise StageError(f"Native runtime license changed: {relative}")
    if license_dir.parent.resolve() != (output.parent / "licenses").resolve():
        raise StageError("Place the receipt beside its licenses directory")
    validate_links(python_dir); validate_links(blender_dir)
    require_architecture(python_dir / "bin/python3", "x64")
    require_architecture(blender_dir / "blender", "x64")
    receipt = {"schemaVersion": 1, "targetId": TARGETS[target]["id"], "runtimeLockSha256": sha256_file(lock_path(target)),
               "nativeExtractionEvidenceSha256": sha256_file(evidence_path),
               "assets": [{"name": asset["name"], "filename": asset["filename"], "sha256": asset["sha256"]} for asset in lock["assets"]],
               "pythonInventory": distribution_files(python_dir), "blenderInventory": distribution_files(blender_dir),
               "licenseInventory": distribution_files(license_dir.parent)}
    output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return {"receipt": str(output), "target": target, "native_execution_verified": False}


def remove_bytecode(root: Path) -> list[str]:
    removed = []
    for path in root.rglob("*.pyc"):
        removed.append(path.relative_to(root).as_posix())
        path.unlink()
    for path in sorted(root.rglob("__pycache__"), key=lambda item: len(item.parts), reverse=True):
        if path.is_dir() and not path.is_symlink() and not any(path.iterdir()):
            path.rmdir()
    return removed


# ---------------------------------------------------------------- staging helpers
def _require_file(path: Path, what: str) -> Path:
    if not path.is_file():
        raise StageError(f"Missing {what}: {path}")
    return path


def _copy_tree(source: Path, destination: Path, suffixes: tuple[str, ...] | None = None) -> None:
    if not source.is_dir():
        raise StageError(f"Missing directory: {source}")
    for path in sorted(source.rglob("*")):
        if path.is_symlink():
            raise StageError(f"Linked source path refused: {path}")
        if path.is_dir() or "__pycache__" in path.parts or path.suffix == ".pyc":
            continue
        if suffixes is not None and path.suffix not in suffixes:
            continue
        target = destination / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


def copy_backend(repo: Path, backend: Path) -> None:
    """Allowlisted backend payload; bootstrap.py is the trusted entry point."""
    package = repo / "src" / "unityanimbridge"
    bootstrap = _require_file(package / "bootstrap.py", "trusted bootstrap (src/unityanimbridge/bootstrap.py)")
    for file in sorted(package.glob("*.py")):
        target = backend / "src" / "unityanimbridge" / file.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(file, target)
    shutil.copy2(bootstrap, backend / "bootstrap.py")
    for folder in ("blender", "unity", "unreal"):
        _copy_tree(repo / folder, backend / folder, (".py", ".cs"))
    # The shipped default preview character.
    _copy_tree(repo / "assets" / "characters", backend / "assets" / "characters", (".fbx",))
    for name in ("pyproject.toml", "VERSION", "LICENSE"):
        shutil.copy2(_require_file(repo / name, name), backend / name)


def app_version(repo: Path) -> str:
    version = (repo / "VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9]+)*", version):
        raise StageError("VERSION must contain a release version")
    return version


def lock_versions(target: str) -> dict:
    return {asset["name"]: asset.get("version", "") for asset in load_lock(target).get("assets", [])}


def build_runtime_manifest(target: str, version: str) -> dict:
    target = canonical(target)
    paths, arch = layout(target), TARGETS[target]["arch"]
    versions = lock_versions(target)

    def entry(key, name, asset=None):
        return {key: paths[name], "version": versions.get(asset, "") if asset else version,
                "architecture": arch}

    components = {
        "backend": entry("resourcePath", "backend"),
        "ui": entry("resourcePath", "ui"),
        "python": entry("executablePath", "python", "python"),
        "blender": entry("executablePath", "blender", "blender"),
        "supervisor": entry("executablePath", "supervisor"),
        "bootstrap": entry("resourcePath", "bootstrap"),
    }
    if TARGETS[target]["os"] == "windows":
        components["webview2"] = entry("resourcePath", "webview2", "webview2")
    return {"schemaVersion": 1, "appVersion": version, "targetId": TARGETS[target]["id"], "architecture": arch,
            "components": components}


def validate_runtime_manifest(manifest: dict, target: str, root: Path) -> list[str]:
    """Contract check shared by stage, verify and tests."""
    target = canonical(target)
    problems = []
    arch = TARGETS[target]["arch"]
    if manifest.get("schemaVersion") != 1:
        problems.append("schemaVersion must be 1")
    if not isinstance(manifest.get("appVersion"), str) or not manifest["appVersion"]:
        problems.append("appVersion must be a string")
    if manifest.get("targetId") != TARGETS[target]["id"]:
        problems.append(f"targetId {manifest.get('targetId')!r} != {TARGETS[target]['id']!r}")
    if manifest.get("architecture") != arch:
        problems.append(f"architecture must be {arch}")
    components = manifest.get("components", {})
    if not isinstance(components, dict):
        return problems + ["components must be an object"]
    expected = {"backend", "ui", "python", "blender", "supervisor", "bootstrap"}
    if TARGETS[target]["os"] == "windows":
        expected.add("webview2")
    elif "webview2" in components:
        problems.append("webview2 is Windows-only")
    for name in sorted(expected - set(components)):
        problems.append(f"missing component {name}")
    for name, value in components.items():
        if name not in expected:
            problems.append(f"unknown component {name}")
        if not isinstance(value, dict):
            problems.append(f"{name}: component must be an object")
            continue
        key = "executablePath" if name in ("python", "blender", "supervisor") else "resourcePath"
        relative = value.get(key)
        if not isinstance(relative, str):
            problems.append(f"{name}.{key} missing")
            continue
        if not isinstance(value.get("version"), str) or not value.get("version") or value.get("architecture") != arch:
            problems.append(f"{name}: version/architecture invalid")
        try:
            if "\\" in relative or any(part in ("", ".", "..") for part in relative.split("/")):
                raise StageError(f"Runtime manifest paths must be portable relative paths: {relative}")
            path = safe_join(root, relative)
        except StageError as error:
            problems.append(str(error))
            continue
        if not path.exists():
            problems.append(f"{name}: {relative} does not exist")
        elif name in ("backend", "ui", "webview2") and not path.is_dir():
            problems.append(f"{name}: {relative} is not a directory")
        elif name not in ("backend", "ui", "webview2") and not path.is_file():
            problems.append(f"{name}: {relative} is not a file")
    return problems


def write_runtime_manifest(target: str, root: Path, version: str) -> Path:
    path = root / layout(target)["manifest"]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(build_runtime_manifest(target, version), indent=2) + "\n", encoding="utf-8")
    return path


def validate_tree(target: str, root: Path, profile: str = "production") -> None:
    """Layout presence + CPU architecture for every native executable."""
    target = canonical(target)
    paths, arch = layout(target, profile), TARGETS[target]["arch"]
    validate_links(root)
    for name in ("main", "cli", "supervisor", "python", "blender"):
        path = _require_file(root / paths[name], f"executable {paths[name]}")
        require_architecture(path, arch)
        with path.open("rb") as stream:
            magic = stream.read(4)
        kind = TARGETS[target]["os"]
        if (kind == "windows" and magic[:2] != b"MZ") or (kind == "linux" and magic != b"\x7fELF") or (kind == "macos" and magic not in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf")):
            raise StageError(f"{path}: native executable format does not match {kind}")
        if os.name != "nt" and kind != "windows" and not path.stat().st_mode & 0o111:
            raise StageError(f"{path}: executable permission is missing")
    for name in ("ui", "backend"):
        if not (root / paths[name]).is_dir():
            raise StageError(f"Missing directory {paths[name]}")
    _require_file(root / paths["bootstrap"], "bootstrap")
    _require_file(root / paths["backend"] / "src/unityanimbridge/__init__.py", "backend package")
    if profile != "development":
        for asset in UI_ASSETS:
            _require_file(root / paths["ui"] / asset, f"UI asset {asset}")
    if TARGETS[target]["os"] == "windows" and not (root / paths["webview2"]).is_dir():
        raise StageError("Missing WebView2 runtime directory")
    if target == "windows":
        require_architecture(_require_file(root / paths["webview2"] / "msedgewebview2.exe", "fixed WebView2 executable"), arch)


def finalize(target: str, root: Path, repo: Path, profile: str = "production",
             build_dir: Path | None = None, asset_cache: Path | None = None) -> dict:
    target = canonical(target)
    version = app_version(repo)
    validated_proof = build_provenance(build_dir, repo, profile) if build_dir is not None else None
    if validated_proof is not None:
        verify_embedded_build(_require_file(root / layout(target, profile)["main"], "native desktop executable"), validated_proof)
    bootstrap = _require_file(repo / "src/unityanimbridge/bootstrap.py", "trusted bootstrap")
    destination = root / layout(target)["bootstrap"]
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(bootstrap, destination)
    probe = PACKAGING / "runtime_probe.py"
    if probe.is_file():
        shutil.copy2(probe, destination.parent / "runtime_probe.py")
    validate_tree(target, root, profile)
    manifest_path = write_runtime_manifest(target, root, version)
    problems = validate_runtime_manifest(json.loads(manifest_path.read_text(encoding="utf-8")), target, root)
    if problems:
        raise StageError("; ".join(problems))
    metadata_path = root / "staging-provenance.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8")) if metadata_path.is_file() else {}
    removed = remove_bytecode(root)
    if removed:
        metadata["removedLooseBytecode"] = sorted(set(metadata.get("removedLooseBytecode", [])) | set(removed))
    if asset_cache is not None:
        if target != "windows":
            raise StageError("Unix runtime preparation must use verified runtime receipts")
        checked = fetch(target, asset_cache, offline=True)
        metadata["archiveInputsVerified"] = True
        metadata["runtimePreparation"] = "Build-Distribution.ps1: verified archive extraction and allowlisted transformations"
        metadata["runtimeReceiptVerified"] = True
        metadata["pythonInventorySha256"] = hashlib.sha256(json.dumps(distribution_files(root / layout(target)["python_dir"]), sort_keys=True).encode()).hexdigest()
        metadata["blenderInventorySha256"] = hashlib.sha256(json.dumps(distribution_files(root / layout(target)["blender_dir"]), sort_keys=True).encode()).hexdigest()
    proof = validated_proof if validated_proof is not None else {"validated": False, "profile": profile, "testHooks": profile == "harness", "sourceFallback": None}
    proof.update({"runtimeLockSha256": sha256_file(lock_path(target)), "targetId": TARGETS[target]["id"]})
    metadata.update({"schemaVersion": 1, "profile": profile, "build": proof,
                     "releaseInputsVerified": bool(proof["validated"] and metadata.get("runtimeReceiptVerified") and profile == "production")})
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return {"target": target, "targetId": TARGETS[target]["id"], "runtimeManifest": str(manifest_path), "provenance": metadata}


def _validate_receipt(target: str, receipt_path: Path, python_dir: Path, blender_dir: Path) -> dict:
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    if receipt.get("schemaVersion") != 1 or receipt.get("targetId") != TARGETS[target]["id"] or receipt.get("runtimeLockSha256") != sha256_file(lock_path(target)):
        raise StageError("Runtime receipt differs from the selected target lock")
    if receipt.get("pythonInventory") != distribution_files(python_dir) or receipt.get("blenderInventory") != distribution_files(blender_dir):
        raise StageError("Extracted runtime changed after verified preparation")
    licenses = receipt_path.parent / "licenses"
    if receipt.get("licenseInventory", []) != (distribution_files(licenses) if licenses.is_dir() else []):
        raise StageError("Runtime license inventory changed after verified preparation")
    return receipt


def stage(args) -> dict:
    target = canonical(args.target)
    profile = getattr(args, "profile", "production")
    info, paths = TARGETS[target], layout(target, profile)
    repo, out = Path(args.repo).resolve(), Path(args.out).resolve()
    if out.exists() and any(out.iterdir()):
        raise StageError(f"Output directory is not empty: {out}")
    development = profile == "development"
    if args.allow_unresolved and not development:
        raise StageError("--allow-unresolved is forbidden for production and harness artifacts")
    problems = check_lock(target, require_resolved=not development)
    if problems:
        raise StageError("Lock is not releasable:\n  " + "\n  ".join(problems))
    build = Path(args.build_dir).resolve()
    proof = None if development else build_provenance(build, repo, profile)
    python_dir, blender_dir = Path(args.python_dir).resolve(), Path(args.blender_dir).resolve()
    for source in (python_dir, blender_dir):
        if not source.is_dir():
            raise StageError(f"Missing extracted runtime directory: {source}")
    receipt = None
    receipt_arg = getattr(args, "runtime_receipt", None)
    if receipt_arg:
        receipt = _validate_receipt(target, Path(receipt_arg), python_dir, blender_dir)
    elif not development:
        raise StageError("Production staging requires --runtime-receipt from prepare-runtimes")
    native = Path(args.native_dir).resolve() if args.native_dir else build
    if not args.native_dir and info["os"] == "windows" and (native / "Release").is_dir():
        native /= "Release"
    if not args.native_dir and info["os"] == "macos":
        candidate = native / (Path(paths["main"]).name + ".app/Contents/MacOS")
        if candidate.is_dir():
            native = candidate
    sources = {}
    for name in ("main", "cli", "supervisor"):
        source = native / Path(paths[name]).name
        # CMake places the CLI/supervisor beside the .app rather than inside it.
        if info["os"] == "macos" and name != "main" and not source.is_file():
            source = build / Path(paths[name]).name
        sources[name] = _require_file(source, f"built {Path(paths[name]).name}")
        require_architecture(source, info["arch"])
    if proof is not None:
        verify_embedded_build(sources["main"], proof)
    if info["os"] == "windows" and not args.webview2_dir:
        raise StageError("--webview2-dir is required for the windows target")
    for name, source in sources.items():
        destination = out / paths[name]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        destination.chmod(0o755)
    _copy_tree(repo / "native" / "ui", out / paths["ui"], (".js", ".css", ".html", ".png"))
    copy_backend(repo, out / paths["backend"])
    shutil.copytree(python_dir, out / paths["python_dir"], symlinks=True)
    shutil.copytree(blender_dir, out / paths["blender_dir"], symlinks=True)
    if info["os"] == "windows":
        shutil.copytree(Path(args.webview2_dir).resolve(), out / paths["webview2"], symlinks=True)
    if info["os"] == "macos":
        bundle_name = Path(paths["main"]).name
        plist = {"CFBundleExecutable": bundle_name, "CFBundleIdentifier": "org.animbridge.desktop" + (".harness" if profile == "harness" else ""),
                 "CFBundleName": bundle_name, "CFBundlePackageType": "APPL", "CFBundleShortVersionString": app_version(repo),
                 "CFBundleVersion": app_version(repo), "LSMinimumSystemVersion": "14.5", "NSHighResolutionCapable": True,
                 "CFBundleDocumentTypes": [{"CFBundleTypeName": "AnimBridge inputs", "CFBundleTypeRole": "Viewer", "CFBundleTypeExtensions": ["unitypackage", "anim", "fbx", "json", "uasset"]}]}
        with (out / "Info.plist").open("wb") as stream:
            plistlib.dump(plist, stream)
    shutil.copy2(repo / "LICENSE", out / "LICENSE")
    notices = repo / "packaging"
    for name in ("THIRD_PARTY_NOTICES.txt", "SOURCE-AVAILABILITY.txt"):
        if (notices / name).is_file():
            (out / "licenses").mkdir(exist_ok=True)
            shutil.copy2(notices / name, out / "licenses" / name)
        elif not development:
            raise StageError(f"Missing required distribution notice {name}")
    if (notices / "licenses").is_dir():
        if target == "windows":
            _copy_tree(notices / "licenses", out / "licenses")
        else:
            for name in ("Feather-LICENSE.txt", "utf8proc-LICENSE.txt"):
                source = notices / "licenses" / name
                if source.is_file():
                    (out / "licenses").mkdir(exist_ok=True)
                    shutil.copy2(source, out / "licenses" / name)
    if receipt_arg and (Path(receipt_arg).parent / "licenses").is_dir():
        _copy_tree(Path(receipt_arg).parent / "licenses", out / "licenses")
    shutil.copy2(lock_path(target), out / "dependencies.lock.json")
    cache_arg = getattr(args, "download_cache", None)
    for asset in load_lock(target)["assets"]:
        if asset["name"].endswith("-source") or asset["name"] in ("nlohmann-json", "zlib", "utf8proc") or asset["name"].endswith("-license"):
            if cache_arg:
                source = fetch_asset(asset, Path(cache_arg), offline=True)
                destination = out / ("licenses" if asset["name"].endswith("-license") else "sources")
                destination.mkdir(exist_ok=True)
                shutil.copy2(source, destination / source.name)
            elif not development:
                raise StageError("Production staging needs --download-cache for corresponding sources and licenses")
    if cache_arg:
        for descriptor in license_inputs(load_lock(target)):
            source = fetch_asset(descriptor, Path(cache_arg), offline=True)
            (out / "licenses").mkdir(exist_ok=True)
            shutil.copy2(source, out / "licenses" / source.name)
    metadata = {"schemaVersion": 1, "profile": profile, "developmentStaging": development,
                "runtimeReceiptVerified": receipt is not None,
                "assets": receipt.get("assets", []) if receipt else []}
    (out / "staging-provenance.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return finalize(target, out, repo, profile, build if proof is not None else None)


# ---------------------------------------------------------------- distribution manifest
def distribution_files(root: Path) -> list[dict]:
    files = []
    if not root.exists():
        return files
    validate_links(root)
    for path in sorted(root.rglob("*"), key=lambda p: p.relative_to(root).as_posix()):
        relative = path.relative_to(root).as_posix()
        if relative == "distribution-manifest.json":
            continue
        if path.is_symlink():
            files.append({"path": relative, "symlink": os.readlink(path)})
        elif path.is_file():
            files.append({"path": relative, "bytes": path.stat().st_size, "sha256": sha256_file(path),
                          "mode": f"{path.stat().st_mode & 0o777:04o}"})
    return files


def default_distribution_manifest(target: str, root: Path) -> Path:
    if canonical(target) == "macarm64":
        app = root.parent if root.name == "Contents" else root
        return app.parent / (app.stem + "-payload-manifest.json")
    return root / "distribution-manifest.json"


def _check_mac_signing(root: Path, notarized: bool) -> None:
    if sys.platform != "darwin" or root.name != "Contents" or root.parent.suffix != ".app":
        raise StageError("Signed macOS manifests require an actual .app on macOS")
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(root.parent)], check=True)
    subprocess.run(["spctl", "--assess", "--type", "execute", "--verbose=4", str(root.parent)], check=True)
    if notarized:
        subprocess.run(["xcrun", "stapler", "validate", str(root.parent)], check=True)


def write_distribution_manifest(target: str, root: Path, repo: Path, signed: bool,
                                notarized: bool = False, provenance: dict | None = None,
                                output: Path | None = None) -> Path:
    """Run after final signing/staple. macOS output is OUTSIDE the signed .app."""
    target = canonical(target)
    if notarized and not signed:
        raise StageError("Notarization cannot be claimed without signing")
    if signed:
        if target != "macarm64":
            raise StageError("Signing evidence verification is only implemented for macOS")
        _check_mac_signing(root, notarized)
    metadata_path = root / "staging-provenance.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8")) if metadata_path.is_file() else {}
    proof = dict(metadata.get("build", {}))
    if provenance:
        proof.update(provenance)
    path = output or default_distribution_manifest(target, root)
    if target == "macarm64" and path.resolve().is_relative_to(root.parent.resolve() if root.name == "Contents" else root.resolve()):
        raise StageError("macOS payload manifests must remain outside the .app")
    lock = load_lock(target)
    manifest = {"schema_version": 1, "product": "AnimBridge", "version": app_version(repo),
                "platform": lock.get("platform", TARGETS[target]["id"]), "targetId": TARGETS[target]["id"],
                "signed": signed, "notarized": notarized, "source_fallback": proof.get("sourceFallback"),
                "test_hooks": proof.get("testHooks"), "profile": metadata.get("profile", "unverified"),
                "releaseInputsVerified": metadata.get("releaseInputsVerified", False), "releaseGatesPassed": False,
                "external_editors_bundled": False, "hardware_verified": False,
                "dependencies": lock.get("assets", []), "provenance": proof,
                "files": distribution_files(root)}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return path


def verify(target: str, root: Path, manifest_path: Path | None = None,
           require_manifest: bool = False) -> dict:
    target = canonical(target)
    metadata_path = root / "staging-provenance.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8")) if metadata_path.is_file() else {}
    validate_tree(target, root, metadata.get("profile", "production"))
    manifest = json.loads((root / layout(target)["manifest"]).read_text(encoding="utf-8"))
    problems = validate_runtime_manifest(manifest, target, root)
    if any(root.rglob("*.pyc")):
        problems.append("Loose Python bytecode is forbidden in finalized payloads")
    distribution = manifest_path or default_distribution_manifest(target, root)
    if distribution.is_file():
        recorded = json.loads(distribution.read_text(encoding="utf-8-sig"))
        if recorded.get("targetId") != TARGETS[target]["id"]:
            problems.append("distribution targetId differs from runtime target")
        entries = recorded.get("files", [])
        expected = {}
        for entry in entries:
            relative = entry["path"]
            # Keep the symlink itself, rather than resolving its final component.
            safe_join(root, relative)
            if relative in expected:
                problems.append(f"duplicate manifest path: {relative}")
            expected[relative] = entry
        actual = {entry["path"]: entry for entry in distribution_files(root)}
        for relative in sorted(set(expected) | set(actual)):
            if expected.get(relative) != actual.get(relative):
                problems.append(f"payload changed: {relative}")
    elif require_manifest:
        problems.append("distribution manifest is missing")
    if problems:
        raise StageError("; ".join(problems))
    return {"target": target, "targetId": TARGETS[target]["id"], "status": "layout-verified", "hardware_verified": False,
            "releaseGatesPassed": False, "manifest_verified": distribution.is_file()}


def deb_control(target: str, version: str, installed_kib: int) -> str:
    target = canonical(target)
    if target not in ("ubuntu22.04", "ubuntu24.04"):
        raise StageError("DEB assembly requires an Ubuntu target")
    modern = target == "ubuntu24.04"
    depends = ["libc6 (>= " + ("2.39" if modern else "2.35") + ")", "libstdc++6", "libgcc-s1",
               "libgtk-3-0t64" if modern else "libgtk-3-0", "libwebkit2gtk-4.1-0 (>= 2.40)",
               "libssl3t64" if modern else "libssl3", "libglib2.0-0t64" if modern else "libglib2.0-0",
               "libasound2t64" if modern else "libasound2", "libx11-6", "libxfixes3", "libxi6", "libxrender1",
               "libxxf86vm1", "libxkbcommon0", "libsm6", "libice6", "libegl1", "libgl1", "libwayland-client0",
               "libwayland-cursor0", "libwayland-egl1", "libdecor-0-0", "libpulse0", "libdbus-1-3", "libfontconfig1", "libfreetype6", "libxrandr2", "libxcursor1", "libxinerama1"]
    return f"Package: animbridge\nVersion: {version}-1~{target}\nSection: graphics\nPriority: optional\nArchitecture: amd64\nMaintainer: AnimBridge contributors\nInstalled-Size: {installed_kib}\nDepends: {', '.join(depends)}\nHomepage: https://github.com/AidenKim923/AnimBridge\nDescription: Local animation transfer and package analysis\n Includes private Python and Blender runtimes. GTK, WebKit and other system\n dependencies must be installed through the distribution package manager.\n"


def build_deb(target: str, root: Path, output: Path, repo: Path) -> dict:
    target = canonical(target)
    if sys.platform != "linux":
        raise StageError("DEB assembly must run on Linux with dpkg-deb")
    verify(target, root, require_manifest=True)
    metadata = json.loads((root / "staging-provenance.json").read_text(encoding="utf-8"))
    if not metadata.get("releaseInputsVerified") or metadata.get("profile") != "production":
        raise StageError("DEB input needs validated production build and runtime receipts")
    executable = shutil.which("dpkg-deb")
    if not executable:
        raise StageError("dpkg-deb is unavailable")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise StageError("DEB output already exists")
    with tempfile.TemporaryDirectory(prefix="animbridge-deb-", dir=output.parent) as temporary:
        package = Path(temporary)
        prefix = package / "opt/animbridge"
        shutil.copytree(root, prefix, symlinks=True)
        control = package / "DEBIAN"
        control.mkdir()
        size = sum(path.stat().st_size for path in root.rglob("*") if path.is_file() and not path.is_symlink())
        (control / "control").write_text(deb_control(target, app_version(repo), (size + 1023) // 1024), encoding="utf-8")
        bin_dir = package / "usr/bin"; bin_dir.mkdir(parents=True)
        (bin_dir / "animbridge").symlink_to("/opt/animbridge/bin/AnimBridge")
        (bin_dir / "animbridge-cli").symlink_to("/opt/animbridge/bin/AnimBridgeCli")
        applications = package / "usr/share/applications"; applications.mkdir(parents=True)
        (applications / "animbridge.desktop").write_text("[Desktop Entry]\nType=Application\nName=AnimBridge\nExec=/opt/animbridge/bin/AnimBridge %F\nIcon=applications-graphics\nTerminal=false\nCategories=Graphics;\nStartupNotify=true\n", encoding="utf-8")
        subprocess.run([executable, "--build", "--root-owner-group", "--uniform-compression", "-Zxz", str(package), str(output)], check=True)
    return {"target": target, "deb": str(output), "sha256": sha256_file(output), "status": "assembled",
            "installation_verified": False, "network_disabled": False, "releaseGatesPassed": False}


def signing_plan(root: Path, identity: str, entitlements: dict) -> list[list[str]]:
    required = {"desktop", "cli", "supervisor", "python", "blender"}
    if set(entitlements) != required:
        raise StageError("Signing requires explicitly reviewed entitlements for desktop, cli, supervisor, python and blender")
    for path in entitlements.values():
        with Path(path).open("rb") as stream:
            if not isinstance(plistlib.load(stream), dict):
                raise StageError("Entitlements must be plist dictionaries")
    paths = layout("macarm64")
    def owner(path):
        relative = path.relative_to(root).as_posix()
        for component in ("python", "blender"):
            if relative.startswith(paths[component + "_dir"] + "/"):
                return component
        return "desktop" if relative == paths["main"] else "cli" if relative == paths["cli"] else "supervisor"
    commands = []
    for path in sorted((path for path in root.rglob("*") if path.is_file() and not path.is_symlink()), key=lambda path: (-len(path.parts), path.as_posix())):
        with path.open("rb") as stream:
            head = stream.read(16)
        if head[:4] != b"\xcf\xfa\xed\xfe":
            continue
        require_architecture(path, "arm64")
        command = ["codesign", "--force", "--options", "runtime", "--timestamp", "--sign", identity]
        if len(head) >= 16 and struct.unpack_from("<I", head, 12)[0] == 2:
            command += ["--entitlements", str(entitlements[owner(path)])]
        commands.append(command + [str(path)])
    bundles = sorted((path for path in root.rglob("*") if path.is_dir() and not path.is_symlink() and path.suffix in (".app", ".framework")), key=lambda path: (-len(path.parts), path.as_posix()))
    for path in bundles:
        command = ["codesign", "--force", "--options", "runtime", "--timestamp", "--sign", identity]
        if path.suffix == ".app":
            command += ["--entitlements", str(entitlements[owner(path)])]
        commands.append(command + [str(path)])
    commands.append(["codesign", "--force", "--options", "runtime", "--timestamp", "--sign", identity,
                     "--entitlements", str(entitlements["desktop"]), str(root.parent)])
    return commands


def sign_macos(root: Path, config_path: Path, evidence_path: Path, kind: str = "app") -> dict:
    if sys.platform != "darwin":
        raise StageError("Signing/notarization can only run explicitly on macOS")
    config = json.loads(config_path.read_text(encoding="utf-8"))
    identity, profile = config.get("identity"), config.get("notaryProfile")
    if not isinstance(identity, str) or not identity or not isinstance(profile, str) or not profile:
        raise StageError("Configure a Developer ID identity and a pre-existing keychain notary profile")
    evidence = {"kind": kind, "identity": identity, "signed": False, "notarized": False}
    if kind == "app":
        if root.name != "Contents" or root.parent.suffix != ".app" or evidence_path.resolve().is_relative_to(root.parent.resolve()):
            raise StageError("Use .app/Contents and keep signing evidence outside the app")
        entitlements = {name: (config_path.parent / value).resolve() for name, value in config.get("entitlements", {}).items()}
        for command in signing_plan(root, identity, entitlements):
            subprocess.run(command, check=True)
        artifact = root.parent
        evidence["entitlements"] = {name: {"file": path.name, "sha256": sha256_file(path)} for name, path in entitlements.items()}
    else:
        if root.suffix != ".dmg":
            raise StageError("DMG signing requires a .dmg artifact")
        artifact = root
        subprocess.run(["codesign", "--force", "--timestamp", "--sign", identity, str(artifact)], check=True)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(artifact)], check=True)
    evidence["signed"] = True
    with tempfile.TemporaryDirectory(prefix="animbridge-notary-", dir=evidence_path.parent) as temporary:
        submission = artifact
        if kind == "app":
            submission = Path(temporary) / "AnimBridge.zip"
            subprocess.run(["ditto", "-c", "-k", "--keepParent", str(artifact), str(submission)], check=True)
        result = subprocess.run(["xcrun", "notarytool", "submit", str(submission), "--keychain-profile", profile,
                                 "--output-format", "json", "--wait"], capture_output=True, text=True, check=True)
        response = json.loads(result.stdout)
        evidence["submission"] = {key: response[key] for key in ("id", "status", "name", "createdDate") if key in response}
        if response.get("status") != "Accepted":
            raise StageError("Notarization did not return Accepted")
    subprocess.run(["xcrun", "stapler", "staple", str(artifact)], check=True)
    subprocess.run(["xcrun", "stapler", "validate", str(artifact)], check=True)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(artifact)], check=True)
    assess = ["spctl", "--assess", "--type", "execute", "--verbose=4", str(artifact)] if kind == "app" else ["spctl", "--assess", "--type", "open", "--context", "context:primary-signature", "--verbose=4", str(artifact)]
    subprocess.run(assess, check=True)
    evidence.update({"notarized": True, "staplerValidated": True, "gatekeeperAssessed": True,
                     "sha256": sha256_file(artifact) if kind == "dmg" else None, "releaseGatesPassed": False})
    evidence_path.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    return evidence


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("check-lock", "fetch", "prepare-runtimes", "record-runtimes", "stage", "finalize", "manifest", "verify", "extract", "deb", "compare-provenance", "build-proof", "sign-macos"):
        sub = commands.add_parser(name)
        if name not in ("extract", "compare-provenance", "sign-macos"):
            sub.add_argument("--target", required=True)
        if name == "build-proof":
            sub.add_argument("--build-dir", required=True, type=Path)
            sub.add_argument("--repo", default=REPO, type=Path)
            sub.add_argument("--profile", choices=("production", "harness"), required=True)
            sub.add_argument("--output", required=True, type=Path)
        if name == "sign-macos":
            sub.add_argument("--root", required=True, type=Path)
            sub.add_argument("--config", required=True, type=Path)
            sub.add_argument("--evidence", required=True, type=Path)
            sub.add_argument("--kind", choices=("app", "dmg"), default="app")
        if name == "check-lock":
            sub.add_argument("--require-resolved", action="store_true")
        if name in ("fetch", "prepare-runtimes"):
            sub.add_argument("--cache", required=True, type=Path)
            sub.add_argument("--offline", action="store_true")
        if name == "prepare-runtimes":
            sub.add_argument("--out", required=True, type=Path)
            sub.add_argument("--mac-blender-app", type=Path)
        if name == "record-runtimes":
            for option in ("--cache", "--python-dir", "--blender-dir", "--license-dir", "--evidence", "--output"):
                sub.add_argument(option, required=True, type=Path)
        if name == "stage":
            for option in ("--build-dir", "--python-dir", "--blender-dir", "--out"):
                sub.add_argument(option, required=True)
            sub.add_argument("--native-dir")
            sub.add_argument("--webview2-dir")
            sub.add_argument("--runtime-receipt")
            sub.add_argument("--download-cache")
            sub.add_argument("--profile", choices=("production", "harness", "development"), default="production")
            sub.add_argument("--repo", default=str(REPO))
            sub.add_argument("--allow-unresolved", action="store_true", help="Unverified development staging; not a release artifact")
        if name in ("finalize", "manifest", "verify", "deb"):
            sub.add_argument("--root", required=True, type=Path)
            sub.add_argument("--repo", default=REPO, type=Path)
        if name == "finalize":
            sub.add_argument("--build-dir", type=Path, required=True)
            sub.add_argument("--asset-cache", type=Path)
            sub.add_argument("--profile", choices=("production", "harness"), default="production")
        if name == "manifest":
            sub.add_argument("--signed", action="store_true")
            sub.add_argument("--notarized", action="store_true")
            sub.add_argument("--output", type=Path)
        if name == "verify":
            sub.add_argument("--manifest", type=Path)
            sub.add_argument("--layout-only", action="store_true")
        if name == "deb":
            sub.add_argument("--output", required=True, type=Path)
        if name == "extract":
            sub.add_argument("--archive", required=True, type=Path)
            sub.add_argument("--dest", required=True, type=Path)
            sub.add_argument("--sha256")
        if name == "compare-provenance":
            sub.add_argument("--production", required=True, type=Path)
            sub.add_argument("--harness", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "check-lock":
            problems = check_lock(args.target, args.require_resolved)
            print(json.dumps({"target": canonical(args.target), "problems": problems}, indent=2))
            return 1 if problems else 0
        if args.command == "build-proof":
            target = canonical(args.target)
            result = build_provenance(args.build_dir, args.repo, args.profile)
            result.update({"targetId": TARGETS[target]["id"], "runtimeLockSha256": sha256_file(lock_path(target))})
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        elif args.command == "sign-macos":
            result = sign_macos(args.root, args.config, args.evidence, args.kind)
        elif args.command == "fetch":
            result = fetch(args.target, args.cache, args.offline)
        elif args.command == "prepare-runtimes":
            result = prepare_runtimes(args.target, args.cache, args.out, args.offline, args.mac_blender_app)
        elif args.command == "record-runtimes":
            result = record_native_runtimes(args.target, args.cache, args.python_dir, args.blender_dir, args.license_dir, args.evidence, args.output)
        elif args.command == "stage":
            result = stage(args)
        elif args.command == "finalize":
            result = finalize(args.target, args.root.resolve(), args.repo.resolve(), args.profile, args.build_dir.resolve(), args.asset_cache)
        elif args.command == "manifest":
            result = {"manifest": str(write_distribution_manifest(args.target, args.root.resolve(), args.repo.resolve(), args.signed, args.notarized, output=args.output))}
        elif args.command == "verify":
            result = verify(args.target, args.root.resolve(), args.manifest, not args.layout_only)
        elif args.command == "deb":
            result = build_deb(args.target, args.root.resolve(), args.output.resolve(), args.repo.resolve())
        elif args.command == "compare-provenance":
            production = json.loads(args.production.read_text(encoding="utf-8"))
            harness = json.loads(args.harness.read_text(encoding="utf-8"))
            compare_provenance(production.get("build", production), harness.get("build", harness))
            result = {"status": "matched", "difference": "testHooks only", "runtime_execution_verified": False}
        else:
            safe_extract(args.archive, args.dest, args.sha256)
            result = {"extracted": str(args.dest)}
    except (StageError, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"stage.py: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
