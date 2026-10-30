"""Split an FBX at 29.97 fps with no source keys on either trim boundary."""
import json
import subprocess
from pathlib import Path

import numpy as np
import pytest

from unityanimbridge.blender_runner import find_blender
from unityanimbridge.cli import main

pytestmark = pytest.mark.blender


def helper(*args):
    subprocess.run([str(find_blender()), "-b", "--factory-startup", "--python-exit-code", "1",
                    "--python", str(Path(__file__).with_name("blender_time_helpers.py")),
                    "--", *map(str, args)], check=True, capture_output=True, timeout=300)


def test_sparse_fractional_boundaries(tmp_path, monkeypatch):
    monkeypatch.setenv("UAB_WORK_DIR", str(tmp_path / "jobs"))
    source = tmp_path / "Sparse.fbx"
    helper("make", source)
    source.with_suffix(".fbx.meta").write_text(
        "fileFormatVersion: 2\nModelImporter:\n  animations:\n    clipAnimations:\n"
        "    - name: Cut\n      takeName: Take1\n      firstFrame: 5.25\n      lastFrame: 24.6\n"
        "      loopTime: 0\n      mirror: 0\n      events: []\n"
        "  meshes:\n    globalScale: 1\n    useFileScale: 1\n  animationType: 2\n")
    out = tmp_path / "out"
    assert main(["convert", str(source), "-o", str(out)]) == 0
    clip = json.loads((out / "manifest.json").read_text())["clips"][0]
    side = json.loads(Path(clip["sidecar"]).read_text())
    assert clip["fps"] == pytest.approx(29.97, rel=1e-6)
    assert clip["frames"] == 20
    assert clip["duration"] == pytest.approx(19 / clip["fps"])
    assert any("sparser" in warning for warning in side["warnings"])
    times = np.arange(20) / clip["fps"]
    source_times = times + 5.25 / clip["fps"]
    helper("sample", source, tmp_path / "source.json", json.dumps(source_times.tolist()))
    helper("sample", clip["fbx"], tmp_path / "result.json", json.dumps(times.tolist()))
    original = json.loads((tmp_path / "source.json").read_text())
    converted = json.loads((tmp_path / "result.json").read_text())
    for boundary in (5.25 / clip["fps"], 24.6 / clip["fps"]):
        assert min(abs(boundary - t) for t in original["keys"]) > 1e-3
    for expected, actual in zip(original["frames"], converted["frames"], strict=True):
        for bone in expected:
            np.testing.assert_allclose(actual[bone], expected[bone], atol=1e-3, rtol=1e-5)
    assert main(["convert", str(source), "--strict", "-o", str(tmp_path / "strict")]) == 1
