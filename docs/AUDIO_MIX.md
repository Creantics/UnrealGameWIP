# Audio variation and mix states

Two Core pieces close the gaps the audio review found: sounds that play the same way every time,
and a mix that nothing ducks under dialogue or thins out in combat. Both are header-only plain
C++20 in `Core/include/ftcore/`, allocate nothing, and are deterministic. Tests:
`Core/tests/test_audio_pick.cpp`, `Core/tests/test_mix_state.cpp`, plus the reader cases in
`Core/tests/test_audio.cpp`.

## AudioPick.h: which take plays

A cue can have up to 16 recorded variants (takes). Each play returns a variant index and a pitch
and volume offset.

Rules:
- Never the same variant twice in a row when N >= 2. Never one of the last two when N >= 4.
  `avoidLast` can raise this (up to 4, and at most N - 1). It can never lower it.
- Shuffle bag: each round of N plays uses every variant exactly once, in a shuffled order, so use
  stays equal over a long run. The bag's head is picked so the rules above hold across rounds.
- `roundRobin`: plays the variants in order (k, k+1, ..., wrapping) instead. The start k comes
  from the cue's seed, so two round-robin cues do not run in lockstep.
- Jitter: pitch uniform in +-`pitchCents` cents, volume uniform in +-`volumeDb` dB. Both are 0 by
  default. Every play draws the same count of random numbers, so changing a jitter range never
  changes the variant order.
- Independence: each cue's state carries its own xorshift32 generator, seeded from a game-wide
  seed and a hash of the cue id. One cue's sequence never depends on how often other cues played.
  `audioCueKey` is a 32-bit FNV-1a hash, so two ids can collide. Colliding ids seeded by id alone
  would share a sequence. Use the seed overload that also takes the cue's index in
  `AudioCueTable::cues`. It folds the index in through bijections, so two cues with colliding ids
  still start from different generator states.

API:

```cpp
struct AudioPickRule { int variants = 1; int avoidLast = -1; bool roundRobin = false;
                       float pitchCents = 0.f; float volumeDb = 0.f; };
struct AudioPickResult { int variant; float pitchCents; float volumeDb;
                         float pitchRatio() const; float volumeLinear() const; };
struct AudioPickState { /* ~32 bytes, caller owns one per cue */ };

AudioPickRule audioPickRuleFor(const AudioCue& cue);
int audioPickVariants(const AudioPickRule& rule);   // clamped 1..16
int audioPickAvoid(const AudioPickRule& rule);      // the effective no-repeat depth
std::uint32_t audioCueKey(std::string_view cueId);  // FNV-1a
void audioPickSeed(AudioPickState& st, std::uint32_t seed, std::uint32_t cueKey);
void audioPickSeed(AudioPickState& st, std::uint32_t seed, std::string_view cueId);
void audioPickSeed(AudioPickState& st, std::uint32_t seed, std::string_view cueId,
                   std::uint32_t tableIndex);   // preferred where the table index is known
AudioPickResult audioPick(const AudioPickRule& rule, AudioPickState& st);
```

If a reload changes a cue's variant count under a live state, the bag refills and any remembered
index that no longer exists is dropped.

## MixState.h: the game-wide mix

States, in priority order: Menu > Cutscene > Dialogue > Combat > Explore. Callers request
several at once as a bit mask (`mixRequest(MixMode::Dialogue) | mixRequest(MixMode::Combat)`).
The highest one wins. No request at all means Explore.

Buses: Music, Ambience, Sfx, Footsteps, Voice, Ui.

Default targets in dB (one table, `MixTable::gainDb`):

| State    | Music | Ambience | Sfx | Footsteps | Voice | Ui |
|----------|------:|---------:|----:|----------:|------:|---:|
| Explore  | 0     | 0        | 0   | 0         | 0     | 0  |
| Combat   | 0     | -6       | 0   | -3        | 0     | 0  |
| Dialogue | -6    | -4       | -3  | -3        | 0     | 0  |
| Cutscene | -3    | -6       | -2  | -6        | 0     | -6 |
| Menu     | -12   | -12      | -12 | -12       | -12   | 0  |

Voice caps per state: Explore 24, Combat 16, Dialogue 16, Cutscene 20, Menu 8.

Ramp law: linear in dB. When a bus's target changes, the bus moves from where it is to the new
target in exactly `attackSeconds` when it falls (default 0.15 s) or `releaseSeconds` when it
rises (default 0.6 s). This holds whatever the distance. A target change mid-ramp starts a new
ramp from the current gain. The gain stops on the target and never overshoots. The result is the
same at any step rate as long as requests change at the same moments. A `dt` that is negative or
not finite counts as 0. An attack or release time of 0 jumps straight to the target. On
construction and `reset()`, every bus sits at the Explore target.

API:

