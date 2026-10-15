# Audio formats: `.SEA` sound banks and `PLAYLIST/*.bin` streams (Phase 0, milestone 4)

Status: 2026-10-15, branch `phase0-ambient`, builder B4. Code: `tools/importers/audio.py`
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
| Sound-bank (`SDBK`) cue/sound/track graph | NOT decoded. Only the header magic is checked; cue names are plain strings in the bank. Footsteps are picked by wave name instead | OPEN |
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
  `PedFemaleShoe01..04_8va`. The courtyard is stone, so the cement sets are used; other surfaces (`Shoe_Grass_*`,
  `Gravel*`, `SHOE_WOOD_*`, `Snow_*`, `Sand_*`, `Water Walk *`, rug, metal) are in the same bank, un-wired (no surface
  probe yet).
- ambience: `Ambs/NewSchoolOutdoor_F` (loop, volume 0.8) + `Ambs/Birds01` (0.45). The short `Wind.SEA` / `Birds.SEA`
  loops (11.1 s, 4.9 s) also decode and are the fallback.

## 5. Open questions

- AUDIO-1: `SDBK` layout (cue -> sound -> track -> wave): needed for real cue playback (variations, volume/pitch
  ranges, loop flags, RPCs). Decode against the XACT 2 sound-bank documentation and confirm in `Bully.exe`.
- AUDIO-2: the name hash in the `PLAYLIST` index and the area -> stream mapping.
- AUDIO-3: which loop information is authoritative: the wave bank's loop region is set on every entry (also one-shots);
  the sound bank's play-wave flags probably decide.
- AUDIO-4: speech/music routing (`SpeechDB.bin`, `IMusic.bin`), 40,400 speech streams decode but are unnamed beyond the
  `.lst` file names.
