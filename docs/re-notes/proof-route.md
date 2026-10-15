# Phase 0 proof route (milestone 6)

The exact route of the our-build proof capture (`tools/ue/walk_m6_proof.json`, run by `tools/ue/run_walk.sh`), written
as landmarks and timings so the SAME route can be driven by hand in the original game for the side-by-side.
Video time is seconds from the first recorded frame (30 fps, 2700 frames = 90.0 s). Coordinates: UE cm = (100 x_game,
-100 y_game); the game axes are the ones the original game's own files use (gates face +X in UE).

Pace: **walk** = 2.05 m/s (analog/half stick), **run** = 3.61 m/s (full stick, the default gait). No sprint is used. The
original's third-person follow camera should be left alone (no manual look input) so it trails Jimmy the way ours does.

| # | video t (s) | segment | landmarks | our build (UE cm) |
|---|---|---|---|---|
| 1 | 0.0 - 4.1 | **Idle on the street** | Stand in the road/verge across the street from the school gates, facing the arch. The yellow BUS STOP sign is at the left edge of frame, the Bullworth Academy arch dead ahead, camera behind Jimmy. This is the opening frame (4K still `first_frame_4k.png`). | stand (32200, 7300), face -X (yaw 180), pitch -8 |
| 2 | 4.1 - 13.3 | **Walk** across the road and pavement to the gate arch | Cross the road, step onto the pavement, stop with Jimmy under the arch. | to (30300, 7300), 9.2 s |
| 3 | 13.4 - 16.4 | **Idle 3 s under the arch** | Let the viewer look at the courtyard through the arch. | stand |
| 4 | 16.4 - 24.0 | **Run** through the gates onto the courtyard | Straight on, along the entrance axis, to the circular cobbled "B" emblem in the forecourt. | to (27600, 7300), 7.5 s |
| 5 | 24.0 - 26.4 | **Running jump** over/at the emblem | One jump while running, about 0.8 s after the segment starts; the run continues after the landing. | jump at ~24.8 s, lands ~25.8 s, ends near (26800, 7310) |
| 6 | 26.4 - 38.2 | **Walk** the central path toward the main building | Keep to the centre of the path between the two gate-pier walls and the lawns; ambient students cross here. | to (24300, 7330), 11.7 s |
| 7 | 38.2 - 43.8 | **Run** to the front of the main building | Up to the foot of the front steps. | to (22300, 7330), 5.6 s |
| 8 | 43.8 - 50.2 | **Walk** up the steps to the main door | Up the broad steps between the two handrails to the columned entrance; walk straight into the doorway. The doorway is the trigger. | to (21070, 7330), ~6.1 s; door fires |
| 9 | 49.8 | **Door cut** | Original: the load/door transition into the entrance hall. Ours: 0.25 s fade out, teleport, 0.25 s fade in (one pure black frame at 49.80 s). | main_door_ext -> main_door_int |
| 10 | 50.2 - 51.3 | **Idle 1 s** just inside the hall | Arrive in the vestibule facing into the hall. | stand at (-62860, 32200) |
| 11 | 51.3 - 55.0 | **Walk** into the hall | Straight ahead onto the lobby floor; the two staircases and the Bullworth crest on the floor ahead. | to (-62860, 31400), 3.7 s |
| 12 | 55.1 - 58.1 | **Idle 3 s** in the middle of the hall | Look at the hall (crest, stairs, columns). | stand |
| 13 | 58.1 - 62.0 | **Walk** to the west side of the hall | Diagonally left, passing the left-hand pillar line (camera swings round on its own). | to (-63500, 30900), 3.9 s |
| 14 | 62.1 - 67.6 | **Walk** across to the east side | Across the front of the hall, in front of the staircases and past the crest. | to (-62300, 30900), 5.5 s |
| 15 | 67.6 - 73.9 | **Run** back to the exit door | Diagonally back towards the entrance vestibule and straight through the exit doorway. | to (-62860, 32950), 6.3 s; door fires |
| 16 | 73.6 | **Door cut** | Same transition back outside (black frame at 73.57 s). | main_door_int -> main_door_ext |
| 17 | 74.0 - 75.1 | **Idle ~1 s** on the terrace outside the door | Facing the courtyard / gates (camera behind Jimmy). | stand at (21320, 7330) |
| 18 | 75.2 - 78.2 | **Walk** off the terrace | Out of the doorway onto the top of the steps. | to (22000, 7330), 3.1 s |
| 19 | 78.2 - 88.0 | **Run** back across the courtyard | Down the steps and out along the central path towards the gates (ambient students pass). | to (25500, 7150), 9.7 s |
| 20 | 88.0 - 90.0 | **Idle 2 s**, end | Stop on the courtyard path. | stand |

Total 90.0 s of game time, 0 walk-runner failures. The pacing mixes walk and run on purpose (segments 2, 6, 8, 11, 13,
14, 18 walk; 4, 7, 15, 19 run; 5 jumps). Idle holds: 4.1 s street, 3 s arch, ~1 s + 3 s hall, ~1 s outside, 2 s end.

## Our-build capture settings (so the two halves can be compared like for like)

- 1920x1080, 30 fps, fixed time step (every frame rendered, so the video plays at true game speed regardless of how long
  the render took: 610 s wall clock for 90 s of video). **Fixed-step capture is not real-time performance evidence.**
- Real gameplay objects: `ABullyCharacter` (Jimmy, locomotion blend, follow camera: 4.5 m back, FOV 75), real collision,
  real door links, nine ambient students on waypoint loops (not scripted for this route; they are in the capture because
  they were on their loops, so their positions differ between runs only through timing).
- Lighting variant `B_raking_amber`, locked exposure EV100 11.6, Lumen GI/reflections, no audio (`-NoSound`).
- Opening still: 3840x2160 at the same vantage (route start).
- Not reproduced from the original: sprint (unused), Jimmy's hair (the model has none), start/stop/turn animations.
