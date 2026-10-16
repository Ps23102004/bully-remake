# Scripted NPCs (Day One slice): gauntlet 1_01 and fight crowd 1_02

Status: 2026-10-15. Code: `unreal/BullyRemake/Source/BullyRemake/BullyScriptedNPC*.{h,cpp}` (+ small extensions in `AmbientWalker`, `AmbientSubsystem`). Data: `unreal/BullyRemake/Config/NPCScripts/*.json`. Beats are from `opening-slice-recon.md` (A2 gauntlet, B5-B8 fight). This is event-driven scripting only: no combat AI, no free-roam wandering.

## What it is

- `ABullyScriptedNPC` extends `AAmbientWalker` (same capsule, movement, locomotion anim instance, mesh/clip folder convention, real collision, solid to the player, footsteps via `UAmbientSubsystem::AttachFootsteps`). It takes commands from a queue instead of looping.
- `UScriptedNPCSubsystem` (world subsystem, Bullworth map) loads every `Config/NPCScripts/*.json` (a leading `_` = only when named with `-BRNPCScript=` / `SNPC.Load`), spawns groups on cues, turns beats into NPC commands, listens to the mission subsystem and emits mission events.

## Command set (C++ API on the NPC; JSON `cmd` in a beat step)

| C++ | JSON `cmd` | Notes |
| --- | --- | --- |
| `GoToPoint` | `goto` (`to`, `gait`, `radius_cm`) | straight-line walk with capsule collision; stuck -> side-step -> snap to goal (`BR_SNPC STUCK`); logs a `path-check` of the leg at start |
| `FollowPath` | `follow_path` (`path`) | list of legs (a trigger-file path name, `{ "path": name }`, or inline points) |
| `FaceActor` / `FacePlayer` / `FaceLocation` | `face` (`target`, `track`) | `track` keeps facing it after the command (standing order) |
| `PlayGesture` | `gesture` (`gesture` alias or `clip`, `loop`, `seconds`, `rate`) | single-node clip from the ped's own `Anims/`; `loop` with `seconds 0` starts the loop and moves on; a missing clip is logged and runs on a time budget |
| `BlockPath` | `block` (`at`, `until`, `shove`, `guard`, ...) | hold a post in Jimmy's way until a condition, taunt, shove, then step aside (below) |
| `Disperse` | `disperse` (`from`, `distance_cm`, `to`, `then`) | walk away from a focus (or along an explicit exit path), then `despawn` / `ambient` (hand back to the `AAmbientWalker` loop) / `stay` |
| `Wait`, `Despawn`, `MakeAmbient` | `wait`, `despawn`, `ambient` | |

