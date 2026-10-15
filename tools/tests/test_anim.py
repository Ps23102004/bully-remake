# OWNER: builder-A2 (animation)
"""HXD, AGR, NIF skin blocks and the skinned glTF exporter, on synthetic data."""

import json
import math
import os
import struct

import pytest

from anim_fixtures import axis_quat, make_clip, make_hxd, make_skinned_nif, pack_key
from tools.importers.agr import (arrow_displacement, decode_key, parse_agr, quat_to_rows, rows_to_quat,
                                 sample_position, sample_rotation)
from tools.importers.binreader import FormatError
from tools.importers.gltf import read_accessor, validate_gltf
from tools.importers.hxd import iter_hxds_dat, parse_hxd
from tools.importers.nif import parse_nif
from tools.importers.ped import ClipRequest, export_ped


def close(a, b, eps=2e-3):
    return all(abs(x - y) <= eps for x, y in zip(a, b))


def qclose(a, b, eps=2e-3):
    return abs(abs(sum(x * y for x, y in zip(a, b))) - 1) < eps


# -- AGR keys ------------------------------------------------------------------

@pytest.mark.parametrize("kt", [999, 1000, 1001, 1002, 1003, 1004])
def test_every_key_type_round_trips(kt):
    q = axis_quat(2, -70.0)
    p = (0.12, -0.34, 0.85)
    b = pack_key(kt, 5, 0.5, q, p)
    prev, t, q2, tr = decode_key(kt, b, 0)
    assert prev == 5 and abs(t - 0.5) < 2e-3 and qclose(q, q2)
    if kt in (999, 1003, 1004):
        assert close(tr, p, 0.006)
    else:
        assert tr is None


def test_1002_bit_layout_known_word():
    # w0: prev 3, time 511 (=1.0), x sign set |x| 1023; w1: |w| 0 -> q = (0, -1, 0, 0)
    w0 = 3 | 511 << 11 | 1 << 20 | 1023 << 21
    prev, t, q, _ = decode_key(1002, struct.pack("<II", w0, 0), 0)
    assert prev == 3 and t == 1.0 and q == (0.0, -1.0, 0.0, 0.0)


# -- AGR clips -----------------------------------------------------------------

def _walk_clip(kt=1002, dur=0.8):
    ident = (1.0, 0.0, 0.0, 0.0)
    frames = [(0.0, {b: ident for b in range(3)}),
              (0.5, {1: axis_quat(0, 40)}),
              (1.0, {1: ident, 2: axis_quat(1, -30)})]
    # keys: 0,1,2 heads; 3 = bone1@0.5; 4 = bone1@1.0; 5 = bone2@1.0
    return make_clip(kt, 3, frames, dur, positions=[(1, (0, 0, 0.85)), (3, (0, 0.5, 0.85)), (4, (0, 1.0, 0.85)),
                                                    (2, (0, 0, 0)), (5, (0, 1.0, 0))])


def test_clip_tracks_follow_previous_key_links():
    c = parse_agr(_walk_clip())[0]
    assert c.bone_count == 3 and c.key_count == 6 and c.key_type == 1002
    assert [len(t) for t in c.tracks] == [1, 3, 2]
    assert qclose(sample_rotation(c.tracks[1], 0.5), axis_quat(0, 40))
    assert qclose(sample_rotation(c.tracks[1], 0.25), axis_quat(0, 20))
    assert close(sample_position(c.positions[1], 0.75), (0, 0.75, 0.85))
    assert close(arrow_displacement(c, 2), (0, 1.0, 0))


def test_group_split_and_hxd_sizes():
    a, b = _walk_clip(), _walk_clip(1001, 2.0)
    data = a + b + b"\0" * 64  # sector padding
    clips = parse_agr(data)
    assert [c.key_type for c in clips] == [1002, 1001]
    assert clips[0].size == len(a)
    exact = parse_agr(data, [len(a), len(b)])
    assert [len(p) for p in exact[1].positions] == [len(p) for p in clips[1].positions]


def test_null_keys_are_skipped_and_bad_links_rejected():
    ident = (1.0, 0.0, 0.0, 0.0)
    raw = make_clip(1002, 2, [(0.0, {0: ident, 1: ident}), (1.0, {1: (0.0, 0.0, 0.0, 0.0)})])
    c = parse_agr(raw)[0]
    assert c.null_keys == 1 and len(c.tracks[1]) == 1
    bad = bytearray(raw)
    struct.pack_into("<I", bad, 20 + 2 * 8, struct.unpack_from("<I", bad, 20 + 2 * 8)[0] | 9)  # prev 9 > own index
    with pytest.raises(FormatError):
        parse_agr(bytes(bad))


def test_quaternion_matrix_convention_round_trip():
    q = rows_to_quat(quat_to_rows(axis_quat(2, 120)))
    assert qclose(q, axis_quat(2, 120))
    m = quat_to_rows(axis_quat(2, 90))  # +90 about z: x axis -> y axis (column vectors)
    assert close([m[0][0], m[1][0], m[2][0]], [0, 1, 0])


# -- HXD -----------------------------------------------------------------------

