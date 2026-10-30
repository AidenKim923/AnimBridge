"""Export selected AnimSequences to portable FBX files for AnimBridge / Unity.

Requires an Unreal Editor project with Python Editor Script Plugin enabled.
Run in its Python console or via UnrealEditor-Cmd -run=pythonscript:
  -script="<repository>/unreal/animbridge_export.py --assets /Game/Anims/A_Run.A_Run --out <output-directory>"
Omit --assets to export selected Content Browser AnimSequences. Standalone
.uasset parsing is unsupported: load the owning Unreal project.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

import unreal


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", help="Semicolon-separated Unreal AnimSequence object paths")
    parser.add_argument("--out", required=True)
    return parser.parse_args(sys.argv[1:] if argv is None else argv)


def export_assets(assets, out_dir, missing=()):
    out = Path(out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    manifest = {"animbridge_version": 1, "source_engine": "unreal",
                "engine_version": unreal.SystemLibrary.get_engine_version(), "clips": []}
    failures = [f"{path}: asset not found in this project" for path in missing]
    for asset in assets:
        if not isinstance(asset, unreal.AnimSequence):
            # Name the actual class so a mesh or montage gives a clear message.
            failures.append(f"{asset.get_path_name()} is a {asset.get_class().get_name()}: expected AnimSequence")
            continue
        source = asset.get_path_name()
        name = asset.get_name()
        suffix = hashlib.sha256(source.encode("utf-8")).hexdigest()[:10]
        filename = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name).strip(". ") or "Animation"
        path = out / f"{filename}_{suffix}.fbx"
        try:
            # Unity-ready defaults: Unreal writes centimetres in a Z-up scene and
            # records that axis system/unit in the FBX header. Unity's importer
            # reads it and converts to its own Y-up metres (Convert Units on), so
            # no manual rotation or 0.01 scale is needed. Keeping the default
            # -Y forward axis (force_front_x_axis False) matches Unreal's own
            # mannequin FBX files; no mesh is written, only the skeleton curves.
            options = unreal.FbxExportOption()
            for key, value in {"ascii": False, "force_front_x_axis": False, "export_preview_mesh": False,
                               "export_morph_targets": False, "level_of_detail": False,
                               "collision": False}.items():
                options.set_editor_property(key, value)
            task = unreal.AssetExportTask()
            for key, value in {"object": asset, "filename": str(path), "automated": True,
                               "prompt": False, "replace_identical": True,
                               "exporter": unreal.AnimSequenceExporterFBX(), "options": options}.items():
                task.set_editor_property(key, value)
            if not unreal.Exporter.run_asset_export_task(task) or not path.is_file() or path.stat().st_size == 0:
                raise RuntimeError(f"FBX export failed: {list(task.get_editor_property('errors'))}")
            duration = float(unreal.AnimationLibrary.get_sequence_length(asset))
            frames = int(unreal.AnimationLibrary.get_num_keys(asset))
            # TargetFrameRate is not reflected in UE 5.8 Python. UE's sampled
            # keys include both endpoints; duration therefore gives the rate.
            fps = (frames - 1) / duration if duration > 0 and frames > 1 else 30.0
            markers = unreal.AnimationLibrary.get_animation_sync_markers(asset)
            events = [{"name": str(m.get_editor_property("marker_name")),
                       "time": float(m.get_editor_property("time"))} for m in markers]
            manifest["clips"].append({"id": source, "name": name, "fbx": str(path),
                "source_asset": source, "fps": fps, "frames": frames, "duration": duration,
                "events": events, "skeleton": asset.get_editor_property("skeleton").get_path_name()})
        except Exception as exc:
            failures.append(f"{source}: {exc}")
    manifest["errors"] = failures
    manifest_path = out / "animbridge-manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    unreal.log(f"AnimBridge: exported {len(manifest['clips'])} animations to {manifest_path}")
    if failures or not manifest["clips"]:
        raise RuntimeError("AnimBridge export failed: " + "; ".join(failures or ["no AnimSequences selected"]))
    return manifest


def main(argv=None):
    args = parse_args(argv)
    if not args.assets:
        return export_assets(unreal.EditorUtilityLibrary.get_selected_assets(), args.out)
    paths = [path.strip() for path in args.assets.split(";") if path.strip()]
    loaded = [(path, unreal.load_asset(path)) for path in paths]
    # load_asset returns None for a missing path; the manifest reports it.
    return export_assets([asset for _, asset in loaded if asset], args.out,
                         missing=[path for path, asset in loaded if not asset])


if __name__ == "__main__":
    main()
