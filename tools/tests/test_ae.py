# OWNER: builder-A (importers)
"""AE archive access + material->texture resolution, on a synthetic data_1.zip."""
import os
import struct
import zipfile

import pytest

from tools.importers import ae, wx
from tools.importers.binreader import FormatError

TEXTURES = [
    "zrc_asphalt_swap_d", "zrc_asphalt_swap_n", "zrc_asphalt_swap_s",
    "af00_plant02_d", "af00_plant02_s",
    "sc_mainwall_swap_d_wi", "sc_mainwall_swap_n",
    "zrc_mosspath_swap_d_in_grass02",
    "zwillow_barkd_swap", "zwillow_barkn_swap", "zwillow_barks_swap",
    "conifer_tree01d_swap", "conifer_tree01n_swap",
    "pr00_portapoo1_d", "pr00_portapoo1_d_cs",
    "zbg_rooftop_d", "abacus", "abacus_n", "sc28_tile_01_n",
    # winter/autumn pair of the heuristic fallback
    "cn_strata_swap_d", "zcn_strata_swap_d_in_grass02", "zcn_strata_swap_s",
    # stems named by the synthetic .mtl files below
    "k_wall_d", "k_wall_n", "k_wall_s", "k_leaf_d_sp", "k_leaf_n", "k_pane_d", "k_mask", "k_water_d", "k_water_n",
]


def mtl(effect, tex=(), flags=(), vectors=()):
    t = "{\n\teffect(effect)=%s,\n\tphysical(physicalmaterial)=,\n" % effect
    t += "".join("\t%s(bool)=true,\n" % f for f in flags)
    if tex:
        t += "\ttextures(orderedarray<texture2d>)=[%d,%s]" % (len(tex), ",".join(tex))
    if vectors:
        t += ",\n\tvectors(orderedarray<vector4>)=[%d,%s]" % (len(vectors), ",".join(
            "{\n\t\tx(float)=%.4f,\n\t\ty(float)=%.4f,\n\t\tz(float)=%.4f,\n\t\tw(float)=%.4f\n\t}" % v for v in vectors))
    return t + "\n}\n"


MTLS = {
    "mat_k_wall": mtl("bully_normalspec", ["k_wall_d", "k_wall_n", "k_wall_s"], ["doublesided"],
                      [(1, 1, 1, 1), (8, 0.5, 1, 1)]),
    # name says leaf, the .mtl says otherwise: the .mtl wins
    "mat_k_leaf": mtl("bully_normal", ["k_leaf_d_sp", "k_leaf_n"], ["alphatest", "doublesided"]),
    "mat_k_pane_glass": mtl("bully_diffuse", ["k_pane_d"], ["transparent"], [(1, 1, 1, 0.5)]),
    "mat_k_mask": mtl("bully_radarmask", ["k_pane_d", "k_mask"]),
    "mat_k_water": mtl("bully_waterripple", ["k_water_d", "k_water_n"], ["transparent"], [(1, 1, 1, 1), (0.5, 0, 1, 1)]),
    "mat_k_gone": mtl("bully_normalspec", ["k_wall_d", "k_gone_n", "k_wall_s"]),   # normal map not in the archive
    "mat_k_flat": mtl("constantcolor", [], [], [(0.2, 0.2, 0.2, 1)]),
}


def msh(materials, ver=7):
    b = struct.pack("<7I", ver, 2, 0x539, 0x18, 1, 0, len(materials))
    for m in materials:
        b += struct.pack("<I", len(m)) + m.encode()
    return b + b"\0" * 16


@pytest.fixture
def zpath(tmp_path):
    p = tmp_path / "data_1.zip"
    with zipfile.ZipFile(p, "w") as z:
        for t in TEXTURES:
            z.writestr(f"bully/{t}.tex", b"x")
        z.writestr("bully/Fgrd_Sc1B01.msh", msh(["mat_zrc_asphalt_swap_fgrd_sc1b01"]))
        z.writestr("bully/sub/ignored.msh", b"x")
        for n, txt in MTLS.items():
            z.writestr(f"bully/{n}.mtl", wx.encode(txt.encode()))
        z.writestr("bully/mat_not_wx.mtl", b"{ plain text, not Wx }")
    return str(p)


@pytest.fixture
def arc(zpath):
    with ae.AeArchive(zpath) as a:
        yield a


