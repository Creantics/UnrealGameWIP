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
// The body's own moves come first: while it vaults, climbs, hangs, rolls or slides (setBody) no attack starts, presses
// are dropped rather than kept, and an attack under way is cut off as the move begins. The hold lasts on for the
// hand-back (setHandBack, default kFieldHandBack) after the move ends, while the move's pose fades back into
// locomotion. Falling is no move of its own: attacks run while falling, unless a move or its hand-back is under way.
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

// Something an attack can hit: its centre and body radius (m), the height of its chest above the ground (m), where
// blows are aimed, and a stable id the host keeps for it while it exists (each target hit once per attack is keyed by
// it, so the list may change order or shrink mid-attack; 0: keyed by its place in the list). A target that is not
// alive, or whose numbers are not finite, is skipped. Of a longer list only the nearest kMaxFieldTargets count; only the
// first 255 entries are looked at.
struct FieldTarget {
    Vec2 at;
    float radius = 0.4f;
    bool alive = true;
    float chest = kFieldChestHeight;
    std::uint32_t id = 0;
};

// How far (m) beyond a target's body the reticle point may lie and still pick it as the aim's target.
inline constexpr float kFieldReticleSlack = 0.3f;

// How long attacks stay blocked after a move of the body's own ends (s): the time its pose takes to fade back.
inline constexpr float kFieldHandBack = 0.25f;

// What the body is doing besides fighting. Free and Falling allow attacks; the rest block them (see setBody).
enum class FieldBody : std::uint8_t { Free, Traversing, Sliding, Rolling, Hanging, Falling, Seated }; // Seated: in a vehicle or boarding it

// Whether `body` is a move of the body's own that blocks attacks.
inline constexpr bool fieldBodyBusy(FieldBody body) { return body != FieldBody::Free && body != FieldBody::Falling; }

// Where the attack under way is aimed: the hit point on the ground plane and its height (m), and the target's index in
// the host's list this frame and its id (aimed false: none in reach, the point straight ahead).
struct FieldAim {
    bool aimed = false;
    std::uint8_t target = 0xFF;
    std::uint32_t id = 0;
    Vec2 at;
    float height = kFieldChestHeight;
};

class FTCORE_API FieldStrike {
public:
    // Takes the kit and stops any attack. The clock goes back to 0.
    void begin(const ArenaFighter& kit);

    // One host frame of dt seconds. light / heavy: pressed this frame (heavy wins; both dropped for a kit with no
    // attacks of its own, ArenaFighter::attacks). A press while an attack can't
    // start yet is kept for the kit's bufferTime and starts as soon as it can (from the cancel share of the
    // recovery on, as in the arena). The light chain goes on when the next light starts during the recovery or
    // within chainGap after it, and wraps after lightCount. Hits are tested on each frame that ends in the active
    // part, and on a frame that passes over it whole (a long frame): each target once per attack, at most maxTargets of
    // them (0: all), nearest first. Hit events carry the target's index in this frame's list.
    const ArenaOutput& tick(float dt, bool light, bool heavy, Vec2 pos, Vec2 facing, const FieldTarget* targets,
                            std::size_t targetCount);

    // What the body does now (set before tick). While it is busy (fieldBodyBusy) and for the hand-back after, presses
    // are dropped, the buffer is cleared and an attack under way stops at once (the output goes Idle). The hand-back
    // runs on the ticks after the move ends; the tick on which it runs out is still held.
    void setBody(FieldBody body) {
        body_ = body;
        if (fieldBodyBusy(body)) handBackLeft_ = handBack_;
    }
    FieldBody body() const { return body_; }
    // Whether an attack may start on the next tick (the body free or falling, no hand-back left).
    bool canAttack() const { return !fieldBodyBusy(body_) && handBackLeft_ <= 0.f; }
    // The hand-back time (s, at least 0). Default kFieldHandBack.
    void setHandBack(float seconds) { handBack_ = seconds > 0.f ? seconds : 0.f; }

