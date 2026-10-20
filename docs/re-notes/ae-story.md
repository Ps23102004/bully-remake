<!-- OWNER: lane-D (story research) -->
# AE storyline: where the story lives, what is decodable, and what "every chapter playable" costs

Status: 2026-10-20, read-only research on the owner's Anniversary Edition (iOS/Mac build). Sources: `scripts/scripts.img`,
`act/act.img`, `dat/Trigger.img`, `cuts/Cuts.img`, `stream/world.img` under the extracted tree, `data_1.zip`, and the
iOS executable `bully.app/bully` (static reading only). Nothing from the game is in this repo: the numbers below were
measured by throwaway scripts in the session scratch dir (`.../scratchpad/story/`, listed in section 9). No dialogue or
script text is reproduced; the one code excerpt (section 6) is a short structural sample of a decompiled function.

Confidence tags as in `format-survey.md`: **VERIFIED** (measured over the whole data set), **INFERRED**, **OPEN**.

## 0. Headline

1. **The mission scripts are plain Lua 5.0 bytecode, not encrypted.** `scripts.img` holds 532 `.lur` chunks; all 532
   parse to the last byte with a 150-line Python reader (8,017 functions, 545,447 instructions). Stock Lua 5.0 opcodes;
   the only change is a 4-byte `float` `lua_Number`. Debug info (local names, line numbers) is stripped; global names,
   function names and every string constant survive. A prototype pseudo-decompiler turns 1_01 into readable code that
   matches the beat list in `opening-slice-recon.md`. **VERIFIED**
2. **The `Wx` cipher (`.xml`, `.mtl`, `.eff`, `.wdb`) is broken.** Custom-alphabet base32 + a byte keystream from a
   Wichmann-Hill-style generator; the decoder is readable in the executable, the 32-bit seed was brute-forced from known
   plaintext. All 6,931 `Wx` files in `data_1.zip` decode to valid UTF-8 (6,542 `.mtl`, 338 `.xml`, 48 `.eff`, 3 `.wdb`).
   This unlocks every text table (15,357 entries, 8 languages), every cutscene subtitle, every UI layout and the **true
   material texture slots** (section 4). **VERIFIED**
3. **The game is ~1,250 native functions called from Lua.** The story missions (76 non-trivial scripts in the chapter
   banks) call 739 distinct natives (31,654 call sites, imported libraries included). 1,237 of the 1,250 native names
   exist as strings in the executable, and 1,153 are recovered as `{name, C function address}` pairs from its
   registration tables, so each native's real behaviour can be read in arm64 when its name is not enough. **VERIFIED**
4. **Recommendation: run the original scripts, do not hand-port them.** Hand-porting 83 story scripts (223k bytecode
   instructions) into the current JSON stage vocabulary (15 actions, 9 conditions) is neither faithful nor cheaper. The
   faithful path is a Lua 5.0 VM inside the UE module that loads the owner's `.lur` at runtime, plus a C++ implementation
   of the native API, plus the action-tree runtime (`act.img`), which is the real long pole. Estimate: **~70-85 agent
   sessions** to "every chapter playable", with chapter 1 running from original scripts after ~15 (section 8).

## 1. Where the story lives in AE