def test_hxd_parse_and_group_order():
    anims = [("C_PLAYER\\IDLE", 1.8, 0x111, 100, 0), ("F_X\\WALK", 1.0, 0x222, 50, 1, [(1, 2, 0), (3, 4, 1)]),
             ("C_PLAYER\\RUN", 0.8, 0x333, 60, 0)]
    raw = make_hxd(["dummy", "Root"], anims, [("C_Player", 168), ("F_X", 54)])
    h = parse_hxd(raw)
    assert h.consumed == len(raw) and h.final_word == 0xABCD and h.version == pytest.approx(1.12)
    assert h.bones == ["dummy", "Root"] and h.masks[0].blend == pytest.approx(0.3)
    assert [a.short_name for a in h.group_anims("c_player")] == ["IDLE", "RUN"]
    assert h.anims[1].events == [(1, 2, 0), (3, 4, 1)]
    assert h.by_hash(0x333)[0].duration == pytest.approx(0.8)


@pytest.mark.parametrize("version", [1.09, 1.11])
def test_hxd_older_versions(version):
    raw = make_hxd(["a"], [("G\\A", 1.0, 1, 10, 0)], [("G", 14)], version=version)
    assert parse_hxd(raw).consumed == len(raw)


def test_hxds_dat_records():
    one = make_hxd(["a"], [("G\\A", 1.0, 1, 10, 0)], [("G", 14)])
    blob = struct.pack("<I", 3) + b"".join(b"ANIM" + struct.pack("<I", len(one)) + one for _ in range(2))
    recs = list(iter_hxds_dat(blob))
    assert len(recs) == 2 and all(parse_hxd(blob, o).consumed == s for o, s in recs)


# -- NIF skin blocks -------------------------------------------------------------

@pytest.mark.parametrize("big", [False, True])
def test_skin_blocks_decode_exactly(big):
    raw, info = make_skinned_nif(big_endian=big)
    n = parse_nif(raw)
    assert n.failed_blocks() == []
    si = n.of_type("NiSkinInstance")[0].data
    sd = n.of_type("NiSkinData")[0].data
    sp = n.of_type("NiSkinPartition")[0].data
    assert si["bones"] == info["bones"] and sd["has_weights"] == 1
    assert sd["bones"][1]["weights"][0] == (2, 0.75)
    assert sp["partitions"][0]["triangles"] == (0, 1, 2, 1, 3, 2)


# -- skinned export ------------------------------------------------------------

def test_export_skinned_with_clip(tmp_path):
    raw, _ = make_skinned_nif()
    clip = parse_agr(_walk_clip())[0]
    r = export_ped("Ped", raw, None, None, str(tmp_path), [ClipRequest("WALK", clip, 0.8)], root_motion="raw")
    path = tmp_path / r.gltf
    assert validate_gltf(str(path), check_images=False) == []
    assert r.joints == 4 and r.skins == 1 and r.unweighted_vertices == 0
    assert r.bind_vs_stored_max_m < 1e-6 and r.bind_vs_file_pose_max < 1e-6
    doc = json.loads(path.read_text())
    bins = [(tmp_path / "models" / b["uri"]).read_bytes() for b in doc["buffers"]]
    prim = doc["meshes"][0]["primitives"][0]
    w = read_accessor(doc, bins, prim["attributes"]["WEIGHTS_0"])
    assert close(w[8:12], (0.25 / 1.0, 0.75 / 1.0, 0, 0)) or close(w[8:12], (0.75, 0.25, 0, 0))
    an = doc["animations"][0]
    assert an["extras"]["duration"] == pytest.approx(0.8)
    paths = sorted((c["target"]["node"], c["target"]["path"]) for c in an["channels"])
    assert ("rotation" in [p for _, p in paths]) and ("translation" in [p for _, p in paths])
    # IBM of Bone1 (joint 2, 1 m up) is a translation of -1 on glTF Y
    ibm = read_accessor(doc, bins, doc["skins"][0]["inverseBindMatrices"])
    assert close(ibm[32 + 12:32 + 15], (0, -1, 0))


def test_export_inplace_root_motion(tmp_path):
    raw, _ = make_skinned_nif(n_bones=35)  # Dummy + 35 = 36 joints, ARROW-like track 35
    ident = (1.0, 0.0, 0.0, 0.0)
    frames = [(0.0, {b: ident for b in range(36)}), (1.0, {1: ident, 35: ident})]
    data = make_clip(1002, 36, frames, 1.0, positions=[(1, (0, 0, 0.85)), (36, (0, 2.0, 0.85)),
                                                       (35, (0, 0, 0)), (37, (0, 2.0, 0))])
    clip = parse_agr(data)[0]
    r = export_ped("Ped", raw, None, None, str(tmp_path), [ClipRequest("RUN", clip, 0.5)])
    assert validate_gltf(str(tmp_path / r.gltf), check_images=False) == []
    info = r.clips[0]["root_motion"]
    assert info["travel_game_m"] == [0.0, 2.0, 0.0] and info["speed_m_s"] == pytest.approx(4.0)
    doc = json.loads((tmp_path / r.gltf).read_text())
    bins = [(tmp_path / "models" / b["uri"]).read_bytes() for b in doc["buffers"]]
    root_node = 2  # model node 0, Dummy 1, Root 2
    an = doc["animations"][0]
    ch = [c for c in an["channels"] if c["target"] == {"node": root_node, "path": "translation"}][0]
    vals = read_accessor(doc, bins, an["samplers"][ch["sampler"]]["output"])
    assert close(vals[:3], vals[-3:])  # pelvis travel removed: stays in place
