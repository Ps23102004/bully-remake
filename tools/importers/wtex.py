"""Bully: Anniversary Edition (iOS, WarEngine) .tex reader and decoder.

Layout (little-endian; surveyed over all 14,064 .tex in data_1.zip, see
docs/re-notes/ae-tex.md):

    u32 7, u32 2, u32 0x6e, u32 0x18      magic / version words
    u32 mode_tag                          per-mode constant (tm_standard=9, ...)
    u32 data_start                        = 28 + text_len
    u32 text_len
    char text[text_len]                   "{width=512,height=512,mode=tm_standard,...}"
    u32 fmt, u32 width, u32 height, u32 mips, u32 data_size
    u8  data[data_size]                   mip 0 first, tightly packed

``fmt`` is the real pixel format (``mode_tag`` is only a hint, and the two disagree for
tm_standard/tm_distancefield). If ``compressondisk=true`` the data is u32 raw_size followed
by a zlib stream. Stored width/height can differ from the declared ``width``/``height``
(non-square PVRTC textures are stored stretched to a square, tiny placeholders as 8x8).

    fmt 0 RGBA8888    fmt 1 RGB888    fmt 3 RGB565    fmt 4 RGBA4444
    fmt 5 DXT1        fmt 7 DXT5      fmt 8 L8        fmt 9 PVRTC 4bpp

numpy is required. No game bytes live in this file.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

import numpy as np

from .binreader import FormatError
from .textures import _decode_dxt_np, has_alpha, write_png

FMT_RGBA8888, FMT_RGB888, FMT_RGB565, FMT_RGBA4444 = 0, 1, 3, 4
FMT_DXT1, FMT_DXT5, FMT_L8, FMT_PVRTC4 = 5, 7, 8, 9
FORMAT_NAMES = {0: "RGBA8888", 1: "RGB888", 3: "RGB565", 4: "RGBA4444",
                5: "DXT1", 7: "DXT5", 8: "L8", 9: "PVRTC4"}

_MAGIC = (7, 2, 0x6E, 0x18)
_MAX_DIM = 8192


@dataclass
class TexInfo:
    meta: dict
    fmt: int
    width: int                 # stored
    height: int
    declared_width: int        # from the text header
    declared_height: int
    mips: int                  # count from the header (PVRTC stores mips-1 levels)
    compressed: bool           # compressondisk=true (zlib)
    data_offset: int           # offset of the payload (incl. u32 raw size when compressed)
    data_size: int
    levels: list = field(default_factory=list)   # (w, h, offset, size) into the raw payload

    @property
    def format_name(self) -> str:
        return FORMAT_NAMES.get(self.fmt, f"fmt{self.fmt}")


# ---------------------------------------------------------------------------
# header text:  {k=v,k={k=v,...},k="string"}
# ---------------------------------------------------------------------------

def _parse_text(s: str, i: int = 0):
    """Parse '{...}' starting at s[i]; returns (dict, index after the closing brace)."""
    if s[i] != "{":
        raise FormatError("header text does not start with '{'")
    i += 1
    out = {}
    while i < len(s) and s[i] != "}":
        j = s.index("=", i)
        key = s[i:j]
        i = j + 1
        if s[i] == "{":
            val, i = _parse_text(s, i)
        elif s[i] == '"':
            j = s.index('"', i + 1)
            val, i = s[i + 1:j], j + 1
        else:
            j = i
            while s[j] not in ",}":
                j += 1
            val, i = s[i:j], j
            for conv in (int, float):
                try:
                    val = conv(val)
                    break
                except ValueError:
                    pass
            else:
                val = {"true": True, "false": False}.get(val, val)
        out[key] = val
        if i < len(s) and s[i] == ",":
            i += 1
    if i >= len(s):
        raise FormatError("unterminated header text")
    return out, i + 1


# ---------------------------------------------------------------------------
# size arithmetic
# ---------------------------------------------------------------------------

def level_size(fmt: int, w: int, h: int) -> int:
    if fmt == FMT_PVRTC4:
        return max(w, 8) * max(h, 8) // 2          # 4bpp, padded to 2x2 blocks (8x8 px)
    if fmt in (FMT_DXT1, FMT_DXT5):
        return ((w + 3) // 4) * ((h + 3) // 4) * (8 if fmt == FMT_DXT1 else 16)
    bpp = {FMT_RGBA8888: 4, FMT_RGB888: 3, FMT_RGB565: 2, FMT_RGBA4444: 2, FMT_L8: 1}.get(fmt)
    if bpp is None:
        raise FormatError(f"unknown .tex pixel format {fmt}")
    return w * h * bpp


def _levels(fmt: int, w: int, h: int, count: int, raw_size: int) -> list:
    """Mip table. PVRTC drops the 1x1 level so it stores mips-1; the rest store all."""
    for n in (count, count - 1):
        if n < 1:
            continue
        lv, off = [], 0
        for i in range(n):
            lw, lh = max(w >> i, 1), max(h >> i, 1)
            sz = level_size(fmt, lw, lh)
            lv.append((lw, lh, off, sz))
            off += sz
        if off == raw_size:
            return lv
    raise FormatError(f"{FORMAT_NAMES.get(fmt, fmt)} {w}x{h} with {count} mips does not add up to {raw_size} bytes")


def parse_tex(data: bytes) -> TexInfo:
    if len(data) < 28 + 20:
        raise FormatError("tex: file too short")
    hdr = struct.unpack_from("<7I", data, 0)
    if hdr[:4] != _MAGIC:
        raise FormatError(f"tex: bad magic {hdr[:4]}")
    tlen = hdr[6]
    if hdr[5] != 28 + tlen or 28 + tlen + 20 > len(data):
        raise FormatError("tex: inconsistent header text length")
    try:
        meta, end = _parse_text(data[28:28 + tlen].decode("latin1"))
    except (ValueError, IndexError) as e:
        raise FormatError(f"tex: bad header text: {e}") from None
    if end != tlen:
        raise FormatError("tex: trailing bytes in header text")
    o = 28 + tlen
    fmt, w, h, mips, dsize = struct.unpack_from("<5I", data, o)
    o += 20
    if not (0 < w <= _MAX_DIM and 0 < h <= _MAX_DIM and 0 < mips <= 16):
        raise FormatError(f"tex: implausible size {w}x{h} mips {mips}")
    if o + dsize != len(data):
        raise FormatError(f"tex: data_size {dsize} does not match file ({len(data) - o} bytes follow)")
    compressed = bool(meta.get("compressondisk"))
    if compressed:
        if dsize < 4:
            raise FormatError("tex: compressed payload too short")
        raw_size = struct.unpack_from("<I", data, o)[0]
    else:
        raw_size = dsize
    try:
        dw, dh = int(meta.get("width", w)), int(meta.get("height", h))
    except (TypeError, ValueError):
        raise FormatError("tex: non-numeric declared size") from None
    if not (0 < dw <= _MAX_DIM and 0 < dh <= _MAX_DIM):
        raise FormatError(f"tex: implausible declared size {dw}x{dh}")
    info = TexInfo(meta, fmt, w, h, dw, dh,
                   mips, compressed, o, dsize)
    info.levels = _levels(fmt, w, h, mips, raw_size)
    return info


def _payload(data: bytes, info: TexInfo) -> memoryview:
    p = memoryview(data)[info.data_offset:info.data_offset + info.data_size]
    if not info.compressed:
        return p
    want = struct.unpack_from("<I", p, 0)[0]
    d = zlib.decompressobj()
    try:
        raw = d.decompress(bytes(p[4:]), want + 1)
    except zlib.error as e:
        raise FormatError(f"tex: bad zlib stream: {e}") from None
    if len(raw) != want:
        raise FormatError(f"tex: zlib gave {len(raw)} bytes, header says {want}")
    return memoryview(raw)


# ---------------------------------------------------------------------------
# PVRTC 4bpp (PowerVR texture compression v1), vectorised over all blocks
# ---------------------------------------------------------------------------

_PVR_INTERP = np.array([[2, 2, 0], [1, 3, 0], [0, 4, 0], [0, 3, 1]], np.int32)  # weights of prev/self/next block
_PVR_STD = np.array([0, 3, 5, 8], np.int32)
_PVR_PUNCH = np.array([0, 4, 4, 8], np.int32)


def _morton(x, y, min_dim: int):
    """Block index in the twiddled stream: y in even bits, x in odd bits up to min_dim, then linear."""
    off = np.zeros_like(x)
    shift, mask = 0, 1
    while mask < min_dim:
        off |= ((y & mask) | ((x & mask) << 1)) << shift
        mask <<= 1
        shift += 1
    return off | (((x | y) >> shift) << (shift * 2))


def _pvr_colours(c, is_a: bool):
    """16-bit colour word -> (..., 4) ints: r,g,b in 0..31 and a in 0..15."""
    op = (c & 0x8000) != 0
    r = np.where(op, c >> 10 & 0x1F, (c >> 7 & 0x1E) | (c >> 11 & 1))
    g = np.where(op, c >> 5 & 0x1F, (c >> 3 & 0x1E) | (c >> 7 & 1))
    if is_a:
        b = np.where(op, (c & 0x1E) | (c >> 4 & 1), (c << 1 & 0x1C) | (c >> 2 & 3))
    else:
        b = np.where(op, c & 0x1F, (c << 1 & 0x1E) | (c >> 3 & 1))
    a = np.where(op, 15, c >> 11 & 0xE)
    return np.stack([r, g, b, a], -1)


def _pvr_upscale(col):
    """(bh, bw, 4) block colours -> (bh, 4, bw, 4, 4): bilinear x4 with wraparound neighbours."""
    cx = np.stack([np.roll(col, 1, 1), col, np.roll(col, -1, 1)], 2)       # (bh, bw, 3, c)
    hx = np.einsum("xk,ijkc->ijxc", _PVR_INTERP, cx)                        # (bh, bw, 4x, c)
    cy = np.stack([np.roll(hx, 1, 0), hx, np.roll(hx, -1, 0)], 1)           # (bh, 3, bw, 4x, c)
    return np.einsum("yk,ikjxc->iyjxc", _PVR_INTERP, cy)                    # (bh, 4y, bw, 4x, c)


def decode_pvrtc4(buf, w: int, h: int) -> bytes:
    """Decode a PVRTC1 4bpp image (w, h powers of two, >= 8) to RGBA8 bytes."""
    bw, bh = w // 4, h // 4
    if bw < 2 or bh < 2 or bw & (bw - 1) or bh & (bh - 1) or w % 4 or h % 4:
        raise FormatError(f"PVRTC needs power-of-two sizes >= 8, got {w}x{h}")
    n = bw * bh
    if len(buf) < n * 8:
        raise FormatError(f"PVRTC {w}x{h} needs {n * 8} bytes, got {len(buf)}")
    words = np.frombuffer(buf, "<u4", count=2 * n).reshape(n, 2)
    by, bx = np.mgrid[:bh, :bw]
    idx = _morton(bx, by, min(bw, bh))
    mod = words[idx, 0].astype(np.int64)
    col = words[idx, 1].astype(np.int64)

    def scale(v):               # sum of weights is 16: 5-bit -> 8-bit, 4-bit alpha -> 8-bit
        return np.concatenate([(v[..., :3] >> 1) + (v[..., :3] >> 6), v[..., 3:] + (v[..., 3:] >> 4)], -1)

    A = scale(_pvr_upscale(_pvr_colours(col & 0xFFFF, True)))               # (bh, 4, bw, 4, 4)
    B = scale(_pvr_upscale(_pvr_colours(col >> 16, False)))

    sel = (mod[..., None] >> (2 * np.arange(16))) & 3                       # (bh, bw, 16)
    punch = (col & 1).astype(bool)[..., None]
    wt = np.where(punch, _PVR_PUNCH[sel], _PVR_STD[sel])
    clear = punch & (sel == 2)
    wt = wt.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3)[..., None]          # (bh, 4y, bw, 4x, 1)
    clear = clear.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3)
    out = (A * (8 - wt) + B * wt) // 8
    out[..., 3] = np.where(clear, 0, out[..., 3])
    return np.clip(out, 0, 255).astype(np.uint8).reshape(h, w, 4).tobytes()


# ---------------------------------------------------------------------------
# simple formats
# ---------------------------------------------------------------------------

def _rgb565(buf, w, h):
    v = np.frombuffer(buf, "<u2", count=w * h).astype(np.uint32)
    r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
    out = np.empty((w * h, 4), np.uint8)
    out[:, 0], out[:, 1], out[:, 2], out[:, 3] = (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255
    return out.tobytes()


def _rgba4444(buf, w, h):
    v = np.frombuffer(buf, "<u2", count=w * h).astype(np.uint32)
    out = np.empty((w * h, 4), np.uint8)
    for c, sh in enumerate((12, 8, 4, 0)):      # R is the high nibble, A the low one
        out[:, c] = (v >> sh & 15) * 17
    return out.tobytes()


def _decode_level(fmt: int, buf, w: int, h: int) -> bytes:
    if fmt == FMT_PVRTC4:
        pw, ph = max(w, 8), max(h, 8)
        rgba = decode_pvrtc4(buf, pw, ph)
        if (pw, ph) != (w, h):
            rgba = np.frombuffer(rgba, np.uint8).reshape(ph, pw, 4)[:h, :w].tobytes()
        return rgba
    if fmt == FMT_DXT1:
        return _decode_dxt_np(buf, w, h, 4)
    if fmt == FMT_DXT5:
        return _decode_dxt_np(buf, w, h, 6)
    if fmt == FMT_RGBA8888:
        return bytes(buf)
    if fmt == FMT_RGB888:
        out = np.full((w * h, 4), 255, np.uint8)
        out[:, :3] = np.frombuffer(buf, np.uint8, count=w * h * 3).reshape(-1, 3)
        return out.tobytes()
    if fmt == FMT_RGB565:
        return _rgb565(buf, w, h)
    if fmt == FMT_RGBA4444:
        return _rgba4444(buf, w, h)
    if fmt == FMT_L8:
        v = np.frombuffer(buf, np.uint8, count=w * h)
        return np.stack([v, v, v, np.full_like(v, 255)], -1).tobytes()
    raise FormatError(f"unknown .tex pixel format {fmt}")


def _resample(rgba: bytes, w: int, h: int, dw: int, dh: int) -> bytes:
    a = np.frombuffer(rgba, np.uint8).reshape(h, w, 4)
    for axis, src, dst in ((0, h, dh), (1, w, dw)):
        if src == dst:
            continue
        if src % dst == 0:      # box filter
            k = src // dst
            s = a.reshape(dst, k, w, 4) if axis == 0 else a.reshape(a.shape[0], dst, k, 4)
            a = ((s.astype(np.uint32).sum(1 if axis == 0 else 2) + k // 2) // k).astype(np.uint8)
        else:                   # nearest (upscale or odd ratio)
            a = np.take(a, (np.arange(dst) * src) // dst, axis=axis)
        w = a.shape[1]
        h = a.shape[0]
    return a.tobytes()


def decode_tex(data: bytes, mip: int = 0, declared: bool = False):
    """Return (width, height, rgba_bytes) for one mip level.

    The size is the stored size unless ``declared`` is true and the header declares a different
    one (then mip 0 is resampled to it: box filter down, nearest up).
    """
    info = parse_tex(data)
    if not 0 <= mip < len(info.levels):
        raise FormatError(f"mip {mip} missing ({len(info.levels)} levels)")
    w, h, off, size = info.levels[mip]
    payload = _payload(data, info)
    rgba = _decode_level(info.fmt, payload[off:off + size], w, h)
    if declared and mip == 0 and (info.declared_width, info.declared_height) != (w, h):
        dw, dh = info.declared_width, info.declared_height
        return dw, dh, _resample(rgba, w, h, dw, dh)
    return w, h, rgba


def write_tex_png(data: bytes, path: str, mip: int = 0, declared: bool = False) -> tuple:
    """Decode and write a PNG (RGB if fully opaque, else RGBA). Returns (w, h, has_alpha)."""
    w, h, rgba = decode_tex(data, mip, declared)
    alpha = has_alpha(rgba)
    write_png(path, w, h, rgba, keep_alpha=alpha)
    return w, h, alpha
