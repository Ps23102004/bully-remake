# OWNER: builder-A (importers)
"""`world` command: Bullworth Academy exterior + school interiors.

Reads the placement files, converts every model they place (once), exports
the textures those models use, and writes a scene manifest for the UE import
step. Nothing is written outside `out_dir`; the game install is only read.

Scene selection (survey 4.4): `tschool` is the academy exterior and grounds,
`ischool` the main building interior (lobby, halls, cafeteria), `isc_*` the
school rooms, `zone_school1..6` the street dressing of the school quarter
(trees, lamps, benches). Which IPBs the game streams for which area is
OPEN-4; this command simply converts the requested files.
"""

from __future__ import annotations

import json
import os
import time

from . import coords
from .binreader import FormatError
from .convert import TextureStore, convert_model
from .ide import ModelDefs
from .img import ImgArchive
from .ipb import parse_ipb, placement_quat_game, placement_rotation
from .nft import safe_name

ACADEMY = ["tschool", "ischool", "isc_art", "isc_audt", "isc_auto", "isc_bio", "isc_boil", "isc_chem",
           "isc_clas", "isc_dorm", "isc_Gdrm", "isc_lib", "isc_offi", "isc_pool", "isc_prep", "isc_staf",
           "zone_school1", "zone_school2", "zone_school3", "zone_school4", "zone_school5", "zone_school6"]
PRESETS = {
    "academy": ACADEMY,
    "core": ["tschool", "ischool"],
}

MANIFEST_FORMAT = "bully-remake.scene-manifest/1"

COORDINATE_DOC = {
    "game": "Right-handed, +Z up, 1 unit = 1 metre. Raw values from the .ipb files.",
    "gltf": "Model files under models/ are glTF 2.0 (right-handed, +Y up, metres). "
            "Vertices were mapped game (x, y, z) -> glTF (x, z, -y), a proper rotation, so winding is kept.",
    "ue": "Left-handed, +Z up, centimetres. game (x, y, z) -> UE (100x, -100y, 100z). "
          "UE's glTF importer maps glTF (x, y, z) -> UE (x, z, y) and metres -> cm, so an imported model "
          "and a placement converted here share one frame.",
    "placement_rule": "world_vertex = R * (scale * model_vertex) + position, scale per axis in model space. "
                      "R is the TRANSPOSE of the textbook matrix of the stored quaternion (x, y, z, w), "
                      "i.e. the stored quaternion is conjugated (bully-re docs/ipl.md, test_placement). "
                      "game.rotation_quat_xyzw is already conjugated; rotation_xyzw_stored is the raw value.",
    "ue_rotation": "ue.rotation_quat_xyzw is the quaternion of M*R*M, M = diag(1,-1,1), in UE axes; "
                   "pass it straight to FQuat(X, Y, Z, W). ue.scale = game scale (per axis, no sign change).",
    "model_space": "Models are in entity space: the NIF root and model node transforms are identity "
                   "(the game replaces the model node transform with the placement).",
}


def is_invisible_marker(name: str) -> bool:
    """Utility volumes never drawn by the game (bully-re docs/ipl.md):
    nog_/nogo_/walkable_ prefixes (flag 0x1000000 in the binary) and the
    'no draw' _nd / _ND_ names."""
    n = name.lower()
    return n.startswith(("nog_", "nogo_", "walkable_")) or n.endswith("_nd") or "_nd_" in n


def _placement_record(scene: str, p, model: str, gltf: str | None, visible: bool) -> dict:
    r_game = placement_rotation(p.rotation)
    r_ue = coords.game_to_ue_rotation(r_game)
    return {
        "scene": scene,
        "index": p.index,
        "model": model,
        "model_id": p.model_id,
        "gltf": gltf,
        "visible": visible,
        "game": {
            "position": list(p.position),
            "scale": list(p.scale),
            "rotation_xyzw_stored": list(p.rotation),
            "rotation_quat_xyzw": placement_quat_game(p.rotation),
            "rotation_rows": r_game,
        },
        "ue": {
            "location_cm": coords.game_to_ue_position(p.position),
            "rotation_quat_xyzw": coords.matrix_to_quat(r_ue),
            "rotation_rows": r_ue,
            "scale": list(p.scale),
        },
        "unknown": {"unk68": p.unk68, "unk112": p.unk112, "unk116": p.unk116},
    }


