"""JSON IPC for the local desktop browser: real previews and isolated Unity evaluation.

python -m unityanimbridge.desktop preview --request request.json --out result.json
No package scripts or pre-existing Unity projects are executed.
"""
from __future__ import annotations

import argparse
import contextlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
import hashlib

import numpy as np

from . import process, runtime, unreal_export
from .anim_parser import parse_anim
from .blender_runner import PROJECT_ROOT, default_data_root, find_blender
from .coords import trs_to_mat
from .meta_parser import meta_path_for, read_model_meta
from .sampler import sample_clip
from .unity_yaml import is_text_yaml

HELPER_VERSION = "1.0"
# Operations whose failures are returned as a readable "blocked" result
# (exit 0) instead of a bare non-zero exit, so the host can show the reason.
REPORTED_OPERATIONS = ("preview", "unreal-export")
DEFAULT_UNITY = runtime.default_unity_path()


@process.staged("parse-json")
def _read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def _write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, allow_nan=False), encoding="utf-8")
    temporary.replace(path)


def _source(request, key="source"):
    value = request.get(key)
    if not value:
        raise ValueError(f"Missing {key}")
    path = Path(value).resolve()
    if not path.is_file():
        raise ValueError(f"File does not exist: {path}")
    return path


def _work(request, operation):
    root = Path(request.get("cache") or default_data_root() / "helper").resolve()
    # A user-selected helper location is a cache, never permission to run an
    # existing project's Editor scripts. Reject projects and their descendants.
    for ancestor in (root, *root.parents):
        if (ancestor / "Assets").is_dir() and (ancestor / "ProjectSettings").is_dir():
            raise ValueError("Choose an app cache directory, not an existing Unity project")
    if root == Path(root.anchor):
        raise ValueError("A drive root cannot be the desktop cache")
    work = root / operation / uuid.uuid4().hex
    work.mkdir(parents=True)
    _write(work / ".animbridge-owned.json", {"schema_version": 1, "operation": operation})
    return work


def _blender(source, operation, work, request):
    request_path, output = work / "blender_request.json", work / "blender_result.json"
    job = {"source": str(source), "operation": operation, "clip": request.get("clip"),
           "fps": request.get("fps"), "max_frames": request.get("max_frames", 2000),
           "max_point_frames": request.get("max_point_frames", 150000),
           "include_mesh": bool(request.get("include_mesh", False)),
           **{key: request[key] for key in ("first_frame", "last_frame") if key in request}}
    _write(request_path, job)
    command = [str(find_blender(request.get("blender"))), "-b", "--factory-startup", "--python-use-system-env", "--python-exit-code", "1",
               "--python", str(PROJECT_ROOT / "blender" / "animbridge_preview.py"), "--", str(request_path), str(output)]
    _write(work / "blender_command.json", command)
    temp = work / "temp"
    temp.mkdir(parents=True, exist_ok=True)
    with (work / "blender.log").open("w", encoding="utf-8", errors="replace") as log:
        completed = process.run_process(command, stage="blender", budget=process.stage_budget(request),
                                        env=runtime.blender_env(temp=temp), stdout=log, stderr=subprocess.STDOUT)
    if completed.returncode != 0 or not output.is_file():
        raise ValueError(f"Blender preview failed; see {work / 'blender.log'}")
    result = _read(output)
    result["log"] = str(work / "blender.log")
    if result["status"] == "error":
        raise ValueError(result.get("reason", "Blender could not read this FBX"))
    return result


def _select_baked(data, name=None):
    clips = [clip for clip in data.get("clips", []) if not name or clip["name"] == name]
    if len(clips) != 1:
        raise ValueError("Choose one unambiguous baked clip")
    clip = clips[0]
    fps = float(clip.get("fps") or data.get("fps") or 30)
    if not math.isfinite(fps) or fps <= 0:
        raise ValueError("Invalid baked fps")
    return clip, fps


