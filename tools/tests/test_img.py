# OWNER: builder-A (importers)
import struct

import pytest

from fixtures import make_img
from tools.importers.binreader import FormatError
from tools.importers.img import ImgArchive, parse_dir


def test_parse_dir_records_and_24_char_names():
    raw = struct.pack("<II24s", 0, 4, b"Algie1.lur") + struct.pack("<II24s", 4, 1, b"X" * 24)
    es = parse_dir(raw)
    assert [(e.name, e.offset, e.size) for e in es] == [("Algie1.lur", 0, 8192), ("X" * 24, 8192, 2048)]
    assert es[0].ext == "lur" and es[0].stem == "Algie1"


def test_parse_dir_rejects_partial_record():
    with pytest.raises(FormatError):
        parse_dir(b"\0" * 33)


def test_archive_read_find_case_insensitive(tmp_path):
    img = make_img(str(tmp_path), "World", {"A.nif": b"hello", "B.NFT": b"x" * 3000})
    with ImgArchive(img) as a:
        assert len(a) == 2
        assert a.find("a.NIF").name == "A.nif"
        data = a.read("b.nft")
        assert len(data) == 4096 and data[:3000] == b"x" * 3000 and data[3000:] == b"\0" * 1096
        assert a.census() == {"nif": 1, "nft": 1}
        assert a.check_overlaps() == []
        assert a.names("nft") == ["B.NFT"]


def test_archive_rejects_entry_past_end(tmp_path):
    (tmp_path / "W.img").write_bytes(b"\0" * 2048)
    (tmp_path / "W.dir").write_bytes(struct.pack("<II24s", 0, 2, b"big.nif"))
    with pytest.raises(FormatError):
        ImgArchive(str(tmp_path / "W.img"))
