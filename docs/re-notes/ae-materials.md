<!-- OWNER: builder-A (importers) -->
# AE (Anniversary Edition, iOS) materials -> textures

Source: `data_1.zip`, flat `bully/` namespace, 29,660 entries: 5,714 `.msh`, 14,064 `.tex`, 6,542 `.mtl`
(encrypted, ignored). Code: `tools/importers/ae.py`. Measured by a scratch script outside the repo
over the real install (not committed; game bytes never enter git).

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

## Open

- `.mtl` and `.xml` are encrypted; until decoded, a material's true texture slots are inferred from names.
- `.tex` pixel decoding (PVRTC etc.) and `.msh` geometry are separate importers.
- CLI wiring (`--ae-data`) is not added here; `AeArchive(path)` takes the argument, falling back to
  `BULLY_AE_DATA`, `local_config.json` `"ae_data"`, then autodetect.
