#pragma once

// The beat fight (CombatKind::Beat; docs/COMBAT_SYSTEMS.md): a real-time fight on the music's beat
// grid. The enemies take turns, one bar each (round robin over the living ones). On its turn an
// enemy plays one bar of one of its patterns, and each slot of the bar (an eighth note) may be a
// prompt the lead answers with one of the four arrows:
//   Strike   press the arrow on its eighth: the lead lands a hit.
//   Counter  the enemy strikes on its eighth: press the arrow to parry and hit back. Unanswered, or
//            answered too late, it hits the lead. Mashing is punished: pressing a counter's arrow too
//            early locks counters out for a moment.
//   Hold     press the arrow on its eighth and keep it down for `length` eighths: a heavier hit.
// Answers in a row build a chain, and the chain a damage multiplier; a Miss resets both. When an
// enemy is low its next turn is a finisher: a quick run of strikes that takes the rest of its hp,
// unless one is missed. Only missed counters hurt the lead.
//
//   ftcore::BeatFight fight;
//   fight.begin(setup.def, setup.tuning, clockNow, seed, assist);
//   every frame:  for (const ftcore::BeatEvent& e : fight.update(clockNow)) ...;   // sparks, sounds, HUD
//   on a key:     fight.press(arrow, eventTime) / fight.release(arrow, eventTime)
//   draw:         for i < promptCount(): prompt(i), approach(prompt(i), clockNow) (a marker closing in
//                 over its enemy), plus hp, multiplier(), chain(), turnEnemy(), holding()
//   fight.phase() == Victory: the step is done. Defeat: retry (restart(clockNow)) or quit.
//
// The clock. Every time here is in seconds on the fight's clock: the music's song time when music
// plays (the host's Conductor: songTime(wall) for update(), songTimeForEvent(eventWall) for presses
// and releases), else a pause-aware wall clock. Beat b falls at offset + b * 60 / bpm (the def's bpm
// and offset, which match the music). Times go in order; an earlier time is read as now().
//
// Rules:
// * Schedule. Bars are 4 beats from beat 0 (a 4/4 grid; slot i of the bar starting on beat B is at
//   beat B + i / 2). The first bar is the first bar line at least leadInBars bars after begin()'s
//   time; bars follow back to back. Each bar is generated approachBeats before it starts (when the
//   clock first reaches that time): its enemy is the next living one in turn order, its pattern is
//   picked from that enemy's patterns by the seeded RNG (never the same pattern twice in a row for an
//   enemy with more than one), and its prompts join the open list. A bar whose enemy is down before
//   it starts plays as a rest.
// * Judging. A prompt's `time` is the moment of its eighth, where its marker closes. The windows are
//   the fight's effective ones (effectiveStrikeWindows, effectiveCounterWindows: the tuning's, each
//   edge capped to a share of an eighth so neighbouring slots stay apart at fast tempos), worked out
//   once in begin().
//   - Strike, and the press of a Hold: symmetric windows around `time`. Within `perfect` it is
//     Perfect, within `good` Good. Once the clock is past time + good the prompt is a Miss; a press
//     before time - good answers nothing.
//   - Counter: the defence maths of DefenceTimeline.h, as a Narrow QTE prompt is judged (Qte.h): one
//     hit whose impact is time + parry / 2, so the parry window is centred on the beat. A press
//     inside the parry window is Perfect, inside the rest of the dodge window Good. At the impact the
//     hit lands: a Miss, and the lead takes the enemy's damage (a press of its arrow less than
//     counter.late after the impact that answers nothing else reports Late). A press of the arrow of
//     a counter that is showing (approach() >= 0) but whose dodge window hasn't opened is a whiff:
//     counters are locked out for min(counter.parryLockout, counterLockoutBeats * seconds per beat),
//     across prompts, and a press during the lockout that would answer a counter is ignored
//     (LockedOut). Strikes and holds are never locked out.
//   - Hold, after its press: letting go earlier than holdSlack before its end breaks it (a Miss).
//     Held to its end, or let go within holdSlack of it, it completes with its press's grade.
//   - Strict order: a press answers the earliest open prompt not yet pressed, with two exceptions.
//     (1) A press whose arrow differs from that prompt's but matches the next one's, inside the next
//     one's window, resolves the earliest as a Miss and answers the next (the player skipped a note).
//     (2) A press or release within holdSlack before the current hold's end completes the hold
//     first. A press that answers nothing is a whiff when its arrow is that of a showing counter
//     whose dodge window hasn't opened yet (see Counter), else a stray (a wrong arrow, too early, a
//     counter not showing yet): counted, nothing else.
// * Chain and multiplier. Every Perfect or Good adds one to the chain; a Miss resets it to 0 (strays,
//   whiffs and ignored presses don't). multiplier = min(maxMultiplier, 1 + chain / chainPerStep)
//   (integer division); a hit's damage uses the multiplier from before it counts.
// * Damage (ints; round() is to nearest). Strike: Perfect strikeDamage * m, Good
//   round(strikeDamage * goodFactor) * m. Counter: Perfect counterDamage * m, Good
//   round(counterDamage * goodFactor) * m, and the lead takes nothing either way; Miss: the enemy's
//   damage to the lead. Hold, on completion: holdDamage * length * m, or
//   round(holdDamage * length * goodFactor) * m after a Good press; broken: nothing.
// * Finisher. A bar whose enemy's hp is at most finisherAt of its max when the bar is generated is a
//   finisher instead of a pattern (FinisherStart): finisherLength strikes on consecutive eighths from
//   the bar's start, random arrows from the seeded RNG, no arrow twice in a row. Each finisher strike
//   answered (Perfect or Good alike) deals ceil(the enemy's hp / finisher strikes left), no
//   multiplier, so the last one takes the rest. A Miss breaks the finisher (FinisherBroken): its
//   other strikes are dropped silently and the enemy keeps its hp until a later turn.
// * Down and the end. An enemy at 0 hp is down (EnemyDown); its open prompts are dropped silently.
//   Every enemy down: Victory. The lead at 0 hp: Defeat. After either, update, press and release do
//   nothing. restart() replays the same fight (the same seed: the same patterns in the same order).
// * Assists (QteAssist, from Settings): autoComplete makes update() play the autopilot itself (press
//   and release are ignored); holdToggle makes a hold latch on its press (release does nothing to it).
// * The autopilot (autopilot()) presses every strike, hold and counter on its beat (Perfect), lets
//   go of a hold at its end (a release before a press at the same time), and never strays. It must
//   win every fight in beat.json within 64 bars, and with no input every fight must be lost within
//   64 bars.
//
// Data: Data/Battle/beat.json (ids, numbers and pattern strings; no story text). Like qte.json it is
// keyed by scene id + gameplay beat index + plan step index, with a `default` fight:
//   { "version": 1,
//     "enemies": [ { "id": "wisp", "hp": 60, "damage": 10,
//                    "patterns": [ "sL . sR . cU . . .", "cD . . . sL . hU2 -" ] } ],
//     "default": { "bpm": 120, "offset": 0, "hp": 100, "enemies": ["wisp"], "music": "" },
//     "fights": [ { "scene": "SC-X", "beat": 4, "step": 0, "bpm": 96, "offset": 0.42,
//                   "enemies": ["wisp", "wisp"], "music": "<cue id>" } ] }
//   A pattern is 8 space-separated slots, one per eighth of the bar: `.` a rest, s<A> a strike, c<A> a
//   counter, h<A><n> a hold of n eighths (2..8) followed by n - 1 `-` slots (it may end on the next
//   bar line, not past it); <A> is L, D, U or R. Fight keys (in `default` and each fight): bpm
//   (40..240), offset (seconds), hp, strikeDamage, counterDamage, holdDamage, enemies
//   (1..kMaxBeatEnemies ids), music (a cue id the host plays; "" none), and the tuning overrides
//   `strike` {perfect, good}, `counter` {dodge, parry, parryLockout, late} and holdSlack. A fight's
//   missing key keeps the default fight's value (perfect <= good and parry <= dodge must hold).
//   1..kMaxBeatPatterns patterns per enemy, each with at least one counter (so every bar threatens).
//   An unknown enemy id, a duplicate id or a duplicate (scene, beat, step) is an error; unknown keys
//   are warnings; numbers outside their ranges (the loader documents them) are errors.
//
// Deterministic for the same seed, inputs and times. Loading allocates; nothing else does (fixed
// capacity, results by value).

