#pragma once

// The sea's waves: a sum of eight Gerstner (trochoidal) waves, the reference the sea material (Env/Waves.ush) mirrors
// line by line. A Gerstner wave carries each point of the surface round a circle, so its crests sharpen and its troughs
// flatten the way a real sea's do.
//
//   const ftcore::SeaWaves sea = ftcore::seaWaves(look);   // once per look change
//   float v[4]; ftcore::packSeaWave(sea.waves[i], v);      // the material's SeaWave<i> vector
//   const float z = ftcore::seaHeight(sea, x, y, t);       // the surface's height over (x, y) at t: floats, spray
//
// The set:
//   - Waves 0-2 are the swell, come from far off: seaSwellM significant height at seaSwellPeriodS (the main wave, and two
//     at 0.87 and 1.12 of the period), travelling seaSwellDeg give or take 10 degrees.
//   - Waves 3-7 are the sea the wind raises over seaFetchKm of open water (a fetch-limited JONSWAP sea: its peak
//     frequency and height from windMps and the fetch, held to the fully developed sea's), scaled by seaChop, at 0.85
//     to 1.75 of the peak frequency and within 35 degrees of windDeg, the higher ones spread wider.
//   - Deep water: omega^2 = g k, so a wave of period T is 1.56 T^2 metres long. Every omega is a whole number of turns in
//     kSeaLoopS, so the sea repeats exactly and the material's time can wrap there without a seam (keeping its
//     precision); that moves no wavelength by more than 2%.
//   - The amplitudes split each part's significant height by its energy share (Hs = 4 sqrt(sum A^2 / 2)), and no wave is
//     steeper than k A = 0.3. The shared steepness q holds q * sum(k A) to 0.9 at most, so the surface never folds over.
//   - The phases are fixed per wave, so a look always gives the same sea.
//
// Distances are centimetres, times seconds and angles radians unless a name says degrees; a way is degrees like a yaw
// (0 = toward +X, 90 = toward +Y). Plain value maths, no allocation.

#include "ftcore/Api.h"
#include "ftcore/SceneLook.h"

namespace ftcore {

inline constexpr int kSeaWaveCount = 8;
inline constexpr int kSeaSwellWaves = 3;
inline constexpr float kSeaGravity = 981.f;   // cm per second squared
inline constexpr float kSeaLoopS = 1800.f;    // the sea repeats every this many seconds
inline constexpr float kSeaMaxFold = 0.9f;    // q * sum(k A) stays at or under this
inline constexpr float kSeaMaxSteepness = 0.3f; // k A of any one wave

struct SeaWave {
    float dirX = 1.f, dirY = 0.f; // the way it travels, a unit vector
    float k = 0.f;                // wavenumber, radians a centimetre
    float omega = 0.f;            // angular frequency, radians a second: sqrt(g k)
    float amplitude = 0.f;        // centimetres
    float phase = 0.f;            // radians
};

struct SeaWaves {
    SeaWave waves[kSeaWaveCount];
    float q = 0.f; // the shared Gerstner steepness, 0..1
};

FTCORE_API SeaWaves seaWaves(const SceneLook& look);

// The material's vector for a wave: {k dirX, k dirY, amplitude, phase}; the shader takes k as its length and omega as
// sqrt(g k).
FTCORE_API void packSeaWave(const SeaWave& wave, float out[4]);

// Where the surface point that rests at (x0, y0) is at time t, as an offset from its rest place: {dx, dy, dz}.
FTCORE_API void seaOffset(const SeaWaves& sea, float x0, float y0, float t, float out[3]);

// The surface's height over (x, y) at time t. The point there rests somewhere else, found by six Newton steps on the
// horizontal map (its Jacobian never reaches 0, see seaJacobian): well under a millimetre out, even on the steepest sea.
FTCORE_API float seaHeight(const SeaWaves& sea, float x, float y, float t);

// The horizontal map's Jacobian determinant at a rest point: 1 for a flat sea, less where the crests crowd the surface
// together (the material's whitecaps), and never 0 or less.
FTCORE_API float seaJacobian(const SeaWaves& sea, float x0, float y0, float t);

// The set's significant height, centimetres: 4 sqrt(sum A^2 / 2).
FTCORE_API float seaSignificantHeightCm(const SeaWaves& sea);

} // namespace ftcore
