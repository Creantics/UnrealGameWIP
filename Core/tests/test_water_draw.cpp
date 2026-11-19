// The drawn water: the tiles, the draw groups, and the material's copy of the waves (Water/Surface.ush), compiled here as
// C++ against a small shim so its offset is checked against ftcore::waterWaveOffset and the game's height query.

#include <doctest/doctest.h>

#include "ftcore/WaterDraw.h"
#include "AllocCounter.h"

#include <cmath>
#include <limits>

namespace ftwatershader {

struct float2 {
    float x, y;
    float2(float a, float b) : x(a), y(b) {}
};
struct float3 {
    float x, y, z;
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct float4 {
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
};

inline float max(float a, float b) { return a > b ? a : b; }
inline float length(float2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
inline float smoothstep(float e0, float e1, float v) {
    float t = (v - e0) / (e1 - e0);
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    return t * t * (3.f - 2.f * t);
}
inline float sqrt(float v) { return std::sqrt(v); }
inline float sin(float v) { return std::sin(v); }
inline float cos(float v) { return std::cos(v); }
inline float fmod(float a, float b) { return std::fmod(a, b); }

#define FT_UNROLL
#include "../../Plugins/FTLook/Shaders/Private/Water/Surface.ush"
#undef FT_UNROLL

} // namespace ftwatershader

namespace {

using namespace ftcore;
namespace sh = ftwatershader;

WaterBody canalBox(float flow) {
    WaterBody b;
    b.kind = WaterKind::Canal;
    b.shape = WaterShape::Box;
    b.level = 1.f;
    b.bed = -2.f;
    b.centre = {40.f, -12.f};
    b.half = {30.f, 4.f};
    const float yaw = 0.6f;
    b.cosYaw = std::cos(yaw);
    b.sinYaw = std::sin(yaw);
    b.flow = {flow, 0.f};
    return b;
}

WaterBody seaPlane() {
    WaterBody b;
    b.kind = WaterKind::Sea;
    b.shape = WaterShape::HalfPlane;
    b.level = 0.f;
    b.bed = -20.f;
    b.centre = {0.f, 100.f};
    b.cosYaw = 0.f;
    b.sinYaw = 1.f;
    return b;
}

WaterSurfaceModel stormModel() {
    WaterSurfaceModel m;
    SceneLook look;
    look.windMps = 7.f;
    look.seaSwellM = 0.8f;
    m.sets = waterWaveSets(look, 0.7f, m.spec.waves);
    m.totalHours = 21.0;
    return m;
}

struct Packed {
    sh::float4 w[8];
};

Packed pack(const WaterDrawParams& p) {
    Packed out;
    for (int i = 0; i < 8; ++i) {
        out.w[i].x = p.waves[i][0];
        out.w[i].y = p.waves[i][1];
        out.w[i].z = p.waves[i][2];
        out.w[i].w = p.waves[i][3];
    }
    return out;
}

// The shader's offset at a rest point of a body, cm.
sh::float3 shaderOffset(const WaterSurfaceModel& m, const WaterBody& b, float x0, float y0, float t, float gridCm) {
    const WaterDrawGroup g = waterDrawGroup(b, m.spec.waves);
    const WaterDrawParams p = waterDrawParams(m, g);
    Packed w = pack(p);
    float line[kWaterEdgeFloats];
    waterEdgeLine(b, line);
    sh::float4 edgeA;
    edgeA.x = line[0];
    edgeA.y = line[1];
    edgeA.z = line[2];
    edgeA.w = line[3];
    const float edge = sh::FTWaterEdgeCm(sh::float2(x0, y0), edgeA, sh::float2(line[4], line[5]));
    const float sway = sh::FTWaterSway(edge, p.fadeCm);
    return sh::FTWaterOffset(sh::float2(x0, y0), t, w.w, p.q, gridCm, sway, p.lapShare);
}

sh::float4 f4(float x, float y, float z, float w) {
    sh::float4 v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.w = w;
    return v;
}

// What the masters' offset node did before the sea span (FTWaterSurfaceBuild's kOffsetCode up to 4bb6f18f), kept as the
// reference for the tiles' inside: the waves faded with the eye's distance and over a sea's last 150 m out (ReachCm),
// the tide, the skirt's fold.
sh::float3 oldTileOffset(sh::float3 rest, sh::float2 grid, sh::float3 eye, float t, sh::float4 w[8], float q, float fadeCm,
                         float lapShare, sh::float4 edgeA, sh::float2 edgeB, float waveVerts, float farStartCm, float farEndCm,
                         float tideCm, float reachCm) {
    const sh::float2 r(rest.x, rest.y);
    const float edgeCm = sh::FTWaterEdgeCm(r, edgeA, edgeB);
    float keep = waveVerts * (1.f - sh::smoothstep(farStartCm, farEndCm, sh::length(sh::float2(eye.x - r.x, eye.y - r.y))));
    if (reachCm > 0.f && edgeB.y > 0.5f && edgeB.y < 1.5f) keep *= 1.f - sh::smoothstep(reachCm - 15000.f, reachCm, edgeCm);
    sh::float3 o(0.f, 0.f, 0.f);
    if (keep > 0.f) {
        const sh::float3 waves = sh::FTWaterOffset(r, t, w, q, sh::max(grid.x, 1.f), sh::FTWaterSway(edgeCm, fadeCm), lapShare);
        o = sh::float3(waves.x * keep, waves.y * keep, waves.z * keep);
    }
    o.z += tideCm;
    if (grid.y > 0.f && eye.z < rest.z + grid.y + tideCm) o.z += grid.y;
    return o;
}

// A body's edge line and sea span as the material reads them.
struct TileInputs {
    sh::float4 edgeA;
    sh::float2 edgeB{0.f, 2.f};
    sh::float4 span;
};

TileInputs tileInputs(const WaterBody& b, const WaterTileSpec& spec, const WaterTileBounds& bounds) {
    TileInputs in;
    float line[kWaterEdgeFloats];
    waterEdgeLine(b, line);
    in.edgeA = f4(line[0], line[1], line[2], line[3]);
    in.edgeB = sh::float2(line[4], line[5]);
    float span[kWaterSpanFloats];
    waterSeaSpanCm(b, spec, bounds, span);
    in.span = f4(span[0], span[1], span[2], span[3]);
    return in;
}

// The rest point (cm) out and along a sea's shore (m) from its centre.
sh::float2 shorePoint(const WaterBody& b, float outM, float alongM) {
    return sh::float2((b.centre.x + outM * b.cosYaw - alongM * b.sinYaw) * 100.f, (b.centre.y + outM * b.sinYaw + alongM * b.cosYaw) * 100.f);
}

} // namespace

TEST_CASE("Water/Surface.ush moves a vertex as waterWaveOffset does") {
    const WaterSurfaceModel m = stormModel();
    const WaterBody bodies[3] = {seaPlane(), canalBox(0.8f), canalBox(0.f)};
    float worst = 0.f;
    for (const WaterBody& b : bodies) {
        const WaterWaveClass c = waterWaveClass(b, m.spec.waves);
        for (int k = 0; k < 40; ++k) {
            const float x0 = b.centre.x * 100.f + static_cast<float>((k * 397) % 2000 - 1000) * 1.3f;
            const float y0 = b.centre.y * 100.f + static_cast<float>((k * 733) % 1600 - 800) * 0.7f;
            const float t = 13.f + 97.3f * static_cast<float>(k);
            float cpu[3];
            waterWaveOffset(m.sets, c, waterEdgeM(b, {x0 * 0.01f, y0 * 0.01f}) * 100.f, x0, y0, t, cpu);
            const sh::float3 gpu = shaderOffset(m, b, x0, y0, t, m.sets.gridCm);
            worst = std::max(worst, std::fabs(cpu[0] - gpu.x) + std::fabs(cpu[1] - gpu.y) + std::fabs(cpu[2] - gpu.z));
        }
    }
    CHECK(worst < 0.2f);
}

TEST_CASE("The drawn vertex stands at the game's surface height, to 2 cm") {
    const WaterSurfaceModel m = stormModel();
    WaterBodies water;
    water.add(seaPlane());
    water.add(canalBox(0.8f));
    water.add(canalBox(0.f));
    water.setSurface(&m);
    float worst = 0.f;
    for (std::size_t i = 0; i < water.count(); ++i) {
        const WaterBody& b = water.body(i);
        const float tide = waterTideM(m.spec.tide, b, waterWaveClass(b, m.spec.waves), m.totalHours);
        CHECK(waterDrawParams(m, waterDrawGroup(b, m.spec.waves)).tideM == doctest::Approx(tide).epsilon(1e-5));
        for (int k = 0; k < 30; ++k) {
            const float x0 = b.centre.x * 100.f + static_cast<float>((k * 211) % 1800 - 900);
            const float y0 = b.centre.y * 100.f + static_cast<float>((k * 157) % 600 - 300);
            const float t = 5.f + 61.7f * static_cast<float>(k);
            const sh::float3 o = shaderOffset(m, b, x0, y0, t, m.sets.gridCm);
            const Vec2 drawn{(x0 + o.x) * 0.01f, (y0 + o.y) * 0.01f};
            if (!b.covers(drawn)) continue;
            const float drawnZcm = (b.level + tide) * 100.f + o.z;
            worst = std::max(worst, std::fabs(water.surfaceOf(i, drawn, t) * 100.f - drawnZcm));
        }
    }
    CHECK(worst < 2.f);
}

TEST_CASE("Water/Surface.ush's normal is the cross of the moved grid's tangents") {
    const WaterSurfaceModel m = stormModel();
    const WaterBody b = seaPlane();
    const WaterDrawParams p = waterDrawParams(m, WaterDrawGroup::Sea);
    Packed w = pack(p);
    const float sway = 1.f;
    for (int k = 0; k < 10; ++k) {
        const float x0 = 200.f * static_cast<float>(k), y0 = 15000.f + 37.f * static_cast<float>(k), t = 3.f + 11.f * static_cast<float>(k);
        const float e = 0.5f;
        auto at = [&](float x, float y) { return sh::FTWaterOffset(sh::float2(x, y), t, w.w, p.q, 0.01f, sway, p.lapShare); };
        const sh::float3 px = at(x0 + e, y0), mx = at(x0 - e, y0), py = at(x0, y0 + e), my = at(x0, y0 - e);
        const float ax = 1.f + (px.x - mx.x) / (2 * e), ay = (px.y - mx.y) / (2 * e), az = (px.z - mx.z) / (2 * e);
        const float bx = (py.x - my.x) / (2 * e), by = 1.f + (py.y - my.y) / (2 * e), bz = (py.z - my.z) / (2 * e);
        const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const sh::float3 n = sh::FTWaterNormal(sh::float2(x0, y0), t, w.w, p.q, sway, p.lapShare);
        CHECK(n.x == doctest::Approx(nx).epsilon(0.02).scale(1.0));
        CHECK(n.y == doctest::Approx(ny).epsilon(0.02).scale(1.0));
        CHECK(n.z == doctest::Approx(nz).epsilon(0.02).scale(1.0));
        CHECK(n.z > 0.f);
    }
    (void)b;
}

TEST_CASE("A body's edge line gives waterEdgeM") {
    WaterBody wide = canalBox(0.f);
    WaterBody tall = wide;
    tall.half = {3.f, 25.f};
    const WaterBody sea = seaPlane();
    WaterBody poly;
    poly.shape = WaterShape::Polygon;
    const WaterBody all[4] = {wide, tall, sea, poly};
    for (const WaterBody& b : all) {
        float line[kWaterEdgeFloats];
        waterEdgeLine(b, line);
        for (int k = 0; k < 25; ++k) {
            const Vec2 p{b.centre.x + static_cast<float>(k % 5 - 2) * 2.1f, b.centre.y + static_cast<float>(k / 5 - 2) * 1.7f};
            CHECK(waterEdgeLineCm(line, p.x * 100.f, p.y * 100.f) == doctest::Approx(waterEdgeM(b, p) * 100.f).epsilon(1e-4).scale(1.0));
        }
    }
}

TEST_CASE("Tiles fit a box exactly and run a sea from under the beach out to its reach") {
    WaterBodies water;
    water.add(canalBox(0.f));
    water.add(seaPlane());
    WaterBody pool = canalBox(0.f);
    pool.kind = WaterKind::Pool;
    pool.shape = WaterShape::Polygon;
    water.add(pool);
    WaterTileSpec spec;
    spec.seaReachM = 100.f;
    WaterTileBounds bounds{{-200.f, -50.f}, {200.f, 300.f}};
    WaterTile tiles[512];
    const std::size_t n = waterTileLayout(water, WaterWaveSpec{}, spec, bounds, tiles, 512);
    // Box 60 x 8: 4 x 1 tiles of 15 x 8. Sea: 120 m out (7.5 -> 8 rows) by 400 m along (25 columns).
    CHECK(n == 4 + 8 * 25);
    float boxArea = 0.f;
    for (std::size_t i = 0; i < n; ++i) {
        const WaterTile& t = tiles[i];
        CHECK(t.size.x <= spec.tileM + 1e-3f);
        CHECK(t.size.y <= spec.tileM + 1e-3f);
        if (t.body == 0) {
            boxArea += t.size.x * t.size.y;
            CHECK(water.body(0).covers(t.centre));
            CHECK(t.group == WaterDrawGroup::Canal);
            CHECK(t.z == 1.f);
        } else {
            CHECK(t.body == 1);
            CHECK(t.group == WaterDrawGroup::Sea);
            const float out = water.body(1).out(t.centre);
            CHECK(out > -spec.landOverlapM);
            CHECK(out < spec.seaReachM);
        }
    }
    CHECK(boxArea == doctest::Approx(60.f * 8.f));
    // A short array keeps what fits and says how many were wanted.
    WaterTile few[10];
    CHECK(waterTileLayout(water, WaterWaveSpec{}, spec, bounds, few, 10) == n);
    CHECK(waterTileLayout(water, WaterWaveSpec{}, spec, bounds, nullptr, 0) == n);
}

TEST_CASE("Under the cap, tiles are laid a whole body at a time and a body that doesn't fit is dropped whole") {
    WaterBodies water;
    water.add(canalBox(0.f)); // 4 tiles
    water.add(seaPlane());    // 8 x 25 within these bounds; a far start stretches it
    water.add(canalBox(0.f)); // 4 tiles
    WaterTileSpec spec;
    spec.seaReachM = 100.f;
    WaterTile tiles[64];
    std::uint32_t dropped = 0;
    std::uint32_t need[kWaterMaxBodies] = {};
    WaterTileBounds bounds{{-200.f, -50.f}, {200.f, 300.f}};
    CHECK(waterTileLayoutWhole(water, WaterWaveSpec{}, spec, bounds, tiles, 64, &dropped, need) == 8);
    CHECK(dropped == 0b010u);
    CHECK(need[0] == 4);
    CHECK(need[1] == 200);
    CHECK(need[2] == 4);
    CHECK(tiles[0].body == 0);
    CHECK(tiles[3].body == 0);
    CHECK(tiles[4].body == 2); // the canal after the dropped sea still gets its tiles
    CHECK(tiles[7].body == 2);
    // A start 400 km off: the sea's strip would need some 200 000 tiles; it is dropped, nothing overflows.
    bounds.max.x = 400000.f;
    bounds.max.y = 400000.f;
    CHECK(waterTileLayoutWhole(water, WaterWaveSpec{}, spec, bounds, tiles, 64, &dropped, need) == 8);
    CHECK(dropped == 0b010u);
    CHECK(need[1] > 100000u);
    // Room for everything: nothing dropped, the same tiles as waterTileLayout.
    WaterTile all[256];
    WaterTile plain[256];
    bounds = WaterTileBounds{{-200.f, -50.f}, {200.f, 300.f}};
    CHECK(waterTileLayoutWhole(water, WaterWaveSpec{}, spec, bounds, all, 256, &dropped) == 208);
    CHECK(dropped == 0u);
    CHECK(waterTileLayout(water, WaterWaveSpec{}, spec, bounds, plain, 256) == 208);
    for (std::size_t i = 0; i < 208; ++i) {
        CHECK(all[i].body == plain[i].body);
        CHECK(all[i].centre.x == plain[i].centre.x);
        CHECK(all[i].centre.y == plain[i].centre.y);
    }
    // No array: nothing laid, every body with tiles dropped.
    CHECK(waterTileLayoutWhole(water, WaterWaveSpec{}, spec, bounds, nullptr, 0, &dropped) == 0);
    CHECK(dropped == 0b111u);
}

TEST_CASE("Each body draws in its group") {
    const WaterWaveSpec spec;
    CHECK(waterDrawGroup(seaPlane(), spec) == WaterDrawGroup::Sea);
    CHECK(waterDrawGroup(canalBox(1.f), spec) == WaterDrawGroup::River);
    CHECK(waterDrawGroup(canalBox(0.f), spec) == WaterDrawGroup::Canal);
    WaterBody lake = canalBox(0.f);
    lake.kind = WaterKind::Lake;
    CHECK(waterDrawGroup(lake, spec) == WaterDrawGroup::Lake);
    lake.kind = WaterKind::Pool;
    CHECK(waterDrawGroup(lake, spec) == WaterDrawGroup::Pool);
    for (int g = 0; g < kWaterDrawGroups; ++g) {
        const WaterSurfaceModel m = stormModel();
        const WaterDrawParams p = waterDrawParams(m, static_cast<WaterDrawGroup>(g));
        CHECK(p.q >= 0.f);
        CHECK(p.fadeCm >= m.sets.minFadeCm);
        if (g >= static_cast<int>(WaterDrawGroup::Lake)) CHECK(p.tideM == 0.f);
    }
}

TEST_CASE("A body the tiles don't draw stays flat at its level under the model") {
    const WaterSurfaceModel m = stormModel();
    WaterBodies water;
    water.add(seaPlane());
    water.add(canalBox(0.8f));
    water.setSurface(&m);
    const Vec2 sea{seaPlane().centre.x + 40.f, seaPlane().centre.y};
    const Vec2 canal = canalBox(0.8f).centre;
    const float t = 17.f;
    const float seaRides = water.surfaceOf(0, sea, t);
    const float canalRides = water.surfaceOf(1, canal, t);
    CHECK(canalRides != doctest::Approx(water.body(1).level).epsilon(0.0001));
    water.setSurfaceFlat(1u << 1);
    CHECK(water.surfaceFlat() == 2u);
    CHECK(water.surfaceOf(1, canal, t) == water.body(1).level);
    CHECK(water.restSurfaceOf(1) == water.body(1).level);
    CHECK(water.surfaceOf(0, sea, t) == seaRides);
    water.setSurfaceFlat(0);
    CHECK(water.surfaceOf(1, canal, t) == canalRides);
}

TEST_CASE("water draw: the canal's sky is scaled only on a tier that reflects nothing but the sky") {
    using namespace ftcore;
    // Low: no SSR, no Lumen reflections, the water's reflections as the scene's.
    WaterReflectTier low;
    low.waterReflection = 1;
    low.ssrQuality = 0;
    low.lumenReflections = false;
    CHECK(waterSkyOnly(low));
    CHECK(waterCanalSky(low, kWaterCanalSkyLow) == doctest::Approx(kWaterCanalSkyLow));
    CHECK(kWaterCanalSkyLow > 0.f);
    CHECK(kWaterCanalSkyLow < 1.f);
    // Play (Lumen reflections) and Medium (SSR): unchanged.
    WaterReflectTier play = low;
    play.lumenReflections = true;
    CHECK_FALSE(waterSkyOnly(play));
    CHECK(waterCanalSky(play, 0.025f) == 1.f);
    WaterReflectTier medium = low;
    medium.ssrQuality = 2;
    CHECK_FALSE(waterSkyOnly(medium));
    CHECK(waterCanalSky(medium, 0.025f) == 1.f);
    // A negative SSR quality is off.
    WaterReflectTier negative = low;
    negative.ssrQuality = -1;
    CHECK(waterSkyOnly(negative));
    // The water's reflections off: nothing to scale. Forced captures and sky: sky only whatever the tier.
    WaterReflectTier off = low;
    off.waterReflection = 0;
    CHECK_FALSE(waterSkyOnly(off));
    CHECK(waterCanalSky(off, 0.025f) == 1.f);
    WaterReflectTier captures = play;
    captures.waterReflection = 2;
    captures.ssrQuality = 3;
    CHECK(waterSkyOnly(captures));
    // Forced SSR: sky only when SSR is off.
    WaterReflectTier ssr = play;
    ssr.waterReflection = 3;
    ssr.ssrQuality = 0;
    CHECK(waterSkyOnly(ssr));
    ssr.ssrQuality = 1;
    CHECK_FALSE(waterSkyOnly(ssr));
}

TEST_CASE("water draw: the canal's sky scale is clamped; not finite is no change; nothing is allocated") {
    using namespace ftcore;
    const WaterReflectTier low{1, 0, false};
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(waterCanalSky(low, -1.f) == 0.f);
    CHECK(waterCanalSky(low, 0.f) == 0.f);
    CHECK(waterCanalSky(low, 1.f) == 1.f);
    CHECK(waterCanalSky(low, 3.f) == 1.f);
    CHECK(waterCanalSky(low, std::numeric_limits<float>::quiet_NaN()) == 1.f);
    CHECK(waterCanalSky(low, inf) == 1.f);
    CHECK(waterCanalSky(low, -inf) == 1.f);
    const auto before = ftcore_test::allocStats().count;
    float sink = 0.f;
    for (int k = 0; k < 100; ++k) {
        WaterReflectTier t{k % 4, k % 3, (k & 1) != 0};
        sink += waterCanalSky(t, float(k) / 100.f);
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sink > 0.f);
}

TEST_CASE("water draw: rain rings follow the rain only while on and above the surface, in steps, and allocate nothing") {
    using namespace ftcore;
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Off: none, whatever the rain.
    CHECK(waterRainRings(false, false, 0.f) == 0.f);
    CHECK(waterRainRings(false, false, 0.7f) == 0.f);
    CHECK(waterRainRings(false, false, 1.f) == 0.f);
    // The view under a surface: none, on or off.
    CHECK(waterRainRings(true, true, 1.f) == 0.f);
    CHECK(waterRainRings(true, true, 0.5f) == 0.f);
    CHECK(waterRainRings(false, true, 1.f) == 0.f);
    // On: the rain's intensity, clamped.
    CHECK(waterRainRings(true, false, 0.f) == 0.f);
    CHECK(waterRainRings(true, false, 0.5f) == 0.5f);
    CHECK(waterRainRings(true, false, 1.f) == 1.f);
    CHECK(waterRainRings(true, false, 3.f) == 1.f);
    CHECK(waterRainRings(true, false, -1.f) == 0.f);
    // Not finite: none.
    CHECK(waterRainRings(true, false, nan) == 0.f);
    CHECK(waterRainRings(true, false, inf) == 0.f);
    CHECK(waterRainRings(true, false, -inf) == 0.f);
    // Steps of 1/kWaterRainRingSteps: a ramp moves it only a step at a time, and a drizzle under half a step is none.
    CHECK(waterRainRings(true, false, 0.51f) == 0.5f);
    CHECK(waterRainRings(true, false, 0.5f + 0.4f / kWaterRainRingSteps) == 0.5f);
    CHECK(waterRainRings(true, false, 0.4f / kWaterRainRingSteps) == 0.f);
    CHECK(waterRainRings(true, false, 1.f / kWaterRainRingSteps) == doctest::Approx(1.f / kWaterRainRingSteps));
    int moves = 0;
    float last = 0.f;
    const auto before = ftcore_test::allocStats().count;
    for (int k = 0; k <= 1000; ++k) {
        const float r = waterRainRings(true, false, float(k) / 1000.f);
        CHECK(r >= last);
        if (r != last) ++moves;
        last = r;
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(moves == int(kWaterRainRingSteps));
    CHECK(last == 1.f);
}

TEST_CASE("Water/Surface.ush's tile offset: inside a sea's span and on a canal, the offset node as it was") {
    const WaterSurfaceModel m = stormModel();
    const WaterTileSpec spec;
    const WaterTileBounds bounds{{-900.f, -700.f}, {1100.f, 900.f}};
    WaterBody sea = seaPlane();
    sea.cosYaw = std::cos(1.3f);
    sea.sinYaw = std::sin(1.3f);
    const WaterBody bodies[2] = {sea, canalBox(0.f)};
    const float band = sh::FTWaterSeaBandCm * 0.01f;
    float worst = 0.f;
    int checked = 0;
    for (const WaterBody& b : bodies) {
        const WaterDrawParams p = waterDrawParams(m, waterDrawGroup(b, m.spec.waves));
        Packed w = pack(p);
        const TileInputs in = tileInputs(b, spec, bounds);
        const float tideCm = 37.f;
        for (int k = 0; k < 60; ++k) {
            sh::float2 r(0.f, 0.f);
            if (b.shape == WaterShape::HalfPlane) {
                // Out from under the beach to a band short of the reach; along, a band in from either end.
                const float outM = -spec.landOverlapM + static_cast<float>(k % 10) / 9.f * (spec.seaReachM - band + spec.landOverlapM);
                const float alongM = in.span.y * 0.01f + band + static_cast<float>(k / 10) / 5.f * ((in.span.z - in.span.y) * 0.01f - 2.f * band);
                r = shorePoint(b, outM, alongM);
            } else {
                r = sh::float2(b.centre.x * 100.f + static_cast<float>(k % 10 - 5) * 500.f, b.centre.y * 100.f + static_cast<float>(k / 10 - 3) * 60.f);
            }
            const float skirt = (k % 3 == 0) ? 200.f : 0.f;
            const sh::float3 rest(r.x, r.y, b.level * 100.f - skirt);
            // The eye: over the point, 120 m off, 400 m off (in the distance fade) and under the water line.
            const float offM = (k % 4 == 0) ? 0.f : (k % 4 == 1 ? 120.f : 400.f);
            const float eyeZ = (k % 4 == 3) ? b.level * 100.f - 150.f : b.level * 100.f + 600.f;
            const sh::float3 eye(r.x + offM * 100.f, r.y, eyeZ);
            const sh::float2 grid(25.f, skirt);
            const float t = 4.f + 53.1f * static_cast<float>(k);
            const sh::float3 now = sh::FTWaterTileOffset(rest, grid, eye, t, w.w, p.q, p.fadeCm, p.lapShare, in.edgeA, in.edgeB, in.span,
                                                         1.f, 30000.f, 50000.f, tideCm);
            const sh::float3 was = oldTileOffset(rest, grid, eye, t, w.w, p.q, p.fadeCm, p.lapShare, in.edgeA, in.edgeB, 1.f, 30000.f, 50000.f,
                                                 tideCm, in.span.x);
            worst = std::max(worst, std::fabs(now.x - was.x) + std::fabs(now.y - was.y) + std::fabs(now.z - was.z));
            ++checked;
        }
    }
    CHECK(checked == 120);
    CHECK(worst < 0.001f);
}

TEST_CASE("Water/Surface.ush's tile offset: at a sea's span edge a vertex moves by the tide alone, as the far planes do") {
    const WaterSurfaceModel m = stormModel();
    const WaterTileSpec spec;
    const WaterTileBounds bounds{{-900.f, -700.f}, {1100.f, 900.f}};
    WaterBody sea = seaPlane();
    sea.cosYaw = std::cos(-0.7f);
    sea.sinYaw = std::sin(-0.7f);
    const WaterDrawParams p = waterDrawParams(m, WaterDrawGroup::Sea);
    Packed w = pack(p);
    const TileInputs in = tileInputs(sea, spec, bounds);
    REQUIRE(in.span.w == 1.f);
    REQUIRE(in.span.z - in.span.y > 4.f * sh::FTWaterSeaBandCm);
    const float band = sh::FTWaterSeaBandCm * 0.01f;
    const float lo = in.span.y * 0.01f, hi = in.span.z * 0.01f, reach = in.span.x * 0.01f;
    const float tideCm = -41.f;
    // On the three outer edges (seaward, both ends along the shore) and at the corners: the tide, nothing else.
    const float edges[7][2] = {{reach, 0.5f * (lo + hi)}, {reach - 300.f, lo}, {reach - 300.f, hi}, {0.f, lo}, {0.f, hi}, {reach, lo}, {reach, hi}};
    for (int e = 0; e < 7; ++e) {
        const sh::float2 r = shorePoint(sea, edges[e][0], edges[e][1]);
        CHECK(sh::FTWaterSeaFade(r, in.edgeA, in.edgeB, in.span) == doctest::Approx(0.f).scale(1.0).epsilon(1e-4));
        for (int k = 0; k < 5; ++k) {
            // The eye right over the vertex, so the distance fade keeps the waves whole.
            const sh::float3 eye(r.x, r.y, 500.f);
            const sh::float3 o = sh::FTWaterTileOffset(sh::float3(r.x, r.y, 0.f), sh::float2(25.f, 0.f), eye, 7.f + 31.f * static_cast<float>(k), w.w, p.q,
                                                       p.fadeCm, p.lapShare, in.edgeA, in.edgeB, in.span, 1.f, 30000.f, 50000.f, tideCm);
            CHECK(std::fabs(o.x) < 0.01f);
            CHECK(std::fabs(o.y) < 0.01f);
            CHECK(o.z == doctest::Approx(tideCm).epsilon(1e-4));
        }
    }
    // A band in from every edge the waves are whole; the fade runs 0 to 1 across the band without turning back.
    CHECK(sh::FTWaterSeaFade(shorePoint(sea, reach - band, lo + band), in.edgeA, in.edgeB, in.span) == doctest::Approx(1.f));
    CHECK(sh::FTWaterSeaFade(shorePoint(sea, 0.f, hi - band), in.edgeA, in.edgeB, in.span) == doctest::Approx(1.f));
    float last = -1.f;
    for (int k = 0; k <= 30; ++k) {
        const float f = sh::FTWaterSeaFade(shorePoint(sea, 10.f, lo + band * static_cast<float>(k) / 30.f), in.edgeA, in.edgeB, in.span);
        CHECK(f >= last - 1e-5f);
        CHECK(f >= 0.f);
        CHECK(f <= 1.f);
        last = f;
    }
    CHECK(last == doctest::Approx(1.f));
    // Past the span (beyond the tiles, where the far planes are) nothing comes back.
    CHECK(sh::FTWaterSeaFade(shorePoint(sea, reach + 50.f, 0.5f * (lo + hi)), in.edgeA, in.edgeB, in.span) == 0.f);
    CHECK(sh::FTWaterSeaFade(shorePoint(sea, 10.f, lo - 50.f), in.edgeA, in.edgeB, in.span) == 0.f);
    CHECK(sh::FTWaterSeaFade(shorePoint(sea, 10.f, hi + 50.f), in.edgeA, in.edgeB, in.span) == 0.f);
    // The far planes (WaveVerts 0): the tide anywhere, the span's middle too.
    const sh::float2 mid = shorePoint(sea, 0.5f * reach, 0.5f * (lo + hi));
    const sh::float3 farPlane = sh::FTWaterTileOffset(sh::float3(mid.x, mid.y, 0.f), sh::float2(25.f, 0.f), sh::float3(mid.x, mid.y, 500.f), 9.f, w.w, p.q,
                                                 p.fadeCm, p.lapShare, in.edgeA, in.edgeB, in.span, 0.f, 30000.f, 50000.f, tideCm);
    CHECK(farPlane.x == 0.f);
    CHECK(farPlane.y == 0.f);
    CHECK(farPlane.z == tideCm);
    // A span that is off (w 0), and a box's edge line, fade nothing.
    TileInputs off = in;
    off.span.w = 0.f;
    CHECK(sh::FTWaterSeaFade(shorePoint(sea, reach, lo), off.edgeA, off.edgeB, off.span) == 1.f);
    const TileInputs box = tileInputs(canalBox(0.f), spec, bounds);
    CHECK(box.span.w == 0.f);
    CHECK(sh::FTWaterSeaFade(sh::float2(0.f, 0.f), box.edgeA, box.edgeB, in.span) == 1.f);
}

TEST_CASE("Water/Surface.ush's skirt folds up whole while the eye is under the water line, and only then") {
    const float levelCm = 250.f, skirt = 200.f, tide = 30.f;
    const float restZ = levelCm - skirt; // the skirt's lower rim at rest
    CHECK(sh::FTWaterSkirtLift(skirt, restZ, levelCm + tide - 1.f, tide) == skirt);
    CHECK(sh::FTWaterSkirtLift(skirt, restZ, levelCm - 500.f, tide) == skirt);
    CHECK(sh::FTWaterSkirtLift(skirt, restZ, levelCm + tide, tide) == 0.f);
    CHECK(sh::FTWaterSkirtLift(skirt, restZ, levelCm + tide + 1.f, tide) == 0.f);
    // The tile's top (no drop) never moves for it.
    CHECK(sh::FTWaterSkirtLift(0.f, levelCm, levelCm - 500.f, tide) == 0.f);
    // Through the whole offset: a far plane's skirt vertex rises by its drop under water, not above.
    const TileInputs in = tileInputs(canalBox(0.f), WaterTileSpec{}, WaterTileBounds{});
    sh::float4 w[8];
    const sh::float3 rest(4000.f, -1200.f, restZ);
    const sh::float3 under = sh::FTWaterTileOffset(rest, sh::float2(100.f, skirt), sh::float3(4000.f, -1200.f, levelCm - 100.f), 3.f, w, 0.f, 150.f,
                                                   0.35f, in.edgeA, in.edgeB, in.span, 0.f, 30000.f, 50000.f, tide);
    const sh::float3 above = sh::FTWaterTileOffset(rest, sh::float2(100.f, skirt), sh::float3(4000.f, -1200.f, levelCm + 100.f), 3.f, w, 0.f,
                                                   150.f, 0.35f, in.edgeA, in.edgeB, in.span, 0.f, 30000.f, 50000.f, tide);
    CHECK(under.z == tide + skirt);
    CHECK(above.z == tide);
}

TEST_CASE("A sea's span is where its laid tiles end; a box or polygon has none; nothing is allocated") {
    WaterBodies water;
    WaterBody sea = seaPlane();
    sea.cosYaw = std::cos(0.4f);
    sea.sinYaw = std::sin(0.4f);
    water.add(sea);
    water.add(canalBox(0.f));
    WaterTileSpec spec;
    spec.seaReachM = 300.f;
    const WaterTileBounds bounds{{-500.f, -260.f}, {700.f, 410.f}};
    static WaterTile tiles[4096];
    const std::size_t n = waterTileLayout(water, WaterWaveSpec{}, spec, bounds, tiles, 4096);
    REQUIRE(n < 4096);
    float outMax = -1e9f, alongMin = 1e9f, alongMax = -1e9f;
    int seaTiles = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const WaterTile& t = tiles[i];
        if (t.body != 0) continue;
        ++seaTiles;
        for (int c = 0; c < 4; ++c) {
            const float sx = (c & 1) ? 0.5f : -0.5f, sy = (c & 2) ? 0.5f : -0.5f;
            const float px = t.centre.x + t.cosYaw * t.size.x * sx - t.sinYaw * t.size.y * sy - sea.centre.x;
            const float py = t.centre.y + t.sinYaw * t.size.x * sx + t.cosYaw * t.size.y * sy - sea.centre.y;
            outMax = std::max(outMax, px * sea.cosYaw + py * sea.sinYaw);
            const float along = -px * sea.sinYaw + py * sea.cosYaw;
            alongMin = std::min(alongMin, along);
            alongMax = std::max(alongMax, along);
        }
    }
    REQUIRE(seaTiles > 0);
    float span[kWaterSpanFloats];
    const auto before = ftcore_test::allocStats().count;
    waterSeaSpanCm(sea, spec, bounds, span);
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(std::fabs(span[0] - outMax * 100.f) < 1.f);
    CHECK(std::fabs(span[1] - alongMin * 100.f) < 1.f);
    CHECK(std::fabs(span[2] - alongMax * 100.f) < 1.f);
    CHECK(span[3] == 1.f);
    waterSeaSpanCm(canalBox(0.f), spec, bounds, span);
    for (int k = 0; k < kWaterSpanFloats; ++k) CHECK(span[k] == 0.f);
    WaterBody poly = canalBox(0.f);
    poly.shape = WaterShape::Polygon;
    span[3] = 5.f;
    waterSeaSpanCm(poly, spec, bounds, span);
    for (int k = 0; k < kWaterSpanFloats; ++k) CHECK(span[k] == 0.f);
}
