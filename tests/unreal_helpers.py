"""Run only inside the isolated Unreal verification project."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import traceback

import unreal

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "unreal"))
import uab_import
import animbridge_export


def skeleton_from_source(source, dest):
    cvar = "Interchange.FeatureFlags.Import.FBX"
    old = unreal.SystemLibrary.get_console_variable_int_value(cvar)
    unreal.SystemLibrary.execute_console_command(None, cvar + " 0")
    try:
        ui = unreal.FbxImportUI()
        for prop, value in {"automated_import_should_detect_type": False,
                            "mesh_type_to_import": unreal.FBXImportType.FBXIT_SKELETAL_MESH,
                            "import_as_skeletal": True, "import_mesh": True,
                            "import_animations": False, "import_materials": False,
                            "import_textures": False, "create_physics_asset": False}.items():
            ui.set_editor_property(prop, value)
        paths = uab_import.import_clip(source, dest, "SK_Source", ui)
        assets = [unreal.load_asset(p) for p in paths]
        meshes = [a for a in assets if isinstance(a, unreal.SkeletalMesh)]
        if len(meshes) != 1:
            raise RuntimeError(f"source import produced no single SkeletalMesh: {paths}")
        mesh = meshes[0]
        skeleton = mesh.get_editor_property("skeleton")
        # AssetImportTask(save=True) does not save the newly generated Skeleton.
        for asset in (skeleton, mesh):
            if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
                raise RuntimeError(f"could not save {asset.get_path_name()}")
        return mesh, skeleton
    finally:
        unreal.SystemLibrary.execute_console_command(None, f"{cvar} {old}")


def hierarchy(mesh):
    component = unreal.new_object(unreal.SkeletalMeshComponent)
    component.set_skeletal_mesh(mesh)
    return {str(component.get_bone_name(i)): str(component.get_parent_bone(component.get_bone_name(i)))
            for i in range(component.get_num_bones())}


def dump_pose(anim, bones, frames, fps):
    samples = []
    for frame in range(frames):
        values = {}
        for bone in bones:
            pose = unreal.AnimationLibrary.get_bone_pose_for_time(anim, bone, frame / fps, False)
            t = pose.translation
            r = pose.rotation
            s = pose.scale3d
            values[bone] = {"t": [t.x, t.y, t.z], "r": [r.x, r.y, r.z, r.w], "s": [s.x, s.y, s.z]}
        samples.append(values)
    return samples


def main():
    job_path = Path(os.environ["UAB_UNREAL_JOB"])
    job = json.loads(job_path.read_text(encoding="utf-8"))
    result = {"engine_version": unreal.SystemLibrary.get_engine_version(), "phase": job["phase"], "cases": []}
    try:
        for case in job["cases"]:
            entry = {"label": case["label"]}
            result["cases"].append(entry)
            if job["phase"] == "import":
                mesh, skeleton = skeleton_from_source(case["source"], case["dest"] + "/Source")
                before = hierarchy(mesh)
                entry.update(mesh=mesh.get_path_name(), skeleton=skeleton.get_path_name(), hierarchy_before=before)
                args = ["--manifest", case["manifest"], "--skeleton", skeleton.get_path_name(),
                        "--dest", case["dest"] + "/Animations"]
                if job["adapter"] == "legacy-fbx":
                    args.append("--legacy-fbx")
                imported = uab_import.main(args)
                entry["import_report"] = imported
                entry["hierarchy_after"] = hierarchy(mesh)
                asset_path = imported["clips"][0]["asset"]
            else:
                imported = json.loads(Path(case["import_result"]).read_text(encoding="utf-8"))
                previous = next(c for c in imported["cases"] if c["label"] == case["label"])
                asset_path = previous["import_report"]["clips"][0]["asset"]
                mesh = unreal.load_asset(previous["mesh"])
                entry["hierarchy_reloaded"] = hierarchy(mesh)
            anim = unreal.load_asset(asset_path)
            if not isinstance(anim, unreal.AnimSequence) or not isinstance(anim.get_editor_property("skeleton"), unreal.Skeleton):
                raise RuntimeError(f"animation or Skeleton not persisted: {asset_path}")
            side_path = Path(json.loads(Path(case["manifest"]).read_text())["clips"][0]["sidecar"])
            side = json.loads(side_path.read_text())
            entry["asset"] = asset_path
            entry["markers"] = uab_import.marker_values(anim)
            problems = uab_import.verify_markers(anim, side["events"])
            if problems:
                raise RuntimeError("; ".join(problems))
            entry["frames"] = unreal.AnimationLibrary.get_num_keys(anim)
            entry["duration"] = unreal.AnimationLibrary.get_sequence_length(anim)
            entry["poses"] = dump_pose(anim, ["Hips", "Spine", "Hand"], side["frames"], side["fps"])
            if job["phase"] == "reload":
                entry["export"] = animbridge_export.export_assets([anim], Path(job["out"]).parent / (case["label"] + "_reverse"))
        result["status"] = "ok"
    except Exception:
        result["status"] = "error"
        result["error"] = traceback.format_exc()
        raise
    finally:
        Path(job["out"]).write_text(json.dumps(result, indent=2), encoding="utf-8")


main()
