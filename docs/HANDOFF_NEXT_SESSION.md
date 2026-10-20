# Handoff: the next session

Written 2026-10-18, last updated 2026-10-20. Read `docs/HANDOFF_UE.md` in newengine first, as CLAUDE.md says. This file picks up from where that one stops.

Private notes (character references, concept-art review, script status) are in `D:\projects\ft_private\HANDOFF_PRIVATE.md`. They stay off this public repo.

## Resume here (updated 2026-10-20, Max plan)

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