`BlockPath` details: `until` (any): `shoves:N`, `timer:S` (since engaged), `timer_start:S`, `event:NAME`, `x<V` / `x>V` / `y<V` / `y>V` (player UE cm), `player_far:CM`. `shove`: `gesture`, `delay`, `interval`, `radius_cm`, `speed_cm_s`, `contact` (the push is `AddImpulse` on the player's movement component, horizontal, away from the NPC; logged with the measured result). `guard` = where Jimmy is heading: the NPC holds its post and shuffles across the post->guard axis to stay in front of him. `body_radius_cm` widens the capsule while blocking (a 30 cm capsule is trivially slid around at a run). No combat, nothing can fail.

## Data format (`bully-remake.npc-script/1`)

```
{ "trigger_file": "imported/triggers/day_one.json",     // named points / paths / triggers (importers `trigger`)
  "points":   { "name": [x,y,z] | [[..],[..]] | {"game_m":[..]} },   // inline, UE cm
  "gestures": { "taunt": ["P_TAUNT_01","P_TAUNT_02"] },               // alias -> candidate C_Player clips, rotated per NPC
  "groups": [ { "id", "event_prefix", "spawn_on": [cue], "despawn_on": [cue],
      "npcs":  [ { "id", "ped", "footsteps": "shoe|female", "spawn": <ref>, "face": <ref> } ],
      "beats": [ { "id", "on": [cue], "delay", "interrupt", "repeat", "emit": [event],
                   "steps": [ { "npc": "id,id|*", "cmd": ..., args..., "delay", "stagger", "jitter" } ] } ] } ] }
```

Location refs: `[x,y,z]`, `"player"`, `"point_name"`, `{ "point": n, "index": i | "$i" }` (`$i` = the NPC's ordinal in the group), `{ "game_m": [..] }`, `{ "npc": id }`, `{ "offset_cm": [..], "of": ref }`, `{ "ring": { "center": ref, "radius_cm", "start_deg", "arc_deg" } }` (one slot per NPC), `{ "between": [ref, ref], "t": 0.5 }`.

Cues (a beat's `on`, a group's `spawn_on` / `despawn_on`; `*` suffix = prefix match):

- `spawn` (group just spawned), `beat_done:ID`, `timer:S` (since spawn), `near:POINT[:RADIUS]` (player within, default 600 cm)
- `event:NAME`: any mission event. Mission triggers arrive as `event:trigger_enter:<id>`.
- `mission_start:<mission>`, `stage_begin:<mission>:<stageId>` / `stage_begin:<mission>:#<n>`, `mission_end:<mission>:success|fail`: synthesized from `UMissionSubsystem::OnStageBegan` / `OnMissionFinished`.

## Mission events

Consumed (names are data; these are the ones the shipped script uses): `npc_gauntlet_spawn`, `trigger_enter:1_01_BullyTrig`, `npc_gauntlet_ambient`, `npc_gauntlet_clear`, `npc_crowd_gather`, `enemy_knocked_out` (the combat builder's event; we depend only on the name), `npc_crowd_disperse`, `npc_crowd_clear`, plus `mission_start:1_01` / `mission_end:1_01:*` / `mission_end:1_02:*`.

Emitted through `UMissionSubsystem::FireEvent` (so a mission stage can wait on them with an `event` condition): `<prefix>_spawned`, `_engaged` (first), `_shove` (each), `_released` (first), `_despawned`, plus each beat's `emit`: `gauntlet_complete`, `gauntlet_ambient`, `crowd_gathered`, `crowd_cheering`, `crowd_dispersed`. Prefixes: `gauntlet`, `crowd`.

How a mission uses it: fire `npc_gauntlet_spawn` (or just be mission `1_01`: `mission_start:1_01` spawns the gauntlet) and optionally complete a stage on `gauntlet_complete`; fire `npc_crowd_gather` from the 1_02 fight setup; combat fires `enemy_knocked_out` and the crowd reacts and leaves.

## Console

`SNPC.Load <id>`, `SNPC.Cue <event | stage_begin:m:s>`, `SNPC.Spawn <group>`, `SNPC.Despawn [group]`, `SNPC.Status`, `SNPC.Do <npc> goto x y [z] | gesture <alias|CLIP> [loop] | face player | disperse [cm] | despawn`.

## Assets

Peds are the already-imported students (the data names `GN_Boy01`, `GN_Greekboy`, `GN_Boy02`, `GN_Asiangirl`, `NDH3_Bucky`). Gesture clips are `C_Player` clips (same bank the students' locomotion comes from); `tools/ue/run_import_npc.sh` adds the clips the scripts name to each ped's `Anims/` (30 per ped; `tools/ue/npc_plan.py` prints the plan). To use real bullies: `python3 -m tools.importers ped GN_Bully01 --anims ...`, import, change `ped` in the JSON.

## Verify (headless, no window)

`tools/ue/run_npc_smoke.sh [--phases stage,gauntlet,crowd|none] [--out DIR]` (`-game -nullrhi`, GPU-slot wrapped, window watchdog). The `-BRNPCSmoke` harness (`BullyScriptedNPCSmoke.cpp`) starts `smoke_day_one` (stage hooks), fires the cue events, walks the REAL player pawn through the gauntlet, fires `enemy_knocked_out` for the crowd, and logs `BR_SNPC_SMOKE PASS/FAIL` + `RESULT`. Console run: `BR_EXTRA_ARGS='-ExecCmds="SNPC.Cue npc_gauntlet_spawn,SNPC.Status"' tools/ue/run_npc_smoke.sh --phases none`.

## Needs a visual playtest (cannot be judged from logs)

- Gesture and shove look: the clips are Jimmy's `C_Player` set played on student skeletons; clip choice per alias (`gestures` table), the shove contact timing (`contact`), and whether the shove impulse reads as a shove.
- Foot slide while the blocker shuffles (it side-steps with the forward locomotion clip, no strafe animation) and the instant pose swap between locomotion and a gesture clip (no blend).
- Whether the invisible 40-50 cm blocking capsule feels fair; whether three bullies hold Jimmy long enough or too long (release: 2 shoves / 9 s engaged / past x=28300).
- Crowd ring spacing around the arena, the gate opening they enter and leave through, and that they do not stand in the sparring partner's way (fight geometry is combat's).
