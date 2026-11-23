// The footprint check's massing boxes (crowdMassingBox, crowdBoxClearance): a member under a tree's canopy is not inside
// a building; one inside a wall's collision is.

#include "ftcore/CrowdV2.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace ftcore::crowdv2;

namespace {

CrowdMassBox box(double cx, double cy, double cz, double hx, double hy, double hz) {
    CrowdMassBox b;
    b.cx = cx;
    b.cy = cy;
    b.cz = cz;
    b.hx = hx;
    b.hy = hy;
    b.hz = hz;
    return b;
}

constexpr double kGateCm = 80.0; // S-271's wallInset, in the host's centimetres

} // namespace

TEST_CASE("crowd massing: a tree's canopy is not a wall; its trunk's collision is what the check reads") {
    // A broadleaf tree on the Strip: render bounds 10.6 x 10.8 m, 4 to 16.3 m up (the canopy); simple collision a 0.6 m
    // trunk from the ground to 8 m. A member 4 m from the trunk stands 1.3 m inside the canopy's render bounds.
    const CrowdMassBox render = box(38054.0, -13049.0, 1016.0, 530.0, 540.0, 616.0);
    const CrowdMassBox trunk = box(38054.0, -13049.0, 400.0, 30.0, 30.0, 400.0);
    const double px = 38054.0 + 400.0, py = -13049.0;
    REQUIRE(crowdBoxClearance(render, 1.0, 0.0, px, py) == doctest::Approx(-130.0));
    CrowdMassBox checked;
    REQUIRE(crowdMassingBox(render, &trunk, CrowdMassSize{}, checked));
    const double clear = crowdBoxClearance(checked, 1.0, 0.0, px, py);
    INFO("clearance to the checked box " << clear << " cm");
    CHECK(clear == doctest::Approx(370.0));
    CHECK(clear >= kGateCm);
    // Into the trunk is still caught.
    CHECK(crowdBoxClearance(checked, 1.0, 0.0, 38054.0 + 10.0, py) < 0.0);
}

TEST_CASE("crowd massing: without simple collision (or with an empty one) the render bounds are checked") {
    const CrowdMassBox render = box(0.0, 0.0, 600.0, 800.0, 400.0, 600.0);
    CrowdMassBox checked;
    REQUIRE(crowdMassingBox(render, nullptr, CrowdMassSize{}, checked));
    CHECK(checked.hx == render.hx);
    CHECK(checked.hy == render.hy);
    CHECK(crowdBoxClearance(checked, 1.0, 0.0, 700.0, 0.0) == doctest::Approx(-100.0));
    const CrowdMassBox empty{};
    REQUIRE(crowdMassingBox(render, &empty, CrowdMassSize{}, checked));
    CHECK(checked.hx == render.hx);
    CHECK(checked.hz == render.hz);
    CrowdMassBox nan = box(std::nan(""), 0.0, 0.0, 10.0, 10.0, 10.0);
    REQUIRE(crowdMassingBox(render, &nan, CrowdMassSize{}, checked));
    CHECK(checked.cx == render.cx);
}

TEST_CASE("crowd massing: what counts as a building reads the render bounds, not the collision") {
    const CrowdMassSize size{};
    CrowdMassBox out;
    // A tree: a big canopy, a thin trunk (0.6 m, under minSide); still a box, its trunk's.
    const CrowdMassBox trunk = box(0.0, 0.0, 400.0, 30.0, 30.0, 400.0);
    REQUIRE(crowdMassingBox(box(0.0, 0.0, 900.0, 500.0, 500.0, 600.0), &trunk, size, out));
    CHECK(out.hx == 30.0);
    // A bollard (render 0.5 m wide) and a kerb (2 cm tall) are not buildings, whatever their collision.
    const CrowdMassBox wide = box(0.0, 0.0, 600.0, 400.0, 400.0, 600.0);
    CHECK_FALSE(crowdMassingBox(box(0.0, 0.0, 60.0, 25.0, 25.0, 300.0), &wide, size, out));
    CHECK_FALSE(crowdMassingBox(box(0.0, 0.0, 1.0, 400.0, 400.0, 1.0), &wide, size, out));
    // The landscape-sized: over maxSide on its long side.
    CHECK_FALSE(crowdMassingBox(box(0.0, 0.0, 600.0, 16000.0, 400.0, 600.0), nullptr, size, out));
    // At the limits: exactly minHeight tall and minSide wide counts.
    CHECK(crowdMassingBox(box(0.0, 0.0, 125.0, 100.0, 100.0, 125.0), nullptr, size, out));
}

