# OWNER: builder-A (importers)
import json
import os

import pytest

from fixtures import dxt1_block, make_img, make_ipb, make_nft, make_quad_nif
from tools.importers.convert import TextureStore, convert_model
from tools.importers.gltf import validate_gltf
from tools.importers.img import ImgArchive
from tools.importers.world import build_world, is_invisible_marker


def _fake_install(root, extra_files=None):
    files = {
        "TestBox.nif": make_quad_nif(alpha_flags=0x200 | 0xED, stencil=True),
        "StripBox.nif": make_quad_nif(model="StripBox", strips=True, alpha_flags=0x1),
        "Empty.nif": _empty_nif(),
        "NOGO_test.nif": make_quad_nif(model="NOGO_test"),
        "G_test.nft": make_nft(big_endian=True),
        "town.ipb": make_ipb([
            (10, "TestBox", (100.0, 200.0, 3.0), (1.0, 1.0, 1.0), (0.0, 0.0, 0.7071067811865476, 0.7071067811865476)),
            (11, "StripBox", (0.0, 0.0, 0.0), (2.0, 2.0, 2.0), (0, 0, 0, 1)),
            (12, "NOGO_test", (0.0, 0.0, 0.0), (1.0, 1.0, 1.0), (0, 0, 0, 1)),
            (13, "Empty", (0.0, 0.0, 0.0), (1.0, 1.0, 1.0), (0, 0, 0, 1)),
            (99, "Missing", (0.0, 0.0, 0.0), (1.0, 1.0, 1.0), (0, 0, 0, 1)),
        ]),
    }
    files.update(extra_files or {})
    make_img(os.path.join(root, "Stream"), "World", files)
    os.makedirs(os.path.join(root, "Objects"), exist_ok=True)
    with open(os.path.join(root, "Objects", "test.ide"), "w") as f:
        f.write("objs\n10, TestBox, G_test, 1, 50, 0\n11, StripBox, G_test, 1, 50, 0\n"
                "12, NOGO_test, G_test, 1, 50, 0\n13, Empty, G_test, 1, 50, 0\nend\n")
    return root


def _empty_nif():
    from fixtures import NifWriter
    w = NifWriter()
    w.add("NiNode", w.node(w.s("Scene Root")))
    return w.build()


