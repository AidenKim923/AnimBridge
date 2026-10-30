"""Actual FBX preview must distinguish a shared take's object actions."""
import json
from pathlib import Path
import subprocess
import uuid

import numpy as np
import pytest

from unityanimbridge import desktop
from unityanimbridge.blender_runner import find_blender

pytestmark = pytest.mark.blender


def test_preview_shared_take_evaluates_armature_and_mesh():
    root = Path(__file__).resolve().parents[1]
    work = root / '_verify' / 'preview_actions' / uuid.uuid4().hex
    work.mkdir(parents=True)
    source = work / 'multi-object.fbx'
    command = [str(find_blender()), '--background', '--factory-startup', '--python',
               str(root / 'tests/blender_preview_helpers.py'), '--', str(source)]
    result = subprocess.run(command, capture_output=True, timeout=120)
    (work / 'fixture.log').write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0
    take = json.loads(source.with_suffix('.take.json').read_text(encoding='utf-8'))['take']
    Path(str(source) + '.meta').write_text(
        'ModelImporter:\n  animationType: 2\n  animations:\n    clipAnimations:\n'
        f'    - name: SharedMotion\n      takeName: {take}\n      firstFrame: 0\n      lastFrame: 30\n',
        encoding='utf-8')
    preview = desktop.preview({'source': str(source), 'clip': 'SharedMotion',
                               'cache': str(work / 'cache'), 'include_mesh': True})
    assert preview['status'] == 'ready', preview
    assert preview['frame_count'] == 31 and preview['duration'] == pytest.approx(1)
    assert len(preview['clips']) >= 2
    assert np.max(np.abs(np.asarray(preview['frames'][-1]) - preview['frames'][0])) > .1
    assert preview['mesh']['kind'] == 'evaluated-skinned-mesh'
    assert np.max(np.abs(np.asarray(preview['mesh']['frames'][-1]) - preview['mesh']['frames'][0])) > .1
    missing = desktop.preview({'source': str(source), 'clip': 'Missing', 'cache': str(work / 'missing')})
    assert missing['status'] == 'blocked' and missing['code'] == 'chooseClip'
