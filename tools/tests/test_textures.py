# OWNER: builder-A (importers)
import os
import struct
import zlib

import pytest

from fixtures import dxt1_block, make_nft
from tools.importers import textures as T
from tools.importers.binreader import FormatError
from tools.importers.nft import TextureDictionary, texture_key


def pix(fmt, w, h, data, channels=(4, 19, 19, 19)):
    return {"pixel_format": fmt, "mipmaps": [(w, h, 0)], "num_pixels": len(data), "num_faces": 1,
            "pixels": memoryview(data), "channels": [{"type": t} for t in channels], "palette": -1}


def test_dxt1_four_colour_mode_hand_computed():
    # c0 = pure red, c1 = pure blue, c0 > c1 -> 4-colour mode.
    blk = dxt1_block(0xF800, 0x001F, 0xE4)  # row 0 = indices 0,1,2,3; rest index 0
    w, h, rgba = T.decode_pixel_data(pix(4, 4, 4, blk))
    px = [tuple(rgba[i:i + 4]) for i in range(0, 64, 4)]
    assert px[0] == (255, 0, 0, 255)
    assert px[1] == (0, 0, 255, 255)
    assert px[2] == (170, 0, 85, 255)   # (2*c0 + c1) / 3
    assert px[3] == (85, 0, 170, 255)   # (c0 + 2*c1) / 3
    assert px[4] == (255, 0, 0, 255)


def test_dxt1_punch_through_mode():
    blk = dxt1_block(0x001F, 0xF800, 0xE4)  # c0 <= c1 -> 3 colours + transparent
    _, _, rgba = T.decode_pixel_data(pix(4, 4, 4, blk))
    assert tuple(rgba[8:12]) == (127, 0, 127, 255)
    assert tuple(rgba[12:16]) == (0, 0, 0, 0)


def test_565_bit_replication():
    _, _, rgba = T.decode_pixel_data(pix(4, 4, 4, dxt1_block(0xFFFF, 0x0000, 0)))
    assert tuple(rgba[:4]) == (255, 255, 255, 255)
    _, _, rgba = T.decode_pixel_data(pix(4, 4, 4, dxt1_block(0x8410, 0x0000, 0)))
    assert tuple(rgba[:3]) == (132, 130, 132)  # 10000 -> 10000100, 100000 -> 10000010


def test_dxt5_alpha_modes():
    colour = dxt1_block(0xFFFF, 0x0000, 0)
    bits = 0
    for i, v in enumerate([0, 1, 2, 7, 6]):
        bits |= v << (3 * i)
    eight = bytes([255, 0]) + bits.to_bytes(6, "little") + colour
    _, _, rgba = T.decode_pixel_data(pix(6, 4, 4, eight))
    assert [rgba[3 + 4 * i] for i in range(5)] == [255, 0, 218, 36, 72]
    six = bytes([0, 255]) + bits.to_bytes(6, "little") + colour
    _, _, rgba = T.decode_pixel_data(pix(6, 4, 4, six))
    assert [rgba[3 + 4 * i] for i in range(5)] == [0, 255, 51, 255, 0]


def test_dxt3_explicit_alpha():
    alpha = bytes([0x0F, 0xF0]) + b"\0" * 6
    _, _, rgba = T.decode_pixel_data(pix(5, 4, 4, alpha + dxt1_block(0, 0, 0)))
    assert [rgba[3 + 4 * i] for i in range(4)] == [255, 0, 0, 255]


