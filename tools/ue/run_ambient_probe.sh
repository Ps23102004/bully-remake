#!/bin/bash
# OWNER: builder-B4 (ambient)
# Headless walkability probe of /Game/Imported/Maps/Bullworth for authoring Config/AmbientLoops.json:
#   tools/ue/run_ambient_probe.sh X0 Y0 X1 Y1 STEP [ZTOP ZBOTTOM]      (UE cm; e.g. 18000 3000 31000 12000 150 900 300)
# -nullrhi (no rendering, no window), GPU-slot wrapped, no retries. Prints the ASCII map (BR_PROBE lines).
set -uo pipefail
source "$(dirname "$0")/_common.sh"
[ $# -ge 5 ] || br_die "usage: run_ambient_probe.sh X0 Y0 X1 Y1 STEP [ZTOP ZBOTTOM]"
ARGS=$(IFS=,; echo "$*")
br_preflight
OUT="$BR_REPO/unreal/BullyRemake/Saved/BullyImport"
mkdir -p "$OUT"
LOG="$OUT/probe_$(date +%Y%m%d-%H%M%S).log"
"$GPU_SLOT" capture --label "${BR_GPU_LABEL:-bully-ambient}" -- \
  "$UE_CMD" "$BR_UPROJECT" /Game/Imported/Maps/Bullworth -game -nullrhi -unattended -nosplash -nocrashreports \
  -NoSound -notraceserver -NoVSync -stdout -FullStdOutLogOutput -BRAmbientProbe="$ARGS" > "$LOG" 2>&1
RC=$?
br_reap_crash_reporters
grep 'BR_PROBE' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//; s/^LogAmbientSys: //'
echo "engine exit $RC; log $LOG"
exit $RC
