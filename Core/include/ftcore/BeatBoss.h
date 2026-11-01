#pragma once

// The beat boss (docs/COMBAT_SYSTEMS.md, "Beat boss"): a world's final as one authored track. Where
// the beat fight (BeatFight.h) deals bars round robin to many small enemies, a beat boss plays a
// written bar map: every move the boss makes is pinned to a bar and a beat of its music, and the lead
// answers it with the action buttons (ActionButton.h; never arrows, which are the Turns battle's).
//
//   ftcore::BeatBoss fight;
//   fight.begin(track.def, tuning, clockNow);
//   every frame:  for (const ftcore::BeatBossEvent& e : fight.update(clockNow)) ...; // sparks, sounds
//   on a key:     fight.press(button, eventTime) / fight.release(button, eventTime)
//   draw:         prompts (promptCount(), prompt(i), approach(prompt(i))), fight.beatLine(line), hp,
//                 bossHp, multiplier(), streak(), load(), openRests()
//   fight.state() == Victory: the step is done. Defeat: retry(clockNow) restarts at the checkpoint.
//
// The clock. Every time passed in is the fight's clock: a pause-aware wall clock that keeps running
// through the fight's own silences (never the song time, which stops when the music does). The fight
// keeps a second clock, the track: seconds of the boss's music played so far. The track runs with the
// fight's clock, except while it is frozen: during a count (the lead-in, a retry's count, the count
// back after a silence) and during a scripted silence. The host plays the phase's music while
// trackRunning(), stops it while frozen, and on Resumed starts it again at the bar the event names.
// Prompts, windows and lockouts are all judged in track time, so nothing lands while the track is
// frozen. Times go in order; an earlier time is read as now().
//
// The bar map (one BarMap per phase):
// * A phase has a tempo (bpm of its beat, 40 to kMaxBossBpm 150), a meter and a loop of bars. The
//   beat is a quarter note, except in a compound meter (6/8, 9/8, 12/8), where it is a dotted quarter:
//   at 80 bpm in 6/8 a beat is 0.75 s and an eighth 0.25 s. Bars are measured in eighths.
// * A bar may differ from the phase's meter (a bar that drops a beat, a 5/4 or 7/8 bar): bars[] lists
//   those by number. A bar is counted in the phase's beat; its last beat may be short (a 7/8 bar in a
//   quarter-note phase has beats 1 to 4, the fourth one eighth long).
// * An empty beat is a meter fact, not a move: the music has nothing there, the beat line draws it
//   hollow, and a press on it answers nothing. It is set for every bar of a phase (`empty`) or for one
//   bar (its entry's `empty`, which replaces the phase's for that bar).
// * A position is written "bar.beat", bars from 1 in the loop, beats from 1 in the bar, with "&" for
//   the eighth after the beat and, in a compound meter, "a" for the second eighth after it ("2.3&").
//   A lost beat is a position the bar does not have: a beat past a shortened bar's last ("4.4" in a
//   3/4 bar), or an eighth past its end ("4.4&" in a 7/8 bar). A hold or release may end on the
//   loop's closing bar line ("<loop bars + 1>.1").
// * The loop's length is written twice, in bars and in seconds; the loader checks that the bars sum
//   to the seconds.
//
// Pinned moves. Each move has a tell (where its marker appears) and an impact (where it is answered),
// both positions, an answer, a stem tag (which part of the music it shows; the stem sets the
// marker's size) and a miss cost (a share of the lead's max hp, at most kMaxBossMissCost 35%). The
// answers, on the four buttons:
//   Attack   tap Attack on the impact: a strike, the lead hits the boss.
//   Hold     press `button` (Attack by default) on the impact and keep it down to `end`: a heavier hit.
//   Release  a hold that spends the lead's Load (Load.h) as one hit. Offered only while the Load is
//            above 0; at most one per phase. Let go early, it breaks and the Load is kept.
//   Guard    press Guard and keep it down through the impact: the blow is taken on the guard. A
//            Guard pressed within the perfect width of the impact is perfect and stores the blow
//            (round(miss cost x kMaxLoad)) as Load.
//   Parry    tap Guard on the impact: a perfect parry hits back.
//   Dodge    tap Dodge on the impact.
//   Jump     tap Jump on the impact.
//   Paired   two buttons together (`pair`), within pairWidth of each other, on the impact.
// Guard, Parry, Dodge, Jump and Paired are counters: the boss's move lands at the impact. Only a
// missed counter costs hp; a strike, hold or release has no miss cost.
//
// Rules:
// * Judging (track time; impact is the move's impact, all windows from BeatBossTuning).
//   - Attack, and the press of a Hold or Release: within strike.perfect of the impact Perfect, within
//     strike.good Good; past impact + good it is a Miss (a Release nobody pressed just passes).
//   - Counters: a press within perfect / 2 either side of the impact is Perfect, from impact -
//     counterEarly up to that Good. Unanswered, the move lands at impact + perfect / 2: a Miss, and
//     the lead loses miss cost x max hp, rounded down. A Guard counts its press but resolves at the impact,
//     and only if Guard is still down then (let go, it can be pressed again). A Paired counter
//     records each of its buttons' latest press in its window and resolves when both are in and
//     within pairWidth of each other, graded by the later.
//   - Holds: let go earlier than holdSlack before `end`, a hold is a Miss; held to its end it
//     completes with its press's grade.
//   - A press answers the earliest open prompt it can. Pressing a counter's button while its marker
//     shows but before its window opens is a whiff: that input locks out of counters for
//     min(counterLockout, lockoutBeats beats). Strikes and holds are never locked out. Any other
//     press that answers nothing is a stray.
// * Streak. Every Perfect or Good adds one; any Miss resets it to 0. multiplier = min(maxMultiplier
//   (at most 4), 1 + streak / streakStep). Each reward fires when the streak reaches its count: Open
//   opens a rest, Heal gives the lead `amount` hp, Load stores `amount` Load, None is named only.
// * Damage (ints). Attack: strike x m (Good: round(strike x goodFactor) x m). Parry, Paired perfect:
//   counter x m; a Good counter, a Dodge and a Jump deal nothing. Hold: round(hold x beats held) x m.
//   Release: Load x release (Good: x goodFactor), no multiplier.
// * Phases. The boss's hp falls through each phase down to the phase's floor (endAt x max hp) and no
//   further. At the floor the phase ends at the next bar line: open prompts are dropped and the next
//   phase starts on its bar 1. The last phase's floor is 0, and the boss at 0 is Victory at once. A
//   phase with `loops` above 0 also ends after that many loops.
// * Checkpoints. Each phase's first bar line saves one: the phase, the boss's hp, the Load and the
//   silences played. retry() goes back to it with the lead at full hp, the streak at 0 and no rests
//   open, counts one bar in the phase's meter and starts the phase's bar 1.
// * Finisher. A phase may have finisher bars. When the boss is within finisher.at of max hp above the
//   floor, the next bar line plays them instead of the loop's next bar (once a loop). Every strike
//   answered deals ceil(hp above the floor / finisher strikes left), so the last takes the boss to the
//   floor. A Miss breaks it: the rest of its strikes are dropped, it costs nothing, and the loop
//   resumes at the next bar line where it left off.
// * Scripted silences. At a bar line a silence (at a share of the boss's hp, or at a loop's bar) stops
//   the track for `seconds`; then a one-bar count brings it back. Nothing shows, nothing lands, and no
//   input does anything but count as a stray.
// * Open rests. A phase lists its rests (beats the music leaves open on purpose, never on an impact
//   or inside a hold). A reward of kind Open and a counter marked `opens` (answered) each open the next
//   closed one; a Miss closes the last opened. staggerOpens open at once stagger the boss: they all
//   close, its open counters drop, and the next bar plays without its counters.
// * The end. The lead at 0 hp is Defeat. After Victory or Defeat, update, press and release do nothing.
// * Assists (QteAssist): autoComplete makes update() play the autopilot; holdToggle latches holds,
//   releases and guards on their press.
// * The autopilot (autopilot()) presses every prompt on its impact (a Guard a quarter of the perfect
//   width before it), lets go of holds at their end, and never strays.
//
// Data: Data/Battle/beat_boss/<id>.json, one track a file (ids and numbers only; no story text):
//   { "version": 1, "id": "test_track", "music": "<cue id>",
//     "boss": {"hp": 400}, "lead": {"hp": 100},
//     "damage": {"strike": 10, "counter": 14, "hold": 6, "release": 1},
//     "leadIn": {"bars": 1, "count": "click"},
//     "stems": [ {"id": "bass", "size": 1.0} ],
//     "streak": {"step": 8, "max": 4, "rewards": [ {"at": 6, "id": "r1", "effect": "open"} ]},
//     "staggerOpens": 4,
//     "phases": [ { "id": "p1", "bpm": 120, "meter": "4/4", "loop": {"bars": 4, "seconds": 8.0},
//                   "bars": [ {"bar": 3, "meter": "5/4"}, {"bar": 4, "meter": "3/4", "empty": [2]} ],
//                   "empty": [], "endAt": 0.6, "loops": 0, "count": "click", "rests": ["1.2"],
//                   "moves": [ {"id": "m1", "name": "Sweep", "tell": "1.1", "impact": "1.3",
//                               "answer": "parry", "stem": "bass", "miss": 0.2} ],
//                   "finisher": {"at": 0.1, "bars": 1, "moves": [ ... attack moves ... ]},
//                   "silences": [ {"hp": 0.8, "seconds": 2.0, "count": "voice"},
//                                 {"bar": 1, "loop": 2, "seconds": 1.5} ] } ] }
// Meters are n/4 (n 1..8) or n/8 (n 1..16). A move takes `end` (hold, release), `button` (hold,
// release), `pair` (paired: two distinct buttons) and `opens` (a counter). Counts are "click",
// "voice" or "silent". The loader refuses, each with its path: a tell or impact on an empty or lost
// beat; a tell under kMinBossTell (0.4 s) before its impact, or a counter's under one beat; a tempo
// over 150; two impacts closer than kMinBossGap (0.2 s), across the loop's end too, or one inside a
// hold; a loop whose bars don't sum to its seconds; a miss cost over 35%, or on a move that is not a
// counter. Unknown keys are warnings.
//
// Deterministic for the same inputs and times. Loading allocates; nothing else does (fixed capacity,
// results by value).

