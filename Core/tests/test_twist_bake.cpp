#include "ftcore/TwistBake.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>

using namespace ftcore;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

TwistQuat mul(const TwistQuat& a, const TwistQuat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
} // namespace

TEST_CASE("twist: a pure roll about the axis is all twist") {
    const TwistVec axis{2.0, 0.0, 0.0}; // length need not be 1
    CHECK(twistAngle(axisAngle(axis, 70 * kDeg), axis) == doctest::Approx(70 * kDeg));
    CHECK(twistAngle(axisAngle(axis, -120 * kDeg), axis) == doctest::Approx(-120 * kDeg));
}

TEST_CASE("twist: a hinge across the axis has none") {
    const TwistVec axis{1.0, 0.0, 0.0};
    CHECK(twistAngle(axisAngle({0.0, 1.0, 0.0}, 120 * kDeg), axis) == doctest::Approx(0.0));
    CHECK(twistAngle(axisAngle({0.0, 0.0, 1.0}, -90 * kDeg), axis) == doctest::Approx(0.0));
}

TEST_CASE("twist: swing then roll keeps the roll") {
    const TwistVec axis{1.0, 0.0, 0.0};
    // The roll applied first (in the bone's frame), then a bend: the swing-twist split finds the roll.
    const TwistQuat q = mul(axisAngle({0.0, 0.0, 1.0}, 40 * kDeg), axisAngle(axis, 30 * kDeg));
    CHECK(twistAngle(q, axis) == doctest::Approx(30 * kDeg));
}

TEST_CASE("twist: q and -q give the same angle") {
    const TwistVec axis{0.0, 1.0, 0.0};
    const TwistQuat q = axisAngle(axis, 170 * kDeg);
    const TwistQuat n{-q.x, -q.y, -q.z, -q.w};
    CHECK(twistAngle(n, axis) == doctest::Approx(twistAngle(q, axis)));
    CHECK(twistAngle(q, axis) == doctest::Approx(170 * kDeg));
}

TEST_CASE("twist: a swing of 180 degrees stays finite") {
    const TwistVec axis{1.0, 0.0, 0.0};
    const double a = twistAngle(axisAngle({0.0, 1.0, 0.0}, kPi), axis);
    CHECK(std::isfinite(a));
    CHECK(a == doctest::Approx(0.0));
    CHECK(twistAngle(TwistQuat{}, TwistVec{}) == 0.0);
}

TEST_CASE("twist: unwrapping keeps a roll through 180 degrees continuous") {
    // Raw twist steps 170, 179, -172 (= 188), -163 (= 197): unwrapped it keeps rising.
    double prev = 170 * kDeg;
    for (const double raw : std::array<double, 3>{179.0, -172.0, -163.0}) {
        const double next = unwrapAngle(raw * kDeg, prev);
        CHECK(next > prev);
        CHECK(next - prev < 10 * kDeg);
        prev = next;
    }
    CHECK(prev == doctest::Approx(197 * kDeg));
    CHECK(unwrapAngle(-10 * kDeg, 5 * kDeg) == doctest::Approx(-10 * kDeg));
}

TEST_CASE("twist: fraction along the segment") {
    const TwistVec child{0.0, 30.0, 0.0};
    CHECK(segmentFraction({0.0, 24.0, 0.5}, child) == doctest::Approx(0.8));
    CHECK(segmentFraction({0.0, -3.0, 0.0}, child) == 0.0);
    CHECK(segmentFraction({0.0, 40.0, 0.0}, child) == 1.0);
    CHECK(segmentFraction({1.0, 1.0, 1.0}, TwistVec{}) == 0.0);
}

TEST_CASE("twist: a bone's roll follows the child toward it and holds the segment back near the head") {
    // A wrist roll of 90 degrees: 0.8 of the way down the forearm, the twist bone rolls 72.
    CHECK(twistBoneRoll(0.8, 90 * kDeg, 0.0) == doctest::Approx(72 * kDeg));
    // An upper arm rolled 60 degrees in the shoulder: 0.3 of the way down, the bone takes back 42.
    CHECK(twistBoneRoll(0.3, 0.0, 60 * kDeg) == doctest::Approx(-42 * kDeg));
    CHECK(twistBoneRoll(1.0, 0.0, 60 * kDeg) == 0.0);
    CHECK(twistBoneRoll(0.0, 90 * kDeg, 0.0) == 0.0);
}
