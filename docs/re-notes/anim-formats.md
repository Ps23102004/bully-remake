# Animation formats: skin, HXD, AGR (Phase 0, milestone 3)

Status: 2026-10-15, branch `phase0-anim`, builder A2. Code: `tools/importers/{nif,hxd,agr,ped,skinpreview}.py`,
tests `tools/tests/test_anim.py` (synthetic) and `tools/tests/test_real_install.py` (install, opt-in).

Sources: the installed game (read only), and `bully-re` `docs/agr.md`, `docs/hxd.md`, `docs/nif.md` as a
documentation reference (that repository has no licence, so nothing was copied; the parsers here are written
from the format descriptions and checked against the game bytes). No Ghidra pass was needed for the formats;
bully-re's addresses are cited for anyone who wants to confirm in the exe.

Confidence tags as in `format-survey.md`: **VERIFIED** (checked on every shipped file, or by a numeric
round trip), **INFERRED** (fits the data, not proven), **OPEN**.

## 0. Results

| What | Result | Confidence |
|---|---|---|
| NiSkinInstance / NiSkinData / NiSkinPartition | decode to the exact block size in all 5,724 World.img NIFs (the existing all-NIF test now decodes them) | VERIFIED |
| PLAYER.nif skinned glTF | 36 joints, 1 skin, 4 shapes, 2,587 vertices, 3,831 triangles, max 3 influences, 0 unweighted vertices, weight sums exact | VERIFIED |
| Skinned bind pose vs stored vertices | max 2.32 mm (bully-re reports the same 2.3 mm) | VERIFIED |
| Inverse bind matrices vs inverse of the file pose | max abs element difference 0.0043 (the skeleton is stored almost, not exactly, in bind pose) | VERIFIED |
| HXD | all 20 `Anim/*.HXD` and all 130 records of `Anim/hxds.dat` parse to the last byte | VERIFIED |
| AGR | all 4,374 clips in the 550 World.img `.agr` and the 4 loose `Anim/*.agr` decode; all 3,365 MAINPED clip records link to an .agr clip with a matching size (404 groups; the 21 `NIS_*` / `0_00_BUFFER` groups have no .agr) | VERIFIED |
| Clip export | idle, walk, run, sprint, walk/run start, idle jump, jog jumps, fall, land: glTF animations on Jimmy's skeleton, validated, rendered | VERIFIED (visual, CPU render) |

Evidence renders (outside the repo): `~/bully-remake-evidence/m3/player_locomotion_side.png` and
`player_locomotion_front.png` (row 1 bind pose, then one row per clip, 6 frames across the clip).

## 1. Skin blocks (NIF 20.3.0.9)

All values follow the file's endianness.

- **NiSkinInstance**: i32 skin data ref, i32 partition ref, i32 skeleton root ref, u32 bone count, i32 × n
  bone node refs.
- **NiSkinData**: overall transform (9 f32 rotation rows, 3 f32 translation, f32 scale), u32 bone count,
  u8 has-weights, then per bone: transform (same 13 floats), bounding sphere (3 f32 centre, f32 radius),
  u16 vertex count, and if has-weights, (u16 vertex index, f32 weight) pairs. The overall transform is
  identity on Jimmy.
- **NiSkinPartition**: u32 partition count; per partition: u16 vertices, u16 triangles, u16 bones, u16 strips,
  u16 weights-per-vertex; u16 × bones; u8 has-vertex-map (+ u16 × vertices); u8 has-weights (+ f32 ×
  vertices × weights-per-vertex); u16 × strips (strip lengths); u8 has-faces (+ strips, or u16 × 3 ×
  triangles); u8 has-bone-indices (+ u8 × vertices × weights-per-vertex). Decoded but not used by the
  exporter (NiSkinData carries the full weights).

**Skinning formula** (VERIFIED on PLAYER.nif): a vertex v of a skinned shape lands at
`Σ w_b · M_b · S_b · v`, with `M_b` the bone node's model-space transform (composed with the NiAVObject
convention `R·(s·v)+t`, R as stored row by row) and `S_b` the NiSkinData per-bone transform, no transpose.
In glTF terms the inverse bind matrix is `S_b · W⁻¹`, where W is the shape's own model transform and the
written vertex is `W·v` (identity on Jimmy).

### PLAYER.nif layout

`Scene Root` → `Dummy` (skeleton, 36 NiNodes) and `MA_Player_body01` → `__NDL_MultiMtl_Node` → four
skinned NiTriShapes (`MA_Player_body01:0..3`, 9 / 24 / 8 / 4 bones). Two unskinned shapes are not exported:
`Mesh` under `Root01` (hidden flag) and `Editable Poly` under `ARROW` (the ground-arrow helper). Every
bone carries a NiTransformController / Interpolator / Data (29 data blocks): an embedded pose or clip,
not decoded (OPEN, not needed).

