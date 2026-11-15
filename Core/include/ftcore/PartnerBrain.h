#pragma once

// The partner's brain: what the lead not under the player's hand does in an arena fight, one intent a step. It decides
// only; ArenaFight (through a hook the host adds) feeds it a view each step and applies the intent with the ally's
// actions. Plain data in, one intent out, no allocation per step.
//
// The order (the player's button): StayClose or Spread.
//   1. Target. Spread: the nearest standing enemy the lead is not attacking; if only one enemy stands, that one. When
//      the lead attacks every standing enemy, or none, the nearest standing one. StayClose: the lead's target (none:
//      no target), and never further than leashMeters from the lead: past it the intent is MoveTo back, until within
//      returnMeters (no flicker at the edge); closing on its target it never aims past returnMeters from the lead
//      (so a target in reach only from 3..4 m out is not attacked). A Down enemy is never a target and does not count as standing. The target
//      is chosen when a string starts and kept to its end. Spread keeps a chosen target at least targetHoldSeconds
//      (1.5) before another may take it, unless it falls (Down or absent) or, once it has been within reach, it
//      leaves reach. StayClose has no hold: the lead's target is its target, so the lead changing target (or having
//      none) is the target no longer being attackable, and it follows at once.
//   2. Rhythm. A light string of blowsMin..blowsMax blows, then a pause of pauseMin..pauseMax s, both drawn (seeded)
//      when the string starts. The host says how long each blow of a string lasts (blowSeconds, the i-th light) and
//      keeps to it: a blow once started always runs out (no host can cancel one). The string's clock is those
//      lengths: the next blow starts on the first step at or after the last one ends, and the string is over on the
//      first step after its last blow ends. It never starts a string while its target winds up a blow aimed at it.
//      Budget (a partner never out-fights the player, and a lead pinned down defending does not freeze it): over any
//      rolling budgetWindowSeconds (20) window, its attack time is at most budgetShare (0.60) x the lead's attack
//      time in that window plus budgetAllowanceSeconds (2.0). The host says each step whether the lead is attacking
//      (leadAttacking). The partner's attack time is the whole of every blow it began (whatever it does after: a
//      blow begun counts to its end), and any step the host reports Attacking outside them. Every blow is decided
//      before it starts, on its full length plus what is left of a blow still running, against every window that
//      would hold any of it with the lead attacking no more; so a blow it begins can never carry a window over (a
//      paired counter aside: see below). A
//      string starts only if its first blow fits. When it does not, the intent is Wait (why Budget) on its target,
//      in reach and facing it; it guards as rule 3 says; the string, already drawn, starts on the first step it fits
//      (no new draw). Mid-string, a next blow that would not fit ends the string there (Budget; its pause follows,
//      or none on a staggered target). A string ended mid-blow (a guard, the leash, cannot act, its target gone)
//      leaves that blow running: no new string starts before it ends (Wait, why Committed). The budget is kept in
//      0.05 s slots, counted against it (the partner's part slots in, the lead's out): with the lengths the host
//      keeps, the only slack left is that slot, and it errs on the side of holding back.
//      Attack time outside its strings (a paired counter: the ruled 1.0 s, reported on the step it lands) comes in as
//      spentSeconds. On that step, before it decides, the brain puts it on the partner's side of the ring in the slots
//      it ran in: back from now, each slot filled up to the time it held, less the partner's own attack there. What
//      finds no room within the window goes into the current slot. So the next string waits exactly as it would
//      after a string of that length ending now. The brain cannot refuse a counter (the player triggers it), so with
//      counters the bound is: over any window, its attack time is at most budgetShare x the lead's attack time plus
//      budgetAllowanceSeconds, plus at most 1.0 s for each paired counter whose time falls in that window (with the
//      3 s cooldown, at most 7 land in 20 s). Every blow it begins fits every window as counted when it begins, the
//      counters so far included; a counter that lands after a blow was decided can carry that blow's window over by
//      the counter's own seconds, and the brain pays it back by waiting before its next string. The host must not also report those seconds as doing Attacking (they
//      would count twice). It adds no state: save, load and the hash are as before.
//   3. Defence. When a windup aimed at the partner is first seen (a new one: the enemy entered WindingUp, or its
//      windupLeft rose) with at least guardWarning s left, one seeded draw: guard with guardChance, else take the blow.
//      It guards from then until guardHold s after the windup ends. With less warning it is hit. It never parries,
//      ripostes or dodges. A guard begun mid-string ends that string (its pause runs from there; the blow it was
//      in runs out).
//   4. It never uses Release, gifts or the count (no intent exists for them).
//   5. Down and up. At health 0 it is Down and waits. After downSeconds Down the intent carries rise (level-triggered:
//      every step until the host reports health above 0) at riseHealth.
//   6. On a staggered target it strings without pauses until the stagger ends (punish on the intent); its blows still
//      add no break value and do not extend the stagger (breakValue 0, extendsStagger false). It does not switch to a
//      staggered enemy: only its own target counts.
//   7. Every Attack intent carries partnerBlow: the fight applies partnerBlowDamage and zero break value.
//      partnerBlowDamage is the single home of the ruled damage rule (the part of a partner blow above 60% of the
//      lead's light is halved): the fight calls it and nothing else. blowCapShare (0.6) and overScale (0.5) are not
//      tuning of their own; they are that rule's two numbers, kept in the data only so the rule has one home.
// An enemy view whose pos or radius is not a finite number (or whose radius is under 0) is absent: never a target,
// never drawn for, and a target that turns absent is dropped as if Down. Views past kMaxPartnerEnemies are ignored.
// StepBack (this module's choice of trigger): during a pause, closer than standoffMeters to its target's edge, it
// steps back to standoffMeters. It is never a dodge.
// Stationing (rules 8-12): where the partner stands, so its blows are seen from the side and it never crowds the lead
// or blocks the camera. Opt-in: only while the host sets PartnerInput::station.on with a whole finite view (camera,
// lead facing, arena); off, every rule above is as it was (intent.stationed false). All on the ground plane.
// When the rules collide the winners are, in order: the arena ring (a goal is never outside it), then personal space
// (8), then the camera wedge (9), then the arc and StayClose's cap (10). So a wedge edge that lies inside personal
// space gives a goal outside personal space even if it is in the wedge; a wall (the ring) behind a station pulls it in
// whatever the arc says; a target beside the lead, where no station is both in the arc and outside personal space,
// gives a goal outside personal space and off the arc. Only when the ring leaves no room outside personal space is a
// goal inside it: then as far from the lead as the ring allows, out of the wedge where that ties.
//   8. Personal space: no goal is within personalSpaceMeters (1.6) of the lead, centre to centre, except while the
//      host says it revives the lead, or when the arena leaves no other room (then as far from the lead as it can get).
//      The cross-field check: leashMeters at least personalSpaceMeters + 1, so StayClose always has room for a goal.
//   9. Camera wedge: no goal is inside the wedge from the camera to wedgeBeyondMeters (1.0) past the lead,
//      wedgeHalfDegrees (25) either side of the camera-to-lead line (the camera on the lead: its forward; neither:
//      no wedge). A goal inside it moves to the nearer wedge edge plus wedgeClearMeters (0.5); every goal stationing
//      emits keeps that clearance unless the arena or personal space leaves none outside it (the order above). A goal outside the arena (arenaCentre, arenaRadius; 0: unbounded) is pulled in.
//  10. Stations, on the circle round the target at its radius + half the reach (in reach), by the angle at the
//      target between the lead and the station: StayClose closeArcMin..Max (60-90) and within closeLeadMeters (3.5,
//      at most leashMeters - 0.5) of the lead; Spread spreadArcMin..Max (120-180), any distance. Of the two sides, the one
//      with a station outside the wedge (and personal space and the arena); both: the one nearer the partner. A side
//      once chosen is kept sideHoldSeconds (2.0) unless the wedge (or 8, or the arena) leaves it no station in its
//      arc. A new target (or none, the follow point) keeps the side and its timer while that side's station for the
//      new target is in its arc, and re-picks only when it is not. The named exception: an order change (StayClose
//      to Spread or back) re-picks at once. No station in the arc: the nearest legal point of the circle on that side; none on the circle: the arc's
//      middle, resolved by 8 and 9 (and if that is out of reach, it waits there and never starts a string).
//      StayClose's leash still rules: no station goal (on the circle, resolved or a follow point) is past that cap,
//      so a lead drifting a little never trips the leash and its return; stationing replaces
//      the old returnMeters clamp on the approach; the leash's own goal is a resolved point on the partner's side
//      of the lead, within max(returnMeters, personal space + 0.5).
//  11. No target: stand followMin..followMax (2.0-2.5) behind the lead (by its facing; zero facing: the camera's
//      line) and to one side, outside the wedge, the side kept as in 10.
//  12. A string starts only within stationTolerance (0.5) of its station (blows are seen from the side, not from
//      behind the lead); out of it the intent is MoveTo the station (Approach). The fallback, so a station it cannot
//      reach (one that runs from it, a spot it is blocked from) never starves it: with its target within reach and not
//      stationed for stationGiveUpSeconds (3.0), it may start a string from where it stands if that spot is outside
//      the camera wedge itself (no clearance) and outside personal space; after stationAnySeconds (6.0), from any spot
//      in reach outside personal space. Out of reach (or inside personal space) once the first has run, it goes to the nearest in-reach point of
//      the target's ring in the arena, outside personal space (and within StayClose's cap). The time counts while it
//      tries to station (Ready or a pause, with a target); being within the tolerance and in reach resets it, as do
//      going Down and rolesSwapped(). During a pause it goes back to the
//      station (StepBack when nearer than standoffMeters to the target's edge, else MoveTo). Later blows of a string
//      are not held to it; guarding, the budget and the rest are unchanged.
// Two seeded streams (rhythm, defence), drawn at events only: the same seed and script give the same draws at any step.
// A step that is negative or not a finite number counts as 0, and one step is at most kMaxPartnerStep (the fight's
// kMaxFightStep), as in FightSession.
// save() and load() (PartnerBrainSave, plain data) carry its whole state for a checkpoint; hash() is its play state.
//
// Data/Battle/partner_brain.json (PartnerTuning; every field but version may be left out):
//   { "version": 1, "leashMeters": 4, "returnMeters": 3, "reachMeters": 1.8, "standoffMeters": 0.8, "blowsMin": 2,
//     "blowsMax": 3, "blowSeconds": 0.65, "pauseMin": 1.2, "pauseMax": 2.0, "guardWarning": 0.25, "guardChance": 0.7,
//     "guardHold": 0.3, "downSeconds": 30, "riseHealth": 0.25, "blowCapShare": 0.6, "overScale": 0.5,
//     "budgetWindowSeconds": 20, "budgetShare": 0.6, "budgetAllowanceSeconds": 2.0, "personalSpaceMeters": 1.6,
//     "wedgeHalfDegrees": 25, "wedgeBeyondMeters": 1.0, "wedgeClearMeters": 0.5, "closeArcMin": 60, "closeArcMax": 90,
//     "closeLeadMeters": 3.5, "spreadArcMin": 120, "spreadArcMax": 180, "sideHoldSeconds": 2.0, "followMin": 2.0,
//     "followMax": 2.5, "stationTolerance": 0.5, "stationGiveUpSeconds": 3.0, "stationAnySeconds": 6.0,
//     "targetHoldSeconds": 1.5 }

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/Explore.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kMaxPartnerEnemies = kMaxArenaEnemies; // views past this are ignored
inline constexpr std::size_t kMaxPartnerBlows = 6;                  // a string's blows at most (blowsMax's bound)
// One step's time at most (seconds): a longer step counts as this. FightSession's kMaxFightStep, the same number.
inline constexpr double kMaxPartnerStep = 0.25;

