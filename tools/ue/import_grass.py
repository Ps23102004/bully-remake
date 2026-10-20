# OWNER: builder-A (importers)
"""Instanced grass over the Bullworth lawns, into the SAVED map. Run through tools/ue/run_import_grass.sh.

NOT YET RUN IN THE EDITOR (written without access to Unreal): the first run may need API-name fixes; every
engine call that is not already used by import_content.py is wrapped so the failure names the call.

The AE game draws grass at run time (DL_GrassNN stubs, docs/re-notes/ae-grass.md), so the importer
(tools/importers/convert_ae.py, `world`) scatters tufts instead and writes
  <imported>/grass/index.json            tuft assets, one entry per ground model (file, tile flag, counts)
  <imported>/grass/<model>.grass.json    rows x_cm, y_cm, z_cm, yaw_deg, scale_pct per tuft id, model-local UE space
  <imported>/grass/tuft_<id>.gltf        two crossed alpha cards (+ textures/ae/<stem>.png)
This script imports the three tuft meshes through import_content.py's own texture/material/mesh helpers (same
master materials, masked + two-sided), then, for every placement of a ground tile that has a DL_Grass partner,
spawns one StaticMeshActor at the placement transform carrying one HierarchicalInstancedStaticMeshComponent per
tuft id. Idempotent: actors tagged `br_grass` are destroyed first. Re-run it after every run_import.sh (the level
step rebuilds the map and drops them).

Environment (set by run_import_grass.sh):
  BR_IMPORTED        importer output dir (contains scenes/academy.json and grass/)            [required]
  BR_GRASS_ALL       1 = also place non-tile models (cliffs, mounds, the prep-school ground)    [0]
  BR_GRASS_DENSITY   0..1, keep this fraction of the instances (deterministic stride)            [1]
  BR_GRASS_CULL      "start,end" cm: instances fade out between the two distances                [2200,3800]
  BR_GRASS_DL_M      a ground tile gets grass if a dl_grass* placement is within this many m     [25]
  BR_GRASS_DRY       1 = import assets and count, spawn nothing                                  [0]
  BR_GRASS_REPORT    JSON report path            [<project>/Saved/BullyImport/grass_report.json]
"""
import hashlib
import json
import math
import os
import sys
import time
import traceback

os.environ["BR_NO_MAIN"] = "1"      # import_content only exposes its helpers (import_character.py does the same)
import unreal  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import import_content as ic  # noqa: E402

TAG = "br_grass"
T0 = time.time()
REPORT = {"format": "bully-remake.ue-grass-report/1", "errors": [], "warnings": []}


def log(msg):
    unreal.log("BR_GRASS [%6.1fs] %s" % (time.time() - T0, msg))


def warn(msg):
    REPORT["warnings"].append(msg)
    unreal.log_warning("BR_GRASS WARN " + msg)


def err(msg):
    REPORT["errors"].append(msg)
    unreal.log_error("BR_GRASS ERROR " + msg)


def env_float(name, default):
    try:
        return float(os.environ.get(name, default))
    except ValueError:
        return float(default)


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def allow_instancing(master):
    """HISM needs the master's InstancedStaticMeshes usage flag: without it every tuft draws with the engine's
    default (grey checker) material ("missing usage flag InstancedStaticMeshes" in the game log). The flag survives
    import_content.build_master rebuilds (it reuses the asset and only replaces the expressions)."""
    if master.get_editor_property("used_with_instanced_static_meshes"):
        return
    master.set_editor_property("used_with_instanced_static_meshes", True)
    unreal.MaterialEditingLibrary.recompile_material(master)
    unreal.EditorAssetLibrary.save_loaded_asset(master, only_if_is_dirty=False)
    log("set InstancedStaticMeshes usage on %s" % master.get_name())


def tuft_assets(imported, index, stage_dir):
    """Import the tuft textures/meshes; returns {tuft id: (StaticMesh, gltf dict, gltf path, mesh name)}."""
    gltf_paths, gltfs = {}, {}
    for tid, info in index["tufts"].items():
        name = "grass_tuft_" + tid
        gltf_paths[name] = os.path.join(imported, info["gltf"])
        gltfs[name] = load_json(gltf_paths[name])
    roles = ic.texture_roles(gltf_paths, gltfs, imported)
    REPORT["textures"] = ic.import_textures(imported, roles)
    ic.ensure_masters()
    for two_sided in (False, True):
        allow_instancing(ic.MASTERS[("masked", two_sided)])
    stats, meshes = ic.import_meshes(imported, gltf_paths, gltfs, stage_dir)
    REPORT["meshes"] = stats
    out = {}
    for tid in index["tufts"]:
        name = "grass_tuft_" + tid
        if name in meshes:
            out[tid] = (meshes[name], gltfs[name], gltf_paths[name], name)
        else:
            err("tuft mesh %s was not imported" % name)
    return out


