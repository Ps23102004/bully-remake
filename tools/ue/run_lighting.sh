#!/bin/bash
# OWNER: builder-B5 (shot)
# Re-apply the lighting rig of one variant to the saved map, without re-importing anything.
#
#   tools/ue/run_lighting.sh [--variant A_classic_gold]
#
# pythonscript commandlet, -nullrhi (no GPU, no window), wrapped in the shared GPU slot like run_import.sh.
set -uo pipefail
source "$(dirname "$0")/_common.sh"
VARIANT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --variant) VARIANT="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"
br_preflight
OUT="$BR_REPO/unreal/BullyRemake/Saved/BullyImport"
mkdir -p "$OUT"
LOG="$OUT/lighting_$(date +%Y%m%d-%H%M%S).log"
export BR_LIGHTING_STANDALONE=1 BR_LIGHT_VARIANT="$VARIANT" BR_LIGHT_OUT="$OUT"
START=$(date +%s)
"$GPU_SLOT" capture --label bully-shot -- \
  "$UE_CMD" "$BR_UPROJECT" -run=pythonscript -script="$BR_REPO/tools/ue/lighting.py" \
  -unattended -nosplash -nullrhi -nopause -nocrashreports -NoSound -notraceserver -stdout -FullStdOutLogOutput \
  > "$LOG" 2>&1
RC=$?
br_reap_crash_reporters
echo "lighting: variant=${VARIANT:-default} engine exit $RC after $(( $(date +%s) - START ))s; log $LOG"
grep -E 'BR_LIGHT' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -15
if [ $RC -eq 0 ] && ! grep -q 'BR_LIGHT_DONE OK' "$LOG"; then RC=3; fi
exit $RC
