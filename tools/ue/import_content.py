# OWNER: builder-B (ue)
"""Headless UE import of the tools/importers intermediates into /Game/Imported, plus level assembly.

Runs inside UnrealEditor-Cmd (pythonscript commandlet); launch through tools/ue/run_import.sh, which
applies the GPU-slot wrapper and the headless flags. Configuration comes from environment variables
(set by run_import.sh), because the commandlet's script argument is awkward to quote:

  BR_IMPORTED   path to the importer output (contains scenes/academy.json)        [required]
  BR_SCENES     comma list of manifest scene names, or "core" (tschool,ischool), or "all" [all]
  BR_STEPS      comma list of steps: textures,materials,meshes,level             [all four]
  BR_LIMIT      import at most N models (debugging)                              [no limit]
  BR_REPORT     where to write the JSON report        [<project>/Saved/BullyImport/report.json]

Idempotent: textures and meshes carry a "br_src_sha1" metadata tag; an asset whose tag equals the
current source hash is skipped. Material instances are keyed by a hash of their parameters. The
level is deleted and rebuilt on every run. Everything written lives under /Game/Imported (git-ignored).
See tools/ue/README.md for the material model and the documented approximations.
"""
import hashlib
import json
import os
import re
import shutil
import sys
import time
import traceback

import unreal

ROOT = "/Game/Imported"
TEX_ROOT = ROOT + "/Textures"
MAT_ROOT = ROOT + "/Materials"
MESH_ROOT = ROOT + "/Meshes"
MAP_PATH = ROOT + "/Maps/Bullworth"
MASTER_VERSION = "5"          # bump to force master-material graph rebuild (instances keep their paths)
MESH_VERSION = "4"            # bump to force mesh re-import + slot re-binding
TEX_VERSION = "2"             # bump when texture import settings change
TAG = "br_src_sha1"

AT = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary

T0 = time.time()
REPORT = {"format": "bully-remake.ue-import-report/1", "steps": {}, "errors": [], "warnings": []}


def log(msg):
    unreal.log("BR_IMPORT [%6.1fs] %s" % (time.time() - T0, msg))


def warn(msg):
    REPORT["warnings"].append(msg)
    unreal.log_warning("BR_IMPORT WARN " + msg)


def err(msg):
    REPORT["errors"].append(msg)
    unreal.log_error("BR_IMPORT ERROR " + msg)


def sanitize(name):
    s = re.sub(r"[^A-Za-z0-9_]", "_", name)
    return s or "_"


def sha1_file(path, extra=b""):
    h = hashlib.sha1(extra)
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def tag_matches(asset_path, sig):
    if not EAL.does_asset_exist(asset_path):
        return False
    obj = EAL.load_asset(asset_path)
    return obj is not None and EAL.get_metadata_tag(obj, TAG) == sig


# --------------------------------------------------------------------------------------------
# manifest

def load_manifest(imported):
    with open(os.path.join(imported, "scenes", "academy.json")) as f:
        return json.load(f)


def select_scenes(manifest, spec):
    names = [s["name"] for s in manifest["scenes"]]
    if not spec or spec == "all":
        return names
    if spec == "core":
        return ["tschool", "ischool"]
    want = [s.strip() for s in spec.split(",") if s.strip()]
    for w in want:
        if w not in names:
            raise RuntimeError("unknown scene %r (have %s)" % (w, names))
    return want


def collect(manifest, imported, scenes, limit):
    """Models referenced by visible placements in the selected scenes, with parsed glTF."""
    placements = [p for p in manifest["placements"] if p["scene"] in scenes]
    models = {}
    for p in placements:
        if not p["visible"] or not p["gltf"]:
            continue
        if p["model"] not in models:
            models[p["model"]] = os.path.join(imported, p["gltf"])
    names = sorted(models)
    if limit:
        names = names[:limit]
    gltfs = {}
    for n in names:
        with open(models[n]) as f:
            gltfs[n] = json.load(f)
    return placements, {n: models[n] for n in names}, gltfs


# --------------------------------------------------------------------------------------------
# textures

ROLE_PRIORITY = {"normal": 3, "glow": 2, "base": 1, "gloss": 0}


