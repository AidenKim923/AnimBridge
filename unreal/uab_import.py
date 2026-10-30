"""Import a UnityAnimBridge manifest inside an Unreal Editor project.

UnrealEditor-Cmd.exe Project.uproject -run=pythonscript
  -script="<repository>/unreal/uab_import.py --manifest ... --skeleton /Game/... --dest /Game/Anims"
"""
import argparse
import json
import re
import shlex
import sys
from pathlib import Path

import unreal

EVENT_TRACK = "UnityEvents"
COMPRESSION_NAME = "AB_PreserveSampledBones_v1"


def compression_folder(destination):
    """Keep generated assets inside the user-approved Content destination."""
    if not isinstance(destination, str) or not re.fullmatch(r"/Game(?:/[A-Za-z0-9_]+)+", destination):
        raise ValueError("Unreal destination must be a /Game subfolder with safe path components")
    return destination + "/AnimBridgeGenerated"


def parse_args(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) == 1 and " " in argv[0]:
        argv = shlex.split(argv[0])
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--skeleton", required=True)
    ap.add_argument("--dest", required=True)
    ap.add_argument("--legacy-fbx", action="store_true")
    return ap.parse_args(argv)


def set_prop(obj, name, value):
    """Unreal checks reflected property names and reports incompatible versions."""
    obj.set_editor_property(name, value)


def interchange_options(skeleton, fps=0):
    for cls in ("InterchangeGenericAssetsPipeline", "InterchangePipelineStackOverride"):
        if not hasattr(unreal, cls):
            raise RuntimeError(f"unreal.{cls} is missing; use --legacy-fbx")
    pipe = unreal.InterchangeGenericAssetsPipeline()
    common = pipe.get_editor_property("common_skeletal_meshes_and_animations_properties")
    set_prop(common, "skeleton", skeleton)
    set_prop(common, "import_only_animations", True)
    anim = pipe.get_editor_property("animation_pipeline")
    set_prop(anim, "import_animations", True)
    # Preserve the file's sampling rate rather than silently rebaking to 30 Hz.
    set_prop(anim, "use30_hz_to_bake_bone_animation", False)
    set_prop(anim, "custom_bone_animation_sample_rate", round(fps) if abs(fps - round(fps)) < 1e-5 else 0)
    override = unreal.InterchangePipelineStackOverride()
    override.add_pipeline(pipe)
    return override


def legacy_options(skeleton, fps=0):
    ui = unreal.FbxImportUI()
    for name, value in {
        "automated_import_should_detect_type": False,
        "mesh_type_to_import": unreal.FBXImportType.FBXIT_ANIMATION,
        "import_mesh": False, "import_animations": True, "import_materials": False,
        "import_textures": False, "skeleton": skeleton,
    }.items():
        set_prop(ui, name, value)
    data = ui.get_editor_property("anim_sequence_import_data")
    set_prop(data, "use_default_sample_rate", False)
    set_prop(data, "custom_sample_rate", round(fps) if abs(fps - round(fps)) < 1e-5 else 0)
    # Rebuilding locals from decomposed global transforms can alter a static
    # pose (observed 0.60 degrees on a two-key Humanoid T-pose in UE 5.8).
    set_prop(data, "preserve_local_transform", True)
    return ui


