# OWNER: builder-B3 (jimmy)
"""Headless UE import of a skinned character glTF from tools/importers (`ped`) into /Game/Imported/Characters.

Runs inside UnrealEditor-Cmd (pythonscript commandlet); launch through tools/ue/run_import_character.sh, which
applies the GPU-slot wrapper and the headless flags. Configuration through environment variables:

  BR_IMPORTED    importer output dir (contains models/<MODEL>.gltf)                          [required]
  BR_CHAR_MODEL  ped model name in imported/models                                           [PLAYER]
  BR_CHAR_NAME   UE-side character name (asset folder + SK_/SKEL_/PHYS_ prefixes)            [Jimmy]
  BR_CHAR_ANIMS  "all", or a comma list of glTF animation names (HXD clip names)             [all]
  BR_REPORT      JSON report path                         [<project>/Saved/BullyImport/character_report.json]

Two Interchange passes, both through an override InterchangeGenericAssetsPipeline:
  1. mesh: skinned glTF (animations stripped) -> SkeletalMesh + Skeleton + PhysicsAsset, slots bound to material
     instances of a character master (same graph as M_Bully_Opaque, plus bUsedWithSkeletalMesh).
  2. animations: the same glTF with the selected animations, import_only_animations onto the pass-1 skeleton ->
     one AnimSequence per clip, /Game/Imported/Characters/<Name>/Anims/<CLIP>, sampled at 60 Hz.
Idempotent: the mesh and every AnimSequence carry metadata tag br_src_sha1; equal hash -> skipped.
Verification (written to the report and the log): skeleton bone count/names, per-clip length vs the glTF
duration, and a "not T-pose" check (largest bone-rotation difference between the clip and the reference pose).
"""
import hashlib
import json
import os
import shutil
import sys
import time
import traceback

import unreal

os.environ["BR_NO_MAIN"] = "1"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import import_content as ic  # noqa: E402  (texture import + master-material builder, shared with the level)

EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary
AT = unreal.AssetToolsHelpers.get_asset_tools()

CHAR_VERSION = "3"            # bump to force mesh re-import
ANIM_VERSION = "3"            # bump to force animation re-import
ANIM_RATE = 60                # Hz; Bully runs at 30, the 1/511 key quantisation wants finer sampling
T0 = time.time()
REPORT = {"format": "bully-remake.ue-character-report/1", "errors": [], "warnings": []}


def log(msg):
    unreal.log("BR_CHAR_IMPORT [%6.1fs] %s" % (time.time() - T0, msg))


def warn(msg):
    REPORT["warnings"].append(msg)
    unreal.log_warning("BR_CHAR_IMPORT WARN " + msg)


def err(msg):
    REPORT["errors"].append(msg)
    unreal.log_error("BR_CHAR_IMPORT ERROR " + msg)


def gltf_sig(path, g, extra):
    h = hashlib.sha1(extra.encode())
    with open(path, "rb") as f:
        h.update(f.read())
    for b in g.get("buffers", []):
        uri = b.get("uri", "")
        if uri and not uri.startswith("data:"):
            with open(os.path.join(os.path.dirname(path), uri), "rb") as f:
                h.update(hashlib.sha1(f.read()).digest())
    return h.hexdigest()


def stage(g, src_path, stage_dir, name, keep_anims):
    """Copy of the glTF: materials renamed M000.. without textures (bound later through material instances),
    animations filtered (None = drop all)."""
    g = json.loads(json.dumps(g))
    for i, m in enumerate(g.get("materials", [])):
        m["name"] = "M%03d" % i
        m.pop("extras", None)
        for k in ("normalTexture", "emissiveTexture", "occlusionTexture"):
            m.pop(k, None)
        pbr = m.get("pbrMetallicRoughness", {})
        pbr.pop("baseColorTexture", None)
        pbr.pop("metallicRoughnessTexture", None)
    for k in ("images", "textures", "samplers"):
        g.pop(k, None)
    if keep_anims is None:
        g.pop("animations", None)
    else:
        g["animations"] = [a for a in g.get("animations", []) if a["name"] in keep_anims]
    os.makedirs(stage_dir, exist_ok=True)
    for b in g.get("buffers", []):
        uri = b.get("uri", "")
        if uri and not uri.startswith("data:"):
            dst = os.path.join(stage_dir, os.path.basename(uri))
            src = os.path.join(os.path.dirname(src_path), uri)
            if not os.path.isfile(dst) or os.path.getsize(dst) != os.path.getsize(src):
                shutil.copyfile(src, dst)
            b["uri"] = os.path.basename(uri)
    out = os.path.join(stage_dir, name + ".gltf")
    with open(out, "w") as f:
        json.dump(g, f)
    return out


