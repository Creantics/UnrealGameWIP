// The keep-out build on streets the strip's grid lacks: diagonal and bent streets, streets with no pavement (one band
// down the middle), and streets with a pavement on one side only (either side). Pinned to what the nodes-by-boxes
// scan (the load before it was rebuilt for speed) built on the same streets and boxes, bit for bit.
#include "ftcore/CrowdV2.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

using namespace ftcore;
using namespace ftcore::crowdv2;

namespace {

CrowdStreet street(std::vector<Vec2> pts, float width, float pavement, std::int8_t side) {
    CrowdStreet s;
    s.kind = "street";
    s.width = width;
    s.pavement = pavement;
    s.pavementSide = side;
    s.points = std::move(pts);
    return s;
}

// A diagonal, a bent one, one with no pavement, one with its pavement on the +1 side, one on the -1 side, crossing.
std::vector<CrowdStreet> mixedStreets() {
    return {
        street({{0.f, 0.f}, {200.f, 150.f}}, 10.f, 3.f, 0),
        street({{20.f, 180.f}, {80.f, 192.f}, {150.f, 168.f}, {210.f, 175.f}}, 9.f, 2.5f, 0),
        street({{0.f, 60.f}, {200.f, 60.f}}, 8.f, 0.f, 0),
        street({{100.f, -50.f}, {100.f, 200.f}}, 10.f, 3.f, 1),
        street({{-20.f, 120.f}, {220.f, 130.f}}, 12.f, 3.f, -1),
        street({{150.f, -40.f}, {40.f, 210.f}}, 11.f, 3.f, -1),
    };
}

struct Lcg {
    std::uint32_t s;
    float next() { // 0..1
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) / 16777216.f;
    }
};

// Boxes strewn along every street, both sides, at random sizes, turns and offsets (some on the pavement, some in the
// road, some past the wall), walls and cars.
std::vector<CrowdKeepOut> mixedBoxes(const std::vector<CrowdStreet>& streets) {
    std::vector<CrowdKeepOut> out;
    Lcg rng{777u};
    for (const CrowdStreet& s : streets)
        for (std::size_t i = 0; i + 1 < s.points.size(); ++i) {
            const Vec2 a = s.points[i], b = s.points[i + 1];
            // sqrt and + - * / only (correctly rounded everywhere), no libm trig: the pins below hold on every compiler.
            const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
            const float dx = (b.x - a.x) / len, dy = (b.y - a.y) / len;
            for (float d = 2.f; d < len; d += 3.f + 5.f * rng.next()) {
                const float side = rng.next() < 0.5f ? -1.f : 1.f;
                const float off = side * (1.f + 0.5f * s.width + 4.f * (rng.next() - 0.3f));
                CrowdKeepOut k;
                k.c = {a.x + dx * d - dy * off, a.y + dy * d + dx * off};
                // Along the street, or now and then turned off it by about 16 degrees either way (a 7-24-25 turn).
                float tc = 1.f, ts = 0.f;
                if (rng.next() < 0.3f) tc = 0.96f, ts = rng.next() < 0.5f ? -0.28f : 0.28f;
                k.ax = dx * tc - dy * ts;
                k.ay = dy * tc + dx * ts;
                k.hx = 0.3f + 3.f * rng.next();
                k.hy = 0.3f + 1.5f * rng.next();
                k.gap = rng.next() < 0.5f ? 0.61f : 0.3f;
                k.kind = rng.next() < 0.25f ? CrowdKeepKind::Car : CrowdKeepKind::Wall;
                out.push_back(k);
            }
        }
    return out;
}

std::uint32_t bits(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 1099511628211ull; }

} // namespace

