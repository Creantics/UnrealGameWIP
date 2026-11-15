#pragma once

// Inertial blending ("inertialization") of a skeleton's pose across a clip switch. At the switch the
// offset from the new clip's pose to the last output (per joint: a rotation as an axis and an angle,
// a translation as a direction and a length) and its rate of change (the old output's velocity minus
// the new clip's) are taken, then each offset's size decays to zero on a quintic curve that starts at
// that rate and ends at rest (zero size, velocity and acceleration). Each frame the decayed offset is
// laid on top of the new clip's pose, so only one clip plays and the switch does not pop.
//
// The curve per offset (x0 the start size, v0 its rate, t1 the blend time), after the usual
// formulation:
//  - a start rate pointing away from the target (v0 > 0, the offset growing) is taken as 0, so the
//    offset never grows past x0; only the part of the relative velocity along a translation's offset,
//    and about a rotation's offset axis, is kept;
//  - t1 is shortened to -5 x0 / v0 when the start rate would overshoot zero before t1 (floored at
//    kInertialMinTime, the rate then lowered to match);
//  - a0 = max(0, (-8 v0 t1 - 20 x0) / t1^2), and x(t) = A t^5 + B t^4 + C t^3 + a0/2 t^2 + v0 t + x0
//    with A, B, C making x, x' and x'' zero at t1. The value is kept in [0, x0] (a guard only: the
//    polynomial stays there on its own, see the tests).
//
// Units are seconds, centimetres and radians. Rotations are unit quaternions (TwistQuat, x y z the
// vector part); the offset is the rotation from the new pose to the old one, out = offset * new,
// the short way round (an offset of 350 degrees is -10). Every divide is guarded; a non-finite or
// negative dt counts as 0, a non-finite or zero-length joint is held or skipped (see apply).
//
// No heap allocation anywhere: the state holds fixed arrays for kInertialMaxJoints joints (about
// 136 KiB, so keep it as a member or in a pool, not on the stack). Joints past the capacity are passed through
// from the new clip unblended. Header only: Core/src has a matching Unreal wrapper per source file,
// and this needs none.

#include "ftcore/TwistBake.h" // TwistVec, TwistQuat

#include <cmath>
#include <cstddef>
#include <span>

namespace ftcore {

inline constexpr int kInertialMaxJoints = 512;
inline constexpr double kInertialMinTime = 1e-4;   // the shortest curve, seconds
inline constexpr double kInertialMinOffset = 1e-6; // a translation offset below this (cm) is none
inline constexpr double kInertialMinAngle = 1e-7;  // a rotation offset below this (rad) is none
inline constexpr double kInertialEndSlack = 1e-9;  // elapsed within this of the end time ends it
inline constexpr double kInertialMinDt = 1e-6;     // a begin dt below this gives no velocity (noise)

// One joint's local pose: rotation then translation (parent space, cm).
struct InertialJoint {
    TwistQuat rotation;
    TwistVec translation;
};

// Where a begin() takes the old output from.
enum class InertialFrom : unsigned char {
    Given,    // the prevPose and prevPrevPose passed in
    Continue, // the blender's own last two outputs, when it can (see begin)
};

// The decay of one offset's size. init() fixes it; value(t) is the size t seconds after the switch.
struct InertialCurve {
    double x0 = 0.0, v0 = 0.0, a0 = 0.0, t1 = 0.0;
    double a = 0.0, b = 0.0, c = 0.0; // the t^5, t^4 and t^3 terms

    void init(double startSize, double startRate, double blendTime) {
        *this = InertialCurve{};
        if (!std::isfinite(startSize) || !std::isfinite(blendTime) || startSize <= 0.0 || blendTime <= 0.0) return;
        double v = std::isfinite(startRate) ? startRate : 0.0;
        if (v > 0.0) v = 0.0; // moving away: no overshoot
        double t = blendTime;
        if (v < 0.0) {
            const double reach = -5.0 * startSize / v;
            if (reach < t) t = reach;
        }
        if (t < kInertialMinTime) {
            t = kInertialMinTime;
            v = -5.0 * startSize / t;
        }
        const double t2 = t * t, t3 = t2 * t, t4 = t3 * t, t5 = t4 * t;
        double acc = (-8.0 * v * t - 20.0 * startSize) / t2;
        if (!(acc > 0.0)) acc = 0.0;
        x0 = startSize;
        v0 = v;
        a0 = acc;
        t1 = t;
        a = -(acc * t2 + 6.0 * v * t + 12.0 * startSize) / (2.0 * t5);
        b = (3.0 * acc * t2 + 16.0 * v * t + 30.0 * startSize) / (2.0 * t4);
        c = -(3.0 * acc * t2 + 12.0 * v * t + 20.0 * startSize) / (2.0 * t3);
    }

