# OWNER: builder-B4 (ambient)
"""Headless UE import for milestone 4 "signs of life": student skeletal meshes + clips, and decoded game audio.

Runs inside UnrealEditor-Cmd (pythonscript commandlet); launch through tools/ue/run_import_ambient.sh.
Data-driven by unreal/BullyRemake/Config/AmbientLoops.json:

  students[]   each ped model (imported/models/<model>.gltf from `python3 -m tools.importers ped <model> --anims ...`)
               is imported by RUNNING tools/ue/import_character.py once per student (BR_CHAR_MODEL=<model>,
               BR_CHAR_NAME=<model>): /Game/Imported/Characters/<model>/{SK_,SKEL_,PHYS_}<model> + Anims/.
               Each student gets its own skeleton and clips so its own bone offsets drive its proportions.
  footsteps{}, ambient.sounds[]
               decoded WAVs (imported/audio/<bank>/<NNN>_<name>.wav, from `python3 -m tools.importers audio extract`)
               -> /Game/Imported/Audio/<bank>/<name> as SoundWave; ambient loops get looping=True.
Environment: BR_IMPORTED (importer output dir), BR_REPORT (JSON report), BR_AMBIENT_STEPS (students,audio; default both).
"""
import json
import os
import re
import runpy
import sys
import time
import traceback

import unreal

HERE = os.path.dirname(os.path.abspath(__file__))
EAL = unreal.EditorAssetLibrary
AT = unreal.AssetToolsHelpers.get_asset_tools()
T0 = time.time()
REPORT = {"format": "bully-remake.ue-ambient-report/1", "errors": [], "warnings": [], "students": {}, "audio": {}}


def log(msg):
    unreal.log("BR_AMBIENT_IMPORT [%6.1fs] %s" % (time.time() - T0, msg))


def err(msg):
    REPORT["errors"].append(msg)
    unreal.log_error("BR_AMBIENT_IMPORT ERROR " + msg)


def warn(msg):
    REPORT["warnings"].append(msg)
    unreal.log_warning("BR_AMBIENT_IMPORT WARN " + msg)


def asset_name(s):
    return re.sub(r"[^A-Za-z0-9_]+", "_", s).strip("_") or "unnamed"


def import_students(cfg, imported, report_dir):
    clips = ("IDLE,PLAYERWALK,PLAYERRUN,RUNFAST,IDLEJUMP,JOGJUMP_L,JOGJUMP_R,FALL,FALL_LAND,PLAY_LAND_HURT")
    for s in cfg["students"]:
        model = s["model"]
        env = {"BR_CHAR_MODEL": model, "BR_CHAR_NAME": model, "BR_CHAR_ANIMS": clips,
               "BR_REPORT": os.path.join(report_dir, "ambient_char_%s.json" % model), "BR_IMPORTED": imported}
        os.environ.update(env)
        log("student %s: running import_character.py" % model)
        try:
            runpy.run_path(os.path.join(HERE, "import_character.py"), run_name="br_char_import")
        except SystemExit:
            pass
        except Exception as e:  # noqa: BLE001
            err("student %s: %s\n%s" % (model, e, traceback.format_exc()))
        try:
            with open(env["BR_REPORT"]) as f:
                r = json.load(f)
            REPORT["students"][model] = {"errors": r.get("errors"), "mesh": {k: r["mesh"].get(k) for k in ("bones", "skeleton", "bounds_cm")} if r.get("mesh") else None,
                                         "animations": r.get("animations"), "max_clip_mismatch_deg": max([c.get("max_mismatch_deg", 0) for c in r.get("clip_checks", {}).values()] or [0])}
            if r.get("errors"):
                err("student %s reported errors: %s" % (model, r["errors"][:2]))
        except Exception as e:  # noqa: BLE001
            err("student %s: no report (%s)" % (model, e))


def load_footstep_table(imported):
    """imported/audio/footstep_cues.json (python3 -m tools.importers audio footsteps): surface/role/gait -> cue lists."""
    p = os.path.join(imported, "audio", "footstep_cues.json")
    if not os.path.isfile(p):
        warn("no %s (run python3 -m tools.importers audio footsteps): per-surface footsteps skipped" % p)
        return None
    with open(p) as f:
        return json.load(f)


