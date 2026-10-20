# OWNER: builder-A2 (animation)
"""Skinned character export: a pedestrian NIF (PLAYER.nif = Jimmy) to a
glTF 2.0 skinned mesh with its skeleton, plus .agr clips as glTF
animations on that skeleton.

Skeleton. Every NiNode under the model's skeleton (the "Dummy" subtree for a
pedestrian) becomes a glTF joint, in NIF file order (depth first). For a
pedestrian that order IS the .agr track order (36 tracks: Dummy, Root,
Root Pelvis, ... Right_Shoulder, ARROW; bully-re docs/agr.md, re-checked
here: every track's first key equals the node's stored rotation in at least
one C_Player clip, to 0.12 deg). Node transforms are converted from game
space (Z up) to glTF space (Y up) by conjugation with C = (x, y, z) ->
(x, z, -y), which keeps local rotations proper rotations.

Skinning. Gamebryo: v_model = sum_b w_b * M_b * S_b * v, with M_b the bone's
model-space transform and S_b the NiSkinData per-bone transform (bully-re
docs/nif.md). glTF: v = sum_b w_b * J_b * IBM_b * v'. We write v' = W v
(the shape's model-space position, W = the shape's node transform, the same
vertices the static exporter writes), so IBM_b = S_b * W^-1. All shapes are
put under ONE glTF skin when their IBMs agree (they do on PLAYER.nif); a
shape whose IBMs disagree gets its own skin. Joints not used by any skin
get IBM = inverse of their file-pose model transform.

Entity space: like convert.py, the root and its direct children are
identity (the game replaces the model node with the placement).

Animation. A clip's rotation keys REPLACE the joint's local rotation; key
translations / position records REPLACE the local translation (absolute in
the parent frame). Times: key fraction * HXD play duration.
Root motion modes:
  inplace (default): what the game renders. The ground arrow (track 35) is
      the character's travel; the pelvis track (Root, 1) carries the same
      travel. We subtract the arrow travel from Root and hold ARROW at its
      start, so the clip plays in place; the travel is kept in the
      animation extras (`root_motion`) for the movement code.
  raw: tracks exactly as stored.
  dummy: like inplace, but the arrow travel is put on the skeleton root
      joint (Dummy), for engines that extract root motion from the root bone.
"""

from __future__ import annotations

import math
import os
import struct
from dataclasses import dataclass, field

from . import coords
from .agr import Clip, quat_to_rows, rows_to_quat, sample_position, sample_rotation
from .binreader import FormatError
from .gltf import GltfBuilder
from .nft import safe_name
from .nif import NifFile, parse_nif

GEOMETRY = ("NiTriShape", "NiTriStrips")
ROOT_TRACK = 1    # "Root": the pelvis translation track
ARROW_TRACK = 35  # "ARROW": the ground arrow, the character's travel
ROOT_MOTION_MODES = ("inplace", "raw", "dummy")


# ---------------------------------------------------------------------------
# 4x4 helpers (row lists, column-vector convention: v' = M v)
# ---------------------------------------------------------------------------

def m4_from_xform(x: coords.Xform) -> list:
    r, s, t = x.r, x.s, x.t
    return [[r[0][0] * s, r[0][1] * s, r[0][2] * s, t[0]],
            [r[1][0] * s, r[1][1] * s, r[1][2] * s, t[1]],
            [r[2][0] * s, r[2][1] * s, r[2][2] * s, t[2]],
            [0.0, 0.0, 0.0, 1.0]]


def m4_mul(a, b) -> list:
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def m4_inv_affine(m) -> list:
    """Inverse of an affine 4x4 whose 3x3 part is s * rotation (uniform
    scale). Uses the general 3x3 inverse so slightly non-orthonormal
    matrices are still inverted exactly."""
    a = [row[:3] for row in m[:3]]
    det = coords.det3(a)
    if abs(det) < 1e-12:
        raise FormatError("singular bone transform")
    inv = [[(a[(j + 1) % 3][(i + 1) % 3] * a[(j + 2) % 3][(i + 2) % 3]
             - a[(j + 1) % 3][(i + 2) % 3] * a[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)]
           for i in range(3)]
    t = [m[0][3], m[1][3], m[2][3]]
    it = [-sum(inv[i][k] * t[k] for k in range(3)) for i in range(3)]
    return [inv[0] + [it[0]], inv[1] + [it[1]], inv[2] + [it[2]], [0.0, 0.0, 0.0, 1.0]]


def m4_apply(m, v) -> tuple:
    return tuple(m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3] for i in range(3))