#include "ftcore/Api.h"
#include "ftcore/ActionButton.h"
#include "ftcore/Load.h"
#include "ftcore/Qte.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr double kMaxBossBpm = 150.0;
inline constexpr double kMinBossBpm = 40.0;
inline constexpr double kMinBossTell = 0.4;     // seconds from a tell to its impact, at least
inline constexpr double kMinBossGap = 0.2;      // seconds between two impacts, at least
inline constexpr double kMaxBossMissCost = 0.35; // of the lead's max hp
inline constexpr std::size_t kMaxBossPhases = 4;
inline constexpr std::size_t kMaxBossLoopBars = 32;
inline constexpr std::size_t kMaxBossBarEighths = 16;
inline constexpr std::size_t kMaxBossBarBeats = 8;
inline constexpr std::size_t kMaxBossMoves = 48;         // per phase
inline constexpr std::size_t kMaxBossFinisherMoves = 16;
inline constexpr std::size_t kMaxBossFinisherBars = 4;
inline constexpr std::size_t kMaxBossRests = 16;         // per phase
inline constexpr std::size_t kMaxBossSilences = 4;       // per phase
inline constexpr std::size_t kMaxBossStems = 8;
inline constexpr std::size_t kMaxBossRewards = 4;
inline constexpr std::size_t kMaxBossPrompts = 32;       // open at once
inline constexpr std::size_t kMaxBossEvents = 32;        // per call; more are dropped
inline constexpr int kMaxBossMultiplier = 4;

