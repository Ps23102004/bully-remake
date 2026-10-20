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
import re
import time

from . import coords
from .binreader import FormatError
from .ae import detect_ae
from .convert import TextureStore, convert_model
from .ide import ModelDefs
from .img import ImgArchive
from .ipb import parse_ipb, placement_quat_game, placement_rotation
from .nft import safe_name

ACADEMY = ["tschool", "ischool", "isc_art", "isc_audt", "isc_auto", "isc_bio", "isc_boil", "isc_chem",
           "isc_clas", "isc_dorm", "isc_Gdrm", "isc_lib", "isc_offi", "isc_pool", "isc_prep", "isc_staf",
           "zone_school1", "zone_school2", "zone_school3", "zone_school4", "zone_school5", "zone_school6"]
# Outdoor town districts (Config/world.dat IPL roster): business strip, rich area, industrial, carnival,
# junkyard, BMX park, go-kart track. tGlobal is the always-loaded relief/water, needed by every outdoor area.
TOWN_OUTDOOR = ["tGlobal", "tbusines", "trich", "tindust", "tcarni", "tjyard", "tBMX", "tgokart"]

# Street-dressing tiles of the rest of the town (lamps, power poles, trees, billboards, benches, props). The PC
# roster and the AE both ship them in World.img next to zone_school1..6 (docs/re-notes/ae-town.md); they are
# not listed in world.dat (the game streams them by grid cell), so they are named here. ftest/ttest are
# developer test scenes and are never imported.
TOWN_ZONES = ([f"zone_busines{i}" for i in range(1, 9)] + ["zone_carn1"] + [f"zone_indust{i}" for i in range(1, 7)]
              + [f"zone_rich{i}" for i in range(1, 12)])

# Standalone interiors outside the academy: shops, gym, asylum, observatory, carnival stalls, clique stores, races.
INTERIORS = ["igrocery", "iboxing", "ibkshop", "icomshp", "icloth_p", "icloth_r", "itenn", "ifunhous",
             "iasylum", "ibarber", "ichem", "iobserv", "iballtos", "isouvnir", "ishoot", "itrailer",
             "isalon", "iMGRaceA", "iMGRaceB", "iMGRaceC", "iware", "ipbarb", "ifreaks",
             "iDropS", "iPrepS", "iJockS", "iGrsrS"]