def tinted_mi(asset, tint):
    """Material instance for this tuft with its DiffuseTint set to the importer's per-field tint."""
    _sm, g, path, _name = asset
    d, _h = ic.mat_key(g["materials"][0], os.path.dirname(path), IMPORTED)
    d = dict(d)
    d["diffuse"] = [round(float(c), 2) for c in tint]
    h = hashlib.sha1(json.dumps(d, sort_keys=True).encode()).hexdigest()
    return ic.get_mi(d, h)


def partner_near(p, dl_positions, radius_m):
    x, y = p["game"]["position"][:2]
    return any(math.hypot(x - dx, y - dy) <= radius_m for dx, dy in dl_positions)


def select_fields(manifest, index, all_models, radius_m):
    """[(placement, model grass doc path, doc index entry)] for the placements that get grass."""
    dl = [p["game"]["position"][:2] for p in manifest["placements"] if p["model"].lower().startswith("dl_grass")]
    log("%d dl_grass placements (the game's own per-tile grass markers)" % len(dl))
    out = []
    for p in manifest["placements"]:
        if not p["visible"] or not p["gltf"]:
            continue
        entry = index["models"].get(p["model"])
        if entry is None:
            continue
        if entry["tile"] and not partner_near(p, dl, radius_m) and not all_models:
            continue
        if not entry["tile"] and not all_models:
            continue
        out.append((p, entry))
    return out


def new_hism(actor):
    """Add an instance HISM component under the actor's root, as the editor's "Add Component" does
    (Actor.add_component_by_class is not exposed to editor Python in UE 5.8)."""
    sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    root = sds.k2_gather_subobject_data_for_instance(actor)[0]
    handle, fail = sds.add_new_subobject(unreal.AddNewSubobjectParams(
        parent_handle=root, new_class=unreal.HierarchicalInstancedStaticMeshComponent, blueprint_context=None))
    if not fail.is_empty():
        warn("add_new_subobject: %s" % fail)
        return None
    lib = unreal.SubobjectDataBlueprintFunctionLibrary
    return lib.get_object(lib.get_data(handle))


def add_instances(comp, transforms):
    try:
        comp.add_instances(transforms, False)
        return
    except Exception as e:   # older/newer signature: fall back to one call per instance
        warn("add_instances(list) failed (%s); falling back to add_instance" % e)
    for t in transforms:
        comp.add_instance(t)


def configure(comp, cull):
    """Cheap foliage: no collision, no shadows, no distance-field/Lumen mesh contribution, distance fade."""
    for fn, args in (("set_collision_enabled", (unreal.CollisionEnabled.NO_COLLISION,)),
                     ("set_cast_shadow", (False,)),
                     ("set_cull_distances", cull)):
        try:
            getattr(comp, fn)(*args)
        except Exception as e:
            warn("component.%s failed: %s" % (fn, e))
    for prop, val in (("cast_shadow", False), ("affect_distance_field_lighting", False),
                      ("affect_dynamic_indirect_lighting", False), ("can_ever_affect_navigation", False),
                      ("instance_start_cull_distance", int(cull[0])), ("instance_end_cull_distance", int(cull[1]))):
        try:
            comp.set_editor_property(prop, val)
        except Exception:
            pass       # not exposed in this build; the setters above cover the important ones


