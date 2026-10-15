# OWNER: builder-A (importers)
"""IMG/DIR archive reader (format-survey.md section 1).

A `<stem>.dir` is a flat array of 32-byte little-endian records:

    u32  offset   in 2048-byte sectors from the start of <stem>.img
    u32  size     in sectors (the exact byte length is NOT stored)
    char name[24] NUL-padded, looked up case-insensitively

There is no header and no compression. Entry data handed back by `read()` is
sector padded; format parsers find the real length themselves (OPEN-7).
"""

from __future__ import annotations

import os
import struct
from dataclasses import dataclass

from .binreader import FormatError

SECTOR = 2048
RECORD = 32


@dataclass(frozen=True)
class DirEntry:
    index: int
    name: str
    offset_sectors: int
    size_sectors: int

    @property
    def offset(self) -> int:
        return self.offset_sectors * SECTOR

    @property
    def size(self) -> int:
        return self.size_sectors * SECTOR

    @property
    def ext(self) -> str:
        return os.path.splitext(self.name)[1].lower().lstrip(".")

    @property
    def stem(self) -> str:
        return os.path.splitext(self.name)[0]


def parse_dir(data: bytes) -> list[DirEntry]:
    if len(data) % RECORD:
        raise FormatError(f".dir size {len(data)} is not a multiple of {RECORD}")
    out = []
    for i in range(len(data) // RECORD):
        off, size, raw = struct.unpack_from("<II24s", data, i * RECORD)
        nul = raw.find(b"\0")
        if nul < 0:
            nul = 24  # a name may fill all 24 bytes with no terminator (seen in World.dir)
        name = raw[:nul].decode("ascii")  # non-ASCII names would be a layout error
        if not name:
            raise FormatError(f".dir record {i}: empty name")
        out.append(DirEntry(i, name, off, size))
    return out


class ImgArchive:
    """Read-only view of an IMG/DIR pair. Never writes to the archive."""

    def __init__(self, img_path: str, dir_path: str | None = None):
        self.img_path = img_path
        self.dir_path = dir_path or os.path.splitext(img_path)[0] + ".dir"
        if not os.path.exists(self.dir_path):
            # The install ships World.dir next to World.img; tolerate case differences.
            folder, base = os.path.split(self.dir_path)
            for cand in os.listdir(folder or "."):
                if cand.lower() == base.lower():
                    self.dir_path = os.path.join(folder, cand)
                    break
        with open(self.dir_path, "rb") as f:
            self.entries = parse_dir(f.read())
        self.img_size = os.path.getsize(img_path)
        if self.img_size % SECTOR:
            raise FormatError(f"{img_path}: size {self.img_size} is not a multiple of {SECTOR}")
        self._by_name: dict[str, DirEntry] = {}
        self.duplicates: list[str] = []
        for e in self.entries:
            if e.offset + e.size > self.img_size:
                raise FormatError(f"{e.name}: entry runs past end of image")
            key = e.name.lower()
            if key in self._by_name:
                self.duplicates.append(e.name)  # first wins, like a linear lookup
                continue
            self._by_name[key] = e
        self._fh = open(img_path, "rb")

    def close(self) -> None:
        self._fh.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __len__(self) -> int:
        return len(self.entries)

    def find(self, name: str) -> DirEntry | None:
        return self._by_name.get(name.lower())

    def names(self, ext: str | None = None) -> list[str]:
        if ext is None:
            return [e.name for e in self.entries]
        ext = ext.lower().lstrip(".")
        return [e.name for e in self.entries if e.ext == ext]

    def read_entry(self, e: DirEntry) -> bytes:
        self._fh.seek(e.offset)
        data = self._fh.read(e.size)
        if len(data) != e.size:
            raise FormatError(f"{e.name}: short read {len(data)} of {e.size}")
        return data

    def read(self, name: str) -> bytes:
        e = self.find(name)
        if e is None:
            raise KeyError(name)
        return self.read_entry(e)

    def census(self) -> dict[str, int]:
        counts: dict[str, int] = {}
        for e in self.entries:
            counts[e.ext] = counts.get(e.ext, 0) + 1
        return dict(sorted(counts.items(), key=lambda kv: -kv[1]))

    def check_overlaps(self) -> list[tuple[str, str]]:
        """Pairs of entries whose sector ranges overlap (expected: none)."""
        ordered = sorted(self.entries, key=lambda e: e.offset_sectors)
        bad = []
        for a, b in zip(ordered, ordered[1:]):
            if a.offset_sectors + a.size_sectors > b.offset_sectors and a.size_sectors:
                bad.append((a.name, b.name))
        return bad
