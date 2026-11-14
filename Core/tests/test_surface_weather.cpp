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
    // A puddle's edge darkens before it mirrors: no ring where colour, gloss and normal all change at once.
    CHECK(weatherRough(0.8f, 0.f, spec.puddleCoreFrom * 0.9f, spec) == doctest::Approx(0.8f));
    CHECK(weatherAlbedoScale(0, 0.f, spec.puddleCoreFrom * 0.9f, 0.f, spec) < 1.f);
    // The edge spans a real stretch of noise, not a hairline.
    CHECK(spec.puddleEdge >= 0.15f);
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

TEST_CASE("street noise: an integer lattice hash, so no cell edge tears anywhere in the city") {
    // At the city's own coordinates (the RD1 spot, 811 m by 644 m, on the asphalt's 2.5 cm aggregate cells) and at the
    // origin: either side of each of 2000 cell edges reads the same, the corners spread over 0..1 and are repeatable.
    const StreetSurfaceSpec st;
    for (const float x0 : {811.f, 0.5f}) {
        const float cx = std::floor(x0 / st.aggregateM), cy = std::floor((x0 * 0.79f) / st.aggregateM);
        double sum = 0.0;
        float lo = 1.f, hi = 0.f, worst = 0.f;
        for (int k = 0; k < 2000; ++k) {
            const float ex = cx + static_cast<float>(k), y = cy + 0.37f;
            // 1e-3 of a cell either side of the edge: the noise is C1, so the two may differ by its slope times 2e-3.
            worst = std::max(worst, std::fabs(streetNoise(ex - 1e-3f, y) - streetNoise(ex + 1e-3f, y)));
            const float v = streetLatticeValue(static_cast<int>(ex), static_cast<int>(cy));
            sum += v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            CHECK(streetLatticeValue(static_cast<int>(ex), static_cast<int>(cy)) == v);
        }
        CHECK(worst < 0.01f);
        CHECK(lo >= 0.f);
        CHECK(hi <= 1.f);
        CHECK(hi - lo > 0.95f);
        CHECK(sum / 2000.0 == doctest::Approx(0.5).epsilon(0.1));
    }
    // At a corner the noise is the corner's hash, from any of the four cells round it.
    CHECK(streetNoise(32440.f, 25760.f) == doctest::Approx(streetLatticeValue(32440, 25760)));
    CHECK(streetLatticeValue(-3, 7) != streetLatticeValue(3, 7));
    // Both masters draw it, not the float hash.
    SurfaceWeatherOptions o;
    o.grime = false;
    o.street = true;
    const std::string road = surfaceWeatherHlsl(SurfaceWeatherSpec{}, o);
    CHECK(road.find(streetLatticeHlsl()) != std::string::npos);
    CHECK(road.find("L(i + int2(1, 1))") != std::string::npos);
    CHECK(road.find("0.1031") == std::string::npos);
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
                             "A *= 1.0 - RlCav;", "R = lerp(R, max(R, RlRw), RlWet * (1.0 - Pud));",
                             "Rgh = lerp(Rgh, max(Rgh, RlRw), RlC);", "RlP = lerp(RlO, RlP, RlW);",
                             "max(length(G), 1e-6)", "lerp(0.45, 1.0, RlD)", "lerp(0.5, 1.0, RlD)", "lerp(0.6, 1.0, RlD)"})
        CHECK(relief.find(name) != std::string::npos);
    // The old mirror-glint wet line is gone, and the plain bevel's Fine no longer gates the relief's slope.
    CHECK(relief.find("lerp(R, 0.3, RlWet)") == std::string::npos);
    CHECK(relief.find("RlG * Fine") == std::string::npos);
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

