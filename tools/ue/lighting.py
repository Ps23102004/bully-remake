# OWNER: builder-B5 (shot)
"""Lighting rig for /Game/Imported/Maps/Bullworth: golden-hour sun + sky + fog + locked exposure,
and the interior light isolation. Two entry points:

  build_rig(eas, variant_name)   called by import_content.build_level (fresh level)
  standalone (BR_LIGHTING_STANDALONE=1, run by tools/ue/run_lighting.sh): loads the saved map, replaces
      the rig with the chosen variant (BR_LIGHT_VARIANT, default from lighting_variants.json), re-applies the
      interior isolation, saves, and writes <BR_LIGHT_OUT>/lighting_applied.json.

Variant data: tools/ue/lighting_variants.json (documented there). Design rules:
  * Exposure is manual-locked: the unbound post-process volume sets auto exposure min = max = EV100.
  * Interior pockets (every scene whose name starts with isc_ / ischool, all far from the campus) are on
    lighting channel 1 ONLY. The sun (and the campus) stay on channel 0, so the sun cannot light or
    shadow-leak into interiors. A private directional "interior key" on channel 1 lights them to the
    same EV as the exterior, so a single global exposure serves both and nothing pops at door transitions.
"""
import json
import math
import os
import re
import traceback

import unreal

HERE = os.path.dirname(os.path.abspath(__file__))
VARIANTS_PATH = os.path.join(HERE, "lighting_variants.json")
MAP_PATH = "/Game/Imported/Maps/Bullworth"
FOLDER = "Lighting"


def _log(msg):
    unreal.log("BR_LIGHT " + msg)


def _warn(msg):
    unreal.log_warning("BR_LIGHT WARN " + msg)


def load_variants():
    with open(VARIANTS_PATH, encoding="utf-8") as f:
        return json.load(f)


def _set(obj, name, value, misses, label=""):
    try:
        obj.set_editor_property(name, value)
        return True
    except Exception as e:  # property missing / wrong type: report, keep going
        misses.append("%s%s: %s" % (label, name, str(e).splitlines()[0][:90]))
        return False


def _col(rgb):
    return unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0)


def _spawn(eas, cls, loc, rot, label):
    a = eas.spawn_actor_from_class(cls, loc, rot or unreal.Rotator(0, 0, 0))
    a.set_actor_label(label)
    a.set_folder_path(FOLDER)
    return a


def sun_rotator(elevation_deg, azimuth_deg):
    # light travels from the sun toward the scene: yaw = azimuth + 180, pitch = -elevation
    return unreal.Rotator(roll=0.0, pitch=-float(elevation_deg), yaw=float(azimuth_deg) + 180.0)


def _lighting_channels(c0, c1):
    ch = unreal.LightingChannels()
    ch.set_editor_property("channel0", c0)
    ch.set_editor_property("channel1", c1)
    ch.set_editor_property("channel2", False)
    return ch


def isolate_interiors(eas, cfg):
    """Interior static meshes -> lighting channel 1 only. Returns the number of actors changed."""
    prefixes = tuple(cfg["scene_prefixes"])
    ch = _lighting_channels(False, True)
    n = 0
    for a in eas.get_all_level_actors():
        if not isinstance(a, unreal.StaticMeshActor):
            continue
        scene = None
        for t in a.get_editor_property("tags"):
            s = str(t)
            if s.startswith("br_scene:"):
                scene = s[9:]
                break
        if scene is None or not scene.startswith(prefixes):
            continue
        a.static_mesh_component.set_editor_property("lighting_channels", ch)
        n += 1
    return n


LIGHT_ASSET_DIR = "/Game/Imported/Lighting"
MAT_INSTANCES = "/Game/Imported/Materials/Instances"
EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary


def _scene_of(a):
    for t in a.get_editor_property("tags"):
        s = str(t)
        if s.startswith("br_scene:"):
            return s[9:]
    return ""


def _model_of(a):
    for t in a.get_editor_property("tags"):
        s = str(t)
        if s.startswith("br_model:"):
            return s[9:]
    return ""


