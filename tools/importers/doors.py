# OWNER: builder-B (town + interiors)
"""`doors` command: door/teleport links between the town and every interior, from the game's own data.

Sources (all read-only, game metres, +Z up; AE and PC ship the same files; format notes: docs/re-notes/ae-town.md):
  * DAT/Trigger.img  - door-teleport (DT) pairs: a TRIGGER volume `DT_x` in area A and a POINT of the same name in
                       area B that is the arrival spot (position + yaw) inside B. Files tschool_doors, ischool_doors,
                       BDorm_Doors, tbusines_doors, trich_doors, tindustrial_doors, Store, Gdorm, Janitors, events*,
                       iSaveZones, i*.dat, ... plus AreaTran (`X` trigger -> `SV_X` point).
  * config/dat/ExitLocs.dat - the game's own map of exits: MainMap exit k = the outside door of an area, `<Area> 0` =
                       the door inside the area (used here to cross-check and to fill missing halves).
  * config/dat/areabbox.dat - area boxes; used to drop arrival points that are outside their area (stale data) and to
                       tie each area to the IPB scene whose placements sit inside it.
Two directed edges (outside->inside, inside->outside) that meet at the same physical door become one pair of
ABullyDoorLink entries (tools/ue/door_links.json schema, `bully-remake.door-links/2`). Confidence per link:
  data      both halves read from Trigger.img
  data+exit one half read from Trigger.img, the other from ExitLocs.dat (door position) + the arrival direction
  derived   one half read from Trigger.img, the other placed 3.5 m behind/before the arrival point along its facing
  legacy    hand-measured earlier and verified by the walk scripts (kept: tools/ue/walk_*.json rely on its numbers)
"""

from __future__ import annotations

import json
import math
import os
import re

from . import trigger as T
from .coords import game_to_ue_position

FORMAT = "bully-remake.door-links/2"

# Area codes = index of the `VisibleAreaEnum` list in config/dat/BlipLocs.dat (A=0 MAIN_MAP ... BL=63), name = the
# row name used by ExitLocs.dat / areabbox.dat. Checked against the data (areabbox containment) by `validate()`.
AREAS = {0: "MainMap", 2: "School_Hallways", 4: "Chem_Lab", 5: "Principal", 6: "Bio_Lab", 8: "Janitors_Room",
         9: "Library", 13: "Pool", 14: "Boys_Dorm", 15: "classroom", 16: "Trailer", 17: "Art_Room", 18: "Auto_Shop",
         19: "Auditorium", 20: "Chem_Plant", 22: "Island_3", 23: "Staff_Room", 26: "GroceryStore", 27: "BoxingRing",
         28: "Fire_Works", 29: "Bike_Shop_Rich", 30: "Comic_Shop_Rich", 32: "Prep_House", 33: "Rich_Cloth",
         34: "Poor_Cloth", 35: "Girls_Dorm", 36: "Tenement", 37: "HouseOfMirrors", 38: "Asylum", 39: "Barber",
         40: "Observatory", 42: "TGOKart", 43: "JunkYard", 44: "iLPstore", 46: "Hair_Salon", 50: "Souvenir",
         54: "WareHouse", 55: "Freak_Show", 56: "Poor_Hair", 57: "iDropS", 59: "iJockS", 60: "iPrepS", 61: "iGrsrS",
         62: "BMXTrack"}
OUTDOOR = 0
AREA_CODE_OF = {v: k for k, v in AREAS.items()}
ARRIVAL_BACK_M = 3.5      # distance of a derived door from its arrival point
BOX_W_M, BOX_L_M = 1.5, 4.5   # default door box: passage width, length (1.5 outside the leaf, 3.0 inside), like the ESC doors
BOX_OUT_M, BOX_IN_M = 1.5, 3.0
PAIR_MAX_M = 12.0         # a DT edge pair must close within this distance at both ends


def _find(install: str, *parts) -> str | None:
    """Case-insensitive path lookup under `install` (PC `Config/DAT`, AE `config/dat`)."""
    cur = install
    for part in parts:
        try:
            names = os.listdir(cur)
        except OSError:
            return None
        hit = next((n for n in names if n.lower() == part.lower()), None)
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


