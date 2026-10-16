# OWNER: slice-npc (scripted NPCs)
"""Prints what the scripted NPCs need imported: the peds named in Config/NPCScripts/*.json and every gesture clip
the scripts' alias tables mention. Used by run_import_npc.sh; run it alone to see the plan.

  python3 tools/ue/npc_plan.py            -> "PEDS GN_Boy01 GN_Boy02 ..." / "CLIPS PLAYER_TALKING0 P_TAUNT_01 ..."
"""
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(HERE, "..", "..", "unreal", "BullyRemake", "Config", "NPCScripts")
LOCOMOTION = ["IDLE", "PLAYERWALK", "PLAYERRUN", "RUNFAST", "IDLEJUMP", "JOGJUMP_L", "JOGJUMP_R", "FALL", "FALL_LAND", "PLAY_LAND_HURT"]


def plan(directory=SCRIPTS):
    peds, clips = [], []
    for path in sorted(glob.glob(os.path.join(directory, "*.json"))):
        with open(path) as f:
            d = json.load(f)
        for alias, names in d.get("gestures", {}).items():
            if alias.startswith("_"):
                continue
            for n in (names if isinstance(names, list) else [names]):
                if n not in clips and n not in LOCOMOTION:
                    clips.append(n)
        for g in d.get("groups", []):
            for npc in g.get("npcs", []):
                if npc.get("ped") and npc["ped"] not in peds:
                    peds.append(npc["ped"])
        for g in d.get("groups", []):   # literal "clip" names used in steps
            for b in g.get("beats", []):
                for s in b.get("steps", []):
                    for key in ("clip", "engage_clip", "hold_clip"):
                        v = s.get(key)
                        for n in (v if isinstance(v, list) else [v] if v else []):
                            if n not in clips and n not in LOCOMOTION:
                                clips.append(n)
    return peds, clips


if __name__ == "__main__":
    p, c = plan(sys.argv[1] if len(sys.argv) > 1 else SCRIPTS)
    print("PEDS " + " ".join(p))
    print("CLIPS " + " ".join(LOCOMOTION + c))