TEST_CASE("crowd keep-outs on diagonal, bent, pavement-less and one-sided streets: the same build as the old scan") {
    CrowdGraph g;
    CrowdGraphTuning gt;
    gt.minComponent = 1;
    const std::vector<CrowdStreet> streets = mixedStreets();
    REQUIRE(g.build(streets, gt));
    const std::vector<CrowdKeepOut> boxes = mixedBoxes(streets);
    std::vector<CrowdKeepOut> keeps;
    std::uint64_t points = 1469598103934665603ull;
    for (const CrowdKeepOut& k : boxes) {
        Vec2 at{0.f, 0.f};
        const bool got = crowdKeepStreetPoint(g, k, 15.f, at);
        points = mix(points, got ? 1u : 0u);
        points = mix(points, bits(at.x));
        points = mix(points, bits(at.y));
        if (got) keeps.push_back(k);
    }
    CrowdSim sim;
    CrowdTuning t;
    t.targetAgents = t.maxAgents = 8;
    REQUIRE(sim.init(g, t, 5));
    sim.setTrace(true);
    const std::uint32_t kept = sim.setKeepOuts(keeps);
    std::uint64_t h = 1469598103934665603ull;
    std::uint32_t oneSided = 0, middle = 0;
    for (std::uint32_t n = 0; n < g.nodeCount(); ++n)
        for (std::uint32_t q = 0; q < g.degree(n); ++q) {
            const CrowdEdge& e = g.edge(n, q);
            oneSided += e.side != 0 ? 1u : 0u;
            middle += e.lane <= 0.f ? 1u : 0u;
            h = mix(h, (sim.sideOpen(n, e.to, 1) ? 1u : 0u) | (sim.sideOpen(n, e.to, -1) ? 2u : 0u));
        }
    for (const CrowdClosedStreet& s : sim.closedStreetTrace()) {
        h = mix(h, s.from);
        h = mix(h, s.to);
        h = mix(h, s.box);
        h = mix(h, bits(s.along));
        h = mix(h, static_cast<std::uint64_t>(s.why));
    }
    const std::uint64_t digest = sim.keepDigest();
    std::printf("crowd keep-out mixed: %zu boxes, %zu with a street point, %u kept, %u sides closed, %u streets shut, "
                "%u narrows; %u one-sided and %u middle-band edges; points %llu, trace %llu, digest %llu\n",
                boxes.size(), keeps.size(), kept, sim.closedSides(), sim.closedStreets(), sim.narrowCount(), oneSided,
                middle, static_cast<unsigned long long>(points), static_cast<unsigned long long>(h),
                static_cast<unsigned long long>(digest));
    // The streets the test is for are there.
    CHECK(oneSided > 0u);
    CHECK(middle > 0u);
    CHECK(sim.closedSides() > 0u);
    CHECK(sim.narrowCount() > 0u);
    // Recorded from the old scan (HEAD 2598212c's setKeepOuts and crowdKeepStreetPoint, keepDigest added), 11-24.
    CHECK(boxes.size() == 258u);
    CHECK(keeps.size() == 258u);
    CHECK(kept == 240u);
    CHECK(sim.closedSides() == 18u);
    CHECK(sim.closedStreets() == 11u);
    CHECK(sim.narrowCount() == 8u);
    CHECK(points == 1322351824239975162ull);
    CHECK(h == 16452679497542460960ull);
    CHECK(digest == 15865814603674731889ull);
}

namespace {

// Each street once, as the street grid holds it: (v, k) from its lower node, or from its only end when one way.
bool listedStreet(const CrowdGraph& g, std::uint32_t v, std::uint32_t k) {
    const std::uint32_t w = g.edge(v, k).to;
    if (w > v) return true;
    for (std::uint32_t j = 0; j < g.degree(w); ++j)
        if (g.edge(w, j).to == v) return false;
    return true;
}

bool sameAnswer(bool a, const CrowdStreetPoint& p, bool b, const CrowdStreetPoint& q) {
    if (a != b) return false;
    return !a || (p.n == q.n && p.k == q.k && bits(p.along) == bits(q.along) && bits(p.dist) == bits(q.dist));
}

} // namespace

