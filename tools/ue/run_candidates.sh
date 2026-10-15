#!/bin/bash
# OWNER: builder-B5 (shot)
# THE SHOT candidate set: for each lighting variant, apply it to the map (run_lighting.sh), render the four fixed
# vantages of shots_m5.json at 3840x2160 (run_shots.sh) and write the settings sidecars.
#
#   tools/ue/run_candidates.sh [--variants A_classic_gold,B_raking_amber,C_rose_haze] [--out DIR]
#
# Output: DIR/<variant>/NN_<shot>.png + .json (default DIR ~/bully-remake-evidence/m5/candidates).
# Sequential, one engine at a time, no retries (a failed variant stops the run).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VARIANTS=""
OUT="$HOME/bully-remake-evidence/m5/candidates"
while [ $# -gt 0 ]; do
  case "$1" in
    --variants) VARIANTS="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
[ -n "$VARIANTS" ] || VARIANTS=$(python3 -c "import json;print(','.join(json.load(open('$HERE/lighting_variants.json'))['variants']))")
for V in ${VARIANTS//,/ }; do
  echo "== variant $V"
  "$HERE/run_lighting.sh" --variant "$V" || { echo "lighting failed for $V" >&2; exit 1; }
  mkdir -p "$OUT/$V"
  "$HERE/run_shots.sh" --shots "$HERE/shots_m5.json" --out "$OUT/$V" || { echo "shots failed for $V" >&2; exit 1; }
  python3 "$HERE/shot_sidecar.py" --variant "$V" --shots "$HERE/shots_m5.json" --result "$OUT/$V/shots_result.json" --out "$OUT/$V" || exit 1
done
# leave the map on the default variant (idempotent state)
"$HERE/run_lighting.sh" >/dev/null || true
echo "candidates in $OUT"
