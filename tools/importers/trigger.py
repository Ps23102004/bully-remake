# OWNER: builder-A (importers)
"""DAT/Trigger.img: mission points, paths, trigger volumes, perimeters and POIs -> JSON.

Trigger.img is an IMG/DIR archive of plain-text `.dat` files (one per mission or
place). Format notes: docs/re-notes/trigger-format.md. All coordinates in the
files are game metres (x, y, z), +Z up; the UE columns use the project
convention from coords.py: UE cm = (100x, -100y, 100z).

Yaw handling: game yaw is degrees, 0 = +x, counter-clockwise seen from above
(established on the player start point, yaw 185 = facing -x toward the school).
The Y mirror turns that into UE yaw = -game yaw (UE yaw is measured from +x
toward +y, and UE +y is game -y).

Trigger volumes: a TRIGGER block gives POSITION / YAW / ZHEIGHT and takes its
footprint from the PERIMETER block that precedes it. The perimeter points are
relative to POSITION and rotated by YAW, clockwise seen from above (note the
opposite sense to point yaw). The volume spans POSITION.z up to POSITION.z + ZHEIGHT.
"""

from __future__ import annotations

import json
import math
import os
import re

from .coords import game_to_ue_position

CATEGORIES = ("PREFECT", "NERD", "JOCK", "DROPOUT", "GREASER", "PREPPY", "STUDENT",
              "COP", "TEACHER", "TOWNPERSON", "SHOPKEEP", "BULLY")
BLOCK_TYPES = {"PATH", "POINT", "PERIMETER", "TRIGGER", "POI"}
ARCHIVE = "DAT/Trigger.img"


class TriggerFormatError(ValueError):
    pass


# ---------------------------------------------------------------------------
# Low-level text tree
# ---------------------------------------------------------------------------

class Block:
    """An ordered list of (KEY, value) fields; value is a str or a nested Block."""

    def __init__(self, kind: str):
        self.kind = kind
        self.fields: list[tuple[str, object]] = []

    def get(self, key: str, default=None):
        for k, v in self.fields:
            if k == key:
                return v
        return default


def _split(line: str):
    s = line.strip()
    if not s:
        return None
    parts = s.split(None, 1)
    return parts[0], (parts[1].strip() if len(parts) > 1 else "")


def _parse_block(lines: list, i: int, kind: str):
    """lines[i] is the BEGIN line. Returns (Block, index after END)."""
    blk = Block(kind)
    i += 1
    n = len(lines)
    while i < n:
        kv = lines[i]
        i += 1
        key, val = kv
        if key == "END":
            return blk, i
        if not val and i < n and lines[i][0] == "BEGIN":
            sub, i = _parse_block(lines, i, key)
            blk.fields.append((key, sub))
        else:
            blk.fields.append((key, val))
    return blk, i  # tolerate a missing END at end of file


def parse_text(text: str):
    """Parse one .dat file into (header, items).

    header: {"NPATHS": n, ...} as written.
    items: ordered list of Block (kind PATH/POINT/PERIMETER/TRIGGER/POI). A
    TRIGGER block gets `.perimeter` = the Block of the PERIMETER before it.
    A PERIMETER block also records `.declared_triggers` from its NTRIGGERS line.
    """
    lines = [kv for kv in (_split(l) for l in text.splitlines()) if kv]
    header: dict[str, int] = {}
    items: list[Block] = []
    i, n = 0, len(lines)
    last_perimeter = None
    while i < n:
        key, val = lines[i]
        if key in BLOCK_TYPES and not val and i + 1 < n and lines[i + 1][0] == "BEGIN":
            blk, i = _parse_block(lines, i + 1, key)
            blk.perimeter = None
            blk.declared_triggers = None
            if key == "PERIMETER":
                last_perimeter = blk
            elif key == "TRIGGER":
                blk.perimeter = last_perimeter
            items.append(blk)
            continue
        if key == "NTRIGGERS" and items and items[-1].kind == "PERIMETER":
            items[-1].declared_triggers = _int(val)
        elif key.startswith("N") and not items:
            header[key] = _int(val)
        i += 1
    return header, items


