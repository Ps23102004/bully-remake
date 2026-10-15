# HANDOFF — bully-remake

**Updated:** 2026-10-15 · **Branch:** `phase0` (checkout `~/bully-remake`) · **Machine:** midirstudio (M3 Ultra Mac Studio)

## Verified state

- Repo `oh-ashen-one/bully-remake` created public 2026-10-15, seeded on `phase0` (README, CREDITS, settings, CI blocklist, LICENSE).
- **Game install verified:** *Bully: Scholarship Edition* (PC, Steam via CrossOver), 5.0 GB, complete data tree at:
  `/Users/midir/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Bully Scholarship Edition`
  Key dirs: `Act/ Anim/ audio/ Coll/ Cuts/ DAT/ Data/ Graphics/ Models/ Objects/ Scripts/ Stream/ TXD/ ShaderBinaries/` plus `Bully.exe` (binary for Ghidra RE).
- Kickoff brief: `/Users/midir/Downloads/BULLY-KICKOFF-README.md` (planning session 2026-10-15).

## Phase 0 milestones (status)

1. Assets in (importers → academy grounds + main building in UE) — **in progress**: UE 5.8 project skeleton compiles (`unreal/BullyRemake`, Build.sh, settings V7); format survey done (`docs/re-notes/format-survey.md`); opus-builder writing `tools/importers` on branch `phase0-importers` (worktree `~/bully-remake-wt/importers`)
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

- Scout upstream repos (bully-re, Bully-SE-scripts, bully-mact-tool) for IMG/CAT/NIF/TXD format specs.
- Build the importer pipeline (milestone 1).

## Blockers

- None.
