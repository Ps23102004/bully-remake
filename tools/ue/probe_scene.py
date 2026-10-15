# OWNER: builder-B5 (shot)
"""Diagnostic (commandlet, no GPU): dump actors near a point and/or the materials used by scenes.

  BR_PROBE_OUT=file.json
  BR_PROBE_NEAR="x,y,z,radius_cm"      -> actors whose bounds origin is within radius (label, model, bounds, materials)
  BR_PROBE_MATS="prefix1,prefix2"      -> per material instance used by static-mesh actors of those scenes
                                          (parent, blend, two_sided, base texture, #actors, example models)
Run through run_probe.sh --mode scene.
"""
import json
import os
import traceback

import unreal

MAP_PATH = "/Game/Imported/Maps/Bullworth"


def mat_info(mi):
    d = {"path": mi.get_path_name()}
    try:
        par = mi.get_editor_property("parent")
        d["parent"] = par.get_name() if par else None
    except Exception:
        d["parent"] = None
    try:
        t = unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(mi, "BaseColor")
        d["base"] = t.get_name() if t else None
    except Exception:
        d["base"] = None
    return d


def main():
    out = os.environ["BR_PROBE_OUT"]
    near = os.environ.get("BR_PROBE_NEAR")
    mats = os.environ.get("BR_PROBE_MATS")
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    ok = False
    try:
        les.load_level(MAP_PATH)
        res = {}
        actors = [a for a in eas.get_all_level_actors() if isinstance(a, unreal.StaticMeshActor)]
        if near:
            x, y, z, r = [float(v) for v in near.split(",")]
            lst = []
            for a in actors:
                o, e = a.get_actor_bounds(False)
                if (o.x - x) ** 2 + (o.y - y) ** 2 + (o.z - z) ** 2 > r * r:
                    continue
                tags = [str(t) for t in a.get_editor_property("tags")]
                ms = []
                for m in a.static_mesh_component.get_materials():
                    ms.append(m.get_name() if m else None)
                lst.append({"label": a.get_actor_label(), "model": next((t[9:] for t in tags if t.startswith("br_model:")), ""),
                            "origin": [round(o.x), round(o.y), round(o.z)], "extent": [round(e.x), round(e.y), round(e.z)],
                            "mats": ms})
            res["near"] = lst
        if mats:
            prefixes = tuple(mats.split(","))
            agg = {}
            for a in actors:
                tags = [str(t) for t in a.get_editor_property("tags")]
                scene = next((t[9:] for t in tags if t.startswith("br_scene:")), "")
                if not scene.startswith(prefixes):
                    continue
                model = next((t[9:] for t in tags if t.startswith("br_model:")), "")
                for m in a.static_mesh_component.get_materials():
                    if m is None:
                        continue
                    k = m.get_path_name()
                    if k not in agg:
                        agg[k] = {"info": mat_info(m) if isinstance(m, unreal.MaterialInstance) else {"path": k}, "n": 0, "models": set()}
                    agg[k]["n"] += 1
                    if len(agg[k]["models"]) < 5:
                        agg[k]["models"].add(model)
            res["mats"] = [{"mi": k, **{"n": v["n"], "models": sorted(v["models"])}, **v["info"]} for k, v in sorted(agg.items(), key=lambda kv: -kv[1]["n"])]
        with open(out, "w") as fh:
            json.dump(res, fh, indent=1)
        ok = True
    except Exception as e:
        unreal.log_error("BR_PROBE ERROR %s\n%s" % (e, traceback.format_exc()))
    unreal.log("BR_PROBE_DONE %s" % ("OK" if ok else "FAILED"))


main()
