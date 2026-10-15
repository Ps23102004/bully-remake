# OWNER: builder-A (importers)
"""Trigger.img text parser, on synthetic fixtures only (no game data)."""
import json
import math
import os

from fixtures import make_img
from tools.importers import cli
from tools.importers import trigger as T
from tools.importers.img import ImgArchive

POINT_TAIL = """\t\tPOINTLABEL ""
\t\tPOINTPICKUPNAME ""
\t\tPOINTSFXID ""
\t\tPOINTSOUNDBANKID ""
\t\tPOINTSFXVOLUME 1.000000
\t\tSTREAMFADEINTIME 0
\t\tSTREAMFADEOUTTIME 0
\t\tPOINTSML 0
\t\tSTARTHOUR 0
\t\tENDHOUR 0
\t\tWEATHERMASK ""
\t\tUSEOCCLUSION FALSE
\t\tPOINTMISSIONSPEC FALSE
\t\tMISSIONNAME ""
\t\tPOINTPERMANENT FALSE
\t\tPOINTTAG 0
\t\tPICKUPBUTES ""
"""

SAMPLE = (
    "NPATHS 2\nNPOINTS 3\nNPERIMETERS 4\nNTRIGGERS 3\nNPOIS 0\r\n"
    # perimeter with no trigger (orphan)
    "PERIMETER\n\tBEGIN\n\t\tPLACEMENT \"\"\n\t\tISDOOR FALSE\n\t\tHEIGHT 3.0\n\t\tDEPTH 1.0\n"
    "\t\tNPERIMETERPOINTS 3\n\t\tPERIMETERPOINTX 0\n\t\tPERIMETERPOINTY 0\n"
    "\t\tPERIMETERPOINTX 1\n\t\tPERIMETERPOINTY 0\n\t\tPERIMETERPOINTX 0\n\t\tPERIMETERPOINTY 1\n\tEND\nNTRIGGERS 0\n"
    # perimeter + trigger, yaw 90 (clockwise rotation of the local frame)
    "PERIMETER\n\tBEGIN\n\t\tPLACEMENT \"\"\n\t\tISDOOR FALSE\n\t\tHEIGHT 1.0\n\t\tDEPTH 0.0\n"
    "\t\tNPERIMETERPOINTS 4\n"
    "\t\tPERIMETERPOINTX -1.0\n\t\tPERIMETERPOINTY -2.0\n\t\tPERIMETERPOINTX 1.0\n\t\tPERIMETERPOINTY -2.0\n"
    "\t\tPERIMETERPOINTX 1.0\n\t\tPERIMETERPOINTY 4.0\n\t\tPERIMETERPOINTX -1.0\n\t\tPERIMETERPOINTY 4.0\n\tEND\n"
    "NTRIGGERS 1\nTRIGGER\n\tBEGIN\n\t\tTRIGGERNAME \"T_Room\"\n\t\tAREACODE 2\n"
    "\t\tPOSITION 10.000000, -20.000000, 3.000000\n\t\tYAWPITCHROLL 0.0, 0.0, 0.0\n\t\tYAW 90.000000\n"
    "\t\tZHEIGHT 2.500000\n\t\tISMISSIONSPECIFIC TRUE\n\t\tISLOCKED FALSE\n\t\tAMBIENT 1, 2, 3\n"
    "\t\tUSETIMECYCLE FALSE\n\t\tHASPOPULATIONDATA 1\n\t\tFORCEPOPREAD FALSE\n"
    "\t\tPOPULATIONDATA \n\t\t\tBEGIN\n\t\t\t\tPOPULATION_DAY \n\t\t\t\t\tBEGIN\n"
    "\t\t\t\t\t\tTOTAL 3\n\t\t\t\t\t\tPREFECT 1\n\t\t\t\t\t\tNERD 2\n\t\t\t\t\tEND\n\t\t\tEND\n"
    "\t\tHASAMBIENTVEHICLEDATA 0\n\t\tFORCEVEHREAD FALSE\n\tEND\n"
    # placement-bound (door) perimeter + trigger
    "PERIMETER\n\tBEGIN\n\t\tPLACEMENT \"ScGate\"\n\t\tISDOOR TRUE\n\t\tHEIGHT 1.0\n\t\tDEPTH 0.0\n"
    "\t\tNPERIMETERPOINTS 0\n\tEND\nNTRIGGERS 1\nTRIGGER\n\tBEGIN\n\t\tTRIGGERNAME \"G_Gate\"\n\t\tAREACODE 0\n"
    "\t\tPOSITION 301.5, -72.5, 6.0\n\t\tYAW 0.0\n\t\tZHEIGHT 1.0\n\t\tISMISSIONSPECIFIC TRUE\n"
    "\t\tISLOCKED TRUE\n\t\tAMBIENT 0, 0, 0\n\t\tUSETIMECYCLE FALSE\n\t\tHASPOPULATIONDATA 0\n"
    "\t\tHASAMBIENTVEHICLEDATA 0\n\tEND\n"
    "PATH\n\tBEGIN\n\t\tPATHNAME \"P_Walk\"\n\t\tAREACODE 0\n\t\tPATROLTYPE -1\n\t\tNPATHPOINTS 3\n"
    "\t\tPATHPOINT 0.0, 0.0, 0.5\n\t\tPOINTYAWPITCHROLL 0.0, 0.0, 0.0\n\t\tWAITTIME 1.5\n\t\tUNID 0\n"
    "\t\tNACTIONS 2\n\t\tACTIONTYPE 0\n\t\tORIENTATION 165.0, 0.0, 0.0\n\t\tWAITTIME 2.0\n"
    "\t\tACTIONTYPE 0\n\t\tORIENTATION -38.0, 0.0, 0.0\n\t\tWAITTIME 3.0\n"
    "\t\tPATHPOINT 3.0, 4.0, 0.5\n\t\tPOINTYAWPITCHROLL 0.0, 0.0, 0.0\n\t\tWAITTIME 0.0\n\t\tUNID 1\n\t\tNACTIONS 0\n"
    "\tEND\n"
    "PATH\n\tBEGIN\n\t\tPATHNAME \"P_Short\"\n\t\tAREACODE 2\n\t\tPATROLTYPE 7\n\t\tNPATHPOINTS 2\n"
    "\t\tPATHPOINT 1.0, 1.0, 1.0\n\t\tPOINTYAWPITCHROLL 0.0, 0.0, 0.0\n\t\tWAITTIME 0.0\n\t\tUNID 0\n\t\tNACTIONS 0\n\tEND\n"
    # point with three entries, and an empty point
    "POINT\n\tBEGIN\n\t\tPOINTNAME \"Pt_Three\"\n\t\tAREACODE 14\n\t\tNPOINTPOINTS 3\n"
    "\t\tPOINTPOINT 1.0, 2.0, 3.0\n\t\tPOINTYAWPITCHROLL 90.0, 0.0, 0.0\n" + POINT_TAIL
    + "\t\tPOINTPOINT 4.0, 5.0, 6.0\n\t\tPOINTYAWPITCHROLL -45.0, 0.0, 0.0\n" + POINT_TAIL.replace('POINTLABEL ""', 'POINTLABEL "marker"')
    + "\t\tPOINTPOINT 7.0, 8.0, 9.0\n\t\tPOINTYAWPITCHROLL 0.0, 0.0, 0.0\n" + POINT_TAIL + "\tEND\n"
    "POINT\n\tBEGIN\n\t\tPOINTNAME \"Pt_Empty\"\n\t\tAREACODE 2\n\t\tNPOINTPOINTS 0\n\tEND\n"
    "POINT\n\tBEGIN\n\t\tPOINTNAME \"Pt_One\"\n\t\tAREACODE 0\n\t\tNPOINTPOINTS 1\n"
    "\t\tPOINTPOINT 302.0, -72.0, 6.0\n\t\tPOINTYAWPITCHROLL 185.0, 0.0, 0.0\n" + POINT_TAIL + "\tEND\n"
)