TEST_CASE("crowd massing: clearance to a turned box, outside by a side, past a corner, and inside") {
    // A 4 x 2 m box turned 90 degrees: its x axis along the world's y.
    const CrowdMassBox b = box(100.0, 200.0, 0.0, 200.0, 100.0, 100.0);
    CHECK(crowdBoxClearance(b, 0.0, 1.0, 100.0, 200.0 + 250.0) == doctest::Approx(50.0));  // past its x half (200)
    CHECK(crowdBoxClearance(b, 0.0, 1.0, 100.0 + 130.0, 200.0) == doctest::Approx(30.0));  // past its y half (100)
    CHECK(crowdBoxClearance(b, 0.0, 1.0, 100.0 - 130.0, 200.0 - 240.0) == doctest::Approx(50.0)); // a corner: 30, 40
    CHECK(crowdBoxClearance(b, 0.0, 1.0, 100.0 + 20.0, 200.0 + 10.0) == doctest::Approx(-80.0)); // nearest side 80 in
    // 45 degrees: 1.5 m out along its own x axis is 0.5 m past a side; its corners point along the world's axes, sqrt(2)
    // m out, so 2 m out along the world's x is 2 - sqrt(2) m past a corner.
    const double r = std::sqrt(0.5);
    const CrowdMassBox sq = box(0.0, 0.0, 0.0, 100.0, 100.0, 100.0);
    CHECK(crowdBoxClearance(sq, r, r, 150.0 * r, 150.0 * r) == doctest::Approx(50.0));
    CHECK(crowdBoxClearance(sq, r, r, 200.0, 0.0) == doctest::Approx(200.0 - 100.0 * std::sqrt(2.0)));
}

TEST_CASE("crowd ground: under a slab over the street the ground is the street, not the slab's top") {
    // The Strip at 904.5,518 (V2, 41621672): two members first grounded on the tops of a bay corridor slab and a balcony
    // slab. The host's surfaces under each, top down (cm): the slab's top, the street's slab, the landscape 0.18 m under
    // that (its line up starts inside the street's slab, so it is not clear).
    const double body = 180.0;
    const CrowdGroundHit corridor[] = {{1350.6, true}, {541.6, true}, {523.7, false}};
    CHECK(crowdPickGround(corridor, 3, body) == 1);
    const CrowdGroundHit balcony[] = {{2078.6, true}, {668.6, true}, {650.5, false}};
    CHECK(crowdPickGround(balcony, 3, body) == 1);
    // The first top is what the host took before: the slab.
    CHECK(crowdPickGround(corridor, 3, body) != 0);
}

TEST_CASE("crowd ground: a solid block keeps its top; one surface is itself; none is 0") {
    const double body = 180.0;
    // A building's box down to the street: the line up from the street's top starts inside the box.
    const CrowdGroundHit block[] = {{1350.6, true}, {541.6, false}, {523.7, false}};
    CHECK(crowdPickGround(block, 3, body) == 0);
    const CrowdGroundHit one[] = {{541.6, true}};
    CHECK(crowdPickGround(one, 1, body) == 0);
    const CrowdGroundHit blocked[] = {{541.6, false}};
    CHECK(crowdPickGround(blocked, 1, body) == 0);
    CHECK(crowdPickGround(nullptr, 0, body) == 0);
}

TEST_CASE("crowd ground: a top less than a body under the next top is not ground, even when its line up reads clear") {
    // A one-sided plane 1 m over the street: a line up from under it passes through, so only the gap tells.
    const double body = 180.0;
    const CrowdGroundHit plane[] = {{641.6, true}, {541.6, true}, {523.7, false}};
    CHECK(crowdPickGround(plane, 3, body) == 0);
    // At exactly a body's height it is ground.
    const CrowdGroundHit room[] = {{721.6, true}, {541.6, true}, {523.7, false}};
    CHECK(crowdPickGround(room, 3, body) == 1);
    // Two clear storeys under a roof: the lowest clear one.
    const CrowdGroundHit storeys[] = {{1500.0, true}, {1000.0, true}, {500.0, true}, {480.0, false}};
    CHECK(crowdPickGround(storeys, 4, body) == 2);
}

