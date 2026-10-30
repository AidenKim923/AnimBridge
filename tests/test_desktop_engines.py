"""Real fixture previews and opt-in isolated Unity helper verification."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import uuid

import numpy as np
import pytest

from unityanimbridge import desktop

ROOT = Path(__file__).resolve().parents[1]
V3 = ROOT / "_verify" / "unity" / "v3"


@pytest.fixture
def cache():
    path = ROOT / "_verify" / "desktop" / "engine_tests" / uuid.uuid4().hex
    path.mkdir(parents=True)
    return path


def require_v3():
    if not (V3 / "zup_cm_default_baked.json").is_file():
        pytest.skip("Run the opt-in V3 Unity fixture first")


@pytest.mark.blender
def test_actual_anim_preview_matches_unity_world_fk(cache):
    require_v3()
    request = {"source": str(V3 / "project" / "Assets" / "zup_cm_default.anim"),
               "skeleton": str(V3 / "project" / "Assets" / "zup_cm.fbx"), "cache": str(cache)}
    actual = desktop.preview(request)
    expected = desktop.preview({"source": str(V3 / "zup_cm_default_baked.json")})
    assert actual["status"] == "ready" and actual["frame_count"] == 11
    assert [(bone["name"], bone["parent"]) for bone in actual["bones"]] == [("Hips", -1), ("Spine", 0), ("Hand", 1)]
    wanted = [next(i for i, other in enumerate(expected["bones"]) if other["name"] == bone["name"]) for bone in actual["bones"]]
    error = float(np.max(np.abs(np.asarray(actual["frames"]) - np.asarray(expected["frames"])[:, wanted])))
    (cache / "world_position_error.json").write_text(json.dumps({"max_m": error}), encoding="utf-8")
    assert error < 1e-6
    assert np.max(np.abs(np.asarray(actual["frames"])[-1] - actual["frames"][0])) > .05


@pytest.mark.unity
def test_actual_fbx_mesh_and_bone_motion(cache):
    require_v3()
    if not desktop.DEFAULT_UNITY.is_file():
        pytest.skip("Unity unavailable")
    result = desktop.preview({"source": str(V3 / "project" / "Assets" / "humanoid.fbx"), "cache": str(cache),
                              "include_mesh": True, "max_point_frames": 44})
    assert result["status"] == "ready", result
    assert result["default_character"]["name"] == "Mocapture Humanoid"
    assert result["helper"]["avatar"] == {"exists": True, "is_valid": True, "is_human": True}
    assert "Hips" in {bone["name"] for bone in result["bones"]}
    assert len(result["frames"]) > 1 and result["duration"] > 0
    assert result["mesh"]["kind"] == "evaluated-skinned-mesh" and result["mesh"]["triangles"]
    assert len(result["mesh"]["frames"]) == len(result["frames"])
    assert len(result["frames"]) == 2
    assert any("payload budget" in warning for warning in result["warnings"])
    assert not np.allclose(result["frames"][0], result["frames"][-1])
    assert not np.allclose(result["mesh"]["frames"][0], result["mesh"]["frames"][-1])


@pytest.mark.unity
def test_actual_helper_session_and_humanoid_avatar_are_isolated(cache):
    require_v3()
    if not desktop.DEFAULT_UNITY.is_file():
        pytest.skip("Unity 6000.4.7f1 unavailable")
    connected = desktop.helper_test({"cache": str(cache)})
    assert connected["status"] == "connected" and connected["connected"]
    assert connected["unity_version"] == "6000.4.7f1" and connected["helper_version"] == desktop.HELPER_VERSION
    assert len(connected["session"]) == 32 and connected["isolated"]
    project = Path(connected["project"])
    assert sorted(path.name for path in (project / "Assets").rglob("*.cs")) == ["AnimBridgeDesktopHelper.cs", "AnimBridgePreview.cs", "UnityAnimBridgeBaker.cs"]
    # A package's neighboring source code must never enter the helper project.
    data = cache / "package_data"
    data.mkdir()
    source = V3 / "project" / "Assets" / "humanoid.fbx"
    shutil.copyfile(source, data / source.name)
    shutil.copyfile(Path(str(source) + ".meta"), data / (source.name + ".meta"))
    (data / "Editor").mkdir()
    (data / "Editor" / "DoNotCopy.cs").write_text("#error Package code must never execute", encoding="utf-8")
    avatar = desktop.validate({"model": str(data / source.name), "humanoid": True, "cache": str(cache)})
    assert avatar["status"] == "valid" and avatar["valid"]
    assert avatar["avatar"] == {"exists": True, "is_valid": True, "is_human": True}
    helper = avatar["helper"]
    assert helper["required_missing"] == [] and helper["duplicates"] == []
    helper_project = Path(helper["project"])
    assert not list(helper_project.rglob("DoNotCopy.cs"))
    assert sorted(path.name for path in (helper_project / "Assets").rglob("*.cs")) == ["AnimBridgeDesktopHelper.cs", "AnimBridgePreview.cs", "UnityAnimBridgeBaker.cs"]
    assert list((helper_project / "Assets" / "Data").rglob("*.fbx"))
    (cache / "helper_validation_evidence.json").write_text(json.dumps({"connected": connected, "avatar": avatar}, indent=2), encoding="utf-8")


@pytest.mark.unity
def test_three_bone_generic_rig_cannot_be_claimed_humanoid(cache):
    require_v3()
    if not desktop.DEFAULT_UNITY.is_file():
        pytest.skip("Unity unavailable")
    result = desktop.validate({"model": str(V3 / "project" / "Assets" / "zup_cm.fbx"), "humanoid": True, "cache": str(cache)})
    assert not result["valid"] and result["status"] == "needsAvatar"
    assert not result["avatar"]["is_human"]
    assert result["helper"]["required_missing"]


@pytest.mark.unity
def test_valid_avatar_does_not_turn_a_generic_clip_into_humanoid(cache):
    require_v3()
    if not desktop.DEFAULT_UNITY.is_file():
        pytest.skip("Unity unavailable")
    result = desktop._unity({"model": str(V3 / "project" / "Assets" / "humanoid.fbx"),
                             "source": str(V3 / "project" / "Assets" / "zup_cm_default.anim"),
                             "humanoid": True}, "bake", cache / "mismatched_clip")
    assert result["status"] == "error" and not result["clip_humanoid"]
    assert "not imported as Humanoid" in result["reason"]


@pytest.mark.unity
def test_actual_humanoid_anim_preview_evaluates_in_isolated_unity(cache):
    require_v3()
    if not desktop.DEFAULT_UNITY.is_file():
        pytest.skip("Unity unavailable")
    project = cache / "fixture_project"
    for path in (project / "Assets" / "Editor", project / "Assets" / "Data", project / "Packages", project / "ProjectSettings", cache / "temp"):
        path.mkdir(parents=True, exist_ok=True)
    (project / "Packages" / "manifest.json").write_text('{"dependencies":{}}', encoding="utf-8")
    (project / "ProjectSettings" / "ProjectVersion.txt").write_text("m_EditorVersion: 6000.4.7f1\n", encoding="utf-8")
    shutil.copyfile(ROOT / "tests" / "desktop_fixtures.cs", project / "Assets" / "Editor" / "desktop_fixtures.cs")
    source = V3 / "project" / "Assets" / "humanoid.fbx"
    shutil.copyfile(source, project / "Assets" / "Data" / source.name)
    shutil.copyfile(Path(str(source) + ".meta"), project / "Assets" / "Data" / (source.name + ".meta"))
    animation = cache / "Human.anim"
    command = [str(desktop.DEFAULT_UNITY), "-batchmode", "-nographics", "-quit", "-projectPath", str(project),
               "-executeMethod", "DesktopFixtureBuilder.Run", "-desktopFixtureSource", "Assets/Data/humanoid.fbx",
               "-desktopFixtureOut", str(animation), "-logFile", str(cache / "fixture_unity.log")]
    env = dict(os.environ, TEMP=str(cache / "temp"), TMP=str(cache / "temp"))
    completed = subprocess.run(command, cwd=project, env=env, capture_output=True, timeout=600)
    (cache / "fixture_process.log").write_bytes(completed.stdout + completed.stderr)
    assert completed.returncode == 0 and animation.is_file(), str(cache / "fixture_unity.log")
    assert desktop.inspect({"source": str(animation)})["type"] == "humanoid"
    result = desktop.preview({"source": str(animation), "skeleton": str(source), "cache": str(cache), "fps": 24})
    assert result["status"] == "ready", result
    assert result["helper"]["clip_humanoid"] and result["helper"]["avatar"]["is_valid"] and result["helper"]["avatar"]["is_human"]
    assert result["preview_scope"] == "unity-evaluated-skinned-character"
    expected = desktop.preview({"source": str(V3 / "HumanBakedRoot_baked.json")})
    assert result["frame_count"] == expected["frame_count"] == 31
    assert result["duration"] == pytest.approx(expected["duration"]) == 1.25
    # Unity's Transform traversal and the baked hierarchy use different sibling
    # orders; compare the same paths rather than incidental array positions.
    paths = [bone["path"] for bone in result["bones"]]
    expected_paths = [bone["path"] for bone in expected["bones"]]
    assert set(paths) == set(expected_paths)
    indices = [expected_paths.index(path) for path in paths]
    error = float(np.max(np.abs(np.asarray(result["frames"]) - np.asarray(expected["frames"])[:, indices])))
    (cache / "human_preview_evidence.json").write_text(json.dumps({"max_world_position_m": error, "preview": result}, indent=2), encoding="utf-8")
    assert error < 1e-5
    assert not np.allclose(result["frames"][0], result["frames"][-1])
    output = cache / "native_staging"
    output.mkdir()
    exported = desktop.bake({"source": str(animation), "skeleton": str(source), "cache": str(cache),
                             "fps": 24, "output": str(output)})
    assert exported["status"] == "ok", exported
    manifest = json.loads(Path(exported["manifest"]).read_text())
    assert len(manifest["clips"]) == 1
    entry = manifest["clips"][0]
    assert entry["status"] in ("ok", "warning") and entry["frames"] == 31
    assert entry["duration"] == pytest.approx(1.25)
    assert Path(entry["fbx"]).resolve().is_relative_to(output.resolve())
    (cache / "human_export_evidence.json").write_text(json.dumps(exported, indent=2), encoding="utf-8")
