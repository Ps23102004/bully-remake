# OWNER: builder-B (ue)
# Shared engine-safety helpers for tools/ue/*.sh (sourced, not executed).
# Owner rules: every Unreal launch goes through the shared GPU slot (cap 2), never a visible window,
# never auto-relaunch, SIGTERM -> wait 60 s -> SIGKILL only as a last resort.

BR_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BR_UPROJECT="$BR_REPO/unreal/BullyRemake/BullyRemake.uproject"
UE_ROOT="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
UE_CMD="$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd"
GPU_SLOT="${GPU_SLOT:-$HOME/.cache/gpu-slot/bin/gpu_slot.sh}"

br_die() { echo "error: $*" >&2; exit 2; }

br_preflight() {
  [ -x "$UE_CMD" ] || br_die "UnrealEditor-Cmd not found at $UE_CMD"
  [ -x "$GPU_SLOT" ] || br_die "gpu slot wrapper not found at $GPU_SLOT (engine launches must go through it)"
  # never launch while any Unreal process is stuck exiting (E) or zombie (Z)
  if ps -axo stat=,comm= | grep -E '^[[:space:]]*[EZ]' | grep -qi 'unreal'; then
    ps -axo pid=,stat=,comm= | grep -i unreal >&2
    br_die "an Unreal process is stuck exiting; not launching"
  fi
  # a pending OS dialog means nobody can answer another one: refuse if either dialog agent has a window
  local dpids
  dpids=$(pgrep -x UserNotificationCenter; pgrep -x CoreServicesUIAgent) || true
  if [ -n "$dpids" ] && [ -n "$("$(br_winlist)" $dpids)" ]; then
    "$(br_winlist)" $dpids >&2
    br_die "a system dialog is on screen (UserNotificationCenter/CoreServicesUIAgent); not launching"
  fi
}

# Path of the compiled window lister (built once from tools/ue/winlist.swift into Saved/).
br_winlist() {
  local bin="$BR_REPO/unreal/BullyRemake/Saved/BullyTools/winlist"
  local src="$BR_REPO/tools/ue/winlist.swift"
  if [ ! -x "$bin" ] || [ "$src" -nt "$bin" ]; then
    mkdir -p "$(dirname "$bin")"
    swiftc -O "$src" -o "$bin" >/dev/null 2>&1 || { echo "/usr/bin/false"; return; }
  fi
  echo "$bin"
}

# Background watchdog: while PID lives, record any on-screen window owned by it (or its children).
# Writes one line per sighting to FILE; an empty FILE at the end is the "no window" evidence.
br_window_watch() {
  local pid="$1" file="$2" wl
  wl="$(br_winlist)"
  : > "$file"
  while kill -0 "$pid" 2>/dev/null; do
    local pids="$pid $(pgrep -P "$pid" | tr '\n' ' ')"
    local hit
    hit=$("$wl" $pids)
    if [ -n "$hit" ]; then
      echo "$(date +%T) $hit" >> "$file"
      # owner rule: no visible engine window, ever. Stop the engine (SIGTERM; caller's wait handles the rest).
      echo "$(date +%T) window seen: sending SIGTERM to $pids" >> "$file"
      kill -TERM $(pgrep -P "$pid") 2>/dev/null
    fi
    sleep 1
  done
}

# Kill CrashReportClient instances that belong to this project (owner rule: no orphaned reporters).
br_reap_crash_reporters() {
  local pids
  pids=$(pgrep -f '[C]rashReportClient.*BullyRemake' || true)
  if [ -n "$pids" ]; then
    echo "reaping CrashReportClient: $pids" >&2
    kill -TERM $pids 2>/dev/null || true
  fi
}
