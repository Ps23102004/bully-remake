# OWNER: builder-A2 (animation)
"""CPU contact-sheet renderer for a skinned glTF (no GPU, no engine).

Reads the glTF exactly as a consumer would: node TRS, the skin's joints and
inverse bind matrices, JOINTS_0/WEIGHTS_0, and animation samplers (LINEAR,
slerp for rotations). Poses the mesh at sampled times and rasterises
textured, lambert-shaded triangles into one PNG grid: one row per
animation (plus the bind pose), N frames per row, front or side view.
Evidence only; needs numpy (and Pillow for textures).
"""

from __future__ import annotations

import json
import math
import os

import numpy as np

from .gltf import TYPE_COUNT
from .textures import write_png

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

DT = {5126: "<f4", 5123: "<u2", 5125: "<u4", 5121: "u1"}


def _acc(doc, bins, idx):
    a = doc["accessors"][idx]
    v = doc["bufferViews"][a["bufferView"]]
    n = TYPE_COUNT[a["type"]]
    arr = np.frombuffer(bins[v["buffer"]], dtype=DT[a["componentType"]], count=a["count"] * n,
                        offset=v.get("byteOffset", 0) + a.get("byteOffset", 0))
    return arr.reshape(a["count"], n) if n > 1 else arr


def _quat_mat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def _slerp(a, b, u):
    c = float(np.dot(a, b))
    if c < 0:
        b, c = -b, -c
    if c > 0.9995:
        r = a + u * (b - a)
    else:
        th = math.acos(c)
        r = (math.sin((1 - u) * th) * a + math.sin(u * th) * b) / math.sin(th)
    return r / np.linalg.norm(r)


def _sample(times, values, t, rot):
    if t <= times[0]:
        return values[0]
    if t >= times[-1]:
        return values[-1]
    i = int(np.searchsorted(times, t)) - 1
    u = (t - times[i]) / (times[i + 1] - times[i])
    return _slerp(values[i], values[i + 1], u) if rot else values[i] + u * (values[i + 1] - values[i])