**Facing.** The NIF bind pose faces game −y (the front render of the bind row shows Jimmy's back); every
locomotion clip faces and travels toward game +y. The difference is all in `Root`'s rotation (about 171°
between the bind pose and any clip). Consumers should take "forward" from the clips (game +y = glTF −Z),
not from the rest pose.

**Hair.** Jimmy's head renders bald with the texture the IDE names (`PLAYER` dictionary). Whether hair is
a separate model/texture swap (the game has haircuts) is OPEN.

## 2. HXD: animation dictionary

All little-endian. `Anim/MAINPED.HXD` is version 1.12: 46 masks, 36 bones, 3,365 clip records, 425 groups.

| Part | Layout |
|---|---|
| header | f32 version (1.12), u32 unknown (OPEN) |
| masks | u32 n; per mask: char[32] name, u32 count, f32 (0.3 in every mask, INFERRED blend time), f32 × count weights (one per bone, INFERRED) |
| bones | u32 n; char[32] × n. MAINPED: `dummy, Root, Pelvis, Left UpperLeg, …, Right Comp, TranslationNode`: the same 36 slots as the NIF skeleton under other names (`Jaw` ↔ `Root Ponytail1`, `TranslationNode` ↔ `ARROW`) |
| clips | u32 n; per clip: f32 play duration, f32 default blend, char[64] `GROUP\NAME`, u32 hash, u32 flags, u32 (.agr byte size + 4), u32 group index, u16 event count, per event (u32, u32, u32 kind; kind 1 adds 40 bytes), u16 × 4, u32, u32 if version > 1.09, u32 if version > 1.11, f32 × 3 |
| groups | u32 n; per group: char[32] name, u32 size (= .agr size + 4 per clip), and from version 1.11 32 bytes + u32 |
| tail | 12 bytes per bone, u32 n + n × 48 bytes, u32 |

The per-clip f32 × 3 ends in the start pelvis height (0.82–0.86 m for standing clips, 0.14 for ground
clips): INFERRED meaning "start position of Root". The four u16 are OPEN.

**Clip → .agr link** (VERIFIED): the clips of group g are the records whose group index is g, in record
order; the k-th is the k-th clip in `<group>.agr`, and its size is `record size − 4` (equal to the scanned
clip size for every clip except a file's last, where the HXD size also covers trailing bytes).

**The HXD duration is the play duration.** Key times in the .agr are fractions of the clip; the header
duration in the .agr is an authoring value that differs by more than 2 % for 496 of the 3,365 MAINPED clips, 14.7 % (C_PLAYER\IDLE: .agr 2.0 s,
HXD 1.8 s; C_PLAYER\WALK 1.067 vs 0.933). bully-re traces this to `FUN_006be250`.

`Anim/hxds.dat`: u32 header word (131; meaning OPEN, the file holds 130 records), then records of `ANIM`,
u32 size, an HXD. It holds the prop/vehicle dictionaries (the 149 World.img `.agr` that no MAINPED group
names, e.g. `BIKE`, `aniglobe`).

Where the player's clips live: group 0 of MAINPED is `C_Player` (439 clips, size 1,328,924 =
1,327,168 + 4 × 439), and its data is the loose `Anim/C_Player.agr`. The other loose `.agr` are `Grap`
(also in World.img, byte-identical apart from padding), `MOT_CTRL`, `NPC_Cher`. That the game loads
C_Player from the loose file rather than streaming is INFERRED from it not being in World.img (OPEN-5 in
the survey, now narrowed).

## 3. AGR: clip bank

A group is clips back to back. Each clip: u32 0x100, u32 key type, u32 key count, u32 0, f32 authoring
duration, the keys, then (for types without a translation in the key) 8-byte position records up to the
next clip. Sector padding is zeros; a trailing all-zero position record also occurs (C_Player's last
clip) and is treated as padding.

| type | key bytes | previous / time | rotation | translation in key | count (all 4,374 clips) |
|---|---|---|---|---|---|
| 999 | 32 | u16 / u16 (÷65535) | 4 f32, w x y z | 3 f32 metres | 172 |
| 1000 | 20 | u16 / u16 | 4 f32, w x y z | — | 45 |
| 1001 | 12 | u16 / u16 | 4 i16 ÷32767, **x y z w** | — | 178 |
| 1002 | 8 | packed | packed 4 × (sign + 10 bits ÷1023) | — | 3,282 |
| 1003 | 20 | u16 / u16 | as 1001 | 3 i16 mm, then 2 pad bytes | 239 |
| 1004 | 12 | packed | as 1002 | u32: x 10 bits + sign bit 10, y bits 11-20 + sign 21, z bits 22-30 + sign 31, centimetres | 458 |