def sample_preserving_compression(destination):
    """Use an app-owned native codec, without changing engine/global defaults.

    Default ACL's skin-distance tolerance can exceed the transfer's local
    rotation tolerance on short/finger bones. LeastDestructive retains sampled
    position/scale floats and quaternion components without key reduction.
    The class is loaded natively because it has no generated Python wrapper.
    """
    folder = compression_folder(destination)
    path = folder + "/" + COMPRESSION_NAME + "." + COMPRESSION_NAME
    settings = unreal.load_asset(path)
    if settings is not None:
        if not isinstance(settings, unreal.AnimBoneCompressionSettings):
            raise RuntimeError("sample compression asset path is occupied by another asset: " + path)
        codecs = list(settings.get_editor_property("codecs"))
        if (len(codecs) != 1 or codecs[0].get_class().get_name() != "AnimCompress_LeastDestructive"
                or unreal.EditorAssetLibrary.get_metadata_tag(settings, "AnimBridgeRole") != "sampled-bones-v1"):
            raise RuntimeError("sample compression asset does not match AnimBridge's generated profile: " + path)
        return settings
    codec_class = unreal.load_class(None, "/Script/Engine.AnimCompress_LeastDestructive")
    if codec_class is None:
        raise RuntimeError("the engine's sample-preserving animation codec is unavailable")
    settings = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        COMPRESSION_NAME, folder, unreal.AnimBoneCompressionSettings,
        unreal.AnimBoneCompressionSettingsFactory())
    if settings is None:
        raise RuntimeError("could not create the sample-preserving compression profile")
    codec = unreal.new_object(codec_class, outer=settings)
    set_prop(settings, "codecs", [codec])
    set_prop(settings, "error_threshold", 1e-4)
    set_prop(settings, "force_below_threshold", True)
    unreal.EditorAssetLibrary.set_metadata_tag(settings, "AnimBridgeRole", "sampled-bones-v1")
    if not unreal.EditorAssetLibrary.save_loaded_asset(settings, only_if_is_dirty=False):
        raise RuntimeError("could not save sample-preserving compression profile")
    return settings


def apply_sample_preserving_compression(animation, destination):
    settings = sample_preserving_compression(destination)
    set_prop(animation, "allow_frame_stripping", False)
    set_prop(animation, "bone_compression_settings", settings)
    if not unreal.EditorAssetLibrary.save_loaded_asset(animation, only_if_is_dirty=False):
        raise RuntimeError("could not save sample-preserving animation compression")
    return settings.get_path_name()


def import_clip(fbx, dest, name, options):
    task = unreal.AssetImportTask()
    for prop, value in {
        "filename": str(fbx), "destination_path": dest, "destination_name": name,
        "automated": True, "replace_existing": True, "save": True, "options": options,
    }.items():
        task.set_editor_property(prop, value)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    return list(task.get_editor_property("imported_object_paths"))


def marker_values(anim):
    markers = unreal.AnimationLibrary.get_animation_sync_markers_for_track(anim, EVENT_TRACK)
    return sorted((str(m.get_editor_property("marker_name")), float(m.get_editor_property("time"))) for m in markers)


def expected_markers(events):
    return sorted((str(e.get("name") or "Event"), float(e["time"])) for e in events)


def verify_markers(anim, events):
    actual, expected = marker_values(anim), expected_markers(events)
    if len(actual) != len(expected) or any(a[0] != e[0] or abs(a[1] - e[1]) > 1e-5 for a, e in zip(actual, expected)):
        return [f"sync markers differ: expected {expected}, found {actual}"]
    return []


def add_events(anim, events, path):
    lib = unreal.AnimationLibrary
    names = [str(n) for n in lib.get_animation_notify_track_names(anim)]
    if not events and EVENT_TRACK not in names:
        return []
    if EVENT_TRACK not in names:
        lib.add_animation_notify_track(anim, EVENT_TRACK)
    # Reimport must not duplicate markers from a previous import.
    lib.remove_animation_sync_markers_by_track(anim, EVENT_TRACK)
    for event in events:
        lib.add_animation_sync_marker(anim, event.get("name") or "Event", float(event["time"]), EVENT_TRACK)
    if not unreal.EditorAssetLibrary.save_loaded_asset(anim, only_if_is_dirty=False):
        return ["saving markers failed"]
    # load_asset can return the same in-memory object. This is an immediate check;
    # tests/test_unreal.py also starts a fresh editor process to prove persistence.
    return verify_markers(unreal.load_asset(path), events)


