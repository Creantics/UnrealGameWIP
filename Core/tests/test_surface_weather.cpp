// Weather on the world's surfaces (ftcore/SurfaceWeather.h): wet ground, puddles favouring gutters, contact grime and
// edge darkening, and the HLSL written from the same numbers.

#include "ftcore/SurfaceWeather.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

namespace {
int countOf(const std::string& s, const char* what) {
    int n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
    return n;
}
}  // namespace

TEST_CASE("HLSL: joint depth is its own option; off, the node is as it was") {
    const SurfaceWeatherSpec spec;
    SurfaceWeatherOptions plain;
    plain.grime = false;
    plain.street = true;
    const std::string off = surfaceWeatherHlsl(spec, plain);
    // Off: none of its inputs or names (the output was diffed byte for byte against the generator before the option).
    for (const char* name : {"ViewVec", "JointDepth", "FTRelief", "RlCav", "RlWet", "RlW", "RlP"})
        CHECK(off.find(name) == std::string::npos);
    // Asked for without the street layer: nothing changes either.
    SurfaceWeatherOptions noStreet;
    noStreet.jointDepth = true;
    CHECK(surfaceWeatherHlsl(spec, noStreet) == surfaceWeatherHlsl(spec, SurfaceWeatherOptions{}));

    SurfaceWeatherOptions on = plain;
    on.jointDepth = true;
    const std::string relief = surfaceWeatherHlsl(spec, on);
    for (const char* name : {"ViewVec / 100.0", "saturate(JointDepth)", "struct FTRelief", "if (RlW > 0.001)",
                             "A *= 1.0 - ", "R = min(R, lerp(R, 0.3, RlWet));", "RlP = lerp(RlO, RlP, RlW);"})
        CHECK(relief.find(name) != std::string::npos);
    // The solve runs in each jointed kind (pavers, pavement, kerbs), behind the near-only branch.
    CHECK(countOf(relief, "if (RlW > 0.001)") == 3);
    // A ray-tracing hit shader has no screen derivatives: the footprint's one fwidth stays the only one.
    CHECK(countOf(relief, "fwidth") == countOf(off, "fwidth"));
    CHECK(countOf(relief, "fwidth") == 1);
    for (const char* name : {"ddx", "ddy"}) CHECK(relief.find(name) == std::string::npos);
    // The depths are spliced (pavement, pavers, kerbs).
    const StreetSurfaceSpec st;
    for (const float d : {st.paveJointDepthM, st.paverJointDepthM, st.kerbJointDepthM}) {
        char buf[48];
        std::snprintf(buf, sizeof buf, ", %.9g, ", static_cast<double>(d));
        CHECK(relief.find(buf) != std::string::npos);
    }
}

