# OWNER: builder-A (importers)
"""AE .msh -> glTF on a synthetic data_1.zip (no game bytes)."""
import json
import os
import struct
import zipfile

import pytest

from test_msh import make_msh, quad
from test_wtex import make_tex
from tools.importers import ae
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
