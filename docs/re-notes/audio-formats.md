# Audio formats: `.SEA` sound banks and `PLAYLIST/*.bin` streams (Phase 0, milestone 4)

Status: 2026-10-15, branch `phase0-ambient` (builder B4); SDBK cue graph decoded on `polish1` (section 6). Code: `tools/importers/audio.py`
(`python3 -m tools.importers audio survey|list|extract`), tests `tools/tests/test_audio.py` (synthetic fixtures, plus
opt-in checks against the install). Confidence tags as in `format-survey.md`: **VERIFIED**, **INFERRED**, **OPEN**.
No audio is committed to the repo; decoded WAVs go to `imported/audio/` (git-ignored).

## 0. Result

| What | Result | Confidence |
|---|---|---|
| `audio/xact/*.SEA` (322 files) | outer 5-word header, then an XACT **sound bank** (`SDBK`) and an XACT **wave bank** (`WBND`, version 44, header version 42, the XACT 2 layout). All 322 parse | VERIFIED |
| Wave entries | **4,512 entries, every one mono MS-ADPCM** (128 samples per 70-byte block). Decoded to 16-bit PCM WAV | VERIFIED |
| Decoder | bit-exact against ffmpeg's `adpcm_ms` decoder (mono footsteps and a stereo stream, 0 differing samples); decoded length equals the declared duration for all 4,512 entries | VERIFIED |
| `audio/PLAYLIST/{Ambs,Cuts,Music,Speech}.bin` (2.1 GB) | index table + one `SDBK`+`WBND` blob per stream, same bank layout, **ADPCM, mono and stereo**. 143 / 126 / 263 / 40,400 entries; every entry of Ambs, Cuts, Music and a 200-entry sample of Speech parse | VERIFIED |
| `.bnk` names in `audio/CONFIG/*.txt` -> `.SEA` | 313 of 320 distinct bank stems are the stem of a `.SEA` file (e.g. `FEET\FeetSCHL.bnk` = `FeetSCHL.SEA`); missing: `02, 6_3plyr, 6_4plyr, audtest, b, feetassy, surftest` | VERIFIED (names) |
| Sound-bank (`SDBK`) cue/sound/track graph | **Decoded** (section 6): XACT 3 sound-bank layout (tool 44, format 43); all 322 banks, 11,075 cues share ONE shape (simple cue -> 71-byte complex sound -> 1 track -> 1 clip -> 1 type-4 PlayWave event -> wave index + pitch/volume ranges). Footstep cues per surface, role and gait are mapped from it | VERIFIED (parse of all banks; cue -> wave names coherent, e.g. `WALK_GRASS0..2` -> `Shoe_Grass_03/04/06`) |
| Streamed bank name hash (`PLAYLIST` index) | not identified (not CRC32 of the name with or without extension / case folding); the index order equals the `.lst` line order | OPEN |

## 1. `.SEA`

    u32 sdbk_off, u32 sdbk_len, u32 wbnd_len, u32 a, u32 b      (a: 1..137, b: 0..54, both vary per file; OPEN)
    ... authoring strings (project name, tool paths; not republished) up to sdbk_off ...
    sdbk_off:                 'SDBK' XACT sound bank          (tool version 0x2C, format 0x2B)
    wbnd_off = sdbk_off + sdbk_len:
                              'WBND' XACT wave bank, wbnd_len bytes

`u32[0] + u32[1] + u32[2] == file size` holds for all 322 files (this is what the survey calls "header fields"):
`u32[0]` = offset of the sound bank, `u32[1]` = its length, `u32[2]` = length of the wave bank. `u32[3]` equals the
wave-bank entry count in only 79 of 322 files, so it is not that (the survey guessed so); `u32[3]` and `u32[4]` are
probably sound-bank counts (cues / sounds), OPEN. The wave-bank entry count is in the bank data.