class SkinnedModel:
    def __init__(self, path):
        self.folder = os.path.dirname(path)
        with open(path) as f:
            self.doc = doc = json.load(f)
        self.bins = [open(os.path.join(self.folder, b["uri"]), "rb").read() for b in doc["buffers"]]
        self.nodes = doc["nodes"]
        self.parent = {}
        for i, n in enumerate(self.nodes):
            for c in n.get("children", []):
                self.parent[c] = i
        self.anims = doc.get("animations", [])
        self.textures = {}

    def local(self, i, overrides):
        n = self.nodes[i]
        t = np.array(overrides.get((i, "translation"), n.get("translation", [0, 0, 0])), float)
        r = np.array(overrides.get((i, "rotation"), n.get("rotation", [0, 0, 0, 1])), float)
        s = np.array(n.get("scale", [1, 1, 1]), float)
        m = np.eye(4)
        m[:3, :3] = _quat_mat(r) * s
        m[:3, 3] = t
        return m

    def world_mats(self, overrides):
        cache = {}

        def w(i):
            if i not in cache:
                m = self.local(i, overrides)
                cache[i] = w(self.parent[i]) @ m if i in self.parent else m
            return cache[i]
        return [w(i) for i in range(len(self.nodes))]

    def pose(self, anim_index, t):
        ov = {}
        if anim_index is None:
            return ov
        an = self.anims[anim_index]
        for ch in an["channels"]:
            s = an["samplers"][ch["sampler"]]
            times = _acc(self.doc, self.bins, s["input"]).astype(float)
            vals = _acc(self.doc, self.bins, s["output"]).astype(float)
            path = ch["target"]["path"]
            ov[(ch["target"]["node"], path)] = _sample(times, vals, t, path == "rotation")
        return ov

    def duration(self, anim_index):
        an = self.anims[anim_index]
        return max(float(_acc(self.doc, self.bins, s["input"]).max()) for s in an["samplers"])

    def texture(self, prim):
        mat = self.doc["materials"][prim["material"]] if "material" in prim else {}
        bc = mat.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if bc is None or Image is None:
            return None
        uri = self.doc["images"][self.doc["textures"][bc["index"]]["source"]]["uri"]
        p = os.path.normpath(os.path.join(self.folder, uri))
        if p not in self.textures:
            self.textures[p] = None
            if os.path.exists(p):
                with Image.open(p) as im:
                    self.textures[p] = np.asarray(im.convert("RGB"), dtype=np.float32) / 255.0
        return self.textures[p]

    def skinned_triangles(self, overrides):
        """-> list of (tri positions (n,3,3) glTF space, uv (n,3,2)|None, texture)."""
        worlds = self.world_mats(overrides)
        out = []
        for ni, n in enumerate(self.nodes):
            if "mesh" not in n:
                continue
            mesh = self.doc["meshes"][n["mesh"]]
            for prim in mesh["primitives"]:
                at = prim["attributes"]
                pos = _acc(self.doc, self.bins, at["POSITION"]).astype(float)
                if "skin" in n and "JOINTS_0" in at:
                    sk = self.doc["skins"][n["skin"]]
                    ibm = _acc(self.doc, self.bins, sk["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
                    jm = np.stack([worlds[j] @ ibm[k] for k, j in enumerate(sk["joints"])])
                    J = _acc(self.doc, self.bins, at["JOINTS_0"]).astype(int)
                    W = _acc(self.doc, self.bins, at["WEIGHTS_0"]).astype(float)
                    M = np.einsum("vk,vkij->vij", W, jm[J])
                    hp = np.concatenate([pos, np.ones((len(pos), 1))], 1)
                    pos = np.einsum("vij,vj->vi", M, hp)[:, :3]
                else:
                    m = worlds[ni]
                    pos = pos @ m[:3, :3].T + m[:3, 3]
                idx = _acc(self.doc, self.bins, prim["indices"]).astype(int).reshape(-1, 3)
                uv = _acc(self.doc, self.bins, at["TEXCOORD_0"])[idx] if "TEXCOORD_0" in at else None
                out.append((pos[idx], uv, self.texture(prim)))
        return out


def _raster(tris, W, H, proj, img, zbuf, light):
    for tri, uv, tex in tris:
        p = proj(tri)
        fn = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        fn /= np.maximum(np.linalg.norm(fn, axis=1), 1e-12)[:, None]
        shade = 0.35 + 0.65 * np.clip(fn @ light, 0, 1)
        for i in range(len(tri)):
            sx, sy, sz = p[i, :, 0], p[i, :, 1], p[i, :, 2]
            x0, x1 = int(max(sx.min(), 0)), int(min(sx.max(), W - 1)) + 1
            y0, y1 = int(max(sy.min(), 0)), int(min(sy.max(), H - 1)) + 1
            if x1 <= x0 or y1 <= y0:
                continue
            xs, ys = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
            (ax, bx, cx), (ay, by, cy) = sx, sy
            den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
            if abs(den) < 1e-12:
                continue
            w0 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / den
            w1 = ((cy - ay) * (xs - cx) + (ax - cx) * (ys - cy)) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2]
            zb = zbuf[y0:y1, x0:x1]
            vis = inside & (z > zb)
            if not vis.any():
                continue
            c = np.full(vis.shape + (3,), 0.8)
            if tex is not None and uv is not None:
                u = w0 * uv[i, 0, 0] + w1 * uv[i, 1, 0] + w2 * uv[i, 2, 0]
                v = w0 * uv[i, 0, 1] + w1 * uv[i, 1, 1] + w2 * uv[i, 2, 1]
                th, tw = tex.shape[:2]
                c = tex[(np.mod(v, 1) * th).astype(int).clip(0, th - 1), (np.mod(u, 1) * tw).astype(int).clip(0, tw - 1)]
            zb[vis] = z[vis]
            img[y0:y1, x0:x1][vis] = (c * shade[i])[vis]


def contact_sheet(gltf_path, out_png, anims=None, frames=6, cell=220, view="side", log=print):
    m = SkinnedModel(gltf_path)
    names = [a["name"] for a in m.anims]
    rows = [None] + [names.index(a) for a in (anims or names)]
    # glTF space: +Y up. The game's forward (+y) is glTF -Z. "side" looks along -X
    # (character walks to the left of the image), "front" looks at the face.
    if view == "side":   # camera on +X: screen x = glTF z, depth = +x
        axes, sx_sign, depth_sign = (2, 1, 0), 1.0, 1.0
    else:                # camera in front (-Z, the game's +y): screen x = -x, depth = -z
        axes, sx_sign, depth_sign = (0, 1, 2), -1.0, -1.0
    span = 2.4  # metres shown per cell
    scale = cell / span
    W, H = cell * frames, cell * len(rows)
    img = np.empty((H, W, 3), np.float32)
    img[:] = (0.86, 0.88, 0.92)
    light = np.array([0.3, 0.8, 0.5])
    light /= np.linalg.norm(light)
    for r, ai in enumerate(rows):
        dur = m.duration(ai) if ai is not None else 0.0
        for f in range(frames):
            t = dur * f / max(frames - 1, 1) if ai is not None else 0.0
            tris = m.skinned_triangles(m.pose(ai, t))
            ox, oy = f * cell, r * cell
            zbuf = np.full((cell, cell), -np.inf)
            sub = img[oy:oy + cell, ox:ox + cell]
            # ground line at y = 0 (feet), cell bottom 0.15 m below it
            gy = int(cell - 0.15 * scale)
            sub[gy:gy + 1, :] = (0.5, 0.5, 0.5)

            def proj(v, ox=ox):
                sx = sx_sign * v[..., axes[0]] * scale + cell / 2
                sy = cell - (v[..., axes[1]] + 0.15) * scale
                return np.stack([sx, sy, depth_sign * v[..., axes[2]]], -1)
            _raster(tris, cell, cell, proj, sub, zbuf, light)
    rgba = np.concatenate([np.clip(img, 0, 1), np.ones((H, W, 1), np.float32)], 2)
    os.makedirs(os.path.dirname(os.path.abspath(out_png)), exist_ok=True)
    write_png(out_png, W, H, (rgba * 255).astype(np.uint8).tobytes(), keep_alpha=False)
    log(f"{out_png}: rows {['bind'] + [names[a] for a in rows[1:]]}, {frames} frames each, view {view}")
    return out_png
