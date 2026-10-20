# AE town, interiors and door links

Status: 2026-10-20, lane B (town + interiors). Code: `tools/importers/world.py` (presets, post-processing), `tools/importers/doors.py`
(`python3 -m tools.importers doors`), tests `tools/tests/test_doors.py` (synthetic fixtures only). Source: the owner's Anniversary Edition
(`data_0` tree extracted to `bully-extracted/bullyorig`, `data_1.zip` read in place, nothing executed). Numbers below are from that install.
Confidence tags: **VERIFIED** (read in the data and cross-checked), **INFERRED**, **OPEN**.

## 1. What the AE ships (World.img)

`stream/world.img`: 1,787 entries (`agr` 550, `lip` 493, `col` 488, `act` 119, `ipb` 85, `lur` 52). The 85 `.ipb` placement files are the same
85 stems as the PC game: 16 school scenes (`tschool`, `ischool`, 14 `isc_*`), 8 outdoor districts (`tglobal tbusines trich tindust tcarni tjyard tbmx tgokart`),
27 shop/minigame/clique interiors (`i*`), 31 street-dressing tiles `zone_*` (school 6, business 8, industrial 6, rich 11, carnival 1) and two developer
scenes (`ftest`, `ttest`, never imported). `config/world.dat` lists the same 53 `IPL` rows as the PC; the zone tiles are not in it (the game streams them by grid
cell), so the importer names them (`world.TOWN_ZONES`). **VERIFIED**. There is no AE-only scene.

Presets (`world --preset`): `academy` 22 scenes; `town` = academy + 8 districts + 26 zone tiles = 56 scenes; `all` = town + 27 interiors = **83 scenes**.
The earlier `town` preset missed the 26 zone tiles (about 2,000 lamps, power poles, trees, billboards, benches, props).

### Import run (`world --preset all`, CPU only, 74-92 s, 230 MB RSS)

| | |
|---|---|
| scenes / placements | 83 / 7,492 (visible 6,672, with glTF 6,495) |
| models wanted / converted | 2,457 / 2,331 (126 empty in the source, 0 parse failures, 0 texture failures) |
| shapes / vertices / triangles | 11,454 / 2,404,482 / 1,646,241 |
| textures exported | 4,655 PNG |
| output size | 565 MB (git-ignored) |
| hidden on purpose | 444 physics proxies, 39 night variants, 60 duplicates, 5 winter-only, 73 mini-game overlay (see below) |

`msh_empty_in_source` (126): the AE ships 171-byte stub `.msh` files for these (0 parts, `0xCCCCCCCC` bounds), they are not parse faults. Almost all are decal / light
bloom / beam / shadow holders, but a few are real objects the AE dropped or builds at run time: `stop_sign` (25 placements in the zone tiles), `ri1d_turbines01/02`
(rich-area wind turbines, trich (290, 358)), `grd_ri2b_terrain11` (a rich-area ground tile, (339, 388), covered by neighbouring ground in the preview), `make_*_wire*`
(power-line wires), `ri1d_signs02/06`, `dl_in1d_*`, `solar_ivy`, `sc1b_fence_f`, `sc1b_nerdtree_b`, `ch_sludge01/02`. **OPEN**: stop signs and turbines need a
replacement model if the owner wants them back.

### Defects found in CPU previews of every district, and what was done

Previews: `python3 -m tools.importers --out <dir> preview <scene> --view oblique|top [--cut z]` of tglobal, tbusines, trich, tindust, tcarni, tjyard, tbmx, tgokart, tschool and
all districts together (top). Terrain, roads, buildings, bridges, rides and the BMX hall come through with textures and correct placement; no districts missing or offset.

1. **Rivers and the lake drew pitch black** (tglobal, tbusines, tindust). `mat_wtr_base/ripple/shore/caustics` textures are all dark (max 35..74 of 255) with alpha 255:
   layers the game blends over its own water, converted as opaque. FIXED in `world.patch_water_materials`: base -> untextured translucent tint (0.06, 0.22, 0.27, alpha 0.78),
   ripple/shore/caustics -> additive (glTF `alphaMode BLEND`, `extras.blend_mode` and `extras.gamebryo.material` as `tools/ue/import_content.py` reads them); `mat_wtr_waterbase`
   (river bed) stays opaque. 29 models patched. Tint/alpha are an ESTIMATE (no reference render).
