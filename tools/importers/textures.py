# OWNER: builder-A (importers)
"""NiPixelData -> RGBA8 decoding and a dependency-free PNG writer.

Pixel formats (survey 3.2, Gamebryo NiPixelFormat numbering):
    0 RGB24, 1 RGBA32, 2 PAL8 (NiPalette), 4 DXT1/BC1, 5 DXT3/BC2, 6 DXT5/BC3
DXT3 never occurs in the shipped data but is implemented for completeness.
Raw RGB/RGBA channel order is read from the channel type table
(0 red, 1 green, 2 blue, 3 alpha) rather than assumed.

Pixel bytes are little-endian in every file, including the big-endian CS_*
files (survey 3.3); they are never byte-swapped here.

numpy is used when available (fast path); a pure-Python decoder is kept for
environments without it and is what the unit tests cross-check against.
"""

from __future__ import annotations

import struct
import zlib

from .binreader import FormatError

try:  # optional acceleration
    import numpy as np
except ImportError:  # pragma: no cover - exercised only without numpy
    np = None

FMT_RGB = 0
FMT_RGBA = 1
FMT_PAL8 = 2
FMT_DXT1 = 4
FMT_DXT3 = 5
FMT_DXT5 = 6
FORMAT_NAMES = {0: "RGB24", 1: "RGBA32", 2: "PAL8", 4: "DXT1", 5: "DXT3", 6: "DXT5"}


def level_size(fmt: int, w: int, h: int) -> int:
    bw, bh = (w + 3) // 4, (h + 3) // 4
    if fmt == FMT_RGB:
        return w * h * 3
    if fmt == FMT_RGBA:
        return w * h * 4
    if fmt == FMT_PAL8:
        return w * h
    if fmt == FMT_DXT1:
        return bw * bh * 8
    if fmt in (FMT_DXT3, FMT_DXT5):
        return bw * bh * 16
    raise FormatError(f"unsupported pixel format {fmt}")


# ---------------------------------------------------------------------------
# Pure-Python block decoders (reference implementation)
# ---------------------------------------------------------------------------

def _565(c: int) -> list:
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255]


