# Plan: Bully Anniversary Edition remake in UE 5.8

Goal: a faithful, enhanced remake built from the owner's own Anniversary Edition (iOS/Mac) data. Game content never enters git.

## Working method (keeps sessions small)

- One branch per lane, merged to `main` only after its checks pass. Branch names: `ae-importers` (done), `ae-world-ue`, `ae-engine`, `ae-story-vm`, `ae-npc-ai`.
- One session per lane. Each session starts by reading this file and `HANDOFF.md`, finishes by committing on its branch and updating the status table below. No session carries another lane's context.
- Only one Unreal process at a time. Rendered captures use `-RenderOffScreen` (no window), which is why a long Unreal job shows no app window.

## Status

| Lane | Branch | State |
|---|---|---|
| AE importers: models, textures, archive, peds/animals, grass | `ae-importers` | merged to main, pushed to fork |
| Interiors two-sided + COL collision, town/all presets, 113 door links, instant door cut, ragdoll physics | `stage2-world` | built, unverified in the game; stage 2 verifying |
| True .mtl materials + game text (Wx cipher decoded) | `stage2-world` | in progress |
| Full-world UE import (town + interiors + collision + doors) and render/smoke verify | `stage2-world` | in progress |
| Story runtime (original Lua 5.0 scripts on an embedded VM) | `ae-story-vm` | not started; needs owner OK for vendored Lua 5.0.3 (MIT) |
| NPC AI: schedules, factions, reactions, nav from `walkable_*` COL surfaces | `ae-npc-ai` | not started |
| Mission/cutscene/audio systems, minigames, shops | `ae-story-vm` | see roadmap in `re-notes/ae-story.md` section 8 |

## Order

1. Finish and verify `stage2-world` (walls, collision, town, doors, no black frame, ragdoll). Merge.
2. Audio: decode `.ses`/`.snd`, wire footsteps and ambience (no sound at all today).
3. `ae-story-vm`: roadmap items 1-8 in `re-notes/ae-story.md` (chapter 1 playable with cutscenes after about 25 sessions).
4. `ae-npc-ai` in parallel with step 3 once the VM lands (needs the native ped functions).
5. Vehicles and weapons polish, shops/clothing, minigames (roadmap items 11-13).
6. Lighting retune and hardware ray tracing once interiors are stable.

## Known gaps (honest)

- Sea is not geometry in the AE data; water plane is approximated.
- 126 models are empty stubs in the AE files (decals, glow blooms, stop signs, wind turbines).
- Action-tree runtime (`act.img`) is the largest technical risk for faithful fights and scenes.
- Original voices, music and cutscene animation are reused from the owner's install at runtime; a shareable build would need replacements.