TEST_CASE("HLSL: the asphalt detail tile is its own option and replaces the aggregate's near terms") {
    const SurfaceWeatherSpec spec;
    SurfaceWeatherOptions plain;
    plain.grime = false;
    plain.street = true;
    const std::string off = surfaceWeatherHlsl(spec, plain);
    for (const char* name : {"DetAlb", "DetNrm", "DetRA", "DetAO", "DUv"}) CHECK(off.find(name) == std::string::npos);
    CHECK(off.find("float AgF = Fine;") != std::string::npos);
    // Without the street layer the option changes nothing.
    SurfaceWeatherOptions noStreet;
    noStreet.streetDetail = true;
    CHECK(surfaceWeatherHlsl(spec, noStreet) == surfaceWeatherHlsl(spec, SurfaceWeatherOptions{}));

    SurfaceWeatherOptions on = plain;
    on.streetDetail = true;
    const std::string det = surfaceWeatherHlsl(spec, on);
    for (const char* name : {"float DetAO = 1.0;", "float AgF = 0.0;", "Texture2DSample(DetAlb, DetAlbSampler, DUv)",
                             "UnpackNormalMap(Texture2DSample(DetNrm, DetNrmSampler, DUv))",
                             "Texture2DSample(DetRA, DetRASampler, DUv)", "A *= DetAO;"})
        CHECK(det.find(name) != std::string::npos);
    // Declared once before the street block, multiplied into the AO once, sampled in the asphalt kind only.
    CHECK(countOf(det, "float DetAO") == 1);
    CHECK(countOf(det, "A *= DetAO;") == 1);
    CHECK(countOf(det, "Texture2DSample(DetAlb") == 1);
    // Ray-tracing safe: no screen derivatives added.
    CHECK(countOf(det, "fwidth") == countOf(off, "fwidth"));
    for (const char* name : {"ddx", "ddy"}) CHECK(det.find(name) == std::string::npos);
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

    // Lane pavers: a quarter of the tiles' shares.
    {
        const StreetJointGrid lane = streetJointGrid(StreetKind::Lane, st);
        CHECK(lane.flawScale == doctest::Approx(st.paverFlawScale));
        CHECK(st.paverFlawScale == doctest::Approx(0.25f));
        int ps = 0, pt = 0, pc = 0, ts = 0, tt = 0, tc = 0;
        for (int i = 0; i < 160; ++i)
            for (int j = 0; j < 160; ++j) {
                const float kx = static_cast<float>(i - 50), ky = static_cast<float>(j + 3);
                const StreetTileFlaw p = streetTileFlaw(kx, ky, st, lane.flawScale);
                const StreetTileFlaw t = streetTileFlaw(kx, ky, st);
                ps += p.sinkM > 0.f;
                pt += p.tiltX != 0.f || p.tiltY != 0.f;
                pc += p.chipR > 0.f;
                ts += t.sinkM > 0.f;
                tt += t.tiltX != 0.f || t.tiltY != 0.f;
                tc += t.chipR > 0.f;
                if (p.chipR > 0.f) {
                    CHECK(p.chipR >= st.chipMinM - 1e-6f);
                    CHECK(p.chipR <= st.chipMaxM + 1e-6f);
                }
            }
        CHECK(static_cast<float>(ps + pt + pc) / static_cast<float>(ts + tt + tc) == doctest::Approx(0.25f).epsilon(0.25));
        CHECK(ps > 0);
        CHECK(pc > 0);
    }

    // A sunk tile's face sits under its plain neighbour's; a joint's floor is its own depth (0.7..1.3 of the grid's).
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
        const float ex = (kx + 1.f) * pave.cellX;
        const StreetJointFactors jf = streetJointFactors(ex, cy, 0.f, 0.f, pave, st);
        const float zFloor = streetJointZ(ex, cy, 0.f, 0.f, pave, st);
        CHECK(std::fabs(zFloor + pave.depthM * jf.depthX) < 0.002f * pave.depthM);
    }
    CHECK(found);

    // A chipped corner falls to the joint floor at the corner and is whole on the far side of the tile.
    found = false;
    for (int i = 0; i < 4000 && !found; ++i) {
        const float kx = static_cast<float>(i), ky = 11.f;
        const StreetTileFlaw f = streetTileFlaw(kx, ky, st);
        if (f.chipR < 0.025f || f.sinkM > 0.f || f.tiltX != 0.f || f.tiltY != 0.f) continue;
        found = true;
        const float cx = (kx + 0.5f) * pave.cellX, cy = (ky + 0.5f) * pave.cellY;
        const auto at = [&](float in, float sx, float sy) {
            const float x = cx + sx * (0.5f * pave.cellX - in), y = cy + sy * (0.5f * pave.cellY - in);
            return streetJointSample(x, y, 0.f, 0.f, streetJointFactors(x, y, 0.f, 0.f, pave, st), 0.f, 0.f, pave, st);
        };
        // 7 mm in both ways (1 cm along the diagonal): wholly on the joint floor.
        CHECK(at(0.007f, f.chipX, f.chipY).floorShare == doctest::Approx(1.f));
        // 15 mm in both ways: a plain corner is up on its face, the chip still well down.
        const float plain = at(0.015f, -f.chipX, -f.chipY).floorShare;
        CHECK(plain < 0.1f);
        CHECK(at(0.015f, f.chipX, f.chipY).floorShare > plain + 0.15f);
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
    const float floorZ = streetJointZ(0.4f, 0.2f, 0.f, 0.f, pave, st);
    CHECK(floorZ <= -st.jointDepthMin * pave.depthM + 1e-6f);
    CHECK(floorZ >= -st.jointDepthMax * pave.depthM - 1e-6f);
    CHECK(p.x == doctest::Approx(0.4f + floorZ * vx / nv).epsilon(1e-4));
    CHECK(p.y == doctest::Approx(0.2f));
    // On a face: shifted only by that face's own height (none on a plain tile; tile key (0, 0) hashes to sunk).
    const StreetPoint face = streetReliefShift(0.2f, 0.2f, vx, 0.f, nv, 0.f, 0.f, pave, st);
    const float faceZ = streetJointZ(0.2f, 0.2f, 0.f, 0.f, pave, st);
    CHECK(std::fabs(face.x - (0.2f + faceZ * vx / nv)) < 1e-6f);
    const StreetPoint plainFace = streetReliefShift(0.2f, 0.2f, vx, 0.f, nv, 37.f, 5.f, pave, st);
    if (std::fabs(streetJointZ(0.2f, 0.2f, 37.f, 5.f, pave, st)) < 1e-7f) CHECK(std::fabs(plainFace.x - 0.2f) < 1e-6f);
    // Never more than the deepest joint * the clamped slope, any view; and with the walls' fold-back under a third of a
    // mm even at the full-weight glancing limit (under 0.05 mm from face-on to 60 degrees), over 24 joints (their own
    // widths and depths, the narrowest and deepest among them).
    const float deepest = st.jointDepthMax * pave.depthM;
    for (const float n : {1.f, 0.9f, 0.7f, 0.5f, 0.35f, 0.25f, 0.15f, 0.05f}) {
        const float t = std::sqrt(1.f - n * n);
        const float limit = deepest * t / std::max(n, st.parallaxSlopeClamp) + 1e-6f;
        float worst = 0.f;
        for (int k = 0; k < 24; ++k) {
            const float jx = 0.4f * static_cast<float>(1 + k % 6), y = 0.2f + 0.4f * static_cast<float>(k / 6);
            float last = -1.f, back = 0.f;
            for (int i = 0; i <= 600; ++i) {
                const float x = jx - 0.03f + i * 0.0001f;
                const StreetPoint q = streetReliefShift(x, y, t, 0.f, n, 0.f, 0.f, pave, st);
                CHECK(std::fabs(q.x - x) <= limit);
                if (i > 0) back = std::max(back, last - q.x);
                last = q.x;
            }
            worst = std::max(worst, back);
        }
        if (n >= 0.5f) CHECK(worst < 0.00005f);
        if (n >= st.parallaxNdVFull) CHECK(worst < 0.0003f);
    }
}