def make_pipeline(anims_only, skeleton=None):
    p = unreal.InterchangeGenericAssetsPipeline()
    p.set_editor_property("use_source_name_for_asset", not anims_only)
    cm = p.get_editor_property("common_meshes_properties")
    cm.set_editor_property("force_all_mesh_as_type", unreal.InterchangeForceMeshType.IFMT_NONE)
    cm.set_editor_property("vertex_color_import_option", unreal.InterchangeVertexColorImportOption.IVCIO_REPLACE)
    cm.set_editor_property("recompute_normals", False)
    cm.set_editor_property("recompute_tangents", True)
    cm.set_editor_property("use_mikk_t_space", True)
    cm.set_editor_property("import_lods", False)
    sk = p.get_editor_property("common_skeletal_meshes_and_animations_properties")
    sk.set_editor_property("import_only_animations", anims_only)
    sk.set_editor_property("use_t0_as_ref_pose", False)          # the NIF bind pose is the reference pose
    if skeleton is not None:
        sk.set_editor_property("skeleton", skeleton)
    mp = p.get_editor_property("mesh_pipeline")
    mp.set_editor_property("import_static_meshes", False)
    mp.set_editor_property("import_skeletal_meshes", not anims_only)
    mp.set_editor_property("create_physics_asset", not anims_only)
    mp.set_editor_property("import_morph_targets", False)
    matp = p.get_editor_property("material_pipeline")
    matp.set_editor_property("import_materials", False)
    matp.get_editor_property("texture_pipeline").set_editor_property("import_textures", False)
    ap = p.get_editor_property("animation_pipeline")
    ap.set_editor_property("import_animations", anims_only)
    if anims_only:
        ap.set_editor_property("import_bone_tracks", True)
        ap.set_editor_property("use30_hz_to_bake_bone_animation", False)
        ap.set_editor_property("custom_bone_animation_sample_rate", ANIM_RATE)
        ap.set_editor_property("import_custom_attribute", False)
        # HXD play durations are not frame multiples (1/511 key quantisation): snap the end to the nearest frame
        ap.set_editor_property("frame_alignment", unreal.InterchangeFrameAlignment.SNAP_TO_CLOSEST)
    return p


def run_task(staged, dest_path, dest_name, pipeline):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", staged)
    task.set_editor_property("destination_path", dest_path)
    if dest_name:
        task.set_editor_property("destination_name", dest_name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)
    stack = unreal.InterchangePipelineStackOverride()
    stack.add_pipeline(pipeline)
    task.set_editor_property("options", stack)
    AT.import_asset_tasks([task])
    return list(task.get_objects() or [])


# --------------------------------------------------------------------------------------------
# materials

CHAR_MASTER = None


def char_master():
    """M_Bully_Character: the M_Bully_Opaque graph (built by import_content.build_master) with skeletal-mesh
    usage. Kept separate so the level's eight masters never compile skinned permutations."""
    global CHAR_MASTER
    if CHAR_MASTER is None:
        mpc = ic.ensure_mpc()
        mat = ic.build_master("M_Bully_Character", "opaque", False, mpc)
        if not mat.get_editor_property("used_with_skeletal_mesh"):
            mat.set_editor_property("used_with_skeletal_mesh", True)
            MEL.recompile_material(mat)
            EAL.save_loaded_asset(mat, only_if_is_dirty=False)
        CHAR_MASTER = mat
    return CHAR_MASTER