    const ArenaOutput& output() const { return out_; }
    // The aim of the attack under way (or the last one); straight ahead when none has started.
    const FieldAim& aim() const { return aim_; }
    // The aim's reticle (AimStance): while on, an attack faces the reticle point `at` (m on the ground plane) and aims
    // at it, `height` m above the lead's feet, instead of at the nearest target; the hit test is the attack's own along
    // that facing. A target whose body (radius + kFieldReticleSlack) holds the point is the aim's target. With
    // lungeScale > 0 the attack lunges toward the point: lungeScale times its own lunge, spread over startup and active
    // (output().player.velocity, for the host to move the body by), ending lungeStop times its range short of the point.
    void setReticle(bool on, Vec2 at, float height, float lungeScale = 0.f, float lungeStop = 0.6f) {
        reticleOn_ = on;
        reticleAt_ = at;
        reticleHeight_ = height;
        lungeScale_ = lungeScale > 0.f ? lungeScale : 0.f;
        lungeStop_ = lungeStop > 0.f ? lungeStop : 0.f;
    }
    bool reticleOn() const { return reticleOn_; }
    // Attacking on the move (AttackMove): on the next tick an attack under way that has no target or reticle to face
    // (or is aimed at a target but past its active part) turns toward `want` (the body's move or the camera's way, on
    // the ground) by at most rate rad/s, and its hit test goes with it. Rate 0 (the default): its facing holds as it
    // started.
    void setSteer(Vec2 want, float rate) {
        steerWant_ = want;
        steerRate_ = rate > 0.f ? rate : 0.f;
    }
    // The attack under way (null: none), whether it is the heavy, and how far into it (s).
    const ArenaAttack* attack() const { return attack_; }
    bool heavyNow() const { return attack_ != nullptr && move_ == kArenaHeavyMove; }
    float actionTime() const { return actionTime_; }
    // The lead's own chest height (m) for an aim with no target. Default kFieldChestHeight.
    void setChestHeight(float height) { chest_ = height; }
    float time() const { return time_; }
    bool attacking() const { return attack_ != nullptr; }
    // Hits landed and their damage since begin.
    int hits() const { return hits_; }
    int damageDealt() const { return damage_; }

private:
    bool start(std::uint8_t move, Vec2 facing);
    void cancel();
    // The usable targets nearest first (at most kMaxFieldTargets), into near_ / nearCount_.
    void gather(Vec2 pos, const FieldTarget* targets, std::size_t count);
    // Picks the target and turns facing_ at it (or aims straight ahead).
    void takeAim(Vec2 pos, const FieldTarget* targets);
    // Connects with the gathered targets in reach not yet hit by this attack.
    void strikeHits(Vec2 pos, const FieldTarget* targets);
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
    bool chainLive_ = false; // a light chain is going (a heavy or a cut ends it)
    FieldBody body_ = FieldBody::Free;
    float handBack_ = kFieldHandBack;
    float handBackLeft_ = 0.f; // hand-back still to run
    std::uint8_t buffered_ = 0xFF; // the buffered move, 0xFF: none
    float bufferedAt_ = 0.f;
    Vec2 facing_{1.f, 0.f};
    FieldAim aim_{};
    float chest_ = kFieldChestHeight;
    bool reticleOn_ = false;
    Vec2 reticleAt_{};
    float reticleHeight_ = kFieldChestHeight;
    float lungeScale_ = 0.f;
    float lungeStop_ = 0.6f;
    Vec2 steerWant_{};
    float steerRate_ = 0.f;
    float lunge_ = 0.f; // m the attack under way lunges (set as it is aimed at the reticle)
    std::uint32_t hitKeys_[kMaxFieldTargets] = {}; // the keys hit by this attack
    std::uint8_t hitCount_ = 0;
    std::uint8_t near_[kMaxFieldTargets] = {};
    std::uint8_t nearCount_ = 0;
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
