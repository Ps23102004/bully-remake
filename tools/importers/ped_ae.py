# OWNER: builder-A2 (animation)
"""Anniversary Edition skinned character export: a ped `.msh` (player = Jimmy,
gn_boy01, dog_doberman, rat_ped, ...) to the same glTF layout `ped.py` writes for
the PC NIFs, so tools/ue/import_character.py consumes either unchanged.

Format notes and evidence: docs/re-notes/ae-peds.md.

Skeleton. A ped .msh carries, after the property string the generic reader skips,
the full node hierarchy of the exported scene: string "[0]", u32 n, n x (name,
4 u32 list links), u32 n, n x (name, i32 0, i32 parent, i32 first child, i32 next
sibling, 12 f32 local 3x4 rows: rotation | translation, v' = M v). For a biped ped
that is the PC NIF skeleton under its PC names and in its depth-first order
(Dummy, Root, Root Pelvis, ..., Right_Shoulder, ARROW: 36 nodes) = the .agr track
order; the rat has its own 12. Every node becomes a glTF joint in that order.

Bind pose. The .msh bone table (32 of the 36 nodes for Jimmy) holds each skin
bone's inverse bind matrix (game space). The node transforms are the authoring
pose, which differs from the bind pose by a few degrees / up to 5 cm (bone
lengths differ per body), so the rest pose is built from the inverse binds:
world(skin bone) = inverse(IBM), world(other node) = world(parent) * node local.
Joint locals are then parent^-1 * world, so the rest pose IS the bind pose and
every IBM = inverse(rest world). Clips replace the local rotations (and Root /
ARROW translations) exactly as on the PC skeleton (ped.add_clip).

Heads: `head` = (name, .msh bytes) of a clothing head model (b_buzz): its skin
bones map onto the skeleton by name, and the base mesh's head submesh (most
vertices above the head model's lowest vertex) loses those triangles, as ped.py.
"""

from __future__ import annotations

import math
import os
from dataclasses import replace

import numpy as np

from . import coords
from .ae import AeArchive
from .agr import quat_to_rows, rows_to_quat, sample_position, sample_rotation
from .binreader import FormatError, Reader
from .convert import ModelResult
from .convert_ae import AeTextureStore, _material
from .gltf import GltfBuilder
from .msh import _FLAG_BYTES, WAR_VERSION, parse_msh
from .nft import safe_name
from .ped import ARROW_TRACK, ROOT_TRACK, ClipRequest, Joint, PedResult, add_clip, m4_colmajor, quat_game_to_gltf, to_gltf_m4, vec_game_to_gltf

# Bind Root height (m) of the body each clip bank was authored on. The .agr Root translation is absolute, so on a
# taller/shorter body it is scaled by (bind Root z / this) or the pelvis (and a dog's paws) sink into the ground.
# Banks not listed (the rat's own, NPC_Cher, ...) are exported unscaled.
ROOT_REF_Z = {"c_player": 0.845, "qped": 0.436}
GROUND_TOLERANCE_M = 0.03   # frame 0 of an idle clip may not dip further below z=0 than this
TRACK_MATCH_DEG = 10.0  # PLAYER: every track within 0.12 deg in some C_Player clip; other bodies are posed up to 8 deg off


def _m4(rows12) -> np.ndarray:
    m = np.eye(4)
    m[:3, :4] = np.asarray(rows12, dtype=np.float64).reshape(3, 4)
    return m


