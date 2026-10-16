# HANDOFF — bully-remake

**Updated:** 2026-10-15 · **Branch:** `phase0` (checkout `~/bully-remake`) · **Machine:** midirstudio (M3 Ultra Mac Studio)

## Verified state

- Repo `oh-ashen-one/bully-remake` created public 2026-10-15, seeded on `phase0` (README, CREDITS, settings, CI blocklist, LICENSE).
- **Game install verified:** *Bully: Scholarship Edition* (PC, Steam via CrossOver), 5.0 GB, complete data tree at:
  `/Users/midir/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Bully Scholarship Edition`
  Key dirs: `Act/ Anim/ audio/ Coll/ Cuts/ DAT/ Data/ Graphics/ Models/ Objects/ Scripts/ Stream/ TXD/ ShaderBinaries/` plus `Bully.exe` (binary for Ghidra RE).
- Kickoff brief: `/Users/midir/Downloads/BULLY-KICKOFF-README.md` (planning session 2026-10-15).

## Phase 0 milestones (status)

1. Assets in — **DONE** (merged e1fbf74). `tools/importers world` (25 s) → `tools/ue/run_import.sh` (headless Interchange import, ~6 min cold / 25 s warm, idempotent) → `/Game/Imported/Maps/Bullworth` (2,553 placements, 8 master materials, Lumen rig). Evidence: 4×4K Lumen shots in `~/bully-remake-evidence/m1/` (front gates = the milestone shot). Known visual debts: lobby overexposed (sun leaks into interior pocket), fake-mirror floor doubles reflections, gloss/specular mapping guessed, normal-map green channel unverified. Interiors render in distant pockets and there is a mirrored under-ground copy — both match the original game's own tricks, not import bugs.
2. Interior in — **DONE** (merged): complex-as-simple collision, `BullyPawn`+`BullyGameMode` (placeholder tuning in tools/ue/README.md), `ABullyDoorLink` fade-teleport pairs from `tools/ue/door_links.json` (main east door ↔ lobby vestibule), scripted walk gates→lobby→back with 0 collision holes on the route (evidence `~/bully-remake-evidence/m2/`). Known: central-stairs landing stop (1 flagged failure), upper floors untested, interiors overexposed.
3. Jimmy moves — asset side **DONE** (merged): NIF skinning (all 5,724 models), HXD/AGR fully decoded (4,374 clips; `python3 -m tools.importers ped PLAYER --anims '*'`), measured speeds walk 2.05 / run 3.61 / sprint 8.01 m/s (docs/re-notes/anim-formats.md + locomotion-spec.md; rest pose faces backwards vs clips; Jimmy bald — hair mesh OPEN). UE side **DONE** (merged): `SK_Jimmy` + 439 clips via `tools/ue/run_import_character.sh`; `ABullyCharacter` + C++ `UBullyAnimInstance` speed blend; measured speeds applied, leg-speed synced to planted-foot speeds (sprint clip slides 2.9 m/s even in original data); camera eyeballed (4.5 m back, FOV 75, tunable via DefaultGame.ini). Evidence `~/bully-remake-evidence/m3/` incl. sequence.mp4. Glitches: residual sprint slide, fall-cam lag, no start/stop/turn anims, Jimmy bald (hair mesh OPEN).
4. Signs of life — **DONE** (merged): 6 student peds (shared C_Player clip bank — faction clip groups unchecked), `AAmbientWalker` × 9 on 6 loops (`Config/AmbientLoops.json`), capsule-swept paths, audio formats fully decoded (.SEA = XACT sound+wave bank, MS-ADPCM, ffmpeg-verified; docs/re-notes/audio-formats.md), footsteps + courtyard ambience wired (cement set only; cue graph undecoded). Evidence `~/bully-remake-evidence/m4/`. Gaps: walkers/player no mutual collision, no start/stop/turn anims.
5. The shot — **DONE, critic-accepted** after 3 rounds + camera iterations (CRITIQUE_R1/R2/R3/R3B/FINAL.md in `~/bully-remake-evidence/m5/`). Accepted: variant **B_raking_amber**, gates camera (32600, 7300, 625) → look-at (18600, 7300, 3400), FOV 64 (in `tools/ue/shots_m5.json`). Accepted frame: `~/bully-remake-evidence/m5/round3-try6/00_front_gates.png`. Cosmetic-only residuals: 2 grass blades at bottom (cull foliage near camera later), facade/path skies flatter than gates.
6. Proof capture — **our-build half DONE**: 90 s continuous 1080p30 run (gates→courtyard→jump→main door→lobby→out), walkers active, `~/bully-remake-evidence/m6/proof.mp4` + GLITCHES.md (student clips through camera ~11 s, black frame at each door cut, blown lobby-floor window reflections, flat grey backdrop through exit doorway). Fixed-timestep capture — NOT real-time perf evidence. ORIGINAL-game half pending: needs owner-attended CrossOver/Steam launch (dialog risk); drive the identical route per `docs/re-notes/proof-route.md`.

