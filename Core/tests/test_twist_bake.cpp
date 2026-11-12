#include "ftcore/TwistBake.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>

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

TEST_CASE("twist bake: a wrist roll ramps the child twist through 180 degrees without a flip") {
    const TwistVec axis{25.0, 0.0, 0.0}; // the hand's bind offset in the forearm's frame
    constexpr int kKeys = 28;
    std::array<TwistQuat, kKeys> own{};
    std::array<TwistQuat, kKeys> child{};
    for (int k = 0; k < kKeys; ++k) {
        // The hand rolls 0 to 270 degrees while bent 30 degrees at the wrist.
        child[k] = mul(axisAngle({0.0, 0.0, 1.0}, 30 * kDeg), axisAngle(axis, 10.0 * k * kDeg));
    }
    std::array<double, kKeys> ownOut{};
    std::array<double, kKeys> childOut{};
    const TwistSegmentSummary s = bakeSegmentTwist(own.data(), child.data(), kKeys, axis, ownOut.data(), childOut.data());
    for (int k = 0; k < kKeys; ++k) {
        CAPTURE(k);
        CHECK(childOut[k] == doctest::Approx(10.0 * k * kDeg));
        CHECK(ownOut[k] == doctest::Approx(0.0));
    }
    CHECK(s.childPeak == doctest::Approx(270 * kDeg));
    CHECK(s.ownPeak == doctest::Approx(0.0));
    CHECK_FALSE(s.loops);
    // The forearm's twist bones from it: 0.25 and 0.75 down the forearm.
    CHECK(twistBoneRoll(0.75, childOut[9], ownOut[9]) == doctest::Approx(67.5 * kDeg));
    CHECK(twistBoneRoll(0.25, childOut[9], ownOut[9]) == doctest::Approx(22.5 * kDeg));
}

TEST_CASE("twist bake: no child term writes zeros; an arm raised overhead keeps part of its roll") {
    const TwistVec axis{0.0, 30.0, 0.0};
    // Raised 90, 130 and 170 degrees from the bind (across the axis), each rolled 40 in the shoulder.
    const std::array<TwistQuat, 3> own{mul(axisAngle({1.0, 0.0, 0.0}, 90 * kDeg), axisAngle(axis, 40 * kDeg)),
                                       mul(axisAngle({1.0, 0.0, 0.0}, 130 * kDeg), axisAngle(axis, 40 * kDeg)),
                                       mul(axisAngle({1.0, 0.0, 0.0}, 170 * kDeg), axisAngle(axis, 40 * kDeg))};
    std::array<double, 3> ownOut{};
    std::array<double, 3> childOut{1.0, 1.0, 1.0};
    const TwistSegmentSummary s = bakeSegmentTwist(own.data(), nullptr, 3, axis, ownOut.data(), childOut.data());
    CHECK(ownOut[0] == doctest::Approx(40 * kDeg));
    CHECK(ownOut[1] == doctest::Approx(20 * kDeg)); // the fade is 0.5 at 130 degrees
    CHECK(ownOut[2] == doctest::Approx(0.0));
    for (const double c : childOut) CHECK(c == 0.0);
    CHECK(s.childPeak == 0.0);
    CHECK(s.ownPeak == doctest::Approx(40 * kDeg));
    // The upper arm's bone 0.3 down holds back 0.7 of the roll; at the elbow (1.0) it follows the segment.
    CHECK(twistBoneRoll(0.3, childOut[0], ownOut[0]) == doctest::Approx(-28 * kDeg));
    CHECK(twistBoneRoll(1.0, childOut[0], ownOut[0]) == doctest::Approx(0.0));
}

