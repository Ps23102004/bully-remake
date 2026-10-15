# OWNER: builder-A (importers)
"""Compiled placement files `<stem>.ipb` ("Ipl$") from World.img (survey 4.3).

Layout (little-endian):
    0   "Ipl$"
    4   15 bytes, not interpreted (OPEN-3); kept as hex in the output
    19  u32 section count
    23  sections: tag[4] (e.g. "inst"), u32 count, `count` entries

`inst` entry, 120 bytes:
    +0   i32     model id (IDE id)
    +4   char64  model name, NUL padded (may be empty: then the id is used)
    +68  f32     unknown (OPEN-3)
    +72  f32[3]  position, game world units (metres), Z up
    +84  f32[3]  scale, per axis
    +96  f32[4]  rotation quaternion x, y, z, w
    +112 i32     unknown (OPEN-3)
    +116 u32     unknown (OPEN-3)

Other section sizes follow bully-re's IplFile.h notes. Sections we do not
interpret are skipped by their documented sizes, and an unknown tag is a hard
error. After the last section the file must be zero padding to the sector end;
anything else is reported as a layout error.

Rotation convention (bully-re docs/ipl.md, test_placement): the quaternion
must be CONJUGATED before use, i.e. the vertex rotation matrix is the
transpose of the textbook matrix of (x, y, z, w). Scale is per axis and is
applied in model space before the rotation.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from .binreader import FormatError, Reader

INST_SIZE = 120
FIXED_SIZES = {"spec": 128, "proj": 108, "occl": 28}


@dataclass
class Placement:
    index: int
    model_id: int
    name: str
    unk68: float
    position: tuple
    scale: tuple
    rotation: tuple  # quaternion (x, y, z, w) exactly as stored
    unk112: int
    unk116: int

    def to_json(self) -> dict:
        return {
            "index": self.index,
            "model_id": self.model_id,
            "name": self.name,
            "position": list(self.position),
            "scale": list(self.scale),
            "rotation_xyzw_stored": list(self.rotation),
            "unk68": self.unk68,
            "unk112": self.unk112,
            "unk116": self.unk116,
        }


@dataclass
class IpbFile:
    header_unknown: bytes
    section_counts: dict
    placements: list
    data_end: int


def _name8(r: Reader) -> str:
    """u8 length-prefixed name (prop/rail/perm/pont/pois/trig/pthx)."""
    n = r.u8()
    return r.bytes(n).split(b"\0", 1)[0].decode("latin-1")


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin-1")


def parse_ipb(data) -> IpbFile:
    if len(data) < 23 or bytes(data[:4]) != b"Ipl$":
        raise FormatError("not an Ipl$ placement file")
    r = Reader(data, 4)
    header_unknown = r.bytes(15)
    n_sections = r.u32()
    if n_sections > 64:
        raise FormatError(f"section count {n_sections}")
    counts: dict[str, int] = {}
    placements: list[Placement] = []
    for _ in range(n_sections):
        tag = r.bytes(4).decode("latin-1")
        count = r.u32()
        if count > 1_000_000:
            raise FormatError(f"section {tag!r}: count {count}")
        counts[tag] = counts.get(tag, 0) + count
        if tag == "inst":
            for _i in range(count):
                r.need(INST_SIZE)
                mid, raw_name, unk68 = struct.unpack_from("<i64sf", r.data, r.pos)
                vals = struct.unpack_from("<10f", r.data, r.pos + 72)
                unk112, unk116 = struct.unpack_from("<iI", r.data, r.pos + 112)
                r.pos += INST_SIZE
                placements.append(Placement(len(placements), mid, _cstr(raw_name), unk68,
                                            vals[0:3], vals[3:6], vals[6:10], unk112, unk116))
        elif tag in FIXED_SIZES:
            r.skip(count * FIXED_SIZES[tag])
        elif tag == "prop":
            for _i in range(count):
                _name8(r)
        elif tag == "rail":
            for _i in range(count):
                _name8(r)
                r.skip(4)
                r.skip(r.u32() * 12)
        elif tag == "perm":
            for _i in range(count):
                _name8(r)
                r.skip(12)
                r.skip(r.u32() * 4)
        elif tag == "pont":
            for _i in range(count):
                _name8(r)
                r.skip(4 + 12 + 12 + 4)
                _name8(r), _name8(r), _name8(r)
                r.skip(24)
                _name8(r), _name8(r)
        elif tag == "pois":
            for _i in range(count):
                _name8(r)
                r.skip(4)
                for _k in range(r.u32()):
                    for _s in range(4):
                        _name8(r)
                    r.skip(21 * 4)
        elif tag == "trig":
            for _i in range(count):
                _name8(r)
                r.skip(19 * 4)
        elif tag == "pthx":
            for _i in range(count):
                _name8(r)
                r.skip(8)
                r.skip(r.u32() * 36)
        else:
            raise FormatError(f"unknown section tag {tag!r} at {r.pos - 8}")
    end = r.pos
    tail = bytes(data[end:])
    if tail.count(0) != len(tail):
        raise FormatError(f"{len(tail) - tail.count(0)} non-zero bytes after last section (layout mismatch)")
    return IpbFile(header_unknown, counts, placements, end)


# ---------------------------------------------------------------------------
# Rotation helpers (game space: right-handed, Z up, metres)
# ---------------------------------------------------------------------------

def quat_to_matrix(q) -> list:
    """Textbook rotation matrix (column-vector convention) of quaternion
    (x, y, z, w), normalised. Returns 3 rows."""
    x, y, z, w = q
    n = (x * x + y * y + z * z + w * w) ** 0.5
    if n < 1e-12:
        raise FormatError("zero-length placement quaternion")
    x, y, z, w = x / n, y / n, z / n, w / n
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]


def placement_rotation(q) -> list:
    """The rotation the game applies to a placed model's vertices: the
    transpose of quat_to_matrix(q), i.e. the conjugate quaternion
    (bully-re docs/ipl.md, 7 of 11 discriminating placements, 0 against)."""
    m = quat_to_matrix(q)
    return [[m[j][i] for j in range(3)] for i in range(3)]


def placement_quat_game(q) -> list:
    """Quaternion (x, y, z, w) whose textbook matrix is placement_rotation(q):
    the normalised conjugate of the stored quaternion."""
    x, y, z, w = q
    n = (x * x + y * y + z * z + w * w) ** 0.5
    return [-x / n, -y / n, -z / n, w / n]
