"""Convert a raw Unreal .uasset AnimSequence into a Unity-ready FBX.

The native host finds the .uasset's owning .uproject, checks the user's
project and Unreal Editor approvals, and then runs:

    python -m unityanimbridge.desktop unreal-export --request request.json --out result.json

This module starts UnrealEditor-Cmd headless with unreal/animbridge_export.py.
The FBX files and animbridge-manifest.json go to an app-owned cache folder that
the host then scans like any other Unreal export manifest.

Unreal has to load the user's project to read the asset. Like any editor
launch it may write its own Saved/ (logs, config) and DerivedDataCache/ folders
there; AnimBridge never writes into the project itself and never saves assets.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

from . import process, runtime
from .blender_runner import PROJECT_ROOT, default_data_root
from .default_humanoid import _verified_native_unreal, cache_lock

EXPORT_SCRIPT = PROJECT_ROOT / "unreal/animbridge_export.py"
MANIFEST_NAME = "animbridge-manifest.json"

# Commandlet flags for an unattended, windowless run. -nullrhi skips GPU
# initialisation: exporting animation curves needs no rendering. The export
# script needs Python, which many projects do not enable, so enable it here
# for this run only (the .uproject file is not changed).
HEADLESS_FLAGS = ("-unattended", "-nop4", "-nosplash", "-nullrhi", "-nosound", "-stdout", "-FullStdOutLogOutput",
                  "-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities")

# Exporting animation needs no platform SDKs. Without this, startup runs
# Engine/Build/BatchFiles/Build.bat -Mode=ValidatePlatforms, which can wait
# forever on its lock when Unreal runs without a console.
ENGINE_ENVIRONMENT = {"UE_SKIP_UBT_SDK_SETUP": "1"}

# /Game/A/B/Name.Name or /<Plugin>/A/Name.Name, as computed by the native scan.
OBJECT_PATH = re.compile(r"/[A-Za-z0-9_]+(?:/[^/;\x00-\x1f]+)+")


class ExportError(ValueError):
    """A conversion problem the user can act on; reported instead of raised."""

    def __init__(self, message, log=None):
        super().__init__(message)
        self.log = log


def validate_object_path(object_path):
    """Return object_path when it names a package's main object (Pkg.Pkg)."""
    if not isinstance(object_path, str) or not OBJECT_PATH.fullmatch(object_path):
        raise ExportError("Invalid Unreal object path: " + repr(object_path))
    package, _, name = object_path.rpartition("/")[2].partition(".")
    if not package or package != name:
        raise ExportError("Unreal object path must name the asset's main object: " + object_path)
    return object_path


def export_key(source, object_path):
    """Stable folder name, so converting the same asset again replaces its FBX."""
    return hashlib.sha256(f"{source}|{object_path}".encode("utf-8")).hexdigest()[:16]


def headless_command(editor, project, bootstrap):
    return [str(editor), str(project), "-run=pythonscript", "-script=" + _command_line_path(bootstrap), *HEADLESS_FLAGS]


def _command_line_path(path):
    # Unreal reads "-script=<value>" up to the first space, even when quoted.
    text = str(path)
    if not re.search(r"\s", text):
        return text
    if os.name == "nt":
        import ctypes
        buffer = ctypes.create_unicode_buffer(32768)
        if ctypes.windll.kernel32.GetShortPathNameW(text, buffer, len(buffer)) and not re.search(r"\s", buffer.value):
            return buffer.value
    raise ExportError("The app cache path contains spaces that Unreal's command line cannot accept: " + text)


def _existing_file(request, key, suffix):
    value = request.get(key)
    if not value:
        raise ExportError("Missing " + key)
    try:
        path = Path(value).resolve(strict=True)
    except OSError as error:
        raise ExportError(f"File does not exist: {value}") from error
    if not path.is_file() or path.suffix.lower() != suffix:
        raise ExportError(f"Expected a {suffix} file: {path}")
    return path


def _editor(request, context):
    if context.bundled or request.get("_native_editor_policy") == "explicit-only":
        # The host attaches the capability only after its Editor grant checks.
        return _verified_native_unreal(request, context)[2]
    # Development CLI: trust the explicitly named Editor, still version-checked.
    root = runtime.engine_root_from_editor(request.get("unreal_editor") or "")
    if root is None:
        raise ExportError("Choose UnrealEditor or UnrealEditor-Cmd from an Unreal Engine installation")
    editor = runtime.unreal_cmd_path(root, context.platform)
    if runtime.probe_unreal(editor, context.platform).get("state") != "ready":
        raise ExportError("Unreal Editor could not be verified: " + str(editor))
    return editor


