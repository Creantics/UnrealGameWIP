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

Writes `/Game/Maps/Demo_BeatBoss`: a closed, dark room (a floor, four tall walls, a roof, two pillars; no sun, atmosphere or clouds, so no sky shows in any shot), a very low sky light, a post-process volume with manual exposure, a warm key light (the only one that casts a shadow) over the fighters, a red rim light behind the boss, a cold one behind the lead and a kicker above and behind the boss (each tagged, so a shot can scale them apart), the lead and the boss (below), one camera and a player start. `AFTBeatBossGameMode` is the map's game mode. An existing map file is deleted first. The map is generated, never hand-made, and it goes through Git LFS.

## The stage

Both fighters are the test human (`test_human`): two skeletal mesh actors tagged `AFTBeatBossGameMode::LeadTag()` and `BossTag()`. The boss is the same body at 1.35 times the size (`BossScaleFactor`), turned to face back. Their feet stand `FightGap` (110 cm) apart on the x axis, close enough that a jab or a hook reaches (the game mode places them itself, and makes any that a map lacks).

- **Clips.** `ChoreoSample` names, for each fighter, a clip (`Data/Anim/beat_clips.json` gives each its asset path), a time into it and a rate. `UFTBeatFighterAnimInstance` (a C++ anim class, no Blueprint) plays that clip at exactly that time and crossfades over 0.12 s whenever the clip changes, so there is no pop back to the stance. Hit-stop is the same time twice; the finisher's slow motion is the choreography's own times. Root motion is never extracted and the root bone's travel is held (`-BeatBossRootMotion` lets it through); the clips' own facing differs from the mesh's, so a few frames in each body is turned on its actor until its stance faces the other (logged once).
- **The boss's hide** is the body's own character material, one dynamic instance per slot, tinted dark red with eyes that burn up as its next blow comes (to two and a half times their glow across the tell); it lifts a little when a blow lands (0.12 at most, held three frames, gone in 0.14 s; 0.05 over 0.3 s under the reduced-flash level, none at the minimal one) and turns amber while the boss is staggered. A point light on the fist about to strike rises across the tell.
- **The boss's body.** Its mass is a bone scale put on the pose after the clips are blended (`UFTBeatFighterAnimInstance::SetBuild`: shoulders and chest 1.15, hands 1.25, head 0.85, net sizes, each bone's own scale its size over its parent's). Its swing leans into the blow (`SetLean`, 16 degrees forward at the impact, so a fist at the boss's height comes down onto the lead instead of over him; the lean turns `spine_03` in component space about the line across the hips, the same for every clip). A stagger recoils: the spine snaps back 18 degrees and recovers, and the body goes 35 cm back over 250 ms and returns over a second. The defeat falls: the root's travel is let go for the boss's defeat clip (and the lead's downed clip) and held for every other.
- **Cameras.** One camera actor moves to the shot the choreography names, as a hard cut on the beat, every shot on the same (+y) side of the line from the lead to the boss so a cut never turns the screen round: `wide` (28 mm, level, 2.7 boss heights out), `over_lead` (32 mm over the lead's shoulder at the boss, which looms), `over_boss` (35 mm over the boss's shoulder at the lead, the lead's rim up), `close_lead` (50 mm, f/2), `close_boss` (50 mm from below, the boss's rim up), `low_hero` (21 mm on the floor by the lead's feet, looking up), `top_down` (35 mm, 8 degrees off the vertical), and `finisher_orbit` (an orbit about 24 degrees a second, 28 to 50 mm and out to in over the three strikes, so the last is the tightest; slowed by the finisher's slow motion and held in a hit-stop). Every lens is on a 24.576 mm sensor with its own f-stop and focus (the target's distance); the exposure is manual, one value for every shot, so a lens changes only the blur. Each shot's lights are scaled apart (`UpdateLights`: the key, the two rims and the kicker, each by the shot's table). Every cut calls `SetGameCameraCutThisFrame` (the renderer's temporal history would otherwise smear the old shot over the new) and starts 3 % tighter, settling over 120 ms. Each shot pushes in slowly (7 % over 3 s).
- **The impact stack.** A blow's feedback is a stack: the pose, the camera and the particles freeze for the hit-stop (the shake runs on the visuals' clock, so it freezes with them); when the stop lets go the frame springs 1.5 cm along the fight's axis, the shake breaks and the fringe comes in (a hit on the lead 0.8 over 300 ms and a vignette that punches to 0.75; a stagger 0.5; a finisher's strike 0.4; a whiff 0.2). A blow the lead's follow-up has not landed yet waits for its contact (`BeatChoreo::hitContact`) before its popup, flash, shake and puff: a parry's counter lands a beat after the parry and the damage comes with it. Dust and a puff of cloth (`Data/Vfx/dust_puff.json`, built into `/Game/VFX/NS_dust_puff` by `FTBuildVfx -Only=dust_puff`) come off the contact, following the visuals' clock; sparks are for metal and nothing here is. In the finisher's slow motion the grade deepens (saturation 0.85, contrast 1.1, vignette +0.15). There is no lens punch or extra motion blur in a hit-stop or the slow motion: the accessibility spec asks for no added zoom or blur there, and it wins over the look spec's. The shake is capped as before (a 1.8 degree coefficient, no roll) and scaled by `beat.shake_scale`.
- **Prompts on the action.** Each prompt of the sample is drawn at its anchor's bone (`boss_head` is the head, `boss_fist_r` and `_l` the hands, `boss_chest` and `lead_chest` the third spine bone, `lead_head` the head; `screen_center` is the middle of the screen). A dark disc with the button on it (its letter, and the face-button diamond under it with the prompt's button filled; two for a pair), the answer written above it on every open prompt (at least 18 px at 1080p), and a ring that closes onto it so the two meet on the impact. Each answer has a shape of its own, so the seven read apart in greyscale: a strike a plain circle (cyan), a parry a circle with eight spikes (red), a guard a double hexagon that fills with a gold arc once it is pressed, a dodge a diamond with a chevron to the side it goes, a jump a triangle point up, a hold a circle with a tail arc that drains as it is held, a release an octagon, a pair two half discs and a finisher's strike a gold circle with a pip for each strike left. The closing ring moves at one fixed speed, 300 px a second at 1080p, so a move told earlier starts farther out (a ring that would start off the screen waits and enters at 570 px); it has a dark underlay a little wider than its bright line, and its alpha pulses 0.8 to 1 with the beat while the disc swells 6 % on it, so the rhythm shows without sound. The disc's radius follows the prompt's distance from the camera (half the projected height of a 24 cm sphere there, held to 34..60 px at 1080p), so one in the far shot is not a speck and one in a close shot does not fill a face. It pops in with an overshoot, flares as the ring lands (not under the flash options), and greys out under a cross while counters are locked out. The next prompt is large, those behind it smaller. A judgement pops where its prompt was: PERFECT punches (gold and white rays and a ring flying out, the word big, then settling), GOOD is a green ring, MISS shatters (red shards flying apart and falling, a grey cross, the word shaking).
- **Effects.** The hit-stop and the finisher's slow motion are in the clip times (and in the camera's orbit, shake and the particles); the fight's clock and the music are never touched.

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
  - *Intensity follows the fight, once a bar.* The tier is read from the boss's hp when the driver first plans a bar (at most 0.1 s before its bar line) and kept for the whole bar (`BeatMusicDriver::tier()`), so a hit in the middle of a bar never switches the music under it. Four tiers from (phase + progress through the phase) / phases, with a floor that rises: the first phase has none, every later phase starts at tier 2 at least, so a phase never begins under where the one before it ended, and the bar after a scripted silence is a lift (tier 2 at least). Tier 0: kick on the downbeat, a soft backbeat, a quiet hat on every eighth (the off-beat impacts have a grid), one held bass note. Tier 1: a pad, a kick on the third beat, hats on the eighths, bass on the beats. Tier 2: stabs, an open hat before each backbeat, bass on the eighths. Tier 3: sixteenth hats, a driving bass, ghost notes, a kick on every beat. On the test track phase 1 plays tiers 0 and 1 and phase 2 tiers 2 and 3.
  - *The build.* The bar before a finisher bar has a riser over the bar (joining late, over what is left, if the hit that opens the finisher lands mid-bar) and a snare roll that grows over the last half of the bar, with the bass, stabs and hats out of the way. The finisher bar plays tier 3 at full velocity: a kick on every beat, a crash and an impact.
  - *Silences cut everything.* A scripted silence stops the music at its bar line (a short fade) and leaves the count; the bar that follows opens with an impact. A count bar has no music, only its blips.
  - *Odd meters.* A bar the loop breaks keeps the kick on the downbeat; the snare falls on the second, fourth ... beat of the bar when the beat is whole (a 7/8 bar's short last beat takes none), hats and bass only on steps the bar has, and an empty beat has nothing but the riser and the impact.
  - *Harmony.* A progression in D minor per phase, each its own (the first a chord every two bars, Dm Dm Bb Bb; the second Dm Gm Bb A; the third Dm Bb Gm A), a chord a bar; the bar before a finisher plays the dominant, the finisher the tonic. The finisher's first bar has the impact and the crash; its later bars a lighter crash alone, so kick, snare, bass, stab, crash and accent are never stacked on one downbeat.
  - *Told moves.* Each move's impact gets an accent hit (stem 0 a thump, 1 a snare crack, 2 a bell), and the hats, stabs and bass leave that step. The accents sit on their own bus above the music and each ducks the music to about a fifth for 90 ms, so the player's audio cue reads over a full mix.
- **The player's cues** come from the same driver, on the synth's queue, so they are sample-timed and the only duck is the music's. A tick when a move is told (`TellTick`: its own voice for each answer, so a Guard and a Parry sound different; one tick for a finisher's three; none when another move's impact falls within 80 ms of it, as a jab's impact and the cross told on the same beat), a quiet pickup (`Pickup`) an eighth before the impact but never earlier than 40 ms inside the counter window that opens `counterEarly` before it (so pressing on it is never a whiff), and the verdicts on the impact (or at once if it has passed): a Perfect rings a tone of the playing chord (`PerfectChime`, the three tones in turn), a Good a softer, duller one (`GoodCue`), a Miss the buzz. The host triggers none of them.
- **The driver** (`BeatMusicDriver`) follows `ftcore::BeatBoss` each frame and turns the patterns into timed events (`MusicEvent`: Play, Cut, Revoke) on the fight's clock, up to 0.1 s ahead. It reads the bar playing from the fight and the next bar from `BeatBoss::upcomingBar()` (a read-only view of what the beat line shows as its second bar), so the next downbeat goes out before its bar line. When the bar that comes is not the one predicted (the boss's hp crossed a threshold in that last tenth of a second: a finisher or an hp silence), it revokes what it sent and plans the real bar. The count's blips come from it too, and the count back after a silence is scheduled when the silence starts.
- **The synth** (`Core/include/ftcore/BeatMusicSynth.h`, `BeatMusicSynth`) makes every sound: the kick a sine that drops from about 200 Hz to 46 Hz with a click, the snare noise plus a tone, hats high-passed noise, the bass a sine and two detuned saws through a low-pass that closes with each note, the stabs three notes of three detuned saws, the riser a noise band and a sine that climb, the impact a falling sub with a noise burst. It has its own sample clock: a sound is queued for an absolute frame and starts exactly there. The output is soft-clipped, so it never reaches full scale. Because that makes the output's own peak useless as a test, the synth also measures the mix in front of the clip (`preClipPeak()`); an autoplayed cine_track fight stays under 1.3 there, and the finisher's bars are no louder than the rest.

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

