#!/bin/bash
# OWNER: builder-B6 (proof)
# Re-place the door-link actors (door_links.json) into the saved map, without re-importing anything
# (M6: the M5 re-import ran while the editor module was unbuilt and saved a map with no doors).
#
#   tools/ue/run_place_doors.sh
#
# pythonscript commandlet, -nullrhi (no GPU, no window), wrapped in the shared GPU slot like run_import.sh.
set -uo pipefail
source "$(dirname "$0")/_common.sh"
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"
br_preflight
OUT="$BR_REPO/unreal/BullyRemake/Saved/BullyImport"
mkdir -p "$OUT"
LOG="$OUT/place_doors_$(date +%Y%m%d-%H%M%S).log"

START=$(date +%s)
"$GPU_SLOT" capture --label bully-proof -- \
  "$UE_CMD" "$BR_UPROJECT" -run=pythonscript -script="$BR_REPO/tools/ue/place_doors.py" \
  -unattended -nosplash -nullrhi -nopause -nocrashreports -NoSound -notraceserver -stdout -FullStdOutLogOutput \
  > "$LOG" 2>&1
RC=$?
br_reap_crash_reporters
echo "place_doors: engine exit $RC after $(( $(date +%s) - START ))s; log $LOG"
grep -E 'BR_DOORS' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -15
if [ $RC -eq 0 ] && ! grep -q 'BR_DOORS_DONE OK' "$LOG"; then RC=3; fi
exit $RC
