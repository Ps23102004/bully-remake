<!-- OWNER: builder-A (importers) -->
# AE (Anniversary Edition, iOS) materials -> textures

Source: `data_1.zip`, flat `bully/` namespace, 29,660 entries: 5,714 `.msh`, 14,064 `.tex`, 6,542 `.mtl`.
**Since 2026-10-20 the `.mtl` files are decoded (`tools/importers/wx.py`, section 6) and are the source of every
material's textures, effect and flags; sections 1-4 describe the name heuristic that is now only the fallback for a
material without a `.mtl` (none of the 6,250 materials the `.msh` files use lacks one).** Code: `tools/importers/ae.py`
(`material_info`, `resolve_material_textures`), `convert_ae.py`. Measured by scratch scripts outside the repo over the
real install (not committed; game bytes never enter git).

## .msh header (verified on all 5,714 files)

`u32 version (6..12), u32 2, u32 0x539, u32 0x18, u32 1, u32 size, u32 material_count`, then per
material `u32 len + ASCII name`. Every file parses; none has a non-`mat_` name except `default` and
`cn_oldcanyonx_swap_d_trans` (already a texture stem). Version distribution: 6:3064, 7:1127, 11:806,
8:309, 12:214, 9:165, 10:29 (meaning of the version not investigated). Geometry after the name table is
not decoded here.

## Resolution rules

`resolve_material_textures(name, archive)` lowercases, strips `mat_`, splits on `_`, then for the base
formed from all tokens, then with trailing tokens dropped one at a time (`drop-suffix`), and for each
base also its `z`-toggled form (`z-stripped` / `z-added`), it tries these in order and stops at the
first base whose diffuse exists. `normal`/`specular` are looked up by the same rule (usually present
only for some materials). A diffuse miss still reports N/S found at an earlier base (`unresolved`).

Per-base match order: (1) **model-suffix** `<base>_d_<dropped tokens>` (per-model texture variant,
e.g. `pr00_portapoo1_d_cs`); (2) **exact** `<base>_d`; (3) **variant-suffix** `_d` + one of `_wi`,
`_sp`, `_in_grass02`, `_swap`, `_swap_sp`, `_swap_in_grass02`, or `_d2`; (4) **bare** `<base>` with no
role letter (`abacus`, `sky_bot`, `default`). **embedded-role** is checked first for each base: the base
is itself a texture whose role letter is inside a token before `swap` (`zwillow_barkd_swap` ->
`..._barkn_swap` / `..._barks_swap`; `zsmall_tree_branch_d_swap`).

Suffix meanings are INFERRED: `_wi` wind-animated foliage, `_sp` specular packed in alpha,
`_in_grass02` grass-blended terrain, `d2` second diffuse. The `z` prefix marks a vegetation/terrain
variant of a texture family (`zcn_strata_swap` -> `cn_strata_swap`, `bg_rooftop` -> `zbg_rooftop`).

## Results

Academy (298 models in `imported/scenes/academy.json` -> 564 distinct materials, all with a matching
`.mtl`): **561 / 564 diffuse resolved (99.5%)**; of those 309 also have a normal and 394 a specular.

| rank | rule | academy hits |
|---|---|---|
| 1 | exact | 390 |
| 2 | exact + drop-suffix (`mat_<tex>_<model name>`) | 158 |
| 3 | variant-suffix + drop-suffix | 5 |
| 4 | embedded-role | 3 |
| 5 | variant-suffix | 2 |
| 6 | embedded-role + z-stripped | 2 |
| 7 | embedded-role + drop-suffix | 1 |
| - | unresolved | 3 |

Unresolved (no diffuse texture of any spelling exists in the archive; probably diffuse comes from the
encrypted `.mtl` or a colour): `mat_pr00_drainp` (only `pr00_drainp_s`; a near name `pr_drainp_swap_d`
exists but is a guess, not applied), `mat_sc28_tile_01`, `mat_sc28_tile_01_first_floor01` (only
`sc28_tile_01_n`). Consumers should fall back to a flat material and still use the N/S stems returned.

