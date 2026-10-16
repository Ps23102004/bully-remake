#!/bin/bash
# OWNER: slice-stairs
# Headless (-nullrhi, no window) capsule-sweep walkability probe of /Game/Imported/Maps/Bullworth.
# The python runs INSIDE the running game (`-game -ExecCmds="py <script>"`), so traces/sweeps use the same
# runtime physics scene the pawn walks on.
#   tools/ue/run_stairs_probe.sh [--script stairs_probe.py] [--out DIR]
set -uo pipefail
source "$(dirname "$0")/_common.sh"
SCRIPT=stairs_probe.py
OUT="$HOME/bully-remake-evidence/stairs"
while [ $# -gt 0 ]; do
  case "$1" in
    --script) SCRIPT="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
br_preflight
mkdir -p "$OUT"
export BR_STAIRS_OUT="$(cd "$OUT" && pwd)" BR_STAIRS_GAME=1
LOG="$BR_REPO/unreal/BullyRemake/Saved/BullyImport/stairs_$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$(dirname "$LOG")"
"$GPU_SLOT" capture --label "${BR_GPU_LABEL:-bully-stairs}" -- \
  "$UE_CMD" "$BR_UPROJECT" /Game/Imported/Maps/Bullworth -game -nullrhi -unattended -nosplash -nocrashreports \
  -NoSound -notraceserver -NoVSync -stdout -FullStdOutLogOutput -BRNoAmbient \
  -ExecCmds="py $BR_REPO/tools/ue/$SCRIPT" > "$LOG" 2>&1
RC=$?
br_reap_crash_reporters
grep -E 'BR_STAIRS' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -${BR_TAIL:-60}
echo "engine exit $RC; log $LOG"
exit $RC
