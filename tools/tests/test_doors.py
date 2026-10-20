# OWNER: builder-B (town + interiors)
"""Door-link generator and town post-processing, on synthetic fixtures only (no game data)."""
import json
import math

import pytest

from tools.importers import doors as D
from tools.importers import world as W

AB = {  # areabbox rows: xmin, xmax, ymin, ymax, zmin, zmax
    "MainMap": (-200, 700, -600, 700, -20, 60),
    "School_Hallways": (-680, -580, -345, -260, -2, 15),
    "Library": (-790, -750, 180, 230, 89, 102),
    "Pool": (-685, -605, -80, -40, 53, 72),
}
EXITS = {"MainMap": {0: (210.0, -73.0, 8.6)}, "School_Hallways": {0: (-628.6, -330.0, -1.5)}}


def trig(name, area, pos, yaw, box=(-1.4, 0.0, -1.5, 3.0), placement="ESCDoorL"):
    a0, a1, b0, b1 = box
    poly = [D.local_to_world(pos, yaw, a, b) for a, b in ((a0, b0), (a1, b0), (a1, b1), (a0, b1))]
    return {"type": "trigger-volume", "name": name, "area_code": area, "location_game": list(pos), "yaw_deg": yaw,
            "zheight": 1.0, "locked": False, "polygon_game": [list(p) for p in poly], "placement": placement}


def point(name, area, pos, yaw=0.0):
    return {"type": "point", "name": name, "area_code": area,
            "points": [{"location_game": list(pos), "yaw_deg": yaw}]}


def files_for(ents, name="tdoors"):
    return {name: {"entities": ents}}


# front door of a building facing +x: outside trigger at the door, the pawn walks to -x; arrival outside is +x
OUT = trig("DT_front", 0, (208.0, -73.2, 8.6), -90.0)
IN_ARRIVAL = point("DT_front", 2, (-628.6, -324.3, 0.0), 90.0)          # inside: facing +y
IN = trig("DT_inside", 2, (-628.6, -328.2, 0.0), 180.0)
OUT_ARRIVAL = point("DT_inside", 0, (212.7, -73.1, 8.3), 0.0)


def build(ents, exitlocs=EXITS, scenes=None):
    scenes = scenes or {2: "ischool", 9: "isc_lib", 13: "isc_pool"}
    return D.build_links(files_for(ents), exitlocs, AB, scenes)


def test_yaw_convention_roundtrip_and_away_direction():
    pos = (10.0, 20.0, 1.0)
    for yaw in (-90.0, 0.0, 45.0, 180.0):
        a, b = 0.7, 1.9
        assert D.world_to_local(pos, yaw, D.local_to_world(pos, yaw, a, b)) == pytest.approx((a, b))
    # SchoolFrontDoorL: yaw -90, the outside is +x (measured on the shipped data)
    assert D.away_dir(-90.0) == pytest.approx((1.0, 0.0))
    assert D.away_dir(180.0) == pytest.approx((0.0, 1.0))


def test_pair_makes_two_mutual_links_with_data_exits():
    res = build([OUT, IN_ARRIVAL, IN, OUT_ARRIVAL])
    assert (res["edges"], res["pairs"], res["singles"]) == (2, 1, 0)
    ext = next(l for l in res["links"] if l["area_code"] == 0)
    inn = next(l for l in res["links"] if l["area_code"] == 2)
    assert ext["partner"] == inn["id"] and inn["partner"] == ext["id"]
    assert ext["confidence"] == inn["confidence"] == "data"
    # exit of a side = arrival of the partner edge, UE cm = (100x, -100y, 100z), yaw = -game yaw
    assert ext["exit"]["location_cm"] == pytest.approx([21270.0, 7310.0, 830.0], abs=0.2)
    assert inn["exit"]["location_cm"] == pytest.approx([-62860.0, 32430.0, 0.0], abs=0.2)
    assert inn["exit"]["yaw"] == pytest.approx(-90.0)
    assert ext["area"] == "MainMap" and inn["area"] == "School_Hallways" and inn["scene"] == "ischool"


