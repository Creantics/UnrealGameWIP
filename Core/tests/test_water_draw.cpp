// The drawn water: the tiles, the draw groups, and the material's copy of the waves (Water/Surface.ush), compiled here as
// C++ against a small shim so its offset is checked against ftcore::waterWaveOffset and the game's height query.

#include <doctest/doctest.h>

#include "ftcore/WaterDraw.h"

#include <cmath>

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