| Container | Content | Format | Status |
|---|---|---|---|
| `scripts/scripts.img` (+`.dir`) | 532 `.lur`: 98 chapter/mission scripts (`1_01` .. `6_03`), libraries (`lib*.lur`), world/system scripts (`main`, `stimecycle`, `sstores`, `schoolhallways`, area scripts), minigames, ~150 `test*` dev scripts | Lua 5.0 bytecode, float numbers | **decodable** (prototype) |
| `act/act.img` | 598 action trees: mission banks (`chap1..5.act`, `classes`, `minigames`, `punish`, `tutorials`, `doors`), per-mission trees (`1_01.act`, `1_02aggro.act`), AI (`ai_*.act`), player/combat/social trees | `/AcT` + `/AcTrV2/` node graph with a `/String/` table | string tables parsed; node graph **OPEN** (partly mapped in `locomotion-spec.md`) |
| `stream/world.img` | 119 more `.act` (props), 550 `.agr` (anim groups incl. mission NIS groups like `1_02_meetwithgary`), 493 `.lip`, 52 `.lur` that are only `error("This file should never be loaded")` stubs | known | agr/HXD decoders exist (`agr.py`, `hxd.py`) |
| `dat/Trigger.img` | 410 text `.dat`: per-mission points, paths, trigger volumes | text | **importer exists** (`trigger.py`, `trigger-format.md`) |
| `cuts/Cuts.img` | 170 baked cutscenes: `.agr` + `.HXD` (actor animation), `.dat` (text camera/FOV key tracks), `.inf` (actor list, e.g. `CS_PLAY`, `CS_TE_SECRETARY`, facial `F_*`), `.SUB` (MicroDVD-style `{frame}{frame}` timing), `.mxd` (168, prop sets) | mixed | agr/HXD decodable; `.dat` camera tracks readable text; `.mxd` **OPEN** |
| `data_1.zip` `bully/*.xml` | 338: per-mission string tables (`1_01.xml` ...), 107 cutscene subtitle tables (`1-1-1_sub.xml` ...), `general.xml` (2.4 MB global text), minigame/class tables, UI scenes (`HUDScene.xml` ...) | `Wx` cipher -> XML-ish text | **decodable** (section 3) |
| `data_1.zip` `bully/*.snd` | 2,946 sounds (927 MB): 126 `cuts_*` full cutscene mixes, 187 `speech_*`, 263 `mx_*` music, 143 ambience, 2,211 sfx | small header + text descriptor (`{length(float)=..,samplerate(u4)=..}`) + a plain **Ogg Vorbis** stream | playable (strip header; ffmpeg reads the `OggS` payload) |
| `audio/CONFIG` | `SpeechDB.bin`, `missbank.txt`, `trigbank.txt`, `IMusic.bin` ... | as SE (`audio-formats.md`) | partly decoded (SE notes) |

Mission naming (from the banks, section 2): `<chapter>_<nn>` main missions; suffix letters `B` boss/chapter finale,
`G` girl missions, `S` specials, `R` races/errands/paper route, `E` errand tutorial, `xNN`/`xp` Halloween,
`ChaptNTrans` chapter transitions; `classes`/`Class*` lessons; `MG*` minigames; `test*` developer scripts (not shipped
content). Some ids sit in a later chapter bank than their number (e.g. `2_S07` in the chapter 5 bank, `3_R07` in 6).

## 2. Scripts: format and census

Header (all 532 identical): `1B 4C 75 61 50 01 04 04 04 06 08 09 09 04 3B AF EF 4B` = `ESC Lua`, version 5.0,
little endian, `int`/`size_t`/`Instruction` 4 bytes, `SIZE_OP 6, A 8, B 9, C 9`, `lua_Number` 4 bytes, test number
31415926.535 as a float. Function layout is stock `lundump.c` 5.0 (source, line, nups, params, vararg, maxstack,
lines, locals, upvalues, constants + nested protos, code); line/local/upvalue-name arrays are empty (stripped). The main
chunk's source string is `=(none)`. Instruction encoding is stock 5.0 (`OP` bits 0-5, `C` 6-14, `B` 15-23, `A` 24-31,
RK constants at index >= 250). **VERIFIED** on every chunk; opcodes used: all 35 of Lua 5.0 (`GETGLOBAL` 109,729,
`LOADK` 88,927, `CALL` 76,589 ... `POW` 4).

Stock tools: no `luac`/`lua` is installed here, and stock `luac -l` 5.0 would refuse the chunks anyway (it checks
`sizeof(lua_Number)` = 8); unluac (Java) supports 5.0 but was not downloaded (no network fetch was needed). The prototype
(`lua50.py`, ~150 lines; `decomp.py`, ~200 lines, expression folding + labels/gotos, no loop structuring) is enough to
read any mission.

### How missions are registered and what they reference

- **Mission bank = action trees.** `missions.act` references `chap1.act` .. `chap5.act`, `classes.act`,
  `minigames.act`, `punish.act`, `tutorials.act`, `doors.act`. Each mission node carries the script path
  (`chap1\1_01.lua`), a debug title, a title text key (`1_01_TITLE`), a corona/launch marker (`1_01_Corona`, `BUT_LAUNCH`)
  and `Succeed`/`Fail`/`Active` children; unlock conditions are action-tree conditions (`ConditionMissionAvailable`,
  `ConditionMissionSuccessCount`, `ConditionCanLaunchMission` are class names in the executable). Parsed string tables:
  148 mission nodes, 143 distinct scripts, all but 9 present in `scripts.img` (the 9 missing are dummies and arcade
  cabinet games). **VERIFIED** (strings), unlock-condition semantics **OPEN** (node graph).

