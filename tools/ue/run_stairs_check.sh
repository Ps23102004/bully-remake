#!/bin/bash
# OWNER: slice-stairs
# One-shot headless regression for the opening slice's walk route (no GPU, no window, GPU-slot wrapped, no retries):
#   1. capsule-sweep flood  lobby door exit -> stairs -> landing -> upper floor -> principal's office point  (stairs_probe.py)
#   2. capsule-sweep flood  front gates -> main building door                                              (ground floor)
#   3. the REAL pawn (walk runner, real CharacterMovement) gates -> door link -> lobby -> office -> back out (walk_stairs_full.json)
# Logs `BR_STAIRS path lobby->office: PASS|FAIL`, `BR_STAIRS path gates->door: ...`, `BR_STAIRS pawn full route: ...`.
#   tools/ue/run_stairs_check.sh [--out DIR] [--tag NAME]     (default ~/bully-remake-evidence/stairs, tag "check")
# Regenerate the pawn route after a level change:
#   BR_STAIRS_TAG=x tools/ue/run_stairs_probe.sh && python3 tools/ue/gen_stairs_walk.py DIR/x_lobby_to_office.json tools/ue/walk_stairs_full.json --full
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HOME/bully-remake-evidence/stairs"
TAG=check
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --tag) TAG="$2"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
mkdir -p "$OUT"
RC=0
CHECKS="PrincipalRoom:-63000,28450,550;SecretaryInOffice:-63270,28630,550;LandingCentral:-62860,30000,165;EastFlightTop:-62300,29925,500;FirstSetLobby:-62600,32370,0"

echo "== 1. lobby -> office (interior flood)"
BR_STAIRS_TAG="$TAG" BR_STAIRS_CHECK="$CHECKS" "$HERE/run_stairs_probe.sh" --out "$OUT" | tee "$OUT/${TAG}_lobby_to_office.log" | grep -E "BR_STAIRS (path|check|probe)|reached|engine exit" | sed -E 's/^LogPython: //'
grep -q "path lobby->office: PASS" "$OUT/${TAG}_lobby_to_office.log" || RC=1

echo "== 2. gates -> door (ground-floor flood)"
BR_STAIRS_LABEL="gates->door" BR_STAIRS_START="32200,7300,500" BR_STAIRS_TARGET="21320,7330,864" BR_STAIRS_BOX="19000,3000,33500,12500" \
  BR_STAIRS_STEP=50 BR_STAIRS_TAG="$TAG" "$HERE/run_stairs_probe.sh" --out "$OUT" | tee "$OUT/${TAG}_gates_to_door.log" | grep -E "BR_STAIRS (path|probe)|reached|engine exit" | sed -E 's/^LogPython: //'
grep -q "path gates->door: PASS" "$OUT/${TAG}_gates_to_door.log" || RC=1

echo "== 3. real pawn, whole route and back"
BR_TAIL=400 "$HERE/run_walk_nullrhi.sh" --script "$HERE/walk_stairs_full.json" --out "$OUT/${TAG}_pawn" --label bully-stairs > "$OUT/${TAG}_pawn.log" 2>&1
if grep -q "script complete, 0 failures" "$OUT/${TAG}_pawn.log" && grep -q "log 'office_arrived'" "$OUT/${TAG}_pawn.log" && grep -q "log 'back_outside'" "$OUT/${TAG}_pawn.log"; then
  echo "BR_STAIRS pawn full route (gates -> lobby -> office -> out): PASS"
else
  echo "BR_STAIRS pawn full route: FAIL"; grep -E "STUCK|TIMEOUT|failures" "$OUT/${TAG}_pawn.log" | tail -5; RC=1
fi
grep -E "office_arrived" "$OUT/${TAG}_pawn.log" | sed -E 's/.*BR_WALK/  /'

# zoomed ASCII of the stair block (UE x -64400..-61400, y 29000..31200)
python3 - "$OUT/${TAG}_lobby_to_office_trace.txt" "$OUT/${TAG}_stairs_zoom.txt" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
try:
    lines = open(src).read().split("\n")
    m = re.search(r"cols = UE Y from (-?\d+) step (\d+)", lines[0]); y0, st = int(m.group(1)), int(m.group(2))
    a, b = (29000 - y0) // st, (31200 - y0) // st
    out = ["# stair block, rows UE X (north = more negative X), cols UE Y 29000..31200 step %d; char = floor z in 50 cm bands over the lowest z reached "
           "(3 = ground floor, 6 = landing at z~165, E = upper floor z~550); S/* only in the full trace" % st, lines[1]]
    for ln in lines[2:]:
        mm = re.match(r"x=\s*(-?\d+) \|(.*)\|", ln)
        if mm and -64400 <= int(mm.group(1)) <= -61400:
            out.append("x=%6d |%s|" % (int(mm.group(1)), mm.group(2)[a:b]))
    open(dst, "w").write("\n".join(out) + "\n")
except Exception as e:
    print("zoom skipped:", e)
PY
echo "BR_STAIRS summary: $([ $RC -eq 0 ] && echo ALL PASS || echo FAIL) (out $OUT, tag $TAG)"
exit $RC
