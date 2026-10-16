"""Headless stage driver for the Day One missions (1_01, 1_02, day_one). Runs INSIDE the game via
`-ExecCmds="py <this file>"` (see run_headless.sh); -ExecutePythonScript does not work outside the editor.

It plays the missions through their real stage machine with console commands only, no C++ and no window:
  Mission.Start / Mission.Goto / Mission.Fire / Mission.Skip / Mission.Status, SNPC.Status, Combat.SpawnEnemy,
  Combat.Invulnerable, Combat.Status. Stage ids are read from the "BR_MISSION stage N/M: <objective> -> begin" log lines
  (mapped through the mission JSON), so it reacts to what the framework really did instead of a clock.

Modes (env BR_VERIFY_MODE):
  chain    Mission.Start 1_01 -> drive to its end -> the driver issues Mission.Start 1_02 (there is no start_mission
           action in the framework, so this chaining step is DRIVER-ASSISTED and logged as such) -> drive to the end card.
  day_one  the game was launched with -BRMission=day_one (real auto-start); only drive.

Evidence: <out>/driver_result.json, BR_VERIFY lines in the engine log. Exits the engine with `quit` when finished.
"""
import json
import os
import re

import unreal

MODE = os.environ.get("BR_VERIFY_MODE", "chain")
OUT = os.environ.get("BR_VERIFY_OUT", "/tmp")
LOG = os.environ.get("BR_VERIFY_LOG", "")
MISSIONS = os.environ.get("BR_VERIFY_MISSIONS", "")
WORLD_PATH = "/Game/Imported/Maps/Bullworth.Bullworth"
MAX_SIM_SECONDS = 900.0
STAGE_STUCK_SECONDS = 70.0

# stage id -> [(delay seconds after the stage began, console command)]
STEPS = {
    # ---- 1_01 ----
    "walk_in": [(1.5, "Mission.Goto")],
    "get_past_bullies": [(2.0, "SNPC.Status")],   # real NPC behaviour must release the player; see FALLBACK
    "cross_courtyard": [(1.5, "Mission.Goto")],
    "use_door": [(1.5, "Mission.Goto")],
    "reach_lobby": [(3.0, "Mission.Goto")],
    "go_upstairs": [(1.5, "Mission.Goto")],
    # ---- 1_02 ----
    "briefing": [],                                # timer stage
    "leave_office": [(1.5, "Mission.Goto")],
    "exit_building": [(1.5, "Mission.Goto")],
    "cross_campus": [(1.5, "Mission.Goto")],
    "fight_intro": [(1.2, "Combat.Invulnerable 1"), (1.5, "Combat.SpawnEnemy 5"), (3.0, "Combat.Status"), (3.5, "SNPC.Status")],
    "tut1_block_a": [(1.5, "Mission.Fire block_success")],
    "tut1_block_b": [(1.5, "Mission.Fire block_success")],
    "tut2_combo": [(1.5, "Mission.Fire combo_done")],
    "tut3_grapple": [(1.5, "Mission.Fire grapple_start")],
    "tut4_strikes": [(1.0, "Mission.Fire grapple_strike"), (1.2, "Mission.Fire grapple_strike"), (1.4, "Mission.Fire grapple_strike"),
                     (1.6, "Mission.Fire grapple_strikes_done")],
    "tut5_grapple_again": [(1.5, "Mission.Fire grapple_start")],
    "tut6_push": [(1.5, "Mission.Fire grapple_done")],
    "tut7_ground_kick": [(1.5, "Mission.Fire ground_kick")],
    "free_fight": [(1.5, "Mission.Fire enemy_beaten")],
    "humiliate": [(1.5, "Mission.Fire humiliation_done")],
    "fight_end": [(1.5, "Mission.Fire enemy_knocked_out")],   # the crowd's disperse beat listens for the same event
    "crowd_leaves": [(2.0, "SNPC.Status")],
    "staff_coming": [],                            # timer stage (day_one mode skips it with Mission.Skip next)
    "to_dorm": [(1.5, "Mission.Goto")],
    "end_card": [],                                # timer stage
}
# stage id -> (seconds, command) used once if the stage has not advanced by then (logged as FORCED)
FALLBACK = {"get_past_bullies": (30.0, "Mission.Fire gauntlet_complete")}
# fail/retry exercise: on the first visit to this stage fire the fail event instead of the normal steps
FAIL_TEST_STAGE = "tut2_combo"
FAIL_TEST_CMD = "Mission.Fire player_knocked_out"
SKIP_STAGE_IN_DAY_ONE = "staff_coming"

