#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "ftcore/SceneLook.h"

using ftcore::SceneLook;

namespace {

bool approx(float a, float b, float eps = 1e-5f) { return a - b <= eps && b - a <= eps; }

// A look that differs from the default in every field.
SceneLook allChanged() {
    SceneLook l;
#define FTCORE_LOOK_CHANGE(name, def) l.name = (def) * 2.f + 1.f;
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

TEST_CASE("Every field takes part in ==") {
    const SceneLook base;
    const SceneLook changed = allChanged();
    int fields = 0;
#define FTCORE_LOOK_ONE(name, def)                                                                             \
    {                                                                                                          \
        SceneLook l = base;                                                                                    \
        l.name = changed.name;                                                                                 \
        CAPTURE(#name);                                                                                        \
        CHECK(l != base);                                                                                      \
        ++fields;                                                                                              \
    }
    FTCORE_LOOK_FIELDS(FTCORE_LOOK_ONE)
#undef FTCORE_LOOK_ONE
    CHECK(fields == 17);
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
#define FTCORE_LOOK_QUARTER(name, def)                                                                         \
    CAPTURE(#name);                                                                                            \
    CHECK(approx(quarter.name, a.name + (b.name - a.name) * 0.25f));
    FTCORE_LOOK_FIELDS(FTCORE_LOOK_QUARTER)
#undef FTCORE_LOOK_QUARTER
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