def parse_exitlocs(text: str) -> dict:
    """ExitLocs.dat -> {area: {exit_id: (x, y, z)}} (comments after '#', trailing commas tolerated)."""
    out: dict[str, dict] = {}
    for line in text.splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        p = [t for t in re.split(r"[,\s]+", line) if t]
        if len(p) < 5:
            continue
        try:
            out.setdefault(p[0], {})[int(p[1])] = (float(p[2]), float(p[3]), float(p[4]))
        except ValueError:
            continue
    return out


def parse_areabbox(text: str) -> dict:
    """areabbox.dat -> {area: (xmin, xmax, ymin, ymax, zmin, zmax)} (file order is Left Right Top Bottom Base Height)."""
    out = {}
    for line in text.splitlines():
        line = line.split("#")[0].strip()
        p = [t for t in re.split(r"[,\s]+", line) if t]
        if len(p) < 7:
            continue
        try:
            l, r, t, b, base, h = (float(x) for x in p[1:7])
        except ValueError:
            continue
        out[p[0]] = (min(l, r), max(l, r), min(t, b), max(t, b), base, base + h)
    return out


def in_box(bb, pos, margin=3.0, zmargin=8.0) -> bool:
    """pos inside the area box grown by `margin` (x, y) and `zmargin` (z: the shipped Height of the multi-floor areas
    is often too small, e.g. Girls_Dorm is 4.7 m tall but its upper floor is at +3)."""
    return (bb[0] - margin <= pos[0] <= bb[1] + margin and bb[2] - margin <= pos[1] <= bb[3] + margin
            and bb[4] - zmargin <= pos[2] <= bb[5] + zmargin)


