#pragma once

// The beat boss demo's music (docs/BEAT_BOSS_DEMO.md, "The clock and the music"): a pattern sequencer that
// decides, for any bar of the bar map, which voices hit on which sixteenth and how hard, and a driver that
// follows the fight (BeatBoss.h) and turns those patterns into timed events for the synth (BeatMusicSynth.h).
// No recorded audio: everything is code.
//
// The pattern. A bar is a grid of sixteenths: two per eighth of the bar map, so a 6/8 beat is six of them
// and a 7/8 bar has fourteen.
//
//   ftcore::MusicBarContext ctx;      // the bar's shape and where the fight stands
//   ftcore::MusicPattern pattern;
//   ftcore::planBeatMusic(ctx, pattern);
//   pattern.at(step, ftcore::MusicVoice::Kick).velocity   // 0: no hit
//
// Voices: kick, snare (a clap layered on later tiers), closed and open hat (a crash on the finisher), a bass
// line, a stab or pad chord, a riser and an impact hit; and the move accents, an accent hit on each told
// move's impact. Rules:
// * Intensity follows the fight. level = (phase + progress through the phase) / phase count, in four tiers:
//   0 sparse (kick on the downbeat, a soft backbeat, hats on the beats, a held bass note), 1 (a pad, more
//   kick, hats on the eighths, the bass on the beats), 2 (stabs, an open hat, bass on the eighths), 3 (full:
//   sixteenth hats, a driving bass, ghost notes). The first phase of a two-phase fight stays in tiers 0 and 1.
// * The bar before a finisher bar (`nextFinisher`) gets a riser over the whole bar and a snare roll that
//   crescendos over the last half of the bar; the bass and the stabs drop out for the roll. The finisher bar
//   plays tier 3 at full velocity, a kick on every beat, a crash and an impact.
// * A silent bar (`silent`: a scripted silence, a count) is empty. An empty beat of the bar map is empty
//   for every voice but the riser and the impact.
// * An odd meter (a bar the loop breaks) keeps the kick on the downbeat; the rest is laid out by the bar's
//   own beats: the snare on the second, fourth ... beat when the beat is whole, hats and bass only on steps
//   the bar has.
// * A told move's impact (`accent`) gets an accent hit (the stem picks its timbre: thump, snare, bell) and the
//   hats, stabs, ghost notes and bass leave that step so it reads clearly; the synth also ducks the music.
// * The bass follows a fixed progression in D minor that changes every bar or two.
// * Deterministic: the same context gives the same pattern. Nothing allocates.
//
// The driver. One per fight; the host calls it every frame after the fight has taken the frame.
//
//   ftcore::BeatMusicDriver music;
//   music.reset(now, events);                                   // begin, retry
//   for each BeatBossEvent e:      music.onEvent(e);
//   after fight.update(now), fight.beatLine(line):  music.update(fight, line, now, countKind, events);
//   for (const MusicEvent& e : events) ...                      // Play at e.time (the fight's clock), Cut, Revoke
//
// It schedules ahead (kMusicLookahead): every sixteenth whose time falls before now + lookahead, so the host
// can give each event an exact sample position. The next bar's first steps go out before its bar line, from
// BeatBoss::upcomingBar; if the bar that comes is not the one predicted (the boss's hp crossed a threshold in
// the last tenth of a second), the driver revokes the pending events from the bar line and plans the real bar.
// A scripted silence cuts everything and leaves the count: the count blips for the silence's count back are
// scheduled when it starts, and the bar that follows is planned during its count, with an impact.

#include "ftcore/Api.h"
#include "ftcore/BeatBoss.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

inline constexpr int kMusicMaxSteps = 2 * static_cast<int>(kMaxBossBarEighths); // sixteenths in the longest bar
inline constexpr double kMusicLookahead = 0.1;                                  // seconds scheduled ahead

enum class MusicVoice : std::uint8_t { Kick, Snare, HatClosed, HatOpen, Bass, Stab, Riser, Impact, Accent, Count };
inline constexpr std::size_t kMusicVoices = static_cast<std::size_t>(MusicVoice::Count);

// One hit. velocity 0: none. note: MIDI (Bass, Stab). length: sixteenths (Bass, Stab, Riser, Impact, open hat).
// aux: Snare 1 = clap layered; HatOpen 1 = crash; Stab bit 0 = major (else minor), bit 1 = pad; Accent = the
// stem's timbre (0 thump, 1 snare, 2 bell).
struct MusicHit {
    std::uint8_t velocity = 0;
    std::uint8_t note = 0;
    std::uint8_t length = 0;
    std::uint8_t aux = 0;
};

struct MusicPattern {
    std::uint8_t steps = 0; // the bar's sixteenths
    std::uint8_t tier = 0;  // 0..3
    std::array<std::array<MusicHit, kMusicVoices>, kMusicMaxSteps> hits{};

    const MusicHit& at(int step, MusicVoice v) const { return hits[static_cast<std::size_t>(step)][static_cast<std::size_t>(v)]; }
    FTCORE_API int hitCount() const;
    FTCORE_API int velocitySum() const;
};