// How a move is answered (append only). Data: "attack", "guard", "parry", "dodge", "jump", "hold",
// "paired", "release".
enum class BeatBossAnswer : std::uint8_t { Attack, Guard, Parry, Dodge, Jump, Hold, Paired, Release };
FTCORE_API const char* beatBossAnswerKey(BeatBossAnswer a);
FTCORE_API bool parseBeatBossAnswer(std::string_view key, BeatBossAnswer& out);
// The boss's move lands at the impact (Guard, Parry, Dodge, Jump, Paired).
inline bool isBossCounter(BeatBossAnswer a) {
    return a == BeatBossAnswer::Guard || a == BeatBossAnswer::Parry || a == BeatBossAnswer::Dodge ||
           a == BeatBossAnswer::Jump || a == BeatBossAnswer::Paired;
}
// Held to an end (Hold, Release).
inline bool isBossHold(BeatBossAnswer a) { return a == BeatBossAnswer::Hold || a == BeatBossAnswer::Release; }

// The kind of a count (the lead-in, a retry, the way back from a silence): the host plays clicks, a
// voice, or nothing (the beat line's numerals alone). Data: "click", "voice", "silent".
enum class BeatCountKind : std::uint8_t { Click, Voice, Silent };

// What a streak reward does. Data: "none", "open", "heal", "load".
enum class BossRewardEffect : std::uint8_t { None, Open, Heal, Load };

