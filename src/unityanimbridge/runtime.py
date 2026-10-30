"""Host-validated runtime context, OS paths, runtime manifest, safe child environment and engine locators.

Release ("bundled") mode is fail-closed: executable and backend locations come only from
runtime-manifest.json, and user supplied UAB_* overrides are rejected. Development mode keeps
the explicit environment/CLI overrides that the source checkout relies on.
"""
from __future__ import annotations

import contextlib
from contextvars import ContextVar
import json
import os
import platform as _platform
import plistlib
import re
import struct
import sys
import tempfile
import threading
from dataclasses import dataclass
from pathlib import Path, PurePosixPath, PureWindowsPath

APP = "AnimBridge"
MODES = ("bundled", "development")
ARCHITECTURES = ("x64", "arm64")
DEFAULT_JOB_SECONDS = 3600.0
UNITY_BASELINE_VERSION = "6000.4.7f1"
UNREAL_BASELINE_VERSION = "5.8.2"

HOST_VARIABLES = frozenset({
    "UAB_RUNTIME_MODE", "UAB_RUNTIME_MANIFEST", "UAB_RUNTIME_ROOT", "UAB_DATA_DIR", "UAB_CACHE_DIR",
    "UAB_STATE_DIR", "UAB_LOG_DIR", "UAB_WORK_DIR", "UAB_BACKEND_DIR", "UAB_STAGE_FD", "UAB_STAGE_HANDLE",
    "UAB_JOB_TIMEOUT_SECONDS", "UAB_STAGE_TIMEOUT_SECONDS", "UAB_JOB_DEADLINE_MONOTONIC_MS", "UAB_ATTEMPT_NONCE"})
BLOCKED_PREFIXES = ("PYTHON", "LD_", "DYLD_", "WEBKIT_", "WEBVIEW2_")
_DIR_VARIABLES = (("data_dir", "UAB_DATA_DIR"), ("cache_dir", "UAB_CACHE_DIR"), ("state_dir", "UAB_STATE_DIR"),
                  ("log_dir", "UAB_LOG_DIR"), ("work_dir", "UAB_WORK_DIR"))
_REQUIRED_COMPONENTS = {"backend": "resourcePath", "ui": "resourcePath", "python": "executablePath",
                        "blender": "executablePath", "supervisor": "executablePath", "bootstrap": "resourcePath"}
_MANIFEST_LOCATIONS = ("runtime-manifest.json", "Resources/runtime-manifest.json", "share/runtime-manifest.json")


class RuntimeContextError(RuntimeError):
    """The host handed the worker an unusable or unsafe runtime description."""


class EngineError(RuntimeError):
    """A user-chosen or discovered engine executable is unusable."""


def host_platform():
    return "windows" if sys.platform.startswith("win") else "macos" if sys.platform == "darwin" else "linux"


def host_architecture():
    machine = _platform.machine().lower()
    return "arm64" if machine in ("arm64", "aarch64") else "x64" if machine in ("x86_64", "amd64", "x64") else machine


def _flavor(platform):
    if platform == host_platform():
        return Path
    return PureWindowsPath if platform == "windows" else PurePosixPath


def _env_get(environ, name, platform):
    if platform == "windows":
        for key, value in environ.items():
            if key.upper() == name.upper():
                return value
        return None
    return environ.get(name)


def _absolute_value(flavor, value):
    return bool(value) and flavor(value).is_absolute() and ".." not in flavor(value).parts


def default_dirs(platform=None, environ=None, home=None):
    """OS-conventional writable locations. Relative XDG values are ignored."""
    platform = platform or host_platform()
    environ = os.environ if environ is None else environ
    flavor = _flavor(platform)
    home = flavor(str(home)) if home is not None else flavor(str(Path.home()))
    if platform == "windows":
        local = _env_get(environ, "LOCALAPPDATA", platform)
        base = flavor(local) if _absolute_value(flavor, local) else home / "AppData" / "Local"
        data = base / APP
        cache, state, logs = data / "cache", data, data / "logs"
    elif platform == "macos":
        data = home / "Library" / "Application Support" / APP
        cache, logs = home / "Library" / "Caches" / APP, home / "Library" / "Logs" / APP
        state = data
    else:
        def xdg(name, fallback):
            value = environ.get(name)
            return flavor(value) / APP if _absolute_value(flavor, value) else home / fallback / APP
        data, cache = xdg("XDG_DATA_HOME", ".local/share"), xdg("XDG_CACHE_HOME", ".cache")
        state = xdg("XDG_STATE_HOME", ".local/state")
        logs = state / "logs"
    return {"data_dir": data, "cache_dir": cache, "state_dir": state, "log_dir": logs, "work_dir": cache / "jobs"}


