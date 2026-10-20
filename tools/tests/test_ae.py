# OWNER: builder-A (importers)
"""AE archive access + material->texture resolution, on a synthetic data_1.zip."""
import os
import struct
import zipfile

import pytest

from tools.importers import ae
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
    # winter/autumn pairs and the MATERIAL_FIXES targets (a missing stem is ignored, so each fix needs its own)
    "cn_strata_swap_d", "zcn_strata_swap_d_in_grass02", "zcn_strata_swap_s",
    "zrc_grasse_swap_d", "rc_grasse_swap_d", "zrc_grasse_swap_n_in_grass04", "zrc_grasse_swap_s",
    "sc22_brain_d", "psc_bio_brain_n", "psc_bio_brain_s",
]


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


def test_fix_fills_a_missing_diffuse(arc):
    r = ae.resolve_material_textures("mat_psc_bio_brain", arc)
    assert r == {"diffuse": "sc22_brain_d", "normal": "psc_bio_brain_n", "specular": "psc_bio_brain_s",
                 "rule": "fix:art-match"}


def test_forced_fix_replaces_a_snow_texture(arc):
    # exact match "zrc_grasse_swap_d" is the white winter lawn; the fix points at the green one
    r = ae.resolve_material_textures("mat_zrc_grasse_swap_fgrd_inground09_a", arc)
    assert r["diffuse"] == "rc_grasse_swap_d" and r["normal"] == "zrc_grasse_swap_n_in_grass04"
    assert r["specular"] == "zrc_grasse_swap_s" and r["rule"] == "fix:winter"


def test_fix_prefix_matches_on_token_boundaries():
    assert ae._fix_for("mat_sc28_tile_01_first_floor01")[0] == "mat_sc28_tile_01"
    assert ae._fix_for("mat_sc28_tile_011")[0] is None


def test_every_fix_names_its_evidence():
    for k, v in ae.MATERIAL_FIXES.items():
        assert k.startswith("mat_") and "diffuse" in v and v["kind"] in ("art-match", "name", "sibling", "winter")
        assert v["evidence"]


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
