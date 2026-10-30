"""Desktop IPC logic: truthful blocked states, FK, mapping and isolation paths."""
import json
from pathlib import Path
import uuid

import numpy as np
import pytest

from unityanimbridge import desktop

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture
def work():
    path = ROOT / "_verify" / "desktop" / "unit_tests" / uuid.uuid4().hex
    path.mkdir(parents=True)
    return path


def rig():
    identity = np.eye(4).flatten().tolist()
    return {"bones": [{"name": "Hips", "path": "Armature/Hips", "parent": -1, "rest_matrix": identity},
                       {"name": "Spine", "path": "Armature/Hips/Spine", "parent": 0, "rest_matrix": identity}],
            "facts": {"duplicate_bones": []}, "warnings": []}


def test_baked_preview_evaluates_parent_rotation_and_motion(work):
    source = work / "baked.json"
    source.write_text(json.dumps({"uab_baked": 1, "fps": 1, "clips": [{"name": "Move", "frames": 2, "duration": 1,
        "tracks": {"Hips": {"t": [[0, 0, 0], [2, 0, 0]], "r": [[0, 0, 0, 1], [0, 0, 2 ** -.5, 2 ** -.5]]},
                   "Hips/Hand": {"t": [[1, 0, 0], [1, 0, 0]]}}}]}), encoding="utf-8")
    result = desktop.preview({"source": str(source)})
    assert result["status"] == "ready" and result["kind"] == "bone-motion"
    assert [bone["parent"] for bone in result["bones"]] == [-1, 0]
    np.testing.assert_allclose(result["frames"][0], [[0, 0, 0], [-1, 0, 0]], atol=1e-10)
    np.testing.assert_allclose(result["frames"][1], [[-2, 0, 0], [-2, 1, 0]], atol=1e-10)


def test_missing_model_never_produces_fake_preview(work):
    source = work / "Binary.anim"
    source.write_bytes(b"binary animation placeholder")
    result = desktop.preview({"source": str(source)})
    assert result["status"] == "blocked" and result["code"] == "needsSkeleton"
    assert "frames" not in result
    info = desktop.inspect({"source": str(source)})
    assert info["type"] == "binary" and info["route"] == "B"
    assert not info["capabilities"]["preview"] and not info["capabilities"]["bake"]


def test_existing_unity_project_cannot_be_helper_cache(work):
    (work / "Assets").mkdir()
    (work / "ProjectSettings").mkdir()
    with pytest.raises(ValueError, match="existing Unity project"):
        desktop._work({"cache": str(work)}, "helper")
    with pytest.raises(ValueError, match="existing Unity project"):
        desktop._work({"cache": str(work / "subfolder")}, "helper")


def test_mapping_checks_missing_and_duplicate_targets(work, monkeypatch):
    model = work / "Model.fbx"
    model.write_bytes(b"not-read-by-unit-adapter")
    source = work / "Clip.anim"
    source.write_text("%YAML", encoding="utf-8")
    class Clip:
        is_humanoid = False
        channels = {"Armature/Hips": {}, "Armature/Hips/Spine": {}}
    monkeypatch.setattr(desktop, "parse_anim", lambda _: Clip())
    monkeypatch.setattr(desktop, "_blender", lambda *_: rig())
    request = {"model": str(model), "source": str(source), "cache": str(work / "cache")}
    assert desktop.validate(request)["valid"]
    bad = desktop.validate(dict(request, mapping={"Armature/Hips": "Missing"}))
    assert bad["status"] == "invalid" and bad["missing"] == ["Armature/Hips"]
    duplicate = desktop.validate(dict(request, mapping={"Armature/Hips": "Spine"}))
    assert not duplicate["valid"] and any("Multiple source paths" in error for error in duplicate["errors"])


def test_rest_and_original_bone_duplicates_are_not_complete(work, monkeypatch):
    model = work / "Model.fbx"
    model.write_bytes(b"not-read-by-unit-adapter")
    data = rig()
    data["facts"]["duplicate_bones"] = ["Spine"]
    data["bones"][0]["rest_matrix"] = [0.0] * 16
    monkeypatch.setattr(desktop, "_blender", lambda *_: data)
    result = desktop.validate({"model": str(model), "cache": str(work / "cache")})
    assert not result["valid"] and result["duplicates"] == ["Spine"]
    assert any("rest transform" in error for error in result["errors"])


def test_ipc_error_writes_utf8_result(work, capsys):
    request, output = work / "request.json", work / "결과.json"
    request.write_text(json.dumps({"source": str(work / "없는 파일.anim")}), encoding="utf-8")
    assert desktop.main(["inspect", "--request", str(request), "--out", str(output)]) == 2
    result = json.loads(output.read_text(encoding="utf-8"))
    assert result["status"] == "error" and "없는 파일" in result["reason"]
    assert json.loads(capsys.readouterr().out)["result"] == str(output.resolve())


def test_bake_refuses_output_conflict_before_starting_unity(work):
    source, model, output = work / "Clip.anim", work / "Model.fbx", work / "output"
    source.write_bytes(b"binary clip placeholder")
    model.write_bytes(b"FBX placeholder")
    output.mkdir()
    (output / "keep.txt").write_text("existing data", encoding="utf-8")
    result = desktop.bake({"source": str(source), "skeleton": str(model), "output": str(output)})
    assert result["status"] == "blocked" and result["code"] == "outputConflict"
    assert (output / "keep.txt").read_text() == "existing data"


def test_fbx_humanoid_meta_is_forwarded_to_unity_bake(work, monkeypatch):
    source, model = work / "Human.fbx", work / "Model.fbx"
    source.write_bytes(b"FBX fixture")
    model.write_bytes(b"model fixture")
    Path(str(source) + ".meta").write_text("ModelImporter:\n  animationType: 3\n", encoding="utf-8")
    captured = {}
    def helper(request, operation, cache):
        captured.update(request)
        assert operation == "bake"
        return {"status": "needsAvatar", "connected": True}
    monkeypatch.setattr(desktop, "_unity", helper)
    result = desktop.bake({"source": str(source), "skeleton": str(model), "clip": "Selected", "cache": str(work / "cache"), "output": str(work / "out")})
    assert captured["humanoid"] is True and captured["clip"] == "Selected"
    assert result["status"] == "blocked" and result["code"] == "needsAvatar"


def test_parameter_only_anim_does_not_simulate_character_playback(work, monkeypatch):
    source = work / "Parameter.anim"
    source.write_text("%YAML", encoding="utf-8")
    class Clip:
        is_humanoid = False
        channels = {}
    monkeypatch.setattr(desktop, "parse_anim", lambda _: Clip())
    result = desktop.preview({"source": str(source)})
    assert result["status"] == "blocked" and result["code"] == "unsupported"
    assert "frames" not in result