2. **No sea.** The sea/background water is not geometry in the AE: `sb1_water` (21 placements, z = 0.0), `pg_water09/10` (isc_pool), `dl_water08_a` are empty stubs. They are now listed in
   the manifest as `water_markers` (24 entries with scene, model, position) for the UE step to put a water plane at z = 0.
3. **Seasonal props**: `dl_bu_winteronly02`, `dl_rc_winteronly02`, `dl_rcxmaspx`, `buxmastreepx`, `fgrd_sc1d_cice` are winter-chapter objects; the importer's season is autumn, so they are hidden
   (`hidden_reason: winter_only`).
4. **Mini-game overlay maps** `imgracea/b/c` (arcade races) are authored at the world origin and overlap the school playing field and each other in 3D (A z 27-61, B z 0-18, C z 9-75; measured
   against tschool, zone_school2..6, tglobal). The game streams one in only during its race. Hidden by default (`hidden_reason: minigame_overlay`, scene `resident: false`);
   `world --keep-overlays` draws them.
5. **Duplicates**: 62 exact duplicate placements (same model, position, rotation, scale to 1 mm), all in `isc_clas` (kiddesk 15, kidchair 10, dpi_plantpot 9, flolight 4, ...), none in any other scene.
   They z-fight; 60 are now hidden (`duplicate_placement`; the other 2 were already hidden proxies). The classroom has the layout twice at the same spot (the 228 placements are 166 distinct).
6. Roofs look nearly black from above: that is the real texture (`zgb_tarpaper_swap`, mean luminance 0.10), not a fault.
7. Owner complaint "walls have no inside face / walk through walls": diagnosed and fixed in lane A, see `ae-interiors.md` (single-sided shells + back-face-culled materials + missing `nogo_*` COL blockers).
   The town previews here draw both sides, so they cannot show it; the town interiors share the same cure.

## 2. Pocket convention, mirrored copy (item 3)

