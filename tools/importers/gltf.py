# OWNER: builder-A (importers)
"""Minimal glTF 2.0 writer (.gltf JSON + one external .bin) and validator.

Only what the importers need: static triangle meshes, materials with
textures referenced by relative URI, node matrices. No external dependency.
"""

from __future__ import annotations

import json
import math
import os
import struct

FLOAT = 5126
UBYTE = 5121
USHORT = 5123
UINT = 5125
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
COMPONENT_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
TYPE_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
REPEAT, CLAMP = 10497, 33071
LINEAR, LINEAR_MIPMAP_LINEAR, NEAREST = 9729, 9987, 9728


class GltfBuilder:
    def __init__(self, generator: str = "bully-remake tools/importers"):
        self.doc = {
            "asset": {"version": "2.0", "generator": generator},
            "scene": 0,
            "scenes": [{"nodes": []}],
            "nodes": [],
            "meshes": [],
            "materials": [],
            "textures": [],
            "images": [],
            "samplers": [],
            "accessors": [],
            "bufferViews": [],
            "buffers": [],
        }
        self.bin = bytearray()
        self._images: dict[str, int] = {}
        self._samplers: dict[tuple, int] = {}
        self._textures: dict[tuple, int] = {}

    # -- buffers ----------------------------------------------------------
    def _view(self, payload: bytes, target: int | None) -> int:
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(payload)}
        if target:
            view["target"] = target
        self.bin += payload
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def add_floats(self, values, type_: str, minmax: bool = False, vertex: bool | None = None) -> int:
        """`vertex`: mark the buffer view as vertex data (ARRAY_BUFFER).
        Default: True for VEC2/VEC3/VEC4, False for SCALAR/MAT4 (animation
        times, inverse bind matrices must not carry a target)."""
        n = TYPE_COUNT[type_]
        if len(values) % n:
            raise ValueError("float data not a multiple of component count")
        if vertex is None:
            vertex = type_ in ("VEC2", "VEC3", "VEC4")
        count = len(values) // n
        payload = struct.pack(f"<{len(values)}f", *values)
        acc = {"bufferView": self._view(payload, ARRAY_BUFFER if vertex else None), "componentType": FLOAT,
               "count": count, "type": type_}
        if minmax and count:
            acc["min"] = [min(values[k::n]) for k in range(n)]
            acc["max"] = [max(values[k::n]) for k in range(n)]
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    def add_uints(self, values, type_: str, ctype: int = USHORT) -> int:
        """Unsigned integer vertex attribute (JOINTS_0): UNSIGNED_BYTE or
        UNSIGNED_SHORT components."""
        n = TYPE_COUNT[type_]
        if len(values) % n:
            raise ValueError("int data not a multiple of component count")
        code = {UBYTE: "B", USHORT: "H"}[ctype]
        payload = struct.pack(f"<{len(values)}{code}", *values)
        acc = {"bufferView": self._view(payload, ARRAY_BUFFER), "componentType": ctype,
               "count": len(values) // n, "type": type_}
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    def add_indices(self, indices) -> int:
        big = max(indices) > 0xFFFF if indices else False
        ctype, code = (UINT, "I") if big else (USHORT, "H")
        payload = struct.pack(f"<{len(indices)}{code}", *indices)
        acc = {"bufferView": self._view(payload, ELEMENT_ARRAY_BUFFER), "componentType": ctype,
               "count": len(indices), "type": "SCALAR"}
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    # -- materials --------------------------------------------------------
    def sampler(self, wrap_s: int = REPEAT, wrap_t: int = REPEAT) -> int:
        key = (wrap_s, wrap_t)
        if key not in self._samplers:
            self.doc["samplers"].append({"magFilter": LINEAR, "minFilter": LINEAR_MIPMAP_LINEAR,
                                         "wrapS": wrap_s, "wrapT": wrap_t})
            self._samplers[key] = len(self.doc["samplers"]) - 1
        return self._samplers[key]

    def texture(self, uri: str, wrap_s: int = REPEAT, wrap_t: int = REPEAT) -> int:
        if uri not in self._images:
            self.doc["images"].append({"uri": uri})
            self._images[uri] = len(self.doc["images"]) - 1
        key = (self._images[uri], self.sampler(wrap_s, wrap_t))
        if key not in self._textures:
            self.doc["textures"].append({"source": key[0], "sampler": key[1]})
            self._textures[key] = len(self.doc["textures"]) - 1
        return self._textures[key]

    def material(self, mat: dict) -> int:
        self.doc["materials"].append(mat)
        return len(self.doc["materials"]) - 1

    # -- scene ------------------------------------------------------------
    def mesh(self, name: str, primitives: list) -> int:
        self.doc["meshes"].append({"name": name, "primitives": primitives})
        return len(self.doc["meshes"]) - 1

    def node(self, node: dict, root: bool = True) -> int:
        self.doc["nodes"].append(node)
        idx = len(self.doc["nodes"]) - 1
        if root:
            self.doc["scenes"][0]["nodes"].append(idx)
        return idx

    def write(self, gltf_path: str) -> tuple:
        base = os.path.splitext(os.path.basename(gltf_path))[0]
        bin_name = base + ".bin"
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc["buffers"] = [{"uri": bin_name, "byteLength": len(self.bin)}]
        doc = {k: v for k, v in self.doc.items() if v != [] or k in ("nodes", "scenes")}
        with open(os.path.join(os.path.dirname(gltf_path), bin_name), "wb") as f:
            f.write(self.bin)
        with open(gltf_path, "w") as f:
            json.dump(doc, f, separators=(",", ":"))
        return gltf_path, len(self.bin)


# ---------------------------------------------------------------------------
# Structural validation (used by tests and by the `validate` CLI command)
# ---------------------------------------------------------------------------

def validate_gltf(path: str, check_images: bool = True) -> list:
    """Return a list of problems (empty = valid for our subset of glTF)."""
    problems = []
    folder = os.path.dirname(path)
    with open(path) as f:
        doc = json.load(f)
    if doc.get("asset", {}).get("version") != "2.0":
        problems.append("asset.version != 2.0")
    buffers = []
    for i, b in enumerate(doc.get("buffers", [])):
        p = os.path.join(folder, b["uri"])
        if not os.path.exists(p):
            problems.append(f"buffer {i}: missing file {b['uri']}")
            buffers.append(b"")
            continue
        data = open(p, "rb").read()
        if len(data) != b["byteLength"]:
            problems.append(f"buffer {i}: byteLength {b['byteLength']} != file size {len(data)}")
        buffers.append(data)
    views = doc.get("bufferViews", [])
    for i, v in enumerate(views):
        if v["buffer"] >= len(buffers):
            problems.append(f"bufferView {i}: bad buffer")
            continue
        if v.get("byteOffset", 0) + v["byteLength"] > len(buffers[v["buffer"]]):
            problems.append(f"bufferView {i}: runs past buffer end")
    accessors = doc.get("accessors", [])
    for i, a in enumerate(accessors):
        if a["bufferView"] >= len(views):
            problems.append(f"accessor {i}: bad bufferView")
            continue
        v = views[a["bufferView"]]
        need = a.get("byteOffset", 0) + a["count"] * COMPONENT_SIZE[a["componentType"]] * TYPE_COUNT[a["type"]]
        if need > v["byteLength"]:
            problems.append(f"accessor {i}: needs {need} bytes, view has {v['byteLength']}")
        if a["componentType"] == FLOAT and v["byteLength"] and a["bufferView"] < len(views):
            buf = buffers[v["buffer"]]
            start = v.get("byteOffset", 0) + a.get("byteOffset", 0)
            n = a["count"] * TYPE_COUNT[a["type"]]
            if start + 4 * n <= len(buf):
                vals = struct.unpack_from(f"<{n}f", buf, start)
                if any(math.isnan(x) or math.isinf(x) for x in vals):
                    problems.append(f"accessor {i}: NaN/Inf values")
    for mi, m in enumerate(doc.get("meshes", [])):
        for pi, p in enumerate(m["primitives"]):
            attrs = p["attributes"]
            if "POSITION" not in attrs:
                problems.append(f"mesh {mi} prim {pi}: no POSITION")
                continue
            pos = accessors[attrs["POSITION"]]
            if "min" not in pos or "max" not in pos:
                problems.append(f"mesh {mi} prim {pi}: POSITION without min/max")
            for k, ai in attrs.items():
                if accessors[ai]["count"] != pos["count"]:
                    problems.append(f"mesh {mi} prim {pi}: {k} count != POSITION count")
            if "indices" in p:
                ia = accessors[p["indices"]]
                if ia["count"] % 3:
                    problems.append(f"mesh {mi} prim {pi}: index count not a multiple of 3")
                v = views[ia["bufferView"]]
                buf = buffers[v["buffer"]]
                code = "H" if ia["componentType"] == USHORT else "I"
                idx = struct.unpack_from(f"<{ia['count']}{code}", buf, v.get("byteOffset", 0) + ia.get("byteOffset", 0))
                if idx and max(idx) >= pos["count"]:
                    problems.append(f"mesh {mi} prim {pi}: index {max(idx)} >= vertex count {pos['count']}")
            if "material" in p and p["material"] >= len(doc.get("materials", [])):
                problems.append(f"mesh {mi} prim {pi}: bad material index")
    for ti, t in enumerate(doc.get("textures", [])):
        if t["source"] >= len(doc.get("images", [])):
            problems.append(f"texture {ti}: bad image index")
    if check_images:
        for ii, im in enumerate(doc.get("images", [])):
            if "uri" in im and not os.path.exists(os.path.join(folder, im["uri"])):
                problems.append(f"image {ii}: missing {im['uri']}")
    for ni, n in enumerate(doc.get("nodes", [])):
        if "mesh" in n and n["mesh"] >= len(doc.get("meshes", [])):
            problems.append(f"node {ni}: bad mesh index")
        if "matrix" in n and len(n["matrix"]) != 16:
            problems.append(f"node {ni}: matrix is not 16 floats")
        if "skin" in n and n["skin"] >= len(doc.get("skins", [])):
            problems.append(f"node {ni}: bad skin index")
        if "rotation" in n:
            q = n["rotation"]
            if len(q) != 4 or abs(math.sqrt(sum(c * c for c in q)) - 1.0) > 1e-3:
                problems.append(f"node {ni}: rotation is not a unit quaternion")
    problems += _validate_skins_animations(doc, views, accessors, buffers)
    return problems


def read_accessor(doc, buffers, idx) -> tuple:
    """Flat tuple of an accessor's components (float, ubyte or ushort)."""
    a = doc["accessors"][idx]
    v = doc["bufferViews"][a["bufferView"]]
    code = {FLOAT: "f", USHORT: "H", UINT: "I", UBYTE: "B"}[a["componentType"]]
    n = a["count"] * TYPE_COUNT[a["type"]]
    return struct.unpack_from(f"<{n}{code}", buffers[v["buffer"]], v.get("byteOffset", 0) + a.get("byteOffset", 0))


def _validate_skins_animations(doc, views, accessors, buffers) -> list:
    problems = []
    nodes = doc.get("nodes", [])
    skins = doc.get("skins", [])
    for si, sk in enumerate(skins):
        joints = sk.get("joints", [])
        if not joints or any(j >= len(nodes) for j in joints):
            problems.append(f"skin {si}: bad joint list")
            continue
        if "inverseBindMatrices" in sk:
            a = accessors[sk["inverseBindMatrices"]]
            if a["type"] != "MAT4" or a["count"] != len(joints):
                problems.append(f"skin {si}: inverseBindMatrices count {a['count']} != joints {len(joints)}")
            if views[a["bufferView"]].get("target"):
                problems.append(f"skin {si}: inverseBindMatrices bufferView must not have a target")
    for mi, m in enumerate(doc.get("meshes", [])):
        users = [n for n in nodes if n.get("mesh") == mi]
        for pi, p in enumerate(m["primitives"]):
            at = p["attributes"]
            if ("JOINTS_0" in at) != ("WEIGHTS_0" in at):
                problems.append(f"mesh {mi} prim {pi}: JOINTS_0 without WEIGHTS_0 or vice versa")
                continue
            if "JOINTS_0" not in at:
                continue
            skinned_users = [n for n in users if "skin" in n]
            if not skinned_users:
                problems.append(f"mesh {mi}: has JOINTS_0 but no node with a skin uses it")
                continue
            nj = len(skins[skinned_users[0]["skin"]]["joints"]) if skinned_users[0]["skin"] < len(skins) else 0
            jv = read_accessor(doc, buffers, at["JOINTS_0"])
            wv = read_accessor(doc, buffers, at["WEIGHTS_0"])
            if jv and max(jv) >= nj:
                problems.append(f"mesh {mi} prim {pi}: joint index {max(jv)} >= {nj} joints")
            bad = 0
            for k in range(0, len(wv), 4):
                if abs(sum(wv[k:k + 4]) - 1.0) > 2e-3 or min(wv[k:k + 4]) < 0:
                    bad += 1
            if bad:
                problems.append(f"mesh {mi} prim {pi}: {bad} vertices whose weights do not sum to 1")
    for ai, an in enumerate(doc.get("animations", [])):
        samplers = an.get("samplers", [])
        for ci, ch in enumerate(an.get("channels", [])):
            t = ch.get("target", {})
            if t.get("node", -1) >= len(nodes) or ch["sampler"] >= len(samplers):
                problems.append(f"animation {ai} channel {ci}: bad node or sampler")
                continue
            s = samplers[ch["sampler"]]
            ia, oa = accessors[s["input"]], accessors[s["output"]]
            want = {"rotation": "VEC4", "translation": "VEC3", "scale": "VEC3"}.get(t.get("path"))
            if want is None or oa["type"] != want:
                problems.append(f"animation {ai} channel {ci}: path {t.get('path')} with output {oa['type']}")
            if ia["count"] != oa["count"]:
                problems.append(f"animation {ai} channel {ci}: {ia['count']} times, {oa['count']} values")
            if "min" not in ia or "max" not in ia:
                problems.append(f"animation {ai} sampler {ch['sampler']}: input without min/max")
            times = read_accessor(doc, buffers, s["input"])
            if any(b <= a for a, b in zip(times, times[1:])):
                problems.append(f"animation {ai} sampler {ch['sampler']}: times not strictly increasing")
            if t.get("path") == "rotation":
                q = read_accessor(doc, buffers, s["output"])
                if any(abs(math.sqrt(sum(c * c for c in q[k:k + 4])) - 1.0) > 1e-3 for k in range(0, len(q), 4)):
                    problems.append(f"animation {ai} channel {ci}: non-unit rotation")
    return problems
