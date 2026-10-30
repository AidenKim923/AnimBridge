"""Regression for independent FBX Model-default and skin-bind references."""
import copy
import json
from pathlib import Path
import subprocess

import numpy as np
import pytest

from unityanimbridge.blender_runner import find_blender
from unityanimbridge.cli import main as convert

pytestmark = pytest.mark.blender
HELPERS = Path(__file__).with_name("blender_helpers.py")


def run_blender(script, *arguments):
    subprocess.run([str(find_blender()), "-b", "--factory-startup", "--python-exit-code", "1",
                    "--python", str(script), "--", *map(str,arguments)],
                   check=True, capture_output=True, timeout=300)


def test_bind_pose_is_not_the_unity_default_reference(tmp_path):
    source = tmp_path / "different_poses.fbx"
    run_blender(Path(__file__).with_name("blender_rest_helpers.py"), source)
    reference = {"t": [-.25,1,0], "r": [0,0,0,1], "s": [1,1,1]}
    path = "Armature/Hips/Spine"
    baked = {"uab_baked": 1, "model": "different_poses.fbx", "rest": {path: reference},
             "clips": [{"name": "DifferentPoses", "fps": 2, "frames": 3, "duration": 1,
                        "loop": False, "events": [], "tracks": {path: {
                            "t": [[-.25,1,0],[-.35,1,0],[-.45,1,0]],
                            "r": [[0,0,0,1]]*3, "s": [[1,1,1]]*3}}}]}
    baked_path = tmp_path / "baked.json"
    baked_path.write_text(json.dumps(baked),encoding="utf-8")
    output = tmp_path / "converted"
    assert convert(["convert", "--baked", str(baked_path), "--skeleton", str(source), "-o", str(output)]) == 0
    manifest = json.loads((output / "manifest.json").read_text())
    pose_path = tmp_path / "poses.json"
    run_blender(HELPERS,"dump_pose",manifest["clips"][0]["fbx"],pose_path)
    poses = json.loads(pose_path.read_text())["frames"]
    assert len(poses) == 3
    for index, pose in enumerate(poses):
        actual = np.asarray(pose["Spine"]).reshape(4,4)
        np.testing.assert_allclose(actual[:3,3],[25+index*10,100,0],atol=1e-4,rtol=0)
        np.testing.assert_allclose(actual[:3,:3],np.eye(3),atol=1e-5,rtol=0)
    # A real mismatch with the independently parsed default must still fail;
    # switching references must not disable or calibrate away the safety check.
    baked["rest"][path]["t"][0] = -.26
    baked_path.write_text(json.dumps(baked),encoding="utf-8")
    bad_output = tmp_path / "bad_reference"
    assert convert(["convert", "--baked", str(baked_path), "--skeleton", str(source), "-o", str(bad_output)]) == 1
    sidecar = json.loads((bad_output / "DifferentPoses.uab.json").read_text())
    assert "FBX default local transform" in sidecar["error"]


def test_real_armature_and_model_root_motion_are_preserved(tmp_path):
    helper = Path(__file__).with_name("blender_root_helpers.py")
    source = tmp_path / "real_root.fbx"
    run_blender(helper,"make",source)
    root_q = [-np.sqrt(.5),0,0,np.sqrt(.5)]
    root_t = [[0,0,0],[.25,.5,.75],[.5,1,1.5]]
    wrapper_t = [[0,0,0],[.1,.2,.3],[.2,.4,.6]]
    yaw = np.asarray([0,.3,.6])
    wrapper_q = [[0,float(np.sin(angle/2)),0,float(np.cos(angle/2))]for angle in yaw]
    identity = {"t":[0,0,0],"r":[0,0,0,1],"s":[1,1,1]}
    baked = {"uab_baked":1,"rest":{"":identity,"Bip001":{"t":[0,0,0],"r":root_q,"s":[100,100,100]}},
             "clips":[{"name":"MovingRoot","fps":2,"frames":3,"duration":1,"events":[],"tracks":{
                 "":{"t":wrapper_t,"r":wrapper_q,"s":[[1,1,1]]*3},
                 "Bip001":{"t":root_t,"r":[root_q]*3,"s":[[100,100,100]]*3}}}]}
    second = copy.deepcopy(baked["clips"][0])
    second["name"] = "MovingRootAgain"
    baked["clips"].append(second)
    baked_path = tmp_path / "root.json"
    baked_path.write_text(json.dumps(baked),encoding="utf-8")
    output = tmp_path / "root_output"
    assert convert(["convert","--baked",str(baked_path),"--skeleton",str(source),"-o",str(output)]) == 0
    manifest = json.loads((output / "manifest.json").read_text())
    assert len(manifest["clips"]) == 2
    side = json.loads(Path(manifest["clips"][0]["sidecar"]).read_text())
    assert not side["dropped"]
    dump_path = tmp_path / "root_pose.json"
    run_blender(helper,"dump",manifest["clips"][0]["fbx"],dump_path)
    actual = json.loads(dump_path.read_text())
    assert actual["object"] == "Bip001" and actual["bones"] == ["Pelvis"]
    for index,angle in enumerate(yaw):
        c,s = np.cos(angle),np.sin(angle)
        rotate_y = np.asarray([[c,0,s],[0,1,0],[-s,0,c]])
        unity_position = rotate_y @ root_t[index] + wrapper_t[index]
        expected_position = unity_position * [-1,1,1]
        expected_position = expected_position[[0,2,1]] * [1,-1,1]
        expected_rotation = np.asarray([[c,s,0],[-s,c,0],[0,0,1]])
        matrix = np.asarray(actual["frames"][index])
        np.testing.assert_allclose(matrix[:3,3],expected_position,atol=1e-5,rtol=0)
        np.testing.assert_allclose(matrix[:3,:3],expected_rotation,atol=1e-5,rtol=0)
