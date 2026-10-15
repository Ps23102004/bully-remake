# OWNER: builder-A (importers)
"""Synthetic fixtures: tiny IMG/DIR, NIF, NFT, IPB and IDE files built from
scratch in the documented layouts. No game bytes are used or committed."""

from __future__ import annotations

import os
import struct

MAGIC = b"Gamebryo File Format, Version 20.3.0.9"


class NifWriter:
    """Builds a 20.3.0.9 NIF. Block payloads are written with the file's
    endianness (`e` is '<' or '>')."""

    def __init__(self, big_endian: bool = False):
        self.e = ">" if big_endian else "<"
        self.big = big_endian
        self.blocks = []  # (type, payload)
        self.strings = []

    def s(self, text: str) -> int:
        if text not in self.strings:
            self.strings.append(text)
        return self.strings.index(text)

    def p(self, fmt: str, *vals) -> bytes:
        return struct.pack(self.e + fmt, *vals)

    def add(self, type_name: str, payload: bytes) -> int:
        self.blocks.append((type_name, payload))
        return len(self.blocks) - 1

    # -- payload helpers ------------------------------------------------------
    def object_net(self, name: int = -1, extra=(), controller: int = -1) -> bytes:
        return self.p("iI", name, len(extra)) + b"".join(self.p("i", x) for x in extra) + self.p("i", controller)

    def av(self, name, flags=0x10, t=(0, 0, 0), r=(1, 0, 0, 0, 1, 0, 0, 0, 1), scale=1.0, props=()) -> bytes:
        return (self.object_net(name) + self.p("H", flags) + self.p("3f", *t) + self.p("9f", *r)
                + self.p("f", scale) + self.p("I", len(props)) + b"".join(self.p("i", x) for x in props)
                + self.p("i", -1))

    def node(self, name, children=(), **kw) -> bytes:
        return (self.av(name, **kw) + self.p("I", len(children)) + b"".join(self.p("i", c) for c in children)
                + self.p("I", 0))

    def geometry(self, name, data_ref, **kw) -> bytes:
        return self.av(name, **kw) + self.p("iiI", data_ref, -1, 0) + self.p("i", -1) + b"\x00"

    def geometry_data(self, verts, normals=None, colors=None, uvs=()) -> bytes:
        nv = len(verts)
        b = self.p("iH", 0, nv) + b"\x00\x00"
        b += b"\x01" + b"".join(self.p("3f", *v) for v in verts)
        b += self.p("H", len(uvs) & 0x3F)
        if normals:
            b += b"\x01" + b"".join(self.p("3f", *n) for n in normals)
        else:
            b += b"\x00"
        b += self.p("3f", 0, 0, 0) + self.p("f", 1.0)
        if colors:
            b += b"\x01" + b"".join(self.p("4f", *c) for c in colors)
        else:
            b += b"\x00"
        for uvset in uvs:
            b += b"".join(self.p("2f", *uv) for uv in uvset)
        b += self.p("H", 0) + self.p("i", -1)
        return b

    def tri_shape_data(self, verts, tris, **kw) -> bytes:
        b = self.geometry_data(verts, **kw)
        flat = [i for t in tris for i in t]
        b += self.p("HI", len(tris), len(flat)) + b"\x01" + b"".join(self.p("H", i) for i in flat)
        return b + self.p("H", 0)

    def tri_strips_data(self, verts, strips, **kw) -> bytes:
        b = self.geometry_data(verts, **kw)
        b += self.p("HH", 0, len(strips)) + b"".join(self.p("H", len(s)) for s in strips)
        b += b"\x01" + b"".join(self.p("H", i) for s in strips for i in s)
        return b

    def source_texture(self, file_name: int, pixel_ref: int = -1, external: int = 1, extra=()) -> bytes:
        return (self.object_net(-1, extra) + bytes([external]) + self.p("ii", file_name, pixel_ref)
                + self.p("III", 0, 1, 3) + b"\x01\x00\x00")

    def texturing(self, base_ref: int, flags: int = 0x3200, normal_ref: int | None = None) -> bytes:
        b = self.object_net() + self.p("H", 0) + self.p("I", 9)
        for slot in range(9):
            ref = base_ref if slot == 0 else (normal_ref if slot == 6 else None)
            if ref is None:
                b += b"\x00"
            else:
                b += b"\x01" + self.p("iH", ref, flags) + b"\x00"
        return b + self.p("I", 0)

    def material(self, diffuse=(1, 1, 1), alpha=1.0, name=-1) -> bytes:
        return (self.object_net(name) + self.p("3f", .5, .5, .5) + self.p("3f", *diffuse) + self.p("3f", 0, 0, 0)
                + self.p("3f", 0, 0, 0) + self.p("ff", 10.0, alpha))

    def alpha(self, flags: int, threshold: int) -> bytes:
        return self.object_net() + self.p("H", flags) + bytes([threshold])

    def stencil(self, flags: int = 0x4D80) -> bytes:
        return self.object_net() + self.p("HII", flags, 0, 0xFFFFFFFF)

    def flags_prop(self, flags: int) -> bytes:
        return self.object_net() + self.p("H", flags)

    def pixel_data(self, fmt: int, mips, pixels: bytes, palette: int = -1, faces: int = 1,
                   channel_types=(4, 19, 19, 19)) -> bytes:
        b = self.p("I", fmt) + b"\x00" + self.p("iI", -1, 0) + b"\x00" + self.p("I", 0) + b"\x00"
        for t in channel_types:
            b += self.p("II", t, 5) + b"\x08\x00"
        b += self.p("i", palette) + self.p("I", len(mips)) + self.p("I", 0)
        for w, h, off in mips:
            b += self.p("III", w, h, off)
        b += self.p("II", len(pixels) // faces, faces)
        return b + pixels  # raw bytes: never byte-swapped

    def palette(self, entries: bytes, has_alpha: int = 0) -> bytes:
        return bytes([has_alpha]) + self.p("I", len(entries) // 4) + entries

    def string_extra(self, name: int, value: int) -> bytes:
        return self.p("ii", name, value)

    # -- file ---------------------------------------------------------------
    def build(self, roots=(0,), pad_to: int | None = 2048, crlf: bool = False) -> bytes:
        types = []
        for t, _ in self.blocks:
            if t not in types:
                types.append(t)
        h = MAGIC + (b"\r\n" if crlf else b"\n")
        h += struct.pack("<I", 0x14030009) + bytes([0 if self.big else 1]) + struct.pack("<I", 0)
        h += struct.pack("<I", len(self.blocks))  # block count: always little-endian
        h += self.p("H", len(types))
        for t in types:
            h += self.p("I", len(t)) + t.encode()
        h += b"".join(self.p("H", types.index(t)) for t, _ in self.blocks)
        h += b"".join(self.p("I", len(pl)) for _, pl in self.blocks)
        h += self.p("II", len(self.strings), max([len(x) for x in self.strings] + [0]))
        for st in self.strings:
            h += self.p("I", len(st)) + st.encode("latin-1")
        h += self.p("I", 0)  # groups
        body = b"".join(pl for _, pl in self.blocks)
        foot = self.p("I", len(roots)) + b"".join(self.p("i", r) for r in roots)
        data = h + body + foot
        if pad_to:
            data += b"\0" * (-len(data) % pad_to)
        return data


def make_quad_nif(big_endian=False, model="TestBox", tex="Wall_d.tga", model_t=(5.0, 6.0, 7.0),
                  child_t=(1.0, 0.0, 0.0), strips=False, alpha_flags=None, stencil=False) -> bytes:
    """Scene Root -> model node (translated, must be ignored in entity space)
    -> child node (translated by child_t) -> one quad shape."""
    w = NifWriter(big_endian)
    w.add("NiNode", b"")          # 0 root (filled below)
    w.add("NiNode", b"")          # 1 model node
    w.add("NiNode", b"")          # 2 child node
    shape_type = "NiTriStrips" if strips else "NiTriShape"
    w.add(shape_type, b"")        # 3 shape
    verts = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    normals = [(0, 0, 1)] * 4
    uvs = [[(0, 0), (1, 0), (1, 1), (0, 1)]]
    colors = [(1, 0.5, 0.25, 1)] * 4
    if strips:
        w.add("NiTriStripsData", w.tri_strips_data(verts, [[0, 1, 3, 2]], normals=normals, colors=colors, uvs=uvs))
    else:
        w.add("NiTriShapeData", w.tri_shape_data(verts, [(0, 1, 2), (0, 2, 3)], normals=normals, colors=colors, uvs=uvs))
    src = w.add("NiSourceTexture", w.source_texture(w.s(tex)))           # 5
    texp = w.add("NiTexturingProperty", w.texturing(src))                # 6
    mat = w.add("NiMaterialProperty", w.material(name=w.s("mat0")))      # 7
    vcp = w.add("NiVertexColorProperty", w.flags_prop(0x28))             # 8 (AMB_DIF vertex mode)
    props = [texp, mat, vcp]
    if alpha_flags is not None:
        props.append(w.add("NiAlphaProperty", w.alpha(alpha_flags, 128)))
    if stencil:
        props.append(w.add("NiStencilProperty", w.stencil()))
    w.add("NiUnknownThing", b"\x01\x02\x03")  # unknown type: skipped by size
    zb = w.add("NiZBufferProperty", w.flags_prop(15))
    w.blocks[0] = ("NiNode", w.node(w.s("Scene Root"), [1], props=[zb]))
    w.blocks[1] = ("NiNode", w.node(w.s(model), [2], t=model_t))
    w.blocks[2] = ("NiNode", w.node(w.s("child"), [3], t=child_t))
    w.blocks[3] = (shape_type, w.geometry(w.s(model + ":0"), 4, props=props))
    return w.build()


def dxt1_block(c0: int, c1: int, idx: int) -> bytes:
    return struct.pack("<HHI", c0, c1, idx)


def make_nft(big_endian=False, textures=None) -> bytes:
    """textures: list of (tga_path, fmt, w, h, pixel_bytes[, palette_bytes])."""
    w = NifWriter(big_endian)
    textures = textures or [("Z:\\Bully\\Temp\\Export\\Textures\\Wall_d.tga", 4, 4, 4,
                             dxt1_block(0xF800, 0x001F, 0x00000000))]
    fname = w.s("Filename")
    for t in textures:
        path, fmt, wd, ht, px = t[:5]
        pal = t[5] if len(t) > 5 else None
        base = len(w.blocks)
        pal_ref = -1
        n_extra = 1
        src_i = w.add("NiSourceTexture", b"")
        w.add("NiStringExtraData", w.string_extra(fname, w.s(path)))
        if pal is not None:
            pal_ref = w.add("NiPalette", w.palette(pal))
        chans = (4, 19, 19, 19) if fmt in (4, 5, 6) else ((0, 1, 2, 19) if fmt == 0 else
                                                          ((2, 1, 0, 3) if fmt == 1 else (16, 19, 19, 19)))
        pix_i = w.add("NiPixelData", w.pixel_data(fmt, [(wd, ht, 0)], px, palette=pal_ref, channel_types=chans))
        w.blocks[src_i] = ("NiSourceTexture", w.source_texture(w.s(path), pix_i, external=0,
                                                               extra=[base + 1][:n_extra]))
    return w.build(roots=(0,))


def make_ipb(placements, extra_sections=True, tail=b"") -> bytes:
    """placements: list of (model_id, name, pos, scale, quat)."""
    out = b"Ipl$" + bytes(range(15))
    sections = []
    inst = b""
    for mid, name, pos, scale, quat in placements:
        inst += struct.pack("<i64sf3f3f4fiI", mid, name.encode(), 2.0, *pos, *scale, *quat, 0, 1234)
    sections.append(b"inst" + struct.pack("<I", len(placements)) + inst)
    if extra_sections:
        sections.append(b"spec" + struct.pack("<I", 1) + b"\x07" * 128)
        rail = b"\x04rail" + struct.pack("<iI", 0, 2) + b"\x01" * 24
        sections.append(b"rail" + struct.pack("<I", 1) + rail)
        sections.append(b"prop" + struct.pack("<I", 2) + b"\x03abc\x02xy")
    out += struct.pack("<I", len(sections)) + b"".join(sections) + tail
    return out + b"\0" * (-len(out) % 2048)


def make_img(folder: str, stem: str, files: dict) -> str:
    """Write <stem>.img/.dir with the given {name: bytes}; returns img path."""
    img = bytearray()
    d = bytearray()
    for name, data in files.items():
        data = bytes(data) + b"\0" * (-len(data) % 2048)
        d += struct.pack("<II24s", len(img) // 2048, len(data) // 2048, name.encode())
        img += data
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, stem + ".img"), "wb") as f:
        f.write(img)
    with open(os.path.join(folder, stem + ".dir"), "wb") as f:
        f.write(d)
    return os.path.join(folder, stem + ".img")