```cpp
enum class MixMode : std::uint8_t { Explore, Combat, Dialogue, Cutscene, Menu, Count };
enum class MixBus : std::uint8_t { Music, Ambience, Sfx, Footsteps, Voice, Ui, Count };
using MixRequests = std::uint8_t;
constexpr MixRequests mixRequest(MixMode m);
constexpr MixMode activeMixMode(MixRequests requests);
constexpr MixBus mixBusFor(AudioBus bus);
MixBus mixBusForCue(const AudioCue& cue);
struct MixTable { float gainDb[5][6]; float attackSeconds = 0.15f; float releaseSeconds = 0.6f;
                  int voiceCap[5]; };
MixTable defaultMixTable();
struct MixGains { float linear[6]; float db[6]; float operator[](MixBus) const; };
class MixState {
    MixState(); explicit MixState(const MixTable&);
    void reset();
    const MixGains& step(MixRequests requests, float dt);
    MixMode active() const; const MixGains& gains() const;
    float gainDb(MixBus) const; float targetDb(MixBus) const;
    int voiceCap() const; int voiceCapFor(MixMode) const;
};
struct MixVoice { int priority = 0; float ageSeconds = 0.f; };
struct MixVoiceStops { std::uint32_t keep[32]; int keepCount; std::size_t count;
                       bool stops(std::size_t i) const; };
MixVoiceStops mixVoicesToStop(std::span<const MixVoice> voices, int cap);
```

`mixVoicesToStop` takes any number of voices and returns the ones that keep playing, so at most
`cap` keep playing. Every other voice is to be stopped (`stops(i)`, `count` in all). It stops the
lowest priority first, then the oldest, and breaks a full tie by the lower index. At most 32
voices can be kept, so a cap above 32 counts as 32. It uses a fixed-capacity selection,
O(n x cap), with no allocation.

Gains: a table value of -infinity, or anything at or below -120 dB (`kMixFloorDb`), is a mute.
The bus ramps to -120 dB, where its linear gain is exactly 0, and comes back as usual. NaN and
+infinity count as 0 dB. `priority` uses the cue's existing `prio` (0..100). The per-cue `max`
still limits one cue's own voices. That limit is separate from the per-state cap.

## Data keys (Data/Audio/cues.json)

The reader is `ftcore::importAudioCues` in `Core/src/AudioCues.cpp`. Unreal compiles the same
source through its wrapper. New optional keys:

| Key          | Range / values | Default | Meaning |
|--------------|----------------|---------|---------|
| `variants`   | 1..16 | 1 | recorded takes of the cue |
| `avoidLast`  | 0..4 | automatic | recent takes kept out; only raises the automatic rule |
| `roundRobin` | true / false | false | in order instead of shuffled |
| `pitchCents` | 0..1200 | 0 | +- pitch jitter |
| `volumeDb`   | 0..12 | 0 | +- volume jitter. `gainDb` stays the cue's level. |
| `mixBus`     | music, ambience, sfx, footsteps, voice, ui | from `bus` | the mix bus when it is not the cue bus |

Missing keys keep today's behaviour: one take, no jitter, and the mix bus of the cue's `bus`
(music to Music, voice to Voice, sfx to Sfx, ambience to Ambience). The cue `bus` key itself is
unchanged. It still picks the import folder and the loudness target. Footsteps and Ui exist only
as mix buses, set through `mixBus`. `Data/Audio/cues.json` is written by `tools/make_cues_json.py`,
so the new keys go in through that tool's sheet when takes exist.

## How the Unreal side should call them

(Not wired yet. This is the intended use; nothing under `Source/` changed.)

- **Audio subsystem start**: keep one `AudioPickState` per cue next to the cue table, for example
  an array parallel to `AudioCueTable::cues`, sized at load. Seed each with
  `audioPickSeed(state, gameSeed, cue.id, cueIndex)`. Keep `audioPickRuleFor(cue)` with it.
- **Each play of a cue**: `audioPick(rule, state)`. Pick the imported take for `r.variant`, for
  example `/Game/Audio/<Bus>/<CueId>_<NN>`, with a single-take cue staying `<CueId>`. Set the
  component's pitch multiplier to `r.pitchRatio()` and its volume to
  `10^((cue.gainDb + r.volumeDb) / 20)`. The take naming and import are the commandlet's job and
  are not decided here.
- **Each frame**: build the request mask from game state. Request Menu while a menu is open,
  Cutscene during sequences, Dialogue while a line plays, and Combat while a fight is on.
  Call `mix.step(mask, DeltaSeconds)`. Push `gains.linear[bus]` to each bus's Sound Class or
  Submix volume, in a Sound Class Mix or a submix's output volume, for example. Route each cue
  by `mixBusForCue(cue)`.
- **Voice cap**: when a sound starts, or once per frame, fill a `MixVoice` array with every
  active voice (cue `prio`, seconds since start). Stop, with a short fade, each voice where the
  result of `mixVoicesToStop(voices, mix.voiceCap())` gives `stops(i)`.
