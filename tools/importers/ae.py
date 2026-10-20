# OWNER: builder-A (importers)
"""Bully: Anniversary Edition (iOS build) install access and material -> texture lookup.

The AE keeps its models/textures in `data_1.zip` under `bully/` (flat, lowercase names:
`.msh` models, `.tex` textures, `.mtl` materials). The zip is opened read-only and its
contents are treated as untrusted data: nothing is extracted to disk or executed here.

Format reference and measured rules: docs/re-notes/ae-materials.md. A material's textures, effect and flags are read
from its decoded `.mtl` (wx.py); the name heuristic below is only the fallback for a material without one (none in the
shipped archive). Seasonal variants ("_wi", "_in_grass0N" are snow): docs/re-notes/ae-materials.md, ae-grass.md.

Archive path, in order: explicit argument (--ae-data), $BULLY_AE_DATA,
tools/importers/local_config.json key "ae_data", then autodetect of the stock
macOS-on-Apple-Silicon location (last resort only).
"""

from __future__ import annotations

import json
import os
import re
import struct
import zipfile
import zlib

from . import wx
from .binreader import FormatError

HERE = os.path.dirname(os.path.abspath(__file__))
LOCAL_CONFIG = os.path.join(HERE, "local_config.json")
PREFIX = "bully/"
_AUTODETECT = ["/Applications/bully.app/Wrapper/bully.app/data_1.zip"]


def resolve_ae_data(arg: str | None = None) -> str:
    path = arg or os.environ.get("BULLY_AE_DATA")
    if not path and os.path.exists(LOCAL_CONFIG):
        with open(LOCAL_CONFIG) as f:
            path = json.load(f).get("ae_data")
    if not path:
        path = next((p for p in _AUTODETECT if os.path.isfile(p)), None)
    if not path:
        raise FormatError("AE data_1.zip not given: pass --ae-data, set BULLY_AE_DATA, or add "
                          '"ae_data" to tools/importers/local_config.json')
    if not os.path.isfile(path):
        raise FormatError(f"{path}: AE data_1.zip not found")
    return path


def detect_ae(install_dir: str) -> bool:
    """True for an extracted AE data_0 tree (lowercase dirs, jumptable.bin / runtime.xml)."""
    j = lambda *p: os.path.join(install_dir, *p)
    return (os.path.isdir(j("stream")) and os.path.isfile(j("stream", "world.img"))
            and (os.path.isfile(j("jumptable.bin")) or os.path.isfile(j("runtime.xml"))))


class AeArchive:
    """Case-insensitive, read-only view of data_1.zip's `bully/` entries."""

    def __init__(self, path: str | None = None):
        self.path = resolve_ae_data(path)
        self._z = zipfile.ZipFile(self.path, "r")
        self._by_name: dict[str, str] = {}   # lowercase "stem.ext" -> real entry name
        self._exts: dict[str, list] = {}     # ext -> [stem, ...] (lowercase)
        for n in self._z.namelist():
            if not n.lower().startswith(PREFIX) or n.endswith("/"):
                continue
            base = n[len(PREFIX):].lower()
            if "/" in base:                  # only the flat bully/ level is addressed
                continue
            self._by_name[base] = n
            stem, dot, ext = base.rpartition(".")
            if dot:
                self._exts.setdefault(ext, []).append(stem)
        self._tex_set = None

    def close(self) -> None:
        self._z.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    @staticmethod
    def _key(name: str, ext: str | None) -> str:
        k = name.lower()
        if k.startswith(PREFIX):
            k = k[len(PREFIX):]
        if ext:
            e = ext.lower().lstrip(".")
            if not k.endswith("." + e):
                k += "." + e
        return k

    def find(self, name: str, ext: str | None = None) -> str | None:
        """Real archive entry name for `name` (+ ext), or None."""
        return self._by_name.get(self._key(name, ext))

    def read(self, name: str, ext: str | None = None) -> bytes:
        entry = self.find(name, ext)
        if entry is None:
            raise FormatError(f"{name}{'.' + ext if ext else ''}: not in {os.path.basename(self.path)}")
        try:
            return self._z.read(entry)
        except (zipfile.BadZipFile, zlib.error, NotImplementedError, RuntimeError) as e:
            raise FormatError(f"{entry}: unreadable zip entry: {e}") from None

    def list(self, ext: str) -> list:
        """Sorted lowercase stems of every entry with this extension."""
        return sorted(self._exts.get(ext.lower().lstrip("."), []))

    def texture_names(self) -> frozenset:
        if self._tex_set is None:
            self._tex_set = frozenset(self._exts.get("tex", []))
        return self._tex_set


