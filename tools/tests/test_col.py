# OWNER: builder-A (importers)
"""COL3 collision -> collision-only glTF, on synthetic bytes (no game data)."""
import json
import os
import struct

import pytest

from fixtures import make_img
from tools.importers.binreader import FormatError
from tools.importers.col import export_collision, parse_col, write_gltf
from tools.importers.gltf import read_accessor, validate_gltf


def col3(model_id, boxes=(), verts=(), faces=(), tail=b"\x07" * 12):
    """One COL3 entry in the layout col.py documents."""
    body = struct.pack("<HH20s", 4, 1, b"")                                # +0x08
    body += struct.pack("<I", model_id)                                    # +0x20
    body += struct.pack("<4f4f4f", 0, 0, 0, 1, -1, -1, -1, 0, 1, 1, 1, 0)  # +0x24 centre/radius, min, max
    body += struct.pack("<II", 0, 0)                                       # +0x54
    body += struct.pack("<I", len(boxes))                                  # +0x5c
    for mn, mx, s in boxes:
        body += struct.pack("<4f4fI", *mn, 0, *mx, 0, s)
    body += struct.pack("<I", len(verts))
    for v in verts:
        body += struct.pack("<3h", *(round(c * 128) for c in v))
    body += b"\0" * (-(8 + len(body)) % 4)
    if verts:
        body += struct.pack("<I", len(faces))
        for a, b, c, s in faces:
            body += struct.pack("<3HBB", a, b, c, s, 255)
        body += tail
    return b"COL3" + struct.pack("<I", len(body)) + body


# a floor triangle, wound clockwise seen from above as the game stores it (odd vertex count: padding)
FLOOR = col3(7, verts=[(0, 0, 0), (0, 2, 0), (2, 0, 0)], faces=[(0, 1, 2, 34)])


def test_parse_entries_boxes_padding():
    data = col3(5) + col3(6, boxes=[((-1, -2, 0), (1, 2, 3), 13)]) + FLOOR + b"\0" * 40
    e = parse_col(data)
    assert [m.model_id for m in e] == [5, 6, 7]
    assert e[0].faces == [] and e[0].boxes == []
    assert e[1].boxes == [((-1, -2, 0), (1, 2, 3), 13)]
    assert e[2].vertices == [(0, 0, 0), (0, 2, 0), (2, 0, 0)] and e[2].faces == [(0, 1, 2, 34)]


def test_parse_rejects_bad_index_and_tag():
    with pytest.raises(FormatError):
        parse_col(col3(1, verts=[(0, 0, 0)] * 3, faces=[(0, 1, 3, 0)]))
    with pytest.raises(FormatError):
        parse_col(b"COL2" + FLOOR[4:])


def test_write_gltf_reverses_winding(tmp_path):
    out = str(tmp_path)
    info = write_gltf("fgrd_nd_x", parse_col(FLOOR)[0], "floor", out)
    path = os.path.join(out, info["gltf"])
    assert info["gltf"] == "collision/fgrd_nd_x.gltf" and validate_gltf(path) == []
    doc = json.load(open(path))
    bins = [open(os.path.join(out, "collision", b["uri"]), "rb").read() for b in doc["buffers"]]
    prim = doc["meshes"][0]["primitives"][0]
    flat = read_accessor(doc, bins, prim["attributes"]["POSITION"])
    a, b, c = (flat[3 * i:3 * i + 3] for i in read_accessor(doc, bins, prim["indices"]))
    # glTF +Y is game +Z: the reversed floor faces up (counter-clockwise from above)
    cross_y = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2])
    assert cross_y > 0
    assert doc["nodes"][0]["extras"]["collision_only"] is True


def test_export_collision_markers_only(tmp_path):
    install, out = str(tmp_path / "install"), str(tmp_path / "out")
    make_img(os.path.join(install, "Stream"), "World", {"isc_x.col": col3(1, verts=[(0, 0, 0), (0, 1, 0), (1, 0, 0)],
                                                                          faces=[(0, 1, 2, 13)]) + FLOOR
                                                        + col3(9, verts=[(0, 0, 0)] * 3, faces=[(0, 1, 2, 0)])})
    os.makedirs(os.path.join(out, "scenes"))
    pl = [{"scene": "isc_x", "index": 0, "model": "nogo_xop", "model_id": 1},
          {"scene": "isc_x", "index": 1, "model": "fgrd_nd_xop", "model_id": 7},
          {"scene": "isc_x", "index": 2, "model": "walkable_xop", "model_id": 9}]
    json.dump({"scenes": [{"name": "isc_x"}], "placements": pl}, open(os.path.join(out, "scenes", "academy.json"), "w"))
    idx = export_collision(install, out, log=lambda m: None)
    assert sorted(idx["models"]) == ["fgrd_nd_xop", "nogo_xop"]      # walkable_ is ped navigation, not collision
    assert {m["kind"] for m in idx["models"].values()} == {"nogo", "floor"}
    assert [p["index"] for p in idx["placements"]] == [0, 1]
    assert json.load(open(os.path.join(out, "collision", "index.json")))["models"] == idx["models"]