def main(argv=None):
    args = parse_args(argv)
    compression_folder(args.dest)
    manifest_path = Path(args.manifest).resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    skeleton = unreal.load_asset(args.skeleton)
    if not isinstance(skeleton, unreal.Skeleton):
        raise RuntimeError(f"{args.skeleton} is not a Skeleton asset")
    cvar = "Interchange.FeatureFlags.Import.FBX"
    old_fbx = unreal.SystemLibrary.get_console_variable_int_value(cvar) if args.legacy_fbx else None
    if args.legacy_fbx:
        unreal.SystemLibrary.execute_console_command(None, cvar + " 0")
        if unreal.SystemLibrary.get_console_variable_int_value(cvar) != 0:
            raise RuntimeError("Could not disable Interchange FBX for --legacy-fbx")
    report = []
    try:
        for clip in manifest["clips"]:
            result = {"clip": clip["clip"], "status": "error", "problems": []}
            report.append(result)
            if clip["status"] == "error":
                result["problems"].append("conversion failed")
                continue
            try:
                side_path = Path(clip["sidecar"])
                side_path = side_path if side_path.is_absolute() else manifest_path.parent / side_path
                side = json.loads(side_path.read_text(encoding="utf-8-sig"))
                # The Interchange 5.8 animation pipeline has no preserve-local
                # setting. Route tiny static/reference clips through the native
                # legacy option that preserves authored bone-local transforms.
                local_fallback = not args.legacy_fbx and int(side.get("frames", 0)) <= 2
                use_legacy = args.legacy_fbx or local_fallback
                options = legacy_options(skeleton, side.get("fps", 0)) if use_legacy else interchange_options(skeleton, side.get("fps", 0))
                fbx = Path(clip["fbx"])
                fbx = fbx if fbx.is_absolute() else manifest_path.parent / fbx
                if local_fallback:
                    previous = unreal.SystemLibrary.get_console_variable_int_value(cvar)
                    unreal.SystemLibrary.execute_console_command(None, cvar + " 0")
                    try:
                        paths = import_clip(fbx, args.dest, "A_" + fbx.stem, options)
                    finally:
                        unreal.SystemLibrary.execute_console_command(None, cvar + " " + str(previous))
                else:
                    paths = import_clip(fbx, args.dest, "A_" + fbx.stem, options)
                anims = [unreal.load_asset(p) for p in paths]
                anims = [a for a in anims if isinstance(a, unreal.AnimSequence)]
                if len(anims) != 1:
                    raise RuntimeError(f"expected one AnimSequence, imported {len(anims)} ({paths})")
                anim = anims[0]
                result["asset"] = anim.get_path_name()
                result["adapter"] = "legacy-fbx" if use_legacy else "interchange"
                result["preserve_local_transform"] = use_legacy
                result["adapter_reason"] = ("short clip: preserve sampled local transforms" if local_fallback
                    else "explicit legacy request" if args.legacy_fbx else "default Interchange pipeline")
                if anim.get_editor_property("skeleton") != skeleton:
                    raise RuntimeError("AnimSequence does not use the target Skeleton")
                result["compression_profile"] = apply_sample_preserving_compression(anim, args.dest)
                result["allow_frame_stripping"] = False
                length = unreal.AnimationLibrary.get_sequence_length(anim)
                frames = unreal.AnimationLibrary.get_num_keys(anim)
                result.update(duration=length, frames=frames)
                if side.get("duration") is not None and abs(length - side["duration"]) > 1.5 / max(side.get("fps") or 30, 1):
                    result["problems"].append(f"length {length:.6f}s differs from sidecar {side['duration']:.6f}s")
                if side.get("frames") is not None and frames != side["frames"]:
                    result["problems"].append(f"sample count {frames} differs from sidecar {side['frames']}")
                marker_problems = add_events(anim, side.get("events", []), result["asset"])
                if marker_problems:
                    raise RuntimeError("; ".join(marker_problems))
                result["status"] = "warning" if result["problems"] else "ok"
            except Exception as exc:
                result["problems"].append(str(exc))
    finally:
        if old_fbx is not None:
            unreal.SystemLibrary.execute_console_command(None, f"{cvar} {old_fbx}")
        adapters = sorted({entry["adapter"] for entry in report if "adapter" in entry})
        payload = {"engine_version": unreal.SystemLibrary.get_engine_version(),
                   "requested_adapter": "legacy-fbx" if args.legacy_fbx else "interchange",
                   "adapter": adapters[0] if len(adapters) == 1 else "mixed" if adapters else "none",
                   "adapters": adapters, "clips": report}
        manifest_path.with_name("unreal_import_report.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
        lines = [f"{r['status'].upper():7} {r['clip']} -> {r.get('asset', '')}; {'; '.join(r['problems'])}" for r in report]
        manifest_path.with_name("unreal_import_report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        for line in lines:
            unreal.log(line)
    if any(r["status"] == "error" for r in report):
        raise RuntimeError("One or more animations failed; see unreal_import_report.json")
    return payload


if __name__ == "__main__":
    main()
