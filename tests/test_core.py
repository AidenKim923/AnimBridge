import math
import textwrap

import numpy as np
import pytest

from unityanimbridge.anim_parser import AnimParseError, parse_anim
from unityanimbridge.curves import WRAP_LOOP, WRAP_PINGPONG, Curve, Key
from unityanimbridge.sampler import euler_to_quat_unity, sample_clip
from unityanimbridge.unity_yaml import to_float

GENERIC_ANIM = textwrap.dedent(
    """\
    %YAML 1.1
    %TAG !u! tag:unity3d.com,2011:
    --- !u!74 &7400000
    AnimationClip:
      m_ObjectHideFlags: 0
      m_Name: Wave
      serializedVersion: 7
      m_Legacy: 0
      m_RotationCurves:
      - curve:
          serializedVersion: 2
          m_Curve:
          - serializedVersion: 3
            time: 0
            value: {x: 0, y: 0, z: 0, w: 1}
            inSlope: {x: 0, y: 0, z: 0, w: 0}
            outSlope: {x: 0, y: 0, z: 0, w: 0}
            tangentMode: 0
            weightedMode: 0
            inWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334, w: 0.33333334}
            outWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334, w: 0.33333334}
          - serializedVersion: 3
            time: 1
            value: {x: 0, y: 0, z: 0.70710677, w: 0.70710677}
            inSlope: {x: 0, y: 0, z: 0, w: 0}
            outSlope: {x: 0, y: 0, z: 0, w: 0}
            tangentMode: 0
            weightedMode: 0
            inWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334, w: 0.33333334}
            outWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334, w: 0.33333334}
          m_PreInfinity: 2
          m_PostInfinity: 2
          m_RotationOrder: 4
        path: Root/Arm
      m_CompressedRotationCurves: []
      m_EulerCurves: []
      m_PositionCurves:
      - curve:
          serializedVersion: 2
          m_Curve:
          - serializedVersion: 3
            time: 0
            value: {x: 0, y: 1, z: 0}
            inSlope: {x: 0, y: 0, z: 0}
            outSlope: {x: 1, y: 0, z: 0}
            tangentMode: 0
            weightedMode: 0
            inWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334}
            outWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334}
          - serializedVersion: 3
            time: 1
            value: {x: 1, y: 1, z: 0}
            inSlope: {x: 1, y: 0, z: 0}
            outSlope: {x: 0, y: 0, z: 0}
            tangentMode: 0
            weightedMode: 0
            inWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334}
            outWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334}
          m_PreInfinity: 2
          m_PostInfinity: 2
          m_RotationOrder: 4
        path: Root
      m_ScaleCurves: []
      m_FloatCurves:
      - curve:
          serializedVersion: 2
          m_Curve:
          - serializedVersion: 3
            time: 0
            value: 2
            inSlope: 0
            outSlope: 0
            tangentMode: 0
            weightedMode: 0
            inWeight: 0
            outWeight: 0
          m_PreInfinity: 2
          m_PostInfinity: 2
          m_RotationOrder: 4
        attribute: Speed
        path:
        classID: 95
        script: {fileID: 0}
        flags: 16
      m_PPtrCurves: []
      m_SampleRate: 30
      m_WrapMode: 0
      m_AnimationClipSettings:
        serializedVersion: 2
        m_StartTime: 0
        m_StopTime: 1
        m_LoopTime: 1
      m_EditorCurves:
      - curve:
          serializedVersion: 2
          m_Curve:
          - serializedVersion: 3
            time: 0
            value: 0
            inSlope: 0
            outSlope: 0
            tangentMode: 0
            weightedMode: 0
            inWeight: 0
            outWeight: 0
          m_PreInfinity: 2
          m_PostInfinity: 2
          m_RotationOrder: 4
        attribute: m_LocalPosition.x
        path: Root
        classID: 4
        script: {fileID: 0}
        flags: 0
      m_Events:
      - time: 0.5
        functionName: FootStep
        data: left
        objectReferenceParameter: {fileID: 0}
        floatParameter: 1.5
        intParameter: 3
        messageOptions: 0
    """
)


@pytest.fixture
def generic_anim(tmp_path):
    p = tmp_path / "Wave.anim"
    p.write_text(GENERIC_ANIM, encoding="utf-8")
    return p


def test_to_float_handles_unity_infinity():
    assert to_float("Infinity") == math.inf
    assert to_float("-Infinity") == -math.inf
    assert math.isnan(to_float("NaN"))
    assert to_float(3) == 3.0