def wanted_sounds(cfg, table=None):
    out = []
    for lst in cfg["footsteps"].values():
        out += [(x, False) for x in lst]
    for x in cfg["ambient"]["sounds"]:
        out.append((x, True))
    if table:
        for bank, waves in table["waves"].items():
            out += [({"bank": bank, "name": w}, False) for w in waves]
    seen, res = set(), []
    for x, loop in out:
        k = (x["bank"], x["name"])
        if k not in seen:
            seen.add(k)
            res.append((x, loop))
    return res


def import_audio(cfg, imported):
    man_path = os.path.join(imported, "audio", "manifest.json")
    with open(man_path) as f:
        man = json.load(f)
    by_key = {(r["bank"], r["name"]): r for r in man["entries"]}
    ok = 0
    table = load_footstep_table(imported)
    for x, loop in wanted_sounds(cfg, table):
        row = by_key.get((x["bank"], x["name"]))
        if row is None:
            err("audio %s/%s not in %s (run python3 -m tools.importers audio extract %s)" % (x["bank"], x["name"], man_path, x["bank"]))
            continue
        dest_dir = "/Game/Imported/Audio/%s" % x["bank"]
        name = asset_name(x["name"])
        path = "%s/%s" % (dest_dir, name)
        src = os.path.join(imported, "audio", row["wav"])
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", src)
        task.set_editor_property("destination_path", dest_dir)
        task.set_editor_property("destination_name", name)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", False)
        AT.import_asset_tasks([task])
        sw = EAL.load_asset(path) if EAL.does_asset_exist(path) else None
        if sw is None or not isinstance(sw, unreal.SoundWave):
            err("audio import failed: %s -> %s" % (src, path))
            continue
        sw.set_editor_property("looping", bool(loop))
        EAL.save_loaded_asset(sw, only_if_is_dirty=False)
        dur = sw.get_editor_property("duration")
        REPORT["audio"][path] = {"duration": round(float(dur), 3), "looping": bool(loop), "samples": row["samples"], "rate": row["sample_rate"]}
        ok += 1
        log("audio %s duration %.3f s looping=%s" % (path, dur, loop))
    log("audio imported: %d" % ok)
    if table:
        write_footstep_table(table)


def write_footstep_table(table):
    """Content/Imported/Audio/footstep_cues.json: the resolved table plus each cue's SoundWave asset path (read at runtime
    by UBullyFootstepComponent next to Config/FootstepSurfaces.json). Git-ignored like all imported content."""
    n = 0
    for ctx in table["contexts"].values():
        for roles in ctx.values():
            for gaits in roles.values():
                for cues in gaits.values():
                    for c in cues:
                        c["asset"] = "/Game/Imported/Audio/%s/%s" % (c["bank"], asset_name(c["wave"]))
                        n += 1
    dest = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_content_dir()), "Imported", "Audio", "footstep_cues.json")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "w") as f:
        json.dump(table, f)
    REPORT["audio"]["footstep_cues"] = {"file": dest, "cues": n, "unresolved": len(table.get("unresolved", []))}
    log("footstep table: %d cue entries -> %s" % (n, dest))


def main():
    imported = os.environ["BR_IMPORTED"]
    steps = os.environ.get("BR_AMBIENT_STEPS", "students,audio").split(",")
    proj = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
    with open(os.path.join(proj, "Config", "AmbientLoops.json")) as f:
        cfg = json.load(f)
    report_path = os.environ.get("BR_REPORT") or os.path.join(proj, "Saved", "BullyImport", "ambient_report.json")
    report_dir = os.path.dirname(report_path)
    os.makedirs(report_dir, exist_ok=True)
    if "audio" in steps:
        import_audio(cfg, imported)
    if "students" in steps:
        import_students(cfg, imported, report_dir)
    REPORT["wall_seconds"] = round(time.time() - T0, 1)
    with open(report_path, "w") as f:
        json.dump(REPORT, f, indent=1)
    log("report -> %s (errors=%d)" % (report_path, len(REPORT["errors"])))
    log("BR_AMBIENT_IMPORT_DONE %s" % ("OK" if not REPORT["errors"] else "WITH_ERRORS"))


try:
    main()
except Exception as e:  # noqa: BLE001
    err("fatal: %s\n%s" % (e, traceback.format_exc()))
    log("BR_AMBIENT_IMPORT_DONE WITH_ERRORS")