#include "ftcore/Api.h"
#include "ftcore/Arrow.h"
#include "ftcore/DefenceTimeline.h"
#include "ftcore/Qte.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kMaxBeatEnemies = 4;
inline constexpr std::size_t kBeatSlots = 8;       // eighths in a 4/4 bar
inline constexpr std::size_t kMaxBeatPatterns = 6; // per enemy
inline constexpr std::size_t kMaxBeatPrompts = 32; // open at once (generated and not yet resolved)
inline constexpr std::size_t kMaxBeatEvents = 32;  // per call; more are dropped (BeatFight::droppedEvents)

// What a slot holds (append only). Held: an eighth a hold covers (the `-` after h<A><n>).
enum class BeatKind : std::uint8_t { Rest, Strike, Counter, Hold, Held };

struct BeatSlot {
    BeatKind kind = BeatKind::Rest;
    Arrow input = Arrow::Left;
    std::uint8_t length = 0; // Hold: eighths, 2..8
};

// One bar of an enemy's turn.
struct BeatPattern {
    std::array<BeatSlot, kBeatSlots> slots{};

    bool hasCounter() const {
        for (const BeatSlot& s : slots)
            if (s.kind == BeatKind::Counter) return true;
        return false;
    }
};

// The beat.json spelling of a pattern ("sL . sR . cU . . ."). On failure `out` is untouched and
// *error says why. Allocates only the error text.
FTCORE_API bool parseBeatPattern(std::string_view text, BeatPattern& out, std::string* error = nullptr);

