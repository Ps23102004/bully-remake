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


def cmd_nft(a) -> int:
    from .nft import TextureDictionary, safe_name
    install = resolve_install(a.install)
    with _archive(install, "Stream/World.img") as img:
        rc = 0
        for name in a.names:
            stem = os.path.splitext(name)[0]
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

    a = ap.parse_args(argv)
    return a.fn(a)