TEST_CASE("joint depth: depths, an imperfect grid and its shares") {
    const StreetSurfaceSpec st;
    CHECK(st.paveJointDepthM >= 0.005f);
    CHECK(st.paveJointDepthM <= 0.008f);
    CHECK(st.paverJointDepthM >= 0.005f);
    CHECK(st.paverJointDepthM <= 0.008f);
    CHECK(st.kerbJointDepthM >= 0.005f);
    CHECK(st.kerbJointDepthM <= 0.008f);
    CHECK(st.tiltMaxRad < 0.0175f);  // well under a degree
    const StreetJointGrid pave = streetJointGrid(StreetKind::Pavement, st);
    CHECK(pave.depthM == st.paveJointDepthM);
    CHECK(streetJointGrid(StreetKind::Lane, st).bond == doctest::Approx(0.5f));
    CHECK(streetJointGrid(StreetKind::Kerb, st).depthM == st.kerbJointDepthM);

    // Shares over 4096 tiles: sunk, tilted, chipped near the spec's.
    int sunk = 0, tilted = 0, chipped = 0;
    for (int i = 0; i < 64; ++i)
        for (int j = 0; j < 64; ++j) {
            const StreetTileFlaw f = streetTileFlaw(static_cast<float>(i - 20), static_cast<float>(j + 7), st);
            // Stable per tile: a pure function of the key.
            const StreetTileFlaw g = streetTileFlaw(static_cast<float>(i - 20), static_cast<float>(j + 7), st);
            CHECK(g.sinkM == f.sinkM);
            CHECK(g.chipR == f.chipR);
            if (f.sinkM > 0.f) {
                ++sunk;
                CHECK(f.sinkM >= st.sinkMinM - 1e-6f);
                CHECK(f.sinkM <= st.sinkMaxM + 1e-6f);
            }
            if (f.tiltX != 0.f || f.tiltY != 0.f) {
                ++tilted;
                CHECK(std::sqrt(f.tiltX * f.tiltX + f.tiltY * f.tiltY) <= st.tiltMaxRad + 1e-6f);
            }
            if (f.chipR > 0.f) {
                ++chipped;
                CHECK(f.chipR >= st.chipMinM - 1e-6f);
                CHECK(f.chipR <= st.chipMaxM + 1e-6f);
            }
        }
    const float n = 4096.f;
    CHECK((sunk + tilted) / n == doctest::Approx(st.sunkShare + st.tiltShare).epsilon(0.25));
    CHECK(sunk / n == doctest::Approx(st.sunkShare).epsilon(0.3));
    CHECK(chipped / n == doctest::Approx(st.chipShare).epsilon(0.3));

    // A sunk tile's face sits under its plain neighbour's; a joint's floor is the full depth either way.
    bool found = false;
    for (int i = 0; i < 400 && !found; ++i) {
        const float kx = static_cast<float>(i), ky = 3.f;
        const StreetTileFlaw f = streetTileFlaw(kx, ky, st);
        const StreetTileFlaw nb = streetTileFlaw(kx + 1.f, ky, st);
        const bool plainNb = nb.sinkM == 0.f && nb.tiltX == 0.f && nb.tiltY == 0.f && nb.chipR == 0.f;
        if (f.sinkM <= 0.f || f.chipR > 0.f || !plainNb) continue;
        found = true;
        // Key offset 0: tile (kx, ky) spans x in [kx, kx + 1) * 0.4.
        const float cx = (kx + 0.5f) * pave.cellX, cy = (ky + 0.5f) * pave.cellY;
        const float zSunk = streetJointZ(cx, cy, 0.f, 0.f, pave, st);
        const float zNb = streetJointZ(cx + pave.cellX, cy, 0.f, 0.f, pave, st);
        CHECK(std::fabs(zNb) < 1e-6f);
        CHECK(std::fabs(zSunk + f.sinkM) < 1e-6f);
        CHECK(zSunk < zNb - 0.0009f);
        CHECK(std::fabs(streetJointZ((kx + 1.f) * pave.cellX, cy, 0.f, 0.f, pave, st) + pave.depthM) < 1e-6f);
    }
    CHECK(found);

    // A chipped corner falls to the joint floor at the corner and is whole on the far side of the tile.
    found = false;
    for (int i = 0; i < 2000 && !found; ++i) {
        const float kx = static_cast<float>(i), ky = 11.f;
        const StreetTileFlaw f = streetTileFlaw(kx, ky, st);
        if (f.chipR <= 0.f || f.sinkM > 0.f || f.tiltX != 0.f || f.tiltY != 0.f) continue;
        found = true;
        const float cx = (kx + 0.5f) * pave.cellX, cy = (ky + 0.5f) * pave.cellY;
        // 7 mm in from the chipped corner both ways (1 cm along the diagonal): on the joint floor, where a plain
        // corner is already up on its arris.
        const auto at = [&](float in, float sx, float sy) {
            return streetJointZ(cx + sx * (0.5f * pave.cellX - in), cy + sy * (0.5f * pave.cellY - in), 0.f, 0.f, pave, st);
        };
        CHECK(std::fabs(at(0.007f, f.chipX, f.chipY) + pave.depthM) < 1e-6f);
        CHECK(at(0.007f, -f.chipX, -f.chipY) > -pave.depthM + 0.0002f);
        // 12 mm in both ways: a plain corner is nearly up, the chip still at least a millimetre lower.
        const float plain = -pave.depthM + pave.depthM * streetTileHeight(0.012f, st);
        CHECK(std::fabs(at(0.012f, -f.chipX, -f.chipY) - plain) < 1e-6f);
        CHECK(at(0.012f, f.chipX, f.chipY) < plain - 0.001f);
    }
    CHECK(found);
}