    // The polynomial alone, without the [0, x0] guard (0 past t1, x0 before 0).
    double rawValue(double t) const {
        if (!(t > 0.0)) return x0;
        if (t >= t1) return 0.0;
        return ((((a * t + b) * t + c) * t + 0.5 * a0) * t + v0) * t + x0;
    }
    double value(double t) const {
        const double x = rawValue(t);
        return x < 0.0 ? 0.0 : (x > x0 ? x0 : x);
    }
    double velocity(double t) const {
        if (t >= t1) return 0.0;
        if (t < 0.0) t = 0.0;
        return (((5.0 * a * t + 4.0 * b) * t + 3.0 * c) * t + a0) * t + v0;
    }
    double acceleration(double t) const {
        if (t >= t1) return 0.0;
        if (t < 0.0) t = 0.0;
        return ((20.0 * a * t + 12.0 * b) * t + 6.0 * c) * t + a0;
    }
};

namespace inertial_detail {

inline bool finite(const TwistVec& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline bool finite(const TwistQuat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}
inline bool finite(const InertialJoint& j) { return finite(j.rotation) && finite(j.translation); }

// q scaled to unit length; false (and identity) when it is zero or not finite.
inline bool normalized(const TwistQuat& q, TwistQuat& out) {
    out = TwistQuat{};
    if (!finite(q)) return false;
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-12) || !std::isfinite(n)) return false;
    out = TwistQuat{q.x / n, q.y / n, q.z / n, q.w / n};
    return true;
}

inline TwistQuat mul(const TwistQuat& p, const TwistQuat& q) {
    return TwistQuat{p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y,
                     p.w * q.y - p.x * q.z + p.y * q.w + p.z * q.x,
                     p.w * q.z + p.x * q.y - p.y * q.x + p.z * q.w,
                     p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z};
}
inline TwistQuat conj(const TwistQuat& q) { return TwistQuat{-q.x, -q.y, -q.z, q.w}; }

// The signed angle about the unit axis u of the rotation old * inverse(nw) (both unit), the short way.
inline double twistAbout(const TwistQuat& old, const TwistQuat& nw, const TwistVec& u) {
    TwistQuat q = mul(old, conj(nw));
    if (q.w < 0.0) q = TwistQuat{-q.x, -q.y, -q.z, -q.w};
    return 2.0 * std::atan2(q.x * u.x + q.y * u.y + q.z * u.z, q.w);
}

} // namespace inertial_detail

