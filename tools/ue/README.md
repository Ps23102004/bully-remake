<!-- OWNER: builder-B (ue); M2 sections (pawn, doors, walk runner) by builder-B2 -->
# tools/ue

Turns the `tools/importers` intermediates (`imported/`, git-ignored) into Unreal assets under
`unreal/BullyRemake/Content/Imported/` (git-ignored) and assembles `/Game/Imported/Maps/Bullworth`.
Everything here is reproducible from a fresh clone, an owned copy of the game and UE 5.8.

    python3 -m tools.importers world              # 25 s, writes imported/
    tools/ue/run_import.sh                        # all 22 academy scenes (~6 min cold, ~25 s re-run)
    tools/ue/run_import.sh --scenes core          # tschool + ischool only (~2.5 min cold)
    tools/ue/run_shots.sh                         # 3840x2160 Lumen PNGs -> ~/bully-remake-evidence/m1/

The editor module must be built once (`Build.sh BullyRemakeEditor Mac Development -project=...`):
the shot runner is C++ (`Source/BullyRemake/BullyShotRunner.*`).

## Engine safety (owner rules)

Both wrappers source `_common.sh`, which refuses to launch while an Unreal process is stuck exiting
or a system dialog is on screen, wraps every launch in `~/.cache/gpu-slot/bin/gpu_slot.sh capture`
(shared slot, global cap 2) and reaps this project's CrashReportClient afterwards. No retries:
a failed run is reported, never relaunched.

- Import: `UnrealEditor-Cmd -run=pythonscript -nullrhi -unattended -nosplash` (commandlet, no GPU,
  no window; the ModelContextProtocol plugin skips auto-start in commandlets).
- Shots: `UnrealEditor-Cmd <map> -game -RenderOffScreen -unattended -nosplash`. On Mac,
  `-RenderOffScreen` makes Slate create a generic (non-native) window, so nothing reaches the screen.
  `winlist.swift` (compiled into `Saved/BullyTools/`) polls the on-screen window list every second
  for the engine process tree; any sighting is logged to `<out>/window_watch.txt` and the engine is
  sent SIGTERM. An empty `window_watch.txt` is the evidence that no window appeared.

## import_content.py

Configured through environment variables set by `run_import.sh` (`BR_IMPORTED`, `BR_SCENES`,
`BR_STEPS`, `BR_LIMIT`, `BR_REPORT`). Steps:

1. **textures** — every PNG referenced by a selected model's materials ->
   `/Game/Imported/Textures/<TXD>/<name>`. Role decides settings: `normal` -> TC_Normalmap, linear;
   `gloss` (`_s`) -> linear; `base`/`glow` -> sRGB. Normal green channel is not flipped (Gamebryo is
   D3D-convention like UE; unverified, see importer README).
2. **materials** — `MPC_Bully` plus 8 master materials `M_Bully_{Opaque,Masked,Translucent,Additive}`
   `[_TwoSided]` (built by script, versioned by `MASTER_VERSION`). No static switches: absent maps
   fall back to engine default textures, so every instance shares its parent's shaders.
