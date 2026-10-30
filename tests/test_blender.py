"""Blender round trip: build skeletons, convert Unity clips, and reimport the FBX."""
import json
import math
import subprocess
from pathlib import Path

import numpy as np
import pytest

from unityanimbridge import coords
from unityanimbridge.blender_runner import find_blender
from unityanimbridge.cli import main as uab_main

HELPERS = Path(__file__).with_name("blender_helpers.py")
pytestmark = pytest.mark.blender


def blender(*args):
    exe = find_blender()
    subprocess.run([str(exe), "-b", "--factory-startup", "--python-exit-code", "1", "--python", str(HELPERS), "--", *map(str, args)],
                   check=True, capture_output=True, timeout=300)


def vec_key(t, v, comps):
    return ("      - serializedVersion: 3\n        time: %r\n" % float(t)
            + "        value: {%s}\n" % ", ".join(f"{c}: {float(x)!r}" for c, x in zip(comps, v))
            + "        inSlope: {%s}\n" % ", ".join(f"{c}: 0" for c in comps)
            + "        outSlope: {%s}\n" % ", ".join(f"{c}: 0" for c in comps)
            + "        tangentMode: 0\n        weightedMode: 0\n")


def anim_yaml(pos_path, pos_keys, rot_path, rot_keys, rate=10):
    """Keys at every sample frame avoid interpolation differences."""
    def block(path, keys, comps):
        body = "".join(vec_key(t, v, comps) for t, v in keys)
        return f"  - curve:\n      serializedVersion: 2\n      m_Curve:\n{body}      m_PreInfinity: 2\n      m_PostInfinity: 2\n      m_RotationOrder: 4\n    path: {path}\n"
    return (
        "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!74 &7400000\nAnimationClip:\n  m_Name: Test\n"
        "  m_RotationCurves:\n" + block(rot_path, rot_keys, "xyzw")
        + "  m_PositionCurves:\n" + block(pos_path, pos_keys, "xyz")
        + f"  m_ScaleCurves: []\n  m_EulerCurves: []\n  m_FloatCurves: []\n  m_SampleRate: {rate}\n"
        "  m_AnimationClipSettings:\n    m_StartTime: 0\n    m_StopTime: 1\n    m_LoopTime: 0\n  m_EditorCurves: []\n  m_Events: []\n"
    )


def unity_motion(n=11):
    ts = np.arange(n) / 10.0
    pos = np.stack([0.1 * ts, 1 + 0.05 * ts, np.full(n, 0.02)], 1)
    ang = 0.8 * ts
    axis = np.array([0.3, 0.8, 0.52])
    axis /= np.linalg.norm(axis)
    rot = np.concatenate([axis[None] * np.sin(ang / 2)[:, None], np.cos(ang / 2)[:, None]], 1)
    return ts, pos, rot


