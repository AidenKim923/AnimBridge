"""Unity local TRS <-> FBX node-local matrices. NumPy only; also used inside Blender."""
from __future__ import annotations

import numpy as np

# Mirror across X: Unity (left-handed) <-> FBX (right-handed).
S = np.diag([-1.0, 1.0, 1.0, 1.0])


class CoordError(Exception):
    pass


def quat_to_mat3(q: np.ndarray) -> np.ndarray:
    q = q / np.linalg.norm(q, axis=1, keepdims=True)
    x, y, z, w = q[:, 0], q[:, 1], q[:, 2], q[:, 3]
    m = np.empty((len(q), 3, 3))
    m[:, 0, 0] = 1 - 2 * (y * y + z * z)
    m[:, 0, 1] = 2 * (x * y - z * w)
    m[:, 0, 2] = 2 * (x * z + y * w)
    m[:, 1, 0] = 2 * (x * y + z * w)
    m[:, 1, 1] = 1 - 2 * (x * x + z * z)
    m[:, 1, 2] = 2 * (y * z - x * w)
    m[:, 2, 0] = 2 * (x * z - y * w)
    m[:, 2, 1] = 2 * (y * z + x * w)
    m[:, 2, 2] = 1 - 2 * (x * x + y * y)
    return m


def mat3_to_quat(m: np.ndarray) -> np.ndarray:
    """Rotation matrices [N,3,3] -> quaternions x,y,z,w (w >= 0 not enforced)."""
    n = len(m)
    q = np.empty((n, 4))
    tr = m[:, 0, 0] + m[:, 1, 1] + m[:, 2, 2]
    for i in range(n):
        a = m[i]
        if tr[i] > 0:
            s = np.sqrt(tr[i] + 1.0) * 2
            q[i] = [(a[2, 1] - a[1, 2]) / s, (a[0, 2] - a[2, 0]) / s, (a[1, 0] - a[0, 1]) / s, 0.25 * s]
        elif a[0, 0] > a[1, 1] and a[0, 0] > a[2, 2]:
            s = np.sqrt(1.0 + a[0, 0] - a[1, 1] - a[2, 2]) * 2
            q[i] = [0.25 * s, (a[0, 1] + a[1, 0]) / s, (a[0, 2] + a[2, 0]) / s, (a[2, 1] - a[1, 2]) / s]
        elif a[1, 1] > a[2, 2]:
            s = np.sqrt(1.0 + a[1, 1] - a[0, 0] - a[2, 2]) * 2
            q[i] = [(a[0, 1] + a[1, 0]) / s, 0.25 * s, (a[1, 2] + a[2, 1]) / s, (a[0, 2] - a[2, 0]) / s]
        else:
            s = np.sqrt(1.0 + a[2, 2] - a[0, 0] - a[1, 1]) * 2
            q[i] = [(a[0, 2] + a[2, 0]) / s, (a[1, 2] + a[2, 1]) / s, 0.25 * s, (a[1, 0] - a[0, 1]) / s]
    return q / np.linalg.norm(q, axis=1, keepdims=True)


def trs_to_mat(t: np.ndarray, q: np.ndarray, s: np.ndarray) -> np.ndarray:
    n = len(t)
    m = np.zeros((n, 4, 4))
    m[:, :3, :3] = quat_to_mat3(q) * s[:, None, :]
    m[:, :3, 3] = t
    m[:, 3, 3] = 1.0
    return m


def mat_to_trs(m: np.ndarray):
    """Decompose [N,4,4] without shear. Negative determinant puts the sign on scale x."""
    t = m[:, :3, 3].copy()
    r = m[:, :3, :3]
    s = np.linalg.norm(r, axis=1)  # column norms
    det = np.linalg.det(r)
    s[:, 0] *= np.where(det < 0, -1.0, 1.0)
    if np.any(np.abs(s) < 1e-12):
        raise CoordError("zero scale in matrix")
    rot = r / s[:, None, :]
    return t, mat3_to_quat(rot), s


def axis_conversion(up_axis: int, up_sign: int, front_axis: int, front_sign: int,
                    coord_axis: int, coord_sign: int) -> np.ndarray:
    """Rotation C (4x4) taking an FBX file axis system to Y-up, +Z front, +X right (Unity before mirroring)."""
    e = np.eye(3)
    right = coord_sign * e[coord_axis]
    up = up_sign * e[up_axis]
    front = front_sign * e[front_axis]
    f = np.stack([right, up, front], 1)  # columns: file axes for (right, up, front)
    if abs(abs(np.linalg.det(f)) - 1) > 1e-9:
        raise CoordError("degenerate FBX axis system")
    if np.linalg.det(f) < 0:
        raise CoordError("FBX axis system is left-handed; not supported (v1)")
    c = np.eye(4)
    c[:3, :3] = f.T
    return c


def unity_scaled(t, q, s, k: float) -> np.ndarray:
    """G(x): mirrored Unity local TRS with translation divided by k."""
    if k <= 0:
        raise CoordError(f"unit scale k must be > 0 (got {k})")
    return S @ trs_to_mat(t / k, q, s) @ S


def unity_to_fbx(t, q, s, k: float, a: np.ndarray | None = None) -> np.ndarray:
    """FBX node-local matrices for Unity local TRS samples. a = A(b) (C^-1 for top-level nodes)."""
    g = unity_scaled(t, q, s, k)
    return g if a is None else a @ g


def fbx_to_unity(m: np.ndarray, k: float, a: np.ndarray | None = None):
    """Inverse of unity_to_fbx: FBX node-local matrices -> Unity local TRS."""
    if a is not None:
        if abs(np.linalg.det(a)) < 1e-12:
            raise CoordError("singular correction matrix")
        m = np.linalg.inv(a) @ m
    t, q, s = mat_to_trs(S @ m @ S)
    return t * k, q, s


def rest_correction(fbx_rest_local: np.ndarray, unity_rest_t, unity_rest_q, unity_rest_s, k: float) -> np.ndarray:
    """Rest correction A(b) = F_rest . G(U_rest)^-1."""
    g = unity_scaled(unity_rest_t[None], unity_rest_q[None], unity_rest_s[None], k)[0]
    if abs(np.linalg.det(g)) < 1e-12:
        raise CoordError("singular Unity rest matrix")
    return fbx_rest_local @ np.linalg.inv(g)


def correction_deviation(a: np.ndarray, bone_length: float) -> tuple[float, float, float]:
    """(rotation degrees, translation / bone length, max relative scale error) of A from identity."""
    t, q, s = mat_to_trs(a[None])
    w = min(1.0, abs(q[0, 3]))
    ang = float(np.degrees(2 * np.arccos(w)))
    trans = float(np.linalg.norm(t[0]) / max(bone_length, 1e-9))
    scale = float(np.max(np.abs(np.abs(s[0]) - 1)))
    return ang, trans, scale