3. **meshes** — each glTF is staged into `Saved/BullyImport/stage/` with its materials renamed
   `M000..` and its image references removed, then imported with **Interchange** (UE 5.8's glTF path)
   through an override `InterchangeGenericAssetsPipeline`: static mesh only, Interchange material and
   texture import off, source normals kept, MikkTSpace tangents, vertex colours imported, Nanite off
   (low-poly 2006 meshes), no lightmap UVs. Slot `M###` -> glTF material ### -> one
   `MaterialInstanceConstant` per unique parameter set (`/Game/Imported/Materials/Instances`).
   Collision: complex-as-simple. Staging makes slot binding unambiguous (45 glTFs reuse material
   names).
4. **level** — `/Game/Imported/Maps/Bullworth` (non-World-Partition). Every `visible` placement with a
   converted model becomes a `StaticMeshActor` at the manifest `ue` transform
   (`location_cm`, `FQuat(rotation_quat_xyzw)`, per-axis `scale`), labelled `<scene>_<index>_<model>`,
   foldered by scene and tagged `br_scene:` / `br_model:`. Lighting rig: movable directional sun
   (atmosphere sun), sky atmosphere, real-time-capture sky light, exponential height fog, unbound
   post-process volume, a PlayerStart outside the front gates. Lumen GI + reflections come from
   `Config/DefaultEngine.ini` (`r.AllowStaticLighting=False`: no lightmaps).

Idempotence: textures and meshes carry metadata tag `br_src_sha1` (source bytes + role/version); equal
hash -> skipped. Instances are keyed by a hash of their parameters. The level is cleared and rebuilt
every run. Report: `Saved/BullyImport/report_<stamp>.json` (+ `report_latest.json`), log beside it.

### Material model (approximations, documented on purpose)

| Gamebryo data | UE |
|---|---|
| `blend_mode` opaque / masked / translucent | BLEND_Opaque / Masked / Translucent (surface per-pixel lighting) |
| `blend_mode` additive (SRC_ALPHA, ONE) | BLEND_Additive, **Unlit**, emissive = rgb * alpha * EmissiveGain |
| alpha test ref (`alphaCutoff`) | scalar `AlphaCutoff`; mask = alpha - cutoff + 0.5 against a fixed 0.5 clip |
| alpha blend AND test (e.g. wall-ivy decals, ref 4/255) | masked with cutoff raised to 0.5 (a 4/255 test shows the soft alpha's black texels) |
| alpha | base.a * COLOR_0.a (* material alpha via `DiffuseTint.a`, unused for now) |
| `COLOR_0` (baked ambient+diffuse lighting) | base colour *= lerp(1, COLOR_0.rgb, `MPC_Bully.BakedVertexLight`), default 1.0 |
| diffuse colour | vector `DiffuseTint` multiplies base colour |
| `_s` gloss map (imported sRGB, Color sampler) + specular colour | Specular = 0.35 + gloss.r * SpecularScale (0.25 + 2*spec); Roughness = 0.85 - 0.35 * gloss.r |
| glow map | emissive = glow.rgb * GlowGain |
| `doubleSided` (stencil) | `_TwoSided` parent; mesh distance field built two-sided |

`BakedVertexLight` is a single global knob because the vertex colours were lit for the original
renderer: multiplying them under Lumen darkens occluded areas twice. Tune it in `MPC_Bully` (no
re-import needed). Texture transforms, skinning and the `_s` map's exact meaning remain open
(importer README "Known limits").

## Shots (`run_shots.sh`, `shots_m1.json`, `UBullyShotRunner`)

`UBullyShotRunner` is a GameInstance subsystem that exists only when `-BRShotFile=` is passed. It
waits until the asset (texture / distance field / mesh) and shader compile queues have been empty
for 3 s (uncooked `-game` compiles on demand; cap `max_compile_wait_seconds`), warms up, then per
shot: spawns/moves one camera actor (optional downward trace so `eye_height_cm` is above the ground),
waits `settle_seconds` for Lumen and auto exposure, and writes `<out>/NN_<name>.png`. With
`"highres_size"` it uses the viewport's HighResShot at exactly that size (needed on this Studio: the
viewport is clamped to the largest display, 3840x1920 here); without it, `FScreenshotRequest` grabs
the viewport.

Known: re-keying instances (e.g. the alpha rule above) leaves the old `MI_*` assets unreferenced in
`Instances/` (harmless; delete `Content/Imported` for a clean rebuild). `shots_result.json` records the final camera transforms and trace results.

## Milestone 2: walkable campus, pawn, doors (`run_walk.sh`)

The game boots straight into `/Game/Imported/Maps/Bullworth` (`Config/DefaultEngine.ini`: default map,
`GlobalDefaultGameMode=/Script/BullyRemake.BullyGameMode`, Enhanced Input classes in `DefaultInput.ini`).

**Collision (Phase 0).** Every imported static mesh is complex-as-simple (`ensure_complex_collision`, re-applied
to already-imported meshes on every run): the render triangles are the collision for pawn capsules, camera
probes and traces. The manifest's `NOGO_*` / `WALKABLE_*` / `FGRD_ND_*` placements have no model data
(`gltf: null`, not in `models{}`; they are area/AI markers, `Coll/` only holds `ColAreas.cfg` here), so there is
nothing to import from them yet; parsing the game's own collision data replaces this later.

**Pawn** (`BullyPawn`, C++, no asset dependencies; input mappings are built in code): third-person capsule +
engine-primitive body, Enhanced Input WASD + mouse, left stick + right stick, Space / pad A jump, hold Shift /
L3 run. PLACEHOLDER tuning (real numbers come from the Act.img locomotion notes):

| value | placeholder |
|---|---|
| capsule | radius 30 cm, half height 90 cm (1.8 m) |
| eye / camera pivot | 160 cm, boom 300 cm, over-the-shoulder offset (0, 55, 10), FOV 75 |
| walk / run | 160 / 450 cm/s (stick magnitude scales analog) |
| jump | 420 cm/s up (~0.9 m), air control 0.3 |
| step height / walkable slope | 45 cm / 50 deg |
| accel / braking / friction | 2048 / 2048 / 8, yaw rate 540 deg/s |

The PlayerStart sits above the front gates (UE 32200, 7300, z 1200); the pawn snaps to the first surface below.

**Doors** (`ABullyDoorLink`, data in `door_links.json`, placed by the `level` step of `import_content.py`).
Each link = a pawn-only overlap trigger box in the doorway + an exit point (location/yaw) + `LinkId`/`PartnerId`.
Walking into the trigger fades to black (0.25 s), teleports the pawn to the PARTNER's exit (floor-snapped with a
short complex trace, camera yaw set), fades in; 1.5 s re-entry cooldown and exits placed clear of their own
trigger prevent ping-pong. Shipped pair: `main_door_ext` (east facade of the main building tower, game
(210.4, -73.3)) <-> `main_door_int` (ischool lobby vestibule, game (-628.6, -329.0)). More doors = more JSON
entries, no code. Interiors stay in their distant pockets (the original game's own layout).

**Scripted run** (`run_walk.sh`, `walk_m2.json`, `UBullyWalkRunner`, active only with `-BRWalkFile=`): drives the
real pawn with movement input (so collision and the door triggers are exercised for real), steps: `walk_to`,
`wait_door`, `shot`, `face`, `teleport`, `trace_ground`, `jump`, `log`; writes PNGs and `walk_result.json` with
STUCK/TIMEOUT reports. Same safety envelope as `run_shots.sh` (GPU slot label `bully-doors`, offscreen, window watch).

## Milestone 3: Jimmy (`run_import_character.sh`, `BullyCharacter`, `BullyAnimInstance`, `walk_m3.json`)
<!-- OWNER: builder-B3 (jimmy) -->

    python3 -m tools.importers ped PLAYER --anims '*' --root-motion inplace     # skinned glTF + 439 clips
    tools/ue/run_import_character.sh                                            # ~25 s cold, ~10 s re-run
    tools/ue/run_walk.sh --script tools/ue/walk_m3.json --out ~/bully-remake-evidence/m3 --res 1280x720 --label bully-jimmy

**Import** (`import_character.py`, pythonscript commandlet, same envelope as `run_import.sh`, GPU slot label
`bully-jimmy`). Pass 1: the glTF (animations stripped, materials renamed `M000..`, no images) through an override
`InterchangeGenericAssetsPipeline` (skeletal only, bind pose = reference pose, physics asset on) ->
`/Game/Imported/Characters/Jimmy/SK_Jimmy`, `SKEL_Jimmy`, `PHYS_Jimmy`; slots bound to `MI_Player_0N_d` instances of
`M_Bully_Character` (the `M_Bully_Opaque` graph plus `bUsedWithSkeletalMesh`, kept separate so the level's masters
never compile skinned permutations). Pass 2: batches of 60 clips, `import_only_animations` onto `SKEL_Jimmy`, 60 Hz,
frame-snapped (HXD play durations are not frame multiples) -> `.../Jimmy/Anims/<CLIP>` (all 439). Interchange turns
spaces in joint names into hyphens (`Root L Thigh` -> `Root-L-Thigh`). Idempotent through `br_src_sha1` tags.
Verification in the report (`Saved/BullyImport/character_report_latest.json`): 36 bones; for 14 locomotion clips,
36 bone tracks each and the rotation change of 5 bones between t=0 and t=L/2 equals the glTF keys' (max mismatch
0.2 deg; a T-posed clip would read 0). Jimmy's hair: his starting haircut is the clothing item `B_Buzz` (a full head model with painted
buzz-cut hair, the game's own default), not part of `PLAYER.nif`; `ped PLAYER --head B_Buzz` wires it, any other haircut is one flag away
(polish1, `docs/re-notes/ped-heads-hair.md`).