PRESETS = {
    "academy": ACADEMY,
    "core": ["tschool", "ischool"],
    "town": ACADEMY + TOWN_OUTDOOR + TOWN_ZONES,          # academy + open-world town (keeps the academy in the manifest)
    "all": ACADEMY + TOWN_OUTDOOR + TOWN_ZONES + INTERIORS,  # the whole world
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


# Seasonal props the game only shows in the winter chapter (the importer's season is autumn: ae.py WINTER_SUFFIXES).
# Measured on the town scenes: dl_bu_winteronly02, dl_rc_winteronly02, dl_rcxmaspx, buxmastreepx, fgrd_sc1d_cice.
WINTER_ONLY = re.compile(r"winteronly|xmas|cice$")


def ae_hidden_models(names) -> dict:
    """AE models that must not draw -> reason (docs/re-notes/ae-materials.md "Hidden models"; winter_only: ae-town.md).
    px*: physics/hit proxies (every px*.msh in data_1.zip is a ~2 m box in the one placeholder
    material mat_sc31_medyellow_pxgbed; the real prop is a separate coincident placement).
    Night variants: "<x>night<k>" / "<x>n<k>" placed beside "<x>day<k>" / "<x>d<k>" (window/screen
    lighting states drawn at the same spot); the day one is kept."""
    low = {n.lower() for n in names}
    out = {}
    for n in names:
        s = n.lower()
        if s.startswith("px"):
            out[n] = "physics_proxy"
            continue
        if WINTER_ONLY.search(s):
            out[n] = "winter_only"
            continue
        for a, b in (("night", "day"), ("n", "d")):
            day = re.sub(a + r"(\d*)$", b + r"\1", s)
            if day != s and day in low and (a == "night" or "screen" in s or "win" in s):
                out[n] = "night_variant"
                break
    return out


def hide_duplicate_placements(placements: list) -> int:
    """Hide exact repeats (same scene, model, position, rotation, scale to 1 mm): coplanar z-fighting copies.
    The first one stays. Measured: 62 in isc_clas (kiddesk 15, kidchair 10, ...) and none elsewhere (ae-town.md)."""
    seen, n = set(), 0
    for p in placements:
        g = p["game"]
        k = (p["scene"], p["model"], tuple(round(v, 3) for v in g["position"]),
             tuple(round(v, 3) for v in g["rotation_xyzw_stored"]), tuple(round(v, 3) for v in g["scale"]))
        if k in seen:
            if p["visible"]:
                p["visible"], p["gltf"], p["hidden_reason"] = False, None, "duplicate_placement"
                n += 1
        else:
            seen.add(k)
    return n


# AE water layers (docs/re-notes/ae-town.md "Water"): every mat_wtr_* texture is dark (max 41..74 of 255) with alpha 255,
# i.e. a layer the game blends over its own sea/river surface. Converted as opaque they draw the rivers pitch black.
# base -> a tinted translucent surface (no texture); ripple/shore/caustics -> additive; waterbase (river bed) stays opaque.
# Tint and opacity are an ESTIMATE (no reference render of the original water).
WATER_TINT, WATER_ALPHA = [0.06, 0.22, 0.27], 0.78


def patch_water_materials(out_dir: str, models: dict) -> int:
    """Rewrite the materials of converted glTFs whose names are mat_wtr_base*/ripple*/shore*/caustics*. Returns files changed."""
    changed = 0
    for info in models.values():
        if not info.get("gltf"):
            continue
        path = os.path.join(out_dir, info["gltf"])
        with open(path) as f:
            doc = json.load(f)
        dirty = False
        for m in doc.get("materials", []):
            n = m.get("name", "").lower()
            ex = m.setdefault("extras", {})
            if n.startswith("mat_wtr_base"):
                m["pbrMetallicRoughness"].pop("baseColorTexture", None)
                m["pbrMetallicRoughness"]["baseColorFactor"] = WATER_TINT + [WATER_ALPHA]   # for plain glTF viewers
                ex.get("textures", {}).pop("base", None)
                m["alphaMode"], ex["blend_mode"] = "BLEND", "translucent"
                ex.setdefault("gamebryo", {})["material"] = {"diffuse": WATER_TINT, "alpha": WATER_ALPHA,
                                                             "specular": [0.35, 0.35, 0.35]}
            elif n.startswith(("mat_wtr_ripple", "mat_wtr_shore", "mat_wtr_caustics")):
                m["alphaMode"], ex["blend_mode"] = "BLEND", "additive"
            else:
                continue
            ex["water_layer"] = n.split("_")[2]
            dirty = True
        if dirty:
            with open(path, "w") as f:
                json.dump(doc, f)
            changed += 1
    return changed


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


# The three arcade-race mini-game maps are authored at the world origin, stacked in the sky above / on the school
# playing field (3D overlap with tschool + zone_school2..6 + tGlobal, and with each other; measured, ae-town.md). The
# game streams one of them in only while its race runs, so a resident map must not draw them.
MINIGAME_OVERLAYS = ("imgracea", "imgraceb", "imgracec")


def build_world(install: str, out_dir: str, ipb_stems: list, entity_space: bool = True,
                convert_invisible: bool = False, log=print, ae_data: str | None = None,
                keep_overlays: bool = False) -> dict:
    t0 = time.time()
    # Anniversary Edition: placements come from the extracted data_0 tree, models/textures from
    # data_1.zip (.msh/.tex) instead of World.img (.nif/.nft). The PC path below is unchanged.
    ae_arc = None
    if ae_data or detect_ae(install):
        # lazy: the AE decoders need numpy; the PC path must keep working without it
        from .ae import AeArchive
        from .convert_ae import AeTextureStore, convert_msh
        ae_arc = AeArchive(ae_data)
    os.makedirs(out_dir, exist_ok=True)
    img = ImgArchive(os.path.join(install, "Stream", "World.img"))
    defs = ModelDefs.load_install(install)
    store = AeTextureStore(ae_arc, out_dir) if ae_arc else TextureStore(img, out_dir)
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
                 "resident": not (entry.stem.lower() in MINIGAME_OVERLAYS and not keep_overlays),
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
            if ae_arc:
                found = ae_arc.find(name, "msh")
                canonical = os.path.splitext(os.path.basename(found))[0] if found else name
            else:
                nif_entry = img.find(name + ".nif")
                found = nif_entry
                canonical = nif_entry.stem if nif_entry else name
            visible = not is_invisible_marker(canonical)
            overlay = entry.stem.lower() in MINIGAME_OVERLAYS and not keep_overlays
            visible = visible and not overlay
            if found is None:
                failures.append({"category": "msh_missing" if ae_arc else "nif_missing", "item": name,
                                 "error": "no .msh in data_1.zip" if ae_arc else "no .nif in World.img"})
            elif visible or convert_invisible:
                wanted.setdefault(canonical, (ide.txd if ide else None, ide.id if ide else None))
            gltf = f"models/{safe_name(canonical)}.gltf" if found and (visible or convert_invisible) else None
            placements.append(_placement_record(entry.stem, p, canonical, gltf, visible))
            if overlay:
                placements[-1]["hidden_reason"] = "minigame_overlay"
            for k in range(3):
                lo[k] = min(lo[k], p.position[k])
                hi[k] = max(hi[k], p.position[k])
        scene["bounds_game_positions"] = [lo, hi]
        scenes.append(scene)
        log(f"ipb {entry.name}: {len(ipb.placements)} placements {ipb.section_counts}")

    if ae_arc:
        hidden = ae_hidden_models({p["model"] for p in placements})
        for p in placements:
            if p["model"] in hidden:
                p["visible"], p["gltf"], p["hidden_reason"] = False, None, hidden[p["model"]]
                wanted.pop(p["model"], None)

    n_dup = hide_duplicate_placements(placements)
    log(f"converting {len(wanted)} distinct models ({n_dup} duplicate placements hidden) ...")
    for i, (name, (txd, ide_id)) in enumerate(sorted(wanted.items(), key=lambda kv: kv[0].lower())):
        try:
            if ae_arc:
                r = convert_msh(name, ae_arc.read(name, "msh"), ae_arc, store, out_dir)
            else:
                e = img.find(name + ".nif")
                r = convert_model(e.stem, img.read_entry(e), txd, store, out_dir, entity_space=entity_space)
        except FormatError as ex:
            failures.append({"category": "msh_convert" if ae_arc else "nif_convert", "item": name, "error": str(ex)})
            continue
        if r.gltf is None and ae_arc:
            failures.append({"category": "msh_empty_in_source" if r.empty_source else "msh_no_visible_geometry",
                             "item": name, "error": "; ".join(r.warnings)})
        elif r.gltf is None:
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
    n_water = patch_water_materials(out_dir, models) if ae_arc else 0
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
    # water the AE draws at run time: the *_water models that are empty in the shipped data (sb1_water x21, pg_water09/10,
    # dl_water08_a, ...) mark where; the UE stage should put a water plane at their z (all 0.0 for the town, ae-town.md)
    empty = {f["item"] for f in failures if f["category"].endswith("empty_in_source")}
    water_markers = [{"scene": p["scene"], "model": p["model"], "position": p["game"]["position"]}
                     for p in placements if p["model"] in empty and "water" in p["model"].lower()]
    manifest = {
        "format": MANIFEST_FORMAT,
        "generated_by": "tools/importers world",
        "coordinate_systems": COORDINATE_DOC,
        "units": {"game": "metre", "gltf": "metre", "ue": "centimetre"},
        "scenes": scenes,
        "water_markers": water_markers,
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
    hidden_counts: dict[str, int] = {}
    for p in placements:
        if p.get("hidden_reason"):
            hidden_counts[p["hidden_reason"]] = hidden_counts.get(p["hidden_reason"], 0) + 1
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
        "duplicate_placements_hidden": n_dup,
        "hidden_by_reason": hidden_counts,
        "water_models_patched": n_water,
        "output_bytes": total_bytes,
        "wall_seconds": round(time.time() - t0, 2),
        "manifest": os.path.relpath(manifest_path, out_dir),
    }
    if ae_arc:
        report["edition"] = "anniversary"
        report["ae_data"] = ae_arc.path
        ae_arc.close()
    with open(os.path.join(out_dir, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    img.close()
    return report
