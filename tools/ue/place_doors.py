"""Re-place the ABullyDoorLink actors (door_links.json) into the SAVED map, without re-importing anything.
Needed after a level rebuild that ran while the editor module was not built (import_content then logs
'unreal.BullyDoorLink missing ... door links skipped' and saves a map with no doors). Run by run_place_doors.sh.
Idempotent: existing door-link actors are destroyed first. Milestone 6 (proof capture)."""
import json
import os
import traceback

import unreal

HERE = os.path.dirname(os.path.abspath(__file__))
MAP_PATH = "/Game/Imported/Maps/Bullworth"


def main():
    ok = False
    try:
        les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        cls = getattr(unreal, "BullyDoorLink", None)
        if cls is None:
            raise RuntimeError("unreal.BullyDoorLink missing: build the editor module first")
        if not les.load_level(MAP_PATH):
            raise RuntimeError("load_level failed: " + MAP_PATH)
        old = [a for a in eas.get_all_level_actors() if isinstance(a, cls)]
        if old:
            eas.destroy_actors(old)
        with open(os.path.join(HERE, "door_links.json"), encoding="utf-8") as f:
            data = json.load(f)
        n = 0
        for d in data["links"]:
            c = d["trigger"]["center_cm"]
            h = d["trigger"]["half_extent_cm"]
            a = eas.spawn_actor_from_class(cls, unreal.Vector(*c), unreal.Rotator(0, 0, 0))
            a.set_actor_label("BR_Door_" + d["id"])
            a.set_folder_path("Gameplay/Doors")
            a.set_editor_property("link_id", d["id"])
            a.set_editor_property("partner_id", d["partner"])
            a.set_editor_property("label", d.get("label", d["id"]))
            a.set_editor_property("exit_location", unreal.Vector(*d["exit"]["location_cm"]))
            a.set_editor_property("exit_yaw", float(d["exit"]["yaw"]))
            a.get_editor_property("trigger").set_editor_property("box_extent", unreal.Vector(*h))
            n += 1
            unreal.log("BR_DOORS placed %s -> %s at %s" % (d["id"], d["partner"], c))
        if not les.save_current_level():
            raise RuntimeError("save_current_level failed")
        unreal.log("BR_DOORS removed %d old, placed %d" % (len(old), n))
        ok = n == len(data["links"])
    except Exception as e:
        unreal.log_error("BR_DOORS ERROR %s\n%s" % (e, traceback.format_exc()))
    unreal.log("BR_DOORS_DONE %s" % ("OK" if ok else "FAILED"))


main()