# game -> glTF basis change, as a 4x4 (C) and its inverse (C^T)
C4 = [[1.0, 0.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, -1.0, 0.0, 0.0], [0.0, 0.0, 0.0, 1.0]]
C4T = [[C4[j][i] for j in range(4)] for i in range(4)]


def to_gltf_m4(m) -> list:
    return m4_mul(m4_mul(C4, m), C4T)


def m4_colmajor(m) -> list:
    return [m[r][c] for c in range(4) for r in range(4)]


def quat_game_to_gltf(q) -> list:
    """Game (w, x, y, z) -> glTF [x, y, z, w] for the conjugated rotation
    C R C^T: the axis is mapped like a vector, (x, y, z) -> (x, z, -y)."""
    w, x, y, z = q
    return [x, z, -y, w]


def vec_game_to_gltf(v) -> list:
    return [v[0], v[2], -v[1]]


def _max_abs_diff(a, b) -> float:
    return max(abs(a[i][j] - b[i][j]) for i in range(4) for j in range(4))


# ---------------------------------------------------------------------------
# Skeleton
# ---------------------------------------------------------------------------

@dataclass
class Joint:
    block: int
    name: str
    parent: int          # joint index, -1 for the skeleton root
    local: coords.Xform  # game space, entity rule applied
    world: list = None   # 4x4 game model space (file pose)


@dataclass
class PedResult:
    name: str
    gltf: str | None = None
    joints: int = 0
    shapes: int = 0
    vertices: int = 0
    triangles: int = 0
    skins: int = 0
    animations: int = 0
    max_influences: int = 0
    unweighted_vertices: int = 0
    weight_sum_max_error: float = 0.0
    bind_vs_stored_max_m: float = 0.0       # skinned bind pose vs stored vertices
    bind_vs_file_pose_max: float = 0.0      # IBM vs inverse(file pose), matrix max abs diff
    skipped_shapes: list = field(default_factory=list)
    clips: list = field(default_factory=list)
    warnings: list = field(default_factory=list)
    missing_textures: list = field(default_factory=list)


def _node_xform(d: dict, depth: int, entity_space: bool) -> coords.Xform:
    if entity_space and depth <= 1:
        return coords.Xform()
    return coords.Xform.from_av(d)


def find_skeleton_root(nif: NifFile) -> int:
    """The skeleton is the subtree of the root's child that contains every
    skin bone. For pedestrians that is 'Dummy'."""
    bones = set()
    for b in nif.of_type("NiSkinInstance"):
        bones.update(b.data["bones"])
    root = (nif.roots or [0])[0]
    rd = nif.data(root, "NiNode")
    if rd is None:
        raise FormatError("NIF root is not a NiNode")

    def subtree(ref, acc):
        b = nif.block(ref)
        if b is None or b.type_name != "NiNode" or b.data is None:
            return acc
        acc.add(ref)
        for c in b.data["children"]:
            subtree(c, acc)
        return acc
    for c in rd["children"]:
        st = subtree(c, set())
        if bones and bones <= st:
            return c
    raise FormatError("no child of the NIF root contains every skin bone")


def build_skeleton(nif: NifFile, skel_root: int, entity_space: bool = True) -> list:
    joints: list[Joint] = []
    root = (nif.roots or [0])[0]

    def depth_of(ref):  # skel_root is a child of the NIF root -> depth 1
        return 1 if ref == skel_root else None

    def visit(ref, parent_idx, depth, parent_world):
        b = nif.block(ref)
        if b is None or b.type_name != "NiNode" or b.data is None:
            return
        d = b.data
        local = _node_xform(d, depth, entity_space)
        world = m4_mul(parent_world, m4_from_xform(local))
        idx = len(joints)
        joints.append(Joint(ref, nif.string(d["name"]) or f"#{ref}", parent_idx, local, world))
        for c in d["children"]:
            visit(c, idx, depth + 1, world)
    base = m4_from_xform(coords.Xform() if entity_space else coords.Xform.from_av(nif.data(root, "NiNode")))
    visit(skel_root, -1, 1, base)
    return joints


