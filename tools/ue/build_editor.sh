#!/bin/bash
# OWNER: lane-C (engine)
# Rebuilds the project's C++ module with UE 5.8's Build.sh (BullyRemakeEditor Mac Development = the module the launcher's
# `UnrealEditor -game` and every headless smoke load) and verifies the result:
#   1. a running game (pgrep 'UnrealEditor.*Bullworth', e.g. /Applications/Bully Remake.app) is quit gracefully first
#      (SIGTERM, wait 60 s, never SIGKILL): the rebuild replaces the dylib under it;
#   2. the build; on a PCH error that names a foreign engine path ("malformed or corrupted precompiled file ... /Volumes/...")
#      this project's own Intermediate/Build/Mac/arm64/BullyRemakeEditor is cleared (regenerable, git-ignored) and the build
#      is retried ONCE;
#   3. HANDOFF "Build gotcha": the manifest Binaries/Mac/UnrealEditor.modules must name a dylib that exists and is the newest
#      libUnrealEditor-BullyRemake*.dylib, else the engine would load a stale module; otherwise the numbered dylibs + manifest
#      are removed and the build re-run to get a coherent pair;
#   4. optional symbol check:  tools/ue/build_editor.sh UBullyRagdollComponent UBullyWeaponComponent ...  (nm | grep each).
#
#   tools/ue/build_editor.sh [Symbol ...]            log: unreal/BullyRemake/Saved/BullyTools/build_editor.log
set -uo pipefail
source "$(dirname "$0")/_common.sh"
BUILD="$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh"
[ -x "$BUILD" ] || br_die "Build.sh not found at $BUILD"
BIN="$BR_REPO/unreal/BullyRemake/Binaries/Mac"
LOG="$BR_REPO/unreal/BullyRemake/Saved/BullyTools/build_editor.log"
mkdir -p "$(dirname "$LOG")"

PIDS=$(pgrep -f 'UnrealEditor.*Bullworth' || true)
if [ -n "$PIDS" ]; then
  echo "a Bully game is running ($PIDS): quitting it gracefully (SIGTERM, up to 60 s)"
  kill -TERM $PIDS 2>/dev/null
  for _ in $(seq 60); do pgrep -f 'UnrealEditor.*Bullworth' >/dev/null || break; sleep 1; done
  pgrep -f 'UnrealEditor.*Bullworth' >/dev/null && br_die "the game did not exit after SIGTERM + 60 s; not building (no SIGKILL by policy)"
fi

run_build() { "$BUILD" BullyRemakeEditor Mac Development -project="$BR_UPROJECT" -WaitMutex > "$LOG" 2>&1; }
run_build; RC=$?
if [ $RC -ne 0 ] && grep -q "malformed or corrupted precompiled file" "$LOG"; then
  echo "stale PCH from another engine path: clearing this project's BullyRemakeEditor intermediates and retrying once"
  rm -rf "$BR_REPO/unreal/BullyRemake/Intermediate/Build/Mac/arm64/BullyRemakeEditor"
  run_build; RC=$?
fi
grep -E "error:|Result:|Total execution time" "$LOG" | head -20
[ $RC -eq 0 ] || br_die "build failed (see $LOG)"

manifest_ok() {
  local named newest
  named=$(sed -n 's/.*"BullyRemake": *"\(.*\)".*/\1/p' "$BIN/UnrealEditor.modules")
  newest=$(ls -t "$BIN"/libUnrealEditor-BullyRemake*.dylib 2>/dev/null | head -1)
  [ -n "$named" ] && [ -f "$BIN/$named" ] && [ "$BIN/$named" = "$newest" ]
}
if ! manifest_ok; then
  echo "manifest does not name the newest dylib (HANDOFF 'Build gotcha'): regenerating the pair"
  rm -f "$BIN"/libUnrealEditor-BullyRemake*.dylib "$BIN/UnrealEditor.modules"
  run_build || br_die "rebuild failed (see $LOG)"
  manifest_ok || br_die "manifest still incoherent after a clean rebuild"
fi
DYLIB="$BIN/$(sed -n 's/.*"BullyRemake": *"\(.*\)".*/\1/p' "$BIN/UnrealEditor.modules")"
echo "module: $DYLIB ($(stat -f %z "$DYLIB") bytes, $(stat -f %Sm "$DYLIB"))"
for SYM in "$@"; do
  N=$(nm "$DYLIB" | grep -c "$SYM")
  echo "symbol $SYM: $N match(es)"
  [ "$N" -gt 0 ] || br_die "symbol $SYM missing from $DYLIB"
done
echo "build OK"
