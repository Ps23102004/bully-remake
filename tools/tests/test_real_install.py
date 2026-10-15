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
