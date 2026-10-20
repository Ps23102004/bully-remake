# OWNER: lane M (materials + text)
"""Wx cipher on synthetic text (no game text in the tests)."""
import pytest

from tools.importers import wx
from tools.importers.binreader import FormatError


@pytest.mark.parametrize("n", [0, 1, 2, 3, 4, 5, 7, 8, 9, 31, 300])
def test_round_trip_every_length(n):
    plain = bytes((i * 37 + 11) & 0xFF for i in range(n))
    enc = wx.encode(plain)
    assert enc[:2] == b"Wx" and set(enc[2:]) <= set(wx.ALPHABET)
    assert wx.decode(enc) == plain
    assert wx.decode(enc + b"\r\n\0") == plain          # trailing line end / NUL is ignored


def test_round_trip_text_and_other_seed():
    text = "{\n\teffect(effect)=synthetic,\n}\n é"
    assert wx.decode_text(wx.encode(text.encode())) == text
    assert wx.decode(wx.encode(b"abc", seed=12345), seed=12345) == b"abc"
    assert wx.decode(wx.encode(b"abc", seed=12345)) != b"abc"


def test_keystream_matches_the_documented_recurrence():
    # first outputs for the seed, computed by hand from r = 171*(r%177) - 2*(r//177) mod 2^32
    r = wx.SEED
    want = []
    for _ in range(3):
        r = (171 * (r % 177) - 2 * (r // 177)) & 0xFFFFFFFF
        want.append(r)
    assert list(wx._keystream(3, wx.SEED)) == want
    # one plaintext byte: c = (p - r) ^ 18 -> two base32 symbols
    assert wx.decode(wx.encode(b"Q")) == b"Q"


def test_bad_input():
    assert not wx.is_wx(b"{ plain }") and wx.is_wx(b"Wx2E")
    with pytest.raises(FormatError):
        wx.decode(b"{ plain }")
    with pytest.raises(FormatError):
        wx.decode(b"Wx!!!!")                           # '!' is not in the alphabet
    with pytest.raises(FormatError):
        wx.decode_text(wx.encode(b"\xff\xfe\xfd"))      # not UTF-8