def test_parse_generic(generic_anim):
    clip = parse_anim(generic_anim)
    assert clip.name == "Wave"
    assert clip.sample_rate == 30
    assert clip.loop
    assert not clip.is_humanoid
    assert set(clip.channels) == {"Root/Arm", "Root"}
    # editor copy of m_LocalPosition.x must not override the vector curve
    assert clip.channels["Root"]["pos"][0].keys[-1].value == 1
    assert [t.attribute for t in clip.float_tracks] == ["Speed"]
    assert clip.events[0].name == "FootStep" and clip.events[0].int == 3


def test_humanoid_detected(tmp_path):
    text = GENERIC_ANIM.replace("attribute: Speed", "attribute: Left Arm Down-Up")
    p = tmp_path / "h.anim"
    p.write_text(text, encoding="utf-8")
    assert parse_anim(p).is_humanoid


def test_binary_rejected(tmp_path):
    p = tmp_path / "b.anim"
    p.write_bytes(b"\x00\x01binary")
    with pytest.raises(AnimParseError):
        parse_anim(p)


def test_sample_generic(generic_anim):
    s = sample_clip(parse_anim(generic_anim))
    assert s.frames == 31 and s.fps == 30
    q_end = s.tracks["Root/Arm"].r[-1]
    np.testing.assert_allclose(q_end, [0, 0, math.sqrt(0.5), math.sqrt(0.5)], atol=1e-6)
    np.testing.assert_allclose(np.linalg.norm(s.tracks["Root/Arm"].r, axis=1), 1, atol=1e-9)
    # slope 1 at both ends with values 0->1 over 1 s is linear
    np.testing.assert_allclose(s.tracks["Root"].t[:, 0], np.linspace(0, 1, 31), atol=1e-9)


def test_hermite_matches_reference():
    c = Curve([Key(0, 0, 0, 0), Key(2, 4, 0, 0)])
    # ease in/out: value at middle = 2, at 1/4 -> h01(0.25)=0.15625 * 4
    np.testing.assert_allclose(c.evaluate([1.0, 0.5]), [2.0, 0.625], atol=1e-12)


def test_weighted_with_default_weights_equals_hermite():
    a = Curve([Key(0, 0, 0, 3), Key(1, 1, -2, 0)])
    b = Curve([Key(0, 0, 0, 3, weighted_mode=3, out_weight=1 / 3), Key(1, 1, -2, 0, weighted_mode=3, in_weight=1 / 3)])
    t = np.linspace(0, 1, 11)
    np.testing.assert_allclose(a.evaluate(t), b.evaluate(t), atol=1e-7)


def test_weighted_nondefault_changes_shape():
    b = Curve([Key(0, 0, 0, 0, weighted_mode=2, out_weight=0.9), Key(1, 1, 0, 0)])
    assert 0 < b.evaluate([0.5])[0] < 1


def test_stepped():
    c = Curve([Key(0, 5, 0, math.inf), Key(1, 7, 0, 0)])
    np.testing.assert_allclose(c.evaluate([0, 0.999, 1.0]), [5, 5, 7])


def test_wrap_modes():
    loop = Curve([Key(0, 0, 1, 1), Key(1, 1, 1, 1)], WRAP_LOOP, WRAP_LOOP)
    np.testing.assert_allclose(loop.evaluate([1.25]), [0.25], atol=1e-12)
    pp = Curve([Key(0, 0, 1, 1), Key(1, 1, 1, 1)], WRAP_PINGPONG, WRAP_PINGPONG)
    np.testing.assert_allclose(pp.evaluate([1.25]), [0.75], atol=1e-12)
    clamp = Curve([Key(0, 0, 1, 1), Key(1, 1, 1, 1)])
    np.testing.assert_allclose(clamp.evaluate([-1, 3]), [0, 1])


def test_duplicate_and_unsorted_keys_warn():
    c = Curve([Key(1, 1), Key(0, 0), Key(1, 2)])
    assert [k.value for k in c.keys] == [0, 2]
    assert c.warnings


def test_euler_order_matches_unity():
    # Unity's default Euler order is ZXY: R = Ry * Rx * Rz.
    deg = np.array([[30.0, 45.0, 60.0]])
    q = euler_to_quat_unity(deg)[0]

    def rot(axis, a):
        a = math.radians(a)
        c, s = math.cos(a), math.sin(a)
        if axis == "x":
            return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
        if axis == "y":
            return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
        return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])

    expected = rot("y", 45) @ rot("x", 30) @ rot("z", 60)
    x, y, z, w = q
    m = np.array(
        [
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
        ]
    )
    np.testing.assert_allclose(m, expected, atol=1e-9)
