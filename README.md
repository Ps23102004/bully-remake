# Bully Remake — Unreal Engine 5.8

A **one-to-one port of Bully: Scholarship Edition (2008) to Unreal Engine 5.8**, built by AI, developed in public, in the OpenMW shape.

## How this works

- This repository contains **only original code**: importers, Unreal Engine systems, tooling, and documentation. **No Rockstar code, assets, scripts, audio, or data — ever.** CI enforces a blocklist on the imported-content directory.
- The game's content comes from **your own purchased copy** of *Bully: Scholarship Edition* (PC — Steam or Rockstar Store). Our importers read the installed game and convert its content into Unreal assets locally, at build time, on your machine. **You must own the game.**
- This is an **unofficial, non-commercial fan project**. It is not affiliated with, endorsed by, or connected to Rockstar Games or Take-Two Interactive. We will comply immediately with any rights-holder objection.

## Current scope — Phase 0

Run around the outside of Bullworth Academy and into one part of the school, in Unreal, looking right:

1. **Assets in** — importers convert the academy grounds + main school building into UE levels
2. **Interior in** — entrance hall and corridors enterable, door transitions working
3. **Jimmy moves** — player locomotion and camera matched against captured 2008 footage
4. **Signs of life** — a handful of students walking simple loops, footsteps, ambient sound
5. **The shot** — one golden-hour Lumen setup, tuned against original footage
6. **Proof** — one continuous capture cut side-by-side against the same route in the original

Later phases: free-roam, the clock/schedules, Chapter 1 traced branch-for-branch from the decompiled mission scripts, then a chapter per episode.

## Fidelity rule

One-to-one means **measured**. We record the original game, script the same inputs in our build, and compare. Mission logic must match the original's decompiled scripts branch for branch.

## Credits

This project stands on the shoulders of the Bully research and modding community — see [docs/CREDITS.md](docs/CREDITS.md):

- [LiquidSnake0/bully-re](https://github.com/LiquidSnake0/bully-re) — engine reimplementation research: formats, collision, the Gamebryo NIF layer
- [Razcoina/Bully-SE-scripts](https://github.com/Razcoina/Bully-SE-scripts) — the decompiled mission scripts, the spec for every system
- [marcdred/bully-mact-tool](https://github.com/marcdred/bully-mact-tool) and the wider Bully modding community (VFS modloader, IMG/CAT/NIF tooling, Bully-Board) — proof every file format is readable

Unlicensed upstream repositories are treated as documentation to learn from, not code to copy. Our own reverse-engineering notes live in `docs/re-notes/`.

## Legal

All original code in this repository is MIT licensed. *Bully*, *Bully: Scholarship Edition*, and all related assets are the property of Take-Two Interactive / Rockstar Games. This repository distributes none of them.
