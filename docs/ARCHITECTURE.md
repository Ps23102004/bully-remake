# Architecture — Phase 0

## Pipeline

```
Owner's installed game (read-only)                     this repo (original code only)
┌──────────────────────────────┐    ┌────────────────┐    ┌──────────────────────────┐
│ Bully Scholarship Edition/   │ →  │ tools/importers │ →  │ imported/ (git-ignored)  │
│   Stream/World.img + .dir    │    │  Python, pytest │    │  glTF + PNG + JSON       │
│   Models/*.nif, TXD/*.nft    │    └────────────────┘    └──────────────────────────┘
│   DAT/, Data/, Anim/, audio/ │                                      ↓
└──────────────────────────────┘    ┌───────────────────────────────────────────────┐
                                    │ unreal/BullyRemake — UE 5.8 project           │
                                    │  tools/ue/import_content.py (headless, UE py) │
                                    │  → Content/Imported/** (git-ignored)          │
                                    └───────────────────────────────────────────────┘
```

1. **tools/importers** (plain Python 3, no engine dependency, pytest-tested): parses the game's own formats — IMG/DIR archives, NIF geometry, NFT textures, world placement data — and writes open intermediates (glTF 2.0 for geometry/scenes, PNG for textures, JSON manifests for placements/metadata) into `imported/`. Never modifies the game install; never writes inside the repo tree except `imported/`.
2. **UE import step**: a headless UnrealEditor-Cmd Python script turns the intermediates into `.uasset`s under `Content/Imported/` and assembles the Bullworth level. Both `imported/` and `Content/Imported/` are git-ignored and CI-blocked: a fresh clone + an owned copy of the game reproduces everything.
3. **Game module** (`Source/BullyRemake`): original C++ — character movement, camera, door transitions — written against measurements of the original game, with the decompiled Lua and bully-re as behavior references (cited in `docs/re-notes/`, never copied).

## Format authority

`docs/re-notes/format-survey.md` is the living format reference (sourced from bully-re, the modding tools, and our own Ghidra work on the owner's binary). Importers cite it; unresolved fields are marked OPEN there.

## Agent file ownership (Phase 0)

| Area | Owner |
|---|---|
| Repo root, docs/, unreal project skeleton, HANDOFF.md, integration | orchestrator (main session) |
| `tools/importers/**`, `tools/tests/**` | builder A (importer pipeline) |
| `Source/BullyRemake/**`, `Content/` (non-imported), `tools/ue/**` | builder B (UE game module) |
| ambient life + audio pass | builder C (after milestones 1–3) |

No cross-edits without coordination through the orchestrator. Every file a builder creates starts with an `OWNER:` comment where the format allows one.
