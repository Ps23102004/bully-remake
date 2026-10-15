#!/bin/bash
# OWNER: builder-B2 (doors)
# Scripted gameplay run on /Game/Imported/Maps/Bullworth (run tools/ue/run_import.sh first): the real pawn walks
# a JSON script (UBullyWalkRunner), uses the real door links and takes offscreen PNGs. Same engine-safety
# envelope as run_shots.sh (GPU slot, -RenderOffScreen, no window, SIGTERM then 60 s).
#
#   tools/ue/run_walk.sh [--script tools/ue/walk_m2.json] [--out DIR] [--res 1920x1080]
#
# Standalone -game run of the editor binary with -RenderOffScreen (Slate creates no native window),
# wrapped in the shared GPU slot. UBullyWalkRunner waits for shader/texture/distance-field compilation,
# then executes the script; writes <out>/NN_<name>.png and <out>/walk_result.json, then quits. A watchdog records any on-screen window
# owned by the engine process tree into <out>/window_watch.txt (empty = no window appeared).
set -uo pipefail
source "$(dirname "$0")/_common.sh"

SHOTS="$BR_REPO/tools/ue/walk_m2.json"
OUT="$HOME/bully-remake-evidence/m2"
RES=1920x1080
MAP=/Game/Imported/Maps/Bullworth
while [ $# -gt 0 ]; do
  case "$1" in
    --script) SHOTS="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --res) RES="$2"; shift 2 ;;
    --map) MAP="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ -f "$SHOTS" ] || br_die "script $SHOTS missing"
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"
RX=${RES%x*}; RY=${RES#*x}

br_preflight
mkdir -p "$OUT"
LOG="$OUT/walk_$(date +%Y%m%d-%H%M%S).log"
echo "walk: $SHOTS -> $OUT at ${RX}x${RY}; log $LOG"
START=$(date +%s)
"$GPU_SLOT" capture --label bully-doors -- \
  "$UE_CMD" "$BR_UPROJECT" "$MAP" -game -RenderOffScreen -ResX="$RX" -ResY="$RY" -windowed \
  -unattended -nosplash -nocrashreports -NoSound -notraceserver -NoVSync -stdout -FullStdOutLogOutput \
  -BRWalkFile="$SHOTS" -BRWalkDir="$OUT" \
  > "$LOG" 2>&1 &
WRAP=$!
br_window_watch "$WRAP" "$OUT/window_watch.txt" &
WATCH=$!
# SIGTERM/INT to this script: stop the engine politely (SIGTERM, 60 s, then SIGKILL as last resort)
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
grep -E 'BR_WALK|BR_DOOR|BR_PAWN' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -60
NFAIL=$(grep -c 'Failed to compile Material' "$LOG")
[ "$NFAIL" -gt 0 ] && { echo "WARNING: $NFAIL material compile failures (default material used):"; grep -A2 'Failed to compile Material' "$LOG" | grep -v '^--' | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | sort -u | head -20; }
for f in "$OUT"/[0-9][0-9]_*.png; do [ -f "$f" ] && echo "$(basename "$f") $(sips -g pixelWidth -g pixelHeight "$f" | awk '/pixel/{printf "%s ", $2}')"; done
if [ -s "$OUT/window_watch.txt" ]; then echo "WARNING: engine windows were seen on screen:"; cat "$OUT/window_watch.txt"; else echo "window watch: no on-screen window owned by the engine"; fi
ls -la "$OUT"/*.png 2>/dev/null
exit $RC
