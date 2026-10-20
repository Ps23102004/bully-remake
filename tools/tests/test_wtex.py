"""AE .tex reader/decoder tests on synthetic files. The real-archive test skips when absent."""

import os
import struct
import zipfile
import zlib

import numpy as np
import pytest

from fixtures import dxt1_block
from tools.importers import wtex as W
from tools.importers.binreader import FormatError


def make_tex(fmt, w, h, payload, mips=1, declared=None, compress=False, extra="", mode="tm_standard"):
    dw, dh = declared or (w, h)
    text = (f'{{width={dw},height={dh},mode={mode},nomips=false,compressondisk={"true" if compress else "false"},'
            f'samplerstate={{minfilter=tfm_linear}},importfilepath="D:\\a,b\\x.png"{extra}}}').encode()
    data = struct.pack("<I", len(payload)) + zlib.compress(payload) if compress else payload
    return (struct.pack("<7I", 7, 2, 0x6E, 0x18, 9, 28 + len(text), len(text)) + text
            + struct.pack("<5I", fmt, w, h, mips, len(data)) + data)


def pixels(rgba, w):
    a = np.frombuffer(rgba, np.uint8).reshape(-1, w, 4)
    return a


# -- header ---------------------------------------------------------------------

def test_header_text_nested_quoted_and_typed():
    info = W.parse_tex(make_tex(8, 4, 2, bytes(8), declared=(16, 8), extra=",pp_params={gammamod=0.500000}"))
    m = info.meta
    assert m["width"] == 16 and m["mode"] == "tm_standard" and m["nomips"] is False
    assert m["samplerstate"] == {"minfilter": "tfm_linear"}
    assert m["importfilepath"] == "D:\\a,b\\x.png"           # comma inside quotes survives
    assert m["pp_params"]["gammamod"] == 0.5
    assert (info.width, info.height, info.declared_width, info.declared_height) == (4, 2, 16, 8)
    assert info.format_name == "L8"


def test_bad_inputs_raise_format_error():
    good = make_tex(8, 2, 2, bytes(4))
    with pytest.raises(FormatError):
        W.parse_tex(b"\0" * 12)
    with pytest.raises(FormatError):
        W.parse_tex(struct.pack("<I", 8) + good[4:])                      # bad magic
    with pytest.raises(FormatError):
        W.parse_tex(good + b"x")                                          # trailing byte
    with pytest.raises(FormatError):
        W.parse_tex(make_tex(8, 2, 2, bytes(5)))                          # size does not match any mip count
    with pytest.raises(FormatError):
        W.parse_tex(make_tex(2, 2, 2, bytes(4)))                          # unknown format id
    with pytest.raises(FormatError):
        W.decode_tex(make_tex(8, 2, 2, bytes(4)), mip=3)


# -- plain formats --------------------------------------------------------------

def test_rgba8888_and_rgb888_order():
    _, _, rgba = W.decode_tex(make_tex(0, 2, 1, bytes([1, 2, 3, 4, 5, 6, 7, 8])))
    assert rgba == bytes([1, 2, 3, 4, 5, 6, 7, 8])
    _, _, rgba = W.decode_tex(make_tex(1, 2, 1, bytes([1, 2, 3, 4, 5, 6])))
    assert rgba == bytes([1, 2, 3, 255, 4, 5, 6, 255])


def test_rgb565_and_rgba4444():
    px = struct.pack("<HH", 0xF800, 0x07E0)
    _, _, rgba = W.decode_tex(make_tex(3, 2, 1, px))
    assert rgba == bytes([255, 0, 0, 255, 0, 255, 0, 255])
    px = struct.pack("<HH", 0xF00A, 0x0F80)   # R high nibble, A low nibble
    _, _, rgba = W.decode_tex(make_tex(4, 2, 1, px))
    assert rgba == bytes([255, 0, 0, 170, 0, 255, 136, 0])


def test_l8_grey():
    _, _, rgba = W.decode_tex(make_tex(8, 2, 1, bytes([0, 200])))
    assert rgba == bytes([0, 0, 0, 255, 200, 200, 200, 255])


def test_dxt1_and_dxt5_reuse_textures_decoder():
    blk = dxt1_block(0xF800, 0x001F, 0xE4)
    _, _, rgba = W.decode_tex(make_tex(5, 4, 4, blk))
    assert tuple(rgba[:4]) == (255, 0, 0, 255) and tuple(rgba[4:8]) == (0, 0, 255, 255)
    alpha = bytes([0, 255]) + bytes(6)       # 6-value mode, all index 0 -> alpha 0
    _, _, rgba = W.decode_tex(make_tex(7, 4, 4, alpha + blk))
    assert rgba[3] == 0 and tuple(rgba[:3]) == (255, 0, 0)


