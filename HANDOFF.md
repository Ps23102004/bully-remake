# HANDOFF — bully-remake

**Updated:** 2026-10-15 · **Branch:** `phase0` (checkout `~/bully-remake`) · **Machine:** midirstudio (M3 Ultra Mac Studio)

## Verified state

- Repo `oh-ashen-one/bully-remake` created public 2026-10-15, seeded on `phase0` (README, CREDITS, settings, CI blocklist, LICENSE).
- **Game install verified:** *Bully: Scholarship Edition* (PC, Steam via CrossOver), 5.0 GB, complete data tree at:
  `/Users/midir/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Bully Scholarship Edition`
  Key dirs: `Act/ Anim/ audio/ Coll/ Cuts/ DAT/ Data/ Graphics/ Models/ Objects/ Scripts/ Stream/ TXD/ ShaderBinaries/` plus `Bully.exe` (binary for Ghidra RE).
- Kickoff brief: `/Users/midir/Downloads/BULLY-KICKOFF-README.md` (planning session 2026-10-15).

## Phase 0 milestones (status)

1. Assets in — **importers DONE, merged to phase0** (`tools/importers`: IMG/NIF/NFT/IPB + `world` command; 932/939 academy models → glTF, 48 tests; run `python3 -m tools.importers world` with BULLY_INSTALL set, 25 s). UE side **in progress**: opus-builder on branch `phase0-ue` (worktree `~/bully-remake-wt/ue`) writing `tools/ue/import_content.py` + Bullworth level + front-gate Lumen screenshots → `~/bully-remake-evidence/m1/`
2. Interior in — not started
3. Jimmy moves — not started
4. Signs of life — not started
5. The shot — not started
6. Proof capture — not started

## Rules in force

- `phase0` branch only; draft PR; **no merge to main without owner**.
- `imported/` is git-ignored and CI-blocked; game content never enters the repo.
- GPU slot protocol `~/.cache/gpu-slot`; cap 2 renderer-bearing engines; Unreal work routes through the `unreal-engine` brain skill; Epic native MCP only on a session-owned editor instance.

## Next steps

- Merge `phase0-ue` when the UE import + level + screenshots land; review screenshots.
- Milestone 2 (doors) + 3 (Jimmy): locomotion numbers live in Act.img action trees (see `docs/re-notes/locomotion-spec.md` — CAT_TO_MACT decode verified; param-name mapping OPEN).
- Original-game reference footage (milestones 3/5/6): Bully runs via CrossOver; capture needs a desktop-visible window + GPU slot — schedule when owner is not using the machine/streaming.

## Blockers / notes

- Ghidra is NOT installed on this machine (kickoff assumed a Ghidra MCP). When needed (anim formats, locomotion params, loader confirmation): `brew install --cask ghidra` and prefer headless `analyzeHeadless` scripts — no listener, no macOS dialogs. Not yet installed; not yet needed.
- Evidence convention: disposable captures → `~/bully-remake-evidence/<milestone>/` (outside repo); episode-worthy material noted here before cleanup.