def _shape_world(nif: NifFile, shape_ref: int, entity_space: bool) -> list | None:
    """Model-space transform of a shape (entity rule), found by walking the
    tree from the root."""
    root = (nif.roots or [0])[0]
    found = []

    def visit(ref, depth, world):
        b = nif.block(ref)
        if b is None or b.data is None:
            return
        if b.type_name == "NiNode" or b.type_name in GEOMETRY:
            local = _node_xform(b.data, depth, entity_space)
            w = m4_mul(world, m4_from_xform(local))
            if ref == shape_ref:
                found.append(w)
                return
            for c in b.data.get("children", []):
                visit(c, depth + 1, w)
    visit(root, 0, [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)])
    return found[0] if found else None


def _shape_props(nif: NifFile, shape_ref: int) -> tuple:
    """Effective (inherited) properties and node path of a shape."""
    from .convert import _effective_props
    root = (nif.roots or [0])[0]
    out = []

    def visit(ref, inherited, path):
        b = nif.block(ref)
        if b is None or b.data is None:
            return
        props = _effective_props(nif, inherited, b.data.get("properties", []))
        if ref == shape_ref:
            out.append((props, path))
            return
        if b.type_name == "NiNode":
            for c in b.data["children"]:
                visit(c, props, path + [nif.string(b.data["name"]) or f"#{ref}"])
    visit(root, {}, [])
    return out[0] if out else ({}, [])


# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------

@dataclass
class ClipRequest:
    label: str           # glTF animation name
    clip: Clip
    duration: float      # play duration (HXD); key times are fractions of it
    meta: dict = field(default_factory=dict)