### WBND (offsets relative to the `WBND` magic)

    char[4] 'WBND'; u32 version (44); u32 header version (42)
    5 x { u32 offset, u32 length }:  bank data, entry metadata, seek tables, entry names, entry wave data
    bank data (0x60 bytes): u32 flags (0x00090000 in every bank), u32 entry count, char[64] bank name,
                            u32 metadata element size (24), u32 name element size (64), u32 alignment (4),
                            u32 compact format (0), 8 bytes build time
    entry metadata (24 bytes each):
        u32 flags_and_duration     flags = low 4 bits (0), duration in samples = value >> 4
        u32 format                 bits 0-1 codec (0 PCM, 1 XMA, 2 ADPCM, 3 WMA), bits 2-4 channels,
                                   bits 5-22 sample rate, bits 23-30 block align, bit 31 16-bit flag
        u32 play offset, u32 play length      bytes inside the wave-data segment
        u32 loop start, u32 loop length       samples; set on one-shots too, so not a loop flag
    entry names: 64-byte NUL-padded strings, one per entry (present in all banks)

For ADPCM the stored block align is 48 and the real block is `(stored + 22) * channels` = 70 bytes mono
(128 samples/block). Sample rates are odd values (14,600 to 22,539 Hz; e.g. `Shoe_Grass_06` 14,700): the banks
carry per-sound tuned rates, used as-is for the WAV.

## 2. MS-ADPCM decode (standard Microsoft ADPCM)

Block header grouped by field: `u8 predictor[ch]`, `i16 delta[ch]`, `i16 sample1[ch]`, `i16 sample2[ch]` (7 bytes per
channel; mono and stereo differ in layout only here), then nibbles high-first, alternating channels for stereo.
Output starts with `sample2, sample1`. Predictor `(s1*c1 + s2*c2) / 256` with C-style division (truncate toward
zero; a floor shift is off by one on negative predictions and fails the ffmpeg comparison), standard coefficient pairs
(256,0) (512,-256) (0,0) (192,64) (240,0) (460,-208) (392,-232), delta update `(table[nibble] * delta) >> 8`, minimum
delta 16. The decoder is written from the public format description.

## 3. `audio/PLAYLIST/*.bin` (streamed banks)

    'Hash', u32 count, count x { u32 name hash, u32 byte offset, u32 byte size }   (offsets sector aligned, 0x800)
    each blob:  'SDBK' at +0, 'WBND' at +2048 (every Ambs entry checked), one wave entry named like the stream

Entry i is line i of the `.lst` of the same name (`Ambs.lst` has 143 lines = the count). Wave entries decode with the
same code; `NewSchoolOutdoor_F` (the school's outdoor ambience, 166.7 s, 22,049 Hz **stereo**), `Birds01`
(54.0 s mono) and `Rich_OutdoorCoutyard01REV` come out as clean WAVs. The decoded sample count of a stream can be a
little shorter than the declared duration (`NewSchoolOutdoor_F`: 3,675,632 vs 3,677,056): the last block is cut.
Which `Ambs` entry belongs to which area is in the sound bank / AI data (OPEN): `AreaAmbi.txt` only lists
indoor emitters. `NewSchoolOutdoor_F` is chosen by name (INFERRED: `_F` = day/fall variant of the campus outdoors;
`NewSchoolOutdoor_B22` is its twin).

## 4. What milestone 4 uses

`unreal/BullyRemake/Config/AmbientLoops.json` names the decoded sounds (bank + wave name); `tools/ue/import_ambient.py`
turns the matching WAVs into SoundWave assets (`/Game/Imported/Audio/<bank>/<name>`), `UBullyFootstepComponent` plays a
random one per planted foot, `AAmbientSoundActor` loops the outdoor ambience.

- footsteps (FeetAll.SEA): player `Sneaker Walk Cemnt_03/04/06/08`; students `Shoe_Cement_04/05/06_8va14k`; girl
  `PedFemaleShoe01..04_8va`. M4 hard-wired the cement sets by wave name (no cue graph); **polish1 replaced this with the
  decoded cue graph per surface (section 6)**: the same waves turn out to be exactly the player's / students' / girls'
  cement cue families, now pitched as the bank says and chosen by the surface under the foot. The M4 sets remain as the
  fallback when the per-surface tables are absent.
- ambience: `Ambs/NewSchoolOutdoor_F` (loop, volume 0.8) + `Ambs/Birds01` (0.45). The short `Wind.SEA` / `Birds.SEA`
  loops (11.1 s, 4.9 s) also decode and are the fallback.

## 5. Open questions

- AUDIO-1: ~~`SDBK` layout~~ **decoded for every bank in `audio/xact/*.SEA`** (section 6). Still open: the streamed `PLAYLIST`
  banks' SDBK (not parsed with the same reader yet), the two RPC codes (0xE0 / 0xF7 on every sound; presumably distance
  and occlusion curves, which live in the `.xgs` global settings / game code), and the category index (always 1 here).