def test_zlib_payload_and_mip_table():
    # 4x2 RGB565 with 3 mips: 16 + 4 + 2 bytes (4x2, 2x1, 1x1)
    payload = bytes(range(16)) + bytes(4) + bytes(2)
    tex = make_tex(3, 4, 2, payload, mips=3, compress=True)
    info = W.parse_tex(tex)
    assert info.compressed and [(w, h, o, s) for w, h, o, s in info.levels] == [(4, 2, 0, 16), (2, 1, 16, 4), (1, 1, 20, 2)]
    assert W.decode_tex(tex)[:2] == (4, 2)
    assert W.decode_tex(tex, mip=1)[:2] == (2, 1)
    bad = bytearray(tex)
    bad[-3] ^= 0xFF
    with pytest.raises(FormatError):
        W.decode_tex(bytes(bad))


def test_declared_size_resample():
    # stored 2x2 (black, white / white, black) declared 2x1 -> vertical box filter
    grey = bytes([0, 255, 255, 0])
    tex = make_tex(8, 2, 2, grey, declared=(2, 1))
    w, h, rgba = W.decode_tex(tex, declared=True)
    assert (w, h) == (2, 1) and rgba[0] == 128 and rgba[4] == 128
    assert W.decode_tex(tex)[:2] == (2, 2)
    w, h, rgba = W.decode_tex(make_tex(8, 1, 1, bytes([9]), declared=(2, 2)), declared=True)   # nearest up
    assert (w, h) == (2, 2) and set(rgba[0::4]) == {9}


def test_write_tex_png(tmp_path):
    p = tmp_path / "t.png"
    assert W.write_tex_png(make_tex(1, 2, 1, bytes(6)), str(p)) == (2, 1, False)
    assert p.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"


# -- PVRTC 4bpp, hand-computed ----------------------------------------------------

def colour_word(a15, b16, mode=0):
    """A: opaque 5-5-4 (a15=(r,g,b4)); B: opaque 5-5-5. Returns the 32-bit colour half of a block."""
    ca = 0x8000 | a15[0] << 10 | a15[1] << 5 | a15[2] << 1 | mode
    cb = 0x8000 | b16[0] << 10 | b16[1] << 5 | b16[2]
    return cb << 16 | ca


def block(mod, col):
    return struct.pack("<II", mod, col)


def twiddle(bx, by):
    """Naive reference for the block order: y bits in even positions, x bits in odd positions."""
    i = 0
    for k in range(8):
        i |= ((by >> k & 1) << (2 * k)) | ((bx >> k & 1) << (2 * k + 1))
    return i


def test_pvrtc_flat_colour_and_5bit_expansion():
    col = colour_word((16, 31, 0), (16, 31, 0))
    tex = make_tex(9, 8, 8, block(0, col) * 4)
    px = pixels(W.decode_tex(tex)[2], 8)
    assert (px == [132, 255, 0, 255]).all()           # 16 -> 132, 31 -> 255


def test_pvrtc_modulation_weights_and_orientation():
    col = colour_word((0, 0, 0), (31, 31, 15))          # A black, B white (4-bit blue 15 -> 31 -> 255)
    mod = sum((i % 4) << (2 * i) for i in range(16))    # pixel (x, y) uses weight index x
    px = pixels(W.decode_tex(make_tex(9, 8, 8, block(mod, col) * 4))[2], 8)
    # (A*(8-w) + B*w) / 8 with w = 0, 3, 5, 8
    assert [int(v) for v in px[0, :4, 0]] == [0, 95, 159, 255]
    assert (px[3, :4, 0] == px[0, :4, 0]).all() and (px[:, :, 3] == 255).all()


def test_pvrtc_block_order_and_neighbour_wrap():
    n = 4                                              # 16x16 pixels, 4x4 blocks
    colours = {(bx, by): (bx * 8 + 1, by * 8 + 1, 0) for bx in range(n) for by in range(n)}
    stream = [b""] * (n * n)
    for (bx, by), (r, g, _) in colours.items():
        stream[twiddle(bx, by)] = block(0, colour_word((r, g, 0), (r, g, 0)))
    px = pixels(W.decode_tex(make_tex(9, 16, 16, b"".join(stream)))[2], 16)
    for (bx, by), (r, g, _) in colours.items():
        # pixel (2, 2) of a block weighs only its own block's colour
        assert tuple(px[4 * by + 2, 4 * bx + 2, :3]) == ((r << 3) | (r >> 2), (g << 3) | (g >> 2), 0)
    # pixel (0, 0) of block (0, 0) weighs (2, 2, 0) over (previous, own, next) per axis, wrapping to
    # block 3: R varies along x (1 and 25), G along y (1 and 25) -> 4 * (2*25 + 2*1) = 208 -> 107
    assert tuple(px[0, 0, :2]) == (107, 107)


