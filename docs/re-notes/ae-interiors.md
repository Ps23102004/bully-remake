<!-- OWNER: builder-A (importers) -->
# AE interiors: walls with no inside, walking through walls (2026-10-20)

Owner report: "in some rooms the walls have no inside: from outside there is a wall, from inside it is empty, only
lines; and you can walk through them". Measured on the academy import (22 scenes, AE `.msh` + data_0 `world.img`);
scratch scripts outside the repo, no game bytes in git.

## Findings

| Hypothesis | Result | Confidence |
|---|---|---|
| (a) shells are single-sided | Yes. Exterior buildings face OUT (`sc1b_bldg_main` 11 % of wall area faces its centre, `sc1b_bldglib` 8 %), room shells face IN (`int_library` 100 %, `bld_regclass` 96 %, `chem_lab` 99 %). No separate interior shell for an exterior, no hidden back faces. The game culls back faces, so either shell seen from behind draws nothing; only two-sided trims (masked rails, frames) remain = the "lines". | VERIFIED |
| (b) UE materials one-sided | Yes. `convert_ae.py` set `doubleSided` only on masked materials, so every opaque material used `M_Bully_Opaque` (back-face culled). | VERIFIED (code) |
| (c) empty models / `dpi_`/`dpe_` | Not the cause. The 39 `msh_empty_in_source` are decal/shadow/glow holders, plus `lib_blackbox`, `p_cshwrrm`, `jr_beams`, `solar_ivy`; empty in the AE data itself. `dpi_`/`dpe_` are interior/exterior detail props, not walls. | VERIFIED |
| (d) zero-thickness walls | Rare in opaque geometry: triangles modelled back-to-back (same corners, reversed) cover 53,024 of 776,806 m², 97 % of it foliage (already two-sided masked); opaque cases: `artrmpprtable`, `dcl_artrmshdw01` curtains, `audavrmdtl`, `bioboard` (6 materials). | VERIFIED |
| Walk-through: collision flag | All 903 `/Game/Imported/Meshes` assets carry `CTF_UseComplexAsSimple` (grep of the .uasset name tables), body setups exist. Not the cause. | VERIFIED |
| Walk-through: one-sided collision | **The cause.** `UBodySetup::bDoubleSidedGeometry` is not initialised in the UE 5.8 constructor (zeroed = false) and is in none of the 903 assets. Chaos then sets `CullsBackFaceRaycast` on the trimesh (`ChaosInterfaceUtils.cpp`: `SetCullsBackFaceRaycast(!InParams.bDoubleSided)`) and the sweep visitors in `TriangleMeshImplicitObject.cpp` skip back faces too, so a capsule sweep from behind a wall passes through it. | VERIFIED (engine source + assets) |
| Why the player got behind walls at all | The game's real player collision is not the render mesh. Each scene's `world.img/<scene>.col` (COL3) has the colliders; for the marker models (`nogo_*`, `fgrd_nd_*`, `walkable_*`) the AE `.msh` is a one-triangle stub but the COL entry is the full mesh. `nogo_*` = invisible blocking walls (surfaces 11/13 brick/plaster, 65 "invisible"): doorway blockers in front of the hallway rooms, hall-end boxes in front of the backdrop screens (`nogo_scinvisible`, x -676.7 and -580 on both floors), fences and map edges outdoors. None of it was imported, so the player walked through open doorways into facade rooms and then out through their one-sided walls. | VERIFIED |

COL vs render, world space: of the `nogo_*` wall area, 459 m² in `ischool`, 27 m² per classroom variant and ~60,000 m²
in `tschool` (fences, cliffs, map edge; partly near zone/tGlobal geometry not in that check) lie more than 0.6 m from
any rendered triangle, i.e. blockers the render collision lacks. `fgrd_nd_*` floors sit within 5 cm of the render
floors (median), stairs are ramps. Both door-link openings (main door outside/lobby) are open in the COL (jambs at
y -74.96 / -71.58 outside, x -629.88 / -627.37 inside, lintels above head height).

## COL3 layout (`tools/importers/col.py`)

Little-endian; INFERRED from hex, VERIFIED by parsing all academy `.col` to the byte with no index out of range
(ischool 95 entries / 4,230 faces, tschool 227 / 15,482, classroom 57 / 500, library 64 / 2,166).
`"COL3", u32 size` (entry = 8 + size); +0x08 `u16 4, u16 kind 1..3`, 20 bytes 0; +0x20 `u32 model id` (= IDE id, as in
the placements); +0x24 centre xyz, radius, min xyzw, max xyzw; +0x54 two u32 (0 everywhere; spheres?); +0x5c `u32`
box count, boxes of `min xyzw, max xyzw, u32 surface`; `u32` vertex count, `3 x i16 / 128` m, pad to 4; `u32` face
count, faces `3 x u16, u8 surface, u8 light`; then a face tree (skipped). Faces are clockwise about their outward side
(floors read counter-clockwise face down); the writer reverses them. OPEN: kind, the +0x54 counters, the face tree,
the surface id table (0 walkable, 11 brick, 13 plaster, 34 floor, 65 invisible seen).

## Fixes

1. `convert_ae.py`: opaque materials are exported `doubleSided` (-> `M_Bully_Opaque_TwoSided`; back faces also cast
   shadows, which stops sun leaking into the interior pockets) unless the submesh is modelled back-to-back
   (`BACK_TO_BACK_MAX` = 0.5 of its area): academy 2,919 opaque two-sided, 6 single-sided.
2. `tools/importers/col.py` (new; run after `world`): `python3 -m tools.importers.col --install <data_0> [--out imported]`
   writes `collision/<marker>.gltf` (entity space, collision only) for every `nogo_*`/`fgrd_nd_*` placement and
   `collision/index.json`. Academy: 46 models / 46 placements / 18,235 faces. `walkable_*` (ped navigation, surface 0)
   is left out on purpose: it is the original nav surface for the AI work, not collision.
3. `tools/ue/import_content.py`: every imported mesh gets `double_sided_geometry = True` (also on skipped, unchanged
   meshes, so a plain re-run fixes existing content); the collision glTFs import to `/Game/Imported/Collision` and are
   placed as hidden `InvisibleWall` actors (camera channel ignored, no shadows/DF/GI) in outliner folder
   `Collision/<scene>`, tag `br_collision`.

## Not fixed here

- Walls look right only where the original drew them: a facade room stays a facade room. The doorway blockers now
  keep the player out, as the game did.
- `p.Chaos.Collision.OneSidedTriangleMesh` (default true) still makes *simulated* bodies (ragdolls, physics props)
  collide one-sidedly with trimeshes; `bDoubleSidedGeometry` covers queries/sweeps only.
- `gymwalls` and `gymwalls01` are two different models placed at the same spot (181 + 85 m² of coplanar opposite
  faces); with two-sided materials they may z-fight. Probably a state pair like the day/night variants.
