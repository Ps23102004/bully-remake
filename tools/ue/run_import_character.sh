#!/bin/bash
# OWNER: builder-B3 (jimmy)
# Headless import of a skinned ped (tools/importers `ped`) into /Game/Imported/Characters/<Name>.
#
#   python3 -m tools.importers ped PLAYER --anims '*' --root-motion inplace     # first (writes imported/models/PLAYER.gltf)
#   tools/ue/run_import_character.sh [--model PLAYER] [--name Jimmy] [--anims all|IDLE,PLAYERRUN,...] [--imported DIR]
#
# Same envelope as run_import.sh: UnrealEditor-Cmd pythonscript commandlet, -nullrhi -unattended -nosplash, inside
# the shared GPU slot (label bully-jimmy), no retries. Log + report in unreal/BullyRemake/Saved/BullyImport/.
set -uo pipefail
source "$(dirname "$0")/_common.sh"

MODEL=PLAYER
NAME=Jimmy
ANIMS=all
IMPORTED="$BR_REPO/imported"
while [ $# -gt 0 ]; do
  case "$1" in
    --model) MODEL="$2"; shift 2 ;;
    --name) NAME="$2"; shift 2 ;;
    --anims) ANIMS="$2"; shift 2 ;;
    --imported) IMPORTED="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ -f "$IMPORTED/models/$MODEL.gltf" ] || br_die "$IMPORTED/models/$MODEL.gltf missing: run python3 -m tools.importers ped $MODEL --anims '*' first"

br_preflight
OUT="$BR_REPO/unreal/BullyRemake/Saved/BullyImport"
mkdir -p "$OUT"
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="$OUT/character_$STAMP.log"
REPORT="$OUT/character_report_$STAMP.json"
export BR_IMPORTED="$IMPORTED" BR_CHAR_MODEL="$MODEL" BR_CHAR_NAME="$NAME" BR_CHAR_ANIMS="$ANIMS" BR_REPORT="$REPORT"
echo "character import: $MODEL -> /Game/Imported/Characters/$NAME anims=$ANIMS log=$LOG"
START=$(date +%s)
"$GPU_SLOT" capture --label "${BR_GPU_LABEL:-bully-jimmy}" -- \
  "$UE_CMD" "$BR_UPROJECT" -run=pythonscript -script="$BR_REPO/tools/ue/import_character.py" \
  -unattended -nosplash -nullrhi -nopause -nocrashreports -NoSound -notraceserver -stdout -FullStdOutLogOutput \
  > "$LOG" 2>&1
RC=$?
br_reap_crash_reporters
echo "engine exit $RC after $(( $(date +%s) - START ))s"
grep -E 'BR_CHAR_IMPORT' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -40
[ -f "$REPORT" ] && cp "$REPORT" "$OUT/character_report_latest.json" && echo "report: $REPORT"
if [ $RC -eq 0 ] && ! grep -q 'BR_CHAR_IMPORT_DONE OK' "$LOG"; then RC=3; fi
exit $RC