def texture_roles(gltf_paths, gltfs, imported):
    """abs png path -> role. A texture used in several roles keeps the most restrictive one."""
    roles = {}
    for name, g in gltfs.items():
        base_dir = os.path.dirname(gltf_paths[name])
        for m in g.get("materials", []):
            for role, t in m.get("extras", {}).get("textures", {}).items():
                uri = t.get("uri")
                if not uri:
                    continue
                p = os.path.normpath(os.path.join(base_dir, uri))
                old = roles.get(p)
                if old is None or ROLE_PRIORITY.get(role, 0) > ROLE_PRIORITY.get(old, 0):
                    roles[p] = role
    return roles


def texture_asset_path(imported, png):
    rel = os.path.relpath(png, os.path.join(imported, "textures"))
    txd, fname = os.path.split(rel)
    return "%s/%s/%s" % (TEX_ROOT, sanitize(txd), sanitize(os.path.splitext(fname)[0]))


def import_textures(imported, roles):
    stats = {"total": len(roles), "imported": 0, "skipped_same": 0, "missing_source": 0, "failed": 0}
    todo = []
    sigs = {}
    for png, role in sorted(roles.items()):
        if not os.path.isfile(png):
            stats["missing_source"] += 1
            warn("texture source missing: %s" % png)
            continue
        ap = texture_asset_path(imported, png)
        sig = sha1_file(png, (role + TEX_VERSION).encode())
        sigs[ap] = (sig, role, png)
        if tag_matches(ap, sig):
            stats["skipped_same"] += 1
            continue
        todo.append(ap)
    log("textures: %d to import, %d unchanged" % (len(todo), stats["skipped_same"]))
    batch = 200
    for i in range(0, len(todo), batch):
        tasks = []
        for ap in todo[i:i + batch]:
            sig, role, png = sigs[ap]
            t = unreal.AssetImportTask()
            t.set_editor_property("filename", png)
            t.set_editor_property("destination_path", ap.rsplit("/", 1)[0])
            t.set_editor_property("destination_name", ap.rsplit("/", 1)[1])
            t.set_editor_property("automated", True)
            t.set_editor_property("replace_existing", True)
            t.set_editor_property("save", False)
            tasks.append((ap, t))
        AT.import_asset_tasks([t for _, t in tasks])
        for ap, t in tasks:
            sig, role, png = sigs[ap]
            tex = EAL.load_asset(ap) if EAL.does_asset_exist(ap) else None
            if not isinstance(tex, unreal.Texture2D):
                stats["failed"] += 1
                err("texture import failed: %s -> %s" % (png, ap))
                continue
            if role == "normal":
                tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
                tex.set_editor_property("srgb", False)
                tex.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_WORLD_NORMAL_MAP)
            else:
                # gloss (_s) stays sRGB so it can share the engine's sRGB Black default in a Color
                # sampler (a Linear Color sampler with an sRGB default fails to compile)
                tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT)
                tex.set_editor_property("srgb", True)
            EAL.set_metadata_tag(tex, TAG, sig)
            EAL.save_loaded_asset(tex, only_if_is_dirty=False)
            stats["imported"] += 1
        log("textures: %d/%d" % (min(i + batch, len(todo)), len(todo)))
    return stats


# --------------------------------------------------------------------------------------------
# master materials

def load_engine(path):
    obj = unreal.load_asset(path)
    if obj is None:
        raise RuntimeError("engine asset missing: " + path)
    return obj


def ensure_mpc():
    path = MAT_ROOT + "/MPC_Bully"
    if EAL.does_asset_exist(path):
        return EAL.load_asset(path)
    mpc = AT.create_asset("MPC_Bully", MAT_ROOT, unreal.MaterialParameterCollection,
                          unreal.MaterialParameterCollectionFactoryNew())
    p = unreal.CollectionScalarParameter()
    p.set_editor_property("parameter_name", "BakedVertexLight")
    p.set_editor_property("default_value", 1.0)
    mpc.set_editor_property("scalar_parameters", [p])
    EAL.save_loaded_asset(mpc, only_if_is_dirty=False)
    return mpc


MASTERS = {}   # (blend, two_sided) -> material