def inspect(request):
    source = _source(request)
    result = {"schema_version": 1, "status": "ok", "source": str(source), "name": source.stem,
              "type": "unknown", "route": None, "fps": None, "duration": None,
              "capabilities": {"preview": False, "extract": True, "bake": False}, "reasons": [], "warnings": []}
    if source.suffix.lower() == ".anim":
        if not is_text_yaml(source):
            result.update(type="binary", route="B")
            result["reasons"].append("Binary AnimationClip requires the isolated Unity helper and a compatible model")
            return result
        clip = _parse(source)
        limits = clip.key_range()
        start = clip.start_time if clip.start_time is not None else (limits[0] if limits else 0)
        stop = clip.stop_time if clip.stop_time is not None else (limits[1] if limits else start)
        result.update(name=clip.name, type="humanoid" if clip.is_humanoid else "legacy" if clip.legacy else "generic",
                      route="B" if clip.is_humanoid else "A", fps=clip.sample_rate, duration=max(0, stop - start),
                      loop=clip.loop, paths=list(clip.channels), channels={p: list(c) for p, c in clip.channels.items()},
                      humanoid=clip.is_humanoid, warnings=clip.warnings + clip.unsupported)
        checked = validate(request) if request.get("skeleton") and not clip.is_humanoid else None
        ready = bool(checked and checked.get("valid") and clip.channels)
        if checked:
            result["validation"] = checked
        result["capabilities"].update(preview=ready, bake=ready)
        if clip.is_humanoid:
            result["reasons"].append("A Unity-validated Humanoid Avatar and isolated helper are required")
        elif not clip.channels:
            result["reasons"].append("No supported Transform animation curves; controller and parameter evaluation are outside preview scope")
        elif not ready:
            result["reasons"].append("Connect a skeleton and validate the Transform path mapping")
        return result
    if source.suffix.lower() == ".json":
        data = _read(source)
        if data.get("uab_baked") != 1:
            raise ValueError("JSON is not a UnityAnimBridge baked clip")
        clip, fps = _select_baked(data, request.get("clip"))
        result.update(name=clip["name"], type="baked", route="B-baked", fps=fps,
                      duration=float(clip.get("duration", (clip["frames"] - 1) / fps)), paths=list(clip["tracks"]),
                      frames=clip["frames"], loop=clip.get("loop", False), clips=[{"name": c["name"]} for c in data["clips"]])
        result["capabilities"].update(preview=True, bake=bool(request.get("skeleton")))
        return result
    if source.suffix.lower() != ".fbx":
        result["reasons"].append("Preview supports FBX, text AnimationClip and Unity baked JSON")
        return result
    work = _work(request, "inspect")
    model = _blender(source, "inspect", work, request)
    result.update(type="fbx", route="C", fps=model["fps"], bones=model["bones"], clips=model["clips"], facts=model["facts"],
                  log=model["log"], warnings=model["warnings"])
    if model["clips"]:
        result["duration"] = model["clips"][0]["duration"] if len(model["clips"]) == 1 else None
    result["capabilities"].update(preview=bool(model["bones"]), bake=bool(model["clips"]))
    meta = meta_path_for(source)
    if meta.is_file():
        settings = read_model_meta(meta)
        result.update(humanoid=settings.animation_type == 3, animation_type=settings.animation_type,
                      rig_type={1: "legacy", 2: "generic", 3: "humanoid"}.get(settings.animation_type, "unknown"),
                      meta_clips=[clip.__dict__ for clip in settings.clips])
        result["warnings"].extend(settings.warnings)
        if settings.clips:
            result["clips"] = [{"name": clip.name, "take_name": clip.take_name,
                                "first_frame": clip.first_frame, "last_frame": clip.last_frame,
                                "duration": (clip.last_frame - clip.first_frame) / model["fps"]} for clip in settings.clips]
            chosen = [clip for clip in result["clips"] if not request.get("clip") or clip["name"] == request["clip"]]
            result["duration"] = chosen[0]["duration"] if len(chosen) == 1 else None
    else:
        result.update(humanoid=None, rig_type="unknown")
    return result


