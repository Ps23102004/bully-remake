# Anniversary Edition skinned characters (peds, dogs, rat)

Status: 2026-10-20, branch `ae-importers`. Code: `tools/importers/ped_ae.py` (+ `ped` CLI branch in `cli.py`, an
arrow-less root-motion rule in `ped.add_clip`), tests `tools/tests/test_ped_ae.py` (synthetic; one real-data test skips
when `data_1.zip` is absent). Source: AE `data_1.zip` (`bully/<name>.msh`, `.tex`) read in place, and the extracted
data_0 tree (`anim/*.HXD`, `anim/*.agr`, `stream/world.img` `.agr`), all read-only. Confidence tags as in
`format-survey.md`.

## Findings

| Finding | Evidence | Confidence |
|---|---|---|
| A skinned .msh stores the exported node hierarchy in the gap `msh.py` skips (after the property text): `"[0]"`, u32 n, n x (name, 4 u32 list links), u32 n, n x (name, i32 0, i32 parent, i32 first child, i32 next sibling, 12 f32 local 3x4 rows = rotation \| translation, column vectors). It ends exactly at `mesh_offset` | 1,875 of the 1,884 skinned .msh parse to the byte; the 9 failures are skinned vehicles (`70wagon`, `taxicab`, ..., another layout, not needed here) | VERIFIED |
| Ped skeleton = the PC NIF skeleton: 36 nodes, PC names and case, depth-first PC order: `Dummy, Root, Root Pelvis, Root L Thigh/Calf/Foot, Root R ..., Root01, Root Spine, Root Spine1, Root Spine2, Root Neck, Root Head, Root Ponytail1, Root EyeLids, Root Brow, Root Eyes, Root L Clavicle, Root L UpperArm, Root L Forearm, Root L Hand, Root L Finger0/1/11, Left_Shoulder, Root R ..., Right_Shoulder, ARROW` | 773 skinned .msh have exactly this 36-node list (player, every gn_/ndh/pfh ped checked, both dog bodies) | VERIFIED |
| Node order = `.agr` track order = MAINPED.HXD bone slots (`dummy, Root, Pelvis, Left UpperLeg, ..., Spine Level`=`Root01`, ..., `Jaw`=`Root Ponytail1`, `Left Comp`=`Left_Shoulder`, `TranslationNode`=`ARROW`) | over all 439 C_Player clips, every one of PLAYER's 36 tracks has a clip whose first key equals the node's stored rotation to 0.12 deg (the PC figure exactly); renders below | VERIFIED |
| The .msh bone table lists only skin bones (Jimmy 32 of 36: no Dummy/Root/Root01/ARROW; gn_boy01 30; dogs 28), lowercase, parent = index into that table; blend indices index it | name lookup into the node list succeeds for 1,872 of 1,875 skinned files | VERIFIED |
| Bone matrix = inverse bind (game space). The node transforms are an authoring pose that is NOT the bind pose: Jimmy 0.004 (matrix units), gn_boy01 0.23 (head 18 mm, 3-5 deg everywhere), gn_asiangirl legs 25-51 mm (bone lengths differ) | inverse(bone matrix) vs composed node pose | VERIFIED |
| AE HXD/AGR = PC format (MAINPED.HXD v1.12, 425 groups, C_Player 439 clips, same speeds e.g. RUNFAST 8.01 m/s); existing `hxd.py` / `agr.py` decode them unchanged | `anim` subcommand on the AE tree | VERIFIED |
| Dogs: biped-named 36-node skeleton, clips in MAINPED group `QPED` (22: DOGWALK, DOGRUN, DOGSPRINT, DOGIDLE, DOGBARK, DOG_BITE, ...), `World.img/qped.agr` | sheets below | VERIFIED |
| Rat: own 12-node skeleton (`Root, Torso, Hips, R_RearLeg, L_RearLeg, TailBase, TailTip, Shoulders, Head, R_FrontLeg, L_FrontLeg, None`), `anim/RAT_PED.HXD` (12 bones `joint0..11`, group RAT_PED, 4 clips) + `World.img/rat_ped.agr`. No arrow: the travel is on track 1 (`Torso`, RAT_SCURRY 2.84 m / 0.77 s = 3.7 m/s) | clip position records | VERIFIED |
| Hair: AE peds carry hair in the body mesh (students render with hair). Jimmy's `player.msh` is the PC body (4 materials `mat_player_01..04`, head submesh 697 vertices); `b_buzz.msh` (7 skin bones, 462 vertices) has the same inverse binds as player's head bones (skinned check 0.0 mm), so `--head B_Buzz` works exactly as on PC | export report | VERIFIED |
| Academy placements hold no characters: the 37 skinned models placed in `imported/scenes/academy.json` (273 placements) are 1-3-bone props (`px*` sinks/beds/chairs/garbage, `armoir`, `bikegar`, `scdoor`, `anipillo`, `pxtree6m`); people/animals come from `Config/AmbientLoops.json`, `NPCScripts`, missions and `DefaultGame.ini` `PedMeshPath` | scan of the manifest | VERIFIED |