struct BeatEnemy {
    int hp = 60;
    int damage = 10; // a missed counter costs the lead this much
    std::array<BeatPattern, kMaxBeatPatterns> patterns{};
    std::uint8_t patternCount = 1;
};

struct BeatFightDef {
    double bpm = 120.0;
    double offset = 0.0;  // seconds of beat 0 on the fight's clock
    int hp = 100;         // the lead's
    int strikeDamage = 10;
    int counterDamage = 14;
    int holdDamage = 6;   // per eighth held
    std::array<BeatEnemy, kMaxBeatEnemies> enemies{};
    std::uint8_t enemyCount = 1;

    double secondsPerBeat() const { return 60.0 / bpm; }
    double timeAtBeat(double beat) const { return offset + beat * 60.0 / bpm; }
    double beatAt(double t) const { return (t - offset) * bpm / 60.0; }
};

// The built-in fight used when no data file is there: one enemy with the two patterns in the data
// example above, at 120 bpm.
FTCORE_API BeatFightDef defaultBeatFightDef();

// Symmetric windows around a strike's (or a hold's) eighth, in seconds either side.
struct BeatWindows {
    double perfect = 0.050;
    double good = 0.120;
};

// A counter's defence windows: DefenceWindows' dodge and parry widths, the parry lockout on (its
// length caps a whiff's lockout), the dodge and jump lockouts off.
inline DefenceWindows beatCounterWindows() {
    DefenceWindows w;
    w.dodgeLockout = 0.0;
    w.jumpLockout = 0.0;
    return w;
}

struct BeatTuning {
    BeatWindows strike;                            // strikes and hold presses
    DefenceWindows counter = beatCounterWindows();
    double counterLockoutBeats = 0.75;             // a whiff's lockout is at most this many beats
    double holdSlack = kQteHoldSlack;              // letting go of a hold this much early still counts
    double approachBeats = 2.0;                    // a prompt's marker shows this long before its eighth (0..4)
    int leadInBars = 1;
    int chainPerStep = 8;                          // answers in a row per multiplier step
    int maxMultiplier = 4;
    double goodFactor = 0.6;                       // a Good's share of a Perfect's damage
    double finisherAt = 0.25;                      // share of an enemy's max hp at or below which its turn is a finisher
    int finisherLength = 6;                        // strikes, 1..kBeatSlots
    double strikeCapEighths = 0.5;                 // strike windows reach at most this share of an eighth either side
    double counterCapEighths = 1.0;                // a counter's dodge window opens at most this many eighths early
};