RE_START = re.compile(r"BR_MISSION start id=(\S+) ")
RE_BEGIN = re.compile(r"BR_MISSION stage (\d+)/(\d+): (.*?) -> begin( \(retry\))?")
RE_COMPLETE_STAGE = re.compile(r"BR_MISSION stage (\d+): (.*?) -> complete")
RE_DONE = re.compile(r"BR_MISSION complete id=(\S+) stages=(\d+) attempts=(\d+)")
RE_FAIL = re.compile(r"BR_MISSION stage (\d+): (.*?) -> FAIL")
RE_EVENT = re.compile(r"BR_MISSION event '([^']+)'")

st = {
    "n": 0, "t": 0.0, "world": None, "offset": 0, "buf": "", "mission": None, "stage_ids": {},
    "sched": [], "cur_stage": None, "cur_t0": 0.0, "fallback_done": set(), "fail_tested": False, "skipped": False,
    "seq": [], "events": [], "completed": [], "fails": [], "forced": [], "log": [], "chained": False, "finish_at": None,
    "started": False, "result_written": False, "mission_start_seen": [],
}


def say(msg):
    unreal.log("BR_VERIFY t=%.1f %s" % (st["t"], msg))
    st["log"].append("t=%.1f %s" % (st["t"], msg))


def cmd(c):
    say("exec: %s" % c)
    try:
        unreal.SystemLibrary.execute_console_command(st["world"], c)
    except Exception as e:  # never crash the engine from the driver
        say("exec failed (%s): %s" % (c, e))


def load_stage_ids(mission_id):
    path = os.path.join(MISSIONS, mission_id + ".json")
    with open(path) as f:
        d = json.load(f)
    st["stage_ids"][mission_id] = [s["id"] for s in d["stages"]]
    st["stage_objectives"] = st.get("stage_objectives", {})
    st["stage_objectives"][mission_id] = [s.get("objective", "") for s in d["stages"]]


def on_line(line):
    m = RE_START.search(line)
    if m:
        st["mission"] = m.group(1)
        st["mission_start_seen"].append(m.group(1))
        if m.group(1) not in st["stage_ids"]:
            load_stage_ids(m.group(1))
        say("mission started: %s (%d stages)" % (m.group(1), len(st["stage_ids"][m.group(1)])))
        return
    m = RE_BEGIN.search(line)
    if m and st["mission"]:
        idx = int(m.group(1)) - 1
        sid = st["stage_ids"][st["mission"]][idx]
        retry = bool(m.group(4))
        st["cur_stage"], st["cur_t0"] = sid, st["t"]
        st["seq"].append({"mission": st["mission"], "stage": sid, "index": idx + 1, "objective": m.group(3), "retry": retry, "t": round(st["t"], 2)})
        say("STAGE BEGIN %s #%d/%s %s%s :: %s" % (st["mission"], idx + 1, m.group(2), sid, " (retry)" if retry else "", m.group(3)))
        schedule_stage(sid, retry)
        return
    m = RE_COMPLETE_STAGE.search(line)
    if m:
        say("stage %s complete" % m.group(1))
        return
    m = RE_FAIL.search(line)
    if m:
        st["fails"].append({"stage": st["cur_stage"], "t": round(st["t"], 2)})
        say("STAGE FAIL observed at %s (retry expected)" % st["cur_stage"])
        return
    m = RE_DONE.search(line)
    if m:
        st["completed"].append({"mission": m.group(1), "stages": int(m.group(2)), "attempts": int(m.group(3)), "t": round(st["t"], 2)})
        say("MISSION COMPLETE %s stages=%s attempts=%s" % (m.group(1), m.group(2), m.group(3)))
        on_mission_done(m.group(1))
        return
    m = RE_EVENT.search(line)
    if m:
        st["events"].append({"event": m.group(1), "t": round(st["t"], 2)})
    if "BR_COMBAT spawned" in line or "BR_SNPC group" in line or "BR_SNPC EMIT" in line:
        say("evidence: " + line.split("]", 2)[-1].strip()[:200])


def schedule_stage(sid, retry):
    st["sched"] = []   # one stage at a time
    steps = list(STEPS.get(sid, []))
    if sid == FAIL_TEST_STAGE and not st["fail_tested"] and not retry:
        st["fail_tested"] = True
        steps = [(1.5, FAIL_TEST_CMD)]
        say("fail/retry exercise on %s" % sid)
    if MODE == "day_one" and sid == SKIP_STAGE_IN_DAY_ONE and not st["skipped"]:
        st["skipped"] = True
        steps = [(1.0, "Mission.Skip next")]
    for delay, c in steps:
        st["sched"].append((st["t"] + delay, c))
    st["sched"].append((st["t"] + 0.3, "Mission.Status"))


def on_mission_done(mid):
    if MODE == "chain" and mid == "1_01" and not st["chained"]:
        st["chained"] = True
        st["sched"] = [(st["t"] + 1.0, "@chain")]
    else:
        st["finish_at"] = st["t"] + 3.0


