#!/bin/bash
# OWNER: builder-B4 (ambient)
# Headless import of the milestone-4 students (skeletal meshes + clips) and decoded game audio.
#
#   python3 -m tools.importers audio extract FeetAll Wind Birds           # imported/audio/ (WAV, git-ignored)
#   for m in GN_Boy01 ...; do python3 -m tools.importers ped $m --anims IDLE PLAYERWALK ...; done   (see tools/ue/README.md)
#   tools/ue/run_import_ambient.sh [--steps students,audio]
#
# Same envelope as run_import.sh: UnrealEditor-Cmd pythonscript commandlet, -nullrhi -unattended -nosplash, inside
# the shared GPU slot (label bully-ambient), no retries.
set -uo pipefail
source "$(dirname "$0")/_common.sh"
STEPS=students,audio
IMPORTED="$BR_REPO/imported"
while [ $# -gt 0 ]; do
  case "$1" in
    --steps) STEPS="$2"; shift 2 ;;
    --imported) IMPORTED="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
br_preflight
OUT="$BR_REPO/unreal/BullyRemake/Saved/BullyImport"
mkdir -p "$OUT"
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="$OUT/ambient_$STAMP.log"
REPORT="$OUT/ambient_report_$STAMP.json"
export BR_IMPORTED="$IMPORTED" BR_REPORT="$REPORT" BR_AMBIENT_STEPS="$STEPS"
echo "ambient import: steps=$STEPS log=$LOG"
START=$(date +%s)
"$GPU_SLOT" capture --label "${BR_GPU_LABEL:-bully-ambient}" -- \
  "$UE_CMD" "$BR_UPROJECT" -run=pythonscript -script="$BR_REPO/tools/ue/import_ambient.py" \
  -unattended -nosplash -nullrhi -nopause -nocrashreports -NoSound -notraceserver -stdout -FullStdOutLogOutput \
  > "$LOG" 2>&1
RC=$?
br_reap_crash_reporters
echo "engine exit $RC after $(( $(date +%s) - START ))s"
grep -E 'BR_AMBIENT_IMPORT|BR_CHAR_IMPORT_DONE' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -40
[ -f "$REPORT" ] && cp "$REPORT" "$OUT/ambient_report_latest.json" && echo "report: $REPORT"
if [ $RC -eq 0 ] && ! grep -q 'BR_AMBIENT_IMPORT_DONE OK' "$LOG"; then RC=3; fi
exit $RC