TEST_CASE("crowd street grid: streetsIn lists every street with a point in the square, and nothing on a bad square") {
    CrowdGraph g;
    CrowdGraphTuning gt;
    gt.minComponent = 1;
    REQUIRE(g.build(mixedStreets(), gt));
    Lcg rng{99u};
    std::vector<CrowdStreetPoint> out;
    for (int i = 0; i < 300; ++i) {
        const Vec2 lo{-60.f + 300.f * rng.next(), -80.f + 320.f * rng.next()};
        const float w = 0.5f + 40.f * rng.next(), h = 0.5f + 40.f * rng.next();
        const Vec2 hi{lo.x + w, lo.y + h};
        out.clear();
        g.streetsIn(lo, hi, out);
        for (const CrowdStreetPoint& s : out) {
            CHECK(s.along == 0.f);
            CHECK(s.dist == 0.f);
            // And no street from far off: a listed street's box, grown by the 20 m street cell, touches the square
            // (an answer of every street for any square would fail here).
            const Vec2 a = g.node(s.n), b = g.node(g.edge(s.n, s.k).to);
            constexpr float kCell = 20.f + 0.01f;
            CAPTURE(i);
            CHECK(std::min(a.x, b.x) - kCell <= hi.x);
            CHECK(std::max(a.x, b.x) + kCell >= lo.x);
            CHECK(std::min(a.y, b.y) - kCell <= hi.y);
            CHECK(std::max(a.y, b.y) + kCell >= lo.y);
        }
        for (std::uint32_t v = 0; v < g.nodeCount(); ++v)
            for (std::uint32_t k = 0; k < g.degree(v); ++k) {
                if (!listedStreet(g, v, k)) continue;
                const Vec2 a = g.node(v), b = g.node(g.edge(v, k).to);
                bool inside = false;
                for (int j = 0; j <= 200 && !inside; ++j) {
                    const float f = static_cast<float>(j) / 200.f;
                    const Vec2 p{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
                    inside = p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y;
                }
                if (!inside) continue;
                bool listed = false;
                for (const CrowdStreetPoint& s : out) listed = listed || (s.n == v && s.k == k);
                CAPTURE(i);
                CAPTURE(v);
                CAPTURE(k);
                CHECK(listed);
            }
    }
    // A square turned inside out, or one far off the grid: nothing; `out` is appended to, never cleared.
    out.assign(1, CrowdStreetPoint{});
    g.streetsIn({10.f, 10.f}, {5.f, 20.f}, out);
    g.streetsIn({1.0e9f, 1.0e9f}, {2.0e9f, 2.0e9f}, out);
    g.streetsIn({-2.0e9f, -2.0e9f}, {-1.0e9f, -1.0e9f}, out);
    CHECK(out.size() == 1u);
    // One square over the whole grid lists every street.
    out.clear();
    g.streetsIn({-1.0e9f, -1.0e9f}, {1.0e9f, 1.0e9f}, out);
    for (std::uint32_t v = 0; v < g.nodeCount(); ++v)
        for (std::uint32_t k = 0; k < g.degree(v); ++k) {
            if (!listedStreet(g, v, k)) continue;
            bool listed = false;
            for (const CrowdStreetPoint& s : out) listed = listed || (s.n == v && s.k == k);
            CHECK(listed);
        }
    // An empty graph lists nothing.
    const CrowdGraph empty;
    std::vector<CrowdStreetPoint> none;
    empty.streetsIn({-10.f, -10.f}, {10.f, 10.f}, none);
    CHECK(none.empty());
}

TEST_CASE("crowd street grid: nearestStreetAmong over streetsNear gives nearestStreet's answer, bit for bit") {
    CrowdGraph g;
    CrowdGraphTuning gt;
    gt.minComponent = 1;
    REQUIRE(g.build(mixedStreets(), gt));
    Lcg rng{4242u};
    std::vector<CrowdStreetPoint> nearby;
    int found = 0, missed = 0;
    for (int i = 0; i < 400; ++i) {
        const Vec2 c{-40.f + 280.f * rng.next(), -60.f + 290.f * rng.next()};
        const float spread = 0.2f + 12.f * rng.next(), withinMax = 0.5f + 20.f * rng.next();
        g.streetsNear(c, spread, withinMax, nearby);
        for (int j = 0; j < 40; ++j) {
            // A point within `spread` of c (its rim included), looking no further than withinMax.
            const float ang = 6.2831853f * rng.next(), r = j == 0 ? spread : spread * rng.next();
            const Vec2 p{c.x + r * std::cos(ang), c.y + r * std::sin(ang)};
            const float within = j == 1 ? withinMax : withinMax * rng.next();
            CrowdStreetPoint want, got;
            const bool w = g.nearestStreet(p, within, want);
            const bool a = g.nearestStreetAmong(nearby, p, within, got);
            found += w ? 1 : 0;
            missed += w ? 0 : 1;
            CAPTURE(i);
            CAPTURE(j);
            CHECK(sameAnswer(w, want, a, got));
        }
    }
    // Both kinds of answer were asked for.
    CHECK(found > 1000);
    CHECK(missed > 100);
    // streetsNear clears its list; an empty graph gives an empty one and no answer.
    const CrowdGraph empty;
    empty.streetsNear({0.f, 0.f}, 5.f, 5.f, nearby);
    CHECK(nearby.empty());
    CrowdStreetPoint s;
    CHECK_FALSE(empty.nearestStreetAmong(nearby, {0.f, 0.f}, 5.f, s));
}