// The strike windows a fight at `bpm` judges with: the tuning's, each capped to strikeCapEighths of
// an eighth (at 120 bpm the defaults are unchanged).
inline BeatWindows effectiveStrikeWindows(const BeatTuning& t, double bpm) {
    const double cap = t.strikeCapEighths * 30.0 / bpm; // 30 / bpm: seconds per eighth
    BeatWindows w = t.strike;
    w.perfect = std::min(w.perfect, cap);
    w.good = std::min(w.good, cap);
    return w;
}

// The counter windows a fight at `bpm` judges with: the parry window stays centred on the beat and
// the dodge window opens at most counterCapEighths eighths before it (each half at most that wide).
inline DefenceWindows effectiveCounterWindows(const BeatTuning& t, double bpm) {
    const double cap = t.counterCapEighths * 30.0 / bpm;
    DefenceWindows w = t.counter;
    const double half = std::min(w.parry * 0.5, cap);
    const double early = std::min(w.dodge - w.parry * 0.5, cap);
    w.parry = half * 2.0;
    w.dodge = early + half;
    return w;
}

// One open prompt.
struct BeatPrompt {
    std::uint32_t id = 0;               // unique within a fight, increasing with time
    BeatKind kind = BeatKind::Strike;   // Strike, Counter or Hold
    Arrow input = Arrow::Left;
    std::uint8_t enemy = 0;
    std::uint8_t length = 0;            // Hold: eighths
    bool finisher = false;
    double beat = 0.0;                  // the absolute beat of its eighth
    double time = 0.0;                  // its eighth on the fight's clock
    double end = 0.0;                   // Hold: the end of its last eighth; else time
    QteGrade grade = QteGrade::Pending; // a hold being held: its press's grade
};

enum class BeatEventType : std::uint8_t { // append only
    Judged,         // `prompt` resolved with `grade` (`kind` is its kind)
    EnemyHit,       // `enemy` took `amount` from `prompt`
    LeadHit,        // the lead took `amount` from `enemy`'s counter `prompt`
    Stray,          // a press that answered nothing
    Whiff,          // a counter's arrow pressed too early: counters are locked out for `amount` ms
    LockedOut,      // a press the counter lockout ignored
    Late,           // a press just after counter `prompt` landed (the HUD says LATE)
    Multiplier,     // the multiplier is now `amount`
    FinisherStart,  // `enemy`'s next bar is a finisher (sent when it is generated)
    FinisherBroken, // a finisher strike of `enemy` was missed
    EnemyDown,
    Victory,
    Defeat,
};

struct BeatEvent {
    BeatEventType type = BeatEventType::Judged;
    std::uint8_t enemy = 0;
    std::uint32_t prompt = 0; // a prompt id
    QteGrade grade = QteGrade::Pending;
    BeatKind kind = BeatKind::Rest;
    int amount = 0;
    double time = 0.0;        // the press, or when the prompt resolved
};

// One call's events, in the order they happened. Fixed capacity: no allocation.
struct BeatEvents {
    std::array<BeatEvent, kMaxBeatEvents> items{};
    std::size_t count = 0;

    const BeatEvent* begin() const { return items.data(); }
    const BeatEvent* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const BeatEvent& operator[](std::size_t i) const { return items[i]; }
};

struct BeatStats {
    int perfects = 0;
    int goods = 0;
    int misses = 0;
    int strays = 0;
    int whiffs = 0;
    int ignored = 0;   // presses the counter lockout dropped
    int counters = 0;  // counters answered (Perfect or Good)
    int maxChain = 0;
    int damageDealt = 0;
    int damageTaken = 0;
    int finishers = 0; // finishers completed
    int bars = 0;      // bars started
    int restarts = 0;
};

enum class BeatPhase : std::uint8_t { NotStarted, Playing, Victory, Defeat };

class BeatFight {
public:
    // Starts `def` with the clock at `now`: full hp both sides, chain 0, nothing open; the first bar
    // comes after the lead-in.
    FTCORE_API void begin(const BeatFightDef& def, const BeatTuning& tuning, double now, std::uint32_t seed,
                          const QteAssist& assist = {});
    // Back to the state begin() left, with the clock at `now` (the same seed); stats keep counting,
    // plus one restart.
    FTCORE_API void restart(double now);