@dataclass(frozen=True)
class RuntimeContext:
    mode: str
    platform: str
    data_dir: Path
    cache_dir: Path
    state_dir: Path
    log_dir: Path
    work_dir: Path
    backend_dir: Path
    manifest: dict | None = None
    manifest_root: Path | None = None
    temp_dir: Path | None = None

    @property
    def bundled(self):
        return self.mode == "bundled"

    def to_env(self):
        """The exact values the host injects; a worker started from this reproduces the same context."""
        env = {"UAB_RUNTIME_MODE": self.mode, "UAB_BACKEND_DIR": str(self.backend_dir)}
        env.update({name: str(getattr(self, field)) for field, name in _DIR_VARIABLES})
        if self.temp_dir is not None:
            env.update(TMPDIR=str(self.temp_dir), TMP=str(self.temp_dir), TEMP=str(self.temp_dir))
        if self.manifest is not None and self.manifest_root is not None:
            env["UAB_RUNTIME_ROOT"] = str(self.manifest_root)
        return env

    def component_path(self, component, key):
        if not self.manifest or self.manifest_root is None:
            raise RuntimeContextError("No runtime manifest is loaded")
        entry = self.manifest["components"].get(component)
        if not isinstance(entry, dict) or key not in entry:
            raise RuntimeContextError(f"Runtime manifest has no {component}.{key}")
        return resolve_manifest_path(self.manifest_root, entry[key], f"{component}.{key}")

    @classmethod
    def from_environ(cls, environ=None, platform=None, architecture=None, default_backend=None):
        environ = dict(os.environ if environ is None else environ)
        platform = platform or host_platform()
        architecture = architecture or host_architecture()
        flavor = _flavor(platform)
        mode = environ.get("UAB_RUNTIME_MODE") or "development"
        if mode not in MODES:
            raise RuntimeContextError(f"UAB_RUNTIME_MODE must be one of {MODES}")
        defaults = default_dirs(platform, environ)
        if mode == "bundled":
            rejected = sorted(name for name in environ if name.upper().startswith("UAB_") and name.upper() not in HOST_VARIABLES)
            if rejected:
                raise RuntimeContextError("Overrides are not accepted in a release runtime: " + ", ".join(rejected))
            missing = [name for name in [n for _, n in _DIR_VARIABLES] + ["UAB_BACKEND_DIR"] if not environ.get(name)]
            if missing:
                raise RuntimeContextError("The host did not inject: " + ", ".join(missing))
            targets = {"windows": {"windows-x64"}, "macos": {"macos-arm64"},
                       "linux": {"linux-x64-ubuntu22.04", "linux-x64-ubuntu24.04"}}
        dirs = {}
        for field, name in _DIR_VARIABLES:
            value = environ.get(name)
            if value:
                if not _absolute_value(flavor, value):
                    raise RuntimeContextError(f"{name} must be an absolute path without '..'")
                dirs[field] = flavor(value)
            else:
                dirs[field] = defaults[field]
        backend = environ.get("UAB_BACKEND_DIR")
        if backend and not _absolute_value(flavor, backend):
            raise RuntimeContextError("UAB_BACKEND_DIR must be an absolute path without '..'")
        backend = flavor(backend) if backend else flavor(default_backend or Path(__file__).resolve().parents[2])
        manifest = root = None
        manifest_path, root_value = environ.get("UAB_RUNTIME_MANIFEST"), environ.get("UAB_RUNTIME_ROOT")
        if mode == "bundled" and not (manifest_path or root_value):
            raise RuntimeContextError("A release runtime needs UAB_RUNTIME_MANIFEST or UAB_RUNTIME_ROOT")
        if manifest_path or root_value:
            manifest, root = load_manifest(manifest_path, root_value)
            if mode == "bundled":
                if manifest["targetId"] not in targets.get(platform, set()):
                    raise RuntimeContextError("Runtime manifest target does not match this operating system")
                if manifest["architecture"] != architecture:
                    raise RuntimeContextError(f"Runtime manifest architecture {manifest['architecture']} does not match host {architecture}")
                declared = resolve_manifest_path(root, manifest["components"]["backend"]["resourcePath"], "backend.resourcePath")
                if os.path.normcase(os.path.realpath(declared)) != os.path.normcase(os.path.realpath(backend)):
                    raise RuntimeContextError("UAB_BACKEND_DIR is not the backend declared by the runtime manifest")
        temp = environ.get("TMPDIR") if environ.get("UAB_WORK_DIR") else None
        if temp and not _absolute_value(flavor, temp):
            raise RuntimeContextError("The host TMPDIR must be absolute")
        return cls(mode, platform, dirs["data_dir"], dirs["cache_dir"], dirs["state_dir"], dirs["log_dir"],
                   dirs["work_dir"], backend, manifest, root, flavor(temp) if temp else dirs["work_dir"] / "tmp")


