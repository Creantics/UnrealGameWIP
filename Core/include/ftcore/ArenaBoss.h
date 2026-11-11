#pragma once

// Arena boss pieces: what a story boss in the Arena fight (ArenaFight.h) has that a plain enemy kind
// doesn't. ArenaFight still runs the bodies, the blows and the hit tests. These pieces sit beside it:
// the host feeds them ArenaFight's events and the lead's buttons, and they say what the boss's rules
// make of them. Every Arena boss reuses them; a boss's own rules compose them (StoneBearer.h is the
// first).
//
// Time. Every time passed in is the fight's clock in seconds, fed in order; an earlier time, or one that
// is not finite (inf, NaN), is read as now(), so a bad clock stalls the run instead of hanging it. A hp
// share that is NaN moves no phase and fires no trigger. Data counts in beats of the boss's tempo (one
// beat is 60 / bpm seconds); Core converts once, with arenaBeats(). Comparisons in beats never go
// through seconds.
//
// Ground plane. Positions and facings are ArenaFight's: Vec2 in metres on the host's X/Y, X forward and
// Y to the right seen from above (the host's yaw is atan2(y, x)). A facing f points to its right at
// (-f.y, f.x).
//
// The pieces:
//
// * Called blows (CalledStrike). A called blow has 1..kMaxCalledImpacts impacts, each in a zone: the
//   boss's own Left or Right side, or the Path of a thrown piece (a lane from the boss to a point). The
//   lead answers with a hold, Guard or Intercept, each pressed in a zone (the host judges it with
//   arenaZoneOf against the next pending impact). An impact is answered by the hold that is on when it
//   lands:
//     - no hold: Missed;
//     - a Path impact meets only a body in the path: a hold in any other zone is Missed (out of the
//       path is the miss);
//     - an Intercept on the other side of a Left or Right impact: WrongZone (it lands like a miss);
//     - an Intercept in the impact's zone begun within `window` of it, [impact - window, impact):
//       Intercepted (the lead turns the blow);
//     - an Intercept in the impact's zone begun earlier, or a Guard: Guarded (held, not turned).
//   A Left or Right blow follows the body, so a Guard meets it from either side. Rules of the buttons:
//     - one hold at a time: a press of either while a hold is on is ignored, so an Intercept tapped
//       during a held Guard never registers (Guard stays the answer);
//     - an accepted Intercept press rearms after `rearm` seconds: another Intercept press before then
//       is ignored, so pressing early and again is not free;
//     - a press or release first resolves every pending impact at or before its time with the hold as
//       it was (a press on the impact itself is too late; a release on it still holds);
//     - the hold and the rearm are the lead's buttons, not the blow's: they carry over start().
//
// * Weak points (CritWindow). WeakPoint.h's points, each behind a gate: a direction in the boss's own
//   frame (0 degrees its facing, 90 its right, 180 behind) and a half-arc. While the window is open a
//   blow from inside a point's gate that lands on it (judgeShot over the gated points) is a crit: the
//   point's multiplier on the blow's damage and poise. A body on the boss's own centre passes only an
//   all-round gate (arc 180). open() starts a new opening (and replaces the end time); the first crit of
//   each opening is flagged `first`.
//
// * The locked weak point (WeakPointLock.h). A boss's weak points may be declared locked, with the keys
//   that open them: a shared blow, a read clause, a refusal. Locked, its points take no crit, whatever
//   opened the window. The host reports each key to the run (offerUnlock); an unlock opens the lock and
//   the crit window together for the lock's openSeconds (Unlocked, CritOpened), a deferred refusal or
//   relock() shuts both (Relocked, CritClosed). A full intercept still opens the window, but a
//   locked point stays shut through it. A boss whose points are not locked is unchanged.
//
// * The breakable (ArenaBreakable). A piece the boss carries breaks on its first Stagger, in any phase,
//   once, or earlier when a boss rule breaks it (breakNow: a beat of its own, which cuts nothing). The
//   run then plays each phase's broken move table.
//
// * Phases (ArenaBossPhaseTrack). The boss's hp falls through each phase to its floor (endAt x max hp,
//   as BeatBoss): at or below it the next phase starts. Phases never go back. Each phase has a whole and
//   a broken move table (indices into the boss's move list). ArenaHpTrigger fires once at its share.
//
// * The guest's calls (GuestCaller). A guest at the arena's edge calls the answer of a called move: the
//   k-th call for the k-th impact, `calls[k]` beats after the move starts. Every call lands at least
//   `fromBeats` into the tell and at least `minLeadBeats` before its impact (the loader checks both, in
//   beats). After the lead misses a marked move every call comes `missShiftBeats` later, for the rest of
//   the phase: not cumulative, cleared when the phase changes. The loader keeps the shift below the
//   lead, so a shifted call still comes before its impact. A repeating call (every N beats from a start)
//   takes the shift too.
//
// * The keep-out (ArenaKeepOut). A disc on the floor no attack may reach (the guest's spot). The
//   clamps shorten an arc's or a lane's reach and push an area's centre so the attack only ever touches
//   the disc's edge. Centred on the guest, a lane aimed at the guest ends at the edge.
//
// * The run (ArenaBossRun) composes them for one boss and turns them into events (ArenaBossEvents, a
//   fixed array, cleared by each call that returns it):
//     startMove(move, t, side)   the boss began move `move` (the host's AttackStarted). Refused (false)
//                                when the move isn't in the current table. Cancels the live move. A
//                                called move schedules its calls; an Intercept move starts a CalledStrike
//                                (zones "side" = `side`, "other" = the opposite side, "path" = Path).
//     update(now, hpShare)       the phase first (PhaseChanged, which clears the miss shift), then what
//                                fell due by now in time order: Call, CritClosed (the window ran out),
//                                Answered (an impact), in that order on a tie.
//     intercept/guard Down/Up    the lead's buttons (Answered for impacts they resolve).
//     onStagger(t)               Broke the first time (the tables swap); cancels the live move. Broke
//                                names the move it cut (one with a call or impact still pending), else
//                                kArenaBossNoMove.
//     breakNow()                 a boss rule's break at now(): Broke the first time, with
//                                kArenaBossNoMove (it cuts nothing: the live move plays on).
//     onHit(...)                 a lead's blow on the boss: the crit judge (Crit).
//     offerUnlock(key, t, clause) a key for the locked weak point (Unlocked, CritOpened).
//     onDeferred(t), relock(t)   a deferred refusal (when the lock relocks on one) or a rule shuts the lock
//                                (Relocked, CritClosed).
//   Answered for the last impact of an Intercept move whose impacts were all Intercepted opens the crit
//   window for the move's `opens` seconds from that impact (CritOpened) and gives its `load`
//   (LoadGained). The first Missed or WrongZone impact of each use of a move is flagged (its Answered has
//   value 1; a boss rule that charges a miss once per use reads it); on a marked move it also turns the
//   miss shift on at once (CallsShifted, once a phase), so a cancel after it keeps the shift; calls the
//   live move already scheduled keep their times (the shift reaches the next move's). A Guard is not a
//   miss. A cancelled move's pending calls and impacts are dropped with no event.
//   liveMove() is the last started move until the next startMove, cancelMove or Stagger (a move whose
//   impacts have all landed stays live; pending() says whether anything of it is still to come).
//
// The data (one file per boss, Data/Battle/arena_boss/<id>.json; importArenaBoss reads the generic
// keys, a boss's own loader the rest):
//   { "version": 1, "bpm": 76, "hp": 1400, "poise": 160, "staggerTime": 2.0, "openTime": 3.0,
//     "intercept": { "window": 0.15, "rearm": 0.4 },
//     "guestCall": { "fromBeats": 1, "minLeadBeats": 2, "missShiftBeats": 0.5 },
//     "weakPoints": { "halfWidth": 0.5, "points": [ { "id": "nape", "x": 0, "y": 0.78, "radius": 0.1,
//                     "multiplier": 2.0, "facing": 180, "arc": 60 } ],
//                     "lock": { "keys": ["shared_blow"], "openSeconds": 3.0 } },
//     "keepOut": { "bearing": 90, "distance": 20, "radius": 3 },
//     "moves": [ { "id": "called_fist", "answer": "intercept", "impacts": [3], "calls": [1],
//                  "zones": ["side"], "sheds": [true], "active": 0.2, "recovery": 1.2, "range": 2.6,
//                  "damage": 0.12, "guardStacks": 2, "missStacks": 1, "opens": 3.0 }, ... ],
//     "phases": [ { "endAt": 0.5, "moves": ["called_fist", ...], "brokenMoves": [...] },
//                 { "endAt": 0, "moves": [...] } ] }
//   bpm 40..240; hp 1..100000; poise 1..10000; staggerTime and openTime 0.1..10 s (the wiring copies
//   these four onto the boss's ArenaEnemyKind). intercept.window 0.05..0.5 s, rearm 0..2 s.
//   guestCall: fromBeats 0..8, minLeadBeats 0.5..8, missShiftBeats 0..minLeadBeats (below it).
//   weakPoints: as Data/Battle/weakpoints.json's entries (WeakPoint.h) plus each point's gate,
//   facing 0..360 and arc 0..180 degrees (defaults 0 and 180: all round).
//   lock (optional; absent: not locked): keys, 1..3 distinct of "shared_blow", "read_clause", "refusal";
//   clause 1..65535 (required with "read_clause", else not allowed); openSeconds 0 (open until a relock)
//   or 0.1..30 (default 3); relockOnDefer (default true).
//   A boss whose moves are not in this format (a kind that keeps arena.json's) can still have weak points:
//   a standalone file { "version": 1, "weakPoints": { ... } } with the same block, read by
//   importArenaWeakPoints (Data/Battle/arena_boss/kite_boss_weak.json is the first).
//   keepOut: bearing 0..360 degrees (the arena's own frame, the host's yaw), distance 0..100 m from the
//   arena's centre, radius 0..20 m (0: none). Absent: none.
//   moves: 1..kMaxArenaBossMoves, ids lowercase [a-z0-9_] of 1..kMaxArenaBossMoveId characters, unique.
//     answer: "intercept", "dodge", "jump", "tile" (stand where the call says), "cover" (put something
//       between), "any" (a plain blow). Only an intercept move has zones, sheds, marked, opens, load,
//       guardStacks or missStacks.
//     impacts: 1..kMaxCalledImpacts beats from the move's start, each in (0, 32], strictly rising.
//     calls: none, or one per impact, rising, each at least fromBeats and at least minLeadBeats before
//       its impact.
//     zones: one per impact of an intercept move: "side", "other" or "path". sheds: one bool per impact
//       (default false): an Intercept on it sheds a stack (the boss's rule; the run only carries it).
//     An intercept move's impacts are more than `rearm` seconds apart, so each can be answered.
//     active 0.05..5 s, recovery 0..10 s, range 0..50 m, damage 0..1 (a share of the lead's max hp per
//     impact), damageMax damage..1 (most it deals in one use; default damage x impacts), floor and
//     marked bools (marked: intercept moves only), guardBreak 0..5 s, guardStacks and missStacks 0..5
//     (the boss's rule), opens 0..10 s, load 0..kMaxLoad.
//     contact (optional): { "socket": id, "ahead": m, "side": m, "up": m }, where the part that strikes
//       (socket, an id like a move's) sits at the clip's impact frame, from the boss's root in its own
//       frame: ahead along its facing, side to its right, up; ahead and side -20..20, up -5..20. The host
//       reads it for its strike point and reach; absent: none (hasContact false).
//   phases: 1..kMaxArenaBossPhases; endAt in [0, 1), strictly falling, the last 0. moves and brokenMoves
//     (default: moves): 1..kMaxArenaPhaseMoves known move ids, no repeats.
//   An unknown key is a warning; anything else wrong is an error with its path, and `out` is left
//   default.
//
// Wiring a move onto ArenaFight (ArenaEnemyMove/ArenaAttack, ArenaFight.h):
//   - A move of n impacts is n chained ArenaEnemyMoves: the first's startup is impacts[0] beats, each
//     next one's startup the gap to its impact; each has the move's active, range and damage (times the
//     lead's max hp); the last has the move's recovery, the others none.
//   - poiseDamage and guardDamage are the enemy kind's own defaults; guardBreak (when above 0) is the
//     ArenaAttack's guard-break time.
//   - The opening: on CritOpened the host holds the boss open (kneeling, no new move) until the
//     event's `until`, the move's `opens` seconds from its last impact, instead of its plain
//     `recovery`; CritClosed ends it. The recovery is the boss's time after a blow that was not turned.
//   - damageMax caps what one use deals in all: the host stops landing the move's blows on the lead
//     once their damage reaches it. A "tile" move rains fragments over its area during `active`, each
//     a blow of `damage` on a lead off the clear tile; the host owns the fragments, which tile is
//     clear (the one its call names) and, for a move of two calls, the second tile.
//   - The hit shape by answer: "intercept" and "any" an arc (arcCos 0.5, the kind's default width);
//     "path" impacts and "cover" a lane (width 1 m) from the boss to the aimed point; "tile" all round.
//     Every shape then goes through the keep-out clamp.
//   - Answer "intercept" is not parryable: the run answers it, ArenaFight only lands it.
//
// Nothing here allocates except importArenaBoss and importArenaWeakPoints (load time).