## polish1 (branch `polish1`, worktree `~/bully-remake-wt/polish`; pushed, NOT merged)

CPU-only polish while the owner played (no renderer launched; `-nullrhi` logic runs only). Details: `tools/ue/README.md` "Polish 1".

1. Player vs students: walkers are Pawn capsules (solid for the player), pass through each other, yield to the player. Headless: head-on contact on 3 loops, closest centre distance 60.0 cm (= capsules touching), 3 walker yields each, 0 STUCK in 120 s.
2. Door fade: frame-driven FadeOut -> black frame -> teleport -> settle -> FadeIn (BR_DOOR / -BRDoorTrace evidence); entrance backdrop (`DL_iSchoolEnt4`) self-lit, lobby floor glints toned (both estimates in `lighting_variants.json`, applied to the saved map with `run_lighting.sh`).
3. Hair: Jimmy's start haircut is the clothing head `B_Buzz` (painted-hair crop, reads nearly bald); `ped PLAYER --head B_Buzz` wires it; any HAIR row of Cloths.dat works (`docs/re-notes/ped-heads-hair.md`, contact sheet `~/bully-remake-evidence/polish1/hair_styles.png`).
4. Audio: XACT SDBK cue graph decoded (all banks), per-surface/role/gait footsteps with the cue pitch (`docs/re-notes/audio-formats.md` section 6).
5. Not done: grass blades near the hero camera are triangles inside the 134 m `DL_Grass02` ground mesh (needs a mesh split or a material near-fade); the floor/backdrop numbers need a by-eye check.

Merge-day checklist (rendering session, any time after merge): re-run the importers for the new head (`python3 -m tools.importers ped PLAYER --anims '*' --root-motion inplace --head B_Buzz`; `audio extract FeetAll FeetSCHL`; `audio footsteps`), then `tools/ue/run_import_character.sh`, `tools/ue/run_import_ambient.sh --steps audio`, `tools/ue/run_lighting.sh --variant B_raking_amber`; then `tools/ue/walk_polish_verify.json` (BR_EXTRA_ARGS=-BRDoorTrace tools/ue/run_walk.sh ...) and listen to footsteps on grass / path / lobby tile.

## Phase 0: COMPLETE (owner review pending)

All six milestones done with critic acceptance on the shot. Draft PR #1 open on `phase0` — owner decides the merge.

**Owner playtest:** `cd ~/bully-remake && export BULLY_INSTALL=<install path above> && tools/ue/run_import.sh` is already materialized here; open `unreal/BullyRemake/BullyRemake.uproject` in UE 5.8 and PIE the Bullworth map (boots into it), or watch `~/bully-remake-evidence/m6/proof.mp4` first. WASD+mouse / gamepad; Shift sprint; the main-door transition works both ways.

