#!/bin/bash
# OWNER: slice-missions
# Headless Day One mission playthrough: `-game -nullrhi` (no GPU, no window, no audio), real-time 60 fps cap (the python driver ticks on
# Slate, which an unthrottled fixed-step run starves), inside the
# shared GPU slot (label bully-missions), window watchdog (any on-screen window -> SIGTERM), no retries. The missions are
# driven by drive_missions.py (Python via `-ExecCmds="py ..."`) using only console commands.
#
#   Config/Missions/verify/run_headless.sh --mode chain   [--out DIR]   # Mission.Start 1_01 ... driver chains into 1_02
#   Config/Missions/verify/run_headless.sh --mode day_one [--out DIR]   # real auto-start: -BRMission=day_one
#
# Output (default ~/bully-remake-evidence/missions/<mode>): ue_run.log (engine log), sequence.log (BR_MISSION / BR_SNPC /
# BR_COMBAT / BR_VERIFY lines), driver_result.json, window_watch.txt (empty = no window ever appeared).
# Exit: engine exit code (the driver quits the engine itself); a hard time limit sends SIGTERM, then SIGKILL after 60 s.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../../../.." && pwd)"
source "$REPO/tools/ue/_common.sh"

MODE=chain
OUT=""
MAP=/Game/Imported/Maps/Bullworth
LIMIT=900
while [ $# -gt 0 ]; do
  case "$1" in
    --mode) MODE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --seconds-limit) LIMIT="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ "$MODE" = chain ] || [ "$MODE" = day_one ] || br_die "--mode chain|day_one"
[ -n "$OUT" ] || OUT="$HOME/bully-remake-evidence/missions/$MODE"
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"

br_preflight
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
rm -f "$OUT/driver_result.json" "$OUT/ue_run.log"
EXTRA=""
[ "$MODE" = day_one ] && EXTRA="-BRMission=day_one"
export BR_VERIFY_MODE="$MODE" BR_VERIFY_OUT="$OUT" BR_VERIFY_LOG="$OUT/ue_run.log" BR_VERIFY_MISSIONS="$BR_REPO/unreal/BullyRemake/Config/Missions"
echo "missions headless: mode=$MODE -> $OUT"
START=$(date +%s)
"$GPU_SLOT" capture --label bully-missions -- \
  "$UE_CMD" "$BR_UPROJECT" "$MAP" -game -nullrhi -unattended -nosplash -nocrashreports -NoSound -notraceserver -NoVSync \
  -stdout -FullStdOutLogOutput -forcelogflush -abslog="$OUT/ue_run.log" -ResX=640 -ResY=360 -windowed -BRNoAmbient \
  $EXTRA -ExecCmds="t.MaxFPS 60,py $HERE/drive_missions.py" ${BR_EXTRA_ARGS:-} \
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
sed -E 's/^\[[^]]*\]\[[^]]*\]//' "$OUT/ue_run.log" | grep -E 'BR_MISSION|BR_SNPC|BR_COMBAT|BR_VERIFY|BR_DOOR' | grep -v -E 'BR_SNPC (STUCK|path-check)' > "$OUT/sequence.log"
grep -E 'BR_VERIFY .*(RESULT|STAGE BEGIN|STAGE FAIL|MISSION COMPLETE|CHAIN|FORCED)' "$OUT/sequence.log" | tail -${BR_TAIL:-80}
if [ -s "$OUT/window_watch.txt" ]; then echo "WARNING: engine windows were seen on screen:"; cat "$OUT/window_watch.txt"; else echo "window watch: no on-screen window owned by the engine"; fi
exit $RC
