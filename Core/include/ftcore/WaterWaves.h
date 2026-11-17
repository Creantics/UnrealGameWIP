#pragma once

// The water's waves and tides: one surface function for every body, shared by gameplay and the water materials.
//
//   z(p, t) = body.level + tide(body, clock) + waves(body, p, t)      (+ the interactive field, drawn only)
//
// Waves. Every body rides one of three Gerstner sets (ftcore::SeaWaves, eight trochoidal waves each), by its class:
//   Sea    the swell and the wind's sea from the look (seaWaves), storm adding wind and swell;
//   River  a canal row whose flow is at least riverFlowMin, a lake: short wind chop over riverFetchKm, its significant
//          height held to riverMaxHsCm (storm: riverStormHsCm) and never under the calm floor;
//   Canal  a still canal, a pool: ripples only, held to canalMaxHsCm (storm: canalStormHsCm).
// Near a wall or a shore (edge distance d: a box's long walls, a half-plane's edge; a polygon has none) the waves fade:
// the sideways sway by f = smoothstep(0, fade, d), so no water pushes through a wall, and the height by
// lapShare + (1 - lapShare) f, so the water still rises and falls against the wall (laps) with a share of its height.
// fade is fadeLengths of the set's main wavelength, at least minFadeCm.
//
// The material's copy (Water/Surface.ush) moves each grid vertex by waterWaveOffset at its rest place, and drops a wave
// shorter than four of the grid's cells out of the offset (keep = smoothstep(4, 6, wavelength / gridCm)), so the grid
// never aliases it; this does the same with spec.gridCm, the grid's cell near the party, so the height a game query
// gets is the height the mesh is drawn at there (to 2 cm: test_water_waves).
//
// Tides. A sea's level rises and falls twice a game day (periodHours), rangeM from low to high water, high water at
// highHour; a river follows with riverGain of the range riverLagHours later, a canal with canalGain canalLagHours later.
// Pools and lakes keep their level. The hours run on (day * 24 + hours), so nothing jumps at midnight. A body's saved
// level is its mean water.
//
// Distances are centimetres in the wave functions (as SeaWaves) and metres in the body and tide functions (as
// WaterBodies); times are seconds. Plain value maths: no allocation but parseWaterSpec.

#include "ftcore/Api.h"
#include "ftcore/SceneLook.h"
#include "ftcore/SeaWaves.h"
#include "ftcore/WaterBodies.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

enum class WaterWaveClass : std::uint8_t { Sea, River, Canal };
inline constexpr int kWaterWaveClasses = 3;

FTCORE_API const char* waterWaveClassName(WaterWaveClass c);

struct WaterWaveSpec {
    float stormWindMps = 0.f;    // wind a full storm adds to the look's (0: the weather already raises the look's windMps)
    float stormSwellM = 1.2f;    // swell height a full storm adds
    float calmWindMps = 2.5f;    // a river or canal never sees less wind than this: the air is never quite still
    float riverFlowMin = 0.3f;   // m/s: a canal row flowing this fast or faster is a river
    float riverFetchKm = 0.3f;
    float riverMaxHsCm = 12.f;
    float riverStormHsCm = 25.f;
    float canalFetchKm = 0.05f;
    float canalMaxHsCm = 3.f;
    float canalStormHsCm = 8.f;
    float lapShare = 0.35f;      // share of the height kept against a wall
    float fadeLengths = 0.5f;    // the edge fade over this many of the main wavelength
    float minFadeCm = 150.f;
    float gridCm = 25.f;         // the surface grid's cell near the party
};

struct WaterTideSpec {
    float periodHours = 12.f;    // game hours from high water to high water
    float rangeM = 1.f;          // the sea's, low to high water
    float highHour = 8.5f;       // the sea's first high water of the day (8.5: the 21:00 boot lands at high water)
    float riverGain = 0.55f;
    float riverLagHours = 0.75f;
    float canalGain = 0.35f;
    float canalLagHours = 1.5f;
};

struct WaterSpec {
    WaterWaveSpec waves;
    WaterTideSpec tide;
};

// Data/Water/water.json: {"version": 1, "waves": {<WaterWaveSpec field>: number ...}, "tide": {...}}. A missing field
// keeps its default; an unknown one, a non-number or one out of its range fails (out keeps the defaults, error says).
FTCORE_API bool parseWaterSpec(std::string_view json, WaterSpec& out, std::string* error = nullptr);

// The three sets for a look and a storm (0..1), once per look or weather change.
struct WaterWaveSets {
    SeaWaves set[kWaterWaveClasses];
    float mainLengthCm[kWaterWaveClasses] = {}; // the wavelength of each set's highest wave (0: flat)
    float keep[kWaterWaveClasses][kSeaWaveCount] = {}; // each wave's share in the vertex offset at gridCm
    float lapShare = 0.35f;
    float fadeLengths = 0.5f;
    float minFadeCm = 150.f;
    float gridCm = 25.f;
};

FTCORE_API WaterWaveSets waterWaveSets(const SceneLook& look, float storm, const WaterWaveSpec& spec);

FTCORE_API WaterWaveClass waterWaveClass(const WaterBody& body, const WaterWaveSpec& spec);

// How far p (m) is in from the body's nearest wall or shore, metres, 0 at it or outside; a large number where the body
// has none (a polygon, or a box's open ends).
inline constexpr float kWaterNoEdgeM = 1.0e4f;
FTCORE_API float waterEdgeM(const WaterBody& body, Vec2 p);

// The edge fade's distance for a class (cm), and the two factors at an edge distance (cm): sway and height.
FTCORE_API float waterFadeCm(const WaterWaveSets& sets, WaterWaveClass c);
FTCORE_API void waterEdgeFactors(const WaterWaveSets& sets, WaterWaveClass c, float edgeCm, float& sway, float& height);

// Where the surface point resting at (x0, y0) (cm) is at t, as an offset (cm), with its edge distance (cm): what the
// material moves the vertex by.
FTCORE_API void waterWaveOffset(const WaterWaveSets& sets, WaterWaveClass c, float edgeCm, float x0, float y0, float t,
                                float out[3]);

// The waves' height (cm, from the body's level) over (x, y) (cm) at t: the drawn surface's.
FTCORE_API float waterWaveHeightCm(const WaterWaveSets& sets, const WaterBody& body, WaterWaveClass c, float x, float y,
                                   float t);

// The tide: the level's offset (m) from the body's mean for a class at the clock's total hours (day * 24 + hours), and
// the lowest and highest offsets it reaches (the quay walls' wet band). Pools and lakes: 0.
FTCORE_API float waterTideGain(const WaterTideSpec& spec, const WaterBody& body, WaterWaveClass c);
FTCORE_API float waterTideM(const WaterTideSpec& spec, const WaterBody& body, WaterWaveClass c, double totalHours);
FTCORE_API void waterTideBand(const WaterTideSpec& spec, const WaterBody& body, WaterWaveClass c, float& lowM, float& highM);

// The whole surface (m) over p (m) at t (s) and the clock's total hours.
FTCORE_API float waterSurfaceM(const WaterSpec& spec, const WaterWaveSets& sets, const WaterBody& body, Vec2 p, float t,
                               double totalHours);

} // namespace ftcore
