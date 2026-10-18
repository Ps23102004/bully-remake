# Bully Remake — Unreal Engine 5.8

An **unofficial, non-commercial, work-in-progress fan project** exploring *Bully: Scholarship Edition* in Unreal Engine, built with AI assistance and developed in public.

**This is an experiment you can try and build on, not a finished remake. I probably won't finish the entire game. There is no promise of a full-game release, complete campaign, or release date.** The aim is to let people explore what works today and continue experimenting themselves.

You can give this repository to a coding agent and ask it to help set up the available content on your own computer. Read the requirements and known setup blockers below first: this is currently a developer workflow, not a one-click game download.

**You need your own legally obtained, installed PC copy of Bully: Scholarship Edition. No original game assets are included.**

## Copy/paste this into your coding agent

Give this prompt to an agent that can work on **your own computer**, with permission to access your local game installation:

```text
Help me set up and try the current available content in:
https://github.com/oh-ashen-one/bully-remake

I own a legally obtained copy of Bully: Scholarship Edition. First inspect the
repository's current README, importer/Unreal guides, branch-specific status,
.uproject, build targets, and scripts. Start on main and report the exact commit.
Do not merge experimental branches or assume their features exist on main.

Check my OS, hardware, free storage, Python, native C++ toolchain, Unreal version,
required plugins, and the external GPU-slot helper. Locate my installed PC copy
locally (or ask me for its folder); keep the game installation read-only. Do not
download game assets, upload them, redistribute them, or bypass DRM.

Help me obtain Unreal Engine 5.8 from Epic's official source if it is missing.
Ask me to handle account sign-in and any required license acceptance. Do not
silently substitute another engine version or download unknown plugins/helpers.
Explain required downloads and local changes before making them, and follow
your normal approval requirements.

The existing Unreal wrappers are Mac-specific and depend on a helper not
included in the repo. If my platform or prerequisites do not match, identify
the concrete blocker and propose the smallest reviewed adaptation. Do not
disable safety checks or pretend unsupported setup is already tested.

Follow the checked-out extraction, C++ build, Unreal import, and launch
instructions in dependency order. Check logs and generated assets after each
stage. Keep generated game content in ignored local output folders. Preserve
my existing work; do not modify gameplay code or publish changes as part of setup.

If setup succeeds, help me launch the Bullworth map and try the currently
implemented campus route. Distinguish build success, headless checks, rendered
verification, and actual hands-on play. Report what really works, what remains
unverified, and any errors with their command, exit status, and relevant log
excerpt. If blocked, stop the dependent step and tell me exactly what is needed.
```

An agent can help inspect and diagnose this project; it cannot guarantee compatibility, supply a game you do not own, or turn unfinished systems into a complete game.

## Ownership, rights, and intent

- **You must own a legally obtained, installed copy of *Bully: Scholarship Edition* for PC on your own device.** The importers read that installation locally and leave it unchanged. The documented working source is the Steam PC edition installed through CrossOver on macOS; other installations have not been established as equally tested.
- This repository contains original tooling, Unreal systems, and documentation. **No proprietary Rockstar game code, assets, mission scripts, audio, or game data are bundled.** Converted content stays local in git-ignored output directories. Do not commit, upload, or redistribute it.
- This is an independent fan project, **not affiliated with, endorsed by, or connected to Rockstar Games or Take-Two Interactive**.
- All rights to the original game, its characters, artwork, models, audio, scripts, names, and trademarks remain with Rockstar Games / Take-Two Interactive and their respective rights holders. The original code written for this repository has its own [MIT license](LICENSE); that license does not grant rights to the original game's content.
- The intent is to experiment respectfully with an owned game, **not to infringe copyright, facilitate piracy, bypass copy protection, or break the law**. Ownership and this notice are not a blanket permission to use or redistribute protected material. Respect applicable laws and license terms. We will comply immediately with any rights-holder objection.

## What can I try today?

### `main`: campus exploration + the Day One opening slice