def spawn_field(eas, p, doc, assets, density_stride, cull, dry):
    placed = 0
    ue = p["ue"]
    loc = unreal.Vector(*ue["location_cm"])
    qx, qy, qz, qw = ue["rotation_quat_xyzw"]
    actor = None
    if not dry:
        actor = eas.spawn_actor_from_class(unreal.StaticMeshActor, loc, unreal.Quat(qx, qy, qz, qw).rotator())
        if actor is None:
            err("spawn failed for %s/%s" % (p["scene"], p["index"]))
            return 0
        actor.set_actor_scale3d(unreal.Vector(*ue["scale"]))
        actor.set_actor_label("BR_Grass_%s_%03d" % (p["scene"], p["index"]))
        actor.set_folder_path("Imported/Grass")
        actor.set_editor_property("tags", [unreal.Name(TAG), unreal.Name("br_model:" + p["model"])])
    for tid, t in doc["tufts"].items():
        if tid not in assets:
            continue
        rows = t["rows"]
        n = len(rows) // 5
        take = range(0, n, density_stride)
        placed += len(take)
        if dry:
            continue
        transforms = [unreal.Transform(unreal.Vector(rows[5 * i], rows[5 * i + 1], rows[5 * i + 2]),
                                       unreal.Rotator(0.0, 0.0, float(rows[5 * i + 3])),
                                       unreal.Vector(*([rows[5 * i + 4] / 100.0] * 3))) for i in take]
        comp = new_hism(actor)
        if comp is None:
            err("adding a HISM component to %s failed" % actor.get_actor_label())
            return placed
        sm = assets[tid][0]
        comp.set_static_mesh(sm)
        comp.set_material(0, tinted_mi(assets[tid], t["tint"]))
        configure(comp, cull)
        add_instances(comp, transforms)
    return placed


def main():
    global IMPORTED
    IMPORTED = os.environ.get("BR_IMPORTED")
    if not IMPORTED or not os.path.isfile(os.path.join(IMPORTED, "grass", "index.json")):
        raise RuntimeError("BR_IMPORTED must point at the importer output with grass/index.json "
                           "(run: python3 -m tools.importers --install <data_0> world)")
    all_models = os.environ.get("BR_GRASS_ALL") == "1"
    dry = os.environ.get("BR_GRASS_DRY") == "1"
    density = min(1.0, max(0.01, env_float("BR_GRASS_DENSITY", 1.0)))
    stride = max(1, int(round(1.0 / density)))
    cull = [float(x) for x in os.environ.get("BR_GRASS_CULL", "2200,3800").split(",")]
    radius = env_float("BR_GRASS_DL_M", 25.0)
    proj_saved = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
    report_path = os.environ.get("BR_GRASS_REPORT") or os.path.join(proj_saved, "BullyImport", "grass_report.json")
    stage_dir = os.path.join(proj_saved, "BullyImport", "stage_grass")

    index = load_json(os.path.join(IMPORTED, "grass", "index.json"))
    manifest = ic.load_manifest(IMPORTED)
    fields = select_fields(manifest, index, all_models, radius)
    REPORT.update({"all_models": all_models, "density_stride": stride, "cull_cm": cull, "dry": dry,
                   "fields_selected": len(fields)})
    log("%d fields (all=%s dry=%s stride=%d cull=%s)" % (len(fields), all_models, dry, stride, cull))

    assets = tuft_assets(IMPORTED, index, stage_dir)
    if not assets:
        raise RuntimeError("no tuft meshes imported")

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not les.load_level(ic.MAP_PATH):
        raise RuntimeError("load_level failed: " + ic.MAP_PATH)
    old = [a for a in eas.get_all_level_actors() if TAG in [str(t) for t in a.get_editor_property("tags")]]
    if old and not dry:
        eas.destroy_actors(old)
    log("destroyed %d old grass actors" % len(old))

    total, per_model = 0, {}
    for p, entry in fields:
        doc = load_json(os.path.join(IMPORTED, entry["file"]))
        n = spawn_field(eas, p, doc, assets, stride, cull, dry)
        total += n
        per_model[p["model"]] = per_model.get(p["model"], 0) + n
        log("%s %s#%s: %d instances" % (p["scene"], p["model"], p["index"], n))
    REPORT.update({"instances": total, "per_model": per_model})
    if not dry and not les.save_current_level():
        raise RuntimeError("save_current_level failed")
    log("done: %d instances in %d fields" % (total, len(fields)))


IMPORTED = None
try:
    main()
except Exception as e:
    err("fatal: %s\n%s" % (e, traceback.format_exc()))
REPORT["wall_seconds"] = round(time.time() - T0, 1)
try:
    rp = os.environ.get("BR_GRASS_REPORT") or os.path.join(
        unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), "BullyImport", "grass_report.json")
    os.makedirs(os.path.dirname(rp), exist_ok=True)
    with open(rp, "w") as f:
        json.dump(REPORT, f, indent=1)
except Exception as e:
    unreal.log_error("BR_GRASS report not written: %s" % e)
unreal.log("BR_GRASS_DONE %s" % ("OK" if not REPORT["errors"] else "WITH_ERRORS"))
