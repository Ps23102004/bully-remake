# OWNER: builder-A (importers)
"""Bounded binary reader shared by every format parser.

Every read is bounds-checked. Running past the end raises FormatError instead
of returning garbage, so a wrong layout assumption fails loudly at the first
block where it is wrong.
"""

from __future__ import annotations

import struct


class FormatError(ValueError):
    """Raised when bytes do not match the layout we expect."""


class Reader:
    __slots__ = ("data", "pos", "end", "e")

    def __init__(self, data, pos: int = 0, end: int | None = None, big_endian: bool = False):
        self.data = data
        self.pos = pos
        self.end = len(data) if end is None else end
        if self.end > len(data) or pos > self.end:
            raise FormatError(f"reader window {pos}..{self.end} outside buffer of {len(data)} bytes")
        self.e = ">" if big_endian else "<"

    # -- primitives -------------------------------------------------------
    def need(self, n: int) -> None:
        if n < 0 or self.pos + n > self.end:
            raise FormatError(f"read of {n} bytes at {self.pos} runs past end {self.end}")

    def remaining(self) -> int:
        return self.end - self.pos

    def skip(self, n: int) -> None:
        self.need(n)
        self.pos += n

    def bytes(self, n: int) -> bytes:
        self.need(n)
        b = bytes(self.data[self.pos:self.pos + n])
        self.pos += n
        return b

    def view(self, n: int) -> memoryview:
        self.need(n)
        v = memoryview(self.data)[self.pos:self.pos + n]
        self.pos += n
        return v

    def _one(self, fmt: str, size: int):
        self.need(size)
        v = struct.unpack_from(self.e + fmt, self.data, self.pos)[0]
        self.pos += size
        return v

    def u8(self) -> int:
        self.need(1)
        v = self.data[self.pos]
        self.pos += 1
        return v

    def u16(self) -> int:
        return self._one("H", 2)

    def i16(self) -> int:
        return self._one("h", 2)

    def u32(self) -> int:
        return self._one("I", 4)

    def i32(self) -> int:
        return self._one("i", 4)

    def f32(self) -> float:
        return self._one("f", 4)

    def array(self, fmt: str, count: int) -> tuple:
        """Read `count` items of struct code `fmt` (one letter) in bulk."""
        if count < 0:
            raise FormatError(f"negative array count {count}")
        size = struct.calcsize("<" + fmt) * count
        self.need(size)
        v = struct.unpack_from(f"{self.e}{count}{fmt}", self.data, self.pos)
        self.pos += size
        return v

    def vec3(self) -> tuple:
        return self.array("f", 3)

    def sized_string(self, limit: int = 1 << 16) -> str:
        """u32 length + bytes (NIF header style)."""
        n = self.u32()
        if n > limit:
            raise FormatError(f"string length {n} exceeds limit {limit}")
        return self.bytes(n).decode("latin-1")

    def refs(self, limit: int = 100000) -> list:
        """u32 count + that many i32 block references."""
        n = self.u32()
        if n > limit:
            raise FormatError(f"ref count {n} exceeds limit {limit}")
        return list(self.array("i", n))