- Outdoor area `MainMap` (areabbox.dat): x -227.7..761.4, y -678.1..780.1, z -26.2..42.0. Every interior sits **west of it** (x -900..-400) in its own pocket; elevated pockets are stacked in z
  (library z 90, auto shop 81, auditorium 77, boxing ring 294, pool 55, prep house 45, boys' dorm 31). Only `isc_clas` and `isc_dorm` share x/y (z -3..8 vs 31..36). **VERIFIED**
  against the IPB placement bounds. `tjyard`, `tbmx`, `tgokart` are outdoor scenes in the same pocket region; `imgrace*` is the one pocket that is *not* away from the town (item 4 above).
- `config/dat/areabbox.dat` rows tie to IPB scenes 1:1 by containment (placements inside the box): every row except MainMap/Everywhere/Test_Area (no pocket) maps to exactly one scene (`School_Hallways` -> ischool, `Library` -> isc_lib,
  `Pool` -> isc_pool, `Boys_Dorm` -> isc_dorm, `classroom` -> isc_clas, ... `BMXTrack` -> tbmx, `JunkYard` -> tjyard, `TGOKart` -> tgokart). Three rows have **no scene in World.img**:
  `Fire_Works`, `iLPstore`, `Island_3` (cut content; doors into them exist in AreaTran and are dropped). The areabbox `Height` of multi-floor areas is often too small (Girls_Dorm 4.7 m, real 15 m).
- Area codes in Trigger.img are the index of the `VisibleAreaEnum` list in `BlipLocs.dat` (0 MAIN_MAP, 2 SCHOOLHALLWAYS, 4 CHEM_LAB, ... 62 BMXTRACK): every arrival point of every DT door lies inside the
  areabbox of its code. **VERIFIED** (the code on a few points is wrong, e.g. the observatory arrival declares 40 but sits outdoors; the generator trusts the position).
- **Mirrored under-ground copy**: the same as the PC. `reflect_lobby` (z -9.5) and `schoollob_reflect02` (z -9.2) are baked, mirrored copies of the lobby under its translucent floor (floor z = 0), the original's
  fake reflection; the real planar mirrors are 20 quads in `config/dat/mirrors.dat` (School_Hallways 4, Pool 2, Boys_Dorm 1, Girls_Dorm 1, HouseOfMirrors 2, Barber 3, Rich_Cloth 2, Poor_Cloth 1, Hair_Salon 1,
  Poor_Hair 2, iGrsrS 1: 4 corner points, area, min/max z, near/far). No negative-scale placement exists in any scene. The AE matches the convention; the existing fake-mirror handling in
  `import_content.py` applies unchanged. UE planar reflections could be placed from `mirrors.dat` (not done, OPEN).

## 3. Door links (item 2)

### Where the original defines them

| Source | What | Use |
|---|---|---|
| `dat/Trigger.img` DT pairs | trigger volume `DT_x` in area A + a POINT `DT_x` in area B (arrival position + yaw). Files: tschool_doors, ischool_doors, BDorm_Doors, tbusines_doors, trich_doors, tindustrial_doors, Store, Gdorm, Janitors, events*, iSaveZones, i*.dat, tcarni, FreakShow, ... | primary: 44 outside<->inside pairs |
| `Trigger.img` AreaTran | older system: trigger `X` -> point `SV_X`; the school rows point at an obsolete pocket (x -1265, outside every area box) and are `Disabled_` or stale | only rows that pass the area-box test and are not already covered |
| `config/dat/ExitLocs.dat` | `MainMap k` = outside door of each interior (36), `<Area> 0` = the door inside | cross-check + filling missing halves |
| `config/dat/areabbox.dat`, `BlipLocs.dat` | area boxes and the area-code enum | validation, scene mapping |

Door types (trigger `PLACEMENT`): ESCDoorL/R (school double doors), DoorStr1 (shops), PrepDoor, IndDoor, FMDoor, TadDoorL, CarnCurt (carnival curtains), SCDoor, HallWind/ExtWind (windows). Lock state is in the
data (`ISLOCKED`) but is script-controlled, so it is recorded as `locked_in_game`, not enforced.

### Geometry (VERIFIED on 84 data links)

- A trigger is a box in a local frame: perimeter points are relative to `POSITION`, `YAW` rotates clockwise seen from above (`world = pos + Rz(-YAW) * (a, b)`). The box covers 1.4-1.5 m of passage
  width and runs from 1.5 m on this side of the door leaf to 1.5-3 m past it. The pawn emerging on this side appears on local **-b** at 1.2-7 m (`away_dir(yaw) = (-sin yaw, -cos yaw)`; true for 71 of 84 data links,
  the exceptions are windows, the AreaTran crawl-space rows and two doors whose yaw is flipped; derived arrivals use it, data arrivals never do).
- Game yaw (points) to UE: `-yaw`. Trigger actor yaw in UE = `+YAW` (the Y mirror turns the clockwise rotation into UE's positive yaw); the oriented box is exported with that yaw and half extents
  `[a-size/2, b-size/2]`; the world-axis AABB is also exported for consumers that cannot rotate it.
- Box height: the data's `ZHEIGHT` is 1 m from the floor; the export is floor - 0.5 m .. floor + 2.5 m so the capsule centre always overlaps.
- Arrival inside the door's own box would bounce the pawn straight back; the box is shortened to clear it (0 links remain with the exit in their own trigger).

### Pairing and fill-in

Edges = every DT/AreaTran trigger whose point lies in another area (area decided by position, declared code only as the first guess). 100 edges after removing 23 rows (10 stale, 11 duplicates of the same door, 2 into areas with no scene).
Opposite edges meeting at one physical door (both ends within 12 m) are paired greedily by distance: 44 pairs (gap median 7.4 m). The other 12 edges have no opposite row in the data (school room doors, the
asylum back door, observatory, the second grocery/bike shop doors, trailer, funhouse): their missing half is derived, in order of preference: `ExitLocs.dat` door position (`data+exit`), else 3.5 m behind the
arrival along its facing (`derived`). Three areas with no row at all (Principal, JunkYard, TGOKart) come from ExitLocs (`derived`, **orientation of both arrivals is a guess**: towards the middle of the area box).
**Shared interiors**: the grocery has 3 outside doors (business strip, poor area, rich area), the bike shop 2, one door inside each. The extra outside doors point at the one inside door; leaving
returns through the first (canonical) outside door (the original remembers the door you came in by, which one `ABullyDoorLink` partner cannot).

Cross-check against ExitLocs (independent file): for the 43 data-derived outside doors the nearest `MainMap` exit is a median 0.95 m away (secondary doors, which ExitLocs does not list, are 20-50 m from it);
29 inside doors are within 1.7 m of their `<Area> 0` row.

### Output: `tools/ue/door_links.json` (schema `bully-remake.door-links/2`)

113 links = 55 pairs (110 links) + 3 extra outside doors that share an interior door (grocery 2, bike shop 1); 40 interior areas plus the town. Each link: `id`, `partner`, `label`, `area`, `area_code`, `scene` (IPB scene of that side), `game_m` (door x, y, z),
`trigger` {`center_cm`, `half_extent_cm` (AABB, what `import_content.py` reads), `yaw`, `oriented` {center, half extents, yaw}}, `exit` {`location_cm`, `yaw`}, `confidence`, `source`, `locked_in_game`.
Backward compatible with the v1 consumer (same keys). By confidence: data 87, data+exit 4, derived 20, legacy 2.
`legacy` = `main_door_ext` / `main_door_int` (front door of the main building): ids and trigger boxes are kept because `walk_m2.json` / `walk_m6_proof.json` walk to its trigger centre; the data agrees with
the old hand measurement (exit within 0.5 m).

Covered (outside door -> interior): school main building (front, right-front, 2 back doors, roof door, hall window <-> `School_Hallways`; the left-front door has no DT row), classroom, art room, bio lab, chem lab, principal, staff room, janitor's room (2 doors), library, auditorium,
auto shop, pool and gym (2), boys' dorm (2), girls' dorm (3), prep house, observatory, warehouse (2), tenement window, chem plant, asylum (2), boxing ring, grocery, bike shop, comic shop, rich/poor clothes, both
barbers, hair salon, souvenir tent, freak show (2), house of mirrors, BMX hall, trailer, junkyard, go-kart track and the four clique hide-outs (jocks, preps, dropouts, greasers). `ischool` rooms: cafeteria, nurse,
bathrooms and attic are part of the `ischool` scene (no separate area).
Without a link: `Fire_Works`, `iLPstore`, `Island_3` (no scene), and the mini-game maps `imgrace*`, `ishoot` (midway shooting gallery), `iballtos` (ball toss), which the game enters from scripts
(`MGS_PlayerEnter` (-792.4, 75.8, 10.0) <-> `MGS_PlayerExit` (135.7, 426.6, 6.1); `BASEBALLTOSS_MINIGAME_ENTRY` (-792.7, 89.3, 10.3)); `iNerdS` is referenced by the data but not shipped.

Validation (`doors` command exit code): 0 exits outside their area box, 0 exits inside their own trigger; 1 trigger centre slightly above its area box (`girls_dorm_sv_girlsdormexit`, the dorm's upper crawl
space; the box is too short, see above).

### How to regenerate

    python3 -m tools.importers --install <bullyorig> doors --write tools/ue/door_links.json   # ~2 s, rewrites the tracked file
    python3 -m tools.importers --install <bullyorig> --out <dir> doors                       # <dir>/doors/door_links.json + doors_dropped.json (skipped rows with reasons)
    tools/ue/run_place_doors.sh                                                              # re-place the actors into the saved map (UE, ~15 s)

`tools/ue/place_doors.py` now spawns the rotated box (`oriented`) and refuses a link whose partner id is missing.

## 4. Stage-2 fix (2026-10-20, ue2 lane)

`Principal` (ExitLocs fallback): `ExitLocs.dat` `Principal 0` (-697.0, 230.5) is 13 m outside the `princoff` shell (x -706.5..-696.2,
y 199.5..217.5, floor 31.5), so arriving there fell through the world. The inner door is now the secretary room's north-wall double door
(jambs x -702.7 / -699.8 at y 217.4; measured from the shell's wall triangles), arrival 3.5 m straight in (`EXIT_FALLBACK` takes an
explicit `(x, y, z, (dx, dy))`). VERIFIED by the pawn route in `run_stairs_check.sh` (lobby -> office -> back out -> lobby -> out: PASS).