def build_world(install: str, out_dir: str, ipb_stems: list, entity_space: bool = True,
                convert_invisible: bool = False, log=print) -> dict:
    t0 = time.time()
    os.makedirs(out_dir, exist_ok=True)
    img = ImgArchive(os.path.join(install, "Stream", "World.img"))
    defs = ModelDefs.load_install(install)
    store = TextureStore(img, out_dir)
    scenes, placements, failures = [], [], []
    models: dict[str, dict] = {}
    wanted: dict[str, tuple] = {}  # canonical model name -> (txd, ide id)
    os.makedirs(os.path.join(out_dir, "ipb"), exist_ok=True)

    for stem in ipb_stems:
        entry = img.find(stem + ".ipb")
        if entry is None:
            failures.append({"category": "ipb_missing", "item": stem, "error": "not in World.img"})
            continue
        try:
            ipb = parse_ipb(img.read_entry(entry))
        except FormatError as ex:
            failures.append({"category": "ipb_parse", "item": entry.name, "error": str(ex)})
            continue
        with open(os.path.join(out_dir, "ipb", entry.stem + ".json"), "w") as f:
            json.dump({"source": f"World.img/{entry.name}", "header_unknown_hex": ipb.header_unknown.hex(),
                       "sections": ipb.section_counts,
                       "placements": [p.to_json() for p in ipb.placements]}, f, indent=1)
        kind = "interior" if entry.stem.lower().startswith("i") else "exterior"
        scene = {"name": entry.stem, "source": f"World.img/{entry.name}", "kind": kind,
                 "sections": ipb.section_counts, "placements": len(ipb.placements),
                 "header_unknown_hex": ipb.header_unknown.hex()}
        lo, hi = [float("inf")] * 3, [float("-inf")] * 3
        for p in ipb.placements:
            name = p.name
            ide = defs.get(name) if name else defs.by_id.get(p.model_id)
            if not name and ide:
                name = ide.model
            if ide is None:
                failures.append({"category": "ide_missing", "item": f"{entry.stem}#{p.index}:{name}",
                                 "error": "model not defined in any Objects/*.ide"})
            elif ide.id != p.model_id:
                failures.append({"category": "ide_id_mismatch", "item": f"{entry.stem}#{p.index}:{name}",
                                 "error": f"placement id {p.model_id} != IDE id {ide.id}"})
            nif_entry = img.find(name + ".nif")
            canonical = nif_entry.stem if nif_entry else name
            visible = not is_invisible_marker(canonical)
            if nif_entry is None:
                failures.append({"category": "nif_missing", "item": name, "error": "no .nif in World.img"})
            elif visible or convert_invisible:
                wanted.setdefault(canonical, (ide.txd if ide else None, ide.id if ide else None))
            gltf = f"models/{safe_name(canonical)}.gltf" if nif_entry and (visible or convert_invisible) else None
            placements.append(_placement_record(entry.stem, p, canonical, gltf, visible))
            for k in range(3):
                lo[k] = min(lo[k], p.position[k])
                hi[k] = max(hi[k], p.position[k])
        scene["bounds_game_positions"] = [lo, hi]
        scenes.append(scene)
        log(f"ipb {entry.name}: {len(ipb.placements)} placements {ipb.section_counts}")

    log(f"converting {len(wanted)} distinct models ...")
    for i, (name, (txd, ide_id)) in enumerate(sorted(wanted.items(), key=lambda kv: kv[0].lower())):
        e = img.find(name + ".nif")
        try:
            r = convert_model(e.stem, img.read_entry(e), txd, store, out_dir, entity_space=entity_space)
        except FormatError as ex:
            failures.append({"category": "nif_convert", "item": name, "error": str(ex)})
            continue
        if r.gltf is None:
            # empty_source: the shipped NIF has no geometry at all (e.g. the
            # DCL_* decal holders); that is the game data, not a parse fault.
            cat = "nif_empty_in_source" if r.empty_source else "nif_no_visible_geometry"
            failures.append({"category": cat, "item": name, "error": "; ".join(r.warnings)})
        models[name] = {
            "gltf": r.gltf, "txd": txd, "ide_id": ide_id, "shapes": r.shapes, "vertices": r.vertices,
            "triangles": r.triangles, "skipped_hidden": r.skipped_hidden, "skipped_empty": r.skipped_empty,
            "skinned_shapes": r.skinned_shapes, "bounds_game": r.bounds_game, "bin_bytes": r.bin_bytes,
            "winding_agree": r.winding_agree, "winding_checked": r.winding_checked,
            "missing_textures": r.missing_textures, "warnings": r.warnings[:20],
            "sanitized_nonfinite": r.sanitized,
        }
        if (i + 1) % 100 == 0:
            log(f"  {i + 1}/{len(wanted)} models")
    # a placement whose model failed to convert has no usable glTF
    for pl in placements:
        if pl["gltf"] and (pl["model"] not in models or models[pl["model"]]["gltf"] is None):
            pl["gltf"] = None
    for (txd, key), users in sorted(store.missing.items()):
        failures.append({"category": "texture_missing", "item": f"{txd}/{key}",
                         "error": f"not found in {txd or '(no txd)'} nor any .nft; used by {sorted(set(users))[:5]}"})
    for (txd, key), err in sorted(store.export_errors.items()):
        failures.append({"category": "texture_decode", "item": f"{txd}/{key}", "error": err})
    for txd, err in sorted(store.dict_errors.items()):
        if store.dicts.get(txd) is None:
            failures.append({"category": "txd_unreadable", "item": txd, "error": err})

    textures = sorted(store.exported.values(), key=lambda t: t["path"])
    manifest = {
        "format": MANIFEST_FORMAT,
        "generated_by": "tools/importers world",
        "coordinate_systems": COORDINATE_DOC,
        "units": {"game": "metre", "gltf": "metre", "ue": "centimetre"},
        "scenes": scenes,
        "placements": placements,
        "models": models,
        "textures": textures,
        "texture_fallbacks": [{"wanted_txd": t, "texture": k, "found_in": f}
                              for (t, k), f in sorted(store.fallbacks.items())],
        "failures": failures,
    }
    os.makedirs(os.path.join(out_dir, "scenes"), exist_ok=True)
    manifest_path = os.path.join(out_dir, "scenes", "academy.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=1)

    by_cat: dict[str, int] = {}
    for fl in failures:
        by_cat[fl["category"]] = by_cat.get(fl["category"], 0) + 1
    total_bytes = 0
    for dirpath, _d, files in os.walk(out_dir):
        for fn in files:
            total_bytes += os.path.getsize(os.path.join(dirpath, fn))
    wa = sum(m["winding_agree"] for m in models.values())
    wc = sum(m["winding_checked"] for m in models.values())
    report = {
        "world_img_entries": len(img),
        "world_img_census": img.census(),
        "ipb_files": len(scenes),
        "placements": len(placements),
        "placements_visible": sum(1 for p in placements if p["visible"]),
        "placements_with_gltf": sum(1 for p in placements if p["gltf"]),
        "models_wanted": len(wanted),
        "models_converted": sum(1 for m in models.values() if m["gltf"]),
        "shapes": sum(m["shapes"] for m in models.values()),
        "vertices": sum(m["vertices"] for m in models.values()),
        "triangles": sum(m["triangles"] for m in models.values()),
        "textures_exported": len(textures),
        "texture_fallbacks": len(store.fallbacks),
        "models_with_sanitized_nonfinite": sorted(k for k, m in models.items() if m["sanitized_nonfinite"]),
        "winding_agreement": (wa / wc) if wc else None,
        "failures_by_category": by_cat,
        "output_bytes": total_bytes,
        "wall_seconds": round(time.time() - t0, 2),
        "manifest": os.path.relpath(manifest_path, out_dir),
    }
    with open(os.path.join(out_dir, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    img.close()
    return report
