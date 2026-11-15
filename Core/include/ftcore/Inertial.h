#pragma once

// Inertial blending ("inertialization") of a skeleton's pose across a clip switch. At the switch the
// offset from the new clip's pose to the last output (per joint: a rotation as an axis and an angle,
// a translation as a direction and a length) and its rate of change are taken, then each offset's
// size decays to zero over the blend time on a quintic curve that starts at the old velocity and
// ends at rest (zero size, velocity and acceleration). Each frame the decayed offset is laid on top
// of the new clip's pose, so only one clip plays and the switch does not pop.
//
// The curve per offset (x0 the start size, v0 its rate, t1 the blend time), after the usual
// formulation:
//  - a start rate pointing away from the target (v0 > 0, the offset growing) is taken as 0, so the
//    offset never grows past x0; only the part of a translation's velocity along the offset is kept;
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
// 70 KB, so keep it as a member, not on the stack). Header only: Core/src has a matching Unreal
// wrapper per source file, and this needs none.

#include "ftcore/TwistBake.h" // TwistVec, TwistQuat

#include <cmath>
#include <cstddef>
#include <span>

namespace ftcore {

inline constexpr int kInertialMaxJoints = 256;
inline constexpr double kInertialMinTime = 1e-4;       // the shortest curve, seconds
inline constexpr double kInertialMinOffset = 1e-6;     // a translation offset below this (cm) is none
inline constexpr double kInertialMinAngle = 1e-7;      // a rotation offset below this (rad) is none
inline constexpr double kInertialEndSlack = 1e-6;      // elapsed within this of the blend time ends it

// One joint's local pose: rotation then translation (parent space, cm).
struct InertialJoint {
    TwistQuat rotation;
    TwistVec translation;
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

} // namespace inertial_detail

// One skeleton's blend. begin() at each clip switch, apply() every frame (blending or not).
class InertialBlend {
public:
    // Starts a blend to the new clip from the last two output poses (prevPose the latest, dtPrev
    // seconds after prevPrevPose), newPose being the new clip's pose at the switch. While a blend
    // is running the state's own last two outputs are used instead of prevPose and prevPrevPose
    // (so a chained switch starts from the blended output, not a raw clip). Joints past
    // kInertialMaxJoints or past the shortest span get no offset. blendSeconds not finite or <= 0
    // ends any blend: the next apply outputs the new clip. A joint whose old or new pose is not
    // finite or has a zero rotation gets no offset; a non-finite prevPrevPose joint or dtPrev
    // gives that offset a start rate of 0.
    void begin(std::span<const InertialJoint> prevPose, std::span<const InertialJoint> prevPrevPose, double dtPrev,
               std::span<const InertialJoint> newPose, double blendSeconds) {
        using namespace inertial_detail;
        const bool chained = active_ && history_ >= 2;
        if (!std::isfinite(dtPrev) || dtPrev < 0.0) dtPrev = 0.0;
        std::size_t n = newPose.size();
        if (n > kMax) n = kMax;
        if (!chained) {
            if (prevPose.size() < n) n = prevPose.size();
        } else if (count_ < n) {
            n = count_;
        }
        const std::size_t prevPrevN = chained ? count_ : prevPrevPose.size();

        // Seed the history from the poses the blend starts from (so begin twice, or begin then
        // apply(dt = 0), sees the same start).
        if (!chained) {
            for (std::size_t i = 0; i < n; ++i) {
                buf_[cur_][i] = prevPose[i];
                buf_[cur_ ^ 1u][i] = i < prevPrevN ? prevPrevPose[i] : prevPose[i];
            }
            count_ = n;
            lastDt_ = dtPrev;
            history_ = 2;
        } else {
            dtPrev = lastDt_;
        }
        const InertialJoint* last = buf_[cur_];
        const InertialJoint* before = buf_[cur_ ^ 1u];

        offsets_ = 0;
        elapsed_ = 0.0;
        blend_ = 0.0;
        active_ = false;
        if (!std::isfinite(blendSeconds) || blendSeconds <= 0.0 || n == 0) return;
        blend_ = blendSeconds;
        active_ = true;
        offsets_ = n;

        for (std::size_t i = 0; i < n; ++i) {
            const InertialJoint& p = last[i];
            const InertialJoint& pp = before[i];
            const InertialJoint& nw = newPose[i];
            const bool ppOk = dtPrev > 0.0 && finite(pp);

            // Translation: along the offset's direction.
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
                    if (ppOk) {
                        const double back = (pp.translation.x - nw.translation.x) * u.x +
                                            (pp.translation.y - nw.translation.y) * u.y +
                                            (pp.translation.z - nw.translation.z) * u.z;
                        rate = (len - back) / dtPrev;
                    }
                    dir_[i] = u;
                    tc.init(len, rate, blendSeconds);
                }
            }