def slug(s: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")


def _d(a, b) -> float:
    return math.hypot(a[0] - b[0], a[1] - b[1]) + 0.5 * abs(a[2] - b[2])


# --- door box geometry -----------------------------------------------------------------------------------
# Trigger YAW (degrees) rotates the local frame CLOCKWISE seen from above: world = pos + Rz(-YAW) * (a, b). The box is
# local a in the passage width, b along the approach; local +b is the INSIDE of an outside door, i.e. the pawn walks
# towards +b through the door and appears on the far side. "away" below = the direction a pawn emerges, (-sinY, -cosY)
# (checked on 31 DT pairs: it points at the partner's arrival spot, cos > 0.9; docs/re-notes/ae-town.md).

def local_to_world(pos, yaw, a, b):
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    return (pos[0] + a * c + b * s, pos[1] - a * s + b * c)


def world_to_local(pos, yaw, p):
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    dx, dy = p[0] - pos[0], p[1] - pos[1]
    return (dx * c - dy * s, dx * s + dy * c)


def away_dir(yaw):
    return (-math.sin(math.radians(yaw)), -math.cos(math.radians(yaw)))


def yaw_of_dir(d) -> float:
    return math.degrees(math.atan2(d[1], d[0]))


def _wrap(a):
    return (a + 180.0) % 360.0 - 180.0


class Edge:
    """One directed door: a trigger volume in `src` (area code) that moves the pawn to `arrival` in `dst`."""

    def __init__(self, src, dst, trig, arrival, arrival_yaw, source, prio):
        self.src, self.dst = src, dst
        self.trig = trig                      # dict: pos (x,y,z), yaw, box (a0,a1,b0,b1), zheight, locked, name, file
        self.arrival, self.arrival_yaw = arrival, arrival_yaw
        self.source = source                  # "file/NAME"
        self.prio = prio
        self.half = None                      # paired Edge


def _box_from_entity(e) -> tuple:
    """(a0, a1, b0, b1) of the trigger footprint in its own local frame, or the default door box."""
    pos, yaw = e["location_game"], e["yaw_deg"]
    pts = e.get("polygon_game") or []
    if pts:
        loc = [world_to_local(pos, yaw, p) for p in pts]
        return (min(p[0] for p in loc), max(p[0] for p in loc), min(p[1] for p in loc), max(p[1] for p in loc))
    return (-BOX_W_M / 2, BOX_W_M / 2, -BOX_OUT_M, BOX_IN_M)


def area_of(pos, declared: int, areabox: dict):
    """Area code a position really belongs to. The AREACODE of a trigger/point is sometimes wrong (the observatory
    arrival point declares area 40 but sits outdoors), so the position wins: the declared area if its box contains
    the position, else the smallest other area box that does (MainMap last), else None (stale / unplaced)."""
    name = AREAS.get(declared)
    if name in areabox and in_box(areabox[name], pos, 3.0):
        return declared
    best = None
    for code, n in AREAS.items():
        bb = areabox.get(n)
        if bb is None or n == "MainMap" or not in_box(bb, pos, 1.5):
            continue
        vol = (bb[1] - bb[0]) * (bb[3] - bb[2]) * (bb[5] - bb[4])
        if best is None or vol < best[0]:
            best = (vol, code)
    if best:
        return best[1]
    if "MainMap" in areabox and in_box(areabox["MainMap"], pos, 1.0):
        return OUTDOOR
    return None


def collect_edges(files: dict, areabox: dict) -> tuple:
    """All directed area-changing doors in Trigger.img. Returns (edges, dropped) where dropped explains skipped rows."""
    edges, dropped = [], []
    for fname, f in files.items():
        trig, pts = {}, {}
        for e in f["entities"]:
            if e["type"] == "trigger-volume":
                trig.setdefault(e["name"].lower(), []).append(e)
            elif e["type"] == "point" and e.get("points") and any(p.get("location_game") for p in e["points"]):
                pts.setdefault(e["name"].lower(), []).append(e)
        for key, tl in trig.items():
            if key.startswith("disabled_"):
                continue
            if key.startswith("dt_"):
                cand, prio = pts.get(key), 0
            elif fname == "AreaTran":
                cand, prio = pts.get("sv_" + key) or (pts.get(key) if key.startswith("sv_") else None), 1
            else:
                continue
            if not cand:
                continue
            for te in tl:
                src = area_of(tuple(te["location_game"]), te["area_code"], areabox)
                for pe in cand:
                    first = next(p for p in pe["points"] if p.get("location_game"))
                    dst = area_of(tuple(first["location_game"]), pe["area_code"], areabox)
                    if src is None or dst is None:
                        dropped.append({"source": f"{fname}/{te['name']}", "why": "trigger or arrival outside every area box (stale)",
                                        "trigger": te["location_game"], "arrival": first["location_game"]})
                        continue
                    if src == dst:
                        continue
                    box = _box_from_entity(te)
                    edges.append(Edge(src, dst, {
                        "pos": tuple(te["location_game"]), "yaw": te["yaw_deg"], "box": box,
                        "zheight": te.get("zheight", 1.0), "locked": bool(te.get("locked")), "name": te["name"],
                        "file": fname, "polygon": bool(te.get("polygon_game"))},
                        tuple(first["location_game"]), first.get("yaw_deg") or 0.0, f"{fname}/{te['name']}", prio))
    # drop exact repeats of the same physical door across files; AreaTran rows that a DT row already covers
    keep: list[Edge] = []
    for e in sorted(edges, key=lambda x: x.prio):
        # an AreaTran row (prio 1, the older door system) is dropped when a DT row already has a door within 4 m of it
        # (e.g. AreaTran MainGym sits on the pool door); two DT rows are the same door only if they also arrive together
        dup = next((k for k in keep if k.src == e.src and _d(k.trig["pos"], e.trig["pos"]) < (4.0 if e.prio else 5.0)
                    and (e.prio or (k.dst == e.dst and _d(k.arrival, e.arrival) < 6.0))), None)
        if dup is None:
            keep.append(e)
        else:
            dropped.append({"source": e.source, "why": f"same door as {dup.source}"})
    return keep, dropped


def pair_edges(edges: list) -> list:
    """Greedy one-to-one pairing of opposite edges meeting at the same physical door."""
    cands = []
    for i, a in enumerate(edges):
        for j in range(i + 1, len(edges)):
            b = edges[j]
            if a.src == b.dst and a.dst == b.src:
                d1, d2 = _d(a.trig["pos"], b.arrival), _d(a.arrival, b.trig["pos"])
                if d1 <= PAIR_MAX_M and d2 <= PAIR_MAX_M:
                    cands.append((d1 + d2, i, j))
    pairs, used = [], set()
    for score, i, j in sorted(cands):
        if i in used or j in used:
            continue
        used |= {i, j}
        edges[i].half, edges[j].half = edges[j], edges[i]
        pairs.append((edges[i], edges[j], score))
    return pairs


def _derived_trigger(arrival, arrival_yaw, exitloc, area_name, areabox):
    """Trigger of the opposite side of an arrival point: at ExitLocs (if inside the area box and within 8 m of the
    arrival) else ARRIVAL_BACK_M behind it. The pawn there walks the way it was facing on arrival, reversed."""
    f = (math.cos(math.radians(arrival_yaw)), math.sin(math.radians(arrival_yaw)))   # direction the pawn emerges
    yaw = _wrap(math.degrees(math.atan2(-f[0], -f[1])))   # trigger yaw whose away_dir == f (door is behind the arrival)
    how = "derived"
    pos = (arrival[0] - f[0] * ARRIVAL_BACK_M, arrival[1] - f[1] * ARRIVAL_BACK_M, arrival[2])
    if exitloc is not None and _d(exitloc, arrival) <= 8.0 and (area_name not in areabox or in_box(areabox[area_name], exitloc, 4.0)):
        pos, how = tuple(exitloc), "exitlocs"
    return pos, yaw, how


def _trigger_json(t: dict, exit_loc_game, exit_yaw_game) -> dict:
    pos, yaw = t["pos"], t["yaw"]
    a0, a1, b0, b1 = t["box"]
    # arrival must not start inside this link's own trigger (the pawn would bounce straight back): clip along b
    ax, ay = world_to_local(pos, yaw, exit_loc_game)
    if a0 - 0.3 <= ax <= a1 + 0.3 and b0 - 0.3 <= ay <= b1 + 0.3 and abs(exit_loc_game[2] - pos[2]) < 3.0:
        if ay >= (b0 + b1) / 2:
            b1 = min(b1, ay - 0.6)
        else:
            b0 = max(b0, ay + 0.6)
        if b1 - b0 < 0.6:
            b0, b1 = min(b0, b1 - 0.6), b1
    ca, cb = (a0 + a1) / 2, (b0 + b1) / 2
    cx, cy = local_to_world(pos, yaw, ca, cb)
    hx, hy = (a1 - a0) / 2, (b1 - b0) / 2
    # world AABB of the rotated box (for consumers that cannot rotate it: tools/ue/import_content.py)
    corners = [local_to_world(pos, yaw, a, b) for a in (a0, a1) for b in (b0, b1)]
    ax0, ax1 = min(c[0] for c in corners), max(c[0] for c in corners)
    ay0, ay1 = min(c[1] for c in corners), max(c[1] for c in corners)
    z = pos[2] + 1.0   # box spans floor-0.5 m .. floor+2.5 m (the data's ZHEIGHT 1 m would miss the capsule centre)
    ue_c = game_to_ue_position((cx, cy, z))
    aabb_c = game_to_ue_position(((ax0 + ax1) / 2, (ay0 + ay1) / 2, z))
    r = lambda v: [round(x, 1) for x in v]
    return {"center_cm": r(aabb_c), "half_extent_cm": r([(ax1 - ax0) * 50, (ay1 - ay0) * 50, 150]),
            "yaw": round(_wrap(yaw), 1),
            "oriented": {"center_cm": r(ue_c), "half_extent_cm": r([hx * 100, hy * 100, 150]), "yaw": round(_wrap(yaw), 1)}}


def _link_json(lid, partner_id, label, area_code, trig_edge_pos, trig, exit_game, exit_yaw_game, conf, extra) -> dict:
    ex_cm = game_to_ue_position(exit_game)
    d = {"id": lid, "partner": partner_id, "label": label,
         "area": AREAS.get(area_code, str(area_code)), "area_code": area_code,
         "game_m": {"door": [round(trig["pos"][0], 2), round(trig["pos"][1], 2)], "z": round(trig["pos"][2], 2)},
         "trigger": _trigger_json(trig, exit_game, exit_yaw_game),
         "exit": {"location_cm": [round(v, 1) for v in ex_cm], "yaw": round(_wrap(-exit_yaw_game), 1)},
         "confidence": conf}
    d.update(extra)
    return d


def build_links(files: dict, exitlocs: dict, areabox: dict, scene_of_area: dict) -> dict:
    edges, dropped = collect_edges(files, areabox)
    pairs = pair_edges(edges)
    used = {id(e) for p in pairs for e in p[:2]}
    singles = [e for e in edges if id(e) not in used]
    links, ids = [], set()

    def uid(base):
        base, n = slug(base), 1
        cand = base
        while cand in ids:
            n += 1
            cand = f"{base}_{n}"
        ids.add(cand)
        return cand

    def area_label(c):
        return AREAS.get(c, f"area{c}")

    def side_info(e):
        return {"source": e.source, "locked_in_game": e.trig["locked"], "scene": scene_of_area.get(e.src)}

    for a, b, score in pairs:
        # a is the outside (or lower area code) half for readability: keep the edge whose src is outdoors first
        if b.src == OUTDOOR:
            a, b = b, a
        ia, ib = uid(f"{area_label(a.src)}__{a.trig['name']}"), uid(f"{area_label(b.src)}__{b.trig['name']}")
        lab = f"{area_label(a.src)} -> {area_label(b.src)} via {a.trig['name']}"
        # side A: trigger a.trig in area a.src; arrival when coming back = b.arrival (it lies in a.src)
        links.append(_link_json(ia, ib, lab, a.src, None, a.trig, b.arrival, b.arrival_yaw, "data",
                                {"pair_gap_m": round(score, 1), **side_info(a)}))
        links.append(_link_json(ib, ia, f"{area_label(b.src)} -> {area_label(a.src)} via {b.trig['name']}", b.src, None,
                                b.trig, a.arrival, a.arrival_yaw, "data", {"pair_gap_m": round(score, 1), **side_info(b)}))
    # edges without a matching opposite edge: the missing half is derived
    for e in singles:
        # the missing half: a trigger in e.dst (partner), arrival in e.src
        if e.dst == OUTDOOR:
            ex = None
        else:
            ex = (exitlocs.get(AREAS.get(e.dst), {}) or {}).get(0)
        ptrig_pos, ptrig_yaw, how = _derived_trigger(e.arrival, e.arrival_yaw, ex, AREAS.get(e.dst), areabox)
        if e.dst == OUTDOOR and how == "derived":
            near = [p for p in (exitlocs.get("MainMap", {}) or {}).values() if _d(p, ptrig_pos) < 8.0]
            if near:
                ptrig_pos, how = min(near, key=lambda p: _d(p, ptrig_pos)), "exitlocs"
        partner = {"pos": tuple(ptrig_pos), "yaw": ptrig_yaw, "box": (-BOX_W_M / 2, BOX_W_M / 2, -BOX_OUT_M, BOX_IN_M),
                   "zheight": 1.0, "locked": False, "name": "derived", "file": "-"}
        # the partner's arrival in e.src: just outside e's trigger, in the direction e's pawn would emerge
        d = away_dir(e.trig["yaw"])
        arr_back = (e.trig["pos"][0] + d[0] * ARRIVAL_BACK_M, e.trig["pos"][1] + d[1] * ARRIVAL_BACK_M, e.trig["pos"][2])
        arr_back_yaw = yaw_of_dir(d)
        ia = uid(f"{area_label(e.src)}__{e.trig['name']}")
        conf = "data+exit" if how == "exitlocs" else "derived"
        # an interior shared by several outside doors (the grocery has 3, the bike shop 2) has ONE door inside: the extra
        # outside doors point at it, and walking out always returns through the first (canonical) outside door
        shared = next((l for l in links if l["area_code"] == e.dst and l["partner"] and
                       math.hypot(l["game_m"]["door"][0] - ptrig_pos[0], l["game_m"]["door"][1] - ptrig_pos[1]) < 5.0
                       and abs(l["game_m"]["z"] - ptrig_pos[2]) < 4.0), None)
        if shared is not None:
            links.append(_link_json(ia, shared["id"], f"{area_label(e.src)} -> {area_label(e.dst)} via {e.trig['name']}", e.src, None,
                                    e.trig, arr_back, arr_back_yaw, "data", {"note": f"shares the interior door {shared['id']}: "
                                    "leaving returns to its own outside door", "shared_interior": True, "exit_derived": True, **side_info(e)}))
            continue
        ib = uid(f"{area_label(e.dst)}__{slug(e.trig['name'])}_back")
        links.append(_link_json(ia, ib, f"{area_label(e.src)} -> {area_label(e.dst)} via {e.trig['name']}", e.src, None,
                                e.trig, arr_back, arr_back_yaw, conf, {"note": "partner half derived", **side_info(e)}))
        links.append(_link_json(ib, ia, f"{area_label(e.dst)} -> {area_label(e.src)} (derived return door)", e.dst, None,
                                partner, e.arrival, e.arrival_yaw, conf,
                                {"note": f"trigger from {'ExitLocs.dat' if how == 'exitlocs' else 'arrival offset'}",
                                 "scene": scene_of_area.get(e.dst)}))
    # a door into an area with no placements in World.img (Fire_Works, iLPstore, Island_3: cut content) leads nowhere
    has_scene = lambda c: c == OUTDOOR or c in scene_of_area
    ids_bad = {l["id"] for l in links if not has_scene(l["area_code"])}
    ids_bad |= {l["id"] for l in links if l["partner"] in ids_bad}
    for l in links:
        if l["id"] in ids_bad:
            dropped.append({"source": l.get("source", l["id"]), "why": f"area {l['area']} has no scene in World.img"})
    links = [l for l in links if l["id"] not in ids_bad]
    links += fallback_links(exitlocs, areabox, {l["area_code"] for l in links}, scene_of_area, uid)
    return {"links": links, "edges": len(edges), "pairs": len(pairs), "singles": len(singles), "dropped": dropped}


# Areas that Trigger.img gives no door pair for. Both halves come from ExitLocs.dat (door positions) and the arrival is
# 3.5 m from the door towards the middle of the area box (ORIENTATION IS A GUESS: confidence "low"/"derived").
#   (area code, (outer area, ExitLocs id), (inner area, ExitLocs id | explicit game position))
EXIT_FALLBACK = [
    # inner = the double door in the secretary room's north wall (princoff shell: jambs x -702.7 / -699.8 at y 217.4, floor 31.55).
    # ExitLocs "Principal 0" (-697.0, 230.5) lies 13 m outside the shell: arriving there dropped the pawn into the void.
    (5, ("School_Hallways", 3), ("Principal", (-701.25, 217.4, 31.55, (0.0, -1.0)))),   # 4th item: arrival direction (straight into the room)
    (43, ("MainMap", 27), ("JunkYard", 0)),
    (42, ("MainMap", 28), ("TGOKart", (-368.4, 492.2, 1.0))),   # inner = GoKart_Pos1, the race grid (GoKart.dat)
]


def _toward(a, b):
    d = (b[0] - a[0], b[1] - a[1])
    n = math.hypot(*d) or 1.0
    return (d[0] / n, d[1] / n)


def fallback_links(exitlocs: dict, areabox: dict, linked: set, scene_of_area: dict, uid) -> list:
    out = []
    for code, (oa, oid), (ia, iid) in EXIT_FALLBACK:
        if code in linked or code not in scene_of_area:
            continue
        o = exitlocs.get(oa, {}).get(oid)
        i = iid if isinstance(iid, tuple) else exitlocs.get(ia, {}).get(iid)   # explicit (x, y, z[, (dx, dy) arrival direction])
        if o is None or i is None or ia not in areabox or oa not in areabox:
            continue
        bbc = lambda n: ((areabox[n][0] + areabox[n][1]) / 2, (areabox[n][2] + areabox[n][3]) / 2)
        # inside arrival: from the inner door towards the middle of the inner area; outer arrival: towards the outer middle
        fi, fo = (i[3] if len(i) > 3 else _toward(i, bbc(ia))), _toward(o, bbc(oa))
        arr_in = (i[0] + fi[0] * ARRIVAL_BACK_M, i[1] + fi[1] * ARRIVAL_BACK_M, i[2])
        arr_out = (o[0] + fo[0] * ARRIVAL_BACK_M, o[1] + fo[1] * ARRIVAL_BACK_M, o[2])
        yi, yo = yaw_of_dir(fi), yaw_of_dir(fo)
        # trigger yaw such that away_dir == facing; inner door faces the inner area (pawn emerges in it)
        ti = {"pos": tuple(i[:3]), "yaw": _wrap(math.degrees(math.atan2(-fi[0], -fi[1]))), "box": (-BOX_W_M / 2, BOX_W_M / 2, -BOX_OUT_M, BOX_IN_M),
              "zheight": 1.0, "locked": False, "name": "exitlocs", "file": "config/dat/ExitLocs.dat"}
        to = {"pos": tuple(o), "yaw": _wrap(math.degrees(math.atan2(-fo[0], -fo[1]))), "box": ti["box"],
              "zheight": 1.0, "locked": False, "name": "exitlocs", "file": "config/dat/ExitLocs.dat"}
        ido, idi = uid(f"{oa}__to_{ia}"), uid(f"{ia}__to_{oa}")
        # outer link: trigger at the outer door, exit (arrival when returning) outside the outer door
        out.append(_link_json(ido, idi, f"{oa} -> {ia} (ExitLocs.dat)", AREA_CODE_OF.get(oa, OUTDOOR), None, to, arr_out, yo,
                              "derived", {"note": "both doors from ExitLocs.dat; arrival orientation guessed", "scene": scene_of_area.get(AREA_CODE_OF.get(oa, OUTDOOR))}))
        out.append(_link_json(idi, ido, f"{ia} -> {oa} (ExitLocs.dat)", code, None, ti, arr_in, yi,
                              "derived", {"note": "both doors from ExitLocs.dat; arrival orientation guessed", "scene": scene_of_area.get(code)}))
    return out


def scene_by_area(areabox: dict, placements: list) -> dict:
    """Area name -> IPB scene whose placements lie inside the area box (majority). Placements: (scene, (x, y, z))."""
    res = {}
    for name, bb in areabox.items():
        if name in ("MainMap", "Everywhere", "Test_Area"):
            continue
        cnt: dict[str, int] = {}
        for sc, p in placements:
            if in_box(bb, p, 2.0):
                cnt[sc] = cnt.get(sc, 0) + 1
        if cnt:
            res[name] = max(cnt, key=cnt.get)
    return res


def validate(links: list, areabox: dict) -> dict:
    """Every exit must lie in its own area box; every trigger centre must be in its own area box."""
    bad_exit, bad_trig = [], []
    for l in links:
        name = l["area"]
        if name not in areabox:
            continue
        e = l["exit"]["location_cm"]
        t = l["trigger"]["oriented"]["center_cm"]
        ge = (e[0] / 100, -e[1] / 100, e[2] / 100)
        gt = (t[0] / 100, -t[1] / 100, t[2] / 100)
        if not in_box(areabox[name], ge, 4.0):
            bad_exit.append(l["id"])
        if not in_box(areabox[name], gt, 4.0):
            bad_trig.append(l["id"])
    bounce = []
    for l in links:
        o = l["trigger"]["oriented"]
        c, h, yaw = o["center_cm"], o["half_extent_cm"], o["yaw"]
        e = l["exit"]["location_cm"]
        # UE actor-local frame: rotate the offset by -yaw
        dx, dy = e[0] - c[0], e[1] - c[1]
        cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        u, v = dx * cy + dy * sy, -dx * sy + dy * cy
        if abs(u) <= h[0] and abs(v) <= h[1] and abs(e[2] - c[2]) <= h[2] + 100:
            bounce.append(l["id"])
    return {"exit_outside_area": bad_exit, "trigger_outside_area": bad_trig, "exit_inside_own_trigger": bounce}


def load_sources(install: str) -> tuple:
    """(trigger files, exitlocs, areabbox) from an install tree (AE extracted data_0 or PC install)."""
    from .img import ImgArchive
    tp = _find(install, "dat", "Trigger.img") or _find(install, "DAT", "Trigger.img")
    if tp is None:
        raise SystemExit(f"{install}: no dat/Trigger.img")
    with ImgArchive(tp) as arch:
        files = T.read_archive(arch)
    ep = _find(install, "config", "dat", "ExitLocs.dat")
    bp = _find(install, "config", "dat", "areabbox.dat")
    if not ep or not bp:
        raise SystemExit(f"{install}: config/dat/ExitLocs.dat or areabbox.dat missing")
    read = lambda p: open(p, encoding="latin-1").read()
    return files, parse_exitlocs(read(ep)), parse_areabbox(read(bp))


def ipb_positions(install: str) -> list:
    from .img import ImgArchive
    from .ipb import parse_ipb
    wp = _find(install, "stream", "world.img") or _find(install, "Stream", "World.img")
    out = []
    with ImgArchive(wp) as img:
        for e in img.entries:
            if e.ext == "ipb":
                out += [(e.stem.lower(), p.position) for p in parse_ipb(img.read_entry(e)).placements]
    return out


# hand-verified pair kept verbatim: tools/ue/walk_m2.json and walk_m6_proof.json walk to its trigger centre
LEGACY = {
    "SchoolFrontDoorL": {
        "ext_id": "main_door_ext", "int_id": "main_door_int",
        "ext_trigger": {"center_cm": [21070, 7330, 1000], "half_extent_cm": [50, 150, 250]},
        "int_trigger": {"center_cm": [-62860, 32950, 100], "half_extent_cm": [150, 50, 250]},
    }}


def build(install: str, out_path: str | None = None, positions: list | None = None) -> dict:
    files, exitlocs, areabox = load_sources(install)
    scenes = scene_by_area(areabox, positions if positions is not None else ipb_positions(install))
    name_to_code = {v: k for k, v in AREAS.items()}
    scene_of_area = {name_to_code[n]: s for n, s in scenes.items() if n in name_to_code}
    res = build_links(files, exitlocs, areabox, scene_of_area)
    links = res["links"]
    # apply the legacy pair (ids + trigger boxes), keeping the data-derived exits
    for key, lg in LEGACY.items():
        ext = next((l for l in links if l.get("source", "").endswith("tschool_doors/DT_tschool_" + key)), None)
        inn = next((l for l in links if ext and l["id"] == ext["partner"]), None)
        if ext and inn:
            ext["id"], inn["partner"] = lg["ext_id"], lg["ext_id"]
            inn["id"], ext["partner"] = lg["int_id"], lg["int_id"]
            for l, key2 in ((ext, "ext_trigger"), (inn, "int_trigger")):
                t = lg[key2]
                l["trigger"] = {**t, "yaw": 0.0, "oriented": {**t, "yaw": 0.0}}
            ext["confidence"] = inn["confidence"] = "legacy"
    # areas without any link (minigame/mission-only interiors, cut content)
    linked = {l["area_code"] for l in links}
    unlinked = sorted(AREAS[c] for c in AREAS if c not in linked and c != OUTDOOR)
    doc = {
        "_owner": "builder-B (town + interiors)",
        "_format": FORMAT,
        "_note": ("Our own data (numbers measured from the owner's install; no game content). UE cm / UE yaw degrees "
                  "(UE = (100 x_game, -100 y_game, 100 z_game); yaw 0 = +X_ue, 90 = +Y_ue). `trigger` is the doorway box on THIS "
                  "side: center_cm / half_extent_cm = world-axis-aligned box (what tools/ue/import_content.py uses), "
                  "`oriented` = the real rotated box (actor yaw = oriented.yaw; used by tools/ue/place_doors.py). `exit` is where "
                  "a pawn arriving THROUGH THE PARTNER appears on this side (z is a hint, snapped to the floor at runtime). "
                  "Generated by `python3 -m tools.importers doors`; edit the generator, not this file."),
        "stats": {k: res[k] for k in ("edges", "pairs", "singles")},
        "areas": {AREAS[c]: scene_of_area.get(c) for c in sorted(AREAS)},
        "areas_without_links": unlinked,
        "validation": validate(links, areabox),
        "links": links,
    }
    doc["stats"]["links"] = len(links)
    doc["stats"]["by_confidence"] = {c: sum(1 for l in links if l["confidence"] == c)
                                     for c in sorted({l["confidence"] for l in links})}
    if out_path:
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, "w") as f:
            json.dump(doc, f, indent=1)
        doc["dropped"] = res["dropped"]
    return doc
