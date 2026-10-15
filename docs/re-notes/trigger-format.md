# `DAT/Trigger.img`: mission points, paths and trigger volumes

Status: 2026-10-15. Importer: `tools/importers/trigger.py` (`python3 -m tools.importers trigger`). Tests: `tools/tests/test_trigger.py`. Confidence tags as in `format-survey.md`: **VERIFIED** (read in the data and cross-checked), **INFERRED**. Structure and field names only; no script text is reproduced. The format was first mapped in `bully-re:docs/trigger.md`; this note records what the importer relies on and what we added.

## Container

`DAT/Trigger.img` + `Trigger.dir` is the same IMG/DIR container as `World.img` (`img.py` reads it unchanged). It holds 410 entries, all plain ASCII text `.dat` files, one per mission (`1_01.dat`) or place (`tschool_doors.dat`, `Population.dat`, `Patrol_School.dat` ...). Sector padding is NUL bytes. Lines are CRLF or LF, indented with tabs. **VERIFIED**

Totals over all 410 files: 1543 paths, 4188 points, 2546 triggers, 2694 perimeters (826 with no trigger), 338 POI blocks.

## Grammar

- Header, five count lines: `NPATHS`, `NPOINTS`, `NPERIMETERS`, `NTRIGGERS`, `NPOIS`.
- Then top-level blocks in any order: a bare keyword line (`PATH`, `POINT`, `PERIMETER`, `TRIGGER`, `POI`), a `BEGIN` line, `KEY value` field lines, `END`.
- Nested blocks (population data) use the same shape: `POPULATIONDATA` / `BEGIN` / `POPULATION_DAY` / `BEGIN` ... `END` ... `END`.
- Vectors are `x, y, z` (comma + space). Strings are double-quoted. Booleans are `TRUE`/`FALSE`.
- Units: game metres, +Z up. Angles in degrees.
- Header counts are not always right (`tschool_trees.dat` declares one trigger and has none; `1_S01` declares 9 path points on one path and writes 8). The importer trusts the blocks and reports mismatches in `warnings`.

## POINT (a named position list)

Fields: `POINTNAME`, `AREACODE`, `NPOINTPOINTS n`, then n entries. Each entry starts at `POINTPOINT x, y, z` and continues with `POINTYAWPITCHROLL yaw, pitch, roll`, `POINTLABEL`, `POINTPICKUPNAME`, `POINTSFXID`, `POINTSOUNDBANKID`, `POINTSFXVOLUME`, `STREAMFADEINTIME`/`OUTTIME`, `POINTSML`, `STARTHOUR`, `ENDHOUR`, `WEATHERMASK`, `USEOCCLUSION`, `POINTMISSIONSPEC`, `MISSIONNAME`, `POINTPERMANENT`, `POINTTAG`, `PICKUPBUTES`. A point can have 0 entries (name only, used as an area marker) or many (a set of spawn spots, e.g. `1_01_CheerPoints` has 10). In the missions the extra fields are almost always default. **VERIFIED**

Yaw of a point: 0 = +x, counter-clockwise seen from above (the 1_01 player start has yaw 185, i.e. facing -x toward the school). UE yaw = -game yaw. **VERIFIED** on the start point; **INFERRED** for the rest.

## PATH (ordered waypoints)

Fields: `PATHNAME`, `AREACODE`, `PATROLTYPE` (mostly -1), `NPATHPOINTS`, then per waypoint `PATHPOINT x, y, z`, `POINTYAWPITCHROLL`, `WAITTIME`, `UNID`, `NACTIONS n`, and n actions (`ACTIONTYPE`, `ORIENTATION yaw, 0, 0`, `WAITTIME`). `WAITTIME` appears twice per waypoint with actions: the first belongs to the waypoint, the ones after an `ACTIONTYPE` to that action. All actions seen are type 0 (turn to the orientation and wait). Camera paths in `1_02.dat` (`*CAM_*`) use the same block with one or two waypoints. **VERIFIED**

## PERIMETER + TRIGGER (volumes)

A `PERIMETER` block (`PLACEMENT`, `ISDOOR`, `HEIGHT`, `DEPTH`, `NPERIMETERPOINTS`, then pairs `PERIMETERPOINTX` / `PERIMETERPOINTY`) is followed by `NTRIGGERS k` and then k `TRIGGER` blocks that use it. `NTRIGGERS` is therefore a per-perimeter count outside the header. Perimeters with `NTRIGGERS 0` are unreferenced in the file (1_01 has 25 of 32); the importer lists them as `orphan_perimeters` and does not place them.