TEST_CASE("twist bake: a loop that rolls a whole turn ends a turn away and says so") {
    const TwistVec axis{1.0, 0.0, 0.0};
    constexpr int kKeys = 37;
    std::array<TwistQuat, kKeys> own{};
    for (int k = 0; k < kKeys; ++k) own[k] = axisAngle(axis, 10.0 * k * kDeg); // 0 to 360: back where it started
    std::array<double, kKeys> ownOut{};
    std::array<double, kKeys> childOut{};
    const TwistSegmentSummary s = bakeSegmentTwist(own.data(), nullptr, kKeys, axis, ownOut.data(), childOut.data());
    CHECK(ownOut[kKeys - 1] == doctest::Approx(360 * kDeg));
    CHECK(s.loops);
    CHECK(s.loopPops);
    // A loop that rolls out and back is no pop.
    for (int k = 0; k < kKeys; ++k) own[k] = axisAngle(axis, (k <= 18 ? k : 36 - k) * 10.0 * kDeg);
    const TwistSegmentSummary back = bakeSegmentTwist(own.data(), nullptr, kKeys, axis, ownOut.data(), childOut.data());
    CHECK(back.loops);
    CHECK_FALSE(back.loopPops);
    CHECK(back.ownPeak == doctest::Approx(180 * kDeg));
}

TEST_CASE("twist bake: a swing past 160 degrees and back mid-clip leaves no step at the fade band") {
    // The segment rolls 2 degrees a key (680 in all, past 180 well before the swing peaks) while it swings 0 to 170
    // degrees and back, 1 degree a key. The output is twist * fade; each step must be what the twist's and the fade's
    // own change give, so no turn (2 pi * fade) appears or drops where the fade crosses its band.
    const TwistVec axis{0.0, 30.0, 0.0};
    const TwistVec across{1.0, 0.0, 0.0};
    constexpr int kKeys = 341;
    std::array<TwistQuat, kKeys> own{};
    std::array<double, kKeys> twist{};
    std::array<double, kKeys> fade{};
    for (int k = 0; k < kKeys; ++k) {
        const double swing = (k <= 170 ? k : 340 - k) * kDeg;
        twist[k] = 2.0 * k * kDeg;
        fade[k] = twistFade(swing);
        own[k] = mul(axisAngle(across, swing), axisAngle(axis, twist[k]));
    }
    std::array<double, kKeys> ownOut{};
    std::array<double, kKeys> childOut{};
    const TwistSegmentSummary s = bakeSegmentTwist(own.data(), nullptr, kKeys, axis, ownOut.data(), childOut.data());
    double worst = 0.0;
    for (int k = 1; k < kKeys; ++k) {
        CAPTURE(k);
        const double allowed = std::abs(twist[k] - twist[k - 1]) * fade[k] +
                               std::max(std::abs(twist[k]), std::abs(twist[k - 1])) * std::abs(fade[k] - fade[k - 1]);
        const double step = std::abs(ownOut[k] - ownOut[k - 1]);
        CHECK(step <= allowed + 1e-9);
        worst = std::max(worst, step - allowed);
    }
    CHECK(worst <= 1e-9);
    // Back at full weight the whole roll is there: 680 degrees, not the wrapped -40.
    CHECK(ownOut[kKeys - 1] == doctest::Approx(680 * kDeg));
    CHECK(ownOut[30] == doctest::Approx(60 * kDeg));
    CHECK(ownOut[170] == doctest::Approx(0.0)); // faded out at the peak
    CHECK(s.ownBadKeys == 0);
}