def char_mi(char_dir, d, h):
    base_tex = d["tex"].get("base")
    stem = ic.sanitize(base_tex.rsplit("/", 1)[1]) if base_tex else "untextured"
    name = "MI_%s" % stem
    path = "%s/Materials/%s" % (char_dir, name)
    if EAL.does_asset_exist(path):
        mi = EAL.load_asset(path)
        if EAL.get_metadata_tag(mi, "br_key") == h + ic.MASTER_VERSION:
            return mi
    else:
        mi = AT.create_asset(name, char_dir + "/Materials", unreal.MaterialInstanceConstant,
                             unreal.MaterialInstanceConstantFactoryNew())
    MEL.set_material_instance_parent(mi, char_master())
    for role, pname in (("base", "BaseColor"), ("normal", "Normal"), ("gloss", "Gloss"), ("glow", "Glow")):
        ap = d["tex"].get(role)
        if ap and EAL.does_asset_exist(ap):
            MEL.set_material_instance_texture_parameter_value(mi, pname, EAL.load_asset(ap))
        elif ap:
            warn("character material %s: %s texture %s not imported" % (name, role, ap))
    MEL.set_material_instance_scalar_parameter_value(mi, "SpecularScale", min(1.0, 0.25 + 2.0 * d["spec"]))
    dr, dg, db = d["diffuse"]
    MEL.set_material_instance_vector_parameter_value(mi, "DiffuseTint", unreal.LinearColor(dr, dg, db, d["alpha"]))
    MEL.update_material_instance(mi)
    EAL.set_metadata_tag(mi, "br_key", h + ic.MASTER_VERSION)
    EAL.save_loaded_asset(mi, only_if_is_dirty=False)
    return mi


def bind_materials(sk, g, src_path, imported, char_dir):
    mats = g.get("materials", [])
    out = list(sk.get_editor_property("materials"))
    bound = 0
    for i, skm in enumerate(out):
        slot = str(skm.get_editor_property("material_slot_name"))
        idx = int(slot[1:4]) if slot.startswith("M") and slot[1:4].isdigit() else None
        if idx is None or idx >= len(mats):
            warn("slot %d '%s' has no glTF material" % (i, slot))
            continue
        d, h = ic.mat_key(mats[idx], os.path.dirname(src_path), imported)
        d["blend"] = "opaque"
        skm.set_editor_property("material_interface", char_mi(char_dir, d, h))
        out[i] = skm
        bound += 1
    sk.set_editor_property("materials", out)
    return bound


# --------------------------------------------------------------------------------------------
# verification helpers

def bone_names(sk):
    """Reference-skeleton bone names through a transient SkeletalMeshComponent (GetNumBones/GetBoneName)."""
    try:
        comp = unreal.SkeletalMeshComponent()
        try:
            comp.set_skeletal_mesh_asset(sk)
        except Exception:
            comp.set_skeletal_mesh(sk)
        return [str(comp.get_bone_name(i)) for i in range(comp.get_num_bones())]
    except Exception as e:  # noqa: BLE001
        warn("bone listing failed: %s" % e)
        return None


import math  # noqa: E402

CHECK_BONES = ("Root L Thigh", "Root R Calf", "Root L UpperArm", "Root Spine1", "Root Pelvis")


def ue_bone(gltf_name):
    """Interchange turns spaces in joint names into hyphens ("Root L Thigh" -> "Root-L-Thigh")."""
    return gltf_name.replace(" ", "-")


def qangle(a, b):
    """Angle in degrees between two unit quaternions given as (x, y, z, w)."""
    d = abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3])
    return math.degrees(2.0 * math.acos(min(1.0, d)))