**Character** (`ABullyCharacter`, replaces `ABullyPawn`; `[CoreRedirects]` in DefaultEngine.ini). Mesh yaw +90 turns
the clips' forward (game +y = UE -Y) onto actor +X (the bind pose faces the other way; only the clips matter).
Capsule r 30 / half-height 85 cm (bind pose 1.70 m tall). Falls back to the M2 primitive body if `SK_Jimmy` is
missing. Every number below is `Config=Game` in `Config/DefaultGame.ini` (`[/Script/BullyRemake.BullyCharacter]`,
`[/Script/BullyRemake.BullyAnimInstance]`): edit and re-run, no recompile.

Controls: default gait is **run** (full stick or any WASD key); partial stick (< 0.5 after the 0.15 radial dead zone)
or holding **Left Alt / Left Ctrl** walks; holding **Left Shift / L3 / pad X(Square)** sprints; Space / pad A jumps.
The console original charges sprint by tapping (`SprintIncrementCharge`, tired sprint) - not modelled.

| value | chosen | source / measured |
|---|---|---|
| walk | 2.05 m/s (analog 1.30-2.05) | PLAYERWALK root motion 2.05; action tree WalkBasic 1.3-1.8 m/s; planted-foot speed 1.79 |
| run | 3.61 m/s | PLAYERRUN root motion 3.61; tree 3.6-3.9; foot 3.52 |
| sprint | 8.01 m/s | RUNFAST root motion 8.01; tree 6.5-8.8; foot 5.08 (see below) |
| accel to speed | walk 0.2 s, run/sprint 0.333 s | GlobalLoco param 68 |
| stop | walk 0.3 s, run+ 0.333 s (constant decel) | param 72 |
| turn rate | walk 573, run 315, sprint 344, air 46 deg/s | param 44 (10 / 5.5 / 6 / 0.8 rad/s) |
| jump apex | 1.10 m (vz 464 cm/s at g 9.8) | LocomotionJump param 24 = 1.10 (INFERRED as height); gravity OPEN, UE default |
| air control | 0.15 | air nodes steer at 0.8 rad/s |
| fall damage | > 5 m drop: log + PLAY_LAND_HURT (stub) | Fall_Damage condition DistToGround > 5.0 |

Clip speeds were chosen (task rule) so the gait speeds equal the animations' authored travel. **Measured caveat:**
the ARROW travel is not the foot-lock speed. FK on the clips gives the planted foot's backward speed relative to the
body: walk 1.79, run 3.52, sprint 5.08 m/s; with `--root-motion raw` the sprint clip's planted foot slides forward
2.9 m/s in the authored data itself. The anim instance therefore syncs play rate to the foot speeds (`*FootSpeed`):
feet stay planted at every speed, and the legs cycle 1.15x (walk), 1.03x (run), 1.58x (sprint) the authored rate at the
gait speeds. The action tree's own sprint range (6.5-8.8 m/s) implies 1.28-1.73x, so the fast sprint cadence is
probably original; setting `SprintFootSpeed=801.2` restores the authored rate (with the authored slide). The tree and the
foot data both point to a 1.8 m/s top walk: `WalkSpeed=180` is the alternative if the critic finds the walk hurried.

**Animation** (`UBullyAnimInstance`, C++ only, no AnimBP asset; a custom `FAnimInstanceProxy::Evaluate` samples the
clips and blends them n-way). States:

- **Loco**: idle <-> PLAYERWALK <-> PLAYERRUN <-> RUNFAST by measured ground speed, anchors 0 / 2.05 / 3.61 / 8.01 m/s,
  idle fully out at 0.6 m/s. One shared phase (0 = left-foot mid-stance; per-clip offsets 0.744 / 0.591 / 0.510 measured
  by FK) advanced at speed / blended stride, so blended cycles keep their feet in step.