def current_context(environ=None):
    return RuntimeContext.from_environ(environ)


def resolve_manifest_path(root, value, label="path"):
    """Manifest paths are relative to the bundle root and must stay inside it."""
    if not isinstance(value, str) or not value:
        raise RuntimeContextError(f"Runtime manifest {label} must be a non-empty string")
    portable = value.replace("\\", "/")
    if portable.startswith("/") or re.match(r"^[A-Za-z]:", portable) or ".." in portable.split("/"):
        raise RuntimeContextError(f"Runtime manifest {label} must be relative to the bundle root: {value}")
    resolved = Path(root, *portable.split("/")).resolve()
    if not resolved.is_relative_to(Path(root).resolve()):
        raise RuntimeContextError(f"Runtime manifest {label} escapes the bundle root: {value}")
    return resolved


def load_manifest(manifest_path, root_value):
    root = Path(root_value) if root_value else None
    if manifest_path:
        path = Path(manifest_path)
        if root is None:
            root = path.parent.parent if path.parent.name in ("Resources", "share") else path.parent
    else:
        path = next((root / location for location in _MANIFEST_LOCATIONS if (root / location).is_file()), None)
        if path is None:
            raise RuntimeContextError(f"runtime-manifest.json not found under {root}")
    if not path.is_absolute() or not root.is_absolute():
        raise RuntimeContextError("Runtime manifest and root must be absolute paths")
    if not path.resolve().is_relative_to(root.resolve()):
        raise RuntimeContextError("Runtime manifest is outside the bundle root")
    try:
        manifest = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as error:
        raise RuntimeContextError(f"Cannot read runtime manifest {path}: {error}") from error
    if not isinstance(manifest, dict) or manifest.get("schemaVersion") != 1:
        raise RuntimeContextError("Unsupported runtime manifest schemaVersion")
    for key in ("appVersion", "targetId"):
        if not isinstance(manifest.get(key), str) or not manifest[key]:
            raise RuntimeContextError(f"Runtime manifest {key} is missing")
    if manifest.get("architecture") not in ARCHITECTURES:
        raise RuntimeContextError("Runtime manifest architecture must be x64 or arm64")
    components = manifest.get("components")
    if not isinstance(components, dict):
        raise RuntimeContextError("Runtime manifest components are missing")
    for component, key in _REQUIRED_COMPONENTS.items():
        entry = components.get(component)
        if not isinstance(entry, dict):
            raise RuntimeContextError(f"Runtime manifest component {component} is missing")
        if entry.get("architecture") != manifest["architecture"] or not isinstance(entry.get("version"), str) or not entry["version"]:
            raise RuntimeContextError(f"Runtime manifest component {component} metadata is invalid")
        component_path = resolve_manifest_path(root, entry.get(key), f"{component}.{key}")
        if not (component_path.is_dir() if component in ("backend", "ui") else component_path.is_file()):
            raise RuntimeContextError(f"Runtime component {component} is missing")
    if components["backend"].get("architecture") not in (None, manifest["architecture"]):
        raise RuntimeContextError("Backend architecture differs from the runtime manifest architecture")
    return manifest, root


