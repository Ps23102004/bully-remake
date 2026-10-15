# Bully: Scholarship Edition, format survey (Phase 0 scout)

Status: read-only survey, 2026-10-15, branch `phase0`. Written for the Ghidra pass and for the importer design.

Sources:
- `bully-re` (engine reimplementation, the primary format authority): `/Users/midir/bully-remake-refs/bully-re`. Cited as `bully-re:<path>:<line>`.
- `Bully-SE-scripts` (decompiled Lua): `/Users/midir/bully-remake-refs/Bully-SE-scripts`.
- `bully-mact-tool` (CAT/MACT conversion): `/Users/midir/bully-remake-refs/bully-mact-tool`.
- The installed game, read only. Paths below are relative to the install root `Bully Scholarship Edition/`. Nothing was copied out. Hexdumps are header bytes only.

Confidence tags: **VERIFIED** = checked against the game bytes or against code and counts. **INFERRED** = matched by name or size only. **OPEN** = needs Ghidra or a further test.

---

## 0. Load-bearing findings

1. **Every archive pair uses the same directory format.** `.dir` = 32-byte records: `u32 offset (sectors)`, `u32 size (sectors)`, `char name[24]`. Sector = 2048. No compression is visible in any pair. World.img has 11,980 entries and no container header. VERIFIED.
2. **Every `.nif` and `.nft` is a Gamebryo 20.3.0.9 file.** The first 38 bytes are the ASCII line `Gamebryo File Format, Version 20.3.0.9`, then `\n`, then `u32 0x14030009`. The endian byte sits at offset 0x2B, and it is 1 (little) for most files, 0 (big) for a minority. `.nft` is a NIF whose blocks are `NiSourceTexture` and `NiPixelData`, not a DDS. VERIFIED.
3. **The school is three placement files plus their colliders.** Exterior: `tschool.ipb` (210 placements). Main interior (lobby, halls, cafeteria): `ischool.ipb` (234 placements). Rooms: `isc_*.ipb`. Each has a matching `.idb` in `Objects/ide.img`, a `.col` in World.img, and the models it names are all present. VERIFIED.
4. **The authoritative world manifest is text, not binary.** `Config/world.dat` (53 `IPL` lines, 77 `IDE` lines) and `Config/extra/world2.dat` (9 `IDE` lines) list what loads. The `IPL` entries point at `IPLs/School/*.ipl` text sources that are not shipped. The compiled `.ipb` files in World.img have the same stems. INFERRED for the stem mapping.
5. **There are no `.kf` or `.kfm` files and no MACT animation files in the install.** Jimmy's skeleton is a NIF (`PLAYER.nif`), his animation dictionary is the text-named HXD (`Anim/MAINPED.HXD`), and his clip bank is a loose `.agr` (`Anim/C_Player.agr`). `bully-mact-tool` handles action trees (`.cat` to `.mact`), not skeletal animation. VERIFIED.

---

## 1. IMG/DIR archives

### 1.1 Directory record (all pairs)

    struct CDirectoryEntry {      // 32 bytes, little-endian
        uint32 offset;            // in 2048-byte sectors from the start of the .img
        uint32 size;              // in sectors; the true byte length is NOT stored here
        char   name[24];          // NUL-padded, case-insensitive lookup
    };

Source: `bully-re:src/core/CdStream.h:11-15` (struct), `:17-24` (`CdImage`), `:33` (`AddImage`, 0x73a740, which opens `<stem>.img` and reads `<stem>.dir`). Lookup is case-insensitive (`bully-re:src/core/CdStream.cpp`, `Find`). The `.dir` is read as a flat array of 32-byte records until EOF.

Hexdump (`Stream/World.dir`, entries 0 and 1):

    00000000: 0000 0000 0400 0000 416c 6769 6531 2e6c  ........Algie1.l
    00000010: 7572 0000 0000 0000 0000 0000 0000 0000  ur..............
    00000020: 0400 0000 0400 0000 416c 6769 6545 7363  ........AlgieEsc

Entry 0 is offset 0, size 4 sectors (`Algie1.lur`). Entry 1 starts at sector 4. VERIFIED.

### 1.2 Pairs in the install