# ---------------------------------------------------------------------------
# Value helpers
# ---------------------------------------------------------------------------

def _int(s, default=0):
    try:
        return int(float(s))
    except (TypeError, ValueError):
        return default


def _float(s, default=0.0):
    try:
        return float(s)
    except (TypeError, ValueError):
        return default


def _vec(s, n=3):
    try:
        v = [float(x) for x in re.split(r"[,\s]+", s.strip()) if x]
    except ValueError:
        v = []
    v = (v + [0.0] * n)[:n]
    return v


def _str(s):
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        return s[1:-1]
    return s


def _bool(s):
    return s.strip().upper() == "TRUE"


def _r(v, nd=4):
    return [round(x, nd) for x in v]


def ue_pos(v):
    return _r(game_to_ue_position(v), 2)


def ue_yaw(yaw_deg):
    return round(-yaw_deg, 4)


def _loc(game_xyz):
    g = _r(game_xyz, 6)
    return g, ue_pos(g)


# ---------------------------------------------------------------------------
# Entity decoders
# ---------------------------------------------------------------------------

def decode_point(b: Block, source: str) -> dict:
    name = _str(b.get("POINTNAME", ""))
    ent = {"id": f"{source}/point/{name}", "name": name, "type": "point", "source": source,
           "area_code": _int(b.get("AREACODE")), "declared_points": _int(b.get("NPOINTPOINTS")),
           "points": []}
    cur = None
    for k, v in b.fields:
        if k == "POINTPOINT":
            g, u = _loc(_vec(v))
            cur = {"location_game": g, "location_cm": u, "yaw_pitch_roll": [0.0, 0.0, 0.0]}
            ent["points"].append(cur)
        elif cur is None:
            continue
        elif k == "POINTYAWPITCHROLL":
            cur["yaw_pitch_roll"] = _vec(v)
        elif k in ("POINTLABEL", "POINTPICKUPNAME", "POINTSFXID", "POINTSOUNDBANKID",
                   "WEATHERMASK", "MISSIONNAME", "PICKUPBUTES"):
            if _str(v):
                cur[k.lower()] = _str(v)
        elif k in ("POINTTAG", "POINTSML", "STARTHOUR", "ENDHOUR", "STREAMFADEINTIME", "STREAMFADEOUTTIME"):
            if _int(v):
                cur[k.lower()] = _int(v)
        elif k in ("POINTPERMANENT", "POINTMISSIONSPEC", "USEOCCLUSION"):
            if _bool(v):
                cur[k.lower()] = True
        elif k == "POINTSFXVOLUME":
            if abs(_float(v, 1.0) - 1.0) > 1e-6:
                cur["pointsfxvolume"] = _float(v)
    for p in ent["points"]:
        p["yaw_deg"] = p["yaw_pitch_roll"][0]
        p["yaw_deg_ue"] = ue_yaw(p["yaw_pitch_roll"][0])
    if ent["points"]:
        first = ent["points"][0]
        ent["location_game"] = first["location_game"]
        ent["location_cm"] = first["location_cm"]
        ent["yaw_deg"] = first["yaw_deg"]
        ent["yaw_deg_ue"] = first["yaw_deg_ue"]
    else:
        ent["location_game"] = ent["location_cm"] = None
    return ent


def decode_path(b: Block, source: str) -> dict:
    name = _str(b.get("PATHNAME", ""))
    ent = {"id": f"{source}/path/{name}", "name": name, "type": "path", "source": source,
           "area_code": _int(b.get("AREACODE")), "patrol_type": _int(b.get("PATROLTYPE"), -1),
           "declared_points": _int(b.get("NPATHPOINTS")), "waypoints": []}
    wp = None
    act = None
    for k, v in b.fields:
        if k == "PATHPOINT":
            g, u = _loc(_vec(v))
            wp = {"index": len(ent["waypoints"]), "location_game": g, "location_cm": u,
                  "yaw_pitch_roll": [0.0, 0.0, 0.0], "wait_time": 0.0, "unid": 0, "actions": []}
            ent["waypoints"].append(wp)
            act = None
        elif wp is None:
            continue
        elif k == "POINTYAWPITCHROLL":
            wp["yaw_pitch_roll"] = _vec(v)
        elif k == "ACTIONTYPE":
            act = {"action_type": _int(v), "orientation": [0.0, 0.0, 0.0], "wait_time": 0.0}
            wp["actions"].append(act)
        elif k == "ORIENTATION" and act is not None:
            act["orientation"] = _vec(v)
            act["yaw_deg"] = act["orientation"][0]
            act["yaw_deg_ue"] = ue_yaw(act["orientation"][0])
        elif k == "WAITTIME":
            (act if act is not None else wp)["wait_time"] = _float(v)
        elif k == "UNID":
            wp["unid"] = _int(v)
    ent["waypoint_count"] = len(ent["waypoints"])
    ent["length_m"] = round(sum(
        math.dist(a["location_game"], b2["location_game"])
        for a, b2 in zip(ent["waypoints"], ent["waypoints"][1:])), 3)
    return ent


