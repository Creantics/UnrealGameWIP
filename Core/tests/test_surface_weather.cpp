// Weather on the world's surfaces (ftcore/SurfaceWeather.h): wet ground, puddles favouring gutters, contact grime and
// edge darkening, and the HLSL written from the same numbers.

#include "ftcore/SurfaceWeather.h"

#include <doctest/doctest.h>

#include <algorithm>
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

TEST_CASE("wet: full soak's numbers; a night street (0.6) mirrors in its wet patches, a damp day (0.35) does not") {
    const SurfaceWeatherSpec spec;
    CHECK(weatherAlbedoScale(0, 1.f, 0.f, 0.f, spec) == doctest::Approx(0.5f));
    CHECK(weatherRough(0.8f, 1.f, 0.f, spec) == doctest::Approx(0.05f));
    CHECK(weatherRough(0.8f, 0.f, 1.f, spec) == doctest::Approx(0.02f));
    // Lumen traces reflections under roughness 0.4. Asphalt (0.85) soaked to the night's 0.6 lands under it, so lamps
    // and signs streak on the wet carriageway; its drying patches (dampShare of the soak) and the day's 0.35 stay over.
    CHECK(weatherRough(0.85f, 0.6f, 0.f, spec) < 0.35f);
    CHECK(weatherRough(0.85f, 0.6f * spec.dampShare, 0.f, spec) > 0.4f);
    CHECK(weatherRough(0.85f, 0.35f, 0.f, spec) > 0.4f);
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
    // Sane glossy share: at Solhaven's 0.6, open asphalt puddles under 42% of an even spread (value noise clusters
    // around 0.5, so the real share is far lower), and a damp day (0.2) puddles only the noise's top tail.
    CHECK(share(0.6f, 0.f, 1.f) < 0.42f);
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
    CHECK(road.find("float Dirt") == std::string::npos);
    CHECK(road.find("TopDirt") != std::string::npos);
    CHECK(road.find("Street") == std::string::npos);
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

TEST_CASE("top grime: a ground slab darkens along its edge, other boxes stay clean") {
    const SurfaceWeatherSpec spec;
    CHECK(weatherGroundSlab(0.3f, 3.f, 2.f, spec) == doctest::Approx(1.f));
    CHECK(weatherGroundSlab(3.f, 3.f, 3.f, spec) == doctest::Approx(0.f));  // a building
    CHECK(weatherGroundSlab(0.3f, 1.f, 5.f, spec) == doctest::Approx(0.f)); // a kerb or rail
    CHECK(weatherTopDirt(0.f, 1.f, 1.f, spec) == doctest::Approx(spec.topGrimeStrength));
    CHECK(weatherTopDirt(spec.topGrimeM, 1.f, 1.f, spec) == doctest::Approx(0.f));
    CHECK(weatherTopDirt(0.1f, 1.f, 1.f, spec) < weatherTopDirt(0.05f, 1.f, 1.f, spec));
    CHECK(weatherTopDirt(0.f, 1.f, 0.f, spec) == doctest::Approx(0.f));
    CHECK(weatherTopDirt(0.f, 0.f, 1.f, spec) < weatherTopDirt(0.f, 1.f, 1.f, spec));
}

TEST_CASE("street: wheel paths, joints, paint wear and tiles") {
    const StreetSurfaceSpec st;
    // One 3.5 m lane: wheel paths 0.85 m either side of its middle, none in the middle.
    CHECK(streetWheelPath(1.75f - st.wheelOffM, 3.5f, st) == doctest::Approx(1.f));
    CHECK(streetWheelPath(1.75f + st.wheelOffM, 3.5f, st) == doctest::Approx(1.f));
    CHECK(streetWheelPath(1.75f, 3.5f, st) < 0.01f);
    // Two lanes on a 7 m half: a path near each lane's middle.
    CHECK(streetWheelPath(1.75f + st.wheelOffM, 7.f, st) == doctest::Approx(1.f));
    CHECK(streetWheelPath(5.25f - st.wheelOffM, 7.f, st) == doctest::Approx(1.f));
    // Joints: every 20-40 m, the same for both halves of a road (they depend on the run and the length alone).
    int joints = 0;
    float worst = 0.f;
    for (int i = 0; i < 30000; ++i) {
        const float at = i * 0.01f;
        const float d = streetJointDistance(at, 300.f, st);
        worst = std::max(worst, d);
        if (d < 0.005f) ++joints;
        CHECK(d == streetJointDistance(at, 300.f, st));
    }
    CHECK(worst <= 20.01f);
    CHECK(joints >= 7);
    CHECK(joints <= 15);
    // Paint: fresh stays, faded and crossed by wheels goes.
    CHECK(streetPaintLeft(0.f, 0.f, 0.5f) == doctest::Approx(1.f));
    CHECK(streetPaintLeft(1.f, 1.f, 0.5f) == doctest::Approx(0.f));
    CHECK(streetPaintLeft(0.5f, 1.f, 0.5f) < streetPaintLeft(0.5f, 0.f, 0.5f));
    // A tile: down in its joint, up on its face.
    CHECK(streetTileHeight(0.f, st) == doctest::Approx(0.f));
    CHECK(streetTileHeight(st.tileJointM + st.tileBevelM, st) == doctest::Approx(1.f));
}

TEST_CASE("HLSL: the street layer is an option, branches on Street and keeps the normal at Street 0") {
    const SurfaceWeatherSpec spec;
    SurfaceWeatherOptions plain;
    plain.grime = false;
    plain.street = true;
    const std::string road = surfaceWeatherHlsl(spec, plain);
    for (const char* name : {"Street", "if (SK > 0)", "SK == 1", "SK == 2", "SK == 3", "SK == 4", "SK == 5", "fwidth", "TopDirt",
                             "float3 Nrm = Normal;", "NormalOut = normalize(lerp(Nrm"})
        CHECK(road.find(name) != std::string::npos);
    // The paint's albedo is spliced.
    CHECK(road.find("0.75 / Mx") != std::string::npos);
    CHECK(road.find("frac(Lm.xy / 0.06)") != std::string::npos); // the ramps' tactile strips get domes, not wear
    // Off without a box frame.
    plain.boxFrame = false;
    CHECK(surfaceWeatherHlsl(spec, plain).find("Street") == std::string::npos);
    // The decal snippet: the same noise and numbers.
    const std::string wet = surfaceWetHlsl(spec);
    for (const char* name : {"WetIn", "GutIn", "WSoak", "WPud", "C *=", "R = lerp(R"}) CHECK(wet.find(name) != std::string::npos);
    CHECK(wet.find("WorldPos.xy / 350.0") != std::string::npos);
}