#include "ftcore/Api.h"
#include "ftcore/Explore.h" // Vec2
#include "ftcore/BossScript.h"
#include "ftcore/WeakPoint.h"
#include "ftcore/WeakPointLock.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

// Seconds in `beats` beats at `bpm` (0 when bpm <= 0).
inline double arenaBeats(double beats, double bpm) { return bpm > 0.0 ? beats * 60.0 / bpm : 0.0; }

// ---- Called blows ----------------------------------------------------------------------------------

enum class CalledZone : std::uint8_t { Left, Right, Path }; // append only

// The side of the boss at `pos` facing `facing` (unit) that a body at `p` stands on: Right when it lies
// to the right of the facing line, else Left (on the line, ahead or behind, counts as Left).
FTCORE_API CalledZone arenaSideOf(Vec2 pos, Vec2 facing, Vec2 p);
// `p` lies in the lane from `from` to `to`, `width` metres wide: its projection falls between the two
// ends (both included) and it is at most width / 2 off the line. A lane shorter than a millimetre holds
// nothing.
FTCORE_API bool arenaInLane(Vec2 from, Vec2 to, float width, Vec2 p);
// The zone to press in for an impact in zone `next`: Path when `next` is Path and the body is in the
// lane from the boss to `laneEnd`, else its side of the boss.
FTCORE_API CalledZone arenaZoneOf(CalledZone next, Vec2 pos, Vec2 facing, Vec2 laneEnd, float laneWidth, Vec2 p);

