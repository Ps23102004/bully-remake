#!/bin/bash
# OWNER: builder-B (ue)
# Headless import of imported/ (tools/importers output) into /Game/Imported + level assembly.
#
#   tools/ue/run_import.sh [--scenes core|all|a,b,c] [--steps textures,materials,meshes,level]
#                          [--limit N] [--imported DIR]
#
# Runs UnrealEditor-Cmd as the pythonscript commandlet with -nullrhi -unattended -nosplash (no window,
# no GPU device), wrapped in the shared GPU slot. Exit code = the engine's; the script then checks
# the report for BR_IMPORT errors. Log: unreal/BullyRemake/Saved/BullyImport/import_<stamp>.log
set -uo pipefail
source "$(dirname "$0")/_common.sh"

SCENES=all
STEPS=textures,materials,meshes,level
LIMIT=0
IMPORTED="$BR_REPO/imported"
while [ $# -gt 0 ]; do
  case "$1" in
    --scenes) SCENES="$2"; shift 2 ;;
    --steps) STEPS="$2"; shift 2 ;;
    --limit) LIMIT="$2"; shift 2 ;;
    --imported) IMPORTED="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ -f "$IMPORTED/scenes/academy.json" ] || br_die "$IMPORTED/scenes/academy.json missing: run python3 -m tools.importers world first"

br_preflight
OUT="$BR_REPO/unreal/BullyRemake/Saved/BullyImport"
mkdir -p "$OUT"
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="$OUT/import_$STAMP.log"
REPORT="$OUT/report_$STAMP.json"

export BR_IMPORTED="$IMPORTED" BR_SCENES="$SCENES" BR_STEPS="$STEPS" BR_LIMIT="$LIMIT" BR_REPORT="$REPORT"
echo "import: scenes=$SCENES steps=$STEPS limit=$LIMIT log=$LOG"
START=$(date +%s)
"$GPU_SLOT" capture --label bully-ue-import -- \
  "$UE_CMD" "$BR_UPROJECT" -run=pythonscript -script="$BR_REPO/tools/ue/import_content.py" \
  -unattended -nosplash -nullrhi -nopause -nocrashreports -NoSound -notraceserver -stdout -FullStdOutLogOutput \
  > "$LOG" 2>&1
RC=$?
br_reap_crash_reporters
echo "engine exit $RC after $(( $(date +%s) - START ))s"
grep -E 'BR_IMPORT (\[|ERROR)|BR_IMPORT_DONE' "$LOG" | tail -25
[ -f "$REPORT" ] && cp "$REPORT" "$OUT/report_latest.json" && echo "report: $REPORT"
if [ $RC -eq 0 ] && ! grep -q 'BR_IMPORT_DONE OK' "$LOG"; then RC=3; fi
exit $RC