def python_worker_command(context, args):
    """Argv for a private, isolated CPython worker: `-I -B -X utf8 bootstrap.py --backend <src> -- <args>`."""
    python = context.component_path("python", "executablePath")
    bootstrap = context.component_path("bootstrap", "resourcePath")
    backend = context.component_path("backend", "resourcePath")
    for path in (python, bootstrap):
        if not path.is_file():
            raise RuntimeContextError(f"Runtime component is missing: {path}")
    return [str(python), "-I", "-B", "-X", "utf8", str(bootstrap), "--backend", str(backend / "src"), "--", *map(str, args)]


# ---------------------------------------------------------------- child environment

_UNIX_ALLOWED = ("HOME", "USER", "LOGNAME", "LANG", "LANGUAGE", "TZ", "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY",
                 "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE", "DBUS_SESSION_BUS_ADDRESS", "SHELL")
_WINDOWS_ALLOWED = ("SYSTEMROOT", "WINDIR", "SYSTEMDRIVE", "COMSPEC", "PATHEXT", "USERPROFILE", "HOMEDRIVE", "HOMEPATH",
                    "APPDATA", "LOCALAPPDATA", "PROGRAMDATA", "ALLUSERSPROFILE", "PROGRAMFILES", "PROGRAMFILES(X86)", "PROGRAMW6432",
                    "COMMONPROGRAMFILES", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE", "USERNAME", "USERDOMAIN",
                    "COMPUTERNAME", "OS")
_SCRATCH = ContextVar("animbridge_scratch", default=())


@contextlib.contextmanager
def scratch(path):
    """Redirect converter scratch files into an app-owned directory without touching os.environ."""
    token = _SCRATCH.set((*_SCRATCH.get(), Path(path)))
    try:
        yield
    finally:
        _SCRATCH.reset(token)


def scratch_dir():
    paths = _SCRATCH.get()
    return paths[-1] if paths else None


def safe_env(context=None, *, extra=None, temp=None, environ=None, platform=None, include_context=False):
    """Allowlisted environment for engine/tool children. Nothing else is inherited."""
    environ = os.environ if environ is None else environ
    context = context or RuntimeContext.from_environ(environ)
    platform = platform or context.platform
    env = {}
    if platform == "windows":
        for key, value in environ.items():
            if key.upper() in _WINDOWS_ALLOWED:
                env[key] = value
        root = _env_get(environ, "SYSTEMROOT", platform) or r"C:\Windows"
        env["PATH"] = ";".join((rf"{root}\System32", root, rf"{root}\System32\Wbem"))
    else:
        for key, value in environ.items():
            if key in _UNIX_ALLOWED or key.startswith("LC_"):
                env[key] = value
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    temp = str(temp or context.temp_dir or context.work_dir / "tmp")
    env.update(TMPDIR=temp, TMP=temp, TEMP=temp)
    if include_context:
        env.update(context.to_env())
        for name in ("UAB_RUNTIME_MANIFEST", "UAB_JOB_TIMEOUT_SECONDS", "UAB_STAGE_TIMEOUT_SECONDS",
                     "UAB_JOB_DEADLINE_MONOTONIC_MS", "UAB_ATTEMPT_NONCE"):
            if name in environ:
                env[name] = environ[name]
    for key, value in (extra or {}).items():
        upper = key.upper()
        if upper.startswith(BLOCKED_PREFIXES) or (upper.startswith("UAB_") and upper not in HOST_VARIABLES):
            raise ValueError(f"Environment variable {key} may not be passed to a child process")
        env[key] = str(value)
    return env


def blender_env(context=None, *, temp=None, environ=None, platform=None):
    """Blender's opt-in Python environment contains only fixed isolation flags."""
    env = safe_env(context, temp=temp, environ=environ, platform=platform)
    env.update(PYTHONDONTWRITEBYTECODE="1", PYTHONNOUSERSITE="1", PYTHONUTF8="1")
    return env


# ---------------------------------------------------------------- binary / engine checks

_ARCH_BY_MACHINE = {0x3E: "x64", 0xB7: "arm64", 0x8664: "x64", 0xAA64: "arm64"}
_ARCH_BY_CPU = {0x01000007: "x64", 0x0100000C: "arm64"}