inline constexpr std::size_t kMaxCalledImpacts = 4;

struct CalledImpact {
    double at = 0.0; // impact time
    CalledZone zone = CalledZone::Left;
    bool sheds = false; // carried for the boss's rules
};

enum class CalledOutcome : std::uint8_t { Pending, Intercepted, Guarded, WrongZone, Missed }; // append only
// "INTERCEPT", "GUARD", "WRONG SIDE", "MISSED" (or "" for Pending). A literal: valid forever.
FTCORE_API const char* calledOutcomeName(CalledOutcome o);

struct CalledWindows {
    double window = 0.15; // an Intercept begun this long or less before an impact (not on it) turns it
    double rearm = 0.40;  // after an accepted Intercept press, further Intercept presses are ignored this long
};

struct CalledResult {
    std::uint8_t impact = 0; // index into the impacts
    CalledOutcome outcome = CalledOutcome::Pending;
    double at = 0.0; // the impact time
};

// Impacts resolved by one call, in time order. Fixed capacity: no allocation.
struct CalledResults {
    std::array<CalledResult, kMaxCalledImpacts> items{};
    std::size_t count = 0;

    const CalledResult* begin() const { return items.data(); }
    const CalledResult* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const CalledResult& operator[](std::size_t i) const { return items[i]; }
};

