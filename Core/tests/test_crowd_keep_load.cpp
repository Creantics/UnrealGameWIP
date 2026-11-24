// The keep-out build at the strip's scale (load time): ~4110 street nodes, ~26.6k boxes, the way the host runs it
// (crowdKeepStreetPoint on every box, then setKeepOuts on the boxes near a street).
#include "ftcore/CrowdV2.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ftcore;
using namespace ftcore::crowdv2;

namespace {

// `cols` x `rows` nodes, `spacing` m apart, streets sharing their crossings; every fifth row and column wider.
std::vector<CrowdStreet> stripStreets(int cols, int rows, float spacing) {
    std::vector<CrowdStreet> out;
    for (int r = 0; r < rows; ++r) {
        CrowdStreet h;
        h.kind = "street";
        h.width = r % 5 == 0 ? 14.f : 10.f;
        h.pavement = 3.f;
        for (int c = 0; c < cols; ++c) h.points.push_back({spacing * static_cast<float>(c), spacing * static_cast<float>(r)});
        out.push_back(h);
    }
    for (int c = 0; c < cols; ++c) {
        CrowdStreet v;
        v.kind = "street";
        v.width = c % 5 == 0 ? 14.f : 10.f;
        v.pavement = 3.f;
        for (int r = 0; r < rows; ++r) v.points.push_back({spacing * static_cast<float>(c), spacing * static_cast<float>(r)});
        out.push_back(v);
    }
    return out;
}

struct Lcg {
    std::uint32_t s;
    float next() { // 0..1
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) / 16777216.f;
    }
};

CrowdKeepOut box(Vec2 c, float hx, float hy, float turn, float gap, CrowdKeepKind kind) {
    CrowdKeepOut k;
    k.c = c;
    k.ax = std::cos(turn);
    k.ay = std::sin(turn);
    k.hx = hx;
    k.hy = hy;
    k.gap = gap;
    k.kind = kind;
    return k;
}

// Seven boxes a block: a front on each side (some standing out onto the pavement, some turned a little), a tree trunk
// on a pavement, a parked car, and a yard box in the middle (near no street).
std::vector<CrowdKeepOut> stripBoxes(int cols, int rows, float spacing) {
    std::vector<CrowdKeepOut> out;
    Lcg rng{12345u};
    for (int r = 0; r + 1 < rows; ++r)
        for (int c = 0; c + 1 < cols; ++c) {
            const float x0 = spacing * static_cast<float>(c), y0 = spacing * static_cast<float>(r);
            const float mid = 0.5f * spacing;
            for (int side = 0; side < 4; ++side) {
                const float half = 3.f + 3.f * rng.next();
                // The front 4.2..5.8 m off the centre line (2 m more on a wide street): some stand on the pavement.
                const bool wide = (side == 0 && r % 5 == 0) || (side == 1 && (r + 1) % 5 == 0) || (side == 2 && c % 5 == 0) ||
                                  (side == 3 && (c + 1) % 5 == 0);
                const float off = 4.2f + 1.6f * rng.next() + 2.5f + (wide ? 2.f : 0.f);
                const float slide = (rng.next() - 0.5f) * 6.f;
                const float turn = rng.next() < 0.2f ? (rng.next() - 0.5f) * 0.3f : 0.f;
                Vec2 at{};
                float ang = 0.f;
                switch (side) {
                case 0: at = {x0 + mid + slide, y0 + off}, ang = 0.f; break;
                case 1: at = {x0 + mid + slide, y0 + spacing - off}, ang = 0.f; break;
                case 2: at = {x0 + off, y0 + mid + slide}, ang = 1.5707964f; break;
                default: at = {x0 + spacing - off, y0 + mid + slide}, ang = 1.5707964f; break;
                }
                out.push_back(box(at, half, 2.5f, ang + turn, 0.61f, CrowdKeepKind::Wall));
            }
            out.push_back(box({x0 + mid + (rng.next() - 0.5f) * 10.f, y0 + 3.6f}, 0.3f, 0.3f, 0.f, 0.61f, CrowdKeepKind::Wall));
            out.push_back(box({x0 + 1.5f, y0 + mid + (rng.next() - 0.5f) * 8.f}, 2.2f, 0.9f, 1.5707964f, 0.3f,
                              CrowdKeepKind::Car));
            out.push_back(box({x0 + mid, y0 + mid}, 1.5f, 1.5f, 0.f, 0.61f, CrowdKeepKind::Wall));
        }
    // Odd boxes: sides past 64 samples (the clamp), no size, no axis, off the streets' bounds, turned 45 degrees.
    out.push_back(box({500.f, 8.f}, 90.f, 2.f, 0.f, 0.61f, CrowdKeepKind::Wall));
    out.push_back(box({8.f, 300.f}, 2.f, 80.f, 0.f, 0.61f, CrowdKeepKind::Wall));
    out.push_back(box({1000.f, 4.f}, 0.f, 0.f, 0.f, 0.61f, CrowdKeepKind::Wall));
    CrowdKeepOut noAxis = box({1012.f, 4.5f}, 1.f, 1.f, 0.f, 0.61f, CrowdKeepKind::Wall);
    noAxis.ax = noAxis.ay = 0.f;
    out.push_back(noAxis);
    out.push_back(box({-12.f, -9.f}, 3.f, 3.f, 0.f, 0.61f, CrowdKeepKind::Wall));
    out.push_back(box({3420.f, 740.f}, 3.f, 3.f, 0.3f, 0.61f, CrowdKeepKind::Wall));
    out.push_back(box({-40.f, 200.f}, 2.f, 2.f, 0.f, 0.61f, CrowdKeepKind::Wall));
    out.push_back(box({1250.f, 4.f}, 3.f, 3.f, 0.7853982f, 0.61f, CrowdKeepKind::Wall));
    return out;
}