## Export (same output as the PC `ped` path)

`ped <MODEL>` on an AE install (`ae.detect_ae`, or `--ae-data`) -> `ped_ae.export_from_install`. Same flags and
output: `models/<MODEL>.gltf` (+ `.bin`), the model node, 36 joint nodes named as on PC (Interchange hyphenates
them; UE code uses `Root-L-Foot`, `Root-R-Hand`, `Root`), one skin, animations named by the HXD with the same
`extras` (`ped.add_clip` is reused), same JSON report. New flag `--hxd` (default MAINPED; the rat needs RAT_PED).
Textures: `convert_ae` material rules -> `textures/ae/<stem>.png` (diffuse + normal), `extras.textures` for
`import_content.mat_key`.

- **Rest pose = bind pose.** world(skin bone) = inverse(its .msh IBM); world(other node) = world(parent) x node local;
  joint locals = parent^-1 x world. Every IBM = inverse(rest world), so the rest pose skins the stored vertices
  exactly (report `bind_vs_stored_max_mm` 0.0 on every ped) and bone lengths are the body's own. Clips replace local
  rotations (and Root/ARROW translations) as on PC; `ibm_vs_file_pose_max` reports the bind-vs-authoring-pose gap.
- **Head**: base submeshes with > 80 % of vertices above the head model's lowest vertex - 5 mm lose the triangles that lie
  entirely above it; head-model bones map by name (PLAYER + B_Buzz: 685 of 697 head vertices cut, as PC).
- **Arrow-less clips** (`ped.add_clip`): when a skeleton has no track 35, the ground travel is track 1's x/y motion, so
  `inplace` holds the rat in place and `root_motion.speed_m_s` carries the speed (PC peds unaffected: they have track 35).
- **Root height**: the .agr Root translation is absolute (C_Player authored on Jimmy, bind Root z 0.845 m; QPED on the
  pitbull, 0.436 m), so `export_ped_ae` scales Root and ARROW translations by (this body's bind Root z / bank reference)
  in a copy of the clip (`scale_root_clip`; factor in each animation's `extras.root_height_scale`; PC `ped.py` untouched,
  the rat's own bank unscaled). Unscaled, GN_Asiangirl sank 0.23 m and DOG_Doberman 0.30 m at IDLE/DOGIDLE frame 0;
  scaled they stand within 0.03 m of z=0 (`extras.frame0_min_z_m`, and a warning if an idle dips below -3 cm).
  Doberman walk speed 0.67 -> 1.10 m/s with its longer legs.
- **Track check**: with >= 100 clips the report warns about tracks whose first key is > 10 deg from the node's stored
  rotation in every clip (catches a wrong track order; bodies other than Jimmy are posed up to 8 deg off).

## Generated set (imported/, git-ignored; reports `imported/ped_<MODEL>.json`)

| Model | Clips | Notes |
|---|---|---|
| PLAYER (+ `--head B_Buzz`) | C_Player `*` 439 | 2,364 verts, 4+1 materials |
| GN_Boy01, GN_Boy02, GN_Asiangirl, GN_Bully01-04 | 30: npc_plan locomotion + gestures | same clip list as `run_import_npc.sh` |
| GN_Greekboy, NDH3_Bucky, PFH1_Seth | 10 locomotion | the ambient students list (tools/ue/README.md) |
| GN_Bully05 | C_Player `*` 439 | `PedMeshPath` fight opponent |
| DOG_Doberman, DOG_Pitbull, DOG_Pitbull2, DOG_Pitbull3 | QPED `*` 22 | not referenced by UE config yet |
| RAT_PED | RAT_PED `*` 4 | not referenced by UE config yet |

All 17 validate (0 problems), 0 missing textures, 0 unweighted vertices, max 3 influences. 1,210 clips decoded of
1,210 requested. Contact sheets (CPU, `skinpreview`) checked by eye: upright, textured, limbs intact through walk/run
(Jimmy, 3 students, 6 more peds, dobermann + pitbull walk/run/sprint/bark, rat scurry/death/swim).

## OPEN

- Jimmy's other haircuts / outfits: AE ships `player_*` variants (`player_orderly`, `player_mascot`, `player_ween`, ...)
  and `ma_player_s_tousled`; the clothing builder rules (PC `Cloths.dat`) were not looked up for AE.
- `.mtl` is encrypted: specular maps unused, character materials are imported opaque (hair cards with alpha, if any,
  render opaque).
- Faction clip groups (`F_Nerds`, `F_Girls`, ...) still unused (as PC); students run on C_Player.
- Dogs and the rat have no UE consumer yet (no animal pawn); rat bind pose sits half below z=0 (mesh centred on the
  root), the clips lift it (Torso z 0.077).
- Skinned vehicles' node hierarchy layout (9 files) not decoded.
