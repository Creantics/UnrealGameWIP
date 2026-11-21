#include "ftcore/FleshContact.h"

#include <doctest/doctest.h>

#include <cmath>

namespace fl = ftcore::flesh;

namespace {
// A forearm along +X, 4 cm radius (8 cm thick), soft.
const fl::Receiver kForearm{{{0.f, 0.f, 0.f}, {25.f, 0.f, 0.f}, 4.f}, 0.f};
fl::Capsule palm(float z, float r = 3.f) { return {{10.f, -2.f, z}, {10.f, 2.f, z}, r}; }
} // namespace

TEST_CASE("flesh contact: nothing touching gives 0 contacts") {
    const fl::Capsule p[] = {palm(20.f)};
    const fl::Receiver r[] = {kForearm};
    const fl::Contacts c = fl::solve(p, r);
    CHECK(c.count == 0);
    CHECK(c.touching == 0);
    CHECK(fl::solve({}, r).count == 0);
    CHECK(fl::solve(p, {}).count == 0);
    const fl::Capsule grazing[] = {palm(7.f)}; // surfaces just meet: no overlap
    CHECK(fl::solve(grazing, r).count == 0);
}

TEST_CASE("flesh contact: a palm on the forearm, depth, normal, point and patch") {
    const fl::Capsule p[] = {palm(6.5f)}; // 0.5 cm in
    const fl::Receiver r[] = {kForearm};
    const fl::Contacts c = fl::solve(p, r);
    REQUIRE(c.count == 1);
    const fl::Contact& k = c.c[0];
    CHECK(k.depth == doctest::Approx(0.5f));
    CHECK(k.normal.z == doctest::Approx(1.f));
    CHECK(k.point.x == doctest::Approx(10.f));
    CHECK(k.point.z == doctest::Approx(4.f));
    CHECK(k.radius == doctest::Approx(std::sqrt(2.f * 3.f * 0.5f - 0.25f)));
    CHECK(k.receiver == 0);
    CHECK(k.presser == 0);
}

TEST_CASE("flesh contact: firmness and the dent cap") {
    const fl::Capsule p[] = {palm(6.f)}; // 1 cm in
    fl::Receiver firm = kForearm;
    firm.firmness = 0.75f;
    const fl::Receiver r1[] = {firm};
    CHECK(fl::solve(p, r1).c[0].depth == doctest::Approx(0.25f));
    firm.firmness = 1.f; // bone: touching but no dent
    const fl::Receiver r2[] = {firm};
    const fl::Contacts c2 = fl::solve(p, r2);
    REQUIRE(c2.count == 1);
    CHECK(c2.c[0].depth == 0.f);
    const fl::Capsule deep[] = {palm(5.f)}; // 2 cm in: capped at 0.15 x 8 = 1.2
    const fl::Receiver r3[] = {kForearm};
    CHECK(fl::solve(deep, r3).c[0].depth == doctest::Approx(1.2f));
}

TEST_CASE("flesh contact: a presser deeper than the limb is thick is clamped") {
    const fl::Receiver r[] = {kForearm};
    fl::Params all;
    all.dentFrac = 1.f; // even with no cap from dentFrac, the depth stops at the radius
    const fl::Capsule through[] = {{{10.f, -2.f, -1.f}, {10.f, 2.f, -1.f}, 10.f}};
    const fl::Contacts c = fl::solve(through, r, all);
    REQUIRE(c.count == 1);
    CHECK(c.c[0].depth == doctest::Approx(4.f));
    CHECK(std::isfinite(c.c[0].radius));
    CHECK(c.c[0].radius == doctest::Approx(10.f)); // patch never wider than the presser
    CHECK(fl::solve(through, r).c[0].depth == doctest::Approx(1.2f));
}

