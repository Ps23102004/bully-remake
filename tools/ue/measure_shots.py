#!/usr/bin/env python3
# OWNER: builder-B5 (shot)
"""Luminance measurements of the four hero frames against the critic's round-1 numeric targets.

  measure_shots.py DIR [--md MEASUREMENTS.md] [--sheet sheet.jpg]

Rec.709 luma 0-255 on the full-resolution PNGs (regions scale with the image size; the critic's coordinates
are for 3840x2160). Prints a table and optionally writes it as markdown plus a 2x2 contact sheet.
"""
import argparse
import os

import numpy as np
from PIL import Image

NAMES = ["00_front_gates", "01_facade_courtyard", "02_path_trees", "03_lobby_interior"]


def luma(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def reg(L, x0, y0, x1, y1):
    h, w = L.shape
    sx, sy = w / 3840.0, h / 2160.0
    return L[int(y0 * sy):int(y1 * sy), int(x0 * sx):int(x1 * sx)]


def measure(d):
    rows = []
    for n in NAMES:
        p = os.path.join(d, n + ".png")
        if not os.path.exists(p):
            continue
        L = luma(p)
        r = {"shot": n, "median": float(np.median(L)), "p_lt10": 100.0 * float((L < 10).mean()), "max": float(L.max())}
        if n == "00_front_gates":
            r["road_band_mean (rows 1680-2160)"] = float(reg(L, 0, 1680, 3840, 2160).mean())
        if n == "02_path_trees":
            c = reg(L, 500, 0, 1600, 800)
            r["canopy_pct_lt25 (500-1600,0-800)"] = 100.0 * float((c < 25).mean())
            r["fg_left_median (0-1300,1100-2160)"] = float(np.median(reg(L, 0, 1100, 1300, 2160)))
            r["fg_right_median (2200-3840,1300-2160)"] = float(np.median(reg(L, 2200, 1300, 3840, 2160)))
        if n == "03_lobby_interior":
            r["right35_median"] = float(np.median(reg(L, 2496, 0, 3840, 2160)))
            r["windows_mean (1450-2450,200-400)"] = float(reg(L, 1450, 200, 2450, 400).mean())
            # the five arched clerestory panes themselves (inner boxes, 3840x2160 coords, round-2 framing)
            panes = [reg(L, x0, 95, x1, 205) for x0, x1 in ((1325, 1432), (1590, 1720), (1851, 1981), (2116, 2246), (2381, 2511))]
            r["arch_window_panes_mean"] = float(np.mean([p.mean() for p in panes]))
        rows.append(r)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--md")
    ap.add_argument("--sheet")
    a = ap.parse_args()
    rows = measure(a.dir)
    lines = []
    for r in rows:
        lines.append("%s: " % r["shot"] + ", ".join("%s %.1f" % (k, v) for k, v in r.items() if k != "shot"))
    print("\n".join(lines))
    if a.sheet:
        ims = [Image.open(os.path.join(a.dir, n + ".png")).convert("RGB").resize((1280, 720)) for n in NAMES if os.path.exists(os.path.join(a.dir, n + ".png"))]
        S = Image.new("RGB", (2560, 1440))
        for i, im in enumerate(ims):
            S.paste(im, ((i % 2) * 1280, (i // 2) * 720))
        S.save(a.sheet, quality=90)
    return rows


if __name__ == "__main__":
    main()