def binary_architectures(path):
    """Architectures of an ELF, PE or Mach-O file; empty when unknown (scripts, wrappers)."""
    try:
        with open(path, "rb") as handle:
            head = handle.read(4096)
            if head[:4] == b"\x7fELF" and len(head) > 20:
                endian = "<" if head[5] == 1 else ">"
                return {a for a in (_ARCH_BY_MACHINE.get(struct.unpack_from(endian + "H", head, 18)[0]),) if a}
            if head[:2] == b"MZ" and len(head) > 0x40:
                handle.seek(struct.unpack_from("<I", head, 0x3C)[0])
                pe = handle.read(6)
                if pe[:4] == b"PE\0\0":
                    return {a for a in (_ARCH_BY_MACHINE.get(struct.unpack("<H", pe[4:6])[0]),) if a}
            if head[:4] in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe"):
                return {a for a in (_ARCH_BY_CPU.get(struct.unpack_from("<I", head, 4)[0]),) if a}
            if head[:4] == b"\xca\xfe\xba\xbe":
                count = struct.unpack_from(">I", head, 4)[0]
                if 0 < count <= 8:
                    return {a for a in (_ARCH_BY_CPU.get(struct.unpack_from(">I", head, 8 + 20 * i)[0]) for i in range(count)) if a}
    except (OSError, struct.error):
        pass
    return set()


def check_executable(path, platform=None, architecture=None):
    """Return warnings for a runnable executable, or raise EngineError (missing, not executable, wrong CPU)."""
    platform, architecture = platform or host_platform(), architecture or host_architecture()
    path = Path(path)
    if not path.is_file():
        raise EngineError(f"Executable not found: {path}")
    if platform != "windows" and not os.access(path, os.X_OK):
        raise EngineError(f"File is not executable: {path}")
    found = binary_architectures(path)
    if not found or architecture in found:
        return []
    if architecture == "arm64" and "x64" in found and platform in ("macos", "windows"):
        return [f"{path.name} is x64 and runs under emulation on this arm64 host"]
    raise EngineError(f"{path.name} is built for {'/'.join(sorted(found))}, this host is {architecture}")


def normalize_app_bundle(path, default_name):
    """A macOS `.app` becomes its inner executable; anything else is returned untouched."""
    path = Path(path)
    if path.suffix.lower() != ".app" or not path.is_dir():
        return path
    name = default_name
    try:
        info = plistlib.loads((path / "Contents" / "Info.plist").read_bytes())
        name = info.get("CFBundleExecutable") or name
    except (OSError, ValueError, plistlib.InvalidFileException):
        pass
    return path / "Contents" / "MacOS" / name


def blender_binary_name(platform=None):
    return "blender.exe" if (platform or host_platform()) == "windows" else "blender"


def bundled_blender(context, explicit=None):
    """Release mode: the manifest's Blender is the only Blender."""
    blender = context.component_path("blender", "executablePath")
    if explicit and os.path.normcase(os.path.realpath(explicit)) != os.path.normcase(str(blender)):
        raise EngineError("A release runtime cannot use a Blender outside the runtime manifest")
    check_executable(blender, context.platform)
    return blender


def system_blender_candidates(platform=None, environ=None):
    platform = platform or host_platform()
    environ = os.environ if environ is None else environ
    if platform == "macos":
        home = Path(environ.get("HOME") or Path.home())
        return [Path("/Applications/Blender.app"), home / "Applications" / "Blender.app"]
    if platform == "linux":
        found = [Path(p) for p in ("/usr/bin/blender", "/usr/local/bin/blender", "/opt/blender/blender", "/snap/bin/blender")]
        found += [Path(directory) / "blender" for directory in environ.get("PATH", "").split(os.pathsep) if directory]
        return found
    return []


def _version_dir(path):
    match = re.search(r"(\d{4}\.\d+\.\d+[a-z]\d+)", str(path))
    return match.group(1) if match else "0"


def default_unity_path(platform=None, environ=None):
    platform = platform or host_platform()
    environ = os.environ if environ is None else environ
    if platform == "windows":
        return Path(_env_get(environ, "ProgramFiles", platform) or r"C:\Program Files") / "Unity/Hub/Editor" / UNITY_BASELINE_VERSION / "Editor/Unity.exe"
    if platform == "macos":
        return Path("/Applications/Unity/Hub/Editor") / UNITY_BASELINE_VERSION / "Unity.app/Contents/MacOS/Unity"
    return Path(environ.get("HOME") or Path.home()) / "Unity/Hub/Editor" / UNITY_BASELINE_VERSION / "Editor/Unity"