// One skeleton's blend. begin() at each clip switch, apply() every frame (blending or not).
class InertialBlend {
public:
    // Starts a blend to the new clip.
    //  - prevPose, prevPrevPose: the last two outputs (prevPose the latest, dtPrev seconds later).
    //  - newPose: the new clip at the switch; newPrevPose: the new clip dtNew seconds earlier (its
    //    velocity is taken out of the offset's rate). An empty newPrevPose, a non-finite joint in it
    //    or dtNew below kInertialMinDt counts the new clip as still for that joint (dtPrev below it
    //    likewise gives the old side no velocity: a finite difference over a tiny step is noise).
    //    A rotation term whose angle changed by more than pi over its step (the offset crossing
    //    180 degrees, where the measured angle wraps) counts as 0.
    //  - from = Continue takes the old output from the blender's own last two outputs instead of
    //    prevPose and prevPrevPose, but only when apply ran since the last begin, the history holds
    //    two outputs and its joint count equals this begin's; otherwise the given poses are used.
    //    Returns true when it continued.
    // Joints up to min(newPose, prevPose, kInertialMaxJoints) get an offset. blendTime not
    // finite or <= 0 ends any blend: the next apply outputs the new clip. A joint whose old or new
    // pose is not finite or has a zero rotation gets no offset; a non-finite prevPrevPose joint or
    // dtPrev gives the old side a velocity of 0.
    bool begin(std::span<const InertialJoint> prevPose, std::span<const InertialJoint> prevPrevPose, double dtPrev,
               std::span<const InertialJoint> newPose, std::span<const InertialJoint> newPrevPose, double dtNew,
               double blendTime, InertialFrom from = InertialFrom::Given) {
        using namespace inertial_detail;
        if (!std::isfinite(dtPrev) || dtPrev < kInertialMinDt) dtPrev = 0.0;
        if (!std::isfinite(dtNew) || dtNew < kInertialMinDt) dtNew = 0.0;
        std::size_t nNew = newPose.size();
        if (nNew > kMax) nNew = kMax;
        const bool chained =
            from == InertialFrom::Continue && appliedSinceBegin_ && history_ >= 2 && count_ == nNew && nNew > 0;
        std::size_t n = nNew;
        if (!chained) {
            if (prevPose.size() < n) n = prevPose.size();
            for (std::size_t i = 0; i < n; ++i) {
                buf_[cur_][i] = prevPose[i];
                buf_[cur_ ^ 1u][i] = i < prevPrevPose.size() ? prevPrevPose[i] : prevPose[i];
            }
            count_ = n;
            lastDt_ = dtPrev;
            history_ = 2;
        } else {
            dtPrev = lastDt_;
        }
        appliedSinceBegin_ = false;
        const InertialJoint* last = buf_[cur_];
        const InertialJoint* before = buf_[cur_ ^ 1u];

        offsets_ = 0;
        elapsed_ = 0.0;
        end_ = 0.0;
        blend_ = 0.0;
        active_ = false;
        if (!std::isfinite(blendTime) || blendTime <= 0.0 || n == 0) return chained;
        blend_ = blendTime;
        offsets_ = n;

        for (std::size_t i = 0; i < n; ++i) {
            const InertialJoint& p = last[i];
            const InertialJoint& pp = before[i];
            const InertialJoint& nw = newPose[i];
            const bool oldMoves = dtPrev > 0.0 && finite(pp);
            const bool newMoves = dtNew > 0.0 && i < newPrevPose.size() && finite(newPrevPose[i]);

            // Translation: along the offset's direction. The rate is the change from the old side
            // moving (new held) plus the change from the new side moving (old held).
            InertialCurve& tc = move_[i];
            tc = InertialCurve{};
            dir_[i] = TwistVec{};
            if (finite(p.translation) && finite(nw.translation)) {
                const TwistVec d{p.translation.x - nw.translation.x, p.translation.y - nw.translation.y,
                                 p.translation.z - nw.translation.z};
                const double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                if (len > kInertialMinOffset && std::isfinite(len)) {
                    const TwistVec u{d.x / len, d.y / len, d.z / len};
                    double rate = 0.0;
                    if (oldMoves) {
                        const double back = (pp.translation.x - nw.translation.x) * u.x +
                                            (pp.translation.y - nw.translation.y) * u.y +
                                            (pp.translation.z - nw.translation.z) * u.z;
                        rate += (len - back) / dtPrev;
                    }
                    if (newMoves) {
                        const TwistVec& np = newPrevPose[i].translation;
                        const double back = (p.translation.x - np.x) * u.x + (p.translation.y - np.y) * u.y +
                                            (p.translation.z - np.z) * u.z;
                        rate += (len - back) / dtNew;
                    }
                    dir_[i] = u;
                    tc.init(len, rate, blendTime);
                }
            }

            // Rotation: the angle about the axis from the new pose to the old one, the short way.
            InertialCurve& rc = turn_[i];
            rc = InertialCurve{};
            axis_[i] = TwistVec{};
            TwistQuat qp, qn;
            if (normalized(p.rotation, qp) && normalized(nw.rotation, qn)) {
                TwistQuat q0 = mul(qp, conj(qn));
                if (q0.w < 0.0) q0 = TwistQuat{-q0.x, -q0.y, -q0.z, -q0.w};
                const double s = std::sqrt(q0.x * q0.x + q0.y * q0.y + q0.z * q0.z);
                const double angle = 2.0 * std::atan2(s, q0.w);
                if (s > 0.0 && angle > kInertialMinAngle && std::isfinite(angle)) {
                    const TwistVec u{q0.x / s, q0.y / s, q0.z / s};
                    double rate = 0.0;
                    TwistQuat other;
                    // A change of more than pi over a step is the wrap at 180 degrees, not motion.
                    if (oldMoves && normalized(pp.rotation, other)) {
                        const double change = angle - twistAbout(other, qn, u);
                        if (std::fabs(change) <= kPi) rate += change / dtPrev;
                    }
                    if (newMoves && normalized(newPrevPose[i].rotation, other)) {
                        const double change = angle - twistAbout(qp, other, u);
                        if (std::fabs(change) <= kPi) rate += change / dtNew;
                    }
                    axis_[i] = u;
                    rc.init(angle, rate, blendTime);
                }
            }
            if (tc.t1 > end_) end_ = tc.t1;
            if (rc.t1 > end_) end_ = rc.t1;
        }
        active_ = end_ > 0.0; // no offset anywhere: nothing to blend
        if (!active_) offsets_ = 0;
        return chained;
    }

