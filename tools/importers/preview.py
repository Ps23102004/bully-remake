# OWNER: builder-A (importers)
"""CPU-only preview renderer for an imported scene (no GPU, no engine).

Reads scenes/academy.json and the glTF models exactly as a downstream
consumer would, places every visible model with the manifest's GAME-space
transform, and rasterises textured, lambert-lit triangles with a z-buffer
into a PNG. Purpose: visual evidence that placements, model space, UVs and
textures line up. Needs numpy; Pillow is used to read PNG textures when
present (otherwise flat material colours).
"""

from __future__ import annotations

import json
import math
import os
import struct

import numpy as np

from .textures import write_png

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None


def _accessor(doc, bins, idx):
    a = doc["accessors"][idx]
    v = doc["bufferViews"][a["bufferView"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    dt = {5126: "<f4", 5123: "<u2", 5125: "<u4"}[a["componentType"]]
    start = v.get("byteOffset", 0) + a.get("byteOffset", 0)
    arr = np.frombuffer(bins[v["buffer"]], dtype=dt, count=a["count"] * n, offset=start)
    return arr.reshape(-1, n) if n > 1 else arr


class _TexCache:
    def __init__(self):
        self.cache = {}

    def get(self, path):
        if path not in self.cache:
            img = None
            if Image is not None and os.path.exists(path):
                with Image.open(path) as im:
                    img = np.asarray(im.convert("RGBA"), dtype=np.float32) / 255.0
            self.cache[path] = img
        return self.cache[path]


def load_model(path, texcache):
    folder = os.path.dirname(path)
    with open(path) as f:
        doc = json.load(f)
    bins = [open(os.path.join(folder, b["uri"]), "rb").read() for b in doc["buffers"]]
    prims = []
    for prim in doc["meshes"][0]["primitives"]:
        at = prim["attributes"]
        pos = _accessor(doc, bins, at["POSITION"]).astype(np.float64)
        game = np.stack([pos[:, 0], -pos[:, 2], pos[:, 1]], axis=1)  # glTF -> game
        uv = _accessor(doc, bins, at["TEXCOORD_0"]) if "TEXCOORD_0" in at else None
        col = _accessor(doc, bins, at["COLOR_0"])[:, :3] if "COLOR_0" in at else None
        idx = _accessor(doc, bins, prim["indices"]).astype(np.int64).reshape(-1, 3)
        mat = doc["materials"][prim["material"]] if "material" in prim else {}
        pbr = mat.get("pbrMetallicRoughness", {})
        tex = None
        if "baseColorTexture" in pbr:
            img = doc["images"][doc["textures"][pbr["baseColorTexture"]["index"]]["source"]]
            tex = texcache.get(os.path.normpath(os.path.join(folder, img["uri"])))
        prims.append({"pos": game, "uv": uv, "col": col, "idx": idx, "tex": tex,
                      "factor": np.array(pbr.get("baseColorFactor", [1, 1, 1, 1])[:3]),
                      "mask": mat.get("alphaMode") == "MASK", "cutoff": mat.get("alphaCutoff", 0.5),
                      "additive": mat.get("extras", {}).get("blend_mode") == "additive",
                      "translucent": mat.get("extras", {}).get("blend_mode") == "translucent"})
    return prims


def render(out_dir, scenes, view="top", size=1600, cut=None, out_path=None, log=print):
    with open(os.path.join(out_dir, "scenes", "academy.json")) as f:
        manifest = json.load(f)
    texcache = _TexCache()
    models = {}
    tris_world = []  # list of (verts(n,3,3), uv(n,3,2)|None, col(n,3,3)|None, tex, factor, mask)
    for pl in manifest["placements"]:
        if pl["scene"] not in scenes or not pl["gltf"] or not pl["visible"]:
            continue
        if pl["gltf"] not in models:
            models[pl["gltf"]] = load_model(os.path.join(out_dir, pl["gltf"]), texcache)
        r = np.array(pl["game"]["rotation_rows"])
        s = np.array(pl["game"]["scale"])
        t = np.array(pl["game"]["position"])
        for p in models[pl["gltf"]]:
            w = (r @ (p["pos"] * s).T).T + t
            tri = w[p["idx"]]
            uv = p["uv"][p["idx"]] if p["uv"] is not None else None
            col = p["col"][p["idx"]] if p["col"] is not None else None
            tris_world.append((tri, uv, col, p["tex"], p["factor"], p["mask"], p["cutoff"], p["additive"],
                               p["translucent"]))
    if not tris_world:
        raise SystemExit(f"nothing to render for scenes {scenes}")
    allv = np.concatenate([t[0].reshape(-1, 3) for t in tris_world])
    lo, hi = np.percentile(allv, 0.5, axis=0), np.percentile(allv, 99.5, axis=0)
    if view == "top":
        def proj(v):  # x right, y up (north), depth = z
            return np.stack([v[..., 0], v[..., 1], v[..., 2]], axis=-1)
    else:
        az, el = math.radians(225.0), math.radians(35.0)
        ca, sa, ce, se = math.cos(az), math.sin(az), math.cos(el), math.sin(el)

        def proj(v):
            x, y, z = v[..., 0], v[..., 1], v[..., 2]
            u = x * ca - y * sa
            d = x * sa + y * ca
            return np.stack([u, d * se + z * ce, z * se - d * ce], axis=-1)
    pv = proj(allv)
    plo, phi = pv.min(0), pv.max(0)
    if view == "top":
        plo, phi = proj(lo[None])[0], proj(hi[None])[0]
    span = max(phi[0] - plo[0], phi[1] - plo[1])
    scale = (size - 20) / span
    W = int((phi[0] - plo[0]) * scale) + 20
    H = int((phi[1] - plo[1]) * scale) + 20
    img = np.zeros((H, W, 3), np.float32)
    img[:] = (0.53, 0.70, 0.90)
    zbuf = np.full((H, W), -np.inf)
    sun = np.array([0.4, 0.3, 0.86])
    sun /= np.linalg.norm(sun)
    n_tris = 0
    # translucent then additive surfaces are drawn last, without depth writes
    tris_world.sort(key=lambda t: (t[7], t[8]))
    for tri, uv, col, tex, factor, mask, cutoff, additive, translucent in tris_world:
        if cut is not None:
            keep = tri[:, :, 2].max(1) <= cut
            tri = tri[keep]
            uv = uv[keep] if uv is not None else None
            col = col[keep] if col is not None else None
        if not len(tri):
            continue
        fn = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        ln = np.linalg.norm(fn, axis=1)
        fn = fn / np.maximum(ln, 1e-12)[:, None]
        shade = 0.45 + 0.55 * np.abs(fn @ sun)
        p = proj(tri)
        sx = (p[..., 0] - plo[0]) * scale + 10
        sy = (phi[1] - p[..., 1]) * scale + 10
        sz = p[..., 2]
        for i in range(len(tri)):
            x0, x1 = int(max(sx[i].min(), 0)), int(min(sx[i].max(), W - 1)) + 1
            y0, y1 = int(max(sy[i].min(), 0)), int(min(sy[i].max(), H - 1)) + 1
            if x1 <= x0 or y1 <= y0:
                continue
            xs, ys = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
            (ax, bx, cx), (ay, by, cy) = sx[i], sy[i]
            den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
            if abs(den) < 1e-9:
                continue
            w0 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / den
            w1 = ((cy - ay) * (xs - cx) + (ax - cx) * (ys - cy)) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any():
                continue
            z = w0 * sz[i, 0] + w1 * sz[i, 1] + w2 * sz[i, 2]
            zb = zbuf[y0:y1, x0:x1]
            vis = inside & (z > zb)
            if not vis.any():
                continue
            c = np.ones(vis.shape + (3,)) * factor
            alpha = None
            if tex is not None and uv is not None:
                u = w0 * uv[i, 0, 0] + w1 * uv[i, 1, 0] + w2 * uv[i, 2, 0]
                v = w0 * uv[i, 0, 1] + w1 * uv[i, 1, 1] + w2 * uv[i, 2, 1]
                th, tw = tex.shape[:2]
                tx = (np.mod(u, 1.0) * tw).astype(int).clip(0, tw - 1)
                ty = (np.mod(v, 1.0) * th).astype(int).clip(0, th - 1)
                texel = tex[ty, tx]
                if mask:
                    vis &= texel[..., 3] >= cutoff
                alpha = texel[..., 3]
                c = c * texel[..., :3]
            if col is not None:
                vc = (w0[..., None] * col[i, 0] + w1[..., None] * col[i, 1] + w2[..., None] * col[i, 2])
                c = c * np.clip(vc * 1.6, 0, 1.6)
            if additive:
                img[y0:y1, x0:x1][vis] += c[vis]
                n_tris += 1
                continue
            c = c * shade[i]
            if translucent:
                a = (alpha if alpha is not None else np.ones(vis.shape))[..., None]
                region = img[y0:y1, x0:x1]
                region[vis] = (region * (1 - a) + c * a)[vis]
                n_tris += 1
                continue
            zb[vis] = z[vis]
            img[y0:y1, x0:x1][vis] = c[vis]
            n_tris += 1
    rgba = np.concatenate([np.clip(img, 0, 1), np.ones((H, W, 1), np.float32)], axis=2)
    out_path = out_path or os.path.join(out_dir, "previews", f"{'_'.join(scenes)[:60]}_{view}.png")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    write_png(out_path, W, H, (rgba * 255).astype(np.uint8).tobytes(), keep_alpha=False)
    log(f"preview {out_path}: {W}x{H}, {n_tris} triangles drawn, {len(models)} models")
    return out_path
