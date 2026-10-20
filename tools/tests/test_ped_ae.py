# OWNER: builder-A2 (animation)
"""AE skinned ped export (ped_ae.py) on synthetic .msh files built here; no game bytes."""

import json
import os
import struct

import pytest

from anim_fixtures import axis_quat, make_clip
from test_msh import AE_ZIP, make_msh, part, s
from tools.importers.agr import parse_agr
from tools.importers.binreader import FormatError
from tools.importers.gltf import read_accessor, validate_gltf
from tools.importers.ped import ClipRequest
from tools.importers.ped_ae import export_ped_ae, parse_skeleton

DECL = [(2, 0, 0), (9, 1, 0), (8, 2, 0), (10, 3, 0), (11, 4, 0)]


def translate(z):
    return (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, z)


# authoring (node) pose: pelvis at 0.9 m, spine 0.3 m above it; the bind pose puts the pelvis at 0.95 m
NODES = [("Dummy", -1, translate(0)), ("Root", 0, translate(0.9)), ("Root Pelvis", 1, translate(0)),
         ("Root Spine", 2, translate(0.3))]
BONES = [("root pelvis", translate(-0.95), -1), ("root spine", translate(-1.25), 0)]


def hierarchy(nodes=NODES):
    out = s("[0]") + struct.pack("<I", len(nodes)) + b"".join(s(n) + b"\0" * 16 for n, _p, _m in nodes)
    out += struct.pack("<I", len(nodes))
    for n, parent, m in nodes:
        out += s(n) + struct.pack("<4i", 0, parent, -1, -1) + struct.pack("<12f", *m)
    return out


def vertex(p, joint):
    return struct.pack("<3f", *p) + bytes((255, 0, 0, 0)) + bytes((joint, 0, 0, 0)) + struct.pack("<4b", 0, 0, 127, 0) + b"\0" * 4


def body(bones=BONES, nodes=NODES):
    # a low triangle on the pelvis, a "head" triangle on the spine above 1.3 m
    verts = [vertex(p, j) for p, j in [((0, 0, 0.9), 0), ((0.2, 0, 0.9), 0), ((0, 0.2, 1.0), 0),
                                        ((0, 0, 1.4), 1), ((0.2, 0, 1.4), 1), ((0, 0.2, 1.5), 1)]]
    return make_msh([part("body:0", verts, [0, 1, 2, 3, 4, 5], DECL, [(0, 6, 0, 0)], 11)], ("mat_body",), 11,
                    bones=bones, gap=hierarchy(nodes))


def load(tmp_path, r):
    doc = json.loads((tmp_path / r.gltf).read_text())
    return doc, [(tmp_path / "models" / b["uri"]).read_bytes() for b in doc["buffers"]]


def test_skeleton_from_node_hierarchy():
    nodes = parse_skeleton(body())
    assert [(n, p) for n, p, _m in nodes] == [("Dummy", -1), ("Root", 0), ("Root Pelvis", 1), ("Root Spine", 2)]
    assert nodes[1][2][2, 3] == pytest.approx(0.9)


def test_rest_pose_is_bind_pose_and_joints_remapped(tmp_path):
    r = export_ped_ae("Ped", body(), None, None, str(tmp_path))
    assert validate_gltf(str(tmp_path / r.gltf), check_images=False) == []
    assert r.joints == 4 and r.skins == 1 and r.vertices == 6 and r.unweighted_vertices == 0
    assert r.bind_vs_stored_max_m < 1e-6               # rest pose skins the stored vertices exactly
    assert r.bind_vs_file_pose_max == pytest.approx(0.05, abs=1e-5)   # bind pelvis 5 cm above the node pose
    doc, bins = load(tmp_path, r)
    names = [n["name"] for n in doc["nodes"]]
    pelvis = names.index("Root Pelvis")
    assert doc["nodes"][pelvis]["translation"] == pytest.approx([0, 0.05, 0], abs=1e-6)  # glTF +Y up
    j = read_accessor(doc, bins, doc["meshes"][0]["primitives"][0]["attributes"]["JOINTS_0"])
    assert j[0] == 2 and j[12] == 3                      # msh bone 0/1 -> skeleton joints Pelvis/Spine
    ibm = read_accessor(doc, bins, doc["skins"][0]["inverseBindMatrices"])
    assert ibm[2 * 16 + 13] == pytest.approx(-0.95)      # pelvis IBM: -0.95 on glTF Y


def test_clip_and_arrowless_root_motion(tmp_path):
    ident = (1.0, 0.0, 0.0, 0.0)
    frames = [(0.0, {b: ident for b in range(4)}), (1.0, {1: ident, 3: axis_quat(0, 30)})]
    clip = parse_agr(make_clip(1002, 4, frames, 1.0, positions=[(1, (0, 0, 0.9)), (4, (0, 2.0, 0.9))]))[0]
    r = export_ped_ae("Ped", body(), None, None, str(tmp_path), [ClipRequest("SCURRY", clip, 0.5)])
    assert validate_gltf(str(tmp_path / r.gltf), check_images=False) == []
    info = r.clips[0]["root_motion"]
    assert info["travel_game_m"] == [0.0, 2.0, 0.0] and info["speed_m_s"] == pytest.approx(4.0)
    doc, bins = load(tmp_path, r)
    an = doc["animations"][0]
    root = [n["name"] for n in doc["nodes"]].index("Root")
    ch = [c for c in an["channels"] if c["target"] == {"node": root, "path": "translation"}][0]
    vals = read_accessor(doc, bins, an["samplers"][ch["sampler"]]["output"])
    assert vals[:3] == pytest.approx(vals[-3:], abs=1e-3)   # travel removed: plays in place


