#!/bin/bash
# OWNER: polish1 (headless logic verification)
# Scripted gameplay run with NO RENDERER: `-game -nullrhi` (no GPU, no window, no audio device). Same script format and
# walk runner as run_walk.sh; steps that need pixels (shot, action_shot's shot, record_*) are skipped by the script
# author. Use it to verify game LOGIC from the log: BR_AMBIENT YIELD, BR_DOOR state changes, BR_AUDIO surface picks.
#
#   tools/ue/run_walk_nullrhi.sh --script tools/ue/walk_polish_logic.json --out ~/bully-remake-evidence/polish1/<name>
#
# Same engine-safety envelope as run_walk.sh (GPU-slot wrapper, window watchdog that SIGTERMs the engine on any window,
# no retry). An empty <out>/window_watch.txt is the evidence that no window appeared.
# -UseFixedTimeStep -FPS=60: deterministic 60 Hz game time, run as fast as the CPU allows (an unthrottled nullrhi game ticks
# at ~2000 Hz, which is not the timing the game is tuned for).
set -uo pipefail
source "$(dirname "$0")/_common.sh"

SCRIPT="$BR_REPO/tools/ue/walk_polish_logic.json"
OUT="$HOME/bully-remake-evidence/polish1/logic"
MAP=/Game/Imported/Maps/Bullworth
LABEL=bully-polish-nullrhi
while [ $# -gt 0 ]; do
  case "$1" in
    --script) SCRIPT="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --map) MAP="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ -f "$SCRIPT" ] || br_die "script $SCRIPT missing"
SCRIPT="$(cd "$(dirname "$SCRIPT")" && pwd)/$(basename "$SCRIPT")"
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$SCRIPT" || br_die "script $SCRIPT is not valid JSON"
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"

br_preflight
mkdir -p "$OUT"
LOG="$OUT/run_$(date +%Y%m%d-%H%M%S).log"
echo "nullrhi walk: $SCRIPT -> $OUT; log $LOG"
START=$(date +%s)
"$GPU_SLOT" capture --label "$LABEL" -- \
  "$UE_CMD" "$BR_UPROJECT" "$MAP" -game -nullrhi -unattended -nosplash -nocrashreports -NoSound -notraceserver -NoVSync \
  -stdout -FullStdOutLogOutput -ResX=640 -ResY=360 -windowed -UseFixedTimeStep -FPS=60 \
  -BRWalkFile="$SCRIPT" -BRWalkDir="$OUT" ${BR_EXTRA_ARGS:-} \
  > "$LOG" 2>&1 &
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
wait "$WRAP"
RC=$?
wait "$WATCH" 2>/dev/null
br_reap_crash_reporters
echo "engine exit $RC after $(( $(date +%s) - START ))s"
grep -E 'BR_WALK|BR_DOOR|BR_AMBIENT (YIELD|STUCK|SUMMARY)|BR_AUDIO (surface|step)' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -${BR_TAIL:-120}
if [ -s "$OUT/window_watch.txt" ]; then echo "WARNING: engine windows were seen on screen:"; cat "$OUT/window_watch.txt"; else echo "window watch: no on-screen window owned by the engine"; fi
exit $RC