enum class PartnerOrder : std::uint8_t { StayClose, Spread };
// What the host's ally body is doing this step (its own state, not the brain's intent).
enum class PartnerDoing : std::uint8_t { Idle, Moving, Attacking, Guarding, Hurt };

struct PartnerEnemyView {
    enum class Kind : std::uint8_t { Regular, Boss };
    enum class State : std::uint8_t { Idle, WindingUp, Staggered, Down };
    enum class Aim : std::uint8_t { None, Lead, Partner };
    Vec2 pos{};
    float radius = 0.5f; // reach is measured to its edge
    Kind kind = Kind::Regular;
    State state = State::Idle;
    float windupLeft = 0.f; // WindingUp: seconds until the blow lands
    Aim aim = Aim::None;    // WindingUp: whom the blow is aimed at
    bool leadAttacking = false;
};

// Stationing's view (rules 8-12), on the ground plane, fed by the host each step. Off (the default), or any field not
// a finite number, or arenaRadius under 0, or the partner's or the lead's position not finite: no stationing.
// The target's position is its enemy view's.
struct PartnerStationView {
    bool on = false;
    Vec2 cameraPos{};
    Vec2 cameraForward{};    // need not be unit; the wedge's axis when the camera stands on the lead
    Vec2 leadFacing{};       // need not be unit; zero: the camera-to-lead line (the lead faces away from the camera)
    bool reviving = false;   // the partner is reviving the lead: personal space is off
    Vec2 arenaCentre{};
    float arenaRadius = 0.f; // where the partner may stand (its body's margin taken off); 0: unbounded
};