@process.staged("sample-baked")
def _baked_preview(source, request):
    data = _read(source)
    if data.get("uab_baked") != 1:
        raise ValueError("JSON is not a UnityAnimBridge baked clip")
    clip, fps = _select_baked(data, request.get("clip"))
    tracks, rest = clip["tracks"], data.get("rest", {})
    paths = sorted(set(tracks) | set(rest), key=lambda path: (path.count("/"), len(path), path))
    indices = {path: index for index, path in enumerate(paths)}
    count = int(clip["frames"])
    if count < 1 or count > 10000:
        raise ValueError("Baked preview requires 1..10000 frames")
    bones, matrices = [], []
    for path in paths:
        parent = path.rsplit("/", 1)[0] if "/" in path else "" if path and "" in indices else None
        if parent is not None and parent not in indices:
            raise ValueError(f"Baked hierarchy is missing parent {parent!r}")
        bones.append({"name": path.rsplit("/", 1)[-1] or "Model", "path": path, "parent": indices[parent] if parent is not None else -1})
        components = []
        for key, default in (("t", [0, 0, 0]), ("r", [0, 0, 0, 1]), ("s", [1, 1, 1])):
            values = tracks.get(path, {}).get(key)
            if values is None:
                values = [rest.get(path, {}).get(key, default)] * count
            array = np.asarray(values, dtype=float)
            if array.shape != (count, len(default)) or not np.all(np.isfinite(array)):
                raise ValueError(f"Invalid baked track {path!r}.{key}")
            components.append(array)
        local = trs_to_mat(*components)
        matrices.append(matrices[indices[parent]] @ local if parent is not None else local)
    frames = np.stack([m[:, :3, 3] * [-1, 1, 1] for m in matrices], axis=1)
    if not np.all(np.isfinite(frames)):
        raise ValueError("Baked hierarchy contains invalid transformations")
    return {"schema_version": 1, "status": "ready", "kind": "bone-motion", "source": str(source),
            "clip": clip["name"], "fps": fps, "duration": float(clip.get("duration", (count - 1) / fps)),
            "frame_count": count, "bones": bones, "frames": frames.tolist(),
            "coordinate_system": "right-handed-y-up-meters", "preview_scope": "baked-transform-hierarchy",
            "bounds": {"min": frames.min(axis=(0, 1)).tolist(), "max": frames.max(axis=(0, 1)).tolist()},
            "warnings": ["Baked Transform hierarchy includes model and non-bone nodes; no mesh data is available"]}


def _parse(source):
    with process.stage("parse"):
        return parse_anim(source)


def _convert_preview(source, request, work):
    from .cli import main as convert
    arguments = ["convert"]
    arguments += ["--baked", str(source)] if source.suffix.lower() == ".json" else [str(source)]
    if source.suffix.lower() != ".fbx":
        arguments += ["--skeleton", str(_source(request, "skeleton"))]
    arguments += ["-o", str(work / "converted")]
    if request.get("clip"):
        arguments += ["--clip", str(request["clip"])]
    if request.get("root_to_bone"):
        arguments += ["--root-to-bone", str(request["root_to_bone"])]
    if request.get("blender"):
        arguments += ["--blender", str(request["blender"])]
    if request.get("bone_map") or request.get("mapping"):
        mapping_path = work / "bone_map.json"
        _write(mapping_path, request.get("bone_map") or request["mapping"])
        arguments += ["--bone-map", str(mapping_path)]
    if request.get("fps"):
        arguments += ["--fps", str(request["fps"])]
    # Converter's scratch files must remain inside this app-owned work item.
    with runtime.scratch(work / "converter_work"):
        with (work / "convert.log").open("w", encoding="utf-8") as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            code = convert(arguments)
    if code:
        raise ValueError(f"Clip conversion failed (code {code}); see {work / 'convert.log'}")
    manifest = _read(work / "converted" / "manifest.json")
    clips = [clip for clip in manifest["clips"] if clip["status"] in ("ok", "warning")]
    if len(clips) != 1:
        raise ValueError("Choose one unambiguous clip for preview")
    sidecar = _read(clips[0]["sidecar"])
    result = _blender(Path(clips[0]["fbx"]), "preview", work, dict(request, clip=None))
    result.update(source=str(source), clip=clips[0]["clip"], root_motion="model-root ignored unless root_to_bone was selected")
    result["warnings"].extend(sidecar.get("warnings", []))
    result["dropped"] = sidecar.get("dropped", [])
    return result


def _copy_data(source, folder, provenance_root=None):
    if source.suffix.lower() not in (".fbx", ".anim"):
        raise ValueError("The isolated helper accepts only model FBX and AnimationClip data")
    folder.mkdir(parents=True, exist_ok=True)
    destination = folder / source.name
    from .default_humanoid import file_digest
    source_hash = file_digest(source)
    changed = not destination.is_file() or source_hash != file_digest(destination)
    provenance_root = Path(provenance_root) if provenance_root else folder.parent / ".animbridge-copies"
    provenance = provenance_root / (hashlib.sha256(str(destination).encode()).hexdigest()[:16] + ".json")
    previous = _read(provenance) if provenance.is_file() else {}
    if changed:
        shutil.copyfile(source, destination)
    meta = meta_path_for(source)
    if meta.is_file():
        copied_meta = meta_path_for(destination)
        if not copied_meta.is_file() or file_digest(meta) != file_digest(copied_meta):
            shutil.copyfile(meta, copied_meta)
    elif (changed or previous.get("copied_meta")) and meta_path_for(destination).is_file():
        # Keep Unity-generated importers for unchanged data. A deleted source
        # meta, or changed source content, must not inherit the old copied meta.
        meta_path_for(destination).unlink()
    _write(provenance, {"source": str(source), "sha256": source_hash, "copied_meta": meta.is_file()})
    return destination


