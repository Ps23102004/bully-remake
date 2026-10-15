#!/bin/bash
# OWNER: slice-combat
# Headless combat test: `-game -nullrhi -NoSound` (no GPU, no window, no audio), -BRCombatTest drives the fight
# tutorial (block x2, 3-hit chain, grapple + 3 strikes, grapple + push, ground kick), a free fight to KO and a
# floored partner humiliated, through the real Enhanced Input actions. Logs BR_COMBAT lines; writes
# <out>/combat_result.json. Same envelope as run_walk_nullrhi.sh: GPU-slot wrapper, window watchdog, no retry,
# deterministic 60 Hz fixed step, SIGTERM -> 60 s -> SIGKILL only as a last resort.
#
#   tools/ue/run_combat_nullrhi.sh [--out ~/bully-remake-evidence/combat] [--label bully-combat]
set -uo pipefail
source "$(dirname "$0")/_common.sh"

OUT="$HOME/bully-remake-evidence/combat"
MAP=/Game/Imported/Maps/Bullworth
LABEL=bully-combat
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --map) MAP="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"

br_preflight
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
LOG="$OUT/combat_$(date +%Y%m%d-%H%M%S).log"
echo "nullrhi combat test -> $OUT; log $LOG"
START=$(date +%s)
"$GPU_SLOT" capture --label "$LABEL" -- \
  "$UE_CMD" "$BR_UPROJECT" "$MAP" -game -nullrhi -unattended -nosplash -nocrashreports -NoSound -notraceserver -NoVSync \
  -stdout -FullStdOutLogOutput -ResX=640 -ResY=360 -windowed -UseFixedTimeStep -FPS=60 -BRNoAmbient \
  -BRCombatTest="$OUT" ${BR_EXTRA_ARGS:-} \
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
grep -E 'BR_COMBAT|BR_MISSION event' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' > "$OUT/combat_sequence.log"
grep -E 'BR_COMBAT (PASS|FAIL|SUMMARY|enemy KO|event fired)' "$OUT/combat_sequence.log" | tail -${BR_TAIL:-60}
if [ -s "$OUT/window_watch.txt" ]; then echo "WARNING: engine windows were seen on screen:"; cat "$OUT/window_watch.txt"; else echo "window watch: no on-screen window owned by the engine"; fi
exit $RC
