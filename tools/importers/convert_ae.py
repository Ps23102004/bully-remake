# OWNER: builder-A (importers)
"""Anniversary Edition .msh -> glTF 2.0, and the AE texture store.

Same conventions as convert.py (so AE and PC placements line up): positions and
normals are game space (+Z up, metres) mapped with coords.game_to_gltf, triangle
order is kept (proper rotation), one node / one mesh per model, one primitive per
submesh. .msh vertices are already in entity space, so no node transforms exist.

Materials: the .mtl files are encrypted, so the texture slots come from the
material *name* (ae.resolve_material_textures). Diffuse -> baseColorTexture,
normal -> normalTexture; the specular stem is recorded in material.extras only.
Textures are decoded once each to <out>/textures/ae/<stem>.png (wtex).

Skinned props (px*) are exported as static meshes: joints/weights are dropped.
Tangents are dropped (15% unreliable, docs/re-notes/ae-msh.md): viewers regenerate.

Grass: the game draws its lawns at run time (every DL_GrassNN / RC1a_GrassNN / BU1a_ / IN_GrassNN model is a
one-triangle stub), so the .msh has no blades. For ground models whose materials are grass-class
(GRASS_MATERIALS) `emit_grass` scatters crossed-card tufts over the up-facing triangles and writes
<out>/grass/<model>.grass.json plus three tuft glTFs; tools/ue/import_grass.py instances them. Notes:
docs/re-notes/ae-grass.md.
"""

from __future__ import annotations

import json
import os
import re
import zlib

import numpy as np

from . import coords
from .ae import AeArchive, resolve_material_textures
from .binreader import FormatError
from .convert import ModelResult
from .gltf import GltfBuilder
from .msh import parse_msh
from .nft import safe_name
from .wtex import decode_tex, parse_tex, write_tex_png

TEX_DIR = "textures/ae"
# INFERRED from names (.mtl is encrypted): light-beam / window-glow decals are additive in the PC build
# (FloorGlowAdditive*); here they are mat_glow_*, *_lightglow*, *_additive*, sunbeams.
# chalk decals (hopscotch, tic-tac-toe, goal line) are RGB chalk on black with no alpha: additive, not opaque
ADDITIVE = re.compile(r"glow|additive|sunbeam|hopscotch|tictac|soccergoalline")


class AeTextureStore:
    """Decodes archive .tex files to PNG once each. Mirrors the attributes of
    convert.TextureStore that world.build_world reads for its report."""

    def __init__(self, arc: AeArchive, out_dir: str):
        self.arc = arc
        self.out_dir = out_dir
        self.exported: dict[str, dict] = {}
        self.export_errors: dict[tuple, str] = {}
        self.missing: dict[tuple, list] = {}
        self.fallbacks: dict = {}
        self.dicts: dict = {}
        self.dict_errors: dict = {}

    def resolve(self, stem: str, model: str) -> dict | None:
        """Export `stem` (lowercase archive name, no .tex); return its info or None."""
        if stem in self.exported:
            return self.exported[stem]
        if ("", stem) in self.export_errors:
            return None
        try:
            data = self.arc.read(stem, "tex")
            info = parse_tex(data)
            # 8x8 placeholders declare a bigger size: keep them small instead of upsampling
            keep_stored = info.width <= 8 and info.height <= 8
            folder = os.path.join(self.out_dir, TEX_DIR)
            os.makedirs(folder, exist_ok=True)
            fname = safe_name(stem) + ".png"
            w, h, alpha = write_tex_png(data, os.path.join(folder, fname), declared=not keep_stored)
        except FormatError as ex:
            self.export_errors[("", stem)] = str(ex)
            return None
        self.exported[stem] = {"txd": "ae", "name": stem, "path": f"{TEX_DIR}/{fname}", "width": w,
                               "height": h, "has_alpha": alpha, "format": info.format_name,
                               "placeholder": keep_stored and (info.declared_width, info.declared_height) != (w, h)}
        return self.exported[stem]