// A place in a bar map: the bar (from 1) and the eighth within it (from 0).
struct BossPos {
    std::uint8_t bar = 1;
    std::uint8_t eighth = 0;
};

struct PinnedMove {
    BossPos tell, impact, end;     // end: Hold and Release only (else = impact)
    double tellAt = 0.0;           // seconds from the loop's (or the finisher's) start
    double impactAt = 0.0;
    double endAt = 0.0;
    BeatBossAnswer answer = BeatBossAnswer::Attack;
    ActionButton button = ActionButton::Attack; // the button pressed (Paired: the first)
    ActionButton second = ActionButton::Attack; // Paired: the other
    std::uint8_t stem = 0;                      // index into the track's stems
    float missCost = 0.0f;                      // counters: share of the lead's max hp
    bool opens = false;                         // a counter that opens a rest when answered
};

// One bar's shape.
struct BossBar {
    std::uint8_t eighths = 8;
    std::uint8_t meterCount = 4;     // the meter as written ("5/4": 5 and 4), for the HUD
    std::uint8_t meterUnit = 4;
    std::uint16_t emptyBeats = 0;    // bit b: beat b + 1 is empty
};

struct BossRest {
    BossPos at;
    double time = 0.0; // seconds from the loop's start
};

struct BossSilence {
    float hp = 0.0f;           // at or under this share of the boss's max hp (0: by bar)
    std::uint8_t bar = 0;      // or at this bar of the loop (1-based) ...
    std::uint16_t loop = 1;    // ... of this loop (from 1)
    double seconds = 1.0;
    BeatCountKind count = BeatCountKind::Click;
};

// One phase: its tempo, meter, loop and moves.
struct BarMap {
    double bpm = 120.0;
    std::uint8_t beatEighths = 2;    // a quarter (2) or a dotted quarter (3)
    BossBar meter;                   // the phase's own bar
    std::uint8_t loopBars = 4;
    std::array<BossBar, kMaxBossLoopBars> bars{};
    std::array<std::uint16_t, kMaxBossLoopBars + 1> barStart{}; // eighths from the loop's start
    double loopSeconds = 8.0;
    int loops = 0;                   // the phase ends after this many loops (0: by hp alone)
    float endAt = 0.0f;              // the floor, as a share of the boss's max hp
    BeatCountKind count = BeatCountKind::Click; // a retry's count
    std::array<PinnedMove, kMaxBossMoves> moves{};
    std::uint8_t moveCount = 0;
    std::array<BossRest, kMaxBossRests> rests{};
    std::uint8_t restCount = 0;
    float finisherAt = 0.0f;
    std::uint8_t finisherBars = 0;   // 0: no finisher (its bars are the phase's meter)
    std::array<PinnedMove, kMaxBossFinisherMoves> finisher{};
    std::uint8_t finisherCount = 0;
    std::array<BossSilence, kMaxBossSilences> silences{};
    std::uint8_t silenceCount = 0;