    // Moves the clock to `now`: generates bars, resolves prompts that ran out (and holds that reached
    // their end).
    FTCORE_API BeatEvents update(double now);
    // A press / let-go of `input` at time t (moves the clock there first). Returns everything it
    // resolved: what ran out before t, then what the input did.
    FTCORE_API BeatEvents press(Arrow input, double t);
    FTCORE_API BeatEvents release(Arrow input, double t);

    // The next press or release the autopilot makes at or before `now` (None when it is waiting).
    // Apply it with press()/release() at action.time, then ask again until None.
    FTCORE_API QteAction autopilot(double now) const;

    FTCORE_API BeatPhase phase() const;
    bool over() const {
        const BeatPhase p = phase();
        return p == BeatPhase::Victory || p == BeatPhase::Defeat;
    }
    FTCORE_API double now() const;
    FTCORE_API const BeatFightDef& def() const;
    FTCORE_API const BeatTuning& tuning() const;
    FTCORE_API const BeatStats& stats() const;
    FTCORE_API std::uint32_t droppedEvents() const;

    double beatAt(double t) const { return def().beatAt(t); }
    double timeAtBeat(double beat) const { return def().timeAtBeat(beat); }
    double secondsPerBeat() const { return def().secondsPerBeat(); }
    // The beat the first bar starts on (the HUD counts the lead-in down to it).
    FTCORE_API double firstBarBeat() const;

    // The open prompts, in time order (the next one first).
    FTCORE_API std::size_t promptCount() const;
    FTCORE_API const BeatPrompt& prompt(std::size_t i) const;
    // Prompt p's marker at time t: 1 when it appears (approachBeats before p.time) down to 0 at
    // p.time, then 0 until it resolves; -1 before it appears.
    FTCORE_API double approach(const BeatPrompt& p, double t) const;

    FTCORE_API int hp() const;
    FTCORE_API int maxHp() const;
    FTCORE_API std::size_t enemyCount() const;
    FTCORE_API int enemyHp(std::size_t i) const;
    FTCORE_API int enemyMaxHp(std::size_t i) const;
    FTCORE_API bool enemyAlive(std::size_t i) const;
    FTCORE_API int multiplier() const;
    FTCORE_API int chain() const;
    // The enemy whose bar is playing at now(); -1 during the lead-in and in a bar played as a rest.
    FTCORE_API int turnEnemy() const;
    // A hold is pressed and not yet resolved / its progress 0..1 from its eighth to its end.
    FTCORE_API bool holding() const;
    FTCORE_API double holdProgress() const;
    // Counters are locked out at time t (a whiff).
    FTCORE_API bool counterLockedOut(double t) const;

private:
    // The fight's state: fixed capacity only (copying a fight never allocates). Times are seconds on the
    // fight's clock.

    // How a press stands to one prompt.
    enum class Reach : std::uint8_t {
        Outside,   // outside its window (a counter's dodge window hasn't opened yet)
        Hidden,    // a counter whose marker isn't showing yet
        Answers,   // inside its window: it answers the prompt
        LockedOut, // it would answer a counter, but the counter lockout drops it
    };
    struct Judgement {
        Reach reach = Reach::Outside;
        QteGrade grade = QteGrade::Pending;
    };
    // A generated bar (the last two are kept, for turnEnemy()): its number counted from the first bar,
    // and its enemy (-1: none alive).
    struct Bar {
        std::int32_t index = -1;
        std::int32_t enemy = -1;
    };
    // A counter that landed lately: a press of its arrow within counter.late of its impact is LATE.
    struct Landed {
        std::uint32_t id = 0;
        Arrow input = Arrow::Left;
        std::uint8_t enemy = 0;
        bool open = false; // not reported late yet
        double impact = 0.0;
    };

    // The state begin() and restart() leave (the stats stay).
    void reset(double now);
    // update(): the autopilot's moves first when the assist is on, then advance().
    void runTo(double t, BeatEvents& out);
    // Moves the clock to t, in time order: bars are generated, start and resolve prompts that ran out
    // (at the same moment a bar is generated first, then prompts resolve, then the bar starts).
    void advance(double t, BeatEvents& out);
    void doPress(Arrow input, double t, BeatEvents& out);
    void doRelease(Arrow input, double t, BeatEvents& out);
    QteAction pilot(double horizon) const; // autopilot()

