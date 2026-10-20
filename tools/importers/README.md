<!-- OWNER: builder-A (importers) -->
# tools/importers

Converts content from your own installed copy of *Bully: Scholarship Edition* into open
intermediates (glTF 2.0, PNG, JSON). Python 3 standard library, plus numpy (fast DXT
decoding, preview renders) and Pillow (preview only) when present. The game install is
opened read-only. Output goes to `--out` (default `imported/`, git-ignored). Format
reference: `docs/re-notes/format-survey.md`.

## Install path

Set one of these. The path is never hard-coded.

1. `--install "<path>"`
2. `export BULLY_INSTALL="<path>"`
3. `tools/importers/local_config.json` (git-ignored): `{"install": "<path>"}`

## Commands

    python3 -m tools.importers world                      # academy: tschool, ischool, isc_*, zone_school1-6
    python3 -m tools.importers world --preset core        # tschool + ischool only
    python3 -m tools.importers world --ipb tschool tGlobal
    python3 -m tools.importers validate                   # structural check of every imported glTF
    python3 -m tools.importers preview tschool --view oblique   # CPU render to imported/previews/
    python3 -m tools.importers img list Stream/World.img --ext ipb
    python3 -m tools.importers img extract Stream/World.img tschool.ipb
    python3 -m tools.importers nif SC1b_bldg_main LOBBY01
    python3 -m tools.importers nft G_scBuilding           # or a path to a loose .nft/.txd
    python3 -m tools.importers ipb tschool ischool
    python3 -m tools.importers trigger                    # DAT/Trigger.img -> imported/triggers/day_one.json (missions 1_01 + 1_02 slice)
    python3 -m tools.importers trigger --list             # every .dat with point/path/trigger/POI counts
    python3 -m tools.importers trigger --files 1_01 --all # also per-file JSON and all.json (all 410 files, ~14 MB)
    python3 -m tools.importers ped PLAYER                  # Jimmy: skinned glTF + locomotion clips
    python3 -m tools.importers ped PLAYER --anims '*' --sheet ~/bully-remake-evidence/m3/sheet.png
    python3 -m tools.importers anim                       # list MAINPED.HXD animation groups
    python3 -m tools.importers anim C_Player              # clips: play duration, travel, speed
    python3 -m tools.importers ped GN_Boy01 --anims IDLE PLAYERWALK PLAYERRUN RUNFAST ...   # any student ped (same 36-joint skeleton)
    python3 -m tools.importers audio survey               # .SEA sound banks + PLAYLIST streams: counts, codecs
    python3 -m tools.importers audio list FeetAll         # wave names, durations, rates
    python3 -m tools.importers audio extract FeetAll Wind # MS-ADPCM -> 16-bit WAV in imported/audio/<bank>/ + manifest.json
    python3 -m tools.importers audio extract Ambs --names 'NewSchoolOutdoor_F|Birds01'   # streamed banks (audio/PLAYLIST), on request

Tests: `python3 -m pytest tools/tests` (synthetic fixtures). Set `BULLY_INSTALL` to also
run the integration tests against the real install.

