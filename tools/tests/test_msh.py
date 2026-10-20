# OWNER: builder-A (importers)
"""AE .msh reader tests on synthetic files built here. The real-archive test skips when absent."""

import os
import struct
import zipfile

import numpy as np
import pytest

from tools.importers.binreader import FormatError
from tools.importers.msh import parse_msh

PROPS = b"{physical(physicalmaterial)=,setups(orderedarray<collisionshapesetup>)=[0]}"
FLAG_BYTES = {6: 1, 7: 2, 8: 3, 9: 4, 10: 4, 11: 4, 12: 5}
SIZES = {1: 8, 2: 12, 8: 4, 9: 4, 10: 4, 11: 4}


def s(text):
    b = text.encode() if isinstance(text, str) else text
    return struct.pack("<I", len(b)) + b


def part(name, verts, indices, decl, subs, version, pivot=(0.0, 0.0, 0.0)):
    """verts: list of per-vertex bytes, already packed in decl order."""
    out = s(name) + b"\0\0"
    if version >= 10:
        out += struct.pack("<3f", *pivot)
    if version >= 11:
        out += b"\0" * 4
    out += struct.pack("<II", len(verts), len(indices)) + struct.pack(f"<{len(indices)}H", *indices)
    out += struct.pack("<I", len(decl)) + b"".join(struct.pack("<IIBB", t, sem, u, 1) for t, sem, u in decl)
    out += b"".join(verts)
    out += struct.pack("<I", len(subs)) + b"".join(struct.pack("<4I", *sub) for sub in subs)
    return out


def make_msh(parts, materials=("mat_a",), version=7, bones=(), bounds=None, gap=b"\x03\0\0\0[0]" + b"\0" * 8):
    body = struct.pack("<I", len(materials)) + b"".join(s(m) for m in materials)
    body += struct.pack("<I", len(bones))
    for name, m12, parent in bones:
        body += s(name) + struct.pack("<12fi", *m12, parent)
    body += b"\0" * FLAG_BYTES[version]
    body += struct.pack("<10f", *(bounds or (0,) * 10)) + s(PROPS) + gap
    mesh_off = 24 + len(body)
    return struct.pack("<6I", version, 2, 1337, 24, 1, mesh_off) + body + struct.pack("<I", len(parts)) + b"".join(parts)


STATIC_DECL = [(2, 0, 0), (10, 3, 0), (10, 5, 0), (8, 6, 0), (11, 4, 0)]


def static_vertex(p, n=(0, 0, 127), uv=(0, 0)):
    return (struct.pack("<3f", *p) + struct.pack("<4b", *n, 0) + struct.pack("<4b", -127, 0, 0, 0)
            + bytes((0, 0, 0, 100)) + struct.pack("<2h", *uv))


def quad(version=7, pivot=(0.0, 0.0, 0.0)):
    v = [static_vertex(p, uv=uv) for p, uv in [((0, 0, 0), (0, 0)), ((1, 0, 0), (2048, 0)),
                                                ((1, 1, 0), (2048, 2048)), ((0, 1, 0), (-1024, 4096))]]
    return part("quad:0", v, [0, 1, 2, 0, 2, 3], STATIC_DECL, [(0, 3, 0, 0), (3, 3, 1, 0)], version, pivot)


@pytest.mark.parametrize("version", range(6, 13))
def test_static_quad_every_version(version):
    m = parse_msh(make_msh([quad(version)], ("mat_a", "mat_b"), version, bounds=(0, 0, 0, 1, 1, 0, .5, .5, 0, .7)))
    assert m.version == version and m.materials == ["mat_a", "mat_b"] and not m.skinned
    assert m.bounds["max"] == [1, 1, 0] and m.properties.startswith("{physical")
    assert len(m.flags) == {6: 1, 7: 2, 8: 3, 12: 5}.get(version, 4)
    assert [x.material for x in m.submeshes] == [0, 1]
    a, b = m.submeshes
    assert a.material_name == "mat_a" and a.part == "quad:0" and a.part_index == 0
    # submesh 1 uses vertices 0, 2, 3 compacted to 0, 1, 2
    np.testing.assert_array_equal(b.positions, [[0, 0, 0], [1, 1, 0], [0, 1, 0]])
    np.testing.assert_array_equal(b.indices, [0, 1, 2])
    np.testing.assert_allclose(a.uv0, [[0, 0], [1, 0], [1, 1]])
    np.testing.assert_allclose(b.uv0[2], [-0.5, 2.0])  # short / 2048
    np.testing.assert_allclose(a.normals, [[0, 0, 1]] * 3)
    np.testing.assert_allclose(a.tangents, [[-1, 0, 0]] * 3)
    assert a.extra6.tolist() == [[0, 0, 0, 100]] * 3
    assert a.uv1 is None and a.joints is None and m.warnings == []
    assert m.parts[0]["decl"] == [list(d) for d in STATIC_DECL] and m.parts[0]["vertices"] == 4


def test_winding_is_ccw_in_game_space():
    m = parse_msh(make_msh([quad()], ("a", "b")))
    sm = m.submeshes[0]
    p = sm.positions[sm.indices]
    assert np.dot(np.cross(p[1] - p[0], p[2] - p[0]), sm.normals[0]) > 0


