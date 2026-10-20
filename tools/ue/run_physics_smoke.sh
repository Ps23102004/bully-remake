#!/bin/bash
# OWNER: lane-C (engine)
# Headless physics / combat-on-students / door smoke (-BRPhysicsSmoke, UBullyPhysicsSmoke): real ambient students as
# targets, ragdoll launch + settle + get-up, KO body stays down, vehicle-style throw, player ragdoll, instant door cut
# (no fade) + a requested fade still works. -nullrhi, no window, no audio; the smoke quits the engine itself.
#   tools/ue/run_physics_smoke.sh [--out DIR]     (result: DIR/physics_result.json, BR_PHYS lines in DIR/summary.log)
set -uo pipefail
OUT="$HOME/bully-remake-evidence/physics"
[ "${1:-}" = "--out" ] && OUT="$2"
exec "$(dirname "$0")/run_smoke_nullrhi.sh" --flag "-BRPhysicsSmoke=$OUT" --ambient --out "$OUT" --label bully-physics --grep 'BR_PHYS|BR_RAGDOLL|BR_DOOR|BR_COMBAT ai|BR_AMBIENT BRAWL' --seconds-limit 420
