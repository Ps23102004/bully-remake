# Ped heads and hair (polish1)

Status: 2026-10-15, branch `polish1`. Code: `python3 -m tools.importers ped <MODEL> --head <HAIR>` (`tools/importers/ped.py`,
`export_ped(head=...)`). Confidence tags as in `format-survey.md`: **VERIFIED**, **INFERRED**, **OPEN**.

## Where hair lives

| Fact | Evidence | Confidence |
|---|---|---|
| A haircut is a *clothing item*, not part of the ped model. `Config/dat/Cloths.dat` lists 69 `HAIR` rows (`B_Buzz`, `B_Bald`, `B_Caesar_01..04`, `B_CrewCut_*`, `B_FlatTop_*`, `B_MFade_*`, `R_GoodBoy_*`, `R_Hwood_*`, `R_HThrob_*`, `R_SShag_*`, `R_ILeague_*`, `R_SSmart_*`, `P_Fauhawk_*`, `P_Mh_Flat_*`, `P_Mh_Spike_*`, `P_Details1/2_*`, `P_Spiky_*`, `P_Taper_*`) and `HEAD` rows (hats, masks, helmets: `B_Bhat1..6`, `SP_*`, `C_*`) | Cloths.dat; `Bully-SE-scripts/scripts/SCloth.lua` (`ClothingDefineOutfit(name, label, "Hair", top, glove L, glove R, legs, shoes, ...)`: every outfit has a `"Hair"` slot); `F_PlayerHasFancyHaircut()` in `chap2/2_08.lua` compares `ClothingGetPlayersHair()` against `R_*` names | VERIFIED |
| Jimmy's starting haircut is **`B_Buzz`** | `scripts/SInitGl.lua:179` `shared.gDefaultHead = "B_Buzz"`, `main.lua:479` `ClothingSetPlayersHair(shared.gDefaultHead)`; IDE row `10905 B_Buzz B_Buzz` in `Objects/default.ide` (the `clth` block: model name = txd name) | VERIFIED |
| Each haircut is a **full skinned head** in `Stream/World.img` (`<name>.nif` + `<name>.nft`, e.g. `B_Buzz.nif` 462 vertices, shape `Head_default`) on the same skeleton as the ped: 35 of PLAYER's 36 joints by name (plus `Root Footsteps`), skin bones `Root Spine2/Neck/Head/Brow/EyeLids/Eyes/Ponytail1`. Its per-bone inverse bind matrices equal PLAYER's head shape's exactly (max difference 0.0) and the bind-pose joint transforms agree to 2.3 mm, so it skins onto `SK_Jimmy`'s skeleton unchanged | `ped.py` comparison on the real files | VERIFIED |
| `PLAYER.nif` itself carries a default head: shape `MA_Player_body01:0` (697 vertices, 685 of them above z = 1.3 m, bones Neck/Head/Brow/EyeLids/Eyes/Ponytail1) with its own texture `Player_01_d`. The game's clothing builder swaps this part for the haircut model | geometry: the two heads coincide in shape (448 of 462 `B_Buzz` vertices within 1 cm of a PLAYER head vertex); the NIF has no separate hair shape | VERIFIED (geometry) / INFERRED (swap rule) |
| **Short cuts are painted, tall cuts are geometry.** `B_Buzz` (and `B_Bald`, nearly the same mesh) are the closest-cropped heads: the hair is brown stipple in the texture (`B_Buzz_d`, 1024x512, face + hair in one atlas), on a skull-hugging mesh, so Jimmy looks almost bald at distance in the proof video. `B_Caesar_*`, `B_CrewCut_*`, `B_FlatTop_*`, `R_*`, `P_*` add real shells (`B_Caesar_01`: 28 shapes; `P_Spiky_01`, `P_Fauhawk_01`: spikes) | CPU contact sheets, `~/bully-remake-evidence/polish1/hair_styles.png` | VERIFIED (renders) |
| `Models/Peds/MXDs.MGR` (555 KB) is a per-model attribute manager: attachment points by name (`Head`, `HeadDir`, `Left Hand`, `LeftCig`, ...) and `TO_*` outfit-style strings; not decoded | hexdump of the first records | OPEN |

So **Jimmy is not missing hair data**: the original game's Jimmy starts as a buzz cut. What the M3 import did was use the
base head from `PLAYER.nif`; the faithful default head is `B_Buzz`, and any other haircut is one flag away.

## What is wired

    python3 -m tools.importers ped PLAYER --anims '*' --root-motion inplace --head B_Buzz     # Jimmy as the game starts him
    python3 -m tools.importers ped PLAYER --anims '*' --root-motion inplace --head B_Caesar_01 # any HAIR row of Cloths.dat
    tools/ue/run_import_character.sh                                                           # SK_Jimmy: 5 sections, 36 bones

`--head` cuts the base head shape out (every triangle whose vertices all lie above the head model's lowest vertex minus
5 mm; vertex arrays compacted: 697 -> 12 vertices left in that shape) and adds the head model's visible skinned shapes
onto the same glTF skin, joints matched by name, textures from `imported/textures/<head txd>/` (txd = IDE row, else the
model name). `--head-txd` overrides. Students and other peds are unaffected unless the flag is given.

The M3/M4 import commands in `tools/ue/README.md` did not pass `--head`; with the flag the glTF gets one more material
(`B_Buzz_d`) and `import_character.py` binds it like the other slots (`MI_B_Buzz_d`).

## Not done / next

- **Hats and masks** (`HEAD` rows) are additive, not replacing: they would be attached on top of the haircut (same
  mechanism without the head cut, or sockets from `MXDs.MGR`). Not wired.
- **Outfits** (jackets `B_Jacket6`, pants `B_Pants2`, shoes `P_Sneakers2`, the `"Starting"` outfit in `SCloth.lua`) are the same
  kind of skinned clothing models; `PLAYER.nif`'s own body shapes are the underwear/base layer. Same `--head`-style
  replacement per body part would do it; the part -> triangle-range rule is per slot and OPEN.
- Hair colour variants (`_01.._04` suffix) are different textures of one mesh family; the importer takes the model by name.
