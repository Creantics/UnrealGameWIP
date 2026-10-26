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

#include "ftcore/Api.h"
#include "ftcore/Explore.h"

#include <array>
#include <cstddef>

namespace ftcore {

class PartyTrail {
public:
    static constexpr std::size_t kCapacity = 64;   // samples kept; older ones fall off
    static constexpr float kSampleSpacing = 0.25f; // metres between samples
    static constexpr float kFollowerSpacing = 1.6f; // metres between party members along the trail
    static constexpr std::size_t kMaxFollowers = 3;

    // Starts over at `lead`, with a straight trail of `seedLength` metres behind it, opposite
    // `facing` (+x when the facing is zero), so followers start in a line rather than on the lead.
    FTCORE_API void reset(Vec2 lead, Vec2 facing, float seedLength);
    // The lead's new position. A move longer than the whole trail starts it over, facing the move.
    FTCORE_API void push(Vec2 lead);

    // The point `arc` metres behind the lead along the trail (on its segments, not at a sample).
    // Past the end of the trail: its oldest point. Zero or less: the lead.
    FTCORE_API Vec2 pointBehind(float arc) const;
    // Metres from the lead back to the oldest point.
    FTCORE_API float length() const;

    Vec2 lead() const { return lead_; }
    std::size_t samples() const { return count_; }

private:
    // Sample k back from the newest (0 = newest).
    Vec2 sample(std::size_t k) const { return ring_[(newest_ + kCapacity - k) % kCapacity]; }
    void add(Vec2 p);

    std::array<Vec2, kCapacity> ring_{};
    std::size_t newest_ = 0;
    std::size_t count_ = 0;
    Vec2 lead_;
};

} // namespace ftcore
