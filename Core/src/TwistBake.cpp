#include "ftcore/TwistBake.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {
constexpr double kPi = 3.14159265358979323846;

double dot(const TwistVec& a, const TwistVec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Into (-pi, pi].
double wrap(double a) {
    a = std::fmod(a + kPi, 2.0 * kPi);
    if (a <= 0.0) {
        a += 2.0 * kPi;
    }
    return a - kPi;
}
} // namespace

double twistAngle(const TwistQuat& q, const TwistVec& axis) {
    const double len = std::sqrt(dot(axis, axis));
    if (len <= 0.0) {
        return 0.0;
    }
    // The twist part is (w, (v . a) a) normalised; its angle is 2 atan2(v . a, w). atan2(0, 0) is 0, so a
    // swing of 180 degrees (w and v . a both zero) stays finite.
    const double along = dot(TwistVec{q.x, q.y, q.z}, axis) / len;
    return wrap(2.0 * std::atan2(along, q.w));
}

double unwrapAngle(double angle, double previous) {
    return previous + wrap(angle - previous);
}

double segmentFraction(const TwistVec& boneOffset, const TwistVec& childOffset) {
    const double length2 = dot(childOffset, childOffset);
    if (length2 <= 0.0) {
        return 0.0;
    }
    return std::clamp(dot(boneOffset, childOffset) / length2, 0.0, 1.0);
}

double twistBoneRoll(double fraction, double childTwist, double ownTwist) {
    return fraction * childTwist - (1.0 - fraction) * ownTwist;
}

TwistQuat axisAngle(const TwistVec& axis, double angle) {
    const double len = std::sqrt(dot(axis, axis));
    if (len <= 0.0) {
        return {};
    }
    const double s = std::sin(angle * 0.5) / len;
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5)};
}

} // namespace ftcore