def _unity(request, operation, work):
    if operation != "preview":
        return _unity_run(request, operation, work)
    from .default_humanoid import cache_slot, file_digest
    model = _source(request, "skeleton")
    fingerprint = {"model": str(model), "sha": file_digest(model), "mapping": request.get("human_mapping", {}),
        "editor": _editor_identity(request),
        "meta": file_digest(meta_path_for(model)) if meta_path_for(model).is_file() else None,
        "scripts": [file_digest(PROJECT_ROOT / "unity/Editor" / name) for name in
                    ("UnityAnimBridgeBaker.cs", "AnimBridgeDesktopHelper.cs", "AnimBridgePreview.cs")]}
    key = hashlib.sha256(json.dumps(fingerprint, sort_keys=True).encode()).hexdigest()[:32]
    root = Path(request.get("cache") or default_data_root() / "helper").resolve() / "unity-preview-projects" / key
    # Each model has two independent AssetDatabases, retaining imported data and
    # compiled helpers while allowing two cold clips to evaluate concurrently.
    with cache_slot(root) as slot:
        return _unity_run(dict(request, _preview_project=str(slot / "project")), operation, work)


def _editor_identity(request):
    try:
        editor = runtime.locate_unity(request, default=DEFAULT_UNITY)
    except runtime.EngineError:
        editor = None
    if editor is None:
        return {"path": None, "size": None, "mtime_ns": None, "version": None}
    editor = Path(editor).resolve()
    facts = editor.stat() if editor.is_file() else None
    version = runtime.probe_unity(editor).get("version") if facts else None
    return {"path": str(editor), "size": facts.st_size if facts else None,
            "mtime_ns": facts.st_mtime_ns if facts else None, "version": version}


def _clear_old_clips(project, keep):
    clips = project / "Assets/Data/Clips"
    if not clips.is_dir():
        return
    if not clips.resolve().is_relative_to(project.resolve()):
        raise ValueError("The preview clip cache must remain inside its app-owned project")
    retained = {keep, keep + ".meta"} if keep else set()
    for path in clips.iterdir():
        if path.name not in retained and path.is_file() and path.suffix.lower() in (".anim", ".fbx", ".meta"):
            path.unlink()


