# OWNER: builder-A (importers)
"""Integration smoke tests against the owner's installed game.

Skipped unless BULLY_INSTALL points at a Bully: Scholarship Edition install.
Read-only: nothing is written into the install; output goes to tmp_path.
Expected values are the counts measured on the Steam PC release.
"""

import json
import os

import pytest

INSTALL = os.environ.get("BULLY_INSTALL")
pytestmark = pytest.mark.skipif(not INSTALL or not os.path.isdir(os.path.join(INSTALL or "", "Stream")),
                                reason="BULLY_INSTALL not set")


@pytest.fixture(scope="module")
def world_img():
    from tools.importers.img import ImgArchive
    with ImgArchive(os.path.join(INSTALL, "Stream", "World.img")) as a:
        yield a


def test_world_img_directory(world_img):
    assert len(world_img) == 11980
    census = world_img.census()
    assert census["nif"] == 5724 and census["nft"] == 4469 and census["ipb"] == 85
    assert world_img.check_overlaps() == []


def test_school_placements(world_img):
    from tools.importers.ipb import parse_ipb
    t = parse_ipb(world_img.read("tschool.ipb"))
    i = parse_ipb(world_img.read("ischool.ipb"))
    # the survey's 210 / 234 count the first inst section only; both files
    # carry a second inst section (collectibles), 215 / 236 in total
    assert len(t.placements) == 215 and len(i.placements) == 236
    assert t.placements[0].name == "FGRD_SC1b01" and t.placements[0].model_id == 0x1410


def test_every_nif_decodes_cleanly(world_img):
    from tools.importers.nif import parse_nif
    failures = []
    big = 0
    for name in world_img.names("nif"):
        n = parse_nif(world_img.read(name))
        big += n.big_endian
        if n.failed_blocks() or n.trailing_nonzero:
            failures.append(name)
    assert failures == [] and big == 247


def test_four_text_mode_nfts_are_the_only_bad_dictionaries(world_img):
    from tools.importers.binreader import FormatError
    from tools.importers.nft import TextureDictionary
    bad = []
    for name in world_img.names("nft"):
        try:
            TextureDictionary(name, world_img.read(name))
        except FormatError:
            bad.append(name)
    assert sorted(bad) == ["BBonusB.nft", "Barr01_Switch.nft", "BeerKeg.nft", "BirdBath.nft"]


def test_convert_main_building(world_img, tmp_path):
    from tools.importers.convert import TextureStore, convert_model
    from tools.importers.gltf import validate_gltf
    from tools.importers.ide import ModelDefs
    defs = ModelDefs.load_install(INSTALL)
    store = TextureStore(world_img, str(tmp_path))
    for model in ("SC1b_bldg_main", "LOBBY01"):
        e = world_img.find(model + ".nif")
        r = convert_model(e.stem, world_img.read_entry(e), defs.get(model).txd, store, str(tmp_path))
        assert r.gltf and r.missing_textures == [] and r.triangles > 500
        assert r.winding_agree / r.winding_checked > 0.99
        assert validate_gltf(os.path.join(str(tmp_path), r.gltf)) == []
        doc = json.load(open(os.path.join(str(tmp_path), r.gltf)))
        assert doc["images"]


# -- animation (builder-A2) ---------------------------------------------------

def test_every_hxd_parses_exactly():
    from tools.importers.hxd import iter_hxds_dat, parse_hxd
    anim = os.path.join(INSTALL, "Anim")
    files = [f for f in os.listdir(anim) if f.upper().endswith(".HXD")]
    assert len(files) == 20
    for f in files:
        b = open(os.path.join(anim, f), "rb").read()
        assert parse_hxd(b).consumed == len(b), f
    blob = open(os.path.join(anim, "hxds.dat"), "rb").read()
    recs = list(iter_hxds_dat(blob))
    assert len(recs) == 130 and all(parse_hxd(blob, o).consumed == s for o, s in recs)
    h = parse_hxd(open(os.path.join(anim, "MAINPED.HXD"), "rb").read())
    assert (len(h.masks), len(h.bones), len(h.anims), len(h.groups)) == (46, 36, 3365, 425)


def test_every_agr_decodes_and_matches_mainped(world_img):
    from tools.importers.agr import parse_agr
    from tools.importers.hxd import parse_hxd
    h = parse_hxd(open(os.path.join(INSTALL, "Anim", "MAINPED.HXD"), "rb").read())
    blobs = [(f, open(os.path.join(INSTALL, "Anim", f), "rb").read())
             for f in os.listdir(os.path.join(INSTALL, "Anim")) if f.lower().endswith(".agr")]
    blobs += [(n, world_img.read(n)) for n in world_img.names("agr")]  # Grap.agr is in both
    clips = linked = 0
    for name, data in blobs:
        recs = h.group_anims(os.path.splitext(name)[0])
        c = parse_agr(data, [r.agr_size - 4 for r in recs] if recs else None)
        clips += len(c)
        if recs:
            assert len(c) == len(recs), name
            linked += len(c)
    assert clips == 4374 and linked == 3424  # 404 MAINPED groups with an .agr (21 NIS_* groups have none)


def test_player_skinned_export(world_img, tmp_path):
    from tools.importers.gltf import validate_gltf
    from tools.importers.ped import clip_requests, export_ped
    reqs = clip_requests(INSTALL, world_img, "C_Player", ["IDLE", "PLAYERRUN", "RUNFAST"])
    r = export_ped("PLAYER", world_img.read("PLAYER.nif"), None, None, str(tmp_path), reqs)
    assert validate_gltf(str(tmp_path / r.gltf), check_images=False) == []
    assert (r.joints, r.skins, r.shapes, r.vertices, r.unweighted_vertices) == (36, 1, 4, 2587, 0)
    assert r.bind_vs_stored_max_m < 0.003  # bully-re reports 2.3 mm for the same check
    speeds = {c["name"]: c["root_motion"]["speed_m_s"] for c in r.clips}
    assert speeds["IDLE"] == 0 and abs(speeds["PLAYERRUN"] - 3.61) < 0.01 and abs(speeds["RUNFAST"] - 8.01) < 0.01
