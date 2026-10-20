# Anniversary Edition `.msh` models (WarEngine 1337)

Status: 2026-10-20, branch `ae-importers`. Code: `tools/importers/msh.py` (`parse_msh`), tests
`tools/tests/test_msh.py` (synthetic files built in the test, plus one real-archive test that skips when
`data_1.zip` is absent; `BULLY_AE_ZIP` overrides the path).

Source: the AE `data_1.zip` (iOS build, `bully/<name>.msh`, 5,714 files), opened read-only. Worked out
from hex dumps and checked with a survey script over every file (kept outside the repo). No game bytes are
in the repo. Confidence tags as in `format-survey.md`: **VERIFIED** (holds on every shipped file),
**INFERRED** (fits the data, not proven), **OPEN**.

## Results

| What | Result | Confidence |
|---|---|---|
| Parse to the last byte | 5,714 / 5,714 files, 0 trailing bytes (3,830 static, 1,884 with blend weights) | VERIFIED |
| Academy models (`imported/scenes/academy.json`, 298 distinct) | 298 / 298 parse: 271 static, 16 skinned `px*` props (2 bones, rigid), 11 empty (below) | VERIFIED |
| Empty models | 146 files have 0 parts and 0 materials (bounds are `0xCCCCCCCC` fill): decal/shadow/glow holders and dropped models (academy: `dcl_bbathsd05 decals01 dl_sc1d_g flrglowisc03 flrglowisc06 sc1b_fence_f sc1b_nerdtree_b sc_score03 sccafeshad sch_cafedcl sch_cafeflrglw`) | VERIFIED |
| Indices | u16 triangle lists (every submesh count is a multiple of 3, every index < vertex count) | VERIFIED |
| Winding | CCW about the stored normal in game space: 3,692,569 / 3,695,917 triangles (99.91%); 8 degenerate | VERIFIED |
| Positions vs header bounds | header min/max equals the vertex min/max (1 mm) in 5,530 files; in 30 more it equals min/max of `position - pivot`; 8 differ (gymfloor, 7 skinned vehicles) | VERIFIED |
| Normals | byte4 / 127, length 0.99..1.006 (p0..p100); one file has all-zero normals (`dl_waterinas_a`, warned) | VERIFIED |
| Texcoords | short2 / 2048 (raw 2048, 1024, 682, 1365 are the most common values = 1, 1/2, 1/3, 2/3), or float2 | VERIFIED (scale), INFERRED (V origin, see OPEN) |
| Blend weights | ubyte4, sum exactly 255 on all 1,457,824 skinned vertices; up to 4 influences | VERIFIED |
| Blend indices | index the header bone list directly (Jimmy: dominant bone is among the 2 nearest bones for half the vertices; feet use calf/foot bones only) | INFERRED |
| Bones | 3x4 inverse bind matrices: inverting Jimmy's gives pelvis at z 0.846 m, head 1.47 m, feet 0.13 m; parent is an index into the same list (calf -> thigh -> pelvis) | INFERRED |

Totals over all files: 17,263 submeshes, 4,402,749 vertices, 3,695,941 triangles.

## Layout

Little-endian, no padding or alignment anywhere. Strings are `u32 length + bytes` (ASCII, no terminator).

    u32 version            6..12 (6: 3,064 files, 7: 1,127, 8: 309, 9: 165, 10: 29, 11: 806, 12: 214)
    u32 2, u32 1337 (WarEngine), u32 24, u32 1
    u32 mesh_offset        absolute file offset of the part table
    u32 material count, material names (e.g. mat_zrc_asphalt_swap_fgrd_sc1b01)
    u32 bone count, per bone: name, 12 f32 (3 rows of 4: rotation | translation), i32 parent (-1 none)
    flag bytes             1/2/3/4/4/4/5 bytes for versions 6/7/8/9/10/11/12. Values 0/1. OPEN
    10 f32                 bounds min xyz, max xyz, sphere centre xyz, radius
    string                 "{physical(physicalmaterial)=,setups(orderedarray<collisionshapesetup>)=[0]}" (all files)
    ...                    "[0]" string, then a node hierarchy ("Scene Root", "Bone01", per-node 3x4
                           transforms; 15 bytes when empty). Skipped: mesh_offset is absolute. OPEN
    -- at mesh_offset --
    u32 part count
    per part:
      string name          "<model>:<n>", "editable mesh", "editable poly", "mesh"
      2 bytes              always 0
      v>=10: 3 f32 pivot   0 in all but 35 files (handheld props, pool signs, 4 vehicles, gymfloor)
      v>=11: 4 bytes       always 0
      u32 vertex count, u32 index count, u16 x index count
      u32 element count, per element: u32 type, u32 semantic, u8 usage index, u8 1
      vertex count x stride bytes, interleaved in element order, stride = sum of type sizes
      u32 submesh count, per submesh: u32 first index, u32 index count, u32 material, u32 0