// One step's view. Enemies by the fight's own index (keep the order stable through the fight).
struct PartnerInput {
    PartnerOrder order = PartnerOrder::Spread;
    Vec2 pos{};
    float health = 1.f; // fraction of max
    bool canAct = true;
    PartnerDoing doing = PartnerDoing::Idle;
    Vec2 leadPos{};
    int leadTarget = -1; // an index into enemies, or -1
    // The lead is attacking this step (rule 2's budget counts its time). A host that never says gets the budget of
    // a lead who never attacks: budgetAllowanceSeconds per window.
    bool leadAttacking = false;
    // Each blow of a string the host would run, by its place in the string: its whole length (seconds). The host
    // keeps to it: a blow it starts always runs out. 0, or a value out of 0.1..2: the tuning's blowSeconds. Read
    // when a string starts.
    std::array<float, kMaxPartnerBlows> blowSeconds{};
    std::span<const PartnerEnemyView> enemies;
    PartnerStationView station{}; // rules 8-12; off by default
    // Rule 2: seconds of partner attack time spent outside its own strings since the last step (a paired counter's
    // 1.0 s on the step it lands). Not finite, or under 0: 0. Above budgetWindowSeconds: that. 0: as before.
    float spentSeconds = 0.f;
};

enum class PartnerAct : std::uint8_t { Wait, MoveTo, Attack, Guard, StepBack };
// Why this intent (for logs and tests).
enum class PartnerWhy : std::uint8_t {
    NotBegun, Down, CannotAct, Guard, Leash, String, Pause, NoTarget, Approach, HeldOff, Start, Budget,
    Committed // a blow it began before its string ended still runs out: no new string yet
};