**Owner decisions pending:**
1. Merge `phase0` → `main` (PR #1)?
2. A session to capture ORIGINAL-game footage (CrossOver launch raises Steam/firewall dialogs — needs you at the desk once; ~20 min following `docs/re-notes/proof-route.md`), unlocking the side-by-side cut for episode 1.
3. Keep or trim `~/bully-remake-evidence/` (episode material: m1 gates 4K, m5 FINAL_front_gates.png + critique trail, m6 proof.mp4).

**For future sessions:** reference clones at `~/bully-remake-refs/` (re-clonable, disposable); UE Content/Imported + DDC left warm in this checkout for the owner's playtest (regenerable via run_import.sh, ~6 min).

## Rules in force

- `phase0` branch only; draft PR; **no merge to main without owner**.
- `imported/` is git-ignored and CI-blocked; game content never enters the repo.
- GPU slot protocol `~/.cache/gpu-slot`; cap 2 renderer-bearing engines; Unreal work routes through the `unreal-engine` brain skill; Epic native MCP only on a session-owned editor instance.

## Next steps

- Merge `phase0-ue` when the UE import + level + screenshots land; review screenshots.
- Milestone 2 (doors) + 3 (Jimmy): locomotion numbers live in Act.img action trees (see `docs/re-notes/locomotion-spec.md` — CAT_TO_MACT decode verified; param-name mapping OPEN).
- Original-game reference footage (milestones 3/5/6): Bully runs via CrossOver; capture needs a desktop-visible window + GPU slot — schedule when owner is not using the machine/streaming.

## Blockers / notes

- Ghidra is NOT installed on this machine (kickoff assumed a Ghidra MCP). When needed (anim formats, locomotion params, loader confirmation): `brew install --cask ghidra` and prefer headless `analyzeHeadless` scripts — no listener, no macOS dialogs. Not yet installed; not yet needed.
- Evidence convention: disposable captures → `~/bully-remake-evidence/<milestone>/` (outside repo); episode-worthy material noted here before cleanup.

## Polish pass (2026-10-15, merged to main)

Post-Phase-0 polish, verified + merged (owner granted standing merge authority for this repo):
- **Student collision**: walkers solid to player, yield+sidestep (60 cm closest, 0 stuck/120 s); walkers still pass through each other (blocking deadlocked them).
- **Door fade**: frame-driven, teleport at full black — the one-black-frame cut is gone. Exit-doorway backdrop (`DL_iSchoolEnt4`) made self-lit (gain 5000); verified render reads as exterior trees/sky, not flat grey.
- **Jimmy's hair**: his canonical `B_Buzz` head model is now imported (`ped PLAYER --head B_Buzz`); reads as a buzz-cut (near-bald by design — faithful). Other cuts one flag away; sheet in evidence.
- **Footsteps**: full cue graph decoded (11,075 cues); per-surface + per-gait via `Config/FootstepSurfaces.json`. **OPEN — needs owner by-ear check**: cue-graph pitch drops steps ~1 octave vs M4; tunable if wrong.
- **Lobby floor**: glints toned (roughness 0.45/spec 0.35); verified clean.
- Deferred: hero-shot grass blades are baked into the ground mesh, not cullable foliage.

Verify evidence: `~/bully-remake-evidence/polish1/` (render_verify/, logic logs, hair sheet). Merge-day checklist items that still want eyes/ears in a live playtest: footstep pitch, backdrop/floor brightness feel.

## Phase 1 — "Day One" opening slice (in progress, 2026-10-15)

Goal: ~10-min playable remaster of the game's opening (missions 1_01 + 1_02) to prove UE5 remastering. Recon: `docs/re-notes/opening-slice-recon.md`.

**Built + integrated on phase0 (four-system build links green; headless-verified):**
- Trigger.img importer → mission points/paths/volumes as JSON (`day_one.json`).
- Mission/objective framework + C++ HUD (`UMissionSubsystem`, `AMissionTrigger`, `BullyHUD`: objective, waypoint, E-prompt, banner). Data-driven missions in `Config/Missions/`.
- Melee combat (`BullyCombat*`): combos, block, grapple, ground kick, lock-on, enemy AI, KO; fires mission events. FEEL unjudged — needs owner hands-playtest; knobs in `docs/re-notes/combat.md`.
- Scripted NPCs (`BullyScriptedNPC`, `UScriptedNPCSubsystem`): gauntlet block/shove + fight crowd, mission-cue driven.
- Upstairs office reachable (Phase-0 "landing stop" was an old walk-script bug, not the level).
- Missions 1_01 (6 stages) + 1_02 (20 stages) authored (original objective wording); bully peds GN_Bully01-05 imported. Boot with `-BRMission=day_one` (or `1_01`). Headless driver run: 26/26 stages PASS incl. fail/retry.

**NOT done — 5 C++ framework gaps blocking seamless end-to-end play (data can't bridge; need small Source additions). Flagged, NOT fixed, per owner "flag real breaks, don't auto-fix":**
1. No data auto-start (only `-BRMission` cmdline — fine for launching).
2. No `start_mission` action → 1_01 doesn't auto-chain into 1_02 (headless driver issued it).
3. Mission can't spawn the fight opponent / set invuln (`Combat.SpawnEnemy`/`Invulnerable` console-only) → **the fight won't start on its own in a real playthrough.**
4. No fade action / no dorm `BullyDoorLink` → epilogue uses teleport + banner (rough).
5. Mission loader ignores trigger-file layout (points inlined — ok); string `blip` misparsed by `TryGetBoolField` → **mission waypoint blips dropped** (HUD regression for missions).

**Also pending:** combat juice (hit SFX/effects/camera shake — deferred to owner's feel-tuning pass); Jimmy's start outfit; a VISUAL verification render (never done for the mission HUD/combat/gauntlet — logs only so far). Best done together with closing the 5 gaps.

**Next session (on owner's go):** close the 5 C++ gaps → materialize content + visual render → rebuild + relaunch for owner playtest + combat feel-tuning. Nothing of main merged; phase0 is the slice branch.
