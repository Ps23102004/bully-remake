# OWNER: slice-stairs
"""Headless capsule-sweep reachability probe of the saved map (runs INSIDE `-game -nullrhi`, no GPU, no window).

Flood-fills what a capsule (r30, half-height 85, step 45, walkable slope 50 deg: the BullyCharacter numbers) can walk
from a start point, using the level's real runtime collision (complex-as-simple triangles) via
SystemLibrary traces/sweeps, then checks whether the target is reached and writes an ASCII walkability trace.

Environment (set by run_stairs_probe.sh; defaults in brackets):
  BR_STAIRS_OUT     output directory for the json + ascii files
  BR_STAIRS_TAG     file-name tag                                             [probe]
  BR_STAIRS_START   "x,y[,zhint]" UE cm of the start (floor snapped from zhint+-400)  [-62860,32200  = lobby door exit]
  BR_STAIRS_CHECK   "name:x,y,z;name:x,y,z" extra points; each logged `BR_STAIRS check <name>: PASS|FAIL` (reached within
                    60 cm xy / 100 cm z of a flood node)
  BR_STAIRS_TARGET  "x,y,z" UE cm of the goal                                 [-63320,29140,550 = principal's office trigger]
  BR_STAIRS_STEP    node grid pitch in cm                                     [25]
  BR_STAIRS_BOX     "x0,y0,x1,y1" UE cm search box                            [-67700,26200,-57900,33600]
  BR_STAIRS_MAXH    MaxStepHeight cm                                          [45]
  BR_STAIRS_SLOPE   walkable floor angle deg                                  [50]

Log lines (grep BR_STAIRS): `BR_STAIRS path <label>: PASS|FAIL`, reach statistics, blocker histogram at the frontier.
"""
import collections
import heapq
import json
import math
import os
import traceback

import unreal

MAP_PATH = "/Game/Imported/Maps/Bullworth"
R, HH = 30.0, 85.0


def log(msg):
    unreal.log("BR_STAIRS " + msg)


def game_world():
    # -game has no editor subsystems (EditorLevelLibrary.get_game_world() segfaults there); the map's UWorld is a plain object
    w = unreal.find_object(None, MAP_PATH + ".Bullworth")
    if w is None:
        raise RuntimeError("UWorld %s not found" % MAP_PATH)
    return w


def envf(name, default):
    v = os.environ.get(name)
    return v if v else default


class Probe:
    def __init__(self, world, max_h, slope_deg):
        self.w = world
        self.SL = unreal.SystemLibrary
        self.ch = unreal.TraceTypeQuery.TRACE_TYPE_QUERY1  # ECC_Visibility (all level meshes block it)
        self.max_h = max_h
        self.min_nz = math.cos(math.radians(slope_deg))
        self.V = unreal.Vector
        self.none = unreal.DrawDebugTrace.NONE
        self.calls = 0

    def line(self, x, y, z0, z1):
        self.calls += 1
        return self.SL.line_trace_single(self.w, self.V(x, y, z0), self.V(x, y, z1), self.ch, True, [], self.none, True)

    def sweep(self, a, b):
        """capsule sweep a->b (centres, tuples). Returns hit (HitResult) or None."""
        self.calls += 1
        return self.SL.capsule_trace_single(self.w, self.V(*a), self.V(*b), R, HH, self.ch, True, [], self.none, True)

    @staticmethod
    def hv(h):
        t = h.to_tuple()
        # (blocking, initial_overlap, time, distance, location, impact_point, normal, impact_normal, physmat, actor, comp, ...)
        return t

    def floor_under(self, x, y, z):
        """Highest walkable floor at/below z+HH (a line trace for seeding the start only). Returns foot z or None."""
        h = self.line(x, y, z + 400, z - 400)
        if not h:
            return None
        t = h.to_tuple()
        return t[5].z

    def free(self, c):
        h = self.sweep(c, (c[0], c[1], c[2] + 0.05))
        if not h:
            return True
        t = h.to_tuple()
        return not (t[0] or t[1])

    def step(self, node, dx, dy):
        """One CharacterMovement-style ground step from foot position node=(x,y,z) by (dx,dy).
        Returns (new_foot_z, None) on success, (None, reason) on failure."""
        x, y, z = node
        c0 = (x, y, z + HH + 1.0)
        up = (x, y, z + HH + 1.0 + self.max_h)
        # 1. step-up headroom
        h = self.sweep(c0, up)
        lift = self.max_h
        if h and h.to_tuple()[0]:
            lift = max(0.0, h.to_tuple()[3] - 0.5)
        a = (x, y, z + HH + 1.0 + lift)
        b = (x + dx, y + dy, a[2])
        # 2. forward at lifted height
        h = self.sweep(a, b)
        if h:
            t = h.to_tuple()
            if t[0]:
                return None, ("blocked", self.actor_name(t[9]), round(t[7].z, 2), round(t[5].z))
        # 3. settle down: max step height below the original foot level (walk-down of one step) plus the lift
        d = (b[0], b[1], z + HH + 1.0 - self.max_h)
        h = self.sweep(b, d)
        if not h:
            return None, ("nofloor",)
        t = h.to_tuple()
        if not t[0]:
            return None, ("nofloor",)
        if t[1]:
            return None, ("startpen",)
        nz = t[7].z
        foot = t[4].z - HH
        if nz < self.min_nz:
            return None, ("steep", round(nz, 2), self.actor_name(t[9]), round(t[5].z))
        return foot, None

    @staticmethod
    def actor_name(a):
        try:
            return a.get_actor_label() if hasattr(a, "get_actor_label") else a.get_name()
        except Exception:
            return str(a)