| Pair | Entries | .img bytes | Sectors | Contents | First-4-byte census |
|---|---|---|---|---|---|
| `Stream/World.img` | 11,980 | 1,927,151,616 | 940,992 | `.nif` 5,724 · `.nft` 4,469 · `.agr` 550 · `.lip` 493 · `.col` 488 · `.cat` 119 · `.ipb` 85 · `.lur` 52 | `Game` 10,193 (nif+nft), `00010000` 550 (agr), `COL3` 406, `COL2` 68, `COLL` 14, `Ipl$` 85, `\x1bLua` 52 |
| `Scripts/Scripts.img` | 515 | 5,081,088 | 2,481 | `.lur` (Lua 5.0 bytecode) | `\x1bLua` 515 |
| `Act/Act.img` | 479 | 2,465,792 | 1,204 | `.cat` (action trees) | no fixed magic; first u32 = byte length |
| `Cuts/Cuts.img` | 863 | 103,114,752 | 50,349 | `.agr` 170, `.hxd` 170 (132 upper-case, 38 lower-case), `.mxd` 168, `.dat` 126, `.inf` 122, `.sub` 107 | `.agr` starts 0x100; `.mxd` is binary (f32 1.06 at +0); `.sub` is text (`{1}{`); rest not surveyed |
| `DAT/Trigger.img` | 410 | 7,766,016 | 3,792 | `.dat` text, one per mission/location | `NPATHS` 410 |
| `Objects/ide.img` | 77 | 499,712 | 244 | `.idb` (binary IDE) | no fixed magic |

Checks run on all six pairs (script in scratchpad, read-only): `.dir` size is a multiple of 32; `.img` size is a multiple of 2048; no two entries overlap; the largest `offset+size` equals the image sector count in every pair. VERIFIED.

Compression: none seen. Each magic matches the raw format (Gamebryo header, `COL3`, `Ipl$`, `\x1bLua`, `0x100` AGR header). The `.cat` files are read raw by `bully-mact-tool` (see 5.5), so there is no container-level compression. "Compressed" in `bully-re:docs/cat.md` refers to the node encoding, not to zlib-style packing. OPEN: confirm in `FUN_005fb3f0` (CompressedActionTree::load).

Size caveat: the directory gives sectors only. For a NIF the exact length is the header plus the sum of block sizes, and the rest is zero padding. Example: `Act/AI.cat` has first u32 `0x9cac` = 40,108 bytes in a 20-sector (40,960-byte) entry. VERIFIED.

### 1.3 Other pairs

- `Scripts/Scripts.img` holds all 515 mission scripts. Streaming names them `%s.LUC` (`bully-re:docs/streaming.md`).
- `Act/Act.img`: 479 `.cat` files for characters (`bully-re:docs/cat.md`, `FUN_005f6f20` opens `Act\Act.IMG`).
- `Cuts/Cuts.img`: 863 entries for cutscenes (`.agr`, `.hxd`, `.mxd`, `.dat`, `.inf`, `.sub`). Pairing of the types is not checked; not surveyed beyond the census.
- `DAT/Trigger.img`: text, `NPATHS n` / `NPOINTS` / `NPERIMETERS` / `NTRIGGERS` / `NPOIS` header (`bully-re:docs/trigger.md:6-12`).
- `Objects/ide.img`: binary IDE sections (`bully-re:docs/idb.md`), loaded whole by `LoadImgIde` (0x42caf0).

---

## 2. NIF (Gamebryo 20.3.0.9)

### 2.1 Header layout (VERIFIED on the bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0x00 | 38 | ASCII `Gamebryo File Format, Version 20.3.0.9` | |
| 0x26 | 1 | `\n` | |
| 0x27 | 4 | version u32 = `0x14030009` | little-endian always |
| 0x2B | 1 | endian byte | **1 = little, 0 = big** |
| 0x2C | 4 | user version = 0 | |
| 0x30 | 4 | number of blocks (u32) | read little-endian in all files |
| 0x34 | 2 | number of block types (u16) | endian follows the byte at 0x2B |
| then | var | block type names: `u32 len` + bytes, one per type | |
| then | 2 × nblocks | type index per block (u16) | |
| then | 4 × nblocks | block size (u32) | lets unknown blocks be skipped |
| then | var | string table: count u32, max length u32, then `u32 len` + bytes | |
| then | var | groups: count u32, then u32 per group | |

Source: `bully-re:src/gamebryo/NifFile.cpp:416-421` (version, endian `r.be = endian == 0`, user version, block count, then `r.U16()` for types). The per-block reader also picks endianness from the byte (`bully-re:src/gamebryo/NifFile.cpp:447`). Doc: `bully-re:docs/nif.md:1-13`.

Hexdumps (first 64 bytes):

    Models/pickup.nif (loose, big-endian):
    00000000: 4761 6d65 6272 796f 2046 696c 6520 466f  Gamebryo File Fo
    00000010: 726d 6174 2c20 5665 7273 696f 6e20 3230  rmat, Version 20
    00000020: 2e33 2e30 2e39 0a09 0003 1400 0000 0000  .3.0.9..........
    00000030: 0d00 0000 000a 0000 0006 4e69 4e6f 6465  ..........NiNode

    World.img / PLAYER.nif (little-endian, sector 761532, 216 sectors):
    00000000: 4761 6d65 6272 796f 2046 696c 6520 466f  Gamebryo File Fo
    00000020: 2e33 2e30 2e39 0a09 0003 1401 0000 0000  .3.0.9..........
    00000030: da00 0000 1000 0600 0000 4e69 4e6f 6465  ..........NiNode