That writes `test_track_30s.wav` (the first 30 s), `test_track_full.wav` and `test_track_cues_only.wav` (the move accents and the count alone), and for the cinematic track `cine_track_full.wav` and one file a phase (`cine_track_phase_a.wav`, `_b`, `_c`). The same test case checks, in memory, that the kicks start on the bar lines to the sample, that nothing sounds in a silence but its count, that the build and the finisher's impact are there, that the mix peaks under full scale, and that each accent is clearly louder than the music it sits on.

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
| Retry from the checkpoint (after a defeat), or play again (after a victory) | Enter, Space, pad right shoulder |
| Give up (after a defeat; asked twice) | Q, Backspace, pad left shoulder |

The pad's face buttons are the fight's (Jump and Dodge among them), so the result does not read them; it reads no key until it has shown 0.5 s and every one of its keys has been let go, and Give up asks "press again" before it ends the run.

## Flags

For checking the map headless:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject Demo_BeatBoss -game -NullRHI -BeatBossAutoPlay -BeatBossQuitOnFinish -unattended -nosplash
```

- `-BeatBossAutoPlay`: Core's autopilot plays (`QteAssist::autoComplete`), and keys are ignored. After a defeat the retry prompt shows as it would for a player and is confirmed on its own a second later, three times at most; after that the run logs a warning and ends lost. It wins `cine_track` in 30 bars, about 78 s, with no miss (`test_track`: 17 bars, 38 s).
- `-BeatBossMissPhase=<n>`: the checkpoint retry, headless. It implies `-BeatBossAutoPlay`, but the host drives the autopilot itself (`BeatBoss::autopilot`, each action at its own time, as the Core tests' pilot does) and presses nothing from phase n's first bar (1 is the first phase) until the lead is down. The retry prompt then shows and is confirmed, `BeatBoss::retry` restarts at that phase's checkpoint, and the autopilot plays on to the end. On `test_track` with `-BeatBossMissPhase=2`: phase 2 starts after 11 bars with the boss at 240 hp, is lost in bar 18, and the retry restarts phase 2 on its bar 1 with the boss at 240 and the lead at 100, not the track's bar 1; the run then wins in 32 bars, 67.3 s, with 1 retry.
- `-BeatBossIdle`: presses nothing and never retries, so the fight must be lost. On `cine_track` the lead goes down in bar 8, on `test_track` in bar 8 as well.
- `-BeatBossQuitOnFinish`: exits when the fight is over (a victory, or a defeat under `-BeatBossIdle`, after the autoplay's retries or when the player gives up), with code 0 for a victory, 1 for a defeat, 2 when the 300 s watchdog ends a run that stalls and 3 when the track doesn't load. The engine's graceful exit returns 0 whatever it is asked, so `FTRequestExit` (`ForgottenThird.h`) keeps a non-zero code and, once the engine has torn the world down, ends the process with it as the engine ends a commandlet (the log flushed first); a victory takes the whole graceful exit. Checked: the idle run exits with 1 under `UnrealEditor-Cmd`, the autoplay and `-BeatBossMissPhase=2` runs with 0, each log complete to its last line.
- `-BeatWindowScale=<1..2>`, `-BeatWhiffMs=<0..400>`, `-BeatPairMode=<0..2>`, `-BeatUiScale=<1..2>`, `-BeatCuts=<0..2>`, `-BeatShake=<0..1>`, `-BeatFlash=<0..2>`, `-BeatTempo=<0.5..1>`: the accessibility options (see "Accessibility"), over the file `Saved/BeatAccess.json`; the file is read at the start, on every retry, and again whenever it changes (it lands at the next bar line). The log says which at the start (`BeatBoss: access {...}`). `-BeatBossExposure=<ev>` sets the stage's manual exposure, to tune by eye.
- `-BeatBossTrack=<id>`: another track in `Data/Battle/beat_boss/` (`cine_track` by default; `test_track` is the dense one the old shapes boss played).
- `-BeatBossRootMotion`: lets the clips' root bone travel (off by default: the actors stay where the stage puts them).
- `-BeatBossMetronome`: the old click-and-stems stand-in instead of the procedural music (the fight, its result and its numbers are the same either way).
- `-BeatBossShot=<prefix>`: renders the views with `HighResShot` as `<prefix>_<n>_<name>.png`, holding the fight's clock still a moment for each, then quits after the result. A view the track never gives is left out (the log says how many came). `01_countin` (the count-in); `10_shot_wide`, `11_shot_over_lead`, `12_shot_over_boss`, `13_shot_close_lead`, `14_shot_close_boss`, `15_shot_low_hero`, `16_shot_top_down` and `17_shot_finisher_orbit` (each of the eight shots, once its cut has settled, with a prompt on it); `20_prompt_attack` to `27_prompt_release` (a prompt of each answer, its ring part of the way in); `30_hitstop` (the lead's blow on the beat, the hit-stop on it, PERFECT bursting), `31_silence_roar` (a silence, 0.6 s in, the boss roaring), `32_stagger` (the boss in its recoil), `33_finisher` (three strikes told at once); `40_victory_freeze` (0.1 s after the final blow), `41_victory_ramp_a` and `42_victory_ramp_b` (0.65 s and 1.4 s, the visuals ramping up), `43_retry` (with `-BeatBossMissPhase`, the retry prompt after the lost phase) and `44_result` (the victory). It implies `-BeatBossAutoPlay` and needs a renderer, so not `-NullRHI`:

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

- **Timing.** A boss move's clip is started so its impact frame lands on the move's impact, at a rate clamped to 0.75 to 1.35; if the windup does not fit after the last move, the clip starts partway in. The lead's answer is pre-rolled the same way: when a move is told, the lead's `ok` clip is scheduled so its blow (or guard, dodge or jump) lands on the move's impact too, starting during the lead-in once the lead is free of his previous answer. The choreography is committed to the music; the press decides which branch plays. A Perfect or Good keeps the clip, and after a Perfect the `then` follow-up is thrown straight from the contact (0.08 s after the impact, not after the clip's recovery) to land on the first eighth of the beat at least 0.4 s later, its windup entered partway through if it does not fit, if that is before his next answer or the end of the bar: a parry's counter lands a beat after it. `BeatChoreo::hitContact()` says when the last blow that hurt the boss lands on screen, so the host holds its damage popup, flash and shake until the track reaches it. A Miss (unanswered, a late press, a hold let go) cuts the clip at the impact to the move's `miss` clip, which begins on its own blow. A Whiff (a counter pressed too early) cuts to the stumble clip (`whiff` in the file) at the press, and the answer after the lockout plays from its press, the old way. The stance loops on the beat, one cycle a whole number of beats.
- **Hit-stop and slow motion** are visual only. A Perfect blow pauses the animation for 50 to 80 ms, and it has caught up by the next beat; in a finisher bar the animation dips between the strikes. The clip times in a sample already have both applied, and the fight's clock and the music are never touched, so every impact is on screen on its beat. `visualRate` and `slowMo` are for effects that should follow (sparks, the camera's move).
- **Shots.** `wide`, `over_lead`, `over_boss`, `close_lead`, `close_boss`, `low_hero`, `top_down` and `finisher_orbit`; the host maps each to a camera. A cut is only ever on a beat or a bar line: at the beat a move's tell falls on, to that move's shot; to `finisher_orbit` on a finisher bar's line; on the next beat after a stagger, the victory and the defeat. A silence cuts nothing but the roar: on the bar line it stops at, the camera cuts to the file's `silence` shot (`close_boss`), the boss plays the file's `silence` clip (`boss_roar`) in the time of the silence and its count, and the shot that was there comes back when the track runs.
- **The win.** On Victory the final blow freezes for 0.2 s, then the visuals ramp from a quarter speed to full over 1.5 s (`visualRate`, `hitStop` and `slowMo` of the sample say so; the clock and the music are untouched).
- **Data.** `Data/Anim/beat_clips.json` lists the clips (id, length in seconds, impact offset, loop; up to 128; the animation side overwrites the placeholder numbers and may add ids). `Data/Battle/beat_boss/<track>.choreo.json` has the stance clips, the stagger, victory, defeat and silence, and for each move, by index and checked by id, the boss's clip, the lead's clip on an answer (`ok`, `then` after a Perfect), on a miss, the boss's reaction to a blow (`hit`), the shot and the prompt's anchor (`boss_head`, `boss_fist_r`, `boss_fist_l`, `boss_chest`, `lead_head`, `lead_chest`, `screen_center`). Ids and numbers only; the loader refuses an unknown clip, a wrong move id or count, or a stance that does not loop.
- **Limits.** `observe` must see a move's prompt while it is open (every frame does); one opened and answered between two calls is not choreographed and is counted in `unmatched()`. A boss move whose windup is on screen is not interrupted by a reaction to a blow, and a `then` follow-up that could not land before the next move's answer is due is not played.
- **For the animation side.** Both fighters' clips are pinned by their `impact_s`, so the length of a windup is free: the clip is started `impact_s / rate` before the blow (at most 1.35 times speed), or partway in when the previous clip leaves no room. Give each answer clip its blow (guard, dodge or jump) at `impact_s`, and keep the track's moves far enough apart that a lead's answer (its `impact_s` over 1.35, plus 0.2 s of follow-through after the last) fits between two impacts, or the windup is skipped.

## The cinematic track

`Data/Battle/beat_boss/cine_track.json` and `cine_track.choreo.json` are the demo's default track, made for the real clips. Three phases at 96, 100 and 108 bpm (4/4), a loop of four bars each, a boss attack on a clear beat with room for its windup (a hook or thrust parried, a smash guarded, sweeps dodged or jumped, a grab paired), the lead's exchanges on attack prompts (jab, cross, kick, a held kick), one silence (phase two, at half the boss's hp), a two-bar finisher in the last two phases (three strikes, `finisher_orbit`, the slow motion dipping between them) and a stagger moment from the open rests. No two moves in a row share a shot and every shot and anchor is used. Each phase has a Release (tell 1.4, impact 2.1, held to 2.2), offered when there is Load. The Core tests (`test_beat_cine.cpp`) check that the autopilot wins it (78 s, 30 bars, no miss), an idle player loses it, a lost phase 1, 2 or 3 retries from its checkpoint and wins, every blow of both fighters is on its impact on screen with the windup played in full, and the cuts are on beats.

## Accessibility

The options are for the beat screen only (`Core/include/ftcore/BeatAccess.h`, `BeatAccessOptions`; the story's auto-complete, hold toggle and reduce-flashes stay in `Settings`). All defaults are today's behaviour. WCAG 2.1 AA on PC keyboard and gamepad is the target.

| id | default | range | what it does |
|---|---|---|---|
| `beat.window_scale` | 1.0 | 1 to 2 | Scales the Good windows fully (a strike's, a counter's early edge and late limit, a hold's slack, a pair's width) and the Perfect widths at half the rate, capped at 1.5. The result screen has no "assisted" mark. `maxBeatWindowScale` caps it per track so no two windows of one button overlap (cine_track tolerates 2.0). |
| `beat.whiff_lockout_ms` | 400 | 0 to 400 | The lockout after a counter is pressed before its window. It is this divided by `beat.window_scale`. |
| `beat.pair_mode` | 0 | 0 to 2 | 0 two buttons together; 1 the second may follow the first by 0.5 s (nothing simultaneous); 2 one press counts as both. Full auto-complete is not a substitute. |
| `beat.ui_scale` | 1.0 | 1 to 2 | The prompts, labels and strip of the HUD. |
| `beat.cuts_reduced` | 0 | 0 to 2 | 1: a shot holds 2.5 s at least and no cut falls on an impact beat; 2: one shot for the phase. |
| `beat.shake_scale` | 1.0 | 0 to 1 | The camera shake and jolt. |
| `beat.flash_level` | 0 | 0 to 2 | 1: no edge frame, bursts fade slowly; 2: no screen-wide effect and no ring bursts. `Settings::reduceFlashes` raises it to 1. |
| `beat.tempo_scale` | 1.0 | 0.5 to 1 | Practice tempo: `rescaleBeatBossDef` plays the track at that share of its tempo (every bpm times it, every time divided by it, never under 40 bpm). |

The assists (`QteAssist`) and the windows are changed in a running fight with `BeatBoss::requestAccess`: it lands at the next bar line, or at once from a retry, never in the middle of a bar. The options are stored as the ids above in a JSON object (`encodeBeatAccess`, `decodeBeatAccess`), in `Saved/BeatAccess.json`, which the host reads (and re-reads when it changes: the assists and windows land at the next bar line). Settings' reduce-flashes raises `beat.flash_level` to 1, and its auto-complete and hold toggle are the assists. The window scale is held, with a warning, at the widest the track allows. There are no menu rows for these yet; the file and the flags set them.

## Not yet

- No story step opens the map, and the track's music cue has no recording: the procedural music stands in. It is one fixed key, one voice set and one pattern family for every track; a track's own style, and a mix balanced by ear on real speakers, wait for a person to listen.
- Giving up at the retry prompt is played by hand only; `-BeatBossMissPhase` covers the retry itself.
- The clips are text-to-motion output retargeted onto one body: the lead's stance has the hands up at the face (the host lowers them with a procedural arm pose, below). The camera and lighting are first passes, and the colours are the demo's own until the per-world table.
- Deferred: key remapping, blood or gore per world, the whip transition between shots, the per-world colour table, Beat Overload and the shared Release threshold.
- The Release has no clip of its own: its answer plays `lead_block`.