TEST_CASE("joint depth: parallax fades and the relief solve") {
    const StreetSurfaceSpec st;
    // Full near and face-on, gone by 15 m, by a glancing 0.12, with the switch off or the fine detail gone.
    CHECK(streetParallaxWeight(5.f, 1.f, 1.f, 1.f, st) == doctest::Approx(1.f));
    CHECK(streetParallaxWeight(st.parallaxNearM, 1.f, 1.f, 1.f, st) == doctest::Approx(1.f));
    CHECK(streetParallaxWeight(st.parallaxFarM, 1.f, 1.f, 1.f, st) == doctest::Approx(0.f));
    CHECK(streetParallaxWeight(13.5f, 1.f, 1.f, 1.f, st) > 0.f);
    CHECK(streetParallaxWeight(13.5f, 1.f, 1.f, 1.f, st) < 1.f);
    CHECK(streetParallaxWeight(5.f, st.parallaxNdVFull, 1.f, 1.f, st) == doctest::Approx(1.f));
    CHECK(streetParallaxWeight(5.f, st.parallaxNdVNone, 1.f, 1.f, st) == doctest::Approx(0.f));
    CHECK(streetParallaxWeight(5.f, 1.f, 0.f, 1.f, st) == doctest::Approx(0.f));
    CHECK(streetParallaxWeight(5.f, 1.f, 1.f, 0.f, st) == doctest::Approx(0.f));
    CHECK(st.parallaxNearM == doctest::Approx(12.f));
    CHECK(st.parallaxFarM == doctest::Approx(15.f));
    CHECK(st.parallaxNdVFull == doctest::Approx(0.25f));
    CHECK(st.parallaxNdVNone == doctest::Approx(0.12f));

    const StreetJointGrid pave = streetJointGrid(StreetKind::Pavement, st);
    // Straight down: no shift anywhere.
    for (int i = 0; i < 200; ++i) {
        const float x = 0.37f + i * 0.0003f;
        const StreetPoint p = streetReliefShift(x, 0.2f, 0.f, 0.f, 1.f, 0.f, 0.f, pave, st);
        CHECK(p.x == doctest::Approx(x));
        CHECK(p.y == doctest::Approx(0.2f));
    }
    // On a joint's floor (x = 0.4, mid-edge of tiles 0 and 1), the viewer toward +x: the point moves away from the
    // viewer by depth * slope (it stays on the floor, so the solve lands exactly there).
    const float nv = 0.95f, vx = std::sqrt(1.f - nv * nv);
    const StreetPoint p = streetReliefShift(0.4f, 0.2f, vx, 0.f, nv, 0.f, 0.f, pave, st);
    CHECK(p.x == doctest::Approx(0.4f - pave.depthM * vx / nv).epsilon(1e-4));
    CHECK(p.y == doctest::Approx(0.2f));
    // On a face: shifted only by that face's own height (none on a plain tile; tile key (0, 0) hashes to sunk).
    const StreetPoint face = streetReliefShift(0.2f, 0.2f, vx, 0.f, nv, 0.f, 0.f, pave, st);
    const float faceZ = streetJointZ(0.2f, 0.2f, 0.f, 0.f, pave, st);
    CHECK(std::fabs(face.x - (0.2f + faceZ * vx / nv)) < 1e-6f);
    const StreetPoint plainFace = streetReliefShift(0.2f, 0.2f, vx, 0.f, nv, 37.f, 5.f, pave, st);
    if (std::fabs(streetJointZ(0.2f, 0.2f, 37.f, 5.f, pave, st)) < 1e-7f) CHECK(std::fabs(plainFace.x - 0.2f) < 1e-6f);
    // Never more than depth * the clamped slope, any view; and with the walls' fold-back under a third of a mm
    // even at the full-weight glancing limit (monotone from face-on to 60 degrees).
    for (const float n : {1.f, 0.9f, 0.7f, 0.5f, 0.35f, 0.25f, 0.15f, 0.05f}) {
        const float t = std::sqrt(1.f - n * n);
        const float limit = pave.depthM * t / std::max(n, st.parallaxSlopeClamp) + 1e-6f;
        float last = -1.f, back = 0.f;
        for (int i = 0; i <= 600; ++i) {
            const float x = 0.37f + i * 0.0001f;
            const StreetPoint q = streetReliefShift(x, 0.2f, t, 0.f, n, 0.f, 0.f, pave, st);
            CHECK(std::fabs(q.x - x) <= limit);
            if (i > 0) back = std::max(back, last - q.x);
            last = q.x;
        }
        if (n >= 0.5f) CHECK(back <= 1e-6f);
        if (n >= st.parallaxNdVFull) CHECK(back < 0.0003f);
    }
}