Decoded: `PLAYER.nif` has 218 blocks (0xDA) and 16 block types. `Models/pickup.nif` has 13 blocks and 10 types, big-endian (endian byte 0x00). The `pickup.nif` inside World.img is a different export: little-endian, 10 blocks, 9 types. VERIFIED.

Other checked headers: `Graphics/cube.nif` (LE, 12 blocks), `Models/GlowBeam.nif` (BE, 14 blocks), `Models/SkyDome/SkyTop.nif` (LE, 13 blocks), `Models/pointer.nif` (BE, 12 blocks), `World.img/first_floor.nif` (LE, 106 blocks), `World.img/SC1b_bldg_main.nif` (LE, 49 blocks), `World.img/LOBBY01.nif` (LE, 133 blocks).

Corpus facts from bully-re (not re-counted here): 5,724 NIFs in World.img, 247 big-endian (all `CS_*` cutscene files), 587,264 blocks, 363,085 decoded (`bully-re:docs/nif.md:3-8`, `:69`; `bully-re:docs/nft.md:66-89`).

OPEN: the loose `Models/` NIFs (`pickup`, `GlowBeam`, `pointer`) are big-endian, but bully-re's 247 big-endian count covers World.img only. Confirm which loader reads loose `Models/` and whether the loose and World.img versions of the same name are both live.

### 2.2 Block types and decode status (bully-re)

- Decoded layouts: `NiAVObject`, `NiNode`, `NiTriShape`, `NiTriStrips`, `NiTriShapeData`, `NiTriStripsData`, `NiSourceTexture`, `NiMaterialProperty`, `NiTexturingProperty` (9 slots), `NiAlphaProperty`, `NiStencilProperty`, and the skin trio `NiSkinInstance` / `NiSkinData` / `NiSkinPartition`. Source: `bully-re:docs/nif.md:36-117`.
- Skipped by size: transform controllers and interpolators, most extra data, specular, Z-buffer, vertex colour, effects (`bully-re:docs/nif.md`, section "Non décodés").
- Rotation convention: `v' = R·(s·v) + t`, with R read row-major and the transpose ruled out by 3,842 collision boxes (`bully-re:docs/nif.md:119-152`). Placements use the **transpose** of the quaternion (`bully-re:docs/ipl.md:28-52`). Do not reuse one convention for both.

### 2.3 Where the parser lives

- `bully-re:src/gamebryo/NifFile.h` and `NifFile.cpp`: header, block table, block decoders (dispatch at `NifFile.cpp:39`, `:454`).
- `bully-re:src/gamebryo/NifSkin.{h,cpp}`: skin formula. A vertex is the weighted sum of `M_bone · S_bone · v`, with no transpose (`bully-re:docs/nif.md:94-106`).
- `bully-re:src/gamebryo/NifTransform.{h,cpp}`: node composition and placement conversion.
- `bully-re:src/gamebryo/TextureDecode.{h,cpp}`: pixel decode (see section 3).
- Tests: `tests/test_nif.cpp`, `tests/test_skin.cpp` (PLAYER.nif), `tests/test_transform.cpp`, `tests/test_nft.cpp`, `tests/test_texture.cpp`.
- Tools: `outils/nif2obj.cpp`, `outils/rendu.cpp`, `src/render/SoftRaster.{h,cpp}`.

---

## 3. Textures: `.nft` and `.txd`

### 3.1 What a `.nft` is (VERIFIED)

- A `.nft` is a NIF file. Its header is identical to 2.1, and it carries only texture blocks. Source: `bully-re:docs/nft.md:1-11`.
- Block types: `NiPixelData` (35,635), `NiStringExtraData` (~35,600, holds the original `.tga` path), `NiIntegerExtraData` (~35,000, exporter version), `NiSourceTexture` (~35,400), `NiSourceCubeMap` (214), `NiPalette` (127). `bully-re:docs/nft.md:13-24`.
- Example: `TXD/bully1.nft` has 4 blocks, LE, types `NiSourceTexture, NiStringExtraData, NiIntegerExtraData, NiPixelData`. Hexdump (first 48 bytes):

      00000000: 4761 6d65 6272 796f 2046 696c 6520 466f  Gamebryo File Fo
      00000020: 2e33 2e30 2e39 0a09 0003 1401 0000 0000  .3.0.9..........
      00000030: 0400 0000 0400 0f00 0000 4e69 536f 7572  ..........NiSour

- `PLAYER.nft` (World.img) has 48 blocks with the same first types.

### 3.2 `NiPixelData` (bully-re:docs/nft.md:24-60)

    u32  pixelFormat       // 4 = DXT1, 6 = DXT5, 0 = RGB24, 1 = RGBA32, 2 = palette8
    u8   bitsPerPixel
    i32  rendererHint      // -1
    u32  extraData
    u8   flags
    u32  tiling
    u8   sRGB              // specific to 20.3
    4 x { u32 type; u32 convention; u8 bitsPerChannel; u8 isSigned; }
    i32  palette           // NiPalette ref, -1 if none
    u32  numMipmaps
    u32  bytesPerPixel
    numMipmaps x { u32 width; u32 height; u32 offset; }
    u32  numPixels         // bytes of one face, all mips
    u32  numFaces
    u8   pixels[numPixels x numFaces]

