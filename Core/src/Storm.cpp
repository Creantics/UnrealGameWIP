#include "ftcore/Storm.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

float clamp01(float v) { return std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f; }

float smooth(float lo, float hi, float v) {
    if (!(hi > lo)) return v >= hi ? 1.f : 0.f;
    const float t = std::clamp((v - lo) / (hi - lo), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

std::uint32_t nextU(StormState& s) {
    std::uint32_t x = s.rng ? s.rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s.rng = x;
    return x;
}

// [0, 1), 24 bits: the same on every compiler.
double unit(StormState& s) { return static_cast<double>(nextU(s) >> 8) / 16777216.0; }
double range(StormState& s, double lo, double hi) { return lo + (hi - lo) * unit(s); }

double headGap(const StormSpec& spec, StormState& s, float intensity) {
    const float span = 1.f - spec.strikeFrom;
    const float t = span > 0.f ? std::clamp((intensity - spec.strikeFrom) / span, 0.f, 1.f) : 1.f;
    const double mean = static_cast<double>(spec.meanGapLightS + (spec.meanGapHeavyS - spec.meanGapLightS) * t);
    const double gap = -mean * std::log(1.0 - unit(s)); // unit < 1: finite
    return std::max(gap, static_cast<double>(spec.headGapMinS));
}

void pushThunder(StormState& s, const StormStrike& k) {
    if (s.thunderCount < kStormMaxThunder) {
        s.thunder[s.thunderCount++] = k;
        return;
    }
    // Full: the farthest of the queue and the new one is dropped (the quietest).
    int farthest = 0;
    for (int i = 1; i < s.thunderCount; ++i)
        if (s.thunder[i].distanceM > s.thunder[farthest].distanceM) farthest = i;
    ++s.dropped;
    if (k.distanceM < s.thunder[farthest].distanceM) s.thunder[farthest] = k;
}

} // namespace

StormSky stormSky(const StormSpec& spec, float rainIntensity) {
    StormSky k;
    k.cover = smooth(spec.overcastFrom, spec.overcastFull, clamp01(rainIntensity));
    k.sunScale = 1.f + (spec.sunAtOvercast - 1.f) * k.cover;
    k.skyScale = 1.f + (spec.skyAtOvercast - 1.f) * k.cover;
    k.starAlpha = 1.f - smooth(0.f, spec.starsGoneAt, k.cover);
    k.moonAlpha = 1.f - smooth(spec.starsGoneAt, spec.moonGoneAt, k.cover);
    k.windMps = spec.windCalmMps + (spec.windStormMps - spec.windCalmMps) * k.cover;
    return k;
}

ThunderTier thunderTier(const StormSpec& spec, float distanceM) {
    if (distanceM < spec.nearM) return ThunderTier::Near;
    if (distanceM < spec.midM) return ThunderTier::Mid;
    return ThunderTier::Far;
}

StormState stormStart(std::uint32_t seed) {
    StormState s;
    s.rng = seed ? seed : 0x9E3779B9u;
    return s;
}

void stormStep(const StormSpec& spec, StormState& s, float dtS, float rainIntensity, bool held, StormEvents& out) {
    out.strikeCount = 0;
    out.thunderCount = 0;
    if (!std::isfinite(dtS) || dtS <= 0.f || held) return;
    const double end = s.clockS + static_cast<double>(std::min(dtS, kStormMaxStepS));
    const float intensity = clamp01(rainIntensity);
    if (intensity < spec.strikeFrom) {
        // No lightning in light rain: a drawn strike lapses, and the next is drawn when the storm returns.
        s.nextS = -1.0;
        s.clusterLeft = 0;
    } else if (s.nextS < 0.0) {
        s.nextS = s.clockS + headGap(spec, s, intensity);
    }
    while (s.nextS >= 0.0 && s.nextS <= end && out.strikeCount < kStormMaxStepEvents) {
        StormStrike k;
        k.atS = s.nextS;
        k.head = s.clusterLeft == 0;
        if (k.head) {
            const double lo = static_cast<double>(spec.minDistM), hi = static_cast<double>(std::max(spec.maxDistM, spec.minDistM));
            k.distanceM = static_cast<float>(std::sqrt(range(s, lo * lo, hi * hi))); // uniform over the ring's area
            k.bearingDeg = static_cast<float>(range(s, 0.0, 360.0));
            const int most = std::clamp(spec.clusterMax, 1, 3) - 1;
            s.clusterLeft = (most > 0 && unit(s) < static_cast<double>(spec.clusterChance)) ? 1 + static_cast<int>(nextU(s) % static_cast<std::uint32_t>(most)) : 0;
            s.clusterDistM = k.distanceM;
            s.clusterBearingDeg = k.bearingDeg;
        } else {
            --s.clusterLeft;
            k.distanceM = std::max(spec.minDistM * 0.5f, s.clusterDistM * static_cast<float>(range(s, 0.9, 1.1)));
            k.bearingDeg = std::fmod(s.clusterBearingDeg + static_cast<float>(range(s, -8.0, 8.0)) + 360.f, 360.f);
        }
        k.brightness = std::clamp(spec.nearM / std::max(k.distanceM, 1.f), 0.15f, 1.f) * (k.head ? 1.f : static_cast<float>(range(s, 0.7, 1.0)));
        k.tier = thunderTier(spec, k.distanceM);
        k.thunderAtS = k.atS + static_cast<double>(k.distanceM / kThunderMps);
        s.lastFlashS = k.atS;
        s.lastFlash = k.brightness;
        if (k.head) {
            s.lastHeadS = k.atS;
            s.lastHead = k.brightness;
        }
        pushThunder(s, k);
        out.strikes[out.strikeCount++] = k;
        s.nextS = s.clusterLeft > 0 ? k.atS + range(s, spec.clusterGapMinS, std::max(spec.clusterGapMaxS, spec.clusterGapMinS))
                                    : k.atS + headGap(spec, s, intensity);
    }
    s.clockS = end;
    // Thunder that has arrived, earliest first; past kStormMaxStepEvents it waits a step.
    while (out.thunderCount < kStormMaxStepEvents) {
        int first = -1;
        for (int i = 0; i < s.thunderCount; ++i)
            if (s.thunder[i].thunderAtS <= s.clockS && (first < 0 || s.thunder[i].thunderAtS < s.thunder[first].thunderAtS)) first = i;
        if (first < 0) break;
        out.thunders[out.thunderCount++] = s.thunder[first];
        s.thunder[first] = s.thunder[--s.thunderCount];
    }
}

float stormFlash(const StormSpec& spec, const StormState& s, bool reduce, bool held) {
    if (held) return 0.f;
    if (reduce) {
        const double t = s.clockS - s.lastHeadS;
        const double rise = std::max(static_cast<double>(spec.reducedRiseS), 1e-3), fall = std::max(static_cast<double>(spec.reducedFallS), 1e-3);
        if (t < 0.0 || t > rise + fall) return 0.f;
        const float u = t < rise ? smooth(0.f, 1.f, static_cast<float>(t / rise)) : 1.f - smooth(0.f, 1.f, static_cast<float>((t - rise) / fall));
        return clamp01(s.lastHead * spec.reducedScale * u);
    }
    const double t = s.clockS - s.lastFlashS;
    constexpr double kRise = 0.03;
    const double fall = std::max(static_cast<double>(spec.flashS), 1e-3);
    if (t < 0.0 || t > kRise + fall) return 0.f;
    if (t < kRise) return clamp01(s.lastFlash * static_cast<float>(t / kRise));
    const float f = 1.f - static_cast<float>((t - kRise) / fall);
    return clamp01(s.lastFlash * f * f);
}

} // namespace ftcore
