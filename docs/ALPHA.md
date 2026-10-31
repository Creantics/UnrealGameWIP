# Alpha: the whole story, start to end

One build plays the story from the title screen to the end card. Every gameplay step in the script
opens a screen: a battle, a field walk, a dialogue loop, a quick-time event, a cutscene, a banter, a mini-game or a stub card. Nothing is polished. The art is blockouts,
and there is no voice or music.

## Play it

1. Build: `powershell -File tools/gate.ps1 -Ubt`.
2. Regenerate the maps if they're missing (each doc has the command): `Demo`
   ([RIGGING_TEST.md](RIGGING_TEST.md)), `BattleDemo` ([BATTLE_DEMO.md](BATTLE_DEMO.md)), `Explore`
   ([EXPLORE_DEMO.md](EXPLORE_DEMO.md)), `Stub` ([STUB_SCREEN.md](STUB_SCREEN.md)), `Choice`
   ([CHOICE_SCREEN.md](CHOICE_SCREEN.md)), `Qte` ([QTE_SCREEN.md](QTE_SCREEN.md)), `Cutscene`
   ([CUTSCENE_SCREEN.md](CUTSCENE_SCREEN.md)), `Banter` ([BANTER_SCREEN.md](BANTER_SCREEN.md)) and
   `Mini` ([MINI_SCREEN.md](MINI_SCREEN.md)). The director opens `Choice` for every CHC and HE step,
   `Qte` for every QTE and co-op QTE step, `Cutscene` for every CG step, `Banter` for every BNT step
   and `Mini` for every MINI step, so all five must exist.
3. Copy newengine `assets/story/` to `Data/Story/`. It's gitignored and never committed, because the
   repo is public.
4. Open the editor (it opens `Demo`) and click Play, or run the game. The title offers New Game
   and Continue. Only `Demo` shows the title. Playing a screen's own map (`Explore`, `BattleDemo`
   and the rest) opens that screen alone, with no story around it.

