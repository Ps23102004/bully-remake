# OWNER: builder-A2 (animation)
"""Synthetic HXD / AGR / skinned-NIF builders in the documented layouts.
No game bytes are used or committed."""

from __future__ import annotations

import math
import struct

from fixtures import NifWriter


# ---------------------------------------------------------------------------
# AGR
# ---------------------------------------------------------------------------

def pack_1002(prev: int, t: float, q) -> bytes:
    """Inverse of the type-1002 decoder: q = (w, x, y, z), t in 0..1."""
    w, x, y, z = q
    m = lambda c: min(int(round(abs(c) * 1023)), 1023)
    s = lambda c: 1 if c < 0 else 0
    w0 = (prev & 0x7FF) | (int(round(t * 511)) & 0x1FF) << 11 | s(x) << 20 | m(x) << 21 | s(y) << 31
    w1 = m(y) | s(z) << 10 | m(z) << 11 | s(w) << 21 | m(w) << 22
    return struct.pack("<II", w0, w1)


def pack_cm(p) -> bytes:
    """Type-1004 translation word (centimetres, x/y 10 bits, z 9 bits)."""
    cm = [int(round(c * 100)) for c in p]
    v = (abs(cm[0]) & 0x3FF) | (cm[0] < 0) << 10 | (abs(cm[1]) & 0x3FF) << 11 | (cm[1] < 0) << 21
    v |= (abs(cm[2]) & 0x1FF) << 22 | (cm[2] < 0) << 31
    return struct.pack("<I", v)


def pack_key(kt: int, prev: int, t: float, q, p=(0.0, 0.0, 0.0)) -> bytes:
    if kt in (1002, 1004):
        b = pack_1002(prev, t, q)
        return b + pack_cm(p) if kt == 1004 else b
    head = struct.pack("<HH", prev, int(round(t * 65535)))
    if kt in (1001, 1003):
        w, x, y, z = q
        b = head + struct.pack("<4h", *(int(round(c * 32767)) for c in (x, y, z, w)))
        if kt == 1003:
            b += struct.pack("<3h", *(int(round(c * 1000)) for c in p)) + b"\0\0"
        return b
    b = head + struct.pack("<4f", *q)
    if kt == 999:
        b += struct.pack("<3f", *p)
    return b


def axis_quat(axis: int, deg: float):
    h = math.radians(deg) / 2
    v = [0.0, 0.0, 0.0]
    v[axis] = math.sin(h)
    return (math.cos(h), v[0], v[1], v[2])


def make_clip(kt: int, n_bones: int, frames, duration: float = 1.0, positions=()) -> bytes:
    """frames: list of (t, {bone: quat}) in time order; frame 0 must give
    every bone (track heads). positions: (key index, (x, y, z) metres)."""
    keys = []
    last = {}
    for fi, (t, rots) in enumerate(frames):
        for bone in range(n_bones) if fi == 0 else sorted(rots):
            prev = last.get(bone, 0)
            last[bone] = len(keys)
            keys.append((prev, t, rots[bone], (0.1 * bone, 0.0, 0.0)))
    body = b"".join(pack_key(kt, *k) for k in keys)
    pos = b"".join(struct.pack("<Hhhh", k, *(int(round(c * 1000)) for c in p)) for k, p in positions)
    return struct.pack("<IIIIf", 0x100, kt, len(keys), 0, duration) + body + pos


# ---------------------------------------------------------------------------
# HXD
# ---------------------------------------------------------------------------

def name(s: str, n: int) -> bytes:
    return s.encode("latin-1").ljust(n, b"\0")


def make_hxd(bones, anims, groups, version: float = 1.12) -> bytes:
    """anims: (name, duration, hash, agr_size, group[, events]); groups: (name, size)."""
    b = struct.pack("<fI", version, 7)
    b += struct.pack("<I", 1) + name("default", 32) + struct.pack("<If", len(bones), 0.3)
    b += struct.pack(f"<{len(bones)}f", *([1.0] * len(bones)))
    b += struct.pack("<I", len(bones)) + b"".join(name(x, 32) for x in bones)
    b += struct.pack("<I", len(anims))
    for a in anims:
        nm, dur, h, size, grp = a[:5]
        events = a[5] if len(a) > 5 else []
        b += struct.pack("<ff", dur, 0.3) + name(nm, 64) + struct.pack("<IIII", h, 0x4E, size, grp)
        b += struct.pack("<H", len(events))
        for x, y, kind in events:
            b += struct.pack("<III", x, y, kind) + (b"\0" * 40 if kind == 1 else b"")
        b += struct.pack("<4H", 1, 2, 3, 4) + b"\0" * 4
        if version > 1.09 + 1e-6:
            b += b"\0" * 4
            if version > 1.11 + 1e-6:
                b += b"\0" * 4
        b += struct.pack("<3f", 0.0, 0.0, 0.85)
    b += struct.pack("<I", len(groups))
    for g, size in groups:
        b += name(g, 32) + struct.pack("<I", size)
        if version >= 1.11 - 1e-6:
            b += b"\0" * 32 + struct.pack("<I", 0)
    b += b"\0" * (12 * len(bones)) + struct.pack("<I", 1) + b"\0" * 0x30 + struct.pack("<I", 0xABCD)
    return b


