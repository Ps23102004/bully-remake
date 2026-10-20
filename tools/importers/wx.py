# OWNER: lane M (materials + text)
"""Bully: Anniversary Edition 'Wx' text cipher (.mtl, .xml, .eff, .wdb in data_1.zip).

Layout: the tag 'Wx', then the ciphertext bytes written as base32 with the game's own 32-symbol alphabet
(5 bits per symbol, MSB first). Each ciphertext byte c[i] is turned back into plaintext with a byte keystream:

    r = seed;  for i: r = (171 * (r % 177) - 2 * (r // 177)) mod 2^32
                      plain[i] = ((c[i] ^ (6*i + 18)) + r) & 0xFF

(Wichmann-Hill generator 1 without its +30269 fix-up). The alphabet and routine are in the game executable; the seed is
one of many equivalent 32-bit values found by brute force against known plaintext, so no key string is needed.
Evidence and the executable addresses: docs/re-notes/ae-story.md section 3. All 6,931 Wx files in data_1.zip decode to
UTF-8 with this module.

`encode` is the exact inverse; it exists for synthetic test fixtures (tests must not contain game text).
"""

from __future__ import annotations

from .binreader import FormatError

TAG = b"Wx"
ALPHABET = b"6Ev2GlK1sWoCa5MfQ0pj43DH8Rzi9UnX"
SEED = 0x0CDFC798
_REV = {c: i for i, c in enumerate(ALPHABET)}


def is_wx(raw: bytes) -> bool:
    return raw[:2] == TAG


def _keystream(n: int, seed: int):
    r = seed
    for _ in range(n):
        r = (171 * (r % 177) - 2 * (r // 177)) & 0xFFFFFFFF
        yield r


def decode(raw: bytes, seed: int = SEED) -> bytes:
    """Wx file bytes -> plaintext bytes. Trailing NUL/CR/LF are not part of the cipher text."""
    raw = bytes(raw).rstrip(b"\0\r\n")
    if raw[:2] != TAG:
        raise FormatError("not a Wx file (missing 'Wx' tag)")
    cipher = bytearray()
    acc = bits = 0
    for ch in raw[2:]:
        v = _REV.get(ch)
        if v is None:
            raise FormatError(f"Wx: byte {ch:#04x} is not in the cipher alphabet")
        acc = ((acc << 5) | v) & 0xFFFF
        bits += 5
        if bits >= 8:
            bits -= 8
            cipher.append((acc >> bits) & 0xFF)
    return bytes(((c ^ ((6 * i + 18) & 0xFF)) + r) & 0xFF
                 for i, (c, r) in enumerate(zip(cipher, _keystream(len(cipher), seed))))


def decode_text(raw: bytes, seed: int = SEED) -> str:
    try:
        return decode(raw, seed).decode("utf-8")
    except UnicodeDecodeError as ex:
        raise FormatError(f"Wx: plaintext is not UTF-8: {ex}") from None


def encode(plain: bytes, seed: int = SEED) -> bytes:
    """Inverse of decode (test fixtures only)."""
    cipher = bytes(((p - r) & 0xFF) ^ ((6 * i + 18) & 0xFF)
                   for i, (p, r) in enumerate(zip(plain, _keystream(len(plain), seed))))
    out = bytearray(TAG)
    acc = bits = 0
    for c in cipher:
        acc = (acc << 8) | c
        bits += 8
        while bits >= 5:
            bits -= 5
            out.append(ALPHABET[(acc >> bits) & 31])
        acc &= (1 << bits) - 1
    if bits:
        out.append(ALPHABET[(acc << (5 - bits)) & 31])
    return bytes(out)