def test_archive_case_insensitive(arc):
    assert arc.find("FGRD_SC1B01", "msh") == "bully/Fgrd_Sc1B01.msh"
    assert arc.find("fgrd_sc1b01.msh") == "bully/Fgrd_Sc1B01.msh"
    assert arc.find("nope", "msh") is None
    assert arc.list("msh") == ["fgrd_sc1b01"]          # nested entries are not addressed
    assert len(arc.list("tex")) == len(TEXTURES)
    assert ae.msh_materials(arc.read("fgrd_sc1b01", "msh")) == ["mat_zrc_asphalt_swap_fgrd_sc1b01"]
    with pytest.raises(FormatError):
        arc.read("missing", "msh")


def test_msh_materials_rejects_garbage():
    with pytest.raises(FormatError):
        ae.msh_materials(b"\0" * 40)
    with pytest.raises(FormatError):
        ae.msh_materials(msh(["mat_a"])[:30])           # truncated name table


def test_path_resolution_order(tmp_path, monkeypatch, zpath):
    cfg = tmp_path / "local_config.json"
    monkeypatch.setattr(ae, "LOCAL_CONFIG", str(cfg))
    monkeypatch.delenv("BULLY_AE_DATA", raising=False)
    monkeypatch.setattr(ae, "_AUTODETECT", [])
    with pytest.raises(FormatError):
        ae.resolve_ae_data()
    cfg.write_text('{"ae_data": "%s"}' % zpath)
    assert ae.resolve_ae_data() == zpath
    monkeypatch.setenv("BULLY_AE_DATA", "/env/not/there.zip")
    with pytest.raises(FormatError):                    # env beats config; missing file is an error
        ae.resolve_ae_data()
    assert ae.resolve_ae_data(zpath) == zpath           # argument beats env


def test_detect_ae(tmp_path):
    assert not ae.detect_ae(str(tmp_path))
    (tmp_path / "stream").mkdir()
    (tmp_path / "stream" / "world.img").write_bytes(b"")
    assert not ae.detect_ae(str(tmp_path))
    (tmp_path / "jumptable.bin").write_bytes(b"")
    assert ae.detect_ae(str(tmp_path))


@pytest.mark.parametrize("mat,diffuse,normal,specular,rule", [
    ("mat_zrc_asphalt_swap", "zrc_asphalt_swap_d", "zrc_asphalt_swap_n", "zrc_asphalt_swap_s", "exact"),
    ("mat_zrc_asphalt_swap_fgrd_sc1b01", "zrc_asphalt_swap_d", "zrc_asphalt_swap_n",
     "zrc_asphalt_swap_s", "exact+drop-suffix"),
    ("MAT_AF00_Plant02_dpi_planterb", "af00_plant02_d", None, "af00_plant02_s", "exact+drop-suffix"),
    ("mat_sc_mainwall_swap_ri1d_walls02_a", "sc_mainwall_swap_d_wi", "sc_mainwall_swap_n", None,
     "variant-suffix+drop-suffix"),
    ("mat_zrc_mosspath_swap", "zrc_mosspath_swap_d_in_grass02", None, None, "variant-suffix"),
    ("mat_zwillow_barkd_swap", "zwillow_barkd_swap", "zwillow_barkn_swap", "zwillow_barks_swap",
     "embedded-role"),
    ("mat_zconifer_tree01d_swap", "conifer_tree01d_swap", "conifer_tree01n_swap", None,
     "embedded-role+z-stripped"),
    ("mat_pr00_portapoo1_cs", "pr00_portapoo1_d_cs", None, None, "model-suffix+drop-suffix"),
    ("mat_bg_rooftop", "zbg_rooftop_d", None, None, "exact+z-added"),
    ("mat_abacus", "abacus", "abacus_n", None, "bare"),
])
def test_resolve(arc, mat, diffuse, normal, specular, rule):
    r = ae.resolve_material_textures(mat, arc)
    assert r == {"diffuse": diffuse, "normal": normal, "specular": specular, "rule": rule}


def test_winter_variant_loses_to_plain_autumn_texture(arc):
    # only "zcn_strata_swap_d_in_grass02" (snow) exists under the z spelling; the plain "cn_strata_swap_d" wins
    r = ae.resolve_material_textures("mat_zcn_strata_swap", arc)
    assert r["diffuse"] == "cn_strata_swap_d" and r["rule"] == "exact+z-stripped"
    assert r["specular"] == "zcn_strata_swap_s"
    # with no plain alternative the variant is kept (mat_zrc_mosspath_swap in test_resolve)


def test_resolve_unresolved_keeps_partial(arc):
    r = ae.resolve_material_textures("mat_sc28_tile_01_first_floor01", arc)
    assert r == {"diffuse": None, "normal": "sc28_tile_01_n", "specular": None, "rule": "unresolved"}
    assert ae.resolve_material_textures("mat_nothing_here", arc)["diffuse"] is None