| Bank folder | scripts | non-trivial (> 100 instr) |
|---|---|---|
| chap1 | 23 | 18 |
| chap2 | 14 | 13 |
| chap3 | 18 | 17 |
| chap4 | 10 | 10 |
| chap5 | 11 | 11 |
| chap6 | 7 | 6 |
| **story total** | **83** | **76** (223,133 instructions) |
| secnd (errands, girls, specials, classes, carnival, go-kart, arcade races) | 34 | 33 |
| Multiplayer (AE 2-player classes) | 11 | not counted |

Largest story scripts: `4_06` 8,450 instr, `4_04` 6,544, `3_04` 6,440, `1_08` 6,210, `1_11xp` 6,005.

- **Per-mission data** (string constants of each story script, cross-checked against the archives, **VERIFIED**):
  - `DATLoad("<id>.DAT", n)` + `DATInit()` in 75 of 76 -> `Trigger.img`. The engine exposes the loaded entities as
    global tables, e.g. `TRIGGER._1_01_PRINCOFFICE`, `POINTLIST._1_01_KIDRUN`, `PATH._...` (upper-cased name with a
    leading `_`). This maps 1:1 onto our `trigger.py` output ids.
  - `PlayCutsceneWithLoad("<cut>", ...)`: 89 distinct baked cutscenes in story scripts, 12 more in secondary ones, all
    present in `Cuts.img` (170 total) with matching `cuts_<cut>.snd` audio and `<cut>_sub.xml` subtitles.
  - In-engine NIS scenes: 947 distinct `/Global/<mission>/NIS/...` action-node paths in 67 scripts, driven by
    `PedSetActionNode` (the most-called native after `Wait`) on per-mission trees `Act/Conv/<id>.act`.
  - `LoadAnimationGroup("<group>")` -> `.agr` in world.img; `LoadPedModels({ids})` / `PedCreatePoint(id, ...)` use numeric
    ped ids from `objects/*.ide`; `TextPrint("<key>")` / `MissionObjectiveAdd("<key>")` -> the mission's `.xml` table;
    `SoundPlayScriptedSpeechEvent(ped, "<event>", n, ...)` -> speech banks.
- **World/system logic is also script-side**: `main.lur` (4,723 instr: save points, busted/wake-up, collectibles, BMX
  garage, holiday items), `stimecycle.lur` (class/curfew/time-of-day state machine: attended/missed class, curfew
  tiredness stages), `smisspass.lur` (mission requirements and restart points), `sstores.lur` (shops),
  `schoolhallways.lur` and other area scripts (prefect patrols, school store, spawners). Running the original scripts
  therefore brings these systems for free once the natives exist.

## 3. The `Wx` cipher (supersedes `ae-materials.md` section 5)

Located by searching the executable for the 32 cipher symbols as one 32-byte string: `__cstring` at VM `0x10098F8CC`
holds `6Ev2GlK1sWoCa5MfQ0pj43DH8Rzi9UnX`, referenced by a pointer in `__data` (`0x100A4BB30`). Two functions load it:

- `0x10085B164`: base32 encoder, 5 bits per symbol, MSB first, symbol = alphabet[value].
- `0x10085B33C`: builds the 256-entry reverse table, skips the `Wx` tag, decodes `len*5/8` bytes.
- Called from the codec wrapper `0x1000E9C70` (decode); the encode loop is just above it, near `0x1000E9B00`, which applies a keystream:

```
r = seed                      # seed = djb2(key string): h = 5381; h = h*33 + c  (function 0x10085BA60)
for i, c in enumerate(base32_decoded_bytes):
    r = 171 * (r % 177) - 2 * (r // 177)      # uint32 arithmetic (Wichmann-Hill generator 1, no +30269 fix-up)
    plain[i] = ((c ^ (6*i + 18)) + r) & 0xFF
```

