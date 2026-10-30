"""Default preview character.

The app ships the Mocapture humanoid FBX and uses it whenever no character is
chosen. When the host approves an Unreal Engine install, the user's own UE5 Manny
can be prepared instead; Epic's files are never distributed. Only Epic's known
template data files enter an app-owned scratch project. No user project, package
scripts or third-party plugins are loaded or modified.
"""
from __future__ import annotations

import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

from . import process, runtime
from .blender_runner import PROJECT_ROOT, default_data_root
from .meta_parser import meta_path_for


HUMAN_MAPPING = {
    "Hips": "pelvis", "Spine": "spine_01", "Chest": "spine_03", "UpperChest": "spine_05",
    "Neck": "neck_01", "Head": "head",
    **{human_side + human: bone + "_" + side
       for human_side, side in (("Left", "l"), ("Right", "r"))
       for human, bone in (("Shoulder", "clavicle"), ("UpperArm", "upperarm"),
                           ("LowerArm", "lowerarm"), ("Hand", "hand"),
                           ("UpperLeg", "thigh"), ("LowerLeg", "calf"),
                           ("Foot", "foot"), ("Toes", "ball"))},
}


# The bundled character uses Unity humanoid bone names directly.
BUNDLED_CHARACTER = PROJECT_ROOT / "assets/characters/Default_Humanoid.fbx"
BUNDLED_NAME = "Mocapture Humanoid"
BUNDLED_MAPPING = {name: name for name in (
    "Hips", "Spine", "Chest", "Neck", "Head",
    *(side + part for side in ("Left", "Right")
      for part in ("Shoulder", "UpperArm", "LowerArm", "Hand", "UpperLeg", "LowerLeg", "Foot", "Toes")))}


def bundled_default():
    """Return the shipped default character, or None when it is missing."""
    model = BUNDLED_CHARACTER
    if not model.is_file():
        return None
    return {"model": str(model.resolve()), "name": BUNDLED_NAME,
            "human_mapping": dict(BUNDLED_MAPPING), "bundled": True}


def fbx_has_skinned_mesh(path):
    """True when a binary FBX stores a Skin deformer, i.e. it carries a skinned character.

    Only node headers and the first three properties of each Objects/Deformer node are
    read. ASCII FBX, other FBX layouts and unreadable files report False.
    """
    try:
        with open(path, "rb") as handle:
            if handle.read(20) != b"Kaydara FBX Binary  ":
                return False
            handle.seek(23)
            wide = struct.unpack("<I", handle.read(4))[0] >= 7500
            head = "<QQQB" if wide else "<IIIB"
            size = struct.calcsize(head)

            def node(position):
                handle.seek(position)
                raw = handle.read(size)
                if len(raw) < size:
                    return None
                end, count, length, name_length = struct.unpack(head, raw)
                if end <= position:
                    return None
                name = handle.read(name_length)
                return end, count, length, name, handle.tell()

            def deformer_type(start, count):
                handle.seek(start)
                for index in range(min(count, 3)):
                    kind = handle.read(1)
                    if kind == b"L":
                        handle.read(8)
                    elif kind == b"S":
                        value = handle.read(struct.unpack("<I", handle.read(4))[0])
                        if index == 2:
                            return value
                    else:
                        return None
                return None

            position = 27
            while True:
                top = node(position)
                if top is None:
                    return False
                end, _, length, name, start = top
                if name == b"Objects":
                    child_position = start + length
                    while child_position < end:
                        child = node(child_position)
                        if child is None:
                            break
                        child_end, count, _, child_name, child_start = child
                        if child_name == b"Deformer" and deformer_type(child_start, count) == b"Skin":
                            return True
                        child_position = child_end
                    return False
                position = end
    except (OSError, struct.error):
        return False


_AVATAR_SETUP = re.compile(r"^\s*avatarSetup:\s*(\d+)", re.M)
_AVATAR_SOURCE = re.compile(r"^\s*lastHumanDescriptionAvatarSource:\s*\{[^}]*?guid:\s*([0-9a-f]{32})", re.M)
_META_GUID = re.compile(r"^guid:\s*([0-9a-f]{32})", re.M)


def _read_text(path, limit):
    try:
        with open(path, "rb") as handle:
            return handle.read(limit).decode("utf-8", "replace")
    except OSError:
        return ""


def _meta_avatar(asset):
    """(avatarSetup, source-avatar GUID) from an FBX's importer metadata."""
    text = _read_text(meta_path_for(asset), 4 << 20)
    setup, source = _AVATAR_SETUP.search(text), _AVATAR_SOURCE.search(text)
    return (int(setup.group(1)) if setup else 0), (source.group(1) if source else None)