    double eighthSeconds() const { return 60.0 / bpm / static_cast<double>(beatEighths); }
    double beatSeconds() const { return 60.0 / bpm; }
    // Seconds from the loop's start to the downbeat of bar index b (0-based; loopBars: the loop's end).
    double barOffset(std::size_t b) const { return static_cast<double>(barStart[b]) * eighthSeconds(); }
    double barSeconds(std::size_t b) const { return static_cast<double>(bars[b].eighths) * eighthSeconds(); }
    double finisherBarSeconds() const { return static_cast<double>(meter.eighths) * eighthSeconds(); }
};

struct BossReward {
    int at = 8;
    BossRewardEffect effect = BossRewardEffect::None;
    int amount = 0;
};

// A whole track, as data. Fixed capacity: copying it never allocates.
struct BeatBossDef {
    int bossHp = 400;
    int leadHp = 100;
    int strikeDamage = 10;
    int counterDamage = 14;
    int holdDamage = 6;    // per beat held
    int releaseDamage = 1; // per Load released
    int leadInBars = 1;
    BeatCountKind leadInCount = BeatCountKind::Click;
    std::array<float, kMaxBossStems> stemSize{};
    std::uint8_t stemCount = 0;
    int streakStep = 8;
    int maxMultiplier = 4;
    std::array<BossReward, kMaxBossRewards> rewards{};
    std::uint8_t rewardCount = 0;
    int staggerOpens = 4;  // 0: no stagger
    std::array<BarMap, kMaxBossPhases> phases{};
    std::uint8_t phaseCount = 0;
};

struct BeatBossTuning {
    double strikePerfect = 0.050;
    double strikeGood = 0.120;
    double perfect = 0.12;       // a counter's perfect width, centred on its impact
    double counterEarly = 0.30;  // a counter's window opens this long before the impact
    double pairWidth = 0.12;     // a paired prompt's two presses, at most this far apart
    double counterLockout = 0.40;
    double lockoutBeats = 0.75;
    double holdSlack = kQteHoldSlack;
    double goodFactor = 0.6;
};

// One open prompt (track times).
struct BeatBossPrompt {
    std::uint32_t id = 0;      // unique within a fight, increasing
    std::uint8_t move = 0;     // index into its phase's moves (or finisher)
    bool finisher = false;
    BeatBossAnswer answer = BeatBossAnswer::Attack;
    ActionButton button = ActionButton::Attack;
    ActionButton second = ActionButton::Attack;
    std::uint8_t stem = 0;
    float missCost = 0.0f;
    bool opens = false;
    double tell = 0.0;
    double impact = 0.0;
    double end = 0.0;
    QteGrade grade = QteGrade::Pending; // a hold or guard pressed: its press's grade
    double pressA = -1.0;               // Guard, holds: the press; Paired: `button`'s latest press
    double pressB = -1.0;               // Paired: `second`'s latest press
};

enum class BeatBossEventType : std::uint8_t { // append only
    Judged,         // `prompt` resolved with `grade` (`answer` is its answer)
    BossHit,        // the boss took `amount`
    LeadHit,        // the lead took `amount` from `prompt`
    Stray,          // a press that answered nothing
    Whiff,          // a counter's button pressed too early: locked out for `amount` ms
    LockedOut,      // a press the lockout ignored
    Multiplier,     // the multiplier is now `amount`
    Reward,         // the streak reached reward `amount`
    LoadStored,     // a perfect Guard stored `amount` Load
    Released,       // a Release spent `amount` Load
    FinisherStart,
    FinisherBroken,
    PhaseStart,     // phase `amount` starts on its bar 1 (a checkpoint)
    BarStart,       // a bar starts: `amount` its number in the loop (0: a finisher bar)
    Count,          // a count beat: `amount` its numeral
    SilenceStart,   // the track stops for `amount` ms, then counts back
    Resumed,        // the track runs again (at the bar line it stopped on)
    RestOpened,     // `amount` rests are open now
    RestClosed,     // `amount` rests are open now
    Stagger,
    Victory,
    Defeat,
};