TEST_CASE("joint depth: varied joints, a grey gritty floor, no glints") {
    const StreetSurfaceSpec st;
    CHECK(st.jointWidthMin == doctest::Approx(0.7f));
    CHECK(st.jointWidthMax == doctest::Approx(1.4f));
    CHECK(st.jointDepthMin == doctest::Approx(0.7f));
    CHECK(st.jointDepthMax == doctest::Approx(1.3f));
    CHECK(st.jointCavityAO == doctest::Approx(0.6f));
    CHECK(st.jointOpenAO > st.jointCavityAO);

    // Width and depth factors stay in their ranges, vary from joint to joint and along a joint, and agree from both
    // sides of a joint's centreline (tiles, and pavers in running bond).
    for (const StreetKind kind : {StreetKind::Pavement, StreetKind::Lane}) {
        const StreetJointGrid g = streetJointGrid(kind, st);
        float wLo = 9.f, wHi = 0.f, dLo = 9.f, dHi = 0.f;
        int along = 0;
        for (int r = 0; r < 40; ++r)
            for (int k = 0; k < 40; ++k) {
                // The x joint k of row r, at the row's middle, and the y joint under row r at column k.
                const float rowShift = g.bond * static_cast<float>(r % 2);
                const float xj = (static_cast<float>(k) - rowShift) * g.cellX, ym = (static_cast<float>(r) + 0.5f) * g.cellY;
                const StreetJointFactors a = streetJointFactors(xj - 1e-4f, ym, 3.f, 5.f, g, st);
                const StreetJointFactors b = streetJointFactors(xj + 1e-4f, ym, 3.f, 5.f, g, st);
                CHECK(a.widthX == doctest::Approx(b.widthX).epsilon(1e-3));
                CHECK(a.depthX == doctest::Approx(b.depthX).epsilon(1e-3));
                // Continuous across the centreline in height too.
                CHECK(std::fabs(streetJointZ(xj - 1e-4f, ym, 3.f, 5.f, g, st) - streetJointZ(xj + 1e-4f, ym, 3.f, 5.f, g, st)) <
                      2e-5f);
                const float yj = static_cast<float>(r) * g.cellY, xm = (static_cast<float>(k) + 0.3f) * g.cellX;
                const StreetJointFactors c = streetJointFactors(xm, yj - 1e-4f, 3.f, 5.f, g, st);
                const StreetJointFactors d = streetJointFactors(xm, yj + 1e-4f, 3.f, 5.f, g, st);
                CHECK(c.widthY == doctest::Approx(d.widthY).epsilon(1e-3));
                CHECK(c.depthY == doctest::Approx(d.depthY).epsilon(1e-3));
                for (const float w : {a.widthX, c.widthY}) {
                    CHECK(w >= st.jointWidthMin - 1e-5f);
                    CHECK(w <= st.jointWidthMax + 1e-5f);
                    wLo = std::min(wLo, w);
                    wHi = std::max(wHi, w);
                }
                for (const float v : {a.depthX, c.depthY}) {
                    CHECK(v >= st.jointDepthMin - 1e-5f);
                    CHECK(v <= st.jointDepthMax + 1e-5f);
                    dLo = std::min(dLo, v);
                    dHi = std::max(dHi, v);
                }
                // Along the joint (a quarter cell on): the same joint, a different noise.
                const StreetJointFactors e = streetJointFactors(xj + 1e-4f, ym + 0.25f * g.cellY, 3.f, 5.f, g, st);
                along += std::fabs(e.widthX - b.widthX) > 0.005f;
            }
        CHECK(wHi - wLo > 0.4f);
        CHECK(dHi - dLo > 0.35f);
        CHECK(along > 600);
    }

    // The floor's tone: jointFloorToneLo..Hi of the unit clean and dry, never under Lo * dirt * wet.
    const float floorMin = st.jointFloorToneLo * st.jointDirtTone * st.jointWetTone;
    CHECK(floorMin >= 0.3f);
    for (int i = 0; i <= 10; ++i)
        for (int j = 0; j <= 10; ++j)
            for (int k = 0; k <= 10; ++k) {
                const float tone = streetJointFloorTone(i * 0.1f, j * 0.1f, k * 0.1f, st);
                CHECK(tone >= floorMin - 1e-6f);
                CHECK(tone <= st.jointFloorToneHi + 1e-6f);
                if (j == 0 && k == 0) CHECK(tone >= st.jointFloorToneLo - 1e-6f);
            }
    CHECK(st.jointFloorToneLo >= 0.45f - 1e-6f);
    CHECK(st.jointFloorToneHi <= 0.6f + 1e-6f);

    // The floor's roughness: at least 0.6 dry and 0.45 wet whatever it was, with the grit on top; a rougher one stays.
    for (int i = 0; i <= 10; ++i) {
        const float grit = i * 0.1f;
        CHECK(streetJointFloorRough(0.02f, grit, 0.f, st) >= 0.6f - 1e-6f);
        CHECK(streetJointFloorRough(0.02f, grit, 1.f, st) >= 0.45f - 1e-6f);
        CHECK(streetJointFloorRough(0.02f, grit, 0.5f, st) >= 0.45f - 1e-6f);
        CHECK(streetJointFloorRough(0.95f, grit, 1.f, st) == doctest::Approx(0.95f));
    }
    CHECK(streetJointFloorRough(0.f, 1.f, 0.f, st) > streetJointFloorRough(0.f, 0.f, 0.f, st));

    // The slope never tips past 60 degrees (kerbs, the deepest, reach the clamp), and is 0 off the joints.
    {
        const StreetJointGrid kerb = streetJointGrid(StreetKind::Kerb, st);
        const StreetJointGrid pave = streetJointGrid(StreetKind::Pavement, st);
        float most = 0.f;
        for (int gi = 0; gi < 2; ++gi)
            for (int k = 0; k < 40; ++k)
                for (int i = 0; i <= 400; ++i) {
                    const StreetJointGrid& g = gi == 0 ? kerb : pave;
                    const float x = g.cellX * static_cast<float>(k + 1) - 0.02f + i * 0.0001f;
                    const float y = gi == 0 ? 0.5f * g.cellY : 0.37f;
                    const StreetJointFactors f = streetJointFactors(x, y, 1.f, 2.f, g, st);
                    const StreetJointSample s = streetJointSample(x, y, 1.f, 2.f, f, 0.f, 0.f, g, st);
                    const float m = std::sqrt(s.gx * s.gx + s.gy * s.gy);
                    CHECK(m <= st.jointSlopeMax + 1e-4f);
                    most = std::max(most, m);
                }
        CHECK(st.jointSlopeMax == doctest::Approx(std::tan(3.14159265f / 3.f)));
        CHECK(most > 0.99f * st.jointSlopeMax);
    }

    // The joint's slope fades with the footprint across it: full near, gone once the groove is under a pixel; the
    // footprint across a joint is the pixel's for one across the view, less by nDotV for one along it.
    CHECK(streetJointFootprint(0.02f, 1.f, 0.f) == doctest::Approx(0.02f));
    CHECK(streetJointFootprint(0.02f, 0.3f, std::sqrt(1.f - 0.09f)) == doctest::Approx(0.02f));
    CHECK(streetJointFootprint(0.02f, 0.3f, 0.f) == doctest::Approx(0.006f));
    {
        const StreetJointGrid pave = streetJointGrid(StreetKind::Pavement, st);
        int checked = 0;
        for (int k = 0; k < 200 && checked < 20; ++k) {
            const float kx = static_cast<float>(k);
            const StreetTileFlaw fl = streetTileFlaw(kx, 0.f, st);
            if (fl.tiltX != 0.f || fl.tiltY != 0.f || fl.chipR > 0.f) continue;
            ++checked;
            const float x = (kx + 1.f) * pave.cellX - 0.012f, y = 0.2f;  // on the arris of tile kx's +x edge
            const StreetJointFactors f = streetJointFactors(x, y, 0.f, 0.f, pave, st);
            const float groove = 2.f * (pave.halfJointM * f.widthX + pave.bevelM);
            const StreetJointSample nearS = streetJointSample(x, y, 0.f, 0.f, f, 0.f, 0.f, pave, st);
            const StreetJointSample gone = streetJointSample(x, y, 0.f, 0.f, f, st.jointNormalTo * groove, 0.f, pave, st);
            CHECK(std::fabs(nearS.gx) > 0.05f);
            CHECK(std::fabs(gone.gx) < 1e-6f);
        }
        CHECK(checked == 20);
    }

    // AO: the open joints' floor lighter than the narrow, deep ones'.
    {
        const StreetJointGrid pave = streetJointGrid(StreetKind::Pavement, st);
        StreetJointFactors open, tight;
        open.widthX = st.jointWidthMax;
        open.depthX = st.jointDepthMin;
        tight.widthX = st.jointWidthMin;
        tight.depthX = st.jointDepthMax;
        CHECK(streetJointSample(0.4f, 0.2f, 0.f, 0.f, open, 0.f, 0.f, pave, st).open > 0.95f);
        CHECK(streetJointSample(0.4f, 0.2f, 0.f, 0.f, tight, 0.f, 0.f, pave, st).open < 0.05f);
    }
}