def _unity_run(request, operation, work):
    try:
        located = runtime.locate_unity(request, default=DEFAULT_UNITY)
    except (runtime.EngineError, runtime.RuntimeContextError) as error:
        return {"status": "helperUnavailable", "connected": False, "reason": str(error)}
    if located is None:
        return {"status": "helperUnavailable", "connected": False, "reason": "Unity Editor not found"}
    editor = Path(located).resolve()
    info = runtime.probe_unity(editor)
    if info.get("state") != "ready" or not info.get("version"):
        return {"status": "helperUnavailable", "connected": False,
                "reason": info.get("reason") or f"Unity Editor is not ready: {editor}", "editor": info}
    project = Path(request["_preview_project"]) if operation == "preview" else work / "project"
    for directory in (project / "Assets" / "Editor", project / "Packages", project / "ProjectSettings", work / "temp"):
        directory.mkdir(parents=True, exist_ok=True)
    _write(project / "Packages" / "manifest.json", {"dependencies": {}})
    (project / "ProjectSettings" / "ProjectVersion.txt").write_text(f"m_EditorVersion: {info['version']}\n", encoding="utf-8")
    trusted = ("UnityAnimBridgeBaker.cs", "AnimBridgeDesktopHelper.cs", "AnimBridgePreview.cs")
    for name in trusted:
        original, copied = PROJECT_ROOT / "unity" / "Editor" / name, project / "Assets" / "Editor" / name
        if not copied.is_file() or original.read_bytes() != copied.read_bytes():
            shutil.copyfile(original, copied)
    session = uuid.uuid4().hex
    job = {"operation": operation, "session": session, "helper_version": HELPER_VERSION,
           "humanoid": bool(request.get("humanoid", False)), "fps": request.get("fps") or 0,
           "clip_name": request.get("clip") or "", "baked_file": str(work / "baked.json"),
           "human_mapping": [{"human": key, "bone": value} for key, value in request.get("human_mapping", {}).items()]}
    if operation == "preview":
        job.update(preview_file=str(work / "preview.json"), include_mesh=bool(request.get("include_mesh", True)),
                   max_frames=int(request.get("max_frames", 121)),
                   max_mesh_vertices=int(request.get("max_mesh_vertices", 1800)),
                   max_point_frames=int(request.get("max_point_frames", 150000)))
    model_value = request.get("model") or request.get("skeleton") or request.get("path")
    if operation != "test":
        model = Path(model_value).resolve() if model_value else None
        if model is None or not model.is_file():
            raise ValueError("Connect a model FBX before Unity Avatar evaluation")
        copied = _copy_data(model, project / "Assets" / "Data" / "Model", project / ".animbridge-copies")
        job["model"] = copied.relative_to(project).as_posix()
        source = _source(request) if request.get("source") else None
        _clear_old_clips(project, source.name if source and source != model else None)
        if request.get("source"):
            if source == model:
                job["clip"] = job["model"]
            else:
                copied = _copy_data(source, project / "Assets" / "Data" / "Clips", project / ".animbridge-copies")
                job["clip"] = copied.relative_to(project).as_posix()
    input_path, output = work / "helper_request.json", work / "helper_result.json"
    _write(input_path, job)
    command = [str(editor), "-batchmode", "-nographics", "-quit", "-projectPath", str(project),
               "-executeMethod", "AnimBridgeDesktopHelper.Run", "-animBridgeDesktopRequest", str(input_path),
               "-animBridgeDesktopResult", str(output), "-logFile", str(work / "unity.log")]
    _write(work / "unity_command.json", command)
    env = runtime.safe_env(temp=work / "temp")
    with (work / "unity_process.log").open("wb") as log:
        completed = process.run_process(command, stage="unity", budget=process.stage_budget(request),
                                        cwd=PROJECT_ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    if not output.is_file():
        return {"status": "helperUnavailable", "connected": False,
                "reason": f"Unity did not return a helper session (exit {completed.returncode})", "log": str(work / "unity.log")}
    result = _read(output)
    if result.get("session") != session or result.get("helper_version") != HELPER_VERSION:
        return {"status": "helperUnavailable", "connected": False, "reason": "Helper version or session response mismatch"}
    result.update(log=str(work / "unity.log"), editor=str(editor), editor_version=info["version"],
                  project=str(project), isolated=True,
                  copied_scripts=list(trusted), process_exit_code=completed.returncode)
    return result


def helper_test(request):
    result = _unity(request, "test", _work(request, "helper"))
    if not result.get("connected"):
        result["status"] = "error"
    return result


def _unity_preview(request, humanoid):
    """Validate and sample together; cache only successful, content-bound results."""
    from .default_humanoid import cache_lock, file_digest, resolve_default, UnrealApprovalRequired
    request = dict(request, humanoid=humanoid)
    default = None
    if not request.get("skeleton") and not humanoid:
        return {"status": "blocked", "code": "needsSkeleton", "reason": "Connect a compatible character before preview"}
    # Validate the cache ownership boundary before preparing a default character.
    work = _work(request, "preview")
    if not request.get("skeleton"):
        try:
            default = resolve_default(request)
        except UnrealApprovalRequired as error:
            return {"status": "blocked", "code": "needsUnrealEditor", "reason": str(error)}
        except process.ProcessTimeout:
            raise
        except (ValueError, OSError, subprocess.SubprocessError) as error:
            return {"status": "blocked", "code": "needsAvatar", "reason": str(error)}
        request.update(skeleton=default["model"], human_mapping=default["human_mapping"])
    if not request.get("human_mapping") and humanoid:
        request["human_mapping"] = request.get("mapping", {})
    files = [_source(request), _source(request, "skeleton"),
             PROJECT_ROOT / "unity/Editor/AnimBridgeDesktopHelper.cs",
             PROJECT_ROOT / "unity/Editor/UnityAnimBridgeBaker.cs", PROJECT_ROOT / "unity/Editor/AnimBridgePreview.cs"]
    content = [(str(path), file_digest(path), file_digest(meta_path_for(path)) if meta_path_for(path).is_file() else None)
               for path in files]
    key = hashlib.sha256(json.dumps({"files": content, "version": 1,
        "editor": _editor_identity(request),
        "options": {key: request.get(key) for key in ("clip", "humanoid", "human_mapping", "fps", "include_mesh",
                    "max_frames", "max_mesh_vertices", "max_point_frames")}}, sort_keys=True).encode()).hexdigest()
    base = Path(request.get("cache") or default_data_root() / "helper").resolve() / "preview-results"
    cached = base / (key + ".json")
    with cache_lock(base / (key + ".lock")):
        if cached.is_file():
            try:
                result = _read(cached)
                if result.get("status") == "ready":
                    result["cache_hit"] = True
                    return result
            except (ValueError, OSError):
                pass
        evaluated = _unity(request, "preview", work)
        if evaluated.get("status") != "ok" or not evaluated.get("preview_file"):
            return {"status": "blocked", "code": "needsAvatar" if humanoid else "needsSkeleton",
                    "reason": evaluated.get("reason", "Unity could not evaluate this clip"), "helper": evaluated}
        result = _read(evaluated["preview_file"])
        result.update(source=str(files[0]), helper=evaluated, preview_scope="unity-evaluated-skinned-character",
                      model=request["skeleton"])
        if default:
            result["default_character"] = default
        if result.get("status") == "ready":
            current = [(str(path), file_digest(path), file_digest(meta_path_for(path)) if meta_path_for(path).is_file() else None)
                       for path in files]
            if current != content:
                return {"status": "blocked", "code": "sourceChanged", "reason": "The clip, character, or helper changed during preview. Reload the source."}
            _write(cached, result)
        return result


def validate(request):
    model = request.get("model") or request.get("skeleton") or request.get("path")
    if not model:
        return {"status": "invalid", "valid": False, "reason": "Connect a skeleton model", "missing": ["skeleton"]}
    model = _source({"source": model})
    work = _work(request, "validate")
    result = _blender(model, "inspect", work, dict(request, clip=None))
    bones = result["bones"]
    errors, missing, pairs = [], [], []
    duplicates = result["facts"]["duplicate_bones"]
    if not bones:
        errors.append("No skeleton bones found")
    if duplicates:
        errors.append("Duplicate bone names: " + ", ".join(duplicates))
    for index, bone in enumerate(bones):
        rest = np.asarray(bone["rest_matrix"]).reshape(4, 4)
        if bone["parent"] >= index or bone["parent"] < -1:
            errors.append(f"Invalid parent chain for {bone['name']}")
        if not np.all(np.isfinite(rest)) or abs(np.linalg.det(rest[:3, :3])) < 1e-12:
            errors.append(f"Invalid rest transform for {bone['name']}")
    humanoid = bool(request.get("humanoid", False))
    requires_helper = humanoid
    paths = []
    source = Path(request["source"]).resolve() if request.get("source") else None
    if source and source.suffix.lower() == ".anim":
        if not is_text_yaml(source):
            requires_helper = True  # Let Unity determine Generic vs Humanoid.
        else:
            clip = _parse(source)
            humanoid = humanoid or clip.is_humanoid
            requires_helper = requires_helper or humanoid
            paths = [path for path in clip.channels if path]
    mapping = request.get("mapping") or {}
    used = set()
    for path in paths:
        target = mapping.get(path, mapping.get(path.rsplit("/", 1)[-1], path))
        matches = [b for b in bones if b["path"] == target]
        if not matches:
            matches = [b for b in bones if b["name"] == target.rsplit("/", 1)[-1]]
        if len(matches) != 1:
            missing.append(path)
            continue
        bone = matches[0]
        if bone["name"] in used:
            errors.append(f"Multiple source paths map to {bone['name']}")
        used.add(bone["name"])
        if path not in mapping and path.rsplit("/", 1)[-1] not in mapping:
            source_chain, target_chain = path.split("/"), bone["path"].split("/")
            existing = {b["name"] for b in bones}
            source_chain = source_chain[next((i for i, name in enumerate(source_chain) if name in existing), len(source_chain) - 1):]
            if target_chain[-len(source_chain):] != source_chain:
                errors.append(f"Parent chain mismatch for {path}")
        pairs.append({"source": path, "bone": bone["name"], "path": bone["path"]})
    if missing:
        errors.append("Required Transform paths are missing")
    valid = not errors
    response = {"schema_version": 1, "status": "valid" if valid else "invalid", "valid": valid,
                "model": str(model), "bones": bones, "mapping": pairs, "missing": missing,
                "duplicates": duplicates, "errors": errors, "rest_pose": "finite-nonsingular-transforms" if valid else "invalid",
                "validation_scope": "clip-paths-and-skeleton" if paths else "skeleton-structure", "warnings": result["warnings"]}
    if requires_helper and valid:
        human_mapping = request.get("human_mapping") or (mapping if humanoid else {})
        avatar = _unity(dict(request, model=str(model), humanoid=humanoid, human_mapping=human_mapping), "validate", work / "unity")
        response["helper"] = avatar
        response["avatar"] = avatar.get("avatar")
        valid = avatar.get("status") == "ok"
        response.update(valid=valid, status="valid" if valid else "needsAvatar" if avatar.get("connected") else "helperUnavailable",
                        validation_scope="unity-humanoid-avatar" if humanoid or avatar.get("clip_humanoid") else "unity-animation-and-model",
                        reason=avatar.get("reason", "") if not valid else "Unity validated the model and Avatar requirements")
    return response


def preview(request):
    source = _source(request)
    suffix = source.suffix.lower()
    if suffix == ".json":
        return _baked_preview(source, request)
    if suffix == ".fbx":
        meta = meta_path_for(source)
        settings = read_model_meta(meta) if meta.is_file() else None
        if (settings and settings.animation_type == 3) or request.get("humanoid"):
            return _unity_preview(request, True)
        if settings and settings.clips:
            selected = request.get("clip") or (settings.clips[0].name if len(settings.clips) == 1 else None)
            if not selected:
                return {"status": "blocked", "code": "chooseClip", "reason": "Choose one FBX clip defined by the ModelImporter metadata"}
            clips = [clip for clip in settings.clips if clip.name == selected]
            if len(clips) != 1:
                return {"status": "blocked", "code": "chooseClip", "reason": "The requested metadata clip is missing or ambiguous"}
            clip = clips[0]
            result = _blender(source, "preview", _work(request, "preview"),
                              dict(request, clip=clip.take_name or None, first_frame=clip.first_frame, last_frame=clip.last_frame))
            result["clip"] = clip.name
            result["warnings"].extend(settings.warnings)
        else:
            result = _blender(source, "preview", _work(request, "preview"), request)
        if meta.is_file() and read_model_meta(meta).animation_type == 3:
            result["warnings"].append("Source FBX motion preview; Unity Humanoid retargeting is not applied")
        result.update(preview_scope="source-fbx-animation", root_motion="source FBX Transform motion; Unity importer root-bake settings are not applied")
        return result
    if suffix != ".anim":
        return {"status": "blocked", "code": "unsupported", "reason": "This input has no supported preview evaluator"}
    binary = not is_text_yaml(source)
    clip_data = _parse(source) if not binary else None
    humanoid = bool(clip_data and clip_data.is_humanoid)
    if clip_data and not humanoid and not clip_data.channels:
        return {"status": "blocked", "code": "unsupported", "reason": "No supported Transform animation curves; this preview does not execute Animator controllers or parameter logic"}
    requires_helper = binary or humanoid
    if requires_helper:
        return _unity_preview(request, humanoid or bool(request.get("humanoid")))
    if not request.get("skeleton"):
        return {"status": "blocked", "code": "needsAvatar" if humanoid else "needsSkeleton", "reason": "Connect a compatible character before preview"}
    checked = validate(dict(request, humanoid=humanoid))
    if not checked["valid"]:
        return {"status": "blocked", "code": checked["status"], "reason": "Skeleton or Avatar validation failed", "validation": checked}
    work = _work(request, "preview")
    return _convert_preview(source, request, work)


def bake(request):
    """Bake through an isolated Unity project, then convert the evaluated samples.

    The native app supplies an empty staging directory and atomically commits
    the standard manifest/FBX results after its normal collision checks.
    """
    source = _source(request)
    model_value = request.get("skeleton") or request.get("model")
    if not model_value:
        return {"status": "blocked", "code": "needsAvatar", "reason": "Connect a compatible model FBX before Unity bake"}
    model = _source({"source": model_value})
    if source.suffix.lower() not in (".anim", ".fbx") or model.suffix.lower() != ".fbx":
        return {"status": "blocked", "code": "unsupported", "reason": "Unity bake requires AnimationClip/FBX and a model FBX"}
    output_value = request.get("output")
    if not output_value:
        raise ValueError("Missing output staging directory")
    output = Path(output_value).resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        return {"status": "blocked", "code": "outputConflict", "reason": "The bake staging directory must be empty"}
    work = _work(request, "bake")
    humanoid = bool(request.get("humanoid", False))
    if source.suffix.lower() == ".anim" and is_text_yaml(source):
        humanoid = humanoid or _parse(source).is_humanoid
    elif source.suffix.lower() == ".fbx" and meta_path_for(source).is_file():
        humanoid = humanoid or read_model_meta(meta_path_for(source)).animation_type == 3
    human_mapping = request.get("human_mapping") or (request.get("mapping", {}) if humanoid else {})
    helper = _unity(dict(request, skeleton=str(model), humanoid=humanoid, human_mapping=human_mapping), "bake", work)
    if helper.get("status") != "ok" or not helper.get("baked_file"):
        return {"status": "blocked", "code": "helperUnavailable" if not helper.get("connected") else "needsAvatar" if helper.get("status") == "needsAvatar" else "evaluationFailed",
                "reason": helper.get("reason", "Unity could not evaluate the selected clip"), "helper": helper}
    from .cli import main as convert
    arguments = ["convert", "--baked", helper["baked_file"], "--skeleton", str(model), "-o", str(output)]
    for option in ("blender", "root_to_bone"):
        if request.get(option):
            arguments += ["--" + option.replace("_", "-"), str(request[option])]
    if request.get("bone_map"):
        mapping_path = work / "bone_map.json"
        _write(mapping_path, request["bone_map"])
        arguments += ["--bone-map", str(mapping_path)]
    _write(work / "convert_arguments.json", arguments)
    with runtime.scratch(work / "converter_work"):
        with (work / "convert.log").open("w", encoding="utf-8") as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            code = convert(arguments)
    manifest_path = output / "manifest.json"
    manifest = _read(manifest_path) if manifest_path.is_file() else None
    if code or not manifest or not manifest.get("clips") or any(clip["status"] == "error" for clip in manifest["clips"]):
        return {"status": "error", "code": "conversionFailed", "reason": f"Baked clip conversion failed (exit {code}); see {work / 'convert.log'}",
                "helper": helper, "manifest": str(manifest_path) if manifest else None}
    files = [Path(clip["fbx"]).resolve() for clip in manifest["clips"]]
    if any(not file.is_relative_to(output) or not file.is_file() or file.stat().st_size == 0 for file in files):
        raise ValueError("Converter did not produce valid FBX files inside staging")
    return {"schema_version": 1, "status": "ok", "manifest": str(manifest_path), "baked": helper["baked_file"],
            "helper": helper, "outputs": [str(file) for file in files], "warnings": manifest.get("warnings", []),
            "checks": {"unity_avatar": helper.get("avatar"), "engine_import": "notRun", "staging_files_exist": True}}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("inspect", "preview", "validate", "helper-test", "bake", "unreal-export"))
    parser.add_argument("--request", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    try:
        runtime.current_context()
        request = _read(args.request)
        handler = {"inspect": inspect, "preview": preview, "validate": validate, "helper-test": helper_test, "bake": bake,
                   "unreal-export": unreal_export.unreal_export}[args.operation]
        with process.job(request.get("job_timeout"), stage_seconds=process.stage_budget(request)):
            result = handler(request)
        result.setdefault("schema_version", 1)
    except process.ProcessTimeout as error:
        result = {"schema_version": 1, "status": "blocked" if args.operation in REPORTED_OPERATIONS else "error",
                  "code": "timeout", **process.describe(error)}
    except Exception as error:
        result = {"schema_version": 1, "status": "blocked" if args.operation in REPORTED_OPERATIONS else "error", "code": "error", "reason": str(error)}
    _write(args.out, result)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    print(json.dumps({"schema_version": 1, "status": result["status"], "result": str(Path(args.out).resolve()),
                      **{key: result[key] for key in ("reason", "frame_count", "fps", "duration") if key in result}}, ensure_ascii=False))
    return 0 if result["status"] not in ("error",) else 2


if __name__ == "__main__":
    raise SystemExit(main())