struct BeatBossEvent {
    BeatBossEventType type = BeatBossEventType::Judged;
    std::uint32_t prompt = 0;
    QteGrade grade = QteGrade::Pending;
    BeatBossAnswer answer = BeatBossAnswer::Attack;
    int amount = 0;
    double time = 0.0; // the fight's clock
};

struct BeatBossEvents {
    std::array<BeatBossEvent, kMaxBossEvents> items{};
    std::size_t count = 0;

    const BeatBossEvent* begin() const { return items.data(); }
    const BeatBossEvent* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const BeatBossEvent& operator[](std::size_t i) const { return items[i]; }
};

struct BeatBossStats {
    int perfects = 0;
    int goods = 0;
    int misses = 0;
    int strays = 0;
    int whiffs = 0;
    int ignored = 0;
    int maxStreak = 0;
    int damageDealt = 0;
    int damageTaken = 0;
    int loadStored = 0;
    int releases = 0;
    int finishers = 0;
    int staggers = 0;
    int silences = 0;
    int bars = 0;      // bars started (finisher bars too)
    int retries = 0;
};

enum class BeatBossState : std::uint8_t { NotStarted, Playing, Victory, Defeat };

// ---- the beat line: what the HUD draws under the boss (the sound-off fallback) ----

inline constexpr std::size_t kMaxBeatLineTicks = 2 * kMaxBossBarBeats + 1;
inline constexpr std::size_t kMaxBeatLineRests = 2 * kMaxBossRests;
inline constexpr std::size_t kMaxBeatLineMarkers = 32;

enum class BeatTickKind : std::uint8_t { Beat, BarLine, Empty, End };

struct BeatLineBar {
    double seconds = 0.0;
    std::uint8_t beats = 0;
    std::uint8_t eighths = 0;
    std::uint8_t number = 0;   // its bar in the loop (0: a count or finisher bar)
    bool count = false;        // a count bar (the lead-in, a retry, after a silence)
    bool finisher = false;
    bool shortBar = false;     // fewer eighths than the phase's meter
    bool longBar = false;      // more
};

struct BeatLineTick {
    double at = 0.0;           // seconds from the line's start
    float x = 0.0f;            // 0..1 along the line
    BeatTickKind kind = BeatTickKind::Beat;
    std::uint8_t bar = 0;      // 0 the current bar, 1 the next
    std::uint8_t beat = 0;     // from 1
    std::uint8_t numeral = 0;  // a count's numeral over the beat (0: none)
};

struct BeatLineRest {
    double at = 0.0;
    float x = 0.0f;
    bool open = false;         // an opened rest: drawn as a gap
};

struct BeatLineMarker {
    double at = 0.0;           // the impact
    float x = 0.0f;
    float endX = 0.0f;         // holds: their end (else x)
    BeatBossAnswer answer = BeatBossAnswer::Attack;
    std::uint8_t stem = 0;
    std::uint32_t prompt = 0;  // the open prompt (0: a move of the next bar, not yet told)
};

// The current bar and the next, laid out over 0..1 by time, so a short bar is drawn short. During a
// count, the count bar is the current one; during a silence the line is frozen at the count's start.
struct BeatLine {
    std::array<BeatLineBar, 2> bars{};
    double seconds = 0.0;
    double playhead = 0.0;     // seconds from the line's start
    float playheadX = 0.0f;
    bool frozen = false;       // a scripted silence
    bool counting = false;
    std::array<BeatLineTick, kMaxBeatLineTicks> ticks{};
    std::size_t tickCount = 0;
    std::array<BeatLineRest, kMaxBeatLineRests> rests{};
    std::size_t restCount = 0;
    std::array<BeatLineMarker, kMaxBeatLineMarkers> markers{};
    std::size_t markerCount = 0;
};

