# Beat boss demo

A demo map for the beat boss (`ftcore::BeatBoss`; the rules are in `Core/include/ftcore/BeatBoss.h` and in [COMBAT_SYSTEMS.md](COMBAT_SYSTEMS.md), "Beat boss"). It plays one authored track: every move the boss makes is pinned to a bar and a beat, the beat line shows it coming, and the player answers with the action buttons. A defeat offers a retry from the phase's checkpoint; a victory offers the track again. It is a new mode beside the beat fight's screen ([BEAT_SCREEN.md](BEAT_SCREEN.md)), which it leaves alone, and no story step opens it yet.

## Play it

```
UnrealEditor.exe ForgottenThird.uproject Demo_BeatBoss -game
```

Add `-BeatBossAutoPlay` to watch the autopilot play it.

## Regenerate the map

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildBeatBossWorld [-Map=Demo_BeatBoss] -unattended -nosplash
```

Writes `/Game/Maps/Demo_BeatBoss`: a floor, a back wall, two pillars, a sun, sky and lamp, a fixed camera and a player start. The lead is the test human (`test_human`, playing `test_humanidle`), tagged `AFTBeatBossGameMode::LeadTag()`. The boss is a large stand-in of basic shapes (a cylinder body tagged `BossTag()`, with a head, an orange visor, shoulders, arms and fists attached to it). `AFTBeatBossGameMode` is the map's game mode. An existing map file is deleted first. The map is generated, never hand-made, and it goes through Git LFS.

## Flow

1. `AFTBeatBossGameMode::StartPlay` reads the flags and loads `Data/Battle/beat_boss/<id>.json` (`-BeatBossTrack=<id>`, `test_track` by default; ids and numbers only). A track the loader refuses logs one warning with its errors, and an unattended run quits.
2. It finds the lead and the boss by their tags, makes the map's camera the view and starts the stand-in music (below).
3. The fight begins on the first tick, once the clock runs, with a bar counted in.
4. Each frame it polls the keys, feeds `ftcore::BeatBoss` the presses and releases with their event times, ticks it with the clock, fills the beat line and sounds the music. `AFTBeatBossHUD` draws it.
5. A defeat shows for 1.4 s, then asks: Retry goes back to the phase's checkpoint (`BeatBoss::retry`: the lead at full hp, the streak at 0, one bar counted in), Give up stays on the result. A victory shows the result and waits; the same key plays the track again from the top.

## The clock and the music

The fight runs on `UFTConductorComponent` on the wall clock (`bWallClockWithoutAudio`, no music bound). It keeps running through the fight's own silences and stands still while the pause menu is open. Presses are stamped with the conductor's latency-compensated input time.

There is no music yet, so `UFTBeatBossMetronome` stands in, driven from the same clock. It clicks every beat the bar has (accented on the bar line, nothing on an empty beat), blips the count's numerals, and sounds each told move's stem as the track crosses its impact, so what is heard and what the beat line shows agree. It is a small synth: no recorded audio. The clock is logged once, when the map starts:

```
BeatBoss: track <id> on the wall clock with the metronome standing in for music cue <cue id>.
```

## Keys

The buttons are the QTE screen's (`ftcore::ActionButton`, `FTActionButtons.h`). The boss never asks for arrows.

| Answer | Keys |
|---|---|
| Attack (a strike; a hold or the Release is held to its end) | J, pad face left |
| Guard (held through the impact) or parry (tapped on it) | I, pad face top |
| Dodge | L, pad face right |
| Jump | K, pad face bottom |
| Paired | both buttons the marker shows, together |

Keys are polled each frame, so a held Guard or hold sees its key go up. Pressing a counter's button while its marker shows but before its window opens is a whiff: counters lock out for a moment (TOO EARLY, and the hit line turns orange). Escape (keyboard) or Start (pad) opens the pause menu (see [GAME_FRAME.md](GAME_FRAME.md), "Pause"). A key still held in the frame after the menu closes is not a press until it is let go and pressed again.

On the result:

| Pick | Keys |
|---|---|
| Retry from the checkpoint (after a defeat), or play again (after a victory) | Enter, Space, pad A |
| Give up (after a defeat) | Q, Backspace, pad B |

## Flags

For checking the map headless:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject Demo_BeatBoss -game -NullRHI -BeatBossAutoPlay -BeatBossQuitOnFinish -unattended -nosplash
```