def _mesh_actors(eas):
    return [a for a in eas.get_all_level_actors() if isinstance(a, unreal.StaticMeshActor)]


def _tex_name(mi):
    try:
        t = MEL.get_material_instance_texture_parameter_value(mi, "BaseColor")
        return t.get_name() if t else ""
    except Exception:
        return ""


def _parent_name(mi):
    try:
        p = mi.get_editor_property("parent")
        return p.get_name() if p else ""
    except Exception:
        return ""


def _emissive_from_base(mi, gain):
    """Glow channel of the master material = emissive texture * GlowGain: feed it the base colour."""
    t = MEL.get_material_instance_texture_parameter_value(mi, "BaseColor")
    if not t:
        return False
    MEL.set_material_instance_texture_parameter_value(mi, "Glow", t)
    MEL.set_material_instance_scalar_parameter_value(mi, "GlowGain", float(gain))
    return True


def apply_foliage_glow(eas, cfg, misses):
    """Backlit leaf cards: alpha-masked cards lit by a sun behind them have their camera-facing side in shadow
    (near-black cut-outs). Emissive = albedo * gain emulates leaf transmission (amber glow, as refs/07 and 10)
    without touching the shared master materials. Applies only to masked leaf/branch/conifer/vine cards."""
    if not cfg:
        return {"foliage_mis": 0}
    rx = re.compile(cfg.get("texture_regex", "branch|crossdecid|crossever|evergreen|tree01|tree_01|climbvine|ivy|vine|bush|fern|plant|grass"), re.I)
    skip = re.compile(cfg.get("skip_regex", "fence|rail|chain|wire|glass|net|bark"), re.I)
    seen = {}
    for a in _mesh_actors(eas):
        for m in a.static_mesh_component.get_materials():
            if m is None or not isinstance(m, unreal.MaterialInstanceConstant):
                continue
            pa = m.get_path_name()
            if pa in seen:
                continue
            par = _parent_name(m)
            tn = _tex_name(m)
            seen[pa] = (m, par, tn)
    n = 0
    for pa, (m, par, tn) in seen.items():
        if not par.startswith("M_Bully_Masked") or not tn or skip.search(tn) or not rx.search(tn):
            continue
        if _emissive_from_base(m, cfg["gain"]):
            MEL.update_material_instance(m)
            EAL.save_loaded_asset(m, only_if_is_dirty=False)
            n += 1
    _log("foliage glow: %d material instances gain %s" % (n, cfg["gain"]))
    return {"foliage_mis": n}


def apply_interior_materials(eas, icfg, misses):
    """Interior look: day-window glass glows (the *Night twins are hidden: they sit coplanar with the day
    panes and z-fight to flat grey), the lobby tile floor gets a glossy roughness."""
    res = {}
    wcfg = icfg.get("window_glow")
    prefixes = tuple(icfg["scene_prefixes"])
    seen, floor_mis = {}, {}
    hidden = 0
    night_rx = re.compile(icfg.get("hide_night_regex", "win.*night"), re.I)
    for a in _mesh_actors(eas):
        if not _scene_of(a).startswith(prefixes) and "Win" not in _model_of(a):
            continue
        model = _model_of(a)
        if night_rx.search(model):
            a.set_actor_hidden_in_game(True)
            hidden += 1
            continue
        for m in a.static_mesh_component.get_materials():
            if m is None or not isinstance(m, unreal.MaterialInstanceConstant):
                continue
            pa = m.get_path_name()
            if wcfg and re.search(wcfg["texture_regex"], _tex_name(m), re.I):
                seen[pa] = m
            if icfg.get("floor") and model in icfg["floor"]["models"] and re.search(icfg["floor"]["texture_regex"], _tex_name(m), re.I):
                floor_mis[pa] = m
    for m in seen.values():
        if _emissive_from_base(m, wcfg["gain"]):
            MEL.update_material_instance(m)
            EAL.save_loaded_asset(m, only_if_is_dirty=False)
    for m in floor_mis.values():
        f = icfg["floor"]
        MEL.set_material_instance_scalar_parameter_value(m, "RoughnessMax", float(f["roughness"]))
        MEL.set_material_instance_scalar_parameter_value(m, "SpecularBase", float(f["specular"]))
        MEL.update_material_instance(m)
        EAL.save_loaded_asset(m, only_if_is_dirty=False)
    res = {"window_mis": len(seen), "floor_mis": len(floor_mis), "night_windows_hidden": hidden}
    _log("interior materials: %s" % json.dumps(res))
    return res