TEST_CASE("flesh contact: presser axis on the receiver axis gives a finite unit normal") {
    const fl::Receiver r[] = {kForearm};
    const fl::Capsule crossing[] = {{{10.f, -5.f, 0.f}, {10.f, 5.f, 0.f}, 2.f}};
    const fl::Capsule along[] = {{{5.f, 0.f, 0.f}, {15.f, 0.f, 0.f}, 2.f}};
    const fl::Capsule centre[] = {{{10.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, 2.f}};
    for (const fl::Contacts& c : {fl::solve(crossing, r), fl::solve(along, r), fl::solve(centre, r)}) {
        REQUIRE(c.count == 1);
        const fl::V3 n = c.c[0].normal;
        CHECK(std::isfinite(n.x));
        CHECK(std::isfinite(n.y));
        CHECK(std::isfinite(n.z));
        CHECK(fl::length(n) == doctest::Approx(1.f));
        CHECK(std::fabs(n.x) < 1e-5f); // at right angles to the forearm
        CHECK(c.c[0].depth == doctest::Approx(1.2f));
    }
    const fl::Receiver ball[] = {{{{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 4.f}, 0.f}};
    const fl::Capsule same[] = {{{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 1.f}};
    const fl::Contacts c = fl::solve(same, ball);
    REQUIRE(c.count == 1);
    CHECK(fl::length(c.c[0].normal) == doctest::Approx(1.f));
}

TEST_CASE("flesh contact: a sphere presser and a sphere receiver") {
    const fl::Receiver r[] = {kForearm};
    const fl::Capsule tip[] = {{{10.f, 0.f, 4.5f}, {10.f, 0.f, 4.5f}, 1.f}}; // a fingertip, 0.5 cm in
    const fl::Contacts c = fl::solve(tip, r);
    REQUIRE(c.count == 1);
    CHECK(c.c[0].depth == doctest::Approx(0.5f));
    CHECK(c.c[0].radius == doctest::Approx(std::sqrt(0.75f)));
    const fl::Receiver head[] = {{{{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 9.f}, 0.8f}};
    const fl::Capsule knuckle[] = {{{0.f, 9.5f, 0.f}, {0.f, 9.5f, 0.f}, 1.f}};
    const fl::Contacts h = fl::solve(knuckle, head);
    REQUIRE(h.count == 1);
    CHECK(h.c[0].normal.y == doctest::Approx(1.f));
    CHECK(h.c[0].depth == doctest::Approx(0.1f));
}

TEST_CASE("flesh contact: more than K keeps the deepest, deepest first, ties in found order") {
    const fl::Receiver r[] = {kForearm};
    // Six fingertips along the forearm, 0.1 .. 0.6 cm in, in a jumbled order, and two ties.
    const float in[] = {0.3f, 0.6f, 0.1f, 0.5f, 0.5f, 0.2f};
    fl::Capsule p[6];
    for (int i = 0; i < 6; ++i) {
        const float z = 4.f + 1.f - in[i];
        p[i] = {{3.f * static_cast<float>(i) + 2.f, 0.f, z}, {3.f * static_cast<float>(i) + 2.f, 0.f, z}, 1.f};
    }
    const fl::Contacts c = fl::solve(p, r);
    CHECK(c.touching == 6);
    REQUIRE(c.count == fl::kMaxContacts);
    CHECK(c.c[0].presser == 1);
    CHECK(c.c[1].presser == 3); // the two 0.5s keep the order found
    CHECK(c.c[2].presser == 4);
    CHECK(c.c[3].presser == 0);
    for (int i = 1; i < c.count; ++i) CHECK(c.c[i - 1].depth >= c.c[i].depth);
}

TEST_CASE("flesh contact: every presser against every receiver") {
    const fl::Receiver r[] = {kForearm, {{{0.f, 20.f, 0.f}, {25.f, 20.f, 0.f}, 4.f}, 0.f}};
    const fl::Capsule p[] = {palm(6.5f), {{10.f, 20.f, 6.f}, {10.f, 20.f, 6.f}, 3.f}};
    const fl::Contacts c = fl::solve(p, r);
    REQUIRE(c.count == 2);
    CHECK(c.c[0].receiver == 1); // 1 cm in, deeper
    CHECK(c.c[0].presser == 1);
    CHECK(c.c[1].receiver == 0);
    CHECK(c.c[1].presser == 0);
}

TEST_CASE("flesh contact: bad radii and NaN never make a contact") {
    const fl::Receiver r[] = {kForearm};
    const fl::Capsule zero[] = {palm(6.f, 0.f)};
    const fl::Capsule nan[] = {palm(std::nanf(""))};
    CHECK(fl::solve(zero, r).count == 0);
    CHECK(fl::solve(nan, r).count == 0);
    const fl::Receiver flat[] = {{{{0.f, 0.f, 0.f}, {25.f, 0.f, 0.f}, -1.f}, 0.f}};
    const fl::Capsule p[] = {palm(1.f)};
    CHECK(fl::solve(p, flat).count == 0);
}

TEST_CASE("flesh profile: dent, ring, and exactly 0 at depth 0") {
    const fl::Shape sh{1.f, 1.f};
    CHECK(fl::profile(0.f, 2.f, 0.5f, sh) == doctest::Approx(-0.5f));        // the centre is the depth asked
    CHECK(fl::profile(2.5f, 2.f, 0.5f, sh) == doctest::Approx(0.1f));        // ring peak: 0.2 of the depth
    CHECK(fl::profile(2.5f, 2.f, 0.5f, {2.f, 1.f}) == doctest::Approx(0.2f)); // bulge 2 doubles it
    CHECK(fl::profile(2.5f, 2.f, 0.5f, {0.f, 1.f}) == 0.f);
    CHECK(fl::profile(-1.f, 2.f, 0.5f, sh) == fl::profile(1.f, 2.f, 0.5f, sh));
    CHECK(fl::profile(10.f, 2.f, 0.5f, sh) == 0.f);
    for (float s = 0.f; s < 4.f; s += 0.25f) {
        CHECK(fl::profile(s, 2.f, 0.f, sh) == 0.f);
        CHECK(fl::slope(s, 2.f, 0.f, sh) == 0.f);
    }
    CHECK(fl::profile(1.f, 0.f, 0.5f, sh) == 0.f);
}

TEST_CASE("flesh profile: continuous at the dent edge and the ring's outer edge") {
    const fl::Shape sh{1.f, 1.5f};
    const float R = 2.f, d = 0.8f, e = 1e-4f;
    CHECK(fl::profile(R - e, R, d, sh) == doctest::Approx(0.f).epsilon(1e-3));
    CHECK(fl::profile(R + e, R, d, sh) == doctest::Approx(0.f).epsilon(1e-3));
    CHECK(fl::profile(R + 1.5f - e, R, d, sh) == doctest::Approx(0.f).epsilon(1e-3));
    CHECK(fl::profile(R + 1.5f, R, d, sh) == 0.f);
    CHECK(std::fabs(fl::slope(R - e, R, d, sh)) < 1e-3f);
    CHECK(std::fabs(fl::slope(R + e, R, d, sh)) < 1e-3f);
    CHECK(std::fabs(fl::slope(R + 1.5f - e, R, d, sh)) < 1e-3f);
    CHECK(fl::slope(0.f, R, d, sh) == 0.f);
}

TEST_CASE("flesh profile: slope matches a numeric derivative") {
    const fl::Shape sh{1.3f, 1.2f};
    const float R = 2.5f, d = 0.6f, h = 1e-3f;
    for (float s = -4.f; s <= 4.f; s += 0.137f) {
        const float num = (fl::profile(s + h, R, d, sh) - fl::profile(s - h, R, d, sh)) / (2.f * h);
        CHECK(fl::slope(s, R, d, sh) == doctest::Approx(num).epsilon(0.02).scale(1.f));
    }
}
