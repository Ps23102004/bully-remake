#!/bin/bash
# OWNER: slice-weapons
# Generic headless smoke: `-game -nullrhi` on the Bullworth map with whatever harness flag(s) you pass; the harness
# drives itself, logs, and quits the engine on its own (e.g. -BRWeaponSmoke, -BRVehicleSmoke).
#
#   tools/ue/run_smoke_nullrhi.sh --flag -BRWeaponSmoke [--flag ...] [--out DIR] [--grep 'BR_WEAPON'] [--seconds-limit 300]
#
# Same engine-safety envelope as the other runners: GPU-slot wrapper, window watchdog (empty window_watch.txt =
# no-window evidence), no retry, SIGTERM -> 60 s -> SIGKILL. Exit code is the engine's.
set -uo pipefail
source "$(dirname "$0")/_common.sh"

FLAGS=""
OUT=""
GREP="BR_"
LIMIT=300
LABEL=bully-smoke
MAP=/Game/Imported/Maps/Bullworth
while [ $# -gt 0 ]; do
  case "$1" in
    --flag) FLAGS="$FLAGS $2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --grep) GREP="$2"; shift 2 ;;
    --seconds-limit) LIMIT="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ -n "$FLAGS" ] || br_die "usage: run_smoke_nullrhi.sh --flag -BRSomethingSmoke [--out DIR] [--grep PATTERN]"
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"
[ -n "$OUT" ] || OUT="$HOME/bully-remake-evidence/smoke_$(echo "$FLAGS" | tr -dc 'A-Za-z')"

br_preflight
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
rm -f "$OUT/ue_run.log" "$OUT/summary.log" "$OUT/window_watch.txt"
echo "smoke:$FLAGS -> $OUT"
START=$(date +%s)
"$GPU_SLOT" capture --label "$LABEL" -- \
  "$UE_CMD" "$BR_UPROJECT" "$MAP" -game -nullrhi -unattended -nosplash -nocrashreports -NoSound -notraceserver -NoVSync \
  -stdout -FullStdOutLogOutput -forcelogflush -abslog="$OUT/ue_run.log" -ResX=640 -ResY=360 -windowed -BRNoAmbient \
  -UseFixedTimeStep -FPS=60 $FLAGS ${BR_EXTRA_ARGS:-} \
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
sed -E 's/^\[[^]]*\]\[[^]]*\]//' "$OUT/ue_run.log" | grep -E "$GREP" > "$OUT/summary.log" || true
tail -${BR_TAIL:-60} "$OUT/summary.log"
if [ -s "$OUT/window_watch.txt" ]; then echo "WARNING: engine windows were seen on screen:"; cat "$OUT/window_watch.txt"; else echo "window watch: no on-screen window owned by the engine"; fi
exit $RC
