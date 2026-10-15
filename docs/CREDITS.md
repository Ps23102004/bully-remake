# Credits

This project would not exist without the Bully research and modding community. Everything below is used as **reference documentation**; no upstream code is copied unless its license permits, and no game content is ever redistributed.

## Research and reimplementation

- **[LiquidSnake0/bully-re](https://github.com/LiquidSnake0/bully-re)** — an engine reimplementation read directly from the game binary (~20,800 functions): file formats, collision, the Gamebryo NIF layer, engine behavior. Our primary format and behavior reference for the importers and game systems.

## Game scripts

- **[Razcoina/Bully-SE-scripts](https://github.com/Razcoina/Bully-SE-scripts)** — the game's decompiled Lua: missions, schedules, minigames. The spec every reimplemented system is traced against. Where a system in this repo reimplements behavior, the docs cite the Lua it was traced from.

## Modding tools and format knowledge

- **[marcdred/bully-mact-tool](https://github.com/marcdred/bully-mact-tool)** — MACT animation format research.
- The wider Bully modding community: the VFS modloader, IMG/CAT archive knowledge, NIF tooling, and the **Bully-Board** scene. Their work is proof that every file format in the game is readable; we reimplement those formats in our own importer code.

## Reverse engineering

Where the community sources don't have an answer, we reverse engineer the gap ourselves (Ghidra against our own purchased copy) and publish the findings as original documentation in `docs/re-notes/`.

## The game

*Bully: Scholarship Edition* © Rockstar Games / Take-Two Interactive. Buy it. You need it for this project to do anything at all.
