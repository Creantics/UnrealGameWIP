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
//      is chosen when a string starts and kept to its end.
//   2. Rhythm. A light string of blowsMin..blowsMax blows, then a pause of pauseMin..pauseMax s, both drawn (seeded)
//      when the string starts. The host's `doing` is the string's clock: the string is over on the first step the host
//      no longer reports Attacking after it did (blows x blowSeconds when the host never reports it, 2 x that when it
//      never stops). It never starts a string while its target winds up a blow aimed at it.
//      Budget (a partner never out-fights the player, and a lead pinned down defending does not freeze it): over any
//      rolling budgetWindowSeconds (20) window, its attack time is at most budgetShare (0.60) x the lead's attack
//      time in that window plus budgetAllowanceSeconds (2.0). The host says each step whether the lead is attacking
//      (leadAttacking); the partner's own attack time is every step its intent is Attack or the host reports
//      Attacking. A string starts only if its drawn blows x blowSeconds fit every window that would hold any of it
//      with the lead attacking no more, so the rule holds whatever the lead does next. When it does not fit, the
//      intent is Wait (why Budget) on its target, in reach and facing it; it guards as rule 3 says; the string,
//      already drawn, starts on the first step it fits (no new draw). Mid-string, a step that would break the
//      budget ends the string (Budget; its pause follows, or none on a staggered target). The budget is kept in
//      0.05 s slots, counted against it: the partner's part slot in, the lead's out.
//   3. Defence. When a windup aimed at the partner is first seen (a new one: the enemy entered WindingUp, or its
//      windupLeft rose) with at least guardWarning s left, one seeded draw: guard with guardChance, else take the blow.
//      It guards from then until guardHold s after the windup ends. With less warning it is hit. It never parries,
//      ripostes or dodges. A guard begun mid-string ends that string (its pause runs from there).
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
// Two seeded streams (rhythm, defence), drawn at events only: the same seed and script give the same draws at any step.
//
// Data/Battle/partner_brain.json (PartnerTuning; every field but version may be left out):
//   { "version": 1, "leashMeters": 4, "returnMeters": 3, "reachMeters": 1.8, "standoffMeters": 0.8, "blowsMin": 2,
//     "blowsMax": 3, "blowSeconds": 0.65, "pauseMin": 1.2, "pauseMax": 2.0, "guardWarning": 0.25, "guardChance": 0.7,
//     "guardHold": 0.3, "downSeconds": 30, "riseHealth": 0.25, "blowCapShare": 0.6, "overScale": 0.5,
//     "budgetWindowSeconds": 20, "budgetShare": 0.6, "budgetAllowanceSeconds": 2.0 }

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/Explore.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ftcore {

inline constexpr std::size_t kMaxPartnerEnemies = kMaxArenaEnemies; // views past this are ignored

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
    std::span<const PartnerEnemyView> enemies;
};

enum class PartnerAct : std::uint8_t { Wait, MoveTo, Attack, Guard, StepBack };
// Why this intent (for logs and tests).
enum class PartnerWhy : std::uint8_t {
    NotBegun, Down, CannotAct, Guard, Leash, String, Pause, NoTarget, Approach, HeldOff, Start, Budget
};

struct PartnerIntent {
    PartnerAct act = PartnerAct::Wait;
    PartnerWhy why = PartnerWhy::NotBegun;
    Vec2 point{};        // MoveTo, StepBack: where to go
    int enemy = -1;      // Attack: the target; MoveTo (Approach): the enemy it closes on
    int blows = 0;       // Attack: the string's length
    bool newString = false; // Attack: the string's first step (the host starts the light chain here)
    bool partnerBlow = false; // Attack: always true (rule 7)
    float breakValue = 0.f;   // Attack: always 0 (rules 6, 7)
    bool extendsStagger = false; // Attack: always false (rule 6)
    bool punish = false;      // Attack on a staggered target: no pause after it (rule 6)
    bool rise = false;        // Wait while Down past downSeconds: stand now
    float riseHealth = 0.f;   // with rise: the health to stand at (fraction of max)
};

