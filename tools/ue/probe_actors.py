# OWNER: builder-B5 (shot)
"""Diagnostic (commandlet, no GPU): which level actors project into given pixel rects of a shot camera?

  BR_PROBE_SHOT=facade_courtyard BR_PROBE_RECTS="1600,740,1720,880;2200,860,2260,1040" BR_PROBE_OUT=/path/probe.json \
  UnrealEditor-Cmd ... -run=pythonscript -script=tools/ue/probe_actors.py -nullrhi   (via run_probe.sh)

Projects every static-mesh actor's world bounds (8 corners) through the shot camera from shots_m5.json
(or BR_PROBE_SHOTS) and lists those whose projected box overlaps a rect, with label, model tag, bounds size
and distance. Used to find the importer-artifact actors behind the round-1 floating rods / lamppost / bar.
"""
import json
import math
import os
import traceback

import unreal

HERE = os.path.dirname(os.path.abspath(__file__))
MAP_PATH = "/Game/Imported/Maps/Bullworth"


def _norm(v):
    n = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / n for c in v]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def project_factory(shot, W, H):
    loc = shot["location_cm"]
    look = shot["look_at_cm"]
    f = _norm([look[i] - loc[i] for i in range(3)])
    r = _norm(_cross([0, 0, 1], f))
    u = _cross(f, r)
    t = math.tan(math.radians(shot["fov"]) / 2.0)

    def proj(p):
        d = [p[i] - loc[i] for i in range(3)]
        z = _dot(d, f)
        if z <= 1.0:
            return None
        return (W / 2.0 + (_dot(d, r) / z) / t * (W / 2.0), H / 2.0 - (_dot(d, u) / z) / t * (W / 2.0), z)
    return proj


def main():
    out = os.environ.get("BR_PROBE_OUT")
    shots_path = os.environ.get("BR_PROBE_SHOTS") or os.path.join(HERE, "shots_m5.json")
    name = os.environ["BR_PROBE_SHOT"]
    rects = [[float(x) for x in r.split(",")] for r in os.environ["BR_PROBE_RECTS"].split(";") if r]
    data = json.load(open(shots_path))
    W, H = data["highres_size"]
    shot = next(s for s in data["shots"] if s["name"] == name)
    proj = project_factory(shot, W, H)
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    ok = False
    try:
        les.load_level(MAP_PATH)
        hits = {i: [] for i in range(len(rects))}
        for a in eas.get_all_level_actors():
            if not isinstance(a, unreal.StaticMeshActor):
                continue
            o, e = a.get_actor_bounds(False)
            pts = []
            for sx in (-1, 1):
                for sy in (-1, 1):
                    for sz in (-1, 1):
                        p = proj([o.x + sx * e.x, o.y + sy * e.y, o.z + sz * e.z])
                        if p:
                            pts.append(p)
            if len(pts) < 8:
                continue
            x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
            y0, y1 = min(p[1] for p in pts), max(p[1] for p in pts)
            dist = min(p[2] for p in pts)
            tags = [str(t) for t in a.get_editor_property("tags")]
            for i, (rx0, ry0, rx1, ry1) in enumerate(rects):
                if x1 < rx0 or x0 > rx1 or y1 < ry0 or y0 > ry1:
                    continue
                # ignore huge actors (terrain, sky) that merely contain the rect
                if (x1 - x0) * (y1 - y0) > 0.4 * W * H:
                    continue
                hits[i].append({"label": a.get_actor_label(), "tags": tags,
                                "bounds_origin": [round(o.x), round(o.y), round(o.z)],
                                "extent": [round(e.x), round(e.y), round(e.z)],
                                "dist_cm": round(dist), "screen": [round(x0), round(y0), round(x1), round(y1)]})
        for i in hits:
            hits[i].sort(key=lambda h: h["dist_cm"])
        res = {"shot": name, "rects": rects, "hits": {str(i): hits[i][:40] for i in hits}}
        if out:
            with open(out, "w") as fh:
                json.dump(res, fh, indent=1)
        ok = True
    except Exception as e:
        unreal.log_error("BR_PROBE ERROR %s\n%s" % (e, traceback.format_exc()))
    unreal.log("BR_PROBE_DONE %s" % ("OK" if ok else "FAILED"))


main()
