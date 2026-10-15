# OWNER: builder-A (importers)
"""`.nft` texture dictionaries (survey section 3).

A .nft is a NIF holding only texture blocks. Each NiSourceTexture has
use_external = 0, a file_name string (the exporter's original .tga path,
e.g. `Z:\\Bully\\Temp\\Export\\Textures\\Scenes\\tschool\\GB_wall1_swap_d.tga`)
and a reference to its NiPixelData. Models look textures up by the .tga base
name, case-insensitively, inside the dictionary named by the IDE TXD column.
"""

from __future__ import annotations

import ntpath
import os
from dataclasses import dataclass

from .binreader import FormatError
from .nif import NifFile, parse_nif
from .textures import FORMAT_NAMES, decode_pixel_data, has_alpha, write_png

NFT_TYPES = {"NiSourceTexture", "NiSourceCubeMap", "NiPixelData", "NiPalette", "NiStringExtraData"}


def texture_key(file_name: str) -> str:
    """Normalise a texture reference to a lookup key: base name, no
    extension, lower case. Handles both Windows and POSIX separators."""
    base = ntpath.basename(file_name.replace("/", "\\"))
    return os.path.splitext(base)[0].lower()


def safe_name(name: str) -> str:
    keep = "".join(c if (c.isalnum() or c in "-_.") else "_" for c in name)
    return keep or "unnamed"


@dataclass
class DictTexture:
    key: str          # lookup key (lower-case stem)
    name: str         # original-case stem, used for the output file name
    source_path: str  # exporter path recorded in the file
    pixel_block: int
    cube: bool


class TextureDictionary:
    def __init__(self, name: str, data):
        self.name = name
        self.nif: NifFile = parse_nif(data, types=NFT_TYPES)
        bad = self.nif.failed_blocks()
        if bad:
            raise FormatError(f"{name}: {len(bad)} texture blocks failed to decode: {bad[0].error}")
        self.textures: dict[str, DictTexture] = {}
        self.duplicates: list[str] = []
        for b in self.nif.blocks:
            if b.type_name not in ("NiSourceTexture", "NiSourceCubeMap"):
                continue
            d = b.data
            path = self.nif.string(d["file_name"]) or ""
            if d["use_external"] or d["pixel_data"] < 0:
                continue  # an external reference inside a dictionary: nothing to decode here
            key = texture_key(path)
            stem = os.path.splitext(ntpath.basename(path.replace("/", "\\")))[0]
            if key in self.textures:
                self.duplicates.append(key)
                continue
            self.textures[key] = DictTexture(key, stem, path, d["pixel_data"], b.type_name == "NiSourceCubeMap")

    def pixel_data(self, tex: DictTexture) -> tuple:
        pix = self.nif.data(tex.pixel_block, "NiPixelData")
        if pix is None:
            raise FormatError(f"{self.name}:{tex.name}: pixel data block {tex.pixel_block} missing")
        pal = self.nif.data(pix["palette"], "NiPalette") if pix["palette"] >= 0 else None
        return pix, pal

    def export(self, tex: DictTexture, out_path: str) -> dict:
        """Decode mip 0 and write a PNG. Returns metadata for the manifest."""
        pix, pal = self.pixel_data(tex)
        w, h, rgba = decode_pixel_data(pix, pal)
        alpha = has_alpha(rgba)
        write_png(out_path, w, h, rgba, keep_alpha=alpha)
        return {
            "width": w,
            "height": h,
            "format": FORMAT_NAMES.get(pix["pixel_format"], str(pix["pixel_format"])),
            "mips": len(pix["mipmaps"]),
            "faces": pix["num_faces"],
            "has_alpha": alpha,
            "cube": tex.cube,
        }
