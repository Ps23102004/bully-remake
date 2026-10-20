#!/bin/bash
# OWNER: builder-B (ue)
# Character render check: spawn (or with --remove, delete) the br_lineup actors in the saved Bullworth map.
#   tools/ue/run_lineup.sh [--remove]      then: tools/ue/run_shots.sh --shots tools/ue/shots_lineup.json --out DIR
# Headless pythonscript commandlet (-nullrhi), through the GPU slot like every other Unreal launch.
set -uo pipefail
source "$(dirname "$0")/_common.sh"
[ "${1:-}" = "--remove" ] && export BR_LINEUP_REMOVE=1
br_preflight
OUT="$BR_REPO/unreal/BullyRemake/Saved/BullyImport"
mkdir -p "$OUT"
LOG="$OUT/lineup_$(date +%Y%m%d-%H%M%S).log"
"$GPU_SLOT" capture --label bully-lineup -- \
  "$UE_CMD" "$BR_UPROJECT" -run=pythonscript -script="$BR_REPO/tools/ue/lineup.py" \
  -unattended -nosplash -nullrhi -nopause -nocrashreports -NoSound -notraceserver -stdout -FullStdOutLogOutput \
  > "$LOG" 2>&1
RC=$?
br_reap_crash_reporters
grep -E 'BR_LINEUP' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -25
echo "lineup: engine exit $RC; log $LOG"
if [ $RC -eq 0 ] && ! grep -q 'BR_LINEUP_DONE OK' "$LOG"; then RC=3; fi
exit $RC
