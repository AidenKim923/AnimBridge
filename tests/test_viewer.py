"""Run the Canvas viewer's executable cache/animation regression checks."""
from pathlib import Path
import shutil
import subprocess

import pytest


def test_pose_viewer_reuses_geometry_and_canvas_backing_store():
    node = shutil.which("node")
    if not node:
        pytest.skip("Node.js is required for the browser viewer checks")
    root = Path(__file__).resolve().parents[1]
    result = subprocess.run(
        [node, str(root / "tests/viewer_test.mjs"), str(root / "native/ui/viewer.js")],
        capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
