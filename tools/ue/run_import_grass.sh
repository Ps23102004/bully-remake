#!/bin/bash
# OWNER: builder-A (importers)
# Instanced grass tufts into the saved Bullworth map (see tools/ue/import_grass.py and docs/re-notes/ae-grass.md).
# Needs imported/grass/ from `python3 -m tools.importers --install <data_0> world` and a map from run_import.sh.
# Re-run after every run_import.sh: its level step rebuilds the map and drops the grass actors.
#
#   tools/ue/run_import_grass.sh [--all] [--density 0.5] [--cull 2200,3800] [--dry]
#
# Same envelope as run_place_doors.sh: pythonscript commandlet, -nullrhi (no GPU, no window), shared GPU slot, no retries.
set -uo pipefail
source "$(dirname "$0")/_common.sh"
IMPORTED="$BR_REPO/imported"
while [ $# -gt 0 ]; do
  case "$1" in
    --all) export BR_GRASS_ALL=1; shift ;;
    --dry) export BR_GRASS_DRY=1; shift ;;
    --density) export BR_GRASS_DENSITY="$2"; shift 2 ;;
    --cull) export BR_GRASS_CULL="$2"; shift 2 ;;
    --imported) IMPORTED="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ -f "$IMPORTED/grass/index.json" ] || br_die "no $IMPORTED/grass/index.json: run the world importer first"
[ -f "$BR_REPO/unreal/BullyRemake/Content/Imported/Maps/Bullworth.umap" ] || br_die "level missing: run tools/ue/run_import.sh first"
br_preflight
OUT="$BR_REPO/unreal/BullyRemake/Saved/BullyImport"
mkdir -p "$OUT"
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="$OUT/grass_$STAMP.log"
export BR_IMPORTED="$IMPORTED" BR_GRASS_REPORT="$OUT/grass_report_$STAMP.json"

START=$(date +%s)
"$GPU_SLOT" capture --label "${BR_GPU_LABEL:-bully-grass}" -- \
  "$UE_CMD" "$BR_UPROJECT" -run=pythonscript -script="$BR_REPO/tools/ue/import_grass.py" \
  -unattended -nosplash -nullrhi -nopause -nocrashreports -NoSound -notraceserver -stdout -FullStdOutLogOutput \
  > "$LOG" 2>&1
RC=$?
br_reap_crash_reporters
echo "grass: engine exit $RC after $(( $(date +%s) - START ))s; log $LOG"
grep -E 'BR_GRASS|BR_IMPORT ERROR' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -40
[ -f "$BR_GRASS_REPORT" ] && cp "$BR_GRASS_REPORT" "$OUT/grass_report_latest.json" && echo "report: $BR_GRASS_REPORT"
if [ $RC -eq 0 ] && ! grep -q 'BR_GRASS_DONE OK' "$LOG"; then RC=3; fi
exit $RC