- AUDIO-2: the name hash in the `PLAYLIST` index and the area -> stream mapping.
- AUDIO-3: which loop information is authoritative: the wave bank's loop region is set on every entry (also one-shots);
  the sound bank's play-wave flags probably decide.
- AUDIO-4: speech/music routing (`SpeechDB.bin`, `IMusic.bin`), 40,400 speech streams decode but are unnamed beyond the
  `.lst` file names.

## 6. `SDBK`: the cue graph (polish1; `tools/importers/audio.py` `parse_sdbk`, `cue_table`, `resolve_footsteps`)

The sound bank is the XACT 3 format as read by the open FAudio/FACT reader (tool version 44, format version 43,
platform 1) with ONE shape in all 322 `.SEA` banks (11,075 cues). `parse_sdbk` raises on anything else, so a bank that
differs cannot silently mis-map. Offsets are relative to the `SDBK` magic. Confidence: VERIFIED against the data
(structure sums, every wave index in range, cue names coherent with the waves they play, `-1200` cents paired with `*8va*` waves).

    0x00  'SDBK', u16 tool 44, u16 format 43, u16 crc, u64 FILETIME
    0x12  u8 platform
    0x13  u16 n_simple, u16 n_complex (always 0), u16 ?, u16 n_total (NOT the cue count: 16 in many banks)
    0x1B  u8 n_wavebanks (always 1)
    0x1C  u16 n_sounds (= n_simple), u16 cue-name table length, u16 ?
    0x22  10 x u32: simple_cues, complex_cues (-1), cue_names, ?, variation_tables (-1), ?, wavebank_names,
                    cue_name_hashes, cue_name_hash_values, sounds
    0x4A  char[64] bank name, then char[64] wave-bank name table (offset above)
    sounds: n x 71 bytes (listed in cue order, sound i = cue i)
    simple cue: u8 flags (4) + u32 offset of its sound; cue i's NAME is the i-th NUL-terminated string of the name table

    sound (71 bytes)
       u8 flags (3 = complex + RPC), u16 category (1), u8 volume, s16 pitch, u8 priority (255), u16 entry length (71)
       u8 n_tracks (1), u16 extra length (11 = 2 + 9), u8 n_rpc (2), u32 rpc codes 0xE0, 0xF7
       track: u8 volume, u32 clip offset, u16 filter frequency (24000), u16 filter Q (1000)
       clip @ clip offset (= sound + 30): u8 n_events (1), then ONE 40-byte event:
          u32 header (0x20000004: low 5 bits = event type 4 = PlayWave with effect variation, next 16 bits = timestamp 0)
          u16 random offset, u8 0xFF, u8 4, u16 WAVE INDEX, u8 wave-bank index (0)
          5 reserved bytes, s16 pitch_min, s16 pitch_max, u8 volume_min, u8 volume_max (= the sound volume),
          4 x f32 (1000, 1000, 30, 30: filter frequency / Q min and max), 2 bytes (0x00 0x30)