def _polygon_world(pos, yaw_deg, local_pts):
    """Perimeter points are relative to the trigger POSITION and rotated by YAW *clockwise*
    (seen from above): world = pos + Rz(-yaw) * local. Verified on 11 door triggers with
    yaw +-90, where local +y (the 3 m approach side) then points at the exit spot."""
    c, s = math.cos(math.radians(-yaw_deg)), math.sin(math.radians(-yaw_deg))
    return [(pos[0] + c * x - s * y, pos[1] + s * x + c * y) for x, y in local_pts]


def decode_perimeter(b: Block) -> dict:
    xs = [_float(v) for k, v in b.fields if k == "PERIMETERPOINTX"]
    ys = [_float(v) for k, v in b.fields if k == "PERIMETERPOINTY"]
    return {"placement": _str(b.get("PLACEMENT", "")), "is_door": _bool(b.get("ISDOOR", "FALSE")),
            "height": _float(b.get("HEIGHT")), "depth": _float(b.get("DEPTH")),
            "declared_points": _int(b.get("NPERIMETERPOINTS")),
            "points_local": [[round(x, 6), round(y, 6)] for x, y in zip(xs, ys)]}


def _decode_population(b: Block, periods) -> dict:
    out = {}
    for k, v in b.fields:
        if isinstance(v, Block):
            if k in periods:
                out[k] = {kk: _int(vv) for kk, vv in v.fields if not isinstance(vv, Block)}
    return out