# ---------------------------------------------------------------------------
# Skinned NIF
# ---------------------------------------------------------------------------

def transform13(w: NifWriter, r=(1, 0, 0, 0, 1, 0, 0, 0, 1), t=(0, 0, 0), s=1.0) -> bytes:
    return w.p("9f", *r) + w.p("3f", *t) + w.p("f", s)


def make_skinned_nif(n_bones: int = 3, big_endian: bool = False):
    """Scene Root -> Dummy -> bone chain (each 1 m up +z) and ARROW-like
    leaf; plus 'Body' node -> one skinned 4-vertex shape weighted to bones
    1 and 2. Returns (bytes, expected info)."""
    w = NifWriter(big_endian)
    root = w.add("NiNode", b"")
    dummy = w.add("NiNode", b"")
    bones = [w.add("NiNode", b"") for _ in range(n_bones)]
    body = w.add("NiNode", b"")
    shape = w.add("NiTriShape", b"")
    verts = [(0, 0, 1.0), (0.2, 0, 1.0), (0, 0, 2.0), (0.2, 0, 2.0)]
    data = w.add("NiTriShapeData", w.tri_shape_data(verts, [(0, 1, 2), (1, 3, 2)], normals=[(0, -1, 0)] * 4,
                                                    uvs=[[(0, 0), (1, 0), (0, 1), (1, 1)]]))
    skin = w.add("NiSkinInstance", b"")
    sdata = w.add("NiSkinData", b"")
    part = w.add("NiSkinPartition", b"")
    w.blocks[root] = ("NiNode", w.node(w.s("Scene Root"), [dummy, body]))
    w.blocks[dummy] = ("NiNode", w.node(w.s("Dummy"), [bones[0]]))
    for i, b in enumerate(bones):
        kids = [bones[i + 1]] if i + 1 < n_bones else []
        t = (0.0, 0.0, 0.0) if i == 0 else (0.0, 0.0, 1.0)
        w.blocks[b] = ("NiNode", w.node(w.s(f"Bone{i}"), kids, t=t))
    w.blocks[body] = ("NiNode", w.node(w.s("Body"), [shape]))
    w.blocks[shape] = ("NiTriShape", w.av(w.s("Body:0")) + w.p("iiI", data, skin, 0) + w.p("i", -1) + b"\x00")
    used = [bones[1], bones[2]]
    w.blocks[skin] = ("NiSkinInstance", w.p("iii", sdata, part, root) + w.p("I", 2) + w.p("2i", *used))
    # S_b = inverse(bone world): bone1 at z=1, bone2 at z=2
    sd = transform13(w) + w.p("I", 2) + b"\x01"
    for z, wts in ((1.0, [(0, 1.0), (1, 1.0), (2, 0.25), (3, 0.0)]), (2.0, [(2, 0.75), (3, 1.0)])):
        sd += transform13(w, t=(0, 0, -z)) + w.p("3f", 0, 0, 0) + w.p("f", 1.0)
        sd += w.p("H", len(wts)) + b"".join(w.p("Hf", i, x) for i, x in wts)
    w.blocks[sdata] = ("NiSkinData", sd)
    pp = w.p("I", 1) + w.p("5H", 4, 2, 2, 0, 2) + w.p("2H", 0, 1) + b"\x01" + w.p("4H", 0, 1, 2, 3)
    pp += b"\x01" + w.p("8f", 1, 0, 1, 0, .25, .75, 0, 1) + b"\x01" + w.p("6H", 0, 1, 2, 1, 3, 2)
    pp += b"\x01" + bytes([0, 1, 0, 1, 0, 1, 1, 0])
    w.blocks[part] = ("NiSkinPartition", pp)
    return w.build(), {"bones": used, "shape": shape}