def pump_log():
    if not LOG or not os.path.exists(LOG):
        return
    try:
        with open(LOG, "r", errors="replace") as f:
            f.seek(st["offset"])
            data = f.read()
            st["offset"] = f.tell()
    except OSError:
        return
    if not data:
        return
    data = st["buf"] + data
    lines = data.split("\n")
    st["buf"] = lines.pop()
    for ln in lines:
        if "BR_VERIFY" in ln:
            continue   # our own output
        if "BR_MISSION" in ln or "BR_COMBAT spawned" in ln or "BR_SNPC" in ln:
            on_line(ln)


def expected_sequence():
    ids = []
    if MODE == "chain":
        ids = [("1_01", s) for s in st["stage_ids"].get("1_01", [])] + [("1_02", s) for s in st["stage_ids"].get("1_02", [])]
    else:
        ids = [("day_one", s) for s in st["stage_ids"].get("day_one", [])]
    return ids


def write_result_and_quit(reason):
    if st["result_written"]:
        return
    st["result_written"] = True
    seen = []
    for e in st["seq"]:
        key = (e["mission"], e["stage"])
        if not e["retry"] and key not in seen:
            seen.append(key)
    expected = expected_sequence()
    want_missions = ["1_01", "1_02"] if MODE == "chain" else ["day_one"]
    done = [c["mission"] for c in st["completed"]]
    ok = (seen == expected) and (done == want_missions) and st["fail_tested"] and any(f["stage"] == FAIL_TEST_STAGE for f in st["fails"])
    result = {
        "mode": MODE, "reason": reason, "pass": bool(ok), "sim_seconds": round(st["t"], 1),
        "missions_completed": st["completed"], "stages_in_order": [{"mission": a, "stage": b} for a, b in seen],
        "expected_stage_count": len(expected), "seen_stage_count": len(seen), "fails_observed": st["fails"],
        "forced_commands": st["forced"], "driver_assisted_chain": st["chained"],
        "mission_starts": st["mission_start_seen"], "stage_log": st["seq"], "events": st["events"],
    }
    try:
        with open(os.path.join(OUT, "driver_result.json"), "w") as f:
            json.dump(result, f, indent=1)
    except OSError as e:
        say("could not write result: %s" % e)
    say("RESULT %s: %s (stages seen %d/%d, missions completed %s, forced=%d)" % ("PASS" if ok else "FAIL", reason, len(seen), len(expected), done, len(st["forced"])))
    cmd("quit")


def tick(dt):
    st["n"] += 1
    st["t"] += dt
    try:
        if st["world"] is None:
            st["world"] = unreal.find_object(None, WORLD_PATH)
            if st["world"] is None:
                return
        if not st["started"] and st["t"] > 4.0:
            st["started"] = True
            if MODE == "chain":
                load_stage_ids("1_01")
                load_stage_ids("1_02")
                cmd("Mission.Start 1_01")
            else:
                load_stage_ids("day_one")
                say("waiting for the -BRMission=day_one auto-start")
        if st["n"] % 6 == 0:
            pump_log()
        # due commands
        due = [s for s in st["sched"] if s[0] <= st["t"]]
        if due:
            st["sched"] = [s for s in st["sched"] if s[0] > st["t"]]
            for _, c in due:
                if c == "@chain":
                    say("CHAIN (driver-assisted): 1_01 finished, starting 1_02")
                    cmd("Mission.Start 1_02")
                else:
                    cmd(c)
        # fallback / stuck handling
        sid = st["cur_stage"]
        if sid and st["finish_at"] is None and not st["result_written"]:
            age = st["t"] - st["cur_t0"]
            fb = FALLBACK.get(sid)
            if fb and age > fb[0] and (sid, st["cur_t0"]) not in st["fallback_done"]:
                st["fallback_done"].add((sid, st["cur_t0"]))
                st["forced"].append({"stage": sid, "cmd": fb[1], "t": round(st["t"], 1)})
                say("FORCED after %.0fs: %s" % (age, fb[1]))
                cmd(fb[1])
            if age > STAGE_STUCK_SECONDS and sid not in ("briefing", "end_card", "crowd_leaves", "staff_coming"):
                write_result_and_quit("stage %s stuck for %.0fs" % (sid, age))
        if st["finish_at"] is not None and st["t"] >= st["finish_at"]:
            write_result_and_quit("done")
        if st["t"] > MAX_SIM_SECONDS:
            write_result_and_quit("sim time limit")
    except Exception as e:
        unreal.log_warning("BR_VERIFY tick exception: %s" % e)


unreal.log("BR_VERIFY driver loaded mode=%s out=%s" % (MODE, OUT))
_handle = unreal.register_slate_post_tick_callback(tick)