def decode_trigger(b: Block, source: str) -> dict:
    name = _str(b.get("TRIGGERNAME", ""))
    pos = _vec(b.get("POSITION", ""))
    yaw = _float(b.get("YAW"))
    zh = _float(b.get("ZHEIGHT"))
    g, u = _loc(pos)
    ent = {"id": f"{source}/trigger-volume/{name}", "name": name, "type": "trigger-volume", "source": source,
           "area_code": _int(b.get("AREACODE")),
           "location_game": g, "location_cm": u, "yaw_deg": yaw, "yaw_deg_ue": ue_yaw(yaw),
           "zheight": zh, "zheight_cm": round(zh * 100, 2),
           "z_range_game": _r([pos[2], pos[2] + zh], 4),
           "mission_specific": _bool(b.get("ISMISSIONSPECIFIC", "FALSE")),
           "locked": _bool(b.get("ISLOCKED", "FALSE")),
           "ambient": _vec(b.get("AMBIENT", ""), 3),
           "use_time_cycle": _bool(b.get("USETIMECYCLE", "FALSE"))}
    per = b.perimeter
    if per is not None:
        pd = decode_perimeter(per)
        ent["perimeter"] = {k: pd[k] for k in ("placement", "is_door", "height", "depth")}
        ent["perimeter"]["points_local_game"] = pd["points_local"]
        if pd["placement"]:
            ent["placement"] = pd["placement"]
        ent["shape"] = "polygon" if pd["points_local"] else ("placement" if pd["placement"] else "point")
        world = _polygon_world(pos, yaw, pd["points_local"])
        if world:
            ent["polygon_game"] = [[round(x, 4), round(y, 4)] for x, y in world]
            ent["polygon_cm"] = [[round(x * 100, 2), round(-y * 100, 2)] for x, y in world]
            xs = [p[0] for p in world]
            ys = [p[1] for p in world]
            mn, mx = [min(xs), min(ys), pos[2]], [max(xs), max(ys), pos[2] + zh]
            ent["aabb_game"] = {"min": _r(mn), "max": _r(mx)}
            ent["aabb_cm"] = {"min": ue_pos([mn[0], mx[1], mn[2]]), "max": ue_pos([mx[0], mn[1], mx[2]])}
            ent["center_game"] = _r([(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, pos[2] + zh / 2])
            ent["center_cm"] = ue_pos(ent["center_game"])
            ent["extents_cm"] = _r([(mx[0] - mn[0]) * 50, (mx[1] - mn[1]) * 50, zh * 50], 2)  # half-extents of the AABB
    else:
        ent["shape"] = "point"
    if _int(b.get("HASPOPULATIONDATA")):
        pop = b.get("POPULATIONDATA")
        if isinstance(pop, Block):
            ent["population"] = _decode_population(
                pop, ("POPULATION_DAY", "POPULATION_CLASS", "POPULATION_NIGHT", "POPULATION_CURFEW"))
    if _int(b.get("HASAMBIENTVEHICLEDATA")):
        veh = b.get("AMBIENTVEHICLEDATA")
        if isinstance(veh, Block):
            ent["ambient_vehicles"] = _decode_population(
                veh, ("POPULATION_DAY", "POPULATION_CLASS", "POPULATION_NIGHT", "POPULATION_CURFEW"))
    return ent


def decode_poi(b: Block, source: str) -> dict:
    name = _str(b.get("NAME", ""))
    ent = {"id": f"{source}/poi/{name}", "name": name, "type": "poi", "source": source,
           "area_code": _int(b.get("AREACODE")), "points": []}
    cur = None
    top = True
    for k, v in b.fields:
        if k == "GENDER":
            cur = {"gender": _str(v)}
            ent["points"].append(cur)
            top = False
        elif top or cur is None:
            continue
        elif k == "POIPOINT":
            g, u = _loc(_vec(v))
            cur["location_game"], cur["location_cm"] = g, u
        elif k == "YAWPITCHROLL":
            cur["yaw_pitch_roll"] = _vec(v)
        elif k in ("NAME", "TYPE", "PEDTYPE"):
            cur[k.lower()] = _str(v)
        elif k in ("MAX",):
            cur["max"] = _int(v)
        elif k in ("RADIUS", "USERADIUS"):
            cur[k.lower()] = _float(v)
        elif k in ("IGNOREPOPULATION", "OVERRIDELIMIT"):
            cur[k.lower()] = _bool(v) if v.upper() in ("TRUE", "FALSE") else _int(v)
        elif k in ("AFTERNOON", "MORETIRED", "SLIGHTLYTIRED", "TIRED", "TOOTIRED",
                   "EARLYCLASS", "EVENING", "LATECLASS", "LUNCH", "MORNING"):
            cur.setdefault("periods", {})[k.lower()] = _int(v)
    ent["declared_points"] = _int(b.get("NPOIPOINTS"))
    return ent


def parse_file(text: str, source: str) -> dict:
    """Parse one .dat into {"source", "header", "entities", "orphan_perimeters", "warnings"}."""
    header, items = parse_text(text)
    ents, orphans, warnings = [], [], []
    for b in items:
        if b.kind == "POINT":
            ents.append(decode_point(b, source))
        elif b.kind == "PATH":
            ents.append(decode_path(b, source))
        elif b.kind == "TRIGGER":
            ents.append(decode_trigger(b, source))
        elif b.kind == "POI":
            ents.append(decode_poi(b, source))
        elif b.kind == "PERIMETER":
            if b.declared_triggers == 0:
                d = decode_perimeter(b)
                d["index"] = len(orphans)
                orphans.append(d)
    counts = {"path": 0, "point": 0, "trigger-volume": 0, "poi": 0}
    for e in ents:
        e["index"] = counts[e["type"]]       # position among this type in the file (names can repeat: 7 cases)
        counts[e["type"]] += 1
    for hk, tk in (("NPATHS", "path"), ("NPOINTS", "point"), ("NTRIGGERS", "trigger-volume"), ("NPOIS", "poi")):
        if hk in header and header[hk] != counts.get(tk, 0):
            warnings.append(f"{hk} declares {header[hk]}, file holds {counts.get(tk, 0)}")
    for e in ents:
        if e["type"] == "path" and e["declared_points"] != e["waypoint_count"]:
            warnings.append(f"path {e['name']}: declares {e['declared_points']} points, holds {e['waypoint_count']}")
        if e["type"] == "point" and e["declared_points"] != len(e["points"]):
            warnings.append(f"point {e['name']}: declares {e['declared_points']} entries, holds {len(e['points'])}")
    return {"source": source, "header": header, "counts": counts, "entities": ents,
            "orphan_perimeters": orphans, "warnings": warnings}


# ---------------------------------------------------------------------------
# Archive level
# ---------------------------------------------------------------------------

def read_archive(archive) -> dict:
    """archive: an open ImgArchive of DAT/Trigger.img -> {stem: parsed file dict}."""
    out = {}
    for name in archive.names("dat"):
        text = archive.read(name).rstrip(b"\0").decode("latin1")
        out[os.path.splitext(name)[0]] = parse_file(text, os.path.splitext(name)[0])
    return out


# The Day One slice: mission files taken whole (the file name is the mission tag), plus the
# door / area-transition entities the slice walks through (named in the recon).
DAY_ONE_MISSIONS = ("1_01", "1_02")
DAY_ONE_EXTRAS = {
    # file stem -> entity names (case-insensitive) pulled from shared door / interior files
    "tschool_doors": ["DT_tschool_SchoolFrontDoorL", "tschool_SchoolFrontDoorL", "tschool_SchoolFrontDoorR",
                      "tschool_SchoolFrontDoorR", "DT_tschool_BoysDormL", "tschool_BoysDormL",
                      "tschool_BoysDormR"],
    "ischool_doors": ["DT_ischool_FrontDoorL", "ischool_FrontDoorL", "ischool_FrontDoorR",
                      "DT_ischool_FrontDoorRight", "ischool_FrontDoorRight", "DT_ischool_PrincipalDoorL"],
    "BDorm_Doors": ["DT_DormExitDoorL", "DormExitDoorL", "DormExitDoorR", "DT_DormExitDoorL"],
    "1_02A": ["AreaTransitionBoys", "InBedroom", "EndPosition"],
}
DAY_ONE_NOTES = {
    "1_01": "arrival at the front gates -> bully gauntlet -> courtyard -> main building -> upstairs principal's office",
    "1_02": "office briefing -> campus walk -> boys' dorm court tutorial fight",
    "doors": "front door of the main building, boys' dorm door pair, interior arrival (1_02A epilogue)",
}


def day_one(files: dict) -> dict:
    out = {"slice": "day_one", "missions": {}, "extras": {}, "notes": DAY_ONE_NOTES,
           "mapping": "mission tag = source file name (1_01.dat, 1_02.dat); shared door/interior "
                      "entities are pulled by name from the files listed under extras"}
    for m in DAY_ONE_MISSIONS:
        f = files.get(m)
        if f is None:
            continue
        out["missions"][m] = {"counts": f["counts"], "entities": f["entities"],
                              "orphan_perimeter_count": len(f["orphan_perimeters"]),
                              "warnings": f["warnings"]}
    for stem, names in DAY_ONE_EXTRAS.items():
        f = files.get(stem)
        if f is None:
            continue
        want = {n.lower() for n in names}
        ents = [e for e in f["entities"] if e["name"].lower() in want]
        out["extras"][stem] = ents
    return out


def summary(files: dict) -> dict:
    tot = {"files": len(files), "path": 0, "point": 0, "trigger-volume": 0, "poi": 0, "orphan_perimeters": 0}
    for f in files.values():
        for k, v in f["counts"].items():
            tot[k] = tot.get(k, 0) + v
        tot["orphan_perimeters"] += len(f["orphan_perimeters"])
    return tot


def write_json(path: str, obj) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w") as f:
        json.dump(obj, f, indent=1)
        f.write("\n")
