# Beat boss demo

A demo map for the beat boss (`ftcore::BeatBoss`; the rules are in `Core/include/ftcore/BeatBoss.h` and in [COMBAT_SYSTEMS.md](COMBAT_SYSTEMS.md), "Beat boss"). It plays one authored track as a fight scene: every move the boss makes is pinned to a bar and a beat, the two fighters play fight clips whose blows land on those beats, the camera cuts between shots on the beat, a prompt sits on the body part it answers with a ring that closes on the impact, and the player answers with the action buttons. A thin beat line along the bottom shows the same moves, so the fight still works with the sound off. A defeat offers a retry from the phase's checkpoint; a victory offers the track again. It is a new mode beside the beat fight's screen ([BEAT_SCREEN.md](BEAT_SCREEN.md)), which it leaves alone, and no story step opens it yet.

## Play it

```
UnrealEditor.exe ForgottenThird.uproject Demo_BeatBoss -game
```

Add `-BeatBossAutoPlay` to watch the autopilot play it.

## Regenerate the map

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildBeatBossWorld [-Map=Demo_BeatBoss] -unattended -nosplash
```

Writes `/Game/Maps/Demo_BeatBoss`: a walled room (a floor, four tall walls, two pillars), a sun and sky, a warm key light over the fighters, a red rim light behind the boss and a cold one behind the lead, the lead and the boss (below), one camera and a player start. `AFTBeatBossGameMode` is the map's game mode. An existing map file is deleted first. The map is generated, never hand-made, and it goes through Git LFS.

## The stage

Both fighters are the test human (`test_human`): two skeletal mesh actors tagged `AFTBeatBossGameMode::LeadTag()` and `BossTag()`. The boss is the same body at 1.35 times the size (`BossScaleFactor`), turned to face back. Their feet stand `FightGap` (110 cm) apart on the x axis, close enough that a jab or a hook reaches (the game mode places them itself, and makes any that a map lacks).

- **Clips.** `ChoreoSample` names, for each fighter, a clip (`Data/Anim/beat_clips.json` gives each its asset path), a time into it and a rate. `UFTBeatFighterAnimInstance` (a C++ anim class, no Blueprint) plays that clip at exactly that time and crossfades over 0.12 s whenever the clip changes, so there is no pop back to the stance. Hit-stop is the same time twice; the finisher's slow motion is the choreography's own times. Root motion is never extracted and the root bone's travel is held (`-BeatBossRootMotion` lets it through); the clips' own facing differs from the mesh's, so a few frames in each body is turned on its actor until its stance faces the other (logged once).
- **The boss's hide** is the body's own character material, one dynamic instance per slot, tinted dark red with eyes that burn; it flashes when a blow lands and turns amber while the boss is staggered.
- **Cameras.** One camera actor moves to the shot the choreography names, as a hard cut on the beat: `wide` (square on from the side), `over_lead` (over the lead's shoulder at the boss), `over_boss` (over the boss's shoulder at the lead), `close_lead`, `close_boss`, `low_hero` (on the floor by the lead's feet, looking up), `top_down`, and `finisher_orbit` (an orbit about 24 degrees a second, slowed by the finisher's slow motion and held in a hit-stop). Close shots have the tighter lens. Each shot pushes in slowly (7 % over 3 s), and heavy impacts shake it: a counter's or a multiplied blow, a perfect guard, a hit on the lead (the most), a stagger, the victory.
- **Prompts on the action.** Each prompt of the sample is drawn at its anchor's bone (`boss_head` is the head, `boss_fist_r` and `_l` the hands, `boss_chest` and `lead_chest` the third spine bone, `lead_head` the head; `screen_center` is the middle of the screen). A dark disc with the button on it (its letter, and the face-button diamond under it with the prompt's button filled; two for a pair), its answer written above, a coloured ring (cyan a strike, red and spiked a counter, gold a hold or a finisher strike) and a ring that closes onto it so the two meet on the impact. It pops in with an overshoot, flares as the ring lands, and greys out under a cross while counters are locked out. The next prompt is large, those behind it smaller. A judgement pops where its prompt was: PERFECT punches (gold and white rays and a ring flying out, the word big, then settling), GOOD is a green ring, MISS shatters (red shards flying apart and falling, a grey cross, the word shaking).
- **Effects.** The hit-stop and the finisher's slow motion are in the clip times only (and in the camera's orbit); the fight's clock and the music are never touched.

## Flow

1. `AFTBeatBossGameMode::StartPlay` reads the flags and loads `Data/Battle/beat_boss/<id>.json` (`-BeatBossTrack=<id>`, `cine_track` by default; ids and numbers only), the clip catalogue (`Data/Anim/beat_clips.json`) and the track's choreography (`<id>.choreo.json`), and loads the clips. A track the loader refuses logs one warning with its errors, and an unattended run quits.
2. It finds the lead and the boss by their tags, makes the map's camera the view and starts the music (below).
3. The fight begins on the first tick, once the clock runs, with a bar counted in.
4. Each frame it polls the keys, feeds `ftcore::BeatBoss` the presses and releases with their event times, hands every call's events to `ftcore::BeatChoreo` with the clock the call was made at, ticks the fight with the clock, fills the beat line, sounds the music, then samples the scene and sets the fighters, the camera and the prompts. `AFTBeatBossHUD` draws it.
5. A defeat shows for 1.4 s, then asks: Retry goes back to the phase's checkpoint (`BeatBoss::retry`: the lead at full hp, the streak at 0, one bar counted in), Give up stays on the result. A victory shows the result and waits; the same key plays the track again from the top.

## The clock and the music

The fight runs on `UFTConductorComponent` on the wall clock (`bWallClockWithoutAudio`, no music bound). It keeps running through the fight's own silences and stands still while the pause menu is open. Presses are stamped with the conductor's latency-compensated input time.

The track's music cue has no recording, so the music is made in code, from the same clock. No audio file is involved anywhere. It is three layers, the first two plain C++20 in `Core/` with tests that run outside Unreal:

- **The sequencer** (`Core/include/ftcore/BeatMusic.h`, `planBeatMusic`). For any bar of the bar map it decides which voices hit on which sixteenth (two per eighth of the bar map, so a 6/8 beat has six) and how hard: kick, snare (a clap layered on the later tiers), closed and open hat (a crash on the finisher), a bass line, a stab or pad chord, a riser and an impact. The same context always gives the same pattern, and nothing allocates.
  - *Intensity follows the fight, once a bar.* The tier is read from the boss's hp when the driver first plans a bar (at most 0.1 s before its bar line) and kept for the whole bar (`BeatMusicDriver::tier()`), so a hit in the middle of a bar never switches the music under it. Four tiers from (phase + progress through the phase) / phases. Tier 0: kick on the downbeat, a soft backbeat, hats on the beats, one held bass note. Tier 1: a pad, a kick on the third beat, hats on the eighths, bass on the beats. Tier 2: stabs, an open hat before each backbeat, bass on the eighths. Tier 3: sixteenth hats, a driving bass, ghost notes, a kick on every beat. On the test track phase 1 plays tiers 0 and 1 and phase 2 tiers 2 and 3.
  - *The build.* The bar before a finisher bar has a riser over the bar (joining late, over what is left, if the hit that opens the finisher lands mid-bar) and a snare roll that grows over the last half of the bar, with the bass, stabs and hats out of the way. The finisher bar plays tier 3 at full velocity: a kick on every beat, a crash and an impact.
  - *Silences cut everything.* A scripted silence stops the music at its bar line (a short fade) and leaves the count; the bar that follows opens with an impact. A count bar has no music, only its blips.
  - *Odd meters.* A bar the loop breaks keeps the kick on the downbeat; the snare falls on the second, fourth ... beat of the bar when the beat is whole (a 7/8 bar's short last beat takes none), hats and bass only on steps the bar has, and an empty beat has nothing but the riser and the impact.
  - *Harmony.* A fixed progression in D minor per phase (Dm Bb F C; Dm Gm Bb A; ...), a chord a bar or two; the bar before a finisher plays the dominant, the finisher the tonic.
  - *Told moves.* Each move's impact gets an accent hit (stem 0 a thump, 1 a snare crack, 2 a bell), and the hats, stabs and bass leave that step. The accents sit on their own bus above the music and each ducks the music to about a fifth for 90 ms, so the player's audio cue reads over a full mix.
- **The driver** (`BeatMusicDriver`) follows `ftcore::BeatBoss` each frame and turns the patterns into timed events (`MusicEvent`: Play, Cut, Revoke) on the fight's clock, up to 0.1 s ahead. It reads the bar playing from the fight and the next bar from `BeatBoss::upcomingBar()` (a read-only view of what the beat line shows as its second bar), so the next downbeat goes out before its bar line. When the bar that comes is not the one predicted (the boss's hp crossed a threshold in that last tenth of a second: a finisher or an hp silence), it revokes what it sent and plans the real bar. The count's blips come from it too, and the count back after a silence is scheduled when the silence starts.
- **The synth** (`Core/include/ftcore/BeatMusicSynth.h`, `BeatMusicSynth`) makes every sound: the kick a sine that drops from about 200 Hz to 46 Hz with a click, the snare noise plus a tone, hats high-passed noise, the bass a sine and two detuned saws through a low-pass that closes with each note, the stabs three notes of three detuned saws, the riser a noise band and a sine that climb, the impact a falling sub with a noise burst. It has its own sample clock: a sound is queued for an absolute frame and starts exactly there. The output is soft-clipped, so it never reaches full scale.

`UFTBeatBossMusic` (`Source/ForgottenThird/FTBeatBossMusic.h`) is the Unreal side: a procedural sound that drains a lock-free ring into the synth on the audio thread. The game thread maps the fight's clock to the synth's frames with one fixed offset (found when the stream is first seen running, and again after a pause or a held view), so events keep exactly the spacing the clock gave them, whatever the frame time or the buffer size. The music sounds a small margin (40 ms) after the clock plus the device's own latency; a sound that still arrives behind the stream grows the offset by what it was late. The answers' own cues (a perfect, a miss) are triggered at once. `-BeatBossMetronome` brings back the old `UFTBeatBossMetronome`, a click on every beat with the count's blips and each move's stem, unchanged.

The clock is logged once, when the map starts, and the stream's health once at the end:

```
BeatBoss: track <id> on the wall clock with the procedural music standing in for music cue <cue id>.
BeatBoss: track <id> on the wall clock with the metronome standing in for music cue <cue id>.   (-BeatBossMetronome)
BeatBoss: music ran|did not run, <n> late, <n> dropped, <n> revoked.
```

`ran` means the audio thread produced sound (a run with no audio device, or `-nosound`, says `did not run`); `late` counts sounds the synth had to start after their frame, `dropped` those that found a queue full, and `revoked` the bars the driver had to re-plan. On the test track an autoplayed win logs 1 late and 2 revoked.

To hear it without the game, the offline render drives the same driver and synth against an autoplayed fight and writes WAV files:

```
set FTCORE_MUSIC_RENDER_DIR=<folder>
ctest --test-dir Core/build -C Debug -R ftcore_tests        (or ftcore_tests -tc="beat music*")
```

That writes `test_track_30s.wav` (the first 30 s), `test_track_full.wav` and `test_track_cues_only.wav` (the move accents and the count alone). The same test case checks, in memory, that the kicks start on the bar lines to the sample, that nothing sounds in a silence but its count, that the build and the finisher's impact are there, that the mix peaks under full scale, and that each accent is clearly louder than the music it sits on.

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

- `-BeatBossAutoPlay`: Core's autopilot plays (`QteAssist::autoComplete`), and keys are ignored. After a defeat the retry prompt shows as it would for a player and is confirmed on its own a second later, three times at most; after that the run logs a warning and ends lost. It wins `cine_track` in 33 bars, about 86 s, with no miss (`test_track`: 17 bars, 38 s).
- `-BeatBossMissPhase=<n>`: the checkpoint retry, headless. It implies `-BeatBossAutoPlay`, but the host drives the autopilot itself (`BeatBoss::autopilot`, each action at its own time, as the Core tests' pilot does) and presses nothing from phase n's first bar (1 is the first phase) until the lead is down. The retry prompt then shows and is confirmed, `BeatBoss::retry` restarts at that phase's checkpoint, and the autopilot plays on to the end. On `test_track` with `-BeatBossMissPhase=2`: phase 2 starts after 11 bars with the boss at 240 hp, is lost in bar 18, and the retry restarts phase 2 on its bar 1 with the boss at 240 and the lead at 100, not the track's bar 1; the run then wins in 32 bars, 67.3 s, with 1 retry.
- `-BeatBossIdle`: presses nothing and never retries, so the fight must be lost. On `cine_track` the lead goes down in bar 8, on `test_track` in bar 8 as well.
- `-BeatBossQuitOnFinish`: exits when the fight is over (a victory, or a defeat under `-BeatBossIdle`, after the autoplay's retries or when the player gives up), with code 0 for a victory, 1 for a defeat, 2 when the 300 s watchdog ends a run that stalls and 3 when the track doesn't load. The engine's graceful exit returns 0 whatever it is asked, so `FTRequestExit` (`ForgottenThird.h`) keeps a non-zero code and, once the engine has torn the world down, ends the process with it as the engine ends a commandlet (the log flushed first); a victory takes the whole graceful exit. Checked: the idle run exits with 1 under `UnrealEditor-Cmd`, the autoplay and `-BeatBossMissPhase=2` runs with 0, each log complete to its last line.
- `-BeatBossTrack=<id>`: another track in `Data/Battle/beat_boss/` (`cine_track` by default; `test_track` is the dense one the old shapes boss played).
- `-BeatBossRootMotion`: lets the clips' root bone travel (off by default: the actors stay where the stage puts them).
- `-BeatBossMetronome`: the old click-and-stems stand-in instead of the procedural music (the fight, its result and its numbers are the same either way).
- `-BeatBossShot=<prefix>`: renders the views with `HighResShot` as `<prefix>_<n>_<name>.png`, holding the fight's clock still a moment for each, then quits after the result. A view the track never gives is left out (the log says how many came). `1_countin` (the count-in), `2_counter` (a counter's ring on the boss's fist, a third of the way from landing), `3_strike` (the lead's blow on the beat, the hit-stop on it, PERFECT bursting), `4_close` (a close shot settled with a prompt on it), `5_orbit` (the finisher's orbit), `6_silence` (a silence, half a second in), `7_release` (the Release's marker, when the track has one), `8_retry` (with `-BeatBossMissPhase`, the retry prompt after the lost phase) and `9_result` (the victory). It implies `-BeatBossAutoPlay` and needs a renderer, so not `-NullRHI`:

```
UnrealEditor.exe ForgottenThird.uproject Demo_BeatBoss -game -RenderOffScreen -ResX=1600 -ResY=900 -nosound -unattended -nosplash -BeatBossShot=<folder>/beatboss
```

- `-BeatBossSeq=<prefix>`: a frame sequence instead, `<prefix>_0000.png` on, 1280 x 720, one every tenth of a second of the fight's clock from the first parry's tell (30 frames, `-BeatBossSeqCount=<n>` for others): the boss's blow coming, the parry, the counter, the cut to the close shot. It implies `-BeatBossAutoPlay`; the same render flags as above.

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

- **The prompts** are on the action (see "The stage"); a count's numerals pop large in the middle of the screen.
- **The beat line** is a thin strip along the bottom, 60 px tall at 1080p, so the fight is playable with the sound off. It holds the current bar and the next, laid out in seconds, so a beat is as wide in every bar and markers move at one speed into the hit line on the left. Bar lines are tall and beats short; an empty beat is a gap. Each bar is labelled with its number and meter: a meter the loop breaks in orange, a count bar in blue, a finisher bar in gold. A marker is the boss's move at its impact, a small disc with the button in it: a strike cyan, a counter red and spiked, a hold gold with a tail to its end, the Release orange; a move not yet told is dim, and counters locked out by a whiff are grey and crossed. Open rests are green diamonds. A silence dims and freezes the line under SILENCE, with a bar draining to the count.
- **The boss** has its hp across the top with the phase's floor marked, PHASE n/N and the bar playing; its damage rises from its head.
- **The lead's panel**, just over the strip: hp, the Load gauge, the open rests, the streak and the multiplier.
- **Popups** (TOO EARLY, LOCKED, rewards, Load gains) show by the prompt they belong to; the screen's edges flash red when the lead is hit; banners (a finisher, a broken finisher, a stagger, a Release, a new phase) sit at the top left.
- **Result**: VICTORY or DEFEAT in a band across the top with the counts, over the scene's end poses. After a defeat the retry prompt follows in the band: Retry from phase n in gold and Give up under it; it shows on an autoplay run too. After a victory, Play again, when keys are read.
- **Pause**: the pause overlay is drawn last.

## Choreography

`ftcore::BeatChoreo` (`Core/include/ftcore/BeatChoreo.h`) is the logic of the fight as a cinematic scene: the lead and the boss play fight clips, the camera cuts between shots, and each prompt sits by the part of the body it answers. It only reads the fight, so the judging, the scores and the results are `BeatBoss`'s, unchanged. `AFTBeatBossGameMode` is its host (see "The stage"); the host's job each frame is:

```
events = fight.update(clock);  scene.observe(fight, events, clock);   // after every press and release too
scene.sampleNow(s);
```

then play `s.lead` and `s.boss` (clip index, seconds into the clip, rate; look the clip up with `scene.clips().id(i)`), put the camera on `s.shot`, draw `s.prompts[0..promptCount)` at their anchors with `progress` 0..1 as the lead-in, and, when `s.frozen`, keep the stance going. After `fight.retry(clock)` call `scene.retry(fight)`. `observe` must also run every frame after the fight is over, so the end poses keep time.

- **Timing.** A boss move's clip is started so its impact frame lands on the move's impact, at a rate clamped to 0.75 to 1.35; if the windup does not fit after the last move, the clip starts partway in. The lead's answer is pre-rolled the same way: when a move is told, the lead's `ok` clip is scheduled so its blow (or guard, dodge or jump) lands on the move's impact too, starting during the lead-in once the lead is free of his previous answer. The choreography is committed to the music; the press decides which branch plays. A Perfect or Good keeps the clip, and after a Perfect the `then` follow-up is thrown on the first beat it can land on after the lead is free, if that is before his next answer or the end of the bar. A Miss (unanswered, a late press, a hold let go) cuts the clip at the impact to the move's `miss` clip, which begins on its own blow. A Whiff (a counter pressed too early) cuts to the stumble clip (`whiff` in the file) at the press, and the answer after the lockout plays from its press, the old way. The stance loops on the beat, one cycle a whole number of beats.
- **Hit-stop and slow motion** are visual only. A Perfect blow pauses the animation for 50 to 80 ms, and it has caught up by the next beat; in a finisher bar the animation dips between the strikes. The clip times in a sample already have both applied, and the fight's clock and the music are never touched, so every impact is on screen on its beat. `visualRate` and `slowMo` are for effects that should follow (sparks, the camera's move).
- **Shots.** `wide`, `over_lead`, `over_boss`, `close_lead`, `close_boss`, `low_hero`, `top_down` and `finisher_orbit`; the host maps each to a camera. A cut is only ever on a beat or a bar line: at the beat a move's tell falls on, to that move's shot; to `finisher_orbit` on a finisher bar's line; on the next beat after a stagger, the victory and the defeat. A silence cuts nothing.
- **Data.** `Data/Anim/beat_clips.json` lists the clips (id, length in seconds, impact offset, loop; up to 128; the animation side overwrites the placeholder numbers and may add ids). `Data/Battle/beat_boss/<track>.choreo.json` has the stance clips, the stagger, victory and defeat, and for each move, by index and checked by id, the boss's clip, the lead's clip on an answer (`ok`, `then` after a Perfect), on a miss, the boss's reaction to a blow (`hit`), the shot and the prompt's anchor (`boss_head`, `boss_fist_r`, `boss_fist_l`, `boss_chest`, `lead_head`, `lead_chest`, `screen_center`). Ids and numbers only; the loader refuses an unknown clip, a wrong move id or count, or a stance that does not loop.
- **Limits.** `observe` must see a move's prompt while it is open (every frame does); one opened and answered between two calls is not choreographed and is counted in `unmatched()`. A boss move whose windup is on screen is not interrupted by a reaction to a blow, and a `then` follow-up that could not land before the next move's answer is due is not played.
- **For the animation side.** Both fighters' clips are pinned by their `impact_s`, so the length of a windup is free: the clip is started `impact_s / rate` before the blow (at most 1.35 times speed), or partway in when the previous clip leaves no room. Give each answer clip its blow (guard, dodge or jump) at `impact_s`, and keep the track's moves far enough apart that a lead's answer (its `impact_s` over 1.35, plus 0.2 s of follow-through after the last) fits between two impacts, or the windup is skipped.

## The cinematic track

`Data/Battle/beat_boss/cine_track.json` and `cine_track.choreo.json` are the demo's default track, made for the real clips. Three phases at 96, 100 and 108 bpm (4/4), a loop of four bars each, a boss attack on a clear beat with room for its windup (a hook or thrust parried, a smash guarded, sweeps dodged or jumped, a grab paired), the lead's exchanges on attack prompts (jab, cross, kick, a held kick), one silence (phase two, at half the boss's hp), a two-bar finisher in the last two phases (three strikes, `finisher_orbit`, the slow motion dipping between them) and a stagger moment from the open rests. No two moves in a row share a shot and every shot and anchor is used. The Core tests (`test_beat_cine.cpp`) check that the autopilot wins it (86 s, 33 bars, no miss), an idle player loses it, a lost phase 1, 2 or 3 retries from its checkpoint and wins, every blow of both fighters is on its impact on screen with the windup played in full, and the cuts are on beats.

## Not yet

- No story step opens the map, and the track's music cue has no recording: the procedural music stands in. It is one fixed key, one voice set and one pattern family for every track; a track's own style, and a mix balanced by ear on real speakers, wait for a person to listen.
- Giving up at the retry prompt is played by hand only; `-BeatBossMissPhase` covers the retry itself.
- The clips are text-to-motion output retargeted onto one body: the lead's stance has the hands up at the face, the boss's hook sweeps over the lead's head more than it lands on him, and the boss's defeat does not fall (the root's travel is held). The camera and lighting are first passes.
