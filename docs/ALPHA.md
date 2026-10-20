# Alpha: the whole story, start to end

One build plays the story from the title screen to the end card. Every gameplay step in the script
opens a screen: a battle, a field walk or a stub card. Nothing is polished. The art is blockouts,
and there is no voice or music.

## Play it

1. Build: `powershell -File tools/gate.ps1 -Ubt`.
2. Regenerate the maps if they're missing (each doc has the command): `Demo`
   ([RIGGING_TEST.md](RIGGING_TEST.md)), `BattleDemo` ([BATTLE_DEMO.md](BATTLE_DEMO.md)), `Explore`
   ([EXPLORE_DEMO.md](EXPLORE_DEMO.md)) and `Stub` ([STUB_SCREEN.md](STUB_SCREEN.md)).
3. Copy newengine `assets/story/` to `Data/Story/`. It's gitignored and never committed, because the
   repo is public.
4. Open the editor (it opens `Demo`) and click Play, or run the game. The title offers New Game
   and Continue.

| Screen | What you do | Doc |
|---|---|---|
| Title | New Game, or Continue from the autosave (slot 0, written at every scene start) | [GAME_FRAME.md](GAME_FRAME.md) |
| Story | Read and continue; pick options 1–4 | [DIALOGUE_UI.md](DIALOGUE_UI.md) |
| Battle | Fight the step's line-up from `Data/Battle/encounters.json`. On a loss, Retry restores the HP the party started the battle with, or Quit to title | [BATTLE_DEMO.md](BATTLE_DEMO.md) |
| Field | Walk to the glowing goal marker and press Use | [EXPLORE_DEMO.md](EXPLORE_DEMO.md) |
| Stub | A card naming the step (choice, banter, cutscene, heart-to-heart, quick-time event, mini-game); press to continue | [STUB_SCREEN.md](STUB_SCREEN.md) |
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
  stub ends after one second.
- `-AlphaFast` runs the gameplay screens at global time dilation 8.

Pass means exit code 0 and this line:

```
Alpha: FINISHED 73 scenes, 167 screens (40 battle, 91 field, 36 stub), 0 STUCK, 0 warnings
```

It needs the local `Data/Story`, so only the main checkout runs it. Worktrees run the Core side
instead: `Core/tests/test_alpha.cpp` plays the real script through the plans with fake screens. It
skips when the story is absent.

**Last results.**

| Date | Run | Result | Time |
|---|---|---|---|
| 2026-10-20 | 8x clock (main at 20211b2) | passed | about 7 minutes |
| 2026-10-19 | full speed, before stream 2 | passed | — |

The 8x run logs harmless character-movement substep warnings from the field pilot. They aren't
counted as alpha warnings.

## What's stubbed

- **Choice, banter, cutscene, heart-to-heart, quick-time event, mini-game.** All of these show
  the stub card.
- **Art.** Blockout maps and the proof head only, with no portraits.
- **Audio.** No voice or music. The Conductor runs on game time.
- **Balance.** The only balance rule is that the autopilot wins and no input loses.
- **Not built yet:** navmesh, localisation, a dialogue backlog or skip, and settings menus.

[PLAN.md](PLAN.md) lists what comes next.