The key string comes from a virtual call on a global singleton (vtable slot `0xD0`) and was not traced; instead the seed
was brute-forced (2^32, ~50 s in C) against the plaintext prefix `<Entr` of `1_01.xml`: seed `0x0CDFC798` (many
seeds are equivalent because only the generator's orbit matters). With it **all 6,931 `Wx` files decode** to UTF-8 and
every `.mtl` starts with `{` and names an effect. This explains the old observations (fixed position-keyed stream,
carry-like symbol pairs = the add inside a 5-bit packing). **VERIFIED**

What is inside:

- `.xml` text tables: `<Entry name="KEY"` with `default`, `french`, `german`, `italian`, `spanish`, `british`, `russian`,
  `japanese` attributes; 270 files, 15,357 entries. Mission tables key by text id (`1_01_...`) and speech id (`S20350`).
  `_sub.xml` entries are `<cut>.SUB0001..` lines whose timing is in `Cuts.img` `<cut>.SUB` (`{start}{end}` frames).
- UI scenes (`HUDScene.xml`, `Menu*.xml`, `*Controller.xml`): War Drum UI markup (`UIContainer`, `UIImage`, `#include`).
- `.mtl`: `effect(effect)=bully_normalspec|bully_normal|bully_diffuse|bully_spec|...` (20 effects; 3,184 / 1,294 /
  1,089 / 896 for the top four), `textures(orderedarray<texture2d>)=[n, diffuse, normal?, spec?]` and optional
  `vectors(...)` (UV tiling/tint). Checked against the hand-made `MATERIAL_FIXES` in `ae.py`: 8 of 11 confirmed, **3
  wrong**: `mat_pr00_drainp` is `black4_d` (+`pr00_drainp_s`), `mat_mascot_head` is `sp_mascot_h_d` (+`mascot_head_n`),
  `mat_sc28_tile_01*` is `bdormsc06_floor_d` (+`sc28_tile_01_n`). The importer now reads the `.mtl` (`tools/importers/ae.py`, `wx.py`; `ae-materials.md` section 6) and `MATERIAL_FIXES` is deleted; the
  text tables are decoded by `python3 -m tools.importers text` (`ae_text.py`).

## 4. Native API census (the engine surface the scripts need)

1,250 globals are called but never defined by any script = engine natives (plus a handful of Lua builtins such as
`collectgarbage`). Story missions + their imported libraries use 739 of them. Families (distinct natives used by
story): Ped 208, Player 51, Sound 51, Area 45, Mission 30, Vehicle 29, Camera 26, Minigame 16, Race 16, Clothing 12,
Button 11, Counter 11, Pickup 10, Object 7, Effect 6, Class 6, Blip 5, Text 5, Weapon 4, Clock 3.

Call-site coverage if natives are implemented in frequency order (story missions):

| natives implemented | share of call sites |
|---|---|
| 25 | 42.1 % |
| 50 | 59.2 % |
| 100 | 74.5 % |
| 200 | 88.1 % |
| 300 | 93.8 % |
| 500 | 98.6 % |
| 739 | 100 % |

Per mission: median 118 distinct natives (min 18, max 209). No mission is covered before ~400 natives, so the long tail
must be **stub-tolerant**: an unknown native logs once and returns a neutral value (nil/false/0), which keeps a mission
running with that feature missing. Most of the tail (sound/FX/HUD toggles, stats, debug) is safe to stub; the ones that
are not are Ped/Player/Area/Mission/Camera/Vehicle core (~250).

Top of the list: `Wait`, `PedSetActionNode`, `PedCreatePoint`, `CameraFade`, `PlayerSetControl`,
`SoundPlayScriptedSpeechEvent`, `PedIsValid`, `CameraSetWidescreen`, `TextPrint`, `PlayerIsInTrigger`, `PedStop`,
`CameraSetXYZ`, `BlipRemove`, `PedDelete`, `PedFaceObject`, `LoadAnimationGroup`, `PedClearObjectives`,
`PedFollowPath`, `PedMakeAmbient`, `GetTimer`, `MissionObjectiveAdd`, `PedOverrideStat`, `CreateThread`,
`AreaSetDoorLocked`, `PedSetPedToTypeAttitude`.

Executable side: 44 `{const char* name, lua_CFunction fn}` tables in `__data` (1,511 registrations); 1,153 of the 1,250
script natives resolve to a code address (e.g. `PedSetActionNode` -> `0x100547C00`). The `__text` section is readable
except the 4 KB FairPlay page at file offset 16 KB, so a native whose arguments or effect are unclear can be read
directly. **VERIFIED** (table extraction); per-native semantics **OPEN** by design (read on demand).

## 5. Mapping to the in-repo mission framework

`UMissionSubsystem` (JSON stages) + `UScriptedNPCSubsystem` (NPC beats) + `trigger.py` already cover the opening
missions by hand. How the original API maps:

| Original natives (examples) | In-repo equivalent | Gap |
|---|---|---|
| `DATLoad`/`DATInit`, `TRIGGER.*`, `POINTLIST.*`, `PATH.*`, `PlayerIsInTrigger`, `GetPointList` | `trigger.py` JSON + `trigger_enter`, `reach_location`, NPC `follow_path` | none in data; needs a runtime bridge exposing them as Lua tables |
| `MissionObjectiveAdd/Complete/Remove`, `BlipAddPoint`, `AddBlipForChar`, `MissionSucceed/Fail`, `MissionTimer*` | stage objective + blip, `complete`/`fail`, `timer` | timers on HUD, multiple concurrent objectives, blips on peds |
| `TextPrint`, `TextPrintString`, tutorial pop-ups | `hint`, `banner` | text keys must resolve through the decrypted `.xml` tables |
| `CameraFade`, `CameraSetWidescreen`, `CameraSetXYZ`, `CameraSetFOV`, `CameraReturnToPlayer` | `fade`, `letterbox`, `camera`, `shake` | camera paths, look-at-ped |
| `PlayCutsceneWithLoad` | none (`say` + `camera` stand-ins) | baked cutscene player (Cuts.img) |
| `PedCreatePoint`, `PedFollowPath`, `PedMoveToPoint`, `PedFaceObject`, `PedMakeAmbient`, `PedWander`, `PedStop` | scripted NPC `goto`, `follow_path`, `face`, `ambient` | spawning by ped id, ~200 Ped natives |
| `PedAttack`, `PedSetPedToTypeAttitude`, `SetFactionRespect`, `PedSetInvulnerable`, `PedOverrideStat`, `PedLockTarget` | `spawn_enemy` + `BullyCombatAI` | faction/attitude model, AI objectives, stats |
| `PedSetActionNode`, `PAnimSetActionNode`, `LoadActionTree` | gesture clips by name | **action-tree runtime** (no equivalent) |
| `CreateThread`, `Wait`, `GetTimer`, `shared.*`, globals | stage polling | real coroutines + persistent `shared` table |
| `ClockSet`, `WeatherSet`, `AreaSetDoorLocked`, `GeometryInstance`, `AreaGetVisible`, `AreaTransition*` | none / `ABullyDoorLink` | clock, weather, door locks, area (interior) streaming and visibility |
| `VehicleCreatePoint`, `VehicleFollowPath`, races | `ABullyVehicle` (player only) | AI driving, races |
| `Clothing*`, `Shop*`, `Class*`, `Minigame*`, `MG*` | none | whole systems |
| `MissionSuccessCountInc`, `IsMissionCompleated`, `ChapterSet`, save | `UBullyProgressSubsystem` (money, respect) | mission flags, chapter, saves |

Missing systems for the full game (not mission-specific): school bell / class timetable / curfew (script-side in
`stimecycle.lur`, natives `ClockGet/Set`, `MissionStartNextClass`); the 9 class minigames (English, Math, Chemistry,
Art, Biology, Geography, Music, Shop, Photography: native `Class*` API, 62 natives, plus UI from `MG*Controller.xml`);
punishment / prefect / "trouble meter" and busting (`punish.act`, `PlayerSetPunishmentPoints`, `RegisterCallbackOnBusted`);
fighting styles and humiliations (action trees `*_striker_a.act`, `grapple_*.act`, `playerhumiliate.act`); weapon
inventory and pickups (`PedSetWeapon`, `Pickup*`, `WeaponRequestModel`); bikes / skateboard / go-kart / AI cars and races
(`Vehicle*`, `Race*`, `gokartraces.lur`, `bikerace_util.lur`); errands, paper route, lawn mowing, dares
(`nt_dares.xml`), carnival games; clothing, barber, tattoo and shops (`Clothing*`, `Shop*`, `sstores.lur`,
`Cloths.dat`); faction respect and clique territory; social actions (greet, taunt, apologise, give gifts, kiss); save
books / save points; collectibles; day-night time cycle and weather (holiday variants: Halloween `1_11*`, winter).

## 6. Sample: mission 1_01 decompiled (structure only)

`1_01.lur`: 254-instruction main chunk + 53 functions. The main chunk imports `Library/LibTable.lua`,
`LibTrigger.lua`, `LibSchool.lua` and initialises ~59 locals. Functions by role: `MissionSetup` (load `1_01.DAT`,
action trees `Act/Conv/1_01.act` + `1_02.act`, clock 08:30, first-attempt-only cutscene `1-1-1` with the front gate
held open by an action node, then two `GeometryInstance` swaps to closed gates), `F_MissionSetup` (anim groups, 17 ped
models, faction respect values, social override tree `Mission/1_01_AngryShove.act`), `main` (stage loop), stage
functions `F_StageOneSetup` / `F_CheckFinish` (plus dead `F_StageTwo*` / `F_StageThree*` and a bully NIS that the live
path never calls), threads `T_Lost` / `T_Tutorials`, ten `CB_GotThere1..10` path callbacks, door locks
`F_ParkDoorClose` / `F_LockNurseDoor`, and action-tree condition functions `F_PlayerGrapple`, `F_PlayerSloppyAttacks`,
`F_PlayerHeavyAttacks`, `F_CanBlock` that the trees call back into. Natives used: 109 distinct.

Excerpt (prototype output, `main` of 1_01; register names are synthetic because locals are stripped):

```lua
-- function main()  [49 instr]
  F_MissionSetup()
  stageFunction = F_StageOneSetup
  gMissionRunning = true
  goto L16
  ::L7::
  UpdateTextQueue()
  stageFunction()
  F_UpdatePopulation()
  Wait(0)
  ::L16::
  if gMissionRunning then goto L7 end
  L_StopMonitoringTriggers()
  L_CutsceneFade()
  if not gMissionSuccess then goto L42 end
  MissionSucceed(true, false, false)
  PedSetActionNode(gPlayer, "/Global/Player", "Act/Player.act")
  goto L48
  ::L42::
  SoundPlayMissionEndMusic(false, 10)
  MissionFail()
```

`F_CheckFinish` (paraphrased): once the school interior (area 2) is visible, spawn the secretary (ped 59) at the
`1_01_SECRETARYINOFFICE` point; on the first update spawn a running kid (69) and a bully (102) on `1_01_KIDRUN`, give both
infinite sprint, order the bully to attack after 1 s, and hand both back to ambient life when the player enters
`1_01_CHECKOBJS1`; when the player enters `1_01_PRINCOFFICE`, take control, face heading 0, play the wave action node
and the `M_1_01` speech event, wait for the speech to end, mark success. This is exactly beats A1-A7 of
`opening-slice-recon.md` (which was written from SE scripts): the AE mission logic matches SE. **VERIFIED** for 1_01.

## 7. Strategy: why run the original scripts

| | Hand-port to JSON (current) | Host the original `.lur` |
|---|---|---|
| Faithfulness | paraphrase per mission; drift guaranteed | exact logic, timings, fail conditions, retries |
| Work scales with | 83 missions x their logic (223k instructions) | ~740 natives once + action-tree runtime once |
| Content in repo | authored JSON per mission | none: scripts load from the owner's install at runtime (same rule as meshes) |
| Free extras | none | world scripts: class schedule, curfew, shops, hallways, save points, punishment hooks |
| Risk | effort, bugs per mission | native semantics guessed from names (mitigated: executable readable); action trees |

Mechanics: vendor the Lua 5.0.3 VM (MIT licence, ~15 kLOC C) into a UE ThirdParty module built with
`LUA_NUMBER float` so it loads the chunks unmodified; `CreateThread`/`Wait` map to coroutines resumed per tick;
`ImportScript("Library/X.lua")` resolves to `scripts.img` `x.lur`; `DATLoad` builds `TRIGGER`/`POINTLIST`/`PATH` tables
from the existing trigger importer output; natives are a C++ registration table with a stub fallback. The current JSON
missions stay as the test harness and for original (non-Bully) content. Needs owner approval as a new third-party
source dependency (no cost, no network).

## 8. Roadmap to "every chapter playable" (ordered; sizes in agent sessions)

| # | Work item | Size | Unblocks |
|---|---|---|---|
| 1 | `ae_text` importer: decode `Wx` -> JSON string tables + subtitles (with `.SUB` timings); tests on synthetic encoded fixtures | 1 | all text, objectives, subtitles |
| 2 | `ae.py`: read true texture slots + effect from `.mtl` (drop the inferred `MATERIAL_FIXES`) | 1 | correct materials (3 known wrong today) |
| 3 | `lur` importer: promote the reader/disassembler + mission bank extraction (`chap*.act` string tables) into `tools/importers`, synthetic-chunk tests | 1 | tooling, per-mission dependency reports |
| 4 | Lua 5.0 VM in UE (ThirdParty, float numbers), coroutine scheduler, `ImportScript`, `shared` table, stub-tolerant native registry with a "missing natives" report | 2 | everything below |
| 5 | Native core 1 (~120): mission/objective/blip/text/timer, player control, triggers/points/paths via `DATLoad`, camera fade/widescreen/XYZ, area visibility + door locks, `GeometryInstance`, clock/weather | 3 | 1_01 from the original script |
| 6 | Native core 2 (~130): ped create/delete/move/follow/face/wander/ambient, attitudes + faction respect, `PedAttack`/stats/invulnerability on `BullyCombatAI`, ped spawning by id (ped importer for all ~250 ped models) | 4 | chapter 1 ped logic |
| 7 | Action-tree runtime: decode `act.img` node graph (nodes, conditions, tracks, file refs), drive anims/branching; callbacks into Lua condition functions | 6-10 | `PedSetActionNode` (most-called native), NIS scenes, fighting styles, humiliations, props |
| 8 | Baked cutscene player: `Cuts.img` actors (agr/HXD -> UE anim), camera `.dat` tracks, `cuts_*.snd` Ogg, subtitles | 3 | 101 cutscenes |
| 9 | Speech: `SpeechDB` + `speech_*.snd` + scripted/ambient speech events, lip (`.lip`) optional | 2 | mission dialogue audio |
| 10 | Time cycle + bell/classes + curfew (`stimecycle.lur` on natives), punishment/prefects/busted | 3 | free-roam school life |
| 11 | Weapons/pickups/inventory natives on `UBullyWeaponComponent`; vehicles: AI driving, bike/kart races (`Vehicle*`, `Race*`) | 5 | chapters 2-5 races, chases |
| 12 | Clothing/shops/barber (`Clothing*`, `Shop*`, `sstores.lur`, `Cloths.dat`) | 3 | 1_02A wardrobe, stores |
| 13 | Class minigames (9) + carnival/arcade/lawn/paper route/boxing/dodgeball (native `Class*`/`MG*`/`Minigame*` + UI) | 10-15 | class progression, side content |
| 14 | Save/progress: mission flags, chapter, `shared` persistence, save books | 1 | chaining chapters |
| 15 | Mission bank: unlock/launch conditions from `chap*.act`, coronas, mission start flow | 2 | story order without debug launch |
| 16 | Per-chapter bring-up and bug bash (1: 4, 2: 3, 3: 4, 4: 3, 5: 3, 6: 2) | 19 | each chapter end to end |
| | **Total** | **~66-85** | |

Milestones: after items 1-6 (~12 sessions) 1_01 runs from the original scripts with placeholder animation;
after item 7-8 chapter 1 is playable with its cutscenes (~25 sessions in). Items 11-13 can run in parallel lanes once
item 4 lands. Only items 4-8 and 16 need Unreal; 1-3 and the native research are CPU-only.

## 9. Honest limits

- **Natives are reimplemented, not ported.** Names and call sites say most of it; for the rest the arm64 is readable.
  Expect behavioural drift in AI and timing until each native is checked against the executable.
- **Action trees are the long pole.** 717 trees drive player moves, AI, NIS scenes and props; their node graph is only
  partly mapped. Without them missions run but NIS scenes and fights are approximations.
- **The original hand-made content (voices, music, cutscene animation) is reused from the owner's install at runtime**,
  never committed. A shareable build would need replacements; this plan targets the owner's own machine.
- The `Wx` key string itself was not identified (the seed was brute-forced); irrelevant for decoding, noted for
  completeness. Lua-side local variable names are gone for good (stripped).
- Not checked: whether every AE mission equals SE (only 1_01 compared), `.mxd`, `SpeechDB.bin` in AE, `act.img` node
  semantics beyond strings, Multiplayer (AE 2-player) scripts.

Scratch prototypes (session scratch dir `.../scratchpad/story/`, not in the repo): `lua50.py` (chunk reader +
disassembler), `decomp.py` (pseudo-decompiler), `survey.py` / `cover.py` / `xref.py` / `mbank.py` (census, coverage,
cross-references, mission bank), `wxdec.py` + `bf3.c` (cipher + seed brute force), `natives.py` (native table
extraction), `selfcheck.py` (asserts: 532 chunks parse, 1_01 decompiles, 6,931 `Wx` files decode).