| Screen | What you do | Doc |
|---|---|---|
| Title | New Game, or Continue from the autosave (slot 0, written at every scene start) | [GAME_FRAME.md](GAME_FRAME.md) |
| Story | Read and continue; pick options 1–4 | [DIALOGUE_UI.md](DIALOGUE_UI.md) |
| Battle | Fight the step's line-up from `Data/Battle/encounters.json`. On a loss, Retry restores the HP the party started the battle with, or Quit to title | [BATTLE_DEMO.md](BATTLE_DEMO.md) |
| Field | Walk to the glowing goal marker and press Use | [EXPLORE_DEMO.md](EXPLORE_DEMO.md) |
| Choice | CHC and HE steps: a dialogue loop from `Data/Story/loops/<scene>_<beat>_<step>.json`. Read the claim, pick 1–4 (used wrong options grey out), Backspace skips to the closing lines; its flags are set at the end. With no loop file it shows the stub card | [CHOICE_SCREEN.md](CHOICE_SCREEN.md) |
| Qte | QTE and co-op QTE steps: a short timed input sequence from `Data/Qte/qte.json`. Press the action button (J, I, L, K or the pad's face buttons) as the ring closes in (hold or mash where asked); a miss plays on, and the result (Perfect to Poor) shows for a second | [QTE_SCREEN.md](QTE_SCREEN.md) |
| Cutscene | Watch a CG step's shot list from `Data/Cutscene/cutscenes.json` on a blockout set; press to skip | [CUTSCENE_SCREEN.md](CUTSCENE_SCREEN.md) |
| Banter | BNT steps: lines from `Data/Story/banter/<scene>_<beat>_<step>.json` that advance by themselves, left and right by speaker. Press for the next line, Backspace skips. With no banter file it shows the stub card | [BANTER_SCREEN.md](BANTER_SCREEN.md) |
| Mini | MINI steps: a short rhythm round from `Data/Mini/mini.json`. Press each button as it reaches the line; a round below the pass mark retries, and after the last try the story goes on | [MINI_SCREEN.md](MINI_SCREEN.md) |
| Stub | A card naming the step (unknown tokens); press to continue | [STUB_SCREEN.md](STUB_SCREEN.md) |
| End card | Press to return to the title | [GAME_FRAME.md](GAME_FRAME.md) |

A gameplay beat's `mode` can chain several steps (a field walk, then a fight, then a field walk).
`ftcore::GameplayPlan` lowers it to steps, and the story director plays them in order before the
story resumes.

## The gate

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/gate.ps1 -Alpha      # ~7 min on the 8x clock
powershell -NoProfile -ExecutionPolicy Bypass -File tools/gate.ps1 -AlphaFull  # full speed, ~45 min
```

`-Alpha` builds with UBT, then runs headless:

```
UnrealEditor-Cmd ForgottenThird.uproject -game -NullRHI -AlphaAutoPlay -AlphaFast -StoryQuitOnFinish -unattended -nosplash -nosound
```

- `-AlphaAutoPlay` starts at New Game, plays every scene at fast text and hands off on every step.
  Each screen then plays itself: the battle autopilot wins, the field pilot walks to the goal, and a
  stub ends after one second of game time, a dialogue loop skips to its closing lines and ends after
  about one second, the quick-time event autopilot presses at the ideal times and finishes within
  about 10 s of game time, a cutscene skips itself after one second of real time, a banter skips
  after 0.25 s and a mini-game round clears itself in about a second. Choice, quick-time event,
  cutscene, banter and mini-game steps count in the `stub` bucket of the line below.
- `-AlphaFast` runs the gameplay screens at global time dilation 8.

Pass means exit code 0 and this line (choice, quick-time event, cutscene, banter and mini-game steps count in the stub bucket):

```
Alpha: FINISHED 73 scenes, 167 screens (40 battle, 91 field, 36 stub), 0 STUCK, 0 warnings, 12 overworld hops
```

When `Content/Maps/Overworld.umap` exists, the gate also needs at least one overworld hop (see `docs/WORLDS.md`).

It needs the local `Data/Story`, so only the main checkout runs it. Worktrees run the Core side
instead: `Core/tests/test_alpha.cpp` plays the real script through the plans with fake screens. It
skips when the story is absent.

**Last results.**

| Date | Run | Result | Time |
|---|---|---|---|
| 2026-10-21 | 8x clock (main at 50a6c4e: encounters, step data, banter/mini, battle aim, audio, voice, validator merged) | passed | about 7 minutes |
| 2026-10-21 | 8x clock (main at efef15d: choice, quick-time event, cutscene, settings and defence streams merged) | passed | about 7 minutes |
| 2026-10-20 | full speed (main at ac32638) | passed | — |
| 2026-10-20 | 8x clock (main at 20211b2) | passed | about 7 minutes |
| 2026-10-19 | full speed, before stream 2 | passed | — |

The 8x run logs harmless character-movement substep warnings from the field pilot. They aren't
counted as alpha warnings.

## What's stubbed

- **Banter.** The screen is real, but every BNT step shows the stub card until its file is in
  `Data/Story/banter/`. No walking: the lines play over a fixed camera.
- **Mini-game.** One generic rhythm round for every MINI step. The script names MINI only in
  hidden direction notes, not in any beat's mode, so the alpha never opens it.
- **Quick-time events (QTE, co-op QTE).** The screen is real, with a built-in sequence for every step
  the table doesn't list; its grade is only shown, and the story never branches on it.
- **Dialogue loops (CHC, HE).** The screen is real, but no loop files are written yet, so each one
  shows the stub card until its file is in `Data/Story/loops/`.
- **Cutscenes (CG).** Every CG step plays the `default` shot list on the blockout set; real shot
  lists aren't authored yet.
- **Art.** Blockout maps and the proof head only, with no portraits.
- **Audio.** No voice. One placeholder music loop (`MUS-KNK`, see [ASSET_INTAKE.md](ASSET_INTAKE.md)); the battle beat grid follows the story music when its cue has a tempo. The headless run is `-nosound`, so its Conductor runs on game time.
- **Balance.** The only balance rule is that the autopilot wins and no input loses.
- **Navmesh.** `FTBuildExploreWorld` builds and saves a navmesh over the field map and fails if any spot or field goal has no full path from the start. The field autopilot follows the path and walks straight only when there's no navmesh.
- **Interface text** comes from `Data/Loc/en/ui.json` ([LOCALISATION.md](LOCALISATION.md)). There are no other cultures yet.

[PLAN.md](PLAN.md) lists what comes next.