struct PartnerIntent {
    PartnerAct act = PartnerAct::Wait;
    PartnerWhy why = PartnerWhy::NotBegun;
    Vec2 point{};        // MoveTo, StepBack: where to go
    int enemy = -1;      // Attack: the target; MoveTo (Approach): the enemy it closes on
    int blows = 0;       // Attack: the string's length
    bool newString = false; // Attack: the string's first step (the host starts the light chain here)
    // Attack: start a blow now (the string's blowIndex-th light, of the length the host gave): the only step a blow
    // starts on. The host runs it out whatever comes after; on steps without it, the host starts none.
    bool blowStart = false;
    int blowIndex = -1;
    bool partnerBlow = false; // Attack: always true (rule 7)
    float breakValue = 0.f;   // Attack: always 0 (rules 6, 7)
    bool extendsStagger = false; // Attack: always false (rule 6)
    bool punish = false;      // Attack on a staggered target: no pause after it (rule 6)
    bool rise = false;        // Wait while Down past downSeconds: stand now
    float riseHealth = 0.f;   // with rise: the health to stand at (fraction of max)
    bool stationed = false;   // stationing (rules 8-12) ran this step: point, when set, obeys them
};

struct PartnerTuning {
    float leashMeters = 4.f;    // StayClose: never further from the lead (1..10)
    float returnMeters = 3.f;   // StayClose: a pull back ends within this (0.5..leash)
    float reachMeters = 1.8f;   // strings start within this of the target's edge (0.5..4)
    float standoffMeters = 0.8f; // pause: step back when closer than this to the target's edge (0..reach)
    int blowsMin = 2;           // a string's blows (1..6)
    int blowsMax = 3;           // (blowsMin..6)
    float blowSeconds = 0.65f;  // one light blow when the host gives no length (0.1..2); the default fighter's first light
    float pauseMin = 1.2f;      // seconds after a string (0..10)
    float pauseMax = 2.0f;      // (pauseMin..10)
    float guardWarning = 0.25f; // least windup left to guard (0..2)
    float guardChance = 0.7f;   // (0..1)
    float guardHold = 0.3f;     // guard kept this long after the windup ends (0..2)
    float downSeconds = 30.f;   // Down this long, then rise (1..600)
    float riseHealth = 0.25f;   // (0.01..1)
    float blowCapShare = 0.6f;  // rule 7's share (0..1): see partnerBlowDamage
    float overScale = 0.5f;     // rule 7's scale over it (0..1)
    float budgetWindowSeconds = 20.f;  // rule 2's budget: any rolling window this long (5..30)
    float budgetShare = 0.6f;          // its attack time at most this x the lead's in the window (0..1)
    float budgetAllowanceSeconds = 2.f; // plus this (0..10)
    // Stationing (rules 8-12). Cross-field limits are applied where used: closeLeadMeters (and, StayClose, followMax)
    // at most leashMeters - 0.5, the follow distances at least personal space. Checked: stationTolerance at most half
    // reachMeters (a station is half the reach off the target's edge, so arriving always means in reach).
    float personalSpaceMeters = 1.6f; // rule 8 (0.5..4)
    float wedgeHalfDegrees = 25.f;    // rule 9 (5..60)
    float wedgeBeyondMeters = 1.f;    // rule 9: the wedge runs this far past the lead (0..5)
    float wedgeClearMeters = 0.5f;    // rule 9: a goal moved out keeps this from the edge (0..2)
    float closeArcMin = 60.f;         // rule 10, StayClose, degrees at the target from the lead (0..180)
    float closeArcMax = 90.f;         // (closeArcMin..180)
    float closeLeadMeters = 3.5f;     // rule 10, StayClose: a station within this of the lead (1..10)
    float spreadArcMin = 120.f;       // rule 10, Spread (0..180)
    float spreadArcMax = 180.f;       // (spreadArcMin..180)
    float sideHoldSeconds = 2.f;      // rule 10: a side is kept at least this long (0..10)
    float followMin = 2.f;            // rule 11, no target: behind the lead (0.5..10)
    float followMax = 2.5f;           // (followMin..10)
    float stationTolerance = 0.5f;    // rule 12: a string starts within this of its station (0.05..2, <= reach/2)
    float stationGiveUpSeconds = 3.f; // rule 12's fallback: not stationed this long, from where it stands (0.5..30)
    float stationAnySeconds = 6.f;    // rule 12's fallback: and this long, from anywhere outside personal space
                                      // (stationGiveUpSeconds..60)
    float targetHoldSeconds = 1.5f;   // rule 1, Spread: a chosen target is kept at least this long (0..10)
};

