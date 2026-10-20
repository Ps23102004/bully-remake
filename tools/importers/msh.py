# OWNER: builder-A (importers)
"""Bully Anniversary Edition `.msh` model reader (WarEngine, version 1337).

Format notes and evidence: docs/re-notes/ae-msh.md. Every layout rule here was
checked against all 5,714 `.msh` files in the AE `data_1.zip`.

All integers little-endian, no alignment padding. Positions are game space
(right-handed, +Z up, metres), as in the PC NIFs; convert with
coords.game_to_gltf. Triangles are lists of u16 indices.

Header (u32 unless noted):
    version (6..12), 2, 1337, 24, 1, mesh_offset (absolute)
    material count, then per material: u32 length + ASCII name
    bone count, then per bone: name, 12 f32 (3x4 row-major inverse bind
        matrix, translation in column 3), i32 parent/bone id (-1 = none)
    flag bytes: version 6:1, 7:2, 8:3, 9..11:4, 12:5 (meaning OPEN)
    bounds: 3 f32 min, 3 f32 max, 3 f32 sphere centre, f32 radius
    u32 length + property text "{physical(...)=,setups(...)=[0]}"
    ... node hierarchy / collision setups (skipped: mesh_offset is absolute)
At mesh_offset:
    part count, then per part:
        name; 2 bytes (always 0); version >= 10: 3 f32 pivot (usually 0; the
        header bounds are the bounds of position - pivot); version >= 11:
        4 more bytes (always 0)
        vertex count, index count, u16 x index count
        element count, per element: u32 type, u32 semantic, u8 usage index, u8 (always 1)
        vertex count x stride bytes (interleaved, stride = sum of type sizes)
        submesh count, per submesh: u32 first index, u32 index count, u32 material, u32 (always 0)
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .binreader import FormatError, Reader

WAR_VERSION = 1337
VERSIONS = range(6, 13)
_FLAG_BYTES = {6: 1, 7: 2, 8: 3, 9: 4, 10: 4, 11: 4, 12: 5}

# element type -> (byte size, numpy dtype, components)
TYPES = {
    1: (8, "<f4", 2),   # float2
    2: (12, "<f4", 3),  # float3
    8: (4, "u1", 4),    # ubyte4 (blend indices; semantic 6)
    9: (4, "u1", 4),    # ubyte4 normalized (blend weights, sum 255)
    10: (4, "i1", 4),   # byte4 signed normalized (normal, tangent), w unused
    11: (4, "<i2", 2),  # short2, value / 2048 (texcoords)
}
POSITION, BLENDWEIGHT, BLENDINDICES, NORMAL, TEXCOORD, TANGENT, EXTRA6, BINORMAL = range(8)
UV_SCALE = 1.0 / 2048.0
_LIMIT = 1 << 24


@dataclass
class MshBone:
    name: str
    matrix: list   # 3 rows of 4 floats (inverse bind, game space)
    parent: int    # raw i32; -1 = none (OPEN: not always an index into this list)


@dataclass
class MshSubmesh:
    part: str
    part_index: int
    material: int
    material_name: str | None
    positions: np.ndarray          # (n, 3) float32, game space
    indices: np.ndarray            # (3t,) uint32 into this submesh's vertices
    normals: np.ndarray | None = None
    tangents: np.ndarray | None = None
    binormals: np.ndarray | None = None
    uv0: np.ndarray | None = None
    uv1: np.ndarray | None = None
    uv2: np.ndarray | None = None
    joints: np.ndarray | None = None    # (n, 4) uint8, raw bone indices
    weights: np.ndarray | None = None   # (n, 4) float32, sum 1
    extra6: np.ndarray | None = None    # (n, 4) uint8, semantic 6 raw (constant per mesh; OPEN)


@dataclass
class MshModel:
    version: int
    materials: list
    bones: list
    flags: bytes
    bounds: dict
    properties: str
    parts: list = field(default_factory=list)  # per part: name, file offset, counts, decl, pivot
    submeshes: list = field(default_factory=list)
    warnings: list = field(default_factory=list)

    @property
    def skinned(self) -> bool:
        return any(s.weights is not None for s in self.submeshes)


def _string(r: Reader) -> str:
    return r.sized_string(4096)


def parse_msh(data: bytes) -> MshModel:
    if len(data) < 28:
        raise FormatError(f"{len(data)} bytes: too short for an .msh header")
    r = Reader(data)
    version, two, war, hsize, one, mesh_off = r.array("I", 6)
    if war != WAR_VERSION or two != 2 or hsize != 24 or one != 1:
        raise FormatError(f"not a WarEngine mesh: header {version, two, war, hsize, one}")
    if version not in VERSIONS:
        raise FormatError(f"unsupported .msh version {version}")
    materials = [_string(r) for _ in range(_count(r, 4096, "material"))]
    bones = []
    for _ in range(_count(r, 1024, "bone")):
        name = _string(r)
        m = r.array("f", 12)
        bones.append(MshBone(name, [list(m[0:4]), list(m[4:8]), list(m[8:12])], r.i32()))
    flags = r.bytes(_FLAG_BYTES[version])
    b = r.array("f", 10)
    bounds = {"min": list(b[0:3]), "max": list(b[3:6]), "center": list(b[6:9]), "radius": b[9]}
    properties = _string(r)
    if mesh_off < r.pos or mesh_off >= len(data):
        raise FormatError(f"mesh offset {mesh_off} outside {r.pos}..{len(data)}")
    model = MshModel(version, materials, bones, flags, bounds, properties)
    r.pos = mesh_off
    for pi in range(_count(r, 4096, "part")):
        _part(r, model, pi)
    if r.remaining():
        model.warnings.append(f"{r.remaining()} trailing bytes after the last part")
    return model


def _count(r: Reader, limit: int, what: str) -> int:
    n = r.u32()
    if n > limit:
        raise FormatError(f"{what} count {n} exceeds {limit}")
    return n


def _part(r: Reader, model: MshModel, pi: int) -> None:
    start = r.pos
    name = _string(r)
    r.skip(2)
    pivot = list(r.array("f", 3)) if model.version >= 10 else [0.0, 0.0, 0.0]
    if model.version >= 11:
        r.skip(4)
    nv, ni = r.u32(), r.u32()
    if nv > 0xFFFF + 1 or ni > _LIMIT:
        raise FormatError(f"part {name!r}: {nv} vertices / {ni} indices out of range")
    idx = np.frombuffer(r.bytes(2 * ni), "<u2").astype(np.uint32)
    if ni and int(idx.max()) >= nv:
        raise FormatError(f"part {name!r}: index {int(idx.max())} >= {nv} vertices")
    decl = []
    for _ in range(_count(r, 16, "vertex element")):
        t, sem = r.u32(), r.u32()
        decl.append((t, sem, r.u8(), r.u8()))
        if t not in TYPES:
            raise FormatError(f"part {name!r}: unknown vertex element type {t} (semantic {sem})")
    stride = sum(TYPES[t][0] for t, *_ in decl)
    raw = np.frombuffer(r.bytes(nv * stride), np.uint8).reshape(nv, stride)
    attrs = {}
    col = 0
    for t, sem, usage, _one in decl:
        size, dtype, comps = TYPES[t]
        a = np.ascontiguousarray(raw[:, col:col + size]).view(dtype).reshape(nv, comps)
        col += size
        if (sem, usage) in attrs:
            raise FormatError(f"part {name!r}: duplicate vertex element {sem}/{usage}")
        attrs[(sem, usage)] = (t, a)
    if (POSITION, 0) not in attrs or attrs[(POSITION, 0)][0] != 2:
        raise FormatError(f"part {name!r}: no float3 position element")
    subs = [r.array("I", 4) for _ in range(_count(r, 4096, "submesh"))]
    model.parts.append({"name": name, "offset": start, "vertices": nv, "indices": ni,
                        "decl": [list(d[:3]) for d in decl], "pivot": pivot})
    dec = _decode(attrs, name, model.warnings)
    for first, count, mat, unk in subs:
        if first + count > ni or count % 3:
            raise FormatError(f"part {name!r}: submesh {first}+{count} not a triangle range of {ni} indices")
        if mat >= len(model.materials):
            raise FormatError(f"part {name!r}: material {mat} >= {len(model.materials)}")
        if unk:
            model.warnings.append(f"part {name!r}: submesh field 4 = {unk}")
        if count == 0:
            continue
        used, local = np.unique(idx[first:first + count], return_inverse=True)
        sm = MshSubmesh(name, pi, mat, model.materials[mat], dec["positions"][used],
                        local.astype(np.uint32))
        for k, v in dec.items():
            if k != "positions" and v is not None:
                setattr(sm, k, v[used])
        model.submeshes.append(sm)


def _unit(v: np.ndarray) -> np.ndarray:
    """Normalise rows; zero rows stay zero (some float3 tangents are 0)."""
    n = np.linalg.norm(v, axis=1, keepdims=True)
    return (v / np.where(n > 1e-12, n, 1.0)).astype(np.float32)


def _finite(a: np.ndarray, label: str, part: str, warnings: list) -> np.ndarray:
    bad = ~np.isfinite(a) | (np.abs(a) > 1e6)
    if bad.any():  # sch_cafeworktble uv0 (NaN, 1e34): the exporter fault the PC NIF has too
        warnings.append(f"part {part!r}: {int(bad.sum())} non-finite/absurd {label} values zeroed")
        a = np.where(bad, np.float32(0), a)
    return a


def _decode(attrs: dict, name: str, warnings: list) -> dict:
    def vec(sem, usage=0):
        if (sem, usage) not in attrs:
            return None
        t, a = attrs[(sem, usage)]
        if t == 10:
            return _unit(a[:, :3].astype(np.float32) / 127.0)
        if t == 2:
            return _unit(_finite(a.astype(np.float32), f"semantic {sem}", name, warnings))
        raise FormatError(f"part {name!r}: semantic {sem} with type {t}")

    def uv(usage):
        if (TEXCOORD, usage) not in attrs:
            return None
        t, a = attrs[(TEXCOORD, usage)]
        if t == 11:
            return a.astype(np.float32) * UV_SCALE
        if t == 1:
            return _finite(a.astype(np.float32), f"uv{usage}", name, warnings)
        raise FormatError(f"part {name!r}: texcoord with type {t}")

    pos = _finite(attrs[(POSITION, 0)][1].astype(np.float32), "position", name, warnings)
    nrm = vec(NORMAL)
    if nrm is not None and not nrm.any(axis=1).all():  # e.g. dl_waterinas_a: all zero
        warnings.append(f"part {name!r}: {int((~nrm.any(axis=1)).sum())} zero-length normals")
    out = {"positions": pos, "normals": nrm, "tangents": vec(TANGENT), "binormals": vec(BINORMAL),
           "uv0": uv(0), "uv1": uv(1), "uv2": uv(2), "joints": None, "weights": None,
           "extra6": attrs[(EXTRA6, 0)][1] if (EXTRA6, 0) in attrs else None}
    if (BLENDWEIGHT, 0) in attrs:
        w = attrs[(BLENDWEIGHT, 0)][1].astype(np.float32)
        s = w.sum(axis=1, keepdims=True)
        out["weights"] = np.where(s > 0, w / np.maximum(s, 1), np.array([1.0, 0, 0, 0], np.float32))
        j = attrs.get((BLENDINDICES, 0))
        out["joints"] = j[1] if j else np.zeros((len(w), 4), np.uint8)
    return out
