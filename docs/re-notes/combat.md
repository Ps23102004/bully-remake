# Melee combat, first playable pass (slice-combat)

Status: 2026-10-15. Everything below is **authored**, not decoded: the original's fight logic lives in action trees we have not decoded (`Act/TrainingPlayer.act`, `Act/Anim/Fight_Tutorial.act`, `Act/AI/AI_FightTutorial.act`, see `opening-slice-recon.md` section 1 and the risk note). The clips are the decoded `C_Player` bank already on `SK_Jimmy`. Feel (hitstop, timing, camera) has NOT been judged by eye or hands yet; the headless test proves the logic only.

## Code

| File | What |
|---|---|
| `BullyCombatComponent.*` | Health, move table, block + guard meter, grapple pairs, lock-on / soft target, hit reactions, hitstop, mission events. On every fighter. |
| `BullyCombatAnimInstance.*` | Subclass of Jimmy's `UBullyAnimInstance`: cross-fades the component's clip request over locomotion, 4-way `STRAFE_*` while locked on. |
| `BullyCombatPawn.*` | The sparring partner: `ABullyCharacter` subclass on an imported ped mesh, enemy team, tutorial gates. |
| `BullyCombatAI.*` | `ABullyCombatAIController` (plain `AController`, no navmesh): approach, circle, telegraphed swings, sometimes block, back off after a flurry, tether. |
| `BullyCombatTest.*` | `-BRCombatTest` headless run + console commands `Combat.SpawnEnemy [floor]`, `Combat.Status`, `Combat.Gate attack|block 0|1`, `Combat.Invulnerable 0|1`, `Combat.Reset`. |
| `BullyCharacter.*` | Combat component on Jimmy, the four input actions, movement gated while busy, lock-on camera. |
| `tools/ue/run_combat_nullrhi.sh` | Headless runner (GPU slot, window watchdog, fixed 60 Hz). |

## Controls

| Action | Keyboard / mouse | Gamepad |
|---|---|---|
| Attack (chain, ground kick near a downed enemy, grapple strike while holding) | LMB | B |
| Grapple (grab; while holding = push to the ground; on a beaten partner = humiliate) | F | RB |
| Block (hold) | Q | LB |
| Lock-on (hold) | RMB | RT |

A jump, X sprint and Y/E interact were already taken; Esc stays on the controller's pause menu (legacy key binding, untouched).

## Moveset and clip map

Hit windows are centred on the clip's striking-limb peak reach, found by forward kinematics over the imported clips (`imported/models/PLAYER.gltf`; peak of the horizontal hand/foot-to-pelvis distance). Measured peaks (s): PUNCH_SLOP_1 0.18 (L hand), PUNCH_SLOP_2 0.32 (R hand), PUNCH_SLOP_3 0.25 (L hand), GROUND_KICK 0.40 (R foot), GRAP_HITA_GIV 0.40, GRAP_HITB_GIV 0.13, GRAP_HITC_GIV 0.30, GRAP_PUSH_ACT 0.40, PUNCH_S_4 0.37, HEAVY_PUNCH_2 0.55, G_PUNCH_FINISH_GV 1.07.

| Move | Clip (victim clip) | Window s | Dmg | Reaction |
|---|---|---|---|---|
| Jab1 -> Jab2 -> Jab3 (light chain) | PUNCH_SLOP_1 / _2 / _3 | .12-.24 / .25-.37 / .18-.31 | 5 / 6 / 9 | flinch / flinch / stagger |
| GroundKick (attack near a downed enemy) | GROUND_KICK | .32-.46 | 10 | ground hit, unblockable |
| Grab | GRAP_GIV -> GRAP_IDLE1_GIV (GRAP_RCV -> GRAP_IDLE1_RCV) | - | - | held |
| GrappleStrikeA/B/C | GRAP_HITA/B/C_GIV (_RCV) | .36 / .10 / .27 | 4 / 4 / 6 | held, held, stagger (ends the hold) |
| GrapplePush | GRAP_PUSH_ACT (GRAP_PUSH_VIC) | .32-.42 | 3 | knockdown |
| Humiliate (beaten partner) | G_PUNCH_FINISH_GV (G_PUNCH_FINISH_RCV) | 1.03 | - | KO |
| Bully jab / hook | PUNCH_CHARGE wind-up (0.35 / 0.5 s) auto-chained into PUNCH_S_4 / HEAVY_PUNCH_2 | .30-.42 / .48-.62 | 6 / 10 | flinch / stagger |
| Reactions | HIT_HEAD_JAB, HIT_HEAD_CROSS, HIT_GUT_RIB_1 (flinch); HIT_HEAVY (stagger); HIT_FRONT_FALL_HARD -> GROUND_ONBACK (down); HIT_GROUND_HEAVY; GETUP (i-frames); HIT_KO_COLLAPSE -> GROUND_HURT_IDLE (KO); STUNNED (beaten); BLOCK_IN -> BLOCK_IDLE_B, BLOCK_HIT_LIGHT, BLOCK_BREAK | | | |