enum class ArenaHold : std::uint8_t { None, Guard, Intercept }; // append only

class CalledStrike {
public:
    void setWindows(const CalledWindows& w) { windows_ = w; }
    const CalledWindows& windows() const { return windows_; }

    // Replaces the impacts with `impacts[0..count)` (sorted by time; past kMaxCalledImpacts dropped).
    // The hold and the rearm carry over.
    FTCORE_API void start(const CalledImpact* impacts, std::size_t count);
    // Drops every pending impact unresolved (the move was cancelled).
    FTCORE_API void cancel();

    // Moves the clock to `now`: impacts at or before it resolve against the hold. The clock starts
    // before any time, so the first update or press sets it (a fight clock may start below 0). A time
    // that is not finite moves nothing (here and in every press).
    FTCORE_API CalledResults update(double now);
    // The lead's buttons, pressed with the body in `zone`.
    FTCORE_API CalledResults interceptDown(CalledZone zone, double t);
    FTCORE_API CalledResults interceptUp(double t);
    FTCORE_API CalledResults guardDown(CalledZone zone, double t);
    FTCORE_API CalledResults guardUp(double t);

    double now() const { return now_; }
    ArenaHold hold() const { return hold_; }
    double holdFrom() const { return holdFrom_; }
    CalledZone holdZone() const { return holdZone_; }
    // An Intercept press at t would be ignored for the rearm.
    bool rearming(double t) const { return t < rearmUntil_; }

    std::size_t size() const { return count_; }
    const CalledImpact& impact(std::size_t i) const { return impacts_[i]; }
    CalledOutcome outcome(std::size_t i) const { return outcomes_[i]; }
    // Earliest pending impact (size() when none).
    std::size_t nextIndex() const { return next_; }
    bool complete() const { return next_ >= count_; }
    // The zone of the next pending impact (Left when none).
    CalledZone nextZone() const { return complete() ? CalledZone::Left : impacts_[next_].zone; }
    // A press time that turns impact i: the middle of its window.
    double idealPress(std::size_t i) const { return impacts_[i].at - windows_.window * 0.5; }

private:
    CalledOutcome judge(const CalledImpact& imp) const;
    CalledResults resolveUpTo(double t);

    CalledWindows windows_{};
    std::array<CalledImpact, kMaxCalledImpacts> impacts_{};
    std::array<CalledOutcome, kMaxCalledImpacts> outcomes_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
    double now_ = -1.0e300;
    ArenaHold hold_ = ArenaHold::None;
    double holdFrom_ = 0.0;
    CalledZone holdZone_ = CalledZone::Left;
    double rearmUntil_ = -1.0e300;
};

// ---- Weak points -----------------------------------------------------------------------------------

struct ArenaWeakGate {
    float facing = 0.0f; // degrees in the boss's frame: 0 its facing, 90 its right, 180 behind
    float arc = 180.0f;  // half-arc in degrees: a blow from within it of `facing` reaches the point (180: all round)
};

// WeakPoint.h's points with a gate each (gates[i] guards points[i]).
struct ArenaWeakPoints {
    WeakPoints points;
    std::array<ArenaWeakGate, kMaxWeakPoints> gates{};
    bool locked = false; // the points are behind a WeakPointLock with `lock`
    LockTuning lock{};
};

// A body at `from` is inside `gate` of a boss at `pos` facing `facing` (unit).
FTCORE_API bool arenaGatePasses(const ArenaWeakGate& gate, Vec2 pos, Vec2 facing, Vec2 from);

