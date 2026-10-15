# OWNER: builder-A2 (animation)
"""Animation groups (.agr): skeletal clips, concatenated.

Layout notes and confidence: docs/re-notes/anim-formats.md. Field meanings
follow the description in bully-re docs/agr.md (decoders 0x6b1710,
0x6b4970, 0x6b5440 in Bully.exe); this is an independent implementation.

A group is a run of clips. Each clip:

    u32 0x100, u32 key type (999..1004), u32 key count, u32 0, f32 duration
    key_count * key_size bytes of keys
    for the types without a translation in the key (1000, 1001, 1002):
        8-byte position records (u16 key index, i16 x, y, z millimetres)
        up to the next clip header

Keys of all bones are interleaved in time order. Every key names the
previous key of the same bone (0 = none); the first N keys (previous 0 and
time 0) open one track each, N = bone count (36 for a pedestrian). A
position record borrows the time and the bone of the key it points at.

Key times are fractions of the clip's play duration (0..1). The play
duration is the one in the HXD record; the .agr header duration is only
the authoring value (they differ for ~15% of MAINPED clips).
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

from .binreader import FormatError

HEADER_MAGIC = 0x100
KEY_SIZE = {999: 32, 1000: 20, 1001: 12, 1002: 8, 1003: 20, 1004: 12}
KEY_HAS_TRANSLATION = {999: True, 1000: False, 1001: False, 1002: False, 1003: True, 1004: True}
MAX_BONES = 64


@dataclass
class Key:
    t: float                # fraction of the clip duration, 0..1
    q: tuple                # rotation quaternion (w, x, y, z), NiAVObject convention
    p: tuple | None = None  # translation in metres (parent frame), if the key carries one


@dataclass
class Clip:
    offset: int
    key_type: int
    key_count: int
    duration: float                                # .agr header duration (s)
    tracks: list = field(default_factory=list)     # per bone: list[Key] rotation keys
    positions: list = field(default_factory=list)  # per bone: list[(t, (x, y, z))] metres
    null_keys: int = 0                             # zero-norm keys skipped (no data for that bone)
    size: int = 0                                  # bytes up to the next clip

    @property
    def bone_count(self) -> int:
        return len(self.tracks)


def _u32(b, p):
    return struct.unpack_from("<I", b, p)[0]


def is_header(b, p: int, end: int) -> bool:
    if p + 20 > end:
        return False
    magic, kt, n, zero = struct.unpack_from("<IIII", b, p)
    d = struct.unpack_from("<f", b, p + 16)[0]
    return (magic == HEADER_MAGIC and kt in KEY_SIZE and zero == 0 and 0 < n < 20000
            and 0 < d < 1000)


def _packed_quat(w0: int, w1: int) -> tuple:
    """Type 1002/1004: four 10-bit magnitudes in 1/1023 with sign bits.
    w0: bit 20 sign x, bits 21-30 |x|, bit 31 sign y;
    w1: bits 0-9 |y|, bit 10 sign z, bits 11-20 |z|, bit 21 sign w, bits 22-31 |w|."""
    s = 1.0 / 1023.0

    def sg(bit):
        return -1.0 if bit else 1.0
    x = ((w0 >> 21) & 0x3FF) * s * sg((w0 >> 20) & 1)
    y = (w1 & 0x3FF) * s * sg((w0 >> 31) & 1)
    z = ((w1 >> 11) & 0x3FF) * s * sg((w1 >> 10) & 1)
    w = ((w1 >> 22) & 0x3FF) * s * sg((w1 >> 21) & 1)
    return (w, x, y, z)


def _packed_cm(v: int) -> tuple:
    """Type 1004 translation word: x bits 0-9 (sign 10), y bits 11-20
    (sign 21), z bits 22-30 (sign 31), centimetres -> metres."""
    x = (v & 0x3FF) * (-1 if (v >> 10) & 1 else 1)
    y = ((v >> 11) & 0x3FF) * (-1 if (v >> 21) & 1 else 1)
    z = ((v >> 22) & 0x1FF) * (-1 if (v >> 31) & 1 else 1)
    return (x * 0.01, y * 0.01, z * 0.01)


def decode_key(kt: int, b, p: int) -> tuple:
    """-> (previous key index, time fraction, quaternion wxyz, translation|None)."""
    if kt in (1002, 1004):
        w0, w1 = struct.unpack_from("<II", b, p)
        prev = w0 & 0x7FF
        t = ((w0 >> 11) & 0x1FF) / 511.0
        q = _packed_quat(w0, w1)
        tr = _packed_cm(_u32(b, p + 8)) if kt == 1004 else None
        return prev, t, q, tr
    prev, tt = struct.unpack_from("<HH", b, p)
    t = tt / 65535.0
    if kt in (1001, 1003):
        x, y, z, w = struct.unpack_from("<4h", b, p + 4)
        s = 1.0 / 32767.0
        q = (w * s, x * s, y * s, z * s)
        tr = None
        if kt == 1003:
            tx, ty, tz = struct.unpack_from("<3h", b, p + 12)  # +18: 2 pad bytes
            tr = (tx * 0.001, ty * 0.001, tz * 0.001)
        return prev, t, q, tr
    q = struct.unpack_from("<4f", b, p + 4)  # 1000/999: w, x, y, z
    tr = struct.unpack_from("<3f", b, p + 20) if kt == 999 else None
    return prev, t, tuple(q), tr


def _decode_clip(b, p: int, end: int, data_end: int, size: int | None = None) -> Clip:
    kt, n = _u32(b, p + 4), _u32(b, p + 8)
    dur = struct.unpack_from("<f", b, p + 16)[0]
    ks = KEY_SIZE[kt]
    keys_at = p + 20
    after = keys_at + n * ks
    if after > end:
        raise FormatError(f"clip at {p}: {n} keys of {ks} bytes run past end")
    # the clip ends at the next header (searched on 4-byte steps); trailing
    # zero padding is trimmed only for this search, never for key data (a
    # 1003 key ends in two zero pad bytes)
    if size is not None:  # exact size from the HXD record (agr_size - 4)
        nxt = p + size
        if nxt < after or nxt > end:
            raise FormatError(f"clip at {p}: HXD size {size} disagrees with {n} keys")
    else:
        nxt = after
        while nxt + 20 <= data_end and not is_header(b, nxt, data_end):
            nxt += 4
        if nxt + 20 > data_end:
            # last clip: the zero trim may have eaten the tail of the final
            # position record; round the record area back up to 8 bytes
            nxt = max(data_end, after)
            if not KEY_HAS_TRANSLATION[kt]:
                nxt = min(after + -(-(nxt - after) // 8) * 8, end)
    clip = Clip(p, kt, n, dur, size=nxt - p)
    decoded = [decode_key(kt, b, keys_at + i * ks) for i in range(n)]
    heads = 0
    while heads < n and heads < MAX_BONES and decoded[heads][0] == 0 and decoded[heads][1] == 0.0:
        heads += 1
    if heads == 0:
        raise FormatError(f"clip at {p}: no track heads")
    bone = list(range(heads)) + [-1] * (n - heads)
    for i in range(heads, n):
        prev = decoded[i][0]
        if prev >= i or bone[prev] < 0:
            raise FormatError(f"clip at {p}: key {i} points at key {prev}")
        bone[i] = bone[prev]
    clip.tracks = [[] for _ in range(heads)]
    clip.positions = [[] for _ in range(heads)]
    for i, (_prev, t, q, tr) in enumerate(decoded):
        n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]
        if n2 < 0.25:  # zero key: no data for this bone at this time
            clip.null_keys += 1
            continue
        inv = 1.0 / math.sqrt(n2)
        clip.tracks[bone[i]].append(Key(t, tuple(c * inv for c in q), tr))
        if tr is not None:
            clip.positions[bone[i]].append((t, tr))
    for tr in clip.tracks:
        for a, c in zip(tr, tr[1:]):
            if c.t < a.t:
                raise FormatError(f"clip at {p}: key times go backwards")
    if not KEY_HAS_TRANSLATION[kt]:
        for r in range(after, nxt - 7, 8):
            k, x, y, z = struct.unpack_from("<Hhhh", b, r)
            if k >= n or (k == 0 and x == 0 and y == 0 and z == 0):
                break  # an all-zero record is padding (C_Player's last clip ends in one)
            clip.positions[bone[k]].append((decoded[k][1], (x * 0.001, y * 0.001, z * 0.001)))
        for pos in clip.positions:
            pos.sort(key=lambda e: e[0])
    return clip


def parse_agr(data, sizes: list | None = None) -> list:
    """Split a group into clips and decode every one.

    `sizes`: optional per-clip byte sizes from the HXD (record agr_size - 4,
    in group order). Without them, clip ends are found by scanning for the
    next header; the two agree on every clip but the last of a file, where
    the scan cannot tell trailing position bytes from sector padding."""
    end = len(data)
    data_end = end
    while data_end > 0 and data[data_end - 1] == 0:
        data_end -= 1
    clips = []
    p = 0
    while p + 20 <= data_end and is_header(data, p, data_end):
        size = sizes[len(clips)] if sizes is not None and len(clips) < len(sizes) else None
        c = _decode_clip(data, p, end, data_end, size)
        clips.append(c)
        p += c.size
    if not clips:
        raise FormatError("no AGR clip header at offset 0")
    return clips


# ---------------------------------------------------------------------------
# Sampling helpers
# ---------------------------------------------------------------------------

def slerp(a, b, u):
    c = sum(x * y for x, y in zip(a, b))
    if c < 0:
        b = tuple(-x for x in b)
        c = -c
    if c > 0.9995:
        ka, kb = 1 - u, u
    else:
        th = math.acos(min(c, 1.0))
        s = math.sin(th)
        ka, kb = math.sin((1 - u) * th) / s, math.sin(u * th) / s
    out = [ka * x + kb * y for x, y in zip(a, b)]
    n = math.sqrt(sum(x * x for x in out)) or 1.0
    return tuple(x / n for x in out)


def sample_rotation(track: list, t: float):
    if not track:
        return None
    if t <= track[0].t:
        return track[0].q
    if t >= track[-1].t:
        return track[-1].q
    for a, b in zip(track, track[1:]):
        if b.t >= t:
            dt = b.t - a.t
            return slerp(a.q, b.q, (t - a.t) / dt if dt > 1e-9 else 0.0)
    return track[-1].q


def sample_position(pos: list, t: float):
    if not pos:
        return None
    if t <= pos[0][0]:
        return pos[0][1]
    if t >= pos[-1][0]:
        return pos[-1][1]
    for (ta, a), (tb, b) in zip(pos, pos[1:]):
        if tb >= t:
            u = (t - ta) / (tb - ta) if tb - ta > 1e-9 else 0.0
            return tuple(x + u * (y - x) for x, y in zip(a, b))
    return pos[-1][1]


def quat_to_rows(q) -> list:
    """(w, x, y, z) -> 3x3 rows; the NiAVObject convention (the node's stored
    row-major matrix is exactly this matrix, bully-re docs/agr.md)."""
    w, x, y, z = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def rows_to_quat(m) -> tuple:
    """3x3 rows (proper rotation) -> (w, x, y, z), w >= 0."""
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        w, x, y, z = 0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        w, x, y, z = (m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        w, x, y, z = (m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        w, x, y, z = (m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s
    n = math.sqrt(w * w + x * x + y * y + z * z) or 1.0
    q = (w / n, x / n, y / n, z / n)
    return q if q[0] >= 0 else tuple(-c for c in q)


def arrow_displacement(clip: Clip, arrow_bone: int = 35) -> tuple:
    """Ground-arrow travel over the clip (the character's root motion)."""
    if arrow_bone >= len(clip.positions) or len(clip.positions[arrow_bone]) < 2:
        return (0.0, 0.0, 0.0)
    a, b = clip.positions[arrow_bone][0][1], clip.positions[arrow_bone][-1][1]
    return tuple(y - x for x, y in zip(a, b))