Whole archive (all 6,250 distinct materials): 6,191 resolved (99.1%): exact 4,818, exact+drop-suffix
1,280, bare 45, embedded-role 10, bare+drop-suffix 9, variant-suffix 8(+7 with drop), others < 5. The 59
unresolved are mostly characters/props that ship only `_n`/`_s` (or nothing, e.g. `mat_grey`,
`mat_sky_*`), so they need the encrypted `.mtl`.

## 2026-10-20: winter variants and the 16 untextured models

The numbers above were taken on an early 298-model academy list. On the full import (900 models, 1,600 distinct
materials) the first resolver left 10 materials / 16 models without a diffuse and chose snow textures for 6 more.
`ae.py` now has two additions (tests in `tools/tests/test_ae.py`):

1. **Winter variants lose.** `<name>_wi` and `<name>_d_in_grass0N` are snow versions (see `ae-grass.md` section 2). When
   the z-spelled name only has such a variant and the plain autumn texture exists under the other spelling, the plain
   one is taken (`exact+z-stripped`): `zcn_strata_swap` (cliffs; `cn_strata_swap_s` correlates with
   `cn_strata_swap_d` at r = 0.97), `zrc_bikepath_swap`, `zveg_bush3z_swap`. Over all 6,250 materials 24 change diffuse:
   13 untextured ones filled by `MATERIAL_FIXES`, 6 by the winter rule, 5 forced by `MATERIAL_FIXES`; 16 of the 1,600
   academy materials change.
2. **`MATERIAL_FIXES`** (rule `fix:<kind>`; REMOVED, superseded by the `.mtl`, see section 6, kept here as the record of the evidence):

| material | diffuse | evidence |
|---|---|---|
| `mat_psc_bio_brain` | `sc22_brain_d` | art-match: `psc_bio_brain_s` r = 0.95, `_n` relief r = 0.83 |
| `mat_pr00_drainp` | `pr_drainp_swap_d` | art-match: `pr00_drainp_s` r = 0.91 |
| `mat_sc_bathrtile01` | `bdormsc06_floor_d` | art-match: `sc_bathrtile01_s` r = 0.94 |
| `mat_re01_sinkvalve2` | `sc29_sink_d` | art-match: `re01_sinkvalve2_s` r = 0.87; ciphertext window (below) shares most with `mat_sc29_sink` |
| `mat_sp_boxing_gloves` | `sp_boxing_g_r_d` (+n, s) | name; `_n` relief r = 0.72 |
| `mat_mascot_head` | `mascot_head_brn_d` (+`_s`) | name only (the head normal does not correlate with either mascot diffuse) |
| `mat_sc22_skelet` | `psc_bio_skeleton_d` | sibling prop; `sc22_skelet_s` is cloud noise, no match anywhere |
| `mat_re01_bookshelf` | `sc30_bshelf_d` | sibling: same shelf layout in `re01_bookshelf_n` / `sc30_bshelf_n`; ciphertext window matches `mat_sc30_bshelf` |
| `mat_sc28_tile_01` (+`_first_floor01`) | `schallfloor01z_d` | sibling: "school hall floor", 3x3 tiles like `sc28_tile_01_n`; no diffuse in the archive correlates with the normal. GUESS (tan/olive tile) |
| `mat_zrc_grasse_swap` (forced) | `rc_grasse_swap_d` | `zrc_grasse_swap_d` is snow; normal r = 0.40 with `rc_grasse`; ciphertext window matches `mat_rc_grasse_swap` (score 233 vs 181 next) |
| `mat_zrc_mosspath_swap` (forced) | `fun_gravedirt1_d` | `zrc_mosspath_swap_s` r = 0.81 (snow diffuse only) |