FTCORE_API bool validPartnerTuning(const PartnerTuning& t);
// Reads Data/Battle/partner_brain.json. A field out of its range, of the wrong type or unknown, a bad version or a
// file that does not parse: `out` is the defaults and false (`error` says why). A cross-field pair broken (returnMeters
// over leashMeters, leashMeters under personalSpaceMeters + 1, standoffMeters over reachMeters, stationTolerance over
// half reachMeters, each min over its max: blows, pause, both arcs, follow, stationGiveUpSeconds over
// stationAnySeconds): both fields of that pair take their defaults, every other field is kept, it is reported in
// `notes`, and the import succeeds.
FTCORE_API bool importPartnerBrain(std::string_view json, PartnerTuning& out, std::string* error = nullptr,
                                   std::vector<std::string>* notes = nullptr);

// Rule 7, and its only home: a partner blow's hp damage from its raw damage and the lead's light damage (the part
// over 60% of the lead's light is halved, with the default tuning).
inline float partnerBlowDamage(float raw, float leadLight, const PartnerTuning& t = PartnerTuning{}) {
    const float cap = t.blowCapShare * leadLight;
    return raw <= cap ? raw : cap + (raw - cap) * t.overScale;
}

// The brain's whole state, plain data (a checkpoint keeps it; PartnerBrain::save and load).
struct PartnerBrainSave {
    enum class Phase : std::uint8_t { Ready, String, Pause };
    struct Watch {
        bool winding = false; // was WindingUp at Partner last step
        float left = 0.f;
        bool guard = false;   // this windup's draw said guard
        double hold = 0.0;    // guard kept after it ends
    };
    static constexpr double kSlot = 0.05;          // the budget's slot (s)
    static constexpr std::size_t kSlots = 604;     // 30 s of slots, and room either side

