#pragma once

// Each lead's special move: a hidden gauge that fills in play, and, once it is full, one press that runs the move as a
// cinematic with the rest of the fight frozen in time. ArenaFight runs it (ArenaFight::setSpecials, ArenaInput::special,
// ArenaFight::special()); this file holds the data, the gauge store the host keeps between fights, and the plain state.
//
// The gauge. One per lead, never shown: the HUD reads only whether it is full. It fills from the lead's own play, by
// ArenaSpecialGaugeRules: per hp it deals, per parry, per block and per hp it takes; it never decays and stops at full.
// It is kept between fights within a session of play (LeadSpecialGauges, beside the party's other state): the host
// gives a fight each lead's value (ArenaFight::setSpecialGauges) and reads them back when it ends. A lead's gauge
// reaching full (or arriving full: the first tick after setSpecials or a restart) raises ArenaEventType::SpecialReady
// once (the prompt; the voice line unless that lead's line played under `readyLineGap` ago on the host's clock, which
// the store carries between fights: LeadSpecialGauges::clock and Lead::lineAt); using the move empties it. No Ready or
// Unready is said on the tick the fight ends. A lead switch takes `switchLoss` of the value off the gauges
// `switchLossAppliesTo` names; a gauge that drops below full, or whose lead is no longer the one controlled, raises
// SpecialUnready (the prompt goes) and says Ready again when it is full and shown again.
//
// The move (ArenaSpecialRules, one per lead, data: Data/Battle/arena_special.json, neutral ids). Only the player fires
// it, from the prompt (the partner's brain never does): the controlled lead's own move, or the move of a party member
// who is never played (the partner body when its kit is not playable). A partner lead's move has no input path: its
// gauge fills, hidden, and its prompt shows once it is the controlled lead. It needs a living enemy, the player's target
// (its lock, else its attack's soft target, else the nearest living enemy in front of it), in the performer's line of
// sight (the host's to say) and within `range` of the performer; a locked enemy is the target at any range (the
// cinematic covers the distance). An enemy the fight holds unhurt (a scripted step back) shows the hit and loses
// nothing; a press whose target is unhurt with no hurtable enemy in the lane is refused (Unhurt), the gauge kept. A
// target gone by the impact passes the hit to the nearest living enemy in the lane, else nobody. From the press to
// `duration`:
//   * Frozen: the fight's time does not run, so nothing in it moves or counts down: no enemy, player, partner, brain, token,
//     cooldown, hit pause or status timer. The host holds its own (projectiles, hazards, weather) while
//     ArenaFight::frozen() is true, and poses the performer from the cinematic. The performer is out of reach.
//   * At `impactAt` the results are ruled: the target is always hit for `damage`; every other living enemy whose body
//     reaches into the lane (`laneLength` x `laneWidth` from the performer toward the target) for damage x `otherShare`.
//     A boss loses `bossPhaseShare` of its current break phase's hp span instead, never a kill from full hp. The
//     results are reported then (ArenaSpecialView::results, the Impact cue) and land at the cut back: hp, then a stagger
//     and `knockBack` metres away from the performer over `knockTime` for every enemy that is not a boss. Weak points
//     and the host's blow hook play no part: the hit is guaranteed.
//   * The cue timeline (`cues`): named beats at set times for the camera, effects and sound; each fires once, in order.
//   * `skippable`: once a lead's move has been seen to its end once (LeadSpecialGauges::seen), a skip input jumps to
//     the cut back; the results are the same.
//
// Nothing here allocates except importArenaSpecials (load time). Implemented in ArenaFight.cpp.

#include "ftcore/Api.h"
#include "ftcore/Explore.h" // Vec2

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kMaxArenaSpecialId = 31;
inline constexpr std::size_t kMaxArenaSpecials = 8;
inline constexpr std::size_t kMaxArenaSpecialCues = 24;
inline constexpr std::size_t kMaxArenaSpecialTargets = 8; // >= kMaxArenaEnemies
inline constexpr std::size_t kMaxSpecialLeads = 4;
inline constexpr std::uint8_t kArenaSpecialMove = 13;     // the move id on the special's Hit events (after kArenaAirMove)
inline constexpr std::uint8_t kArenaSpecialNone = 0xFF;   // no move (an index into ArenaSpecialTable::moves)
inline constexpr float kArenaSpecialNoLine = -1.0e9f;     // no ready line played yet (LeadSpecialGauges::Lead::lineAt)

