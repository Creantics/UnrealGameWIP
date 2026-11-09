#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "ftcore/SceneLook.h"

using ftcore::SceneLook;

namespace {

bool approx(float a, float b, float eps = 1e-5f) { return a - b <= eps && b - a <= eps; }

// A look that differs from the default in every field.
SceneLook allChanged() {
    SceneLook l;
#define FTCORE_LOOK_CHANGE(name, def, lo, hi) l.name = (def) * 2.f + 1.f;
    FTCORE_LOOK_FIELDS(FTCORE_LOOK_CHANGE)
#undef FTCORE_LOOK_CHANGE
    return l;
}

} // namespace

TEST_CASE("The default look keeps the light scale and one warm cast") {
    const SceneLook look;
    // The local lights and emissive effects are tuned against a sun of a few lux under auto exposure.
    CHECK(look.sunLux == 6.f);
    // The warmth lives in the sun's colour, so the camera's white balance stays neutral.
    CHECK(look.sunKelvin < 6500.f);
    CHECK(look.whiteBalanceKelvin == 6500.f);
    // The engine's extended-range auto exposure defaults until the scenes are measured.
    CHECK(look.exposureMinEV100 == -10.f);
    CHECK(look.exposureMaxEV100 == 20.f);
    CHECK(look.fogForward > -0.9f);
    CHECK(look.fogForward < 0.9f);
    CHECK(look.hasVolumetricFog());
    CHECK(look.hasClouds());
}

TEST_CASE("A default look adds no wetness or glow and leaves the fog white") {
    const SceneLook look;
    CHECK(look.wetness == 0.f);
    CHECK(look.glowStrength == 0.f);
    CHECK(look.fogAlbedoR == 1.f);
    CHECK(look.fogAlbedoG == 1.f);
    CHECK(look.fogAlbedoB == 1.f);
    CHECK(look.practicalScatter == 1.f);
    CHECK(look.waterWaveStrength == 1.f);
}

TEST_CASE("A default look draws the atmosphere's own sky and keeps the engine's cloud layer") {
    const SceneLook look;
    CHECK(look.skyStars == 0.f);
    CHECK(look.skyGlow == 0.f);
    CHECK(look.skyFlat == 0.f);
    CHECK(look.windMps == 0.f);
    // No cover: the layer keeps the engine's material, at the engine's altitude and depth.
    CHECK(look.cloudCover == 0.f);
    CHECK(look.cloudBaseKm == 5.f);
    CHECK(look.cloudThickKm == 10.f);
    CHECK(look.cloudUnderglow == 0.f);
    // No swell, and with no wind no sea: the sea material, wherever a map puts it, lies flat.
    CHECK(look.seaSwellM == 0.f);
}

TEST_CASE("Every field takes part in ==") {
    const SceneLook base;
    const SceneLook changed = allChanged();
    int fields = 0;
#define FTCORE_LOOK_ONE(name, def, lo, hi)                                                                             \
    {                                                                                                          \
        SceneLook l = base;                                                                                    \
        l.name = changed.name;                                                                                 \
        CAPTURE(#name);                                                                                        \
        CHECK(l != base);                                                                                      \
        ++fields;                                                                                              \
    }
    FTCORE_LOOK_FIELDS(FTCORE_LOOK_ONE)
#undef FTCORE_LOOK_ONE
    CHECK(fields == 92);
    CHECK(ftcore::kSceneLookFieldCount == 92u);
    CHECK(base == SceneLook{});
}

TEST_CASE("lerp keeps its endpoints exactly and clamps t") {
    const SceneLook a;
    const SceneLook b = allChanged();
    CHECK(ftcore::lerp(a, b, 0.f) == a);
    CHECK(ftcore::lerp(a, b, 1.f) == b);
    CHECK(ftcore::lerp(a, b, -2.f) == a);
    CHECK(ftcore::lerp(a, b, 3.f) == b);
    const float ts[] = {0.f, 0.1f, 0.37f, 0.5f, 0.9f, 1.f};
    for (const float t : ts) {
        CAPTURE(t);
        CHECK(ftcore::lerp(a, a, t) == a);
        CHECK(ftcore::lerp(b, b, t) == b);
    }
}

TEST_CASE("lerp blends every field linearly") {
    const SceneLook a;
    const SceneLook b = allChanged();
    const SceneLook quarter = ftcore::lerp(a, b, 0.25f);
#define FTCORE_LOOK_QUARTER(name, def, lo, hi)                                                                         \
    CAPTURE(#name);                                                                                            \
    CHECK(approx(quarter.name, a.name + (b.name - a.name) * 0.25f));
    FTCORE_LOOK_FIELDS(FTCORE_LOOK_QUARTER)
#undef FTCORE_LOOK_QUARTER
}

TEST_CASE("lerp blends the surface, water, fog albedo, practical and glow fields") {
    SceneLook wet;
    wet.wetness = 0.8f;
    wet.glowStrength = 40.f;
    wet.waterAbsorbR = 2.45f;
    wet.fogAlbedoB = 0.5f;
    wet.practicalKelvin = 9200.f;
    const SceneLook mid = ftcore::lerp(SceneLook{}, wet, 0.5f);
    CHECK(approx(mid.wetness, 0.4f));
    CHECK(approx(mid.glowStrength, 20.f));
    CHECK(approx(mid.waterAbsorbR, 1.45f));
    CHECK(approx(mid.fogAlbedoB, 0.75f));
    CHECK(approx(mid.practicalKelvin, 6200.f, 1e-3f));
}

TEST_CASE("The switches follow the blended values") {
    SceneLook clear;
    clear.volumetricFog = 0.f;
    clear.clouds = 0.f;
    CHECK_FALSE(clear.hasVolumetricFog());
    CHECK_FALSE(clear.hasClouds());
    const SceneLook cloudy;
    // Volumetric fog fades in with its extinction; the cloud layer is on or off and flips halfway.
    CHECK(ftcore::lerp(clear, cloudy, 0.25f).hasVolumetricFog());
    CHECK_FALSE(ftcore::lerp(clear, cloudy, 0.25f).hasClouds());
    CHECK(ftcore::lerp(clear, cloudy, 0.5f).hasClouds());
    CHECK_FALSE(ftcore::lerp(cloudy, clear, 1.f).hasVolumetricFog());
}

TEST_CASE("Blending looks allocates nothing") {
    const SceneLook a;
    const SceneLook b = allChanged();
    float sink = 0.f;
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i < 240; ++i) {
        sink += ftcore::lerp(a, b, static_cast<float>(i) / 240.f).fogDensity;
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(sink > 0.f);
}