def parsed(text=SAMPLE, source="9_99"):
    return T.parse_file(text, source)


def by_name(f):
    return {e["name"]: e for e in f["entities"]}


def test_ids_are_unique_per_type():
    f = parsed()
    ids = [e["id"] for e in f["entities"]]
    assert len(ids) == len(set(ids)) and "9_99/point/Pt_One" in ids and "9_99/trigger-volume/T_Room" in ids


def test_counts_and_header_agree():
    f = parsed()
    assert f["counts"] == {"path": 2, "point": 3, "trigger-volume": 2, "poi": 0}
    assert f["header"]["NPATHS"] == 2 and f["header"]["NPOINTS"] == 3
    # header declares 3 triggers, file holds 2 -> warning, not an error
    assert any("NTRIGGERS declares 3" in w for w in f["warnings"])
    assert len(f["orphan_perimeters"]) == 1
    assert f["orphan_perimeters"][0]["points_local"] == [[0, 0], [1, 0], [0, 1]]


def test_point_multi_entries_and_ue_conversion():
    e = by_name(parsed())["Pt_Three"]
    assert e["type"] == "point" and e["area_code"] == 14 and len(e["points"]) == 3
    assert e["location_game"] == [1.0, 2.0, 3.0] and e["location_cm"] == [100.0, -200.0, 300.0]
    assert e["points"][1]["location_cm"] == [400.0, -500.0, 600.0]
    assert e["points"][1]["pointlabel"] == "marker"
    assert "pointlabel" not in e["points"][0]
    assert e["points"][1]["yaw_deg"] == -45.0 and e["points"][1]["yaw_deg_ue"] == 45.0


