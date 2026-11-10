// Weather on the world's surfaces (ftcore/SurfaceWeather.h): wet ground, puddles favouring gutters, contact grime and
// edge darkening, and the HLSL written from the same numbers.

#include "ftcore/SurfaceWeather.h"

#include <doctest/doctest.h>

#include <string>

using namespace ftcore;

TEST_CASE("wet: only up-facing pixels soak, drying patches stay damp, dry looks stay dry") {
    const SurfaceWeatherSpec spec;
    CHECK(weatherUp(1.f, spec) == doctest::Approx(1.f));
    CHECK(weatherUp(0.f, spec) == doctest::Approx(0.f));
    CHECK(weatherUp(-1.f, spec) == doctest::Approx(0.f));
    // A wall never soaks, whatever the wetness.
    CHECK(weatherSoak(1.f, weatherUp(0.f, spec), 1.f, spec) == doctest::Approx(0.f));
    // No look, no water.
    CHECK(weatherSoak(0.f, 1.f, 1.f, spec) == doctest::Approx(0.f));
    CHECK(weatherPuddle(0.f, 1.f, 1.f, 1.f, spec) == doctest::Approx(0.f));
    // A wet patch soaks fully, a drying one dampShare of it.
    CHECK(weatherSoak(1.f, 1.f, 1.f, spec) == doctest::Approx(1.f));
    CHECK(weatherSoak(1.f, 1.f, 0.f, spec) == doctest::Approx(spec.dampShare));
    CHECK(weatherSoak(0.6f, 1.f, 1.f, spec) == doctest::Approx(0.6f));
}

TEST_CASE("wet: full soak hits the brief's numbers, a half-wet street stays out of traced reflections") {
    const SurfaceWeatherSpec spec;
    CHECK(weatherAlbedoScale(0, 1.f, 0.f, 0.f, spec) == doctest::Approx(0.6f));
    CHECK(weatherRough(0.8f, 1.f, 0.f, spec) == doctest::Approx(0.1f));
    CHECK(weatherRough(0.8f, 0.f, 1.f, spec) == doctest::Approx(0.02f));
    // Lumen traces reflections under roughness 0.4: asphalt at 0.8 soaked to 0.6 (Solhaven's look) stays over it.
    CHECK(weatherRough(0.8f, 0.6f, 0.f, spec) > 0.4f);
    // Already smooth surfaces (glass) never get rougher from rain.
    CHECK(weatherRough(0.05f, 1.f, 0.f, spec) <= 0.05f);
    // Monotonic in soak.
    float last = 1.f;
    for (int i = 0; i <= 10; ++i) {
        const float r = weatherRough(0.8f, i / 10.f, 0.f, spec);
        CHECK(r <= last + 1e-6f);
        last = r;
    }
}

TEST_CASE("puddles: cover grows with wetness and favours the gutter of a cambered slab") {
    const SurfaceWeatherSpec spec;
    // A carriageway slab 3.5 m across, tilted down towards -Y (its Y axis rises): the low edge is at y = -1.75.
    const float tilt = 0.025f, size = 3.5f;
    CHECK(weatherGutter(tilt, -1.75f, size, spec) == doctest::Approx(1.f));
    CHECK(weatherGutter(tilt, 1.75f, size, spec) == doctest::Approx(0.f));
    CHECK(weatherGutter(tilt, -1.75f + spec.gutterM, size, spec) == doctest::Approx(0.f));
    // Mirrored tilt, mirrored gutter.
    CHECK(weatherGutter(-tilt, 1.75f, size, spec) == doctest::Approx(1.f));
    CHECK(weatherGutter(-tilt, -1.75f, size, spec) == doctest::Approx(0.f));
    // A flat box has no gutter.
    CHECK(weatherGutter(0.f, -1.75f, size, spec) == doctest::Approx(0.f));
    CHECK(weatherGutter(spec.tiltMin * 0.5f, -1.75f, size, spec) == doctest::Approx(0.f));

    // Count puddled samples over an even noise spread: more when wetter, more in the gutter, none on a wall.
    const auto share = [&](float wet, float gutter, float up) {
        int n = 0;
        for (int i = 0; i < 1000; ++i) n += weatherPuddle(wet, up, (i + 0.5f) / 1000.f, gutter, spec) > 0.5f ? 1 : 0;
        return n / 1000.f;
    };
    CHECK(share(0.6f, 0.f, 1.f) > share(0.2f, 0.f, 1.f));
    CHECK(share(0.6f, 1.f, 1.f) > share(0.6f, 0.f, 1.f));
    CHECK(share(1.f, 1.f, 0.f) == doctest::Approx(0.f));
    // Sane glossy share: at Solhaven's 0.6, open asphalt puddles under a third of an even spread (value noise clusters
    // around 0.5, so the real share is far lower), and a damp day (0.2) puddles only the noise's top tail.
    CHECK(share(0.6f, 0.f, 1.f) < 0.34f);
    CHECK(share(0.2f, 0.f, 1.f) < 0.1f);
}