struct ArenaSpecialCue {
    float at = 0.f;
    std::array<char, kMaxArenaSpecialId + 1> name{};
    std::string_view view() const { return std::string_view(name.data()); }
};

struct ArenaSpecialRules {
    std::array<char, kMaxArenaSpecialId + 1> id{};
    std::array<char, kMaxArenaSpecialId + 1> lead{}; // whose move it is: the id the host gives setSpecials (a lead's,
                                                     // or a party member's current identity id)
    float duration = 8.f;       // s, press to cut back
    float impactAt = 5.6f;      // s: the results are ruled
    float range = 30.f;         // m: the farthest target it may start on (body centre)
    float laneLength = 14.f;    // m, from the performer toward the target
    float laneWidth = 4.f;      // m, whole
    int damage = 220;           // the target's hp (an enemy that is not a boss)
    float otherShare = 0.5f;    // of damage, on every other enemy in the lane
    float bossPhaseShare = 0.2f; // of a boss's current break phase's hp span (x otherShare when not the target)
    float knockBack = 2.f;      // m, away from the performer, at the cut back (not a boss)
    float knockTime = 0.35f;    // s
    bool skippable = true;      // once seen
    std::array<ArenaSpecialCue, kMaxArenaSpecialCues> cues{};
    std::uint8_t cueCount = 0;

    std::string_view idView() const { return std::string_view(id.data()); }
    std::string_view leadView() const { return std::string_view(lead.data()); }
};

// Whose gauge a lead switch lowers: both leads', the one handing over, or the one taking over.
enum class ArenaSpecialSwitchLoss : std::uint8_t { Both, Outgoing, Incoming };

struct ArenaSpecialGaugeRules {
    float full = 1000.f;
    float perDamageDealt = 1.f; // per hp the lead's blows take
    float perParry = 25.f;
    float perBlock = 8.f;       // the player lead's guard stopping a blow, or the partner's
    float perDamageTaken = 1.5f; // per hp the lead loses
    float switchLoss = 0.25f;   // of the current value, off the gauges switchLossAppliesTo names, on a lead switch
    ArenaSpecialSwitchLoss switchLossAppliesTo = ArenaSpecialSwitchLoss::Both;
    float readyLineGap = 60.f;  // s of the host's clock (LeadSpecialGauges::clock): a lead's ready line at most once per this
};

struct ArenaSpecialTable {
    ArenaSpecialGaugeRules gauge;
    std::array<ArenaSpecialRules, kMaxArenaSpecials> moves{};
    std::size_t count = 0;
    FTCORE_API const ArenaSpecialRules* find(std::string_view id) const;
    FTCORE_API const ArenaSpecialRules* forLead(std::string_view leadId) const; // its move, or nullptr
};

// Whether the move's numbers are in range (the loader's rules): duration 0.5..30, impactAt in (0, duration], range
// 1..200, lane 0.5..100 by 0.5..50, damage 1..100000, otherShare and bossPhaseShare 0..1, knockBack 0..20, knockTime
// 0.05..5, cues in rising time inside [0, duration] with names of 1..31 letters, digits, _ and -.
FTCORE_API bool validArenaSpecialRules(const ArenaSpecialRules& r);
FTCORE_API bool validArenaSpecialGauge(const ArenaSpecialGaugeRules& g);

// Reads Data/Battle/arena_special.json:
//   { "version": 1,
//     "gauge": { "full": 1000, "perDamageDealt": 1, "perParry": 25, "perBlock": 8, "perDamageTaken": 1.5 },
//     "moves": [ { "id": "...", "lead": "...", "duration": 8, "impactAt": 5.6, ..., "cues": [ { "at": 0, "cue": "..." } ] } ] }
// Every move key is optional but id and lead (ArenaSpecialRules' defaults); ids unique, one move per lead. An unknown
// key is a warning; anything else wrong is an error with its path, and `out` is left empty.
FTCORE_API bool importArenaSpecials(std::string_view json, ArenaSpecialTable& out, std::string* error = nullptr,
                                    std::vector<std::string>* warnings = nullptr);