## Output layout

    imported/
      models/<Model>.gltf + .bin   one node, one mesh per model; primitives per NIF shape
      textures/<TXD>/<Texture>.png mip 0 of each referenced texture (RGB, or RGBA if it has alpha)
      ipb/<stem>.json              raw placements, all fields, as stored
      scenes/academy.json          scene manifest (below)
      report.json                  counts, failures by category, size, wall time
      previews/*.png               only if `preview` was run

## Scene manifest (`scenes/academy.json`, format `bully-remake.scene-manifest/1`)

- `coordinate_systems`, `units`: the conventions below, written into the file itself.
- `scenes[]`: one per IPB: `name`, `kind` (`exterior`/`interior`), section counts, bounds.
- `placements[]`: `scene`, `index`, `model`, `model_id`, `gltf` (null if not converted),
  `visible` (false for `nogo_`/`walkable_`/`_nd` marker volumes), and:
  - `game`: `position`, `scale` (per axis), `rotation_xyzw_stored` (raw),
    `rotation_quat_xyzw` (conjugated, ready to use), `rotation_rows`.
  - `ue`: `location_cm`, `rotation_quat_xyzw` (pass to `FQuat(X, Y, Z, W)`),
    `rotation_rows`, `scale`.
  - `unknown`: `unk68` (equals a per-file area code: 0 outdoors, 2 ischool, 15 isc_clas, ...;
    INFERRED), `unk112`, `unk116`.
- `models{}`: per model `gltf`, `txd`, `ide_id`, counts, `bounds_game`, warnings, missing
  textures, `sanitized_nonfinite`.
- `textures[]`, `texture_fallbacks[]`, `failures[]` (category, item, error).

## Coordinates

| Space | Handedness | Up | Unit | From game (x, y, z) |
|---|---|---|---|---|
| game (NIF, IPB) | right | +Z | metre | |
| glTF files | right | +Y | metre | (x, z, -y) |
| UE | left | +Z | cm | (100x, -100y, 100z) |

UE's glTF importer maps glTF (x, y, z) to UE (x, z, y) and metres to centimetres, so an
imported model and a manifest `ue` placement share one frame. Models are in entity space:
the NIF root and model-node transforms are identity, because the game replaces the
model-node transform with the placement. Placement rule:
`world = R * (scale * v) + position`, R = transpose of the stored quaternion's matrix.

## Materials

Base colour, normal (tangent-space RGB) and glow textures map to glTF slots; the `_s`
specular map and all raw Gamebryo values are kept in `material.extras`. `extras.blend_mode`
is `opaque`, `masked`, `translucent` or `additive`. glTF has no additive mode, so additive
materials are written as `BLEND` and the UE step must read `blend_mode`. Vertex colours are
`COLOR_0`. They hold baked lighting (`vertex_color_mode` 2 = ambient+diffuse).

## Known limits / OPEN

- OPEN-8: `BBonusB`, `Barr01_Switch`, `BeerKeg`, `BirdBath` `.nft` were LF->CRLF converted
  (header line ends in CRLF, zero bare LF bytes); the inverse is ambiguous. They are skipped.
- Normal-map green channel convention (D3D vs GL) is not verified.
- TexDesc clamp/filter bit layout is inferred from Gamebryo docs (school data: all 0x3200).
- `nif`/`world` still export skinned shapes as static meshes in their stored pose; use `ped` for a
  skinned character (skeleton, JOINTS_0/WEIGHTS_0, inverse bind matrices, glTF animations).
- Texture transforms on slots are recorded but not applied.

## Skinned characters and animation (`ped`, `anim`)

Formats and confidence: `docs/re-notes/anim-formats.md`. `ped <MODEL>` writes `models/<MODEL>.gltf`:

- one glTF node for the model, under it the skeleton joints (every NiNode of the skeleton subtree, NIF
  file order, which is the `.agr` track order: 36 joints for a pedestrian, `Dummy` first, `ARROW` last);
- one mesh node per skinned shape, all sharing one skin (inverse bind matrices from NiSkinData), up to 4
  weights per vertex (Jimmy uses at most 3);
- one glTF animation per clip, named by the HXD (`IDLE`, `PLAYERRUN`, ...), times in seconds of the HXD
  play duration, `extras` with group, hash, key type, loop check and `root_motion`
  (`travel_game_m`, `travel_gltf_m`, `speed_m_s`, `mode`).

`--root-motion`: `inplace` (default; pelvis minus the ground-arrow travel, arrow held, the travel left to
the movement code), `raw` (as stored), `dummy` (travel on the root joint, for root-motion extraction).
Forward in every clip is game +y = glTF -Z; the bind pose itself faces the other way (see the notes).
`--sheet` renders a CPU contact sheet (bind pose + frames of every clip) as evidence.

## Anniversary Edition (iOS build): `world --ae-data`

Placements still come from the extracted `data_0` tree (`stream/world.img` IPBs, `objects/*.ide`; pass its root as
`--install`). Models and textures come from `data_1.zip` instead of World.img: `bully/<name>.msh` (`msh.py`),
`bully/<name>.tex` (`wtex.py`: PVRTC4, DXT, raw formats), material to texture by name (`ae.py`; the `.mtl` files are
encrypted). The zip is read in place, never extracted. The AE path is used when `--ae-data` is given or the install
looks like an AE tree (`ae.detect_ae`); otherwise the PC path runs unchanged.

    python3 -m tools.importers --install ~/bully-extracted/bullyorig world --ae-data /path/to/data_1.zip
    # --ae-data may be omitted: $BULLY_AE_DATA, local_config.json {"ae_data": ...}, or the stock /Applications path

Differences from the PC output: textures land in `textures/ae/<stem>.png` (one dictionary, decoded once each);
`convert_ae.py` writes the same glTF layout and coordinate mapping as `convert.py`. Skinned props are exported static.
Materials named `*glow*`, `*additive*`, `sunbeams` are marked `extras.blend_mode: additive` (INFERRED from names).
Failure categories: `msh_missing`, `msh_convert`, `msh_empty_in_source` (the shipped .msh has no geometry),
`msh_no_visible_geometry`. Not yet decoded: `.mtl` (3 academy materials without a diffuse texture render untextured),
tangents (dropped), the specular map (stem kept in `material.extras.ae`). Notes: `docs/re-notes/ae-*.md`.

**AE characters (`ped`, `ped_ae.py`).** On an AE install `ped <MODEL>` reads `bully/<model>.msh` (any case) and writes the
same `models/<MODEL>.gltf` layout as the PC path (same flags, joint names, clip names, `extras`), so
`tools/ue/run_import_character.sh` / `run_import_npc.sh` / `run_import_ambient.sh` work unchanged. The skeleton is the
node hierarchy stored in the .msh (PC names and order: 36 joints `Dummy` .. `ARROW`); the rest pose is the bind pose.
Clips come from the data_0 tree's `anim/` (same HXD/AGR format as PC). Notes: `docs/re-notes/ae-peds.md`.

    python3 -m tools.importers --install ~/bully-extracted/bullyorig ped PLAYER --anims '*' --root-motion inplace --head B_Buzz
    python3 -m tools.importers --install ~/bully-extracted/bullyorig ped DOG_Doberman --group QPED --anims '*'
    python3 -m tools.importers --install ~/bully-extracted/bullyorig ped RAT_PED --hxd RAT_PED --group RAT_PED --anims '*'