            // Rotation: the angle about the axis from the new pose to the old one, the short way.
            InertialCurve& rc = turn_[i];
            rc = InertialCurve{};
            axis_[i] = TwistVec{};
            TwistQuat qp, qn;
            if (normalized(p.rotation, qp) && normalized(nw.rotation, qn)) {
                const TwistQuat inv = conj(qn);
                TwistQuat q0 = mul(qp, inv);
                if (q0.w < 0.0) q0 = TwistQuat{-q0.x, -q0.y, -q0.z, -q0.w};
                const double s = std::sqrt(q0.x * q0.x + q0.y * q0.y + q0.z * q0.z);
                const double angle = 2.0 * std::atan2(s, q0.w);
                if (s > 0.0 && angle > kInertialMinAngle && std::isfinite(angle)) {
                    const TwistVec u{q0.x / s, q0.y / s, q0.z / s};
                    double rate = 0.0;
                    TwistQuat qpp;
                    if (ppOk && normalized(pp.rotation, qpp)) {
                        TwistQuat qm = mul(qpp, inv);
                        if (qm.w < 0.0) qm = TwistQuat{-qm.x, -qm.y, -qm.z, -qm.w};
                        const double along = qm.x * u.x + qm.y * u.y + qm.z * u.z;
                        const double back = 2.0 * std::atan2(along, qm.w); // twist about the axis
                        rate = (angle - back) / dtPrev;
                    }
                    axis_[i] = u;
                    rc.init(angle, rate, blendSeconds);
                }
            }
        }
    }

    // One frame: the new clip's pose dt seconds after the last apply (or after the switch), with
    // the decayed offsets on top, into outPose. Writes min(newPose, outPose, kInertialMaxJoints)
    // joints. The first apply after begin with dt = 0 gives the start pose itself. A joint of
    // newPose that is not finite holds its last output. At and past the blend time the output is
    // the new pose exactly and the blend ends.
    void apply(std::span<const InertialJoint> newPose, double dt, std::span<InertialJoint> outPose) {
        using namespace inertial_detail;
        if (!std::isfinite(dt) || dt < 0.0) dt = 0.0;
        std::size_t n = newPose.size();
        if (outPose.size() < n) n = outPose.size();
        if (n > kMax) n = kMax;

        if (active_) {
            elapsed_ += dt;
            if (elapsed_ >= blend_ - kInertialEndSlack) active_ = false;
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

        // History: the last two outputs, moved on only when time moved.
        if (dt > 0.0) {
            cur_ ^= 1u;
            lastDt_ = dt;
            if (history_ < 2) ++history_;
        } else if (history_ == 0) {
            history_ = 1;
        }
        for (std::size_t i = 0; i < n; ++i) buf_[cur_][i] = outPose[i];
        if (dt > 0.0 && history_ >= 2 && count_ < n) {
            // Joints new to the history have no earlier output: take this one.
            for (std::size_t i = count_; i < n; ++i) buf_[cur_ ^ 1u][i] = outPose[i];
        }
        count_ = n;
    }

    bool active() const { return active_; }
    double elapsed() const { return elapsed_; }
    double blendSeconds() const { return blend_; }
    // The decay curves of joint i (for checks); empty past the joints begin covered.
    const InertialCurve& translationCurve(std::size_t i) const { return i < offsets_ ? move_[i] : empty_; }
    const InertialCurve& rotationCurve(std::size_t i) const { return i < offsets_ ? turn_[i] : empty_; }
    // Ends any blend and forgets the output history.
    void reset() {
        active_ = false;
        elapsed_ = blend_ = lastDt_ = 0.0;
        offsets_ = count_ = 0;
        history_ = 0;
    }

private:
    static constexpr std::size_t kMax = static_cast<std::size_t>(kInertialMaxJoints);

    InertialCurve move_[kMax];
    InertialCurve turn_[kMax];
    TwistVec dir_[kMax];
    TwistVec axis_[kMax];
    InertialJoint buf_[2][kMax]; // the last two outputs; buf_[cur_] the latest
    InertialCurve empty_;
    std::size_t offsets_ = 0;    // joints with an offset in this blend
    std::size_t count_ = 0;      // joints in the history
    unsigned cur_ = 0;
    int history_ = 0;            // outputs in the history (0..2)
    double lastDt_ = 0.0;        // seconds between the two outputs in the history
    double elapsed_ = 0.0;
    double blend_ = 0.0;
    bool active_ = false;
};

} // namespace ftcore
