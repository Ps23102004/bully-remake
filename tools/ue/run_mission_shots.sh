#!/bin/bash
# OWNER: mission-fw
# Rendered cutscene-composition pass: run a mission in a real renderer (`-game -RenderOffScreen`, Slate makes no native
# window) and have UMissionSubsystem grab one HighResShot a beat after each stage begins -- the real mission cameras, the
# spawned characters and the authored framings, in pixels. This is the render the headless smoke harness can't give you:
# the smoke run proves the cuts/beats fire; this shows what they actually frame. The subsystem waits for shader/texture/
# distance-field compilation to drain, warms up, starts the mission, shoots every stage, then quits itself (it stops
# before a chaining mission -- e.g. arrival -> day_one -- so you only get the mission you asked for).
#
#   tools/ue/run_mission_shots.sh <mission_id> [--out DIR] [--res 1920x1080] [--seconds-limit 900]
#
# Same engine-safety envelope as the other runners: GPU-slot wrapper (cap enforced; FIFO-waits if both slots are busy),
# window watchdog that SIGTERMs the engine if any window appears (empty window_watch.txt = no-window evidence), no retry,
# SIGTERM -> 60 s -> SIGKILL. Output: <out>/NN_<stageid>.png per beat + the run log. Exit code is the engine's.
set -uo pipefail
source "$(dirname "$0")/_common.sh"

MISSION=""
OUT=""
RES=1920x1080
MAP=/Game/Imported/Maps/Bullworth
LABEL=bully-mission-shots
LIMIT=900
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --res) RES="$2"; shift 2 ;;
    --seconds-limit) LIMIT="$2"; shift 2 ;;
    -*) br_die "unknown argument $1" ;;
    *) [ -z "$MISSION" ] || br_die "only one mission id"; MISSION="$1"; shift ;;
  esac
done
[ -n "$MISSION" ] || br_die "usage: run_mission_shots.sh <mission_id> [--out DIR] [--res WxH] [--seconds-limit N]"
[ -f "$BR_REPO/unreal/BullyRemake/Config/Missions/$MISSION.json" ] || br_die "no such mission: Config/Missions/$MISSION.json"
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"
[ -n "$OUT" ] || OUT="$HOME/bully-remake-evidence/missions/shots_$MISSION"
RX=${RES%x*}; RY=${RES#*x}

br_preflight
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
rm -f "$OUT/ue_run.log" "$OUT/window_watch.txt" "$OUT"/[0-9][0-9]_*.png
echo "mission shots: $MISSION -> $OUT at ${RX}x${RY}"
START=$(date +%s)
"$GPU_SLOT" capture --label "$LABEL" -- \
  "$UE_CMD" "$BR_UPROJECT" "$MAP" -game -RenderOffScreen -ResX="$RX" -ResY="$RY" -windowed \
  -unattended -nosplash -nocrashreports -NoSound -notraceserver -NoVSync -stdout -FullStdOutLogOutput -forcelogflush \
  -abslog="$OUT/ue_run.log" -BRNoAmbient -BRMissionShots="$MISSION" -BRShotDir="$OUT" -BRShotResX="$RX" -BRShotResY="$RY" \
  ${BR_EXTRA_ARGS:-} \
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
grep -E 'BR_MISSIONSHOT|BR_MISSION ' "$OUT/ue_run.log" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -40
for f in "$OUT"/[0-9][0-9]_*.png; do [ -f "$f" ] && echo "$(basename "$f") $(sips -g pixelWidth -g pixelHeight "$f" 2>/dev/null | awk '/pixel/{printf "%s ", $2}')"; done
if [ -s "$OUT/window_watch.txt" ]; then echo "WARNING: engine windows were seen on screen:"; cat "$OUT/window_watch.txt"; else echo "window watch: no on-screen window owned by the engine"; fi
exit $RC
