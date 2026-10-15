# Jimmy locomotion and camera: what the reference sources say

Scope: Phase 0 (Jimmy run/jump, third-person camera). Sources read:

- `/Users/midir/bully-remake-refs/Bully-SE-scripts/scripts/` (decompiled Lua, 551 files)
- `/Users/midir/bully-remake-refs/bully-re/` (RE notes and C++ skeletons; no game data)
- `/Users/midir/bully-remake-refs/bully-mact-tool/` (CAT/MACT converter; checked as a lead)

Game data (`Config/Dat/*.dat`, `.act`/CAT action trees, `.agr` animations, `World.img`) is not in any of these repos. Anything that lives only there is marked **OPEN**.

## Headline

1. **No walk, run, sprint, sneak, swim, acceleration, turn-rate, jump-velocity, jump-height or gravity constant for Jimmy exists in the Lua or in bully-re.** The Lua only names action-tree nodes and toggles native flags. The numbers live in the action trees (`Act/Player.act`, `Act/Globals/SimpleLoco.act`) and/or the per-ped stats file, neither of which is in the refs.
2. **No default camera distance, height, lag or FOV value is in the Lua or bully-re.** `CameraDefaultFOV()` is a native whose value is OPEN.
3. **Jimmy's identity and action roots are confirmed** (IDE entry, see section 1).
4. **Units:** the game world is in metres (sectors of 50 m). The pedestrian animation data is in metres, millimetres and centimetres depending on the key type. Walk-cycle speed is observable from animation root motion (about 1.1 to 1.7 m/s). The Lua itself has no unit annotations.
5. **Several "constants" that look authoritative are project choices, not game values:** eye height 1.6 m, body radius 0.3 m, step height 0.45 m, gravity 9.81 m/s², default FOV 60°. They appear in `bully-re/src/collision/Marche.h` and `src/render/Camera.h`, and `Marche.h:1-4` says so explicitly. Do not copy them into the spec as game values.

## 1. Jimmy's identity and action roots (confirmed)

IDE ped entry id 0, asserted in `bully-re/tests/test_ide.cpp`:

- `tests/test_ide.cpp:80`: id 0, model `player`, txd `player`, not female.
- `tests/test_ide.cpp:81`: size `Medium`, type `PLAYER1`, stat `STAT_PLAYER`.
- `tests/test_ide.cpp:82`: animGroup[0] = `Grap`, animGroup[3] = `null`, unique = -1.
- `tests/test_ide.cpp:83`: action root `/Global/Player`, file `Act/Player.act`.
- `tests/test_ide.cpp:84`: AI root `/Global/PlayerAI`, file `Act/PlayerAI.act`.
- `tests/test_ide.cpp:85`: name `N_Jimmy`.

The test file is `tests/test_ide.cpp`, which reads the game's IDE data. The IDE file itself is not in the refs.

Spawn: `scripts/main.lua:468-471`. `PlayerCreateXYZ(gPlayerStartX, gPlayerStartY, gPlayerStartZ)`, then `CameraReset()`, then `gPlayer = PlayerGetPedIndex()`, then `CameraSetHeading(180)`. The heading argument is likely degrees (180 is a half turn), but that is unconfirmed.

## 2. Movement natives used by Lua, with citations

Each row is a Lua call site. None of them passes a speed or jump-height number for Jimmy.

