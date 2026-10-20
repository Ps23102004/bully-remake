<!-- OWNER: builder-A (importers) -->
# AE (Anniversary Edition) grass, ground textures and seasons

Status: 2026-10-20, branch `ae-importers`. Code: `tools/importers/convert_ae.py` (`emit_grass`, `ensure_tufts`),
`tools/importers/ae.py` (`MATERIAL_FIXES`, winter handling), `tools/ue/import_grass.py` + `run_import_grass.sh`.
Tests: `tools/tests/test_convert_ae.py`, `test_ae.py` (synthetic). All numbers below come from scratch scripts over
the real `data_1.zip` / extracted `data_0`, outside the repo. Confidence tags: **VERIFIED** (measured on the data),
**INFERRED** (fits, not proven), **OPEN**. The UE side has **not been run** (no Unreal access while writing it).

## 1. How the AE draws grass

| Finding | Evidence | Confidence |
|---|---|---|
| The grass is generated at run time; no blade geometry is shipped. | Every `DL_GrassNN` (10), `RC1a_GrassNN` (6), `BU1a_GrassNN` (4), `IN_GrassNN` (4) `.msh` is the same stub: 1 triangle, 3 vertices, area 0, at `(2.5, -29.6, -1.6)`, material `mat_grass_x_dl_grass03`, with UVs that span `7.1..11.3 x -0.3..4.8` (values meant for a large mesh). | VERIFIED (stub), INFERRED (run-time generation) |
| One marker per ground tile. | `dl_grass01..10` are placed at the centres of `fgrd_sc1b01, 04, 06, 10, 11, 17, 18, 22, 29, 30` (distance 0.0 to 24.8 m; the other tiles `02`, `19`, `31`, `cice`, `b01_a` have none, nearest marker >= 57 m). `dl_grass` Z is +0.1 m over the tile. | VERIFIED |
| The marker's texture pair is a tiling blade mask, not a colour. | `grass_x_d` is near black (PVRTC, no alpha); `grass_x_n` is a 256x256 normal map of small round bumps (one per blade tip). Reads as a fur/instance mask. | INFERRED |
| Short lawns are the ground texture plus that layer; tall tufts are separate alpha cards. | Alpha cards exist: `rc_grassz_swap_d` (olive clump, 256x128), `rc_grasstripz_swap_d` (blade strip, 512x64), `te03_grass3_d` (dry tall grass), `veg_hanggrassz`, `pr_grass1_swap`, `te03_grass1`; `rc_grassz_swap_d_wi` is the white winter version. They are used by props (`dpe_flowersa/b`, `tallfirs`, `treeclimbgrass`, `tgk_grassy`, `ifreakgrass`), not by the lawns. | VERIFIED (use), INFERRED (the lawn layer) |
| `lawnplacment/Mower_Placement.txt` is only the lawn-mowing minigame. | Three rectangles `MOWER1..3` (`TL/TR/BL/BR` x,y + `Z`) plus two player starts each, all near game x = -684..-749 (the prep-school district). The mowed look is `lawnmowgrass_d/_n` (+`effects_grassmowed`) on the real meshes `rc_lawnmowgrass` (68 tris, 275 m2) and `rc_lawnmowgrass2`. It does not describe grass drawing. `config/*` has no grass entry (grep of data_0 for `grass0N` only finds the model names in `world.img` and `*.ide`). | VERIFIED |
| The shader parameters are unreadable. | `.mtl` and `.xml` are encrypted (section 5 of `ae-materials.md`); a serious break attempt did not finish. | OPEN |

Consequence: faithful = ground textures right + a blade layer over the same tiles. The importer therefore scatters
cards (below). The look of the original blades is not recoverable without the `.mtl`.

## 2. Seasons: `_wi` and `_in_grass0N` are SNOW, the plain texture is autumn

The archive keeps seasonal variants side by side under one family name (`_swap` marks the swappable families).