# ---------------------------------------------------------------------------
# .msh material list
# ---------------------------------------------------------------------------

def msh_materials(data: bytes) -> list:
    """Material names embedded as plaintext in a .msh header.

    Header: u32 version(6..12), u32 2, u32 0x539 (1337), u32 0x18, u32 1, u32 size,
    u32 material count, then per material u32 length + ASCII name.
    """
    if len(data) < 28:
        raise FormatError("msh too short")
    ver, two, magic, c18, one, _size, n = struct.unpack_from("<7I", data, 0)
    if two != 2 or magic != 0x539 or c18 != 0x18:
        raise FormatError(f"not an AE msh (header {ver},{two},{magic:#x},{c18:#x})")
    if n > 512:
        raise FormatError(f"implausible material count {n}")
    p, out = 28, []
    for _ in range(n):
        if p + 4 > len(data):
            raise FormatError("msh material table truncated")
        (ln,) = struct.unpack_from("<I", data, p)
        p += 4
        if ln > 256 or p + ln > len(data):
            raise FormatError("bad msh material name length")
        out.append(data[p:p + ln].decode("ascii", "replace"))
        p += ln
    return out


# ---------------------------------------------------------------------------
# material -> texture resolution
# ---------------------------------------------------------------------------

ROLES = {"diffuse": "d", "normal": "n", "specular": "s"}
# Variant suffixes found after the role token in the shipped texture names, in preference
# order when the plain "<base>_<role>" is missing. INFERRED meanings: _wi wind-animated
# foliage, _sp specular-in-alpha, _in_grass02 grass-blended terrain; "d2" = second diffuse.
VARIANT_SUFFIXES = ("_wi", "_sp", "_in_grass02", "_swap", "_swap_sp", "_swap_in_grass02")


def _role(tex, base: str, r: str, rest: str):
    """(stem, how) for role letter r on base, or None. how: model-suffix|exact|variant|bare."""
    if rest and f"{base}_{r}_{rest}" in tex:
        return f"{base}_{r}_{rest}", "model-suffix"
    s = f"{base}_{r}"
    if s in tex:
        return s, "exact"
    for suf in VARIANT_SUFFIXES:
        if s + suf in tex:
            return s + suf, "variant"
    if s + "2" in tex:
        return s + "2", "variant"
    if r == "d" and base in tex:      # un-roled bare texture ("abacus", "sky_bot")
        return base, "bare"
    return None


def _embedded(tex, base: str):
    """`base` itself is a diffuse texture carrying the role letter inside a token
    ("zwillow_barkd_swap", "zsmall_tree_branch_d_swap"). Returns {role: stem} or None."""
    if base not in tex:
        return None
    toks = base.split("_")
    for i, t in enumerate(toks):
        if i + 1 < len(toks) and toks[i + 1] == "swap" and (t == "d" or (i > 0 and t.endswith("d"))):
            out = {role: "_".join(toks[:i] + [t[:-1] + r] + toks[i + 1:]) for role, r in ROLES.items()}
            return {k: v for k, v in out.items() if v in tex}
    return None


WINTER_SUFFIXES = ("_wi", "_in_grass02", "_in_grass04")
# The shipped archive keeps seasonal variants side by side. Measured by eye and by correlating spec/normal
# maps with candidate diffuse maps (docs/re-notes/ae-materials.md, ae-grass.md): "<name>_wi" and
# "<name>_d_in_grass0N" are SNOW-covered winter versions, the plain "<name>_d" is the autumn one.


def _winter(stem: str) -> bool:
    return stem.endswith(WINTER_SUFFIXES)