@pytest.mark.skipif(T.np is None, reason="numpy not installed")
@pytest.mark.parametrize("fmt", [4, 5, 6])
def test_numpy_matches_reference_decoder(fmt):
    import random
    rnd = random.Random(fmt)
    w, h = 12, 8  # non-power-of-two, partial blocks absent; plus a 2x2 mip below
    step = 8 if fmt == 4 else 16
    data = bytes(rnd.randrange(256) for _ in range((w // 4) * (h // 4) * step))
    a = T.decode_pixel_data(pix(fmt, w, h, data), use_numpy=True)
    b = T.decode_pixel_data(pix(fmt, w, h, data), use_numpy=False)
    assert a == b
    small = bytes(rnd.randrange(256) for _ in range(step))
    assert T.decode_pixel_data(pix(fmt, 2, 2, small), use_numpy=True) == \
        T.decode_pixel_data(pix(fmt, 2, 2, small), use_numpy=False)


def test_raw_formats_follow_channel_types():
    _, _, rgba = T.decode_pixel_data(pix(0, 1, 1, bytes([10, 20, 30]), channels=(0, 1, 2, 19)))
    assert rgba == bytes([10, 20, 30, 255])
    _, _, rgba = T.decode_pixel_data(pix(1, 1, 1, bytes([30, 20, 10, 40]), channels=(2, 1, 0, 3)))
    assert rgba == bytes([10, 20, 30, 40])  # stored BGRA


def test_pal8_and_errors():
    p = pix(2, 2, 1, bytes([1, 0]), channels=(16, 19, 19, 19))
    pal = {"has_alpha": 0, "entries": bytes([1, 2, 3, 0, 4, 5, 6, 0])}
    assert T.decode_pixel_data(p, pal)[2] == bytes([4, 5, 6, 255, 1, 2, 3, 255])
    with pytest.raises(FormatError):
        T.decode_pixel_data(p, None)
    with pytest.raises(FormatError):
        T.decode_pixel_data(pix(4, 8, 8, b"\0" * 8))  # face too small for the mip
    with pytest.raises(FormatError):
        T.decode_pixel_data(pix(9, 4, 4, b"\0" * 64))


def _png_pixels(path):
    data = open(path, "rb").read()
    w, h, _bd, ct = struct.unpack(">IIBB", data[16:26])
    idat = b""
    pos = 8
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        if tag == b"IDAT":
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    bpp = 4 if ct == 6 else 3
    rows = [raw[y * (w * bpp + 1) + 1:(y + 1) * (w * bpp + 1)] for y in range(h)]
    return w, h, ct, b"".join(rows)


def test_png_writer_roundtrip(tmp_path):
    rgba = bytes(range(16)) * 2  # 2x4 RGBA
    p = str(tmp_path / "t.png")
    T.write_png(p, 2, 4, rgba)
    assert _png_pixels(p) == (2, 4, 6, rgba)
    T.write_png(p, 2, 4, rgba, keep_alpha=False)
    w, h, ct, px = _png_pixels(p)
    assert ct == 2 and px == b"".join(rgba[i:i + 3] for i in range(0, len(rgba), 4))
    assert T.read_png_header(p) == (2, 4, 2)


@pytest.mark.parametrize("big", [False, True])
def test_texture_dictionary_lookup_and_export(tmp_path, big):
    pal = bytes([9, 8, 7, 0]) * 256
    data = make_nft(big_endian=big, textures=[
        ("Z:\\Bully\\Temp\\Export\\Textures\\Wall_d.tga", 4, 4, 4, dxt1_block(0xF800, 0x001F, 0)),
        ("Z:\\x\\Raw_D.TGA", 1, 1, 1, bytes([3, 2, 1, 128])),
        ("Z:\\x\\pal.tga", 2, 2, 1, bytes([0, 0]), pal),
    ])
    td = TextureDictionary("test", data)
    assert sorted(td.textures) == ["pal", "raw_d", "wall_d"]
    assert td.textures["raw_d"].name == "Raw_D"
    info = td.export(td.textures["wall_d"], str(tmp_path / "w.png"))
    assert info == {"width": 4, "height": 4, "format": "DXT1", "mips": 1, "faces": 1,
                    "has_alpha": False, "cube": False}
    info = td.export(td.textures["raw_d"], str(tmp_path / "r.png"))
    assert info["has_alpha"] is True
    assert _png_pixels(str(tmp_path / "r.png"))[3] == bytes([1, 2, 3, 128])  # BGRA stored -> RGBA
    td.export(td.textures["pal"], str(tmp_path / "p.png"))
    assert _png_pixels(str(tmp_path / "p.png"))[3] == bytes([9, 8, 7]) * 2
    assert os.path.getsize(str(tmp_path / "w.png")) > 0


def test_texture_key_normalisation():
    assert texture_key("Z:\\Bully\\Temp\\GB_Wall1_swap_d.tga") == "gb_wall1_swap_d"
    assert texture_key("GB_wall1_swap_d.TGA") == "gb_wall1_swap_d"
    assert texture_key("a/b/c.tga") == "c"