def build_master(name, blend, two_sided, mpc):
    path = "%s/%s" % (MAT_ROOT, name)
    if EAL.does_asset_exist(path):
        mat = EAL.load_asset(path)
        if EAL.get_metadata_tag(mat, "br_master_version") == MASTER_VERSION:
            return mat
        MEL.delete_all_material_expressions(mat)
    else:
        mat = AT.create_asset(name, MAT_ROOT, unreal.Material, unreal.MaterialFactoryNew())

    black = load_engine("/Engine/EngineResources/Black")
    white = load_engine("/Engine/EngineResources/WhiteSquareTexture")
    flatn = load_engine("/Engine/EngineMaterials/DefaultNormal")

    mat.set_editor_property("two_sided", two_sided)
    mat.set_editor_property("blend_mode", {
        "opaque": unreal.BlendMode.BLEND_OPAQUE,
        "masked": unreal.BlendMode.BLEND_MASKED,
        "translucent": unreal.BlendMode.BLEND_TRANSLUCENT,
        "additive": unreal.BlendMode.BLEND_ADDITIVE,
    }[blend])
    unlit = blend == "additive"
    mat.set_editor_property("shading_model",
                            unreal.MaterialShadingModel.MSM_UNLIT if unlit else unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    if blend == "masked":
        mat.set_editor_property("opacity_mask_clip_value", 0.5)
    if blend == "translucent":
        mat.set_editor_property("translucency_lighting_mode",
                                unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)

    def expr(cls, x, y):
        return MEL.create_material_expression(mat, cls, x, y)

    def tex_param(pname, default, sampler, x, y):
        e = expr(unreal.MaterialExpressionTextureSampleParameter2D, x, y)
        e.set_editor_property("parameter_name", pname)
        e.set_editor_property("texture", default)
        e.set_editor_property("sampler_type", sampler)
        return e

    def scalar(pname, value, x, y):
        e = expr(unreal.MaterialExpressionScalarParameter, x, y)
        e.set_editor_property("parameter_name", pname)
        e.set_editor_property("default_value", value)
        return e

    def connect(a, aout, b, bin_):
        if not MEL.connect_material_expressions(a, aout, b, bin_):
            raise RuntimeError("connect failed %s.%s -> %s.%s in %s" % (a.get_name(), aout, b.get_name(), bin_, name))

    def out(e, eout, prop):
        if not MEL.connect_material_property(e, eout, prop):
            raise RuntimeError("property connect failed %s in %s" % (prop, name))

    base = tex_param("BaseColor", white, unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, -1400, -300)
    vc = expr(unreal.MaterialExpressionVertexColor, -1400, 100)
    k = expr(unreal.MaterialExpressionCollectionParameter, -1400, 300)
    k.set_editor_property("collection", mpc)
    k.set_editor_property("parameter_name", "BakedVertexLight")
    tint = expr(unreal.MaterialExpressionVectorParameter, -1400, -500)
    tint.set_editor_property("parameter_name", "DiffuseTint")
    tint.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))

    # baked vertex lighting: lerp(1, VC.rgb, BakedVertexLight)
    lerp = expr(unreal.MaterialExpressionLinearInterpolate, -1100, 150)
    lerp.set_editor_property("const_a", 1.0)
    connect(vc, "", lerp, "B")
    connect(k, "", lerp, "Alpha")
    m1 = expr(unreal.MaterialExpressionMultiply, -900, -300)
    connect(base, "RGB", m1, "A")
    connect(tint, "", m1, "B")
    color = expr(unreal.MaterialExpressionMultiply, -700, -200)
    connect(m1, "", color, "A")
    connect(lerp, "", color, "B")

    # alpha = tex.a * vc.a
    alpha = expr(unreal.MaterialExpressionMultiply, -900, 300)
    connect(base, "A", alpha, "A")
    connect(vc, "A", alpha, "B")

    if unlit:
        # additive: emissive = rgb * alpha (Gamebryo SRC_ALPHA, ONE)
        em = expr(unreal.MaterialExpressionMultiply, -400, -100)
        connect(color, "", em, "A")
        connect(alpha, "", em, "B")
        gain = scalar("EmissiveGain", 1.0, -600, 0)
        em2 = expr(unreal.MaterialExpressionMultiply, -250, -100)
        connect(em, "", em2, "A")
        connect(gain, "", em2, "B")
        out(em2, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    else:
        out(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
        normal = tex_param("Normal", flatn, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, -1400, 600)
        out(normal, "RGB", unreal.MaterialProperty.MP_NORMAL)
        gloss = tex_param("Gloss", black, unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, -1400, 900)
        # Specular = SpecularBase + gloss.r * SpecularScale ; Roughness = RoughnessMax - gloss.r * RoughnessRange
        sb = scalar("SpecularBase", 0.35, -1100, 800)
        ss = scalar("SpecularScale", 0.5, -1100, 900)
        sm = expr(unreal.MaterialExpressionMultiply, -900, 850)
        connect(gloss, "R", sm, "A")
        connect(ss, "", sm, "B")
        sa = expr(unreal.MaterialExpressionAdd, -700, 850)
        connect(sb, "", sa, "A")
        connect(sm, "", sa, "B")
        out(sa, "", unreal.MaterialProperty.MP_SPECULAR)
        rmax = scalar("RoughnessMax", 0.85, -1100, 1000)
        rr = scalar("RoughnessRange", 0.35, -1100, 1100)
        rm = expr(unreal.MaterialExpressionMultiply, -900, 1050)
        connect(gloss, "R", rm, "A")
        connect(rr, "", rm, "B")
        rs = expr(unreal.MaterialExpressionSubtract, -700, 1050)
        connect(rmax, "", rs, "A")
        connect(rm, "", rs, "B")
        out(rs, "", unreal.MaterialProperty.MP_ROUGHNESS)
        glow = tex_param("Glow", black, unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, -1400, 1300)
        gg = scalar("GlowGain", 1.0, -1100, 1400)
        gm = expr(unreal.MaterialExpressionMultiply, -900, 1350)
        connect(glow, "RGB", gm, "A")
        connect(gg, "", gm, "B")
        out(gm, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        if blend == "masked":
            # pass when alpha > AlphaCutoff with a fixed 0.5 clip value (no per-instance permutation)
            cut = scalar("AlphaCutoff", 0.5, -1100, 400)
            sub = expr(unreal.MaterialExpressionSubtract, -700, 350)
            connect(alpha, "", sub, "A")
            connect(cut, "", sub, "B")
            add = expr(unreal.MaterialExpressionAdd, -500, 350)
            add.set_editor_property("const_b", 0.5)
            connect(sub, "", add, "A")
            out(add, "", unreal.MaterialProperty.MP_OPACITY_MASK)
        elif blend == "translucent":
            out(alpha, "", unreal.MaterialProperty.MP_OPACITY)

    MEL.layout_material_expressions(mat)
    MEL.recompile_material(mat)
    EAL.set_metadata_tag(mat, "br_master_version", MASTER_VERSION)
    EAL.save_loaded_asset(mat, only_if_is_dirty=False)
    return mat


def ensure_masters():
    mpc = ensure_mpc()
    for blend in ("opaque", "masked", "translucent", "additive"):
        for ts in (False, True):
            name = "M_Bully_%s%s" % (blend.capitalize(), "_TwoSided" if ts else "")
            MASTERS[(blend, ts)] = build_master(name, blend, ts, mpc)
    return {"masters": len(MASTERS), "mpc": "MPC_Bully"}


# --------------------------------------------------------------------------------------------
# material instances

def mat_key(m, base_dir, imported):
    ex = m.get("extras", {})
    texs = {}
    for role, t in ex.get("textures", {}).items():
        if t.get("uri"):
            texs[role] = texture_asset_path(imported, os.path.normpath(os.path.join(base_dir, t["uri"])))
    gm = ex.get("gamebryo", {}).get("material", {})
    spec = gm.get("specular", [0.0, 0.0, 0.0])
    diff = gm.get("diffuse", [1.0, 1.0, 1.0])
    cutoff = max(float(m.get("alphaCutoff", 0.5)), 1.0 / 255.0)
    ga = ex.get("gamebryo", {}).get("alpha") or {}
    if ex.get("blend_mode") == "masked" and ga.get("blend") and ga.get("test"):
        # Gamebryo alpha blend AND test (e.g. wall ivy decals, threshold 4/255): the soft alpha was
        # meant to be blended; a near-zero test threshold under BLEND_Masked shows the black
        # texels. Masked at 0.5 keeps them opaque-sorted and crisp.
        cutoff = max(cutoff, 0.5)
    d = {
        "blend": ex.get("blend_mode", "opaque"),
        "two_sided": bool(m.get("doubleSided", False)),
        "tex": texs,
        "cutoff": round(cutoff, 4),
        "spec": round(sum(spec) / 3.0, 4),
        "diffuse": [round(c, 4) for c in diff],
        "alpha": round(float(gm.get("alpha", 1.0)), 4),
    }
    return d, hashlib.sha1(json.dumps(d, sort_keys=True).encode()).hexdigest()


MI_CACHE = {}   # key hash -> MaterialInstanceConstant
MI_STATS = {"created": 0, "reused_existing": 0, "missing_texture_refs": 0}


def get_mi(d, h):
    if h in MI_CACHE:
        return MI_CACHE[h]
    base_tex = d["tex"].get("base")
    stem = sanitize(base_tex.rsplit("/", 1)[1]) if base_tex else "untextured"
    name = "MI_%s_%s" % (stem[:48], h[:8])
    path = "%s/Instances/%s" % (MAT_ROOT, name)
    if EAL.does_asset_exist(path):
        mi = EAL.load_asset(path)
        if EAL.get_metadata_tag(mi, "br_key") == h and EAL.get_metadata_tag(mi, "br_master_version") == MASTER_VERSION:
            MI_CACHE[h] = mi
            MI_STATS["reused_existing"] += 1
            return mi
    else:
        mi = AT.create_asset(name, MAT_ROOT + "/Instances", unreal.MaterialInstanceConstant,
                             unreal.MaterialInstanceConstantFactoryNew())
    MEL.set_material_instance_parent(mi, MASTERS[(d["blend"], d["two_sided"])])
    for role, pname in (("base", "BaseColor"), ("normal", "Normal"), ("gloss", "Gloss"), ("glow", "Glow")):
        ap = d["tex"].get(role)
        if not ap:
            continue
        if d["blend"] == "additive" and role != "base":
            continue
        if not EAL.does_asset_exist(ap):
            MI_STATS["missing_texture_refs"] += 1
            warn("material %s: %s texture %s not imported" % (name, role, ap))
            continue
        MEL.set_material_instance_texture_parameter_value(mi, pname, EAL.load_asset(ap))
    if d["blend"] != "additive":
        # Gamebryo specular colour (0..1, typically 0.08-0.24) scales the gloss-map contribution
        MEL.set_material_instance_scalar_parameter_value(mi, "SpecularScale", min(1.0, 0.25 + 2.0 * d["spec"]))
    if d["blend"] == "masked":
        MEL.set_material_instance_scalar_parameter_value(mi, "AlphaCutoff", d["cutoff"])
    dr, dg, db = d["diffuse"]
    MEL.set_material_instance_vector_parameter_value(mi, "DiffuseTint", unreal.LinearColor(dr, dg, db, d["alpha"]))
    MEL.update_material_instance(mi)
    EAL.set_metadata_tag(mi, "br_key", h)
    EAL.set_metadata_tag(mi, "br_master_version", MASTER_VERSION)
    EAL.save_loaded_asset(mi, only_if_is_dirty=False)
    MI_CACHE[h] = mi
    MI_STATS["created"] += 1
    return mi


# --------------------------------------------------------------------------------------------
# meshes

def stage_gltf(name, src_path, g, stage_dir):
    """Copy of the glTF with unique material names M000.. and no images (textures are imported
    separately and bound through material instances), so slot -> material is unambiguous."""
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
    src_dir = os.path.dirname(src_path)
    os.makedirs(stage_dir, exist_ok=True)
    for b in g.get("buffers", []):
        uri = b.get("uri", "")
        if uri and not uri.startswith("data:"):
            dst = os.path.join(stage_dir, os.path.basename(uri))
            shutil.copyfile(os.path.join(src_dir, uri), dst)
            b["uri"] = os.path.basename(uri)
    out = os.path.join(stage_dir, name + ".gltf")
    with open(out, "w") as f:
        json.dump(g, f)
    return out


def make_pipeline(two_sided_df):
    p = unreal.InterchangeGenericAssetsPipeline()
    p.set_editor_property("use_source_name_for_asset", True)
    cm = p.get_editor_property("common_meshes_properties")
    cm.set_editor_property("force_all_mesh_as_type", unreal.InterchangeForceMeshType.IFMT_STATIC_MESH)
    cm.set_editor_property("vertex_color_import_option", unreal.InterchangeVertexColorImportOption.IVCIO_REPLACE)
    cm.set_editor_property("recompute_normals", False)
    cm.set_editor_property("recompute_tangents", True)
    cm.set_editor_property("use_mikk_t_space", True)
    cm.set_editor_property("import_lods", False)
    mp = p.get_editor_property("mesh_pipeline")
    mp.set_editor_property("import_static_meshes", True)
    mp.set_editor_property("import_skeletal_meshes", False)
    mp.set_editor_property("build_nanite", False)
    mp.set_editor_property("collision", False)
    mp.set_editor_property("generate_lightmap_u_vs", False)
    mp.set_editor_property("generate_distance_field_as_if_two_sided", two_sided_df)
    matp = p.get_editor_property("material_pipeline")
    matp.set_editor_property("import_materials", False)
    tp = matp.get_editor_property("texture_pipeline")
    tp.set_editor_property("import_textures", False)
    ap = p.get_editor_property("animation_pipeline")
    try:
        ap.set_editor_property("import_animations", False)
    except Exception:
        pass
    return p


def bind_slots(name, sm, mats, base_dir, imported, stats):
    """Slot M### -> material instance for glTF material ###. Returns True if anything changed.
    Runs for skipped meshes too, so material-model changes never need a mesh re-import."""
    changed = False
    static_mats = list(sm.get_editor_property("static_materials"))
    for si, sml in enumerate(static_mats):
        slot = str(sml.get_editor_property("material_slot_name"))
        mm = re.match(r"^M(\d{3})", slot)
        mi_index = int(mm.group(1)) if mm else None
        if mi_index is None or mi_index >= len(mats):
            stats["slots_unbound"] += 1
            warn("mesh %s slot %d '%s' has no glTF material" % (name, si, slot))
            continue
        d, h = mat_key(mats[mi_index], base_dir, imported)
        mi = get_mi(d, h)
        cur = sml.get_editor_property("material_interface")
        if cur is None or cur.get_path_name() != mi.get_path_name():
            sm.set_material(si, mi)
            changed = True
        stats["slots_bound"] += 1
    return changed


def import_meshes(imported, gltf_paths, gltfs, stage_dir):
    stats = {"total": len(gltfs), "imported": 0, "skipped_same": 0, "failed": 0, "slots_bound": 0,
             "slots_unbound": 0, "seconds": 0.0}
    t_start = time.time()
    meshes = {}
    for idx, (name, g) in enumerate(sorted(gltfs.items())):
        src = gltf_paths[name]
        bin_extra = b""
        for b in g.get("buffers", []):
            if b.get("uri") and not b["uri"].startswith("data:"):
                with open(os.path.join(os.path.dirname(src), b["uri"]), "rb") as f:
                    bin_extra += hashlib.sha1(f.read()).digest()
        sig = sha1_file(src, bin_extra + MESH_VERSION.encode())
        ap = "%s/%s" % (MESH_ROOT, sanitize(name))
        base_dir = os.path.dirname(src)
        mats = g.get("materials", [])
        if tag_matches(ap, sig):
            sm = EAL.load_asset(ap)
            meshes[name] = sm
            stats["skipped_same"] += 1
            try:
                if bind_slots(name, sm, mats, base_dir, imported, stats):
                    EAL.save_loaded_asset(sm, only_if_is_dirty=False)
                    stats["rebound_saved"] = stats.get("rebound_saved", 0) + 1
            except Exception as e:
                stats["failed"] += 1
                err("mesh %s rebinding: %s\n%s" % (name, e, traceback.format_exc()))
            continue
        try:
            staged = stage_gltf(sanitize(name), src, g, stage_dir)
            two_sided = any(m.get("doubleSided") for m in mats)
            task = unreal.AssetImportTask()
            task.set_editor_property("filename", staged)
            task.set_editor_property("destination_path", MESH_ROOT)
            task.set_editor_property("destination_name", sanitize(name))
            task.set_editor_property("automated", True)
            task.set_editor_property("replace_existing", True)
            task.set_editor_property("save", False)
            stack = unreal.InterchangePipelineStackOverride()
            stack.add_pipeline(make_pipeline(two_sided))
            task.set_editor_property("options", stack)
            AT.import_asset_tasks([task])
            sm = EAL.load_asset(ap) if EAL.does_asset_exist(ap) else None
            if not isinstance(sm, unreal.StaticMesh):
                found = [o for o in (task.get_objects() or []) if isinstance(o, unreal.StaticMesh)]
                if found:
                    sm = found[0]
                    warn("mesh %s landed at %s (expected %s)" % (name, sm.get_path_name(), ap))
            if not isinstance(sm, unreal.StaticMesh):
                stats["failed"] += 1
                err("mesh import failed: %s (objects: %s)" % (name, [o.get_path_name() for o in (task.get_objects() or [])]))
                continue
            bind_slots(name, sm, mats, base_dir, imported, stats)
            body = sm.get_editor_property("body_setup")
            if body:
                body.set_editor_property("collision_trace_flag", unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
            EAL.set_metadata_tag(sm, TAG, sig)
            EAL.save_loaded_asset(sm, only_if_is_dirty=False)
            meshes[name] = sm
            stats["imported"] += 1
        except Exception as e:
            stats["failed"] += 1
            err("mesh %s: %s\n%s" % (name, e, traceback.format_exc()))
        if (idx + 1) % 25 == 0:
            log("meshes: %d/%d (%.1fs)" % (idx + 1, len(gltfs), time.time() - t_start))
    stats["seconds"] = round(time.time() - t_start, 1)
    stats["material_instances"] = dict(MI_STATS)
    return stats, meshes


# --------------------------------------------------------------------------------------------
# level

def spawn(cls, loc, rot=None, label=None, folder=None):
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    a = eas.spawn_actor_from_class(cls, loc, rot or unreal.Rotator(0, 0, 0))
    if label:
        a.set_actor_label(label)
    if folder:
        a.set_folder_path(folder)
    return a


def build_level(manifest, placements, scenes):
    stats = {"placed": 0, "skipped_invisible": 0, "skipped_no_gltf": 0, "skipped_no_mesh": 0}
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if EAL.does_asset_exist(MAP_PATH):
        # rebuilt every run: load it and destroy every actor (deleting the package and calling
        # new_level in the same process fails: the asset registry still lists the deleted map)
        if not les.load_level(MAP_PATH):
            raise RuntimeError("load_level failed for " + MAP_PATH)
        old = [a for a in eas.get_all_level_actors() if not isinstance(a, unreal.WorldSettings)]
        if old:
            eas.destroy_actors(old)
        stats["cleared_actors"] = len(old)
    elif not les.new_level(MAP_PATH):
        raise RuntimeError("new_level failed for " + MAP_PATH)
    mesh_cache = {}
    for p in placements:
        if not p["visible"]:
            stats["skipped_invisible"] += 1
            continue
        if not p["gltf"]:
            stats["skipped_no_gltf"] += 1
            continue
        name = p["model"]
        if name not in mesh_cache:
            ap = "%s/%s" % (MESH_ROOT, sanitize(name))
            mesh_cache[name] = EAL.load_asset(ap) if EAL.does_asset_exist(ap) else None
        sm = mesh_cache[name]
        if sm is None:
            stats["skipped_no_mesh"] += 1
            continue
        ue = p["ue"]
        loc = unreal.Vector(*ue["location_cm"])
        qx, qy, qz, qw = ue["rotation_quat_xyzw"]
        rot = unreal.Quat(qx, qy, qz, qw).rotator()
        # spawn_actor_from_object goes through actor factories, which refuse in the commandlet;
        # spawning the class and assigning the mesh works headless.
        a = eas.spawn_actor_from_class(unreal.StaticMeshActor, loc, rot)
        if a is not None:
            a.static_mesh_component.set_static_mesh(sm)
        if a is None:
            stats["skipped_no_mesh"] += 1
            warn("spawn failed %s/%d %s" % (p["scene"], p["index"], name))
            continue
        a.set_actor_scale3d(unreal.Vector(*ue["scale"]))
        a.set_actor_label("%s_%03d_%s" % (p["scene"], p["index"], name))
        a.set_folder_path("Imported/%s" % p["scene"])
        a.set_editor_property("tags", [unreal.Name("br_scene:" + p["scene"]), unreal.Name("br_model:" + name)])
        stats["placed"] += 1

    # first lighting rig: sun + sky atmosphere + realtime skylight + height fog (Lumen GI/reflections
    # are enabled project-wide in DefaultEngine.ini)
    sun = spawn(unreal.DirectionalLight, unreal.Vector(0, 0, 10000), unreal.Rotator(roll=0, pitch=-38, yaw=200),
                "BR_Sun", "Lighting")
    sc = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sc.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    sc.set_editor_property("intensity", 10.0)
    sc.set_editor_property("atmosphere_sun_light", True)
    sky = spawn(unreal.SkyLight, unreal.Vector(0, 0, 12000), None, "BR_SkyLight", "Lighting")
    skc = sky.get_component_by_class(unreal.SkyLightComponent)
    skc.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    skc.set_editor_property("real_time_capture", True)
    spawn(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0), None, "BR_SkyAtmosphere", "Lighting")
    fog = spawn(unreal.ExponentialHeightFog, unreal.Vector(0, 0, 0), None, "BR_HeightFog", "Lighting")
    fc = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
    fc.set_editor_property("fog_density", 0.01)
    fc.set_editor_property("fog_height_falloff", 0.1)
    ppv = spawn(unreal.PostProcessVolume, unreal.Vector(0, 0, 0), None, "BR_PostProcess", "Lighting")
    ppv.set_editor_property("unbound", True)
    # spawn point in front of the main gates (game ~(322, -73)), facing the main building
    spawn(unreal.PlayerStart, unreal.Vector(32200, 7300, 1200), unreal.Rotator(0, 0, 180), "BR_PlayerStart", "Gameplay")

    if not les.save_current_level():
        raise RuntimeError("save_current_level failed")
    stats["actors_total"] = len(eas.get_all_level_actors())
    return stats


# --------------------------------------------------------------------------------------------

def main():
    imported = os.environ.get("BR_IMPORTED")
    if not imported or not os.path.isfile(os.path.join(imported, "scenes", "academy.json")):
        raise RuntimeError("BR_IMPORTED must point at the importer output (scenes/academy.json missing)")
    steps = [s.strip() for s in os.environ.get("BR_STEPS", "textures,materials,meshes,level").split(",")]
    limit = int(os.environ.get("BR_LIMIT", "0") or 0)
    proj_saved = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
    report_path = os.environ.get("BR_REPORT") or os.path.join(proj_saved, "BullyImport", "report.json")
    stage_dir = os.path.join(proj_saved, "BullyImport", "stage")

    manifest = load_manifest(imported)
    scenes = select_scenes(manifest, os.environ.get("BR_SCENES", "all"))
    placements, gltf_paths, gltfs = collect(manifest, imported, scenes, limit)
    REPORT.update({"imported_dir": imported, "scenes": scenes, "steps_requested": steps, "limit": limit,
                   "models_selected": len(gltfs),
                   "placements_selected": len(placements)})
    log("scenes=%s models=%d placements=%d steps=%s" % (",".join(scenes), len(gltfs), len(placements), steps))

    def run(step, fn, *a):
        t = time.time()
        log("step %s ..." % step)
        res = fn(*a)
        if isinstance(res, tuple):
            res = res[0]
        res = dict(res) if isinstance(res, dict) else {"result": str(res)}
        res["seconds"] = round(time.time() - t, 1)
        REPORT["steps"][step] = res
        log("step %s done: %s" % (step, json.dumps(res)))

    try:
        if "textures" in steps:
            run("textures", import_textures, imported, texture_roles(gltf_paths, gltfs, imported))
        ensure_masters_done = False
        if "materials" in steps or "meshes" in steps:
            run("materials", ensure_masters)
            ensure_masters_done = True
        if "meshes" in steps:
            run("meshes", import_meshes, imported, gltf_paths, gltfs, stage_dir)
        if "level" in steps:
            run("level", build_level, manifest, placements, scenes)
    except Exception as e:
        err("fatal: %s\n%s" % (e, traceback.format_exc()))
    REPORT["wall_seconds"] = round(time.time() - T0, 1)
    os.makedirs(os.path.dirname(report_path), exist_ok=True)
    with open(report_path, "w") as f:
        json.dump(REPORT, f, indent=1)
    log("report -> %s (errors=%d warnings=%d)" % (report_path, len(REPORT["errors"]), len(REPORT["warnings"])))
    log("BR_IMPORT_DONE %s" % ("OK" if not REPORT["errors"] else "WITH_ERRORS"))


main()
