# Stub screen

The placeholder for gameplay steps that have no real screen yet: `GameplayScreen::Stub` in `ftcore::GameplayPlan` (BNT, CG, MINI and any unknown token; QTE and co-op QTE open the quick-time event screen, [QTE_SCREEN.md](QTE_SCREEN.md); CHC and HE open the choice screen, [CHOICE_SCREEN.md](CHOICE_SCREEN.md), which shows this same card when its loop file is missing). It names the step, waits for a press and hands control back to the story.

## Regenerate the map

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildStubWorld [-Map=Stub] -NullRHI -unattended -nopause -nosplash
```

Writes `/Game/Maps/Stub`: a small lit room (floor, back wall, sun, sky light, one lamp), a camera facing the wall and a player start. `WorldSettings.DefaultGameMode` is `AFTStubGameMode`. An existing map file is deleted first (loading the old one into the new package crashed on a duplicate WorldSettings). The map is generated, never hand-made, and is never committed as story content.

## Flow

1. The story director fills `UFTGameInstance::StoryCarry` (`Kind`, `StepToken`, `StepLabel`, `Step`, `StepCount`, `Scene`, `Beat`, `ReturnMap`, `bAutoPlay`) and opens `/Game/Maps/Stub`.
2. `AFTStubGameMode::StartPlay` reads the carry, makes the map's camera the view target, and builds the card text once: the title from `ftcore::stubTitle(token)`, the label, and "Step i of n".
3. `AFTStubHUD` draws a centred card with those and "Space / Enter / pad A: continue". Drawing builds no strings.
4. A press (Space, Enter, pad A, read by key polling on the player controller) or, with `bAutoPlay`, one second of game time ends the screen. It sets `bDone` and `bWon`, logs `Stub: <TOKEN> step i/n at <scene> beat <b> done.` (token name, ids and indices only; the label is displayed but never logged) and opens `ReturnMap`.

Titles: CHC Choice, BNT Banter, CG Cutscene, HE Dialogue loop, QTE and co-op QTE Quick-time event, MINI Mini-game, anything else shown as written. When `StepToken` is empty the carry's `Kind` is used.

## Rules (Core)

`Core/include/ftcore/StubScreen.h` holds `stubTitle` and the `StubScreen` state (no step / waiting / done): autoplay delay 1.0 s, a 0.25 s input grace so the press that opened the screen can't skip it, and `speedFactor(fast, worldDilation)` for `-AlphaFast`. Tests are in `Core/tests/test_stub_screen.cpp`.

## Headless behaviour

- With `-AlphaFast` the screen's timers run at 8x. The world's own time dilation already counts, so a run that sets dilation 8 elsewhere isn't sped up twice.
- With `bAutoPlay` and no input the screen continues after about one second of game time (about 0.13 s real at 8x).
- Opened directly (no valid carry, or the carry is already done) the card says "No story step" and nothing else happens: no log line, no map change.
- If `ReturnMap` is empty the carry is still marked done but no level is opened.