def _resolve(s: str, tex) -> dict:
    toks = s.split("_")
    partial = {}
    for i in range(len(toks), 0, -1):
        base, rest = "_".join(toks[:i]), "_".join(toks[i:])
        forms = [(base, "")] + [(base[1:], "z-stripped") if base.startswith("z") else ("z" + base, "z-added")]
        found = []
        for b, zr in forms:
            emb = _embedded(tex, b)
            if emb and "diffuse" in emb:
                res, how = {k: emb.get(k) for k in ROLES}, "embedded-role"
            else:
                hits = {k: _role(tex, b, r, rest) for k, r in ROLES.items()}
                res = {k: h[0] if h else None for k, h in hits.items()}
                how = hits["diffuse"][1] if hits["diffuse"] else None
                if how == "variant":
                    how = "variant-suffix"
            if res["diffuse"]:
                found.append((res, how, zr))
            elif not partial and (res["normal"] or res["specular"]):
                partial = res
        if found:
            # a winter-only variant loses to the plain autumn texture of the z-toggled spelling
            swapped = False
            if (len(found) == 2 and found[0][1] == "variant-suffix" and _winter(found[0][0]["diffuse"])
                    and found[1][1] in ("exact", "model-suffix", "embedded-role")):
                found, swapped = found[::-1], True
            res, how, zr = found[0]
            res = dict(res)
            if swapped:                              # the winter spelling may carry the maps the plain one lacks
                for role in ("normal", "specular"):
                    res[role] = res[role] or found[1][0][role]
            return {**res, "rule": "+".join(x for x in (how, zr, "drop-suffix" if rest else "") if x)}
    return {"diffuse": None, "normal": partial.get("normal"), "specular": partial.get("specular"),
            "rule": "unresolved"}


def _heuristic(material_name: str, archive) -> dict:
    tex = archive.texture_names()
    s = material_name.lower()
    if s.startswith("mat_"):
        s = s[4:]
    return _resolve(s, tex)


# ---------------------------------------------------------------------------
# .mtl (Wx-decoded text): effect, texture slots, flags, vectors
#
#   {
#   \teffect(effect)=bully_normalspec,
#   \tdoublesided(bool)=true,                 optional: doublesided alphatest transparent unlit noshadows
#   \ttextures(orderedarray<texture2d>)=[3,a_d,a_n,a_s],
#   \tvectors(orderedarray<vector4>)=[2,{ x(float)=1.0000, y(float)=.. , z(float)=.. , w(float)=.. },{..}]
#   }
#
# Slot order is fixed by the effect name (measured on all 6,542 files, docs/re-notes/ae-materials.md section 6).

MTL_FLAGS = ("doublesided", "alphatest", "transparent", "unlit", "noshadows")
_EFFECT_ROLES = {"bully_waterripple": ("diffuse", "normal"), "bully_watermain": ("diffuse", "normal"),
                 "bully_radarmask": ("diffuse", "mask")}
_RE_EFFECT = re.compile(r"^\s*effect\(effect\)=([^,\s]*)", re.M)
_RE_TEX = re.compile(r"textures\(orderedarray<texture2d>\)=\[(\d+)((?:,[^,\]\s]*){0,16})\]")
_RE_VEC = re.compile(r"\{\s*x\(float\)=([-\d.eE+]+),\s*y\(float\)=([-\d.eE+]+),\s*z\(float\)=([-\d.eE+]+),"
                     r"\s*w\(float\)=([-\d.eE+]+)\s*\}")


def effect_roles(effect: str, n_textures: int) -> tuple:
    """Role of each texture slot for an effect: bully_<diffuse|spec|normal|normalspec>[uv|color] -> d [n] [s]."""
    if effect in _EFFECT_ROLES:
        roles = _EFFECT_ROLES[effect]
    elif effect.startswith("bully_") and effect not in ("bully_comic", "bully_sky"):
        e = effect[6:]
        roles = ("diffuse",) + (("normal",) if "normal" in e else ()) + (("specular",) if "spec" in e else ())
    else:
        roles = ("diffuse",)
    return roles[:n_textures] + ("extra",) * max(0, n_textures - len(roles))


