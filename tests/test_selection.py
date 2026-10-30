"""Selection must never silently export unchecked clips."""
import json
from pathlib import Path

import pytest

from unityanimbridge import cli


def test_dropped_tracks_are_visible_to_desktop_callers(tmp_path):
    fbx = str(tmp_path / "Idle.fbx")
    result = {"clips": [{"name": "Idle", "out_fbx": fbx, "status": "ok",
                          "dropped": [{"path": "Mesh", "reason": "bone_not_found"}]}]}
    assert cli.finish([result], {}, tmp_path, []) == 0
    sidecar = json.loads((tmp_path / "Idle.uab.json").read_text())
    assert sidecar["status"] == "warning"
    assert sidecar["warnings"] == ["Omitted track 'Mesh': bone_not_found"]
    assert sidecar["dropped"][0]["path"] == "Mesh"


def test_unity_baked_samples_do_not_claim_raw_humanoid_conversion(tmp_path):
    skeleton = tmp_path / "Hero.fbx"
    skeleton.write_bytes(b"fixture")
    Path(str(skeleton) + ".meta").write_text(
        "ModelImporter:\n  animationType: 3\n  animations:\n    clipAnimations:\n"
        "    - name: MirroredReference\n      takeName: Take\n      firstFrame: 0\n"
        "      lastFrame: 30\n      mirror: 1\n", encoding="utf-8")
    from types import SimpleNamespace
    args = SimpleNamespace(meta=None, armature=None, bone_map=None,
                           root_to_bone=None, strict=False, fps=None)
    _, _, sample_warnings = cli.samples_job(args, skeleton, [], tmp_path, None)
    _, _, raw_warnings, _ = cli.split_job(args, skeleton, tmp_path)
    assert not sample_warnings
    assert any("path C" in warning for warning in raw_warnings)
    assert any("mirror" in warning for warning in raw_warnings)


@pytest.mark.parametrize("name", ["NUL", "con.txt", "COM1", "LPT¹", "Trailing.", "control\x01"])
def test_output_name_can_be_created_on_windows(tmp_path, name):
    output = tmp_path / (cli.safe_name(name, set()) + ".fbx")
    output.write_bytes(b"fixture")
    assert output.is_file() and output.read_bytes() == b"fixture"


def test_selected_baked_clips_only_reach_backend(tmp_path, monkeypatch):
    baked = tmp_path / "baked.json"
    baked.write_text(json.dumps({"uab_baked": 1, "fps": 30, "clips": [
        {"name": name, "frames": 1, "duration": 0, "tracks": {}}
        for name in ("Walk", "Run", "Idle")
    ]}))
    jobs = []
    monkeypatch.setattr(cli, "find_blender", lambda _: Path("blender"))
    monkeypatch.setattr(cli, "run_job", lambda job, *_: jobs.append(job) or {"clips": []})
    assert cli.main(["convert", "--baked", str(baked), "--skeleton", "Hero.fbx",
                     "--clip", "Run", "--clip", "Idle", "-o", str(tmp_path / "out")]) == 0
    assert [c["name"] for c in jobs[0]["clips"]] == ["Run", "Idle"]


@pytest.mark.parametrize("names,requested", [(["Run"], ["Missing"]), (["Run", "Run"], ["Run"])])
def test_invalid_selection_rejected(names, requested):
    with pytest.raises(cli.InputError):
        cli.select_clips(names, requested, lambda c: c)


def test_fbx_selection_after_take_probe(tmp_path, monkeypatch):
    jobs = []

    def backend(job, *_):
        jobs.append(job)
        return {"stacks": ["Walk", "Run"]} if job.get("probe") else {"clips": []}

    monkeypatch.setattr(cli, "run_job", backend)
    args = cli.build_parser().parse_args(["convert", "source.fbx", "--clip", "Run", "-o", str(tmp_path)])
    cli.run_split(args, tmp_path / "source.fbx", tmp_path, Path("blender"))
    assert jobs[0]["probe"]
    assert [c["name"] for c in jobs[1]["split_clips"]] == ["Run"]