def test_real_install_resolves_academy():
    try:
        a = ae.AeArchive()
    except FormatError:
        pytest.skip("AE data_1.zip not present")
    with a:
        if "fgrd_sc1b01" not in a.list("msh"):
            pytest.skip("unexpected archive contents")
        mats = ae.msh_materials(a.read("fgrd_sc1b01", "msh"))
        got = [ae.resolve_material_textures(m, a)["diffuse"] for m in mats]
        assert sum(g is not None for g in got) >= 0.9 * len(got)


def test_corrupt_zip_entry_is_format_error(tmp_path):
    p = tmp_path / "bad.zip"
    with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("bully/a.tex", b"hello" * 100)
    raw = bytearray(p.read_bytes())
    raw[40:48] = b"\xff" * 8                      # corrupt the deflate stream
    p.write_bytes(raw)
    with ae.AeArchive(str(p)) as a, pytest.raises(FormatError):
        a.read("a", "tex")


def test_world_imports_without_numpy():
    import subprocess, sys
    code = "import sys; sys.modules['numpy']=None; import tools.importers.world"
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    r = subprocess.run([sys.executable, "-c", code], cwd=root, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


def test_parse_mtl():
    m = ae.parse_mtl(MTLS["mat_k_wall"])
    assert m["effect"] == "bully_normalspec" and m["textures"] == ["k_wall_d", "k_wall_n", "k_wall_s"]
    assert m["flags"] == {"doublesided": True, "alphatest": False, "transparent": False, "unlit": False,
                          "noshadows": False}
    assert m["vectors"] == [(1.0, 1.0, 1.0, 1.0), (8.0, 0.5, 1.0, 1.0)]
    assert ae.parse_mtl(MTLS["mat_k_flat"])["textures"] == []
    with pytest.raises(ValueError):
        ae.parse_mtl("{ nothing useful }")


@pytest.mark.parametrize("effect,n,roles", [
    ("bully_diffuse", 1, ("diffuse",)), ("bully_spec", 2, ("diffuse", "specular")),
    ("bully_normal", 2, ("diffuse", "normal")), ("bully_normalspec", 3, ("diffuse", "normal", "specular")),
    ("bully_normalspeccolor", 3, ("diffuse", "normal", "specular")), ("bully_normaluv", 2, ("diffuse", "normal")),
    ("bully_waterripple", 2, ("diffuse", "normal")), ("bully_radarmask", 2, ("diffuse", "mask")),
    ("bully_sky", 1, ("diffuse",)), ("eff_marker", 1, ("diffuse",)), ("bully_future", 3, ("diffuse", "extra", "extra"))])
def test_effect_roles(effect, n, roles):
    assert ae.effect_roles(effect, n) == roles


def test_mtl_beats_the_name_heuristic(arc):
    i = ae.material_info("mat_k_leaf", arc)
    assert (i["diffuse"], i["normal"], i["specular"], i["rule"], i["source"]) == \
        ("k_leaf_d_sp", "k_leaf_n", None, "mtl", "mtl")
    assert i["flags"]["alphatest"] and i["effect"] == "bully_normal"
    # the name would resolve mat_zrc_asphalt_swap_x by drop-suffix; an .mtl-less material still does
    assert ae.material_info("mat_zrc_asphalt_swap_x", arc)["source"] == "heuristic"
    assert ae.resolve_material_textures("mat_k_wall", arc) == {
        "diffuse": "k_wall_d", "normal": "k_wall_n", "specular": "k_wall_s", "rule": "mtl"}


def test_mtl_vectors_extra_and_missing_texture(arc):
    assert ae.material_info("mat_k_wall", arc)["vectors"][1] == (8.0, 0.5, 1.0, 1.0)
    m = ae.material_info("mat_k_mask", arc)
    assert m["diffuse"] == "k_pane_d" and m["extra"] == ["k_mask"]
    g = ae.material_info("mat_k_gone", arc)             # normal missing from the archive -> the role stays empty
    assert g["normal"] is None and g["diffuse"] == "k_wall_d" and g["missing"] == ["k_gone_n"]
    flat = ae.material_info("mat_k_flat", arc)
    assert flat["diffuse"] is None and flat["effect"] == "constantcolor"


def test_undecodable_mtl_falls_back(arc):
    assert ae.read_mtl(arc, "mat_not_wx") is None
    assert ae.material_info("mat_not_wx", arc)["source"] == "heuristic"