"art-match" works because the shipped `_n`/`_s` maps were baked from the diffuse art: luminance of the diffuse
correlates with the spec map, and the diffuse's luminance gradient with the normal's xy (brain, drain pipe and bathroom
tile reach 0.9 and more). A scan of all 14,064 textures at 64 px finds the best diffuse for a spec/normal map.

Result on the full academy import: **1,600 / 1,600 distinct materials resolve a diffuse (was 1,590)**, 0 models
with a missing texture (was 16). Rules: exact 1,146, exact+drop-suffix 422, embedded-role 12, `fix:*` 12
(art-match 5, sibling 4, name 2, winter 1), winter-swap 4, variant-suffix 2, bare 2. 877 have a normal, 1,211 a specular.

### Normal maps (verified, no change needed)

Convention: **Direct3D style, green = down, which is what UE expects**; no flip and no Z derivation. Measured over
3,204 `_n`/`_d` pairs (luminance as height): of the 1,463 strong pairs (|r| > 0.3 on both axes) `n.x` anti-correlates
with `dL/dx` in 1,384 (95 %), and `n.y` with `dL/drow` (the D3D sign) in 1,098 of those against 286 for the OpenGL
sign (79 %); on grooved materials (brick, tile, cobble, plank, floor, wall: 366
pairs, 199 strong) 157 vs 37, median r(x) = -0.33, r(y) = -0.30. `sqrt(1 - x^2 - y^2)` matches the stored blue to 0.017
median (mean vector length 0.98), so they are ordinary tangent-space maps and TC_NORMALMAP (BC5) is right. The old
PC note "green channel unverified" is settled for the AE path. Tangents are still dropped (MikkTSpace in UE).

## Hidden models (world.py `ae_hidden_models`, 2026-10-20)

- **px\* physics proxies** (262 academy placements, 33 models): every `px*.msh` in data_1.zip (74 files) is a ~2 m
  box whose only material is `mat_sc31_medyellow_pxgbed` (4 add two `default` slots). They are hit/rigging proxies;
  the visible prop is a separate coincident placement (pxgarb + dpe_garbstuff*, pxlad5m + ladder_5m, ...). They were
  the grey-blue pillars in the AE renders. Placements stay in the manifest with `visible: false`,
  `hidden_reason: "physics_proxy"`.
- **Night variants** (17 placements): `<x>night<k>` / `<x>n<k>` screens and windows placed beside their
  `<x>day<k>` / `<x>d<k>` twin (scwinnight01, dl_ischoolscreenn, gdormscreen01n ...); the day state is kept.
- Chalk decals `sc_hopscotch`, `sc_tictac`, `sc_soccergoalline` are RGB chalk on black with no alpha: blend mode
  additive (convert_ae.ADDITIVE) instead of opaque black rectangles.

## 5. `.mtl` / `.xml` cipher: broken

Superseded by `docs/re-notes/ae-story.md` section 3 and `tools/importers/wx.py` (custom base32 + keystream, all 6,931
`Wx` files decode). The attempt notes that used to be here (position-keyed stream, carry-like symbol pairs) are explained
by that cipher: the keystream is added bytewise before the 5-bit packing.

## 6. The decoded `.mtl` (2026-10-20, lane M)

Format (every one of the 6,542 files parses; `ae.parse_mtl`):

```
{
	effect(effect)=bully_normalspec,
	physical(physicalmaterial)=,                  # always empty (6,537 files); no physics surface here
	doublesided(bool)=true,                       # optional flags: doublesided 2,258, alphatest 709, transparent 212,
	alphatest(bool)=true, transparent(bool)=true, #   unlit 51, noshadows 20 (only the value `true` occurs)
	textures(orderedarray<texture2d>)=[3,a_d,a_n,a_s],
	vectors(orderedarray<vector4>)=[2,{x(float)=..,y(float)=..,z(float)=..,w(float)=..},{...}]   # 305 files
}
```
(`width`/`height`/`edgethickness` appear on 7 helper materials such as `constantcolor` and `toon`.)

