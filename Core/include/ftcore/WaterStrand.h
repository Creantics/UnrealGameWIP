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
    WaterStrandFailure first[kWaterStrandKept]; // the first stranded pairs
    bool ok() const { return stranded == 0 && bodiesWithout == 0; }
};

FTCORE_API WaterWayOut waterWayOutFromExit(const WaterBodies& water, const WaterExit& exit);
FTCORE_API bool waterWayUsable(const WaterWayOut& way, float surfaceZ, const WaterStrandSpec& spec);
FTCORE_API WaterStrandReport waterStrandCheck(const WaterBodies& water, const WaterSpec& spec, const WaterWayOut* ways, int count,
                                              const WaterStrandSpec& strand = WaterStrandSpec{});

} // namespace ftcore
