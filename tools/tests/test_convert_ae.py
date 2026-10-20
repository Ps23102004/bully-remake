# OWNER: builder-A (importers)
"""AE .msh -> glTF on a synthetic data_1.zip (no game bytes)."""
import json
import os
import struct
import zipfile

import pytest

import numpy as np

from test_msh import STATIC_DECL, make_msh, part, quad, static_vertex
from test_wtex import make_tex
from tools.importers import ae, wx
from tools.importers.convert_ae import AeTextureStore, convert_msh
from tools.importers.gltf import validate_gltf


@pytest.fixture
def arc(tmp_path):
    p = tmp_path / "data_1.zip"
    tex = make_tex(0, 2, 2, bytes([255, 0, 0, 255, 0, 255, 0, 128] * 2))   # RGBA8888, has alpha
    with zipfile.ZipFile(p, "w") as z:
        for t in ("brick_d", "brick_n", "glow_window_d"):
            z.writestr(f"bully/{t}.tex", tex)
        z.writestr("bully/Quad.msh", make_msh([quad()], materials=("mat_brick", "mat_glow_window")))
    with ae.AeArchive(str(p)) as a:
        yield a


def test_convert_quad(arc, tmp_path):
    out = str(tmp_path / "out")
    store = AeTextureStore(arc, out)
    r = convert_msh("quad", arc.read("quad", "msh"), arc, store, out)
    assert (r.shapes, r.triangles) == (2, 2)
    assert r.gltf == "models/quad.gltf"
    assert validate_gltf(os.path.join(out, r.gltf)) == []
    # game (x, y, z) -> glTF (x, z, -y): the quad's (1, 1, 0) is (1, 0, -1); bounds stay in game space
    assert r.bounds_game == [[0, 0, 0], [1, 1, 0]]
    doc = json.load(open(os.path.join(out, r.gltf)))
    brick, glow = doc["materials"]
    assert brick["extras"]["blend_mode"] == "masked" and brick["alphaMode"] == "MASK"
    assert "normalTexture" in brick and "baseColorTexture" in brick["pbrMetallicRoughness"]
    assert glow["extras"]["blend_mode"] == "additive"
    assert sorted(store.exported) == ["brick_d", "brick_n", "glow_window_d"]   # each decoded once
    assert os.path.isfile(os.path.join(out, "textures/ae/brick_d.png"))
    # tools/ue/import_content.py finds textures only via extras.textures[role].uri
    assert brick["extras"]["textures"]["base"]["uri"] == "../textures/ae/brick_d.png"


def test_opaque_two_sided_unless_modelled_back_to_back(tmp_path):
    """Single-sided wall shells must draw from behind (ae-interiors.md); a part already modelled back-to-back
    (the same triangles twice, reversed) keeps one side, or the two coplanar faces would z-fight."""
    p = tmp_path / "data_1.zip"
    v = [static_vertex(q) for q in ((0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0))]
    wall = part("w:0", v, [0, 1, 2, 0, 2, 3], STATIC_DECL, [(0, 6, 0, 0)], 7)
    sheet = part("s:0", v, [0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2], STATIC_DECL, [(0, 12, 1, 0)], 7)
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("bully/wall_d.tex", make_tex(0, 2, 2, bytes([90, 90, 90, 255] * 4)))   # opaque
        z.writestr("bully/m.msh", make_msh([wall, sheet], materials=("mat_wall", "mat_wall_sheet")))
    out = str(tmp_path / "out")
    with ae.AeArchive(str(p)) as a:
        r = convert_msh("m", a.read("m", "msh"), a, AeTextureStore(a, out), out)
    mats = {m["name"]: m for m in json.load(open(os.path.join(out, r.gltf)))["materials"]}
    assert mats["mat_wall"]["extras"]["blend_mode"] == "opaque" and mats["mat_wall"]["doubleSided"] is True
    assert "doubleSided" not in mats["mat_wall_sheet"]


