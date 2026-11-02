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
// * The tier is frozen at the bar line. The driver reads the fight's hp once for each bar, when it first plans it
//   (at most kMusicLookahead before the bar line), and plays the whole bar at that tier, so a hit in the middle
//   of a bar cannot switch the music under it. BeatMusicDriver::tier() is the playing bar's.
// * Intensity follows the fight. level = (phase + progress through the phase) / phase count, in four tiers:
//   0 sparse (kick on the downbeat, a soft backbeat, a quiet hat on every eighth, a held bass note), 1 (a pad,
//   more kick, the hats louder, the bass on the beats), 2 (stabs, an open hat, bass on the eighths), 3 (full:
//   sixteenth hats, a driving bass, ghost notes). Each phase has a floor that rises: the first phase none, every
//   later one tier 2, so a phase never starts below where the one before it ended. The bar after a scripted
//   silence is a lift: tier 2 at least.
// * Each phase has its own chords (a progression a bar or two long); the finisher is the tonic and the build
//   before it the dominant. A finisher's first bar has the impact and the crash; its later bars a lighter crash
//   alone, so that four loud sounds are never stacked on one downbeat.
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
// The driver also sounds the player's cues, on the same queue (docs/BEAT_BOSS_AUDIO.md, "the synth is the only place
// the music ducks"): a tick at every move's tell (TellTick; one for a finisher's three, none when an impact falls
// within 80 ms of it) and a quiet pickup (Pickup) shortly before the move's window opens, at an eighth before the
// impact but never earlier than 40 ms inside the window's open edge, so pressing on it is never a whiff; and the
// verdicts, on the impact (or at once if that has passed): a Perfect rings a tone of the playing chord
// (PerfectChime, the three chord tones in turn), a Good a softer, duller one (GoodCue), a Miss the buzz at once.
// The host triggers none of them. A scripted silence cuts everything and leaves the count: the count blips for the silence's count back are
// scheduled when it starts, and the bar that follows is planned during its count, with an impact.

#include "ftcore/Api.h"
#include "ftcore/BeatBoss.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

inline constexpr int kMusicMaxSteps = 2 * static_cast<int>(kMaxBossBarEighths); // sixteenths in the longest bar
inline constexpr double kMusicLookahead = 0.1;                                  // seconds scheduled ahead

// The chord a bar plays: its root as a MIDI note (the bass's) and its quality.
struct MusicChord {
    int root = 38;
    bool isMajor = false;
};

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
    bool lift = false;              // the bar after a scripted silence: tier 2 at least
    std::uint8_t finisherBar = 0;   // a finisher bar: its index (0 the first)
    std::int8_t forcedTier = -1;    // 0..3: play at this tier whatever the fight's hp says (the driver freezes a bar's tier at its bar line)
    std::array<std::uint8_t, kMusicMaxSteps> accent{}; // a told move's impact on this step: 1 + its stem's timbre (0 thump, 1 snare, 2 bell)
};

// The tier (0..3) the context plays at.
FTCORE_API int musicTier(const MusicBarContext& ctx);
// The tier no bar of that phase (or after a silence) plays under.
FTCORE_API int musicTierFloor(const MusicBarContext& ctx);
// The chord the bar plays (the bass's root and the quality).
FTCORE_API MusicChord musicChord(const MusicBarContext& ctx);
// Fills `out` (every field). No allocation.
FTCORE_API void planBeatMusic(const MusicBarContext& ctx, MusicPattern& out);

// ---- the driver ----

// Everything the synth can sound.
enum class MusicSound : std::uint8_t {
    Kick, Snare, HatClosed, HatOpen, Bass, Stab, Riser, Impact, // the music
    AccentThump, AccentSnare, AccentBell,                       // a told move's impact
    CountClick, CountVoice, Good, Miss,                         // the count and the answers
    TellTick,     // a move told: aux = its answer (0..7), 8 a finisher's tell
    Pickup,       // a quiet tick shortly before a move's window opens, inside it
    PerfectChime, // a Perfect: note = a tone of the playing chord
    GoodCue,      // a Good: the Perfect's softer, duller cousin; note = a tone of the playing chord
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

