<!-- OWNER: builder-A (importers) -->
# AE (Anniversary Edition, iOS) `.tex` textures

Source: 14,064 `bully/*.tex` in `data_1.zip`. Code: `tools/importers/wtex.py`
(`parse_tex`, `decode_tex`, `write_tex_png`); tests: `tools/tests/test_wtex.py` (synthetic). Numbers
below were measured by scratch scripts outside the repo over the real install; game bytes never enter git.

## Layout (little-endian, all 14,064 files)

    u32 7, u32 2, u32 0x6e, u32 0x18        constant in every file
    u32 mode_tag                            tm_standard/noatc 9, nopvr 5|7, raw16 3|4, raw32 0|1, singlechannel 8, distancefield 112
    u32 data_start                          = 28 + text_len
    u32 text_len
    char text[text_len]                     {width=,height=,mode=,nomips=,compressondisk=,addressmodeu=,addressmodev=,
                                             samplerstate={...},df_params={...},pp_params={...},importfilepath="..."}
    u32 fmt, u32 width, u32 height, u32 mips, u32 data_size
    u8  data[data_size]                     ends exactly at EOF in every file

The text has nested braces and a quoted Windows path (may contain commas), so it needs a real parser
(`_parse_text`). `fmt` (the second one, after the text) is authoritative; `mode_tag` equals it except
for tm_standard (9 even when `fmt` is 0 or 1) and tm_distancefield (112 vs `fmt` 8).

`compressondisk=true` (177 files): `data` = `u32 raw_size` + zlib stream (`78 da`); every stream inflates
to exactly `raw_size`. `mode` only records how the texture was authored; the codec comes from `fmt`.

## Format ids (verified by size arithmetic on all files, and by eye)

| fmt | files | encoding | mode(s) | levels stored |
|---|---|---|---|---|
| 9 | 12,503 | PVRTC1 4bpp RGB(A) | tm_standard 12,343, tm_noatc 160 | `mips - 1` (1x1 dropped) |
| 5 | 422 | DXT1 / BC1 (8-byte 4x4 blocks) | tm_nopvr | `mips` |
| 7 | 184 | DXT5 / BC3 (16-byte blocks; alpha uses the 6-value 0..255 mode) | tm_nopvr | `mips` |
| 0 | 601 | RGBA8888, byte order R,G,B,A | tm_standard 559, tm_raw32 42 | `mips` |
| 1 | 30 | RGB888, byte order R,G,B | tm_standard 26, tm_raw32 4 | `mips` |
| 3 | 171 | RGB565 | tm_raw16 | `mips` |
| 4 | 144 | RGBA4444, R = high nibble, A = low nibble | tm_raw16 | `mips` |
| 8 | 9 | L8 (8-bit single channel; fonts are distance fields) | tm_singlechannel, tm_distancefield | `mips` |

Level table: level `i` is `max(w>>i,1) x max(h>>i,1)`, tightly packed, level 0 first. PVRTC level size is
`max(w,8)*max(h,8)/2`, so the 4x4, 2x2 (and 1x1) levels are each 32 bytes (padded to 8x8, take the
top-left corner). For every file `sum(level sizes) == raw size` with the counts above. No 2bpp PVRTC, ETC,
ATC or LZ variant exists in the archive (the 237 PVRTC files whose size also fits "16-byte blocks" are tiny
8x8..16x16 coincidences). Ruled out: DXT3 (alpha blocks are `00 05 00..` / `00 05 ff..`, i.e. DXT5 6-value mode).

## Stored vs declared size

Declared `width`/`height` differ from stored in 4,134 files, all fmt 9. Two cases:
* non-square textures are stored stretched to the square of the longer side (declared 256x128 -> stored
  256x256; 128x256 -> 256x256; 1024x512 -> 1024x1024). Restore the aspect with a box filter
  (`decode_tex(declared=True)`).
* 8x8 stored with a larger declared size (e.g. declared 128x128, `mips=4`, 186 files; a further 51 are genuinely 8x8): placeholder/low-res
  payload. `declared=True` upsamples it nearest-neighbour; consumers can instead keep the stored size.
All other formats are stored at the declared size. Pixel rows are top-down (no vertical flip needed).

## PVRTC1 4bpp

Block = u32 modulation + u32 colour, 8 bytes, 4x4 pixels. Blocks are in Morton order over the block grid
(y bits in even positions, x bits in odd, up to the smaller dimension, then linear). Colour word: low half =
colour A, high half = colour B; bit15 of each = opaque flag (opaque: A 5-5-4, B 5-5-5; else A/B 3-bit alpha +
4-4-3 / 4-4-4); bit0 of the word = modulation mode (1 = punch-through: weights 0,4,4,8 and index 2 clears
alpha). Per pixel: A and B are bilinearly upscaled from the 3x3 neighbouring blocks (block-centre samples,
weights (2,2,0),(1,3,0),(0,4,0),(0,3,1) per axis, **wrapping** at the image edge), then
`(A*(8-w) + B*w)/8` with w from 2 bits (0,3,5,8). `decode_pvrtc4` is vectorised (einsum over blocks).
Punch-through and translucent colours are common (10.1 M of 72.8 M mip-0 blocks use the mode bit;
2.9 M / 2.7 M have non-opaque A / B), so they are not optional.

## Verification

* **Oracle**: `texture2ddecoder.decode_pvrtc` (throwaway venv under the scratchpad, not a dependency)
  vs `decode_pvrtc4` on 1,500 random PVRTC textures (1,576 levels incl. all mips of 12): **bit-exact**
  (max abs diff 0). Sizes 2..1024.
* **Survey**: all 14,064 files parse and decode every mip (`fail 0`), ~212 s total (mostly zip reads);
  mip 0 of 512x512 PVRTC ~0.03 s, 256x256 ~0.01 s, 1024 raw ~0 s.
* **Visual**: contact sheet of 30 textures (brick d/n/s, grass normal, asphalt, "GO-KARTS" and "Aqua...berry"
  signs, Ratsnake poster, Bull logo, lockers, window, door, Lithuania/Colorado flags, HUD RGBA4444 icons, ornate
  frame DXT5, world map DXT1, radar tile, L8 comic atlas and fonts, title logo, Jimmy portrait): all
  recognisable, text reads left-to-right, no flip, channels correct.

Open: whether `_n` is tangent-space with Z recomputed (the decoded normals look standard +Z-blue); the
8x8 placeholder textures' intended resolution; fmt 8 as alpha vs luminance (decoded to L,L,L,255).