- **Jump**: IDLEJUMP standing (pelvis rise clamped to the reference height: the capsule already jumps), JOGJUMP_L/_R
  when moving (the foot ahead in the phase picks the side). Take-off part (to 0.10 s / 0.22 s) at rate 1, air part
  (to 0.58 s / 0.56 s) time-scaled to the predicted landing and held while airborne.
- **Fall**: FALL loop after 0.25 s off a ledge or 1.2 m below the take-off height. **Land**: the jump clip's own landing
  (rest of the clip), FALL_LAND when still, the JOGJUMP landing part fading into loco when moving, PLAY_LAND_HURT
  over 5 m. State changes cross-fade 0.15 s (fall 0.3 s, land 0.06 s); outgoing clips keep playing while they fade.

Not yet: start/stop clips (PLAYERRUN_START, RUNSTOP*), turn-in-place, strafing, sprint meter, crouch, climbs.
`BR_CHAR t=...` log lines (1 Hz) report speed, gait, anim state, layers and the **median horizontal speed of the
planted foot** (should be ~0; transitions and slopes add noise).

**Camera** (spring arm, `Bully|Camera`): distance 450 cm, look-at 140 cm above the feet, resting pitch -8 deg (lens
~2.0 m up, ~0.4 m above Jimmy's head), horizontal FOV 75, location lag speed 8 (max 80 cm), no rotation lag; centred
(no shoulder offset). Auto-follow: after 1.0 s without look input while moving, yaw swings behind the travel direction
at 120 deg/s (scaled by speed; not when running toward the camera) and pitch returns to -8 at 30 deg/s. The original's
values are OPEN (`CameraDefaultFOV`, FollowCam3Butes); no reference stills were available, so these are a GTA-era
eyeball, all `Config`.

**Doors** work unchanged (`ABullyDoorLink` now takes `ABullyCharacter`).

**Evidence runner additions** (`UBullyWalkRunner`): `walk_to` takes `"gait"` and `"cam": "follow"` (leave the camera to
the character); `action_shot` moves (target or yaw; `"gait": "none"` stands), optionally jumps, and on its trigger
(`time`, `apex`, `air`, `land`) pauses the game, waits `settle_seconds` and takes a high-res shot of the frozen moment
(`"cam": "lock" | "side" | "front" | "follow"`); triggers `none` / `none_land` move without a shot;
`record_start {"fps":30}` / `record_stop` switch to a fixed time step and dump one viewport PNG per frame, which
`run_walk.sh` encodes to `<out>/sequence.mp4` (keeping every 10th frame in `frames_keep/`).

## Milestone 4: signs of life (`run_import_ambient.sh`, `AmbientLoops.json`, `UAmbientSubsystem`)
<!-- OWNER: builder-B4 (ambient) -->

Students walking fixed waypoint loops on the campus, with footsteps and an outdoor ambience decoded from the game's own
audio. No schedules, no AI, no pathfinding: a loop is a list of points.

    # 1. importer side (git-ignored outputs in imported/)
    python3 -m tools.importers audio extract FeetAll
    python3 -m tools.importers audio extract Ambs --names 'NewSchoolOutdoor_F|Birds01'
    for m in GN_Boy01 GN_Boy02 GN_Asiangirl GN_Greekboy NDH3_Bucky PFH1_Seth; do
      python3 -m tools.importers ped $m --anims IDLE PLAYERWALK PLAYERRUN RUNFAST IDLEJUMP JOGJUMP_L JOGJUMP_R FALL FALL_LAND PLAY_LAND_HURT --root-motion inplace
    done
    # 2. UE side (needs /Game/Imported/Maps/Bullworth from run_import.sh; ~30 s)
    tools/ue/run_import_ambient.sh                  # students + audio; --steps students | audio
    # 3. evidence
    BR_EXTRA_ARGS="-BRAmbientCam=nerd_bucky:9,boy01:9" tools/ue/run_walk.sh --script tools/ue/walk_m4_sequence.json --out ~/bully-remake-evidence/m4/sequence --res 1280x720 --label bully-ambient

**Students** (`students[]` in `unreal/BullyRemake/Config/AmbientLoops.json`; IDE rows of `Objects/default.ide`): `GN_Boy01`,
`GN_Boy02`, `GN_Asiangirl`, `GN_Greekboy` (generic students), `NDH3_Bucky` (nerd), `PFH1_Seth` (prefect). All ped NIFs have
the same 36-joint skeleton and use the same C_Player clip bank (INFERRED: the faction `F_*` groups hold fight/ambient
clips, the locomotion tree uses C_Player). `import_ambient.py` runs `import_character.py` once per student, so each gets
its own `/Game/Imported/Characters/<Model>/{SK_,SKEL_,PHYS_}<Model>` and a 10-clip `Anims/` (a shared Jimmy skeleton would
force Jimmy's bone offsets on every body). `UBullyAnimInstance` now resolves its clips from the mesh's own folder.

**Walkers** (`AAmbientWalker : ABullyCharacter`): the same capsule, movement and C++ locomotion as Jimmy. Walks `Waypoints`
at `speed_cm_s` (walk clip: measured 2.05 m/s authored, set to 1.7-2.0 here), idles `idle_min..idle_max` s at each
waypoint, real collision with the level (M4 shipped them overlapping the player; polish1 made them solid for the player and yielding, see "Polish 1"). A walker that makes < 25 % of
the expected progress for 3 s skips its waypoint and logs `BR_AMBIENT STUCK`. `UAmbientSubsystem` (world subsystem, active on
`*Bullworth*` when the JSON exists; `-BRNoAmbient` disables) spawns them at BeginPlay and runs `BR_AMBIENT path-check`: a
ground trace per waypoint and a capsule sweep per segment (capsule lifted by the 45 cm step height) against the level.

**Loops** (UE cm, `[x, y, ground z]`): `plaza_ew` (4 pts, 45 m legs), `central_walk` (2 pts, out and back), `crossing`
(2 pts), `front_of_building` (raised front terrace, z 860), `gate_approach` (outside the gates), `inner_courtyard`.
Authoring aid: `tools/ue/run_ambient_probe.sh X0 Y0 X1 Y1 STEP [ZTOP ZBOTTOM]` (headless, `-nullrhi`) logs an ASCII
walkability map (`.` open, `,` sloped, `#` capsule blocked, `_` no ground) plus ground heights, from which the loops were
picked with >= 1 m clearance. The probe maps the whole courtyard (x 19000-31500, y 3000-12000) in ~2 min.

**Audio**: `import_ambient.py` imports the decoded WAVs as SoundWave assets (`/Game/Imported/Audio/<bank>/<name>`;
ambience waves `looping=True`). `UBullyFootstepComponent` (on every walker and on the player pawn) detects foot plants
from the ankle bones of the playing animation (a foot's height drops within 3.5 cm of the lowest ankle height seen),
plays a random one-shot (never the same twice in a row, pitch +-6 %, volume 0.85-1.0 x 0.8, linear attenuation 1.5 to 23.5 m)
and logs `BR_AUDIO step`. `AAmbientSoundActor` loops `NewSchoolOutdoor_F` (stereo school outdoor ambience) and `Birds01`
at the front courtyard (inner 25 m, falloff 90 m). Runs use `-NoSound` (no audio device), so the evidence for playback is
the log (`BR_AUDIO ambient loop play ... audioDevice=0`, one `BR_AUDIO step` per foot plant), not captured sound.

**Evidence camera** (`-BRAmbientCam=`, passed through `BR_EXTRA_ARGS`): `<student>:<seconds>` chases a walker (3.5 m behind-left,
chest height, FOV 70), `fixed:x:y:z:lookX:lookY:lookZ:<seconds>` holds a free camera; entries cycle. The walk runner's
`record_start/record_stop` and `shot` steps capture whatever the view target shows.

Known (M4; both fixed in polish1): walkers and the player passed through each other; all footsteps were the cement set. Still: no start/stop/turn clips (walkers turn in place on the C_Player
locomotion blend); `front_of_building` sits on a raised terrace the player reaches by stairs (not path-checked across levels).

## Milestone 5: THE SHOT (`lighting.py`, `run_lighting.sh`, `run_candidates.sh`, `shots_m5.json`)
<!-- OWNER: builder-B5 (shot) -->

    tools/ue/run_lighting.sh --variant B_raking_amber        # re-apply one variant to the saved map (~20 s, no GPU)
    tools/ue/run_candidates.sh [--variants a,b] [--out DIR]  # per variant: lighting -> 4 vantages @3840x2160 -> sidecars (~2.5 min each)

The lighting rig moved out of `import_content.py` into `lighting.py` (`import_content` calls `build_rig`; the
same module re-runs standalone). All numbers live in `lighting_variants.json` (3 variants: `A_classic_gold`,
`B_raking_amber`, `C_rose_haze`; sun elevation/azimuth/intensity/temperature, sky, atmosphere, fog, EV100,
white balance). Azimuth is the world yaw toward the sun (0 = +X; gates and facade face +X).

Debts fixed:

- **Auto exposure -> locked.** The unbound post-process volume sets auto-exposure method Basic with min = max
  brightness = `exposure_ev100` (EV100 because the project uses `ExtendDefaultLuminanceRange`), bias 0. Every frame of a
  variant has the same exposure (A 11.8, B 11.6, C 12.0), so rounds are comparable. Sun is physical (90-120 klx).
- **Interior overexposure.** All interior pockets (scenes `isc_*`, `ischool`: 1867 actors, every one far from the campus
  at x < -40000) are on *lighting channel 1 only*; the sun stays on channel 0, so it neither lights nor shadows them.
  A private directional "interior key" (channel 1 only, 30 klx, pitch -78, 10 deg source angle, 4600 K) lights them to the
  same EV as the exterior. Chosen over a per-pocket bounded exposure volume: one global exposure, nothing pops at door
  transitions, and python cannot build a volume brush in the commandlet anyway. The open top of the lobby is no longer a
  white void. Sky-light ambient can still reach open-top interiors (sky light has no channel mask); it is now a minor fill.
- **Fake-mirror floor.** `LM_lobbyflr` (materials `TransFloor`, `TransFloor Dark`) was a translucent tile layer over
  `reflect_LOBBY`, an upside-down lobby copy 9.5 m below. Documented choice: the floor material is forced **opaque**
  (`FAKE_MIRROR_FLOOR_MATERIALS` in `mat_key`; a new instance hash, no master rebuild) and `reflect_LOBBY` is **not placed**
  (`FAKE_MIRROR_MODELS`). Lumen supplies real reflections. Not touched: `GymFloor` (`TransFloorGym`) uses the same trick.

`shots_m5.json` holds the four fixed vantages (front gates, facade from the courtyard, path with oaks, lobby); the
sidecar writer `shot_sidecar.py` writes `NN_<name>.json` beside every PNG (variant settings, camera, resolution,
warm-up/settle seconds, commit). Determinism: same map, 20 s warm-up + 12 s settle, no auto exposure, static sun; a repeat
render differed by 0.2 to 0.9 / 255 mean absolute (foliage noise only). `run_shots.sh` now makes `--shots`/`--out` absolute
(the engine runs from another cwd; a relative path left an idle engine running).

### Round 2 (critic CRITIQUE_R1 fixes, winner `B_raking_amber`)
<!-- OWNER: builder-B5 (shot), round 2 -->

    tools/ue/run_candidates.sh --variants B_raking_amber --out ~/bully-remake-evidence/m5/round2   # 3840x2160 + sidecars
    python3 tools/ue/measure_shots.py DIR [--sheet s.jpg]                                          # luma medians / near-black / window glow
    tools/ue/run_probe.sh --mode scene --near "x,y,z,r" --mats "," --out p.json                    # actors/materials near a point (no GPU)
    tools/ue/run_probe.sh --shot NAME --rects "x0,y0,x1,y1;..." --out p.json                       # actors projecting into pixel rects of a shot

`default_variant` is now B. New in `lighting.py` / `lighting_variants.json`:
- **Clouds**: `VolumetricCloud` with `MI_BullyClouds` parented to the engine's `m_SimpleVolumetricCloud_Inst` (the base material alone renders nothing:
  the Inst carries the noise/pattern textures). B: coverage 0.15, density 0.2, scale 256, layer 1.6-4.6 km.
- **Shade lift**: B sky light 1.4 -> 5.0 with a warm lower hemisphere (ground bounce) instead of black.
- **Backlit foliage**: leaf/branch/vine card MIs (masked, ~43) get `Glow` = base colour and `GlowGain` 1500 (emissive = albedo*gain, a transmission
  cheat that needs no master-material change).
- **Lobby**: interior key now soft (14 klx, no shadows, 5 deg); three rect lights (back windows, both sides); the `*Night` window twins (coplanar with the day
  panes, z-fighting to grey) hidden; day-window MIs emissive (gain 14000); floor MIs roughness 0.22 / specular 0.6.
- **Shot hides** (`shot_hides`): actors set hidden-in-game for the hero shots (see MEASUREMENTS.md in the round-2 evidence).
- Birds: `build_birds` exists but is disabled (count 0); lit flat cubes read as debris.
- Cameras (`shots_m5.json`): gates x 33200, eye 5.6 m, FOV 57; facade from x 28800, FOV 68, pitch ~12 deg; path panned and tilted up, FOV 66; lobby from the far wall, FOV 72.

## Milestone 6: proof capture (`walk_m6_proof.json`, `place_doors.py`, `run_place_doors.sh`)
<!-- OWNER: builder-B6 (proof) -->

    tools/ue/run_walk.sh --script tools/ue/walk_m6_proof.json --out ~/bully-remake-evidence/m6 --res 1920x1080 --label bully-proof
    # -> 00_first_frame_4k.png (3840x2160 opening vantage), sequence.mp4 (30 fps fixed step, 90.0 s, 2700 frames)

One continuous recording of real gameplay: street outside the gates, walk to the arch, run through the gates and
courtyard with one running jump, walk the central path, run to the building, walk up the steps, door, entrance-hall
loop, door back out, run across the courtyard. All movement is `walk_to`/`action_shot` with `"cam": "follow"` (the
character's own camera); ambient walkers stay on. The route as landmarks and timings (for driving the same route in the
original game) is `docs/re-notes/proof-route.md`.

Preconditions the run needs, both silently missing in a bad checkout: `/Game/Imported/Characters/Jimmy/SK_Jimmy` (else
the pawn is the M2 placeholder body: `BR_CHAR Jimmy mesh MISSING` in the log; fix `run_import_character.sh`) and the
two `ABullyDoorLink` actors in the saved map (else `wait_door TIMEOUT`; fix `tools/ue/run_place_doors.sh`, which
re-places `door_links.json` into the saved map in ~15 s, no re-import). A level rebuild while the editor module is not
built saves a map without doors (`door links skipped` warning in `import_*.log`).

## Polish 1 (`polish1`): collision, doors, hair, footsteps (`run_walk_nullrhi.sh`, `walk_polish_*.json`)
<!-- OWNER: polish1 -->

Logic is verified without a renderer: `tools/ue/run_walk_nullrhi.sh --script tools/ue/walk_polish_logic.json --out DIR`
runs the real game (`-game -nullrhi -NoSound`, same GPU-slot / window-watchdog envelope as the other runners, no window, no
GPU) and logs `BR_WALK`, `BR_DOOR`, `BR_AMBIENT YIELD`, `BR_AUDIO surface`. `BR_EXTRA_ARGS=-BRDoorTrace` adds one
`BR_DOOR_TRACE` line per frame of a door transition.

- **Player vs students.** Walkers are normal Pawn capsules, so the player is blocked by them. Walker <-> walker stays
  pass-through (every pair ignores the other while moving, `AAmbientWalker::BeginPlay`): two walkers meeting head-on on a
  shared loop would otherwise deadlock and the stuck logic would skip waypoints. Walkers yield to the player
  (`TickYield`): player within 190 cm and closing in (or touching) -> 0.35 s pause, then a 0.9 s side-step toward the free
  side, 2.5 s cooldown, yield time excluded from the stuck check. (A walker with its own collision channel was tried first
  and rejected: the level's meshes answer a new channel differently from Pawn and walkers froze at corners.)
  Test: `walk_into_walker` runs head-on into walkers on three loops and reports the closest centre distance (capsules
  touch at 60 cm; < 51 = interpenetration = failure).
- **Door transitions** (`ABullyDoorLink`): frame-driven state machine, FadeOut (smoothstep) -> one full-black frame ->
  teleport + camera cut + spring-arm lag drop -> hold black >= 0.12 s and >= 3 frames -> FadeIn. The timer version
  teleported on a timer tick that runs before the camera manager applied the last fade step, so the cut landed on a ~90 %
  faded frame and the new view restarted at ~87 %. The exterior seen through the entrance doorway from inside is the
  original's own backdrop (`DL_iSchoolEnt4`, SC_*Screen cards) lit only by the dim interior key: `lighting_variants.json`
  `interior.backdrop_glow` now makes those cards self-lit (gain 5000, an ESTIMATE; re-apply with `run_lighting.sh`); the
  lobby floor's window glints are toned down (`interior.floor` roughness 0.22 -> 0.45, specular 0.6 -> 0.35, also an estimate).
- **Hair**: `docs/re-notes/ped-heads-hair.md` (`ped PLAYER --head B_Buzz`).
- **Footsteps**: `docs/re-notes/audio-formats.md` section 6 (decoded cue graph, `Config/FootstepSurfaces.json`,
  `tools/ue/walk_polish_surfaces.json` maps the courtyard headless).
- **For the next rendering session**: `tools/ue/walk_polish_verify.json` (walk into a student, both door directions, footsteps on
  grass / path / sidewalk / lobby tile) -- needs a visible-quality run, NOT run by this task.

## Stairs and upper floor (`slice-stairs`: `run_stairs_check.sh`, `stairs_probe.py`, `gen_stairs_walk.py`)
<!-- OWNER: slice-stairs -->

Headless (`-game -nullrhi`, GPU-slot wrapped, no window) proof that the 1_01 route is walkable, gates to the principal's office and back:

    tools/ue/run_stairs_check.sh --tag mycheck        # ~85 s; logs BR_STAIRS path lobby->office / gates->door / pawn full route: PASS|FAIL
    BR_STAIRS_MODE=profile BR_STAIRS_PROFILE="-62860,30500,-62860,29700,5,400" tools/ue/run_stairs_probe.sh   # floor height along a line (risers)
    BR_STAIRS_MODE=sweep BR_STAIRS_SWEEP="x,y,zfoot,x2,y2" tools/ue/run_stairs_probe.sh                       # what blocks one capsule sweep

`stairs_probe.py` runs inside the running game (`-ExecCmds="py ..."`; `EditorLevelLibrary.get_game_world()` segfaults in `-game`, the
map's UWorld is found with `find_object`). It flood-fills 8-neighbour capsule steps (r30, hh85, step 45, slope 50 deg: the
`BullyCharacter` numbers; lift by step height, sweep forward, settle down) over the level's runtime complex-as-simple collision, so
it sees what the pawn sees. Output: JSON + ASCII walkability trace per run, plus `BR_STAIRS check <name>` for extra points.
`gen_stairs_walk.py` turns the probe path into a `walk_runner` script (`walk_stairs_*.json`) so the REAL pawn is driven along it.

Findings (2026-10-15): there is no collision gap. The central stairs are a switchback: 8 risers of 20.6 cm (floor 0 -> 165 cm), a
landing whose back wall (`ischool_103_BULL2_Banner04`, y=29726) ends a straight walk, then two mirrored flights (west and east,
3 m wide, 165 -> 550 cm) to the upper floor, which holds the office trigger (-63320, 29140, z 550) and `PrincipalRoom`. The M2
"landing stop" was `walk_m2.json` walking straight on to (-62860, 29500) into that wall; its target is now on the landing. Pawn
passes lobby -> office -> out at walk, run and sprint gait; risers (20.6 cm) are well under `MaxStepHeight` 45.

## AE render check (2026-10-20): characters, grass, ray tracing

    tools/ue/run_lineup.sh            # temporary line-up of every imported character in the saved map (tag br_lineup)
    tools/ue/run_shots.sh --shots <shots.json> --out DIR    # e.g. vantages at game x 333..345, y -73 (road east of the gates)
    tools/ue/run_lineup.sh --remove   # always remove it again (run_import.sh's level step also clears it)

- Characters: Jimmy (`run_import_character.sh`), the six AmbientLoops students (`run_import_ambient.sh --steps students`;
  the audio step fails on the AE because `imported/audio/manifest.json` does not exist: AE audio is `.ses` banks,
  not the PC `.SEA` the decoder reads), the npc_plan peds (`run_import_npc.sh`, needs `BULLY_INSTALL`), `GN_Bully05`
  with all 439 clips, and the animals `DOG_Pitbull`/`DOG_Doberman`/`DOG_Pitbull2`/`DOG_Pitbull3`/`RAT_PED`
  (`run_import_character.sh --model M --name M`). Animals have no gameplay consumer yet; `lineup.py` is the only place
  they are spawned.
- Grass (`import_grass.py`): HISM components are added through `SubobjectDataSubsystem` (`Actor.add_component_by_class`
  is not exposed to editor Python in 5.8); the masked masters get the InstancedStaticMeshes usage flag (without it every
  tuft drew the engine's grey checker material); tuft cards are single-sided with both windings (a two-sided material
  flips the up normal on the back face and those cards rendered black).
- Hardware ray tracing (UE 5.8, M3 Pro, METAL_SM6): `r.RayTracing=True`, `r.SkinCache.CompileShaders=True`,
  `r.Lumen.HardwareRayTracing=True` works - no crash, no errors, log `Ray tracing is enabled (dynamic)`, ~4 min of
  extra shader compile. Exteriors and the lobby match or beat software Lumen, but the hallways drop from mean
  luminance 91 to 40 and the gym from 160 to 91: the interior rig (unshadowed channel-1 key + Lumen bounce) relied on
  software Lumen's distance-field light leakage. `r.RayTracing.Shadows=True` darkened the gate wall and courtyard,
  and `r.Lumen.HardwareRayTracing.LightingMode=2` changed nothing indoors. The project stays on software Lumen until
  the interior lighting is re-balanced for HWRT (evidence `~/bully-remake-evidence/ae/hwrt_test/`).

## Engine fixes, lane E (2026-10-20)

All verified by `tools/ue/run_physics_smoke.sh` (`-BRPhysicsSmoke`, 65 checks) plus the regular smokes.

- Door placement (`import_content.select_door_links`): a link is spawned only if its own interior scene and its partner's are imported
  (`--scenes core` no longer spawns 113 doors into the void; skipped ids are logged); triggers use the oriented box + yaw.
- Ragdoll vs teleports: `UBullyRagdollComponent::Abort()` runs before every teleport (door, mission `TeleportPlayer`, `BRGo`, sandbox, walk
  runner); a ragdoll also drops itself if the capsule is found > `ExternalMoveTolerance` (60 cm) from where it put it (any other teleport).
  Possessing a ragdolled pawn (brawl AI) puts the movement component back to Falling; the ragdoll tick now keeps it off.
- Recovery: Jimmy gets up `PlayerKOGetUpSeconds` (4.5 s) after a KO (mission retry resets him sooner); a stagger that threw the body ends in
  a get-up (`bOnStagger` is safe to enable).
- `ABullyDoorLink`: releases only an input lock it took; a shared interior door returns to the door the pawn came in by
  (`ABullyCharacter::LastEntryDoor`); only mission triggers the traveller touched are force-entered (`AMissionTrigger::TouchesSegment`);
  `r.LumenScene.FastCameraMode` is ref-counted and restored on timer or EndPlay.
- `[ConsoleVariables] p.Chaos.Collision.OneSidedTriangleMesh=0` (DefaultEngine.ini): simulated bodies hit the single-sided room shells from both sides.
- Bystanders (`AAmbientWalker`, `[/Script/BullyRemake.AmbientWalker]`): a student's first hit makes idle students within 12 m flee or join by
  student-id rule (nerd/girl flee, boy/prefect join); mission NPCs never react. Students do not fight each other (only the player).

## Whole world in the map, stage 2 (2026-10-20)

    python3 -m tools.importers --install <bullyorig> world --preset all && python3 -m tools.importers.col --install <bullyorig>
    tools/ue/run_import.sh --scenes all          # 83 scenes, 2331 meshes + 129 COL meshes, ~41 min cold, ~30 s re-run
    tools/ue/run_import_grass.sh                 # always after a level rebuild (it drops the grass actors)
    python3 tools/ue/gen_teleport_spots.py       # Config/InteriorSpots.json (47 spots)

- Result (report `Saved/BullyImport/report_20261020-145539.json`): 0 errors, 0 warnings; 6495 placed, 129 COL blockers, 113 door links,
  3961 material instances, 3190 interior actors on lighting channel 1.
- **Sea** (`place_sea`): one flat plane at z 0 (`MI_BullySea`, opaque, roughness 0.12, no collision, no shadow/DF) over UE x -33500..300000,
  y -300000..300000, i.e. the outdoor map and the open sea; it stops short of the interior pockets (game x < -389). Placed only when a
  town scene is imported. Colour is an ESTIMATE (no reference render).
- **River/lake water** (`water_layer == "base"` from `world.patch_water_materials`): same dark colour as the sea, alpha 0.92, roughness
  0.08. With the master's default roughness 0.85 the untextured tint rendered as flat pale-cyan paint.
- **Interior lighting**: `lighting_variants.json` `interior.scene_prefixes` is now `["i"]`, so the town interiors (grocery, asylum, shops,
  hide-outs, ...) get the channel-1 isolation and interior key like the school rooms; outdoor scenes start with `t` / `zone_`.
- **Camera**: `ABullyCharacter` capsules ignore `ECC_Camera` (a student walking between the lens and Jimmy collapsed the spring arm onto
  his back).
- `gen_teleport_spots.py` matches scene names case-insensitively (AE manifest names are lower case; girls' dorm, BMX and the four
  hide-outs were silently skipped).
- **Principal's office door** (`doors.py` `EXIT_FALLBACK`): the inner door is now the double door in the secretary room's north wall
  (game (-701.25, 217.4, 31.55), arrival 3.5 m straight into the room). ExitLocs `Principal 0` lies 13 m outside the `princoff` shell, so the
  old arrival dropped the pawn into the void (found by `run_stairs_check.sh`, pawn at z -800000). `walk_stairs_full.json` now goes through
  that door and back out (the office is a separate pocket, as in the original).
- **Door retry** (`ABullyDoorLink::OnTriggerBegin`): a use refused by the 1.5 s re-entry cooldown is retried every 0.25 s while the player
  still overlaps the trigger. An overlap begins only once, so walking straight back into a doorway right after arriving left the
  player standing in it.
