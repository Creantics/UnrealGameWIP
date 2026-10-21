# Handoff: the next session

Written 2026-10-18, last updated 2026-10-21. Read `docs/HANDOFF_UE.md` in newengine first, as CLAUDE.md says. This file picks up from where that one stops.

Private notes (character references, concept-art review, script status) are in `D:\projects\ft_private\HANDOFF_PRIVATE.md`. They stay off this public repo.

## Resume here (updated 2026-10-21, Max plan)

- **Character piece-modelling test (2026-10-21, outside the repo).** Local image-to-3D (Hunyuan3D-2mv, shape only, on the 3080 Ti) made the body and each clothing piece from a concept sheet's views, then a Blender script fitted them together. Silhouette IoU against the drawn views is 0.83–0.95 for body, tunic, trousers and boots; arm wraps 0.65–0.94; the sash's thin tails fail (back 0.49). Nothing is retopologised, UV'd, textured or rigged, and nothing went into Content/. Next candidates to compare, from video research: Pixal3D Multiview and Trellis.2 (GGUF) in ComfyUI, both local. Details are in the private handoff.
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
