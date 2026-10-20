# OWNER: lane M (materials + text)
"""AE text tables on a synthetic data_1.zip (Wx-encoded fixtures, invented strings)."""
import json
import os
import zipfile

import pytest

from tools.importers import ae, ae_text, wx
from tools.importers.binreader import FormatError

TABLE = '''<Entry name="K_ONE"
\tdefault="Hello &quot;you&quot; &amp; co"
\tfrench="Salut"
\tjapanese="こん"
/>

<Entry name="K_TWO"
\tdefault="~i~:~z~"
\tfrench="b"
\tbritish="Colour"
/>
'''
SUB = '<Entry name="9-9-9.SUB0001"\n\tdefault="line"\n\tfrench="ligne"\n/>\n'
UI = '#include "T.xml"\n<UIContainer template="X"/>\n'


@pytest.fixture
def arc(tmp_path):
    p = tmp_path / "data_1.zip"
    with zipfile.ZipFile(p, "w") as z:
        for n, t in (("t_one", TABLE), ("9-9-9_sub", SUB), ("hudx", UI)):
            z.writestr(f"bully/{n}.xml", wx.encode(t.encode()))
        z.writestr("bully/bad.xml", wx.encode(b'<Entry name="x" default="y" oops />'))
    with ae.AeArchive(str(p)) as a:
        yield a


def test_parse_entries_unescapes_and_keeps_codes():
    e = ae_text.parse_entries(TABLE)
    assert [n for n, _ in e] == ["K_ONE", "K_TWO"]
    assert e[0][1]["default"] == 'Hello "you" & co' and e[1][1]["default"] == "~i~:~z~"
    with pytest.raises(FormatError):
        ae_text.parse_entries('<Entry default="no name"/>')


def test_convert_writes_per_language_json(arc, tmp_path):
    out = str(tmp_path / "o")
    doc = ae_text.convert(arc, out, ["en", "fr", "en-GB"])
    assert doc["tables"] == 2 and doc["entries_total"] == 3 and doc["ui_files"] == ["hudx"]
    assert [e["file"] for e in doc["errors"]] == ["bad"]                       # reported, not fatal
    en = json.load(open(os.path.join(out, "text", "en", "t_one.json"), encoding="utf-8"))
    assert en["entries"] == {"K_ONE": 'Hello "you" & co', "K_TWO": "~i~:~z~"} and en["kind"] == "strings"
    assert json.load(open(os.path.join(out, "text", "en-GB", "t_one.json")))["entries"] == {"K_TWO": "Colour"}
    assert json.load(open(os.path.join(out, "text", "fr", "9-9-9_sub.json")))["kind"] == "subtitles"
    assert doc["languages"]["en"] == {"strings": 2, "subtitles": 1, "files": 2}
    assert open(os.path.join(out, "text", "ui", "hudx.xml")).read() == UI
    assert not os.path.exists(os.path.join(out, "text", "de"))                  # only the asked languages


def test_unknown_language_rejected(arc, tmp_path):
    with pytest.raises(FormatError):
        ae_text.convert(arc, str(tmp_path), ["klingon"])
