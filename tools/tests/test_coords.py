# OWNER: builder-A (importers)
import math
import random

from tools.importers import coords
from tools.importers.ipb import placement_rotation, quat_to_matrix


def ue_import_of_gltf(v):
    """What UE's glTF importer does (ConvertVec3 + m -> cm)."""
    return (v[0] * 100, v[2] * 100, v[1] * 100)


def test_game_gltf_ue_chain_agrees():
    for v in [(1, 0, 0), (0, 1, 0), (0, 0, 1), (3.5, -2.25, 7.0)]:
        via_gltf = ue_import_of_gltf(coords.game_to_gltf(v))
        direct = coords.game_to_ue_position(v)
        assert all(abs(a - b) < 1e-9 for a, b in zip(via_gltf, direct))


def test_gltf_basis_is_proper_rotation():
    cols = [coords.game_to_gltf(e) for e in [(1, 0, 0), (0, 1, 0), (0, 0, 1)]]
    m = [[cols[j][i] for j in range(3)] for i in range(3)]
    assert abs(coords.det3(m) - 1.0) < 1e-12  # winding preserved


def test_ue_rotation_matches_mirrored_points_and_quat_roundtrip():
    rnd = random.Random(1)
    for _ in range(50):
        q = [rnd.uniform(-1, 1) for _ in range(4)]
        r_game = placement_rotation(q)
        r_ue = coords.game_to_ue_rotation(r_game)
        assert abs(coords.det3(r_ue) - 1.0) < 1e-9
        v = (rnd.uniform(-5, 5), rnd.uniform(-5, 5), rnd.uniform(-5, 5))
        # rotating in game space then converting == converting then rotating in UE space
        a = coords.game_to_ue_position(coords.mat_vec(r_game, v))
        b = coords.mat_vec(r_ue, coords.game_to_ue_position(v))
        assert all(abs(x - y) < 1e-6 for x, y in zip(a, b))
        back = quat_to_matrix(coords.matrix_to_quat(r_ue))
        assert all(abs(back[i][j] - r_ue[i][j]) < 1e-6 for i in range(3) for j in range(3))


def test_xform_compose_matches_bully_re_rule():
    rz = [[0, -1, 0], [1, 0, 0], [0, 0, 1]]  # +90 about Z, column vectors
    parent = coords.Xform(rz, 2.0, (10, 0, 0))
    child = coords.Xform(None, 1.0, (1, 0, 0))
    w = parent.compose(child)
    # child origin: R*(s*t_child) + t_parent = (10, 2, 0)
    assert tuple(round(c, 9) for c in w.t) == (10, 2, 0) and w.s == 2.0
    assert coords.Xform().is_identity() and not parent.is_identity()
    assert math.isclose(coords.det3(w.r), 1.0)
