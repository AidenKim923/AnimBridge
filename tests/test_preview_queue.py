"""Executable scheduling/race regression checks for automatic preview loading."""
from pathlib import Path
import shutil
import subprocess

import pytest


def test_preview_queue_bounds_concurrency_and_discards_stale_results():
    node = shutil.which("node")
    if not node:
        pytest.skip("Node.js is required for the preview scheduler checks")
    root = Path(__file__).resolve().parents[1]
    result = subprocess.run(
        [node, str(root / "tests/preview_queue_test.mjs"), str(root / "native/ui/preview-queue.js")],
        capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
