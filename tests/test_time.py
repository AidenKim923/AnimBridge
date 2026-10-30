"""Output cadence, fractional lengths, and sub-frame clips."""
import numpy as np
import pytest

from unityanimbridge.anim_parser import AnimClip, AnimEvent
from unityanimbridge.curves import Curve, Key
from unityanimbridge.sampler import sample_clip


@pytest.mark.parametrize("fps,duration,frames,last_sample", [
    (29.97, 1.0, 31, 1.0),
    (30.0, 1.017, 32, 1.017),
    (30.0, 1.01, 31, 1.0),
    (30.0, 0.01, 2, 0.01),
])
def test_fractional_time_and_event_mapping(fps, duration, frames, last_sample):
    start = 2.0
    clip = AnimClip("Time", "synthetic", fps, start, start + duration, False, False, False)
    clip.channels = {"Root": {"pos": [Curve([Key(start, 0, 1, 1), Key(start + duration, duration, 1, 1)]), None, None]}}
    clip.events = [AnimEvent(start + duration, "End"), AnimEvent(start - .1, "Outside")]
    result = sample_clip(clip)
    assert result.frames == frames
    assert result.duration == pytest.approx((frames - 1) / fps, abs=1e-12)
    assert result.times[-1] == pytest.approx(last_sample, abs=1e-12)
    np.testing.assert_allclose(result.tracks["Root"].t[:, 0], result.times, atol=1e-12)
    assert len(result.events) == 1 and result.events[0]["name"] == "End"
    assert result.events[0]["time"] == pytest.approx(min(duration, result.duration), abs=1e-12)
    assert any("outside" in w for w in result.warnings)
    assert any("whole number" in w for w in result.warnings)
