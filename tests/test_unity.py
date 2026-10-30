"""Unity integration tests; opt in with ``python -m pytest -m unity``."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np
import pytest

from unityanimbridge.anim_parser import parse_anim
from unityanimbridge import coords
from unityanimbridge.blender_runner import find_blender
from unityanimbridge.sampler import sample_clip

pytestmark = pytest.mark.unity
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EDITOR = Path(r"C:\Program Files\Unity\Hub\Editor\6000.4.7f1\Editor\Unity.exe")


def _run(cmd, log, env, timeout=600):
    result = subprocess.run(list(map(str, cmd)), cwd=ROOT, env=env, capture_output=True, text=True, timeout=timeout)
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    assert result.returncode == 0, f"exit {result.returncode}; see {log}"
    return result


@pytest.fixture(scope="module")
def unity_v3():
    editor = Path(os.environ.get("UAB_UNITY", str(DEFAULT_EDITOR)))
    if not editor.is_file():
        pytest.skip("Unity editor unavailable; set UAB_UNITY")
    work = ROOT / "_verify" / "unity" / "v3"
    assets = work / "project" / "Assets"
    (assets / "Editor").mkdir(parents=True, exist_ok=True)
    (work / "temp").mkdir(exist_ok=True)
    env = dict(os.environ, TEMP=str(work / "temp"), TMP=str(work / "temp"), UAB_WORK_DIR=str(work / "temp"))
    packages = work / "project" / "Packages"
    packages.mkdir(exist_ok=True)
    (packages / "manifest.json").write_text('{"dependencies":{}}', encoding="utf-8")
    settings = work / "project" / "ProjectSettings"
    settings.mkdir(exist_ok=True)
    (settings / "ProjectVersion.txt").write_text("m_EditorVersion: 6000.4.7f1\n", encoding="utf-8")
    shutil.copy2(ROOT / "unity" / "Editor" / "UnityAnimBridgeBaker.cs", assets / "Editor")
    shutil.copy2(ROOT / "tests" / "UnityValidation.cs", assets / "Editor")
    for kind in ("zup_cm", "yup_m", "offset_take", "humanoid"):
        _run([find_blender(), "-b", "--factory-startup", "--python-exit-code", "1", "--python",
              ROOT / "tests" / "unity_blender_fixtures.py", "--", assets / f"{kind}.fbx", kind],
             work / f"fixture_{kind}.log", env, 300)
    # A repeated invocation overwrites only the generated clip assets in the isolated project.
    for clip in assets.glob("*.anim"):
        clip.unlink()
    unity_log = work / "unity.log"
    _run([editor, "-batchmode", "-nographics", "-quit", "-projectPath", work / "project",
          "-executeMethod", "UnityValidation.Run", "-logFile", unity_log], work / "unity_process.log", env)
    log = unity_log.read_text(encoding="utf-8")
    assert "UAB_V3_COMPLETE" in log and "error CS" not in log, str(unity_log)
    summary = json.loads((work / "unity_summary.json").read_text())
    yield work, env, summary


@pytest.mark.parametrize("label", ["zup_cm_default", "zup_cm_preserve", "yup_m_default", "yup_m_preserve"])
def test_generic_sampler_and_analytic_rest(unity_v3, label):
    work, env, summary = unity_v3
    item = next(i for i in summary["items"] if i["label"] == label)
    anim = work / "project" / item["clip"]
    baked_path = Path(item["baked"])
    baked = json.loads(baked_path.read_text())
    expected = sample_clip(parse_anim(anim))
    assert len(expected.tracks) == 3
    actual = baked["clips"][0]
    assert actual["frames"] == expected.frames == 11
    maxima = {}
    for path, track in expected.tracks.items():
        for channel in ("t", "r", "s"):
            values = getattr(track, channel)
            if values is None:
                continue
            result = np.asarray(actual["tracks"][path][channel])
            if channel == "r":
                result = result * np.where(np.sum(values * result, axis=1) < 0, -1, 1)[:, None]
            error = float(np.max(np.abs(result - values)))
            maxima[f"{path}:{channel}"] = error
            np.testing.assert_allclose(result, values, atol=1e-4, rtol=0)
            assert np.max(np.abs(values[-1] - values[0])) > 1e-3  # includes actual motion, not only rest
    assert len(maxima) == 3 and {key.rsplit(":", 1)[1] for key in maxima} == {"t", "r", "s"}
    (work / f"{label}_sampler_errors.json").write_text(json.dumps(maxima, indent=2), encoding="utf-8")
    out = work / f"converted_{label}"
    _run([sys.executable, "-m", "unityanimbridge.cli", "convert", anim, "--skeleton", work / "project" / item["model"],
          "--unity-rest", baked_path, "-o", out], work / f"{label}_convert.log", env)
    manifest = json.loads((out / "manifest.json").read_text())
    assert manifest["clips"][0]["status"] in ("ok", "warning"), manifest
    side = json.loads(Path(manifest["clips"][0]["sidecar"]).read_text())
    assert not any("rest pose" in w.lower() for w in side["warnings"]), side
    expected_k = .01 if label.startswith("zup_cm") else 1
    assert side["unity_units_per_fbx_unit"] == pytest.approx(expected_k)


def test_offset_take_absolute_frames(unity_v3):
    work, env, summary = unity_v3
    assert summary["offsetFirst"] == pytest.approx(48)
    assert summary["offsetLast"] == pytest.approx(78)
    baked = json.loads((work / "offset_baked.json").read_text())
    actual = baked["clips"][0]
    assert actual["name"] == "OffsetMid"
    assert actual["frames"] == 13
    assert actual["duration"] == pytest.approx(.5)
    out = work / "converted_offset"
    _run([sys.executable, "-m", "unityanimbridge.cli", "convert", work / "project/Assets/offset_take.fbx", "-o", out],
         work / "offset_convert.log", env)
    manifest = json.loads((out / "manifest.json").read_text())
    assert manifest["clips"][0]["frames"] == 13, manifest
    pose_path = work / "offset_pose.json"
    _run([find_blender(), "-b", "--factory-startup", "--python-exit-code", "1", "--python", ROOT / "tests/blender_helpers.py",
          "--", "dump_pose", manifest["clips"][0]["fbx"], pose_path], work / "offset_dump.log", env, 300)
    pose = json.loads(pose_path.read_text())
    maxima = {}
    for bone in ("Hips", "Spine", "Hand"):
        path = next(p for p in actual["tracks"] if p.endswith("/" + bone))
        tr = actual["tracks"][path]
        expected = coords.unity_to_fbx(*[np.asarray(tr[c]) for c in "trs"], 1.0)
        result = np.asarray([np.asarray(frame[bone]).reshape(4, 4) for frame in pose["frames"]])
        maxima[bone] = float(np.max(np.abs(result - expected)))
        np.testing.assert_allclose(result, expected, atol=1e-4, rtol=0)
    (work / "offset_pose_errors.json").write_text(json.dumps(maxima, indent=2), encoding="utf-8")


def test_humanoid_avatar_sampling_and_root_modes(unity_v3):
    work, env, summary = unity_v3
    assert len(summary["humanoid"]) == 2
    report = {}
    for item in summary["humanoid"]:
        assert item["valid"] and item["human"] and item["clipHuman"], item
        assert "RootT.x" in item["curveNames"] and "RootQ.w" in item["curveNames"]
        name = item["name"]
        baked_path = work / f"{name}_baked.json"
        baked = json.loads(baked_path.read_text())
        clip = baked["clips"][0]
        assert clip["frames"] == 31 and clip["duration"] == pytest.approx(1.25)
        spine = np.asarray(clip["tracks"]["Armature/Hips/Spine"]["r"])
        expected_rotation = [np.sin(.25), 0, 0, np.cos(.25)]  # source animated X rotation = .5 rad
        if np.dot(spine[-1], expected_rotation) < 0:
            spine = -spine
        np.testing.assert_allclose(spine[-1], expected_rotation, atol=1e-4, rtol=0)
        root_t = np.asarray(clip["tracks"][""]["t"])
        hips_t = np.asarray(clip["tracks"]["Armature/Hips"]["t"])
        if name == "HumanBakedRoot":
            np.testing.assert_allclose(root_t, 0, atol=1e-5)
            assert hips_t[-1, 0] == pytest.approx(-.3, abs=1e-4)
        else:
            assert root_t[-1, 0] == pytest.approx(-.3, abs=1e-4)
            assert hips_t[-1, 0] == pytest.approx(0, abs=1e-4)
        report[name] = {"source_RootT_end": item["rootTEnd"], "source_RootQ_end": item["rootQEnd"],
                        "gameobject_t_end": root_t[-1].tolist(), "hips_t_end": hips_t[-1].tolist(),
                        "spine_q_end": spine[-1].tolist()}
        out = work / f"converted_{name}"
        _run([sys.executable, "-m", "unityanimbridge.cli", "convert", "--baked", baked_path,
              "--skeleton", work / "project/Assets/humanoid.fbx", "--root-to-bone", "Hips", "-o", out],
             work / f"{name}_convert.log", env)
        manifest = json.loads((out / "manifest.json").read_text())
        assert manifest["clips"][0]["status"] in ("ok", "warning"), manifest
    (work / "humanoid_root_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