def _material(mname: str, arc: AeArchive, store: AeTextureStore, gb: GltfBuilder, res: ModelResult,
              model: str) -> dict:
    t = resolve_material_textures(mname, arc)
    out = {"name": mname, "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                                                   "metallicFactor": 0.0, "roughnessFactor": 1.0}}
    pbr = out["pbrMetallicRoughness"]
    textures_meta = {}  # role -> {"uri"}: tools/ue/import_content.py reads this, as for the NIF path
    blend_mode = "opaque"
    if t["diffuse"] is None:
        res.missing_textures.append(f"diffuse:{mname}")
    else:
        info = store.resolve(t["diffuse"], model)
        if info is None:
            res.missing_textures.append(f"diffuse:{t['diffuse']}")
        else:
            pbr["baseColorTexture"] = {"index": gb.texture("../" + info["path"])}
            textures_meta["base"] = {"ref": t["diffuse"], "uri": "../" + info["path"]}
            # "_sp" diffuse variants keep specular in alpha: it is not transparency
            if ADDITIVE.search(mname):
                # glTF has no additive mode: BLEND here, extras.blend_mode tells the UE step (as convert.py)
                blend_mode = "additive"
                out["alphaMode"] = "BLEND"
            elif info["has_alpha"] and "sp" not in t["diffuse"].split("_"):
                blend_mode = "masked"
                out["alphaMode"] = "MASK"
                out["alphaCutoff"] = 0.5
                out["doubleSided"] = True
    if t["normal"]:
        info = store.resolve(t["normal"], model)
        if info is not None:
            out["normalTexture"] = {"index": gb.texture("../" + info["path"])}
            textures_meta["normal"] = {"ref": t["normal"], "uri": "../" + info["path"]}
    out["extras"] = {"blend_mode": blend_mode, "textures": textures_meta,
                     "ae": {"material": mname, "rule": t["rule"], "diffuse": t["diffuse"],
                            "normal": t["normal"], "specular": t["specular"]}}
    return out


# ---------------------------------------------------------------------------------------------
# grass (see module docstring and docs/re-notes/ae-grass.md)

GRASS_DIR = "grass"
# material name (after "mat_", longest prefix wins) -> class. The "e" in zrc_grasse is the EDGE strip: its V runs
# 0..1 across the strip with a dark soil fringe at V=0, so it is thinned there. INFERRED from the textures.
GRASS_MATERIALS = (("zrc_grasst_swap", "lawn"), ("zrc_grasseb_swap", "lawn"), ("zrc_grasse_swap", "edge"),
                   ("zsc_bggrass_swap", "wild"), ("te04_grass_01", "lawn"))
# tuft id -> (texture stem, width m, height m, sink m). Textures are the game's own alpha cards.
TUFTS = {"clump": ("rc_grassz_swap_d", 0.60, 0.30), "strip": ("rc_grasstripz_swap_d", 1.20, 0.15),
         "dry": ("te03_grass3_d", 0.70, 0.36)}
# tufts per square metre (before the up-facing filter), per class and tuft
GRASS_DENSITY = {"lawn": {"clump": 1.6, "strip": 2.2}, "edge": {"clump": 0.8, "strip": 1.6},
                 "wild": {"clump": 1.0, "dry": 2.0}}
GRASS_MIN_UP = 0.8          # cos of the steepest slope that grows grass
GRASS_MIN_AREA_M2 = 25.0    # a model with less grass-class ground than this gets no file
GRASS_SINK_M = 0.02


def grass_class(material_name: str):
    s = material_name.lower()
    s = s[4:] if s.startswith("mat_") else s
    best = None
    for prefix, cls in GRASS_MATERIALS:
        if s == prefix or s.startswith(prefix + "_") or (prefix == "te04_grass_01" and s.startswith(prefix)):
            if best is None or len(prefix) > len(best[0]):
                best = (prefix, cls)
    return best[1] if best else None


def _mean_rgb(arc: AeArchive, stem: str):
    """Alpha-weighted mean sRGB (0..1) of the smallest mip that is at least 8 px; None if unreadable."""
    try:
        data = arc.read(stem, "tex")
        info = parse_tex(data)
        mip = max(i for i, (w, h, _o, _s) in enumerate(info.levels) if min(w, h) >= 8) if info.levels else 0
        w, h, rgba = decode_tex(data, mip)
    except (FormatError, ValueError):
        return None
    a = np.frombuffer(rgba, np.uint8).reshape(h, w, 4).astype(np.float64)
    wgt = np.maximum(a[..., 3:4] / 255.0, 1e-3)
    return ((a[..., :3] * wgt).sum((0, 1)) / wgt.sum() / 255.0).tolist()


def _smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def ensure_tufts(arc: AeArchive, store: AeTextureStore, out_dir: str) -> dict:
    """Write <out>/grass/tuft_<id>.gltf (two crossed vertical quads, base at z=0, game +Z up) once per run.
    Returns id -> {"gltf", "texture", "mean_rgb", "size_m"}."""
    done = getattr(store, "grass_tufts", None)
    if done is not None:
        return done
    folder = os.path.join(out_dir, GRASS_DIR)
    os.makedirs(folder, exist_ok=True)
    done = {}
    for tid, (stem, w, h) in TUFTS.items():
        info = store.resolve(stem, "grass")
        if info is None:
            continue
        gb = GltfBuilder()
        hw = w / 2
        pos, nrm, uv, idx = [], [], [], []
        for k, (dx, dy) in enumerate(((1.0, 0.0), (0.0, 1.0))):
            corners = ((-hw, 0.0), (hw, 0.0), (hw, h), (-hw, h))
            for (c, z), (u, v) in zip(corners, ((0, 1), (1, 1), (1, 0), (0, 0))):
                gx, gy, gz = c * dx, c * dy, z
                pos += [gx, gz, -gy]                      # game -> glTF
                nrm += [0.0, 1.0, 0.0]                    # up: soft, even lighting like the original cards
                uv += [u, v]
            b = 4 * k
            # both windings, single-sided material: a two-sided material flips the up normal on the back face, so
            # every card seen from behind rendered black (lit from below) in UE
            idx += [b, b + 1, b + 2, b, b + 2, b + 3, b, b + 2, b + 1, b, b + 3, b + 2]
        mat = {"name": "grass_" + tid, "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0.0,
                                                               "roughnessFactor": 1.0,
                                                               "baseColorTexture": {"index": gb.texture("../" + info["path"])}},
               "alphaMode": "MASK", "alphaCutoff": 0.4, "doubleSided": False,
               "extras": {"blend_mode": "masked", "textures": {"base": {"ref": stem, "uri": "../" + info["path"]}}}}
        prim = {"attributes": {"POSITION": gb.add_floats(pos, "VEC3", minmax=True), "NORMAL": gb.add_floats(nrm, "VEC3"),
                               "TEXCOORD_0": gb.add_floats(uv, "VEC2")},
                "indices": gb.add_indices(idx), "material": gb.material(mat)}
        gb.node({"name": "tuft_" + tid, "mesh": gb.mesh("tuft_" + tid, [prim]),
                 "extras": {"source": "generated", "space": "entity", "width_m": w, "height_m": h}})
        path, _n = gb.write(os.path.join(folder, f"tuft_{tid}.gltf"))
        done[tid] = {"gltf": os.path.relpath(path, out_dir), "texture": stem, "mean_rgb": _mean_rgb(arc, stem),
                     "size_m": [w, h]}
    store.grass_tufts = done
    return done