// A door on a front every few blocks, its room behind it.
std::vector<CrowdDoor> stripDoors(int cols, int rows, float spacing) {
    std::vector<CrowdDoor> out;
    for (int r = 0; r + 1 < rows; r += 3)
        for (int c = 1; c + 1 < cols; c += 11) {
            CrowdDoor d;
            d.id = std::to_string(out.size());
            const float x = spacing * (static_cast<float>(c) + 0.5f), y = spacing * static_cast<float>(r);
            d.at = {x, y + 5.f};
            d.through = {x, y + 5.6f};
            d.inside = {x, y + 9.f};
            out.push_back(d);
        }
    return out;
}

// crowdKeepStreetPoint as it was first written: each side point asks nearestStreet on its own. The reference the
// load-time one is held to, bit for bit.
bool refKeepStreetPoint(const CrowdGraph& g, const CrowdKeepOut& k, float reach, Vec2& at) {
    float ax = k.ax, ay = k.ay;
    const float al = std::hypot(ax, ay);
    if (al > 1e-6f) {
        ax /= al;
        ay /= al;
    } else {
        ax = 1.f;
        ay = 0.f;
    }
    CrowdStreetPoint nearCentre;
    if (!g.nearestStreet(k.c, reach + std::hypot(k.hx, k.hy), nearCentre)) return false;
    static constexpr float kU[4] = {-1.f, 1.f, 1.f, -1.f};
    static constexpr float kV[4] = {-1.f, -1.f, 1.f, 1.f};
    bool found = false;
    CrowdStreetPoint best;
    for (int side = 0; side < 4; ++side) {
        const int next = (side + 1) % 4;
        const float u0 = kU[side] * k.hx, v0 = kV[side] * k.hy;
        const float u1 = kU[next] * k.hx, v1 = kV[next] * k.hy;
        const Vec2 p0{k.c.x + u0 * ax - v0 * ay, k.c.y + u0 * ay + v0 * ax};
        const Vec2 p1{k.c.x + u1 * ax - v1 * ay, k.c.y + u1 * ay + v1 * ax};
        const float len = std::hypot(p1.x - p0.x, p1.y - p0.y);
        const int n = std::clamp(static_cast<int>(std::ceil(len / 1.f)), 1, 64);
        const float within = reach + 0.5f * len / static_cast<float>(n);
        for (int j = 0; j < n; ++j) {
            const float f = static_cast<float>(j) / static_cast<float>(n);
            const Vec2 p{p0.x + (p1.x - p0.x) * f, p0.y + (p1.y - p0.y) * f};
            CrowdStreetPoint s;
            if (g.nearestStreet(p, within, s) && (!found || s.dist < best.dist)) {
                best = s;
                found = true;
            }
        }
    }
    if (!found) return false;
    const CrowdEdge& e = g.edge(best.n, best.k);
    const Vec2 a = g.node(best.n), b = g.node(e.to);
    const float t = best.along / std::max(e.length, 1e-3f);
    const Vec2 point{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    CrowdKeepOut unit = k;
    unit.ax = ax;
    unit.ay = ay;
    if (crowdKeepDistance(unit, point) > reach) return false;
    at = point;
    return true;
}

std::uint32_t bits(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 1099511628211ull; }

bool perfAsserts() {
    std::string value;
#ifdef _MSC_VER
    char* raw = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&raw, &length, "FTCORE_PERF_ASSERTS") == 0 && raw != nullptr) value = raw;
    std::free(raw);
#else
    if (const char* raw = std::getenv("FTCORE_PERF_ASSERTS")) value = raw;
#endif
    return !value.empty() && value != "0";
}

} // namespace

