# OWNER: builder-A (importers)
"""Coordinate conventions: game space, glTF space, Unreal space.

GAME   Bully world/model space as stored in NIF and IPB data: right-handed,
       +Z up, 1 unit = 1 metre (survey 4.4 bounds are in metres; the world
       is 36 x 36 sectors of 50 m from -900 to 900).
GLTF   glTF 2.0: right-handed, +Y up, metres, front face CCW.
       game (x, y, z) -> glTF (x, z, -y). This is a proper rotation
       (-90 deg about X): handedness and triangle winding are preserved.
UE     Unreal: left-handed, +Z up, centimetres.
       game (x, y, z) -> UE (100 x, -100 y, 100 z). The Y mirror is the
       handedness change. UE's glTF importer maps glTF (x, y, z) to UE
       (x, z, y) (Engine/Plugins/Interchange/.../GLTFCore/.../
       ConversionUtilities.h, ConvertVec3) and applies the metre->cm scale,
       so a glTF mesh written here and a placement converted here land in
       the same UE frame: glTF(game(v)) = (x, z, -y) -> UE (x, -y, z) * 100.

Rotations: a game-space rotation matrix R (column vectors, v' = R v)
becomes M R M in UE space, M = diag(1, -1, 1). Its quaternion is returned in
(x, y, z, w) such that the textbook formula reproduces M R M, which is what
FQuat::RotateVector computes.
"""

from __future__ import annotations

import math

UE_UNITS_PER_GAME_UNIT = 100.0


# ---------------------------------------------------------------------------
# Small 3x3 helpers (row lists)
# ---------------------------------------------------------------------------

def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def mat_vec(m, v):
    return (m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2])


def transpose(m):
    return [[m[j][i] for j in range(3)] for i in range(3)]


def det3(m):
    return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))


IDENTITY3 = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]


class Xform:
    """v' = R (s v) + t, the Gamebryo NiAVObject convention (survey 2.2)."""

    __slots__ = ("r", "s", "t")

    def __init__(self, r=None, s=1.0, t=(0.0, 0.0, 0.0)):
        self.r = r if r is not None else [row[:] for row in IDENTITY3]
        self.s = s
        self.t = tuple(t)

    @classmethod
    def from_av(cls, av: dict) -> "Xform":
        r9 = av["rotation"]
        return cls([list(r9[0:3]), list(r9[3:6]), list(r9[6:9])], av["scale"], av["translation"])

    def compose(self, child: "Xform") -> "Xform":
        """world(child) = world(parent) o local(child)."""
        st = mat_vec(self.r, tuple(self.s * c for c in child.t))
        return Xform(mat_mul(self.r, child.r), self.s * child.s,
                     (self.t[0] + st[0], self.t[1] + st[1], self.t[2] + st[2]))

    def is_identity(self, eps=1e-6) -> bool:
        return (abs(self.s - 1) < eps and all(abs(c) < eps for c in self.t)
                and all(abs(self.r[i][j] - (1.0 if i == j else 0.0)) < eps for i in range(3) for j in range(3)))

    def to_json(self) -> dict:
        return {"rotation_rows": [list(r) for r in self.r], "scale": self.s, "translation": list(self.t)}


def game_to_gltf(v):
    return (v[0], v[2], -v[1])


def game_to_ue_position(v):
    k = UE_UNITS_PER_GAME_UNIT
    return [v[0] * k, -v[1] * k, v[2] * k]


def game_to_ue_rotation(r):
    m = [[1, 0, 0], [0, -1, 0], [0, 0, 1]]
    return mat_mul(mat_mul(m, r), m)


def matrix_to_quat(m) -> list:
    """Quaternion (x, y, z, w) of an orthonormal rotation matrix (column
    vectors). Shepperd's method; w >= 0."""
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        w = 0.25 * s
        x = (m[2][1] - m[1][2]) / s
        y = (m[0][2] - m[2][0]) / s
        z = (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        w = (m[2][1] - m[1][2]) / s
        x = 0.25 * s
        y = (m[0][1] + m[1][0]) / s
        z = (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        w = (m[0][2] - m[2][0]) / s
        x = (m[0][1] + m[1][0]) / s
        y = 0.25 * s
        z = (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        w = (m[1][0] - m[0][1]) / s
        x = (m[0][2] + m[2][0]) / s
        y = (m[1][2] + m[2][1]) / s
        z = 0.25 * s
    q = [x, y, z, w]
    if w < 0:
        q = [-c for c in q]
    n = math.sqrt(sum(c * c for c in q))
    return [c / n for c in q]