def test_empty_point_has_no_location():
    e = by_name(parsed())["Pt_Empty"]
    assert e["points"] == [] and e["location_game"] is None and e["location_cm"] is None


def test_player_start_like_point():
    e = by_name(parsed())["Pt_One"]
    assert e["location_cm"] == [30200.0, 7200.0, 600.0]
    assert e["yaw_deg"] == 185.0 and e["yaw_deg_ue"] == -185.0


def test_path_waypoints_order_waits_and_actions():
    p = by_name(parsed())["P_Walk"]
    assert p["type"] == "path" and p["waypoint_count"] == 2 and p["declared_points"] == 3
    w0, w1 = p["waypoints"]
    assert w0["wait_time"] == 1.5                       # the point's own wait, not an action's
    assert [a["wait_time"] for a in w0["actions"]] == [2.0, 3.0]
    assert [a["yaw_deg"] for a in w0["actions"]] == [165.0, -38.0]
    assert w1["location_cm"] == [300.0, -400.0, 50.0] and w1["unid"] == 1 and w1["actions"] == []
    assert p["length_m"] == 5.0
    assert any("P_Walk" in w and "declares 3" in w for w in parsed()["warnings"])
    assert by_name(parsed())["P_Short"]["patrol_type"] == 7


def test_trigger_volume_rotation_extents_and_population():
    t = by_name(parsed())["T_Room"]
    assert t["type"] == "trigger-volume" and t["shape"] == "polygon" and t["area_code"] == 2
    assert t["location_game"] == [10.0, -20.0, 3.0] and t["location_cm"] == [1000.0, 2000.0, 300.0]
    # local box x -1..1, y -2..4 rotated clockwise by 90 deg: local +y -> world +x
    xs = sorted(p[0] for p in t["polygon_game"])
    ys = sorted(p[1] for p in t["polygon_game"])
    assert (xs[0], xs[-1]) == (8.0, 14.0)
    assert (ys[0], ys[-1]) == (-21.0, -19.0)
    assert t["aabb_game"] == {"min": [8.0, -21.0, 3.0], "max": [14.0, -19.0, 5.5]}
    # UE: y mirrored, cm; half extents of the AABB
    assert t["aabb_cm"] == {"min": [800.0, 1900.0, 300.0], "max": [1400.0, 2100.0, 550.0]}
    assert t["extents_cm"] == [300.0, 100.0, 125.0]
    assert t["center_cm"] == [1100.0, 2000.0, 425.0]
    assert t["zheight"] == 2.5 and t["z_range_game"] == [3.0, 5.5]
    assert t["mission_specific"] is True and t["ambient"] == [1.0, 2.0, 3.0]
    assert t["population"] == {"POPULATION_DAY": {"TOTAL": 3, "PREFECT": 1, "NERD": 2}}


def test_placement_bound_trigger_has_no_polygon():
    t = by_name(parsed())["G_Gate"]
    assert t["shape"] == "placement" and t["placement"] == "ScGate" and t["locked"] is True
    assert "polygon_game" not in t and t["location_cm"] == [30150.0, 7250.0, 600.0]


def test_polygon_world_zero_yaw_is_translation():
    w = T._polygon_world((5.0, 6.0, 0.0), 0.0, [(1.0, 2.0)])
    assert w == [(6.0, 8.0)]
    w = T._polygon_world((0.0, 0.0, 0.0), 90.0, [(0.0, 1.0)])
    assert math.isclose(w[0][0], 1.0, abs_tol=1e-9) and math.isclose(w[0][1], 0.0, abs_tol=1e-9)


