# OWNER: builder-A (importers)
"""Gamebryo 20.3.0.9 NIF reader for Bully: Scholarship Edition.

Format authority: docs/re-notes/format-survey.md section 2 (header, endian
byte at 0x2B) and section 3 (.nft = NIF with NiSourceTexture/NiPixelData).
Block layouts follow the field order documented by bully-re (docs/nif.md,
docs/nft.md); this is an independent implementation.

Endianness: the version u32, the endian byte and the block COUNT are always
little-endian; from the block-type count onward every field follows the
endian byte (1 = little, 0 = big). Pixel bytes inside NiPixelData stay
little-endian even in big-endian files (they are raw GPU blocks).

Strictness: every decoded block must consume exactly the size given in the
header's block-size table. A block that does not is recorded with an error and
its data dropped, never half-used. Unknown block types are skipped by size.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .binreader import FormatError, Reader

MAGIC = b"Gamebryo File Format, Version 20.3.0.9"
VERSION = 0x14030009


@dataclass
class Block:
    index: int
    type_name: str
    offset: int
    size: int
    data: dict | None = None
    error: str | None = None


@dataclass
class NifFile:
    version: int
    big_endian: bool
    user_version: int
    block_types: list
    blocks: list
    strings: list
    groups: list
    data_end: int  # offset just past the footer
    trailing_nonzero: int = 0  # non-zero bytes after the footer (expected 0)
    roots: list = field(default_factory=list)

    def string(self, idx: int) -> str | None:
        if idx is None or idx < 0 or idx >= len(self.strings):
            return None
        return self.strings[idx]

    def block(self, ref: int) -> Block | None:
        if ref is None or ref < 0 or ref >= len(self.blocks):
            return None
        return self.blocks[ref]

    def data(self, ref: int, *types: str) -> dict | None:
        b = self.block(ref)
        if b is None or b.data is None:
            return None
        if types and b.type_name not in types:
            return None
        return b.data

    def of_type(self, *types: str) -> list:
        return [b for b in self.blocks if b.type_name in types]

    def type_counts(self) -> dict:
        c: dict[str, int] = {}
        for b in self.blocks:
            c[b.type_name] = c.get(b.type_name, 0) + 1
        return c

    def failed_blocks(self) -> list:
        return [b for b in self.blocks if b.error]


# ---------------------------------------------------------------------------
# Block decoders. Each takes a Reader bounded to the block and returns a dict.
# ---------------------------------------------------------------------------

def _object_net(r: Reader) -> dict:
    return {"name": r.i32(), "extra": r.refs(), "controller": r.i32()}


def _av_object(r: Reader) -> dict:
    d = _object_net(r)
    d["flags"] = r.u16()
    d["translation"] = r.vec3()
    d["rotation"] = r.array("f", 9)  # row-major 3x3, v' = R*(s*v) + t (survey 2.2)
    d["scale"] = r.f32()
    d["properties"] = r.refs()
    d["collision"] = r.i32()
    return d


def _node(r: Reader) -> dict:
    d = _av_object(r)
    d["children"] = r.refs()
    d["effects"] = r.refs()
    return d


def _geometry(r: Reader) -> dict:
    d = _av_object(r)
    d["data"] = r.i32()
    d["skin"] = r.i32()
    n_mat = r.u32()
    if n_mat > 1000:
        raise FormatError(f"material count {n_mat}")
    d["materials"] = [(r.i32(), r.i32()) for _ in range(n_mat)]  # (name idx, extra data)
    d["active_material"] = r.i32()
    d["dirty"] = r.u8()
    return d


def _geometry_data(r: Reader) -> dict:
    d = {"group_id": r.i32()}
    nv = r.u16()
    d["num_vertices"] = nv
    d["keep_flags"] = r.u8()
    d["compress_flags"] = r.u8()
    d["vertices"] = r.array("f", nv * 3) if r.u8() else None
    flags = r.u16()
    d["data_flags"] = flags
    n_uv = flags & 0x3F
    has_normals = r.u8()
    d["normals"] = r.array("f", nv * 3) if has_normals else None
    if has_normals and flags & 0x1000:  # tangents then bitangents follow the normals
        d["tangents"] = r.array("f", nv * 3)
        d["bitangents"] = r.array("f", nv * 3)
    d["center"] = r.vec3()
    d["radius"] = r.f32()
    d["colors"] = r.array("f", nv * 4) if r.u8() else None
    d["uv_sets"] = [r.array("f", nv * 2) for _ in range(n_uv)]
    d["consistency"] = r.u16()
    d["additional_data"] = r.i32()
    return d


def _tri_shape_data(r: Reader) -> dict:
    d = _geometry_data(r)
    nt = r.u16()
    num_points = r.u32()
    if num_points != nt * 3:
        raise FormatError(f"NiTriShapeData: points {num_points} != 3 x triangles {nt}")
    d["triangles"] = r.array("H", nt * 3) if r.u8() else ()
    n_groups = r.u16()
    for _ in range(n_groups):  # match groups: not needed for export
        r.array("H", r.u16())
    return d


def strips_to_triangles(strips) -> list:
    """Convert strips to a flat triangle index list, alternating winding,
    dropping degenerate triangles (two equal indices)."""
    out = []
    for s in strips:
        for i in range(2, len(s)):
            a, b, c = s[i - 2], s[i - 1], s[i]
            if a == b or b == c or a == c:
                continue
            if i & 1:
                out += (b, a, c)
            else:
                out += (a, b, c)
    return out


def _tri_strips_data(r: Reader) -> dict:
    d = _geometry_data(r)
    d["declared_triangles"] = r.u16()
    n_strips = r.u16()
    lengths = r.array("H", n_strips)
    strips = []
    if r.u8():
        for n in lengths:
            strips.append(r.array("H", n))
    d["strip_lengths"] = lengths
    d["triangles"] = tuple(strips_to_triangles(strips))
    return d


def _source_texture(r: Reader) -> dict:
    # Same layout for NiSourceTexture and NiSourceCubeMap. In .nft files
    # use_external = 0 and pixel_data points at the NiPixelData; in .nif files
    # the texture is external and pixel_data is -1 (bully-re docs/nft.md).
    d = _object_net(r)
    d["use_external"] = r.u8()
    d["file_name"] = r.i32()
    d["pixel_data"] = r.i32()
    d["pixel_layout"] = r.u32()
    d["use_mipmaps"] = r.u32()
    d["alpha_format"] = r.u32()
    d["is_static"] = r.u8()
    d["direct_render"] = r.u8()
    d["persist_render_data"] = r.u8()
    return d


def _pixel_data(r: Reader) -> dict:
    # Size identity: 79 + 12*mips + num_pixels*num_faces (survey 3.2)
    d = {
        "pixel_format": r.u32(),
        "bits_per_pixel": r.u8(),
        "renderer_hint": r.i32(),
        "extra_data": r.u32(),
        "flags": r.u8(),
        "tiling": r.u32(),
        "srgb": r.u8(),
    }
    d["channels"] = [
        {"type": r.u32(), "convention": r.u32(), "bits": r.u8(), "signed": r.u8()} for _ in range(4)
    ]
    d["palette"] = r.i32()
    n_mips = r.u32()
    if n_mips > 32:
        raise FormatError(f"NiPixelData: {n_mips} mipmaps")
    d["bytes_per_pixel"] = r.u32()
    d["mipmaps"] = [(r.u32(), r.u32(), r.u32()) for _ in range(n_mips)]  # width, height, offset
    d["num_pixels"] = r.u32()
    d["num_faces"] = r.u32()
    total = d["num_pixels"] * d["num_faces"]
    d["pixels"] = r.view(total)  # raw little-endian GPU bytes, never swapped
    return d


def _palette(r: Reader) -> dict:
    has_alpha = r.u8()
    n = r.u32()
    if n > 65536:
        raise FormatError(f"NiPalette: {n} entries")
    return {"has_alpha": has_alpha, "entries": r.bytes(n * 4)}


def _extra_data(r: Reader) -> dict:
    # NiStringExtraData: value is a string index; NiIntegerExtraData: an int.
    return {"name": r.i32(), "value": r.i32()}


def _material(r: Reader) -> dict:
    d = _object_net(r)
    d["ambient"] = r.vec3()
    d["diffuse"] = r.vec3()
    d["specular"] = r.vec3()
    d["emissive"] = r.vec3()
    d["glossiness"] = r.f32()
    d["alpha"] = r.f32()
    return d


TEXTURE_SLOTS = ("base", "dark", "detail", "gloss", "glow", "bump", "normal", "parallax", "decal")


def _tex_desc(r: Reader) -> dict:
    d = {"source": r.i32(), "flags": r.u16()}
    if r.u8():  # has texture transform
        d["transform"] = {
            "translation": r.array("f", 2),
            "scale": r.array("f", 2),
            "rotation": r.f32(),
            "method": r.u32(),
            "center": r.array("f", 2),
        }
    return d


def _texturing(r: Reader) -> dict:
    d = _object_net(r)
    d["flags"] = r.u16()
    count = r.u32()
    if count > 16:
        raise FormatError(f"NiTexturingProperty: {count} slots")
    slots = {}
    for i in range(count):
        if r.u8():
            desc = _tex_desc(r)
            if i == 5:  # bump: luma scale, luma offset, 2x2 matrix
                desc["bump"] = r.array("f", 6)
            if i == 7:  # parallax offset
                desc["parallax_offset"] = r.f32()
            slots[TEXTURE_SLOTS[i] if i < len(TEXTURE_SLOTS) else f"slot{i}"] = desc
    d["slots"] = slots
    n_shader = r.u32()
    if n_shader > 64:
        raise FormatError(f"NiTexturingProperty: {n_shader} shader textures")
    shader = []
    for _ in range(n_shader):
        if r.u8():
            desc = _tex_desc(r)
            desc["map_id"] = r.u32()
            shader.append(desc)
    d["shader_textures"] = shader
    return d


def _alpha(r: Reader) -> dict:
    d = _object_net(r)
    d["flags"] = r.u16()
    d["threshold"] = r.u8()
    return d


def _stencil(r: Reader) -> dict:
    d = _object_net(r)
    d["flags"] = r.u16()
    d["ref"] = r.u32()
    d["mask"] = r.u32()
    return d


def _flags_property(r: Reader) -> dict:
    # NiVertexColorProperty / NiZBufferProperty / NiSpecularProperty in
    # 20.3.0.9 are NiObjectNET + u16 flags (older-version fields are absent).
    # Validated by the exact-size rule on real files, not assumed.
    d = _object_net(r)
    d["flags"] = r.u16()
    return d


DECODERS = {
    "NiNode": _node,
    "NiTriShape": _geometry,
    "NiTriStrips": _geometry,
    "NiTriShapeData": _tri_shape_data,
    "NiTriStripsData": _tri_strips_data,
    "NiSourceTexture": _source_texture,
    "NiSourceCubeMap": _source_texture,
    "NiPixelData": _pixel_data,
    "NiPalette": _palette,
    "NiStringExtraData": _extra_data,
    "NiIntegerExtraData": _extra_data,
    "NiMaterialProperty": _material,
    "NiTexturingProperty": _texturing,
    "NiAlphaProperty": _alpha,
    "NiStencilProperty": _stencil,
    "NiVertexColorProperty": _flags_property,
    "NiZBufferProperty": _flags_property,
    "NiSpecularProperty": _flags_property,
}


def _decode(data, type_name: str, offset: int, size: int, big: bool):
    dec = DECODERS.get(type_name)
    if dec is None:
        return None, None
    try:
        r = Reader(data, offset, offset + size, big_endian=big)
        d = dec(r)
        if r.pos != r.end:
            return None, f"decoded {r.pos - offset} of {size} bytes"
        return d, None
    except FormatError as ex:
        return None, str(ex)


class TextModeCorruptionError(FormatError):
    """The file went through a LF -> CRLF text-mode conversion (survey OPEN-8)."""


def parse_nif(data, types: set | None = None) -> NifFile:
    """Parse a NIF/NFT byte buffer (may carry sector padding at the end).

    `types`: if given, only these block types are decoded; all others are
    skipped by size (the header's size table makes that safe).
    """
    if len(data) < len(MAGIC) + 14 or bytes(data[: len(MAGIC)]) != MAGIC:
        raise FormatError("not a Gamebryo 20.3.0.9 file (header line mismatch)")
    term = bytes(data[len(MAGIC):len(MAGIC) + 2])
    if term == b"\r\n":
        # Survey OPEN-8 (BBonusB, Barr01_Switch, BeerKeg, BirdBath .nft).
        # Finding: these files were passed through a unix2dos-style text
        # conversion. The header line ends in CRLF, and the files contain
        # zero bare LF bytes (BBonusB: 119 CRLF pairs, 0 bare LF), which
        # random DXT data cannot produce by chance. Pre-existing CRLF pairs
        # were apparently left alone (undoing every CRLF overshoots the next
        # block start by 3 bytes on BBonusB), so the inverse is ambiguous
        # inside opaque pixel payloads. We refuse rather than guess.
        raise TextModeCorruptionError(
            "header line ends in CRLF: file was LF->CRLF converted (OPEN-8); "
            "inverse is ambiguous, skipped")
    if term[:1] != b"\n":
        raise FormatError(f"header line terminator {term!r} is not LF")
    r = Reader(data, len(MAGIC) + 1)
    version = r.u32()
    if version != VERSION:
        raise FormatError(f"version 0x{version:08x} != 0x{VERSION:08x}")
    endian = r.u8()
    if endian not in (0, 1):
        raise FormatError(f"endian byte {endian} at 0x2B is neither 0 nor 1")
    user_version = r.u32()
    num_blocks = r.u32()  # always little-endian (survey 2.1, bully-re docs/nft.md)
    if num_blocks > 200000:
        raise FormatError(f"block count {num_blocks}")
    big = endian == 0
    r.e = ">" if big else "<"
    n_types = r.u16()
    if n_types > 1000:
        raise FormatError(f"type count {n_types}")
    type_names = [r.sized_string(256) for _ in range(n_types)]
    type_idx = r.array("H", num_blocks)
    for t in type_idx:
        if t >= n_types:
            raise FormatError(f"block type index {t} >= {n_types}")
    sizes = r.array("I", num_blocks)
    n_strings = r.u32()
    r.u32()  # max string length
    if n_strings > 200000:
        raise FormatError(f"string count {n_strings}")
    strings = [r.sized_string() for _ in range(n_strings)]
    n_groups = r.u32()
    groups = list(r.array("I", n_groups))

    blocks = []
    off = r.pos
    for i in range(num_blocks):
        tname = type_names[type_idx[i]]
        size = sizes[i]
        if off + size > len(data):
            raise FormatError(f"block {i} ({tname}) runs past end of data")
        b = Block(i, tname, off, size)
        if types is None or tname in types:
            b.data, b.error = _decode(data, tname, off, size, big)
        blocks.append(b)
        off += size
    # Footer: u32 root count + i32 root refs (standard Gamebryo footer), then
    # zero padding up to the sector boundary. Verified on all 5,724 World.img
    # .nif files (every one has exactly one root, block 0).
    fr = Reader(data, off, big_endian=big)
    n_roots = fr.u32()
    if n_roots > num_blocks:
        raise FormatError(f"footer root count {n_roots} > block count {num_blocks}")
    roots = list(fr.array("i", n_roots))
    tail = bytes(data[fr.pos:])
    trailing_nonzero = len(tail) - tail.count(0)
    return NifFile(version, big, user_version, type_names, blocks, strings, groups,
                   fr.pos, trailing_nonzero, roots)