def export_ped(name: str, data, txd: str | None, store, out_dir: str, clips: list = (),
               root_motion: str = "inplace", entity_space: bool = True,
               bone_track_check: bool = True, head: tuple | None = None) -> PedResult:
    """`head` = (model name, NIF bytes, txd) of a clothing "head" model (hair): see _HEAD_MODEL below."""
    if root_motion not in ROOT_MOTION_MODES:
        raise ValueError(f"root_motion must be one of {ROOT_MOTION_MODES}")
    res = PedResult(name)
    nif = parse_nif(data)
    failed = nif.failed_blocks()
    if failed:
        raise FormatError(f"{len(failed)} blocks failed to decode, first: #{failed[0].index} "
                          f"{failed[0].type_name}: {failed[0].error}")
    skel_root = find_skeleton_root(nif)
    joints = build_skeleton(nif, skel_root, entity_space)
    jidx = {j.block: i for i, j in enumerate(joints)}
    res.joints = len(joints)

    gb = GltfBuilder()
    doc = gb.doc
    # glTF nodes: one model root (identity), then the joints in order
    model_node = gb.node({"name": name, "children": []})
    first_joint_node = len(doc["nodes"])
    for j in joints:
        q = quat_game_to_gltf(rows_to_quat(j.local.r))
        n = {"name": j.name, "translation": vec_game_to_gltf(j.local.t), "rotation": q}
        if abs(j.local.s - 1.0) > 1e-6:
            n["scale"] = [j.local.s] * 3
        gb.node(n, root=False)
    for i, j in enumerate(joints):
        if j.parent < 0:
            doc["nodes"][model_node]["children"].append(first_joint_node + i)
        else:
            doc["nodes"][first_joint_node + j.parent].setdefault("children", []).append(first_joint_node + i)

    # --- skinned shapes --------------------------------------------------
    from .convert import ModelResult, _material_json
    mres = ModelResult(name)
    ibm_game: list = [None] * len(joints)   # per joint, from the first shape using it
    skins: list = []                        # list of (joint list, ibm list) per glTF skin
    main_skin_ibm: dict = {}
    mesh_nodes = []
    sources = [(nif, jidx, txd, None)]
    if head is not None:
        hname, hdata, htxd = head
        hnif = parse_nif(hdata)
        hfailed = hnif.failed_blocks()
        if hfailed:
            raise FormatError(f"{hname}: {len(hfailed)} blocks failed to decode, first: #{hfailed[0].index} {hfailed[0].type_name}")
        hroot = find_skeleton_root(hnif)
        hjoints = build_skeleton(hnif, hroot, entity_space)
        by_name = {j.name: i for i, j in enumerate(joints)}
        hjidx = {}
        for hj in hjoints:
            if hj.name in by_name:   # joints only the head model has (e.g. 'Root Footsteps') are not skin bones; a skin
                hjidx[hj.block] = by_name[hj.name]   # bone outside the skeleton raises FormatError in the shape loop
        # head models replace the base head: cut the base shape's triangles at the head model's lowest vertex
        hz = []
        for hb in hnif.blocks:
            if hb.type_name in GEOMETRY and hb.data and hb.data["skin"] >= 0 and not hb.data["flags"] & 1:
                hg = hnif.data(hb.data["data"], "NiTriShapeData", "NiTriStripsData")
                hz += hg["vertices"][2::3]
        head_cut = (min(hz) - 0.005) if hz else None
        sources = [(nif, jidx, txd, head_cut), (hnif, hjidx, htxd, None)]
        res.warnings.append(f"head model {hname} (txd {htxd}) replaces the {name} head above z={head_cut:.3f} m")
    for snif, sjidx, stxd, head_cut in sources:
        for b in snif.blocks:
            if b.type_name not in GEOMETRY or b.data is None:
                continue
            d = b.data
            sname = snif.string(d["name"]) or f"#{b.index}"
            if d["skin"] < 0:
                res.skipped_shapes.append({"shape": sname, "reason": "not skinned" if not d["flags"] & 1 else "hidden"})
                continue
            if d["flags"] & 1:
                res.skipped_shapes.append({"shape": sname, "reason": "hidden"})
                continue
            si = snif.data(d["skin"], "NiSkinInstance")
            sd = snif.data(si["data"], "NiSkinData") if si else None
            gd = snif.data(d["data"], "NiTriShapeData", "NiTriStripsData")
            if si is None or sd is None or gd is None or not gd["vertices"]:
                res.skipped_shapes.append({"shape": sname, "reason": "incomplete skin/geometry"})
                continue
            if len(sd["bones"]) != len(si["bones"]):
                raise FormatError(f"{sname}: skin instance has {len(si['bones'])} bones, data {len(sd['bones'])}")
            if head_cut is not None:
                # the base model's head shape (nearly all vertices above the head model's lowest vertex) is cut out and
                # the vertex arrays compacted; the head model brings its own (hair + face) mesh and texture
                Wc = _shape_world(snif, b.index, entity_space)
                nv0 = gd["num_vertices"]
                zs = [m4_apply(Wc, gd["vertices"][3 * i:3 * i + 3])[2] for i in range(nv0)]
                if sum(1 for z in zs if z >= head_cut) > 0.8 * nv0:
                    tri = gd["triangles"]
                    kept = [tri[3 * t:3 * t + 3] for t in range(len(tri) // 3)
                            if not all(zs[i] >= head_cut for i in tri[3 * t:3 * t + 3])]
                    used = sorted({i for t in kept for i in t})
                    remap = {o: n for n, o in enumerate(used)}
                    gd = dict(gd)
                    gd["num_vertices"] = len(used)
                    gd["vertices"] = [c for o in used for c in gd["vertices"][3 * o:3 * o + 3]]
                    if gd.get("normals"):
                        gd["normals"] = [c for o in used for c in gd["normals"][3 * o:3 * o + 3]]
                    gd["uv_sets"] = [[c for o in used for c in uv[2 * o:2 * o + 2]] for uv in gd["uv_sets"]]
                    if gd.get("colors"):
                        gd["colors"] = [c for o in used for c in gd["colors"][4 * o:4 * o + 4]]
                    gd["triangles"] = [remap[i] for t in kept for i in t]
                    sd = dict(sd)
                    sd["bones"] = [dict(bn, weights=[(remap[vi], w) for vi, w in bn["weights"] if vi in remap]) for bn in sd["bones"]]
                    res.warnings.append(f"{sname}: head cut out ({nv0 - len(used)} of {nv0} vertices, {(len(tri) // 3) - len(kept)} triangles dropped)")
            ov = coords.Xform.from_av({"rotation": sd["overall"]["rotation"], "scale": sd["overall"]["scale"],
                                       "translation": sd["overall"]["translation"]})
            if not ov.is_identity(1e-5):
                res.warnings.append(f"{sname}: NiSkinData overall transform is not identity (applied as S_b * O)")
            W = _shape_world(snif, b.index, entity_space)
            Winv = m4_inv_affine(W)
            nv = gd["num_vertices"]
            src = gd["vertices"]
            # per-bone IBM in game space, and the skinned bind pose check
            shape_ibm = {}
            for k, bone_ref in enumerate(si["bones"]):
                if bone_ref not in sjidx:
                    raise FormatError(f"{sname}: skin bone #{bone_ref} is outside the skeleton")
                S = m4_mul(m4_from_xform(coords.Xform.from_av(sd["bones"][k]["transform"])), m4_from_xform(ov))
                shape_ibm[sjidx[bone_ref]] = (k, m4_mul(S, Winv), S)
            consistent = all(main_skin_ibm.get(ji) is None or _max_abs_diff(main_skin_ibm[ji], ibm) < 1e-4
                             for ji, (_k, ibm, _S) in shape_ibm.items())
            if consistent:
                for ji, (_k, ibm, _S) in shape_ibm.items():
                    main_skin_ibm.setdefault(ji, ibm)
                skin_slot = 0
            else:
                res.warnings.append(f"{sname}: bind matrices disagree with the shared skin; own skin")
                skins.append(dict((ji, ibm) for ji, (_k, ibm, _S) in shape_ibm.items()))
                skin_slot = len(skins)
            # weights per vertex
            infl = [[] for _ in range(nv)]
            for ji, (k, _ibm, _S) in shape_ibm.items():
                for vi, w in sd["bones"][k]["weights"]:
                    if vi >= nv:
                        raise FormatError(f"{sname}: weight for vertex {vi} >= {nv}")
                    if w > 0:
                        infl[vi].append((w, ji))
            joints_attr, weights_attr = [], []
            worst = 0.0
            for vi in range(nv):
                lst = sorted(infl[vi], reverse=True)
                res.max_influences = max(res.max_influences, len(lst))
                if not lst:
                    res.unweighted_vertices += 1
                    lst = [(1.0, sjidx.get(si["skeleton_root"], 0))]
                if len(lst) > 4:
                    res.warnings.append(f"{sname}: vertex {vi} has {len(lst)} influences, kept 4")
                    lst = lst[:4]
                tot = sum(w for w, _ in lst)
                res.weight_sum_max_error = max(res.weight_sum_max_error, abs(tot - 1.0))
                lst = [(w / tot, j) for w, j in lst] + [(0.0, 0)] * (4 - len(lst))
                joints_attr.extend(j for _w, j in lst)
                weights_attr.extend(w for w, _j in lst)
                # bind pose check: sum w * M_b * S_b * v vs the stored vertex W * v
                v = src[3 * vi:3 * vi + 3]
                acc = [0.0, 0.0, 0.0]
                for w, ji in lst:
                    if w <= 0:
                        continue
                    p = m4_apply(m4_mul(joints[ji].world, shape_ibm[ji][2]), v)
                    for c in range(3):
                        acc[c] += w * p[c]
                ref = m4_apply(W, v)
                worst = max(worst, math.dist(acc, ref))
            res.bind_vs_stored_max_m = max(res.bind_vs_stored_max_m, worst)
            # geometry in model space (glTF basis)
            pos, nrm = [], []
            for vi in range(nv):
                pos.extend(vec_game_to_gltf(m4_apply(W, src[3 * vi:3 * vi + 3])))
            if gd["normals"]:
                R = [row[:3] for row in W[:3]]
                for vi in range(nv):
                    n = coords.mat_vec(R, gd["normals"][3 * vi:3 * vi + 3])
                    ln = math.sqrt(sum(c * c for c in n)) or 1.0
                    nrm.extend(vec_game_to_gltf([c / ln for c in n]))
            attrs = {"POSITION": gb.add_floats(pos, "VEC3", minmax=True)}
            if nrm:
                attrs["NORMAL"] = gb.add_floats(nrm, "VEC3")
            for k, uv in enumerate(gd["uv_sets"]):
                attrs[f"TEXCOORD_{k}"] = gb.add_floats(list(uv), "VEC2")
            if gd["colors"]:
                attrs["COLOR_0"] = gb.add_floats([min(max(c, 0.0), 1.0) for c in gd["colors"]], "VEC4")
            attrs["JOINTS_0"] = gb.add_uints(joints_attr, "VEC4", 5123)
            attrs["WEIGHTS_0"] = gb.add_floats(weights_attr, "VEC4")
            props, path = _shape_props(snif, b.index)
            mat = _material_json(snif, props, store, stxd, name, gb, mres, len(gd["uv_sets"]), bool(gd["colors"])) \
                if store is not None else {"name": "material", "pbrMetallicRoughness": {"metallicFactor": 0.0}}
            prim = {"attributes": attrs, "indices": gb.add_indices(list(gd["triangles"])),
                    "material": gb.material(mat),
                    "extras": {"shape": sname, "block": b.index, "node_path": "/".join(path)}}
            mesh = gb.mesh(sname, [prim])
            mesh_nodes.append((sname, mesh, skin_slot))
            res.shapes += 1
            res.vertices += nv
            res.triangles += len(gd["triangles"]) // 3
    res.missing_textures = mres.missing_textures
    res.warnings += [w for w in mres.warnings if w not in res.warnings]
    if not mesh_nodes:
        raise FormatError(f"{name}: no skinned shapes")

    # --- skins -----------------------------------------------------------
    all_joint_nodes = [first_joint_node + i for i in range(len(joints))]
    skin_defs = [main_skin_ibm] + skins
    doc["skins"] = []
    for sk in skin_defs:
        mats = []
        for i, j in enumerate(joints):
            ibm = sk.get(i)
            if ibm is None:
                ibm = m4_inv_affine(j.world)
            else:
                res.bind_vs_file_pose_max = max(res.bind_vs_file_pose_max,
                                                _max_abs_diff(ibm, m4_inv_affine(j.world)))
            mats.extend(m4_colmajor(to_gltf_m4(ibm)))
        doc["skins"].append({"name": f"{name}_skin", "joints": all_joint_nodes,
                             "skeleton": first_joint_node,
                             "inverseBindMatrices": gb.add_floats(mats, "MAT4")})
    res.skins = len(doc["skins"])
    for sname, mesh, slot in mesh_nodes:
        gb.node({"name": sname, "mesh": mesh, "skin": slot})

    # --- animations ------------------------------------------------------
    for req in clips:
        info = add_clip(gb, joints, first_joint_node, req, root_motion, bone_track_check, res)
        res.clips.append(info)
    res.animations = len(doc.get("animations", []))

    doc["nodes"][model_node]["extras"] = {
        "source": f"World.img/{name}.nif", "txd": txd, "space": "entity" if entity_space else "file",
        "skeleton_root": joints[0].name, "joint_order": "NIF file order = .agr track order",
        "root_motion": root_motion, "units": "metres, glTF +Y up (game +Z up)"}
    folder = os.path.join(out_dir, "models")
    os.makedirs(folder, exist_ok=True)
    path, _ = gb.write(os.path.join(folder, safe_name(name) + ".gltf"))
    res.gltf = os.path.relpath(path, out_dir)
    return res


def _dedupe(times, values, width):
    """glTF sampler inputs must strictly increase: keep the last of equal times."""
    ot, ov = [], []
    for i, t in enumerate(times):
        v = values[i * width:(i + 1) * width]
        if ot and t <= ot[-1] + 1e-7:
            ov[-width:] = v
            continue
        ot.append(t)
        ov.extend(v)
    return ot, ov


def add_clip(gb: GltfBuilder, joints: list, first_joint_node: int, req: ClipRequest, root_motion: str,
             bone_track_check: bool, res: PedResult) -> dict:
    clip, dur = req.clip, req.duration
    if dur <= 0:
        dur = clip.duration
    n_tracks = clip.bone_count
    info = {"name": req.label, "tracks": n_tracks, "key_type": clip.key_type, "duration": dur,
            "agr_header_duration": clip.duration, "null_keys": clip.null_keys}
    info.update(req.meta)
    if n_tracks > len(joints):
        raise FormatError(f"{req.label}: {n_tracks} tracks > {len(joints)} joints")
    if n_tracks != len(joints):
        res.warnings.append(f"{req.label}: {n_tracks} tracks for {len(joints)} joints (first {n_tracks} animated)")
    arrow_pos = clip.positions[ARROW_TRACK] if n_tracks > ARROW_TRACK else []
    if n_tracks <= ARROW_TRACK and n_tracks > ROOT_TRACK:
        # no ground arrow (the AE rat, 12 tracks): the travel is the Root track's ground-plane motion
        arrow_pos = [(t, (p[0], p[1], 0.0)) for t, p in clip.positions[ROOT_TRACK]]
    travel = tuple(b - a for a, b in zip(arrow_pos[0][1], arrow_pos[-1][1])) if len(arrow_pos) > 1 else (0.0, 0.0, 0.0)
    info["root_motion"] = {"travel_game_m": [round(c, 5) for c in travel],
                           "travel_gltf_m": [round(c, 5) for c in vec_game_to_gltf(travel)],
                           "speed_m_s": round(math.sqrt(sum(c * c for c in travel)) / dur, 4) if dur else 0.0,
                           "mode": root_motion}
    a0 = arrow_pos[0][1] if arrow_pos else (0.0, 0.0, 0.0)

    def arrow_offset(t):
        p = sample_position(arrow_pos, t)
        return tuple(p[i] - a0[i] for i in range(3)) if p is not None else (0.0, 0.0, 0.0)

    channels, samplers = [], []

    def add_channel(node, path, times, values, width):
        times, values = _dedupe(times, values, width)
        if times[0] > 0:
            times = [0.0] + times
            values = values[:width] + values
        if times[-1] < dur - 1e-6:
            times = times + [dur]
            values = values + values[-width:]
        inp = gb.add_floats(times, "SCALAR", minmax=True)
        out = gb.add_floats(values, "VEC4" if width == 4 else "VEC3")
        samplers.append({"input": inp, "output": out, "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})

    pose_err = 0.0
    for k in range(n_tracks):
        node = first_joint_node + k
        tr = clip.tracks[k]
        if tr:
            times, vals = [], []
            prev = None
            for key in tr:
                q = key.q
                if prev is not None and sum(a * b for a, b in zip(prev, q)) < 0:
                    q = tuple(-c for c in q)  # keep the shortest path for LINEAR (nlerp/slerp) playback
                prev = q
                times.append(key.t * dur)
                vals.extend(quat_game_to_gltf(q))
            add_channel(node, "rotation", times, vals, 4)
        pos = clip.positions[k]
        if pos:
            times, vals = [], []
            for t, p in pos:
                if root_motion != "raw" and k == ROOT_TRACK:
                    off = arrow_offset(t)
                    p = (p[0] - off[0], p[1] - off[1], p[2] - off[2])
                if root_motion != "raw" and k == ARROW_TRACK:
                    p = a0
                times.append(t * dur)
                vals.extend(vec_game_to_gltf(p))
            add_channel(node, "translation", times, vals, 3)
    if root_motion == "dummy" and arrow_pos:
        times = [t * dur for t, _p in arrow_pos]
        vals = []
        base = joints[0].local.t
        for t, _p in arrow_pos:
            off = arrow_offset(t)
            vals.extend(vec_game_to_gltf((base[0] + off[0], base[1] + off[1], base[2] + off[2])))
        add_channel(first_joint_node, "translation", times, vals, 3)
    # loop check: start pose vs end pose (max joint rotation difference, deg)
    worst = 0.0
    for k in range(n_tracks):
        a, b = sample_rotation(clip.tracks[k], 0.0), sample_rotation(clip.tracks[k], 1.0)
        if a and b:
            c = min(1.0, abs(sum(x * y for x, y in zip(a, b))))
            worst = max(worst, math.degrees(2 * math.acos(c)))
    info["start_end_max_deg"] = round(worst, 3)
    info["loops"] = worst < 2.0
    anim = {"name": req.label, "channels": channels, "samplers": samplers, "extras": info}
    gb.doc.setdefault("animations", []).append(anim)
    return info


# ---------------------------------------------------------------------------
# Clip lookup against the install
# ---------------------------------------------------------------------------

# Jimmy's locomotion set. Each clip is cited by the shipped action trees
# (Act.img: GlobalLoco, SimpleLoco, JumpActions; docs/re-notes/anim-formats.md).
PLAYER_LOCOMOTION = ("IDLE", "PLAYERWALK", "PLAYERRUN", "RUNFAST", "PLAYERWALK_START", "PLAYERRUN_START",
                     "IDLEJUMP", "JOGJUMP_L", "JOGJUMP_R", "FALL", "FALL_LAND")


def load_hxd(install: str, name: str = "MAINPED"):
    from .hxd import parse_hxd
    with open(os.path.join(install, "Anim", name + ".HXD"), "rb") as f:
        return parse_hxd(f.read())


def read_group_agr(install: str, img, group: str):
    """A group's .agr: the loose Anim/<group>.agr when present (C_Player,
    Grap, MOT_CTRL, NPC_Cher), else World.img/<group>.agr (streamed)."""
    anim_dir = os.path.join(install, "Anim")
    for f in os.listdir(anim_dir):
        if f.lower() == group.lower() + ".agr":
            with open(os.path.join(anim_dir, f), "rb") as fh:
                return fh.read(), f"Anim/{f}"
    e = img.find(group + ".agr") if img is not None else None
    if e is None:
        raise FormatError(f"no .agr for group {group}")
    return img.read_entry(e), f"World.img/{e.name}"


def clip_requests(install: str, img, group: str, names, hxd=None) -> list:
    """Resolve clip names (short "IDLE" or full "C_PLAYER\\IDLE"; "*" = all)
    in `group` to ClipRequests with HXD play durations."""
    from .agr import parse_agr
    hxd = hxd or load_hxd(install)
    recs = hxd.group_anims(group)
    if not recs:
        raise FormatError(f"group {group} not in the HXD")
    data, src = read_group_agr(install, img, group)
    clips = parse_agr(data, [r.agr_size - 4 for r in recs])
    if len(clips) != len(recs):
        raise FormatError(f"{src}: {len(clips)} clips, HXD lists {len(recs)}")
    by_name = {}
    for k, r in enumerate(recs):
        by_name.setdefault(r.name.lower(), k)
        by_name.setdefault(r.short_name.lower(), k)
    if list(names) == ["*"]:
        wanted = range(len(recs))
    else:
        wanted = []
        for n in names:
            k = by_name.get(n.lower())
            if k is None:
                raise FormatError(f"clip {n} not in group {group}")
            wanted.append(k)
    out = []
    shorts = [recs[k].short_name.lower() for k in wanted]
    for k in wanted:
        r = recs[k]
        # a group can hold clips of other prefixes (C_Player has F_NERDS\RUN next to
        # C_PLAYER\RUN): fall back to the full name when the short one repeats
        label = r.short_name if shorts.count(r.short_name.lower()) == 1 else r.name.replace("\\", "_")
        out.append(ClipRequest(label, clips[k], r.duration,
                               {"hxd_name": r.name, "hxd_hash": f"0x{r.hash:08x}", "group": group,
                                "index_in_group": k, "agr_source": src, "default_blend_s": round(r.blend, 4),
                                "hxd_flags": f"0x{r.flags:x}"}))
    return out