**Effects (20)**: `bully_normalspec` 3,184, `bully_normal` 1,294, `bully_diffuse` 1,089, `bully_spec` 896, then `diffuseuv` 22,
`normaluv` 12, `normalspeccolor` 12, `specuv` 9, `normalspecuv` 8, `waterripple` 3, `diffusecolor` 2, `diffuse` 2,
`constantcolor` 2, and one each of `unlit`, `radarmask`, `toon`, `comic`, `eff_marker`, `watermain`, `sky`.

**Slot order is fixed by the effect name**: `bully_<diffuse|spec|normal|normalspec>[uv|color]` lists diffuse, then normal
(if the name has `normal`), then specular (if it has `spec`); water effects are diffuse + normal; `radarmask` is base +
mask. Checked on the stems: of 3,184 `normalspec` files 2,905 are exactly `*_d, *_n, *_s`, the rest differ only in the
stem's own spelling (a spec slot can name a `_d` texture; the slot, not the suffix, decides the role).
`ae.effect_roles` encodes this; a slot beyond the effect's roles becomes "extra".

**Vectors** (INFERRED from value ranges; there is no UV-tiling field in the `.mtl`: tiling is in the mesh UVs, and the
`*uv` effects scroll them at run time, rate not stored):

| effect family | vectors[0] | vectors[1] | vectors[2] |
|---|---|---|---|
| `bully_diffuse/spec/normal/normalspec` | colour tint (always 1,1,1 in the 305 files that have it) + `w` = opacity (0.09..0.99 on 91 of them) | spec effects: (x, y) on 214 materials, x 2..40 (typically 4..10), y 0..10 (typically 0.2..0.8): read as specular power and strength | `speccolor`: (1,1,1,1) |
| `bully_waterripple`, `bully_watermain` | (1,1,1,1) | UV scroll rate, e.g. (0.875, 0) | watermain: (2.0, 0.5, 0.1) wave parameters (unidentified) |
| `bully_sky` | three gradient colours | | |
| `constantcolor`, `toon` | the colour | | |

There are no glow or environment slots: the maximum is 3 textures (diffuse, normal, specular).

**Flags -> blend mode** (`convert_ae.blend_for`): `alphatest` and the diffuse has alpha -> `masked` (cutoff 0.5, two-sided);
`transparent` (and an alpha channel or opacity < 1) -> `translucent`; otherwise `opaque` (two-sided by the interiors rule,
ae-interiors.md); `unlit`, `noshadows`, `doublesided` are recorded in `extras.ae.flags`. There is **no additive flag**: the
light-beam / halo / glow / chalk materials stay `additive` when the name matches `glow|additive|sunbeam|hopscotch|tictac|
soccergoalline` and the `.mtl` is unlit, transparent, alphatest or a diffuse-only sprite (the chalk decals are alphatest on
an alpha-less texture: all pixels pass, so they must be additive). A glow-named material with a normal/spec map and no flag
is a lit surface (arcade cabinets, interior window panes) and stays opaque. Water: `waterripple` is `transparent` ->
translucent, `watermain` (river bed) opaque; world.py `patch_water_materials` still recolours `mat_wtr_*` by name.
The old name rules (`_sp` = specular in alpha, alpha channel -> masked) apply only to a material with no `.mtl`.

