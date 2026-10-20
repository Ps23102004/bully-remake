"""Cheap source guards for gameplay invariants the headless smokes only cover in part (unreal/ C++ sources)."""
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "unreal/BullyRemake/Source/BullyRemake"


def _read(name):
    return (SRC / name).read_text()


def test_scripted_npc_brawl_does_not_alert_bystanders():
    # a mission fight must never rally the ambient students (AAmbientWalker::StartBrawl defaults to bAlert=true)
    assert "StartBrawl(false)" in _read("BullyScriptedNPC.cpp")


def test_mission_npcs_keep_player_sized_health():
    src = _read("AmbientWalker.cpp")
    assert "IsA<ABullyScriptedNPC>()" in src.split("C->MaxHealth = C->EnemyMaxHealth")[0][-200:]


def test_beaten_and_humiliation_ko_drop_a_running_ragdoll():
    src = _read("BullyCombatComponent.cpp")
    for state in ("Beaten", "KO"):
        assert f"AbortActiveRagdoll(GetOwner());\n\t\t\tSetState(EBullyCombatState::{state});" in src