class GltfSampler:
    """Linear (normalised) sampling of the glTF rotation channels, for the import check."""

    def __init__(self, path, g):
        import struct
        self.g = g
        with open(os.path.join(os.path.dirname(path), g["buffers"][0]["uri"]), "rb") as f:
            self.bin = f.read()
        self.struct = struct
        self.node_by_name = {n.get("name"): i for i, n in enumerate(g["nodes"])}
        self.anim = {a["name"]: a for a in g.get("animations", [])}

    def acc(self, i):
        a = self.g["accessors"][i]
        bv = self.g["bufferViews"][a["bufferView"]]
        n = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[a["type"]]
        off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        vals = self.struct.unpack_from("<%df" % (a["count"] * n), self.bin, off)
        return [vals[k * n:(k + 1) * n] for k in range(a["count"])]

    def rot(self, clip, bone, t):
        a = self.anim[clip]
        node = self.node_by_name[bone]
        for ch in a["channels"]:
            if ch["target"]["node"] == node and ch["target"]["path"] == "rotation":
                s = a["samplers"][ch["sampler"]]
                ts = [v[0] for v in self.acc(s["input"])]
                qs = self.acc(s["output"])
                if t <= ts[0]:
                    return qs[0]
                for k in range(1, len(ts)):
                    if t <= ts[k]:
                        u = (t - ts[k - 1]) / max(1e-9, ts[k] - ts[k - 1])
                        q0, q1 = qs[k - 1], qs[k]
                        sgn = 1.0 if sum(x * y for x, y in zip(q0, q1)) >= 0 else -1.0
                        q = [x * (1 - u) + sgn * y * u for x, y in zip(q0, q1)]
                        n = math.sqrt(sum(x * x for x in q)) or 1.0
                        return [x / n for x in q]
                return qs[-1]
        n = self.g["nodes"][node]
        return n.get("rotation", [0, 0, 0, 1])


def check_clip(anim, gl, clip):
    res = {}
    try:
        res["length"] = round(unreal.AnimationLibrary.get_sequence_length(anim), 4)
    except Exception:
        res["length"] = round(anim.get_editor_property("sequence_length"), 4)
    res["length_expected"] = round(float(gl.anim[clip]["extras"].get("duration") or 0.0), 4)
    try:
        res["frames"] = unreal.AnimationLibrary.get_num_frames(anim)
    except Exception:
        pass
    try:
        res["tracks"] = len(unreal.AnimationLibrary.get_animation_track_names(anim))
    except Exception:
        res["tracks"] = None
    L = res["length"] or res["length_expected"]
    t1 = 0.5 * L
    motion, worst = {}, 0.0
    for b in CHECK_BONES:
        try:
            ub = ue_bone(b)
            u0 = unreal.AnimationLibrary.get_bone_pose_for_time(anim, ub, 0.0, False).rotation
            u1 = unreal.AnimationLibrary.get_bone_pose_for_time(anim, ub, t1, False).rotation
        except Exception as e:  # noqa: BLE001
            motion[b] = "n/a (%s)" % e
            continue
        ue = qangle((u0.x, u0.y, u0.z, u0.w), (u1.x, u1.y, u1.z, u1.w))
        gt = qangle(gl.rot(clip, b, 0.0), gl.rot(clip, b, t1))
        motion[b] = [round(ue, 1), round(gt, 1)]
        worst = max(worst, abs(ue - gt))
    res["motion_deg"] = motion
    res["max_mismatch_deg"] = round(worst, 2)
    return res


# --------------------------------------------------------------------------------------------