TEST_CASE("dirt: grime at the foot of a wall, edges darker, thin pieces and tops clean") {
    const SurfaceWeatherSpec spec;
    const float wall = weatherSide(0.f);
    CHECK(wall == doctest::Approx(1.f));
    CHECK(weatherSide(1.f) == doctest::Approx(0.f));
    // At the ground, mid-face: the grime's full strength (noise 1).
    CHECK(weatherDirt(wall, 3.f, 0.f, 5.f, 1.f, spec) == doctest::Approx(spec.grimeStrength));
    // Gone by grimeHeightM, and falling on the way.
    CHECK(weatherDirt(wall, 3.f, spec.grimeHeightM, 5.f, 1.f, spec) == doctest::Approx(0.f));
    CHECK(weatherDirt(wall, 3.f, 0.3f, 5.f, 1.f, spec) < weatherDirt(wall, 3.f, 0.1f, 5.f, 1.f, spec));
    // Below the street floor (a building's buried base) it stays full.
    CHECK(weatherDirt(wall, 3.f, -1.f, 5.f, 1.f, spec) == doctest::Approx(spec.grimeStrength));
    // A corner darkens high up too.
    CHECK(weatherDirt(wall, 3.f, 10.f, 0.f, 1.f, spec) == doctest::Approx(spec.edgeStrength));
    CHECK(weatherDirt(wall, 3.f, 10.f, spec.edgeWidthM, 1.f, spec) == doctest::Approx(0.f));
    // A sign plate or pole takes none; a roof top takes none.
    CHECK(weatherDirt(wall, 0.05f, 0.f, 0.f, 1.f, spec) == doctest::Approx(0.f));
    CHECK(weatherDirt(weatherSide(1.f), 3.f, 0.f, 0.f, 1.f, spec) == doctest::Approx(0.f));
    // Never past 1, and it darkens every channel, warm (blue most).
    CHECK(weatherDirt(wall, 3.f, 0.f, 0.f, 1.f, spec) <= 1.f);
    CHECK(weatherAlbedoScale(2, 0.f, 0.f, 1.f, spec) < weatherAlbedoScale(0, 0.f, 0.f, 1.f, spec));
    CHECK(weatherAlbedoScale(0, 0.f, 0.f, 1.f, spec) < 1.f);
}

TEST_CASE("HLSL: inputs, outputs and options") {
    const SurfaceWeatherSpec spec;
    const std::string box = surfaceWeatherHlsl(spec, SurfaceWeatherOptions{});
    for (const char* name : {"Albedo", "Normal", "Rough", "AO", "WorldPos", "VertexNormal", "Wetness", "Amount", "LocalPos",
                             "AxisX", "AxisY", "AxisZ", "NormalOut =", "RoughOut =", "AOOut =", "return max(C",
                             "Lm.z + 0.5 * Sz", "Dirt", "Gut ="})
        CHECK(box.find(name) != std::string::npos);
    CHECK(box.find("Above") == std::string::npos);
    CHECK(box.find("Noise[3]") != std::string::npos);

    SurfaceWeatherOptions face;
    face.aboveInput = true;
    const std::string win = surfaceWeatherHlsl(spec, face);
    CHECK(win.find("float Height = Above;") != std::string::npos);
    CHECK(win.find("Lm.z + 0.5 * Sz") == std::string::npos);

    // The plain master (roads): wet and gutters, no dirt.
    SurfaceWeatherOptions plain;
    plain.grime = false;
    const std::string road = surfaceWeatherHlsl(spec, plain);
    CHECK(road.find("Dirt") == std::string::npos);
    CHECK(road.find("Gut = abs(Uy.z)") != std::string::npos);
    CHECK(road.find("Noise[2]") != std::string::npos);

    // No box frame (ground): no local inputs at all, so dirt is off even when asked.
    SurfaceWeatherOptions ground;
    ground.boxFrame = false;
    const std::string g = surfaceWeatherHlsl(spec, ground);
    for (const char* name : {"LocalPos", "AxisX", "AxisY", "AxisZ", "Dirt", "Sx"}) CHECK(g.find(name) == std::string::npos);

    // The spec's numbers are spliced, as floats (an HLSL int would truncate a division), and the output is deterministic.
    CHECK(box.find("WorldPos.xy / 350.0") != std::string::npos);
    CHECK(box.find("smoothstep(0.0, 0.600000024, GutD)") != std::string::npos);
    CHECK(box == surfaceWeatherHlsl(spec, SurfaceWeatherOptions{}));
    SurfaceWeatherSpec other;
    other.puddleCover = 0.25f;
    CHECK(surfaceWeatherHlsl(other, SurfaceWeatherOptions{}) != box);
}