def emit_grass(name: str, m, arc: AeArchive, store: AeTextureStore, out_dir: str) -> dict | None:
    """Scatter tufts over the up-facing triangles of `m`'s grass-class submeshes. Positions are the model's
    entity space converted to UE centimetres (x, -y, z), so a placement's actor transform maps them to the world.
    Deterministic (seeded by the model name). Returns the index entry, or None."""
    subs = [(s, grass_class(s.material_name or "")) for s in m.submeshes]
    subs = [(s, c) for s, c in subs if c and len(s.indices)]
    if not subs:
        return None
    tufts_ok = ensure_tufts(arc, store, out_dir)
    rng = np.random.default_rng(zlib.crc32(name.lower().encode()))
    inst = {}                                  # tuft id -> list of [x, y, z, yaw, scale] rows (UE cm / degrees / %)
    areas = {}
    ground = {}                                # class -> list of (area, mean rgb)
    for s, cls in subs:
        p = s.positions.astype(np.float64)
        tri = s.indices.reshape(-1, 3)
        a, b, c = p[tri[:, 0]], p[tri[:, 1]], p[tri[:, 2]]
        n = np.cross(b - a, c - a)
        area = 0.5 * np.linalg.norm(n, axis=1)
        if s.normals is not None:                # orient by the stored normals: the game's winding is CCW about them
            n = np.where((n * s.normals[tri].mean(1)).sum(1, keepdims=True) < 0, -n, n)
        up = n[:, 2] / np.maximum(np.linalg.norm(n, axis=1), 1e-12)
        ok = (up >= GRASS_MIN_UP) & (area > 1e-6)
        areas[cls] = areas.get(cls, 0.0) + float(area[ok].sum())
        t = _mean_rgb(arc, resolve_material_textures(s.material_name, arc)["diffuse"] or "")
        if t is not None and ok.any():
            ground.setdefault(cls, []).append((float(area[ok].sum()), t))
        for tid, dens in GRASS_DENSITY[cls].items():
            if tid not in tufts_ok:
                continue
            want = area * ok * dens
            cnt = np.floor(want).astype(np.int64) + (rng.random(len(want)) < (want - np.floor(want)))
            tot = int(cnt.sum())
            if tot == 0:
                continue
            ti = np.repeat(np.arange(len(tri)), cnt)
            r1, r2 = rng.random(tot), rng.random(tot)
            flip = r1 + r2 > 1
            r1, r2 = np.where(flip, 1 - r1, r1), np.where(flip, 1 - r2, r2)
            w1, w2, w0 = r1, r2, 1 - r1 - r2
            pt = w0[:, None] * a[ti] + w1[:, None] * b[ti] + w2[:, None] * c[ti]
            keep = np.ones(tot, bool)
            if cls == "edge" and s.uv0 is not None:
                v = (w0 * s.uv0[tri[ti, 0], 1] + w1 * s.uv0[tri[ti, 1], 1] + w2 * s.uv0[tri[ti, 2], 1]) % 1.0
                keep = rng.random(tot) < _smoothstep(0.12, 0.40, v)
            pt = pt[keep]
            k = len(pt)
            rows = np.empty((k, 5), np.int64)
            rows[:, 0] = np.rint(pt[:, 0] * 100)
            rows[:, 1] = np.rint(-pt[:, 1] * 100)
            rows[:, 2] = np.rint((pt[:, 2] - GRASS_SINK_M) * 100)
            rows[:, 3] = rng.integers(0, 360, k)
            rows[:, 4] = np.rint(rng.uniform(75, 130, k))
            inst.setdefault(tid, []).append(rows)
    total_area = sum(areas.values())
    if total_area < GRASS_MIN_AREA_M2 or not inst:
        return None
    tufts = {}
    for tid, parts in inst.items():
        rows = np.concatenate(parts)
        info = tufts_ok[tid]
        blade = info["mean_rgb"] or [0.4, 0.45, 0.25]
        gs = [(aa, t) for cls in GRASS_DENSITY for aa, t in ground.get(cls, []) if tid in GRASS_DENSITY[cls]]
        tint = [1.0, 1.0, 1.0]
        if gs:
            w = sum(aa for aa, _ in gs)
            g = [sum(aa * t[i] for aa, t in gs) / w for i in range(3)]
            ratio = [0.9 * g[i] / max(blade[i], 0.02) for i in range(3)]
            lum = 0.299 * ratio[0] + 0.587 * ratio[1] + 0.114 * ratio[2]
            # match the ground's brightness fully, its hue by half (blade hue is the art we want to keep)
            tint = [float(np.clip(lum * (ratio[i] / lum) ** 0.5, 0.55, 1.35)) for i in range(3)]
        tufts[tid] = {"mesh": f"tuft_{tid}", "count": int(len(rows)), "tint": [round(x, 3) for x in tint],
                      "rows": rows.ravel().tolist()}
    doc = {"format": "bully-remake.grass/1", "model": name, "source": f"data_1.zip/bully/{name}.msh",
           "units": "rows of x_cm, y_cm, z_cm, yaw_deg, scale_pct in the model's entity space, UE axes (x, -y, z)",
           "tile": name.lower().startswith("fgrd_"), "area_m2": {k: round(v, 1) for k, v in sorted(areas.items())},
           "tufts": tufts}
    folder = os.path.join(out_dir, GRASS_DIR)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, safe_name(name) + ".grass.json"), "w") as f:
        json.dump(doc, f, separators=(",", ":"))
    entry = {"file": f"{GRASS_DIR}/{safe_name(name)}.grass.json", "area_m2": doc["area_m2"], "tile": doc["tile"],
             "instances": {t: v["count"] for t, v in tufts.items()}}
    idx = getattr(store, "grass_index", None)
    if idx is None:
        idx = store.grass_index = {}
    idx[name] = entry
    with open(os.path.join(folder, "index.json"), "w") as f:
        json.dump({"format": "bully-remake.grass-index/1", "tufts": store.grass_tufts, "models": idx}, f, indent=1)
    return entry


