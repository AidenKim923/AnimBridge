"""Independent source-reference rejection and connected split regressions."""
import json
from pathlib import Path
import subprocess

import numpy as np
import pytest

from unityanimbridge.blender_runner import find_blender
from unityanimbridge.cli import main as convert
from unityanimbridge import coords

pytestmark = pytest.mark.blender


def blender(script,*args):
    completed = subprocess.run([str(find_blender()),"-b","--factory-startup","--python-exit-code","1",
        "--python",str(Path(__file__).with_name(script)),"--",*map(str,args)],capture_output=True,timeout=300)
    assert completed.returncode == 0,(completed.stdout+completed.stderr).decode("utf-8","replace")


def baked_source(tmp_path):
    path = "Armature/Hips/Spine"
    baked = {"uab_baked":1,"rest":{path:{"t":[-.25,1,0],"r":[0,0,0,1],"s":[1,1,1]}},
        "clips":[{"name":"Check","fps":2,"frames":3,"duration":1,"events":[],"tracks":{
            path:{"t":[[-.25,1,0]]*3,"r":[[0,0,0,1]]*3,"s":[[1,1,1]]*3}}}]}
    filename = tmp_path / "baked.json"
    filename.write_text(json.dumps(baked),encoding="utf-8")
    return filename


@pytest.mark.parametrize("mode",["default_scale","bind_scale","bind_shear","cluster_scale","ambiguous_bone"])
def test_invalid_independent_reference_is_rejected(tmp_path,monkeypatch,mode):
    monkeypatch.setenv("UAB_WORK_DIR",str(tmp_path / "jobs"))
    source = tmp_path / (mode+".fbx")
    blender("blender_rest_helpers.py",source,mode)
    output = tmp_path / "out"
    assert convert(["convert","--baked",str(baked_source(tmp_path)),"--skeleton",str(source),"-o",str(output)]) == 3
    manifest = json.loads((output / "manifest.json").read_text())
    assert not manifest["clips"]
    text = " ".join(manifest["warnings"])
    expected = "unique FBX Model" if mode == "ambiguous_bone" else "skin bind" if mode.startswith("bind_") or mode == "cluster_scale" else "scale or shear"
    assert expected in text


def test_limb_node_wins_over_same_named_mesh(tmp_path,monkeypatch):
    monkeypatch.setenv("UAB_WORK_DIR",str(tmp_path / "jobs"))
    source = tmp_path / "duplicate_names.fbx"
    blender("blender_rest_helpers.py",source,"duplicate_mesh_name")
    assert convert(["convert","--baked",str(baked_source(tmp_path)),"--skeleton",str(source),"-o",str(tmp_path / "out")]) == 0


def test_split_preserves_connected_child_translation(tmp_path,monkeypatch):
    monkeypatch.setenv("UAB_WORK_DIR",str(tmp_path / "jobs"))
    source = tmp_path / "connected.fbx"
    blender("blender_connected_helpers.py","make",source)
    original_dump = tmp_path / "source_pose.json"
    blender("blender_connected_helpers.py","dump",source,original_dump)
    original = json.loads(original_dump.read_text())
    assert "Spine" in original["connected_after_import"]
    source.with_suffix(".fbx.meta").write_text(
        "fileFormatVersion: 2\nModelImporter:\n  animations:\n    clipAnimations:\n"
        "    - name: ChildMove\n      takeName: TranslateChild\n      firstFrame: 10\n      lastFrame: 20\n"
        "  meshes:\n    globalScale: 1\n    useFileScale: 1\n  animationType: 2\n",encoding="utf-8")
    output = tmp_path / "out"
    assert convert(["convert",str(source),"-o",str(output)]) == 0
    manifest = json.loads((output / "manifest.json").read_text())
    dump = tmp_path / "pose.json"
    blender("blender_connected_helpers.py","dump",manifest["clips"][0]["fbx"],dump)
    poses = json.loads(dump.read_text())
    matrices = np.asarray(poses["frames"])
    # Source keyed endpoints are 0 and .6 Blender units; the FBX importer
    # reconnected joints, so the old route C returned zero for every sample.
    assert len(matrices) == 11
    assert abs(matrices[-1,0,3]-matrices[0,0,3]) > .1
    np.testing.assert_allclose(matrices,np.asarray(original["frames"])[10:21],atol=1e-4,rtol=0)