def test_oriented_box_matches_the_game_polygon_in_ue_space():
    res = build([OUT, IN_ARRIVAL, IN, OUT_ARRIVAL])
    ext = next(l for l in res["links"] if l["area_code"] == 0)
    o = ext["trigger"]["oriented"]
    yaw = math.radians(o["yaw"])
    ue_corners = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            u, v = sx * o["half_extent_cm"][0], sy * o["half_extent_cm"][1]
            ue_corners.append((o["center_cm"][0] + u * math.cos(yaw) - v * math.sin(yaw),
                               o["center_cm"][1] + u * math.sin(yaw) + v * math.cos(yaw)))
    for gx, gy in OUT["polygon_game"]:
        assert min(math.hypot(gx * 100 - c[0], -gy * 100 - c[1]) for c in ue_corners) < 1.0
    # the AABB used by import_content.py contains every polygon corner
    cx, cy, _ = ext["trigger"]["center_cm"]
    hx, hy, _ = ext["trigger"]["half_extent_cm"]
    for gx, gy in OUT["polygon_game"]:
        assert abs(gx * 100 - cx) <= hx + 1 and abs(-gy * 100 - cy) <= hy + 1


def test_arrival_inside_own_trigger_is_clipped():
    # an arrival 1.0 m in front of the door, inside the default 3 m box, must not be inside the box
    ent_in = point("DT_inside", 0, (208.0 + 1.0, -73.2, 8.6), 0.0)
    res = build([OUT, IN_ARRIVAL, IN, ent_in])
    assert D.validate(res["links"], AB)["exit_inside_own_trigger"] == []


def test_wrong_area_code_is_fixed_by_position_and_stale_rows_dropped():
    # declared area 40 (not in the box table) but the position is outdoors
    assert D.area_of((50.0, -134.0, 3.0), 40, AB) == 0
    assert D.area_of((-1265.0, -611.0, -1.4), 2, AB) is None            # old school pocket: stale
    stale = [trig("MainSchool", 0, (208.0, -73.0, 8.6), 0.0), point("SV_MainSchool", 2, (-1265.0, -609.0, -1.4))]
    f = {"AreaTran": {"entities": stale}}
    edges, dropped = D.collect_edges(f, AB)
    assert edges == [] and "stale" in dropped[0]["why"]


def test_single_edge_gets_a_derived_return_door_and_shared_interiors_reuse_it():
    only_out = [OUT, IN_ARRIVAL]                                         # no interior trigger in the data
    res = build(only_out)
    ext = next(l for l in res["links"] if l["area_code"] == 0)
    inn = next(l for l in res["links"] if l["area_code"] == 2)
    assert ext["partner"] == inn["id"] and inn["confidence"] in ("derived", "data+exit")
    # derived interior door is 3.5 m behind the arrival (-y of it, the arrival faces +y) or at ExitLocs (-330)
    assert inn["game_m"]["door"][1] < -324.3
    # a second outside door into the same interior reuses the interior door instead of adding a duplicate
    out2 = trig("DT_front2", 0, (260.0, -73.2, 8.6), -90.0)
    in2 = point("DT_front2", 2, (-628.5, -324.5, 0.0), 90.0)
    res2 = build([OUT, IN_ARRIVAL, out2, in2])
    interior = [l for l in res2["links"] if l["area_code"] == 2]
    assert len(interior) == 1
    assert sum(1 for l in res2["links"] if l["partner"] == interior[0]["id"]) == 2


def test_area_without_scene_is_skipped():
    res = D.build_links(files_for([OUT, IN_ARRIVAL, IN, OUT_ARRIVAL]), EXITS, AB, {})
    assert res["links"] == [] and any("no scene" in x["why"] for x in res["dropped"])


def test_parsers():
    ex = D.parse_exitlocs("# c\nMainMap,\t0,\t523.46,\t-70.87,\t4.44\t#->Downtown\nSouvenir, 0, -794.7, 45.7, 6.7,  #x\nbad line\n")
    assert ex["MainMap"][0] == (523.46, -70.87, 4.44) and ex["Souvenir"][0] == (-794.7, 45.7, 6.7)
    ab = D.parse_areabbox("MainMap,\t-227.7\t761.4\t-678.1\t780.0\t-26.2\t68.1\tOutdoor\n")
    assert ab["MainMap"] == (-227.7, 761.4, -678.1, 780.0, -26.2, -26.2 + 68.1)