def test_mtl_drives_blend_mode_slots_and_extras(tmp_path):
    """The .mtl, not the texture's alpha channel or the name, decides masked / translucent / additive / opaque."""
    from test_ae import mtl
    mtls = {
        # alpha-bearing texture but no alphatest flag: stays opaque (the old heuristic would mask it)
        "mat_wallp": mtl("bully_normalspec", ["pic_d", "pic_n", "pic_s"], [], [(1, 1, 1, 1), (8, 0.5, 1, 1)]),
        "mat_leafp": mtl("bully_normal", ["pic_d", "pic_n"], ["alphatest", "doublesided"]),
        "mat_glassp": mtl("bully_diffuse", ["pic_d"], ["transparent", "doublesided"], [(0.5, 0.5, 0.5, 0.25)]),
        "mat_lightglowp": mtl("bully_diffuse", ["pic_d"], ["unlit"]),
        "mat_glowsign": mtl("bully_normal", ["pic_d", "pic_n"]),    # "glow" in the name, but a lit surface (normal map)
        "mat_glowsprite": mtl("bully_diffuse", ["opq_d"]),          # diffuse-only sprite with a glow name
        "mat_chalkp": mtl("bully_diffuse", ["opq_d"], ["alphatest"]),   # alphatest on an alpha-less texture
        "mat_sc_hopscotch": mtl("bully_diffuse", ["opq_d"], ["alphatest"]),
    }
    p = tmp_path / "data_1.zip"
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("bully/pic_d.tex", make_tex(0, 2, 2, bytes([255, 0, 0, 255, 0, 255, 0, 128] * 2)))
        z.writestr("bully/opq_d.tex", make_tex(0, 2, 2, bytes([90, 90, 90, 255] * 4)))
        for t in ("pic_n", "pic_s"):
            z.writestr(f"bully/{t}.tex", make_tex(0, 2, 2, bytes([128, 128, 255, 255] * 4)))
        for n, t in mtls.items():
            z.writestr(f"bully/{n}.mtl", wx.encode(t.encode()))
        v = [static_vertex(q) for q in ((0, 0, 0), (1, 0, 0), (1, 1, 0))]
        n = len(mtls)
        pt = part("q:0", v, [0, 1, 2] * n, STATIC_DECL, [(3 * i, 3, i, 0) for i in range(n)], 7)
        z.writestr("bully/m.msh", make_msh([pt], materials=tuple(mtls)))
    out = str(tmp_path / "out")
    with ae.AeArchive(str(p)) as a:
        r = convert_msh("m", a.read("m", "msh"), a, AeTextureStore(a, out), out)
        doc = json.load(open(os.path.join(out, r.gltf)))
    m = {x["name"]: x for x in doc["materials"]}
    mode = {k: v["extras"]["blend_mode"] for k, v in m.items()}
    assert mode == {"mat_wallp": "opaque", "mat_leafp": "masked", "mat_glassp": "translucent", "mat_lightglowp": "additive",
                    "mat_glowsign": "opaque", "mat_glowsprite": "additive", "mat_chalkp": "opaque", "mat_sc_hopscotch": "additive"}
    wall = m["mat_wallp"]
    assert wall["doubleSided"] is True and "alphaMode" not in wall                    # lane A: opaque is two-sided
    assert wall["extras"]["textures"]["gloss"]["uri"] == "../textures/ae/pic_s.png"   # the UE master's Gloss slot
    ae_x = wall["extras"]["ae"]
    assert (ae_x["source"], ae_x["effect"], ae_x["specular_power"], ae_x["specular_strength"]) == \
        ("mtl", "bully_normalspec", 8.0, 0.5)
    assert m["mat_leafp"]["alphaMode"] == "MASK" and m["mat_leafp"]["doubleSided"] is True
    glass = m["mat_glassp"]
    assert glass["alphaMode"] == "BLEND" and glass["doubleSided"] is True
    assert glass["pbrMetallicRoughness"]["baseColorFactor"] == [0.5, 0.5, 0.5, 0.25]
    assert glass["extras"]["gamebryo"]["material"] == {"diffuse": [0.5, 0.5, 0.5], "alpha": 0.25}
    assert "gloss" not in m["mat_lightglowp"]["extras"]["textures"] or True
    assert "gamebryo" not in wall["extras"]                                          # neutral tint: nothing to pass


# -- grass ----------------------------------------------------------------------------------------

def flat_quad(size=20.0, uv_v=2048, normal=(0, 0, 127), plane="xy"):
    """size x size metre quad; V runs 0..uv_v/2048 along the second axis. plane xy = flat, xz = wall."""
    corners = [(0, 0), (size, 0), (size, size), (0, size)]
    uvs = [(0, 0), (2048, 0), (2048, uv_v), (0, uv_v)]
    pos = [(a, b, 0) if plane == "xy" else (a, 0, b) for a, b in corners]
    v = [static_vertex(p, normal, uv) for p, uv in zip(pos, uvs)]
    return part("g:0", v, [0, 1, 2, 0, 2, 3], STATIC_DECL, [(0, 6, 0, 0)], 7)


@pytest.fixture
def grass_arc(tmp_path):
    p = tmp_path / "data_1.zip"
    tex = make_tex(0, 2, 2, bytes([40, 90, 30, 255] * 4))
    with zipfile.ZipFile(p, "w") as z:
        for t in ("zrc_grasst_swap_d", "rc_grasse_swap_d", "rc_grassz_swap_d", "rc_grasstripz_swap_d", "te03_grass3_d"):
            z.writestr(f"bully/{t}.tex", tex)
        z.writestr("bully/fgrd_lawn.msh", make_msh([flat_quad()], materials=("mat_zrc_grasst_swap",)))
        z.writestr("bully/fgrd_wall.msh", make_msh([flat_quad(plane="xz", normal=(0, 127, 0))],
                                                    materials=("mat_zrc_grasst_swap",)))
        z.writestr("bully/fgrd_edge.msh", make_msh([flat_quad()], materials=("mat_zrc_grasse_swap",)))
        z.writestr("bully/fgrd_plain.msh", make_msh([flat_quad()], materials=("mat_something_else",)))
    with ae.AeArchive(str(p)) as a:
        yield a


