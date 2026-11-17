#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "ftcore/WaterWaves.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

using namespace ftcore;

namespace {

constexpr float kPi = 3.14159265358979323846f;

SceneLook harbour() {
    SceneLook l;
    l.seaSwellM = 1.5f;
    l.seaSwellPeriodS = 8.f;
    l.seaSwellDeg = 200.f;
    l.windMps = 8.f;
    l.windDeg = 230.f;
    return l;
}

// A sea out from a quay along x = 0 (+x is water), a river box and a still canal box.
WaterBody seaBody() { return waterSea(0.f, 0.f, -12.f); }

WaterBody canalBody(float flow) {
    WaterBody b;
    b.kind = WaterKind::Canal;
    b.shape = WaterShape::Box;
    b.centre = {0.f, 0.f};
    b.half = {200.f, 6.f};
    b.flow = {flow, 0.f};
    b.level = -1.f;
    b.bed = -4.f;
    return b;
}

// ---- The shader's copy (Water/Surface.ush), line for line, fed only what the material gets: the packed vectors, Q,
// the grid's cell, the fade's distance and the lap share. ----
struct ShaderWave {
    float dirX, dirY, k, omega, a, phase;
};

ShaderWave unpack(const float v[4]) {
    ShaderWave w;
    w.k = std::max(std::sqrt(v[0] * v[0] + v[1] * v[1]), 1e-7f);
    w.dirX = v[0] / w.k;
    w.dirY = v[1] / w.k;
    w.omega = std::sqrt(981.f * w.k);
    w.a = v[2];
    w.phase = v[3];
    return w;
}

float smoothstepH(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

struct ShaderInputs {
    float waves[kSeaWaveCount][4];
    float q, gridCm, fadeCm, lapShare;
};

ShaderInputs shaderInputs(const WaterWaveSets& sets, WaterWaveClass c) {
    ShaderInputs in;
    const int ci = static_cast<int>(c);
    for (int i = 0; i < kSeaWaveCount; ++i) packSeaWave(sets.set[ci].waves[i], in.waves[i]);
    in.q = sets.set[ci].q;
    in.gridCm = sets.gridCm;
    in.fadeCm = waterFadeCm(sets, c);
    in.lapShare = sets.lapShare;
    return in;
}

void shaderOffset(const ShaderInputs& in, float restX, float restY, float edgeCm, float time, float out[3]) {
    time = std::fmod(time, kSeaLoopS);
    const float sway = smoothstepH(0.f, in.fadeCm, edgeCm);
    const float height = in.lapShare + (1.f - in.lapShare) * sway;
    out[0] = out[1] = out[2] = 0.f;
    for (int i = 0; i < kSeaWaveCount; ++i) {
        const ShaderWave w = unpack(in.waves[i]);
        const float th = w.k * (w.dirX * restX + w.dirY * restY) - w.omega * time + w.phase;
        const float keep = smoothstepH(4.f, 6.f, 2.f * kPi / (w.k * std::max(in.gridCm, 1.f)));
        out[0] += sway * keep * in.q * w.a * std::cos(th) * w.dirX;
        out[1] += sway * keep * in.q * w.a * std::cos(th) * w.dirY;
        out[2] += height * keep * w.a * std::sin(th);
    }
}

// The drawn surface's height over (x, y): the grid (cell gridCm, rest corners on its multiples, each cell cut along
// its rising diagonal) moved by the shader's offset at its corners, rasterized there.
bool drawnHeight(const ShaderInputs& in, const WaterBody& body, float x, float y, float t, float& z) {
    const float g = in.gridCm;
    const int reach = 6 + static_cast<int>(std::ceil(200.f / g));
    const int ci = static_cast<int>(std::floor(x / g)), cj = static_cast<int>(std::floor(y / g));
    auto corner = [&](int i, int j, float p[3]) {
        const float rx = static_cast<float>(i) * g, ry = static_cast<float>(j) * g;
        float d[3];
        shaderOffset(in, rx, ry, waterEdgeM(body, {rx * 0.01f, ry * 0.01f}) * 100.f, t, d);
        p[0] = rx + d[0];
        p[1] = ry + d[1];
        p[2] = d[2];
    };
    auto inTri = [&](const float* a, const float* b, const float* c) {
        const float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        if (std::fabs(d) < 1e-6f) return false;
        const float l0 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d;
        const float l1 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d;
        const float l2 = 1.f - l0 - l1;
        const float e = -1e-5f;
        if (l0 < e || l1 < e || l2 < e) return false;
        z = l0 * a[2] + l1 * b[2] + l2 * c[2];
        return true;
    };
    for (int i = ci - reach; i <= ci + reach; ++i) {
        for (int j = cj - reach; j <= cj + reach; ++j) {
            float p00[3], p10[3], p01[3], p11[3];
            corner(i, j, p00);
            corner(i + 1, j, p10);
            corner(i, j + 1, p01);
            corner(i + 1, j + 1, p11);
            if (inTri(p00, p10, p11) || inTri(p00, p11, p01)) return true;
        }
    }
    return false;
}

float maxDrawnError(const WaterWaveSets& sets, const WaterBody& body, WaterWaveClass c, float x0, float x1, float y0, float y1) {
    const ShaderInputs in = shaderInputs(sets, c);
    float worst = 0.f;
    int checked = 0;
    for (int s = 0; s < 40; ++s) {
        const float fx = static_cast<float>((s * 37) % 40) / 40.f, fy = static_cast<float>((s * 23) % 40) / 40.f;
        const float x = x0 + (x1 - x0) * fx, y = y0 + (y1 - y0) * fy, t = 3.1f + 7.7f * static_cast<float>(s);
        float drawn = 0.f;
        if (!drawnHeight(in, body, x, y, t, drawn)) continue;
        ++checked;
        worst = std::max(worst, std::fabs(drawn - waterWaveHeightCm(sets, body, c, x, y, t)));
    }
    CHECK(checked >= 36);
    return worst;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

float hs(const WaterWaveSets& sets, WaterWaveClass c) { return seaSignificantHeightCm(sets.set[static_cast<int>(c)]); }

} // namespace

TEST_CASE("Each body rides its class of waves") {
    const WaterWaveSpec spec;
    CHECK(waterWaveClass(seaBody(), spec) == WaterWaveClass::Sea);
    CHECK(waterWaveClass(canalBody(0.6f), spec) == WaterWaveClass::River);
    CHECK(waterWaveClass(canalBody(0.1f), spec) == WaterWaveClass::Canal);
    WaterBody pool = canalBody(0.f);
    pool.kind = WaterKind::Pool;
    CHECK(waterWaveClass(pool, spec) == WaterWaveClass::Canal);
    pool.kind = WaterKind::Lake;
    CHECK(waterWaveClass(pool, spec) == WaterWaveClass::River);
    CHECK(std::string(waterWaveClassName(WaterWaveClass::River)) == "river");
}

TEST_CASE("A calm day: flat sea, the river and canals never quite still, and low") {
    const WaterWaveSpec spec;
    const WaterWaveSets sets = waterWaveSets(SceneLook{}, 0.f, spec);
    CHECK(hs(sets, WaterWaveClass::Sea) == 0.f);
    CHECK(hs(sets, WaterWaveClass::River) > 0.5f);
    CHECK(hs(sets, WaterWaveClass::River) <= spec.riverMaxHsCm + 1e-3f);
    CHECK(hs(sets, WaterWaveClass::Canal) > 0.2f);
    CHECK(hs(sets, WaterWaveClass::Canal) <= spec.canalMaxHsCm + 1e-3f);
}

TEST_CASE("The storm raises the sea and the river; the caps hold") {
    const WaterWaveSpec spec;
    const WaterWaveSets calm = waterWaveSets(harbour(), 0.f, spec);
    const WaterWaveSets storm = waterWaveSets(harbour(), 1.f, spec);
    CHECK(hs(storm, WaterWaveClass::Sea) > hs(calm, WaterWaveClass::Sea) + 60.f); // the storm's swell adds 1.2 m to the 1.5 m
    // The weather raises the look's wind itself; the storm lifts the river's cap, so a gale's chop shows.
    SceneLook gale = harbour();
    gale.windMps = 25.f;
    CHECK(hs(waterWaveSets(gale, 1.f, spec), WaterWaveClass::River) > hs(waterWaveSets(gale, 0.f, spec), WaterWaveClass::River) + 5.f);
    CHECK(hs(storm, WaterWaveClass::River) <= spec.riverStormHsCm + 1e-3f);
    CHECK(hs(storm, WaterWaveClass::Canal) <= spec.canalStormHsCm + 1e-3f);
    CHECK(hs(storm, WaterWaveClass::River) < hs(storm, WaterWaveClass::Sea));
    for (int c = 0; c < kWaterWaveClasses; ++c) {
        float fold = 0.f;
        for (const SeaWave& w : storm.set[c].waves) fold += w.k * w.amplitude;
        CHECK(storm.set[c].q * fold <= kSeaMaxFold + 1e-4f);
    }
    // Bad storm input is no storm.
    CHECK(hs(waterWaveSets(harbour(), std::nanf(""), spec), WaterWaveClass::Sea) == doctest::Approx(hs(calm, WaterWaveClass::Sea)));
}

TEST_CASE("Edges: a box's long walls, a half-plane's edge, none for a polygon") {
    CHECK(waterEdgeM(canalBody(0.f), {0.f, 0.f}) == doctest::Approx(6.f));
    CHECK(waterEdgeM(canalBody(0.f), {150.f, 5.f}) == doctest::Approx(1.f));
    CHECK(waterEdgeM(canalBody(0.f), {0.f, 9.f}) == 0.f);
    CHECK(waterEdgeM(seaBody(), {3.f, 40.f}) == doctest::Approx(3.f));
    CHECK(waterEdgeM(seaBody(), {-3.f, 40.f}) == 0.f);
    WaterBody poly;
    poly.shape = WaterShape::Polygon;
    CHECK(waterEdgeM(poly, {0.f, 0.f}) == kWaterNoEdgeM);
}

TEST_CASE("At a wall the water laps: no sway, a share of the height; open water keeps it all") {
    const WaterWaveSets sets = waterWaveSets(harbour(), 0.5f, WaterWaveSpec{});
    float sway = 0.f, height = 0.f;
    waterEdgeFactors(sets, WaterWaveClass::Sea, 0.f, sway, height);
    CHECK(sway == 0.f);
    CHECK(height == doctest::Approx(0.35f));
    const float fade = waterFadeCm(sets, WaterWaveClass::Sea);
    CHECK(fade >= 150.f);
    waterEdgeFactors(sets, WaterWaveClass::Sea, fade, sway, height);
    CHECK(sway == doctest::Approx(1.f));
    CHECK(height == doctest::Approx(1.f));
    float last = -1.f;
    for (float d = 0.f; d <= fade; d += fade / 20.f) {
        waterEdgeFactors(sets, WaterWaveClass::Sea, d, sway, height);
        CHECK(sway >= last);
        last = sway;
    }
    // Against the quay the surface still moves up and down, never across.
    float o[3];
    waterWaveOffset(sets, WaterWaveClass::Sea, 0.f, 0.f, 500.f, 2.f, o);
    CHECK(o[0] == 0.f);
    CHECK(o[1] == 0.f);
    float range = 0.f;
    for (float t = 0.f; t < 20.f; t += 0.25f) {
        waterWaveOffset(sets, WaterWaveClass::Sea, 0.f, 0.f, 500.f, t, o);
        range = std::max(range, std::fabs(o[2]));
    }
    CHECK(range > 10.f);
}

TEST_CASE("The CPU offset is the shader's, from the packed numbers, to a millimetre") {
    for (float storm : {0.f, 0.6f, 1.f}) {
        const WaterWaveSets sets = waterWaveSets(harbour(), storm, WaterWaveSpec{});
        for (int c = 0; c < kWaterWaveClasses; ++c) {
            const WaterWaveClass cls = static_cast<WaterWaveClass>(c);
            const ShaderInputs in = shaderInputs(sets, cls);
            for (int s = 0; s < 50; ++s) {
                const float x = -3000.f + 131.f * static_cast<float>(s), y = 77.f * static_cast<float>(s) - 900.f;
                const float edge = static_cast<float>(s % 7) * 60.f, t = 0.37f * static_cast<float>(s * s);
                float cpu[3], gpu[3];
                waterWaveOffset(sets, cls, edge, x, y, t, cpu);
                shaderOffset(in, x, y, edge, t, gpu);
                for (int k = 0; k < 3; ++k) CHECK(std::fabs(cpu[k] - gpu[k]) < 0.1f);
            }
        }
    }
}

TEST_CASE("A gameplay height is the drawn surface's to 2 cm on the near grid") {
    const WaterWaveSpec spec;
    for (float storm : {0.f, 1.f}) {
        CAPTURE(storm);
        const WaterWaveSets sets = waterWaveSets(harbour(), storm, spec);
        // Open sea, and the band against the quay where the fade works.
        CHECK(maxDrawnError(sets, seaBody(), WaterWaveClass::Sea, 2000.f, 6000.f, -2000.f, 2000.f) < 2.f);
        CHECK(maxDrawnError(sets, seaBody(), WaterWaveClass::Sea, 0.f, 400.f, -2000.f, 2000.f) < 2.f);
        CHECK(maxDrawnError(sets, canalBody(0.6f), WaterWaveClass::River, -2000.f, 2000.f, -550.f, 550.f) < 2.f);
        CHECK(maxDrawnError(sets, canalBody(0.f), WaterWaveClass::Canal, -2000.f, 2000.f, -550.f, 550.f) < 2.f);
    }
}

TEST_CASE("The waves repeat with the sea's loop") {
    const WaterWaveSets sets = waterWaveSets(harbour(), 0.7f, WaterWaveSpec{});
    const WaterBody sea = seaBody();
    for (float t : {1.f, 13.5f, 600.f}) {
        const float a = waterWaveHeightCm(sets, sea, WaterWaveClass::Sea, 3000.f, 120.f, t);
        const float b = waterWaveHeightCm(sets, sea, WaterWaveClass::Sea, 3000.f, 120.f, t + kSeaLoopS);
        CHECK(a == doctest::Approx(b).epsilon(1e-3));
    }
}

TEST_CASE("The tide: twice a day, the river and canals damped and late, pools still") {
    const WaterTideSpec spec;
    const WaterWaveSpec ws;
    const WaterBody sea = seaBody();
    const WaterBody river = canalBody(0.6f), canal = canalBody(0.f);
    WaterBody pool = canalBody(0.f);
    pool.kind = WaterKind::Pool;
    const WaterWaveClass sc = waterWaveClass(sea, ws), rc = waterWaveClass(river, ws), cc = waterWaveClass(canal, ws);
    const double hi = spec.highHour;
    CHECK(waterTideM(spec, sea, sc, hi) == doctest::Approx(0.5f));
    CHECK(waterTideM(spec, sea, sc, hi + 6.0) == doctest::Approx(-0.5f));
    CHECK(waterTideM(spec, sea, sc, hi + 12.0) == doctest::Approx(0.5f));
    CHECK(waterTideM(spec, river, rc, hi + 0.75) == doctest::Approx(0.5f * 0.55f));
    CHECK(waterTideM(spec, canal, cc, hi + 1.5) == doctest::Approx(0.5f * 0.35f));
    CHECK(waterTideM(spec, canal, cc, hi) < waterTideM(spec, canal, cc, hi + 1.5));
    CHECK(waterTideM(spec, pool, cc, 3.0) == 0.f);
    // No jump at midnight or after many days, and a slow rise: under 1 cm a game minute.
    CHECK(std::fabs(waterTideM(spec, sea, sc, 23.9999) - waterTideM(spec, sea, sc, 24.0001)) < 1e-3f);
    CHECK(waterTideM(spec, sea, sc, hi + 24.0 * 400.0) == doctest::Approx(0.5f).epsilon(1e-4));
    for (double h = 0.0; h < 24.0; h += 0.25) CHECK(std::fabs(waterTideM(spec, sea, sc, h + 1.0 / 60.0) - waterTideM(spec, sea, sc, h)) < 0.01f);
    float lo = 0.f, hiM = 0.f;
    waterTideBand(spec, sea, sc, lo, hiM);
    CHECK(lo == doctest::Approx(-0.5f));
    CHECK(hiM == doctest::Approx(0.5f));
    waterTideBand(spec, pool, cc, lo, hiM);
    CHECK(lo == 0.f);
    CHECK(hiM == 0.f);
    CHECK(waterTideM(spec, sea, sc, std::nan("")) == 0.f);
}

TEST_CASE("The demo's 21:00 boot lands at mid to high water in every tidal body") {
    WaterSpec spec;
    std::string err;
    REQUIRE(parseWaterSpec(readFile(std::string(FTCORE_PROJECT_DATA_DIR) + "/Water/water.json"), spec, &err));
    const WaterBody sea = seaBody(), river = canalBody(0.6f), canal = canalBody(0.f);
    for (const WaterBody* b : {&sea, &river, &canal}) {
        const WaterWaveClass c = waterWaveClass(*b, spec.waves);
        float lo = 0.f, hiM = 0.f;
        waterTideBand(spec.tide, *b, c, lo, hiM);
        CHECK(waterTideM(spec.tide, *b, c, 21.0) >= 0.6f * hiM);
    }
}

TEST_CASE("The whole surface is level, tide and waves") {
    WaterSpec spec;
    const WaterWaveSets sets = waterWaveSets(harbour(), 0.3f, spec.waves);
    const WaterBody sea = seaBody();
    const float z = waterSurfaceM(spec, sets, sea, {25.f, 3.f}, 42.f, 27.0);
    const float expect = sea.level + waterTideM(spec.tide, sea, WaterWaveClass::Sea, 27.0) +
                         0.01f * waterWaveHeightCm(sets, sea, WaterWaveClass::Sea, 2500.f, 300.f, 42.f);
    CHECK(z == doctest::Approx(expect));
}

TEST_CASE("The water file: the shipped one reads, bad ones fail and keep the defaults") {
    WaterSpec spec;
    std::string err;
    const std::string text = readFile(std::string(FTCORE_PROJECT_DATA_DIR) + "/Water/water.json");
    REQUIRE(!text.empty());
    CHECK(parseWaterSpec(text, spec, &err));
    CHECK(err.empty());
    CHECK(spec.tide.rangeM == doctest::Approx(1.f));
    CHECK(spec.waves.gridCm == doctest::Approx(25.f));

    CHECK(parseWaterSpec(R"({"version": 1, "tide": {"rangeM": 1.2}})", spec, &err));
    CHECK(spec.tide.rangeM == doctest::Approx(1.2f));
    CHECK(spec.tide.periodHours == doctest::Approx(12.f));

    CHECK_FALSE(parseWaterSpec(R"({"version": 1, "tide": {"rangeM": 9}})", spec, &err));
    CHECK(err.find("rangeM") != std::string::npos);
    CHECK(spec.tide.rangeM == doctest::Approx(1.f));
    CHECK_FALSE(parseWaterSpec(R"({"version": 1, "waves": {"bogus": 1}})", spec, &err));
    CHECK_FALSE(parseWaterSpec(R"({"version": 2})", spec, &err));
    CHECK_FALSE(parseWaterSpec(R"({"version": 1, "waves": {"gridCm": "x"}})", spec, &err));
    CHECK_FALSE(parseWaterSpec("not json", spec, &err));
}

TEST_CASE("Height queries allocate nothing and are cheap enough for a fan of points per resting body") {
    const WaterSpec spec;
    const WaterWaveSets sets = waterWaveSets(harbour(), 1.f, spec.waves);
    const WaterBody sea = seaBody();
    float sink = 0.f;
    const auto before = ftcore_test::allocStats().count;
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kQueries = 20000;
    for (int i = 0; i < kQueries; ++i)
        sink += waterSurfaceM(spec, sets, sea, {static_cast<float>(i % 97), static_cast<float>(i % 13)}, 0.01f * static_cast<float>(i), 30.0);
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(std::isfinite(sink));
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kQueries;
    MESSAGE("waterSurfaceM: " << us << " us a query");
    CHECK(us < 20.0); // generous for debug builds; release is about 1 us
}

TEST_CASE("Bodies with the surface model set answer waterSurfaceM: wade, swim and eye see the drawn water") {
    WaterSurfaceModel model;
    model.sets = waterWaveSets(harbour(), 0.5f, model.spec.waves);
    model.totalHours = 21.0;
    WaterBodies water;
    REQUIRE(water.add(seaBody()));
    const Vec2 p{30.f, -40.f};
    const int i = water.bodyAt(p);
    REQUIRE(i >= 0);
    const float flat = water.surfaceOf(static_cast<std::size_t>(i), p, 3.f);
    water.setSurface(&model);
    const float drawn = waterSurfaceM(model.spec, model.sets, water.body(static_cast<std::size_t>(i)), p, 3.f, 21.0);
    CHECK(water.surfaceOf(static_cast<std::size_t>(i), p, 3.f) == doctest::Approx(drawn));
    float out = 0.f;
    CHECK(water.surfaceHeight(p, 3.f, out));
    CHECK(out == doctest::Approx(drawn));
    CHECK(drawn != doctest::Approx(flat));
    water.setSurface(nullptr);
    CHECK(water.surfaceOf(static_cast<std::size_t>(i), p, 3.f) == doctest::Approx(flat));
}

TEST_CASE("A wader in a storm sea for 30 s: no mode flips and no splash in and out; the surface still moves") {
    WaterSurfaceModel model;
    model.sets = waterWaveSets(harbour(), 1.f, model.spec.waves);
    model.totalHours = 21.0;
    WaterBodies water;
    REQUIRE(water.add(seaBody()));
    water.setSurface(&model);
    const Vec2 p{30.f, -40.f};
    const int i = water.bodyAt(p);
    REQUIRE(i >= 0);
    const float rest = water.restSurfaceOf(static_cast<std::size_t>(i));
    for (float column : {0.15f, 0.7f, 1.15f}) {
        CAPTURE(column);
        WaterCapsule cap;
        cap.p = p;
        cap.hasGround = true;
        cap.groundZ = rest - column;
        cap.feetZ = cap.groundZ;
        WaterRipples rings;
        WaterStir stir;
        const WaterMode first = waterState(water, cap, 0.f).mode;
        int flips = 0, entries = 0;
        float lo = 1e9f, hi = -1e9f;
        WaterMode last = first;
        for (int k = 0; k < 900; ++k) {
            const float t = static_cast<float>(k) / 30.f;
            const WaterState s = waterState(water, cap, t);
            flips += s.mode != last ? 1 : 0;
            last = s.mode;
            entries += stir.step(rings, s, p, {0.f, 0.f}, 0.f, 1.f / 30.f).entered ? 1 : 0;
            lo = std::min(lo, s.surface);
            hi = std::max(hi, s.surface);
        }
        CHECK(first != WaterMode::Dry);
        CHECK(flips == 0);
        CHECK(entries <= 1);
        CHECK(hi - lo > 0.3f);
    }
}

TEST_CASE("The water file refuses a storm cap under the calm cap") {
    WaterSpec spec;
    std::string err;
    CHECK_FALSE(parseWaterSpec(R"({"version": 1, "waves": {"riverMaxHsCm": 20, "riverStormHsCm": 10}})", spec, &err));
    CHECK(err.find("riverStormHsCm") != std::string::npos);
    CHECK_FALSE(parseWaterSpec(R"({"version": 1, "waves": {"canalMaxHsCm": 8, "canalStormHsCm": 5}})", spec, &err));
    CHECK(err.find("canalStormHsCm") != std::string::npos);
    CHECK(parseWaterSpec(R"({"version": 1, "waves": {"riverMaxHsCm": 20, "riverStormHsCm": 20}})", spec, &err));
}