struct MusicBarContext {
    std::uint8_t eighths = 8;       // the bar's length in eighths (1..16)
    std::uint8_t beatEighths = 2;   // a beat: 2 (quarter) or 3 (dotted quarter)
    std::uint16_t emptyBeats = 0;   // bit b: beat b + 1 is empty (the music leaves it)
    std::uint8_t phase = 0;
    std::uint8_t phaseCount = 1;
    float phaseProgress = 0.0f;     // 0..1 through the phase (the boss's hp from where the phase began to its floor)
    std::uint16_t bar = 1;          // the bar in the loop, from 1 (0: a finisher bar)
    bool finisher = false;
    bool nextFinisher = false;      // a finisher bar follows: the build
    bool entry = false;             // the first bar of a phase, or of the track after a silence or a count: an impact
    bool silent = false;            // nothing plays
    std::array<std::uint8_t, kMusicMaxSteps> accent{}; // a told move's impact on this step: 1 + its stem's timbre (0 thump, 1 snare, 2 bell)
};

// The tier (0..3) the context plays at.
FTCORE_API int musicTier(const MusicBarContext& ctx);
// Fills `out` (every field). No allocation.
FTCORE_API void planBeatMusic(const MusicBarContext& ctx, MusicPattern& out);

// ---- the driver ----

// Everything the synth can sound.
enum class MusicSound : std::uint8_t {
    Kick, Snare, HatClosed, HatOpen, Bass, Stab, Riser, Impact, // the music
    AccentThump, AccentSnare, AccentBell,                       // a told move's impact
    CountClick, CountVoice, Good, Miss,                         // the count and the answers
    Count
};

enum class MusicEventKind : std::uint8_t {
    Play,   // sound `sound` at `time`
    Cut,    // at `time` everything stops (a quick fade) and nothing pending from then on plays
    Revoke, // pending sounds from `time` on are dropped (those already playing go on)
};

struct MusicEvent {
    MusicEventKind kind = MusicEventKind::Play;
    MusicSound sound = MusicSound::Kick;
    std::uint8_t velocity = 0;
    std::uint8_t note = 0;
    std::uint8_t aux = 0;
    double time = 0.0;   // the fight's clock
    double length = 0.0; // seconds (Bass, Stab, Riser, Impact, open hat); 0: the sound's own
};

inline constexpr std::size_t kMaxMusicEvents = 256; // per call; more are dropped

struct MusicEvents {
    std::array<MusicEvent, kMaxMusicEvents> items{};
    std::size_t count = 0;

    const MusicEvent* begin() const { return items.data(); }
    const MusicEvent* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const MusicEvent& operator[](std::size_t i) const { return items[i]; }
};

// A bar's identity: what the driver compares a prediction with.
struct MusicBarKey {
    std::int8_t phase = -1;
    std::int8_t bar = -1;
    std::int32_t loop = 0;
    bool finisher = false;
    bool operator==(const MusicBarKey& o) const {
        return phase == o.phase && bar == o.bar && loop == o.loop && finisher == o.finisher;
    }
};

class BeatMusicDriver {
public:
    // Starts over (a fight begins or retries at `now`): everything pending is revoked and cut.
    FTCORE_API void reset(double now, MusicEvents& out);
    // Every event the fight returns (the driver reads the silence, the end and the answers' voices).
    FTCORE_API void onEvent(const BeatBossEvent& e);
    // After the frame's fight.update and fight.beatLine. `count` is the kind of the count playing or coming.
    // `out` is cleared first.
    FTCORE_API void update(const BeatBoss& fight, const BeatLine& line, double now, BeatCountKind count, MusicEvents& out);

    std::uint32_t dropped() const { return dropped_; }
    std::uint32_t revoked() const { return revoked_; }

private:
    struct Cue {
        double impact = 0.0; // track time
        std::uint8_t timbre = 0;
    };
    static constexpr std::size_t kCues = 64;

    void add(MusicEvents& out, const MusicEvent& e);
    void play(MusicEvents& out, MusicSound s, double time, std::uint8_t velocity, std::uint8_t note = 0,
              std::uint8_t aux = 0, double length = 0.0);
    void trackCues(const BeatBoss& fight);
    void fillContext(const BeatBoss& fight, const BossBarPreview& bar, double startTrack, bool entry, bool nextFinisher,
                     MusicBarContext& ctx) const;
    // Plans the bar that starts at `start` (the fight's clock) and sends its steps after the mark, up to `until`.
    void scheduleBar(const MusicBarContext& ctx, double start, double stepSeconds, double until, bool& riserSent,
                     MusicEvents& out);
    void sendCountBlip(MusicEvents& out, BeatCountKind kind, double time);

    double mark_ = -1e30;      // music events up to this time are sent
    double countMark_ = -1e30; // count blips up to this time are sent
    std::uint32_t barsSeen_ = 0;
    int lastPhase_ = -1;
    MusicBarKey predicted_{};
    bool predictedValid_ = false;
    bool entryNext_ = true;
    bool entryCur_ = true;
    bool afterSilence_ = false;
    bool riserSent_[2] = {false, false}; // the current bar, the next
    float phaseStartHp_ = 0.0f;
    bool over_ = false;
    bool silencePending_ = false;
    double silenceAt_ = 0.0;
    int silenceMs_ = 0;
    bool endPending_ = false;
    bool endVictory_ = false;
    double endAt_ = 0.0;
    std::uint32_t lastPrompt_ = 0;
    std::array<Cue, kCues> cues_{};
    std::size_t cueCount_ = 0;
    std::uint32_t dropped_ = 0;
    std::uint32_t revoked_ = 0;
};

} // namespace ftcore
