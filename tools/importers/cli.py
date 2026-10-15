# OWNER: builder-A (importers)
"""Command line: `python3 -m tools.importers <command> ...`

The game install path comes from, in order: --install, the BULLY_INSTALL
environment variable, or `tools/importers/local_config.json` (git-ignored,
{"install": "..."}). It is never hard-coded. The install is opened
read-only; output goes under --out (default `imported/`).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

from .binreader import FormatError
from .gltf import validate_gltf

HERE = os.path.dirname(os.path.abspath(__file__))
LOCAL_CONFIG = os.path.join(HERE, "local_config.json")


def resolve_install(arg: str | None) -> str:
    path = arg or os.environ.get("BULLY_INSTALL")
    if not path and os.path.exists(LOCAL_CONFIG):
        with open(LOCAL_CONFIG) as f:
            path = json.load(f).get("install")
    if not path:
        sys.exit("game install not given: pass --install, set BULLY_INSTALL, or create "
                 f"{os.path.relpath(LOCAL_CONFIG)} with {{\"install\": \"...\"}}")
    if not os.path.isdir(os.path.join(path, "Stream")):
        sys.exit(f"{path}: not a Bully: Scholarship Edition install (no Stream/ folder)")
    return path


def _archive(install: str, rel: str):
    from .img import ImgArchive
    p = rel if os.path.isabs(rel) else os.path.join(install, rel)
    return ImgArchive(p)


def cmd_img(a) -> int:
    install = resolve_install(a.install)
    with _archive(install, a.archive) as img:
        if a.action == "list":
            names = img.names(a.ext)
            for e in img.entries:
                if e.name in names or a.ext is None:
                    if a.ext is None or e.ext == a.ext.lower().lstrip("."):
                        print(f"{e.index:6d} {e.offset_sectors:9d} {e.size_sectors:6d} {e.name}")
            print(f"# {len(img)} entries; census {img.census()}; overlaps {len(img.check_overlaps())}; "
                  f"duplicate names {len(img.duplicates)}", file=sys.stderr)
        else:
            dest = os.path.join(a.out, "raw", os.path.splitext(os.path.basename(a.archive))[0])
            os.makedirs(dest, exist_ok=True)
            names = a.names or img.names(a.ext)
            for n in names:
                e = img.find(n)
                if e is None:
                    print(f"missing: {n}", file=sys.stderr)
                    continue
                with open(os.path.join(dest, e.name), "wb") as f:
                    f.write(img.read_entry(e))
            print(f"extracted {len(names)} entries to {dest}")
    return 0


def cmd_nif(a) -> int:
    from .convert import TextureStore, convert_model
    from .ide import ModelDefs
    install = resolve_install(a.install)
    with _archive(install, "Stream/World.img") as img:
        defs = ModelDefs.load_install(install)
        store = TextureStore(img, a.out)
        rc = 0
        for name in a.names:
            stem = os.path.splitext(name)[0]
            e = img.find(stem + ".nif")
            if e is None:
                print(f"{name}: not in World.img", file=sys.stderr)
                rc = 1
                continue
            ide = defs.get(e.stem)
            txd = a.txd or (ide.txd if ide else None)
            try:
                r = convert_model(e.stem, img.read_entry(e), txd, store, a.out, entity_space=not a.file_space)
            except FormatError as ex:
                print(f"{name}: FAILED {ex}", file=sys.stderr)
                rc = 1
                continue
            print(json.dumps({"model": e.stem, "gltf": r.gltf, "txd": txd, "shapes": r.shapes,
                              "vertices": r.vertices, "triangles": r.triangles,
                              "missing_textures": r.missing_textures, "warnings": r.warnings[:10]}))
    return rc


def cmd_ped(a) -> int:
    from .convert import TextureStore
    from .ide import ModelDefs
    from .ped import PLAYER_LOCOMOTION, clip_requests, export_ped
    install = resolve_install(a.install)
    with _archive(install, "Stream/World.img") as img:
        defs = ModelDefs.load_install(install)
        e = img.find(a.name + ".nif")
        if e is None:
            print(f"{a.name}: not in World.img", file=sys.stderr)
            return 1
        ide = defs.get(e.stem)
        txd = a.txd or (ide.txd if ide else None)
        store = None if a.no_textures else TextureStore(img, a.out)
        names = a.anims if a.anims else (PLAYER_LOCOMOTION if a.group.lower() == "c_player" else [])
        if a.no_anims:
            names = []
        try:
            reqs = clip_requests(install, img, a.group, names) if names else []
            head = None
            if a.head:
                he = img.find(a.head + ".nif")
                if he is None:
                    print(f"{a.head}: not in World.img", file=sys.stderr)
                    return 1
                hide = defs.get(he.stem)
                head = (he.stem, img.read_entry(he), a.head_txd or (hide.txd if hide else he.stem))   # clothing rows: model == txd name
            r = export_ped(e.stem, img.read_entry(e), txd, store, a.out, reqs, root_motion=a.root_motion, head=head)
        except FormatError as ex:
            print(f"{a.name}: FAILED {ex}", file=sys.stderr)
            return 1
    problems = validate_gltf(os.path.join(a.out, r.gltf), check_images=store is not None)
    out = {"model": r.name, "gltf": r.gltf, "joints": r.joints, "skins": r.skins, "shapes": r.shapes,
           "vertices": r.vertices, "triangles": r.triangles, "max_influences": r.max_influences,
           "unweighted_vertices": r.unweighted_vertices,
           "weight_sum_max_error": round(r.weight_sum_max_error, 6),
           "bind_vs_stored_max_mm": round(r.bind_vs_stored_max_m * 1000, 3),
           "ibm_vs_file_pose_max": round(r.bind_vs_file_pose_max, 6),
           "skipped_shapes": r.skipped_shapes, "missing_textures": r.missing_textures,
           "animations": [{k: c[k] for k in ("name", "duration", "key_type", "tracks", "root_motion",
                                              "loops", "start_end_max_deg")} for c in r.clips],
           "warnings": r.warnings[:10], "validation_problems": problems}
    print(json.dumps(out, indent=1))
    if a.sheet:
        from .skinpreview import contact_sheet
        for view in a.sheet_views:
            stem, ext = os.path.splitext(a.sheet)
            contact_sheet(os.path.join(a.out, r.gltf), f"{stem}_{view}{ext or '.png'}", frames=a.sheet_frames,
                          view=view, log=lambda m: print(m, file=sys.stderr))
    return 1 if problems else 0


def cmd_audio(a) -> int:
    from . import audio
    install = resolve_install(a.install)
    if a.action == "survey":
        r = audio.survey(install)
        r["streams"] = audio.survey_streams(install)
        print(json.dumps(r, indent=1))
        return 0
    if a.action == "list":
        for p in audio.sea_files(install):
            if a.banks and p.stem.lower() not in {b.lower() for b in a.banks}:
                continue
            b = audio.parse_sea(p)
            print(f"# {p.stem}: bank '{b.bank_name}', {len(b.entries)} entries")
            for e in b.entries:
                print(f"{e.index:4d} {e.name:40s} {e.seconds:7.3f}s {e.sample_rate:6d} Hz {e.channels}ch "
                      f"{'adpcm' if e.tag == 2 else e.tag}")
        return 0
    out = os.path.join(a.out, "audio")
    man = audio.extract(install, out, a.banks or None, a.names, log=lambda m: print(m, file=sys.stderr))
    print(json.dumps({"out": out, "wavs": man["count"]}))
    return 0


def cmd_anim(a) -> int:
    from .agr import arrow_displacement, parse_agr
    from .ped import load_hxd, read_group_agr
    install = resolve_install(a.install)
    hxd = load_hxd(install, a.hxd)
    if not a.group:
        for g in hxd.groups:
            print(f"{g.index:4d} {g.name:24s} {len(hxd.group_anims(g.index)):4d} clips")
        return 0
    recs = hxd.group_anims(a.group)
    with _archive(install, "Stream/World.img") as img:
        data, src = read_group_agr(install, img, a.group)
    clips = parse_agr(data, [r.agr_size - 4 for r in recs]) if recs else parse_agr(data)
    print(f"# {src}: {len(clips)} clips, HXD {a.hxd} lists {len(recs)}")
    for k, c in enumerate(clips):
        r = recs[k] if k < len(recs) else None
        d = r.duration if r else c.duration
        t = arrow_displacement(c)
        speed = (t[0] ** 2 + t[1] ** 2 + t[2] ** 2) ** 0.5 / d if d else 0
        print(f"{k:4d} {(r.name if r else '?'):40s} type {c.key_type} bones {c.bone_count:2d} "
              f"keys {c.key_count:5d} dur {d:6.3f}s travel ({t[0]:+.3f},{t[1]:+.3f},{t[2]:+.3f}) m "
              f"{speed:5.2f} m/s")
    return 0


def cmd_nft(a) -> int:
    from .nft import TextureDictionary, safe_name
    install = resolve_install(a.install)
    with _archive(install, "Stream/World.img") as img:
        rc = 0
        for name in a.names:
            stem = os.path.splitext(os.path.basename(name))[0]
            try:
                if os.path.exists(name):
                    with open(name, "rb") as f:
                        td = TextureDictionary(stem, f.read())
                else:
                    td = TextureDictionary(stem, img.read(stem + ".nft"))
            except (KeyError, FormatError) as ex:
                print(f"{name}: FAILED {ex}", file=sys.stderr)
                rc = 1
                continue
            folder = os.path.join(a.out, "textures", safe_name(td.name))
            os.makedirs(folder, exist_ok=True)
            for t in td.textures.values():
                try:
                    info = td.export(t, os.path.join(folder, safe_name(t.name) + ".png"))
                    print(f"{td.name}/{t.name}: {info['width']}x{info['height']} {info['format']}")
                except FormatError as ex:
                    print(f"{td.name}/{t.name}: FAILED {ex}", file=sys.stderr)
                    rc = 1
    return rc


def cmd_ipb(a) -> int:
    from .ipb import parse_ipb
    install = resolve_install(a.install)
    os.makedirs(os.path.join(a.out, "ipb"), exist_ok=True)
    with _archive(install, "Stream/World.img") as img:
        for name in a.names:
            stem = os.path.splitext(name)[0]
            ipb = parse_ipb(img.read(stem + ".ipb"))
            p = os.path.join(a.out, "ipb", stem + ".json")
            with open(p, "w") as f:
                json.dump({"source": f"World.img/{stem}.ipb", "header_unknown_hex": ipb.header_unknown.hex(),
                           "sections": ipb.section_counts,
                           "placements": [x.to_json() for x in ipb.placements]}, f, indent=1)
            print(f"{stem}: {len(ipb.placements)} placements {ipb.section_counts} -> {p}")
    return 0


def cmd_world(a) -> int:
    from .world import PRESETS, build_world
    install = resolve_install(a.install)
    stems = a.ipb or PRESETS[a.preset]
    report = build_world(install, a.out, stems, entity_space=not a.file_space,
                         convert_invisible=a.convert_invisible)
    print(json.dumps(report, indent=1))
    return 0 if report["models_converted"] else 1


def cmd_validate(a) -> int:
    t = time.time()
    folder = os.path.join(a.out, "models")
    bad = 0
    files = sorted(f for f in os.listdir(folder) if f.endswith(".gltf"))
    for f in files:
        problems = validate_gltf(os.path.join(folder, f))
        if problems:
            bad += 1
            print(f"{f}: {problems[:5]}")
    print(f"validated {len(files)} glTF files, {bad} with problems, {time.time() - t:.1f}s")
    return 1 if bad else 0


def cmd_preview(a) -> int:
    from .preview import render
    render(a.out, a.scenes, view=a.view, size=a.size, cut=a.cut, out_path=a.png)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python3 -m tools.importers", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", help="game install root (else BULLY_INSTALL or local_config.json)")
    ap.add_argument("--out", default="imported", help="output folder (default: imported/)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("img", help="list or extract IMG/DIR archive entries")
    p.add_argument("action", choices=["list", "extract"])
    p.add_argument("archive", nargs="?", default="Stream/World.img", help="archive path, relative to the install")
    p.add_argument("names", nargs="*", help="entries to extract (default: all, or all of --ext)")
    p.add_argument("--ext", help="filter by extension, e.g. nif")
    p.set_defaults(fn=cmd_img)

    p = sub.add_parser("nif", help="convert World.img models to glTF")
    p.add_argument("names", nargs="+")
    p.add_argument("--txd", help="override the texture dictionary (default: from the IDE)")
    p.add_argument("--file-space", action="store_true", help="keep root/model node transforms")
    p.set_defaults(fn=cmd_nif)

    p = sub.add_parser("ped", help="skinned character (e.g. PLAYER) to glTF with skeleton and .agr clips")
    p.add_argument("name", help="World.img model stem, e.g. PLAYER")
    p.add_argument("--group", default="C_Player", help="animation group (.agr) for the clips (default C_Player)")
    p.add_argument("--anims", nargs="*", help="clip names (IDLE, C_PLAYER\\RUN, or * for all); "
                                               "default: Jimmy's locomotion set")
    p.add_argument("--no-anims", action="store_true", help="skeleton and skin only")
    p.add_argument("--root-motion", default="inplace", choices=["inplace", "raw", "dummy"])
    p.add_argument("--head", help="clothing head/hair model (e.g. B_Buzz = Jimmy's default hair) that replaces the base head: "
                                  "its shape is skinned onto the ped's skeleton by joint name, the base head triangles are cut out")
    p.add_argument("--head-txd", help="texture dictionary of --head (default: its IDE row, else the model name)")
    p.add_argument("--txd", help="override the texture dictionary (default: from the IDE)")
    p.add_argument("--no-textures", action="store_true")
    p.add_argument("--sheet", help="also render a CPU contact sheet PNG (bind pose + frames per clip)")
    p.add_argument("--sheet-views", nargs="+", default=["side", "front"], choices=["side", "front"])
    p.add_argument("--sheet-frames", type=int, default=6)
    p.set_defaults(fn=cmd_ped)

    p = sub.add_parser("anim", help="list HXD groups, or the clips of one group with durations and travel")
    p.add_argument("group", nargs="?", help="group name, e.g. C_Player (omit to list groups)")
    p.add_argument("--hxd", default="MAINPED")
    p.set_defaults(fn=cmd_anim)

    p = sub.add_parser("audio", help="audio/xact/*.SEA sound banks: survey, list, or decode to WAV (imported/audio/)")
    p.add_argument("action", choices=["survey", "list", "extract"])
    p.add_argument("banks", nargs="*", help="bank stems, e.g. FeetAll Birds Wind, or streamed Ambs/Cuts/Music/Speech (default: all .SEA)")
    p.add_argument("--names", help="regex on wave names (extract only)")
    p.set_defaults(fn=cmd_audio)

    p = sub.add_parser("nft", help="decode .nft texture dictionaries to PNG")
    p.add_argument("names", nargs="+", help="World.img .nft names or paths to loose .nft/.txd files")
    p.set_defaults(fn=cmd_nft)

    p = sub.add_parser("ipb", help="parse placement files to JSON")
    p.add_argument("names", nargs="+")
    p.set_defaults(fn=cmd_ipb)

    p = sub.add_parser("world", help="convert the academy: placements, models, textures, scene manifest")
    p.add_argument("--preset", default="academy", choices=["academy", "core"])
    p.add_argument("--ipb", nargs="*", help="explicit IPB stems (overrides --preset)")
    p.add_argument("--file-space", action="store_true", help="keep root/model node transforms in models")
    p.add_argument("--convert-invisible", action="store_true", help="also convert nogo/walkable/_nd markers")
    p.set_defaults(fn=cmd_world)

    p = sub.add_parser("validate", help="structurally validate every glTF under --out/models")
    p.set_defaults(fn=cmd_validate)

    p = sub.add_parser("preview", help="CPU software render of imported scenes to PNG (evidence only)")
    p.add_argument("scenes", nargs="+", help="scene names from the manifest, e.g. tschool")
    p.add_argument("--view", choices=["top", "oblique"], default="top")
    p.add_argument("--size", type=int, default=1600)
    p.add_argument("--cut", type=float, help="drop triangles above this game-space height (see inside)")
    p.add_argument("--png", help="output path (default: <out>/previews/...)")
    p.set_defaults(fn=cmd_preview)

    a = ap.parse_args(argv)
    return a.fn(a)