def test_convert_model_entity_space_and_materials(tmp_path):
    root = _fake_install(str(tmp_path / "game"))
    out = str(tmp_path / "out")
    with ImgArchive(os.path.join(root, "Stream", "World.img")) as img:
        store = TextureStore(img, out)
        r = convert_model("TestBox", img.read("TestBox.nif"), "G_test", store, out)
        assert r.gltf == os.path.join("models", "TestBox.gltf")
        assert (r.shapes, r.vertices, r.triangles, r.missing_textures) == (1, 4, 2, [])
        assert r.winding_agree == r.winding_checked == 2
        # model node translation (5,6,7) ignored; child node (1,0,0) applied
        assert r.bounds_game == [[1.0, 0.0, 0.0], [2.0, 1.0, 0.0]]
        path = os.path.join(out, r.gltf)
        assert validate_gltf(path) == []
        doc = json.load(open(path))
        acc = doc["accessors"][doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"]]
        # game (x, y, z) -> glTF (x, z, -y)
        assert acc["min"] == [1.0, 0.0, -1.0] and acc["max"] == [2.0, 0.0, 0.0]
        mat = doc["materials"][0]
        assert mat["alphaMode"] == "MASK" and abs(mat["alphaCutoff"] - 128 / 255) < 1e-9
        assert mat["doubleSided"] is True
        assert mat["pbrMetallicRoughness"]["baseColorFactor"] == [1.0, 1.0, 1.0, 1.0]
        assert doc["images"][0]["uri"] == "../textures/G_test/Wall_d.png"
        assert doc["samplers"][0]["wrapS"] == 10497
        assert doc["nodes"][0]["extras"]["model_node_transform_game"]["translation"] == [5.0, 6.0, 7.0]

        r2 = convert_model("TestBox", img.read("TestBox.nif"), "G_test", store, str(tmp_path / "o2"),
                           entity_space=False)
        assert r2.bounds_game == [[6.0, 6.0, 7.0], [7.0, 7.0, 7.0]]


def test_missing_texture_and_bad_txd_reported(tmp_path):
    root = _fake_install(str(tmp_path / "game"))
    out = str(tmp_path / "out")
    with ImgArchive(os.path.join(root, "Stream", "World.img")) as img:
        store = TextureStore(img, out)
        r = convert_model("TestBox", img.read("TestBox.nif"), "NoSuchTxd", store, out)
        # falls back to the global index and records it
        assert r.missing_textures == [] and store.fallbacks == {("NoSuchTxd", "wall_d"): "G_test"}


def test_world_end_to_end(tmp_path):
    root = _fake_install(str(tmp_path / "game"))
    out = str(tmp_path / "out")
    rep = build_world(root, out, ["town", "nothere"], log=lambda *_: None)
    assert rep["placements"] == 5 and rep["models_wanted"] == 3 and rep["models_converted"] == 2
    assert rep["failures_by_category"] == {"ipb_missing": 1, "ide_missing": 1, "nif_missing": 1,
                                           "nif_empty_in_source": 1}
    m = json.load(open(os.path.join(out, "scenes", "academy.json")))
    assert m["format"] == "bully-remake.scene-manifest/1"
    pl = {p["model"]: p for p in m["placements"]}
    tb = pl["TestBox"]
    assert tb["gltf"] == "models/TestBox.gltf" and tb["visible"]
    assert tb["ue"]["location_cm"] == [10000.0, -20000.0, 300.0]
    # stored quat is +90 about Z (textbook); the game uses its conjugate (-90 about Z),
    # which in UE axes (Y mirrored) becomes +90 about Z again.
    q = tb["ue"]["rotation_quat_xyzw"]
    assert [round(c, 6) for c in q] == [0.0, 0.0, 0.707107, 0.707107]
    assert [round(c, 6) for c in tb["game"]["rotation_quat_xyzw"]] == [0.0, 0.0, -0.707107, 0.707107]
    assert pl["NOGO_test"]["visible"] is False and pl["NOGO_test"]["gltf"] is None
    assert pl["Empty"]["gltf"] is None
    assert os.path.exists(os.path.join(out, "ipb", "town.json"))
    assert os.path.exists(os.path.join(out, "textures", "G_test", "Wall_d.png"))
    assert json.load(open(os.path.join(out, "report.json")))["models_converted"] == 2


def test_invisible_marker_names():
    for n in ["NOGO_tschool01", "nog_x", "WALKABLE_tschool2", "FGRD_ND", "FGRD_ND_classOP"]:
        assert is_invisible_marker(n)
    for n in ["SC1b_bldg_main", "FGRD_SC1b01", "Window_ND2"]:
        assert not is_invisible_marker(n)


def test_text_mode_corrupted_txd_is_reported(tmp_path):
    bad = make_nft().replace(b"9\n", b"9\r\n", 1)
    root = _fake_install(str(tmp_path / "game"), {"G_bad.nft": bad})
    with ImgArchive(os.path.join(root, "Stream", "World.img")) as img:
        store = TextureStore(img, str(tmp_path / "out"))
        assert store.dictionary("G_bad") is None
        assert "OPEN-8" in store.dict_errors["g_bad"]


def test_cli_requires_install(monkeypatch, tmp_path):
    from tools.importers import cli
    monkeypatch.delenv("BULLY_INSTALL", raising=False)
    monkeypatch.setattr(cli, "LOCAL_CONFIG", str(tmp_path / "none.json"))
    with pytest.raises(SystemExit):
        cli.resolve_install(None)
    root = _fake_install(str(tmp_path / "game"))
    monkeypatch.setenv("BULLY_INSTALL", root)
    assert cli.resolve_install(None) == root
    assert cli.main(["--out", str(tmp_path / "o"), "world", "--ipb", "town"]) == 0
    assert cli.main(["--out", str(tmp_path / "o"), "validate"]) == 0


def test_nonfinite_uvs_are_zeroed_and_counted(tmp_path):
    from fixtures import NifWriter
    w = NifWriter()
    w.add("NiNode", b"")
    w.add("NiNode", b"")
    w.add("NiTriShape", b"")
    nan = float("nan")
    w.add("NiTriShapeData", w.tri_shape_data([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)],
                                             uvs=[[(0, 0), (nan, 1), (0, float("inf"))]]))
    w.blocks[0] = ("NiNode", w.node(w.s("Scene Root"), [1]))
    w.blocks[1] = ("NiNode", w.node(w.s("M"), [2]))
    w.blocks[2] = ("NiTriShape", w.geometry(w.s("M:0"), 3))
    root = _fake_install(str(tmp_path / "game"), {"M.nif": w.build()})
    out = str(tmp_path / "out")
    with ImgArchive(os.path.join(root, "Stream", "World.img")) as img:
        r = convert_model("M", img.read("M.nif"), None, TextureStore(img, out), out)
    assert r.sanitized == {"TEXCOORD_0": 2}
    assert validate_gltf(os.path.join(out, r.gltf)) == []


def test_alpha_flag_decoding_and_additive_material(tmp_path):
    from tools.importers.convert import decode_alpha_flags
    assert decode_alpha_flags(0x10EC)["blend"] is False and decode_alpha_flags(0x10EC)["test"] is False
    d = decode_alpha_flags(0x32ED)
    assert (d["blend"], d["src"], d["dst"], d["test"], d["test_func"], d["no_sort"]) == \
        (True, "SRCALPHA", "INVSRCALPHA", True, "GREATER", True)
    assert (decode_alpha_flags(0x1001)["src"], decode_alpha_flags(0x1001)["dst"]) == ("ONE", "ONE")
    root = _fake_install(str(tmp_path / "game"), {"Glow.nif": make_quad_nif(model="Glow", alpha_flags=0x1001)})
    out = str(tmp_path / "out")
    with ImgArchive(os.path.join(root, "Stream", "World.img")) as img:
        r = convert_model("Glow", img.read("Glow.nif"), "G_test", TextureStore(img, out), out)
    mat = json.load(open(os.path.join(out, r.gltf)))["materials"][0]
    assert mat["alphaMode"] == "BLEND" and mat["extras"]["blend_mode"] == "additive"


def test_preview_renders_png(tmp_path):
    pytest.importorskip("numpy")
    from tools.importers.preview import render
    from tools.importers.textures import read_png_header
    root = _fake_install(str(tmp_path / "game"))
    out = str(tmp_path / "out")
    build_world(root, out, ["town"], log=lambda *_: None)
    png = render(out, ["town"], size=200, log=lambda *_: None)
    w, h, _ = read_png_header(png)
    assert w > 0 and h > 0