Pair clips are started together (receiver half begins when the giver's move begins) with the victim snapped `GrappleHoldDistance` in front of the giver; the true pair alignment offset is not decoded. The partner ped plays Jimmy's clips through UE's runtime skeleton remapping (same 36 joint names).

Hit detection is an arc, not a bone hitbox: in the active window every opposing fighter within `Range` cm (capsule surface to surface) and inside `ArcDeg` is hit once per swing. Standing swings pass over downed fighters; ground kicks only hit downed ones.

## Mission events (UMissionSubsystem::FireEvent)

| Event | When | Tutorial step |
|---|---|---|
| `block_success` | Jimmy blocks a hit | 1 (twice) |
| `combo_done` | all three light-chain hits landed | 2 |
| `grapple_start` | Jimmy grabs | 3, 5 |
| `grapple_strike` | each grapple strike lands | 4 |
| `grapple_strikes_done` | the third strike ended | 4 |
| `grapple_done` | grapple push landed (partner on the ground) | 6 |
| `ground_kick` | ground kick landed | 7 |
| `enemy_beaten` | partner health reached its floor (`HealthFloor` > 0) | B7 |
| `humiliation_done` | humiliation landed | B7 |
| `enemy_knocked_out` | partner KO (damage or humiliation) | B7 |
| `player_knocked_out` | Jimmy KO | fail |

The AI also reads two mission flags (`set_flag` actions): `combat_ai_passive` (no swings) and `combat_ai_noblock`.

## Feel knobs (Config/DefaultGame.ini, no recompile)

- `[/Script/BullyRemake.BullyCombatComponent]`: per-move `ActiveStart/ActiveEnd`, `BufferOpen/ChainTime`, `Lunge/LungeTime`, `Knockback`, `HitStop`, `Range/ArcDeg`, `Damage`; `HitStopDilation`; `FlinchTime`, `StaggerTime`, `DownedTime`, `GetUpTime`, `FlinchesBeforeStagger`; block `BlockStunTime`, `GuardMax/GuardRegen`, `PerfectBlockWindow`, `BlockArcDeg`; targeting `AssistRange/AssistConeDeg`, `TrackTurnRate`, `LungeStopDistance`, `LockOnMoveScale`; grapple `GrappleRange`, `GrappleHoldDistance`, `GrappleEscapeTime`; health per team.
- `[/Script/BullyRemake.BullyCombatAnimInstance]`: `CombatBlendIn/Out`, `ClipCrossFade`, `bStrafeWhenLocked`.
- `[/Script/BullyRemake.BullyCombatAIController]`: `EngageDistance`, `AttackRange`, `AttackIntervalMin/Max`, `FirstAttackDelay`, `HookChance`, `BlockChance`, `BlockReactTime`, `BlockHoldMin/Max`, `RetreatAfterHits/RetreatTime`.
- `[/Script/BullyRemake.BullyCharacter]`: `LockOnCameraYawRate`.
- `[/Script/BullyRemake.BullyCombatPawn]`: `PedMeshPath`, `bAttackAllowed`, `bBlockAllowed`, `TetherRadius`, `HealthFloor`.

## Needs eyes and hands (not verifiable headless)

Hitstop length/strength, chain timing and buffer windows, lunge distances, whether the arc hits look like contact (no camera shake, no impact VFX/SFX yet), lock-on camera swing, the grapple pair alignment (offset and facing), the skeleton-remapped clips on the ped's proportions, blend times, the bully's telegraph readability and cadence.

## Not done

Charged/heavy punch and running punch, enemy grabbing Jimmy, grapple escape mashing for the player, impact sounds (cue graph decoded, names not looked up), hit VFX, camera shake, the tutorial button-prompt widget and the 1_02 mission JSON (mission-fw owns those), spectator ring, `GN_Bully05` import (the partner uses `GN_Boy02` until then).
