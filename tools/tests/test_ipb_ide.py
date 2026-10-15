# OWNER: builder-A (importers)
import math

import pytest

from fixtures import make_ipb
from tools.importers.binreader import FormatError
from tools.importers.ide import ModelDefs, parse_ide_text
from tools.importers.ipb import parse_ipb, placement_quat_game, placement_rotation, quat_to_matrix

Q90Z = (0.0, 0.0, math.sin(math.pi / 4), math.cos(math.pi / 4))  # +90 deg about Z (textbook)


def test_parse_ipb_sections_and_inst():
    data = make_ipb([(5136, "FGRD_SC1b01", (1.0, 2.0, 3.0), (1.0, 1.0, 2.0), Q90Z),
                     (474, "GeekCard", (-4.0, 5.0, 6.0), (1.0, 1.0, 1.0), (0, 0, 0, 1))])
    f = parse_ipb(data)
    assert f.section_counts == {"inst": 2, "spec": 1, "rail": 1, "prop": 2}
    assert f.header_unknown == bytes(range(15))
    p = f.placements[0]
    assert (p.model_id, p.name, p.position, p.scale) == (5136, "FGRD_SC1b01", (1.0, 2.0, 3.0), (1.0, 1.0, 2.0))
    assert p.unk68 == 2.0 and p.unk112 == 0 and p.unk116 == 1234
    assert f.placements[1].name == "GeekCard"


def test_ipb_rejects_garbage_after_sections_and_unknown_tags():
    with pytest.raises(FormatError):
        parse_ipb(make_ipb([(1, "a", (0, 0, 0), (1, 1, 1), (0, 0, 0, 1))], tail=b"\x01"))
    bad = bytearray(make_ipb([(1, "a", (0, 0, 0), (1, 1, 1), (0, 0, 0, 1))]))
    i = bad.index(b"spec")
    bad[i:i + 4] = b"zzzz"
    with pytest.raises(FormatError):
        parse_ipb(bytes(bad))
    with pytest.raises(FormatError):
        parse_ipb(b"IPL!" + b"\0" * 40)


def test_placement_rotation_is_transpose_of_textbook():
    m = quat_to_matrix(Q90Z)
    # textbook +90 about Z maps X to Y
    assert [round(c, 6) for c in (m[0][0], m[1][0], m[2][0])] == [0, 1, 0]
    r = placement_rotation(Q90Z)
    assert all(abs(r[i][j] - m[j][i]) < 1e-12 for i in range(3) for j in range(3))
    # and the conjugated quaternion's textbook matrix equals that transpose
    rq = quat_to_matrix(placement_quat_game(Q90Z))
    assert all(abs(r[i][j] - rq[i][j]) < 1e-12 for i in range(3) for j in range(3))


def test_ide_text_comma_and_tab_rows():
    text = ("objs\n5143, SC1b_bldg_main, G_scBuilding, 1, 120, 8192, 0, 1, 0, 255, 0, 0, 0\nend\n"
            "item\t\t\n500\tRubBand\tRubBand\nend\npath\n1, 2, 3\nend\n")
    es = parse_ide_text(text, "x.ide")
    assert [(e.id, e.model, e.txd, e.section) for e in es] == [
        (5143, "SC1b_bldg_main", "G_scBuilding", "objs"), (500, "RubBand", "RubBand", "item")]
    assert es[0].draw_distance == 120.0 and es[0].flags == 8192
    d = ModelDefs()
    d.add(es)
    assert d.get("sc1b_BLDG_main").txd == "G_scBuilding" and d.by_id[500].model == "RubBand"
    d.add(parse_ide_text("objs\n9, SC1b_bldg_main, Other, 1, 1, 0\nend\n"))
    assert len(d.conflicts) == 1 and d.get("SC1b_bldg_main").txd == "G_scBuilding"
