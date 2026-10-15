#!/usr/bin/env python3
# OWNER: builder-B5 (shot)
"""Write one settings sidecar JSON per rendered shot (plain python3, no Unreal).

  shot_sidecar.py --variant A_classic_gold --shots shots_m5.json --result <out>/shots_result.json --out <dir>

For every <out>/NN_<name>.png listed in shots_result.json it writes <out>/NN_<name>.json with the lighting
variant's settings (sun elevation / azimuth / intensity, fog, exposure EV100 and white balance), the final
camera, resolution and the settle/warm-up timing, so a critique round can be reproduced exactly.
"""
import argparse
import datetime
import json
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", required=True)
    ap.add_argument("--shots", required=True)
    ap.add_argument("--result", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    lv = json.load(open(os.path.join(HERE, "lighting_variants.json")))
    v = lv["variants"][a.variant]
    shots = json.load(open(a.shots))
    res = json.load(open(a.result))
    try:
        commit = subprocess.check_output(["git", "-C", HERE, "rev-parse", "--short", "HEAD"], text=True).strip()
    except Exception:
        commit = "unknown"
    for s in res["shots"]:
        base = os.path.splitext(os.path.basename(s["file"]))[0]
        png = os.path.join(a.out, base + ".png")
        if not os.path.exists(png):
            continue
        sc = {
            "format": "bully-remake.shot-sidecar/1",
            "image": base + ".png",
            "resolution": shots["highres_size"],
            "variant": a.variant,
            "description": v["description"],
            "sun": {"elevation_deg": v["sun"]["elevation_deg"], "azimuth_deg": v["sun"]["azimuth_deg"],
                    "intensity_lux": v["sun"]["intensity_lux"], "temperature_k": v["sun"]["temperature_k"],
                    "source_angle_deg": v["sun"]["source_angle_deg"]},
            "sky_light_intensity": v["sky"]["intensity"],
            "fog": v["fog"],
            "atmosphere": v["atmosphere"],
            "exposure": {"mode": "manual lock (auto exposure min = max)", "ev100": v["exposure_ev100"]},
            "white_balance": v["white_balance"],
            "post": v["post"],
            "interior": lv["interior"],
            "camera": {"name": s["name"], "location_cm": s["location_cm"], "look_at_cm": s["look_at_cm"], "fov_deg": s["fov"]},
            "determinism": {"warmup_seconds": shots["warmup_seconds"], "settle_seconds": shots["settle_seconds"],
                            "exec": shots["exec"], "auto_exposure": "locked", "time_of_day": "static sun, no animation"},
            "repo_commit": commit,
            "rendered_utc": datetime.datetime.fromtimestamp(os.path.getmtime(png), datetime.timezone.utc).isoformat(timespec="seconds"),
        }
        with open(os.path.join(a.out, base + ".json"), "w") as f:
            json.dump(sc, f, indent=1)
        print("sidecar", base + ".json")


if __name__ == "__main__":
    main()