def _avatar_owner(source, guid, packages_root):
    """The skinned FBX that defines a copied avatar, found by GUID inside the clip's own package."""
    try:
        packages = Path(packages_root).resolve()
        parts = source.relative_to(packages).parts
        if len(parts) < 2:
            return None
        for meta in sorted((packages / parts[0]).rglob("*.meta")):
            owned = _META_GUID.search(_read_text(meta, 256)) if meta.name.lower().endswith(".fbx.meta") else None
            if owned and owned.group(1) == guid:
                owner = meta.with_suffix("").resolve()
                if owner.is_file() and owner.is_relative_to(packages) and _meta_avatar(owner)[0] == 1 \
                        and fbx_has_skinned_mesh(owner):
                    return owner
                return None
    except (OSError, ValueError):
        return None
    return None


def embedded_default(request, packages_root=None):
    """Prefer the character the clip was authored with over a stand-in character.

    Motion authored together with its character previews faithfully on that character.
    A stand-in such as Manny can distort it when the stand-in's rest pose is not a T-pose.
    A clip whose importer builds its avatar from its own skinned model uses itself. A clip
    that copies another asset's avatar uses that asset, looked up by GUID, but only inside
    the app-extracted package cache because the host granted no access elsewhere.
    """
    source = Path(request["source"]).resolve() if request.get("source") else None
    if source is None or source.suffix.lower() != ".fbx" or not request.get("humanoid"):
        return None
    setup, guid = _meta_avatar(source)
    model = None
    if setup == 1:
        model = source if fbx_has_skinned_mesh(source) else None
    elif setup == 2 and guid:
        if packages_root is None:
            try:
                packages_root = runtime.current_context().cache_dir / "packages"
            except runtime.RuntimeContextError:
                return None
        model = _avatar_owner(source, guid, packages_root)
    if model is None:
        return None
    return {"model": str(model), "name": "Embedded model (" + model.name + ")",
            "human_mapping": {}, "embedded": True}


class UnrealApprovalRequired(ValueError):
    """The host must obtain/revalidate its narrowly scoped Unreal capability."""


def _verified_native_unreal(request, context):
    cap = request.get("_native_unreal_capability")
    if not isinstance(cap, dict) or cap.get("version") != runtime.UNREAL_BASELINE_VERSION:
        raise UnrealApprovalRequired("Select and approve Unreal Engine " + runtime.UNREAL_BASELINE_VERSION + " to prepare the local Manny preview")
    try:
        root = Path(cap["engineRoot"]).resolve(strict=True)
        editor = runtime.unreal_cmd_path(root, context.platform)
        version = root / "Engine/Build/Build.version"
        template = root / "Templates/TemplateResources/High/Characters/Content/Mannequins/Meshes"
        expected_templates = {template / "SKM_Manny_Simple.uasset", template / "SK_Mannequin.uasset"}
        records = cap["templates"]
        if not isinstance(records, list) or len(records) != 2 or {Path(record["path"]) for record in records} != expected_templates:
            raise ValueError("Template capability does not match the two approved Manny files")
        selected = request.get("unreal_editor")
        if not selected or Path(selected).resolve(strict=True) != editor:
            raise ValueError("The selected Editor differs from the approved command executable")
        for record, expected in [(cap["editor"], editor), (cap["versionFile"], version), *[(record, Path(record["path"])) for record in records]]:
            digest = record["sha256"]
            path = Path(record["path"])
            if (not re.fullmatch(r"[0-9a-f]{64}", digest) or path != expected or
                    path.resolve(strict=True) != expected or not expected.is_relative_to(root) or
                    file_digest(path) != digest):
                raise ValueError("An approved Unreal component changed; select the installation again")
        facts = runtime.probe_unreal(editor, context.platform)
        if facts.get("state") != "ready" or facts.get("version") != cap["version"]:
            raise ValueError("The approved Unreal installation version changed")
        return root, template, editor
    except (OSError, KeyError, TypeError, ValueError, runtime.EngineError) as error:
        raise UnrealApprovalRequired(str(error)) from error


def file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