    double generationTime() const;                                  // when the next bar is generated
    double startTime() const;                                       // when the next bar starts
    void generate(double time, BeatEvents& out);                    // the next bar: enemy, pattern, prompts
    void addPrompt(BeatKind kind, Arrow input, std::uint8_t enemy, std::uint8_t length, bool finisher, double beat);
    double dueTime(const BeatPrompt& p) const;                      // when p resolves if nothing answers it
    bool isDue(const BeatPrompt& p, double t) const;
    void expire(std::size_t i, BeatEvents& out);                    // prompt i ran out or reached its end
    void resolve(std::size_t i, QteGrade grade, double time, BeatEvents& out); // prompt i is over
    void answer(std::size_t i, QteGrade grade, double t, BeatEvents& out);     // a press answered it
    void finishHolds(double t, BeatEvents& out);                    // holds within holdSlack of their end
    void judgePress(Arrow input, double t, BeatEvents& out);
    void classify(Arrow input, double t, BeatEvents& out);          // a press that answered nothing
    Judgement judge(const BeatPrompt& p, double t) const;
    void hitLead(const BeatPrompt& p, double time, BeatEvents& out);
    void dropEnemy(std::uint8_t enemy);                             // its open prompts, silently
    void dropFinisher(const BeatPrompt& p);                         // the rest of p's finisher, silently
    std::size_t finisherLeft(const BeatPrompt& p) const;            // strikes of p's finisher still open
    std::size_t indexOf(std::uint32_t id) const;                    // an open prompt's place (or the max value)
    double barOf(const BeatPrompt& p) const;                        // the bar p belongs to (a number)
    void removePrompt(std::size_t i);
    void push(BeatEvents& out, const BeatEvent& e);

    BeatFightDef def_{};
    BeatTuning tuning_{};
    QteAssist assist_{};
    BeatStats stats_{};
    BeatPhase phase_ = BeatPhase::NotStarted;
    std::uint32_t droppedEvents_ = 0;
    std::uint32_t seed_ = 1;
    std::uint32_t rng_ = 1; // xorshift32: a copied or restarted fight replays the same draws

    // Worked out once in begin().
    BeatWindows strikeWindows_{};
    DefenceWindows counterWindows_{};
    double counterHalf_ = 0.06;    // a counter's impact comes this long after its eighth
    double lockoutLength_ = 0.0;   // counters are locked out this long after a whiff
    double approachSeconds_ = 1.0; // approachBeats in seconds
    double firstBarBeat_ = 4.0;

    double now_ = 0.0;
    int hp_ = 0;
    int maxHp_ = 0;
    int chain_ = 0;
    std::array<int, kMaxBeatEnemies> enemyHp_{};
    std::array<std::int8_t, kMaxBeatEnemies> lastPattern_{}; // each enemy's last pattern (-1: none yet)
    int turnCursor_ = 0;                                     // the enemy the round robin tries next
    std::int32_t nextGenBar_ = 0;                            // the next bar to generate
    std::int32_t nextStartBar_ = 0;                          // the next bar to start (stats().bars counts them)
    std::array<Bar, 2> bars_{};
    std::uint32_t nextId_ = 1;
    double lockoutUntil_ = -1e30;

    std::array<BeatPrompt, kMaxBeatPrompts> prompts_{}; // open prompts, in time order
    std::size_t promptCount_ = 0;
    std::array<Landed, 8> landed_{};
    std::size_t landedNext_ = 0;
};

// ---- data ----

// One fight as the host starts it.
struct BeatSetup {
    BeatFightDef def;
    BeatTuning tuning;
    std::string music; // the cue id the host plays; "" none (the host keeps time with the wall clock)
};

struct BeatEntry {
    std::string scene;
    std::uint32_t beat = 0, step = 0; // the gameplay beat index and plan step index (not a musical beat)
    BeatSetup setup;
};

struct BeatTable {
    std::vector<std::string> enemyIds;
    std::vector<BeatEnemy> enemies;
    BeatSetup fallback; // the "default" fight
    std::vector<BeatEntry> entries;

    // The entry for (scene, beat, step), or nullptr. No allocation.
    FTCORE_API const BeatEntry* findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
    // The fight for (scene, beat, step): the entry's, else the default. No allocation.
    FTCORE_API const BeatSetup& find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importBeatTable(std::string_view json, BeatTable& out, std::string* error = nullptr,
                                std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