TEST_CASE("crowd keep-outs at the strip's scale: the same verdicts and closes as the per-point look, and the load's cost") {
    constexpr int kCols = 137, kRows = 30;
    constexpr float kSpacing = 25.f;
    CrowdGraph g;
    CrowdGraphTuning gt;
    gt.minComponent = 1;
    REQUIRE(g.build(stripStreets(kCols, kRows, kSpacing), gt));
    CHECK(g.nodeCount() == static_cast<std::uint32_t>(kCols * kRows));
    const std::vector<CrowdKeepOut> boxes = stripBoxes(kCols, kRows, kSpacing);
    CHECK(boxes.size() > 26000u);

    // The host's pass: each box's street point (the ground there is the host's; every box with one is kept here).
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<CrowdKeepOut> keeps;
    std::vector<Vec2> ats(boxes.size(), Vec2{0.f, 0.f});
    std::vector<std::uint8_t> got(boxes.size(), 0);
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        got[i] = crowdKeepStreetPoint(g, boxes[i], 15.f, ats[i]) ? 1 : 0;
        if (got[i]) keeps.push_back(boxes[i]);
    }
    const auto t1 = std::chrono::steady_clock::now();
    CrowdSim sim;
    CrowdTuning t;
    t.targetAgents = t.maxAgents = 8;
    REQUIRE(sim.init(g, t, 5));
    const std::vector<CrowdDoor> doors = stripDoors(kCols, kRows, kSpacing);
    const std::uint32_t attached = sim.setDoors(doors);
    CHECK(attached == doors.size());
    sim.setTrace(true);
    const std::uint32_t kept = sim.setKeepOuts(keeps);
    const auto t2 = std::chrono::steady_clock::now();
    const double pointMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double simMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf("crowd keep-out load: %zu boxes, %u nodes, %u doors: street points %.1f ms, setKeepOuts %.1f ms (%u kept, "
                "%u sides closed, %u streets shut, %u narrows, %u doors under a box)\n",
                boxes.size(), g.nodeCount(), attached, pointMs, simMs, kept, sim.closedSides(), sim.closedStreets(),
                sim.narrowCount(), sim.doorsUnderKeep());

    // Every box's street point, bit for bit as the per-point look gives it.
    std::uint32_t pointDiffs = 0, withPoint = 0;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        Vec2 ref{0.f, 0.f};
        const bool r = refKeepStreetPoint(g, boxes[i], 15.f, ref);
        withPoint += r ? 1u : 0u;
        if (r != (got[i] != 0) || (r && (bits(ref.x) != bits(ats[i].x) || bits(ref.y) != bits(ats[i].y)))) ++pointDiffs;
    }
    CHECK(pointDiffs == 0);
    CHECK(withPoint == keeps.size());

    // The sim's verdicts on the strip, as the nodes-by-boxes scan gave them (recorded from it, 11-24).
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint32_t n = 0; n < g.nodeCount(); ++n)
        for (std::uint32_t q = 0; q < g.degree(n); ++q) {
            const std::uint32_t to = g.edge(n, q).to;
            h = mix(h, (sim.sideOpen(n, to, 1) ? 1u : 0u) | (sim.sideOpen(n, to, -1) ? 2u : 0u));
        }
    for (const CrowdClosedStreet& s : sim.closedStreetTrace()) {
        h = mix(h, s.from);
        h = mix(h, s.to);
        h = mix(h, s.box);
        h = mix(h, bits(s.along));
        h = mix(h, static_cast<std::uint64_t>(s.why));
    }
    std::printf("crowd keep-out load: %zu with a street point, trace hash %llu\n", keeps.size(), static_cast<unsigned long long>(h));
    // Each node's box list and every narrow's stretch and slot (keepDigest), as the old load built them.
    const std::uint64_t digest = sim.keepDigest();
    std::printf("crowd keep-out load: keep digest %llu\n", static_cast<unsigned long long>(digest));
    // Measured on the old load (before the rebuild for speed): the new one must build the same boxes and narrows.
    CHECK(digest == 8556212126233948035ull);
    CHECK(keeps.size() == 27614u);
    CHECK(kept == 23668u);
    CHECK(sim.closedSides() == 336u);
    CHECK(sim.closedStreets() == 31u);
    CHECK(sim.narrowCount() == 3527u);
    CHECK(sim.doorsUnderKeep() == 130u);
    CHECK(h == 13328439925592824335ull);

    // Strip load target: under 0.5 s for both passes (Release); the gate's Debug build gets a loose bound. Wall-clock
    // bounds fail on a busy PC, so asserted only with FTCORE_PERF_ASSERTS=1.
    if (perfAsserts()) {
#ifdef NDEBUG
        CHECK(pointMs + simMs < 500.0);
#else
        CHECK(pointMs + simMs < 5000.0);
#endif
    }
}
