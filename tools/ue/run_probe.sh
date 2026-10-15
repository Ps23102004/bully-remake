#!/bin/bash
# OWNER: builder-B5 (shot)
# Diagnostic: list level actors projecting into pixel rects of a shot camera (probe_actors.py), no GPU/no window.
#   tools/ue/run_probe.sh --mode scene --near "x,y,z,r" --mats isc_,ischool --out probe.json
#   tools/ue/run_probe.sh --shot facade_courtyard --rects "1600,740,1720,880;2200,860,2260,1040" --out probe.json [--shots file]
set -uo pipefail
source "$(dirname "$0")/_common.sh"
SHOT=""; RECTS=""; OUTF=""; SHOTS=""; MODE=actors; NEAR=""; MATS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --shot) SHOT="$2"; shift 2 ;; --rects) RECTS="$2"; shift 2 ;; --out) OUTF="$2"; shift 2 ;; --shots) SHOTS="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;; --near) NEAR="$2"; shift 2 ;; --mats) MATS="$2"; shift 2 ;;
    *) br_die "unknown argument $1" ;;
  esac
done
[ -n "$OUTF" ] || br_die "need --out"
SCRIPT=probe_actors.py; [ "$MODE" = scene ] && SCRIPT=probe_scene.py
[ "$MODE" = scene ] || { [ -n "$SHOT" ] && [ -n "$RECTS" ] || br_die "need --shot --rects"; }
br_preflight
LOG="$BR_REPO/unreal/BullyRemake/Saved/BullyImport/probe_$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$(dirname "$LOG")"
export BR_PROBE_SHOT="$SHOT" BR_PROBE_RECTS="$RECTS" BR_PROBE_OUT="$(cd "$(dirname "$OUTF")" && pwd)/$(basename "$OUTF")"
[ -n "$SHOTS" ] && export BR_PROBE_SHOTS="$SHOTS"
export BR_PROBE_NEAR="$NEAR" BR_PROBE_MATS="$MATS"
"$GPU_SLOT" capture --label bully-shot2 -- \
  "$UE_CMD" "$BR_UPROJECT" -run=pythonscript -script="$BR_REPO/tools/ue/$SCRIPT" \
  -unattended -nosplash -nullrhi -nopause -nocrashreports -NoSound -notraceserver -stdout -FullStdOutLogOutput > "$LOG" 2>&1
RC=$?
br_reap_crash_reporters
grep -E 'BR_PROBE' "$LOG" | tail -5
[ $RC -eq 0 ] && ! grep -q 'BR_PROBE_DONE OK' "$LOG" && RC=3
exit $RC