Block size is `79 + 12 x numMipmaps + numPixels x numFaces`. Five formats occur in the archive: RGB24 (134 blocks), RGBA32 (134), palette8 (127), DXT1 (31,714), DXT5 (3,526). DXT3 (5) never occurs. Per `bully-re:docs/nft.md:110-146` and the tests named there; not re-run here.

### 3.3 Endianness exception

131 `.nft` files are big-endian, all named `CS_*`. Only the header fields are swapped. Pixel bytes stay little-endian. A byte-swapped DXT reading gives chance-level colour order, so swapping pixels is wrong. `bully-re:docs/nft.md:91-98`.

### 3.4 `.txd` files

`TXD/colramp.txd` and `TXD/fe1.txd` begin with the same Gamebryo header (LE, endian byte 0x01). So `.txd` is the same container. `TXD/` also has `arcf1.nft`, `effects.nft`, `HUDElems.nft`, `JPFONT1.nft` and subfolders `Arcade/`, `ls/`, `lsw/`, `Missions/`. OPEN: confirm whether `.txd` decodes through the same path, since bully-re only documents `.nft`.

### 3.5 Other image formats

- `Graphics/UISkinFull.dds`: real DDS (magic `DDS `, header size 0x7C). Keep this as a standard DDS.
- `Graphics/NormalizedRandomDirections.tga`: TGA, uncompressed truecolour (image type 2), 512 x 512, 32 bpp. VERIFIED from the header bytes; file size 1,048,607 = 18 + 13 + 512 x 512 x 4.

### 3.6 Known exceptions (bully-re:docs/nft.md:99-109)

Four `.nft` (`BBonusB`, `Barr01_Switch`, `BeerKeg`, `BirdBath`) have one extra byte between blocks 3 and 4. The cause is unknown. Keep these as known failures in the importer tests.

---

## 4. World placement: which files put the school in the world

### 4.1 The manifest

`Config/world.dat` (CRLF text, 4,088 bytes) is the load order. Comments say: IDE first, then models, then IPLs, "loaded on a per directory basis and in alphabetical order".

- `IMGIDE OBJECTS/IDE` opens the IDE image (`Objects/ide.img`).
- 77 `IDE` lines name `.ide` sources. The school lines are `Objects/SchlIntr/isc_*.ide`, `Objects/SchlIntr/ischool.ide`, `Objects/SchlProp/psc_*.ide`, `Objects/Terrain/tschool.ide` and `Objects/Terrain/tGlobal.ide`.
- 53 `IPL` lines name `IPLs/…/*.ipl`. The school ones are `IPLs/School/tschool.ipl`, `ischool.ipl`, `isc_clas.ipl`, `isc_dorm.ipl`, `isc_Gdrm.ipl`, `isc_chem.ipl`, `isc_bio.ipl`, `isc_pool.ipl`, `isc_boil.ipl`, `isc_art.ipl`, `isc_lib.ipl`, `isc_offi.ipl`, `isc_staf.ipl`, `isc_auto.ipl`, `isc_prep.ipl`, `isc_audt.ipl`. The outdoor set includes `tGlobal.ipl`, `tbusines.ipl`, `trich.ipl`, `tindust.ipl`, `tcarni.ipl`, `tjyard.ipl`, `tgokart.ipl`, `tBMX.ipl`.
- `Config/extra/world2.dat` adds 9 IDE lines (`pAnimsI`, `isc_dorm`, `isc_Gdrm`, `psc_hall`, `psc_dorm`, `psc_Gdrm`, `psc_boil`, `props`, `access`).

The `IPLs/` folder is not in the install. The `.ipl` sources are compiled into World.img as `.ipb`. The stems match exactly for all 85 `.ipb` files I checked, so the mapping `IPLs/X/stem.ipl` to `stem.ipb` is INFERRED. OPEN: confirm the loader path (`bully-re:docs/streaming.md:19-24`, `LoadCdDirectoryIPB` 0x52ded0).

Related: `bully-re:docs/demarrage.md:52` (`CdStreamAddImage('Stream/world.img')` during startup).

### 4.2 Streaming and loading