# --- world.py additions ----------------------------------------------------------------------------------

def _pl(scene, model, pos=(0, 0, 0), vis=True):
    return {"scene": scene, "model": model, "visible": vis, "gltf": "models/x.gltf",
            "game": {"position": list(pos), "rotation_xyzw_stored": [0, 0, 0, 1], "scale": [1, 1, 1]}}


def test_duplicate_placements_hidden_but_first_kept():
    pls = [_pl("isc_clas", "kiddesk", (1, 2, 3)), _pl("isc_clas", "kiddesk", (1, 2, 3)),
           _pl("isc_clas", "kiddesk", (1, 2, 3.5)), _pl("other", "kiddesk", (1, 2, 3))]
    assert W.hide_duplicate_placements(pls) == 1
    assert [p["visible"] for p in pls] == [True, False, True, True]
    assert pls[1]["hidden_reason"] == "duplicate_placement" and pls[1]["gltf"] is None


def test_winter_only_and_zone_roster():
    hid = W.ae_hidden_models(["dl_bu_winteronly02", "dl_rcxmaspx", "fgrd_sc1d_cice", "kiddesk", "pxbox"])
    assert hid == {"dl_bu_winteronly02": "winter_only", "dl_rcxmaspx": "winter_only", "fgrd_sc1d_cice": "winter_only",
                   "pxbox": "physics_proxy"}
    assert len(W.TOWN_ZONES) == 26 and len(set(W.TOWN_ZONES)) == 26
    assert set(W.TOWN_ZONES) <= set(W.PRESETS["town"]) <= set(W.PRESETS["all"])
    assert not any(s.lower() in ("ftest", "ttest") for s in W.PRESETS["all"])


def test_water_materials_patched(tmp_path):
    (tmp_path / "models").mkdir()
    doc = {"materials": [
        {"name": "mat_wtr_base", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}},
         "extras": {"blend_mode": "opaque", "textures": {"base": {"uri": "../x.png"}}}},
        {"name": "mat_wtr_ripple_tgk_waterripple", "pbrMetallicRoughness": {}, "extras": {"blend_mode": "opaque"}},
        {"name": "mat_wtr_waterbase", "pbrMetallicRoughness": {}, "extras": {"blend_mode": "opaque"}}]}
    (tmp_path / "models" / "w.gltf").write_text(json.dumps(doc))
    assert W.patch_water_materials(str(tmp_path), {"w": {"gltf": "models/w.gltf"}, "none": {"gltf": None}}) == 1
    m = json.loads((tmp_path / "models" / "w.gltf").read_text())["materials"]
    assert m[0]["extras"]["blend_mode"] == "translucent" and "baseColorTexture" not in m[0]["pbrMetallicRoughness"]
    assert m[0]["extras"]["gamebryo"]["material"]["alpha"] == W.WATER_ALPHA
    assert m[1]["extras"]["blend_mode"] == "additive" and m[2]["extras"]["blend_mode"] == "opaque"


def test_fallback_explicit_inner_door_and_direction():
    """EXIT_FALLBACK inner side as (x, y, z, (dx, dy)): trigger at the door, arrival 3.5 m along (dx, dy), not toward the area box middle."""
    from tools.importers import doors
    box = lambda x0, x1, y0, y1: (x0, x1, y0, y1, 0.0, 50.0)
    areabox = {"School_Hallways": box(-680, -579, -341, -262), "Principal": box(-720, -690, 190, 240)}
    exitlocs = {"School_Hallways": {3: (-634.0, -293.0, 5.5)}}
    ids = iter(range(100))
    out = doors.fallback_links(exitlocs, areabox, set(), {5: "isc_offi"}, lambda s: f"{s}_{next(ids)}")
    inner = [l for l in out if l["area_code"] == 5][0]
    assert inner["game_m"]["door"] == [-701.25, 217.4]
    ex = inner["exit"]["location_cm"]
    assert abs(ex[0] - -70125.0) < 1 and abs(ex[1] - -(217.4 - 3.5) * 100) < 1   # 3.5 m along game -y (UE y = -100 * game y)
    assert inner["exit"]["yaw"] == 90.0
