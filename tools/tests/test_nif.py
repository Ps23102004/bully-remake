# OWNER: builder-A (importers)
import pytest

from fixtures import NifWriter, make_quad_nif
from tools.importers.binreader import FormatError
from tools.importers.nif import TextModeCorruptionError, parse_nif, strips_to_triangles


@pytest.mark.parametrize("big", [False, True])
def test_header_and_blocks_both_endians(big):
    n = parse_nif(make_quad_nif(big_endian=big))
    assert n.big_endian is big
    assert n.roots == [0]
    assert n.trailing_nonzero == 0
    assert n.failed_blocks() == []
    tc = n.type_counts()
    assert tc["NiNode"] == 3 and tc["NiTriShape"] == 1 and tc["NiUnknownThing"] == 1
    root = n.blocks[0].data
    assert n.string(root["name"]) == "Scene Root" and root["children"] == [1]
    model = n.blocks[1].data
    assert model["translation"] == (5.0, 6.0, 7.0)
    shape = n.blocks[3].data
    gd = n.data(shape["data"], "NiTriShapeData")
    assert gd["num_vertices"] == 4
    assert gd["triangles"] == (0, 1, 2, 0, 2, 3)
    assert gd["uv_sets"][0][:4] == (0.0, 0.0, 1.0, 0.0)
    assert gd["colors"][:4] == (1.0, 0.5, 0.25, 1.0)
    unknown = [b for b in n.blocks if b.type_name == "NiUnknownThing"][0]
    assert unknown.data is None and unknown.error is None and unknown.size == 3


def test_big_endian_block_count_stays_little_endian():
    data = make_quad_nif(big_endian=True)
    assert data[0x2B] == 0  # endian byte: big
    assert int.from_bytes(data[0x30:0x34], "little") == len(parse_nif(data).blocks)


def test_strips_converted_with_alternating_winding():
    assert strips_to_triangles([[0, 1, 2, 3, 4]]) == [0, 1, 2, 2, 1, 3, 2, 3, 4]
    assert strips_to_triangles([[0, 1, 1, 2]]) == []  # degenerates dropped
    n = parse_nif(make_quad_nif(strips=True))
    gd = n.data(n.blocks[3].data["data"], "NiTriStripsData")
    assert gd["triangles"] == (0, 1, 3, 3, 1, 2)


def test_block_size_mismatch_is_recorded_not_used():
    w = NifWriter()
    w.add("NiAlphaProperty", w.alpha(0x201, 10) + b"\x00")  # one byte too many
    n = parse_nif(w.build())
    assert n.blocks[0].data is None and "decoded" in n.blocks[0].error


def test_crlf_header_is_text_mode_corruption():
    with pytest.raises(TextModeCorruptionError):
        parse_nif(make_quad_nif().replace(b"9\n", b"9\r\n", 1))


def test_rejects_other_versions():
    with pytest.raises(FormatError):
        parse_nif(b"Gamebryo File Format, Version 20.0.0.5\n" + b"\0" * 64)
    with pytest.raises(FormatError):
        parse_nif(b"NetImmerse" + b"\0" * 64)


def test_types_filter_skips_decoding():
    n = parse_nif(make_quad_nif(), types={"NiNode"})
    assert n.blocks[0].data is not None
    assert n.blocks[4].data is None and n.blocks[4].error is None
