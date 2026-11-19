#pragma once

// Whether a picked water spot (a dry edge, an "in" point, a far point) can be walked and swum, judged from what the
// host's traces found. The dev captures' water search uses it; each candidate gives a short list of hits:
// - DryToIn: the lead's capsule swept from the dry edge to over the "in" point, at walking height lifted by the step
//   height (what the walk can't step over: a wall, a glass front, a railing);
// - InToFar: the capsule swept from the "in" point to the far point at swimming height (a pier, a pile, a wall in the
//   water);
// - Up: a look up from the dry edge's ground (a roof or a ceiling within roofHeight: the edge is indoors or covered).
// Only blocking hits count; a sweep that starts inside something is a blocking hit at 0. A spot with no ground under
// its dry edge is rejected first, then a roofed one, then one blocked on the way in, then one blocked out in the water.
// rejectDryToIn false accepts a spot blocked on the way in but says so (the land capture keeps its railing, which the
// lead must jump; the held-up jump and the harness placement handle it). The tally counts the reasons over a search.
// No heap use.

#include <cstddef>
#include <cstdint>

namespace ftcore {

enum class WaterSpotLeg : std::uint8_t { DryToIn, InToFar, Up };

struct WaterSpotHit {
    WaterSpotLeg leg = WaterSpotLeg::DryToIn;
    float distance = 0.f; // m along the leg (Up: over the dry edge's ground)
    bool blocking = true; // an overlap (not blocking) is ignored
};

enum class WaterSpotReason : std::uint8_t { Ok, NoGround, Roofed, DryToInBlocked, InToFarBlocked };
inline constexpr int kWaterSpotReasons = 5;

struct WaterSpotRules {
    float roofHeight = 4.f;    // m: a blocking hit this near overhead is a roof
    bool rejectDryToIn = true; // false: a spot blocked on the way in is kept (said in dryToInBlocked)
};

struct WaterSpotVerdict {
    WaterSpotReason reason = WaterSpotReason::Ok; // why it was rejected (Ok: accepted)
    bool accepted = false;
    bool dryToInBlocked = false; // a blocking hit on the way in (also when it was kept)
    float dryToInAt = -1.f;      // m along the way in to the nearest such hit (-1: none)
    float at = -1.f;             // m along its leg to the rejecting hit (-1: none, or no ground)
};

inline const char* waterSpotReasonName(WaterSpotReason r)
{
    switch (r) {
    case WaterSpotReason::Ok: return "ok";
    case WaterSpotReason::NoGround: return "no ground";
    case WaterSpotReason::Roofed: return "roofed";
    case WaterSpotReason::DryToInBlocked: return "blocked on the way in";
    case WaterSpotReason::InToFarBlocked: return "blocked in the water";
    }
    return "?";
}

// Judges one candidate from its hits (count of them; hits may be null when count is 0).
inline WaterSpotVerdict judgeWaterSpot(const WaterSpotHit* hits, std::size_t count, bool hasGround, const WaterSpotRules& rules)
{
    WaterSpotVerdict v;
    float roof = -1.f;
    float out = -1.f;
    for (std::size_t i = 0; i < count; ++i) {
        const WaterSpotHit& h = hits[i];
        if (!h.blocking) {
            continue;
        }
        const float d = h.distance > 0.f ? h.distance : 0.f;
        switch (h.leg) {
        case WaterSpotLeg::Up:
            if (d <= rules.roofHeight && (roof < 0.f || d < roof)) {
                roof = d;
            }
            break;
        case WaterSpotLeg::DryToIn:
            if (v.dryToInAt < 0.f || d < v.dryToInAt) {
                v.dryToInAt = d;
            }
            v.dryToInBlocked = true;
            break;
        case WaterSpotLeg::InToFar:
            if (out < 0.f || d < out) {
                out = d;
            }
            break;
        }
    }
    if (!hasGround) {
        v.reason = WaterSpotReason::NoGround;
    } else if (roof >= 0.f) {
        v.reason = WaterSpotReason::Roofed;
        v.at = roof;
    } else if (v.dryToInBlocked && rules.rejectDryToIn) {
        v.reason = WaterSpotReason::DryToInBlocked;
        v.at = v.dryToInAt;
    } else if (out >= 0.f) {
        v.reason = WaterSpotReason::InToFarBlocked;
        v.at = out;
    } else {
        v.reason = WaterSpotReason::Ok;
    }
    v.accepted = v.reason == WaterSpotReason::Ok;
    return v;
}

// Counts the verdicts of a search by reason.
struct WaterSpotTally {
    int counts[kWaterSpotReasons] = {};
    void add(const WaterSpotVerdict& v) { ++counts[static_cast<int>(v.reason)]; }
    int of(WaterSpotReason r) const { return counts[static_cast<int>(r)]; }
    int total() const
    {
        int n = 0;
        for (int c : counts) {
            n += c;
        }
        return n;
    }
};

} // namespace ftcore
