import numpy as np
import pytest

from unityanimbridge import coords
from unityanimbridge.sampler import euler_to_quat_unity, quat_mul


def rand_quats(n, seed=0):
    q = np.random.default_rng(seed).normal(size=(n, 4))
    return q / np.linalg.norm(q, axis=1, keepdims=True)


def test_unity_fbx_roundtrip():
    rng = np.random.default_rng(1)
    t = rng.normal(size=(20, 3))
    q = rand_quats(20)
    s = rng.uniform(0.5, 2, size=(20, 3))
    m = coords.unity_to_fbx(t, q, s, 0.01)
    t2, q2, s2 = coords.fbx_to_unity(m, 0.01)
    np.testing.assert_allclose(t2, t, atol=1e-9)
    np.testing.assert_allclose(s2, s, atol=1e-9)
    np.testing.assert_allclose(np.abs(np.sum(q2 * q, axis=1)), 1, atol=1e-9)


def test_mirror_and_units():
    m = coords.unity_to_fbx(np.array([[1.0, 2.0, 3.0]]), np.array([[0, 0, 0, 1.0]]), np.ones((1, 3)), 0.01)
    np.testing.assert_allclose(m[0, :3, 3], [-100, 200, 300])
    # rotation about Unity +Y by 90 deg becomes rotation about FBX +Y by -90 deg
    q = np.array([[0, np.sin(np.pi / 4), 0, np.cos(np.pi / 4)]])
    r = coords.unity_to_fbx(np.zeros((1, 3)), q, np.ones((1, 3)), 1.0)[0, :3, :3]
    np.testing.assert_allclose(r @ [1, 0, 0], [0, 0, 1], atol=1e-12)


def test_axis_conversion():
    # Y-up file (Maya/Blender default export): identity
    np.testing.assert_allclose(coords.axis_conversion(1, 1, 2, 1, 0, 1), np.eye(4))
    # Z-up right-handed (3ds Max style: up +Z, front -Y, right +X): maps file +Z to +Y
    c = coords.axis_conversion(2, 1, 1, -1, 0, 1)
    np.testing.assert_allclose(c[:3, :3] @ [0, 0, 1], [0, 1, 0], atol=1e-12)
    assert np.linalg.det(c[:3, :3]) == pytest.approx(1)
    with pytest.raises(coords.CoordError):
        coords.axis_conversion(1, 1, 2, 1, 0, -1)  # left-handed


@pytest.mark.parametrize("order,names", [(0, "XYZ"), (1, "XZY"), (2, "YZX"), (3, "YXZ"), (4, "ZXY"), (5, "ZYX")])
def test_euler_orders(order, names):
    deg = np.array([[20.0, -35.0, 50.0]])
    half = np.radians(deg[0]) / 2
    axis = {"X": [np.sin(half[0]), 0, 0, np.cos(half[0])],
            "Y": [0, np.sin(half[1]), 0, np.cos(half[1])],
            "Z": [0, 0, np.sin(half[2]), np.cos(half[2])]}
    first, second, third = (np.array([axis[a]]) for a in names)
    expected = quat_mul(quat_mul(third, second), first)
    np.testing.assert_allclose(euler_to_quat_unity(deg, order), expected, atol=1e-12)


def test_rest_check_detects_basis_change():
    # A basis mismatch must remain visible in the rest-pose deviation.
    rest_fbx = coords.unity_to_fbx(np.array([[1.0, 0, 0]]), np.array([[0, 0, 0, 1.0]]), np.ones((1, 3)), 1.0)[0]
    rz = np.eye(4)
    rz[:3, :3] = [[0, -1, 0], [1, 0, 0], [0, 0, 1]]
    a = coords.rest_correction(rz @ rest_fbx, np.array([1.0, 0, 0]), np.array([0, 0, 0, 1.0]), np.ones(3), 1.0)
    ang, trans, scl = coords.correction_deviation(a, 1.0)
    assert ang > 1 or trans > 1e-3
