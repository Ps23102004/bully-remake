# OWNER: builder-A (importers)
"""Bully: Anniversary Edition (iOS build) install access and material -> texture lookup.

The AE keeps its models/textures in `data_1.zip` under `bully/` (flat, lowercase names:
`.msh` models, `.tex` textures, `.mtl` materials). The zip is opened read-only and its
contents are treated as untrusted data: nothing is extracted to disk or executed here.

Format reference and measured rules: docs/re-notes/ae-materials.md. Seasonal variants ("_wi", "_in_grass0N" are
snow) and the per-material corrections in MATERIAL_FIXES: docs/re-notes/ae-materials.md, ae-grass.md.

Archive path, in order: explicit argument (--ae-data), $BULLY_AE_DATA,
tools/importers/local_config.json key "ae_data", then autodetect of the stock
macOS-on-Apple-Silicon location (last resort only).
"""

from __future__ import annotations

import json
import os
import struct
import zipfile
import zlib

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


# Per-material corrections, keyed by lowercase material-name PREFIX (matched on "_" boundaries, so
# "mat_sc28_tile_01" also covers "mat_sc28_tile_01_first_floor01"). Each entry: role -> texture stem,
# "force" (replace a diffuse that did resolve) and the evidence. A stem that is not in the archive is
# ignored. Evidence "art-match" = the material's own .n/.s map correlates with that diffuse's luminance
# or relief at |r| >= 0.8 (the maps were baked from the same art); "sibling" = same prop/family,
# chosen by eye; "name" = the stem is the obvious spelling of the material name. INFERRED, not read
# from the encrypted .mtl.
MATERIAL_FIXES = {
    # missing diffuse (the 16 academy models that used to render white)
    "mat_psc_bio_brain": {"diffuse": "sc22_brain_d", "kind": "art-match", "evidence": "psc_bio_brain_s r=0.95, _n relief r=0.83"},
    "mat_pr00_drainp": {"diffuse": "pr_drainp_swap_d", "kind": "art-match", "evidence": "pr00_drainp_s r=0.91"},
    "mat_sc_bathrtile01": {"diffuse": "bdormsc06_floor_d", "kind": "art-match", "evidence": "sc_bathrtile01_s r=0.94"},
    "mat_re01_sinkvalve2": {"diffuse": "sc29_sink_d", "kind": "art-match", "evidence": "re01_sinkvalve2_s r=0.87"},
    "mat_sp_boxing_gloves": {"diffuse": "sp_boxing_g_r_d", "normal": "sp_boxing_g_r_n", "specular": "sp_boxing_g_r_s",
                             "kind": "name", "evidence": "boxing gloves, red"},
    "mat_mascot_head": {"diffuse": "mascot_head_brn_d", "specular": "mascot_head_brn_s", "kind": "name", "evidence": "mascot head, brown"},
    "mat_sc22_skelet": {"diffuse": "psc_bio_skeleton_d", "kind": "sibling", "evidence": "classroom skeleton prop"},
    "mat_re01_bookshelf": {"diffuse": "sc30_bshelf_d", "kind": "sibling", "evidence": "re01_bookshelf_n has the sc30_bshelf_n shelf layout"},
    "mat_sc28_tile_01": {"diffuse": "schallfloor01z_d", "kind": "sibling", "evidence": "3x3 hall tile, same layout as sc28_tile_01_n"},
    # winter/snow textures where the autumn one exists or can be matched
    "mat_zrc_grasse_swap": {"diffuse": "rc_grasse_swap_d", "normal": "zrc_grasse_swap_n_in_grass04", "force": True, "kind": "winter",
                            "evidence": "zrc_grasse_swap_d is snow; its normal correlates with rc_grasse_swap_d (r=0.40), not with it"},
    "mat_zbu_cobroaddirt_swap": {"diffuse": "zbu_cobroaddirt_swap_d_in_grass04", "force": True, "kind": "winter",
                                 "evidence": "zbu_cobroaddirt_swap_d is snow between the flagstones; the _in_grass04 diffuse is "
                                             "the same flagstone layout (matches zbu_cobroaddirt_swap_n_in_grass02) without snow"},
    "mat_zrc_mosspath_swap": {"diffuse": "fun_gravedirt1_d", "force": True,
                              "kind": "art-match", "evidence": "zrc_mosspath_swap_s r=0.81; the _in_grass02 diffuse is snow"},
}


def _fix_for(name: str):
    parts = name.split("_")
    for i in range(len(parts), 1, -1):
        k = "_".join(parts[:i])
        if k in MATERIAL_FIXES:
            return k, MATERIAL_FIXES[k]
    return None, None


def _apply_fix(name: str, res: dict, tex) -> dict:
    key, fix = _fix_for(name)
    if not fix or (res["diffuse"] and not fix.get("force")):
        return res
    out = dict(res)
    for role in ROLES:
        if role in fix and fix[role] in tex:
            out[role] = fix[role]
    if out["diffuse"] != res["diffuse"]:
        out["rule"] = "fix:" + fix["kind"]
    return out


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


def resolve_material_textures(material_name: str, archive) -> dict:
    """Map a material name to texture stems: {"diffuse","normal","specular","rule"}.

    `archive` is an AeArchive (or anything with texture_names()). Stems are lowercase archive
    names without ".tex"; a role with no texture is None. `rule` names the heuristic that
    found the diffuse texture ("unresolved" if none; normal/specular may still be set).
    Rules are tried per candidate base, longest first (trailing "_token"s dropped one at a time), so
    the rule string is "<match>[+z-added|z-stripped][+drop-suffix]". MATERIAL_FIXES then replaces
    or fills the roles it names (rule "fix:<evidence kind>").
    """
    tex = archive.texture_names()
    s = material_name.lower()
    if s.startswith("mat_"):
        s = s[4:]
    return _apply_fix(material_name.lower(), _resolve(s, tex), tex)