Status checked **2026-10-17**, against commit [`ee28789`](https://github.com/oh-ashen-one/bully-remake/commit/ee28789). `main` is the most up-to-date version of the project; the Day One opening work that previously lived on a separate branch has been merged in.

The repository's development records report these working on the development Mac:

- Imported academy grounds and school interiors, with a walkable route from the front gates through the courtyard into the entrance hall and back.
- Jimmy's imported skeletal mesh, buzz-cut head, locomotion animations, third-person camera, walking, running, sprinting, and jumping.
- A main-door transition in both directions.
- Six student models used by nine ambient walkers on simple loops; player collision and yielding behavior.
- Locally extracted footsteps and courtyard ambience, plus a golden-hour Lumen lighting setup.
- Settings-menu code on `main` for horizontal/vertical look sensitivity and invert-Y.
- An opening-mission ("Day One") framework: a JSON-driven objective/stage machine with an in-game HUD (objective text, waypoint, interaction prompts, mission banners).
- A melee-combat prototype: light combos, blocking with a guard meter, grappling, a ground kick, lock-on, and a basic enemy with a knock-out.
- Scripted NPC beats for the opening: a gauntlet of bullies, and a fight crowd that gathers and reacts.
- A windowed-game mouse-capture fix so look and clicks work on macOS without extra setup.

**Verification has limits.** Development notes record scripted collision checks, renders, and a 90-second fixed-timestep route capture. Those are not a fresh-install certification, a real-time performance benchmark, or proof that every part of the campus works. The original-game half of the side-by-side comparison is still pending. Footstep pitch and the overall live visual/audio feel still need hands-on review. Known rough edges include sprint foot sliding, missing start/stop/turn animations, walkers passing through each other, incomplete area testing, and material/lighting approximations. The settings code also needs checking in your actual play session.

### The Day One opening slice: runs end-to-end, still needs a hands-on pass

The opening mission now runs start-to-finish in the framework: the first two missions auto-chain, the schoolyard fight spawns its own opponent (no console command), waypoint blips parse correctly, and the epilogue is a proper fade-to-black-and-back rather than a bare teleport. A headless playthrough (`-game -nullrhi`, no renderer) completes all 26 stages both as one combined `day_one` mission and as the two chained missions, including the deliberate fail/retry on the combo step.

Hits now carry **impact camera shake**: landing or taking a blow kicks the follow camera, scaled by how hard the hit lands (a light jab barely registers; a knockdown or KO rocks the frame; taking a hit shakes harder than dealing one). It is a pure-C++ additive shake on the camera — no UMG, no CameraShake asset — fully tunable from `Config/DefaultGame.ini` (`[/Script/BullyRemake.BullyCharacter]` magnitudes, `[/Script/BullyRemake.BullyCombatComponent]` per-event scales). The headless combat test fires it on every connecting hit (28 kicks across the tutorial + free fight, amplitudes 0.16–0.85 by reaction).

What a headless log still cannot settle: the remaining **combat "feel"** — impact sound effects, and the exact *magnitudes* of the shake and hitstop — is unjudged and wants hands-on play, and the mission, combat, and gauntlet have been checked in headless logs rather than a rendered play session. A headless test passing does not establish that a person can play the slice unaided, so treat the live feel as unverified until it has had a real play pass.

### Beyond Day One: the objective → reward loop

A first step past the opening slice: an **optional side mission** (`Config/Missions/side_errand.json`, an original campus errand) and a **reward loop**. Finishing a mission can now pay out through a `reward` action into a persistent session wallet (money and respect), shown on the mission HUD. A headless smoke run (`tools/ue/run_mission_smoke.sh side_errand`) drives the errand through all five stages and confirms the payout. This is the loop groundwork; an **open town to roam between missions is not in yet** — the current import only covers the academy grounds and interiors, and bringing in the town districts needs another pass over the original game's world data.

### In-engine arrival cutscene

The opening is now framed by an **in-engine cutscene** (`Config/Missions/arrival.json`), not a pre-rendered video: the new transfer is dropped off at the school gates, a faculty member walks up and lays down the rules, and it hands straight off to `day_one`. It is built entirely from data-driven mission primitives, two of them new and reusable:

- `camera` — cut or blend the view to a scripted cutscene camera (aimed at the player, a named point, or a spot), with an optional FOV; `{ "type": "camera", "release": true }` hands the view back to the player. The framings compose the drop-off and the face-to-face with the faculty member; the camera actor is owned by the mission and always released on mission end, so gameplay is never left looking through it.
- `letterbox` — slides cinematic bars in/out (`{ "type": "letterbox", "on": true }`).
- `say` — one cutscene dialogue line at a time, with a speaker label (`{ "type": "say", "speaker": "Prefect", "text": "…", "seconds": 4 }`).
- `shake` — kick the player's camera for an impact beat (`{ "type": "shake", "amp": 0.4, "seconds": 0.35 }`), the same shake combat uses. The arrival uses a subtle one as the faculty member steps up.

Each line is its own timer-paced stage, so the beats read in order with no player input — which also means it runs clean under the headless harness (`tools/ue/run_mission_smoke.sh arrival`, PASS 6/6, every cut and line logged in order). The faculty figure reuses the existing spawn as a silent, non-hostile stand-in (there is no dedicated friendly-NPC or branching-dialogue system yet). All dialogue is original wording and the characters are original (no likenesses from the source game). A mission whose `on_complete` chains into another now skips the "MISSION COMPLETE" end-card so the hand-off into gameplay reads clean. What a headless log still cannot settle: the camera *composition* and timing want a real play pass — the logs confirm the cuts fire and the view blends, not that the shots are well framed.

## Requirements and current setup blockers

1. **Your own installed PC copy of Bully: Scholarship Edition.** Locate its game root, including `Stream/World.img` and the other installed data directories. Do not use someone else's extracted assets.
2. **Unreal Engine 5.8.** Both [the project](unreal/BullyRemake/BullyRemake.uproject) and its C++ target specify 5.8. Obtain Unreal only through [Epic's official download/setup route](https://www.unrealengine.com/download), select the matching engine version, and review Epic's terms yourself. Do not silently convert the project to another engine version. If 5.8 is unavailable to you, report that as a blocker.
3. **A compatible native C++ toolchain.** The documented development machine is an Apple Silicon M3 Ultra Mac Studio. For the Mac route, use Xcode and the SDK compatible with your engine build; also ensure `swiftc` is available for the wrapper's window-check helper. [Epic's macOS requirements](https://dev.epicgames.com/documentation/en-us/unreal-engine/macos-development-requirements-for-unreal-engine) currently list macOS 14.5 minimum, Xcode 26.0 minimum / 26.1.1 recommended, and warn that Xcode 26.4 is incompatible. Confirm the requirements for the exact engine build you install. The repo does not pin the developer's exact macOS/Xcode versions or establish a minimum hardware/performance specification.
4. **Git and Python 3.** The importers use the standard library, with NumPy for accelerated decoding and Pillow for previews; pytest is used for tests. Use an isolated Python environment. There is no pinned Python environment/lockfile in the current tree.
5. **Required engine plugins.** The project enables `EnhancedInput`, `PythonScriptPlugin`, and `ModelContextProtocol`; the asset pipeline uses Unreal's Interchange glTF importer. Verify these are available in your engine installation. A missing plugin is a setup blocker, not a reason to download an unknown binary or silently alter the project.
6. **Local storage for the engine, build products, converted assets, and caches.** A clone does not include the generated map or characters. First-time shader compilation and imports take time; the developer's recorded timings are not a guarantee for your computer.

**Important portability gap:** [`tools/ue/_common.sh`](tools/ue/_common.sh) hard-codes Mac engine binaries and uses macOS process/window tooling. The checked-in wrappers also require an external executable GPU-slot helper, defaulting to `~/.cache/gpu-slot/bin/gpu_slot.sh`, which **is not supplied in this repository**. `GPU_SLOT` can point to an existing compatible helper; changing that variable alone does not install one. The wrappers refuse to launch without it. Report a missing helper and agree on a reviewed local setup/adaptation before proceeding; do not bypass the checks or claim a clean clone is self-contained.

**Windows and Linux setup is not verified by this README.** Unreal supporting a platform does not make these Mac-specific scripts portable. Porting their launch/toolchain handling is additional work. CrossOver is part of the documented original-game installation/reference workflow, not how the Unreal Mac build itself runs.

## Setup and launch outline

Read [the importer guide](tools/importers/README.md) and [the Unreal pipeline guide](tools/ue/README.md) before running commands. They contain detailed options and historical milestone notes; use the checked-out code and latest branch-specific status to resolve stale notes. [HANDOFF.md](HANDOFF.md) includes developer-local paths and older status sections, not paths that will exist on your computer.

### 1. Clone and locate your game

```sh
git clone https://github.com/oh-ashen-one/bully-remake.git
cd bully-remake
git switch main
git rev-parse HEAD

# Replace this with the root of YOUR installed PC copy:
export BULLY_INSTALL="/absolute/path/to/Bully Scholarship Edition"
# Replace if your official Unreal installation is elsewhere:
export UE_ROOT="/Users/Shared/Epic Games/UE_5.8"

python3 -m tools.importers --help
```

Alternatively, the importer accepts `--install` before the subcommand or a git-ignored `tools/importers/local_config.json`. Never copy the developer's personal install path blindly.

### 2. Check prerequisites and build the editor module

Resolve the toolchain, plugin, and GPU-slot prerequisites above first. On the documented Mac route, from the repository root:

```sh
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" \
  BullyRemakeEditor Mac Development \
  -project="$PWD/unreal/BullyRemake/BullyRemake.uproject"
```

Stop on a build failure and read the error. **Build before assembling the map**: without the module, a level import can skip the custom door actors.

### 3. Convert your local content, then import it into Unreal

These commands describe the existing Mac workflow **after its prerequisites are satisfied**:

```sh
python3 -m tools.importers world
python3 -m tools.importers ped PLAYER --anims '*' --root-motion inplace --head B_Buzz

python3 -m tools.importers audio extract FeetAll FeetSCHL
python3 -m tools.importers audio extract Ambs --names 'NewSchoolOutdoor_F|Birds01'
python3 -m tools.importers audio footsteps

for m in GN_Boy01 GN_Boy02 GN_Asiangirl GN_Greekboy NDH3_Bucky PFH1_Seth; do
  python3 -m tools.importers ped "$m" --anims IDLE PLAYERWALK PLAYERRUN RUNFAST IDLEJUMP JOGJUMP_L JOGJUMP_R FALL FALL_LAND PLAY_LAND_HURT --root-motion inplace
done

python3 -m tools.importers validate

tools/ue/run_import.sh
tools/ue/run_import_character.sh
tools/ue/run_import_ambient.sh
tools/ue/run_lighting.sh --variant B_raking_amber
```

Run stages in order and check each result before continuing. Intermediates go to `imported/`; Unreal assets go to `unreal/BullyRemake/Content/Imported/`. Both are git-ignored. Import logs and reports are under `unreal/BullyRemake/Saved/BullyImport/`. Inspect reported failures and missing textures instead of assuming an exit alone proves everything is playable. The importer also has synthetic tests: `python3 -m pytest tools/tests`; setting `BULLY_INSTALL` enables tests against your installation.

### 4. Open the project and play the available route

Open `unreal/BullyRemake/BullyRemake.uproject` with UE 5.8 after a successful build/import. Load `/Game/Imported/Maps/Bullworth` (the configured default map), then use Play in Editor. A deliberately launched interactive editor is separate from the headless/offscreen capture wrappers.

- WASD / left stick: move; mouse / right stick: look
- Left Alt or Left Ctrl / partial stick: walk
- Left Shift / L3 / gamepad X (Square): sprint
- Space / gamepad A: jump
- Esc: settings/resume in the game controller; editor play modes can intercept Esc

Try the gates → courtyard → main east door → lobby → outside route first. If Jimmy is a primitive placeholder, verify the character import. If the door fails, check for missing door actors and the import log's `door links skipped` warning; after a successful module build, the existing `tools/ue/run_place_doors.sh` can re-place them. See the Unreal guide for details. Do not interpret missing content as a finished feature.

## Fidelity and future work

Faithfulness to the original is a goal to test through measurements and reference footage, not a claim that the current prototype is already a one-to-one remake. Broader free-roam systems, schedules, missions, and later chapters are experiments or possible directions, not delivery commitments. Contributions should describe what was actually built and verified, preserve the original-code-only boundary, and keep proprietary content out of commits and issue attachments.

## Credits

This project stands on the shoulders of the Bully research and modding community. See [docs/CREDITS.md](docs/CREDITS.md).

- [LiquidSnake0/bully-re](https://github.com/LiquidSnake0/bully-re) — engine reimplementation research: formats, collision, the Gamebryo NIF layer
- [Razcoina/Bully-SE-scripts](https://github.com/Razcoina/Bully-SE-scripts) — decompiled mission-script research used as a behavior reference
- [marcdred/bully-mact-tool](https://github.com/marcdred/bully-mact-tool) and the wider Bully modding community (VFS modloader, IMG/CAT/NIF tooling, Bully-Board) — file-format research and tooling

Unlicensed upstream repositories are treated as documentation to learn from, not code to copy. Our own reverse-engineering notes live in [docs/re-notes/](docs/re-notes/).

## License

Original code in this repository is [MIT licensed](LICENSE). *Bully*, *Bully: Scholarship Edition*, and their related proprietary content belong to Take-Two Interactive / Rockstar Games and their respective rights holders. That content is neither licensed by this repository nor distributed with it.