    // The tier (0..3) the playing bar was planned at and keeps for all its length (-1 before the first bar).
    int tier() const { return tierCur_; }
    std::uint32_t dropped() const { return dropped_; }
    std::uint32_t revoked() const { return revoked_; }

private:
    struct Cue {
        double impact = 0.0; // track time
        std::uint32_t id = 0;
        std::uint8_t timbre = 0;
    };
    static constexpr std::size_t kCues = 64;
    struct Judgement {
        std::uint32_t prompt = 0;
        QteGrade grade = QteGrade::Pending;
        double time = 0.0; // the fight's clock
    };
    static constexpr std::size_t kJudgements = 16;
    struct Sent {
        MusicSound sound = MusicSound::TellTick;
        std::uint8_t aux = 0;
        double time = 0.0;
    };
    static constexpr std::size_t kSent = 64;
    struct Candidate {
        MusicSound sound = MusicSound::TellTick;
        std::uint8_t aux = 0;
        std::uint8_t velocity = 0;
        double time = 0.0;
        bool impact = false; // not a cue: an impact (for the ticks to yield to)
    };
    static constexpr std::size_t kCandidates = 128;

    void add(MusicEvents& out, const MusicEvent& e);
    void play(MusicEvents& out, MusicSound s, double time, std::uint8_t velocity, std::uint8_t note = 0,
              std::uint8_t aux = 0, double length = 0.0);
    void trackCues(const BeatBoss& fight);
    void fillContext(const BeatBoss& fight, const BossBarPreview& bar, double startTrack, bool entry, bool nextFinisher,
                     bool lift, MusicBarContext& ctx) const;
    // Plans the bar that starts at `start` (the fight's clock) and sends its steps after the mark, up to `until`.
    void scheduleBar(const MusicBarContext& ctx, double start, double stepSeconds, double until, bool& riserSent,
                     MusicEvents& out);
    void sendCountBlip(MusicEvents& out, BeatCountKind kind, double time);
    // The ticks and pickups of the open moves and of the bar `up` (null: none; it starts at `upStart` on the fight's
    // clock), sent once each when they fall inside the lookahead. Track time t is clock anchorClock + (t - anchorTrack).
    void scheduleCues(const BeatBoss& fight, const BossBarPreview* up, double upStart, double anchorClock,
                      double anchorTrack, double now, double until, MusicEvents& out);
    void scheduleJudgements(const BeatBoss& fight, double now, MusicEvents& out);
    bool wasSent(MusicSound s, std::uint8_t aux, double time) const;
    void markSent(MusicSound s, std::uint8_t aux, double time);
    void forgetSent(double from);

    double mark_ = -1e30;      // music events up to this time are sent
    double countMark_ = -1e30; // count blips up to this time are sent
    std::uint32_t barsSeen_ = 0;
    int lastPhase_ = -1;
    MusicBarKey predicted_{};
    bool predictedValid_ = false;
    bool entryNext_ = true;
    bool entryCur_ = true;
    bool afterSilence_ = false;
    bool liftCur_ = false;  // the playing bar follows a silence
    bool liftNext_ = false;
    MusicChord chordCur_{}; // the playing bar's chord, and the next bar's once planned
    MusicChord chordNext_{};
    double nextBarClock_ = 0.0; // the next bar's line on the fight's clock
    std::uint32_t chimes_ = 0;  // Perfects so far: which chord tone rings
    std::array<Judgement, kJudgements> judgements_{};
    std::size_t judgementCount_ = 0;
    std::array<Sent, kSent> sent_{};
    std::size_t sentCount_ = 0;
    bool riserSent_[2] = {false, false}; // the current bar, the next
    float phaseStartHp_ = 0.0f;
    int tierCur_ = -1;  // the playing bar's tier, fixed when it was first planned
    int tierNext_ = -1; // the predicted next bar's
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