| Native | Use for Jimmy | Citation |
|---|---|---|
| `PedSetEffectedByGravity(gPlayer, false/true)` | Gravity off/on around scripted transitions (bus drop, teleport-style transitions) | `scripts/main.lua:375, 382`; `scripts/SGlFunc.lua:986, 1009`; `scripts/chap3/3_G3.lua:902, 907` |
| `PedSetActionNode(gPlayer, "/Global/Player/JumpActions/Jump/IdleJump", "Act/Player.act")` | Forces the standing jump node | `scripts/chap4/4_04.lua:311`; also `scripts/secnd/2_S04.lua:849` (same node path) |
| `PedSetActionNode(gPlayer, "/Global/Player/JumpActions/Jump/Falling/Fall/Falling/Fall_No_Damage", "Act/Player.act")` | Fall without damage | `scripts/chap3/3_04.lua:694` |
| `PedSetInfiniteSprint(ped, true)` | Used on NPCs only (no call on `gPlayer` found) | `scripts/chap3/3_01.lua:465-466` (gord, lola); others in chap3 and chap4 |
| `PedOverrideStat(ped, idx, value)` | Stat overrides on NPCs. Index 24 is a bike speed (Chad's bike = 70); indices 36/37 are that speed ±10 | `scripts/chap3/3_04.lua:45` (value), `:833`, `:1246-1247` |
| `PedJump(ped, pointlist, ...)` | NPC jumps, not the player | `scripts/chap3/3_03.lua:693-694` |
| `PlayerSetControl(0/1)` | Freezes or releases player input around cutscenes | `scripts/main.lua:374, 405`; many chap4 and chap5 files |

Native addresses (from `bully-re/docs/api-lua-tables.tsv`, for Ghidra): `PedJump` 0x005cd1d0, `PedSetActionNode` 0x005ca0d0, `PedSetEffectedByGravity` 0x005cd420, `PedSetInfiniteSprint` 0x005cde60, `PedOverrideStat` 0x005c9070, `PlayerCreateXYZ` 0x005d2b80, `PlayerSetControl` 0x005d12f0.

`PlayerSetControl` and `PedSetActionNode` do not reveal any value. The numbers must come from the action trees or from the native implementation.

## 3. Ped stats (STAT_PLAYER)

- `bully-re/tests/test_pedstats.cpp:20-27` asserts the first row of `PEDSTATS.DAT` is `STAT_PLAYER`: pickup none, drop likelihood 20, vision yaw 100, vision range 12, max health 200, unarmed, and humiliation 15 in the last column.
- `bully-re/docs/pedstats.md:1-10` lists the 65 columns (A to BM). Walk and run speed are **not** among the described columns. Only bike speeds are named ("vitesses de vélo").
- Conclusion: Jimmy's walk and run speeds are not in PEDSTATS. They most likely come from the action tree. Marked **OPEN** (see section 7).

## 4. Camera

### Lua evidence

- **Default FOV:** `CameraDefaultFOV()` is called to reset FOV at the start of cutscenes (`scripts/SCutscenes.lua:56`) and after fight shots (`scripts/chap4/4_B2.lua:337, 449`). The value is native and not in the Lua. **OPEN.** Ghidra: `CameraDefaultFOV` at 0x005b99a0 (`api-lua-map.tsv`: FUN_004f1510).
- **Per-scene FOV overrides** (not defaults): 80 (`scripts/chap4/4_01.lua:1042, 1091`; `4_04.lua:125`; `4_06.lua:1087`), 40 (`chap4/4_03.lua:536`; `chap4/4_B2.lua:303`), 50 (`chap4/4_06.lua:959`), 30 (`chap4/4_06.lua:1140`).
- **Test-only FOV:** `scripts/Test/SwimRace_Util.lua:11` sets FOV 70 for a swim race, and `Test/SwimRace_Test.lua:23` sets 80. These are test configs, not authoritative.
- **Follow camera:** `CameraFollowPed(gPlayer)` at `scripts/SObjTest.lua:429` and `scripts/chap3/3_02.lua:662`. `CameraReturnToPlayer()` is used at `scripts/main.lua:357, 402`.
- **Camera speed:** `CameraSetSpeed(a, b, c)` appears in many places with values 0.2 to 20 (`chap4/4_B2.lua:305` = 2,2,2; `chap3/3_01C.lua:273` = 0.2; `chap3/3_04.lua:1396` = 20). The units and the default are not in the Lua. **OPEN.** Ghidra: `CameraSetSpeed` at 0x005b9410.
- **Yaw limits:** `CameraSetRotationLimitRel(45, 55, 0, -1, 0, bike, 1)` at `scripts/chap3/3_04.lua:591` (bike chase). The units are presumably degrees, not confirmed.
- **Fight and scripted shots:** named shots such as `FollowCamSetFightShot("4B2")` followed by `CameraSetShot(1, "4B2", true)` (`scripts/chap4/4_B2.lua:170-171`). The shot data is not in the Lua. **OPEN.**
- **Offset helper:** `F_PedSetCameraOffsetXYZ(ped, x1, y1, z1, x2, y2, z2)` (`scripts/SGlFunc.lua:966-975`) converts a ped-local offset to world coordinates with `PedGetOffsetInWorldCoords`. The first triple is the camera position and the second is the look-at point. Example: `chap4/4_B2.lua:387` uses (1, 2.5, 2) and (0, 0, 1). This is for fights, not the default follow cam.

### bully-re evidence

- `bully-re/src/squelettes/FollowCameraController3.h` (vtable @ 0x00914598) and `FollowCam3Butes.h` (@ 0x00914580) are generated skeletons with function addresses only. **No distance, height, lag or FOV values.**
- `bully-re/src/render/Camera.h:16` uses `fovY = 1.0472f` (60°) and `Camera.h:17` uses near plane 0.05 m. These are the project's own renderer defaults, not the game's. Do not treat them as game values.

### Not found anywhere (OPEN)

Default follow distance, height offset, look-at offset, pitch and yaw defaults, lag and smoothing constants, and any collision-based camera pull-in. These are most likely in the `FollowCam3Butes` data or in native code near `FollowCameraController3` slots.

## 5. Animation groups and action trees for Jimmy

- **Player action tree:** `Act/Player.act`, root `/Global/Player` (IDE, section 1). Used throughout Lua, for example `scripts/chap3/3_01A.lua:545` and `scripts/chap2/Boxing_util.lua:356`.
- **Player AI tree:** `Act/PlayerAI.act`, root `/Global/PlayerAI` (IDE). Used for the car-stop brake at `scripts/SGlFunc.lua:1016-1018`.
- **Global trees loaded for all characters:** `scripts/LoadActionTrees.lua`. The branch list includes `/Global/SimpleLoco` (line 12), `/Global/Cameras` (line 8) and `/Global/PlayerFacial` (line 9). The tree file list includes `Act/Globals/SimpleLoco.act` (line 37), `Act/Globals/Cameras.act` (line 43) and `Act/Globals/PlayerFacial.act` (line 44). `SimpleLoco.act` is the likely home for the generic locomotion branch. **OPEN** until the file is decoded.
- **Animation group for Jimmy:** the IDE animGroup list starts with `Grap` (`tests/test_ide.cpp:82`). No `C_PLAYER`, `Idle`, `Walk` or `Run` group name appears in the Lua for Jimmy. **OPEN.** The known anim file is `Player_Tired.agr` in `World.img` (`bully-re/tests/test_agr.cpp:1, 39`; `docs/agr.md:137`). The skeleton is `PLAYER.nif` (`tests/test_skin.cpp:1`).
- **Pedestrian walk groups** (not Jimmy, but the same format): `F_Girls`, `F_Jocks`, `F_Greas`, `F_Preps`, `F_Nerds` (`bully-re/docs/agr.md:126-131`). Jimmy's walk group is not named in the refs. **OPEN.**
- **Jump and fall nodes** are in section 2.
- **Sprint and locomotion action-tree classes exist in the game** (names only, from `bully-re/export/rtti.json` and the `squelettes` headers): `ConditionSprint`, `SprintTrack`, `SprintIncreaseTrack`, `SprintIncrementChargeTrack`, `SetDesiredSpeedTrack`, `SpeedUpTrack`, `ConditionMotionIntentionSpeed`, `ConditionEntitySpeed`, `LocomotionJumpTrack`, `JumpLocoTrack`, `LocomotionCycleTrack`, `GravityTrack`, `GravityMovementInfo` (vtable @ 0x0093d884), `FrictionMovementInfo` (@ 0x0093d86c). The hash table `bully-mact-tool/DB/HASHES_TRACKS.txt` also has `SprintLocoAnims`, `TiredSprintLocoAnims` and `SprintIncrementCharge`. These names suggest a sprint meter and a tired sprint, but no values are in the refs.
- **Swim:** no swim locomotion in Lua for Jimmy. `SwimIt.lua` and `SwimRace_*.lua` are scripted events. Swim is noted as existing and is not specified here.

## 6. Units

- **Lua:** no unit annotations, and no "metres" or "units" comments in the scripts. `radius = 10` in `scripts/SCutscenes.lua:144` is the only size-like value in the grep.
- **World:** sectors are 50 m, world is 36×36 sectors, range [-900, 900[ on x and y (`bully-re/docs/world.md:9`; `src/core/World.h:72-73`, which divides by 50).
- **Pedestrian animation** (`bully-re/docs/agr.md`):
  - Position key types: type 999 in metres (line 41), type 1003 in millimetres (line 43), type 1004 in centimetres (line 45).
  - The 1004 scale is 0.01 (constant 0x900d30, line 50).
  - Pelvis height for a pedestrian is about 0.845 m (line 120). The pelvis of `1_02_MeetWithGary` drops to 0.87 m (lines 50-51).
  - Root motion from the arrow bone (`ARROW`, key 35): 1.48 m in 1.067 s for `F_Jocks` n°2 (lines 120-122), about 1.39 m/s. Walk-cycle speed range 1.1 to 1.7 m/s (line 135).
- **Heading:** `CameraSetHeading(180)` (`scripts/main.lua:471`), probably degrees. **OPEN** (confirm in the native at 0x00824d30).
- **Not units of Jimmy's own locomotion:** the 1.39 m/s and 1.1 to 1.7 m/s values come from pedestrian walk animations. Jimmy's walk and run speeds are still OPEN.

## 7. OPEN gaps (for Ghidra against Bully.exe, or for decoding game data)

1. **Walk, run, sprint, sneak, swim speeds; acceleration; turn rates.** Not in Lua or bully-re. Needed: decoded `Act/Player.act` and `Act/Globals/SimpleLoco.act`, or the native that reads them. Ghidra targets from the RTTI names above: `SetDesiredSpeedTrack`, `SpeedUpTrack`, `SprintTrack`, `SprintIncrementChargeTrack`, `ConditionSprint`, `ConditionMotionIntentionSpeed`. Use the vtable addresses in `bully-re/src/squelettes/`.
2. **Jump velocity, jump height, air control.** Not in Lua. Ghidra targets: `JumpLocoTrack` (vtable @ 0x00935638), `LocomotionJumpTrack` (@ 0x00931044), `JumpObjective`. Native `PedJump` 0x005cd1d0 is NPC-only.
3. **Gravity constant.** The 9.81 in `bully-re/src/collision/Marche.cpp:195` is the project's value, not the game's. Ghidra targets: `GravityMovementInfo` (0x0093d884), `GravityTrack`, native `PedSetEffectedByGravity` 0x005cd420.
4. **Player physics step.** The game's `CWorld::ProcessLineOfSight` and `CCollision::ProcessColModels` are not located (`bully-re/src/collision/Marche.h:2-4`). The player ped update is probably `CPlayerPed::ProcessControl` at 0x49bae0, 928 bytes (`bully-re/src/peds/PlayerPed.h:9`). **Start here** for input-to-velocity and jump.
5. **Default camera FOV value.** `CameraDefaultFOV` at 0x005b99a0 (FUN_004f1510). `CameraSetFOV` at 0x005b9970.
6. **Follow-camera defaults** (distance, height, look-at, lag, pitch and yaw limits). Ghidra: `FollowCameraController3` (vtable 0x00914598), `FollowCam3Butes` (0x00914580), `CameraSetNearPlane` (0x005b9250), `CameraSetSpeed` (0x005b9410), `CameraSetRotationSpeed` (0x005b9490). Values may sit in a butes data file.
7. **Jimmy's anim groups.** Which `.agr` files Jimmy uses for idle, walk, run, jump and land. Start from `Player_Tired.agr` and the `Act/Player.act` node names.
8. **Heading units** for `CameraSetHeading`, `CameraSetRotationLimit` and `CameraSetRotationLimitRel`. Confirm in native code.
9. **Jimmy's walk-group name** in the IDE data.

## 8. Lead for the action-tree values

`/Users/midir/bully-remake-refs/bully-mact-tool/` converts Bully's CAT action-tree files into readable MACT text (`README.md`: `python3 CAT_TO_MACT.py YourCatFile.cat`). `Act/Player.act` and `Act/Globals/SimpleLoco.act` are CAT files in the game data. If the game's `Act` folder is available, this tool may be the fastest way to read Jimmy's locomotion track parameters without Ghidra. It is a lead only; it was not run, and no CAT file is in the refs.

## Method notes

- Lua scan: grep for movement, camera, jump, gravity, sprint, speed and FOV names across all `.lua` files, then read the call sites above.
- bully-re scan: docs (`api-lua-tables.tsv`, `api-lua-map.tsv`, `pedstats.md`, `agr.md`, `world.md`, `journal.md`), tests (`test_ide.cpp`, `test_pedstats.cpp`, `test_agr.cpp`), and source (`src/render/Camera.*`, `src/collision/Marche.*`, `src/peds/PlayerPed.h`, `src/squelettes/*`).
- No file outside this one was written. No git state was touched.

## Verified lead (2026-10-15, orchestrator): action trees decode

The install's `Act/Act.img` (479 entries, same IMG format) contains `Player.cat`, `SimpleLoco.cat`, `Locomotion.cat`, `GlobalLoco.cat`. Extracting them and running `bully-mact-tool/CAT_TO_MACT.py` (numpy required) produces readable MACT text — e.g. `SimpleLoco.cat` → 931 lines with `MotionIntentionSpeed` / `PedVelocity` conditions and `Locomotion` tracks carrying float params (speeds/rates, e.g. `3.400000`, `6.283185` = 2π turn rate). Decoded dumps are **derived game data — never commit them**; regenerate into the scratchpad as needed.

**OPEN:** map `Tracks/Locomotion` param offsets (param00016/24/36/44/48/52…) to names via the tool's `DB/` + `TEMPLATES/`, cross-checked in Ghidra (`CPlayerPed::ProcessControl` @0x49bae0). That mapping yields the ground-truth run/sprint/jump numbers for milestone 3.