def test_unmapped_model_root_motion_requires_a_target(tmp_path,monkeypatch):
    monkeypatch.setenv("UAB_WORK_DIR",str(tmp_path / "jobs"))
    source = tmp_path / "root.fbx"
    blender("blender_helpers.py","make_skeleton",source,"cm")
    identity = {"t":[0,0,0],"r":[0,0,0,1],"s":[1,1,1]}
    baked = {"uab_baked":1,"rest":{"":identity},"clips":[{"name":"Root","fps":2,"frames":3,
        "duration":1,"events":[],"tracks":{"":{"t":[[0,0,0],[.2,0,0],[.4,0,0]],
            "r":[[0,0,0,1]]*3,"s":[[1,1,1]]*3}}}]}
    filename = tmp_path / "root.json"
    filename.write_text(json.dumps(baked),encoding="utf-8")
    assert convert(["convert","--baked",str(filename),"--skeleton",str(source),"-o",str(tmp_path / "blocked")]) == 1
    error = json.loads((tmp_path / "blocked" / "Root.uab.json").read_text())["error"]
    assert "use --root-to-bone" in error
    assert convert(["convert","--baked",str(filename),"--skeleton",str(source),"--root-to-bone","Hips","-o",str(tmp_path / "preserved")]) == 0
    # Equivalent quaternion signs on a static model root are harmless.
    baked["clips"][0]["tracks"][""]["t"] = [[0,0,0]]*3
    baked["clips"][0]["tracks"][""]["r"] = [[0,0,0,-1]]*3
    filename.write_text(json.dumps(baked),encoding="utf-8")
    assert convert(["convert","--baked",str(filename),"--skeleton",str(source),"-o",str(tmp_path / "static")]) == 0


def test_pivot_pre_post_reference_and_pose(tmp_path,monkeypatch):
    monkeypatch.setenv("UAB_WORK_DIR",str(tmp_path / "jobs"))
    source = tmp_path / "pivot.fbx"
    blender("blender_rest_helpers.py",source,"pivot")
    def translation(values):
        matrix = np.eye(4)
        matrix[:3,3] = values
        return matrix
    def rotation(axis,degrees):
        matrix = np.eye(4)
        c,s = np.cos(np.radians(degrees)),np.sin(np.radians(degrees))
        if axis == "x": matrix[:3,:3] = [[1,0,0],[0,c,-s],[0,s,c]]
        elif axis == "y": matrix[:3,:3] = [[c,0,s],[0,1,0],[-s,0,c]]
        else: matrix[:3,:3] = [[c,-s,0],[s,c,0],[0,0,1]]
        return matrix
    expected = translation([25,100,0]) @ translation([1,2,3]) @ translation([10,0,0]) @ rotation("y",20) @ rotation("z",30) @ rotation("x",-10) @ translation([-10,0,0])
    t,r,s = coords.fbx_to_unity(expected[None],.01)
    path = "Armature/Hips/Spine"
    reference = {"t":t[0].tolist(),"r":r[0].tolist(),"s":s[0].tolist()}
    tracks = {key:[value]*3 for key,value in reference.items()}
    baked = {"uab_baked":1,"rest":{path:reference},"clips":[{"name":"Pivot","fps":2,"frames":3,"duration":1,"events":[],"tracks":{path:tracks}}]}
    filename = tmp_path / "pivot.json"
    filename.write_text(json.dumps(baked),encoding="utf-8")
    output = tmp_path / "out"
    assert convert(["convert","--baked",str(filename),"--skeleton",str(source),"-o",str(output)]) == 0
    manifest = json.loads((output / "manifest.json").read_text())
    dump = tmp_path / "pose.json"
    blender("blender_helpers.py","dump_pose",manifest["clips"][0]["fbx"],dump)
    for pose in json.loads(dump.read_text())["frames"]:
        np.testing.assert_allclose(np.asarray(pose["Spine"]).reshape(4,4),expected,atol=2e-4,rtol=0)