- World.img is streamed by numeric id. `0x52e990` maps an id to a file name. Placements are ids `0x5af0..0x5b54` (100 slots), `%s.IPB` (`bully-re:docs/streaming.md:4-17`).
- The world is a 36 x 36 grid of 50 m sectors, from -900 to 900 on x and y (`bully-re:docs/world.md:7-22`).
- Outdoor loading is by tile: the quarter files `t*` (`tschool`, `tbusines`, `trich`, ...), street tiles `zone_*` (`zone_school1..6`, ...), and `tGlobal` (terrain, always loaded). Interiors `i*` are separate scenes. The loader pulls everything within 60 m and frees beyond 2x that (`bully-re:docs/rendu-temps-reel.md:99-117`).
- Only 41 placement files are loaded by location, out of the 85 in World.img. OPEN: reconcile with the 53 `IPL` lines in `world.dat`.

### 4.3 IPB binary layout (VERIFIED on 3 files by hexdump, parsed on 13)

    bytes 0..3     "Ipl$"
    bytes 4..18    15 bytes, identical in the files compared (not interpreted)
    byte  19..22   u32 section count (ischool 7, tschool 8, iboxing 4)
    byte  23..     section: tag (4 bytes, e.g. "inst"), u32 count, then count entries

`inst` entry (120 bytes, from `bully-re:src/core/IplFile.h:34-`):

    +0    i32   modelId
    +4    char  name[64]        // model name, NUL-padded; NIF is name.nif in World.img
    +68   f32   unk68           // 22.0 in ftest.ipb
    +72   f32[3] pos            // world position
    +84   f32[3] scale          // per axis
    +96   f32[4] rot            // quaternion x y z w
    +112  8 bytes unknown

Hexdump, `Stream/World.img` sector 478379 (`tschool.ipb`), first 48 bytes:

    00000000: 4970 6c24 53e7 b5fc b607 667a e5b6 83eb  Ipl$S.....fz....
    00000010: bfde b408 0000 0069 6e73 74d2 0000 0010  .......inst.....
    00000020: 1400 0046 4752 445f 5343 3162 3031 0000  ...FGRD_SC1b01..

Count 0xD2 = 210 entries. The first entry has modelId 0x1410 and name `FGRD_SC1b01`. Source of the layout: `bully-re:src/core/IplFile.h:1-19` (comment) and `:34` (struct).

### 4.4 The academy: which files cover what

Counts are from parsing the `inst` section of each `.ipb`. Bounds are in world metres. Placed model names were checked against World.dir for 7 files (tschool, ischool, isc_clas, isc_dorm, zone_school1, tGlobal, iboxing): 0 missing.