def _color_palette(c0: int, c1: int, allow_punchthrough: bool) -> list:
    p0, p1 = _565(c0), _565(c1)
    if c0 > c1 or not allow_punchthrough:
        p2 = [(2 * p0[k] + p1[k]) // 3 for k in range(3)] + [255]
        p3 = [(p0[k] + 2 * p1[k]) // 3 for k in range(3)] + [255]
    else:
        p2 = [(p0[k] + p1[k]) // 2 for k in range(3)] + [255]
        p3 = [0, 0, 0, 0]
    return [p0, p1, p2, p3]


def decode_dxt_block_py(block: bytes, fmt: int) -> list:
    """Decode one 4x4 block into 16 [r, g, b, a] lists (row-major)."""
    if fmt == FMT_DXT1:
        c0, c1, idx = struct.unpack_from("<HHI", block, 0)
        pal = _color_palette(c0, c1, True)
        return [list(pal[(idx >> (2 * i)) & 3]) for i in range(16)]
    c0, c1, idx = struct.unpack_from("<HHI", block, 8)
    pal = _color_palette(c0, c1, False)
    px = [list(pal[(idx >> (2 * i)) & 3]) for i in range(16)]
    if fmt == FMT_DXT3:
        bits = int.from_bytes(block[0:8], "little")
        for i in range(16):
            a4 = (bits >> (4 * i)) & 15
            px[i][3] = a4 * 17
        return px
    a0, a1 = block[0], block[1]
    if a0 > a1:
        alphas = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
    else:
        alphas = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
    bits = int.from_bytes(block[2:8], "little")
    for i in range(16):
        px[i][3] = alphas[(bits >> (3 * i)) & 7]
    return px


def _decode_dxt_py(src, w: int, h: int, fmt: int) -> bytes:
    step = 8 if fmt == FMT_DXT1 else 16
    bw, bh = (w + 3) // 4, (h + 3) // 4
    out = bytearray(w * h * 4)
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * step
            px = decode_dxt_block_py(bytes(src[o:o + step]), fmt)
            for y in range(4):
                yy = by * 4 + y
                if yy >= h:
                    break
                for x in range(4):
                    xx = bx * 4 + x
                    if xx >= w:
                        break
                    d = (yy * w + xx) * 4
                    out[d:d + 4] = bytes(px[y * 4 + x])
    return bytes(out)


# ---------------------------------------------------------------------------
# numpy block decoders (same arithmetic, vectorised over all blocks)
# ---------------------------------------------------------------------------

def _np_565(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1)


def _np_colors(color_part, allow_punchthrough: bool):
    """color_part: (n, 8) uint8 -> (n, 16, 4) uint8 RGBA."""
    n = color_part.shape[0]
    words = color_part.view("<u2").reshape(n, 4).astype(np.int32)
    c0, c1 = words[:, 0], words[:, 1]
    idx = (words[:, 2] | (words[:, 3] << 16)).astype(np.int64)
    p0, p1 = _np_565(c0), _np_565(c1)
    four = (c0 > c1)[:, None] if allow_punchthrough else np.ones((n, 1), bool)
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    pal = np.zeros((n, 4, 4), np.int32)
    pal[:, 0, :3], pal[:, 1, :3], pal[:, 2, :3], pal[:, 3, :3] = p0, p1, p2, p3
    pal[:, :3, 3] = 255
    pal[:, 3, 3] = np.where(four[:, 0], 255, 0)
    sel = (idx[:, None] >> (2 * np.arange(16))[None, :]) & 3  # (n, 16)
    return np.take_along_axis(pal, sel[:, :, None].repeat(4, axis=2), axis=1).astype(np.uint8)


def _decode_dxt_np(src, w: int, h: int, fmt: int) -> bytes:
    step = 8 if fmt == FMT_DXT1 else 16
    bw, bh = (w + 3) // 4, (h + 3) // 4
    n = bw * bh
    blocks = np.frombuffer(bytes(src[: n * step]), np.uint8).reshape(n, step)
    if fmt == FMT_DXT1:
        px = _np_colors(np.ascontiguousarray(blocks), True)
    else:
        px = _np_colors(np.ascontiguousarray(blocks[:, 8:]), False)
        if fmt == FMT_DXT3:
            bits = blocks[:, :8].copy().view("<u8")[:, 0]
            a = (bits[:, None] >> (4 * np.arange(16, dtype=np.uint64))[None, :]) & np.uint64(15)
            px[:, :, 3] = (a * 17).astype(np.uint8)
        else:
            a0 = blocks[:, 0].astype(np.int32)
            a1 = blocks[:, 1].astype(np.int32)
            k = np.arange(1, 7)
            eight = np.concatenate([a0[:, None], a1[:, None],
                                    ((7 - k)[None, :] * a0[:, None] + k[None, :] * a1[:, None]) // 7], axis=1)
            k5 = np.arange(1, 5)
            six = np.concatenate([a0[:, None], a1[:, None],
                                  ((5 - k5)[None, :] * a0[:, None] + k5[None, :] * a1[:, None]) // 5,
                                  np.zeros((n, 1), np.int32), np.full((n, 1), 255, np.int32)], axis=1)
            table = np.where((a0 > a1)[:, None], eight, six)
            raw = np.zeros((n, 8), np.uint8)
            raw[:, :6] = blocks[:, 2:8]
            bits = raw.view("<u8")[:, 0]
            sel = ((bits[:, None] >> (3 * np.arange(16, dtype=np.uint64))[None, :]) & np.uint64(7)).astype(np.int64)
            px[:, :, 3] = np.take_along_axis(table, sel, axis=1).astype(np.uint8)
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w].tobytes()


# ---------------------------------------------------------------------------

def _channel_order(channels) -> list:
    order = [0, 1, 2, 3]
    for k, ch in enumerate(channels):
        if ch["type"] <= 3:
            order[ch["type"]] = k
    return order


def decode_pixel_data(pix: dict, palette: dict | None = None, mip: int = 0, use_numpy: bool = True):
    """Return (width, height, rgba_bytes) for one mip level of a NiPixelData."""
    fmt = pix["pixel_format"]
    if fmt not in FORMAT_NAMES:
        raise FormatError(f"unsupported pixel format {fmt}")
    if mip >= len(pix["mipmaps"]):
        raise FormatError(f"mip {mip} missing ({len(pix['mipmaps'])} levels)")
    w, h, off = pix["mipmaps"][mip]
    if w == 0 or h == 0:
        raise FormatError("zero-sized mip level")
    size = level_size(fmt, w, h)
    if off + size > pix["num_pixels"]:
        raise FormatError(f"mip {mip} ({w}x{h}) needs {off + size} bytes, face has {pix['num_pixels']}")
    src = pix["pixels"][off:off + size]
    if fmt in (FMT_DXT1, FMT_DXT3, FMT_DXT5):
        if use_numpy and np is not None:
            return w, h, _decode_dxt_np(src, w, h, fmt)
        return w, h, _decode_dxt_py(src, w, h, fmt)
    if fmt in (FMT_RGB, FMT_RGBA):
        n = 3 if fmt == FMT_RGB else 4
        o = _channel_order(pix["channels"])
        out = bytearray(w * h * 4)
        src = bytes(src)
        for c in range(3):
            out[c::4] = src[o[c]::n]
        out[3::4] = src[o[3]::n] if n == 4 else b"\xff" * (w * h)
        return w, h, bytes(out)
    # PAL8
    if palette is None:
        raise FormatError("PAL8 texture without NiPalette")
    entries = palette["entries"]
    count = len(entries) // 4
    out = bytearray(w * h * 4)
    for i, e in enumerate(bytes(src)):
        if e >= count:
            raise FormatError(f"palette index {e} >= {count}")
        out[i * 4:i * 4 + 4] = entries[e * 4:e * 4 + 4]
        if not palette["has_alpha"]:
            out[i * 4 + 3] = 255
    return w, h, bytes(out)


def has_alpha(rgba: bytes) -> bool:
    return any(a != 255 for a in rgba[3::4]) if np is None else bool((np.frombuffer(rgba, np.uint8)[3::4] != 255).any())


def write_png(path: str, w: int, h: int, rgba: bytes, keep_alpha: bool = True) -> None:
    """Minimal PNG encoder (RGBA8 or RGB8), filter 0, zlib level 6."""
    if len(rgba) != w * h * 4:
        raise ValueError("rgba buffer size mismatch")
    if keep_alpha:
        ctype, row_bytes, pixels = 6, w * 4, rgba
    else:
        ctype, row_bytes = 2, w * 3
        if np is not None:
            pixels = np.frombuffer(rgba, np.uint8).reshape(-1, 4)[:, :3].tobytes()
        else:
            b = bytearray(w * h * 3)
            for c in range(3):
                b[c::3] = rgba[c::4]
            pixels = bytes(b)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += pixels[y * row_bytes:(y + 1) * row_bytes]

    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def read_png_header(path: str) -> tuple:
    """(width, height, color_type) of a PNG we wrote; used by validation."""
    with open(path, "rb") as f:
        head = f.read(33)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        raise FormatError(f"{path}: not a PNG")
    w, h, _bd, ct = struct.unpack(">IIBB", head[16:26])
    return w, h, ct