def main():
    imported = os.environ.get("BR_IMPORTED")
    model = os.environ.get("BR_CHAR_MODEL", "PLAYER")
    cname = os.environ.get("BR_CHAR_NAME", "Jimmy")
    want = os.environ.get("BR_CHAR_ANIMS", "all")
    proj_saved = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
    report_path = os.environ.get("BR_REPORT") or os.path.join(proj_saved, "BullyImport", "character_report.json")
    stage_dir = os.path.join(proj_saved, "BullyImport", "stage_char")
    src = os.path.join(imported or "", "models", model + ".gltf")
    if not os.path.isfile(src):
        raise RuntimeError("%s missing: run python3 -m tools.importers ped %s --anims '*' first" % (src, model))
    with open(src) as f:
        g = json.load(f)
    char_dir = "%s/Characters/%s" % (ic.ROOT, cname)
    anim_dir = char_dir + "/Anims"
    sk_path = "%s/SK_%s" % (char_dir, cname)
    all_anims = [a["name"] for a in g.get("animations", [])]
    sel = all_anims if want in ("all", "*", "") else [a for a in all_anims if a in set(w.strip() for w in want.split(","))]
    REPORT.update({"model": model, "character": cname, "source": src, "anims_in_source": len(all_anims),
                   "anims_selected": len(sel)})
    log("character %s from %s: %d joints, %d meshes, %d/%d animations selected" % (
        cname, src, len(g["skins"][0]["joints"]), len(g.get("meshes", [])), len(sel), len(all_anims)))

    # textures (shared with the level: /Game/Imported/Textures/<TXD>/...)
    roles = ic.texture_roles({model: src}, {model: g}, imported)
    REPORT["textures"] = ic.import_textures(imported, roles)

    # ---- pass 1: skeletal mesh
    mesh_sig = gltf_sig(src, {"buffers": g.get("buffers", [])}, "mesh" + CHAR_VERSION)
    sk = EAL.load_asset(sk_path) if EAL.does_asset_exist(sk_path) else None
    if sk is not None and EAL.get_metadata_tag(sk, ic.TAG) == mesh_sig:
        log("mesh unchanged: %s" % sk_path)
        REPORT["mesh"] = {"skipped_same": True}
    else:
        staged = stage(g, src, stage_dir, "SK_" + cname, None)
        objs = run_task(staged, char_dir, "SK_" + cname, make_pipeline(False))
        log("mesh import objects: %s" % [o.get_path_name() for o in objs])
        sks = [o for o in objs if isinstance(o, unreal.SkeletalMesh)]
        if not sks:
            err("skeletal mesh import produced no SkeletalMesh (objects %s)" % [o.get_path_name() for o in objs])
            return finish(report_path)
        sk = sks[0]
        if sk.get_path_name().split(".")[0] != sk_path:
            warn("skeletal mesh landed at %s" % sk.get_path_name())
            sk_path = sk.get_path_name().split(".")[0]
        # canonical names for the skeleton / physics asset
        for o in objs:
            p = o.get_path_name().split(".")[0]
            want_name = None
            if isinstance(o, unreal.Skeleton):
                want_name = "SKEL_" + cname
            elif isinstance(o, unreal.PhysicsAsset):
                want_name = "PHYS_" + cname
            if want_name and p.rsplit("/", 1)[1] != want_name:
                dst = "%s/%s" % (char_dir, want_name)
                if EAL.does_asset_exist(dst):
                    EAL.delete_asset(dst)
                EAL.rename_asset(p, dst)
        EAL.set_metadata_tag(sk, ic.TAG, mesh_sig)
        REPORT["mesh"] = {"imported": sk_path}
    skel = sk.get_editor_property("skeleton")
    REPORT["mesh"]["material_slots_bound"] = bind_materials(sk, g, src, imported, char_dir)
    EAL.save_loaded_asset(sk, only_if_is_dirty=False)
    if skel:
        EAL.save_loaded_asset(skel, only_if_is_dirty=False)
    names = bone_names(sk)
    REPORT["mesh"]["skeleton"] = skel.get_path_name() if skel else None
    REPORT["mesh"]["bones"] = len(names) if names is not None else None
    REPORT["mesh"]["bone_names"] = names
    bounds = sk.get_bounds()
    REPORT["mesh"]["bounds_cm"] = {"origin": [round(bounds.origin.x, 1), round(bounds.origin.y, 1), round(bounds.origin.z, 1)],
                                   "extent": [round(bounds.box_extent.x, 1), round(bounds.box_extent.y, 1), round(bounds.box_extent.z, 1)]}
    log("mesh %s: bones=%s skeleton=%s bounds=%s" % (sk_path, REPORT["mesh"]["bones"], REPORT["mesh"]["skeleton"],
                                                    REPORT["mesh"]["bounds_cm"]))
    if skel is None:
        err("skeletal mesh has no skeleton")
        return finish(report_path)

    # ---- pass 2: animations (only the ones whose source hash changed)
    anim_sig = gltf_sig(src, {"buffers": g.get("buffers", [])}, "anim" + ANIM_VERSION + str(ANIM_RATE))
    todo = [a for a in sel if not (EAL.does_asset_exist("%s/%s" % (anim_dir, a)) and
                                   EAL.get_metadata_tag(EAL.load_asset("%s/%s" % (anim_dir, a)), ic.TAG) == anim_sig)]
    log("animations: %d to import, %d unchanged" % (len(todo), len(sel) - len(todo)))
    astats = {"imported": 0, "skipped_same": len(sel) - len(todo), "failed": 0}
    batch = 60
    for i in range(0, len(todo), batch):
        part = todo[i:i + batch]
        staged = stage(g, src, stage_dir, "%s_anims_%03d" % (cname, i // batch), set(part))
        objs = run_task(staged, anim_dir, None, make_pipeline(True, skel))
        got = {}
        for o in objs:
            if isinstance(o, unreal.AnimSequence):
                got[o.get_path_name().split(".")[0].rsplit("/", 1)[1]] = o
        for a in part:
            o = got.get(a)
            if o is None:
                # Interchange may prefix/suffix; find by suffix
                cand = [v for k, v in got.items() if k.endswith(a)]
                o = cand[0] if len(cand) == 1 else None
                if o is not None:
                    p = o.get_path_name().split(".")[0]
                    EAL.rename_asset(p, "%s/%s" % (anim_dir, a))
                    o = EAL.load_asset("%s/%s" % (anim_dir, a))
            if o is None:
                astats["failed"] += 1
                err("animation %s not produced (got %s)" % (a, sorted(got)[:8]))
                continue
            try:
                o.set_editor_property("enable_root_motion", False)
            except Exception:
                pass
            EAL.set_metadata_tag(o, ic.TAG, anim_sig)
            EAL.save_loaded_asset(o, only_if_is_dirty=False)
            astats["imported"] += 1
        log("animations: %d/%d" % (min(i + batch, len(todo)), len(todo)))
    REPORT["animations"] = astats

    # ---- verification of the locomotion set
    # A T-posed or broken clip would have no bone tracks or static bones. For each checked clip: the track count,
    # and for a few bones the rotation change between t=0 and t=L/2 in UE against the same quantity computed from
    # the glTF keys (a rotation angle is invariant under the glTF->UE basis change), so the UE data is shown to be
    # the source motion, not the reference pose.
    gl = GltfSampler(src, g)
    checks = {}
    for a in ("IDLE", "PLAYERWALK", "PLAYERRUN", "RUNFAST", "PLAYERWALK_START", "PLAYERRUN_START", "IDLEJUMP",
              "JOGJUMP_L", "JOGJUMP_R", "FALL", "FALL_LAND", "PLAY_LAND_HURT", "RUNSTOP0", "PLAYERWALK_STOP"):
        ap = "%s/%s" % (anim_dir, a)
        if not EAL.does_asset_exist(ap):
            if a in sel:
                err("locomotion clip missing: %s" % ap)
            continue
        try:
            checks[a] = check_clip(EAL.load_asset(ap), gl, a)
        except Exception as e:
            warn("check %s failed: %s\n%s" % (a, e, traceback.format_exc()))
    REPORT["clip_checks"] = checks
    for a, c in checks.items():
        log("clip %-16s length %.3f s (glTF %.3f) frames %s tracks %s motion UE/glTF %s" % (
            a, c.get("length", 0), c.get("length_expected", 0), c.get("frames"), c.get("tracks"), c.get("motion_deg")))
        if c.get("tracks") is not None and c["tracks"] < 30:
            err("clip %s has only %s bone tracks" % (a, c["tracks"]))
        if c.get("max_mismatch_deg", 0) > 3.0:
            err("clip %s differs from its glTF source by %.1f deg" % (a, c["max_mismatch_deg"]))
    finish(report_path)


def finish(report_path):
    REPORT["wall_seconds"] = round(time.time() - T0, 1)
    os.makedirs(os.path.dirname(report_path), exist_ok=True)
    with open(report_path, "w") as f:
        json.dump(REPORT, f, indent=1)
    log("report -> %s (errors=%d warnings=%d)" % (report_path, len(REPORT["errors"]), len(REPORT["warnings"])))
    log("BR_CHAR_IMPORT_DONE %s" % ("OK" if not REPORT["errors"] else "WITH_ERRORS"))


try:
    main()
except Exception as e:  # noqa: BLE001
    err("fatal: %s\n%s" % (e, traceback.format_exc()))
    finish(os.environ.get("BR_REPORT") or "/tmp/br_char_report.json")