| Suffix | Meaning | Evidence |
|---|---|---|
| `<name>_wi` (222 files) | winter | `rc_grassz_swap_d_wi` is the same tuft in white; `sc_mainwall_swap_d_wi` has a snow strip on top. VERIFIED by eye |
| `<name>_d_in_grass02` / `_in_grass04` (92 files) | snow-covered version, named after the model it was baked for (`IN_Grass02`, the industrial grass patch) | `zcn_strata_swap_d_in_grass02` is the cliff rock under snow; `zrc_mosspath..._in_grass02`, `zrc_cobble..._in_grass02` likewise. VERIFIED by eye |
| `z` prefix | a second texture set of the same family; not consistently darker or lighter (20 z/non-z pairs: grasse 176 vs 57 mean, cn_strata3 83 vs 108). Meaning OPEN | |

The old resolver (`ae.py`, first commit) took the first spelling that existed, so the school's cliffs
(`mat_zcn_strata_swap*`, 93k m2 summed over all placements), paths and lawn strips came out white (top preview before:
white rings and paths). Fixes, each recorded in `ae.MATERIAL_FIXES` / `_resolve` with its evidence:

* A winter-only variant now loses to the plain autumn texture of the z-toggled spelling:
  `mat_zcn_strata_swap -> cn_strata_swap_d` (`cn_strata_swap_s` correlates with it at r = 0.97),
  `zrc_bikepath_swap -> rc_bikepath_swap_d`, `zveg_bush3z_swap -> veg_bush3z_swap_d`: 3 families, 6 material names.
* `mat_zrc_grasse_swap` (the main lawn material, 24.9k m2 incl. instances) resolved to `zrc_grasse_swap_d`, a white
  snow texture (mean 173,174,185). Forced to `rc_grasse_swap_d` (green, dark soil fringe at the top; seamless
  0.95 seam/interior ratio) because its normal map correlates with that texture (r = 0.40) and not with the white one
  (0.03). Normal set to `zrc_grasse_swap_n_in_grass04`, the only one shipped.
* `mat_zrc_mosspath_swap` has only a snow diffuse; `zrc_mosspath_swap_s` correlates with `fun_gravedirt1_d` at
  r = 0.81 (dark soil), used instead.
* Still the winter file because no autumn one exists: `sc_mainwall_swap_d_wi` (pebble-dash wall, light grey, 13k m2)
  and `zrc_boulder_swap_d_in_grass02` (grey boulders, 0.7k m2). Both read as plausible grey stone.

The gate shot (`~/bully-remake-evidence/ae/gates/00_front_gates.png`) used the old choices; it needs a re-import to
show the corrected ground (the lawns beyond the gate were the white `zrc_grasse` strips).

## 3. Ground tiles

Academy ground = `fgrd_sc1b*` tiles (14), `ext_ground` (prep district), mounds/cliffs. Grass-class materials and
what they cover (up-facing triangles only, `n.z >= 0.8`), VERIFIED from the `.msh`:

| material | class | texture | area in the 10 grass tiles | UV | m per UV unit |
|---|---|---|---|---|---|
| `mat_zrc_grasse_swap` | edge | `rc_grasse_swap_d` | 23,954 m2 | u -7..7.6, v 0..2 (a band, not a thin strip: 6..12 m wide) | 4.3..5.8 |
| `mat_zrc_grasst_swap` | lawn | `zrc_grasst_swap_d` (green, 60,67,45) | ~9,000 | u -7..5, v -4.3..6.6 | 4..6.4 |
| `mat_zrc_grasseb_swap` | lawn | `zrc_grasseb_swap_d` (dark green + mud) | 352 | | 6.4 |
| `mat_zsc_bggrass_swap` | wild | `zsc_bggrass_swap_d` (yellow-green) | 700 | | 5..5.1 |
| `mat_te04_grass_01` | lawn | `te04_grass_01_d` (`ext_ground`, 21.8k m2, far district) | | u,v -12..12 | 8.4 |

Texture scale is therefore 4..8 m per 256 px repeat (1.6..3.3 cm/texel): correct for lawn, no UV rescale needed.
Seam test (edge-pixel difference over interior neighbour difference, 1.0 = seamless): `rc_grasse` 1.06/1.00,
`zrc_grasst` 1.03/0.85, `zsc_bggrass` 0.98/1.01, `zrc_grasseb` 0.82/1.9 (intended edge band), `te04_grass_01` 2.8/3.2
(visible seam, far district only). Macro variation is not in the data and not in the UE master material; the tuft
layer (random yaw/scale, per-tuft-type tint) is what breaks the uniformity. A UE macro-colour noise in the master
material would be the next step.