@pytest.mark.parametrize("unit,usf", [("m", 1.0), ("cm", None)])
def test_roundtrip_generic_anim(tmp_path, unit, usf):
    skel = tmp_path / f"skel_{unit}.fbx"
    blender("make_skeleton", skel, unit)
    blender("dump_pose", skel, tmp_path / "rest.json")
    rest = json.loads((tmp_path / "rest.json").read_text())["frames"][0]

    ts, pos, rot = unity_motion()
    text = anim_yaml("Armature/Hips", [(t, p) for t, p in zip(ts, pos)],
                     "Armature/Hips/Spine", [(t, q) for t, q in zip(ts, rot)])
    anim = tmp_path / "Test.anim"
    anim.write_text(text, encoding="utf-8")
    out = tmp_path / "out"
    code = uab_main(["convert", str(anim), "--skeleton", str(skel), "-o", str(out)])
    manifest = json.loads((out / "manifest.json").read_text())
    assert code == 0, manifest
    fbx = manifest["clips"][0]["fbx"]
    side = json.loads(Path(manifest["clips"][0]["sidecar"]).read_text())
    k = side["unity_units_per_fbx_unit"]
    if usf is not None:
        assert k == pytest.approx(usf / 100)  # Blender writes UnitScaleFactor=1 (cm) by default
    blender("dump_pose", fbx, tmp_path / "pose.json")
    pose = json.loads((tmp_path / "pose.json").read_text())
    assert len(pose["frames"]) == 11
    assert pose["object"] == "Armature"

    exp_hips = coords.unity_to_fbx(pos, np.tile([0, 0, 0, 1.0], (11, 1)), np.ones((11, 3)), k)
    exp_spine = coords.unity_to_fbx(np.zeros((11, 3)), rot, np.ones((11, 3)), k)
    # Blender re-import keeps FBX units in armature space only if the round trip is unit-consistent.
    # Compare translations through the ratio to the rest skeleton to stay unit-agnostic.
    rest_spine = np.array(rest["Spine"]).reshape(4, 4)
    for f, frame in enumerate(pose["frames"]):
        hips = np.array(frame["Hips"]).reshape(4, 4)
        spine = np.array(frame["Spine"]).reshape(4, 4)
        hand = np.array(frame["Hand"]).reshape(4, 4)
        np.testing.assert_allclose(hips[:3, 3], exp_hips[f][:3, 3], rtol=1e-5, atol=1e-3)
        np.testing.assert_allclose(spine[:3, :3], exp_spine[f][:3, :3], atol=2e-4)
        np.testing.assert_allclose(spine[:3, 3], rest_spine[:3, 3], atol=1e-4)  # unanimated -> rest
        np.testing.assert_allclose(hand, np.array(rest["Hand"]).reshape(4, 4), atol=1e-4)


def test_split_fbx_resamples_between_keys(tmp_path):
    src = tmp_path / "Anim.fbx"
    blender("make_anim_fbx", src)
    meta = tmp_path / "Anim.fbx.meta"
    meta.write_text(
        "fileFormatVersion: 2\nModelImporter:\n  animations:\n    clipAnimations:\n"
        "    - name: Mid\n      takeName: Take1\n      firstFrame: 10\n      lastFrame: 20\n      loopTime: 1\n      mirror: 0\n"
        "      events:\n      - time: 0.5\n        functionName: Hit\n        data: \n        floatParameter: 0\n        intParameter: 0\n"
        "  meshes:\n    globalScale: 1\n    useFileScale: 1\n  animationType: 2\n",
        encoding="utf-8",
    )
    out = tmp_path / "out"
    code = uab_main(["convert", str(src), "-o", str(out)])
    manifest = json.loads((out / "manifest.json").read_text())
    assert code == 0, manifest
    clip = manifest["clips"][0]
    assert clip["clip"] == "Mid" and clip["frames"] == 11
    side = json.loads(Path(clip["sidecar"]).read_text())
    # The helper FBX is written at Blender's default 24 fps. Event time is normalized (0.5) over 10 frames / 24 fps.
    assert side["fps"] == pytest.approx(24)
    assert side["events"][0]["time"] == pytest.approx(0.5 * 10 / 24, rel=1e-6)
    blender("dump_pose", clip["fbx"], tmp_path / "pose.json")
    pose = json.loads((tmp_path / "pose.json").read_text())
    assert len(pose["frames"]) == 11
    s0 = np.array(pose["frames"][0]["Spine"]).reshape(4, 4)[:3, :3]
    s1 = np.array(pose["frames"][-1]["Spine"]).reshape(4, 4)[:3, :3]
    rel = s0.T @ s1
    ang = math.degrees(math.acos(max(-1, min(1, (np.trace(rel) - 1) / 2))))
    # Source quaternion components go linearly from (1,0,0,0) to (cos .6, sin .6, 0, 0) over frames 0..30.
    def angle_at(s):
        return 2 * math.atan2(s * math.sin(0.6), (1 - s) + s * math.cos(0.6))

    expected = math.degrees(angle_at(20 / 30) - angle_at(10 / 30))
    assert ang == pytest.approx(expected, abs=0.05)