TEST_CASE("twist bake: a NaN or zero key holds the last good value and poisons nothing after it") {
    const TwistVec axis{1.0, 0.0, 0.0};
    constexpr int kKeys = 8;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::array<TwistQuat, kKeys> own{};
    std::array<TwistQuat, kKeys> child{};
    for (int k = 0; k < kKeys; ++k) {
        own[k] = axisAngle(axis, 30.0 * k * kDeg);
        child[k] = axisAngle(axis, -20.0 * k * kDeg);
    }
    own[3] = TwistQuat{nan, 0.0, 0.0, 1.0};
    own[4] = TwistQuat{0.0, 0.0, 0.0, 0.0};
    child[0] = TwistQuat{0.0, std::numeric_limits<double>::infinity(), 0.0, 1.0};
    std::array<double, kKeys> ownOut{};
    std::array<double, kKeys> childOut{};
    const TwistSegmentSummary s = bakeSegmentTwist(own.data(), child.data(), kKeys, axis, ownOut.data(), childOut.data());
    CHECK(s.ownBadKeys == 2);
    CHECK(s.childBadKeys == 1);
    for (int k = 0; k < kKeys; ++k) {
        CAPTURE(k);
        CHECK(std::isfinite(ownOut[k]));
        CHECK(std::isfinite(childOut[k]));
    }
    CHECK(ownOut[3] == doctest::Approx(60 * kDeg)); // held from key 2
    CHECK(ownOut[4] == doctest::Approx(60 * kDeg));
    CHECK(ownOut[5] == doctest::Approx(150 * kDeg)); // unwrapped on from key 2
    CHECK(ownOut[7] == doctest::Approx(210 * kDeg)); // past 180, no flip
    CHECK(childOut[0] == 0.0);                       // nothing good yet: 0
    CHECK(childOut[7] == doctest::Approx(-140 * kDeg));
    CHECK(s.ownPeak == doctest::Approx(210 * kDeg));
    // A first or last key that is unusable is no loop.
    own[kKeys - 1] = TwistQuat{nan, nan, nan, nan};
    own[0] = own[kKeys - 1];
    CHECK_FALSE(bakeSegmentTwist(own.data(), nullptr, kKeys, axis, ownOut.data(), childOut.data()).loops);
}

TEST_CASE("twist bake: a clip of non-unit rotations bakes the same as the unit one") {
    const TwistVec axis{0.0, 0.0, 12.0};
    constexpr int kKeys = 20;
    std::array<TwistQuat, kKeys> unit{};
    std::array<TwistQuat, kKeys> scaled{};
    for (int k = 0; k < kKeys; ++k) {
        unit[k] = mul(axisAngle({1.0, 0.0, 0.0}, 8.0 * k * kDeg), axisAngle(axis, 15.0 * k * kDeg));
        const double sc = 0.3 + 0.25 * k;
        scaled[k] = TwistQuat{unit[k].x * sc, unit[k].y * sc, unit[k].z * sc, unit[k].w * sc};
    }
    std::array<double, kKeys> a{};
    std::array<double, kKeys> b{};
    std::array<double, kKeys> c{};
    const TwistSegmentSummary su = bakeSegmentTwist(unit.data(), unit.data(), kKeys, axis, a.data(), c.data());
    const TwistSegmentSummary ss = bakeSegmentTwist(scaled.data(), scaled.data(), kKeys, axis, b.data(), c.data());
    for (int k = 0; k < kKeys; ++k) {
        CAPTURE(k);
        CHECK(b[k] == doctest::Approx(a[k]));
    }
    CHECK(ss.ownPeak == doctest::Approx(su.ownPeak));
    CHECK(ss.ownBadKeys == 0);
}

TEST_CASE("twist bake: an empty clip or a missing array writes nothing") {
    const TwistVec axis{1.0, 0.0, 0.0};
    double ownOut = 7.0, childOut = 7.0;
    const TwistQuat q = axisAngle(axis, 1.0);
    CHECK(bakeSegmentTwist(&q, nullptr, 0, axis, &ownOut, &childOut).ownPeak == 0.0);
    CHECK(bakeSegmentTwist(nullptr, nullptr, 1, axis, &ownOut, &childOut).ownPeak == 0.0);
    CHECK(ownOut == 7.0);
    CHECK(childOut == 7.0);
    // One key: no loop.
    CHECK_FALSE(bakeSegmentTwist(&q, &q, 1, axis, &ownOut, &childOut).loops);
    CHECK(ownOut == doctest::Approx(1.0));
}
