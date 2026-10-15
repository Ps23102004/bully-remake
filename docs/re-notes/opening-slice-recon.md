# Opening slice recon: what the first ~10 minutes of Bully contain, and how to cut a playable slice

Status: 2026-10-15, read-only recon. Confidence tags as in `format-survey.md`: **VERIFIED** (read in the data or script), **INFERRED**, **OPEN**.

Sources (read-only, nothing copied): decompiled Lua `Bully-SE-scripts/scripts/chap1/{1_01,1_02,1_02A}.lua` (+ `SMissPass.lua`, `main.lua`, `SCloth.lua`, `SInitGl.lua`), the install's own text data `DAT/Trigger.img` (410 text `.dat` files: `1_01.dat`, `1_02.dat`, `1_02A.dat`, `BoysDorm.dat`, `BDorm_Doors.dat`, `tschool_doors.dat`, `ischool_doors.dat`), `Objects/default.ide` (ped model ids), `audio/PLAYLIST/Speech.lst`, `Cuts/Cuts.img` directory, and `bully-re:docs/trigger.md`. Our side: `HANDOFF.md`, `docs/re-notes/*`, `imported/` manifests, `tools/ue/door_links.json`. This document describes structure and logic in our own words; no dialogue, cutscene script or narrative text is reproduced. Script names are citations only.

Units: all coordinates are game metres (x, y, z) as in the data; UE cm = (100x, -100y, 100z). Area codes seen in the data: 0 = outdoors (`tschool`), 2 = `ischool`, 5 = `isc_offi`, 14 = `isc_dorm` (2/14/5 matched against IPB bounds; **INFERRED**, consistent in every point checked).

## 0. Headline

