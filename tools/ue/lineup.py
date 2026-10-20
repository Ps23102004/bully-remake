# OWNER: builder-B (ue)
"""Character render check: a temporary line-up of every imported character in the SAVED Bullworth map.

Run through tools/ue/run_lineup.sh (headless, -nullrhi). Spawns one SkeletalMeshActor per
/Game/Imported/Characters/<name>/SK_<name> on the road east of the front gates (game x 333..336, y -64..-82,
outside every gameplay area), each looping one of its own clips (walk for people, DOGWALK / RAT_SCURRY for
animals), tagged `br_lineup`; render it with run_shots.sh + tools/ue/shots_lineup.json, then remove it with
`run_lineup.sh --remove` (re-running run_import.sh also clears it). Environment: BR_LINEUP_REMOVE=1 removes only.
"""
import os

import unreal

TAG = "br_lineup"
MAP = "/Game/Imported/Maps/Bullworth"
ROOT = "/Game/Imported/Characters"
# (name, clip, game x, game y); people face +X (the camera), MeshYawOffset 90 as in ABullyCharacter
PEOPLE = ["Jimmy", "GN_Boy01", "GN_Boy02", "GN_Asiangirl", "GN_Greekboy", "NDH3_Bucky", "PFH1_Seth",
          "GN_Bully01", "GN_Bully02", "GN_Bully03", "GN_Bully04", "GN_Bully05"]
ANIMALS = [("DOG_Pitbull", "DOGWALK"), ("DOG_Doberman", "DOGIDLE"), ("DOG_Pitbull2", "DOGBARK"),
           ("DOG_Pitbull3", "DOGRUN"), ("RAT_PED", "RAT_SCURRY")]
GROUND_Z_CM = 436.0   # road surface at game x 333..336 (measured from the tschool/zone_school ground triangles)


def log(m):
    unreal.log("BR_LINEUP " + m)


def spawn(eas, name, clip, gx, gy, z_extra=0.0):
    sk = unreal.EditorAssetLibrary.load_asset("%s/%s/SK_%s" % (ROOT, name, name))
    anim = unreal.EditorAssetLibrary.load_asset("%s/%s/Anims/%s" % (ROOT, name, clip))
    if sk is None or anim is None:
        unreal.log_error("BR_LINEUP missing %s (mesh %s, clip %s)" % (name, sk is not None, anim is not None))
        return False
    a = eas.spawn_actor_from_class(unreal.SkeletalMeshActor, unreal.Vector(gx * 100.0, -gy * 100.0, GROUND_Z_CM + z_extra),
                                   unreal.Rotator(0.0, 0.0, 90.0))
    a.set_actor_label("BR_Lineup_" + name)
    a.set_folder_path("Dev/Lineup")
    a.set_editor_property("tags", [unreal.Name(TAG)])
    c = a.skeletal_mesh_component
    c.set_skeletal_mesh_asset(sk)
    c.set_editor_property("animation_mode", unreal.AnimationMode.ANIMATION_SINGLE_NODE)
    c.set_editor_property("animation_data", unreal.SingleAnimationPlayData(anim_to_play=anim, saved_looping=True,
                                                                           saved_playing=True, saved_position=0.3))
    log("spawned %s playing %s at game (%.1f, %.1f)" % (name, clip, gx, gy))
    return True


def main():
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not les.load_level(MAP):
        raise RuntimeError("load_level failed")
    old = [a for a in eas.get_all_level_actors() if TAG in [str(t) for t in a.get_editor_property("tags")]]
    if old:
        eas.destroy_actors(old)
    log("removed %d old line-up actors" % len(old))
    n = 0
    if os.environ.get("BR_LINEUP_REMOVE") != "1":
        for i, name in enumerate(PEOPLE):          # one row, 1.4 m apart, centred on the gate axis (y -73)
            n += spawn(eas, name, "PLAYERWALK", 333.0, -73.0 + (i - (len(PEOPLE) - 1) / 2.0) * 1.4)
        for i, (name, clip) in enumerate(ANIMALS):  # animals 2.5 m in front of the people
            # the rat skeleton is centred on its root (half of the bind pose is below z=0): lift it
            n += spawn(eas, name, clip, 335.5, -73.0 + (i - (len(ANIMALS) - 1) / 2.0) * 1.6, 8.0 if name == "RAT_PED" else 0.0)
    if not les.save_current_level():
        raise RuntimeError("save_current_level failed")
    log("done: %d actors" % n)
    unreal.log("BR_LINEUP_DONE OK")


try:
    main()
except Exception as e:  # noqa: BLE001
    unreal.log_error("BR_LINEUP ERROR %s" % e)
