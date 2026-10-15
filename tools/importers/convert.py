# OWNER: builder-A (importers)
"""NIF model -> glTF 2.0 conversion, and the shared texture store.

Model space ("entity space", bully-re docs/nif.md): the game replaces the
transform of the model node (the root's direct child) with the placement
matrix, so the root and the model node are treated as identity and every
deeper node transform is composed down to the shapes. The original model
node transform is kept in the glTF node extras for reference. A shape that
hangs directly under the root is treated the same way.

Each model becomes ONE glTF node holding ONE mesh: every visible
NiTriShape/NiTriStrips becomes a primitive with its node transforms baked
into the vertices (game -> glTF basis, see coords.py). Shapes and nodes with
the APP_CULLED flag (bit 0) are skipped, as the game does.

Properties inherit down the scene graph (Gamebryo rule): a property on a
parent applies to its children unless a child carries one of the same type.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field

from . import coords
from .binreader import FormatError
from .gltf import CLAMP, REPEAT, GltfBuilder
from .nft import TextureDictionary, safe_name, texture_key
from .nif import NifFile, TextModeCorruptionError, parse_nif

GEOMETRY = ("NiTriShape", "NiTriStrips")
PROPERTY_TYPES = ("NiTexturingProperty", "NiMaterialProperty", "NiAlphaProperty", "NiStencilProperty",
                  "NiVertexColorProperty", "NiZBufferProperty", "NiSpecularProperty")


class TextureStore:
    """Resolves texture references to exported PNG files under out/textures.

    Lookup order: the model's own texture dictionary (IDE TXD column, the
    game's rule); then, if missing there, a global index over every .nft in
    World.img. A global hit is recorded as a `fallback` so it can be audited
    (whether the game has TXD parent dictionaries is not established: OPEN).
    """

    def __init__(self, img, out_dir: str):
        self.img = img
        self.out_dir = out_dir
        self.dicts: dict[str, TextureDictionary | None] = {}
        self.dict_errors: dict[str, str] = {}
        self.exported: dict[tuple, dict] = {}
        self.export_errors: dict[tuple, str] = {}
        self.fallbacks: dict[tuple, str] = {}
        self.missing: dict[tuple, list] = {}
        self._global: dict[str, list] | None = None

    def dictionary(self, txd: str) -> TextureDictionary | None:
        k = txd.lower()
        if k not in self.dicts:
            entry = self.img.find(txd + ".nft")
            if entry is None:
                self.dicts[k] = None
                self.dict_errors[k] = "no such .nft in archive"
            else:
                try:
                    self.dicts[k] = TextureDictionary(entry.stem, self.img.read_entry(entry))
                except FormatError as ex:
                    self.dicts[k] = None
                    self.dict_errors[k] = str(ex)
        return self.dicts[k]

    def global_index(self) -> dict:
        if self._global is None:
            self._global = {}
            for name in self.img.names("nft"):
                d = self.dictionary(os.path.splitext(name)[0])
                if d is None:
                    continue
                for key in d.textures:
                    self._global.setdefault(key, []).append(d.name)
        return self._global

    def resolve(self, txd: str | None, ref: str, model: str) -> tuple | None:
        """Return (relative uri from models/, texture info) or None."""
        key = texture_key(ref)
        d = self.dictionary(txd) if txd else None
        if d is None or key not in d.textures:
            hits = self.global_index().get(key, [])
            if not hits:
                self.missing.setdefault((txd or "", key), []).append(model)
                return None
            d = self.dictionary(hits[0])
            self.fallbacks[(txd or "", key)] = d.name
        tex = d.textures[key]
        ek = (d.name.lower(), key)
        if ek in self.export_errors:
            return None
        if ek not in self.exported:
            folder = os.path.join(self.out_dir, "textures", safe_name(d.name))
            os.makedirs(folder, exist_ok=True)
            fname = safe_name(tex.name) + ".png"
            try:
                info = d.export(tex, os.path.join(folder, fname))
            except FormatError as ex:
                self.export_errors[ek] = str(ex)
                return None
            info.update({"txd": d.name, "name": tex.name, "path": f"textures/{safe_name(d.name)}/{fname}",
                         "source_path": tex.source_path})
            self.exported[ek] = info
        info = self.exported[ek]
        return "../" + info["path"], info


@dataclass
class ModelResult:
    name: str
    gltf: str | None = None
    shapes: int = 0
    vertices: int = 0
    triangles: int = 0
    skipped_hidden: int = 0
    skipped_empty: int = 0
    skinned_shapes: int = 0
    winding_agree: int = 0
    winding_checked: int = 0
    bin_bytes: int = 0
    warnings: list = field(default_factory=list)
    missing_textures: list = field(default_factory=list)
    bounds_game: list | None = None
    empty_source: bool = False


def _effective_props(nif: NifFile, inherited: dict, refs) -> dict:
    props = dict(inherited)
    for ref in refs:
        b = nif.block(ref)
        if b is not None and b.type_name in PROPERTY_TYPES and b.data is not None:
            props[b.type_name] = b.data
    return props


def _wrap_modes(flags: int) -> tuple:
    # TexDesc flags in 20.1+: bits 0-7 UV set, 8-11 filter, 12-15 clamp mode
    # (0 clamp/clamp, 1 clamp S/wrap T, 2 wrap S/clamp T, 3 wrap/wrap).
    # INFERRED from Gamebryo's documented TexDesc packing; every school
    # texture slot reads 0x3200 = wrap/wrap, trilinear, UV set 0.
    clamp = (flags >> 12) & 0xF
    s = REPEAT if clamp in (2, 3) else CLAMP
    t = REPEAT if clamp in (1, 3) else CLAMP
    return s, t


def _material_json(nif: NifFile, props: dict, store: TextureStore, txd: str | None, model: str,
                   gb: GltfBuilder, res: ModelResult, n_uv: int, has_colors: bool) -> dict:
    mat = props.get("NiMaterialProperty")
    tex = props.get("NiTexturingProperty")
    alpha = props.get("NiAlphaProperty")
    stencil = props.get("NiStencilProperty")
    vcol = props.get("NiVertexColorProperty")
    raw = {}
    diffuse = [1.0, 1.0, 1.0]
    opacity = 1.0
    emissive = [0.0, 0.0, 0.0]
    name = None
    if mat:
        raw["material"] = {k: mat[k] for k in ("ambient", "diffuse", "specular", "emissive", "glossiness", "alpha")}
        diffuse = list(mat["diffuse"])
        opacity = mat["alpha"]
        emissive = [min(max(c, 0.0), 1.0) for c in mat["emissive"]]
        name = nif.string(mat["name"])
    vertex_mode = None
    if vcol:
        vertex_mode = (vcol["flags"] >> 4) & 3  # 0 ignore, 1 emissive, 2 ambient+diffuse
        raw["vertex_color_flags"] = vcol["flags"]
    pbr = {"metallicFactor": 0.0, "roughnessFactor": 1.0}
    out = {"pbrMetallicRoughness": pbr}
    textures_meta = {}
    if tex:
        raw["texturing_flags"] = tex["flags"]
        for slot, desc in tex["slots"].items():
            src = nif.data(desc["source"], "NiSourceTexture", "NiSourceCubeMap")
            if src is None:
                continue
            ref = nif.string(src["file_name"]) or ""
            uv_set = desc["flags"] & 0xFF
            entry = {"ref": ref, "flags": desc["flags"], "uv_set": uv_set}
            if "transform" in desc:
                entry["transform"] = desc["transform"]
                res.warnings.append(f"texture transform on {slot} slot ignored")
            if uv_set >= max(n_uv, 1):
                res.warnings.append(f"{slot} uses UV set {uv_set} but shape has {n_uv}")
                uv_set = 0
            resolved = store.resolve(txd, ref, model) if ref else None
            if resolved is None:
                res.missing_textures.append(f"{slot}:{ref}")
            else:
                uri, info = resolved
                entry["uri"] = uri
                ws, wt = _wrap_modes(desc["flags"])
                if slot == "base":
                    pbr["baseColorTexture"] = {"index": gb.texture(uri, ws, wt), "texCoord": uv_set}
                    if info["has_alpha"] and not alpha:
                        entry["note"] = "texture has alpha but no NiAlphaProperty: alpha ignored (OPAQUE)"
                elif slot == "normal":
                    # Tangent-space RGB normal maps (z ~ 255). Green-channel
                    # handedness (D3D vs GL) is unverified: OPEN.
                    out["normalTexture"] = {"index": gb.texture(uri, ws, wt), "texCoord": uv_set}
                elif slot == "glow":
                    out["emissiveTexture"] = {"index": gb.texture(uri, ws, wt), "texCoord": uv_set}
                    emissive = [1.0, 1.0, 1.0]
            textures_meta[slot] = entry
        if not name and "base" in textures_meta:
            name = os.path.splitext(os.path.basename(textures_meta["base"]["ref"].replace("\\", "/")))[0]
    # Base colour factor: when vertex colours supply ambient+diffuse (the
    # prelit-building case), the material diffuse is replaced by them.
    use_vertex_diffuse = has_colors and vertex_mode == 2
    pbr["baseColorFactor"] = ([1.0, 1.0, 1.0] if use_vertex_diffuse else [min(max(c, 0.0), 1.0) for c in diffuse]) + [opacity]
    if any(emissive):
        out["emissiveFactor"] = emissive
    if alpha:
        f = alpha["flags"]
        raw["alpha"] = {"flags": f, "threshold": alpha["threshold"]}
        blend, test = bool(f & 1), bool(f & (1 << 9))
        if test:
            out["alphaMode"] = "MASK"
            out["alphaCutoff"] = alpha["threshold"] / 255.0
        elif blend:
            out["alphaMode"] = "BLEND"
    elif opacity < 1.0:
        out["alphaMode"] = "BLEND"
    if stencil:
        raw["stencil_flags"] = stencil["flags"]
        if (stencil["flags"] >> 10) & 3 == 3:  # draw mode BOTH (bully-re: always 0x4d80)
            out["doubleSided"] = True
    out["name"] = name or "material"
    out["extras"] = {"gamebryo": raw, "textures": textures_meta,
                     "vertex_color_mode": vertex_mode, "vertex_colors_drive_diffuse": use_vertex_diffuse}
    return out


def _check_winding(pos, nrm, tris) -> tuple:
    """Count triangles whose geometric normal (CCW) agrees with the sum of
    their stored vertex normals. Evidence for the winding convention."""
    agree = checked = 0
    if not nrm:
        return 0, 0
    for i in range(0, len(tris), 3):
        a, b, c = tris[i], tris[i + 1], tris[i + 2]
        ax, ay, az = pos[3 * a:3 * a + 3]
        bx, by, bz = pos[3 * b:3 * b + 3]
        cx, cy, cz = pos[3 * c:3 * c + 3]
        ux, uy, uz = bx - ax, by - ay, bz - az
        vx, vy, vz = cx - ax, cy - ay, cz - az
        gx, gy, gz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        sx = nrm[3 * a] + nrm[3 * b] + nrm[3 * c]
        sy = nrm[3 * a + 1] + nrm[3 * b + 1] + nrm[3 * c + 1]
        sz = nrm[3 * a + 2] + nrm[3 * b + 2] + nrm[3 * c + 2]
        d = gx * sx + gy * sy + gz * sz
        if abs(d) < 1e-12:
            continue
        checked += 1
        agree += d > 0
    return agree, checked


def convert_model(name: str, data, txd: str | None, store: TextureStore, out_dir: str,
                  entity_space: bool = True) -> ModelResult:
    res = ModelResult(name)
    try:
        nif = parse_nif(data)
    except TextModeCorruptionError as ex:
        raise FormatError(str(ex)) from ex
    failed = [b for b in nif.failed_blocks()]
    if failed:
        # Fail loudly: a known block that does not decode to the byte means
        # our layout is wrong for this file.
        raise FormatError(f"{len(failed)} blocks failed to decode, first: #{failed[0].index} "
                          f"{failed[0].type_name}: {failed[0].error}")
    if nif.trailing_nonzero:
        res.warnings.append(f"{nif.trailing_nonzero} non-zero bytes after NIF footer")
    gb = GltfBuilder()
    primitives = []
    mat_cache: dict[str, int] = {}
    model_node_xf = None
    bmin = [float("inf")] * 3
    bmax = [float("-inf")] * 3

    def visit(ref: int, parent: coords.Xform, inherited: dict, depth: int, path: list):
        nonlocal model_node_xf
        b = nif.block(ref)
        if b is None or b.data is None or depth > 64:
            return
        d = b.data
        if b.type_name == "NiNode":
            if d["flags"] & 1:
                res.skipped_hidden += 1
                return
            local = coords.Xform.from_av(d)
            if entity_space and depth <= 1:
                if depth == 1:
                    model_node_xf = local
                xf = parent
            else:
                xf = parent.compose(local)
            props = _effective_props(nif, inherited, d["properties"])
            for c in d["children"]:
                visit(c, xf, props, depth + 1, path + [nif.string(d["name"]) or f"#{ref}"])
            return
        if b.type_name not in GEOMETRY:
            return
        if d["flags"] & 1:
            res.skipped_hidden += 1
            return
        gd = nif.data(d["data"], "NiTriShapeData", "NiTriStripsData")
        if gd is None or not gd["vertices"] or not gd["triangles"]:
            res.skipped_empty += 1
            return
        if d["skin"] >= 0:
            res.skinned_shapes += 1
            res.warnings.append(f"shape #{ref} is skinned: exported in its stored (bind) pose")
        local = coords.Xform.from_av(d)
        xf = parent if (entity_space and depth <= 1) else parent.compose(local)
        props = _effective_props(nif, inherited, d["properties"])
        nv = gd["num_vertices"]
        tris = gd["triangles"]
        if max(tris) >= nv:
            raise FormatError(f"shape #{ref}: triangle index {max(tris)} >= {nv} vertices")
        src_p, src_n = gd["vertices"], gd["normals"]
        agree, checked = _check_winding(src_p, src_n, tris)
        res.winding_agree += agree
        res.winding_checked += checked
        pos, nrm = [], []
        rs = [[c * xf.s for c in row] for row in xf.r]
        for i in range(nv):
            v = coords.mat_vec(rs, src_p[3 * i:3 * i + 3])
            g = (v[0] + xf.t[0], v[1] + xf.t[1], v[2] + xf.t[2])
            for k in range(3):
                bmin[k] = min(bmin[k], g[k])
                bmax[k] = max(bmax[k], g[k])
            pos.extend(coords.game_to_gltf(g))
        if src_n:
            for i in range(nv):
                n = coords.mat_vec(xf.r, src_n[3 * i:3 * i + 3])
                ln = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) ** 0.5
                n = (n[0] / ln, n[1] / ln, n[2] / ln) if ln > 1e-12 else (0.0, 0.0, 1.0)
                nrm.extend(coords.game_to_gltf(n))
        if coords.det3(rs) < 0:  # mirrored transform flips winding
            tris = [tris[i + j] for i in range(0, len(tris), 3) for j in (0, 2, 1)]
            res.warnings.append(f"shape #{ref}: mirrored transform, winding flipped")
        attrs = {"POSITION": gb.add_floats(pos, "VEC3", minmax=True)}
        if nrm:
            attrs["NORMAL"] = gb.add_floats(nrm, "VEC3")
        for k, uv in enumerate(gd["uv_sets"]):
            attrs[f"TEXCOORD_{k}"] = gb.add_floats(list(uv), "VEC2")
        if gd["colors"]:
            cols = [min(max(c, 0.0), 1.0) for c in gd["colors"]]
            attrs["COLOR_0"] = gb.add_floats(cols, "VEC4")
        mjson = _material_json(nif, props, store, txd, name, gb, res, len(gd["uv_sets"]), bool(gd["colors"]))
        mkey = json.dumps(mjson, sort_keys=True)
        if mkey not in mat_cache:
            mat_cache[mkey] = gb.material(mjson)
        prim = {"attributes": attrs, "indices": gb.add_indices(list(tris)), "material": mat_cache[mkey],
                "extras": {"shape": nif.string(d["name"]) or f"#{ref}", "block": ref,
                           "node_path": "/".join(path)}}
        primitives.append(prim)
        res.shapes += 1
        res.vertices += nv
        res.triangles += len(tris) // 3

    roots = nif.roots or [0]
    for r in roots:
        visit(r, coords.Xform(), {}, 0, [])
    if not primitives:
        has_geom = any(b.type_name in GEOMETRY for b in nif.blocks)
        res.warnings.append("no visible geometry" if has_geom else "source NIF contains no geometry blocks")
        res.empty_source = not has_geom
        return res
    mesh = gb.mesh(name, primitives)
    extras = {"source": f"World.img/{name}.nif", "txd": txd, "space": "entity" if entity_space else "file",
              "big_endian": nif.big_endian}
    if model_node_xf is not None and not model_node_xf.is_identity():
        extras["model_node_transform_game"] = model_node_xf.to_json()
    gb.node({"name": name, "mesh": mesh, "extras": extras})
    folder = os.path.join(out_dir, "models")
    os.makedirs(folder, exist_ok=True)
    path, nbytes = gb.write(os.path.join(folder, safe_name(name) + ".gltf"))
    res.gltf = os.path.relpath(path, out_dir)
    res.bin_bytes = nbytes
    res.bounds_game = [bmin, bmax]
    return res
