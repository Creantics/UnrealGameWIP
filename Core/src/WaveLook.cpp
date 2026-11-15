#include "ftcore/WaveLook.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

bool finite3(const WaveVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Whether a ray from the eye, along dir, passes through the sphere of radius r round the origin (the eye inside counts).
bool rayCrossesSphere(const WaveVec3& origin, float r, const WaveVec3& eye, const WaveVec3& dir) {
    const float ox = eye.x - origin.x;
    const float oy = eye.y - origin.y;
    const float oz = eye.z - origin.z;
    const float r2 = r * r;
    const float o2 = ox * ox + oy * oy + oz * oz;
    if (o2 < r2) return true;
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (!(len > 1e-6f)) return false;
    const float b = (ox * dir.x + oy * dir.y + oz * dir.z) / len;
    if (b >= 0.f) return false;  // the sphere is behind the eye
    return o2 - b * b < r2;      // the closest approach is inside it
}

} // namespace

float worldWaveViewShare(const WorldWaveFrame& frame, const WaveVec3& origin, const WaveVec3& eye,
                         const WaveViewSample* samples, int count) {
    if (!samples || count <= 0 || !finite3(origin) || !finite3(eye)) return 0.f;
    const bool forward = frame.direction == WaveDirection::Forward;
    const float mid = frame.radius - 0.5f * frame.bandWidth;
    const float r = std::isfinite(mid) ? std::max(mid, 0.f) : 0.f;
    int second = 0;
    int used = 0;
    for (int i = 0; i < count; ++i) {
        const WaveViewSample& s = samples[i];
        bool inside = false;
        if (s.hit) {
            if (!finite3(s.point)) continue;
            const float dx = s.point.x - origin.x;
            const float dy = s.point.y - origin.y;
            const float dz = s.point.z - origin.z;
            inside = r > 0.f && dx * dx + dy * dy + dz * dz < r * r;
        } else {
            if (!finite3(s.dir)) continue;
            inside = r > 0.f && rayCrossesSphere(origin, r, eye, s.dir);
        }
        ++used;
        if (inside == forward) ++second;
    }
    return used > 0 ? static_cast<float>(second) / static_cast<float>(used) : 0.f;
}

float stepWaveCover(float cover, float share, float dt, float rate) {
    if (!std::isfinite(cover)) cover = 0.f;
    cover = std::clamp(cover, 0.f, 1.f);
    if (!std::isfinite(share) || !std::isfinite(dt) || !std::isfinite(rate) || dt <= 0.f || rate <= 0.f) return cover;
    const float want = std::clamp(share, 0.f, 1.f);
    const float most = rate * dt;
    return std::clamp(cover + std::clamp(want - cover, -most, most), 0.f, 1.f);
}

SceneLook waveSkyStart(const SceneLook& from, const SceneLook& to) {
    SceneLook start = from;
    if (to.cloudCover > 0.f && !(from.cloudCover > 0.f)) {
        start.cloudCover = kWaveFairCover;
        start.clouds = 1.f;
    }
    if (to.hasClouds()) start.clouds = 1.f;
    return start;
}

SceneLook waveSkyLook(const SceneLook& from, const SceneLook& to, float t) { return waveSkyLook(from, to, t, t); }

float stepWaveCloud(float cloudT, float cover, float dt, float rate) {
    if (!std::isfinite(cloudT)) cloudT = 0.f;
    cloudT = std::clamp(cloudT, 0.f, 1.f);
    if (!std::isfinite(cover)) return cloudT;
    if (cover >= kWaveCloudFull) return stepWaveCover(cloudT, 1.f, dt, rate);
    if (cover <= kWaveCloudNone) return stepWaveCover(cloudT, 0.f, dt, rate);
    return cloudT;
}

SceneLook waveSkyLook(const SceneLook& from, const SceneLook& to, float t, float cloudT) {
    if (!std::isfinite(t)) t = 0.f;
    t = std::clamp(t, 0.f, 1.f);
    if (!std::isfinite(cloudT)) cloudT = 0.f;
    cloudT = std::clamp(cloudT, 0.f, 1.f);
    SceneLook r = from;
    const auto mixBy = [](float a, float b, float s) { return s <= 0.f ? a : (s >= 1.f ? b : a + (b - a) * s); };
    const auto mix = [t, &mixBy](float a, float b) { return mixBy(a, b, t); };
    const auto mixCloud = [cloudT, &mixBy](float a, float b) { return mixBy(a, b, cloudT); };
#define FTCORE_WAVE_MIX(name) r.name = mix(from.name, to.name);
#define FTCORE_WAVE_CLOUD(name) r.name = mixCloud(from.name, to.name);
    FTCORE_WAVE_MIX(sunLux)
    FTCORE_WAVE_MIX(sunKelvin)
    FTCORE_WAVE_MIX(skyLight)
    FTCORE_WAVE_MIX(fogDensity)
    FTCORE_WAVE_MIX(fogFalloff)
    FTCORE_WAVE_MIX(volumetricFog)
    FTCORE_WAVE_MIX(fogForward)
    FTCORE_WAVE_MIX(fogAlbedoR)
    FTCORE_WAVE_MIX(fogAlbedoG)
    FTCORE_WAVE_MIX(fogAlbedoB)
    FTCORE_WAVE_MIX(windDeg)
    FTCORE_WAVE_MIX(windMps)
    FTCORE_WAVE_CLOUD(cloudCover)
    FTCORE_WAVE_CLOUD(cloudType)
    FTCORE_WAVE_CLOUD(cloudBaseKm)
    FTCORE_WAVE_CLOUD(cloudThickKm)
    FTCORE_WAVE_CLOUD(cloudAlbedo)
    FTCORE_WAVE_CLOUD(cloudDensity)
    FTCORE_WAVE_CLOUD(cloudDrift)
    FTCORE_WAVE_CLOUD(cloudUnderglow)
    FTCORE_WAVE_CLOUD(cloudUnderglowKelvin)
#undef FTCORE_WAVE_CLOUD
#undef FTCORE_WAVE_MIX
    // The layer is a switch: on as soon as either end has it, so it never pops off half way.
    r.clouds = (from.hasClouds() || to.hasClouds()) ? 1.f : from.clouds;
    return r;
}

float waveKeyLight(const SceneLook& look) { return look.sunLux + kWaveSkyWeight * look.skyLight; }

float waveCaptureSkyLight(const SceneLook& from, const SceneLook& now) {
    const float sky = now.skyLight;
    if (!std::isfinite(sky)) return std::isfinite(from.skyLight) ? from.skyLight : 0.f;
    const float a = from.sunLux;
    const float b = now.sunLux;
    if (!std::isfinite(a) || !std::isfinite(b) || a <= 0.f || b <= 0.f) return sky;
    return sky * std::clamp(a / b, 1.f, kWaveCaptureLiftMax);
}

WaveGains waveGains(const SceneLook& from, const SceneLook& now) {
    WaveGains g;
    const float a = waveKeyLight(from);
    const float b = waveKeyLight(now);
    if (!std::isfinite(a) || !std::isfinite(b) || a <= 0.f || b <= 0.f) return g;
    const float lost = std::clamp(a / b, 1.f / 16.f, 16.f);
    g.outside = lost;
    g.inside = 1.f;
    return g;
}

} // namespace ftcore
