# Handoff: the next session

Written 2026-10-18, last updated 2026-10-31 ~12:50. Read `docs/HANDOFF_UE.md` in newengine first, as CLAUDE.md says. This file picks up from where that one stops.

Private notes (character references, concept-art review, script status) are in `D:\projects\ft_private\HANDOFF_PRIVATE.md`. They stay off this public repo.

## Resume here (updated 2026-10-21, Max plan)

- **NEWEST (2026-10-31 ~12:50): the design overhaul of 2026-10-30/31, held at the usage stop.** The Fable
  session (the advisor; the user gave it authority overnight) ran three design changes the user asked for; all the
  text is private in `D:\projectst_private\` (read `HANDOFF_PRIVATE.md` there first: it has the rulings, the scores
  and the queue), and nothing of it is in this repo yet. In short:
  - **Devin's world, Solhaven, changes look:** from the festival town to a contemporary coastal city on a mountain's
    shoulder (concrete terraces, rail, a floodlit stadium for the tournament, the old festival quarter kept as one
    district; a dark palette with one hot accent per fight; the enemies grotesque, calm and graded). Photoreal stays.
    The story's beats don't move; the design document's Solhaven sections are rewritten in newengine (commits
    a191ad9, cc4875f on `feature/engine-foundation`, NOT pushed: the remote branch was rewritten by the engine-freeze
    commits and no longer holds the local history, so a push is non-fast-forward and the user decides how to reconcile;
    never force-push), a Draft 96 outline item lists every scene's
    dressing change, and the private location briefs and boss entries are re-dressed. The Unreal side is untouched:
    the town kind still dresses Solhaven as pale plaster houses; the building kit follows the private look sheet
    when the kit work starts. No franchise names anywhere in this repo: our words are the look sheet's.
  - **Three battle types by tier** (the user's rule): every non-boss fight is **Turns** (the existing turn-based
    battle with active defence; it alone keeps the skill arrow combos), every boss fight is **Arena** (open-arena
    real-time: lock-on, dodge with i-frames, parry, Load as a second gauge beside stamina, punish windows), each
    world's final boss is **Beat** (the music is the timeline; attacks and answers on bars and beats; prompts by
    action buttons; a HUD beat line with the sound off), and the game's final boss runs Turns, then Arena, then Beat.
    The engineering lane is the user's own session on branch `claude/sleepy-grothendieck-a99140` (steps 1 to 5b
    pushed at cfe4f40: arrow combos fenced to Turns, routing by encounter, the beat snap and 0.4 s tell floor,
    line-ups of five, Guard/Load/Release/Overload; 858 doctest cases, ctest 5/5, UBT Succeeded; HUD unchecked on
    screen; left: Intercept, Take Half, Read cards, the Arena host, the Beat mode, one-lead Turns, the three-stage
    sequencer, the first Turns encounter data). Not merged into main.
  - **The boss overhaul** (private): 72 of the 90 slots written beyond the six model entries, 61 at or over the bar,
    every world's finals written as Beat fights where their waves are done, 21 non-boss family stat blocks for Turns.
    Left: the six superbosses (W14) and the six Act IV story slots that wait for the Draft 96 outline (W11), both
    held because the weekly usage read 92% at 12:30; they are the next session's first waves after the reset
    (2026-11-06 ~06:00 EDT). A word-level pass on four entries was in flight at this writing.
  - **Repo state:** main is still d3e5cc6 (terrain and map paths), nothing new committed here; the user's local
    edits (`Config/DefaultEngine.ini`, `ForgottenThird.uproject`, `dv_strap.uasset`, `tools/blender/open_features.py`)
    are untouched; the PC is free; `claude/objective-blackburn-80b6c6` and the PartyLooks follow-up are still open.
  - **Next session, in order:** W14 and then W11 (after the user approves the Draft 96 outline), the queued boss
    passes the private handoff lists, the Solhaven building kit for the Unreal town kind, and the C++ lane's
    remaining steps if the user's session hasn't finished them.
- **Earlier (2026-10-30 ~07:30): the ground darkens where the rain gathers, on main (a537d8b), and every map now opens
  by its long name (main d3e5cc6).**
  - Session: the user switched the main model to Fable at ~03:00 and had it delegate. The PC was freed at 02:55 and
    is free again (`ft_private/gpu_busy_leads.txt`).
    - A private production-brief set (templates, one brief per location, the bosses to follow) is written and lives
      in `ft_private/production`. See the private handoff.
    - The enemy and boss overhaul goes on privately; its exemplars are in reader rounds. Story draft 96 is untouched.
    - Still open: the PartyLooks follow-up for `FTBattleDemoActor.cpp` and `FTExploreSpot.cpp` (the entry below), and
      `claude/objective-blackburn-80b6c6` is still unmerged.
  - The flow mask in the material (a537d8b, `terrain-worlds` fast-forwarded into main):
    - The map builder keeps `ftcore::flowMask`'s bytes in each map as a 1024 x 1024 `T_FlowMask` beside
      `T_ShapeMask`. `M_WorldGround` reads it on the ground layer only, never the rock or the road.
    - From `FlowStart` to `FlowEnd` of the mask the ground darkens by `WetDarken` (0.3), stands `WetRich` (0.3)
      further from its grey and loses `WetSmooth` (0.2) of its roughness.
    - A crest, from halfway up the shape mask's scale and less where it's wet, moves `CrestPale` (0.35) of the way to
      its own colour dried: `CrestDry` (0.3) of the way to its grey, its hue kept, and a quarter lighter. Moving it
      toward plain grey, as first tried, drained the town's grass to a frosty white.
    - The builder sets `FlowStart` and `FlowEnd` per map, at the 95th and 99.5th percentiles of the mask over the open
      ground inside the square (the pads and roads, which read 0, don't count). The master's own 0.4 and 0.8 would
      darken 22 to 25% of the open ground.
      - Solhaven: bytes 153 (0.600) to 210 (0.824).
      - Lattice: bytes 156 (0.612) to 209 (0.820).
      - Unwritten: bytes 150 (0.588) to 194 (0.761).
    - The flow mask adds 0.5 to 0.7 MB to a map.
  - A theory measured and disproved: that the heights' whole steps (7.8 mm) steer the rain into the long parallel
    lines. Run over unrounded heights, 1.3 to 2.0% of the wet samples sit on a whole-step flat against 1.0 to 1.4%,
    and the mask moves by 8 or more on only 0.8 to 1.5% of its pixels.
    - The lines are the worn ground's own rills, which the rain follows as it should. On open slopes of 3 to 50%,
      84 to 93% of the wettest twentieth of the samples sit in a hollow, against 28 to 29% of the drier half.
    - The change was reverted.
  - How it looks:
    - Solhaven: the crests come out 2 to 3% lighter on the grass, hue within about a degree, never frosty. They barely
      show even at 0.35, and that's where they stay.
    - Lattice: thin, darker channels run over the gravel inside the square. The crests come out 2 to 3% lighter and
      hardly show.
    - Unwritten: darker channels branch down from the mesas' feet. Out on the plain, long parallel lines read as dry
      rills.
  - Not yet:
    - A closed hollow keeps all its rain. The 8 m wet speck on Lattice's gravel no longer shows at the 95th percentile,
      but nothing fills the hollows.
    - The parallel rills want `wear()` (Core/src/Terrain.cpp) changed so its rills branch and join on long slopes.
      That's a pass of its own, not a flow change.
    - Queued passes:
      - pads that fall away on a named side, so a climax looks down on its world
      - pad and plaza sizes per site
      - playable mountain ground
      - a seawall edge for Lattice
      - set dressing by data
      - fog colour per world
  - Checks at a537d8b:
    - Gate: 832 doctest cases, ctest 4/4, UBT Succeeded (with -DisablePlugin=Fab).
    - Ten renders log no material or sampler error.
    - The tours: Solhaven 18 legs in 814 s, Lattice 15 in 803 s, Unwritten 17 in 992 s, the lead's feet 5 cm over
      the floor.
    - The headless story run: `Alpha: FINISHED 73 scenes, 167 screens (40 battle, 91 field, 36 stub), 0 STUCK,
      0 warnings, 12 overworld hops`.
  - The map shadowing and its fix (d3e5cc6):
    - That story run logged no Solhaven placings. Every Solhaven step loaded `/Game/Trial/LookBefore/World_Solhaven`,
      a local copy excluded from git, instead of `/Game/Maps/World_Solhaven`.
    - The cause: the engine resolves a short map name to the first package of that name anywhere under /Game.
    - Now every map opens through `FTStep::OpenMap`, by `ftcore::mapPackage`'s long name under /Game/Maps. No screen
      calls `OpenLevel` itself, and the loader takes only short names.
    - The local copy is left as it is.
    - After the fix the story run loads every world from /Game/Maps and logs 59 placings (Solhaven 24, Lattice 14,
      Unwritten 21), the lead's feet 5 to 6 cm over the floor, with the same FINISHED line. Gate: 833 doctest cases,
      ctest 4/4, UBT Succeeded (with -DisablePlugin=Fab).
- **Earlier (2026-10-30 ~02:45): state saved as usage ran out.**
  - In progress: the enemy and boss design pass, and story draft 96. Their notes are private (see the private
    handoff). Nothing of either is in this repo.
  - Why the demo showed the old lead models: it was opened from a worktree (`.claude/worktrees/terrain-worlds`).
    - A worktree holds only what git tracks. The MetaHuman leads, their retargeted clips, `Config/UserGame.ini` (the
      party) and the uproject's MetaHuman plugins are local-only in the main checkout.
    - Run the demo from `D:\projects\unrealgamewip\ForgottenThird.uproject`. The launcher shows both copies as
      "ForgottenThird".
  - Even from the main checkout, the old meshes still load in two places, both by hard-coded path:
    - The battle screen: `FTBattleDemoActor.cpp` loads the old devin and duy meshes and idles.
    - The explore spot's figure: `FTExploreSpot.cpp`.
    - Next: drive them from PartyLooks, keeping the old meshes as the fallback. It needs UBT.
  - `claude/objective-blackburn-80b6c6` (the skin master and FTBuildSkin -Profile) is pushed but not merged into
    main.
  - The machine is on hold for the user: no editor, commandlets, UBT or renders until they free it.
  - Terrain: its session was stopped at ~02:36 before it could save.
    - Its Source side is now committed as unbuilt WIP 63321f3 on the local `flow-mask` branch
      (`.claude/worktrees/flow-mask`): T_FlowMask beside T_ShapeMask, and M_WorldGround's FlowMask input and Flow group.
      It isn't pushed, because UBT can't run until the machine is free.
    - Its Core commits (0fca86d, 7bb7fba) are on `origin/terrain-worlds`, not merged into main.
    - Next for that lane, once the machine is free: UBT, `-run=FTBuildGround`, the three map rebuilds, the renders, the
      tours and the headless story run. Then push, and fast-forward main. The entries below have the details.
- **Earlier (2026-10-30 ~01:45): the worlds measured at other sides (`terrain-worlds` 7bb7fba, not merged into main,
  which stays at 8a2da97). Core only: worlds.json is unchanged, no map is rebuilt, and the 1 km ground is bit for bit
  as before.**
  - Why: larger worlds may be wanted, for more fight sites. Nothing about the size is decided.
  - Core (7bb7fba):
    - The open ground's droplets now come as many to each sample at any side as at `WorldTerrain::kTunedSide`
      (1000 m): 235,368 at 1.5 km, 257,969 at 2 km, 300,709 at 4 km.
    - `flow()`'s unit grows with a sample's area from its 1 km value: 11.1 m² at 2 km, 36 m² at 4 km. `kFlowFull`
      stays a hectare.
    - The mountains' two passes already came as many to each sample and are unchanged.
    - test_terrain takes `FTCORE_WORLD_SIDE` (600 to 4000 m) to generate all three worlds at that side. It prints
      each world's time and a hash of its heights, cliffs and flow.
  - What binds:
    - The field keeps 757 samples a side (the Landscape's 6 x 6 components) and 250 m round the square, so a larger
      side only spreads them: 1.98 m apart at 1 km, 3.31 m at 2 km, 5.95 m at 4 km.
    - The road's strip breaks first. It holds exactly only to about 1.1 km a side. It misses by 5.1 cm at 2 km, with
      the ground still under the road, and by 34 cm at 4 km, with the ground coming through it.
    - At 4 km the pools come out 0.31 to 0.36 m deep for 0.6.
    - The rain's relief falls under its 1 km floor of 1.25: from 1.9 km in the towers (1.17 at 2 km), and at 4 km in
      all three (0.87 to 1.18).
    - The road-mask test fails past 1 km only because it bakes its own 512-pixel mask.
  - Times (optimized, at low priority, so rough):
    - 1.8 / 2.2 / 1.7 s at 1 km, 2.0 / 3.1 / 2.0 s at 2 km, 2.1 / 6.8 / 2.4 s at 4 km.
    - The towers grow most, from their 3 m pool search.
  - To grow past about 1.1 km, either:
    - Use more samples: 1,261 a side (10 x 10 components) at 2 km, or 2,269 (18 x 18) at 4 km. The builder's masks
      and its component check must match, and it costs about 3x and 9x the time and memory.
    - Or cap the side.
  - The layout's limits bind first:
    - 16 districts and 32 roads at most, while districts 150 m apart fit 36 / 169 / 676 at 1 / 2 / 4 km.
    - The 400 m shortcuts give out, leaving only the tree of roads from 1.9 km.
  - The full tables, which numbers follow the side, and the routes to playable mountain ground are in the private
    notes.
  - Gate: 832 doctest cases, ctest 4/4. Next: the flow mask's Source side once the PC is free (the entry below).
- **Earlier (2026-10-30 ~00:50): the generator follows the rain over the finished ground (`terrain-worlds` 0fca86d,
  not merged into main, which stays at 8a2da97). Core only: the maps aren't rebuilt and the ground master doesn't
  read it yet.**
  - Core (0fca86d):
    - Last of all, after the slope limit, `ftcore::generateWorldTerrain` passes each sample's rain, and all that
      reaches it from uphill, to every lower neighbour, most of it the steepest way (shares as the fourth power of
      each drop over its length), upstream first, filling nothing. A closed hollow's floor or a flat keeps what
      reaches it; the field's edge, water over the ground, a trench's floor and a pit's shaft take what reaches them
      and pass nothing on. That was the advisor's pick after a scratch comparison against the steepest way alone and
      against filling the hollows first, which drew straight lines across their levelled floors.
    - Rain falls everywhere but on the pads and the roads' shoulders and fades in round them as the wear does
      (`rainWeight`, split out of `wearWeight`, so the wear is unchanged), so they read 0.
    - `WorldTerrain::flow()` reads 0 to 1 on a log scale, 1 where a hectare's rain runs through (`kFlowUnit` about a
      sample's area, `kFlowFull` a hectare). `ftcore::flowMask` resamples it into a byte mask over the whole field,
      pixel centres as `shapeMask`'s, without allocating.
  - Numbers (Solhaven, Lattice, Unwritten):
    - The pass adds 45, 100 and 104 ms to generation in an optimized build and is deterministic. It agrees within
      6e-8 with a plain reference sorted by height.
    - In a 1024 x 1024 mask, inside the square 0.56, 0.59 and 0.28% of the pixels read 200 or more and 0.005, 0.001
      and 0% read 255. The hollows (the shape mask's 96 or less) read 2.39, 2.52 and 1.78 times what the crests (160
      or more) do.
  - Checks: a new test (test_terrain.cpp) checks that every sample reads 0 to 1, and exactly 0 on the pads and the
    roads out to their shoulders; that each mask pixel is the flow read straight between the four samples round it;
    that 0.1% of the square at least reads 200 or more and 1% at most reads 255; that the hollows read at least 1.5
    times what the crests do; that the mask writes every byte without allocating; and that the ruins, generated
    again, give the same flow. Gate: 832 doctest cases, ctest 4/4.
  - Written but not committed, in `.claude/worktrees/flow-mask` (4 files under `Source/`), waiting for UBT:
    - `FTBuildExploreWorld` keeps a 1024 x 1024 `T_FlowMask` beside `T_ShapeMask` and hands it to the map's
      instance. Its log line gives the share of the field at half or more.
    - `M_WorldGround` gains a `FlowMask` input (the master's default is `T_GroundNoRoads`, so alone it gathers no
      rain) and a Flow group. From `FlowStart` (0.4) to `FlowEnd` (0.8) of the mask the ground darkens by
      `WetDarken` (0.3), stands `WetRich` (0.3) further from its grey and loses `WetSmooth` (0.2) of its roughness.
      A crest moves `CrestPale` (0.2) of the way to a lighter grey. Only the ground layer changes, not the rock or
      the road.
  - Next, when the PC is free: UBT, `-run=FTBuildGround`, the three maps rebuilt, offscreen renders from each entry
    and on each climax, the tours and the headless story run. Tune `FlowStart` and `FlowEnd` against the renders,
    then commit the Source side with SCENE_LOOK's parameters and how it looks, push `terrain-worlds` and
    fast-forward main.
  - Not yet:
    - Each closed hollow keeps its own rain, so a basin's floor shows wet specks (1,400 to 2,500 over each field)
      rather than one channel, and almost nothing reaches 255.
    - The ground just below a pad or across a level road reads dry, with no channels spilling from them.
  - The pass after: each climax's pad falls away past the plaza's edge on its open side. Still to come: as in the
    entry below.
- **Earlier (2026-10-29 ~20:45): the big worlds' mountains are worn by rain, wear stone on their ridges and ground in
  their gullies, and keep their rock's grain from afar; the town's mountains turn grey, and each climax's plaza
  looks out over its world (main 8a2da97, a fast-forward of `terrain-worlds`).**
  - Core:
    - 754f505: `ftcore::generateWorldTerrain` wears the mountains after `range()` raises them, first on a field 4
      times coarser (gullies tens of metres across, fans at their feet), then on the full field (rills down the
      gullies' sides). Nothing inside the square moves. `TerrainOptions::erodeMountains` turns it off.
    - 8356108: the town and the towers drew their mountains from the same seed, so they stood in the same ranges;
      each kind now salts the mountains' noises with its own `rangeSeed`.
    - 8a2da97: `ftcore::shapeMask` bakes the ground's shape over the whole field: 128 where it's flat or slopes
      evenly, more on ridges, less in gullies and hollows, from how far the ground stands over two rings round each
      point (6 and 16 m out). `ftcore::generateWorldLayout` leaves out the buildings of the climax's ring that stand
      ahead of its plaza, toward the world's centre, and keeps those behind (Solhaven 9 out and 8 kept, Lattice 7
      and 8, Unwritten 9 and 6); they still draw their random numbers, so nothing else moves.
  - The ground master `M_WorldGround` (`-run=FTBuildGround`):
    - the high ground's rock follows each map's 1024 x 1024 `T_ShapeMask` in place of the 40 m mottle, so the
      ridges and upper sides turn to stone first and the gullies keep the ground longest (`ShapeLift`,
      `ShapeRock`);
    - far off, a second copy of the rock 15 times its size keeps the photo's cracks and ledges from 300 m instead of
      its mean colour (`RockFarScale`).
    `docs/SCENE_LOOK.md` (The ground) has the parameters.
  - The town's mountains wear `worn_rock_natural_01` tinted a light, slightly cool grey; `rock_wall`, which read
    warm brown beside the grass, is retired with its textures (12 surfaces, 36 textures, about 117 MB through LFS).
  - The three maps were rebuilt on the worn mountains, with the same steep shares, cuts, hidden walls and props left
    off as before, and each map is 0.3 to 0.6 MB larger.
  - How it looks, from 10 offscreen renders (from each entry and at eye height on each climax), with no material
    compile or sampler error:
    - From the hilltops the mountains round Solhaven read as grey stone worn into ridges and gullies, with the grass
      climbing the gullies in tongues; those round Lattice wear grey-brown stone on their ridges and spurs, with the
      pale gravel in the valleys. The ruins' mesas keep the rock's grain from afar.
    - Solhaven's plaza looks out through the open half of its ring on the other districts, the sea and the
      mountains, and Lattice's over the towers to the mountains. Unwritten's plaza fills its butte's flat top, so
      the plain stays hidden below the rim.
    - The entry views match the last pass.
  - Checks:
    - New tests: the shape mask reads 128 on every pad's core, over 128 on ridges and under it in hollows on 99.4
      to 99.8% of the strongly curved pixels, without allocating; the climax's ring leaves out the buildings ahead
      of its plaza, keeps at least three behind, and nothing else moves.
    - Every tour still visits every district in the same legs and game time as before (Solhaven 18 legs in 814 s,
      Lattice 15 in 803 s, Unwritten 17 in 992 s), with the lead's feet 5 cm over the entry's floor.
    - The headless story run (`-AlphaAutoPlay -AlphaFast`) finishes 73 scenes and 167 screens (40 battle, 91 field,
      36 stub) with 0 stuck, 0 warnings and 12 overworld hops. Its 59 placings on the three worlds set the lead's
      feet 5 to 6 cm over the district's floor, on floors from -19.7 to 36.0 m.
    - Gate: 831 doctest cases, ctest 4/4. UBT Succeeded in the worktree and in the main checkout (with
      `-DisablePlugin=Fab`).
  - Next, one structural pass (advisor): the ground's colour follows the water.
    - A Core `flowMask` bakes how much water gathers at each point of the final field (0 on the pads and roads),
      and each map keeps it beside `T_ShapeMask`.
    - The ground master tints the ground by it and by the shape: richer and darker where water gathers, paler and
      drier on the crests, the ruins' mud streaked along the flow lines and Lattice's gravel darker in its trench
      floors. The tint holds through the far fade, so the far grass and the ruins' plain stop reading as one tone.
    - The pass after: each pad is flat out to 62 m, so from a climax's plaza the ground below stays hidden under
      the pad's rim even where the ring is open. The climax's pad falls away past the plaza's edge on that side.
  - Still to come:
    - The mountains wear one rock photo throughout, and the sea is a still, flat plane.
    - Trees and rocks are cubes and spheres. Real meshes are an asset question: the repo is public, so only CC0
      or generated ones.
    - Scattered grass waits for the user's go.
    - The abyss and shaft walls draw as stretched quads until a cliff-mesh or fog pass.
    - No lakes are placed.
    - The clockwork relief map.
    - The three deferred looks: the gullies, the disc pads and the straight quay.
- **Earlier (2026-10-29 ~17:05): the big worlds' ground is worn by rain, its rock settles into the high ground by
  height and its detail fades with distance (main 0f4ad2f, a fast-forward of `terrain-worlds`).**
  - Core (e3ea6d0): `ftcore::generateWorldTerrain` wears each world's open ground before it raises the mountains,
    the coast, the abyss and the features (`wear()` in `Core/src/Terrain.cpp`):
    - 200,000 droplets run downhill over the square and 60 m round it, cutting where they run fast and laying down
      what they carry where they slow;
    - the ground keeps its mass, the pads, the roads' shoulders and the ruins' mesas and gullies don't wear, and
      steep ground wears less from a slope of 0.7 and not at all from 1.2;
    - a test holds the worn open ground to curving at least 1.25 times as sharply as unworn (1.48 to 1.60 now).
    Generation takes about 1.9 s a world optimized, 7 to 9 s in Debug. `TerrainOptions::erode` turns it off, for
    measuring only.
  - The ground master `M_WorldGround` (`-run=FTBuildGround`):
    - rock on the high ground: each map's instance starts it 10 m over the highest ground inside the square (45.6 m
      in the town, 46.0 m among the towers; nothing in the ruins stands that high), where gentler ground joins the
      slope rock over a 40 m band whose line a 40 m mottle moves;
    - rock and ground meet by height: each surface's occlusion map, about its own mean, stands for its height, so
      the stone breaks through first at its high points and the ground keeps to its low ones;
    - far off, from 30 to 120 m from the eye, a third copy of the ground 5 times the size takes 0.6 of its colour,
      the bumps ease to 0.4 of their strength and the fine mottle fades.
    `docs/SCENE_LOOK.md` (The ground) has the parameters.
  - `-WorldDistrict=<n>` starts the lead, and a tour, on district n's spawn and floor. With `-ExplorePortrait` it
    frames the ground from a climax's hilltop (Solhaven's district 14, Lattice's 1, Unwritten's 4).
  - The three maps were rebuilt and now stand on the worn ground. Lattice: 1.0% of the square steeper than 1 in 1,
    25 props left off (28 before). Unwritten: 78 props left off (77). Solhaven's numbers are unchanged, and every pad
    stays where it was.
  - How it looks, from 11 offscreen renders (from each entry close behind the lead, 25 m up, 60 m up and 120 m back,
    and at eye height on each climax), with no material compile or sampler error:
    - The close and mid views match the last pass; cobbles and gravel show at their real size.
    - The far grass and sand have lost their tile grain and speckle, but the grass stays one even green and the
      ruins' plain keeps one even tone to the horizon. Unwritten's worn ground rolls in soft folds.
    - From the hilltops the mountains round Solhaven and Lattice read as smooth tan and khaki slopes with broad
      green or pale patches. The far rock is one flat colour.
  - Checks:
    - Every tour still visits every district (Solhaven 18 legs in 814 s of game time, Lattice 15 in 803 s,
      Unwritten 17 in 992 s), with the lead's feet 5 cm over the entry's floor.
    - The headless story run (`-AlphaAutoPlay -AlphaFast`) finishes 73 scenes and 167 screens (40 battle, 91 field,
      36 stub) with 0 stuck, 0 warnings and 12 overworld hops. Its 59 placings on the three worlds set the lead's
      feet 5 to 6 cm over the district's floor, on floors from -19.7 to 36.0 m.
    - Gate: 828 doctest cases, ctest 4/4. UBT Succeeded in the worktree and in the main checkout (with
      `-DisablePlugin=Fab`).
  - Next, one structural pass (advisor): the mountains.
    - Wear them in Core after `range()` raises them: a second rain pass with steepness limits of its own, or finer
      ridges with scree at their feet. Their ridges are unworn noise now, the finest about 14 m across, so from a
      hilltop they read as smooth clay.
    - Give the rock a far copy like the ground's. From 300 m it shows as its photo's mean colour.
    - Replace the 40 m mottle that sets the high rock's edge with a mask the builder bakes from the ground's shape
      over the whole Landscape: rock on the convex ridges, ground in the hollows.
    - The town's `rock_wall` reads warm brown. `test_surface_table` needs every listed surface worn, so giving the
      town another rock means retiring `rock_wall` or giving it another role.
  - Still to come:
    - The building rings round each climax's plaza hide most of the view from it (a layout fix).
    - Trees and rocks are cubes and spheres.
    - Scattered grass waits for the user's go.
    - The abyss and shaft walls draw as stretched quads until a cliff-mesh or fog pass.
    - No lakes are placed.
    - The clockwork relief map.
    - The three deferred looks: the gullies, the disc pads and the straight quay.
- **Earlier (2026-10-29 ~13:35): the big worlds' ground wears photographed ground, rock and roads (main d66094d, a
  fast-forward of `terrain-worlds`).**
  - The ground master `M_WorldGround` (`-run=FTBuildGround`) blends three photographed surfaces on each world's
    Landscape:
    - the kind's ground, laid twice at two angles and sizes so neither repeat lines up;
    - rock wherever the ground is steeper than about 1 in 1, projected along the world's three axes;
    - the road surface along every road, from a road mask the builder saves with each map (`ftcore::roadMask`, a
      byte per half metre of the playable square).
    `docs/SCENE_LOOK.md` (The ground) has the parameters, the samplers and how it looks.
  - `Data/Field/surfaces.json` now dresses each kind's ground, road and rock:
    | World | Ground | Rock | Road |
    |---|---|---|---|
    | Solhaven | leafy_grass, tinted greener | rock_wall | gravel_road |
    | Lattice | gravel_floor_02, tinted a touch cooler | worn_rock_natural_01, tinted grey | clean_asphalt |
    | Unwritten | gravelly_sand | worn_rock_natural_01 | dry_mud_field_001 |
    That is 13 CC0 surfaces, 39 textures and about 120 MB through LFS. The old grass_ground is gone.
    `FTBuildSurfaces -Only=<id>,...` rebuilds just the named surfaces.
  - How it looks, from offscreen renders:
    - Solhaven's grass reads greener, though lime rather than a lawn's green. From a road, the gravel and the
      cobbles show at their real size. The far view shows the roads running between the districts.
    - Lattice's ground is pale, cool gravel, and its asphalt roads read from afar. The mountains' steep faces wear
      grey stone. A dark rock photo came first, but it read as holes even at the tint's cap, so it was swapped.
    - Unwritten's sand shows no repeat from 25 m up or from afar. Its mud roads stand out, and its mesas and a
      pit's funnel show.
  - Checks:
    - The maps were rebuilt on the same ground. Every tour still visits every district (Solhaven 18 legs, Lattice
      15, Unwritten 17), with the lead's feet 5 cm over the entry's floor.
    - The headless story run (`-AlphaAutoPlay -AlphaFast`) finishes 73 scenes and 167 screens (40 battle, 91 field,
      36 stub) with 0 stuck, 0 warnings and 12 overworld hops. Its 59 placings on the three worlds set the lead's
      feet 5 to 6 cm over the district's floor, on floors from -19.7 to 36.0 m.
    - Gate: 827 doctest cases, ctest 4/4. UBT Succeeded in the worktree and in the main checkout (with
      `-DisablePlugin=Fab`).
  - Not now (advisor): a height blend between rock and ground, a fade of the ground's detail with distance,
    anti-tiling in the shader, and a second ground surface.
  - Still to come:
    - Trees and rocks are cubes and spheres.
    - Scattered grass waits for the user's go.
    - The abyss and shaft walls draw as stretched quads until a cliff-mesh or fog pass.
    - No lakes are placed.
    - The clockwork relief map.
    - The three deferred looks: the gullies, the disc pads and the straight quay.
- **Earlier (2026-10-29 ~11:15): the three big world maps stand on that ground (main 82b03b6, a fast-forward of
  `terrain-worlds`).**
  - `FTBuildExploreWorld` imports each world's height field (the entry below) as a Landscape at Z scale 100: a
    vertex per sample, 6 x 6 components of 2 x 63 quads over 1.5 km, so every height lands exactly. The material
    and scale go on before `Import`. `ForgottenThird.Build.cs` adds the Landscape module. `docs/WORLDS.md` covers
    the maps on the ground, the build's log lines and the feet log.
  - Plazas, buildings and landmarks stand on their district's pad, a building sunk 0.5 m. The road strips are gone,
    because the roads are now the ground's own cuts. Trees and rocks stand on the ground, sunk as far as the slope
    falls across their foot. None stands in water, within 3 m of a cut, or on ground steeper than 0.8.
  - Hidden walls run along the square's edge and round each blocked cut (four round a trench, one per ray round a
    pit). They stop pawns, but not the camera's arm or the Visibility traces the followers and guards use. The sea
    is a dark, glossy plane out past the horizon, and each pool is a plane at its level.
  - Each district's navmesh bounds sit on its pad and reach 30 m either side. All three worlds keep 136 goals
    reachable from each district.
  - The builder saves each district's floor in the director's `DistrictFloors`. The director uses it to:
    - set a story step's lead down on the district's floor;
    - stand a field goal's marker on it;
    - aim the autopilot at it.
    A second after each placing of the lead on a world map, story steps included, the director logs the lead's feet
    against that floor.
  - Ground per world:
    | World | Height range | Steeper than 1 in 1 | Features |
    |---|---|---|---|
    | Solhaven | -33.0 to 145.7 m | 0.0% of the square | the sea |
    | Lattice | -38.7 to 146.7 m | 1.2% | 2 trenches, 3 pools |
    | Unwritten | -185.5 to 36.0 m | 2.8% | 2 pits |
  - Every tour visits every district on the new ground: Solhaven 18 legs, Lattice 15, Unwritten 17.
  - The headless story run (`-AlphaAutoPlay -AlphaFast`) played 73 scenes and 167 screens (40 battle, 91 field,
    36 stub) with 0 stuck, 0 warnings and 12 overworld hops. It logged 59 placings on the three worlds, on floors
    from -19.7 to 36.0 m, with the lead's feet 5 to 6 cm over the floor every time.
  - Gate: 825 doctest cases, ctest 4/4. UBT Succeeded in the worktree and in the main checkout (with
    `-DisablePlugin=Fab`).
  - How it looks: offscreen renders show smooth hills and mountains, with no material, shader or Landscape errors.
    It still reads as flat colour, though:
    - Solhaven's grass photo is dry, so it shows as khaki from any distance.
    - Lattice and Unwritten keep flat tints.
    - Until the ground master lands, a road is only its cut.
  - Next, now landed (the entry above): a ground master on world axes. It draws rock on steep ground by slope, the
    roads from a mask, and the grass in broad patches. The towers and ruins kinds also get photographed surfaces for
    ground, road and rock, picked by measured colour.
  - Still to come:
    - Trees and rocks are cubes and spheres.
    - Scattered grass waits for the user's go.
    - The abyss and shaft walls draw as stretched quads until a cliff-mesh or fog pass.
    - No lakes are placed.
    - The clockwork relief map.
    - The three deferred looks in the next entry: the gullies, the disc pads and the straight quay.
  - To watch a world tour itself, drop `-NullRHI`, `-AlphaFast`, `-unattended` and `-nosound` from the tour line
    in `docs/WORLDS.md`:
    `UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/World_Solhaven -game -WorldAutoPlay -WorldQuitOnDone`.
- **Earlier (2026-10-29 ~09:25): lifelike ground for the three big worlds (branch `terrain-worlds` c14935a; in
  main since 82b03b6).**
  - The goal: realistic, lifelike terrain in each world, with no blocky land. `ftcore::generateWorldTerrain`
    (`Core/include/ftcore/Terrain.h`) shapes each world's ground from its layout and the new `sites` in
    `Data/Field/worlds.json`, which hold district indices and tier words only: one `entry`, one `climax` and any
    `major`s. `docs/WORLDS.md` has a new Ground section.
  - One height field per world: 757 x 757 samples 1.98 m apart over 1.5 km (the 1 km square and a 250 m apron), in
    1/128 m steps that a 16-bit Landscape at Z scale 100 stores exactly (`heightmapValue()`). That is 6 x 6
    Landscape components of 2 x 63 quads.
  - Pads are flat out to the district radius, roads climb in a straight line between pads at 0.25 at most, and the
    ground beside a road banks at 0.65 at most. The climax stands 36 m up where its roads allow and stays in sight
    from the entry. On a broad swell each kind has its own ground: hills and bowls in town, broader and higher hills
    among the towers, and a low plain with mesas and gullies in the ruins. Off the sheer walls nothing is steeper
    than 1.8 (61 degrees).
  - Past the edge: mountains whose foot wanders 0 to 60 m out round the towers and on the town's three dry sides; the
    town's sea behind a quay, with headlands either side; the ruins running on 16 to 114 m in bays and headlands,
    then dropping into an abyss 170 to 190 m deep.
  - Features: up to two trenches among the towers (floors 18, 24 or 30 m down, walls at 1.6 that waver and round
    off at the corners) and up to three pools 0.6 m deep; one or two pits in the ruins, each a funnel round a sheer
    shaft. Each keeps 25 m from the roads, the pads' blends and the others, and 30 m inside the walls. The only
    sheer walls (`cliff()`) are the quay, the abyss's fall and the shafts.
  - `Core/tests/test_terrain.cpp`, 14 cases. Open ground lies near multiples of 3 m (6 m among the towers) no more
    often than smooth ground does by chance (6.65, 3.32 and 7.16% against 6.67, 3.33 and 6.67%), and 0.27 to 1.78%
    of open cells are flat (5% at most). Debug generation takes 1.7 to 3.4 s a world.
  - Looked at beyond the tests: a scratch dump tool, kept out of the repo, drew hillshades and cross-sections of each
    field. The mountain foot lies a median 44 to 46 m past the edge, and the ruins' lip 24 to 115 m (median 67 m).
  - Gate: 825 doctest cases, ctest 4/4, UBT Succeeded (with `-DisablePlugin=Fab`).
  - Deferred by the advisor: the gullies read as grooves of one width (the erosion pass fixes them); the pads read as
    discs or craters from above (a change to the pads' outlines, which moves the placement tests); and Solhaven's
    quay runs in one straight line for 1 km (coves need the coast to reach inside the square).
  - The maps have stood on the field since 82b03b6 (the entry above). The roads get a mask in the ground master
    instead of a weightmap. The queries answer for lakes, but none is placed.
- **Earlier (2026-10-29 ~01:15): hair cards for the leads' grooms (main 0ea522b).**
  - `Data/Grooms/hair_cards.json` lists seven grooms (head hair, brows, beards and moustaches) with each level's
    card, triangle, texture and flyaway targets, atlas size and screen size. `ftcore::HairCardSpec` loads and checks
    it (`test_hair_card_spec.cpp`, 5 cases).
  - The opt-in editor plugin `Plugins/FTHairCards` adds `FT.HairCards [spec] [only=<id>] [quit]`. It drives the
    engine's Experimental hair card generator one level at a time, each level with its own atlas, then switches the
    levels to their cards and saves the groom; a groom with a failed level is left unsaved. It needs the full
    editor, because the generator's notifications crash a commandlet, so a run needs the user at the PC.
    `docs/HAIR_CARDS.md` has the run, the hazards and a by-hand UBT line.
  - The plugin is off by default, so the everyday editor and the other builds leave it out. `tools/gate.ps1
    -HairCards` builds it. Since the launcher's Fab 0.0.22 update, UBT also needs `-DisablePlugin=Fab`, which
    `gate.ps1` doesn't pass, so build by hand after the gate's tests.
  - The generator's Python packages (PyTorch with CUDA among them) go in `D:/projects/ue_pip/ForgottenThird`
    through `UE_PIPINSTALL_PATH`, which every worktree shares: about 5.3 GB on disk, and the first install took
    about 4 minutes.
  - First run, one groom (a moustache of 3,367 strands): 2 levels, 909 cards, 0 failed, in about 2 minutes (63 s
    and 34 s for the levels, then 14 s to save). Its first level came out at 652 cards for 500 asked, because the
    generator gives some clumps more than one card, while its triangles stayed within the budget (5,996 of 6,000).
    The cards stay local with the grooms.
  - Checked against the strands without the editor: each card drawn with its coverage texture through the
    generator's own atlas mapping, front and side. The cards look like the strands in both views. A strand point
    lies a median 0.22 mm from its card on the first level and 0.32 mm on the second, and 97.6% and 92.2% of the
    points lie within 1 mm.
  - Gate: 811 doctest cases, ctest 4/4, UBT Succeeded (with `-DisablePlugin=Fab -EnablePlugin=FTHairCards`).
  - Next, in an attended slot: the other six grooms in one full run. `only=` takes one id, so the moustache is
    rebuilt too (about 2 minutes). They have 2,123 to 90,613 strands, and the time per groom at the top of that
    range isn't measured yet. Also try a 1024 atlas on the moustache's first level and compare it with the 2048
    one: only 12% of the 2048 atlas's texels have any coverage, because the hair in each tile is thin, though the
    tiles fill the atlas. After that the cards get the hair material; until the look pass they draw with the
    engine's default cards material.
- **Earlier (2026-10-28 ~18:22): world building pass 1. Solhaven's shapes wear photographed CC0 surfaces.**
  - `Data/Field/surfaces.json` lists eight CC0 surfaces from one library: grass, gravel, cobbles, plaster, clay
    roof tiles, mossy sandstone, bark and rock. For each it gives the real width one copy covers, the resolution and
    the source page, and for each world kind which surface each role wears, with an optional tint. Only the town kind
    (Solhaven) is dressed; Lattice and Unwritten keep the flat tints.
  - Core: `ftcore::SurfaceTable` (loader, role names, lookups that allocate nothing; `test_surface_table.cpp`, which
    also checks that the committed file dresses every town role at a plausible scale).
  - `tools/fetch_surfaces.py` downloads the maps (colour, DirectX normal, AO/roughness/metal) and refuses anything not
    CC0. The photos stay out of the repo; the textures built from them go in through LFS.
  - `-run=FTBuildSurfaces -Source=<dir> -AllowCommandletRendering` builds `/Game/Rendering/M_WorldSurfaceTex`, then for
    each surface three textures and an instance under `/Game/Rendering/Surfaces/<id>/`. The textures keep their JPEG
    as their source: 62.0 MB for all 24. The master projects each photo along the shape's own axes (each
    instance's, for the instanced roads, walls and trees), so the photo keeps its real size on every shape and a
    pitched roof's tiles run down the slope. It also has the old master's mottle, plus a broad one against repeats.
  - The world builder dresses each role from the table, else the old flat tint, and logs
    `FTBuildExploreWorld: 8 of 8 surface roles wear photographed surfaces.` for Solhaven.
  - Caught by the renders: the first game run drew every dressed shape in the engine's grey default material. The
    master's code sampled with `Tex.Sample`, which a ray-tracing hit shader can't compile, so the whole material fell
    back, and the commandlet's compile check reported 0 errors. It samples through `Texture2DSample` now. After any
    change to a code-built material, grep the first game run's log for `Failed to compile Material`.
  - Caught by the renders, second: the road rendered as a mirror. The library's packed AO/roughness/metal map for
    it has an empty roughness channel (it averages 0.001; the library's separate roughness map averages 0.96).
    `surfaces.json` gives it a constant `roughness: 0.95`, which the master takes through `RoughnessOverride`, and
    `FTBuildSurfaces` now refuses a surface whose channel averages under 0.05 without one, or an override on a map
    that has its own.
  - Render check: offscreen portrait shots of Solhaven, wide (4 angles) and close (3). Both runs exit 0 with no
    `Failed to compile Material`. The cobbles read well at the right scale, and so does the tower's sandstone. The
    roof tiles run down the slopes, though their mottle is a little blotchy from far off. The plaster is smooth,
    nearly flat, and the road is matte gravel. The ground reads tan at grazing angles: it's a dry-grass photo, so a
    tint can't make it green, and pass 2's scattered grass is the fix. The plaza's near-white is the exposure, as
    before.
  - Known: the blockout roofs are cubes turned 45°, so the gable ends show the tiles turned 45°. The building kit
    replaces them.
  - Gate: 799 doctest cases, ctest 4/4, UBT Succeeded (with `-DisablePlugin=Fab`). main and the demo checkout
    are at 7cb0cbf, on top of the animation session's 873106e (foot phase after a traversal).
  - Next: pass 2, terrain in place of the flat floor plus scattered grass, stones and decals (PCG), with
    anti-tiling; then pass 3, a building kit. Lattice and Unwritten get surfaces with the same pipeline.
- **Earlier (2026-10-28 ~16:50): every map switches to a shared photoreal look; the per-world styles become an
  optional layer.**
  - `ftcore::SceneLook` (Core: 17 values with defaults, equality and `lerp`; `test_scene_look.cpp`) holds the look.
    `FTScene::SpawnLook` builds it in a map: a movable sun, a real-time sky light, sky atmosphere, volumetric height
    fog, volumetric clouds, and an unbound post-process volume (priority 0) for the camera finish.
  - `FTScene::LoadSurface` puts the blockout shapes on `/Game/Rendering/M_WorldSurface`. It's opaque and lit:
    roughness 0.8, metallic 0, and a world-space tone variation of 12 % in patches about 3.5 m across. The new
    `-run=FTBuildWorldMaterials` builds it. Without it, a map builder logs that it's missing and how to build it.
  - All 12 map builders use the look. `-Style=<spec>` still adds a style controller over it, and the
    `Demo_<pack>` maps keep theirs. The explore director restyles only a map that has one.
  - All 19 maps are rebuilt: rc 0 and no errors (the only warnings are the usual navigation ones at world init).
    The worlds log:
    - solhaven: 16 districts, 3001 instances, 88 actors
    - lattice: 12 districts, 1017 instances, 73 actors
    - unwritten: 10 districts, 2357 instances, 64 actors
    - the overworld: 3 gates, 433 instances, 38 actors
  - main and the demo checkout are at 50a1a59. Gate: 789 doctest cases, ctest 4/4, UBT Succeeded. In a headless explore walk the party
    follows, the fight is won and the walk completes, and our module logs no warnings.
  - Render check (offscreen portrait shots, new maps against the pre-switch ones):
    - All five runs exit 0: Explore wide (4 angles) and close (2), Solhaven wide (4), and the two pre-switch maps wide (4 each).
    - Before: black toon edge lines on every shape and character, a flat gradient sky and flat shading.
    - After: volumetric clouds, soft sun shadows and distance haze, and no outline on any shape or character.
    - The Solhaven plaza reads near-white under the current exposure. Its albedo (0.55) is already realistic and stays
      under the 0.8 colour clamp, so the fix is measuring the exposure per map, not changing the colour.
    - A figure standing on Explore shows in both the old and the new shots, so it isn't from this change.
  - Docs: `SCENE_LOOK.md` (new), plus the look sections of `WORLDS.md`, `EXPLORE_DEMO.md`, `STYLE_PACKS_UE.md`,
    `VFX_UE.md` and `PHYSICS_UE.md`.
  - Note: UE 5.8's Mutable plugin is out of Experimental (1.8.0, off by default; its clothing, groom and dataflow
    parts still are). Its UV-mask clip takes the same kind of mask our body-hide tools write, so it could replace
    the baked hidden body per outfit with removal at run time. A spike is queued after the animation and outfit work.
  - **UBT is blocked in every checkout since 16:02 (reported by the animation session).** The Epic launcher updated
    the engine's Fab plugin to 0.0.22, which adds an editor module, FabToolset, that the installed rules assembly
    doesn't know, so UBT stops with a RulesError. Until the engine is verified or updated in the launcher, build with
    `Build.bat` plus `-DisablePlugin=Fab` (`gate.ps1 -Ubt` doesn't pass it). The gate above ran before the update,
    and nothing under `Source/` or `Core/` changed after it. The existing editor DLLs load fine for `-game` runs.
  - Next (started): environment work on the worlds, Solhaven first: CC0 surface textures in place of the blockout
    master, then terrain and PCG scatter in place of the sphere trees, then a building kit. Later: measure the
    exposure per map (it's what makes the plaza near-white), and give each world its own look (blended with `lerp`).
- **Earlier (2026-10-28 ~15:10): two leads wear their garments in the demo, and main has the follower ledge catch.**
  - main and the demo checkout are at 6c82674: the animation session's follower ledge catch (b397de7, merged with
    main's body-mask tools in 6fcb250) and the pinhole handling below. Gate: 783 doctest cases, ctest 4/4, UBT Succeeded.
  - **One lead's tunic** is in the party config, with the body under it hidden by `body_mask.py`'s top mode
    (55323be: every boundary loop of the garment is an opening) and its face-seam band kept only at the neck
    (b52cde4, `seam_neck`).
    - A skin fleck at the shoulder front in the run stride is fixed by giving the cloth nearest the neck the face
      mesh's weights, faded out below the collar and kept off the collar panel. Taking the whole band from the
      face had crumpled the collar.
    - Known issue: a small dark serration where the collar seam meets the front piece.
  - **Another lead's trousers.**
    - Finding: in the cloth sim the waistband's length sets its stretch, not its height. It stretches to a loop of
      about 0.84 m and slides down to where the hips flare to that girth (the girth estimate reads about 1 % high).
      So the rise and the leg length are now cut into the pattern, not set by where the band sits.
    - The pick, in the stride test: p95 1.32, p99 2.33, max 4.64 (the first lead's trousers: 1.35, 2.23, 7.46);
      27 vertices within 5 mm of the body (115 before) and none within 15 mm. Bind exact on all 14 bones checked.
    - The body under them is hidden as for the first lead: LOD0 strips 11,469 of 60,816 triangles and loses no kept
      triangle (32,334 to 26,461 vertices). `body_mask.py` now reads a small extra boundary loop (a seam-weld
      pinhole under 3 cm) as a pinhole, not an opening.
    - Editor shots at the run stride: no skin through the trousers from any angle; hidden, the hole runs from the
      waist to just above the ankles.
    - This lead shows a grey chest until its top lands: it's the MetaHuman body's albedo, which the tunic covers on
      the first lead.
  - A headless explore walk: the three leads load 3, 2 and 1 pieces, both followers follow, the walk completes,
    and our module logs no warnings. Not watched at play speed yet.
  - Editor note: a spawned skeletal mesh actor in single-node mode takes its pose when its mesh is set, so set the
    animation mode and data before the mesh.
  - Next: turn the private bind script into a generic garment binder under `tools/blender`, then the second lead's
    top and the remaining lead's outfit.
- **Earlier (2026-10-28 ~09:00): the crotch poke-through is fixed by hiding the body under the trousers (one lead).
  Config only, no code change.**
  - The lead's body is now a copy of its MetaHuman body with the faces under the trousers removed, using the editor's
    `strip_lod_geometry` and a UV mask on all 4 LODs. The copy keeps the body's asset name in another folder, so the
    `<clip>_to_<body name>` lookup still finds the retargeted clips. The party config's body path points at it.
  - The mask comes from two new tools on main (4338efc):
    - `tools/blender/body_mask.py`: a headless Blender ray pass that finds the body faces the garment covers,
      keeping a margin under the waistband and the cuffs.
    - `tools/art/strip_mask_png.py`: system Python, an exact simulation of the engine's strip rule as
      `LODUtilities.cpp` has it.
    - LOD0 strips 11,561 of 60,816 triangles and loses no kept triangle, also under float jitter.
  - Editor checks at the run-loop stride:
    - With the trousers on, no skin shows from five angles.
    - With them hidden, the hole runs from the waistband to the cuffs only.
    - Forcing LOD 1-3 stays clean, and the pose is unchanged.
  - A headless explore run (the walk, then a solo start as another lead):
    - The body loads from its new path. The loader warns when a path fails, and it logged nothing.
    - 2 pieces, the clips found, both runs complete, no warnings from our module.
    - Not watched in the demo at play speed yet.
  - Notes:
    - LOD1-3 keep 183/144/96 triangles that should go. All of them sit inside the trousers on UV seams, so they
      don't show. The tool's `free=1` option cuts them to 60/48/30.
    - The strip doesn't remap morph targets, so check a body has none first (this one has none).
    - The engine refuses reduction-generated LODs.
  - Gate for the tools commit: 774 doctest cases, ctest 4/4. Source is unchanged, so no UBT. The demo checkout is
    at 4338efc, and its 6f3f419 binaries still match, since only tools changed.
  - Next: flip the arm abduction for sleeved pieces (the rest pose needs +42 degrees, not -42). Then run the same
    chain for the other two leads: re-cut, bind, stride test, import, hide the body.
- **Earlier (2026-10-28 ~08:00): the slide is on main and in the demo build, and one lead's trousers are re-cut for
  the MetaHuman body.**
  - main is 6f3f419: the animation session's slide (46bb993 in Core, 45bcdbe on the Unreal side, merged in
    6f3f419). Crouching in a sprint slides, starting from the drop-in clip nearest the body's speed, follows the real
    floor, and gets up into whatever the player is asking for. It hasn't been watched on screen yet. In the
    get-up into a sprint, one planted foot slips about 390 cm/s during the 0.25 s blend. The animation session
    takes the ledge grab next.
  - The demo checkout is at 6f3f419 with UBT Succeeded (07:41), and its local content has the retargeted slide clips.
  - **Trousers re-cut (one lead).** The pattern comes from the MetaHuman body's measurements. The waistband is
    placed higher and held still while the seams lift the panels to it, so once released it settles at the waist,
    not the hips. Eleven Blender bind variants went through the stride test. The chosen one has a 99th-percentile
    edge stretch of 2.23 (gate 2.44), a max of 7.5, and no edges over 10. It is imported onto the shared MetaHuman
    skeleton, and the bind is exact on all 14 bones checked. The check shots caught a run-loop frame rather than the
    A-pose (the check level's body was left on a clip from an earlier pass). Through that stride the waistband holds
    at the waist and both cuffs clear the feet, which is a harder test than the A-pose. The trousers are in the demo's
    local party config but haven't been seen in the demo map's lighting or at play speed yet.
  - A headless explore run with them on (the walk, then a solo start as another lead): the new asset loads, that
    lead has 2 pieces as the lead and as a follower, both runs complete, and there are no warnings from our module.
  - **Known issue then (fixed ~09:00, see NEWEST): skin shows through at the crotch in a deep stride.** Every
    weighting tried shows skin at the front inner thigh of the forward leg, next to the crotch seam; in the engine
    it shows from the side when running.
    The cause is linear blend skinning across a crotch: the crotch cloth blends both legs' weights, so it moves about
    half as far as the thigh under it. Blending over a narrower band makes it tear instead. The fix belongs in the
    engine: hide the body under the pieces being worn (a masked body material, or a body section or variant chosen
    by which pieces are on). Next session starts there, not with more weight variants.
  - Next: hide the body under worn pieces in the engine, then flip the arm abduction for sleeved pieces (the rest
    pose needs +42 degrees, not -42) and run the same chain for the other two leads.
- **Earlier (2026-10-28 morning): seamless cutscenes are on main. A story cutscene plays in place on the map the
  player is on, easing from the gameplay camera into its shots and back without a cut.**
  - main is c44cacf. Since 9af57aa it holds:
    - in place after a field step (7fbb342): the cutscene director's in-place mode, a step-host subsystem, and the
      hand-off that starts the next step on the same map; a field step after a field step stays on its map too;
    - routing (945c96c): a cutscene step in a scene placed in a built world loads that world's map and plays at the
      district's spawn, so the field steps beside it play in place; when an overworld hop is due, it plays on the
      Cutscene stage and the hop waits for the next field step;
    - followers and HUD (6c7bf5e): the followers its shots frame walk onto their marks while the camera blends in,
      the blend times come from `cutscenes.json` version 2, and the field HUD hands the screen to the cutscene's
      letterbox;
    - the stage from Core (8a0c905): the Cutscene map takes its subjects and camera spots from Core's stage marks,
      the same marks an in-place cutscene lays over a field map, so the built map is unchanged;
    - docs (c44cacf): `CUTSCENE_SCREEN.md` and `WORLDS.md`;
    - the animation session's turn fixes (ee66ab3, 36390b1) and land rolls (e48ce88, da69498).
  - **Camera sweep after a teleport, fixed.** The lead's camera boom lags behind the lead. When the explore
    director moved the lead to a district's spawn, the camera eased in from where the lead had stood and swept
    across the map, and a cutscene starting there blended in from that sweep (cuts of 46 to 452 m in the log). The
    director now settles the boom's lag on the new spot after each teleport. The lag tuning is untouched.
  - Gate: 758 doctest cases, ctest 4/4, UBT Succeeded. Alpha: 73 scenes, 167 screens (40 battle, 91 field,
    36 stub), 0 STUCK, 0 warnings, 12 hops, with 6 cutscenes in place and none on the stage. Autoplay has no
    blends, so each cut into a first shot is about 14 m.
  - Blend run (`-CutsceneBlendTest -AlphaFast`, from the first scene with an in-place cutscene; 20 scenes, 0 STUCK,
    0 warnings): all 6 cutscenes start with the view 0.0 cm from the gameplay camera and hand back within 0.7 cm of
    the lead's view. The fast clock runs the shots about 8 times faster than the walks, so its follower count says
    nothing about real play.
  - Real-time blend run (the same flags without `-AlphaFast`; 20 scenes, 68 screens, 0 STUCK, 0 warnings, ended on
    its own): all 6 cutscenes in place, every one with its walks; 12 of 12 follower walks reach their marks, none
    stays put; the view is 0.0 cm from the gameplay camera at all 6 starts and 0.0 cm from the lead's view at all 6
    hand-backs.
  - The demo checkout is at c44cacf with UBT Succeeded.
  - **Outfit pipeline proven on one lead (trousers only).** Marvelous Designer drapes the trousers on the lead's
    MetaHuman body imported in its A-pose rest (the arms-down frame leaves the arrangement points unfitted and the
    cloth falls). Blender binds the drape to the body's own armature, and Unreal imports it on the MetaHuman
    skeleton with an exact bind (0.000 cm and the same rotation on all 14 bones checked). Inner-leg vertices that
    lose their weights when each leg piece drops the other side's bones now take them from their neighbours on the
    same leg instead of the pelvis; in a Blender stride test the 99th-percentile edge stretch fell from 5.9 to 2.4,
    and what is left sits at the crotch seam. The garment is in that lead's look in the demo's local config as a
    first pass: the pattern was cut for the old body, so it is baggy, the cuffs pool on the feet, and the crotch
    still webs in a stride. A headless explore run with it on (walk, then a solo start as another lead): the lead
    loads with 2 pieces, both runs complete, no warnings from our module. It has not been seen in a lit game view
    yet. In the editor, posing a skeletal mesh actor with an animation override did nothing in this attempt, so
    the posed checks went through Blender.
  - Next: re-cut the pattern for the MetaHuman proportions, fix the crotch webbing (a tighter crotch, corrective
    weights along the seam, or a cloth asset), flip the arm abduction for sleeved pieces (the rest pose needs +42
    degrees, not -42), then the same chain for the other two leads and the full wardrobe.
- **Earlier (2026-10-28 early morning): the beat fight screen is on main, one lead's floating brow is fixed, and the
  in-place cutscenes come next.**
  - main is 9af57aa. Since 92a101d it holds:
    - the beat fight in Core (983b82b) and its screen: game mode, HUD and the generated `Beat` map (e5a4e1e, 9af57aa);
    - the arena melee in Core: rules, enemy brain, autopilot and a starter roster (19ca2e4);
    - in-place cutscene playback in Core: blend-in and blend-out, version 2 tables and a blockout stage anchored on
      the lead (b73c0de);
    - one story-step dispatcher that fills the carry, picks the map and can start the next step in place (b574c43);
    - the knight enemy renamed `husk_knight`, so no enemy borrows another game's title (299e06b);
    - the animation session's running-turn bank (9d41292), party hold (c362b77) and traversal query (1d9b953).
  - Gate: 735 doctest cases, ctest 4/4, UBT Succeeded.
  - The demo checkout is at 1d9b953 with UBT Succeeded and the Beat map built. A full alpha rerun is in progress.
  - **Floating brow, root cause:** one lead's custom brow groom had its binding's source mesh and its material slots
    pointing into the MetaHumanCharacter plugin. Checkouts without that plugin (all but the demo checkout) loaded an
    invalid binding, so the groom drew unbound, in front of the face.
    - Fix: the groom copy, its material instances, the binding and a `/Game` copy of the plugin's groom head and
      skeleton now all live under `/Game`, made by a private editor script. A plugin-less run shows no load errors,
      and the lead spawns with both grooms.
    - These assets sit in the MetaHuman folders, which stay local. They were copied by hand into both
      animation-session checkouts.
  - **Asset list for test assets:** a prioritised list of simple props and kits is in the user's GameAssets repo, at
    https://github.com/Creantics/GameAssets/blob/claude/nice-clarke-t1hlvm/ASSET_LIST.md. P0 is used now, P1 is the
    world kits and P2 can wait. It sets the delivery rules: an extra `.glb` per asset, `Glow_*` versus `Emit_*`
    emissive names, climb heights and pivots. Only that file is ours; the rest of the repo belongs to the asset
    session.
  - The animation session has the full list of animations the game needs: locomotion, turning, traversal, combat per
    lead and weapon, interactions, and cutscene idles and reactions. It is fixing the turn feel in walk and run first.
  - **Next: seamless cutscenes, in place.** A story cutscene plays on the world map the player is already on. The
    camera eases from the live gameplay camera to the first shot and back, the followers walk to their marks, and
    input returns at the end. It lands in gated slices:
    1. director in-place mode, a cutscene host subsystem and the field-to-cutscene hand-off;
    2. a cutscene step that arrives on a fresh map load, and routing a cutscene to the world it is placed in;
    3. the HUD and the followers;
    4. the stage-mark table read from Core.
    After each slice the alpha must still show 167 screens, 12 hops, 0 STUCK and 0 warnings.
- **Earlier (2026-10-27 late night): the pause menu, items and the combat outline are on main. The demo checkout is
  built and checked.**
  - main is 92a101d. It holds:
    - items in Core (5effafd): inventory, item table, enemy drops, `Battle::useItem`, shops;
    - the pause menu on every gameplay screen, plus Quit on the title (61d29df);
    - the combat outline (06b58dc);
    - the animation merge (46dc1d3).
  - Gate: 613 doctest cases, ctest 4/4, UBT Succeeded. One warning predates the branch: C4686 at
    `FTStorySubsystem.cpp(265)`.
  - The main checkout was fast-forwarded and built.
    - A full-story alpha run there passed: 73 scenes, 167 screens (40 battle, 91 field, 36 stub), 0 STUCK, 0 warnings,
      12 overworld hops.
    - The party check (walk and solo) shows each lead on its full retargeted clip set from
      `Content/Characters/<Lead>/RigReal3/Retargeted`, with no missing-clip warnings.
    - The demo goes to the user once the animation session's last locomotion fix is on main.
  - **Licence:** the `RigReal3` retargets and `Content/Characters/UEFN_Mannequin/` come from Epic's Game Animation
    Sample (Fab Standard licence), which bars publishing them. They stay local:
    - Each `RigReal3` folder holds a local `.gitignore` of `*`.
    - The mannequin folder is excluded in `.git/info/exclude`.
    - A fresh clone falls back to `Rig/Retargeted`, with a "7 of the 9 base clips" warning.
    - Arena enemies on the mannequin must degrade to a capsule or a hidden mesh, with one warning, when it is missing.
  - Combat: each world names its system with the `combat` key in `worlds.json`. Solhaven gets the arena melee, lattice
    keeps the rhythm battle, and unwritten gets the beat fight.
    - `docs/COMBAT_SYSTEMS.md` is the design. `CombatKind.h`, `ArenaFight.h` and `BeatFight.h` are the contracts.
    - Two Core workers are filling in the implementations, data files and tests.
    - **Beat host (next):** a generated `Beat` map on the QTE screen's pattern: a Core `BeatScreen`, plus a thin game
      mode and HUD.
      - It runs on song time when a cue with a tempo plays. Otherwise it uses the pause-aware wall clock, as it always
        does under `-nosound` and `-AlphaFast`.
      - In-place staging on the world map is a follow-up.
    - **Arena host:** fought in place on the world map, so the animation session builds it, since it owns the explore
      character and director.
      - Clips play in place with root motion off, and Core's velocity moves the bodies.
      - Each attack's play rate puts its contact frame at the start of the active part.
      - Routing is the integrator's: a combat field in the story carry, set by the story director. Its supported mask
        stays rhythm and beat until the arena host lands.
  - One lead's body was conformed to an approved lead's built body, with only the calf constraint re-pinned. Its torso
    now matches within 0.1 mm, which removes the chest hollow.
- **Earlier (2026-10-27 evening): all three MetaHuman leads are finalized and play as a party in the explore demo.** One lead's body is being conformed to an approved lead's built body, because measurement constraints don't pin front-to-back depth. Next: base outfits in Marvelous Designer (needs the user present), then per-lead animation. Details are under "Characters: quality" (2026-10-27 evening).
- **Earlier (2026-10-27 ~11:20, face round 6 scored; ops now sized from measurements; animation on the new bodies):**
  - Round 6 (sign-fixed field ops plus look variants on contact sheets): only the unrigged lead's fit reached the renders.
    The two finalized leads render their face rig, so their fits never showed (see the rig note below). Their round 6
    is held back. Look picks from one reader per criterion: one lead gets a new hair groom, another a new skin texture
    and iris tint; the other slots keep their base.
  - New method (advisor ruling): readers find which feature is off; measurement sets the direction and the size.
    MediaPipe landmarks on the sheet and on the render give ratios. Normalise by a distance no field op touches
    (nasion to chin, after backing out the known chin op) to decide whether IPD or face width is the trustworthy
    anchor. Calibration: chin op CH 1.0 is about 1 cm of chin drop; eye op EM 1.0 per eye is about 2 cm of eye
    spacing. Back out pitch differences by rotating the sheet landmarks before you compare verticals.
  - Round 7 is built from those sizes: one lead's eyes move closer (spacing 7.85 to 7.1 cm), one lead's chin comes up
    0.6 cm with a thinner lower lip, and one lead drops round-5 ops that widened the mouth and opened the lids.
    The editor run does a lens test first (the same front view at 2x distance and half FOV, to see whether the
    render camera inflates nose and mouth widths), then unrigs the two finalized leads, fits, and renders.
    `fit_vertices.py` takes `"unrig": true` in its args. Re-finalize (re-rig) once the faces pass.
  - Animation: the set is retargeted onto all three new MetaHuman bodies, and a lead plus two followers run in game
    facing forward at 450 cm/s. The follower stop lean and stride sync are in the animation session's notes.
- **Earlier (2026-10-27 ~09:15, face round 5 regressed, round 6 queued, clothes chains scripted):**
  - Round 5 (field ops scaled up) scored flat or worse (about 4.9-5.5/10). Cause: several field ops had their sign
    backwards (one widened a jaw that readers wanted narrower), and look items (hair, brows, iris, skin texture) were
    never touched. Round 6 fixes the signs, then sweeps groom/iris/skin variants on a contact sheet with one reader per
    criterion before a full reader round. Check each op's direction on a diff image before spending readers.
  - Field-op signs (fields_r2.py): CW/JW + = wider at cheekbone/jaw, MW + = mouth narrower, CT + = chin tip wider,
    BU_* + = brows up, CH + = chin longer, LD + = upper lid down, ET + = outer corner up, NA + = alae narrower.
  - One lead is finalized (body, rig, assembled, FBX export). Its clothing-sim avatars (arms out at 42 deg, plus the
    rest pose) are exported, and the cloth chain (trousers, boots, tunic, sash, then pose to rest) is scripted. A
    second lead's chain (slim trousers, low shoes, a new V-hem jacket script) is scripted too. Both need the cloth
    tool run with the user present.
  - Sequence scripts: substitute the lead with a placeholder token (`@L@`), never the bare word, or the substitution
    also rewrites the `LEAD_DIR` key. Stopping a background shell does not kill its child `bash`; check `ps` before
    restarting a sequence.
- **Earlier (2026-10-27 mid-morning, lead face round 3 scored):**
  - Round 3 (fourth fit pass, brows and eyelashes added to every lead, one reader per criterion) scored flat at about
    5.4/10 on all three leads. Readers called new and old "nearly identical" except where a groom slot changed. The
    fitted vertices moved at most 2.4 mm, below what a reader sees. Round 5 scales the field ops 3-5x (up to about
    1 cm), aimed at each lead's lowest criterion, and checks a diff image before spending readers.
  - Groom slots: check that Eyebrows and Eyelashes are filled on every MetaHuman Character. Two leads had empty slots
    that rendered as missing brows or lashes.
  - Persisted look: iterating a character's look writes makeup and accents into the asset itself, so later renders
    keep it even when the run's options omit those keys. Set the full look explicitly in each run's options.
  - The editor was handed to the animation session for a UBT build and capture; face passes resume after.
- **Earlier (2026-10-27 morning, lead face round 2 scored):**
  - Second-round fits and a facial-hair rebuild ran on all three leads, then a blind per-criterion reader round (one
    reader per criterion, old against new). Two leads improved; the third regressed and goes back to its first-round
    state. Eyes and brows are the weakest criterion on every lead, so the next pass is brow grooms plus eyelid shape.
  - Groom findings (MetaHuman Creator capture):
    - Some stock groom templates never render strands in a capture render; only their bake texture shows. Before you
      build a custom groom, sweep the stock items of that slot and build only on a template proven to render.
    - A template with a hair-width override ignores a per-strand width change. Set `rendering.hair_width_scale` in the
      build options instead.
    - A groom rebuild must quit the editor and render in a fresh one. Rendering in the same session hit a HairStrands
      assert.
  - Editor launch: the "Wait for ZenServer?" modal can block a scripted launch. The window watcher confirms it with
    Enter, the same way it handles the memory dialog.
  - Details are in the private handoff.
- **(2026-10-27, the three MetaHuman leads):**
  - Main fa4104a: hatching is off until the shader pass; `-StyleHatch` brings it back.
  - Face refinement runs on all three MetaHuman leads at once: measured face fields (cheek, jaw, mouth, nose, lids,
    brows) against each concept sheet, a vertex fit, then a render and a re-measure. The fit lands about 63% of a width
    field in render space, so the second-round fields are sized up to match. Readers wait for the second fits. Body
    proportion changes go to the advisor first. Details are in the private handoff.
  - Two editor findings:
    - An auto-rigged MetaHuman character renders its rig's mesh, so a face fit on it commits but doesn't show. Remove
      the face rig first (`remove_face_rig`), fit, then re-rig at finalize.
    - MetaHuman Creator opens a modal "Not enough memory" dialog when it loads an auto-rigged character with less than
      10 GiB of free commit. The dialog blocks scripted runs. A window watcher confirms it with Enter (the default
      button is Yes), and the system-managed pagefile then grows. Closing each MetaHuman asset editor after use keeps the
      editor's footprint down.
  - Clothes for all three come after the faces, bodies and animation retarget, made in Marvelous Designer.

- **NEWEST (2026-10-26 late, characters and art direction):**
  - Art direction: one photoreal base for every world. Each world gets its own identity from shader and post-process
    differences (grade, light, atmosphere, material tweaks), not separate drawing styles. The style-pack system
    (`ftcore::StylePack`, `FTBuildStyleMaterials`, `AFTStyleController`) carries that layer once its packs are retuned.
    The stylization backlog (cel bands, hatching, hulls, ink lines) is dropped.
  - The Third moves to a MetaHuman head and body. A test conformed a fresh MetaHuman to a mesh made from the new realistic
    reference sheet (used as a shape target only), and in a blind per-criterion check it beat the previous MetaHuman build
    on every shape criterion. The face still needs a refinement pass. The robe continues on the MetaHuman body, after a
    body retune: the body need not match the old avatar; its proportions are set to suit the new head (queued behind the
    animation fixes, because the editor blocks their builds). Details are in the private handoff.
  - Animation gate round 1 ran (per-criterion readers on the contact sheets, a baseline well under the bar). The fixes went to the
    animation session: rebuilt jump, start and stop phases, a layered turn driver, a run upper body, follower spacing, and a hood
    binding fault. Details are in the private handoff.
  - Animation round 2 was a fix check on three criteria (run gait, turns, follower), not a full gate round:
    - The follower improved clearly and no longer overlaps the lead.
    - Run gait and turns did not move, because the retarget is now faithful to the mocap (within about 3 degrees) and the
      source clips are the ceiling.
    - Ruling: matching the mocap is not a pass; the bar is the reference game. The remaining defects are fixed by clip
      selection, pose overrides or a procedural layer:
      - run posture (spine pitch, elbow hold, shorter stride)
      - a chest-led banked turn and a stepped stand pivot
      - a wider follower slot with its own timing
      - the jump, start and stop arm poses
      - the hood plate culprit
    - The body retune is unblocked by the animation session's green push, but it waits for a turn with the user at the
      machine (it needs the editor).
- **NEWEST (2026-10-26, motion tools): SOMA-skeleton motion (NVIDIA Kimodo and MotionBricks) retargets onto the
  MetaHuman Third, headless.** It was a trial run, and nothing generated is in the repo.
  - Both tools output the 77-joint SOMA skeleton.
  - Path:
    - The BVH goes through Blender -b into a skinned GLB.
    - `FTImportCharacter`, then `FTBuildRig` on that mesh. Auto-characterize uses the Mixamo template (score 0.86) and
      gives 20 chains.
    - `FTBuildRig -Mesh=<MH body> -RetargetFrom=<SOMA IK rig> -Clips=<full anim path> -AlignPose`.
  - Main 2c65d20: `FTBuildRig -AlignPose` gives the target a retarget pose aligned to the source chains
    (`AutoAlignAllBones`, ChainToChain). It's off by default. A T-pose source onto the A-pose MH body needs it, or the
    arms cross the body. UBT Succeeded and ctest is green.
  - Checked in engine with UE's AnimPose Python API (world bone positions, source vs Third), all within body scale:
    knees forward in a squat, hand swing, foot lift, pelvis travel.
  - Gaps:
    - SOMA's `Neck1/Neck2` aren't in the Mixamo template, so the Third's neck holds the bind pose. A SOMA fallback in
      `kChains` would fix it.
    - The 8 MH metacarpal chains have no source.
  - Later the same day, Kimodo text-to-motion ran locally (Kimodo-SOMA-RP-v1.2) from plain prompts:
    - A 5 s walk: 6.25 m, clean alternating foot contacts. It went through the same chain onto the Third: 20/29
      chains, pelvis travel ratio 0.85 as with MotionBricks, toes above the floor.
    - A 3 s crouch-and-jump: hips 0.72–1.42 m, 0.67 s airborne, which shows the prompt steers the motion.
    - Setup notes:
      - The LLM2Vec text encoder (Llama 3 8B, bf16) runs on the CPU, about 16 GB RAM.
      - The Llama 3 8B Instruct base came from NousResearch's ungated copy; its shards, tokenizer and config match
        Meta's by LFS sha256. It's under the same Llama 3 licence.
      - Kimodo's `TEXT_ENCODERS_DIR` points at local McGill adapter copies whose `adapter_config` names that base.
      - Kimodo's foot cleanup needs its `MotionCorrection` C++ extension, installed from the repo folder.
    - The LLM2Vec adapters are verified loaded, despite load-report warnings about a key-prefix mismatch on the first
      pass:
      - mntp is merged: layer 0 o_proj equals base + 2·B·A within bf16 rounding.
      - The supervised LoRA tensors equal the adapter file exactly.

- **(2026-10-26, overworld): a small walkable overworld joins the three worlds; the story walks it between
  worlds.** Design by the advisor; the technique only (a miniature map walked between locations), no borrowed names.
  - Main b45cddb (Core): `ftcore::OverworldLayout` in `Core/include/ftcore/Overworld.h`, plus the `"overworld"` entry in
    `Data/Field/worlds.json`. One node per world on a ring: a gate, with the world's emblem (small blocks round a
    landmark) 26 m behind it. Straight roads join the gates, 150-250 m apart. Also `overworldRoute`, and the open rule
    (`worldFirstScenes`, `worldsOpen`). 7 tests.
  - Main e1267c2 (Unreal):
    - The story director opens the Overworld map when a field step is in another world than the last one, with
      `FFTStoryCarry::FromWorld` set.
    - The explore director's `bOverworld` mode walks the hop gate to gate and enters the world's map at the gate.
      Free walk enters open worlds. `-OverworldAutoPlay` tours every gate. The HUD has an OVERWORLD panel.
    - `FTBuildExploreWorld -World=overworld` builds it.
    - The alpha line now ends `, <h> overworld hops`. `gate.ps1 -Alpha` needs at least one once `Overworld.umap`
      exists.
  - Main 430ea10: arrival within 4 m of a gate (as the world tour) and a free walk when the carry's world is unknown.
  - Main 8afd6e0: `Content/Maps/Overworld.umap` built (0 errors, 0 warnings: 3 worlds, 3 roads, 256 props, 482 m).
    `test_overworld` now counts hops as the director does (field-routed steps only; it counted battle and stub scenes
    before and said 19). `FTMeshFacing` falls back to foot->ball bones when a body mesh has no eye bones (MetaHuman
    bodies keep the eyes on the face mesh).
  - Checks, all green and pushed: UBT Succeeded; the tour (all 3 gates in 2 legs, 117.5 s game time, 16 s real);
    `gate.ps1 -Alpha`: `FINISHED 73 scenes, 167 screens (40 battle, 91 field, 36 stub), 0 STUCK, 0 warnings,
    12 overworld hops`. Numbers are in `docs/ALPHA.md` and `docs/WORLDS.md`.
  - Open: the look needs a human eye (nothing was rendered; NullRHI only).

- **NEWEST (2026-10-26, worlds): three big worlds (solhaven town, lattice towers, unwritten ruins), each 1 km a side.**
  Approach set by the advisor: Core first, then one generated map per world. Each map is a blockout plus the world's style
  pack, built from instanced meshes. There is no World Partition, terrain art, streaming or fast travel yet.
  - Main b9e8e04 (Core): `ftcore::WorldLayout` and `Data/Field/worlds.json` (ids and numbers only). Districts are placed
    deterministically and joined by straight roads. Each district has a plaza in the goals.json frame, so every field
    goal plays in any district. Also buildings, landmarks and props. 7 tests; the median run between neighbouring
    districts is 30/34/38 s at run speed.
  - Main 7cb7965 (Unreal):
    - `FTBuildExploreWorld -World=<id>` (`FTWorldMapBuilder`) builds `World_<Name>` with about 90 actors and
      1–3k instances. It builds a navmesh per district and fails if any field goal can't be reached from any
      district's spawn.
    - The story director opens a placed scene's world map and carries the world, district and style. The explore
      director moves the goal into the district and spawns the player there.
    - `-WorldAutoPlay` walks a road tour, and the HUD has a WORLD panel.
  - Main e4becea: the Lattice and Unwritten maps plus `docs/WORLDS.md`.
  - Checks: ctest and UBT green. The alpha gate is green: 73 scenes, 0 STUCK, 0 warnings, with field steps on the world
    maps. The Solhaven tour covers 16 districts in 18 legs, 1108 s game time (the autopilot walks; running is about
    half that). Nothing has been looked at on screen yet: scale, landmarks and the anchor need a human look.
  - The overworld followed (entry above).

- **NEWEST (2026-10-25, combat): one full set-piece fight, the Brass Regent (models set aside again; the user is on the
  machine, so no computer use or GUI).** Scope set by the advisor: enemy phases, scripted skill patterns, results. Items,
  guard, flee, status effects and waves/adds were left out; **waves/adds are the next combat step**.
  - Main bd7ae02 (Core): `phases` on an enemy in `Data/Battle/enemies.json` (up to 4, HP thresholds, a skill pattern each
    with an opener and a loop point, attack/heal on entry); `EventType::PhaseChanged`; `Battle::stats()` (`BattleStats`).
    The Regent (480 HP) has three phases; a Core test plays it with every pilot and each sees all three.
  - Main 6129b40 (Unreal): `FFTEnemyPhase` mirror (phases would otherwise be lost in the FromCore/ToCore round trip),
    `GetEnemyPhase`, `GetNextEnemySkill`, `GetResults` (`FFTBattleResults`). HUD: the enemy's next skill under its NEXT
    slot, a phase banner, green heal popups, an outcome panel with four stat lines plus a story win's reward, and
    press-Enter-to-continue for a player (the autopilot still continues after 2.5 s). `-BattleEncounter=<scene>,<beat>,<step>`
    plays one encounter entry headless; the Regent is `SC-P2-01,12,0`. Docs: `docs/BATTLE_DEMO.md` (Enemy phases, Results).
  - Also in 6129b40: FTFieldGuard's file-scope constants renamed; they collided with FTExploreSpot's in a unity blob once the
    adaptive build reshuffled. Keep file-scope names in Source/ unique per file.
  - Checks: ctest and UBT green; the headless Regent run (`-BattleAutoPlay -BattleQuitOnOutcome -BattleEncounter=SC-P2-01,12,0`)
    logs both phase changes, VICTORY after 41 s (perfect autopilot: 5 enemy turns, 2/2/1 per phase) and the results line.
    Not looked at on screen yet (the HUD layout of the banner, NEXT skill and results panel needs a human look).
  - Next queued by the user: three worlds, each big. Ask the advisor for the approach first.

- **NEWEST (2026-10-25, latest): lead heads rigged headless from generated open-mouth busts (user resumed head-only rigging in
  Blender `-b`; MetaHuman set aside; no body yet).** `tools/blender/rig_head.py` prep -> `face_transfer.py` -> `rig_head.py finish`
  gives the canonical skeleton plus the 56 face shapes on a bust that already has eyes, teeth and tongue as loose parts.
  - Main fc4b2a3: the open mouth is found by its notch in the front profile (works for lips parted a few mm).
  - Main 9b21fe4: eye bones at the eyeball's sphere-fit centre (the vertex mean sat ~1.7 mm forward, pulled by the iris); lids
    turn whole with their own margin inside the opening, a closing upper lid rides out onto a shell through the lower margin;
    lash plates turn with their margin. Main 01bb3d6: the shut lid follows a smooth fitted lid surface (`lid_front`).
  - Results on two test heads: basis, jawOpen, eyeWide and eyeSquint pass the close-up renders. **eyeBlink is a known limit**:
    on the dense head the shut lid still folds near the outer corner; on the coarse head (lid one or two triangle rows tall) the
    shut lid zigzags. The advisor capped it after two structural passes; don't tweak it further. A real fix needs denser lid
    topology (a local remesh of the lid band before rigging) or a lid from a template, a new task.
  - The third head has no eyeballs (painted discs), so its lids can't be built; the user is asked to regenerate it with eyeballs.
  - Check renders only (the flip counts in the diagnostics didn't track what the renders show).

- **NEWEST (2026-10-25, later): character model work is PAUSED by the user while they rethink the process; gameplay work that
  needs no models continues.** Don't resume the lead models until the user gives the new process.
  - Done on main (c2bc63a): party progression. Story fights pay xp and gold (`reward` per enemy in `Data/Battle/enemies.json`,
    plus 10% per tier); the party shares one level 1-10 (`ftcore::Progression`) that raises max HP 8%, attack and defense 6% per
    level and technique every two levels; the save's `"party"` keeps both. Tiers retuned to 10% HP / 11% attack so levelled
    typical pilots end late fights near half HP and novices start losing from tier 6. Docs: `docs/BATTLE_DEMO.md` (Experience,
    gold and levels), `docs/GAME_FRAME.md`.
  - Gold has no use yet, and no HUD shows the level (needs someone to look at a screen).
  - Done on main (25e9cb3): assist options instead of QTE-grade flags (the script never branches on a QTE grade). Three new
    settings rows (QTE auto-complete, hold inputs as a press, skip choices) are mirrored onto the script's `F_SYS_OPT_*` flags at
    story start, after a load and on every change; the QTE screen reads the first two (`ftcore::QteAssist`). Alpha gate counts
    unchanged. Docs: `docs/QTE_SCREEN.md` (Assist options), `docs/DIALOGUE_UI.md` (settings table).
  - Open: skip choices is mirror-only (nothing acts on it yet); story hold beats are still timed waits, not held inputs.
  - Done on main (f6e9474): the party follows the lead on story field steps. `ftcore::PartyTrail` samples the lead's path by
    distance and places each follower (not steered) 1.6 m apart along it; `AFTPartyFollower` is a mesh plus idle/walk clips with
    no controller, movement or collision. Followers come from `ftcore::demo::kPartyNames` (the battle line-up). Alpha counts
    unchanged, one follower on all 91 field steps. Docs: `docs/EXPLORE_DEMO.md`. Not looked at on a screen yet.
  - Done on main (c1bf5a0): field guards on the exploration demo only. `ftcore::GuardSet` (idle, patrol, notice, chase, return;
    vision cone plus sense radius, gated by a per-guard line-trace bit; contact once, then a cooldown) drives two
    `AFTFieldGuard` blockout columns, tinted by state, with no controller, movement or collision. Contact opens the battle demo
    with that guard; a win defeats it, and every return to the field holds all guards off for 3 s. Story field steps spawn no
    guards, so the alpha counts are unchanged. Headless explore run: one guard fight (guard 1), COMPLETE after 26.6 s.
    Docs: `docs/EXPLORE_DEMO.md` (Field guards).
  - Open: guards are not checked on screen yet; a chase goes straight through props (placed, not steered); demo fights pay no xp;
    the log says "1 guards on patrol" after the first fight.
  - Next, in the advisor's order: pause with save slots.

- **NEWEST (2026-10-25): all three leads now conform to concept head models.** Each lead's MetaHuman head is fitted to a scaled, cut
  head model (private), not built from presets. Pipeline per lead, all private scripts driven through the editor bridge:
  1. Blender prep: mediapipe landmarks on orthographic front renders of the concept head and the MetaHuman head; scale the concept so
     pupil-to-chin matches, move its nose tip onto the MetaHuman's, then cut it down to a conform target. Cut modes combine: keep
     only the face (forehead cut height is a parameter), drop separate hair pieces (weld seams, keep the biggest mostly-light island),
     drop the ears, drop a sphere round each pupil.
  2. Import the target as a static mesh, capture a lit portrait, track 16 face curves on it, then `align_to_target_meshes` +
     `conform_to_target_meshes` with `HEAD_ONLY` and pipeline `head_only` (about 80 s), commit the A-pose and face state.
  3. Clear the Hair/Beard/Mustache slots, render front, three-quarter and profile, and build a three-row sheet (art, concept model,
     MetaHuman).
  - Findings: a target that holds only part of an ear conforms into a shrunken nub, and a full cranium with ears swells the skull and
    buries the ears. Leaving the ears out lets the MetaHuman supply whole ones. Stylised (anime) eye spacing and socket size make an
    uncanny realistic face, and pulling the pupils in doesn't fix the lids, so the eye regions are cut out and the MetaHuman prior
    keeps realistic eyes. A forehead cut just above the brow leaves a visible step in profile; it now sits about 4 cm up.
  - A duplicate target asset that was deleted badly stays in the asset registry, where it exists but loads as None, so duplicating
    onto it fails ("could not open the character for editing"). Conform into a new name instead.
  - Hair is grown with `tools/blender/groom_strands.py` from a stock groom against the concept hair shell (grow, clump with spikes,
    length by root region). Side shortening has to apply at every root height, or temple roots fall in front of the ear.
  - Gate: the advisor's yes on a three-view sheet per lead. Then hair per lead, then base outfits in Marvelous Designer.

- **NEWEST (2026-10-24): the image-to-3D Devin is paused after pass 30; the leads restart as MetaHumans built from private face references.** Order: Devin's MetaHuman until the advisor signs off, then Duy's and the Third's the same way, then the base outfits in Marvelous Designer once all three are approved. Assets and references stay private (the MetaHuman content folders are excluded from git).
  - Root cause of the first two bad rounds: `set_face_model_coefficients` must be followed by `commit_face_state`, or the change is dropped and the default face renders. Skin tone changes need `request_texture_sources` before they show.
  - The face coefficient vector is `[22 regions, then per region: 1, quaternion(4), translation(3), k, k PCA weights]`. Swapping one region's PCA weights for another preset's (the eyes, brows or nose, say) keeps that region's placement. This mixes features from different presets without touching the rest of the face.
  - `translate_face_landmarks` is local (only the region that owns the landmark moves). The face model takes only 20-45% of each move, so the residual is pushed again (4 pushes leave about 2 mm).
  - Measuring: the plugin's own 2D face tracker runs on references and renders alike. Ratios normalised by the outer eye-corner span (eye spacing, eye width and opening, mouth width, lip heights, eye-to-mouth, nose width) are compared. Round 7 is within about 1% on the eyes and mouth; the nose is about 7% wide.
  - Next: one more round (lip tint, brow colour, nose width, cranium survey), then blind per-criterion readers (face shape, eyes/brow, nose, mouth, skin tone, facial hair, build; 9/10 each), then rig and import.
  - Update (later 2026-10-24): blind reader rounds are running. Round A scored 6-7 per criterion with an 8 on the nose; Round B is a variant survey rendered in one editor pass (each variant changes one thing, and the base is restored between variants, because changes otherwise pile up). Findings:
    - Swapping a groom (beard, brows, lashes) on an actor that's already spawned does nothing until the actor is respawned.
    - Body constraint changes (shoulders, arms, chest, neck) do land. Earlier rounds just moved them too little to see.
    - Skin: `u` sets how dark the skin is, `v` how pink (lower is more olive). The face "accent" redness defaults to the neutral 0.5; 0 gives green/yellow patches on the forehead, nose and chin, and about 0.3 is the safe low end.
    - Lip makeup at half opacity barely shows; the lips accent region moves the colour more.
    - Groom asset settings (curve decimation, hair width) can be overridden in memory for a variant and restored afterwards, through nested property paths.
  - Update (2026-10-24, Rounds C-D): scores sit at 6-7 per criterion with best variants at 7-8. Findings:
    - Body constraints were only ever switched on, so some sat on their limits; the body config now takes `[active, value]` to switch one off.
    - The body texture index renders pixel-identical across values; it is not a lever.
    - Hiding the Body component does not clear the chin at eye level: the MetaHuman face mesh itself runs down to the upper chest, so chin/trap overlap has to be fixed through the body shape (neck length, traps).
    - The stock beard grooms are each a single hair group, so their cheek stubble can't be dropped per group. **Custom groom route:** a beard wardrobe item points (through `PrincipalAsset`, whose runtime identifier refreshes on save) at a groom binding authored on `SKM_Groom_Head_Legacy02`; the pipeline transfers it onto the character's face. So a groom grown on that head mesh, imported, bound with source = target = that mesh, and referenced from a duplicated wardrobe item works on any MetaHuman. `tools/blender/groom_patch.py` grows such a groom from band masks (Alembic out); the private build script copies a stock groom's materials and rendering settings onto it.
  - Duy's and the Third's starting faces come from the same preset-blend fit, driven by tracker ratios on their references. The tracker-to-landmark offset is calibrated on Devin.
- **NEWEST (2026-10-22 late night, main 701e206): new yoke and collar.**
  - `garment_shells.py` has a `drape` kind: cloth laid over the shoulders from above, wrapping down the back onto the nearest surface. The collar has a `neck` search radius and finds its `over` rings with their own cap.
  - The private garment_fit gained two options:
    - `--still`: thin over-cloth keeps its draped shape and moves only smoothly.
    - a tuck: under-cloth goes back inside the cloth laid over it, measured as distance from the body.
  - Together these stop the yoke and collar crumpling.
  - g19 is 0.9646 / 0.9342 / 0.9575 (g17 was 0.9656 / 0.9360 / 0.9583; accepted within 0.001). The neck look is clean, except one small poke-through on the upper back that's already present before the fit.
  - Next: boots (instep, toe lip, heel, cap ridge, sole shards), then hair, textures, rig and UE import. Every garment stays a separate mesh.
- **NEWEST (2026-10-22 late, main f38a67a): lead models now start from user-made image-to-3D meshes; the MetaHuman Devin is parked.** The user's decision. The MetaHuman fit stopped at front 0.9646 / side 0.9575 against the 0.99 gate, with the boots at 0.9624 / 0.9540 in the boot loop. `garment_shells.py` gained the `boot` kind (f38a67a): rings that are the convex hull of each leg's body section plus offset, widened to the art's measured half-extents, and a sole block following the art's footprint. It works on any body rigged with UE-named calf/foot/ball bones. The first new base (private) is untextured, unrigged and has no UVs. It has about 54k triangles and some non-manifold seams at the neck. Raw silhouette IoU with `--frame height` is 0.74 front / 0.83 side, before any fit, so it can't be compared with the fitted 0.96. With `--frame box`, the wide arm span shrinks the render, so use `--frame height` for A-pose scans. Route: repair or retopo, `prep_scan.py`, `rig_scan.py` (canonical skeleton, UE bone names, so the garment tools carry over), `face_transfer.py` (the 56 face shapes onto any scan), body fit, then garments as separate meshes. The gate is unchanged (IoU 0.99, modular outfits). Known risk from the 10-21 scan route: generated heads have sealed mouths, which tore under jawOpen, so the mouth interior needs building before face_transfer. Inputs and outputs stay private.
- **NEWEST (2026-10-22 evening, main f95cc88): the reshaped neck is in the game with the face rig kept.** The new `Plugins/FTDnaTools` commandlet `-run=FTDna` edits a MetaHuman head DNA outside MetaHuman. `-dump` writes one mesh's raw positions, `-deltas` adds per-vertex offsets through the engine's DNACalib, and `-skm` copies the edited positions into the face skeletal mesh. Findings:
  - The head mesh maps exactly to Unreal space as UE = (DNA x, DNA z, DNA y).
  - The built face mesh's LODs 0-3 are DNA LODs 1/3/5/7, in the same vertex order.
  - `DNAImporterLibrary.import_and_attach_dna` and `import_skeletal_mesh_dna` only replace the rig data (render geometry moved 0), so the commandlet writes each LOD's mesh description itself.
  - DNACalib's lower-LOD recalculation leaked up to 2.9 mm onto the scalp. Each head LOD instead samples the Blender displacement field directly: 38 mm max, nothing above 1.66 m.
  
  The result is a duplicate face mesh (the original is untouched) with the same skeleton, materials and Rig Logic data. Stage E swaps it in, and g15 (0.964/0.937/0.955) is now in the private test map. In UE the neck reads continuous under the collar. Still open: the collar/tunic top edge is ragged with small black slits, the face rig hasn't been exercised beyond the neutral, and 0.99 is not met (boots, neck band and front waist/arm gaps are the next levers).
- **NEWEST (2026-10-22 late evening): under-chin sculpt tried and held back; it needs the user's call.** The user approved taking about 3 cm off under the chin and throat. The row-wise neck shrink can't reach that edge because it's nearly horizontal, so a new private column-map sculpt lifts the jaw underside to the sheet's line. Four field shapes were tried. Gated weights tore the mouth or left a flap at the chin front. A monotone squash and a 3D-local falloff stopped the tearing but crushed the chin and lower lip (lip moved 4-6 mm against a 2 mm limit). Finding: in the side view the extra 3 cm is the chin itself, not just the soft under-chin pad, so the sheet's lower face is about 3 cm shorter than the likeness face. Closing it means a shorter chin, a bigger likeness change than approved, so it goes back to the user. Nothing downstream ran, and the shipping face is unchanged.
- **NEWEST (2026-10-22 evening): Devin outfit fit g17 at 0.9656/0.9360/0.9583 (front/back/side) against the 0.99 gate; not met, and not in the game yet.** g17 is the earlier fit plus two boots-only fit passes (boots rest move max 32 mm, p95 22 mm on top of the build). The export succeeded, but the editor crashed 30 s after the export overwrote the source FBX (access violation in the MetaHuman character editor module; the console command was not the trigger). So the test-map capture still shows the previous export. Next session: relaunch the editor while someone watches it, close the MetaHuman character editor tab (or export to a fresh folder) before overwriting, then run the stage-E import and captures. Findings: tilting the head doesn't fix the under-chin excess, so that is face shape and needs the face DNA sculpt, which waits on the user's OK because it takes about 3 cm off under the chin and throat. Even with every remaining item fixed perfectly, the side view tops out near 0.978: boots 0.94%, under-chin 0.30%, occiput 0.30% (the user's call), collar 0.22%, bare upper back 0.14% (a freeze-line decision for the user). The neck close-ups show jagged tunic and collar edges and a crumpled back collar band, so the collar needs a rebuild (a dedicated yoke piece), not another fit pass. Details are in the private handoff.
- **NEWEST (2026-10-22 late afternoon): neck unfrozen in Blender, Devin at front 0.964 / side 0.955 against the 0.99 gate; the reshaped neck isn't in the game yet.** The MetaHuman fitter can't reshape the neck: `adapt_neck` pins the lower neck to the body, and the `set_body_mesh` route needs `remove_face_rig`, which reverts the face to the archetype DNA and drops the neck about 25 cm. That route is closed, and the character asset was restored byte-exact from backup. The unfreeze was done in Blender instead: the trunk shrink gained a head mode (the face mesh's neck, nape and throat below a height follow the same per-view silhouette map, ramped, smoothed and capped). Nape forward up to 38 mm, throat back up to 20 mm, cranium untouched, bare-skin outward move 5.4 mm max. Outfit fit (front/back/side): 0.964/0.937/0.955, up from 0.964/0.937/0.947. Gate math: on these masks (about 2.4 mm per pixel) a uniform 1 px outline error alone gives IoU 0.979-0.983, so 0.99 needs a mean outline error of about half a pixel. Remaining defect shares on the side view: boots about 0.7%, neck/collar band about 0.7%, occiput about 0.3% (a round-face-versus-sheet choice for the user); front: waist and arm gaps. **Shipping gap:** the export path keeps the built MetaHuman face, because it carries the DNA/Rig Logic face rig, so Blender face edits don't reach the game. The rig-safe route is confirmed: `MetaHumanCharacterExportBlueprintLibrary.export_dna` writes the head .dna (format 2.5); Epic's public DNACalib Python bindings only read 2.1, so the next step is a small editor commandlet linking the engine's own RigLogicLib/DNACalibLib (`SetVertexPositionsCommand` with added deltas, `CalculateMeshLowerLODsCommand`, binary writer), then importing onto a duplicate face mesh with `DNAImporterLibrary.import_and_attach_dna`. Until then g15 stays out of the test map (its collar was cleared against the reshaped neck, so the old nape would poke through).
- **NEWEST (2026-10-22 midday): gate tightened to IoU 0.99; Devin's outfit fit at 0.964/0.937/0.947 (front/back/side), not met.** The user raised the concept gate to under 1% difference (silhouette IoU at least 0.99, front and side). `garment_shells.py` gained two uncommitted options: `face_below` (a shell built from body faces also takes the head mesh's faces below a height, because the MetaHuman body has a wide neck hole that the face mesh's skirt covers) and collar `hull` (each collar row sits on the angular max radius of body plus face near that height). With them the tunic reaches the neck in front. Set too high (0.85) they shred the tunic top edge. Tried and reverted: capping the clear push, ring-following neighbours, and a face-clear post-pass. Each made tears or the score worse. Measured limits: 1 px of boundary shift costs about 2% IoU, and a mask threshold change of 10 levels about 0.002. While the face and neck mesh stay frozen, side tops out near 0.987 and front near 0.985-0.991, so 0.99 needs the face/neck mesh unfrozen (a user decision). Known defects: small slits under the arms, bare traps and back of the neck, a slightly jagged tunic top, flattened boot feet. The chosen fit is exported from rest and re-imported into the private test map (bind pose exact, all materials assigned). `face_below`/collar `hull` are on main as 0f9919a.
- **NEWEST (2026-10-22 dawn, main c7ac95c): Devin's trunk shrink and outfit fixes; concept gate still not met.** A local trunk shrink below the 1.50 m head freeze line pulls the body in where it pokes past the sheet, taken back to rest through the skinning Jacobians: 23k vertices moved, rest move p95 37 mm, max 56 mm, the face untouched. The UV-seam duplicate vertices are welded (an unwelded first try opened 31 mm cracks), and every vertex is clamped to the original rest bounding box, so no axis grows. A deformation check (arm raise, squat) deforms like the unshrunk body. `tools/blender/garment_shells.py` gained `cap` (a shell's vertices on given segments go onto the convex hull of the body there and are smoothed: a boot toe cap instead of visible toes), a ball segment out to the toes, and skirt `leg_curve`. The spec series also cut the trousers under the skirt and deepened the skirt. Honest unwarped silhouette IoU (front/back/side): 0.911/0.916/0.895, up from 0.886/0.901/0.835. The gate is 0.98. At about 2.4 mm per pixel, 0.98 means a mean outline error under about 3 mm against a sheet whose ink line is 2-3 px wide, so the user rules on the threshold. Exported and imported into the private test map (bind check 0.000 cm on all ten checked bones); the garments follow an arm raise and a squat, though the rigid skirt rides up in the squat. UE gotcha: an FBX reimport with replace_existing keeps a texture asset that already exists, so re-baked textures must be reimported as their own import tasks, or the old bake shows on the new UVs as blotches. Garment textures are still soft in UE: check first whether texture compilation or streaming had finished at capture, since a close-up is blurry too; otherwise the likely cause is smart-project UVs with many tiny islands that the mips blend. Remaining visible gaps: the collar floats in front of the throat, faint toe creases on the boots, a small side skirt slit and underarm slit, open neckline, upper back above the freeze line, leg stride in the side view. Inputs and outputs stay private.
- **NEWEST (2026-10-22 morning, main a6f3a71): Devin's outfit regenerated as separate skinned shells; concept gate still not met.** `tools/blender/garment_shells.py` gained per-segment `push` (a forward/back shift), `also` (adds a limb segment's vertices near the midline, for the lats), `clear` (pushes smoothed cloth back outside the body and an optional face mesh), and skirt `squash`. Honest unwarped silhouette IoU against the turnaround (front/back/side): old outfit 0.856/0.860/0.770, regenerated 0.886/0.901/0.835. A lattice fit that scored 0.911/0.897/0.870 is void because it inflated the body. Correction: the overlays show the model is bigger than the sheet, not the other way round. Without the outfit, the current body caps out near front 0.976 and side 0.944; reaching 0.98 needs a local trunk shrink below the head freeze line. Next: that shrink with rest moves recorded, then export the outfit to the test map. The user rules on the 2% threshold. Inputs and outputs stay private.
- **NEWEST (2026-10-22 early morning): rigged Devin MetaHuman dressed in the test map; concept gate number corrected.** Devin is the MetaHuman route now (concept body, the rounder photo-version face, thin goatee groom), rigged by MetaHuman, with the outfit as one skinned mesh on the MH skeleton that follows the body by leader pose, placed in a private test level with front/side/back/three-quarter/face captures. Fixes this round: garment textures were black-blotched because Blender's bake clears the image before baking (`use_clear`), and the mips then bleed black. `tools/blender/garment_shells.py` now keeps the main-colour fill (main 34aa5f1). Garments must be bound to the body's own armature before FBX export, because a second armature copy imports as an 'incompatible Skeleton'. **Gate correction:** the user's gate is under 2% difference from the concept art, measured as silhouette IoU at 0.98 or more, front and side. Earlier front 0.989 / side 0.971 results came from a per-row silhouette warp that moved rest vertices 25-37 cm and crumpled the body and clothes, so they are void. The honest clean-mesh number is front 0.857, side 0.770 (pose only). A smoother warp (sigma 50) still moved vertices 10-14 cm and scored lower. Being tried next: a low-DOF lattice (trilinear control grid shared by body and garments). The 2% gate is not met. With silhouettes, 1 px of outline error costs about 2% IoU, so the user needs to rule on the threshold. Don't quietly loosen it. Any reported IoU must come with the max rest-vertex move and a shaded look-sheet. Open visual gaps against the concept: a stand collar (currently a flat panel), the skirt is rigid, and the trousers should be baggy and tucked into tall boots. Details and paths are in the private handoff.
- **NEWEST (2026-10-22): Devin as two MetaHumans, for a comparison.** The user asked for one version that follows the concept art and one that follows the person it's based on, and will pick the approach for the other leads from these. A route proven for both: export the face mesh (`get_mesh_data_for_conforming`), deform the head verts in numpy (jaw carve, jowls, chin, lips, lids, crown, ears), then `fit_state_to_target_vertices` + `commit_face_state`. About 98% of the intended move lands, so the face is no longer limited to the Creator sliders. Gotchas: a later face blend or sculpt overwrites the fitted state; the CUPID lips makeup turns the lips white (use NONE); Forearm 50 balloons the arm. Neither version is rigged. Scripts and details are in the private handoff (private assets, never committed).
- **NEWEST (2026-10-21 late, main bb9cf3b): Devin's head in the reader loop.** Round 8 readers scored 8.0-8.9 per criterion (gate is 9/10 each). Passes since then reshaped the jaw front, chin, nape, nostril sill and ear root. Two new tool features:
  - `face_sculpt`'s `box` op (530327f) folds an ear rim in without a fold at its root.
  - `region_shell` has `close` (438b509), which fills pinholes, and `taper` (bb9cf3b), which thins a shell to nothing at its rim. An even-thickness shell ends in a step. A taper pulls the visible edge in by about 0.3-0.7 mm, so widen the outline to match.

  Measuring lessons:
  - On a cel-shaded sheet, ink lines can't be darkness targets. Score them on position only.
  - `landmark_warp` rays aimed from below must start just under the chin, or they hit the throat.
  - The side silhouette at the nape and chin sits off the midline, so moving landmarks on the midline only digs a groove.

  Reader round 9 is running. Then Devin's rig, clothes, hands, retopo, UVs, bake and import, then Duy and the Third.

- **NEWEST (2026-10-21 night, main 50d6636): face sculpt tool; Devin's face sculpt done.** Correction to the bullet below: in 931bbfe `landmark_warp`'s lid pass lidded only one eye, because the second eye's loop wrote the unmoved positions back over the first. That is fixed in 711e74a, so the "meets the measured exits" claim below holds only from w4 on. The new check is lid aperture: height and width of the eyeball visible from the front, per eye. Devin is now 5.8-6.0 x 21.5 mm on both eyes, with the opening centred on the iris. New `tools/blender/face_sculpt.py` (085ea4c, 50d6636): a JSON spec of brush strokes, eye-relative points ray-cast onto the surface (rays that pass through a gap such as the mouth are dropped). The `ft_sculpt` brushes run on the multires top level, and the result is written back with Reshape, so the cage is untouched and the spec re-runs after any cage change. Two lessons:
  - Stroke dabs overlap at a quarter of the radius, so the real displacement is about 4x `strength`.
  - A groove doesn't make an edge. A crisp jaw needs a change of plane (`flatten` the jaw side), not a `crease`.

  Devin has a philtrum, cupid's bow, mouth line, a hooded lid (a grab drops the skin over the lid, then a soft edge) and a flattened jaw side; the midline profile moved 0.2 mm at most. Next: brow, goatee and strap blockouts as separate shells, then the shape half of the likeness gate (Opus readers, sheet-matched cameras) before retopo.
- **EARLIER (2026-10-21 later, main 931bbfe): landmark warp and cranium pass; cage stage closed for Devin.** New `tools/blender/landmark_warp.py`: a JSON spec of facial landmarks, each found by a ray from the front (or the side for the jaw) and given a move in millimetres. A 3D thin-plate spline through those landmarks and through fixed far anchors carries the rest of the face smoothly. The eyes ride the warp, and the lids can then be rotated about each eyeball centre to narrow the opening. `fit_head_to_views.py` gained a cranium width pass: the front-panel width per row from the crown down to the brow, smoothed, so the dome becomes an egg instead of a box. Rule: cage-stage likeness readers proved too noisy (they contradicted each other between rounds), so the cage stage exits on measured numbers instead: midline profile within about 2 mm of the sheet, feature widths within 1 px, and lids sitting on the eyes. The 9/10 reader gate runs once, on the finished character, with Opus readers, crops at a fixed scale, and landmark coordinates stated before scoring. Devin now meets the measured exits. Next: sculpt (philtrum, cupid's bow, hooded lid fold, crisp mandible, submental), then hair, goatee and clothes as separate meshes.
- **EARLIER (2026-10-21 late, main 7c803f4): face features and brow passes.** `fit_head_to_views.py` gained `--face eye:R,nose:R,mouth:R`: it finds the mesh's eye line (eyeball centres), nose base and mouth notch from a BVH ray-cast front profile, then moves each to the given front-panel row with a smooth vertical remap, front surface only. Rule: feature heights come from the front panel, because side panels are often drawn chin-up; the side panel gives depth only. `--turn` now uses an elliptical model, since a rounded head barely widens as it turns. `--brow D` brings the brow ridge forward until the upper lids sit D metres behind it, fading in above the eyeballs so the lids stay on the eyes (ebbed9b, 7c803f4). Devin: front 0.904, side 0.934; lids 9.5 mm under the brow. Next: per-criterion likeness readers at cage stage, then sculpt. The crisp jawline and submental are sculpt targets, not cage passes.
- **EARLIER (2026-10-21 night, main 98a833c): head fit tool; goal is now three finished leads.** `tools/blender/fit_head_to_views.py` fits the fitted body's head to a head panel: proportions from the panel, cross-sections from the base. Passes: z warp to crown/nose/chin (`--chin` is given, because a drawn beard hides the chin), one cranium scale with `--turn` to squeeze a side panel drawn slightly turned (Devin's are about 15 degrees), pitch only if it gains 0.01 IoU, jaw width, the side front edge from nose to lips plus the nape (the dome keeps the base shape), a soft grab that puts the chin 4 mm behind the lower lip, and eyes moved by a local affine of the surface. The gate is the three close-ups (round occiput, jaw into neck, chin behind lips, ears in place, no banding). The IoU floors (`--min 0.88,0.92`) are only a sanity check. Devin's head passes: front 0.891, side 0.937. **User goal (2026-10-21):** three modelled, rigged, polished leads matching their concept art, iterated with the advisor. The plan with the per-character gates (silhouette, blind likeness per criterion at 9/10 or above, topology, rig, engine) is in the private handoff. Next: Devin's face features pass, then sculpt detail, hair, clothes and accessories as separate meshes, retopo and bake, rig, import. Then Duy and the Third.
- **NEWEST (2026-10-21 11:30): the third lead's generated model is built, imported and committed.** It uses the same pipeline as the second lead with no rigid parts. A skirt reweight (`rig_scan --skirt 0.24,1.0,0.7,0.32`) keeps the robe tatters from stretching with the legs. It's imported as `/Game/Characters/Third` with 7/7 face bones and 56/56 face shapes, Leads.umap is rebuilt, and the alpha gate is green. The user cleared the likeness, so that Content is committed and pushed in main e06737e. One trap: if the warped proof face sits more than 2 cm from the scan, `face_transfer` writes empty eye shapes, Unreal drops them, and FTImportCharacter fails with "no morph target for face shape". Check the depth anchors against the side view. Known gaps are the same as the second lead's: painted eyes, the jaw tears into the beard, plus a faint nose seam and stiff legs under the robe band. Details are in the private handoff.
- **EARLIER (2026-10-21, main 0463946): the second lead's generated model is built, rigged and imported and committed.** Trellis.2 in a second local ComfyUI made the body, head, hands and hair from the concept views. A Blender projection script colours them from the sheets (now with an occlusion margin so a drawn arm can't paint the torso behind it), an assembly script grafts head and hands onto the body, then `prep_scan` → `rig_scan` → `face_transfer` → the new `attach_rigid.py` (hair and glasses). The fixes that pipeline needed are pushed in 0463946: weld before decimating, the QuadriFlow fallback, voxel-proxy weights when heat weighting fails, a jaw-only weight fix and `attach_rigid.py` (`docs/BLENDER.md`). It's imported as `/Game/Characters/Duy` and Leads.umap is rebuilt, The user cleared the likeness, so that Content is committed and pushed in main e7a96a8. **Re-import trap:** a re-import keeps the existing material instances, because `FTImportMaterials.cpp` skips any instance already parented to the project masters, so a new body texture is imported but never bound. Move the generated `Materials/` and `Textures/` that aren't tracked out of the character folder before re-importing. That was done for the current build, and the gate passes with it. The real fix is in the importer, still to do. Known gaps: painted eyes (no blink), the jaw-open shape tears below the lips, the neck shadow is baked into the albedo, and it's 150k triangles with no retopo or normal bake. No in-engine screenshot yet (no window access overnight). Details are in the private handoff.
- **Character piece-modelling test (2026-10-21, outside the repo).** Local image-to-3D (Hunyuan3D-2mv, shape only, on the 3080 Ti) made the body and each clothing piece from a concept sheet's views, then a Blender script fitted them together. Silhouette IoU against the drawn views is 0.83–0.95 for body, tunic, trousers and boots; arm wraps 0.65–0.94; the sash's thin tails fail (back 0.49). Nothing is retopologised, UV'd, textured or rigged, and nothing went into Content/. Next candidates to compare, from video research: Pixal3D Multiview and Trellis.2 (GGUF) in ComfyUI, both local. Details are in the private handoff.
- **LATEST (2026-10-21, main 846572d): Devin's head has hair and beard blockouts; the shape-half likeness gate is running.** New `tools/blender/region_shell.py` blocks out hair and accessories as separate meshes from outlines measured on the sheet (eye-relative metres). It has two kinds: "surface", where head faces inside a front outline (plus an optional side outline) are cut out, border-smoothed and solidified; and "prism", the volume inside front, side and optional top outlines, voxel remeshed. A same-scale check showed the lower face was 15 mm short (the sheet's goatee hangs from a longer chin), so the cage was reopened with landmark_warp (chin and jaw down, mouth pinned) and the face sculpt re-run. Crown-to-eye and eye-to-beard-bottom now match the sheet within 4 mm. Five Opus readers (face shape, eyes and brows, nose, mouth and facial hair, build) score the grey renders against the sheet; fixes follow from their reports. Sonnet prep agents are measuring the other two leads' sheets in parallel (PIL only, no Blender, which runs one job at a time). Inputs and outputs stay private.
- **LATEST (2026-10-21, main 30cc575): lead models restart from scratch, built from the concept turnarounds (user override; the advisor agreed).** The image-to-3D scan route stops here. Its painted mouths and grey jawOpen slab can't be fixed by patching. New tools: `tools/art/hull_from_views.py` (system Python) carves a blurred occupancy grid from a turnaround's front, side and back silhouettes. It uses superellipse cross-sections, round arm sections, and a per-(x,z) arm mask so a flared hem stays body. `tools/blender/hull_remesh.py` meshes the grid through OpenVDB, then remeshes and smooths it. `tools/blender/silhouette_iou.py` is the outline gate: per-view IoU against the sheet masks with a red/green overlay; pass is 0.95 or more. Devin's body hull passes (front 0.956, side 0.990, back 0.970). Next: head carved from the sheet's HEAD panel (`--arm-run 0`) and merged into the grid above the neck; then features built at measured landmarks (lids and eyeballs via face_patch, plus new mouth code); hands from finger tubes; retopo, texture projection from the views, rig. Inputs and outputs stay private (ft_private/devin_sculpt).
- **LATEST (2026-10-21, main 683e90a): the list after the alpha is done, except the next-lead model work.** The alpha passes on 683e90a with the same pass line (`73 scenes, 167 screens (40 battle, 91 field, 36 stub), 0 STUCK, 0 warnings`). gate -Ubt is green with 4 ctests: `ftcore_tests` (405 cases), `unreal_wrappers`, `macro_names`, and the new `ui_text_keys`.
  - **8474a09, combat balance.** A pilot model misses a fixed share of presses. Every encounter must be won by a typical pilot (75% presses, 60% defences) and the early tiers by a novice (50%, 30%). **Gap:** the party never levels, so tier scaling is kept small. Revisit this when progression exists.
  - **a677f45, first-lead face shapes,** transferred from the proof head (`tools/blender/face_transfer.py`, `docs/BLENDER.md`), so lip-sync can drive that lead. Not checked by eye in a running game yet.
  - **c2b7a05, music loop points and the conductor.**
    - `ftcore::LoopRegion` and `LoopUnwrapper` (`MusicLoop.h`): the intro plays once, then the loop region repeats.
    - The conductor unwraps the cursor across each loop, so song time stays monotonic.
    - BattleDemo takes its beat grid from the music cue when the cue has a bpm.
    - Placeholder track `MUS-KNK`: 60 bpm, -18 LUFS, loop 4–20 s, made by `tools/audio/make_placeholder_music.py`.
    - The cue import warns when a loop isn't a whole number of beats.
    - **Not covered by the alpha,** which runs `-nosound`: music playback, the loop seek and conductor binding. Listen in the editor. The loop seek is frame-granular, and a music change mid-battle leaves the conductor on the old component. Combat was tuned at 120 bpm; MUS-KNK is 60.
  - **23558e4, navmesh.**
    - `FTBuildExploreWorld` bakes a navmesh. It uses an Editor-type world: an Inactive world gets no navigation system. It also clears the async-load build lock and gives the bounds volume a brush model.
    - It fails unless all 6 spots and 6 field goals have full paths from the start.
    - Spots no longer affect navigation.
    - The field autopilot follows the path (`ftcore::advanceWaypoint`) and walks straight when there's no navmesh. Every field screen in the alpha logged a navmesh path.
  - **683e90a, localisation setup (`docs/LOCALISATION.md`).**
    - HUD strings come from `Data/Loc/en/ui.json` (52 keys, no story text) through `FTUiText`.
    - `-UiCulture=<name>` picks the culture, falling back to the language, then English. `-UiCulture=pseudo` gives accented, padded text for layout checks.
    - The game instance loads the table at launch (`UiText: culture en`).
    - **Not done:** other cultures, non-Latin fonts, an in-game language setting, and a visual check of `pseudo`. HUDs don't draw under `-NullRHI`, so the alpha can't check layout.
  - **Next:** the next lead models through the prep and rig path from bb0debd (`docs/BLENDER.md`, "Generated or scanned characters"), when the user supplies them. Also the private modelling exercise in the private handoff.

- **CURRENT (2026-10-21): the playable alpha is on main (86a24e0) and plays start to end.** `gate.ps1 -Alpha` passes: `Alpha: FINISHED 73 scenes, 167 screens (40 battle, 91 field, 36 stub), 0 STUCK, 0 warnings` (8x clock, about 7 min; see `docs/ALPHA.md`). gate -Ubt green, 382 tests. Merged this round: encounter table, per-step field data, Banter and Mini screens (maps built), battle aim with weak points, audio playback and import, voice playback by line id (`FTImportAudio -Voice`), and `FTValidateAssets` (what's left to deliver, per id; `docs/ASSET_INTAKE.md`).
- **The user is generating all art, music and voice**, from the private asset tracker. Work only on what doesn't need those assets. Next from `docs/PLAN.md`: loop points and the music conductor; balance, navmesh and localisation.
- **Done 2026-10-20 (main 864d797):** the story director plays each voice line's lip-sync track on the speaker's face (`Data/Voice/speakers.json` maps script speakers to model ids, which match `UFTFaceComponent::ModelId`), and auto-advance waits for the voice, then a 0.4 s tail. 384 tests. The alpha still passes on 864d797 with the same pass line.
- **Done 2026-10-20 (main 30348bb):** the first lead was rebuilt from the user's concept sheet, inside the user's running Blender through the Blender MCP add-on (`tools/blender/make_leads.py`; `docs/BLENDER.md` covers live builds). Blender 5.2's exporter sorts bone children by name; `ne_blender.export_rig` now keeps the canonical joint order. All three lead glbs are regenerated. rig_validate on the leads still fails R11 height and the ROM checks, as before. The first lead's ROM numbers are somewhat worse (bigger sleeves and skirt). The stand-ins were re-imported later the same day.
- **Done 2026-10-21 (main bb0debd):** the user supplied a generated model for the first lead, and it now plays in the alpha in place of the stand-in. It went through the new `tools/blender/prep_scan.py` and `rig_scan.py` (`docs/BLENDER.md`, "Generated or scanned characters"), then `FTImportCharacter -Face=0`, `FTBuildRig` and `FTBuildLeadsWorld`. The alpha passes with the same pass line. **Committed and pushed in 7cc51d4** once the user cleared the likeness. `Data/Models/devin.glb` is now the scan rig, so don't regenerate it with `make_leads.py`, which would bring the stand-in back. The old stand-in `dv_*` materials are still tracked but unused. The model has no face shapes, so there's no lip-sync on that lead yet.
- `prep_scan --retopo` (QuadriFlow plus a Cycles bake) works on the same model: 34k triangles, with base colour, ORM and normal maps. It rigs with no heat fallback, but the import uses the decimated version for now. Its PNG maps make a 14 MB glb, so try JPEG for base colour.
- The older bullets below are history.

- **Phase:** M0, the playable vertical slice, from `docs/ROADMAP.md`. Phase S (the story gate) is exited: Draft 95 passed the blind gate and scored 9.0/10 on a cold whole-script read.
- **Done in M0 so far (2026-10-20):**
  - Draft 95 is imported into newengine (commit e447462, local only, never pushed). `--check` is clean: 73 scenes and 127 of 127 inserts placed. Ten insert anchors moved with the rewrite; the Draft 93.1 meta set is frozen in `build/meta/draft93_1/`.
  - The Draft 95 VO list is built in newengine `design/forgotten_third/build/draft95/VO_Master_List.csv`: 1,680 script lines, 34 marked "REVIEW: line edited in draft95". Check those before ElevenLabs.
  - `Data/Story/` here holds the Draft 95 data (gitignored; copied from newengine `assets/story`).
  - Both waiting branches are merged to main (e4763eb leads, f711839 battle demo; main was then 505814f), and their branches and worktrees are deleted.
  - The style branch is salvaged and retired (next section).
  - **Two playable demos on main (4d23d8f):** the fight demo now fights with the Devin and Duy meshes, and a new exploration map (`/Game/Maps/Explore`, `docs/EXPLORE_DEMO.md`) plays end to end: talk to Duy, three light shards, the old gate, then an ambush that opens `BattleDemo` and returns to the field on a win. Headless autoplay passes both: `BattleDemo: VICTORY after 61.5 s.` and `ExploreDemo: COMPLETE after 23.5 s.` (field time, with the fight round trip in between). gate -Ubt green.
- **Next actions, in order:**
  1. **Done 2026-10-20:** the user pressed Play in `Explore.umap` and `BattleDemo.umap`: "ok so far". The /goal (a playable fight demo and a playable exploration demo) is met. No specific notes on the grey-characters question yet.
  2. **Done 2026-10-20 (main c6033fe):** the dialogue UI on the Draft 95 data (`docs/DIALOGUE_UI.md`). Core `ftcore::DialoguePresenter` (typewriter in codepoints, two-step press, autoplay), `AFTStoryDirector` and the canvas `AFTStoryHUD`, spawned by `AFTGameMode` on `/Game/Maps/Demo`. Headless `-StoryAutoPlayFast -StoryQuitOnFinish` plays the whole script: `Story: FINISHED after 3042 moves, 73 scenes`. The paced `-StoryAutoPlay` was checked on SC-EP-03. gate -Ubt green (184 cases). The user's Play check of the box is deferred.
  3. **Done 2026-10-20 (main d9b9dae):** gameplay-beat hand-off (`docs/DIALOGUE_UI.md`, "Gameplay hand-off"). CMB beats open BattleDemo; ENG and PLAY open Explore as a free walk ("E  Back to the story"). `FFTStoryCarry` on the game instance; a win or the walk's exit reopens Demo and the new director resumes after the beat. Headless `-StoryAutoPlayHandoff -StoryQuitOnReturn`: SC-P1-02 CMB beat 0 round trip (VICTORY 61.5 s, resumed) and SC-P1-04 PLAY beat 6 round trip (resumed). Fast full run still FINISHED after 3042 moves, 73 scenes (fast skips the hand-off). gate -Ubt green (186 cases). Not yet: beat params drive the fight or field, game-over on a loss, mini-game screens. Next suggestion: movement/staging in the story map, menus and saving, VO hookup. **Waiting on the user (optional):** Play in Demo.umap to see the box and a hand-off.
  3. Next: hand gameplay beats (CMB, PLAY) off to the battle and field maps and resume after, then movement, menus and VO playback, then the Prologue chapters 1-2 vertical slice.
  3. The Prologue chapters 1-2 vertical slice.
- **Not modelled yet in the import:** Draft 95's inline gameplay markers stay hidden notes, and its new seen/fairness/ruling-tier flags are set by no insert. Model them when the dialogue UI needs them.
- **The user is working on** concept art and voice work in parallel. Leave the ChatGPT tab, the private art folder and the voice files alone.
- **User actions open:** the leak purge, and the GitHub billing fix (see `docs/PLAN.md`).

## Style branch: salvaged

The duplicate style port (`worktree-agent-ac78bf565fd6d3d57`) is retired. Main gained what it lacked:

- `FTBuildDemoWorld -Map=<name>`, which writes `/Game/Maps/<name>` instead of `Demo`.
- `Content/Maps/Demo_{Solhaven,Lattice,Unwritten,Restored}.umap`, rebuilt with main's commandlet (`docs/STYLE_PACKS_UE.md`, step 3).
- The `ft.Style <spec>` console command, which drives every `AFTStyleController` in the world.
- Not added: the `DefaultGame.ini` always-cook entry. Add it only if a packaged build drops the style materials.

## Open issue: characters render grey in `-game` screenshots

- In a minimized `-game` run with `-ExecCmds="HighResShot ..."`, the leads, the test human and the proof head all draw in the default material: flat 0.5 grey in `viewmode unlit`. In the editor they load with 0 map-check errors.
- Three different material setups gave pixel-identical shots, so the materials are probably not the cause. The shot fires at frame 5, and a minimized window barely ticks, so the character shaders were likely never compiled. This is not confirmed.
- **Change already made:** `FTImportCharacter` now moves every imported glTF material instance onto `/Game/Characters/Shared/Materials/M_FTCharacter`. That master takes `BaseColor`, `Metallic`, `Roughness`, `Emissive` and `EmissiveStrength`, and has the skeletal mesh and morph target usages, which the Interchange plugin master lacks. Keep it: it removes a plugin dependency and the editor's load-time usage patches.
- **Next step:** open `Leads.umap` in the editor (portrait monitor) and press Play. If the colours show, the open issue was only the capture method. If they don't, compare a cooked build, which compiles shaders up front.

## Story data: Draft 95

- newengine's `tools/story_import` is the canonical importer and defaults to `draft95`. This repo's copy is stale; don't run it.
- To refresh, copy newengine `assets/story` into `Data/Story/` (gitignored).
- This branch also carries a `tools/gate.ps1` fix that finds newengine from any worktree, and Draft 63-era importer tests. Main doesn't have them. Port the gate fix when main's tests read the real script again.

## Characters: quality

- The leads are blockouts: tube bodies, procedural weights, stepped hair edges. Each character's look comes from the head proportions, hair, facial hair and glasses.
- In `Leads.umap` the jaw and tongue stay at rest, because single-node animation doesn't run the face anim instance.
- Next is a sculpt pass in Blender (`tools/blender`, headless `ft_sculpt`/`ft_preview`), then re-export with LINEAR keys and re-import.
- See the "Known limits" section of `docs/LEADS.md`.
- 2026-10-26 latest: the Third's MetaHuman body is rebuilt at a fixed 168 cm (the user's call; about 7.4 heads tall). A first retune at 183 cm with wide shoulders gave a hump of trap and upper back with no visible neck from the side. MetaHuman's Across Shoulder constraint bulks the back, while Shoulder Height and Neck Length had no visible effect, so always check a side render. The build is rigged and assembled in a git-excluded folder, and the body and face FBXs are re-exported as the Marvelous Designer avatar. Next: refit the robe in Marvelous Designer on that body (needs the user present). Values and paths are in the private handoff.
- 2026-10-27: the user chose a leaner, medium build, but said the upper back still looked big and the head hunched. The hunch comes from the MetaHuman rest pose (forward neck lean, high traps). Body constraints can't change it, and the trap yoke lives in the face mesh. Fix: a private editor step after the build lowers the shoulder girdle 2 cm (the user's pick) on the Build body and face skeletal meshes, every LOD, through GeometryScript. Read back, positions and skin weights were identical on all LODs, and the seam stays sealed. The FBXs are re-exported. In game, the forward neck lean only goes away with FTBuildRig -AlignPose; the anim session is checking the head pitch.
- 2026-10-27: animation round 3 was scored by six blind readers (one per criterion): 3-6 against a bar of 9, not a pass. The ruling: add a side-camera row and a logged spine pitch before round 4, then jump/start/stop pose overrides, foot-lock IK for the stand turn, a follower stride-phase offset, and re-skinned cloth pieces.
- 2026-10-27 later: the Third's robe refit on the MetaHuman body now holds in the rest pose. It is draped on the T pose, then Marvelous Designer's pose import carries the garment to the rest pose; the body mesh alone has no neck yoke, so the face mesh is joined into the avatar. Open: the right front panel sinks into the shoulder, the sleeves crumple when the arms drop, and the front flaps stick out. Hatch shading is off in the game until the shader pass (main fa4104a; `-StyleHatch` brings it back). New goal: finish all three MetaHuman leads the way the Third was built, then animate them, then make their clothes.
- 2026-10-27 midday: the MetaHuman face round r8 is running. Readers on r7 scored face shape lowest for all three leads (3-6 against a bar of 9). Findings, measured on the fitted mesh against the sheet's face-oval landmarks at the sheet's own head pitch (a mismatch of 12 degrees moved the rows about 0.1 of the eye-to-chin height):
  - The landmarks track the silhouette on the sheet, so mesh-against-sheet ratios with no bias are the width instrument. Face tracking on renders under-reports contours (chin and jaw by about half) and was retired as a ruler.
  - A uniform jaw widening had built the cheek bulge the readers marked. It is replaced by a per-row width taper sized offline to within 2-3% per row before any editor fit.
  - The leftover face length on one lead sits in the mouth height, not the chin, so the chin op stopped short of it.
  - The Third gets his custom hair and beard grooms back in r8, and freckles are off (the user's rule). His own width taper waits for r9, so this round's grooms read on their own.
  - Stock goatee grooms on the dense end render nothing after a 120-tick wait; r8 waits 600.
  Details are in the private handoff.
- 2026-10-27 late: all three leads are re-conformed from new high-detail head scans, decimated to 40k triangles for the conform. They are bald and beardless for now (hair comes in a later step), with freckles off on the Third. Bodies: Devin is the power build, the other two the lean build. Results:
  - The likeness gate changed. A calibration control showed the absolute bar can't be reached: two reference images of the same person score only 4-8 with the per-criterion readers, and the scans themselves score 5-7 against the sheets.
  - The new gate has two parts. First, the MetaHuman keeps the scan, judged by paired A/B with three readers per criterion and no single-reader rounds. Second, the user's named fixes are checked by eye.
  - A private distance tool (similarity ICP, then signed distance per region) shows the conform holds the cranium, brow, eyes and ears within 1 mm. The cheeks and jaw drift up to about 9 mm outward, which is the next targeted fix.
  - The conform solve settings expose model regularization and face ICP weight, a cheaper fidelity knob than a vertex fit.
  Details are in the private handoff.
- 2026-10-27 evening: the user approved all three MetaHuman faces as built and two of the three bodies. Each lead is finalized from its newest conform (look, cloud auto-rig, UE-optimised build, body FBX for Marvelous Designer, verify render) in a git-excluded folder, one editor launch per lead.
  - The explore demo plays them as a party (main 3ba16e7). The looks come from the local `Config/UserGame.ini` (`PartyLooks`: body, face piece, grooms with bindings, idle and walk clips). Tab or RB switches the lead, P or LB hides the followers, `-ExploreLead=<Name>` picks the starting lead and `-ExploreSolo` starts alone.
  - Body depth: MetaHuman body measurement constraints pin circumferences, not front-to-back depth, and the solve keeps the free directions from the state before it. A lead given another lead's exact measured values still kept a hollow between chest and belly: 3.7 cm deep in the front profile, against 1.8 cm on the approved body. The fix is the body-only conform from a template: `get_mesh_for_body_conforming_from_template` (the DNA vertex map; both are MetaHuman builds) and `get_joints_for_body_conforming_from_template` on the approved lead's built body mesh, then `conform_body_to_target` (A pose, no joint estimate) and `commit_body_state`. It left the face identity untouched (0.0 change over all 1397 coefficients); only the neck seam follows the body.
  - The conform leaves every constraint inactive at the conformed body's measurements. A built body can miss its own targets by centimetres (3 cm of underbust here), so re-pinning the whole set can pull the torso off the conformed shape. Re-pin only the measurements you mean to change.
  - Status: the first run was stopped after the conform and the re-pin, before the build, because the PC was needed. The conformed body is saved in the character asset, while the game still uses the previous build. One launch finishes it: rebuild, torso check, and a calf-only fallback when re-pinning the full set moves the torso more than 6 mm.
  - The torso check is a private front-profile script: per 0.5 cm height slice, the most forward edge crossing within 17 cm of the centre line (so the A-pose arms stay out), the chest peak in 73-80 % of the stature and the belly peak in 55-66 %, and the hollow is the deepest point under the line joining them. The body mesh stops at the neck, at about 0.822 of the stature.
  - Brow build gap: the MetaHuman Character build skips an external wardrobe brow. A custom brow needs a game copy outside the Build folder, with a transfer binding authored on the groom head mesh and bound to the built face mesh. A rebuild rewrites the face, so redo the copy after every build.
  - Built groom assets keep the common `MI_Hair` slots, so in-game brow and lash colours may differ from the verify renders.
  - Git Bash rewrites `/Game/...` arguments into Windows paths. Set `MSYS_NO_PATHCONV=1` before passing map paths to the editor.
  - Next: base outfits for all three in Marvelous Designer (needs the user present), then per-lead animation.
  Details are in the private handoff.
- 2026-10-26 evening: the Third's Marvelous Designer robe pass is parked at pass r28f (bodice/skirt split with inner ties; the sleeve fit and the front corner are still open). The user wants the MetaHuman test from the new realistic reference sheets done first, so the clothing is fitted once on the final body. MetaHuman route: a multi-view mesh made from the sheet, then the existing Mesh-to-MetaHuman bridge. UE 5.8 has no image-only route (footage identity needs depth). Details and the test gate are in the private handoff.
- Lead fit status (2026-10-22): Devin's dressed silhouette is at 0.9656 front / 0.9583 side against the 0.99 gate; details are in the private handoff. `tools/blender/garment_shells.py` (on main, e8b92aa) now stitches a neck skirt into the body's neck hole, trims level, fills small holes, and supports an outer collar (`over`, `radial`). Next steps: rebuild the boots spec, then the face under the chin.

## Fight demo: gaps

- There is no music; the conductor runs a silent wall clock, so timing is plain seconds.
- The party is the lead meshes with idle clips (4d23d8f); the enemies are still cubes, and there are no attack or hit clips.

## Worktrees to clean up

These are under `.claude/worktrees/`:

- `funny-colden-696509`, `save-codec-884678`, `suspicious-euclid-af4c31`: old detached worktrees. Check them for uncommitted work before removing.
- `handoff-next-session`: this file's branch.

## Reminders

- Ask GitHub Support to purge the cached views of commit `362873808ae789806f186b5e554748f27caeb109`.
- Run no UBT while any editor or cook is running against the same project. The fork runs cooks in the main checkout.
- Open GUI programs on the portrait monitor, unfocused: `-WinX=-1440 -WinY=-997`, then restore with `ShowWindow(hwnd, 4)` (show without activating). A minimized Unreal window doesn't render.
