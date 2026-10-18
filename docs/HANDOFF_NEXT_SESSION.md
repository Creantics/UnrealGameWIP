# Handoff: the next session

Written 2026-10-18 at the end of the overnight session. Read `docs/HANDOFF_UE.md` in newengine first, as CLAUDE.md says. This file picks up from where that one stops.

Private notes (character references, concept-art review, script status) are in `D:\projects\ft_private\HANDOFF_PRIVATE.md`. They stay off this public repo.

## Resume here (updated 2026-10-18)

- **Phase:** S, the story gate, from `docs/ROADMAP.md`. No game building happens until every part scores at least 4.9/5 and two whole-script readers each give at least 9/10.
- **Status:** Phase S gate not met. Draft 67 readers gave 8 and 7.8, after two structural passes; reader scores have stayed near 8 for five drafts. A plateau report is written (private; newengine, local only): the blocker is design-level repetition in the middle acts, which prose passes cannot reach. The per-part 4.9 re-grade is outstanding, and the boss count is 15 pending the decision.
- **Next action:** the user kept the 9/10 gate and approved running the design-level pass now (Draft 68), with a hard stop at 98% weekly usage. At 98%, launch no agents; write both handoffs and push. Draft 68 is in progress: outline gate, then revisers, a cross-part check and two blind readers. If both readers give 9 or more, re-grade all five parts and do the Phase S exit. See the private handoff. The /motion story animation waits for the final story (keep it local).
- **Allowed in this repo during Phase S:** merging the two waiting branches (below), once their diffs are read and the gate passes. Nothing else.
- **User actions open:** the leak purge, and the GitHub billing fix (see `docs/PLAN.md`).

## Branches waiting to merge

The fork session owns the main checkout (`D:\projects\unrealgamewip`, on `chaos-physics`) and merges to main. Each branch below has had origin/main (3865bda) merged into it and is gate-green: ctest plus UBT Succeeded.

| Branch | Head | What it adds | Merge |
|---|---|---|---|
| `worktree-agent-ae87df10adccaad05` | e33902e | Test human; three lead characters (`Content/Characters/{Devin,Duy,Third}`) with IK Rigs and retargeters; `FTBuildLeadsWorld` and `Content/Maps/Leads.umap`; `docs/LEADS.md`, `docs/TEST_HUMAN.md`; `FTImportCharacter -Face=0`; the character master material (below) | Yes, first |
| `worktree-agent-a5baae6739480a570` | e6ca5e9 | Playable fight: `Content/Maps/BattleDemo.umap`, `AFTBattleDemoActor`, HUD, game mode, `FTBuildBattleDemo`; `docs/BATTLE_DEMO.md` | Yes, after the leads branch |
| `worktree-agent-ac78bf565fd6d3d57` | 98b9f62 | A second port of the style packs. It duplicates main's b62e9bb..5731842. | **Don't merge.** Salvage only what main lacks (next section). |

Both mergeable branches only gained renames on top of main: unity-build name clashes (`LeadRefBonePosition`, `LeadFacingYaw`, `HudWall`) and `GetFighterName`, which had hidden `UObject::GetName`. After merging, delete each branch locally and on origin, and remove its worktree.

## Style branch: what main lacks

Main has `PP_Style` and the `MI_Style_*` instances in `Content/Rendering`, plus `AFTStyleController`. The duplicate branch also has these, which main doesn't:

- **`Content/Maps/Demo_{Solhaven,Lattice,Unwritten,Restored}.umap`:** one demo world per style pack. This is what the user asked for. Rebuild them on main's `FTBuildDemoWorld -Style=<pack>`; don't copy the branch's assets, which point at its own `/Game/Styles` materials.
- **An `ft.Style <pack|none|dial>` console command:** check whether `AFTStyleController` already covers it before porting.
- **`DefaultGame.ini` always-cook for the style folder:** main's cook was clean without it, so add it only if a packaged build drops the materials.

Then delete the branch, locally and on origin.

## Open issue: characters render grey in `-game` screenshots

- In a minimized `-game` run with `-ExecCmds="HighResShot ..."`, the leads, the test human and the proof head all draw in the default material: flat 0.5 grey in `viewmode unlit`. In the editor they load with 0 map-check errors.
- Three different material setups gave pixel-identical shots, so the materials are probably not the cause. The shot fires at frame 5, and a minimized window barely ticks, so the character shaders were likely never compiled. This is not confirmed.
- **Change already made:** `FTImportCharacter` now moves every imported glTF material instance onto `/Game/Characters/Shared/Materials/M_FTCharacter`. That master takes `BaseColor`, `Metallic`, `Roughness`, `Emissive` and `EmissiveStrength`, and has the skeletal mesh and morph target usages, which the Interchange plugin master lacks. Keep it: it removes a plugin dependency and the editor's load-time usage patches.
- **Next step:** open `Leads.umap` in the editor (portrait monitor) and press Play. If the colours show, the open issue was only the capture method. If they don't, compare a cooked build, which compiles shaders up front.

## Story data: done, on Draft 63

- The game loads the final script (Draft 63). newengine's `tools/story_import` is the canonical importer: it defaults to `--draft draft63`, and `--check` is clean. This repo's copy only gained the `--draft` flag and the blockquote parser, because newengine's copy holds story text.
- To refresh, copy newengine `assets/story` into `Data/Story/` (gitignored).
- `tools/gate.ps1` now finds newengine from any worktree and says whether the real-script tests ran. Before this fix they silently skipped in worktrees.
- The Draft 63 VO list is built. VO rows marked "REVIEW: line edited" need a check before ElevenLabs.
- Deferred, and listed in newengine's `STATUS_draft63.md`:
  - The Home Echo consequence flags (recut, lantern score, diagram pins).
  - Choice branches, which are not modelled.
  - A rebuild of `Interactive_Script.md`.

## Characters: quality

- The leads are blockouts: tube bodies, procedural weights, stepped hair edges. Each character's look comes from the head proportions, hair, facial hair and glasses.
- In `Leads.umap` the jaw and tongue stay at rest, because single-node animation doesn't run the face anim instance.
- Next is a sculpt pass in Blender (`tools/blender`, headless `ft_sculpt`/`ft_preview`), then re-export with LINEAR keys and re-import.
- See the "Known limits" section of `docs/LEADS.md`.

## Fight demo: gaps

- There is no music; `bpm` stays 0, so timing is plain seconds.
- The fighters are placeholder shapes. Swap in the lead meshes once the leads branch is merged.

## Worktrees to clean up

These are under `.claude/worktrees/`:

- `agent-ae87df10adccaad05`, `agent-a5baae6739480a570`, `agent-ac78bf565fd6d3d57`: remove each once its branch is merged or retired.
- `funny-colden-696509`, `save-codec-884678`, `suspicious-euclid-af4c31`: old detached worktrees. Check them for uncommitted work before removing.
- `handoff-next-session`: this file's branch.

## Reminders

- Ask GitHub Support to purge the cached views of commit `362873808ae789806f186b5e554748f27caeb109`.
- Run no UBT while any editor or cook is running against the same project. The fork runs cooks in the main checkout.
- Open GUI programs on the portrait monitor, unfocused: `-WinX=-1440 -WinY=-997`, then restore with `ShowWindow(hwnd, 4)` (show without activating). A minimized Unreal window doesn't render.