Element types (sizes VERIFIED by the to-the-byte parse of every file):

| type | size | encoding |
|---|---|---|
| 1 | 8 | float2 |
| 2 | 12 | float3 |
| 8 | 4 | ubyte4 (blend indices; semantic 6) |
| 9 | 4 | ubyte4, / 255 (blend weights) |
| 10 | 4 | signed byte4, xyz / 127, w always 0 (normal, tangent, binormal) |
| 11 | 4 | signed short2, / 2048 (texcoords; range +-16) |

Semantics: 0 position (always float3), 1 blend weight, 2 blend indices, 3 normal, 4 texcoord (usage index
0..2; up to three sets, the third can be float2 while the first two are short2), 5 tangent (byte4 or
float3), 6 four bytes of 0/1/100 (constant per mesh in 12,871 of 15,851 submeshes; e.g. `(0,0,0,100)` on
buildings, `(0,0,0,1)` on props, corner-like codes on scoreboard quads; OPEN, exposed raw as `extra6`, not a
vertex colour), 7 binormal (3 parts only). 34 distinct declarations; the common ones are
`pos, nrm, tan(byte4|float3), sem6, uv0` and the same with `weights, indices` for skinned meshes.

Parts are separate pieces with their own materials, not LODs (302 files have more than one part).
LODs, if any, are separate files. Submeshes index into their part's vertex buffer; `parse_msh` compacts
each submesh to the vertices it uses, so every `MshSubmesh` is self-contained.

## Reader API

`parse_msh(data) -> MshModel`: `version`, `materials`, `bones` (`MshBone(name, matrix 3x4 rows, parent)`),
`flags` (raw), `bounds` (`min`, `max`, `center`, `radius`), `properties`, `parts` (name, file offset, counts,
declaration, `pivot`), `submeshes`, `skinned`, `warnings`. Each `MshSubmesh`: `part`, `part_index`,
`material`, `material_name`, `positions` (n x 3 float32, game space), `indices` (uint32 triangle list),
`normals`, `tangents`, `binormals` (unit, zero rows stay zero), `uv0`/`uv1`/`uv2`, `joints` (uint8 n x 4),
`weights` (float32, rows sum to 1), `extra6` (uint8 raw). Attributes the file lacks are `None`.
Bad data raises `FormatError` (header, version, counts, index range, element type, submesh range,
material index, truncation). Non-finite or |x| > 1e6 floats are zeroed with a warning (only
`sch_cafeworktble` uv0, 4 values: the same exporter fault its PC NIF has).

Positions are in the same game space as the PC NIFs (right-handed, +Z up, metres): convert with
`coords.game_to_gltf`, keep the triangle order (the rotation preserves CCW).

## OPEN

- Pivot (`parts[].pivot`, v10+): positions are returned as stored. For 30 of the 35 files the header bounds
  are the bounds of `position - pivot` (props become centred on the grip/centre point), for gymfloor and the
  vehicles the raw positions fit better. Which one the world placements expect is unverified; no academy
  model has a non-zero pivot.
- Tangents: 15% of vertices have |tangent . normal| > 0.2 and 13% of float3-tangent submeshes contain zero
  tangents. Treat them as hints; let the engine regenerate (MikkTSpace).
- Texcoord V origin (top vs bottom row of the `.tex` image) is not verified against a textured render.
- Header flag bytes, the semantic-6 bytes, the node hierarchy before `mesh_offset`, and the vehicle bounds
  mismatch. Bone parents are always < bone count (all files), but bones are not stored parent-first
  (cutscene heads list children before parents).
- Skinned vehicles (`truck`, `taxicab`, `limo`, `70wagon`, `foreign`, `cargreen`) are one part with 2
  bones (`modelbone0/1`); how wheels/doors animate is not worked out.
