"""Trusted, read-only export from an app-owned copy of Epic's local template."""
import json
import hashlib
from pathlib import Path

import unreal


def main():
    command_line = unreal.SystemLibrary.get_command_line()
    import re
    match = re.search(r'"-animBridgeMannequin=([^"]+)"|-animBridgeMannequin="([^"]+)"|-animBridgeMannequin=([^\s]+)', command_line)
    if match is None:
        raise RuntimeError("Missing mannequin export request")
    request = json.loads(Path(next(value for value in match.groups() if value)).read_text(encoding="utf-8-sig"))
    model = unreal.load_asset("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple")
    if not isinstance(model, unreal.SkeletalMesh):
        raise RuntimeError("The installed Manny skeletal mesh could not be loaded")
    output = Path(request["output"])
    options = unreal.FbxExportOption()
    options.set_editor_property("bake_material_inputs", unreal.FbxMaterialBakeMode.DISABLED)
    for key, value in {"ascii": False, "level_of_detail": False, "export_morph_targets": False,
                       "collision": False, "force_front_x_axis": False}.items():
        options.set_editor_property(key, value)
    task = unreal.AssetExportTask()
    for key, value in {"object": model, "filename": str(output), "automated": True,
                       "prompt": False, "replace_identical": True, "options": options,
                       "exporter": unreal.SkeletalMeshExporterFBX()}.items():
        task.set_editor_property(key, value)
    success = unreal.Exporter.run_asset_export_task(task)
    if not success or not output.is_file() or not output.stat().st_size:
        raise RuntimeError("Manny FBX export failed: " + str(list(task.get_editor_property("errors"))))
    Path(request["report"]).write_text(json.dumps({"status": "ok", "mesh": model.get_path_name(),
        "engine": unreal.SystemLibrary.get_engine_version(), "output": str(output),
        "sha256": hashlib.sha256(output.read_bytes()).hexdigest()}), encoding="utf-8")


main()