    // One frame: the new clip's pose dt seconds after the last apply (or after the switch), with
    // the decayed offsets on top, into outPose. Writes min(newPose, outPose) joints; those past
    // kInertialMaxJoints are copied from newPose as they are. The first apply after begin with
    // dt = 0 gives the start pose itself. A joint of newPose that is not finite holds its last
    // output. Once every offset has reached zero (the longest per-joint curve) the output is the
    // new pose exactly and active() turns false.
    void apply(std::span<const InertialJoint> newPose, double dt, std::span<InertialJoint> outPose) {
        using namespace inertial_detail;
        if (!std::isfinite(dt) || dt < 0.0) dt = 0.0;
        std::size_t all = newPose.size();
        if (outPose.size() < all) all = outPose.size();
        const std::size_t n = all > kMax ? kMax : all;

        if (active_) {
            elapsed_ += dt;
            if (elapsed_ >= end_ - kInertialEndSlack) {
                active_ = false;
                offsets_ = 0;
            }
        }
        const InertialJoint* last = buf_[cur_];
        const std::size_t lastN = history_ > 0 ? count_ : 0;
        for (std::size_t i = 0; i < n; ++i) {
            const InertialJoint& nw = newPose[i];
            InertialJoint& o = outPose[i];
            if (!finite(nw)) {
                o = i < lastN ? last[i] : InertialJoint{};
                continue;
            }
            o = nw;
            if (!active_ || i >= offsets_) continue;
            const double len = move_[i].value(elapsed_);
            if (len > 0.0) {
                o.translation.x += dir_[i].x * len;
                o.translation.y += dir_[i].y * len;
                o.translation.z += dir_[i].z * len;
            }
            const double angle = turn_[i].value(elapsed_);
            if (angle > 0.0) {
                const double h = 0.5 * angle;
                const double s = std::sin(h);
                const TwistQuat q{axis_[i].x * s, axis_[i].y * s, axis_[i].z * s, std::cos(h)};
                o.rotation = mul(q, nw.rotation);
            }
        }
        for (std::size_t i = n; i < all; ++i) outPose[i] = newPose[i]; // past the capacity: unblended

        // History: the last two outputs, moved on only when time moved.
        if (dt > 0.0) {
            cur_ ^= 1u;
            lastDt_ = dt;
            if (history_ < 2) ++history_;
        } else if (history_ == 0) {
            history_ = 1;
        }
        for (std::size_t i = 0; i < n; ++i) buf_[cur_][i] = outPose[i];
        // Joints new to the history have no earlier output: take this one (whatever dt was).
        if (history_ >= 2 && count_ < n)
            for (std::size_t i = count_; i < n; ++i) buf_[cur_ ^ 1u][i] = outPose[i];
        count_ = n;
        appliedSinceBegin_ = true;
    }

    // True while any joint's offset is not yet zero (until the longest per-joint curve ends).
    bool active() const { return active_; }
    double elapsed() const { return elapsed_; }
    double blendSeconds() const { return blend_; } // as asked in begin
    double endSeconds() const { return end_; }     // when the last offset reaches zero
    // The decay curves of joint i (for checks); empty past the joints begin covered.
    const InertialCurve& translationCurve(std::size_t i) const { return i < offsets_ ? move_[i] : empty_; }
    const InertialCurve& rotationCurve(std::size_t i) const { return i < offsets_ ? turn_[i] : empty_; }
    // Ends any blend and forgets the output history.
    void reset() {
        active_ = false;
        appliedSinceBegin_ = false;
        elapsed_ = blend_ = end_ = lastDt_ = 0.0;
        offsets_ = count_ = 0;
        history_ = 0;
    }

private:
    static constexpr std::size_t kMax = static_cast<std::size_t>(kInertialMaxJoints);
    static constexpr double kPi = 3.14159265358979323846;

    InertialCurve move_[kMax];
    InertialCurve turn_[kMax];
    TwistVec dir_[kMax];
    TwistVec axis_[kMax];
    InertialJoint buf_[2][kMax]; // the last two outputs; buf_[cur_] the latest
    InertialCurve empty_;
    std::size_t offsets_ = 0; // joints with an offset in this blend
    std::size_t count_ = 0;   // joints in the history
    unsigned cur_ = 0;
    int history_ = 0;         // outputs in the history (0..2)
    double lastDt_ = 0.0;     // seconds between the two outputs in the history
    double elapsed_ = 0.0;
    double end_ = 0.0;
    double blend_ = 0.0;
    bool active_ = false;
    bool appliedSinceBegin_ = false;
};

} // namespace ftcore
