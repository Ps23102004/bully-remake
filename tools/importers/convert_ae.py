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
"""

from __future__ import annotations

import os
import re

import numpy as np

from . import coords
from .ae import AeArchive, resolve_material_textures
from .binreader import FormatError
from .convert import ModelResult
from .gltf import GltfBuilder
from .msh import parse_msh
from .nft import safe_name
from .wtex import parse_tex, write_tex_png

TEX_DIR = "textures/ae"
# INFERRED from names (.mtl is encrypted): light-beam / window-glow decals are additive in the PC build
# (FloorGlowAdditive*); here they are mat_glow_*, *_lightglow*, *_additive*, sunbeams.
ADDITIVE = re.compile(r"glow|additive|sunbeam")


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
    return res
