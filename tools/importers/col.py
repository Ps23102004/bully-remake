# OWNER: builder-A (importers)
"""Bully COL3 collision (Stream/World.img `<scene>.col`) -> collision-only glTF.

The render meshes are not the game's player collision. Each scene's `.col` holds one entry per model id of that
scene's IDE (in entity space, like the models). For the marker models the AE ships only a one-triangle `.msh`
stub, but their COL entry is the real thing (docs/re-notes/ae-interiors.md):

    nogo_*      the player's blocking walls (invisible walls in front of hall-end backdrops, closed doorways,
                fences, map edges). Surfaces 11 / 13 brick / plaster, 65 "invisible".
    fgrd_nd_*   no-draw floors and ramps (stairs are ramps here).
    walkable_*  the ped navigation surface (surface 0). Not collision: left out.

Layout (little-endian, INFERRED from hex dumps, then VERIFIED to parse every academy .col to the byte with no
index out of range):

    "COL3", u32 size (entry length = 8 + size)
    +0x08  u16 4, u16 kind (1..3), 20 bytes 0      OPEN
    +0x20  u32 model id (IDE id; matches the placements' model_id)
    +0x24  f32 centre xyz, f32 radius, f32 min xyzw, f32 max xyzw
    +0x54  u32, u32 (always 0 in the academy files)  OPEN (sphere count?)
    +0x5c  u32 box count, boxes (f32 min xyzw, f32 max xyzw, u32 surface)
           u32 vertex count, vertices (3 x i16, / 128 m), pad to 4 bytes
           if vertices: u32 face count, faces (3 x u16 index, u8 surface, u8 light)
           ... (a face tree; skipped, the entry length is known)

Faces are clockwise about their outward side in game space (floors face down when read counter-clockwise), so
the writer reverses them.
"""

from __future__ import annotations

import json
import os
import struct
import sys
from dataclasses import dataclass, field

from .binreader import FormatError

COLLISION_DIR = "collision"
# marker prefix -> kind; walkable_* (ped navigation) is deliberately not collision
KINDS = (("nogo_", "nogo"), ("nog_", "nogo"), ("fgrd_nd_", "floor"))


@dataclass
class ColModel:
    model_id: int
    kind: int
    bounds: tuple                       # (min xyz, max xyz)
    boxes: list = field(default_factory=list)       # [(min xyz, max xyz, surface)]
    vertices: list = field(default_factory=list)    # [(x, y, z)] metres, entity space
    faces: list = field(default_factory=list)       # [(a, b, c, surface)]


def parse_col(data: bytes) -> list:
    """Every COL3 entry of a .col file. Stops at the zero sector padding."""
    out, o, n = [], 0, len(data)
    while o + 8 <= n:
        tag = data[o:o + 4]
        if tag == b"\0\0\0\0":
            break
        if tag != b"COL3":
            raise FormatError(f"offset {o}: expected COL3, got {tag!r}")
        end = o + 8 + struct.unpack_from("<I", data, o + 4)[0]
        if end > n or end < o + 0x64:
            raise FormatError(f"offset {o}: entry end {end} outside 0x64..{n}")

        def need(p, k):
            if p + k > end:
                raise FormatError(f"offset {o}: read of {k} bytes at {p} runs past entry end {end}")

        kind = struct.unpack_from("<H", data, o + 10)[0]
        mid = struct.unpack_from("<I", data, o + 0x20)[0]
        mn = struct.unpack_from("<3f", data, o + 0x34)
        mx = struct.unpack_from("<3f", data, o + 0x44)
        m = ColModel(mid, kind, (mn, mx))
        p = o + 0x5c
        nb = struct.unpack_from("<I", data, p)[0]
        p += 4
        need(p, 36 * nb + 4)
        for _ in range(nb):
            b = struct.unpack_from("<4f4fI", data, p)
            m.boxes.append((b[0:3], b[4:7], b[8]))
            p += 36
        nv = struct.unpack_from("<I", data, p)[0]
        p += 4
        need(p, 6 * nv)
        m.vertices = [tuple(c / 128.0 for c in struct.unpack_from("<3h", data, p + 6 * i)) for i in range(nv)]
        p += 6 * nv
        p += (-(p - o)) % 4
        if nv:
            need(p, 4)
            nf = struct.unpack_from("<I", data, p)[0]
            p += 4
            need(p, 8 * nf)
            for i in range(nf):
                a, b, c, s, _light = struct.unpack_from("<3HBB", data, p + 8 * i)
                if max(a, b, c) >= nv:
                    raise FormatError(f"offset {o} (model {mid}): face {i} index {max(a, b, c)} >= {nv} vertices")
                m.faces.append((a, b, c, s))
        out.append(m)
        o = end
    return out


