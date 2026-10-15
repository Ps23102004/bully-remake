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
- Skinned shapes are exported in their stored bind pose, without skin weights.
- Texture transforms on slots are recorded but not applied.
