# OWNER: lane M (materials + text)
"""Bully: Anniversary Edition text tables: data_1.zip `bully/*.xml` (Wx-decoded, wx.py) -> JSON per language.

Two kinds of file (338 in the shipped archive):
  * string tables (270): `<Entry name="KEY" default="..." french="..." german="..." italian="..." spanish="..."
    british="..." russian="..." japanese="..."/>`. Mission tables (`1_01.xml`) key by text id and speech id (`S20350`),
    `<cut>_sub.xml` are cutscene subtitles (`<cut>.SUB0001`..; their frame timing is in Cuts.img `<cut>.SUB`),
    `general.xml` is the global table. 15,357 entries.
  * UI scenes / templates / font metrics (68): War Drum UI markup (`UIContainer`, `#include`). Written decoded, as is.

Output (git-ignored, under <out>/text/):
  <lang>/<stem>.json  {"format","source","kind","language","count","entries":{key: text}}
  ui/<stem>.xml       the decoded UI markup
  index.json          counts per language and per file
`default` is the base (American English) text -> "en"; `british` -> "en-GB". In-text control codes (`~i~`, ...) are kept.
Entries a language lacks are omitted (not back-filled from English). Text is the game's: it never enters git.
"""

from __future__ import annotations

import json
import os
import re
from xml.sax.saxutils import unescape

from . import wx
from .binreader import FormatError

# directory/ISO name -> attribute in the .xml
LANGS = {"en": "default", "en-GB": "british", "fr": "french", "de": "german", "it": "italian", "es": "spanish",
         "ru": "russian", "ja": "japanese"}
_RE_ENTRY = re.compile(r"<Entry\s+(.*?)/>", re.S)
_RE_ATTR = re.compile(r'(\w+)="([^"]*)"')
_UNESCAPE = {"&quot;": '"', "&apos;": "'"}


def is_table(text: str) -> bool:
    return text.lstrip().startswith("<Entry")


def parse_entries(text: str) -> list:
    """[(name, {attribute: text})] in file order. Raises FormatError if an <Entry> is not understood."""
    out = []
    for m in _RE_ENTRY.finditer(text):
        body = m.group(1)
        attrs = {k: unescape(v, _UNESCAPE) for k, v in _RE_ATTR.findall(body)}
        if "name" not in attrs or _RE_ATTR.sub("", body).strip():
            raise FormatError(f"unparsed <Entry> near offset {m.start()}")
        out.append((attrs.pop("name"), attrs))
    if text.count("<Entry") != len(out):
        raise FormatError(f"{text.count('<Entry')} <Entry> tags but {len(out)} parsed")
    return out


def kind_of(stem: str) -> str:
    return "subtitles" if stem.endswith("_sub") else "strings"


def convert(arc, out_dir: str, langs=None) -> dict:
    """Decode every `bully/*.xml` of the AeArchive `arc`. Returns the index document (also written as index.json)."""
    langs = list(LANGS) if not langs or langs == ["all"] else langs
    for lg in langs:
        if lg not in LANGS:
            raise FormatError(f"unknown language {lg!r}; one of {', '.join(LANGS)}")
    root = os.path.join(out_dir, "text")
    files, ui, errors = {}, [], []
    per_lang = {lg: {"strings": 0, "subtitles": 0, "files": 0} for lg in langs}
    for stem in arc.list("xml"):
        try:
            text = wx.decode_text(arc.read(stem, "xml"))
            if not is_table(text):
                os.makedirs(os.path.join(root, "ui"), exist_ok=True)
                with open(os.path.join(root, "ui", stem + ".xml"), "w", encoding="utf-8") as f:
                    f.write(text)
                ui.append(stem)
                continue
            entries = parse_entries(text)
        except FormatError as ex:
            errors.append({"file": stem, "error": str(ex)})
            continue
        kind = kind_of(stem)
        files[stem] = {"kind": kind, "entries": len(entries), "languages": {}}
        for lg in langs:
            attr = LANGS[lg]
            table = {k: a[attr] for k, a in entries if attr in a}
            os.makedirs(os.path.join(root, lg), exist_ok=True)
            with open(os.path.join(root, lg, stem + ".json"), "w", encoding="utf-8") as f:
                json.dump({"format": "bully-remake.text/1", "source": f"data_1.zip/bully/{stem}.xml", "kind": kind,
                           "language": lg, "count": len(table), "entries": table}, f, ensure_ascii=False, indent=0)
            files[stem]["languages"][lg] = len(table)
            per_lang[lg][kind] += len(table)
            per_lang[lg]["files"] += 1
    doc = {"format": "bully-remake.text-index/1", "languages": per_lang, "tables": len(files),
           "entries_total": sum(f["entries"] for f in files.values()), "ui_files": ui, "errors": errors, "files": files}
    os.makedirs(root, exist_ok=True)
    with open(os.path.join(root, "index.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
    return doc