| File | Placements | Distinct models | Bounds x / y / z | What it is |
|---|---|---|---|---|
| `tschool.ipb` | 210 | 177 | x -84..377, y -260..75, z 0..58 | **Academy exterior and grounds**: `SC1b_bldg_main`, `SC1d_bldgmain`, `SC1b_bldgLib`, `SC1b_bldgGym`, `SC1b_bldgBdorm`, `SC1b_bldgGdorm`, `SC1b_bldgPrep`, `SC1b_bldgObs_A/B`, `SC1b_bldgAuto`, `SC1b_bldgFld`, `SC1b_field`, fences and gates, `FGRD_SC1b*` (name prefix suggests ground mesh; 13 variants placed), `NOGO_tschool*` and `WALKABLE_tschool*` (invisible markers) |
| `ischool.ipb` | 234 | 120 | x -677..-580, y -335..-263, z -10..18 | **Main school interior**: lobby (`LOBBY01`, `LOBBY02`), halls, cafeteria (`SCH_CafeMain`), `first_floor`, `second_floor`, washrooms (`SC_GBath*`, `SC_BBath*`, `StallDoors`), trophy case, `SCDwaysLobby2` |
| `isc_clas.ipb` | 228 | 78 | x -569..-491, y 314..331, z -3..8 | Classroom (`kiddesk`, `kidchair`) |
| `isc_dorm.ipb` | 113 | 63 | x -525..-480, y 297..330, z 31..36 | Boys' dorm (areabbox `Boys_Dorm`) |
| `isc_lib.ipb`, `isc_bio.ipb`, `isc_chem.ipb`, `isc_pool.ipb`, `isc_boil.ipb`, `isc_art.ipb`, `isc_audt.ipb`, `isc_auto.ipb`, `isc_prep.ipb`, `isc_offi.ipb`, `isc_staf.ipb`, `isc_Gdrm.ipb` | not parsed | | | Other rooms (library, biology, chemistry, pool, boiler room, art, auditorium, auto shop, prep house, office, staff room, girls' dorm) |
| `zone_school1..6.ipb` | 50 to 98 each | 16 to 36 | x -141..324 (six tiles), y -235..31 | Street dressing around the school quarter: lamps, benches, fir and willow trees, garbage, paths. Not building geometry |
| `tGlobal.ipb` | 139 | 76 | x -237..787, y -591..659 | Relief, water, pylons (always loaded) |
| `iboxing.ipb` | 58 | 45 | x -737..-699, y 363..392, z 294..304 | Boxing-ring interior, separate scene (`bully-re:docs/rendu-temps-reel.md`) |

Cross-check: `Config/dat/areabbox.dat` gives the room bounds used by the game. `School_Hallways` is x -678.484..-579.746, y -343.153..-261.27, z -1.38..15.22 (`Config/dat/areabbox.dat`, "Indoor_MultiFloor"). The `ischool.ipb` bounds sit inside it. VERIFIED. The same file gives `MainMap` = x -227.7..761.4, y -678.1..780.1, z -26.2..42 (`Outdoor`).

Colliders: `tschool.col` (178 sectors), `ischool.col` (52 sectors), `isc_*.col` (one per room). `zone_school1.col` is absent (only that one was checked). VERIFIED by name in World.dir.

IDE definitions: `tschool.idb`, `ischool.idb`, `isc_*.idb`, `psc_*.idb`, `tGlobal.idb` are all in `Objects/ide.img` (77 entries). VERIFIED.

Room to collision mapping: `Coll/ColAreas.cfg` groups the colliders per area, e.g. `Auditorium` = `isc_audt`, `psc_audt`; `Prep_House` = `isc_prep`, `pAnimsE`, `pAnimsI`. bully-re reads it at `0x56f420` (`bully-re:docs/collision.md:29-33`).

### 4.5 Other world data files

- `Data/excluder.dat` (1,162 bytes, CRLF): zone-exclusion list. Each line is `<Scope> <Name>`. Scopes seen: `Everyone`, `MissionOnly`, `AmbientOnly`, `Player_Restricted`, `FemaleOnly_Restricted`, `MaleOnly_Restricted`. Names include `GirlsBathroom1`, `BoysBathroom1`, `pool_girlslocker`, `SchoolStoreExc`, `GDormExitExclude`. VERIFIED by reading; semantics INFERRED.
- `lawnplacment/Mower_Placement.txt` (1,025 bytes): lawn-mower mini-game quads. Each block has `Z=`, `TL/TR/BL/BR` corners, `PLAYER1/2` start points. Example MOWER1 at about x -684, y 140. OPEN: which area that lies in.
- `Config/GRIDS.DAT` (682,718 bytes, text header): `NCARGRIDNODES 476`, `NPEDGRIDNODES 1234`, `NCARGRIDLINKS 507`, `NPEDGRIDLINKS 1365`. These are navigation graphs, not geometry.
- `Config/dat/areabbox.dat`: area bounds (above).
- `DAT/Trigger.img`: 410 text `.dat` files. School-relevant names: `School_Exterior.dat`, `eventsSchoolgrounds.dat`, `eventsSchoolHalls.dat`, `SP_School_Hallways.dat`, `ischool_doors.dat`, `ischool_fountains.dat`, `ischool_lockers.dat`, `ischool_sitting.dat`, `Patrol_SchoolGrounds.dat`.

---

## 5. Jimmy: model, skeleton, animation

### 5.1 Model and skeleton

- The player NIF is `PLAYER.nif` in World.img (sector 761532, 216 sectors, LE, 218 blocks). Its streaming name is `PLAYER.nif`. The loose files `Models/Peds/` hold only `MXDs.MGR`. VERIFIED.
- Skeleton: `NiSkinInstance` / `NiSkinData` / `NiSkinPartition`. The bone names follow Biped (`Root Pelvis`, `Root L Thigh`, `Root Spine1`, `Root L Clavicle`, ..., `Root Head`, `Root Ponytail1`). Source: `bully-re:docs/nif.md:78-117`. A pedestrian has 36 bones (`bully-re:docs/agr.md`, section on bone counts).
- Vertex formula: `Σ weight · (M_bone · S_bone)(v)`, no transpose. Tested on PLAYER.nif (2.3 mm error). `bully-re:tests/test_skin`.
- Reference: `Models/Peds/MXDs.MGR` (555,148 bytes, LE). First u32 = 0x166 = 358 entries. Entry 0 name = `MODELS\PEDS\PLAYER`. Then the animation dictionary name (`ANIM\MAINPED`), attachment points (96 bytes each), and per-bone vectors. Source: `bully-re:docs/mxd.md:1-38`. VERIFIED on the header bytes.

### 5.2 Animation dictionary (HXD)

- `Anim/MAINPED.HXD` is the dictionary: version f32 `1.12` (bytes `295c8f3f`), then an f32 at +4 (value about 118.14, purpose OPEN), then `u32 46` masks, then 32-byte mask names (`default`, `UpperBody_ALL`, ...). Source: `bully-re:docs/hxd.md:16-30`. VERIFIED for version and mask count.
- MAINPED.HXD has 399 lines matching `C_PLAYER` (`grep -c`, binary file). These are the player's group names (e.g. `C_PLAYER\PUNCH_SLOP_1`, per `bully-re:docs/cat.md:86-90`).
- `Anim/hxds.dat` packs 130 HXDs. It has no `C_PLAYER` text, so it is not where the player's group names are stored. OPEN: whether `MAINPED` is packed inside it.

### 5.3 Clip bank (AGR)

- `Anim/C_Player.agr` (1,327,168 bytes, loose, not in World.img). Header: u32 `0x100`, u32 type `0x3EA` = **1002** (8-byte keys, compressed quaternion), u32 key count `0x163` = 355, u32 0, f32 duration. VERIFIED on the header bytes.
- AGR format: `bully-re:docs/agr.md:1-40`. Six key encodings (types 999 to 1004). Type 1002 is 80 percent of animations.
- The loader for `C_PLAYER` group names to `C_Player.agr` is INFERRED from the name. OPEN for Ghidra.
- World.img has 550 `.agr`, including `Player_Tired.agr`, `Player_VTired.agr`, `SPLAY_A.agr`, `SPLAY_B.agr`. These are streamed, not the main player bank.
- `Anim/` also holds loose `Grap.agr` (181,868 bytes), `MOT_CTRL.agr`, `NPC_Cher.agr`.

### 5.4 Action tree vs. animation

Animation is referenced by the action trees (`.cat` in Act.img) through group and clip names. The trees decide which clip plays.

### 5.5 What bully-mact-tool says about MACT

Source: `bully-mact-tool/README.md` and `CAT_TO_MACT.py`, `MACT_TO_CAT.py`.

- MACT is "a customized version of Bully's ACT files". The tool converts `.cat` to `.mact` and back (`CAT_TO_MACT.py` V4.2, `MACT_TO_CAT.py` V4.2, by marcdred). Optional `--po` parameter optimization on the reverse path, and template files `TEMPLATES_CONDITIONS.txt` / `TEMPLATES_TRACKS.txt` to rename parameters.
- It is a **tree editor** for action trees, not an animation format. It has no keyframes, bones or quaternions. Its CAT header, read at `CAT_TO_MACT.py:332-345`, is nine u32 values: file length, `p_data`, `p_strings`, `p_groups`, four counters, string count. Node data follows. Hash databases in `DB/HASHES_*.txt` map names.
- Verified against `Act/AI.cat`: first u32 = byte length.
- Conclusion: MACT does not help with Jimmy's skeleton or clips. It is relevant to the action-tree port only.

### 5.6 Keyframe files

No `.kf`, `.kfm`, or `.mact` file exists in the install (`find` over the whole install returned none). Animation is only `.agr` (clips), `.hxd` (dictionaries), `.mxd` (attachments), and `.cat` (action trees).

---

## 6. Audio (brief)

Layout under `audio/` (346 files, 2.2 GB):

- `audio/xact/`: 322 `.SEA` (30.0 MB total, largest 538 KB) and one `.xgs`. The `.xgs` starts with `XGSF` and two u16 values (0x2C, 0x2A), which is consistent with Microsoft XACT. No `.xwb` or `.xsb` files exist. VERIFIED.
- `.SEA` header (VERIFIED on 322 files): five u32 values, then strings. `u32[0] + u32[1] + u32[2] = file size` for all 322 files. `u32[2]` is probably the audio data length, `u32[0]` and `u32[1]` are metadata blocks, and `u32[3]` (values 1 to 137) is probably the entry count. The first record of `1_04.SEA` holds a NUL-terminated name, a 4-byte value and an authoring-machine path. The layout of later records is OPEN. OPEN: confirm the field meanings and the codec. Do not republish the embedded paths.
- `audio/CONFIG/`: 11 text files and 2 binary tables. `areabank.txt` is tab-separated `<area>\t<bank>` (the other `.txt` files were not inspected). Bank names point to `EMITTERS\...\*.bnk`; no `.bnk` file exists under `audio/`. OPEN: map each `.bnk` name to an `.SEA`. `IMusic.bin` and `SpeechDB.bin` are binary tables. `IMusic.bin` starts with `0x11, 0x12, 3, 0x46`. Format not documented.
- `audio/PLAYLIST/`: `Ambs.lst` is a CRLF list of `.rsm` names (the other three `.lst` files were not inspected). The four `.bin` siblings are binary. `Ambs.bin` starts with the ASCII `Hash` tag.
- `Bully Original Soundtrack/`: 26 `.mp3` and one `.jpg`. Not part of the engine audio.
- `Movies/`: 8 `.wmv`. Cutscene video, not surveyed.

Lua: the game uses Lua **5.0.2**, with floats as numbers. Bytecode header: `\x1bLuaP`, version 0x50 (`bully-re:docs/formats-restants.md:8`, `bully-re:docs/lua.md`). Scripts are in `Scripts/Scripts.img` (515) and 52 in World.img.

---

## 7. Open questions for Ghidra

OPEN-1. Loader for the loose `Models/` NIFs (`pickup`, `GlowBeam`, `pointer`): big-endian, and different from the World.img `pickup.nif`. Which path loads them, and does the game ever use both?

OPEN-2. `Config/world.dat` IPL names (`IPLs/School/*.ipl`) to World.img `.ipb` names. Confirm the stem rule in the loader (`0x52ebf0` / `0x52ded0` / `0x438f50`).

OPEN-3. The 15 bytes after `Ipl$` (identical in the files compared) and the unlabelled fields in `inst` entries (`unk68`, `unk112`, `unk116`).

OPEN-4. Which IPB files are loaded by location (41 per `bully-re:docs/rendu-temps-reel.md`) versus the 53 `IPL` lines in `world.dat`. Is the selection in code or in `ColAreas.cfg`?

OPEN-5. Mapping `C_PLAYER\...` group names (in MAINPED.HXD) to `Anim/C_Player.agr`, and whether the player loads it from the loose folder, not from streaming.

OPEN-6. MAINPED.HXD header field at +4 (f32, about 118.14). Is it a duration, a scale, or a version-dependent value?

OPEN-7. Byte length of streamed entries. The directory stores sectors only. Does the engine ever read a short trailing sector, or does it always parse block sizes?

OPEN-8. The four `.nft` with one extra byte between blocks 3 and 4 (`BBonusB`, `Barr01_Switch`, `BeerKeg`, `BirdBath`). Cause unknown.

OPEN-9. Whether `.txd` (e.g. `colramp.txd`, `fe1.txd`, `gametypes.txd`) decodes through the same path as `.nft`.

OPEN-10. `.SEA` header field meanings (`u32[1]`, `u32[3]`), codec, and the `.bnk` to `.SEA` mapping. `IMusic.bin` and `SpeechDB.bin` layouts.

OPEN-11. `Data/excluder.dat` scope semantics (`Everyone`, `MissionOnly`, `Player_Restricted`, ...). Confirm in code.

OPEN-12. `Cuts/Cuts.img` (863 entries): format of `.inf`, `.sub`, `.dat`, and the `.MGR` routing. Not surveyed.

OPEN-13. Whether any `.agr` or `.cat` file is stored packed. None was detected by magic, and bully-mact-tool reads them raw.

OPEN-14. Which mower placement area is `lawnplacment/Mower_Placement.txt` in, and which world file it refers to.

OPEN-15. `.cat` "compressed" wording (`bully-re:docs/cat.md`): does `FUN_005fb3f0` (CompressedActionTree::load) apply a bit-packed encoding on top of the plain header that bully-mact-tool reads? The tool reads the raw file without decompression, so the answer should be "no outer compression", but confirm.

---

## Sources cited

- `bully-re`: `src/core/CdStream.h:11-33`, `src/core/CdStream.cpp`, `src/core/IplFile.h:1-34`, `src/gamebryo/NifFile.h`, `src/gamebryo/NifFile.cpp:39,416-421,447,454`, `src/gamebryo/TextureDecode.h:4-19`, `src/anim/Agr.h:1-20`, `tests/test_nif.cpp`, `tests/test_skin.cpp`, `tests/test_nft.cpp`, `tests/test_texture.cpp`, `tests/test_placement.cpp`.
- `bully-re` docs: `docs/nif.md`, `docs/nft.md`, `docs/streaming.md`, `docs/world.md`, `docs/ipl.md`, `docs/rendu-temps-reel.md`, `docs/collision.md`, `docs/trigger.md`, `docs/agr.md`, `docs/hxd.md`, `docs/mxd.md`, `docs/cat.md`, `docs/idb.md`, `docs/formats-restants.md`, `docs/lua.md`, `docs/demarrage.md`.
- `bully-mact-tool`: `README.md`, `CAT_TO_MACT.py:1-20,332-375`, `MACT_TO_CAT.py:1-12`, `DB/HASHES_*.txt`, `TEMPLATES/*.txt`.
- `Bully-SE-scripts`: `README.md` (Lua decompilation; Lua 5.0 compile target). The scripts were not needed for the formats.

Reference repos are licensed by their authors. This brief describes the formats in its own words and quotes only small struct layouts with citations.

## Corrections and resolved OPENs (2026-10-15, after importer build)

- **Placement counts:** tschool.ipb has **215** placements and ischool.ipb **236** (each .ipb has a second placement section the first survey pass missed). The 210/234 figures above are superseded.
- **OPEN-9 resolved:** the seven loose `.txd` files decode through the same NIF/NiPixelData path as `.nft`.
- **OPEN-8 explained:** `BBonusB`, `Barr01_Switch`, `BeerKeg`, `BirdBath` were corrupted upstream by a LF→CRLF line-ending conversion (headers end CRLF, no bare LF bytes anywhere; some original CRLFs ambiguous). Not exactly reversible; the importer skips them with a warning.
- **New inference:** the unknown float at +68 in each placement record behaves like an **area code** (0 outdoors, 2 ischool, 15 isc_clas). Inferred only — confirm in Ghidra.
- Still on Gamebryo-doc trust rather than verification: normal-map green-channel direction; texture wrap bit layout (school slots all read `0x3200`).