struct PartnerTuning {
    float leashMeters = 4.f;    // StayClose: never further from the lead (1..10)
    float returnMeters = 3.f;   // StayClose: a pull back ends within this (0.5..leash)
    float reachMeters = 1.8f;   // strings start within this of the target's edge (0.5..4)
    float standoffMeters = 0.8f; // pause: step back when closer than this to the target's edge (0..reach)
    int blowsMin = 2;           // a string's blows (1..6)
    int blowsMax = 3;           // (blowsMin..6)
    float blowSeconds = 0.65f;  // one light blow, the fallback clock (0.1..2); the default fighter's first light
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
};

FTCORE_API bool validPartnerTuning(const PartnerTuning& t);
// Reads Data/Battle/partner_brain.json; on any error `out` is the defaults and false.
FTCORE_API bool importPartnerBrain(std::string_view json, PartnerTuning& out, std::string* error = nullptr);

// Rule 7, and its only home: a partner blow's hp damage from its raw damage and the lead's light damage (the part
// over 60% of the lead's light is halved, with the default tuning).
inline float partnerBlowDamage(float raw, float leadLight, const PartnerTuning& t = PartnerTuning{}) {
    const float cap = t.blowCapShare * leadLight;
    return raw <= cap ? raw : cap + (raw - cap) * t.overScale;
}

class FTCORE_API PartnerBrain {
public:
    // False (and the defaults) for bad tuning. seed 0 is taken as 1.
    bool begin(const PartnerTuning& t, std::uint32_t seed);
    const PartnerIntent& step(double dt, const PartnerInput& in);

    const PartnerIntent& intent() const { return out_; }
    bool down() const { return down_; }
    double downFor() const { return downFor_; }
    bool stringing() const { return phase_ == Phase::String; }
    bool pausing() const { return phase_ == Phase::Pause; }
    // Rule 2's budget: whether `seconds` more of its attack, from now, would keep every window within it.
    bool budgetFits(double seconds) const;
    // A string drawn and held for the budget, not yet started (its draws are made).
    bool drawnAhead() const { return drawn_; }
    int target() const { return target_; }
    // Draw counts (tests: draws happen at events only).
    std::uint32_t rhythmDraws() const { return rhythmDraws_; }
    std::uint32_t defenceDraws() const { return defenceDraws_; }
    int lastBlows() const { return blows_; }
    double lastPause() const { return pause_; }

private:
    enum class Phase : std::uint8_t { Ready, String, Pause };
    struct Watch {
        bool winding = false; // was WindingUp at Partner last step
        float left = 0.f;
        bool guard = false;   // this windup's draw said guard
        double hold = 0.0;    // guard kept after it ends
    };
    static constexpr double kSlot = 0.05;          // the budget's slot (s)
    static constexpr std::size_t kSlots = 604;     // 30 s of slots, and room either side
    float draw(std::uint32_t& s, std::uint32_t& n);
    const PartnerIntent& decide(double dt, const PartnerInput& in);
    void account(double dt, bool partner, bool lead);
    void endString(bool staggered);
    int chooseTarget(const PartnerInput& in) const;

    PartnerTuning t_{};
    std::uint32_t rhythm_ = 1;
    std::uint32_t defence_ = 1;
    std::uint32_t rhythmDraws_ = 0;
    std::uint32_t defenceDraws_ = 0;
    Phase phase_ = Phase::Ready;
    int target_ = -1;
    int blows_ = 0;
    double pause_ = 0.0;
    double clock_ = 0.0; // in the current string or pause
    bool seenAttacking_ = false;
    bool returning_ = false;
    bool drawn_ = false;     // blows_ and pause_ drawn for a string the budget holds
    // Rule 2's budget: attack seconds per slot (the slot `slot_ % kSlots` is the current one), both sides.
    std::array<float, kSlots> spentPartner_{};
    std::array<float, kSlots> spentLead_{};
    std::int64_t slot_ = 0;
    double now_ = 0.0;
    bool down_ = false;
    double downFor_ = 0.0;
    std::array<Watch, kMaxPartnerEnemies> watch_{};
    PartnerIntent out_{};
    bool begun_ = false;
};

} // namespace ftcore