struct CritResult {
    bool crit = false;
    std::uint8_t point = kMaxWeakPoints; // index of the point hit (kMaxWeakPoints: none)
    float multiplier = 1.0f;             // on the blow's damage and poise
    bool first = false;                  // the first crit of this opening
};

class CritWindow {
public:
    void reset(const ArenaWeakPoints& points) {
        points_ = points;
        openings_ = 0;
        close();
    }
    // A new opening, ending at now + seconds (an open window is replaced).
    void open(double now, double seconds) {
        from_ = now;
        until_ = now + seconds;
        crits_ = 0;
        ++openings_;
    }
    void close() {
        until_ = from_ = -1.0e300;
        crits_ = 0;
    }
    bool isOpen(double t) const { return t >= from_ && t < until_; }
    double closesAt() const { return until_; }
    int openings() const { return openings_; }
    int critsThisOpening() const { return crits_; }
    const ArenaWeakPoints& points() const { return points_; }

    // A blow at t from a body at `from` on the boss at `pos` facing `facing` (unit), landing on figure
    // point `at` (WeakPoint.h's figure space, seen from `from`).
    FTCORE_API CritResult judge(Vec2 from, Vec2 pos, Vec2 facing, AimPoint at, double t);
    // A locked-on blow: it lands on the centre of the best point whose gate passes.
    FTCORE_API CritResult judgeLockOn(Vec2 from, Vec2 pos, Vec2 facing, double t);

private:
    WeakPoints gated(Vec2 from, Vec2 pos, Vec2 facing, std::array<std::uint8_t, kMaxWeakPoints>& index) const;
    CritResult land(const WeakPoints& gated, const std::array<std::uint8_t, kMaxWeakPoints>& index, AimPoint at);

    ArenaWeakPoints points_{};
    double from_ = -1.0e300;
    double until_ = -1.0e300;
    int crits_ = 0;
    int openings_ = 0;
};

// ---- The breakable ---------------------------------------------------------------------------------

struct ArenaBreakable {
    bool broken = false;
    double brokeAt = 0.0;

    // A Stagger (or a boss rule's break) at t: true the first time (it breaks now), false after.
    bool onStagger(double t) {
        if (broken) return false;
        broken = true;
        brokeAt = t;
        return true;
    }
};

// ---- Phases ----------------------------------------------------------------------------------------

inline constexpr std::size_t kMaxArenaBossMoves = 24;   // a boss's whole move list
inline constexpr std::size_t kMaxArenaPhaseMoves = 12;  // one phase's table
inline constexpr std::size_t kMaxArenaBossPhases = 4;
inline constexpr std::uint8_t kArenaBossNoMove = 0xFF;

struct ArenaBossMoveTable {
    std::array<std::uint8_t, kMaxArenaPhaseMoves> moves{}; // indices into the boss's move list
    std::uint8_t count = 0;

    std::size_t size() const { return count; }
    const std::uint8_t* begin() const { return moves.data(); }
    const std::uint8_t* end() const { return moves.data() + count; }
    std::uint8_t operator[](std::size_t i) const { return moves[i]; }
    bool add(std::uint8_t m) {
        if (count >= kMaxArenaPhaseMoves) return false;
        moves[count++] = m;
        return true;
    }
    bool contains(std::uint8_t m) const {
        for (std::size_t i = 0; i < count; ++i)
            if (moves[i] == m) return true;
        return false;
    }
};

struct ArenaBossPhase {
    float endAt = 0.0f; // the floor, as a share of max hp (the last phase: 0)
    ArenaBossMoveTable whole;
    ArenaBossMoveTable broken;
};

class ArenaBossPhaseTrack {
public:
    // Phase 0 of phases[0..count).
    FTCORE_API void reset(const ArenaBossPhase* phases, std::size_t count);
    // The boss at `hpShare` of its max hp: moves past every phase whose floor it is at or below (never
    // past the last). Returns how many phases it moved.
    FTCORE_API int update(float hpShare);
    std::size_t phase() const { return phase_; }
    std::size_t count() const { return count_; }
    const ArenaBossMoveTable& table(bool broken) const {
        return broken ? phases_[phase_].broken : phases_[phase_].whole;
    }

private:
    std::array<ArenaBossPhase, kMaxArenaBossPhases> phases_{};
    std::size_t count_ = 0;
    std::size_t phase_ = 0;
};

// Fires once, the first time the hp share is at or below `at` (NaN never fires it).
struct ArenaHpTrigger {
    float at = 0.0f;
    bool fired = false;

    bool update(float hpShare) {
        if (fired || !(hpShare <= at)) return false;
        fired = true;
        return true;
    }
};

// ---- The guest's calls -----------------------------------------------------------------------------

struct GuestCallTuning {
    double fromBeats = 1.0;      // a call lands at least this far into its move's tell
    double minLeadBeats = 2.0;   // and at least this far before its impact
    double missShiftBeats = 0.5; // after a marked miss, every call this much later, for the phase
};

