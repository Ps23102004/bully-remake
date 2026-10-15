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