def parse_skeleton(data: bytes) -> list:
    """[(name, parent index, 4x4 local game-space matrix)] from the .msh node hierarchy."""
    r = Reader(data)
    version, two, war, hsize, one, mesh_off = r.array("I", 6)
    if war != WAR_VERSION or version not in _FLAG_BYTES:
        raise FormatError(f"not a WarEngine mesh (version {version}, magic {war})")
    for _ in range(r.u32()):                     # materials
        r.sized_string(4096)
    for _ in range(r.u32()):                     # bones: name, 12 f32, i32 parent
        r.sized_string(4096)
        r.skip(52)
    r.skip(_FLAG_BYTES[version] + 40)            # flag bytes, bounds
    r.sized_string(4096)                         # property text
    r.sized_string(64)                           # "[0]"
    n = r.u32()
    if n > 1024:
        raise FormatError(f"node count {n}")
    for _ in range(n):                           # first node list: name + 4 u32 (links, unused)
        r.sized_string(256)
        r.skip(16)
    n2 = r.u32()
    if n2 > 1024:
        raise FormatError(f"node count {n2}")
    nodes = []
    for k in range(n2):
        name = r.sized_string(256)
        _flag, parent, _child, _sibling = r.array("i", 4)
        if parent >= k:
            raise FormatError(f"node {k} {name!r}: parent {parent} not before it")
        nodes.append((name, parent, _m4(r.array("f", 12))))
    if r.pos != mesh_off:
        raise FormatError(f"node hierarchy ends at {r.pos}, mesh table at {mesh_off}")
    if not nodes:
        raise FormatError("no node hierarchy (not a skinned character)")
    return nodes


def _rest_worlds(nodes: list, bones: list) -> list:
    """Bind-pose model-space matrix per node (see module doc)."""
    ibm = {b.name.lower(): _m4([c for row in b.matrix for c in row]) for b in bones}
    worlds = []
    for name, parent, local in nodes:
        m = ibm.get(name.lower())
        if m is not None:
            worlds.append(np.linalg.inv(m))
        else:
            worlds.append(local if parent < 0 else worlds[parent] @ local)
    return worlds


def _xform(m: np.ndarray) -> coords.Xform:
    a = m[:3, :3]
    s = float(np.cbrt(np.linalg.det(a)))
    return coords.Xform((a / s).tolist(), s, tuple(float(c) for c in m[:3, 3]))


def _joint_map(bones: list, names: dict, what: str) -> np.ndarray:
    """msh bone index -> joint index (by case-insensitive name)."""
    out = []
    for b in bones:
        j = names.get(b.name.lower())
        if j is None:
            raise FormatError(f"{what}: skin bone {b.name!r} is not in the skeleton")
        out.append(j)
    return np.asarray(out or [0], dtype=np.int64)


def _cut_head(sm, cut: float):
    """Drop the triangles entirely above z=cut; None if nothing is left."""
    tri = sm.indices.reshape(-1, 3)
    keep = tri[~(sm.positions[tri][:, :, 2] >= cut).all(1)]
    if not len(keep):
        return None
    used, local = np.unique(keep.ravel(), return_inverse=True)
    out = type(sm)(sm.part, sm.part_index, sm.material, sm.material_name, sm.positions[used],
                   local.astype(np.uint32))
    for k in ("normals", "uv0", "uv1", "uv2", "joints", "weights"):
        v = getattr(sm, k)
        if v is not None:
            setattr(out, k, v[used])
    return out


def track_mismatches(nodes: list, clips: list) -> list:
    """Tracks whose first key is > TRACK_MATCH_DEG from the node's stored local rotation in every clip
    (the PC check that track order = skeleton order). [] when the clips fit the skeleton."""
    best = [math.inf] * len(nodes)
    for req in clips:
        for k, tr in enumerate(req.clip.tracks[:len(nodes)]):
            q = sample_rotation(tr, 0.0)
            if q is None:
                continue
            n = rows_to_quat(_xform(nodes[k][2]).r)
            c = min(1.0, abs(sum(a * b for a, b in zip(q, n))))
            best[k] = min(best[k], math.degrees(2 * math.acos(c)))
    return [(nodes[k][0], round(d, 2)) for k, d in enumerate(best) if TRACK_MATCH_DEG < d < math.inf]