- `-BeatBossAutoPlay`: Core's autopilot plays (`QteAssist::autoComplete`), and keys are ignored. After a defeat the retry prompt shows as it would for a player and is confirmed on its own a second later, three times at most; after that the run logs a warning and ends lost. It wins the test track in 17 bars, about 38 s, with no miss.
- `-BeatBossMissPhase=<n>`: the checkpoint retry, headless. It implies `-BeatBossAutoPlay`, but the host drives the autopilot itself (`BeatBoss::autopilot`, each action at its own time, as the Core tests' pilot does) and presses nothing from phase n's first bar (1 is the first phase) until the lead is down. The retry prompt then shows and is confirmed, `BeatBoss::retry` restarts at that phase's checkpoint, and the autopilot plays on to the end. On the test track with `-BeatBossMissPhase=2`: phase 2 starts after 11 bars with the boss at 240 hp, is lost in bar 18, and the retry restarts phase 2 on its bar 1 with the boss at 240 and the lead at 100, not the track's bar 1; the run then wins in 32 bars, 67.3 s, with 1 retry.
- `-BeatBossIdle`: presses nothing and never retries, so the fight must be lost. On the test track the lead goes down in bar 8.
- `-BeatBossQuitOnFinish`: exits when the fight is over (a victory, or a defeat under `-BeatBossIdle`, after the autoplay's retries or when the player gives up), with code 0 for a victory, 1 for a defeat, 2 when the 300 s watchdog ends a run that stalls and 3 when the track doesn't load. The engine's graceful exit returns 0 whatever it is asked, so `FTRequestExit` (`ForgottenThird.h`) keeps a non-zero code and, once the engine has torn the world down, ends the process with it as the engine ends a commandlet (the log flushed first); a victory takes the whole graceful exit. Checked: the idle run exits with 1 under `UnrealEditor-Cmd`, the autoplay and `-BeatBossMissPhase=2` runs with 0, each log complete to its last line.
- `-BeatBossTrack=<id>`: another track in `Data/Battle/beat_boss/`.
- `-BeatBossShot=<prefix>`: renders the views with `HighResShot` as `<prefix>_1_countin.png`, `_2_counter.png` (a counter's marker halfway in), `_3_silence.png`, `_4_release.png` and `_5_result.png` (the victory), and with `-BeatBossMissPhase` also `_6_retry.png` (the retry prompt after the lost phase), holding the fight's clock still a moment for each, then quits. It implies `-BeatBossAutoPlay` and needs a renderer, so not `-NullRHI`:

```
UnrealEditor.exe ForgottenThird.uproject Demo_BeatBoss -game -RenderOffScreen -ResX=1600 -ResY=900 -nosound -unattended -nosplash -BeatBossShot=<folder>/beatboss
```

`-BeatBossShotDelay=<s>` (8 by default) waits that long before the fight begins, so the shaders settle before the count-in.

## Log lines

One line per outcome, counts only, never text:

```
BeatBoss: begin (<phases> phases, boss <hp> hp, lead <hp> hp)[, autoplay][, idle]; losing phase <n> (0: none).
BeatBoss: phase <n> starts on its bar <b> (boss <hp> hp, <bars> bars played).
BeatBoss: losing phase <n> on purpose (boss <hp> hp).
BeatBoss: retry <r> from phase <n>'s checkpoint (boss <hp> hp, lead <hp> hp), lost after <bars> bars.
BEATBOSS RESULT: Victory in <bars> bars, <s> s, <p> perfect, <g> good, <m> miss, best streak <k>, <r> retries, boss <hp>/<max> hp, lead <hp>/<max> hp, <w> whiffs, <s> strays, <f> finishers, <rel> releases, <st> staggers, <si> silences, phase <n>/<N>[, autoplay][, idle].
BEATBOSS RESULT: Defeat in ...
BeatBoss: quitting (<code>).
Exiting with code <code>.
```

The counts run on through retries. The last line comes only for a non-zero code, just before the process ends.

## Screen

All of it is canvas (no UMG), and drawing builds no strings: every word is a static `FTUiText` (the `beatboss.*` keys in `Data/Loc/en/ui.json`), and numbers and composed labels are built once.

- **The beat line** runs across the bottom. It holds the current bar and the next, laid out in seconds, so a beat is as wide in every bar and markers move at one speed into the hit line on the left. Bar lines are tall and beats short; an empty beat is a gap. Each bar is labelled with its number and meter: a meter the loop breaks in orange, a count bar in blue with its numerals, a finisher bar in gold.
- **Markers** are the boss's moves at their impact, sized by their stem, with the button under them. A strike is cyan. A counter is red and spiked, with its answer over it. A hold is gold with a tail to its end, and the Release orange. A move not yet told is dim. While a whiff locks counters out they are grey and crossed through. Open rests are green diamonds under the line.
- **A silence** dims and freezes the line under SILENCE, with a bar draining to the count. A count's numerals pop large in the middle of the screen.
- **The boss** has its hp across the top, with the phase's floor marked, PHASE n/N and the bar playing. Over the boss, the next counter's answer and a ring that closes as its impact comes. The boss itself winds up as a counter's marker closes in (it swells, leans back and reddens), lunges on the impact, flashes white when hit, sags while staggered and sinks when beaten.
- **The lead's panel**, under the line: hp, the Load gauge, the open rests, the streak and the multiplier. The lead lunges, sidesteps or hops with each answer and recoils when hit.
- **Popups**: PERFECT, GOOD, MISS, TOO EARLY, the rewards and Load gains rise from the hit line on dark plates, the boss's damage over the boss. The hit line flashes gold, green or red with each judgement and the screen's edges flash red when the lead is hit. Banners, left of the boss, mark a finisher, a broken finisher, a stagger, a Release and a new phase.
- **Result**: VICTORY or DEFEAT in a band across the screen with the counts (perfect, good, miss, best streak, bars, retries). After a defeat the retry prompt follows in the band: Retry from phase n in gold (the checkpoint's phase) and Give up under it; it shows on an autoplay run too. After a victory, Play again, when keys are read.
- **Pause**: the pause overlay is drawn last.

## Choreography

`ftcore::BeatChoreo` (`Core/include/ftcore/BeatChoreo.h`) is the logic of the fight as a cinematic scene: the lead and the boss play fight clips, the camera cuts between shots, and each prompt sits by the part of the body it answers. It only reads the fight, so the judging, the scores and the results are `BeatBoss`'s, unchanged. The demo host does not draw it yet; the host's job each frame is:

```
events = fight.update(clock);  scene.observe(fight, events, clock);   // after every press and release too
scene.sampleNow(s);
```

then play `s.lead` and `s.boss` (clip index, seconds into the clip, rate; look the clip up with `scene.clips().id(i)`), put the camera on `s.shot`, draw `s.prompts[0..promptCount)` at their anchors with `progress` 0..1 as the lead-in, and, when `s.frozen`, keep the stance going. After `fight.retry(clock)` call `scene.retry(fight)`. `observe` must also run every frame after the fight is over, so the end poses keep time.

- **Timing.** A boss move's clip is started so its impact frame lands on the move's impact, at a rate clamped to 0.75 to 1.35; if the windup does not fit after the last move, the clip starts partway in. The lead's clip starts on the judged press (a blow lands 0.06 s after it, a counter pressed early waits for the boss's impact) and a miss plays from the impact. The stance loops on the beat, one cycle a whole number of beats.
- **Hit-stop and slow motion** are visual only. A Perfect blow pauses the animation for 50 to 80 ms, and it has caught up by the next beat; in a finisher bar the animation dips between the strikes. The clip times in a sample already have both applied, and the fight's clock and the music are never touched, so every impact is on screen on its beat. `visualRate` and `slowMo` are for effects that should follow (sparks, the camera's move).
- **Shots.** `wide`, `over_lead`, `over_boss`, `close_lead`, `close_boss`, `low_hero`, `top_down` and `finisher_orbit`; the host maps each to a camera. A cut is only ever on a beat or a bar line: at the beat a move's tell falls on, to that move's shot; to `finisher_orbit` on a finisher bar's line; on the next beat after a stagger, the victory and the defeat. A silence cuts nothing.
- **Data.** `Data/Anim/beat_clips.json` lists the clips (id, length in seconds, impact offset, loop; up to 128; the animation side overwrites the placeholder numbers and may add ids). `Data/Battle/beat_boss/<track>.choreo.json` has the stance clips, the stagger, victory and defeat, and for each move, by index and checked by id, the boss's clip, the lead's clip on an answer (`ok`, `then` after a Perfect), on a miss, the boss's reaction to a blow (`hit`), the shot and the prompt's anchor (`boss_head`, `boss_fist_r`, `boss_fist_l`, `boss_chest`, `lead_head`, `lead_chest`, `screen_center`). Ids and numbers only; the loader refuses an unknown clip, a wrong move id or count, or a stance that does not loop.
- **Limits.** `observe` must see a move's prompt while it is open (every frame does); one opened and answered between two calls is not choreographed and is counted in `unmatched()`. A boss move whose windup is on screen is not interrupted by a reaction to a blow, and a `then` follow-up that could not land before the next move's answer is due is not played.
- **For the animation side.** The lead's answer clips start on the press, so the blow can land only 0.06 s later (`kChoreoLeadLag`): a clip whose impact offset is longer than that starts partway through its windup, at up to 1.35 times speed. Author the lead's answer clips with the blow within about 60 ms of their start, or the lead's windup is skipped every time.

## Not yet

- No story step opens the map, and the track's music cue has no recording: the metronome stands in.
- Giving up at the retry prompt is played by hand only; `-BeatBossMissPhase` covers the retry itself.
- The boss and the room are placeholders, and the lead has only its idle and the host's nudges for moves.