def test_truncated_file_tolerated():
    f = T.parse_file("NPATHS 0\nNPOINTS 0\nNPERIMETERS 0\nNTRIGGERS 1\nNPOIS 0\n", "tschool_trees")
    assert f["entities"] == [] and any("NTRIGGERS declares 1" in w for w in f["warnings"])
    assert T.parse_file("", "empty")["entities"] == []


POI = (
    "NPATHS 0\nNPOINTS 0\nNPERIMETERS 0\nNTRIGGERS 0\nNPOIS 1\n"
    "POI\n\tBEGIN\n\t\tNAME \"eventsTest\"\n\t\tAREACODE 0\n\t\tNPOIPOINTS 1\n"
    "\t\tGENDER Both\n\t\tNAME \"F_ClassSmokers\"\n\t\tTYPE Wall\n\t\tPEDTYPE DEFAULT\n\t\tMAX 2\n"
    "\t\tPOIPOINT 1.0, -2.0, 3.0\n\t\tYAWPITCHROLL 270.0, 0.0, 0.0\n\t\tAFTERNOON 1\n\t\tMORNING 0\n"
    "\t\tUSERADIUS 1.0\n\t\tRADIUS 3.0\n\t\tIGNOREPOPULATION 0\n\t\tOVERRIDELIMIT 0\n\tEND\n"
)


def test_poi_block():
    f = T.parse_file(POI, "ev")
    e = f["entities"][0]
    assert e["type"] == "poi" and e["name"] == "eventsTest" and e["declared_points"] == 1
    p = e["points"][0]
    assert p["gender"] == "Both" and p["type"] == "Wall" and p["name"] == "F_ClassSmokers"
    assert p["location_cm"] == [100.0, 200.0, 300.0] and p["periods"] == {"afternoon": 1, "morning": 0}
    assert not f["warnings"]


def _install(tmp_path, dats):
    inst = tmp_path / "game"
    (inst / "Stream").mkdir(parents=True)
    make_img(str(inst / "DAT"), "Trigger", {k: v.encode("latin1") for k, v in dats.items()})
    return str(inst)


def test_archive_roundtrip_and_day_one(tmp_path):
    dats = {"1_01.dat": SAMPLE, "1_02.dat": SAMPLE.replace("Pt_", "Q_"), "1_02A.dat": SAMPLE,
            "tschool_doors.dat": SAMPLE.replace("Pt_One", "tschool_SchoolFrontDoorL"),
            "Other.dat": SAMPLE}
    inst = _install(tmp_path, dats)
    with ImgArchive(os.path.join(inst, "DAT", "Trigger.img")) as a:
        files = T.read_archive(a)
    assert set(files) == {"1_01", "1_02", "1_02A", "tschool_doors", "Other"}
    assert files["1_01"]["counts"]["point"] == 3
    d = T.day_one(files)
    assert set(d["missions"]) == {"1_01", "1_02"}
    names = [e["name"] for e in d["missions"]["1_02"]["entities"]]
    assert "Q_Three" in names and "Pt_Three" not in names
    assert [e["name"] for e in d["extras"]["tschool_doors"]] == ["tschool_SchoolFrontDoorL"]
    assert d["extras"]["1_02A"] == [] or all(e["name"] in ("InBedroom", "EndPosition", "AreaTransitionBoys")
                                             for e in d["extras"]["1_02A"])
    s = T.summary(files)
    assert s["files"] == 5 and s["trigger-volume"] == 10


def test_cli_trigger_writes_day_one_and_files(tmp_path, capsys):
    inst = _install(tmp_path, {"1_01.dat": SAMPLE, "1_02.dat": SAMPLE})
    out = tmp_path / "out"
    rc = cli.main(["--install", inst, "--out", str(out), "trigger", "--files", "1_01", "--all"])
    assert rc == 0
    day = json.loads((out / "triggers" / "day_one.json").read_text())
    assert day["slice"] == "day_one" and day["missions"]["1_01"]["counts"]["path"] == 2
    assert (out / "triggers" / "1_01.json").exists()
    allj = json.loads((out / "triggers" / "all.json").read_text())
    assert allj["summary"]["files"] == 2
    assert cli.main(["--install", inst, "--out", str(out), "trigger", "--files", "nope"]) == 1
    assert cli.main(["--install", inst, "--out", str(out), "trigger", "--list"]) == 0
    assert "1_01" in capsys.readouterr().out