glTF output per material: `extras.ae` = {material, rule ("mtl"), source, effect, flags, diffuse/normal/specular stems,
vectors, specular_power/strength, extra_textures, water + uv_scroll, uv_animated}; the specular map is now also decoded and
listed as `extras.textures.gloss` (the UE master's existing Gloss slot); a non-neutral tint or an opacity < 1 on a translucent
material goes to `baseColorFactor` and `extras.gamebryo.material.{diffuse, alpha}` (read by `import_content.mat_key`).

### Measured against the old name heuristic (all 6,250 materials used by the 5,714 `.msh`)

| | count |
|---|---|
| materials with a `.mtl` / without | 6,250 / 0 |
| `.mtl` names a texture that is not in the archive | 9 slots in 9 materials, 5 stems (`ap_boldroll_d`, `circle_button`, `sil4_cnfraffysgn_d`, `mask_s`, `sc27_fence01_n`); the heuristic fills the role if it can |
| diffuse differs from the heuristic | 29 different, 49 the heuristic found none |
| normal differs / heuristic found none / `.mtl` has none | 32 / 441 / 126 |
| specular differs / heuristic found none / `.mtl` has none | 39 / 240 / 1,066 (the name guessed an `_s` the material does not use) |
| diffuse that is a snow (`_wi`, `_in_grass0N`) variant | 3: one pedestrian coat, `zbu_cobroaddirt_swap` and `zrc_planks_swap` (both the `.mtl`'s own choice) |

Placed in the full import (`world --preset all`, 2,331 models): 3,968 distinct materials, all resolved from the `.mtl`
(0 heuristic, 0 `mtl+heuristic`), 3,966 with a diffuse (the other two name a texture that is not shipped:
`mat_sil4_popcup`, `mat_sil4_cnfraffysgn`), 2,408 with a normal, 2,055 with a specular map; academy scenes alone
(814 models): 1,512 materials, all from the `.mtl`, 15 of them with a different diffuse than the heuristic gave. Blend modes
(all): opaque 3,422, masked 357, translucent 165, additive 24. Against the old rule (alpha channel -> masked) the
non-water materials changed: 163 masked -> translucent (graffiti, tyre tracks, dirt, ivy, glass, beakers: soft-edged decals),
141 masked -> opaque (alpha is packed spec/other data, no `alphatest`), 3 additive -> opaque, 1 opaque -> masked.

### The 11 hand-made `MATERIAL_FIXES` (removed)

Checked against the `.mtl`: **8 confirmed, 3 wrong** (ae-story.md section 3). Confirmed: `mat_psc_bio_brain`
(`sc22_brain_d`), `mat_sc_bathrtile01` (`bdormsc06_floor_d`), `mat_re01_sinkvalve2` (`sc29_sink_d`), `mat_sp_boxing_gloves`
(`sp_boxing_g_r_d`), `mat_re01_bookshelf` (`sc30_bshelf_d`), and the three winter/autumn overrides `mat_zrc_grasse_swap`
(`rc_grasse_swap_d`), `mat_zbu_cobroaddirt_swap` (`zbu_cobroaddirt_swap_d_in_grass04`), `mat_zrc_mosspath_swap`
(`fun_gravedirt1_d`). Wrong: `mat_pr00_drainp` is `black4_d` (+ `pr00_drainp_s`), `mat_mascot_head` is `sp_mascot_h_d`
(+ `mascot_head_n`), `mat_sc28_tile_01*` is `bdormsc06_floor_d` (+ `sc28_tile_01_n`). `mat_sc22_skelet` also has a `.mtl`
now read directly. With every material resolved from its `.mtl` the whole table, `_fix_for` and `_apply_fix` were deleted;
the winter-variant rule (section "winter variants" above) stays in the heuristic fallback. The "art-match" method
(correlating `_n`/`_s` maps with diffuse luminance) was right every time it was checked, which is a useful sanity check of
that technique for other assets.

## Open

- Additive blending is not stored in the `.mtl`; the glow/beam/halo/chalk decision still uses names (above). Where the engine
  takes it from (a render state on the model node, the `.msh` flag words) is not decoded.
- Vector semantics (specular power/strength, water scroll, watermain wave parameters) are inferred from value ranges.
- Which season the game shows when: the importer assumes autumn (chapter 1); the `.mtl` has no season switch.
- `.tex` pixel decoding (PVRTC etc.) and `.msh` geometry are separate importers (done: `wtex.py`, `msh.py`).
- CLI wiring: `world --ae-data` and the `text` subcommand take the archive path; `AeArchive(path)` falls back to
  `BULLY_AE_DATA`, `local_config.json` `"ae_data"`, then autodetect.