- **Volume**: byte -> dB `(b / 255) * 102.0206 - 96` (180 = -24 dB, identical on all 11,075 cues; the distance falloff is
  the RPC curves, not decoded, so the game-side attenuation is the project's own linear one).
- **Pitch** is in **cents** (1/1200 octave): the sound's base pitch plus a uniform random offset in [pitch_min, pitch_max]
  per play (typically -300..+200). `-1200` pairs with the waves named `*8va*` / `*8va14k` (recorded an octave up and
  stored at ~14 kHz to save memory; playing them at pitch 1.0, as milestone 4 did, sounds an octave too high and half
  as long). Base pitch rises with gait: walk 0 / run +300 / fast run +500 for the unpitched waves (grass, mud, sand, metal).
- **Variations are separate cues**, not a variation table (n_complex = 0): `WALK_GRASS0, WALK_GRASS1, WALK_GRASS2` are
  three cues; the game picks among them (random, presumably; the footstep component never plays the same one twice in a row).
- 2 of the 11,075 cues repeat a name inside their bank (the name -> cue dict in `cue_table` keeps the last one).

### Footstep cue naming (`FeetAll`, 459 cues; the other `Feet*` banks use the same grammar)

    <GAIT>_<SURFACE>[_PL][_DRY|_WET]<n>
    GAIT     WALK, RUN, FST_RUN (students' fast run) / FRUN (player's fast run), WLK_..._STLTH (stealth walk),
             JUMP_, STAIR_, SCUFF_, STOP_, CLIMB<n>
    SURFACE  CEM_DRY/CEM_WET (cement, wet = puddle splash waves), GRASS, GRASS_FR (frozen), METAL, MUD, SAND, FLR (hard floor,
             wood), LINO (linoleum/tile), DOCK, CARPET, GRAVEL, DIRT (same Gravel waves as GRAVEL), PUDDLE, SNOW, ICE,
             HI_HEEL(WET) (girls' shoes), WATERWADE, ATTIC, DORM
    _PL      the PLAYER's variant (sneakers: `Sneaker Walk Cemnt_xx`, `Sneaker Walk Wood xx`, `Sneaker Walk Lino xx`); P_WALK_/P_RUN_
             = player-only metal and dirt; cues without _PL are the NPC (loafer) sounds, shared by the player where no _PL exists

Which bank an area loads is `audio/CONFIG/areabank.txt` (`School_Hallways`, `Classroom`, `Staff_Room`, `Principal`, `Art_Room` ->
`FEET\FeetSCHL.bnk`; `Boys_Dorm`, `Prep_House` -> `FeetDORM`; `Chem_Plant` -> `FeetCHEM`; `Test_Area`, `Island_3` -> `FeetAll`;
`Janitors_Room` -> `FeetJan`). FeetSCHL (135 cues) only has indoor hard-floor/carpet families; outdoors is FeetAll.

### Wiring (polish1)

    python3 -m tools.importers audio cues FeetAll FeetSCHL              # imported/audio/cues/<bank>.json (cue -> wave, pitch)
    python3 -m tools.importers audio extract FeetAll FeetSCHL           # the WAVs
    python3 -m tools.importers audio footsteps                          # resolve Config/FootstepSurfaces.json -> imported/audio/footstep_cues.json
    tools/ue/run_import_ambient.sh --steps audio                        # SoundWave assets + Content/Imported/Audio/footstep_cues.json

`unreal/BullyRemake/Config/FootstepSurfaces.json` (hand-authored, no game content) holds (1) per surface and role
(`player` / `student` / `female`) the ordered cue-family templates (`{G}_CEM_PL_DRY`, `{G}_GRASS`, ...) with per-gait prefixes
and bank order per context (`exterior` = FeetAll; `interior` = FeetSCHL then FeetAll), and (2) the substring rules that map
the BASE-TEXTURE name (then the model name) under the foot to a surface. `UBullyFootstepComponent` traces down from every
foot plant (complex, world static/dynamic objects), takes the material of the triangle hit
(`UPrimitiveComponent::GetMaterialFromCollisionFaceIndex`), reads its `BaseColor` texture name (`ZRC_grassE_swap_d` -> grass,
`SC28_tile_01_d` -> lino, `ZBU_sidewalkM_swap_d` -> cement, ...), and decides interior vs exterior from the actor's
`br_scene:` tag (`ischool*`, `isc_*` = interior pockets). Surface defaults: cement outdoors, lino indoors. Real surfaces
(`surface.dat`, per-triangle in the `.col` files) are not imported yet; this is the name-based stand-in the task asked for.
`BR_AUDIO surface <who>: a -> b [tex=... model=... scene=...]` logs every surface change, `BR_AUDIO step` every plant with
`surface= role= gait= cue=`. Checked headless (`-nullrhi`): `tools/ue/walk_polish_surfaces.json` maps the courtyard
(grass at the street, `ZBU_cobroaddirt` dirt on the central and crossing paths, cobble and sidewalk = cement, the lobby
floor = lino).
