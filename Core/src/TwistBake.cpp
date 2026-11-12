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

double swingAngle(const TwistQuat& q, const TwistVec& axis) {
    const double len = std::sqrt(dot(axis, axis));
    if (len <= 0.0) {
        return 0.0;
    }
    const TwistVec a{axis.x / len, axis.y / len, axis.z / len};
    // q a q^-1 = a + 2 w (u x a) + 2 u x (u x a), u the vector part; normalised so a non-unit q still works.
    const double n2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (n2 <= 0.0) {
        return 0.0;
    }
    const TwistVec u{q.x, q.y, q.z};
    const TwistVec c{u.y * a.z - u.z * a.y, u.z * a.x - u.x * a.z, u.x * a.y - u.y * a.x};
    const TwistVec cc{u.y * c.z - u.z * c.y, u.z * c.x - u.x * c.z, u.x * c.y - u.y * c.x};
    const TwistVec r{a.x + 2.0 * (q.w * c.x + cc.x) / n2, a.y + 2.0 * (q.w * c.y + cc.y) / n2,
                     a.z + 2.0 * (q.w * c.z + cc.z) / n2};
    const double rl = std::sqrt(dot(r, r));
    return rl > 0.0 ? std::acos(std::clamp(dot(a, r) / rl, -1.0, 1.0)) : 0.0;
}

double twistFade(double swing) {
    const double lo = 100.0 * kPi / 180.0, hi = 160.0 * kPi / 180.0;
    const double t = std::clamp((hi - swing) / (hi - lo), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double unwrapAngle(double angle, double previous) {
    return previous + wrap(angle - previous);
}

double unwrapFadedTwist(double angle, double previous, double fade) {
    return fade < 0.05 ? wrap(angle) : unwrapAngle(angle, previous);
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