def convert_msh(name: str, data, arc: AeArchive, store: AeTextureStore, out_dir: str) -> ModelResult:
    """One .msh -> models/<name>.gltf + .bin. Raises FormatError on a bad file."""
    res = ModelResult(name)
    m = parse_msh(bytes(data))
    res.warnings.extend(m.warnings)
    if m.skinned:
        res.skinned_shapes = len({s.part_index for s in m.submeshes if s.weights is not None})
        res.warnings.append("skinned model exported as static (joints and weights dropped)")
    gb = GltfBuilder()
    mats: dict[str, int] = {}
    prims = []
    bmin = np.full(3, np.inf)
    bmax = np.full(3, -np.inf)
    for s in m.submeshes:
        if len(s.indices) == 0 or len(s.positions) == 0:
            res.skipped_empty += 1
            continue
        if int(s.indices.max()) >= len(s.positions):
            raise FormatError(f"{name}: index {int(s.indices.max())} >= {len(s.positions)} vertices")
        bmin = np.minimum(bmin, s.positions.min(0))
        bmax = np.maximum(bmax, s.positions.max(0))
        p = s.positions
        attrs = {"POSITION": gb.add_floats(np.stack([p[:, 0], p[:, 2], -p[:, 1]], 1).ravel().tolist(),
                                           "VEC3", minmax=True)}
        n = s.normals
        if n is not None and np.linalg.norm(n, axis=1).min() > 0.5:
            attrs["NORMAL"] = gb.add_floats(np.stack([n[:, 0], n[:, 2], -n[:, 1]], 1).ravel().tolist(), "VEC3")
        elif n is not None:
            res.warnings.append(f"part {s.part}: degenerate normals omitted")
        for k, uv in enumerate((s.uv0, s.uv1, s.uv2)):
            if uv is not None:
                attrs[f"TEXCOORD_{k}"] = gb.add_floats(uv.ravel().tolist(), "VEC2")
        key = s.material_name or f"#{s.material}"
        if key not in mats:
            mats[key] = gb.material(_material(key, arc, store, gb, res, name))
        prims.append({"attributes": attrs, "indices": gb.add_indices(s.indices.tolist()),
                      "material": mats[key], "extras": {"part": s.part, "material": key}})
        res.shapes += 1
        res.vertices += len(p)
        res.triangles += len(s.indices) // 3
    if not prims:
        res.warnings.append("source .msh contains no geometry")
        res.empty_source = not m.submeshes
        return res
    gb.node({"name": name, "mesh": gb.mesh(name, prims),
             "extras": {"source": f"data_1.zip/bully/{name}.msh", "space": "entity", "msh_version": m.version}})
    folder = os.path.join(out_dir, "models")
    os.makedirs(folder, exist_ok=True)
    path, nbytes = gb.write(os.path.join(folder, safe_name(name) + ".gltf"))
    res.gltf = os.path.relpath(path, out_dir)
    res.bin_bytes = nbytes
    res.bounds_game = [bmin.tolist(), bmax.tolist()]
    if any(grass_class(s.material_name or "") for s in m.submeshes):
        emit_grass(name, m, arc, store, out_dir)
    return res