def apply_shot_hides(eas, rules):
    """Importer-artifact actors hidden for the hero shots (hidden in game: no render, no shadow, no Lumen).
    rule: {"model": exact model name, "near_cm": [x, y, z, radius]} (both optional but one required)."""
    n = 0
    for a in _mesh_actors(eas):
        model = _model_of(a)
        o = a.get_actor_location()
        for r in rules:
            if "model" in r and model != r["model"]:
                continue
            if "label_regex" in r and not re.search(r["label_regex"], a.get_actor_label()):
                continue
            if "near_cm" in r:
                x, y, z, rad = r["near_cm"]
                if (o.x - x) ** 2 + (o.y - y) ** 2 + (o.z - z) ** 2 > rad * rad:
                    continue
            a.set_actor_hidden_in_game(True)
            n += 1
            break
    _log("shot hides: %d actors" % n)
    return n


def build_clouds(eas, ccfg, misses):
    """Warm volumetric cloud layer. Material instance of the engine's simple volumetric cloud material."""
    if not ccfg:
        return None
    name = "MI_BullyClouds"
    path = "%s/%s" % (LIGHT_ASSET_DIR, name)
    at = unreal.AssetToolsHelpers.get_asset_tools()
    if EAL.does_asset_exist(path):
        mi = EAL.load_asset(path)
    else:
        mi = at.create_asset(name, LIGHT_ASSET_DIR, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    parent = unreal.load_asset("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst")   # carries the engine noise/pattern texture overrides
    MEL.set_material_instance_parent(mi, parent)
    for k, v in ccfg.get("scalars", {}).items():
        MEL.set_material_instance_scalar_parameter_value(mi, k, float(v))
    for k, v in ccfg.get("vectors", {}).items():
        MEL.set_material_instance_vector_parameter_value(mi, k, unreal.LinearColor(*v))
    MEL.update_material_instance(mi)
    EAL.save_loaded_asset(mi, only_if_is_dirty=False)
    cl = _spawn(eas, unreal.VolumetricCloud, unreal.Vector(0, 0, 0), None, "BR_VolumetricClouds")
    cc = cl.get_component_by_class(unreal.VolumetricCloudComponent)
    _set(cc, "material", mi, misses, "cloud.")
    for k in ("layer_bottom_altitude", "layer_height", "tracing_start_max_distance", "tracing_max_distance",
              "view_sample_count_scale", "reflection_view_sample_count_scale_value", "shadow_view_sample_count_scale",
              "stop_tracing_transmittance_threshold", "sky_light_cloud_bottom_occlusion", "ground_albedo"):
        if k in ccfg:
            v = ccfg[k]
            if k == "ground_albedo":
                v = unreal.Color(int(v[0] * 255), int(v[1] * 255), int(v[2] * 255), 255)
            _set(cc, k, v if k == "ground_albedo" else float(v), misses, "cloud.")
    return cl


def build_birds(eas, bcfg, misses):
    """A loose flock of tiny dark 'V' silhouettes (two thin flattened cubes each) in the west sky, like the crows of
    refs/02 and 06. Static, no shadows, no GI contribution beyond a black silhouette."""
    if not bcfg:
        return 0
    import random
    rnd = random.Random(int(bcfg.get("seed", 7)))
    cube = unreal.load_asset("/Engine/BasicShapes/Cube")
    name = "MI_BullyBird"
    path = "%s/%s" % (LIGHT_ASSET_DIR, name)
    at = unreal.AssetToolsHelpers.get_asset_tools()
    mi = EAL.load_asset(path) if EAL.does_asset_exist(path) else at.create_asset(
        name, LIGHT_ASSET_DIR, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    MEL.set_material_instance_parent(mi, unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial"))
    MEL.set_material_instance_vector_parameter_value(mi, "Color", unreal.LinearColor(0.012, 0.01, 0.01, 1.0))
    MEL.update_material_instance(mi)
    EAL.save_loaded_asset(mi, only_if_is_dirty=False)
    cx, cy, cz = bcfg["center_cm"]
    sx, sy, sz = bcfg["spread_cm"]
    wing = float(bcfg.get("wing_cm", 70.0))
    n = 0
    for i in range(int(bcfg["count"])):
        bx = cx + rnd.uniform(-sx, sx)
        by = cy + rnd.uniform(-sy, sy)
        bz = cz + rnd.uniform(-sz, sz)
        yaw = float(bcfg.get("heading_yaw_deg", 200.0)) + rnd.uniform(-25, 25)
        k = rnd.uniform(0.7, 1.3)
        flap = rnd.uniform(8, 32)   # wing dihedral/flap angle, deg
        for side in (-1, 1):
            ay = math.radians(yaw + 90.0)
            off = side * wing * 0.5 * k
            loc = unreal.Vector(bx + math.cos(ay) * off, by + math.sin(ay) * off, bz + math.sin(math.radians(flap)) * abs(off) * 0.5)
            a = eas.spawn_actor_from_class(unreal.StaticMeshActor, loc, unreal.Rotator(roll=side * flap, pitch=0.0, yaw=yaw))
            a.set_actor_label("BR_Bird_%02d_%s" % (i, "L" if side < 0 else "R"))
            a.set_folder_path(FOLDER)
            comp = a.static_mesh_component
            comp.set_static_mesh(cube)
            comp.set_material(0, mi)
            comp.set_editor_property("cast_shadow", False)
            a.set_actor_scale3d(unreal.Vector(0.45 * k, wing * k / 200.0, 0.02))   # cube is 100 cm: chord 45 cm, half-span wing/2
            n += 1
    return n


def build_interior_lights(eas, lights, misses):
    """Motivated interior lighting on lighting channel 1 only: rect lights from the window wall / sides,
    point-light fills. All movable (project is Lumen-only, no baked lighting)."""
    out = []
    for i, L in enumerate(lights):
        kind = L["type"]
        cls = unreal.RectLight if kind == "rect" else unreal.PointLight
        a = _spawn(eas, cls, unreal.Vector(*L["loc_cm"]), unreal.Rotator(roll=0.0, pitch=L.get("pitch", 0.0), yaw=L.get("yaw", 0.0)),
                   "BR_IntLight_%02d_%s" % (i, L.get("name", kind)))
        c = a.get_component_by_class(unreal.RectLightComponent if kind == "rect" else unreal.PointLightComponent)
        _set(c, "mobility", unreal.ComponentMobility.MOVABLE, misses, "ilight.")
        _set(c, "intensity_units", unreal.LightUnits.LUMENS, misses, "ilight.")
        _set(c, "intensity", float(L["lumens"]), misses, "ilight.")
        _set(c, "use_temperature", True, misses, "ilight.")
        _set(c, "temperature", float(L.get("temperature_k", 4800)), misses, "ilight.")
        _set(c, "attenuation_radius", float(L.get("radius_cm", 4000)), misses, "ilight.")
        _set(c, "lighting_channels", _lighting_channels(False, True), misses, "ilight.")
        _set(c, "cast_shadows", bool(L.get("shadows", True)), misses, "ilight.")
        if kind == "rect":
            _set(c, "source_width", float(L.get("width_cm", 300)), misses, "ilight.")
            _set(c, "source_height", float(L.get("height_cm", 300)), misses, "ilight.")
            _set(c, "barn_door_angle", float(L.get("barn_door_angle", 88.0)), misses, "ilight.")
        else:
            _set(c, "source_radius", float(L.get("source_radius_cm", 40)), misses, "ilight.")
        out.append(a)
    return len(out)


def clear_rig(eas):
    old = [a for a in eas.get_all_level_actors() if str(a.get_folder_path()).startswith(FOLDER)]
    if old:
        eas.destroy_actors(old)
    return len(old)


def build_rig(eas, variant_name=None):
    data = load_variants()
    name = variant_name or data["default_variant"]
    v = data["variants"][name]
    icfg = data["interior"]
    misses = []
    cleared = clear_rig(eas)

    # ---- sun
    s = v["sun"]
    sun = _spawn(eas, unreal.DirectionalLight, unreal.Vector(0, 0, 10000),
                 sun_rotator(s["elevation_deg"], s["azimuth_deg"]), "BR_Sun")
    sc = sun.get_component_by_class(unreal.DirectionalLightComponent)
    _set(sc, "mobility", unreal.ComponentMobility.MOVABLE, misses, "sun.")
    _set(sc, "intensity", float(s["intensity_lux"]), misses, "sun.")
    _set(sc, "atmosphere_sun_light", True, misses, "sun.")
    _set(sc, "use_temperature", True, misses, "sun.")
    _set(sc, "temperature", float(s["temperature_k"]), misses, "sun.")
    _set(sc, "light_source_angle", float(s.get("source_angle_deg", 0.5357)), misses, "sun.")
    _set(sc, "cast_shadows", True, misses, "sun.")
    _set(sc, "lighting_channels", _lighting_channels(True, False), misses, "sun.")

    # ---- interior key (channel 1 only, not an atmosphere sun)
    key = _spawn(eas, unreal.DirectionalLight, unreal.Vector(0, 0, 11000),
                 unreal.Rotator(roll=0.0, pitch=icfg["key_pitch_deg"], yaw=icfg["key_yaw_deg"]), "BR_InteriorKey")
    kc = key.get_component_by_class(unreal.DirectionalLightComponent)
    _set(kc, "mobility", unreal.ComponentMobility.MOVABLE, misses, "key.")
    _set(kc, "intensity", float(icfg["key_lux"]), misses, "key.")
    _set(kc, "atmosphere_sun_light", False, misses, "key.")
    _set(kc, "use_temperature", True, misses, "key.")
    _set(kc, "temperature", float(icfg["key_temperature_k"]), misses, "key.")
    _set(kc, "lighting_channels", _lighting_channels(False, True), misses, "key.")
    _set(kc, "light_source_angle", float(icfg.get("key_source_angle_deg", 10.0)), misses, "key.")
    _set(kc, "cast_shadows", bool(icfg.get("key_cast_shadows", True)), misses, "key.")

    # ---- sky light + atmosphere
    sky = _spawn(eas, unreal.SkyLight, unreal.Vector(0, 0, 12000), None, "BR_SkyLight")
    skc = sky.get_component_by_class(unreal.SkyLightComponent)
    _set(skc, "mobility", unreal.ComponentMobility.MOVABLE, misses, "sky.")
    _set(skc, "real_time_capture", True, misses, "sky.")
    _set(skc, "intensity", float(v["sky"]["intensity"]), misses, "sky.")
    lh = v["sky"].get("lower_hemisphere_rgb")
    if lh:   # warm ground bounce instead of a black lower hemisphere: lifts the shade side of everything
        _set(skc, "lower_hemisphere_is_black", False, misses, "sky.")
        _set(skc, "lower_hemisphere_color", _col(lh), misses, "sky.")
    if "tint_rgb" in v["sky"]:
        _set(skc, "light_color", unreal.Color(*[int(round(c * 255)) for c in v["sky"]["tint_rgb"]], 255), misses, "sky.")
    atm = _spawn(eas, unreal.SkyAtmosphere, unreal.Vector(0, 0, 0), None, "BR_SkyAtmosphere")
    ac = atm.get_component_by_class(unreal.SkyAtmosphereComponent)
    at = v["atmosphere"]
    g = at["ground_albedo_rgb"]
    _set(ac, "ground_albedo", unreal.Color(int(g[0] * 255), int(g[1] * 255), int(g[2] * 255), 255), misses, "atm.")
    _set(ac, "aerial_pespective_view_distance_scale", float(at["aerial_perspective_distance_scale"]), misses, "atm.")

    # ---- height fog
    f = v["fog"]
    fog = _spawn(eas, unreal.ExponentialHeightFog, unreal.Vector(0, 0, 0), None, "BR_HeightFog")
    fc = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
    _set(fc, "fog_density", float(f["density"]), misses, "fog.")
    _set(fc, "fog_height_falloff", float(f["height_falloff"]), misses, "fog.")
    _set(fc, "fog_inscattering_luminance", _col(f["inscatter_rgb"]), misses, "fog.")
    _set(fc, "start_distance", float(f["start_distance_cm"]), misses, "fog.")
    _set(fc, "fog_max_opacity", float(f["max_opacity"]), misses, "fog.")
    _set(fc, "enable_volumetric_fog", bool(f["volumetric"]), misses, "fog.")
    _set(fc, "directional_inscattering_exponent", float(f["directional_inscatter_exponent"]), misses, "fog.")
    _set(fc, "directional_inscattering_luminance", _col(f["directional_inscatter_rgb"]), misses, "fog.")

    # ---- global post-process: manual-locked exposure, white balance, mild grade
    ppv = _spawn(eas, unreal.PostProcessVolume, unreal.Vector(0, 0, 0), None, "BR_PostProcess")
    ppv.set_editor_property("unbound", True)
    st = ppv.get_editor_property("settings")
    ev = float(v["exposure_ev100"])
    for k, val in (("auto_exposure_method", unreal.AutoExposureMethod.AEM_BASIC),
                   ("auto_exposure_min_brightness", ev), ("auto_exposure_max_brightness", ev),
                   ("auto_exposure_bias", 0.0),
                   ("white_temp", float(v["white_balance"]["temp_k"])), ("white_tint", float(v["white_balance"]["tint"])),
                   ("bloom_intensity", float(v["post"]["bloom_intensity"])),
                   ("color_saturation", unreal.Vector4(v["post"]["saturation"], v["post"]["saturation"], v["post"]["saturation"], 1.0)),
                   ("color_contrast", unreal.Vector4(v["post"]["contrast"], v["post"]["contrast"], v["post"]["contrast"], 1.0))):
        if _set(st, "override_" + k, True, misses, "ppv."):
            _set(st, k, val, misses, "ppv.")
    for k, val in v["post"].get("extra", {}).items():   # optional extra post-process settings (film toe, grain...)
        if _set(st, "override_" + k, True, misses, "ppv."):
            _set(st, k, val, misses, "ppv.")
    ppv.set_editor_property("settings", st)

    n_int = isolate_interiors(eas, icfg)
    extra = {}
    cl = build_clouds(eas, v.get("clouds"), misses)
    extra["clouds"] = bool(cl)
    extra["bird_wings"] = build_birds(eas, v.get("birds"), misses)
    extra["interior_lights"] = build_interior_lights(eas, icfg.get("lights", []), misses)
    extra.update(apply_foliage_glow(eas, v.get("foliage_glow"), misses))
    extra["interior_materials"] = apply_interior_materials(eas, icfg, misses)
    extra["shot_hides"] = apply_shot_hides(eas, data.get("shot_hides", []))
    res = {"variant": name, "cleared": cleared, "interior_actors_on_channel1": n_int,
           "exposure_ev100": ev, "extra": extra, "property_misses": misses}
    _log("rig built: %s" % json.dumps(res))
    for m in misses:
        _warn("property not set: " + m)
    return res


def standalone():
    name = os.environ.get("BR_LIGHT_VARIANT") or None
    out = os.environ.get("BR_LIGHT_OUT")
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    ok = False
    try:
        if not les.load_level(MAP_PATH):
            raise RuntimeError("load_level failed: " + MAP_PATH)
        res = build_rig(eas, name)
        if not les.save_current_level():
            raise RuntimeError("save_current_level failed")
        ok = True
        if out:
            os.makedirs(out, exist_ok=True)
            with open(os.path.join(out, "lighting_applied.json"), "w") as fh:
                json.dump(res, fh, indent=1)
    except Exception as e:
        unreal.log_error("BR_LIGHT ERROR %s\n%s" % (e, traceback.format_exc()))
    unreal.log("BR_LIGHT_DONE %s" % ("OK" if ok else "FAILED"))


if os.environ.get("BR_LIGHTING_STANDALONE") == "1":
    standalone()