class BeatBoss {
public:
    // Starts `def` with the clock at `now`: the lead-in counts, then phase 0 starts on its bar 1.
    FTCORE_API void begin(const BeatBossDef& def, const BeatBossTuning& tuning, double now,
                          const QteAssist& assist = {});
    // Back to the last checkpoint with the clock at `now` (see "Checkpoints"); stats keep counting.
    FTCORE_API void retry(double now);

    FTCORE_API BeatBossEvents update(double now);
    FTCORE_API BeatBossEvents press(ActionButton input, double t);
    FTCORE_API BeatBossEvents release(ActionButton input, double t);
    // The next press or release the autopilot makes at or before `now` (None when waiting). Apply it
    // at action.time, then ask again until None.
    FTCORE_API QteAction autopilot(double now) const;
    // Fills `out` for the HUD. No allocation.
    FTCORE_API void beatLine(BeatLine& out) const;

    BeatBossState state() const { return state_; }
    bool over() const { return state_ == BeatBossState::Victory || state_ == BeatBossState::Defeat; }
    double now() const { return now_; }
    double trackTime() const { return track_; }
    bool trackRunning() const { return state_ == BeatBossState::Playing && mode_ == Mode::Running; }
    bool silent() const { return mode_ == Mode::Silent; }
    bool counting() const { return mode_ == Mode::Count; }
    const BeatBossDef& def() const { return def_; }
    const BeatBossTuning& tuning() const { return tuning_; }
    const BeatBossStats& stats() const { return stats_; }
    std::uint32_t droppedEvents() const { return droppedEvents_; }

    int phase() const { return phase_; }
    int loop() const { return loop_; }
    // The bar playing (1-based in the loop; 0 in a finisher bar or before the first bar).
    int bar() const { return finisherBar_ >= 0 || !started_ ? 0 : barIndex_ + 1; }
    bool inFinisher() const { return finisherBar_ >= 0; }
    int checkpointPhase() const { return checkpoint_.phase; }

    std::size_t promptCount() const { return promptCount_; }
    const BeatBossPrompt& prompt(std::size_t i) const { return prompts_[i]; }
    // Prompt p's marker: 1 at its tell down to 0 at its impact, then 0; -1 before the tell and while
    // the track is frozen.
    FTCORE_API double approach(const BeatBossPrompt& p) const;

    int hp() const { return hp_; }
    int maxHp() const { return def_.leadHp; }
    int bossHp() const { return bossHp_; }
    int bossMaxHp() const { return def_.bossHp; }
    int floorHp() const { return floor_; }
    int streak() const { return streak_; }
    FTCORE_API int multiplier() const;
    int load() const { return load_.value; }
    FTCORE_API int openRests() const;
    bool counterLockedOut() const { return track_ < lockoutUntil_; }

private:
    enum class Mode : std::uint8_t { Count, Silent, Running };
    // A bar to come: a loop bar of a phase, or a finisher bar.
    struct BarRef {
        std::int8_t phase = 0;
        std::int8_t bar = 0;       // loop bar index (0-based), or finisher bar index
        bool finisher = false;
        bool newPhase = false;
        std::int32_t loop = 1;
    };
    struct Checkpoint {
        int phase = 0;
        int bossHp = 0;
        int load = 0;
        std::uint32_t silencesPlayed = 0;
    };

    void reset(double now, int phase);
    void runTo(double t, BeatBossEvents& out);
    void advance(double t, BeatBossEvents& out);
    void doPress(ActionButton input, double t, BeatBossEvents& out);
    void doRelease(ActionButton input, double t, BeatBossEvents& out);
    QteAction pilot(double horizon) const;
    double horizon() const; // the next wall time the autopilot must not look past

    BarRef nextBar() const;
    BossBar barShape(const BarRef& r) const;
    double barSecondsOf(const BarRef& r) const;
    void barLine(BeatBossEvents& out);
    void startCount(int bars, double beatSeconds, int beatsPerBar);
    void generate(BeatBossEvents& out);
    void addPrompt(const PinnedMove& m, std::uint8_t index, bool finisher, double base);
    int silenceDue(const BarRef& r) const;