// The gauges the host keeps between fights (beside the party's state): by lead id. A party member who changes identity
// in the story is keyed by its current identity id (neutral, Data/), so each identity may have its own move and lines;
// the host carries the gauge to the new id when the identity changes (carry: one body, one gauge). Plain data.
// `clock` is the host's play clock (s) as it hands the store to setSpecials; the fight runs it on by every tick it is
// given (frozen ones too, never back on a restart) and storeSpecialGauges writes it back, so a host with no clock of its
// own just keeps the store. Each lead's last ready line is on that clock, so the line's gap spans fights.
struct LeadSpecialGauges {
    struct Lead {
        std::array<char, kMaxArenaSpecialId + 1> id{};
        float value = 0.f;
        bool seen = false;               // its move has run to its end once (skippable from then)
        float lineAt = kArenaSpecialNoLine; // `clock` when its ready line last played
    };
    std::array<Lead, kMaxSpecialLeads> leads{};
    std::size_t count = 0;
    float clock = 0.f;
    // The lead's entry, added (at 0) when there is room; nullptr when full.
    FTCORE_API Lead* entry(std::string_view leadId);
    FTCORE_API const Lead* find(std::string_view leadId) const;
    // An identity change: `to` takes `from`'s gauge value and `from` empties; `to` keeps its own seen (its move is its
    // own cinematic). False (nothing changed) when an id is empty or the same, or there is no room for `to`.
    FTCORE_API bool carry(std::string_view from, std::string_view to);
};

// Why a special would not start now (ArenaFight::specialRefusal).
enum class ArenaSpecialRefusal : std::uint8_t {
    Ok,
    NoMove,   // that lead has no move (or no such lead in the fight)
    NotControlled, // the partner is a lead: only the controlled lead's move may be fired (no input path for it)
    NotReady, // its gauge isn't full
    NoTarget, // no living enemy targeted, or it is out of range
    NoSight,  // the host says the target is out of its line of sight
    Down,     // the lead can't act: down, in hitstun, guard-broken, in the air (the player) or hurt (the partner)
    Running,  // a special is running
    Over,     // the fight isn't running
    Unhurt,   // the target is held unhurt (a scripted step back) and no hurtable enemy is in the lane
};
FTCORE_API const char* arenaSpecialRefusalName(ArenaSpecialRefusal r);

struct ArenaSpecialResult {
    std::uint8_t foe = 0;
    int damage = 0;      // ruled at impact (before an hp floor: the hp it loses at the cut back may be less); 0 for
                         // an enemy held unhurt (it shows the hit, loses nothing, is neither staggered nor shoved)
    bool target = false;
    bool boss = false;
};

// What ArenaFight::special() shows. While nothing runs only the ready flags and refusals mean anything.
struct ArenaSpecialView {
    bool running = false;
    std::uint8_t move = kArenaSpecialNone;    // the running move: its index in the host's table (ArenaFight::specialMove)
    std::uint8_t performer = 0xFF;            // kArenaPlayer or kArenaAlly
    std::uint8_t slot = 0;                    // the performer's gauge slot (ArenaFight::setSpecialGauges)
    std::uint8_t target = 0xFF;               // the enemy hit in full (a target gone by the impact: the lane's nearest)
    Vec2 origin;                              // the performer's spot at the press
    Vec2 dir{1.f, 0.f};                       // toward the target at the press
    float time = 0.f;                         // s since the press
    float duration = 0.f;
    bool impactDone = false;
    bool skippable = false;                   // a skip input would cut to the end now
    std::uint32_t uses = 0;                   // specials begun since begin() (both leads)
    std::array<std::uint8_t, kMaxArenaSpecialCues> cuesFired{}; // this tick: indices into move->cues, in order
    std::uint8_t cueCount = 0;
    std::array<ArenaSpecialResult, kMaxArenaSpecialTargets> results{}; // from impact on
    std::uint8_t resultCount = 0;
    bool endedThisTick = false;               // the cut back: the results landed this tick
    bool readyPlayer = false;                 // the player lead's gauge is full (the prompt)
    bool readyPartner = false;                // the partner's
    ArenaSpecialRefusal refusalPlayer = ArenaSpecialRefusal::NoMove; // as of the last tick's end
    ArenaSpecialRefusal refusalPartner = ArenaSpecialRefusal::NoMove;
};

} // namespace ftcore