def test_pivot_float_uvs_three_sets_binormal():
    decl = [(2, 0, 0), (10, 3, 0), (10, 7, 0), (11, 4, 0), (11, 4, 1), (1, 4, 2)]
    verts = [struct.pack("<3f", *p) + struct.pack("<4b", 0, 0, 127, 0) + struct.pack("<4b", 0, 127, 0, 0)
             + struct.pack("<4h", 2048, 0, 0, 1024) + struct.pack("<2f", 7.5, -3.25)
             for p in [(0, 0, 0), (1, 0, 0), (0, 1, 0)]]
    m = parse_msh(make_msh([part("p", verts, [0, 1, 2], decl, [(0, 3, 0, 0)], 12, (0.5, 0, 0))], version=12))
    sm = m.submeshes[0]
    assert m.parts[0]["pivot"] == [0.5, 0.0, 0.0]
    np.testing.assert_array_equal(sm.positions[1], [1, 0, 0])  # pivot is reported, not applied
    np.testing.assert_allclose(sm.uv0[0], [1, 0]) and np.testing.assert_allclose(sm.uv1[0], [0, 0.5])
    np.testing.assert_allclose(sm.uv2, [[7.5, -3.25]] * 3)
    np.testing.assert_allclose(sm.binormals, [[0, 1, 0]] * 3)
    assert sm.tangents is None


def test_skinned_weights_joints_bones():
    decl = [(2, 0, 0), (9, 1, 0), (8, 2, 0), (10, 3, 0), (11, 4, 0)]
    verts = [struct.pack("<3f", *p) + bytes(w) + bytes(j) + struct.pack("<4b", 0, 0, 127, 0) + b"\0" * 4
             for p, w, j in [((0, 0, 1), (255, 0, 0, 0), (1, 0, 0, 0)), ((1, 0, 1), (128, 127, 0, 0), (0, 1, 0, 0)),
                             ((0, 1, 1), (85, 85, 85, 0), (0, 1, 1, 0))]]
    eye = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -1)
    bones = [("root pelvis", eye, -1), ("root spine", eye, 0)]
    m = parse_msh(make_msh([part("body:0", verts, [0, 1, 2], decl, [(0, 3, 0, 0)], 11)], version=11, bones=bones))
    assert m.skinned and [b.name for b in m.bones] == ["root pelvis", "root spine"]
    assert m.bones[1].parent == 0 and m.bones[0].matrix[2] == [0, 0, 1, -1]
    sm = m.submeshes[0]
    assert sm.joints.tolist() == [[1, 0, 0, 0], [0, 1, 0, 0], [0, 1, 1, 0]]
    np.testing.assert_allclose(sm.weights.sum(1), 1, atol=1e-6)
    np.testing.assert_allclose(sm.weights[1, :2], [128 / 255, 127 / 255], atol=1e-6)


def test_empty_model_and_gap_skipped():
    m = parse_msh(make_msh([], (), 6, gap=b"\x03\0\0\0[0]" + b"\xab" * 40))
    assert m.parts == [] and m.submeshes == [] and m.materials == []


def test_nonfinite_values_zeroed_with_warning():
    decl = [(2, 0, 0), (1, 4, 0)]
    verts = [struct.pack("<5f", 0, 0, 0, float("nan"), 1e34), struct.pack("<5f", 1, 0, 0, 1, 1),
             struct.pack("<5f", 0, 1, 0, 0, 1)]
    m = parse_msh(make_msh([part("p", verts, [0, 1, 2], decl, [(0, 3, 0, 0)], 7)]))
    assert m.submeshes[0].uv0[0].tolist() == [0, 0] and "2 non-finite" in m.warnings[0]


def _bad(**kw):
    v = [static_vertex((0, 0, 0))] * 3
    p = dict(name="p", verts=v, indices=[0, 1, 2], decl=STATIC_DECL, subs=[(0, 3, 0, 0)], version=7)
    p.update(kw)
    return make_msh([part(**p)])


@pytest.mark.parametrize("data,msg", [
    (b"\0" * 10, "too short"),
    (struct.pack("<6I", 7, 2, 1336, 24, 1, 28) + b"\0" * 8, "not a WarEngine"),
    (struct.pack("<6I", 13, 2, 1337, 24, 1, 28) + b"\0" * 8, "unsupported .msh version"),
    (_bad(indices=[0, 1, 3]), "index 3 >= 3"),
    (_bad(decl=[(2, 0, 0), (5, 3, 0)]), "unknown vertex element type 5"),
    (_bad(subs=[(0, 6, 0, 0)]), "not a triangle range"),
    (_bad(subs=[(0, 3, 4, 0)]), "material 4"),
    (_bad(decl=[(10, 3, 0)], verts=[b"\0" * 4] * 3), "no float3 position"),
    (_bad()[:-20], "runs past end"),
])
def test_rejects_bad_data(data, msg):
    with pytest.raises(FormatError, match=msg):
        parse_msh(data)


AE_ZIP = os.environ.get("BULLY_AE_ZIP", "/Applications/bully.app/Wrapper/bully.app/data_1.zip")


@pytest.mark.skipif(not os.path.isfile(AE_ZIP), reason="Anniversary Edition data_1.zip not present")
def test_real_archive_samples():
    with zipfile.ZipFile(AE_ZIP) as z:
        g = parse_msh(z.read("bully/dl_grass02.msh"))
        assert g.version == 12 and g.materials == ["mat_grass_x_dl_grass03"] and len(g.submeshes) == 1
        assert len(g.submeshes[0].indices) == 3
        t = parse_msh(z.read("bully/fgrd_sc1b01.msh"))
        assert len(t.materials) == 8 and not t.skinned
        p = np.concatenate([x.positions for x in t.submeshes])
        np.testing.assert_allclose(p.min(0), t.bounds["min"], atol=1e-3)
        np.testing.assert_allclose(p.max(0), t.bounds["max"], atol=1e-3)
        j = parse_msh(z.read("bully/player.msh"))
        assert j.skinned and len(j.bones) == 32 and j.bones[9].name == "root pelvis"