    PartnerTuning t{};
    std::uint32_t rhythm = 1;
    std::uint32_t defence = 1;
    std::uint32_t rhythmDraws = 0;
    std::uint32_t defenceDraws = 0;
    Phase phase = Phase::Ready;
    int target = -1;
    int blows = 0;
    double pause = 0.0;
    double clock = 0.0;      // in the current pause
    bool returning = false;
    bool drawn = false;      // blows and pause drawn for a string the budget holds
    std::array<double, kMaxPartnerBlows> lengths{}; // the string's blows, as the host gave them when it started
    int begunBlows = 0;      // of the string's blows, those begun
    double committedEnd = 0.0; // on its own clock (now): when the last blow begun runs out
    // Rule 2's budget: attack seconds per slot (the slot `slot % kSlots` is the current one), both sides.
    std::array<float, kSlots> spentPartner{};
    std::array<float, kSlots> spentLead{};
    std::int64_t slot = 0;
    double now = 0.0;
    bool down = false;
    double downFor = 0.0;
    std::array<Watch, kMaxPartnerEnemies> watch{};
    PartnerIntent out{};
    bool begun = false;
    // Stationing's side (rule 10, 11): -1 or +1 (0: none chosen), what it was chosen for (the target and the order,
    // or no target), and how long it has been held.
    std::int8_t side = 0;
    int sideKey = 0;
    double sideFor = 0.0;
    std::uint32_t swaps = 0; // rolesSwapped() calls
    int sideTarget = -1;     // the target the side was last used for (-1: none, the follow point)
    double offStation = 0.0; // rule 12's fallback: seconds trying to station since it last was stationed
    double targetFor = 0.0;  // rule 1: how long the current target has been held
    bool targetReached = false; // the current target has been within reach since it was chosen

    // FNV-1a over every field (StateHash.h): two brains with the same hash do the same from here.
    FTCORE_API std::uint64_t hash() const;
};

class FTCORE_API PartnerBrain {
public:
    // False (and the defaults) for bad tuning. seed 0 is taken as 1.
    bool begin(const PartnerTuning& t, std::uint32_t seed);
    const PartnerIntent& step(double dt, const PartnerInput& in);
    // The body it drives changed (the host calls it on a party switch, tick.switched, and on a takeover). Everything
    // about the old body goes: the string (its blows, the one still running, a string drawn and held), the pause,
    // the target, the return, the chosen station side, the windups it watched and being down. The budget ring stays
    // (the budget is about the pair, not the body), and so do the clock and the seeds. The next step decides afresh
    // from the new body's position: with a target in reach it starts a string (or moves to its station) at once.
    void rolesSwapped();
    std::uint32_t swaps() const { return s_.swaps; }

    const PartnerIntent& intent() const { return s_.out; }
    bool down() const { return s_.down; }
    double downFor() const { return s_.downFor; }
    bool stringing() const { return s_.phase == Phase::String; }
    bool pausing() const { return s_.phase == Phase::Pause; }
    // Rule 2's budget: whether `seconds` more of its attack, from now, would keep every window within it.
    bool budgetFits(double seconds) const;
    // A string drawn and held for the budget, not yet started (its draws are made).
    bool drawnAhead() const { return s_.drawn; }
    int target() const { return s_.target; }
    // Draw counts (tests: draws happen at events only).
    std::uint32_t rhythmDraws() const { return s_.rhythmDraws; }
    std::uint32_t defenceDraws() const { return s_.defenceDraws; }
    int lastBlows() const { return s_.blows; }
    double lastPause() const { return s_.pause; }
    // Seconds left of the blows it has begun (0: none running).
    double committedLeft() const { return s_.committedEnd > s_.now ? s_.committedEnd - s_.now : 0.0; }

    // Its whole state, for a checkpoint: load(save()) gives a brain that does exactly the same from there.
    const PartnerBrainSave& save() const { return s_; }
    void load(const PartnerBrainSave& state) { s_ = state; }
    std::uint64_t hash() const { return s_.hash(); }

private:
    using Phase = PartnerBrainSave::Phase;
    using Watch = PartnerBrainSave::Watch;
    static constexpr double kSlot = PartnerBrainSave::kSlot;
    static constexpr std::size_t kSlots = PartnerBrainSave::kSlots;
    float draw(std::uint32_t& s, std::uint32_t& n);
    const PartnerIntent& decide(double dt, const PartnerInput& in);
    void account(double dt, double partner, bool lead);
    void spend(float seconds);
    void endString(bool staggered);
    int chooseTarget(const PartnerInput& in) const;

    PartnerBrainSave s_{};
};

} // namespace ftcore
