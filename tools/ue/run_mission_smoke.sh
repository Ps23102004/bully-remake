#!/bin/bash
# OWNER: mission-fw
# Headless single-mission smoke: `-game -nullrhi` (no GPU, no window, no audio device), fixed 60 Hz game time. The
# -BRMissionSmoke=<id> harness (UMissionSubsystem::TickSmoke) starts the mission, drives the real player pawn through
# every stage using only the stage's own conditions (reach / trigger / interact / event), exercises fail->retry when
# the mission defines a smoke_fail stage, and logs BR_MISSION lines + a PASS/FAIL result.
#
#   tools/ue/run_mission_smoke.sh <mission_id> [--out DIR] [--seconds-limit 600]
#
# Same engine-safety envelope as the other headless runners: GPU-slot wrapper (label bully-mission), window watchdog
# that SIGTERMs the engine if any window appears (an empty window_watch.txt is the no-window evidence), no retry,
# SIGTERM -> 60 s -> SIGKILL. Exit code is the engine's (the harness quits the engine itself on PASS/FAIL).
set -uo pipefail
source "$(dirname "$0")/_common.sh"

MISSION=""
OUT=""
MAP=/Game/Imported/Maps/Bullworth
LABEL=bully-mission
LIMIT=600
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --seconds-limit) LIMIT="$2"; shift 2 ;;
    -*) br_die "unknown argument $1" ;;
    *) [ -z "$MISSION" ] || br_die "only one mission id"; MISSION="$1"; shift ;;
  esac
done
[ -n "$MISSION" ] || br_die "usage: run_mission_smoke.sh <mission_id> [--out DIR] [--seconds-limit N]"
[ -f "$BR_REPO/unreal/BullyRemake/Config/Missions/$MISSION.json" ] || br_die "no such mission: Config/Missions/$MISSION.json"
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"
[ -n "$OUT" ] || OUT="$HOME/bully-remake-evidence/missions/smoke_$MISSION"

br_preflight
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
rm -f "$OUT/ue_run.log" "$OUT/sequence.log" "$OUT/window_watch.txt"
echo "mission smoke: $MISSION -> $OUT"
START=$(date +%s)
"$GPU_SLOT" capture --label "$LABEL" -- \
  "$UE_CMD" "$BR_UPROJECT" "$MAP" -game -nullrhi -unattended -nosplash -nocrashreports -NoSound -notraceserver -NoVSync \
  -stdout -FullStdOutLogOutput -forcelogflush -abslog="$OUT/ue_run.log" -ResX=640 -ResY=360 -windowed -BRNoAmbient \
  -BRMissionSmoke="$MISSION" ${BR_EXTRA_ARGS:-} \
  > "$OUT/stdout.log" 2>&1 &
WRAP=$!
br_window_watch "$WRAP" "$OUT/window_watch.txt" &
WATCH=$!
stop_engine() {
  local epid
  epid=$(pgrep -P "$WRAP" | head -1)
  [ -n "$epid" ] || epid=$WRAP
  kill -TERM "$epid" 2>/dev/null
  for _ in $(seq 60); do kill -0 "$epid" 2>/dev/null || break; sleep 1; done
  kill -0 "$epid" 2>/dev/null && kill -KILL "$epid" 2>/dev/null
}
trap 'stop_engine; exit 130' INT TERM
( sleep "$LIMIT"; echo "time limit $LIMIT s reached: stopping the engine" >&2; stop_engine ) &
TIMER=$!
wait "$WRAP"
RC=$?
kill "$TIMER" 2>/dev/null
wait "$WATCH" 2>/dev/null
br_reap_crash_reporters
echo "engine exit $RC after $(( $(date +%s) - START ))s"
sed -E 's/^\[[^]]*\]\[[^]]*\]//' "$OUT/ue_run.log" | grep -E 'BR_MISSION|BR_PROGRESS|BR_HUD' > "$OUT/sequence.log" || true
grep -E 'BR_MISSION_SMOKE|BR_PROGRESS' "$OUT/sequence.log" | tail -${BR_TAIL:-40}
if [ -s "$OUT/window_watch.txt" ]; then echo "WARNING: engine windows were seen on screen:"; cat "$OUT/window_watch.txt"; else echo "window watch: no on-screen window owned by the engine"; fi
exit $RC
