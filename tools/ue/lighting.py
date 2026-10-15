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
import os
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
    _set(kc, "cast_shadows", True, misses, "key.")

    # ---- sky light + atmosphere
    sky = _spawn(eas, unreal.SkyLight, unreal.Vector(0, 0, 12000), None, "BR_SkyLight")
    skc = sky.get_component_by_class(unreal.SkyLightComponent)
    _set(skc, "mobility", unreal.ComponentMobility.MOVABLE, misses, "sky.")
    _set(skc, "real_time_capture", True, misses, "sky.")
    _set(skc, "intensity", float(v["sky"]["intensity"]), misses, "sky.")
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
    ppv.set_editor_property("settings", st)

    n_int = isolate_interiors(eas, icfg)
    res = {"variant": name, "cleared": cleared, "interior_actors_on_channel1": n_int,
           "exposure_ev100": ev, "property_misses": misses}
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
