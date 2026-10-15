# OWNER: builder-A (importers)
"""Model definitions: model name -> (IDE id, texture dictionary).

The game loads compiled `.idb` files from Objects/ide.img; the install also
ships the text `.ide` sources they were compiled from (bully-re docs/idb.md
checks five of them field-for-field against the .idb). We read the text
sources, which are simpler and self-describing, and cross-check every
placement's stored model id against the id found here: a mismatch is reported
per placement, so a stale text source cannot slip through silently.

Text layout: a section keyword line (`objs`, `tobj`, `panm`, ...), CSV rows,
then `end`. For every model-bearing section the first three columns are
`id, model, txd`. Rows are comma separated, except in default.ide where
some sections are tab separated.
"""

from __future__ import annotations

import os
from dataclasses import dataclass

MODEL_SECTIONS = {"objs", "tobj", "panm", "peds", "cars", "weap", "item", "accs", "clth", "cash", "scnd"}


@dataclass
class IdeEntry:
    id: int
    model: str
    txd: str
    section: str
    source: str
    flags: int | None = None
    draw_distance: float | None = None


def parse_ide_text(text: str, source: str = "") -> list:
    out = []
    section = None
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        low = line.lower()
        if section is None:
            section = low
            continue
        if low == "end":
            section = None
            continue
        if section not in MODEL_SECTIONS:
            continue
        # objs/tobj rows are comma separated; default.ide's peds/item/weap
        # rows are tab separated. Accept either.
        if "," in line:
            cols = [c.strip() for c in line.split(",")]
        else:
            cols = line.split()
        if len(cols) < 3:
            continue
        try:
            mid = int(cols[0])
        except ValueError:
            continue
        e = IdeEntry(mid, cols[1], cols[2], section, source)
        if section in ("objs", "tobj") and len(cols) >= 6:
            try:
                e.draw_distance = float(cols[4])
                e.flags = int(cols[5])
            except ValueError:
                pass
        out.append(e)
    return out


class ModelDefs:
    def __init__(self):
        self.by_name: dict[str, IdeEntry] = {}
        self.by_id: dict[int, IdeEntry] = {}
        self.conflicts: list = []
        self.files = 0

    def add(self, entries) -> None:
        for e in entries:
            k = e.model.lower()
            prev = self.by_name.get(k)
            if prev is not None and (prev.id != e.id or prev.txd.lower() != e.txd.lower()):
                self.conflicts.append((prev, e))
                continue
            self.by_name.setdefault(k, e)
            self.by_id.setdefault(e.id, e)

    def get(self, name: str) -> IdeEntry | None:
        return self.by_name.get(name.lower())

    @classmethod
    def load_install(cls, install: str) -> "ModelDefs":
        defs = cls()
        root = os.path.join(install, "Objects")
        for dirpath, _dirs, files in os.walk(root):
            for f in sorted(files):
                if f.lower().endswith(".ide"):
                    p = os.path.join(dirpath, f)
                    with open(p, "r", encoding="latin-1") as fh:
                        defs.add(parse_ide_text(fh.read(), os.path.relpath(p, install)))
                    defs.files += 1
        return defs
