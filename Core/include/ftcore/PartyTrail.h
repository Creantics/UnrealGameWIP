#pragma once

// The party following its lead on the field: a trail of the lead's ground positions, and the point
// a given distance behind the lead along it.
//
//   ftcore::PartyTrail trail;
//   trail.reset(leadPos, leadFacing, 2 * PartyTrail::kFollowerSpacing);   // at spawn
//   trail.push(leadPos);                                                  // every frame
//   follower i stands at trail.pointBehind((i + 1) * PartyTrail::kFollowerSpacing);
//
// A follower is placed, not steered: it is the point that far behind along the path the lead
// walked, so it moves exactly when and as fast as the lead did and can't fall further behind.
// The trail is sampled by distance, not time, so a long frame (a fast-forwarded run) still gives
// a path with the corners in it. Positions are metres on the ground plane. Fixed capacity: nothing
// here allocates.
//
// The lead's turns and jumps ride the trail too: markTurn or markJump where the lead begins one, and
// each follower, as its point reaches that spot, gets it from turnsReached or jumpsReached and plays
// the same there. Each sample also keeps the lead's speed as it passed, for PartySlot.
//
// PartySlot is one follower's place in the party, moving along the trail on its own: it keeps about
// `spacing` behind the lead and `side` metres to one side of the path, reacts to the lead's starts
// and stops a beat late (and with the lead's acceleration), goes no faster through a stretch than
// the lead did there (plus a bounded catch-up), and never comes nearer the lead than `minGap`.

#include "ftcore/Api.h"
#include "ftcore/Explore.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

class PartyTrail {
public:
    static constexpr std::size_t kCapacity = 64;   // samples kept; older ones fall off
    static constexpr float kSampleSpacing = 0.25f; // metres between samples
    static constexpr float kFollowerSpacing = 1.6f; // metres between party members along the trail
    static constexpr std::size_t kMaxFollowers = 3;
    static constexpr std::size_t kTurns = 8; // turns kept; older ones fall off

    // Starts over at `lead`, with a straight trail of `seedLength` metres behind it, opposite
    // `facing` (+x when the facing is zero), so followers start in a line rather than on the lead.
    FTCORE_API void reset(Vec2 lead, Vec2 facing, float seedLength);
    // The lead's new position and ground speed (m/s; below 0 unknown). A move longer than the whole
    // trail starts it over, facing the move.
    FTCORE_API void push(Vec2 lead, float speed = -1.f);

    // The point `arc` metres behind the lead along the trail (on its segments, not at a sample).
    // Past the end of the trail: its oldest point. Zero or less: the lead.
    FTCORE_API Vec2 pointBehind(float arc) const;
    // Metres from the lead back to the oldest point.
    FTCORE_API float length() const;
    // How far the lead has come along the trail (metres, from wherever it began; it goes on counting
    // through a reset). A point `arc` behind the lead is at odometer() - arc.
    FTCORE_API float odometer() const;

    // The lead begins a turn by `delta` degrees where it is now. A reset forgets the turns marked.
    FTCORE_API void markTurn(float delta);
    // The turns the point `arc` behind the lead has reached since `next` (0 at first; the call moves
    // it past them): their degrees summed, 0 when none.
    FTCORE_API float turnsReached(float arc, std::uint32_t& next) const;
    // The lead jumps where it is now; jumpsReached counts those the point `arc` behind has reached
    // since `next`, as turnsReached does.
    FTCORE_API void markJump();
    FTCORE_API int jumpsReached(float arc, std::uint32_t& next) const;
    // The lead's speed as it passed the point `arc` behind it (m/s), below 0 where unknown (the
    // seeded trail, or pushes without a speed).
    FTCORE_API float speedBehind(float arc) const;

    Vec2 lead() const { return lead_; }
    std::size_t samples() const { return count_; }

private:
    // Sample k back from the newest (0 = newest).
    Vec2 sample(std::size_t k) const { return ring_[(newest_ + kCapacity - k) % kCapacity]; }
    float sampleSpeed(std::size_t k) const { return speed_[(newest_ + kCapacity - k) % kCapacity]; }
    void add(Vec2 p, float speed);

    std::array<Vec2, kCapacity> ring_{};
    std::array<float, kCapacity> speed_{};
    std::size_t newest_ = 0;
    std::size_t count_ = 0;
    std::uint32_t laid_ = 0; // samples added in all
    Vec2 lead_;

    struct Turn {
        float at = 0.f; // the odometer where it began
        float delta = 0.f;
    };
    std::array<Turn, kTurns> turns_{};
    std::uint32_t turnCount_ = 0; // turns marked in all
    std::uint32_t turnBase_ = 0;  // the first still on this trail (after the last reset)
    static constexpr std::size_t kJumps = 4;
    std::array<float, kJumps> jumps_{}; // the odometer at each
    std::uint32_t jumpCount_ = 0;
    std::uint32_t jumpBase_ = 0;
    float leadSpeed_ = -1.f;
};

struct PartySlotTuning {
    float spacing = 1.5f;    // metres behind the lead along the trail, when it holds its speed
    float side = 0.7f;       // metres to the side of the path (+ left of the way it goes, - right)
    float minGap = 1.0f;     // never nearer the lead than this (metres, straight line)
    float minArc = 0.9f;     // nor nearer along the trail
    float react = 0.25f;     // seconds: the lag of its speed behind the lead's
    float gain = 1.2f;       // 1/s: how hard it closes a gap that has opened...
    float slack = 0.3f;      // ...past this (metres), so the lead gets a step ahead before it reacts
    float settle = 4.f;      // m/s^2: the braking it plans for, coming into its place behind a standing lead
    float accel = 10.f;      // m/s^2, speeding up
    float decel = 10.f;      // m/s^2, braking
    float catchUp = 0.15f;   // through a stretch, at most this share faster than the lead went there...
    float catchUpMin = 0.6f; // ...plus this (m/s), so a stretch the lead crept along doesn't stall it
    float maxSpeed = 9.f;    // m/s
    float sideTime = 0.35f;  // seconds: how fast the side offset swings round as the path bends
};

class PartySlot {
public:
    // Puts it `tuning().spacing` behind the trail's lead, standing.
    FTCORE_API void reset(const PartyTrail& trail);
    // One frame: the lead's ground speed now (m/s) and the frame time.
    FTCORE_API void step(const PartyTrail& trail, float leadSpeed, float dt);

    Vec2 position() const { return pos_; }
    // Metres behind the lead along the trail (for turnsReached and jumpsReached).
    float arc() const { return arc_; }
    float speed() const { return speed_; }
    // Where its speed is heading, for the animation's starts and stops: the speed it is
    // speeding up or easing toward (m/s), or 0 when it is coming to a stand, with the
    // distance it will take (metres; -1 while it isn't stopping).
    float wantSpeed() const { return want_ > kStanding ? want_ : 0.f; }
    FTCORE_API float stopDistance() const;
    PartySlotTuning& tuning() { return tuning_; }

private:
    Vec2 sideOffset(const PartyTrail& trail, float arc) const;

    PartySlotTuning tuning_;
    float at_ = 0.f;  // its own odometer reading along the trail
    float arc_ = 0.f;
    float speed_ = 0.f;
    float leadLag_ = 0.f;
    float want_ = 0.f;
    static constexpr float kStanding = 0.05f; // m/s
    Vec2 offset_;
    Vec2 pos_;
};

} // namespace ftcore