def scale_root_clip(req: ClipRequest, scale: float) -> ClipRequest:
    """Copy of `req` with the Root and ARROW translations scaled (ped.py's PC path never sees this)."""
    pos = list(req.clip.positions)
    for k in (ROOT_TRACK, ARROW_TRACK):
        if k < len(pos):
            pos[k] = [(t, tuple(c * scale for c in p)) for t, p in pos[k]]
    return ClipRequest(req.label, replace(req.clip, positions=pos), req.duration,
                       {**req.meta, "root_height_scale": round(scale, 4)})


def frame0_min_z(joints: list, clip, skins: list) -> float:
    """CPU-skin frame 0 of `clip` (game space, +Z up) and return the lowest vertex z."""
    world = []
    for k, j in enumerate(joints):
        q = sample_rotation(clip.tracks[k], 0.0) if k < clip.bone_count else None
        m = np.eye(4)
        m[:3, :3] = np.asarray(quat_to_rows(q) if q else j.local.r) * j.local.s
        pos = sample_position(clip.positions[k], 0.0) if k < len(clip.positions) else None
        m[:3, 3] = pos if pos is not None else j.local.t
        world.append(m if j.parent < 0 else world[j.parent] @ m)
    W = np.stack(world)
    lowest = math.inf
    for p4, jidx, raw, w, ibms in skins:
        acc = np.zeros((len(p4), 3))
        for c in range(4):
            acc += w[:, c:c + 1] * np.einsum("nij,nj->ni", W[jidx[:, c]] @ ibms[raw[:, c]], p4)[:, :3]
        lowest = min(lowest, float(acc[:, 2].min()))
    return lowest