    double dueTime(const BeatBossPrompt& p) const;
    bool isDue(const BeatBossPrompt& p, double t) const;
    void expire(std::size_t i, BeatBossEvents& out);
    void resolve(std::size_t i, QteGrade grade, BeatBossEvents& out);
    void judgePress(ActionButton input, BeatBossEvents& out);
    void hitBoss(int amount, BeatBossEvents& out);
    void hitLead(const BeatBossPrompt& p, BeatBossEvents& out);
    void addStreak(BeatBossEvents& out);
    void breakStreak(BeatBossEvents& out);
    void openRest(BeatBossEvents& out);
    void dropCounters();
    void dropFinisher();
    void removePrompt(std::size_t i);
    int finisherLeft() const;
    double toWall(double trackTime) const;
    void push(BeatBossEvents& out, BeatBossEventType type, int amount = 0, const BeatBossPrompt* p = nullptr,
              QteGrade grade = QteGrade::Pending);

    BeatBossDef def_{};
    BeatBossTuning tuning_{};
    QteAssist assist_{};
    BeatBossStats stats_{};
    BeatBossState state_ = BeatBossState::NotStarted;
    Mode mode_ = Mode::Count;
    std::uint32_t droppedEvents_ = 0;

    double now_ = 0.0;        // the fight's clock
    double track_ = 0.0;      // the track's clock
    // A pause (Count or Silent): the silence ends at silentUntil_, then the count runs to pauseEnd_.
    double silentUntil_ = 0.0;
    double pauseEnd_ = 0.0;
    double countBeat_ = 0.5;  // seconds per count beat
    int countBeatsPerBar_ = 4;
    int countBeats_ = 0;      // in this pause's count
    int countDone_ = 0;       // count beats emitted

    bool started_ = false;    // the first bar of the run (after begin or retry) has started
    int phase_ = 0;
    int loop_ = 1;
    int barIndex_ = 0;        // the loop bar playing (or the one a finisher resumes after)
    int finisherBar_ = -1;    // the finisher bar playing (-1: none)
    bool finisherBroken_ = false;
    int finisherLoop_ = 0;    // the loop the last finisher was offered in
    int finisherStrikes_ = 0; // strikes of the finisher not yet resolved or generated
    double barStart_ = 0.0;   // track time of the current bar's downbeat
    double barSeconds_ = 0.0;
    bool phaseEnd_ = false;   // at the floor: the phase ends at the next bar line
    bool skipCounters_ = false; // a stagger: the next bar plays without its counters

    int hp_ = 0;
    int bossHp_ = 0;
    int floor_ = 0;
    int streak_ = 0;
    Load load_{};
    std::uint16_t openMask_ = 0;
    std::array<std::uint8_t, kMaxBossRests> openOrder_{}; // rests in the order they opened
    std::uint32_t silencesPlayed_ = 0; // bit 4 x phase + i
    Checkpoint checkpoint_{};
    double lockoutUntil_ = -1e30;
    std::uint32_t nextId_ = 1;

    std::array<BeatBossPrompt, kMaxBossPrompts> prompts_{}; // open, in impact order
    std::size_t promptCount_ = 0;
};

// ---- data ----

struct BeatBossTrack {
    std::string id;
    std::string music;
    BeatBossDef def;
    std::vector<std::string> stemIds;
    std::vector<std::string> rewardIds;
    std::vector<std::string> phaseIds;
    // Per phase: each move's id and name (finisher moves after the phase's own).
    std::vector<std::vector<std::string>> moveIds;
    std::vector<std::vector<std::string>> moveNames;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importBeatBossTrack(std::string_view json, BeatBossTrack& out, std::string* error = nullptr,
                                    std::vector<std::string>* warnings = nullptr);
// "4.2&" -> bar 4, eighth (beat - 1) x beatEighths + 1. False for anything malformed (the bar map
// checks whether the place exists).
FTCORE_API bool parseBossPos(std::string_view text, std::uint8_t beatEighths, BossPos& out);

} // namespace ftcore