def test_pvrtc_punch_through_and_alpha_colour():
    col = colour_word((31, 0, 0), (0, 31, 0), mode=1)
    mod = 2 | (3 << 2)                                  # pixel 0: weight 4 + alpha cleared; pixel 1: B
    px = pixels(W.decode_tex(make_tex(9, 8, 8, block(mod, col) * 4))[2], 8)
    assert tuple(px[0, 0]) == (127, 127, 0, 0)          # (255*4 + 0*4)/8 on r and (0 + 255*4)/8 on g
    assert tuple(px[0, 1]) == (0, 255, 0, 255)
    # translucent colours: A has alpha 3-bit 7 (-> 238), B alpha 0
    ca = 0x7000 | 0xF << 8 | 0xF << 4 | 0x7 << 1       # a=7 r=15 g=15 b=7, bit15 clear
    cb = 0x0000 | 0xF << 8 | 0xF << 4 | 0xF            # a=0 r=15 g=15 b=15
    px = pixels(W.decode_tex(make_tex(9, 8, 8, block(0, cb << 16 | ca) * 4))[2], 8)
    assert px[0, 0, 3] == 238
    px = pixels(W.decode_tex(make_tex(9, 8, 8, block(0xFFFFFFFF, cb << 16 | ca) * 4))[2], 8)
    assert px[0, 0, 3] == 0


def test_pvrtc_mips_pad_to_8x8_and_drop_1x1():
    # 16x16 PVRTC, header says 5 mips (16,8,4,2,1) but data holds 4: 16x16=128, then 3 x 32
    col = colour_word((31, 0, 0), (31, 0, 0))
    payload = block(0, col) * 16 + block(0, col) * 12
    tex = make_tex(9, 16, 16, payload, mips=5)
    info = W.parse_tex(tex)
    assert [(w, s) for w, _, _, s in info.levels] == [(16, 128), (8, 32), (4, 32), (2, 32)]
    for m, w in enumerate((16, 8, 4, 2)):
        ww, hh, rgba = W.decode_tex(tex, mip=m)
        assert (ww, hh) == (w, w) and (pixels(rgba, w) == [255, 0, 0, 255]).all()


def test_pvrtc_rejects_bad_geometry():
    with pytest.raises(FormatError):
        W.decode_pvrtc4(bytes(8), 4, 4)                # one block: below the 2x2-block minimum
    with pytest.raises(FormatError):
        W.decode_pvrtc4(bytes(8 * 6), 24, 8)           # not a power of two
    with pytest.raises(FormatError):
        W.decode_pvrtc4(bytes(8), 8, 8)                # truncated


# -- real archive (skipped when the Anniversary Edition zip is not installed) ------

AE_ZIP = os.environ.get("BULLY_AE_ZIP", "/Applications/bully.app/Wrapper/bully.app/data_1.zip")


@pytest.mark.skipif(not os.path.isfile(AE_ZIP), reason="Anniversary Edition data_1.zip not present")
def test_real_tex_sample_decodes():
    with zipfile.ZipFile(AE_ZIP) as z:
        names = [n for n in z.namelist() if n.endswith(".tex")]
        assert len(names) > 10000
        seen = set()
        for n in names[::25]:
            data = z.read(n)
            info = W.parse_tex(data)
            w, h, rgba = W.decode_tex(data)
            assert len(rgba) == w * h * 4
            seen.add(info.fmt)
    assert {9, 0} <= seen


@pytest.mark.parametrize("declared", ["0", "-4", "abc", "{x=1}", "100000"])
def test_bad_declared_size_is_format_error(declared):
    data = make_tex(8, 2, 2, bytes(4)).replace(b"width=2,", f"width={declared},".encode())
    # keep the header-length fields consistent with the changed text
    delta = len(declared) - 1
    n = struct.unpack_from("<I", data, 24)[0] + delta
    data = data[:20] + struct.pack("<II", 28 + n, n) + data[28:]
    with pytest.raises(FormatError):
        W.parse_tex(data)