def export_ped_ae(name: str, data: bytes, arc, store, out_dir: str, clips: list = (),
                  root_motion: str = "inplace", head: tuple | None = None) -> PedResult:
    res = PedResult(name)
    model = parse_msh(bytes(data))
    if not model.skinned:
        raise FormatError(f"{name}: not a skinned model")
    nodes = parse_skeleton(bytes(data))
    names = {n.lower(): k for k, (n, _p, _l) in enumerate(nodes)}
    worlds = _rest_worlds(nodes, model.bones)
    joints = []
    for k, (jname, parent, local) in enumerate(nodes):
        loc = worlds[k] if parent < 0 else np.linalg.inv(worlds[parent]) @ worlds[k]
        joints.append(Joint(k, jname, parent, _xform(loc), worlds[k].tolist()))
    res.joints = len(joints)
    # diagnostics: bind pose vs the stored node (authoring) pose
    pose = []
    for _n, parent, local in nodes:
        pose.append(local if parent < 0 else pose[parent] @ local)
    res.bind_vs_file_pose_max = float(max(np.abs(np.linalg.inv(w) - np.linalg.inv(p)).max()
                                          for w, p in zip(worlds, pose)))

    gb = GltfBuilder()
    doc = gb.doc
    model_node = gb.node({"name": name, "children": []})
    first = len(doc["nodes"])
    for j in joints:
        n = {"name": j.name, "translation": vec_game_to_gltf(j.local.t),
             "rotation": quat_game_to_gltf(rows_to_quat(j.local.r))}
        if abs(j.local.s - 1.0) > 1e-4:
            n["scale"] = [j.local.s] * 3
        gb.node(n, root=False)
    for i, j in enumerate(joints):
        parent = doc["nodes"][model_node] if j.parent < 0 else doc["nodes"][first + j.parent]
        parent.setdefault("children", []).append(first + i)

    sources = [(model, name, None)]
    if head is not None:
        hname, hdata = head
        hmodel = parse_msh(bytes(hdata))
        hz = [s.positions[:, 2].min() for s in hmodel.submeshes if s.weights is not None]
        if not hz:
            raise FormatError(f"{hname}: head model has no skinned geometry")
        cut = float(min(hz)) - 0.005
        sources = [(model, name, cut), (hmodel, hname, None)]
        res.warnings.append(f"head model {hname} replaces the {name} head above z={cut:.3f} m")

    mres = ModelResult(name)
    mats: dict = {}
    prims = []
    worst = 0.0
    skins = []   # per submesh (p4, joints, blend indices, weights, IBMs) for the frame-0 ground check
    for m, src, cut in sources:
        jmap = _joint_map(m.bones, names, src)
        ibms = np.stack([_m4([c for row in b.matrix for c in row]) for b in m.bones]) if m.bones else np.eye(4)[None]
        for sm in m.submeshes:
            if sm.weights is None or not len(sm.indices):
                res.skipped_shapes.append({"shape": f"{src}:{sm.part}/{sm.material_name}",
                                           "reason": "not skinned" if sm.weights is None else "empty"})
                continue
            if cut is not None and (sm.positions[:, 2] >= cut).mean() > 0.8:
                nv0, label = len(sm.positions), f"{src}:{sm.material_name}"
                sm = _cut_head(sm, cut)
                if sm is None:
                    res.warnings.append(f"{label}: head submesh removed entirely")
                    continue
                res.warnings.append(f"{label}: head cut out ({nv0 - len(sm.positions)} of {nv0} vertices)")
            w = sm.weights
            raw = sm.joints.astype(np.int64)
            used = w > 0
            if (raw[used] >= len(m.bones)).any():
                raise FormatError(f"{src}: blend index >= {len(m.bones)} bones")
            raw = np.where(used, raw, 0)
            jidx = np.where(used, jmap[raw], 0)
            res.max_influences = max(res.max_influences, int(used.sum(1).max()))
            res.unweighted_vertices += int((~used.any(1)).sum())
            res.weight_sum_max_error = max(res.weight_sum_max_error, float(np.abs(w.sum(1) - 1).max()))
            # bind check: sum w * rest_world(joint) * IBM(msh bone) * v == v (head IBMs vs the base skeleton)
            p4 = np.concatenate([sm.positions, np.ones((len(sm.positions), 1), np.float32)], 1)
            W = np.stack([np.asarray(joints[j].world) for j in range(len(joints))])
            acc = np.zeros((len(p4), 4))
            for c in range(4):
                M = W[jidx[:, c]] @ ibms[raw[:, c]]
                acc += w[:, c:c + 1] * np.einsum("nij,nj->ni", M, p4)
            worst = max(worst, float(np.linalg.norm(acc[:, :3] - sm.positions, axis=1).max()))
            skins.append((p4, jidx, raw, w, ibms))
            p = sm.positions
            attrs = {"POSITION": gb.add_floats(np.stack([p[:, 0], p[:, 2], -p[:, 1]], 1).ravel().tolist(),
                                               "VEC3", minmax=True)}
            nrm = sm.normals
            if nrm is not None and np.linalg.norm(nrm, axis=1).min() > 0.5:
                attrs["NORMAL"] = gb.add_floats(np.stack([nrm[:, 0], nrm[:, 2], -nrm[:, 1]], 1).ravel().tolist(), "VEC3")
            for k, uv in enumerate((sm.uv0, sm.uv1, sm.uv2)):
                if uv is not None:
                    attrs[f"TEXCOORD_{k}"] = gb.add_floats(uv.ravel().tolist(), "VEC2")
            attrs["JOINTS_0"] = gb.add_uints(jidx.ravel().tolist(), "VEC4", 5123)
            attrs["WEIGHTS_0"] = gb.add_floats(np.where(used, w, 0).ravel().tolist(), "VEC4")
            key = sm.material_name or f"#{sm.material}"
            if key not in mats:
                mat = _material(key, arc, store, gb, mres, src) if store is not None else \
                    {"name": key, "pbrMetallicRoughness": {"metallicFactor": 0.0}}
                mats[key] = gb.material(mat)
            prims.append({"attributes": attrs, "indices": gb.add_indices(sm.indices.tolist()),
                          "material": mats[key], "extras": {"source": src, "part": sm.part, "material": key}})
            res.shapes += 1
            res.vertices += len(p)
            res.triangles += len(sm.indices) // 3
    if not prims:
        raise FormatError(f"{name}: no skinned geometry")
    res.bind_vs_stored_max_m = worst
    res.missing_textures = mres.missing_textures
    res.warnings += mres.warnings

    mats4 = []
    for j in joints:
        mats4.extend(m4_colmajor(to_gltf_m4(np.linalg.inv(np.asarray(j.world)).tolist())))
    doc["skins"] = [{"name": f"{name}_skin", "joints": [first + i for i in range(len(joints))],
                     "skeleton": first, "inverseBindMatrices": gb.add_floats(mats4, "MAT4")}]
    res.skins = 1
    gb.node({"name": name + "_mesh", "mesh": gb.mesh(name, prims), "skin": 0})

    bind_root_z = float(joints[ROOT_TRACK].world[2][3]) if len(joints) > ROOT_TRACK else 0.0
    for req in clips:
        ref = ROOT_REF_Z.get(str(req.meta.get("group", "")).lower())
        if ref and bind_root_z > 0:
            req = scale_root_clip(req, bind_root_z / ref)
        info = add_clip(gb, joints, first, req, root_motion, True, res)
        if req.label.upper() in ("IDLE", "DOGIDLE"):
            low = frame0_min_z(joints, req.clip, skins)
            info["frame0_min_z_m"] = round(low, 4)
            if low < -GROUND_TOLERANCE_M:
                res.warnings.append(f"{req.label} frame 0: mesh {-low:.3f} m below the ground (z=0), root height off")
        res.clips.append(info)
    res.animations = len(doc.get("animations", []))
    # a handful of clips can all sit far from the authoring pose (locomotion: Root is turned 160 deg),
    # so the check is only meaningful on a whole bank (C_Player '*': every track matches some clip)
    bad = track_mismatches(nodes, clips) if len(clips) >= 100 else []
    if bad:
        res.warnings.append(f"tracks not matching the skeleton's stored rotations (> {TRACK_MATCH_DEG} deg): {bad}")

    doc["nodes"][model_node]["extras"] = {
        "source": f"data_1.zip/bully/{name.lower()}.msh" + (f" + {head[0].lower()}.msh" if head else ""),
        "space": "entity", "skeleton_root": joints[0].name,
        "joint_order": ".msh node hierarchy order = .agr track order", "rest_pose": "bind pose (from the .msh inverse binds)",
        "root_motion": root_motion, "units": "metres, glTF +Y up (game +Z up)"}
    folder = os.path.join(out_dir, "models")
    os.makedirs(folder, exist_ok=True)
    path, _ = gb.write(os.path.join(folder, safe_name(name) + ".gltf"))
    res.gltf = os.path.relpath(path, out_dir)
    return res


def export_from_install(install: str, a) -> tuple:
    """`ped` subcommand on an AE install (extracted data_0 tree + data_1.zip). Returns (PedResult, textured)."""
    from .img import ImgArchive
    from .ped import PLAYER_LOCOMOTION, clip_requests, load_hxd
    arc = AeArchive(getattr(a, "ae_data", None))
    try:
        data = arc.read(a.name, "msh")
        store = None if a.no_textures else AeTextureStore(arc, a.out)
        names = [] if a.no_anims else (a.anims or (PLAYER_LOCOMOTION if a.group.lower() == "c_player" else []))
        reqs = []
        if names:
            with ImgArchive(os.path.join(install, "stream", "world.img")) as img:
                reqs = clip_requests(install, img, a.group, names, load_hxd(install, a.hxd))
        head = (a.head, arc.read(a.head, "msh")) if a.head else None
        return export_ped_ae(a.name, data, arc, store, a.out, reqs, root_motion=a.root_motion, head=head), store is not None
    finally:
        arc.close()