class GuestCaller {
public:
    void reset(double bpm, const GuestCallTuning& tuning) {
        bpm_ = bpm;
        tuning_ = tuning;
        shifted_ = false;
    }
    // The call `callBeats` beats into a move that started at `start`, with the shift.
    double cue(double start, double callBeats) const { return start + arenaBeats(callBeats + shiftBeats(), bpm_); }
    // The first call at or after `now` of the series start + k x everyBeats (k >= 0), with the shift.
    FTCORE_API double nextRepeat(double start, double everyBeats, double now) const;
    // The call of that series nearest to t (the earlier on a tie).
    FTCORE_API double nearestRepeat(double start, double everyBeats, double t) const;
    // A marked miss: true when it turns the shift on (false when already on).
    bool onMarkedMiss() {
        const bool was = shifted_;
        shifted_ = true;
        return !was;
    }
    void onPhaseChange() { shifted_ = false; }
    bool shifted() const { return shifted_; }
    double shiftBeats() const { return shifted_ ? tuning_.missShiftBeats : 0.0; }
    double bpm() const { return bpm_; }
    const GuestCallTuning& tuning() const { return tuning_; }

private:
    double bpm_ = 120.0;
    GuestCallTuning tuning_{};
    bool shifted_ = false;
};

// ---- The keep-out ----------------------------------------------------------------------------------

struct ArenaKeepOut {
    Vec2 centre;
    float radius = 0.0f; // 0: none

    bool none() const { return !(radius > 0.0f); }
    // The longest reach, at most `reach`, of an arc swept from `origin` round `dir` (unit), `arcCos`
    // wide (ArenaAttack::arcCos; -1 all round), that touches at most the disc's edge. 0 with the origin
    // in the disc.
    FTCORE_API float clampArc(Vec2 origin, Vec2 dir, float arcCos, float reach) const;
    // The same for a straight lane `width` wide (ArenaAttack::width), starting on the segment `width`
    // across `origin`. 0 when even that start touches the disc. A reach of 0 is no attack: the host
    // drops the blow instead of swinging it. Both return 0 for a reach that is not above 0 (or NaN).
    FTCORE_API float clampLane(Vec2 origin, Vec2 dir, float width, float reach) const;
    // `point` moved straight away from the centre until a circle of `areaRadius` round it touches at
    // most the edge (a point on the centre moves along +X).
    FTCORE_API Vec2 clampArea(Vec2 point, float areaRadius) const;
};

// The keep-out of an arena centred on `centre` with yaw `yawDeg`: `distance` metres out along
// `bearingDeg` (the arena's own frame), `radius` wide.
FTCORE_API ArenaKeepOut arenaKeepOutAt(Vec2 centre, float yawDeg, float bearingDeg, float distance, float radius);

// ---- The boss's data -------------------------------------------------------------------------------

enum class ArenaBossAnswer : std::uint8_t { Intercept, Dodge, Jump, Tile, Cover, Any }; // append only
enum class CalledZoneRule : std::uint8_t { Side, Other, Path };                       // append only

inline constexpr std::size_t kMaxArenaBossMoveId = 23;

struct ArenaBossMove {
    std::array<char, kMaxArenaBossMoveId + 1> idChars{};
    ArenaBossAnswer answer = ArenaBossAnswer::Any;
    std::array<float, kMaxCalledImpacts> impacts{}; // beats from the move's start
    std::array<float, kMaxCalledImpacts> calls{};   // beats from the move's start (callCount of them)
    std::array<CalledZoneRule, kMaxCalledImpacts> zones{};
    std::array<bool, kMaxCalledImpacts> sheds{};
    std::uint8_t impactCount = 1;
    std::uint8_t callCount = 0;
    float active = 0.2f;
    float recovery = 1.0f;
    float range = 2.0f;
    float damage = 0.0f;    // share of the lead's max hp per impact
    float damageMax = 0.0f; // most one use deals
    bool floor = false;     // a blow into the floor
    bool marked = false;    // the marked move
    float guardBreak = 0.0f; // a Guard against it breaks the lead's guard this long (the host's ArenaFight)
    std::uint8_t guardStacks = 0;
    std::uint8_t missStacks = 0;
    float opens = 0.0f; // crit window after a full intercept, seconds
    int load = 0;       // Load a full intercept gives
    // The striking part at the clip's impact frame, metres from the boss's root in its own frame (none: hasContact
    // false, the rest zero).
    bool hasContact = false;
    float contactAhead = 0.0f; // along its facing
    float contactSide = 0.0f;  // to its right
    float contactUp = 0.0f;
    std::array<char, kMaxArenaBossMoveId + 1> contactSocketChars{};

    std::string_view id() const { return std::string_view(idChars.data()); }
    std::string_view contactSocket() const { return std::string_view(contactSocketChars.data()); }
    bool called() const { return callCount > 0; }
    bool intercept() const { return answer == ArenaBossAnswer::Intercept; }
};

