#pragma once

// No tide strands a swimmer: at every level a body's tide reaches, each way out of it (a ladder, steps, a mantle edge)
// is usable, or another usable one is within reachM of it.
//
//   Ladder, Steps  usable at surface z when its lowest rung or tread (bottomZ) is at least underM under z, so a hand or a
//                  foot finds it from the water; a ladder or steps from the map's WaterExit rows run to the bed under
//                  their foot unless the builder says otherwise (waterWayOutFromExit).
//   Mantle         a wall's top a swimmer can climb onto: usable when topZ is no more than mantleOverM over z (the climb
//                  reaches 3.3 m over the feet, which hang kWaterSwimDraft under the surface: 2.02 m).
//
// Levels are the body's mean level plus `levels` steps from its tide band's low to high water (WaterWaves.h); waves are
// left out (a crest helps as often as a trough hinders). The map builder fills the ways (its exits, and mantle points
// sampled along each quay with the bank's top there), runs the check and logs the report; the test runs it on fixtures.
// No allocation.
//
// waterExitCoverage proves the other half of the rule: from every point a player can enter the water (the map builder
// samples each reachable bank at 2 m or closer and names the body it enters; Core does not sample a sea's half-plane
// itself), a usable way out lies within reachM, at every level of the body's tide. The distance is a straight line for
// now, so at a bend it can cut across a quay a swimmer would have to go round.

#include "ftcore/Api.h"
#include "ftcore/WaterBodies.h"
#include "ftcore/WaterWaves.h"

#include <cstdint>

namespace ftcore {

enum class WaterWayKind : std::uint8_t { Ladder, Steps, Mantle };

struct WaterWayOut {
    WaterWayKind kind = WaterWayKind::Ladder;
    Vec2 at{};          // m: where a swimmer takes it, in the water
    float bottomZ = 0.f; // m: the lowest rung or tread (ladder, steps)
    float topZ = 0.f;    // m: the bank's top
};

struct WaterStrandSpec {
    float reachM = 25.f;
    float underM = 0.3f;
    float mantleOverM = 3.3f - kWaterSwimDraft;
    int levels = 9;
};

inline constexpr int kWaterStrandKept = 8;

struct WaterStrandFailure {
    int way = -1;      // the way that was not usable, with none usable within reachM; -1 for a body with no way at all
    int body = -1;
    float levelM = 0.f; // the surface it failed at
};

struct WaterStrandReport {
    int ways = 0;           // ways checked (in a body)
    int checks = 0;         // way-and-level pairs
    int stranded = 0;       // pairs with no usable way within reach
    int bodiesWithout = 0;  // tidal or not, bodies with no way out at all
    int unplaced = 0;       // ways in no body
    int listed = 0;         // entries in first (stranded pairs and bodies without a way, in the order found)
    WaterStrandFailure first[kWaterStrandKept];
    bool ok() const { return stranded == 0 && bodiesWithout == 0 && unplaced == 0; }
};

// A point a player can go into the water from, and the body it enters.
struct WaterEntryPoint {
    Vec2 at{};
    int body = -1;
};

inline constexpr int kWaterCoverageMaxLevels = 16; // WaterStrandSpec::levels is held to this here
inline constexpr float kWaterCoverageNoWayM = 1.0e9f; // the distance reported where a body has no usable way at a level

// The worst entry point at one step of the tide band (step l of levels, low water first; each body at its own level).
struct WaterCoverageLevel {
    int point = -1;     // the worst point, -1 when none was checked
    int body = -1;
    float levelM = 0.f; // its body's surface there
    float distM = 0.f;  // to its nearest usable way (kWaterCoverageNoWayM: none)
};

struct WaterCoverageReport {
    int points = 0;    // entry points checked (in a body)
    int unplaced = 0;  // entry points naming no body
    int levels = 0;    // steps of the band checked
    int uncovered = 0;    // point-and-level pairs with no usable way within reach
    WaterCoverageLevel worst[kWaterCoverageMaxLevels];
    // Every point covered at every level; a check of no points proves nothing and fails.
    bool ok() const { return points > 0 && unplaced == 0 && uncovered == 0; }
};

FTCORE_API WaterCoverageReport waterExitCoverage(const WaterBodies& water, const WaterSpec& spec, const WaterEntryPoint* points, int pointCount,
                                                 const WaterWayOut* ways, int wayCount, const WaterStrandSpec& strand = WaterStrandSpec{});

FTCORE_API WaterWayOut waterWayOutFromExit(const WaterBodies& water, const WaterExit& exit);
FTCORE_API bool waterWayUsable(const WaterWayOut& way, float surfaceZ, const WaterStrandSpec& spec);
FTCORE_API WaterStrandReport waterStrandCheck(const WaterBodies& water, const WaterSpec& spec, const WaterWayOut* ways, int count,
                                              const WaterStrandSpec& strand = WaterStrandSpec{});

} // namespace ftcore
