#include "ftcore/SeaWaves.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ftcore {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.f * kPi;
constexpr float kDegToRad = kPi / 180.f;
constexpr float kGravitySi = 9.81f; // metres a second squared, for the spectrum's formulas

// A part of the set: its period against the part's peak, its share of the part's energy, and its turn off the part's way.
struct Part {
    float periodScale;
    float share;
    float turnDeg;
};

constexpr Part kSwell[kSeaSwellWaves] = {{1.f, 0.5f, 0.f}, {0.87f, 0.28f, -8.f}, {1.12f, 0.22f, 10.f}};
// At 0.85, 1, 1.15, 1.4 and 1.75 of the peak frequency; the shares are a JONSWAP spectrum (peak enhancement 3.3)
// integrated over the band round each, rounded.
constexpr Part kWind[kSeaWaveCount - kSeaSwellWaves] = {
    {1.f / 0.85f, 0.14f, 12.f}, {1.f, 0.42f, 0.f}, {1.f / 1.15f, 0.22f, -18.f}, {1.f / 1.4f, 0.14f, 28.f}, {1.f / 1.75f, 0.08f, -35.f}};

// 0..1 from a wave's index, the same on every run.
float unitHash(std::uint32_t i) {
    i ^= i >> 16;
    i *= 0x7feb352dU;
    i ^= i >> 15;
    i *= 0x846ca68bU;
    i ^= i >> 16;
    return static_cast<float>(i >> 8) * (1.f / 16777216.f);
}

SeaWave makeWave(float periodS, float hsCm, float share, float wayDeg, int index) {
    SeaWave w;
    // A whole number of turns in the loop.
    const float turns = std::max(1.f, std::round(kSeaLoopS / periodS));
    w.omega = turns * kTwoPi / kSeaLoopS;
    w.k = w.omega * w.omega / kSeaGravity;
    w.amplitude = std::min(std::sqrt(2.f * share) * hsCm * 0.25f, kSeaMaxSteepness / w.k);
    w.dirX = std::cos(wayDeg * kDegToRad);
    w.dirY = std::sin(wayDeg * kDegToRad);
    w.phase = unitHash(static_cast<std::uint32_t>(index) + 1u) * kTwoPi;
    return w;
}

float wrapTime(float t) { return std::fmod(t, kSeaLoopS); }

float theta(const SeaWave& w, float x0, float y0, float t) { return w.k * (w.dirX * x0 + w.dirY * y0) - w.omega * t + w.phase; }

// The horizontal map's partial derivatives at a rest point: d(x)/d(x0) = 1 - xx, d(y)/d(y0) = 1 - yy, the cross term -xy.
void foldTerms(const SeaWaves& sea, float x0, float y0, float t, float& xx, float& yy, float& xy) {
    xx = yy = xy = 0.f;
    for (const SeaWave& w : sea.waves) {
        const float s = sea.q * w.k * w.amplitude * std::sin(theta(w, x0, y0, t));
        xx += s * w.dirX * w.dirX;
        yy += s * w.dirY * w.dirY;
        xy += s * w.dirX * w.dirY;
    }
}

} // namespace

SeaWaves seaWaves(const SceneLook& look) {
    SeaWaves sea;
    const float swellHsCm = std::clamp(look.seaSwellM, 0.f, 8.f) * 100.f;
    const float swellT = std::clamp(look.seaSwellPeriodS, 4.f, 20.f);

    // The wind's sea, fetch-limited (JONSWAP, in metres and seconds): peak frequency 22 (g^2 / (U F))^(1/3) and
    // significant height 0.0016 U sqrt(F / g), held to the fully developed sea's 0.877 g / U and 0.24 U^2 / g.
    const float u = std::clamp(look.windMps, 0.f, 40.f);
    const float fetchM = std::clamp(look.seaFetchKm, 0.1f, 500.f) * 1000.f;
    float windHsCm = 0.f;
    float windT = 2.f;
    if (u > 0.f) {
        const float peak = std::max(22.f * std::cbrt(kGravitySi * kGravitySi / (u * fetchM)), 0.877f * kGravitySi / u);
        windT = std::clamp(kTwoPi / peak, 0.8f, 12.f);
        windHsCm = std::min(0.0016f * u * std::sqrt(fetchM / kGravitySi), 0.24f * u * u / kGravitySi) * 100.f;
    }
    windHsCm *= std::clamp(look.seaChop, 0.f, 3.f);

    int i = 0;
    for (const Part& p : kSwell) {
        sea.waves[i] = makeWave(swellT * p.periodScale, swellHsCm, p.share, look.seaSwellDeg + p.turnDeg, i);
        ++i;
    }
    for (const Part& p : kWind) {
        sea.waves[i] = makeWave(windT * p.periodScale, windHsCm, p.share, look.windDeg + p.turnDeg, i);
        ++i;
    }

    float fold = 0.f;
    for (const SeaWave& w : sea.waves) fold += w.k * w.amplitude;
    sea.q = fold > 0.f ? std::min(1.f, kSeaMaxFold / fold) : 0.f;
    return sea;
}

void packSeaWave(const SeaWave& wave, float out[4]) {
    out[0] = wave.k * wave.dirX;
    out[1] = wave.k * wave.dirY;
    out[2] = wave.amplitude;
    out[3] = wave.phase;
}

void seaOffset(const SeaWaves& sea, float x0, float y0, float t, float out[3]) {
    t = wrapTime(t);
    out[0] = out[1] = out[2] = 0.f;
    for (const SeaWave& w : sea.waves) {
        const float th = theta(w, x0, y0, t);
        const float c = sea.q * w.amplitude * std::cos(th);
        out[0] += c * w.dirX;
        out[1] += c * w.dirY;
        out[2] += w.amplitude * std::sin(th);
    }
}

float seaHeight(const SeaWaves& sea, float x, float y, float t) {
    t = wrapTime(t);
    float x0 = x, y0 = y;
    float d[3];
    for (int step = 0; step < 6; ++step) {
        seaOffset(sea, x0, y0, t, d);
        const float rx = x0 + d[0] - x;
        const float ry = y0 + d[1] - y;
        float xx, yy, xy;
        foldTerms(sea, x0, y0, t, xx, yy, xy);
        // Solve [[1 - xx, -xy], [-xy, 1 - yy]] * delta = residual.
        const float a = 1.f - xx, b = -xy, c = 1.f - yy;
        const float det = a * c - b * b;
        x0 -= (c * rx - b * ry) / det;
        y0 -= (a * ry - b * rx) / det;
    }
    seaOffset(sea, x0, y0, t, d);
    return d[2];
}

float seaJacobian(const SeaWaves& sea, float x0, float y0, float t) {
    float xx, yy, xy;
    foldTerms(sea, x0, y0, wrapTime(t), xx, yy, xy);
    return (1.f - xx) * (1.f - yy) - xy * xy;
}

float seaSignificantHeightCm(const SeaWaves& sea) {
    float m0 = 0.f;
    for (const SeaWave& w : sea.waves) m0 += 0.5f * w.amplitude * w.amplitude;
    return 4.f * std::sqrt(m0);
}

} // namespace ftcore