`pp_params` (`extrabrightness`, `extracontrast`, `gammamod`) in each `.tex` header are import-tool settings (e.g.
`extrabrightness=-0.1` on `zrc_grasse_swap_d`); the decoded pixels are used as shipped, they are not re-applied.

## 4. The scatter (importer) and the placement (UE)

`world` writes, next to the models:

    imported/grass/index.json                tuft assets (gltf, texture, mean colour) + one entry per ground model
    imported/grass/<model>.grass.json        per tuft id: mesh name, count, tint, flat rows of
                                             x_cm, y_cm, z_cm, yaw_deg, scale_pct (model-local UE space: (x,-y,z) of game)
    imported/grass/tuft_{clump,strip,dry}.gltf   two crossed vertical alpha cards, base at z=0 (+ textures/ae/*.png)

Rules (all in `convert_ae.py`, constants at the top of the grass section; seeded by the model name so a re-run is
byte-identical):

* Only materials in `GRASS_MATERIALS`; triangles with `n.z >= 0.8` (walls and steep cliffs get none); 2 cm sink.
* Density per m2 (before filters): lawn 1.6 clump + 2.2 strip; edge 0.8 + 1.6, and thinned by
  `smoothstep(0.12, 0.40, frac(V))` so the dark soil fringe (V ~ 0, top rows of `rc_grasse_swap_d`) stays bare; wild
  1.0 clump + 2.0 dry. Cards: clump 0.60 x 0.30 m, strip 1.20 x 0.15 m, dry 0.70 x 0.36 m, scale 75..130 %, random yaw.
* Tint per tuft type = ground mean colour over the blade texture's mean (brightness fully, hue by half, clamped
  0.55..1.35), so the cards sit in the ground's autumn palette instead of the card textures' fresh green.
* A model gets a file if it has >= 25 m2 of grass ground: 20 models, 4.4 MB, 204,414 instances in total.
  `tile: true` marks `fgrd_*` (12 files, 81,042 instances). The 10 tiles with a `dl_grass` partner hold **80,348** (23,954 m2 edge,
  9,461 m2 lawn, 700 m2 wild); the 8 non-tile models (mounds, cliffs, prep ground) another 123,372.

`tools/ue/import_grass.py` (via `tools/ue/run_import_grass.sh`, headless like the other scripts) reuses
`import_content.py`'s texture, master-material and Interchange mesh helpers to import the three tuft meshes, then
for each placement of a tile that has a `dl_grass*` placement within 25 m (`BR_GRASS_DL_M`; this selects exactly
the 10 tiles) spawns a `StaticMeshActor` at the placement transform with one HISM component per tuft id:
no collision, no shadow, no distance-field/Lumen contribution, cull fade 22..38 m (`BR_GRASS_CULL`),
`BR_GRASS_ALL=1` also places the mounds/cliffs/prep ground, `BR_GRASS_DENSITY` thins, `BR_GRASS_DRY=1` only imports
the assets. Re-run it after every `run_import.sh` (the level step rebuilds the map).

Unverified in the editor: `Actor.add_component_by_class`, `HierarchicalInstancedStaticMeshComponent.add_instances`,
`set_cull_distances`, the masked master material's look with `DiffuseTint` as the tint, and Lumen cost of ~80k
card instances (tris: 4 per card). Fallbacks (per-instance add, warnings naming the failed call) are in the script;
the first run may still need API-name fixes. Not done: wind (the `_wi`/`swap` families hint at a run-time swap),
mower-trimmed lawns (`Mower_Placement.txt` rectangles are in game xy and could mask the scatter), per-instance tone.

## 5. Open

* What the run-time grass layer really looks like (blade height, density, colour); needs the `.mtl` or a device capture.
* Meaning of the `z` prefix and of `_in_grass0N` beyond "snow variant baked for model X".
* Whether the game picks `rc_grasse` or `zrc_grasse` per season (the fix assumes autumn, chapter 1).