- The opening is three chained missions: **1_01** (arrive, walk to the office), **1_02** (briefing, cross campus, forced tutorial fight, prefect breaks it up), **1_02A** (boys' dorm: soda, bedroom, uniform). 1_01 + 1_02 is about 9-11 minutes at normal pace including cutscenes; 1_02A adds about 4. (`SMissPass.lua`, `main.lua`, `chap1/`; timing is my estimate from walk speeds 2.05/3.61 m/s in `anim-formats.md`, the script `Wait`s and path lengths; cutscene durations are not measured.)
- **Every location the slice touches is already imported**: `tschool` (gates, courtyard, main building, boys' dorm building), `ischool` (lobby, stairs, upper-floor office) and `isc_dorm`. No new world geometry is needed.
- The expensive part is not content, it is **systems**: objectives/HUD, a mission state machine, NPC scripting, and above all **melee combat**, because 1_02 is a seven-step guided fight tutorial.
- **The mission data is text and cheap to import.** `DAT/Trigger.img` holds every POINTLIST, PATH and TRIGGER the missions use (positions, area codes, yaw). One small importer (format: `bully-re:docs/trigger.md`) replaces hand-measuring, as was done for the two door links.

## 1. The opening beat by beat

Mission order is fixed: 1_01 passes, then 1_02 starts in the principal's outer office, then 1_02A starts at the boys' dorm (`SMissPass.lua` sets 1_02A as the first mission that unlocks the clothing manager; `Test/RobertoTest.lua` force-completes them in this order). How each mission is started from the world (trigger/blip in the mission table) is **OPEN**; 1_02 clearly begins on 1_01's success because it starts inside the office 1_01 ends in.

Clock is fixed at 08:30, weather clear, interactive music on, for all three.

### Mission 1_01: arrival (`chap1/1_01.lua`, data `1_01.dat`)

| # | Beat | Logic (functional) |
|---|---|---|
| A0 | **Cutscene 1-1-1** (first attempt only; assets `1-1-1_CHAR.agr`, `1-1-1_CAR.agr`) | Arrival at the front gates. After it, the open gate prop is swapped for a closed-gate geometry instance (front gate at about (301.4, -72.5) and a second at (225.9, 5.8)) so the player cannot go back out. Retries skip the cutscene. |
| A1 | **Start of control** | All peds cleared, player placed at point `1_01_Player_Start` (298.4, -72.2, z 6.07), fades in. Objective added (reach the principal's office) with a map blip on the end point. Two timed start-hint tutorial pop-ups (keys `TUT_START1A/B`; presumably movement) appear, then a short objective text. A watchdog thread shows a "check your objectives" hint after 60 s if the mission is still unfinished. |
| A2 | **Bully gauntlet** | Three bullies are posted talking in a knot about 20 m ahead (cheer points around x 277.8, y -72..-75). When the player enters trigger `1_01_BullyTrig` (286.9, -73.1, a 12 m walk from the start) they stop talking, face the player and walk to flanking points (`1_01_BegginingBullies`, `1_01_FightBullies` at x 286-292). Their attitude toward the player is switched to hostile-social: they shove and taunt, they do not run a scripted fight. The player may walk on or fight back; nothing in 1_01 fails or ends the mission for either. |
| A3 | **Chase set dressing** | On the first update a small student and a bully are spawned at `1_01_KidRun` (272, -55 -> 186, -118); both get infinite sprint, the bully is ordered to attack him one second later. They become ordinary ambient peds when the player enters `1_01_CHECKOBJS1` (234.3, -82.7). Pure ambience. |
| A4 | **Funnel locks** | Trigger `1_01_ParkDoor` (204.5, -3.7) closes and locks the parking gate; `1_01_NurseLock` locks the infirmary grounds door. They keep the player on the route. |
| A5 | **Cross the courtyard** | About 90 m from start to the main building's east door `DT_tschool_SchoolFrontDoorL` (trigger at 208.0, -73.2). On entry the visible area switches 0 -> 2 and ambient population is reverted to default. |
| A6 | **Inside to the office** | Lobby, then stairs, then the **upper floor** (the end point (-633.9, -292.4) and office trigger `1_01_PrincOffice` (-633.2, -291.4, z 4.86..8.86) are at z about 5.5; `SCH_OfficeMain` is at (-632.3, -286.7, z 7.6) in `ischool`). When area 2 becomes visible the secretary ped is created in the office (`1_01_SecretaryInOffice` (-632.7, -286.3)). |
| A7 | **Win** | Entering `1_01_PrincOffice`: control off, player turns to heading 0, waves to the secretary and a greeting voice line plays to the end, objective removed, mission succeeds. There is no lose condition in the live path. |

Dead code in the file (not reached from `main`): a bully cutscene function, a "stage two/three" chain, secretary path-following callbacks. Treat only A0-A7 as the real flow. (`stageFunction` chain: `F_StageOneSetup -> F_MonitorFight -> F_CheckFinish`.)

### Mission 1_02: briefing, the walk, the tutorial fight (`chap1/1_02.lua`, data `1_02.dat`)

| # | Beat | Logic (functional) |
|---|---|---|
| B0 | **Cutscene 1-1-2** (first attempt only; the biggest cutscene script of the opening, 22.5 KB) | Principal's office briefing. Then the player is placed at `PrincipalRoom` (-630.0, -284.5, z 5.5) with the secretary and a female student (Eunice) present. |
| B1 | **In-engine intro tour** | Control off, widescreen, three scripted camera paths (`1_02_1CAM_LOGO`, `2CAM_CAF`, `3CAM_HALL`, all in `ischool`, z -0.1..7.4) accompany four narrator voice lines, then a fade to the chapter title card (held while it displays). This one is **in-engine, not baked**: the camera path data is in `1_02.dat`. |
| B2 | **Secretary gives the task** | Two queued voice lines, then objective: go to the boys' dorm; a blip is set on the `InBedroom` point (area 14). The secretary walks off along a path. A tutorial message follows 1.5 s later. |
| B3 | **Lobby bullies** | When the player comes within 40 m of `1_02_FirstSet` ((-626..-631, -323.7), the lobby by the exit) two bullies spawn and stare the player down; when the player gets within about 2-6 m they turn hostile-social (shoves). |
| B4 | **Cross campus to the boys' dorm** | From door (208, -73) to the dorm front: about 70-75 m. At 40 m from `1_02_BDormBullies` (272, -113) three bullies are spawned asleep, posted in a group, facing the encounter spot. A thread watches them: **if the player hits any of them the bully cutscene fires early.** Otherwise the fight starts when the player enters `GaryTrigger` (271.4, -98.1) or `1_02_RusselTease` (270.6, -109.7). Population is suppressed in the dorm court (`1_02_PopulationTrig`). |
| B5 | **Bully cutscene (in-engine, ~10 s)** | Fade, ambient peds/vehicles cleared, weapons unequipped, HUD weapons locked, boys' dorm door locked, player moved to `1_02_PlayerNISLocs` (271.2, -105.9). Short taunt exchange from the group (two voice lines, skippable), fade out. Then the arena is set: four spectator bullies ring the player at `1_02_BulliesMid` (269-272, -101), a fifth bully (`GN_Bully05`) is the sparring partner at `PlayerNISLocs[2]`, a gate prop (`1_02Gate`) is spawned to close the arena, and the player's action tree is swapped to a **training tree**. |
| B6 | **Seven-step fight tutorial** | See the step table below. Player is **invulnerable** (health raised to at least 150), the sparring partner is tethered to the arena centre (radius 5) and runs a **tutorial AI** whose attack permission is a script flag. HUD parts (three components) are hidden; a button-sequence prompt widget shows each step's key; right/wrong key plays a feedback sound. Spectators cheer with random "watching a fight" voice barks while idle. **Fail:** player leaves `1_02_FightArea` (275.0, -105.2) during training. |
| B7 | **Free fight to a floor, then humiliate** | After step 7 all attack types are enabled (`bFinalStage`). The partner's health is floored at 5, so he cannot be killed. Objective: finish the fight. When his health reaches the floor: objective changes to "humiliate him" and a humiliation tutorial opens. Performing any of the pair-animation humiliation moves (four variants are named in the script: a burn, hitting-himself, a noogie-and-spit, and an emergency fallback) hands off to a lockout: wait until the victim is knocked out, 1.5 s, fade. |
| B8 | **Prefect scene (in-engine, ~25 s)** | Everything cleared, widescreen, FOV 20; player, a teacher/prefect (`TE_MathTeacher`), Russell and a bully gang at `1_02_NISLocations`/`NIS_Bullies`. Russell and the bullies run off (`NIS_BulliesFlee`, y -31), the teacher confronts the player, final player line, fade. Then **mission success**. |

Fail conditions for 1_02: leaving the fight area during training; the secretary (marked mission-critical) dying. Retry skips cutscene B0, restarts the mission at the start.

#### The 1_02 tutorial steps (move table; the script drives these through gating flags)

| Step | Required input (conceptual) | Gate that opens for the partner/player | Completion |
|---|---|---|---|
| 1 | Block | partner is allowed to attack | block a hit, twice; the second success advances |
| 2 | Light attack, three-button chain | light attacks on | pass the sequence twice |
| 3 | Grapple | grapples on | start a grapple (hold) |
| 4 | Grapple strikes (three presses) | grapple punches on | pass once |
| 5 | Grapple again | | grapple success state |
| 6 | Grapple push/throw to ground | | partner pushed |
| 7 | Ground kicks | ground attacks on | land a heavy ground kick |

Gate flags are read by the *action trees* through Lua condition functions: `F_PlayerGrapple`, `F_PlayerSloppyAttacks`, `F_PlayerHeavyAttacks`, `F_PlayerGroundKicks`, `F_GrapplePunches`, `F_FinalAttacks`, `F_CanBlock`, `F_CanBullyAttack`, `F_PlayerIsTraining`, `F_BulliesCanPush`, `F_HealthTutorial` (the names are present as strings in `Act/Act.img`; **INFERRED** that the trees call them as node conditions). The trees themselves (`Act/TrainingPlayer.act`, `Act/Anim/Fight_Tutorial.act`, `Act/AI/AI_FightTutorial.act`) are **not decoded**: we know the order and gating of the tutorial, not hit timing, combo windows, damage or AI cadence.

### Mission 1_02A: the boys' dorm (`chap1/1_02A.lua`, data `1_02A.dat`, `BoysDorm.dat`)

| # | Beat | Logic |
|---|---|---|
| C0 | **Cutscene 1-02B** | Dorm arrival. Player is placed at `AreaTransitionBoys` (-503.0, 318.0, z 31.4), area 14. |
| C1 | **Gary + soda machine** | A friendly ped (Gary, mission-critical) walks to a soda machine (`1_02A_PopMach` (-496.8, 322.6)). Objective + blip: use the machine. Player is given $1 if short. A tutorial popup explains the soda. Using the machine triggers a Gary reaction and he walks to the bedroom. |
| C2 | **Bedroom + clothing manager** | Objective: go to your room (blip `ClothesDorm`; trigger `PLAYER_ROOM` (-495.1, 311.4)). The wardrobe is unlocked with everything locked except the torso/legs choice. The player must put on the school uniform (sweater/pants/boots set defined in `SCloth.lua`, `SInitGl.lua`). |
| C3 | **Cutscene 1-02E, pass, save** | Fade, cutscene, mission passes, `schoolSave` flag set (first save point). |

Fail: hitting or knocking out Gary (dorm exit doors lock, fail text variants for "KO'd" vs "harassed"). The clothing manager (`clothing_m_trigger` (-490.9, 311.4), `SaveBookBoysDorm` (-491.5, 308.4)) is a large subsystem on its own.

## 2. Cutscene points (we replace all of them)

Mark only; do not port content. Durations of baked ones are **not measured** (`Cuts.img` has `.agr`/`.HXD`/`.inf`/`.SUB` per cutscene); budget 15-45 s each.

| Marker | Where in the flow | Type | Replace with |
|---|---|---|---|
| CS-1 | Before A1 (1-1-1) | baked | Fade in on Jimmy at the gates; optional short original establishing shot (~10 s) |
| CS-2 | Before B1 (1-1-2) | baked, long | Original 20-30 s office briefing (letterbox + VO + subtitle) or title-card text |
| CS-3 | B1 intro tour + chapter title | in-engine | Original: 3 camera paths + title card; paths are in `1_02.dat` (reference only) |
| CS-4 | B5 bully cutscene | in-engine | Short camera cut + fade; can fold into trigger fade |
| CS-5 | B8 prefect scene | in-engine | 10-15 s original sequence or fade + objective text |
| CS-6 | C0 / C3 (1-02B, 1-02E) | baked | Out of slice, stub with a fade |

Cutscene-specific animation groups (`NIS_1_02`, `NIS_1_02B`) have no `.agr` in World.img and are not needed.

## 3. Asset coverage

Legend: HAVE = already imported/built in the repo; NEW = needs importing or building. Imported scene names are from `imported/scenes/academy.json` (22 scenes, all in the level).

### World

| Beat | Needed | Status |
|---|---|---|
| A1-A5 | `tschool` exterior: front gates (`SC1b_MGates02/03` at (302.9, -72.5)), courtyard, main building east door | HAVE (milestones 1-2, walk route gates -> door verified) |
| A1/B0 | Closed-gate swap (`ScGate01Closed`, second gate (225.9, 5.8)) and parking gate close | NEW (blocking volume; which model is "closed" is **OPEN**; both `MGates02/03` are placed at the same spot) |
| A5/A6, B3 | `ischool` lobby, central stairs, upper floor, `SCH_OfficeMain` | HAVE import; **upper floor and stairs are untested for walking** (HANDOFF: central-stairs landing stop, 1 flagged failure) |
| A5 door pair | `main_door_ext` <-> `main_door_int` (`ABullyDoorLink`) | HAVE (208.0/-73.2 outside, -628.6/-324 inside) |
| B4-B6 | Boys' dorm exterior building (`SC1b_bldgBdorm` at (271.7, -127.7)), dorm court where the fight happens ((271, -100..-112)) | HAVE (in `tschool`); court is open ground, needs checked flat collision and the arena gate prop |
| End stub / C | Boys' dorm door pair: exterior trigger `DT_tschool_BoysDormL` (271.1, -122.4, z 7.9), exit outside `DT_DormExitDoorL` (271.2, -116.3); interior arrival `AreaTransitionBoys` (-503.0, 318.0, z 31.4), interior exit trigger (-502.4, 306.8) | Scene `isc_dorm` HAVE (114 placements, z 31-36); **door link NEW** (two entries in `door_links.json`, positions given here) |
| C1 | Soda machine prop, wardrobe/closet | NEW (no soda/vending model found among `isc_dorm`/`ischool` placement names; likely a separate prop object, **OPEN**) |

### Characters (ped model ids from `Objects/default.ide`; all use the shared 36-joint skeleton, so `python3 -m tools.importers ped <MODEL>` should apply as for the six existing students, **INFERRED**)

| Role | Model (id) | Used in | Status |
|---|---|---|---|
| Jimmy | `PLAYER` + head `B_Buzz` | all | HAVE (mesh, 439 clips, locomotion). **Outfit not wired:** the start outfit is jacket `B_Jacket6`, pants `B_Pants2`, shoes `P_Sneakers2`; uniform is `S_Sweater5` etc. `ped-heads-hair.md`: body shapes in `PLAYER.nif` are the underwear/base layer and the per-slot replacement rule is **OPEN** |
| Bullies | `GN_Bully01` (102), `GN_Bully02` (99), `GN_Bully03` (85), `GN_Bully04` (145), `GN_Bully05` (146) | A2, B3-B7 | NEW (5 imports) |
| Secretary | `TE_Secretary` (59) | A6, B2 | NEW |
| Small student | `GN_Hispanicboy` (69) | A3 | NEW (stub-able) |
| Teacher/prefect | `TE_MathTeacher` (61), `PFH2_Edward` (51) | B8 | NEW (stub-able) |
| Russell | `DOlead_Russell` (75) | B8 | NEW (stub-able with fade) |
| Gary, Eunice | `Nemesis_Gary` (130), `GN_Fatgirl` (74) | B0/B1, 1_02A | NEW (out of slice) |
| Ambient students | `GN_Boy01/02`, `GN_Asiangirl`, `GN_Greekboy`, `NDH3_Bucky`, `PFH1_Seth` | all | HAVE (9 walkers on 6 loops) |

### Animation (HXD groups; the clip decoder is complete, groups just need `ped --anims`)

| Need | Status |
|---|---|
| Jimmy locomotion, jumps | HAVE |
| **Jimmy combat set**: `C_Player` already has the clips (and they are in `SK_Jimmy`'s 439): about 77 `HIT_*` reactions, `BLOCK*` (13), `PUNCH_*`/`HEAVY_PUNCH_*`, `GROUND_KICK`, `RUN_PUNCH`, 23 `COMBAT\GRAP_*` and `GRAP_*_GV/_RCV`/`_ACT/_VIC` pair clips, `GRAPFLOOR_*`, `TAKEDOWN_*`, `S_PLAY\P_COMBAT_TAUNT_*` | HAVE the **clips**; nothing drives them yet |
| Bully idle/greet/shove/taunt: `SBULL_A` (11 clips: greet, idle, react, shove, shoved, taunt x4), `NPC_AggroTaunt` (6), `Hang_Talking` (11), `Area_School` (8), `B_Striker` (4) | NEW (import groups) |
| Secretary/teacher: `TE_Female` (6), `NPC_Adult` (11), `NPC_Principal` (5) | NEW (stub-able) |
| Humiliation pair clips: `HUMIL_5-8_*`, `HUMIL_6-1_*`, `HUMIL_6-5_*`, `HUMIL_*V*` | NEW; pairing/alignment unverified |
| `Russell` group (25 clips) | NEW, only for CS-5 (stub) |

### Audio

- HAVE: footsteps per surface, courtyard ambience, the speech decoder (all 40,400 `Speech.bin` streams decode).
- NEW: the slice's voice lines. `Speech.lst` names them `<Character>_<mission>_<line>_v<n>.rsm`, e.g. 13 files for `1-01` and about 133 across `1-01`/`1-02`/`1-02A`; the script's `("M_1_02", id)` ids join to the line number in the name. Speaker prefix does not always match the scripted ped, so routing is **INFERRED** (the `SpeechDB.bin` routing table is **OPEN**, AUDIO-4). Extract on demand with `audio extract` after locating by name; no audio is committed.
- NEW: UI sounds used by the tutorial (right/wrong button feedback), fight impact/whoosh cues (cue graph is decoded; names to be looked up per bank).

### Text and UI

Original uses localised string keys (`Config/Text/*.img`). The slice writes its **own** objective/tutorial strings; do not import theirs.

## 4. Systems needed (minimum), ranked by effort

| Rank | System | Effort | What the opening requires from it | Reuse |
|---|---|---|---|---|
| 1 | **Melee combat** | XL | Health, hit/block/grapple state machine, lock-on target (script locks player and partner onto each other), light-attack chain, grapple with strike and push-to-ground, ground kick, hit reactions and knockout, stun/min-health floor, tethered tutorial AI with attack-permission flag, invulnerability flag, spectator ring that does not interfere | Clips HAVE; pawn/locomotion HAVE; anim-instance pattern HAVE (`UBullyAnimInstance`); everything else NEW and the original trees are undecoded |
| 2 | **Mission / objective framework** | L | Stage-function state machine, objective list with add/complete/remove, fail/retry (attempt count skips intro), mission-critical ped, trigger volumes (enter/exit, area-aware), point and path lookup by name, timed hints, spawn/clear of mission peds | Door link actor and walk-runner show the trigger and logging style; new code |
| 3 | **HUD / prompts** | M-L | Objective text and subtitle line, tutorial pop-up queue, world-space or edge blip toward target, on-screen button-sequence prompt with success/fail feedback, enemy health bar, title card, letterbox, fade | Door fade HAVE |
| 4 | **NPC scripting layer** | L | Spawn at named point, face target, walk-to-point, follow-path, play named action node, ambient/hostile-social attitude (approach and shove), cheering idle, flee, sleep/wake, make-ambient, chase | `AAmbientWalker` and capsule-swept paths HAVE; AI/behaviour NEW |
| 5 | **Trigger/point importer** | S | `DAT/Trigger.img` text -> JSON (points, paths, triggers, perimeters, area codes) for the missions in this slice | NEW (format documented in `bully-re:docs/trigger.md`); replaces hand-measuring |
| 6 | **Cinematic player** | M | Letterbox, scripted camera path (path data exists), subtitled VO, skip, fade, take/return player control | Fade HAVE |
| 7 | **Interaction prompts** | S-M | Context action (here: humiliation prompt; later soda/wardrobe); doors are already auto-teleport | Door link HAVE |
| 8 | **Outfit swap** | M | Start outfit (and uniform for 1_02A) on `SK_Jimmy` | per-slot replacement OPEN |
| 9 | **Save / checkpoint** | S | Slice only needs mission restart and a "school save" flag; no persistence | none |

Everything else seen in the opening scripts (punishment system, social/gift systems, POIs, weather, clock tick, population manager, photography, clothing manager UI, minigames) is not needed for 1_01 and 1_02; the script disables or overrides most of them during these missions.

## 5. Proposed slice cut

**Slice name (ours): "Day One".** Content = 1_01 in full + 1_02 in full with simplified cinematics + a one-door epilogue.

| Segment | Included as | ~Time | Build fully vs stub |
|---|---|---|---|
| Fade in at the gates, movement hint, objective | A0-A1 | 0:30 | Full (objective + blip + hint) |
| Bully gauntlet, Pedro chase dressing | A2-A3 | 1:00 | Gauntlet full (hostile-social approach + shove, optional fight); chase = two scripted ambient peds, stub |
| Courtyard, front door, stairs, upper-floor office | A4-A7 | 1:30 | Full; **needs a walked and fixed stairs route** |
| Briefing | B0-B1 | 0:45 | Original short briefing card/VO in place of CS-2 and CS-3 |
| Task + lobby bullies | B2-B3 | 1:00 | Full (reuses the NPC layer) |
| Cross campus to the dorm court | B4 | 1:00 | Full |
| Bully cinematic, arena | B5 | 0:20 | Camera cut + fade (CS-4), ring of spectators |
| Guided fight (steps 1-7) | B6 | 3:00 | **Full, simplified: block, 3-hit chain, grapple + strike, push, ground kick**; invulnerable player; floored partner |
| Finish + humiliation | B7 | 0:30 | One contextual prompt playing one pair clip, or skip straight to the knockout; stub |
| Prefect scene | B8 | 0:20 | 10-15 s original sequence or fade + line (CS-5), stub |
| Epilogue | after B8 | 0:30 | Objective "get to your room", walk 15-20 m to the dorm door, door link, fade to black, end card. Closes the arc and exercises `isc_dorm`. |

Total about 10-11 minutes at a normal pace; a skilled player in under 8.

**End point:** the beat where the prefect breaks it up and Jimmy is sent to his room, followed by crossing the dorm threshold and a fade. It is the original mission boundary (1_02 passes at B8) plus a cheap, satisfying close; 1_02A (Gary, soda, uniform, save) is a stretch goal, not part of the cut.

**Cut out of the slice:** 1_02A, outfit/uniform logic (see risk 2), clothing manager, save/load, punishment and prefect enforcement, schedule/population manager, Gary/Eunice/Russell as playable-scene characters, humiliation variants, vehicles.

**Stub list:** chase pair, closed-gate swap (a blocking volume), humiliation, prefect scene, dorm door epilogue (door link only), outfit (use whatever Jimmy wears now), all strings (original text).

**Suggested build order (each step shippable and testable):** (1) trigger/point importer + mission data JSON; (2) mission framework + objective HUD + route from the gates to the office (this alone is a playable "walk to the office" slice and exposes the stairs problem early); (3) NPC layer + bullies + harass; (4) cinematic player + briefing/title card; (5) cross-campus + arena + dorm door link; (6) combat core with the tutorial AI; (7) tutorial prompts and the seven steps; (8) finish, prefect stub, epilogue; (9) voice lines and polish.

### Biggest risk / unknown

**Combat feel and the paired-animation work.** 1_02 is a forced fight tutorial whose real logic lives in action trees we have not decoded (`TrainingPlayer.act`, `Fight_Tutorial.act`, `AI_FightTutorial.act`; the locomotion pass still lists the param-name mapping as OPEN). The Lua gives only the order and gating of the steps. We have the clips (including pair clips with give/receive halves) but no data for hit windows, damage, combo timing, lock-on rules, or how a grapple aligns attacker and victim. Everything must be authored and tuned by hand, and that is the one place the slice can quietly eat weeks. Mitigation: define the fight as a small state machine from the script's seven steps and timing chosen to feel right; keep the partner's AI a scripted attack scheduler; use `GRAP_*_GIV/RCV` pairs with a snapped alignment transform; budget a dedicated critic pass.

### Other unknowns, ranked

1. **Stairs and the upper floor.** The office is on an upper floor; HANDOFF records a stop at the central-stairs landing and untested upper floors. Mission 1_01 cannot end without it. Interiors are also overexposed (known visual debt).
2. **Jimmy's outfit.** Unwired; the slice shows Jimmy for ten minutes (the base shapes are the underwear layer per `ped-heads-hair.md`). The per-slot replacement rule is the OPEN bit.
3. **Ped import for the 5 bullies and others** is assumed to be "one command each" (shared skeleton); the `Huge`/`Large` size classes and per-model texture variants are not checked.
4. **Voice-line routing** (which `Speech.lst` file for a given scripted event) rests on a name-join, not the routing table.
5. **How missions start and chain** (the mission table lives outside the Lua), irrelevant for our hard-wired slice but relevant if we later extend to 1_02A -> 1_02B.
6. **Collision of the dorm court / arena** and where the `1_02Gate` prop sits are unmeasured.

## 6. Mapping notes for implementers (Lua call -> our system)

Only the calls the three scripts use; there are 208 distinct natives across the three files, but they collapse to a small set of operations.

| Original operation family | Count in the scripts | Our equivalent |
|---|---|---|
| Spawn ped at a point / delete / hide health bar | ~70 | NPC layer spawn from point JSON |
| Set action node / is-playing | ~90 | Anim state machine API (named states), query by name |
| Face / move-to / follow-path / flee / wander / stop | ~50 | NPC layer commands |
| Lock target, tether, health, min-health, invulnerable, attitude | ~45 | Combat core |
| Camera fade / set / path / FOV / widescreen / return to player | ~40 | Cinematic player + camera manager |
| Speech play / preload / is-playing / queue | ~35 | Audio: speech cue by `(mission, id)` + subtitle |
| Objectives, text queue, tutorials, blips, HUD component toggles | ~40 | HUD / objective framework |
| Area transitions, door locks, population triggers | ~20 | Door link actors, mission-scoped locks, existing ambient spawner |
| Button-history sequence | ~15 | Tutorial prompt widget fed by input |
| Mission setup/cleanup, attempt count, DAT load/unload | ~15 | Mission framework lifecycle |

## 7. Pointers

- Opening scripts: `Bully-SE-scripts/scripts/chap1/1_01.lua`, `1_02.lua`, `1_02A.lua`; chapter transition `Chapt1Trans.lua`; mission requirements `SMissPass.lua`; global start `main.lua` (`F_LoadPlayer`, `F_LoadTime`), outfits `SCloth.lua`, `SInitGl.lua`.
- Mission world data (text, in the install): `DAT/Trigger.img` entries `1_01.dat` (28 points, 11 paths, 7 triggers), `1_02.dat`, `1_02A.dat`, `BoysDorm.dat`, `BDorm_Doors.dat`, `tschool_doors.dat`, `ischool_doors.dat`. Reader reference: `bully-re:docs/trigger.md`. Our `ImgArchive` already opens `Trigger.img`/`Trigger.dir` (same format as `World.img`).
- Ped id -> model: `Objects/default.ide` `peds` block (column 2).
- Combat clip names: `python3 -m tools.importers anim C_Player`.
