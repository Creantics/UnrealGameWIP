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

TEST_CASE("twist: negative and oblique axes") {
    const TwistVec neg{-1.0, 0.0, 0.0};
    // A roll of +40 about +x is a roll of -40 about -x.
    CHECK(twistAngle(axisAngle({1.0, 0.0, 0.0}, 40 * kDeg), neg) == doctest::Approx(-40 * kDeg));
    const TwistVec oblique{1.0, 2.0, 3.0};
    CHECK(twistAngle(axisAngle(oblique, 25 * kDeg), oblique) == doctest::Approx(25 * kDeg));
    // A bend about an axis across the oblique one (perpendicular to it) has no twist.
    CHECK(twistAngle(axisAngle({3.0, 0.0, -1.0}, 80 * kDeg), oblique) == doctest::Approx(0.0));
}

TEST_CASE("twist: a perpendicular swing reads the same before or after the twist") {
    const TwistVec axis{1.0, 0.0, 0.0};
    const TwistQuat twistFirst = mul(axisAngle({0.0, 0.0, 1.0}, 60 * kDeg), axisAngle(axis, 30 * kDeg));
    const TwistQuat twistLast = mul(axisAngle(axis, 30 * kDeg), axisAngle({0.0, 0.0, 1.0}, 60 * kDeg));
    // swing * twist splits back exactly; twist * swing is a different rotation, but with the swing axis
    // perpendicular to the twist axis its twist and swing angles read the same.
    CHECK(twistAngle(twistFirst, axis) == doctest::Approx(30 * kDeg));
    CHECK(twistAngle(twistLast, axis) == doctest::Approx(30 * kDeg));
    CHECK(swingAngle(twistFirst, axis) == doctest::Approx(60 * kDeg));
    CHECK(swingAngle(twistLast, axis) == doctest::Approx(60 * kDeg));
    // A rotation about an axis between the twist axis and a perpendicular one is not a plain swing:
    // its twist is neither 0 nor the whole angle.
    const TwistQuat mixed = axisAngle({1.0, 0.0, 1.0}, 90 * kDeg);
    CHECK(std::abs(twistAngle(mixed, axis)) > 30 * kDeg);
    CHECK(std::abs(twistAngle(mixed, axis)) < 80 * kDeg);
    CHECK(swingAngle(mixed, axis) > 30 * kDeg);
    // swingAngle and twistAngle take a non-unit quaternion as its unit one.
    const TwistQuat unit = mul(axisAngle({0.0, 1.0, 0.0}, 70 * kDeg), axisAngle(axis, 20 * kDeg));
    const TwistQuat scaled{3.0 * unit.x, 3.0 * unit.y, 3.0 * unit.z, 3.0 * unit.w};
    CHECK(swingAngle(scaled, axis) == doctest::Approx(70 * kDeg));
    CHECK(twistAngle(scaled, axis) == doctest::Approx(20 * kDeg));
}

TEST_CASE("twist: swing angle and the fade near 180 degrees") {
    const TwistVec axis{1.0, 0.0, 0.0};
    CHECK(swingAngle(axisAngle(axis, 90 * kDeg), axis) == doctest::Approx(0.0));
    CHECK(swingAngle(axisAngle({0.0, 1.0, 0.0}, 100 * kDeg), axis) == doctest::Approx(100 * kDeg));
    CHECK(swingAngle(TwistQuat{}, TwistVec{}) == 0.0);
    CHECK(twistFade(0.0) == 1.0);
    CHECK(twistFade(100 * kDeg) == doctest::Approx(1.0));
    CHECK(twistFade(160 * kDeg) == doctest::Approx(0.0));
    CHECK(twistFade(kPi) == 0.0);
    CHECK(twistFade(130 * kDeg) == doctest::Approx(0.5));
    // An arm raised 90 degrees from its bind, then rolled 40, keeps all its twist.
    const TwistQuat raised = mul(axisAngle({0.0, 1.0, 0.0}, 90 * kDeg), axisAngle(axis, 40 * kDeg));
    CHECK(twistAngle(raised, axis) * twistFade(swingAngle(raised, axis)) == doctest::Approx(40 * kDeg));
    // A hinge bent 150 degrees about an axis 5 degrees off the perpendicular reads about 36 degrees of
    // twist; the fade keeps under a tenth of it.
    const TwistQuat hinge = axisAngle({std::sin(5 * kDeg), 0.0, std::cos(5 * kDeg)}, 150 * kDeg);
    const double raw = twistAngle(hinge, axis);
    CHECK(raw == doctest::Approx(36.0 * kDeg).epsilon(0.03));
    CHECK(std::abs(raw * twistFade(swingAngle(hinge, axis))) < 0.1 * std::abs(raw));
    // A swing of 180 degrees after a roll: the swing is 180, the fade 0, the result finite.
    const TwistQuat over = mul(axisAngle({0.0, 1.0, 0.0}, kPi), axisAngle(axis, 30 * kDeg));
    CHECK(swingAngle(over, axis) == doctest::Approx(kPi));
    CHECK(twistFade(swingAngle(over, axis)) == 0.0);
    CHECK(std::isfinite(twistAngle(over, axis)));
}

TEST_CASE("twist: the unwrap restarts where the fade is near zero") {
    // Faded keys come back wrapped, so a turn picked up there is dropped.
    CHECK(unwrapFadedTwist(170 * kDeg, -170 * kDeg + 2 * kPi * 3, 0.01) == doctest::Approx(170 * kDeg));
    CHECK(unwrapFadedTwist(-170 * kDeg, 170 * kDeg, 0.5) == doctest::Approx(190 * kDeg));
}

TEST_CASE("twist: unwrap across more than a turn, and axisAngle edge cases") {
    // A raw angle 3 turns away comes back to the nearest branch.
    CHECK(unwrapAngle(10 * kDeg + 6 * kPi, 0.0) == doctest::Approx(10 * kDeg));
    CHECK(unwrapAngle(-170 * kDeg, 7 * kPi) == doctest::Approx(7 * kPi + 10 * kDeg));
    const TwistQuat id = axisAngle(TwistVec{}, 1.0);
    CHECK(id.w == 1.0);
    CHECK(id.x == 0.0);
    // A non-unit axis gives a unit quaternion.
    const TwistQuat q = axisAngle({0.0, 0.0, 5.0}, 90 * kDeg);
    CHECK(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w == doctest::Approx(1.0));
    CHECK(q.z == doctest::Approx(std::sin(45 * kDeg)));
}