def hub_editor_candidates(platform=None, environ=None):
    """Unity Hub's Editor installs, newest version first."""
    platform = platform or host_platform()
    environ = os.environ if environ is None else environ
    if platform == "windows":
        roots = [Path(_env_get(environ, "ProgramFiles", platform) or r"C:\Program Files") / "Unity" / "Hub" / "Editor"]
        pattern = "*/Editor/Unity.exe"
    elif platform == "macos":
        roots = [Path("/Applications/Unity/Hub/Editor")]
        pattern = "*/Unity.app"
    else:
        roots = [Path(environ.get("HOME") or Path.home()) / "Unity" / "Hub" / "Editor"]
        pattern = "*/Editor/Unity"
    found = [p for root in roots if root.is_dir() for p in root.glob(pattern)]
    return sorted(found, key=lambda p: tuple(int(n) for n in re.findall(r"\d+", _version_dir(p))), reverse=True)


def _is_hub(path):
    return bool(re.fullmatch(r"unity[ _-]?hub(\.app|\.exe)?", path.name.lower()))


def normalize_unity_editor(path, platform=None):
    """Accept a binary, a macOS Unity.app, or an install folder; refuse Unity Hub itself."""
    path = Path(path)
    if any(_is_hub(part) for part in (path, *path.parents[:3])):
        raise EngineError("Unity Hub is not a Unity Editor; choose the Editor installed by the Hub")
    if path.is_dir() and path.suffix.lower() != ".app":
        for child in ("Editor/Unity.exe", "Unity.app", "Editor/Unity"):
            if (path / child).exists():
                path = path / child
                break
    return normalize_app_bundle(path, "Unity")


def locate_unity(request=None, context=None, default=None, platform=None):
    request = request or {}
    context = context or current_context()
    platform = platform or context.platform
    explicit_only = context.bundled or request.get("_native_editor_policy") == "explicit-only"
    explicit = request.get("unity_editor") or (None if explicit_only else os.environ.get("UAB_UNITY"))
    if explicit:
        return normalize_unity_editor(explicit, platform)
    if explicit_only:
        return None
    for candidate in hub_editor_candidates(platform):
        if _version_dir(candidate) != UNITY_BASELINE_VERSION:
            continue
        try:
            return normalize_unity_editor(candidate, platform)
        except EngineError:
            continue
    return Path(default) if default else default_unity_path(platform)


_VERSION = re.compile(r"\b(\d{4}\.\d+\.\d+[abfpc]\d+)\b")
_PROBES = {}
_PROBE_LOCK = threading.Lock()


def _windows_product_version(editor):
    try:
        import ctypes
        from ctypes import wintypes
        version = ctypes.windll.version
        size = version.GetFileVersionInfoSizeW(str(editor), None)
        if not size:
            return None
        data = ctypes.create_string_buffer(size)
        if not version.GetFileVersionInfoW(str(editor), 0, size, data):
            return None
        translation = ctypes.c_void_p()
        length = wintypes.UINT()
        if not version.VerQueryValueW(data, "\\VarFileInfo\\Translation", ctypes.byref(translation), ctypes.byref(length)) or length.value < 4:
            return None
        language, codepage = struct.unpack("<HH", ctypes.string_at(translation, 4))
        for name in ("ProductVersion", "FileVersion"):
            value = ctypes.c_void_p()
            if version.VerQueryValueW(data, f"\\StringFileInfo\\{language:04x}{codepage:04x}\\{name}", ctypes.byref(value), ctypes.byref(length)):
                match = _VERSION.search(ctypes.wstring_at(value.value, length.value) if value.value else "")
                if match:
                    return match.group(1)
    except (OSError, AttributeError, ImportError):
        pass
    return None


def _run_version_command(editor):
    from . import process
    context = current_context()
    out = context.work_dir / "tmp"
    out.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix="unity-version-", suffix=".txt", dir=out)
    os.close(descriptor)
    target = Path(name)
    try:
        with target.open("wb") as log:
            process.run_process([str(editor), "-version"], stage="unity-version-probe", budget=30.0,
                                env=safe_env(context, temp=out), stdout=log, stderr=process.subprocess.STDOUT)
        return target.read_text(encoding="utf-8", errors="replace")
    except (OSError, process.ProcessTimeout, process.subprocess.SubprocessError):
        return ""
    finally:
        target.unlink(missing_ok=True)