`TRIGGER` fields: `TRIGGERNAME`, `AREACODE`, `POSITION x, y, z`, `YAWPITCHROLL`, `YAW`, `ZHEIGHT`, `ISMISSIONSPECIFIC`, `ISLOCKED`, `AMBIENT r, g, b`, `USETIMECYCLE`, `HASPOPULATIONDATA` (+ optional `POPULATIONDATA` block with a `TOTAL` and twelve ped categories for DAY/CLASS/NIGHT/CURFEW), `HASAMBIENTVEHICLEDATA` (same idea for cars), `FORCEPOPREAD`, `FORCEVEHREAD`.

Geometry rules (**VERIFIED**):

- Perimeter points are 2D, **relative to POSITION**.
- The volume spans z = POSITION.z up to POSITION.z + ZHEIGHT.
- **YAW rotates the local frame clockwise seen from above**: world = POSITION + Rz(-YAW) * local. Checked on 11 door triggers with YAW +-90: the door's local +y side (the 3 m approach side of the 1.5 m / 3.0 m box) then points at the exit spot, cosine 0.97 on every one. This is the opposite sense to point yaw, so do not reuse one for the other.
- A door-style perimeter has `PLACEMENT "<name>"`, `ISDOOR TRUE` and zero points: the trigger is bound to a placed object, not a polygon. `tschool_FrontGate` is one (position (301.5, -72.5, 6.0), locked, mission specific). The importer marks these `shape: "placement"`.
- Door pairs: `DT_<door>L` / `<door>R` triggers share a position and split the doorway at local x = 0.

## POI

`POI` / `BEGIN` / `NAME`, `AREACODE`, `NPOIPOINTS`, then points that each begin at `GENDER` (see `bully-re:docs/trigger.md` for the field list). Not used by the slice; parsed for completeness (name, type, ped type, position, yaw, periods).

## Area codes

`AREACODE` is the interior/exterior id of the entity: 0 = outdoors (`tschool`), 2 = `ischool`, 5 = principal's office, 14 = boys' dorm interior (others seen: 4, 6, 8, 9, 13, 15, 17, 19, 23, 32, 35, 59). The mapping to our scenes is **INFERRED** from positions (2 -> `ischool`, 14 -> `isc_dorm`, 0 -> `tschool`) and consistent on every point checked. Interior areas have their own coordinate space far from the campus (the dorm is around (-500, 310), the school interior around (-630, -300)).

## Which file is which mission

There is no mission tag inside the data: the file name is the mission (`1_01.dat` = mission 1_01). Entity names are also prefixed (`1_01_...`, `1_02_...`), but shared ones (`GaryTrigger`, `InBedroom`, `PrincipalRoom`) are not. Doors and interior transitions live in the shared files `tschool_doors`, `ischool_doors`, `BDorm_Doors`, and boys'-dorm arrival points in `1_02A.dat` / `BoysDorm.dat`. Names are unique per type except for 7 repeated names in the whole archive, so the importer gives every entity `id = <file>/<type>/<name>` plus `index` (order within its type in the file).

## JSON output

`trigger` writes `imported/triggers/day_one.json` (git-ignored), and with `--files` / `--all` per-file or whole-archive dumps. Entity shapes:

- point: `location_game`, `location_cm`, `yaw_deg`, `yaw_deg_ue` (first entry) and `points[]` with the same per entry; no location when the point has no entries.
- path: `waypoints[]` in order, each with `location_game`, `location_cm`, `wait_time`, `unid`, `actions[]`; `length_m`.
- trigger-volume: `location_game/cm` (POSITION), `yaw_deg`, `zheight`, `polygon_game` / `polygon_cm` (rotated footprint), `aabb_game` / `aabb_cm`, `center_cm`, `extents_cm` (half extents of the axis-aligned box, X Y Z, for a UE box component; a rotated footprint should use the polygon and `yaw_deg_ue` instead), flags, `population`; `shape` is `polygon`, `placement` or `point`.

Conversion is the project rule, game metres to UE cm = (100x, -100y, 100z).

## Day One slice mapping (`day_one.json`)

- `missions.1_01` and `missions.1_02`: every entity of `1_01.dat` and `1_02.dat` (28 + 12 points, 11 + 7 paths, 7 + 6 trigger volumes).
- `extras`: named entities from `tschool_doors` (main building front door pair, boys' dorm door pair), `ischool_doors` (lobby front doors, principal door), `BDorm_Doors` (dorm interior exit door pair, `DT_DormExitDoorL` point outside) and `1_02A` (`AreaTransitionBoys`, `InBedroom`, `EndPosition`).

Spot checks against known values (all agree within the rounding of the recon notes): front gate trigger (301.5, -72.5, 6.0) vs placed gate model (302.9, -72.5); player start (298.4, -72.2) = UE (29836, 7223); boys' dorm door trigger (271.1, -122.4, 7.9); dorm exit point (271.2, -116.3); interior arrival (-503.0, 318.0, 31.4); principal office trigger (-633.2, -291.4, z 4.9 to 8.9) next to the placed `SCH_OfficeMain` (-632.3, -286.7).
