# OWNER: builder-A2 (animation)
"""Animation dictionaries (.HXD): names, play durations and groups for the
clips stored in .agr files.

Layout notes: docs/re-notes/anim-formats.md. The field order follows the
description in bully-re docs/hxd.md (FUN_006b2d90 in Bully.exe); this is an
independent implementation, checked to consume every byte of the shipped
Anim/*.HXD files (see tools/tests/test_real_install.py).

Summary, all little-endian:

    f32 version (1.12 in MAINPED), u32 unknown (OPEN)
    u32 masks;  per mask: char[32] name, u32 n, f32 (0.3), f32[n] weights
    u32 bones;  per bone: char[32] name
    u32 anims;  per anim: f32 duration, f32 blend, char[64] "GROUP\\NAME",
                u32 hash, u32 flags, u32 agr_size + 4, u32 group index,
                u16 events, per event (u32, u32, u32 kind, kind 1: +40 bytes),
                u16 x4, u32, [u32 if version > 1.09], [u32 if version > 1.11],
                f32 x3
    u32 groups; per group: char[32] name, u32 size (+ 32 bytes and u32 when
                version >= 1.11)
    12 bytes per bone, u32 n + n * 48 bytes, u32 final word

Linking a clip to its .agr data: the clips of group g are the anim records
whose `group` field equals g, in record order; the k-th such record is the
k-th clip in the group's .agr. Its play duration is the HXD duration (the
.agr key times are normalised, 0..1 of the duration).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .binreader import FormatError, Reader


@dataclass
class HxdAnim:
    index: int
    name: str            # "GROUP\\NAME"
    duration: float      # play duration in seconds (overrides the .agr header)
    blend: float         # default blend-in time (s), 0.3 for most clips
    hash: int            # HashString(name), what action-tree Animation tracks cite
    flags: int
    agr_size: int        # bytes of this clip inside the .agr, plus 4
    group: int           # index into HxdFile.groups
    events: list = field(default_factory=list)  # (u32, u32, kind)
    shorts: tuple = ()   # four u16, meaning OPEN
    vector: tuple = ()   # three f32, meaning OPEN

    @property
    def short_name(self) -> str:
        return self.name.split("\\", 1)[-1]


@dataclass
class HxdMask:
    name: str
    weights: tuple       # one per bone (INFERRED: count equals the bone count)
    blend: float         # 0.3 in every mask seen (meaning INFERRED: blend time)


@dataclass
class HxdGroup:
    index: int
    name: str
    size: int            # .agr byte size + 4 per clip


@dataclass
class HxdFile:
    version: float
    header_word: int
    masks: list
    bones: list
    anims: list
    groups: list
    consumed: int
    final_word: int

    def group_index(self, name: str) -> int | None:
        for g in self.groups:
            if g.name.lower() == name.lower():
                return g.index
        return None

    def group_anims(self, group: str | int) -> list:
        """Clips of a group in .agr order (record order, filtered by group)."""
        gi = group if isinstance(group, int) else self.group_index(group)
        if gi is None:
            return []
        return [a for a in self.anims if a.group == gi]

    def by_hash(self, h: int) -> list:
        return [a for a in self.anims if a.hash == h]

    def by_name(self, name: str) -> list:
        n = name.lower()
        return [a for a in self.anims if a.name.lower() == n]


def _name(r: Reader, n: int) -> str:
    raw = r.bytes(n)
    return raw.split(b"\0", 1)[0].decode("latin-1")


def parse_hxd(data, offset: int = 0) -> HxdFile:
    r = Reader(data, offset)
    version = r.f32()
    if not (0.5 < version < 1.12 + 1e-6):
        raise FormatError(f"HXD version {version!r} out of range")
    header_word = r.u32()
    n = r.u32()
    if n > 10000:
        raise FormatError(f"HXD: {n} masks")
    masks = []
    for _ in range(n):
        name = _name(r, 32)
        k = r.u32()
        if k > 1000:
            raise FormatError(f"HXD mask {name}: {k} weights")
        blend = r.f32()
        masks.append(HxdMask(name, r.array("f", k), blend))
    n = r.u32()
    if n > 1000:
        raise FormatError(f"HXD: {n} bones")
    bones = [_name(r, 32) for _ in range(n)]
    n_anims = r.u32()
    if n_anims > 100000:
        raise FormatError(f"HXD: {n_anims} anims")
    anims = []
    for i in range(n_anims):
        duration, blend = r.f32(), r.f32()
        name = _name(r, 64)
        h, flags, size, group = r.u32(), r.u32(), r.u32(), r.u32()
        events = []
        for _ in range(r.u16()):
            a, b, kind = r.u32(), r.u32(), r.u32()
            if kind == 1:
                r.skip(8 + 32)
            events.append((a, b, kind))
        shorts = r.array("H", 4)
        r.skip(4)
        if version > 1.09 + 1e-6:
            r.skip(4)
            if version > 1.11 + 1e-6:
                r.skip(4)
        vec = r.vec3()
        anims.append(HxdAnim(i, name, duration, blend, h, flags, size, group, events, shorts, vec))
    n = r.u32()
    if n > 100000:
        raise FormatError(f"HXD: {n} groups")
    groups = []
    for i in range(n):
        name = _name(r, 32)
        size = r.u32()
        if version >= 1.11 - 1e-6:
            r.skip(32)
            r.u32()
        groups.append(HxdGroup(i, name, size))
    r.skip(12 * len(bones))
    k = r.u32()
    if k > 100000:
        raise FormatError(f"HXD: {k} trailing blocks")
    r.skip(0x30 * k)
    final = r.u32()
    for a in anims:
        if a.group >= len(groups):
            raise FormatError(f"HXD anim {a.name}: group {a.group} >= {len(groups)}")
    return HxdFile(version, header_word, masks, bones, anims, groups, r.pos - offset, final)


def iter_hxds_dat(data):
    """Anim/hxds.dat packs HXDs: a u32 header word (131 in the shipped file,
    one more than the record count: meaning OPEN), then records of tag
    'ANIM', u32 size, the HXD bytes, to the end of the file. Yields
    (offset, size). VERIFIED: 130 records, each parses to its exact size,
    the last ends on the last byte."""
    p = 4
    while p < len(data):
        if p + 8 > len(data):
            raise FormatError("hxds.dat: record header runs past end")
        tag = bytes(data[p:p + 4])
        if tag != b"ANIM":
            raise FormatError(f"hxds.dat: tag {tag!r} at {p}")
        size = struct.unpack_from("<I", data, p + 4)[0]
        if p + 8 + size > len(data):
            raise FormatError(f"hxds.dat: record at {p} runs past end")
        yield p + 8, size
        p += 8 + size