def _cache_root(request):
    root = Path(request.get("cache") or default_data_root() / "helper").resolve()
    if root == Path(root.anchor):
        raise ExportError("A drive root cannot be the conversion cache")
    return root / "unreal-export"


def _write_bootstrap(path, object_path, out):
    # Arguments travel inside a script file, never through Unreal's command line.
    argv = [str(EXPORT_SCRIPT), "--assets", object_path, "--out", str(out)]
    path.write_text("import runpy, sys\nsys.argv = " + json.dumps(argv) +
                    "\nrunpy.run_path(sys.argv[0], run_name='__main__')\n", encoding="utf-8")


def _verified_manifest(out, exit_code, log):
    # The manifest, not the exit code, decides success: Unreal returns 1 when the
    # user's project logs unrelated load errors even though the export worked.
    manifest_path = out / MANIFEST_NAME
    if not manifest_path.is_file():
        raise ExportError(f"Unreal Editor finished without an export manifest (exit {exit_code}); see {log}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    errors = [str(error) for error in manifest.get("errors") or []]
    if errors:
        raise ExportError("; ".join(errors))
    clips = manifest.get("clips") or []
    if not clips:
        raise ExportError("Unreal Editor exported no animation")
    for clip in clips:
        fbx = Path(clip["fbx"]).resolve()
        if not fbx.is_relative_to(out.resolve()) or not fbx.is_file() or fbx.stat().st_size == 0:
            raise ExportError("Unreal Editor did not write a valid FBX: " + str(fbx))
        # Relative names keep the manifest valid after the folder is moved.
        clip["fbx"] = fbx.name
    return manifest


def _replace_directory(staged, final):
    """Swap in the new export; a previous one may still be open in a preview."""
    previous = final.with_name(final.name + ".previous-" + uuid.uuid4().hex)
    if final.exists():
        try:
            final.rename(previous)
        except OSError as error:
            raise ExportError("The previous conversion is in use; close its preview and try again") from error
    staged.rename(final)
    shutil.rmtree(previous, ignore_errors=True)


def convert(request):
    context = runtime.current_context()
    source = _existing_file(request, "source", ".uasset")
    project = _existing_file(request, "unreal_project", ".uproject")
    if not source.is_relative_to(project.parent):
        raise ExportError("The .uasset is not inside the approved Unreal project")
    object_path = validate_object_path(request.get("unreal_object_path"))
    editor = _editor(request, context)
    base = _cache_root(request)
    key = export_key(source, object_path)
    final = base / key
    with cache_lock(base / (key + ".lock")):
        staging = base / (".staging-" + uuid.uuid4().hex)
        out = staging / key
        out.mkdir(parents=True)
        bootstrap = staging / "animbridge_export_bootstrap.py"
        _write_bootstrap(bootstrap, object_path, out)
        log = staging / "unreal.log"
        (staging / "temp").mkdir()
        with log.open("wb") as output:
            completed = process.run_process(headless_command(editor, project, bootstrap), stage="unreal",
                                            budget=process.stage_budget(request),
                                            env=runtime.safe_env(context, extra=ENGINE_ENVIRONMENT, temp=staging / "temp"),
                                            stdout=output, stderr=subprocess.STDOUT)
        try:
            manifest = _verified_manifest(out, completed.returncode, log)
        except ExportError as error:
            # The staging folder (with Unreal's log) stays for diagnosis; Clear cache removes it.
            raise ExportError(str(error), log=str(log)) from error
        manifest.update(source_uasset=str(source), unreal_project=str(project), unreal_exit_code=completed.returncode)
        (out / MANIFEST_NAME).write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        shutil.copyfile(log, out / "unreal.log")
        _replace_directory(out, final)
        shutil.rmtree(staging, ignore_errors=True)
    clips = [{key: clip.get(key) for key in ("name", "source_asset", "fps", "frames", "duration")} for clip in manifest["clips"]]
    return {"status": "ok", "manifest": str(final / MANIFEST_NAME), "outputs": [str(final / clip["fbx"]) for clip in manifest["clips"]],
            "clips": clips, "object_path": object_path, "log": str(final / "unreal.log"), "unreal_exit_code": completed.returncode}


def unreal_export(request):
    """Worker entry point: user-actionable problems become a 'blocked' result."""
    try:
        return convert(request)
    except ExportError as error:
        return {"status": "blocked", "code": "unrealExportFailed", "reason": str(error), "log": error.log or ""}
