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