def _read_version(editor, platform, probe_command):
    if platform == "windows":
        version = _windows_product_version(editor)
        if version:
            return version, "file-version"
    if platform == "macos":
        bundle = next((p for p in editor.parents if p.suffix.lower() == ".app"), None)
        if bundle:
            try:
                info = plistlib.loads((bundle / "Contents" / "Info.plist").read_bytes())
                for key in ("CFBundleVersion", "CFBundleShortVersionString", "UnityBuildNumber"):
                    match = _VERSION.search(str(info.get(key, "")))
                    if match:
                        return match.group(1), "info-plist"
            except (OSError, ValueError, plistlib.InvalidFileException):
                pass
    match = _VERSION.search(probe_command(editor) or "")
    if match:
        return match.group(1), "command"
    return None, None


def probe_unity(editor, platform=None, probe_command=None):
    """Result state: missing | invalid | unavailable | ready (probing is the transient caller-side state)."""
    platform = platform or host_platform()
    editor = Path(editor)
    blank = {"path": str(editor), "version": None, "source": None}
    if not editor.is_file():
        return {**blank, "state": "missing", "reason": f"Unity Editor not found: {editor}"}
    try:
        warnings = check_executable(editor, platform)
    except EngineError as error:
        return {**blank, "state": "invalid", "reason": str(error)}
    facts = editor.stat()
    key = (str(editor), platform, facts.st_dev, facts.st_ino, facts.st_size, facts.st_mtime_ns)
    with _PROBE_LOCK:
        cached = _PROBES.get(key) if probe_command is None else None
    if cached is not None:
        return dict(cached)
    try:
        version, source = _read_version(editor, platform, probe_command or _run_version_command)
    except OSError as error:
        return {**blank, "state": "unavailable", "reason": f"Could not probe Unity: {error}"}
    if not version:
        return {**blank, "state": "invalid", "reason": "This executable did not report a Unity Editor version"}
    result = {**blank, "state": "ready", "version": version, "source": source, "reason": "", "warnings": warnings}
    if probe_command is None:
        with _PROBE_LOCK:
            _PROBES[key] = dict(result)
    return result


# ---------------------------------------------------------------- Unreal

def unreal_binary_dir(platform=None):
    return {"windows": "Win64", "macos": "Mac"}.get(platform or host_platform(), "Linux")


def unreal_cmd_path(engine, platform=None):
    platform = platform or host_platform()
    name = "UnrealEditor-Cmd.exe" if platform == "windows" else "UnrealEditor-Cmd"
    return Path(engine) / "Engine" / "Binaries" / unreal_binary_dir(platform) / name


def engine_root_from_editor(editor):
    """<UE>/Engine/Binaries/<Plat>/... -> <UE>, or None when the layout is not recognised."""
    parts = Path(editor).resolve().parts
    for index, part in enumerate(parts[:-1]):
        if part == "Engine" and parts[index + 1] == "Binaries":
            return Path(*parts[:index])
    return None


def probe_unreal(editor, platform=None):
    """Read the selected installation's Build.version, never infer it from a directory name."""
    platform = platform or host_platform()
    editor = normalize_app_bundle(editor, "UnrealEditor")
    result = {"path": str(editor), "version": None, "source": "Build.version"}
    if not editor.is_file():
        return {**result, "state": "missing", "reason": "Unreal Editor executable is missing"}
    try:
        warnings = check_executable(editor, platform)
        engine = engine_root_from_editor(editor)
        if engine is None:
            raise EngineError("Unreal Editor is outside a recognized engine installation")
        version_file = engine / "Engine/Build/Build.version"
        data = json.loads(version_file.read_text(encoding="utf-8-sig"))
        values = [data[name] for name in ("MajorVersion", "MinorVersion", "PatchVersion")]
        if any(type(value) is not int or value < 0 for value in values):
            raise ValueError("Invalid Unreal Build.version fields")
        return {**result, "state": "ready", "version": ".".join(map(str, values)), "reason": "", "warnings": warnings}
    except (OSError, ValueError, KeyError, EngineError) as error:
        return {**result, "state": "invalid", "reason": str(error)}