def flood(P, start, target, step, box, label):
    x0, y0, x1, y1 = box
    sz = P.floor_under(start[0], start[1], start[2] if len(start) > 2 else 0)
    if sz is None:
        raise RuntimeError("no floor under start %s" % (start,))
    log("%s start foot z=%.1f" % (label, sz))
    ZB = 10.0  # z bucket (cm) for node identity

    def key(x, y, z):
        return (int(round((x - start[0]) / step)), int(round((y - start[1]) / step)), int(round(z / ZB)))

    nodes = {}
    s_node = (start[0], start[1], sz)
    nodes[key(*s_node)] = (s_node, None)
    q = collections.deque([key(*s_node)])
    dirs = [(1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)]
    fails = collections.Counter()
    fail_at = {}
    reached_goal = None
    while q:
        k = q.popleft()
        n = nodes[k][0]
        for ddx, ddy in dirs:
            dx, dy = ddx * step, ddy * step
            nx, ny = n[0] + dx, n[1] + dy
            if not (x0 <= nx <= x1 and y0 <= ny <= y1):
                continue
            pk_xy = (int(round((nx - start[0]) / step)), int(round((ny - start[1]) / step)))
            # cheap dedupe: skip when a node at this xy within one z bucket of the likely landing exists is not possible
            # before sweeping, so just sweep.
            fz, why = P.step(n, dx, dy)
            if why is not None:
                tag = why[0] if why[0] != "blocked" else "blocked"
                fails[tag] += 1
                # remember the highest-z failure samples per reason for the frontier report
                fail_at.setdefault(k, []).append((ddx, ddy, why))
                continue
            nk = key(nx, ny, fz)
            if nk in nodes:
                continue
            nodes[nk] = ((nx, ny, fz), k)
            q.append(nk)
            if len(nodes) % 5000 == 0:
                log("%s ... %d nodes, queue %d, calls %d" % (label, len(nodes), len(q), P.calls))
            if reached_goal is None and abs(nx - target[0]) <= 60 and abs(ny - target[1]) <= 60 and abs(fz - target[2]) <= 100:
                reached_goal = nk
    return nodes, fails, fail_at, reached_goal, sz


def path_of(nodes, k):
    out = []
    while k is not None:
        out.append(nodes[k][0])
        k = nodes[k][1]
    return out[::-1]