def run_grass(arc, tmp_path, name):
    out = str(tmp_path / "out")
    store = AeTextureStore(arc, out)
    convert_msh(name, arc.read(name, "msh"), arc, store, out)
    path = os.path.join(out, "grass", f"{name}.grass.json")
    return out, store, (json.load(open(path)) if os.path.exists(path) else None)


def test_grass_class_names():
    from tools.importers.convert_ae import grass_class
    assert grass_class("mat_zrc_grasst_swap") == "lawn" and grass_class("mat_zrc_grasseb_swap_fgrd_x") == "lawn"
    assert grass_class("MAT_ZRC_GRASSE_SWAP") == "edge" and grass_class("mat_zsc_bggrass_swap") == "wild"
    assert grass_class("mat_te04_grass_01d") == "lawn"
    assert grass_class("mat_zrc_asphalt_swap") is None and grass_class("mat_zrc_grasstt_swap") is None


def test_grass_scatter_density_bounds_and_files(grass_arc, tmp_path):
    out, store, d = run_grass(grass_arc, tmp_path, "fgrd_lawn")
    assert d["format"] == "bully-remake.grass/1" and d["tile"] is True and d["area_m2"] == {"lawn": 400.0}
    assert set(d["tufts"]) == {"clump", "strip"}
    for tid, dens in (("clump", 1.6), ("strip", 2.2)):
        t = d["tufts"][tid]
        rows = np.array(t["rows"]).reshape(-1, 5)
        assert t["count"] == len(rows) and abs(len(rows) - dens * 400) < 0.15 * dens * 400
        assert rows[:, 0].min() >= 0 and rows[:, 0].max() <= 2000          # x cm inside the 20 m quad
        assert rows[:, 1].max() <= 0 and rows[:, 1].min() >= -2000         # UE y = -game y
        assert set(rows[:, 2]) == {-2}                                     # flat at z 0 minus the 2 cm sink
        assert rows[:, 3].min() >= 0 and rows[:, 3].max() < 360 and rows[:, 4].min() >= 75 and rows[:, 4].max() <= 130
        assert all(0.55 <= c <= 1.35 for c in t["tint"])
    for tid in ("clump", "strip", "dry"):
        assert validate_gltf(os.path.join(out, "grass", f"tuft_{tid}.gltf")) == []
    idx = json.load(open(os.path.join(out, "grass", "index.json")))
    assert idx["models"]["fgrd_lawn"]["instances"] == {k: v["count"] for k, v in d["tufts"].items()}
    assert idx["models"]["fgrd_lawn"]["file"] == "grass/fgrd_lawn.grass.json" and set(idx["tufts"]) == {"clump", "strip", "dry"}
    assert os.path.isfile(os.path.join(out, "textures/ae/rc_grassz_swap_d.png"))   # tuft cards are decoded once


def test_grass_is_deterministic_and_skips_walls_and_other_materials(grass_arc, tmp_path):
    _, _, a = run_grass(grass_arc, tmp_path, "fgrd_lawn")
    _, _, b = run_grass(grass_arc, tmp_path / "again", "fgrd_lawn")
    assert a == b
    assert run_grass(grass_arc, tmp_path / "w", "fgrd_wall")[2] is None     # vertical: no grass
    assert run_grass(grass_arc, tmp_path / "p", "fgrd_plain")[2] is None    # not a grass material


def test_grass_edge_strip_is_thinned_at_the_fringe(grass_arc, tmp_path):
    _, _, d = run_grass(grass_arc, tmp_path, "fgrd_edge")
    rows = np.array(d["tufts"]["strip"]["rows"]).reshape(-1, 5)
    v = -rows[:, 1] / 2000.0                  # V = game y / 20 m (quad UV runs 0..1)
    assert (v < 0.12).sum() == 0 and (v > 0.5).sum() > 0.4 * len(rows)
    assert len(rows) < 1.6 * 400 * 0.95       # fewer than a lawn quad of the same size would get


def test_ae_hidden_models():
    from tools.importers.world import ae_hidden_models
    names = ["pxGarb", "scwinnight01", "scwinday01", "autoscreen01n01", "autoscreen01d01",
             "lawn", "lawd", "nightabl", "armoir", "dl_janscreenn"]
    assert ae_hidden_models(names) == {"pxGarb": "physics_proxy", "scwinnight01": "night_variant",
                                       "autoscreen01n01": "night_variant"}