struct ArenaBossDef {
    double bpm = 120.0;
    int hp = 1000;
    float poise = 100.0f;
    float staggerTime = 2.0f;
    float openTime = 3.0f;
    CalledWindows intercept{};
    GuestCallTuning guestCall{};
    ArenaWeakPoints weakPoints{};
    float keepOutBearing = 0.0f;
    float keepOutDistance = 0.0f;
    float keepOutRadius = 0.0f;
    std::array<ArenaBossMove, kMaxArenaBossMoves> moves{};
    std::uint8_t moveCount = 0;
    std::array<ArenaBossPhase, kMaxArenaBossPhases> phases{};
    std::uint8_t phaseCount = 0;

    // Index of the move with that id, or kArenaBossNoMove.
    FTCORE_API std::uint8_t find(std::string_view id) const;
    double beat() const { return arenaBeats(1.0, bpm); }
};

// Reads the generic keys of a boss file into `out`; the top-level keys in `ownKeys` belong to the
// boss's own loader and draw no warning. On failure `out` is default and *error lists each problem with
// its path, one per line.
FTCORE_API bool importArenaBoss(std::string_view json, ArenaBossDef& out, std::string* error = nullptr,
                                std::vector<std::string>* warnings = nullptr,
                                std::initializer_list<std::string_view> ownKeys = {});

class JsonValue;
// Reads a "weakPoints" object (the format above) at `path` into `out`, for every loader that carries
// one (importArenaBoss, ArenaKindCrit.h): out of range or wrong is an error on `errors` with its path
// under `path`, an unknown key a warning. `out` starts default. False when it added an error.
FTCORE_API bool importArenaWeakPoints(const JsonValue& v, const std::string& path, ArenaWeakPoints& out,
                                      std::vector<std::string>& errors,
                                      std::vector<std::string>* warnings = nullptr);

// Reads a standalone weak-point file ({ "version": 1, "weakPoints": { ... } }, the block importArenaBoss reads
// and the same errors and warnings; "weakPoints" is required here). On failure `out` is default and *error lists
// each problem with its path, one per line.
FTCORE_API bool importArenaWeakPoints(std::string_view json, ArenaWeakPoints& out, std::string* error = nullptr,
                                      std::vector<std::string>* warnings = nullptr);

// Reads a boss script file (Data/Battle/boss_script/<id>.json; docs/BOSS_SCRIPT.md has the format) into
// `out`, with the same checks as the arena bosses: each range and name is an error with its path, an
// unknown key a warning, and the def must pass validBossScript. On failure `out` is default and *error
// lists each problem, one per line.
FTCORE_API bool importBossScript(std::string_view json, BossScriptDef& out, std::string* error = nullptr,
                                 std::vector<std::string>* warnings = nullptr);

// ---- The run ---------------------------------------------------------------------------------------

enum class ArenaBossEventType : std::uint8_t { // append only
    PhaseChanged,  // value = the phase now
    Broke,         // the breakable broke: the broken tables play from now (move: the move a stagger cut,
                   // else kArenaBossNoMove)
    Call,          // the guest's k-th call of `move`, for its k-th impact (impact = k; move
                   // kArenaBossNoMove: a repeating call, impact 0)
    Answered,      // `impact` of `move` resolved: `outcome`
    CallsShifted,  // a marked miss: every call is late by the miss shift for the rest of the phase
    CritOpened,    // the weak points open until `until`
    CritClosed,    // ... and shut
    Crit,          // a crit landed on point `impact`: value = 1 for the first of its opening, else 0
    LoadGained,    // value = the Load the lead gains (a full intercept of `move`)
    // A boss's own rules, appended here so the host reads one event stream (StoneBearer.h):
    StacksChanged,  // value = the stacks now, amount = the change
    ShockRing,      // value = 1 on, 0 off: floor blows send a shock ring
    PoiseFrozen,    // value = 1 on, 0 off
    Invulnerable,   // value = 1 on, 0 off
    Lifted,         // a Release on the call ended the invulnerable phase; down until `until`
    Recovered,      // the boss is up again
    Unlocked,       // the locked weak point opened: value = the UnlockKey, until = when it relocks
                    // (+inf: until a relock); value 0 with amount 1: an offer while open restarted it
    Relocked,       // the locked weak point shut before its time (a deferral or a rule)
};

struct ArenaBossEvent {
    ArenaBossEventType type = ArenaBossEventType::Call;
    std::uint8_t move = kArenaBossNoMove;
    std::uint8_t impact = 0;
    CalledOutcome outcome = CalledOutcome::Pending;
    int value = 0;
    int amount = 0;
    double at = 0.0;    // when it happened
    double until = 0.0; // CritOpened, Lifted
};