def marker_kind(model: str):
    s = model.lower()
    return next((k for prefix, k in KINDS if s.startswith(prefix)), None)


def write_gltf(name: str, cm: ColModel, kind: str, out_dir: str) -> dict:
    """collision/<name>.gltf: one primitive, entity space, game -> glTF axes, faces reversed to CCW."""
    from .gltf import GltfBuilder
    from .nft import safe_name
    gb = GltfBuilder()
    pos = []
    for x, y, z in cm.vertices:
        pos += [x, z, -y]
    idx, surf = [], {}
    for a, b, c, s in cm.faces:
        idx += [a, c, b]
        surf[s] = surf.get(s, 0) + 1
    mat = gb.material({"name": "collision_" + kind, "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 1, 1]},
                       "extras": {"blend_mode": "opaque", "collision_only": True}})
    prim = {"attributes": {"POSITION": gb.add_floats(pos, "VEC3", minmax=True)}, "indices": gb.add_indices(idx),
            "material": mat}
    gb.node({"name": name, "mesh": gb.mesh(name, [prim]),
             "extras": {"source": "World.img COL3", "space": "entity", "collision_only": True, "kind": kind,
                        "model_id": cm.model_id, "surfaces": {str(k): v for k, v in sorted(surf.items())}}})
    folder = os.path.join(out_dir, COLLISION_DIR)
    os.makedirs(folder, exist_ok=True)
    path, _n = gb.write(os.path.join(folder, safe_name(name) + ".gltf"))
    return {"gltf": os.path.relpath(path, out_dir), "kind": kind, "faces": len(cm.faces),
            "vertices": len(cm.vertices), "model_id": cm.model_id}


def export_collision(install: str, out_dir: str, log=print) -> dict:
    """For every scene of <out>/scenes/academy.json: read World.img <scene>.col, write collision/<marker>.gltf for the
    nogo_/fgrd_nd_ models placed in it, and collision/index.json (read by tools/ue/import_content.py)."""
    from .img import ImgArchive
    with open(os.path.join(out_dir, "scenes", "academy.json")) as f:
        man = json.load(f)
    img = ImgArchive(os.path.join(install, "Stream", "World.img"))
    models, placements, missing = {}, [], []
    for sc in man["scenes"]:
        scene = sc["name"]
        by_id = {}
        for p in man["placements"]:
            if p["scene"] == scene and marker_kind(p["model"]):
                by_id.setdefault(p["model_id"], []).append(p)
        if not by_id:
            continue
        e = img.find(scene + ".col")
        if e is None:
            missing.append(scene)
            continue
        for cm in parse_col(img.read_entry(e)):
            ps = by_id.get(cm.model_id)
            if not ps or not cm.faces:
                continue
            name = ps[0]["model"]
            if name not in models:
                models[name] = write_gltf(name, cm, marker_kind(name), out_dir)
            placements += [{"scene": p["scene"], "index": p["index"], "model": name} for p in ps]
    index = {"format": "bully-remake.collision-index/1",
             "note": "collision-only meshes from World.img .col (nogo_ walls, fgrd_nd_ floors); entity space, place "
                     "with the manifest placement of the same scene/index; never rendered",
             "models": models, "placements": placements, "scenes_without_col": missing}
    os.makedirs(os.path.join(out_dir, COLLISION_DIR), exist_ok=True)
    with open(os.path.join(out_dir, COLLISION_DIR, "index.json"), "w") as f:
        json.dump(index, f, indent=1)
    log(f"collision: {len(models)} models, {len(placements)} placements, "
        f"{sum(m['faces'] for m in models.values())} faces; scenes without .col: {missing}")
    return index


def main(argv=None) -> int:
    """python3 -m tools.importers.col [--install X] [--out imported] (run after `world`)."""
    import argparse
    from .cli import resolve_install
    ap = argparse.ArgumentParser(prog="python3 -m tools.importers.col", description=export_collision.__doc__)
    ap.add_argument("--install")
    ap.add_argument("--out", default="imported")
    a = ap.parse_args(argv)
    export_collision(resolve_install(a.install), a.out, log=lambda m: print(m, file=sys.stderr))
    return 0


if __name__ == "__main__":
    sys.exit(main())