Type 1002/1004 packing (two u32, w0 and w1): w0 bits 0-10 previous-key index, bits 11-19 time ÷511,
bit 20 sign of x, bits 21-30 |x|, bit 31 sign of y; w1 bits 0-9 |y|, bit 10 sign of z, bits 11-20 |z|,
bit 21 sign of w, bits 22-31 |w|. Each magnitude ÷1023. A round trip encoder lives in
`tools/tests/anim_fixtures.py`.

**Track structure** (VERIFIED): keys of all bones are interleaved in time order; each points at the previous
key of the same bone. The first N keys with previous = 0 and time = 0 open the N tracks (N = 36 for
pedestrians, 1 to 33 for props). 624 keys in the whole data set have a zero quaternion: "no data for this
bone here", skipped.

**Position records**: u16 key index, i16 x, y, z millimetres. The record takes the time and bone of the key
it names. In pedestrian clips only track 1 (`Root`, the pelvis) and track 35 (`ARROW`) get records.

**Track order = NIF skeleton order** (VERIFIED here): depth-first NiNodes from `Dummy`. Every one of the 36
tracks' first key equals the node's stored local rotation in at least one C_Player clip, to 0.12°, with the
quaternion read as the "usual" matrix (rows = the NiAVObject rows). The conjugate reading fails (up to 134°).
A key rotation replaces the node's local rotation; a position replaces its local translation (parent frame).

**Root motion** (VERIFIED on the clips): ARROW moves along +y by the character's travel; Root carries the
same travel plus the pelvis bob. In-place playback = Root minus the arrow's offset from its start, ARROW
held. The exporter does that by default (`--root-motion inplace`), or keeps the data as stored (`raw`), or
moves the travel to the Dummy root joint (`dummy`, for engines that extract root motion from the root bone).
Travel and speed go into each glTF animation's `extras.root_motion`.

## 4. Jimmy's locomotion clips (C_Player group)

Clip choice: each is cited by a shipped action tree (`Act/Act.img`: `GlobalLoco`, `SimpleLoco`,
`JumpActions`, decoded with bully-mact-tool's CAT_TO_MACT into the scratchpad, never committed).

| Clip | Used by (action tree) | Play s | Key type | Arrow travel | Speed | Loops |
|---|---|---|---|---|---|---|
| IDLE | SimpleLoco, GlobalLoco, Player | 1.800 | 1001 | 0 | 0 | yes (0.02°) |
| PLAYERWALK | GlobalLoco walk, SimpleLoco | 0.800 | 1002 | 1.637 m | 2.05 m/s | yes |
| PLAYERRUN | GlobalLoco Run/RunLocoAnims, JumpActions land | 0.800 | 1002 | 2.891 m | 3.61 m/s | yes |
| RUNFAST | GlobalLoco Sprint/SprintLocoAnims | 0.500 | 1002 | 4.006 m | 8.01 m/s | yes |
| PLAYERWALK_START | GlobalLoco | 1.100 | 1002 | 2.227 m | 2.02 m/s | no |
| PLAYERRUN_START | GlobalLoco, Player | 1.100 | 1002 | 4.003 m | 3.64 m/s | no |
| IDLEJUMP | JumpActions/Jump/IdleJump/IdleJump | 0.933 | 1002 | 0 (vertical in Root) | — | — |
| JOGJUMP_L / _R | JumpActions RunJUMP / SprintJUMP | 0.833 | 1002 | 3.946 m | 4.74 m/s | no |
| FALL | JumpActions Falling | 1.200 | 1002 | 0 | — | yes |
| FALL_LAND | JumpActions Land_still | 0.500 | 1002 | 0 | — | no |

Command: `python3 -m tools.importers ped PLAYER [--anims ...|*] [--root-motion inplace|raw|dummy] [--sheet PNG]`;
`python3 -m tools.importers anim C_Player` lists all 439 clips with durations, travel and speed.

## 5. OPEN

1. HXD: header word (u32 after the version), the per-clip u16 × 4, the unnamed u32s, f32 × 3 beyond the
   pelvis-height reading, the 12-bytes-per-bone and 48-byte tail blocks, event semantics.
2. HXD mask weights: one per bone is inferred from count = bone count; not checked against playback.
3. `hxds.dat` header word 131 versus 130 records.
4. Jimmy's hair (haircut swaps) and the NIF's embedded NiTransformData poses.
5. Whether the engine loads `C_Player` from the loose `Anim/` file (survey OPEN-5): consistent with every
   observation, not traced in code.
6. Animation blending, masks, playback-rate sync (`LocomotionAnimationNew` / `AnimationLocoSync`) are not
   implemented by the exporter; each clip is exported standalone.
7. Linear glTF interpolation of rotations is slerp in most engines; the game's interpolation between keys
   (bully-re: slerp) matches, but the 1/511 time quantisation of type 1002 means keys can land on the
   same frame; equal times are collapsed (last wins).