// Per call. Room for the most one call can emit (a boss's own rules static_assert their bound against
// it, as StoneBearer.cpp does), so nothing a rule reads back is lost; past it events are dropped
// (counted in `dropped`).
inline constexpr std::size_t kMaxArenaBossEvents = 48;

struct ArenaBossEvents {
    std::array<ArenaBossEvent, kMaxArenaBossEvents> items{};
    std::size_t count = 0;
    std::size_t dropped = 0;

    const ArenaBossEvent* begin() const { return items.data(); }
    const ArenaBossEvent* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const ArenaBossEvent& operator[](std::size_t i) const { return items[i]; }
    void clear() { count = dropped = 0; }
    void push(const ArenaBossEvent& e) {
        if (count < kMaxArenaBossEvents) items[count++] = e;
        else ++dropped;
    }
};

class ArenaBossRun {
public:
    // Starts `def` (kept by reference: it must outlive the run) at `now`, phase 0, whole.
    FTCORE_API void begin(const ArenaBossDef& def, double now);

    FTCORE_API const ArenaBossEvents& startMove(std::uint8_t move, double t, CalledZone side, bool* started = nullptr);
    FTCORE_API const ArenaBossEvents& update(double now, float hpShare);
    FTCORE_API const ArenaBossEvents& interceptDown(CalledZone zone, double t);
    FTCORE_API const ArenaBossEvents& interceptUp(double t);
    FTCORE_API const ArenaBossEvents& guardDown(CalledZone zone, double t);
    FTCORE_API const ArenaBossEvents& guardUp(double t);
    FTCORE_API const ArenaBossEvents& onStagger(double t);
    // The stagger at now(), appended to the last call's events instead of clearing them: for a boss
    // rule that advanced the run to the stagger's time first, to judge it with every answer up to it.
    FTCORE_API void staggerNow();
    // A boss rule breaks the breakable at now() without a stagger: Broke (move kArenaBossNoMove), appended
    // as staggerNow's; the live move plays on. True when it broke now, false when it was already broken.
    FTCORE_API bool breakNow();
    // A lead's blow (see CritWindow::judge / judgeLockOn; `at` null: locked on).
    FTCORE_API const ArenaBossEvents& onHit(Vec2 from, Vec2 pos, Vec2 facing, double t, const AimPoint* at,
                                            CritResult* result);
    // Drops the live move's pending calls and impacts (no events).
    FTCORE_API void cancelMove();

    // The locked weak point. `result` (optional) says what the lock made of the key; with no lock
    // declared nothing happens and it is BadKey.
    FTCORE_API const ArenaBossEvents& offerUnlock(UnlockKey key, double t, std::uint16_t clause = 0,
                                                  UnlockResult* result = nullptr);
    FTCORE_API const ArenaBossEvents& onDeferred(double t);
    FTCORE_API const ArenaBossEvents& relock(double t);
    bool weakPointLocked() const { return lock_.locked(); }
    // The lock is open now (always true for a boss whose points are not locked).
    bool weakPointOpen() const { return lock_.open(now_); }
    const WeakPointLock& lock() const { return lock_.lock(); }

    const ArenaBossDef& def() const { return *def_; }
    double now() const { return now_; }
    std::size_t phase() const { return phases_.phase(); }
    bool broken() const { return breakable_.broken; }
    const ArenaBossMoveTable& table() const { return phases_.table(breakable_.broken); }
    std::uint8_t liveMove() const { return live_; }
    // The live move still has a call or an impact to come.
    bool pending() const { return live_ != kArenaBossNoMove && (cueNext_ < cueCount_ || !strike_.complete()); }
    // `t` as the run reads it: now() when it is earlier or not finite.
    double timeOf(double t) const { return std::isfinite(t) && t > now_ ? t : now_; }
    double liveStart() const { return liveStart_; }
    const CalledStrike& strike() const { return strike_; }
    const GuestCaller& guest() const { return guest_; }
    GuestCaller& guest() { return guest_; }
    const CritWindow& crit() const { return crit_; }
    const ArenaBossEvents& events() const { return events_; }
    // Pushes a boss rule's event into the current call's events.
    void push(const ArenaBossEvent& e) { events_.push(e); }
    void clearEvents() { events_.clear(); }

private:
    void advance(double t);
    void absorb(const CalledResults& results);
    void shutLock();

    const ArenaBossDef* def_ = nullptr;
    ArenaBossPhaseTrack phases_{};
    ArenaBreakable breakable_{};
    CalledStrike strike_{};
    GuestCaller guest_{};
    CritWindow crit_{};
    LockedPoint lock_{};
    ArenaBossEvents events_{};
    double now_ = 0.0;
    std::uint8_t live_ = kArenaBossNoMove;
    double liveStart_ = 0.0;
    std::array<double, kMaxCalledImpacts> cues_{};
    std::uint8_t cueCount_ = 0;
    std::uint8_t cueNext_ = 0;
    bool critWasOpen_ = false;
    bool missed_ = false; // the live use has had its first miss
};

} // namespace ftcore
