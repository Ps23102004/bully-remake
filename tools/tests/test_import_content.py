# OWNER: builder-B (ue)
"""tools/ue/import_content.py helpers that do not need a running editor (the `unreal` module is mocked)."""
import importlib
import json
import os
import sys
from unittest import mock

import pytest

UE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ue")


@pytest.fixture
def ic(monkeypatch):
    monkeypatch.setitem(sys.modules, "unreal", mock.MagicMock())
    monkeypatch.setenv("BR_NO_MAIN", "1")
    monkeypatch.syspath_prepend(UE_DIR)
    for m in ("import_content", "lighting"):
        sys.modules.pop(m, None)
    mod = importlib.import_module("import_content")
    yield mod
    for m in ("import_content", "lighting"):
        sys.modules.pop(m, None)


class Body:
    def __init__(self, **props):
        self.props = props

    def get_editor_property(self, k):
        return self.props.get(k)

    def set_editor_property(self, k, v):
        self.props[k] = v


def test_collision_is_complex_and_double_sided(ic):
    body = Body(collision_trace_flag=None, double_sided_geometry=False)
    sm = mock.MagicMock()
    sm.get_editor_property.return_value = body
    assert ic.ensure_complex_collision(sm) is True
    assert body.props["double_sided_geometry"] is True
    assert body.props["collision_trace_flag"] is ic.unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE
    assert ic.ensure_complex_collision(sm) is False           # idempotent: a re-run saves nothing


def test_load_collision_filters_scenes(ic, tmp_path):
    assert ic.load_collision(str(tmp_path), ["a"]) == ({}, {}, {})     # col step not run: nothing, a warning
    col = tmp_path / "collision"
    col.mkdir()
    (col / "nogo_a.gltf").write_text(json.dumps({"asset": {"version": "2.0"}}))
    (col / "index.json").write_text(json.dumps({
        "models": {"nogo_a": {"gltf": "collision/nogo_a.gltf"}, "nogo_b": {"gltf": "collision/nogo_b.gltf"}},
        "placements": [{"scene": "a", "index": 3, "model": "nogo_a"}, {"scene": "b", "index": 1, "model": "nogo_b"}]}))
    where, paths, gltfs = ic.load_collision(str(tmp_path), ["a"])
    assert where == {("a", 3): "nogo_a"} and list(gltfs) == ["nogo_a"]
    assert paths["nogo_a"] == os.path.join(str(tmp_path), "collision/nogo_a.gltf")


def test_select_door_links_skips_unimported_scenes(ic):
    links = [{"id": "out_lib", "partner": "lib_out", "scene": None}, {"id": "lib_out", "partner": "out_lib", "scene": "isc_lib"},
             {"id": "out_gym", "partner": "gym_out", "scene": None}, {"id": "gym_out", "partner": "out_gym", "scene": "isc_gym"},
             {"id": "orphan", "partner": "nowhere", "scene": None}]
    kept, skipped = ic.select_door_links(links, ["tschool", "isc_lib"])
    assert [d["id"] for d in kept] == ["out_lib", "lib_out"]
    assert skipped == ["out_gym", "gym_out", "orphan"]


def test_door_trigger_prefers_oriented_box(ic):
    old = {"trigger": {"center_cm": [1, 2, 3], "half_extent_cm": [4, 5, 6], "yaw": 33.0}}
    assert ic.door_trigger(old) == ([1, 2, 3], [4, 5, 6], 0.0)    # legacy AABB: no rotation
    new = {"trigger": {"center_cm": [0, 0, 0], "half_extent_cm": [9, 9, 9],
                       "oriented": {"center_cm": [1, 2, 3], "half_extent_cm": [4, 5, 6], "yaw": -110.4}}}
    assert ic.door_trigger(new) == ([1, 2, 3], [4, 5, 6], -110.4)
