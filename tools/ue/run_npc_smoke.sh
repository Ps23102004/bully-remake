#!/bin/bash
# OWNER: slice-npc (scripted NPCs)
# Headless scripted-NPC beat run: `-game -nullrhi` (no GPU, no window, no audio device), fixed 60 Hz game time. The
# -BRNPCSmoke harness (BullyScriptedNPCSmoke.cpp) fires the mission events the 1_01 / 1_02 stages would, walks the real
# player pawn through the gauntlet, starts the smoke mission for the stage hooks, and logs BR_SNPC lines + a PASS/FAIL result.
#
#   tools/ue/run_npc_smoke.sh [--phases stage,gauntlet,crowd] [--out DIR] [--script day_one] [--seconds-limit 600]
#
# Same engine-safety envelope as run_walk_nullrhi.sh: GPU-slot wrapper (label bully-npc), window watchdog that SIGTERMs the
# engine if any window appears (an empty window_watch.txt is the no-window evidence), no retry, SIGTERM -> 60 s -> SIGKILL.
set -uo pipefail
source "$(dirname "$0")/_common.sh"

PHASES="stage,gauntlet,crowd"
OUT="$HOME/bully-remake-evidence/npc"
MAP=/Game/Imported/Maps/Bullworth
LABEL=bully-npc
LIMIT=900
SCRIPT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --phases) PHASES="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --script) SCRIPT="$2"; shift 2 ;;
    --seconds-limit) LIMIT="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"
[ -f "$BR_REPO/imported/triggers/day_one.json" ] || br_die "imported/triggers/day_one.json missing: python3 -m tools.importers trigger (BULLY_INSTALL set)"

br_preflight
mkdir -p "$OUT"
LOG="$OUT/run_$(date +%Y%m%d-%H%M%S).log"
echo "npc smoke: phases=$PHASES -> $OUT; log $LOG"
START=$(date +%s)
EXTRA=""
[ -n "$SCRIPT" ] && EXTRA="-BRNPCScript=$SCRIPT"
"$GPU_SLOT" capture --label "$LABEL" -- \
  "$UE_CMD" "$BR_UPROJECT" "$MAP" -game -nullrhi -unattended -nosplash -nocrashreports -NoSound -notraceserver -NoVSync \
  -stdout -FullStdOutLogOutput -ResX=640 -ResY=360 -windowed -UseFixedTimeStep -FPS=60 \
  -BRNPCSmoke="$PHASES" $EXTRA ${BR_EXTRA_ARGS:-} \
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
( sleep "$LIMIT"; echo "time limit $LIMIT s reached: stopping the engine" >&2; stop_engine ) &
TIMER=$!
wait "$WRAP"
RC=$?
kill "$TIMER" 2>/dev/null
wait "$WATCH" 2>/dev/null
br_reap_crash_reporters
echo "engine exit $RC after $(( $(date +%s) - START ))s"
sed -E 's/^\[[^]]*\]\[[^]]*\]//' "$LOG" | grep -E 'BR_SNPC' > "$OUT/beats.log"
grep -c . "$OUT/beats.log" | sed 's/^/BR_SNPC lines: /'
grep -E 'BR_SNPC_SMOKE (PASS|FAIL|RESULT)' "$OUT/beats.log"
if [ -s "$OUT/window_watch.txt" ]; then echo "WARNING: engine windows were seen on screen:"; cat "$OUT/window_watch.txt"; else echo "window watch: no on-screen window owned by the engine"; fi
exit $RC
