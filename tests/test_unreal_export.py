"""Raw .uasset -> FBX conversion worker, without a real Unreal Editor."""
import json
from pathlib import Path
import sys

import pytest

from unityanimbridge import desktop, unreal_export


def test_object_path_must_name_the_package_main_object():
    assert unreal_export.validate_object_path("/Game/Anims/Run.Run") == "/Game/Anims/Run.Run"
    assert unreal_export.validate_object_path("/MoCap/Runs/Sprint.Sprint") == "/MoCap/Runs/Sprint.Sprint"
    for bad in ("/Game/Anims/Run.Walk", "Game/Anims/Run.Run", "/Game/A;B/Run.Run", "/Game/Run", "", None):
        with pytest.raises(unreal_export.ExportError):
            unreal_export.validate_object_path(bad)


def test_headless_command_keeps_arguments_out_of_the_command_line(tmp_path):
    command = unreal_export.headless_command(Path("C:/UE/UnrealEditor-Cmd.exe"), tmp_path / "Game.uproject", tmp_path / "boot.py")
    assert command[2] == "-run=pythonscript" and command[3] == "-script=" + str(tmp_path / "boot.py")
    assert {"-unattended", "-nullrhi", "-nosplash"} <= set(command)
    assert not any("--assets" in part for part in command)


def _project(tmp_path):
    project = tmp_path / "Game"
    (project / "Content/Anims").mkdir(parents=True)
    (project / "Game.uproject").write_text("{}", encoding="utf-8")
    asset = project / "Content/Anims/Run.uasset"
    asset.write_bytes(b"opaque")
    return project, asset


# Plays Unreal: runs the bootstrap's argv against a stand-in export that writes
# one FBX and the manifest, or reports the script's real non-animation error.
FAKE_UNREAL = r'''
import json, re, sys
from pathlib import Path
argv = json.loads(re.search(r"sys.argv = (\[.*\])", Path(sys.argv[1]).read_text(encoding="utf-8")).group(1))
asset, out = argv[argv.index("--assets") + 1], Path(argv[argv.index("--out") + 1])
clips, errors = [], []
if "Mesh" in asset:
    errors.append(asset + " is a SkeletalMesh: expected AnimSequence")
else:
    (out / "Run_abc.fbx").write_bytes(b"Kaydara FBX Binary  fixture")
    clips.append({"id": asset, "name": "Run", "fbx": str(out / "Run_abc.fbx"), "source_asset": asset,
                  "fps": 30.0, "frames": 31, "duration": 1.0, "events": []})
(out / "animbridge-manifest.json").write_text(json.dumps({"animbridge_version": 1, "source_engine": "unreal",
    "clips": clips, "errors": errors}), encoding="utf-8")
print("fake unreal done")
'''


@pytest.fixture
def fake_unreal(tmp_path, monkeypatch):
    script = tmp_path / "fake_unreal.py"
    script.write_text(FAKE_UNREAL, encoding="utf-8")
    monkeypatch.setattr(unreal_export, "_editor", lambda request, context: Path(sys.executable))
    monkeypatch.setattr(unreal_export, "headless_command",
                        lambda editor, project, bootstrap: [sys.executable, str(script), str(bootstrap)])


def test_convert_writes_a_relative_manifest_in_the_app_cache(tmp_path, fake_unreal):
    project, asset = _project(tmp_path)
    request = {"source": str(asset), "unreal_project": str(project / "Game.uproject"),
               "unreal_object_path": "/Game/Anims/Run.Run", "cache": str(tmp_path / "cache")}
    result = unreal_export.unreal_export(request)
    assert result["status"] == "ok", result
    manifest_path = Path(result["manifest"])
    assert manifest_path.parent.parent == (tmp_path / "cache/unreal-export").resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["clips"][0]["fbx"] == "Run_abc.fbx"
    assert Path(result["outputs"][0]).stat().st_size > 0 and Path(result["log"]).is_file()
    # The user's project receives nothing from AnimBridge.
    assert sorted(path.name for path in project.rglob("*")) == ["Anims", "Content", "Game.uproject", "Run.uasset"]
    # A second conversion replaces the first in the same stable folder.
    again = unreal_export.unreal_export(request)
    assert again["manifest"] == result["manifest"]
    assert not list((tmp_path / "cache/unreal-export").glob(".staging-*"))


def test_non_animation_assets_report_the_script_error(tmp_path, fake_unreal):
    project, _ = _project(tmp_path)
    mesh = project / "Content/Anims/Mesh.uasset"
    mesh.write_bytes(b"opaque")
    result = unreal_export.unreal_export({"source": str(mesh), "unreal_project": str(project / "Game.uproject"),
                                          "unreal_object_path": "/Game/Anims/Mesh.Mesh", "cache": str(tmp_path / "cache")})
    assert result["status"] == "blocked" and "expected AnimSequence" in result["reason"] and result["log"]


def test_asset_outside_the_project_is_refused(tmp_path, fake_unreal):
    project, _ = _project(tmp_path)
    stray = tmp_path / "Elsewhere/Run.uasset"
    stray.parent.mkdir()
    stray.write_bytes(b"opaque")
    result = unreal_export.unreal_export({"source": str(stray), "unreal_project": str(project / "Game.uproject"),
                                          "unreal_object_path": "/Game/Run.Run", "cache": str(tmp_path / "cache")})
    assert result["status"] == "blocked" and "not inside" in result["reason"]


def test_desktop_entry_point_reports_failures_as_blocked(tmp_path):
    request = tmp_path / "request.json"
    request.write_text(json.dumps({"source": str(tmp_path / "missing.uasset")}), encoding="utf-8")
    out = tmp_path / "result.json"
    assert desktop.main(["unreal-export", "--request", str(request), "--out", str(out)]) == 0
    assert json.loads(out.read_text(encoding="utf-8"))["status"] == "blocked"