namespace {
using ftcore::Vec2;

// A strip of ground with a raised top over part of it (cm, plan x in m): a roof or a car's top over the street.
struct Ledge {
    double x0, x1, top, ground;
    // The host's one line down at x from from to to: the first surface it meets from above.
    CrowdGroundLine line(double x, double from, double to) const {
        CrowdGroundLine l;
        if (x >= x0 && x <= x1 && top <= from && top >= to) l.hit = true, l.z = top;
        else if (ground <= from && ground >= to) l.hit = true, l.z = ground;
        return l;
    }
    // The tops under x, highest first, as the host's long look finds them (the top's line up clear: a lean-to's open
    // underside, a car's), then its pick.
    double firstGround(double x) const {
        CrowdGroundHit h[2];
        std::size_t n = 0;
        if (x >= x0 && x <= x1) h[n++] = {top, true};
        h[n++] = {ground, true};
        return h[crowdPickGround(h, n, 180.0)].z;
    }
    bool under(double x, double feet) const { // a surface at the feet (within 1 cm)
        return std::fabs(feet - ground) < 1.0 || (x >= x0 && x <= x1 && std::fabs(feet - top) < 1.0);
    }
};

// A walker drawn at 60 frames a second off a ledge's end at 1.3 m/s (sim steps at 15 a second, drawn by stepAlpha),
// from on its top. Frames drawn standing on nothing, by the host's ground rule then (oldRule) or crowdGroundTake.
int framesOnNothing(const Ledge& g, bool oldRule, double& feetAtEnd, float start = 1.f) {
    constexpr double kUp = 300.0, kDown = 600.0, kStep = 60.0;
    const float step = 1.f / 15.f;
    CrowdAgentView v;
    v.pos = v.prevPos = {static_cast<float>(g.x1) - start, 0.f};
    double held = g.top;
    int on = 0;
    for (int f = 0; f < 4 * 60; ++f) {
        const float alpha = static_cast<float>(f % 4) / 4.f;
        if (f % 4 == 0) v.prevPos = v.pos, v.pos.x += 1.3f * step;
        if (oldRule) {
            // The round robin: this member's line once in 6 frames (48 drawn, 8 a frame), at its sim position; a miss
            // keeps the height it had; more than a step up looks again.
            if (f % 6 == 0) {
                const CrowdGroundLine l = g.line(v.pos.x, held + kUp, held - kDown);
                if (l.hit) held = l.z - held > kStep ? g.firstGround(v.pos.x) : l.z;
            }
        } else {
            const Vec2 at = crowdDrawnPoint(v, alpha);
            double z = held;
            if (!crowdGroundTake(g.line(at.x, held + kUp, held - kDown), held, kStep, z)) z = g.firstGround(at.x);
            held = z;
        }
        const float drawnX = v.prevPos.x + (v.pos.x - v.prevPos.x) * alpha; // where the host draws it, by hand
        on += !g.under(drawnX, held);
    }
    feetAtEnd = held;
    return on;
}
} // namespace

TEST_CASE("crowd ground: a member walking off a roof or a car's top is drawn on the ground under it at once, never held") {
    // The Strip's dd4d1092 run: 30 members drawn at a top's height just past its edge, nothing under their feet. The
    // host traced the ground 8 actors a frame (every 6th frame for one of 48), at the sim position, not the drawn one,
    // and kept the height it had when the line met nothing (over kGroundDown, 6 m, down).
    double feet = 0.0;
    // A roof 8.06 m over the street (12.15 over 4.09): the line from it misses the street.
    const Ledge roof{0.0, 5.0, 1215.0, 409.0};
    CHECK(framesOnNothing(roof, true, feet) > 100); // held at the roof's height to the end
    CHECK(feet == doctest::Approx(1215.0));
    CHECK(framesOnNothing(roof, false, feet) == 0);
    CHECK(feet == doctest::Approx(409.0));
    // A ridge 4.71 m over the lane (8.83 over 4.12): the line meets the lane, so only where and how often it is traced.
    const Ledge ridge{0.0, 5.0, 883.0, 412.0};
    int oldOn = 0, newOn = 0;
    for (int k = 0; k < 12; ++k) { // where it starts, so the round robin meets the edge at each phase
        const float start = 1.f + 0.0073f * static_cast<float>(k);
        oldOn += framesOnNothing(ridge, true, feet, start);
        newOn += framesOnNothing(ridge, false, feet, start);
        CHECK(feet == doctest::Approx(412.0));
    }
    INFO("ridge: frames on nothing, the old rule " << oldOn << ", now " << newOn);
    CHECK(oldOn > 0);
    CHECK(newOn == 0);
    // A line that met nothing never keeps the height; within a step up, or any way down, is taken; more than a step up
    // or a start inside a solid looks again.
    double z = 0.0;
    CHECK_FALSE(crowdGroundTake(CrowdGroundLine{}, 1215.0, 60.0, z));
    CHECK(crowdGroundTake(CrowdGroundLine{true, false, 409.0}, 1215.0, 60.0, z));
    CHECK(z == 409.0);
    CHECK(crowdGroundTake(CrowdGroundLine{true, false, 469.0}, 409.0, 60.0, z));
    CHECK(z == 469.0);
    CHECK_FALSE(crowdGroundTake(CrowdGroundLine{true, false, 469.5}, 409.0, 60.0, z));
    CHECK_FALSE(crowdGroundTake(CrowdGroundLine{true, true, 409.0}, 409.0, 60.0, z));
    // The drawn point is the lerp the host draws at.
    CrowdAgentView v;
    v.prevPos = {1.f, 2.f};
    v.pos = {3.f, 6.f};
    CHECK(crowdDrawnPoint(v, 0.25f).x == doctest::Approx(1.5f));
    CHECK(crowdDrawnPoint(v, 0.25f).y == doctest::Approx(3.f));
}