@contextlib.contextmanager
def cache_lock(path, timeout=660):
    """An OS-released lock also serializes separate native helper processes."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as handle:
        if path.stat().st_size == 0:
            handle.write(b"0")
            handle.flush()
        deadline = time.monotonic() + timeout
        while True:
            try:
                handle.seek(0)
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except (BlockingIOError, OSError):
                if time.monotonic() >= deadline:
                    raise TimeoutError("Timed out waiting for the local preview cache")
                time.sleep(.1)
        try:
            yield
        finally:
            handle.seek(0)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


@contextlib.contextmanager
def cache_slot(root, slots=2, timeout=660):
    """Take the first free project slot without queueing behind a busy slot."""
    root = Path(root)
    deadline = time.monotonic() + timeout
    while True:
        for index in range(slots):
            guard = cache_lock(root / ("slot-" + str(index) + ".lock"), timeout=0)
            try:
                guard.__enter__()
            except TimeoutError:
                continue
            try:
                yield root / ("slot-" + str(index))
            finally:
                guard.__exit__(None, None, None)
            return
        if time.monotonic() >= deadline:
            raise TimeoutError("Timed out waiting for a local Unity preview slot")
        time.sleep(.1)


def _engines(request):
    candidates = []
    context = runtime.current_context()
    platform = context.platform
    # Release runtimes ignore UAB_* overrides; only the request may name an engine.
    explicit_only = context.bundled or request.get("_native_editor_policy") == "explicit-only"
    dev_env = (lambda name: None) if explicit_only else os.environ.get
    for value in (request.get("unreal_editor"), dev_env("UAB_UNREAL_EDITOR")):
        if value:
            root = runtime.engine_root_from_editor(value)
            if root is not None:
                candidates.append(root)
    if explicit_only:
        # The native host writes this private request only after resolving an
        # engine grant. Discovery does not create execution authority.
        if request.get("_native_unreal_capability") is not None:
            yield _verified_native_unreal(request, context)[0]
        return
    explicit = dev_env("UAB_UNREAL_ENGINE")
    if explicit:
        candidates.append(Path(explicit).resolve())
    # Epic's launcher records custom installation drives; avoid a drive scan.
    if platform == "windows":
        metadata = Path(os.environ.get("ProgramData", r"C:\ProgramData")) / "Epic/UnrealEngineLauncher/LauncherInstalled.dat"
    elif platform == "macos":
        metadata = Path("/Users/Shared/Epic Games/UnrealEngineLauncher/LauncherInstalled.dat")
    else:
        metadata = Path.home() / ".config/Epic/UnrealEngineLauncher/LauncherInstalled.dat"
    if metadata.is_file():
        try:
            records = json.loads(metadata.read_text(encoding="utf-8-sig")).get("InstallationList", [])
            records = sorted((record for record in records if isinstance(record, dict) and
                re.fullmatch(r"UE_5\.\d+", str(record.get("AppName", "")))),
                key=lambda record: tuple(int(n) for n in re.findall(r"\d+", record["AppName"])), reverse=True)
            candidates.extend(Path(record["InstallLocation"]).resolve() for record in records if record.get("InstallLocation"))
        except (ValueError, OSError, TypeError):
            pass
    if platform == "windows":
        roots = [Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Epic Games"]
    elif platform == "macos":
        roots = [Path("/Users/Shared/Epic Games")]
    else:
        roots = [Path.home() / "UnrealEngine", Path("/opt/UnrealEngine"), Path("/opt/Epic")]
    for installed in roots:
        if installed.is_dir():
            if (installed / "Engine").is_dir():
                candidates.append(installed)
            candidates.extend(sorted(installed.glob("UE_5.*"), key=lambda p: tuple(
                int(n) for n in re.findall(r"\d+", p.name)), reverse=True))
    seen = set()
    for candidate in candidates:
        if candidate not in seen:
            seen.add(candidate)
            yield candidate


def resolve_default(request):
    """Return a real FBX + explicit Unity mapping, or an actionable failure."""
    context = runtime.current_context()
    explicit_only = context.bundled or request.get("_native_editor_policy") == "explicit-only"
    # Manny is used only when an Unreal Editor was chosen; otherwise the shipped character.
    wants_manny = request.get("_native_unreal_capability") is not None or bool(request.get("unreal_editor"))
    explicit = None if explicit_only else os.environ.get("UAB_DEFAULT_HUMANOID_MODEL")
    if explicit:
        model = Path(explicit).resolve()
        if not model.is_file() or model.suffix.lower() != ".fbx":
            raise ValueError("UAB_DEFAULT_HUMANOID_MODEL must identify an existing Manny FBX")
        return {"model": str(model), "name": "UE5 Mannequin (Manny)", "human_mapping": dict(HUMAN_MAPPING)}
    if not wants_manny:
        bundled = bundled_default()
        if bundled is not None:
            return bundled
    if explicit_only:
        _verified_native_unreal(request, context)
    found = None
    for engine in _engines(request):
        template = engine / "Templates/TemplateResources/High/Characters/Content/Mannequins/Meshes"
        editor = runtime.unreal_cmd_path(engine, context.platform)
        if (editor.is_file() and (template / "SKM_Manny_Simple.uasset").is_file() and
                (template / "SK_Mannequin.uasset").is_file() and
                runtime.probe_unreal(editor, context.platform).get("version") == runtime.UNREAL_BASELINE_VERSION):
            found = engine, template, editor
            break
    if found is None:
        raise ValueError("UE5 Mannequin preview needs an installed Unreal Engine 5 template verified as " + runtime.UNREAL_BASELINE_VERSION + ", or a chosen character FBX")
    engine, template, editor = found
    script = PROJECT_ROOT / "unreal/animbridge_default_mannequin.py"
    signature = hashlib.sha256(json.dumps({"version": 1, "engine": str(engine),
        "editor_sha": file_digest(editor), "engine_version": runtime.UNREAL_BASELINE_VERSION,
        "mesh": file_digest(template / "SKM_Manny_Simple.uasset"),
        "skeleton": file_digest(template / "SK_Mannequin.uasset"),
        "exporter": file_digest(script)}, sort_keys=True).encode()).hexdigest()
    cache = Path(request.get("cache") or default_data_root() / "helper").resolve()
    if cache == Path(cache.anchor) or any((ancestor / "Assets").is_dir() and (ancestor / "ProjectSettings").is_dir()
                                        for ancestor in (cache, *cache.parents)):
        raise ValueError("Choose an app cache directory, not an existing Unity project or drive root")
    base = cache / "default-humanoid"
    work = base / signature
    model = work / "UE5_Manny.fbx"
    report = work / "export-result.json"
    with cache_lock(base / (signature + ".lock")):
        if explicit_only:
            _verified_native_unreal(request, context)
        ready = False
        if model.is_file() and model.stat().st_size and report.is_file():
            try:
                saved = json.loads(report.read_text(encoding="utf-8-sig"))
                ready = saved.get("status") == "ok" and saved.get("sha256") == file_digest(model)
            except (ValueError, OSError):
                pass
        if not ready:
            target = work / "project/Content/Characters/Mannequins/Meshes"
            target.mkdir(parents=True, exist_ok=True)
            for name in ("SKM_Manny_Simple.uasset", "SK_Mannequin.uasset"):
                shutil.copyfile(template / name, target / name)
                if explicit_only:
                    expected = next(record["sha256"] for record in request["_native_unreal_capability"]["templates"] if Path(record["path"]).name == name)
                    if file_digest(target / name) != expected:
                        raise UnrealApprovalRequired("A Manny template changed during its isolated copy")
            project = work / "project/AnimBridgeMannequin.uproject"
            project.write_text(json.dumps({"FileVersion": 3, "EngineAssociation": "", "Plugins": [
                {"Name": "PythonScriptPlugin", "Enabled": True},
                {"Name": "EditorScriptingUtilities", "Enabled": True}]}), encoding="utf-8")
            input_file = work / "export-request.json"
            input_file.write_text(json.dumps({"output": str(model), "report": str(report)}), encoding="utf-8")
            command = [str(editor), str(project), "-unattended", "-nop4", "-nosplash", "-nosound", "-AllowCommandletRendering", "-RenderOffscreen"]
            if context.platform == "windows":
                command.append("-d3d11")
            command += ["-run=pythonscript", "-script=" + str(script), "-animBridgeMannequin=" + str(input_file),
                        "-abslog=" + str(work / "unreal.log")]
            (work / "temp").mkdir(parents=True, exist_ok=True)
            if explicit_only:
                _verified_native_unreal(request, context)
            with (work / "unreal-process.log").open("wb") as log:
                completed = process.run_process(command, stage="unreal", budget=process.stage_budget(request),
                                                # Same Build.bat lock-wait fix as unreal_export.ENGINE_ENVIRONMENT.
                                                env=runtime.safe_env(context, extra={"UE_SKIP_UBT_SDK_SETUP": "1"},
                                                                     temp=work / "temp"),
                                                stdout=log, stderr=subprocess.STDOUT)
            if completed.returncode != 0 or not model.is_file() or not report.is_file():
                raise ValueError(f"Could not prepare UE5 Mannequin; see {work / 'unreal.log'}")
            exported = json.loads(report.read_text(encoding="utf-8-sig"))
            if exported.get("status") != "ok":
                raise ValueError(exported.get("reason", "UE5 Mannequin export failed"))
        if explicit_only:
            _verified_native_unreal(request, context)
    return {"model": str(model), "name": "UE5 Mannequin (Manny)", "human_mapping": dict(HUMAN_MAPPING),
            "source": str(template / "SKM_Manny_Simple.uasset"), "locally_prepared": True}