def root_clip(label, root_z, meta=None):
    ident = (1.0, 0.0, 0.0, 0.0)
    frames = [(0.0, {b: ident for b in range(4)}), (1.0, {1: ident})]
    clip = parse_agr(make_clip(1002, 4, frames, 1.0, positions=[(1, (0, 0, root_z)), (4, (0, 1.0, root_z))]))[0]
    return ClipRequest(label, clip, 1.0, meta or {})


def root_z(tmp_path, r):
    doc, bins = load(tmp_path, r)
    an = doc["animations"][0]
    root = [n["name"] for n in doc["nodes"]].index("Root")
    ch = [c for c in an["channels"] if c["target"] == {"node": root, "path": "translation"}][0]
    return read_accessor(doc, bins, an["samplers"][ch["sampler"]]["output"])[1]   # glTF +Y of key 0


def test_root_height_scaled_to_the_body(tmp_path):
    # bind Root is 0.9 m; the QPED bank was authored on a 0.436 m body: Root z and the travel scale by 0.9/0.436
    r = export_ped_ae("Ped", body(), None, None, str(tmp_path), [root_clip("IDLE", 0.436, {"group": "QPED"})])
    scale = 0.9 / 0.436
    assert r.clips[0]["root_height_scale"] == pytest.approx(scale, abs=1e-3)
    assert root_z(tmp_path, r) == pytest.approx(0.9, abs=2e-3)
    assert r.clips[0]["root_motion"]["travel_game_m"][1] == pytest.approx(scale, abs=2e-3)
    assert r.clips[0]["frame0_min_z_m"] == pytest.approx(0.9, abs=2e-3) and not r.warnings
    # a bank without a reference body is left alone
    r = export_ped_ae("Ped", body(), None, None, str(tmp_path), [root_clip("SCURRY", 0.5, {"group": "RAT_PED"})])
    assert "root_height_scale" not in r.clips[0] and root_z(tmp_path, r) == pytest.approx(0.5, abs=2e-3)


def test_idle_below_ground_warns(tmp_path):
    r = export_ped_ae("Ped", body(), None, None, str(tmp_path), [root_clip("IDLE", -0.2)])   # unscaled: mesh min z -0.2
    assert r.clips[0]["frame0_min_z_m"] == pytest.approx(-0.2, abs=5e-3)
    assert any("IDLE frame 0" in w and "below the ground" in w for w in r.warnings)


def test_head_model_replaces_head_triangles(tmp_path):
    hverts = [vertex(p, 0) for p in [(0, 0, 1.35), (0.2, 0, 1.35), (0, 0.2, 1.6)]]
    head = make_msh([part("head:0", hverts, [0, 1, 2], DECL, [(0, 3, 0, 0)], 7)], ("mat_hair",), 7,
                    bones=[("root spine", translate(-1.25), -1)], gap=hierarchy())
    # the base body submesh is only half above the cut, so it is kept whole; a body made only of head triangles is cut
    r = export_ped_ae("Ped", body(), None, None, str(tmp_path), head=("Hair", head))
    assert r.shapes == 2 and r.vertices == 9 and r.bind_vs_stored_max_m < 1e-6
    verts = [vertex(p, 1) for p in [(0, 0, 1.4), (0.2, 0, 1.4), (0, 0.2, 1.5), (0, 0, 1.2), (0.2, 0, 1.4), (0, 0.2, 1.4)]]
    only_head = make_msh([part("b:0", verts, [0, 1, 2, 3, 4, 5], DECL, [(0, 6, 0, 0)], 11)], ("mat_body",), 11,
                         bones=BONES, gap=hierarchy())
    r = export_ped_ae("Ped2", only_head, None, None, str(tmp_path), head=("Hair", head))
    assert r.triangles == 2 and any("head cut out (3 of 6" in w for w in r.warnings)


def test_skin_bone_missing_from_skeleton_is_an_error(tmp_path):
    with pytest.raises(FormatError, match="not in the skeleton"):
        export_ped_ae("Ped", body(nodes=NODES[:3]), None, None, str(tmp_path))


@pytest.mark.skipif(not os.path.isfile(AE_ZIP), reason="Anniversary Edition data_1.zip not present")
def test_real_ped_skeletons():
    from tools.importers.ae import AeArchive
    with AeArchive(AE_ZIP) as arc:
        player = parse_skeleton(arc.read("player", "msh"))
        assert len(player) == 36 and player[0][0] == "Dummy" and player[9][0] == "Root01" and player[35][0] == "ARROW"
        assert [n for n, _p, _m in parse_skeleton(arc.read("dog_doberman", "msh"))] == [n for n, _p, _m in player]
        assert len(parse_skeleton(arc.read("rat_ped", "msh"))) == 12
