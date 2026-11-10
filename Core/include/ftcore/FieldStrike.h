#pragma once

// The lead's weapon out in the field (the explore run): the same kit as the arena (ArenaFighter from
// Data/Battle/arena.json) without the fight around it. No enemies' brains, guard, dodge, stamina, Load or lock: the
// player presses light or heavy, the attack runs its startup, active and recovery, and while it is active it connects
// with the targets the host lists (a target dummy) by the arena's own hit test. The host moves the body; the facing
// it reports when an attack starts is held until that attack ends, unless a target is in reach: then the attack is
// aimed. As an attack starts (and on each frame of its startup) it picks the nearest live target within the attack's
// range + that target's radius, in any direction, turns the facing straight at it and aims at its hit point: the near
// side of its body toward the lead, at its chest height. With none, the aim is straight ahead at the attack's range and
// the lead's own chest height. The host turns the body and points the arm, shot and effects at aim().
//
// Its output is the arena's player output and events (AttackStarted, Hit), so the host's weapon wrapper (clips, props,
// effects) is fed exactly as in a fight. Positions in metres on the ground plane. Fixed capacity: nothing allocates.

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"

#include <cstddef>
#include <cstdint>

namespace ftcore {

inline constexpr std::size_t kMaxFieldTargets = 8;

// The chest height (m above the ground) an aim takes when the host gives none.
inline constexpr float kFieldChestHeight = 1.3f;

// Something an attack can hit: its centre and body radius (m), and the height of its chest above the ground (m), where
// blows are aimed. A target that is not alive is skipped.
struct FieldTarget {
    Vec2 at;
    float radius = 0.4f;
    bool alive = true;
    float chest = kFieldChestHeight;
};

// Where the attack under way is aimed: the hit point on the ground plane and its height (m), and the target's index in
// the host's list (aimed false: none in reach, the point straight ahead).
struct FieldAim {
    bool aimed = false;
    std::uint8_t target = 0xFF;
    Vec2 at;
    float height = kFieldChestHeight;
};

class FTCORE_API FieldStrike {
public:
    // Takes the kit and stops any attack. The clock goes back to 0.
    void begin(const ArenaFighter& kit);

    // One host frame of dt seconds. light / heavy: pressed this frame (heavy wins). A press while an attack can't
    // start yet is kept for the kit's bufferTime and starts as soon as it can (from the cancel share of the
    // recovery on, as in the arena). The light chain goes on when the next light starts during the recovery or
    // within chainGap after it, and wraps after lightCount. Hits are tested on each frame that ends in the active
    // part: each target once per attack, at most maxTargets of them (0: all), nearest first.
    const ArenaOutput& tick(float dt, bool light, bool heavy, Vec2 pos, Vec2 facing, const FieldTarget* targets,
                            std::size_t targetCount);

    const ArenaOutput& output() const { return out_; }
    // The aim of the attack under way (or the last one); straight ahead when none has started.
    const FieldAim& aim() const { return aim_; }
    // The lead's own chest height (m) for an aim with no target. Default kFieldChestHeight.
    void setChestHeight(float height) { chest_ = height; }
    float time() const { return time_; }
    bool attacking() const { return attack_ != nullptr; }
    // Hits landed and their damage since begin.
    int hits() const { return hits_; }
    int damageDealt() const { return damage_; }

private:
    bool start(std::uint8_t move, Vec2 facing);
    // Picks the target and turns facing_ at it (or aims straight ahead).
    void takeAim(Vec2 pos, const FieldTarget* targets, std::size_t n);
    void push(ArenaEventType type, std::uint8_t target, int amount, Vec2 at);

    ArenaFighter kit_{};
    ArenaOutput out_{};
    const ArenaAttack* attack_ = nullptr;
    std::uint8_t move_ = 0;
    float actionTime_ = 0.f;
    float time_ = 0.f;
    float lastLightEnd_ = -1e9f; // when the last light ended (time), for chainGap
    std::uint8_t lastLight_ = 0;
    bool lastWasLight_ = false;
    std::uint8_t buffered_ = 0xFF; // the buffered move, 0xFF: none
    float bufferedAt_ = 0.f;
    Vec2 facing_{1.f, 0.f};
    FieldAim aim_{};
    float chest_ = kFieldChestHeight;
    bool hit_[kMaxFieldTargets] = {};
    std::uint8_t hitCount_ = 0;
    int hits_ = 0;
    int damage_ = 0;
};

// The arena's hit test for one attack: whether a target at `at` with body radius `radius` is reached by `attack`
// thrown from `from` facing `facing` (unit). An arc within range + radius and arcCos; a lane (width > 0) ahead within
// range + radius and width / 2 + radius off the line.
FTCORE_API bool fieldStrikeReaches(const ArenaAttack& attack, Vec2 from, Vec2 facing, Vec2 at, float radius);

// The hit point on a body at `at` with body radius `radius` for a blow from `from`: the point of its outline nearest
// `from` (at itself when `from` is inside it). The arena host aims at its locked enemy with it too.
FTCORE_API Vec2 fieldHitPoint(Vec2 from, Vec2 at, float radius);

} // namespace ftcore
