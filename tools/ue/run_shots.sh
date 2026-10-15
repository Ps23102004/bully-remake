#!/bin/bash
# OWNER: builder-B (ue)
# Offscreen Lumen screenshots of /Game/Imported/Maps/Bullworth (run tools/ue/run_import.sh first).
#
#   tools/ue/run_shots.sh [--shots tools/ue/shots_m1.json] [--out DIR] [--res 3840x2160]
#
# Standalone -game run of the editor binary with -RenderOffScreen (Slate creates no native window),
# wrapped in the shared GPU slot. UBullyShotRunner (Source/BullyRemake) waits for shader/texture/
# distance-field compilation, places a camera per shot, lets Lumen and exposure settle, writes
# <out>/NN_<name>.png and <out>/shots_result.json, then quits. A watchdog records any on-screen window
# owned by the engine process tree into <out>/window_watch.txt (empty = no window appeared).
set -uo pipefail
source "$(dirname "$0")/_common.sh"

SHOTS="$BR_REPO/tools/ue/shots_m1.json"
OUT="$HOME/bully-remake-evidence/m1"
RES=3840x2160
MAP=/Game/Imported/Maps/Bullworth
while [ $# -gt 0 ]; do
  case "$1" in
    --shots) SHOTS="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --res) RES="$2"; shift 2 ;;
    --map) MAP="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ -f "$SHOTS" ] || br_die "shot file $SHOTS missing"
SHOTS="$(cd "$(dirname "$SHOTS")" && pwd)/$(basename "$SHOTS")"   # the engine runs from another cwd
OUT="$(mkdir -p "$OUT" && cd "$OUT" && pwd)"
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"
RX=${RES%x*}; RY=${RES#*x}

br_preflight
mkdir -p "$OUT"
LOG="$OUT/shots_$(date +%Y%m%d-%H%M%S).log"
echo "shots: $SHOTS -> $OUT at ${RX}x${RY}; log $LOG"
START=$(date +%s)
"$GPU_SLOT" capture --label bully-ue-shots -- \
  "$UE_CMD" "$BR_UPROJECT" "$MAP" -game -RenderOffScreen -ResX="$RX" -ResY="$RY" -windowed \
  -unattended -nosplash -nocrashreports -NoSound -notraceserver -NoVSync -stdout -FullStdOutLogOutput \
  -BRShotFile="$SHOTS" -BRShotDir="$OUT" \
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
grep -E 'BR_SHOT' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -20
NFAIL=$(grep -c 'Failed to compile Material' "$LOG")
[ "$NFAIL" -gt 0 ] && { echo "WARNING: $NFAIL material compile failures (default material used):"; grep -A2 'Failed to compile Material' "$LOG" | grep -v '^--' | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | sort -u | head -20; }
for f in "$OUT"/[0-9][0-9]_*.png; do [ -f "$f" ] && echo "$(basename "$f") $(sips -g pixelWidth -g pixelHeight "$f" | awk '/pixel/{printf "%s ", $2}')"; done
if [ -s "$OUT/window_watch.txt" ]; then echo "WARNING: engine windows were seen on screen:"; cat "$OUT/window_watch.txt"; else echo "window watch: no on-screen window owned by the engine"; fi
ls -la "$OUT"/*.png 2>/dev/null
exit $RC