def ascii_trace(nodes, step, box, goal_xy, start_xy, fname, zdiv=50.0):
    """x = rows (UE X), y = columns. char = highest reached floor z / zdiv (0-9A-Za-z), '*' goal, 'S' start."""
    x0, y0, x1, y1 = box
    best = {}
    for (n, _p) in nodes.values():
        ij = (int(round((n[0] - x0) / step)), int(round((n[1] - y0) / step)))
        if ij not in best or n[2] > best[ij]:
            best[ij] = n[2]
    if not best:
        return
    xs = [k[0] for k in best]
    ys = [k[1] for k in best]
    rows = []
    levels = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    gz = (int(round((goal_xy[0] - x0) / step)), int(round((goal_xy[1] - y0) / step)))
    sz = (int(round((start_xy[0] - x0) / step)), int(round((start_xy[1] - y0) / step)))
    rows.append("# rows = UE X from %d step %d cm (north-up is -X), cols = UE Y from %d step %d cm; char = reached floor z/%d cm "
                "(0=z<%d ... min z shown -> %s); '*' goal, 'S' start, ' ' unreached"
                % (x0 + min(xs) * step, step, y0 + min(ys) * step, step, zdiv, zdiv, "see json"))
    zmin = min(best.values())
    for i in range(min(xs), max(xs) + 1):
        line = []
        for j in range(min(ys), max(ys) + 1):
            if (i, j) == gz:
                line.append("*")
            elif (i, j) == sz:
                line.append("S")
            elif (i, j) in best:
                line.append(levels[max(0, min(35, int((best[(i, j)] - zmin) // zdiv)))])
            else:
                line.append(" ")
        rows.append("x=%7d |%s|" % (x0 + i * step, "".join(line)))
    rows.insert(1, "# char value = floor(z - %.0f)/%d cm" % (zmin, zdiv))
    with open(fname, "w") as f:
        f.write("\n".join(rows) + "\n")


def run_one(P, label, start, target, step, box, outdir, tag):
    nodes, fails, fail_at, goal, sz = flood(P, start, target, step, box, label)
    zs = [v[0][2] for v in nodes.values()]
    log("%s reached %d nodes (calls=%d) z range %.0f..%.0f; edge failures: %s"
        % (label, len(nodes), P.calls, min(zs), max(zs), dict(fails)))
    res = {"label": label, "start": start, "target": target, "nodes": len(nodes), "z_min": min(zs), "z_max": max(zs),
           "fail_counts": dict(fails)}
    if goal is not None:
        path = path_of(nodes, goal)
        res.update({"result": "PASS", "path_len_nodes": len(path), "path": [[round(a, 1) for a in p] for p in path]})
        log("path %s: PASS (%d nodes, %.1f m, end z=%.0f)" % (label, len(path), len(path) * step / 100.0, path[-1][2]))
    else:
        # the best node = the reached node closest (3D weighted) to the target; report the blockers around it
        best = min(nodes.items(), key=lambda kv: (kv[1][0][0] - target[0]) ** 2 + (kv[1][0][1] - target[1]) ** 2
                   + 4 * (kv[1][0][2] - target[2]) ** 2)
        bn = best[1][0]
        # highest reached node
        hi = max(nodes.items(), key=lambda kv: kv[1][0][2])
        res.update({"result": "FAIL", "closest": [round(v, 1) for v in bn], "highest": [round(v, 1) for v in hi[1][0]]})
        log("path %s: FAIL closest reached node (%.0f,%.0f,z=%.0f) highest reached z=%.0f at (%.0f,%.0f)"
            % (label, bn[0], bn[1], bn[2], hi[1][0][2], hi[1][0][0], hi[1][0][1]))
        # blockers at the highest nodes (frontier)
        top = sorted(nodes.items(), key=lambda kv: -kv[1][0][2])[:200]
        agg = collections.Counter()
        for k, (n, _p) in top:
            for (ddx, ddy, why) in fail_at.get(k, []):
                agg[why] += 1
        res["frontier_blockers"] = [[list(map(str, k)), c] for k, c in agg.most_common(12)]
        for k, c in agg.most_common(12):
            log("  frontier blocker x%d: %s" % (c, k))
        path = path_of(nodes, hi[0])
        res["path_to_highest"] = [[round(a, 1) for a in p] for p in path]
    checks = os.environ.get("BR_STAIRS_CHECK")
    if checks:
        for item in checks.split(";"):
            if not item.strip():
                continue
            name, xyz = item.split(":")
            cx, cy, cz = [float(v) for v in xyz.split(",")]
            near = [n for (n, _p) in nodes.values() if abs(n[0] - cx) <= 60 and abs(n[1] - cy) <= 60 and abs(n[2] - cz) <= 100]
            res.setdefault("checks", {})[name] = bool(near)
            log("check %s (%.0f,%.0f,%.0f): %s" % (name, cx, cy, cz, "PASS" if near else "FAIL"))
    ascii_trace(nodes, step, box, target, start, os.path.join(outdir, "%s_%s_trace.txt" % (tag, label.replace("->", "_to_").replace(" ", ""))))
    with open(os.path.join(outdir, "%s_%s.json" % (tag, label.replace("->", "_to_").replace(" ", ""))), "w") as f:
        json.dump(res, f)
    return res


def profile(P, outdir, tag):
    """BR_STAIRS_PROFILE="x0,y0,x1,y1,pitch[,ztop]": floor height along a line (highest surface below ztop, default 400 cm
    above... i.e. first hit from ztop). Logs rise/run and every riser > 3 cm: stair geometry as the capsule meets it."""
    a = [float(v) for v in os.environ["BR_STAIRS_PROFILE"].split(",")]
    x0, y0, x1, y1, pitch = a[:5]
    ztop = a[5] if len(a) > 5 else 400.0
    n = int(max(abs(x1 - x0), abs(y1 - y0)) / pitch)
    prev = None
    rows = []
    for i in range(n + 1):
        t = i / float(max(1, n))
        x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
        h = P.line(x, y, ztop, -200)
        if not h:
            rows.append((i * pitch, None, None, None))
            continue
        tt = h.to_tuple()
        z = tt[5].z
        rows.append((i * pitch, round(z, 1), round(tt[7].z, 2), P.actor_name(tt[9])))
    with open(os.path.join(outdir, "%s_profile.txt" % tag), "w") as f:
        for r in rows:
            f.write("%s\n" % (r,))
    risers = []
    for (d0, z0, n0, a0), (d1, z1, n1, a1) in zip(rows, rows[1:]):
        if z0 is not None and z1 is not None and abs(z1 - z0) > 3:
            risers.append((d1, round(z1 - z0, 1), a1))
    log("profile %s -> %s pitch %s: %d samples, %d risers>3cm: %s" % ((x0, y0), (x1, y1), pitch, len(rows), len(risers), risers[:40]))


def sweep_report(P):
    """BR_STAIRS_SWEEP="x,y,zfoot,x2,y2": one capsule sweep (no step lift), logs what blocks it."""
    a = [float(v) for v in os.environ["BR_STAIRS_SWEEP"].split(",")]
    c0 = (a[0], a[1], a[2] + HH + 1.0)
    c1 = (a[3], a[4], a[2] + HH + 1.0)
    h = P.sweep(c0, c1)
    if not h or not h.to_tuple()[0]:
        log("sweep %s -> %s: CLEAR" % (c0, c1))
        return
    t = h.to_tuple()
    log("sweep %s -> %s: BLOCKED after %.0f cm by %s normal=(%.2f,%.2f,%.2f) impact=(%.0f,%.0f,%.0f)"
        % (c0, c1, t[3], P.actor_name(t[9]), t[7].x, t[7].y, t[7].z, t[5].x, t[5].y, t[5].z))


def main():
    world = game_world()
    outdir = envf("BR_STAIRS_OUT", "/tmp")
    tag = envf("BR_STAIRS_TAG", "probe")
    step = float(envf("BR_STAIRS_STEP", "25"))
    box = [float(v) for v in envf("BR_STAIRS_BOX", "-67700,26200,-57900,33600").split(",")]
    P = Probe(world, float(envf("BR_STAIRS_MAXH", "45")), float(envf("BR_STAIRS_SLOPE", "50")))
    start = [float(v) for v in envf("BR_STAIRS_START", "-62860,32200").split(",")]
    target = [float(v) for v in envf("BR_STAIRS_TARGET", "-63320,29140,550").split(",")]
    label = envf("BR_STAIRS_LABEL", "lobby->office")
    if envf("BR_STAIRS_MODE", "flood") == "sweep":
        sweep_report(P)
        return
    if envf("BR_STAIRS_MODE", "flood") == "profile":
        profile(P, outdir, tag)
        return
    log("probe %s start=%s target=%s step=%s maxH=%s box=%s" % (label, start, target, step, P.max_h, box))
    run_one(P, label, start, target, step, box, outdir, tag)


def _run():
    try:
        main()
    except Exception:
        unreal.log_error("BR_STAIRS ERROR " + traceback.format_exc())
    try:
        unreal.SystemLibrary.execute_console_command(game_world(), "quit")
    except Exception:
        unreal.log_error("BR_STAIRS quit failed " + traceback.format_exc())


_state = {"t": 0.0, "h": None}


def _tick(dt):
    _state["t"] += dt
    if _state["t"] < 5.0:
        return
    unreal.unregister_slate_post_tick_callback(_state["h"])
    _run()


_state["h"] = unreal.register_slate_post_tick_callback(_tick)