def parse_mtl(text: str) -> dict:
    """{"effect", "flags": {name: bool}, "textures": [stem...], "vectors": [(x, y, z, w)...]}; ValueError if no effect."""
    e = _RE_EFFECT.search(text)
    if not e:
        raise ValueError("no effect line")
    t = _RE_TEX.search(text)
    tex = [x.lower() for x in t.group(2).split(",")[1:]] if t else []
    vec_part = text.split("vectors(", 1)[1] if "vectors(" in text else ""
    return {"effect": e.group(1).lower(),
            "flags": {f: bool(re.search(r"^\s*%s\(bool\)=true" % f, text, re.M)) for f in MTL_FLAGS},
            "textures": tex,
            "vectors": [tuple(float(x) for x in m.groups()) for m in _RE_VEC.finditer(vec_part)][:8]}


def read_mtl(archive, material_name: str):
    """Parsed .mtl of a material, or None when the archive has none (or it does not decode). Cached on the archive."""
    cache = archive.__dict__.setdefault("_mtl_cache", {}) if hasattr(archive, "__dict__") else {}
    key = material_name.lower()
    if key not in cache:
        mtl = None
        if hasattr(archive, "find") and archive.find(material_name, "mtl"):
            try:
                mtl = parse_mtl(wx.decode_text(archive.read(material_name, "mtl")))
            except (FormatError, ValueError):
                mtl = None
        cache[key] = mtl
    return cache[key]


def material_info(material_name: str, archive) -> dict:
    """Everything the importer knows about a material.

    {"diffuse","normal","specular","rule"} as resolve_material_textures, plus
      "source"   "mtl" | "mtl+heuristic" (a slot's texture is not in the archive: heuristic fills that role) | "heuristic"
      "effect"   the .mtl effect name (None for the heuristic)
      "effect_roles"  role of each texture slot, e.g. ("diffuse", "normal", "specular")
      "flags"    {doublesided, alphatest, transparent, unlit, noshadows}
      "vectors"  the .mtl vector4 list; vectors[0] is a colour/opacity multiplier (w = opacity of transparent
                 materials), vectors[1] of the spec effects is (specular power, specular strength), of the water
                 effects a UV scroll rate (INFERRED from value ranges, ae-materials.md section 6)
      "extra"    textures that fill no diffuse/normal/specular role (mask, ...)
      "missing"  stems the .mtl names that are not in the archive (the heuristic may have filled the role)
    The .mtl has no UV-tiling field: tiling is in the mesh UVs; the *uv effects scroll them at run time.
    """
    mtl = read_mtl(archive, material_name)
    if mtl is None:
        return {**_heuristic(material_name, archive), "source": "heuristic", "effect": None,
                "flags": dict.fromkeys(MTL_FLAGS, False), "effect_roles": (), "vectors": [], "extra": [], "missing": []}
    tex = archive.texture_names()
    out = {"diffuse": None, "normal": None, "specular": None, "rule": "mtl"}
    extra, missing, gone = [], [], []
    for role, stem in zip(effect_roles(mtl["effect"], len(mtl["textures"])), mtl["textures"]):
        if stem not in tex:
            missing.append(role)
            gone.append(stem)
        elif role == "extra" or role == "mask":
            extra.append(stem)
        else:
            out[role] = stem
    source = "mtl"
    if missing:
        fill = _heuristic(material_name, archive)
        for role in missing:
            if role in out and fill.get(role):
                out[role], source = fill[role], "mtl+heuristic"
        out["rule"] = "mtl+" + fill["rule"] if source != "mtl" else "mtl"
    return {**out, "source": source, "effect": mtl["effect"], "effect_roles": effect_roles(mtl["effect"], len(mtl["textures"])), "flags": mtl["flags"], "vectors": mtl["vectors"],
            "extra": extra, "missing": gone}


def resolve_material_textures(material_name: str, archive) -> dict:
    """Map a material name to texture stems: {"diffuse","normal","specular","rule"}.

    From the material's own .mtl when the archive has one (rule "mtl"), else the name heuristic: per candidate base,
    longest first (trailing "_token"s dropped one at a time), so the rule string is
    "<match>[+z-added|z-stripped][+drop-suffix]" ("unresolved" if no diffuse; normal/specular may still be set).
    Stems are lowercase archive names without ".tex"; a role with no texture is None.
    """
    i = material_info(material_name, archive)
    return {k: i[k] for k in ("diffuse", "normal", "specular", "rule")}
