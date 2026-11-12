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

namespace {
// The angle between two rotations (q and -q the same), in [0, pi].
double rotationGap(const TwistQuat& a, const TwistQuat& b) {
    const double na = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
    const double nb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w);
    if (na <= 0.0 || nb <= 0.0) {
        return 0.0;
    }
    const double d = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) / (na * nb);
    return 2.0 * std::acos(std::clamp(d, 0.0, 1.0));
}

// The faded, unwrapped twist of each key of one rotation track.
double fadedTwists(const TwistQuat* delta, int keys, const TwistVec& axis, double* out) {
    double unwrapped = 0.0, peak = 0.0;
    for (int k = 0; k < keys; ++k) {
        const double fade = twistFade(swingAngle(delta[k], axis));
        unwrapped = unwrapFadedTwist(twistAngle(delta[k], axis), k > 0 ? unwrapped : 0.0, fade);
        out[k] = unwrapped * fade;
        peak = std::max(peak, std::abs(out[k]));
    }
    return peak;
}
} // namespace

TwistSegmentSummary bakeSegmentTwist(const TwistQuat* ownDelta, const TwistQuat* childDelta, int keys,
                                     const TwistVec& axis, double* ownOut, double* childOut) {
    TwistSegmentSummary s;
    if (keys < 1 || ownDelta == nullptr || ownOut == nullptr || childOut == nullptr) {
        return s;
    }
    s.ownPeak = fadedTwists(ownDelta, keys, axis, ownOut);
    if (childDelta != nullptr) {
        s.childPeak = fadedTwists(childDelta, keys, axis, childOut);
    } else {
        for (int k = 0; k < keys; ++k) {
            childOut[k] = 0.0;
        }
    }
    const int last = keys - 1;
    constexpr double kSamePose = 0.02;
    s.loops = keys > 1 && rotationGap(ownDelta[0], ownDelta[last]) < kSamePose &&
              (childDelta == nullptr || rotationGap(childDelta[0], childDelta[last]) < kSamePose);
    s.loopPops = s.loops && (std::abs(ownOut[last] - ownOut[0]) > kPi || std::abs(childOut[last] - childOut[0]) > kPi);
    return s;
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
