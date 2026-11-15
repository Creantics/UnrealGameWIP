// Inertial blending (ftcore/Inertial.h): no pop and no velocity step at a clip switch (with a still
// and a moving new clip, old and new rotations about different axes), the new clip exactly once the
// offsets end, active() matching the offsets, no overshoot, chained switches, the joint capacity and
// span lengths, the short way round, bad dt, frame rate independence and no allocation per frame.

#include "ftcore/Inertial.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <vector>

using namespace ftcore;

namespace {

constexpr int kJ = 4;
using Pose = std::array<InertialJoint, kJ>;
constexpr double kPi = 3.14159265358979323846;
const std::span<const InertialJoint> kStill{}; // no previous new-clip pose: the new clip counts as still

TwistQuat aboutAxis(double x, double y, double z, double angle) {
    const double n = std::sqrt(x * x + y * y + z * z);
    const double s = std::sin(0.5 * angle) / n;
    return TwistQuat{x * s, y * s, z * s, std::cos(0.5 * angle)};
}
TwistQuat aboutZ(double angle) { return aboutAxis(0.0, 0.0, 1.0, angle); }
TwistQuat mul(const TwistQuat& p, const TwistQuat& q) { return inertial_detail::mul(p, q); }

// The angle between two rotations (radians, [0, pi]); atan2, not acos, so tiny angles keep their digits.
double angleBetween(const TwistQuat& a, const TwistQuat& b) {
    const TwistQuat q = mul(a, inertial_detail::conj(b));
    const double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    return 2.0 * std::atan2(s, std::fabs(q.w));
}
double dist(const TwistVec& a, const TwistVec& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}
double jointGap(const InertialJoint& a, const InertialJoint& b) {
    return std::fmax(dist(a.translation, b.translation), angleBetween(a.rotation, b.rotation));
}
template <class A, class B> double poseGap(const A& a, const B& b, std::size_t n = kJ) {
    double g = 0.0;
    for (std::size_t i = 0; i < n; ++i) g = std::fmax(g, jointGap(a[i], b[i]));
    return g;
}
bool same(const InertialJoint& p, const InertialJoint& q) {
    return p.rotation.x == q.rotation.x && p.rotation.y == q.rotation.y && p.rotation.z == q.rotation.z &&
           p.rotation.w == q.rotation.w && p.translation.x == q.translation.x &&
           p.translation.y == q.translation.y && p.translation.z == q.translation.z;
}
bool exactlyEqual(const Pose& a, const Pose& b) {
    for (int i = 0; i < kJ; ++i)
        if (!same(a[i], b[i])) return false;
    return true;
}

// The old clip: each joint moving at a constant rate (cm/s, rad/s about its own axis). Its axes
// differ from the new pose's, so a wrong compose order cannot pass the no-pop checks.
struct OldMotion {
    TwistVec vel[kJ];
    double spin[kJ];
    Pose at(double t) const {
        Pose p{};
        for (int i = 0; i < kJ; ++i) {
            p[i].translation = TwistVec{10.0 * i + vel[i].x * t, 5.0 + vel[i].y * t, -3.0 * i + vel[i].z * t};
            p[i].rotation = aboutAxis(1.0, 0.5 * i, 0.25, 0.6 + 0.2 * i + spin[i] * t);
        }
        return p;
    }
};
OldMotion towardMotion() {
    OldMotion m{};
    for (int i = 0; i < kJ; ++i) {
        m.vel[i] = TwistVec{-20.0 - 5.0 * i, -10.0, 0.0};
        m.spin[i] = -0.8;
    }
    return m;
}
Pose newStatic() {
    Pose p{};
    for (int i = 0; i < kJ; ++i) {
        p[i].translation = TwistVec{10.0 * i - 8.0, 1.0, -3.0 * i};
        p[i].rotation = aboutAxis(0.2, 1.0, -0.3 - 0.1 * i, 0.1 + 0.2 * i);
    }
    return p;
}

// A switch where both clips move, built so the relative motion is purely along each joint's offset
// (direction dirs, axis axes): old = Rot(axis, th0 + w t) * base, new = Rot(axis, wn t) * base, and
// old translation = base + 30 dir + V t, new = base + W t, with V = v dir and W = vn dir. The base
// rotations turn about yet another axis.
struct MovingSwitch {
    double v, vn, w, wn;
    TwistVec dir(int i) const {
        const TwistVec d{1.0, 2.0 + 0.3 * i, -1.0};
        const double n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        return TwistVec{d.x / n, d.y / n, d.z / n};
    }
    static TwistVec axisOf(int i) { return TwistVec{0.3, -1.0, 0.5 + 0.2 * i}; }
    static TwistQuat base(int i) { return aboutAxis(1.0, 0.2, 0.0, 0.7 + 0.1 * i); }
    static TwistVec basePos(int i) { return TwistVec{5.0 * i, -2.0, 1.0 + i}; }
    Pose oldAt(double t) const {
        Pose p{};
        for (int i = 0; i < kJ; ++i) {
            const TwistVec a = axisOf(i), d = dir(i), b = basePos(i);
            p[i].rotation = mul(aboutAxis(a.x, a.y, a.z, 0.8 + w * t), base(i));
            const double s = 30.0 + v * t;
            p[i].translation = TwistVec{b.x + d.x * s, b.y + d.y * s, b.z + d.z * s};
        }
        return p;
    }
    Pose newAt(double t) const {
        Pose p{};
        for (int i = 0; i < kJ; ++i) {
            const TwistVec a = axisOf(i), d = dir(i), b = basePos(i);
            p[i].rotation = mul(aboutAxis(a.x, a.y, a.z, wn * t), base(i));
            const double s = vn * t;
            p[i].translation = TwistVec{b.x + d.x * s, b.y + d.y * s, b.z + d.z * s};
        }
        return p;
    }
};

std::unique_ptr<InertialBlend> makeBlend() { return std::make_unique<InertialBlend>(); }

} // namespace

TEST_CASE("inertial: the curve starts at x0 and v0 and ends at rest, within [0, x0]") {
    const double rates[] = {-1000.0, -100.0, -20.0, -5.0, -1.0, 0.0, 1.0, 10.0, 1000.0};
    for (const double v : rates) {
        InertialCurve c;
        c.init(2.0, v, 0.3);
        CHECK(c.rawValue(0.0) == doctest::Approx(2.0));
        if (v <= 0.0) CHECK(c.velocity(0.0) == doctest::Approx(v).epsilon(1e-9));
        else CHECK(c.velocity(0.0) == 0.0); // away: clamped
        CHECK(c.t1 <= 0.3 + 1e-12);
        CHECK(c.t1 >= kInertialMinTime);
        const double e = c.t1 * (1.0 - 1e-9); // just inside t1, from the polynomial itself
        CHECK(std::fabs(c.rawValue(e)) < 1e-6);
        CHECK(std::fabs(c.velocity(e)) < 1e-4 * (1.0 + std::fabs(v)));
        CHECK(std::fabs(c.acceleration(e)) < 1e-3 * (1.0 + std::fabs(v) / c.t1));
        for (int k = 0; k <= 2000; ++k) {
            const double r = c.rawValue(c.t1 * k / 2000.0);
            CHECK(r <= 2.0 + 1e-9);
            CHECK(r >= -1e-9);
        }
    }
}

TEST_CASE("inertial: no pop at the switch (old and new rotations about different axes)") {
    const double dt = 1.0 / 60.0;
    const OldMotion m = towardMotion();
    const Pose prev = m.at(0.0), nw = newStatic();
    // The test can fail: composing the offset on the wrong side would not give the old pose back.
    TwistQuat q0 = mul(prev[1].rotation, inertial_detail::conj(nw[1].rotation));
    REQUIRE(angleBetween(mul(nw[1].rotation, q0), prev[1].rotation) > 0.01);

    auto b = makeBlend();
    b->begin(prev, m.at(-dt), dt, nw, kStill, 0.0, 0.4);
    CHECK(b->active());
    Pose out{};
    b->apply(nw, 0.0, out);
    CHECK(poseGap(out, prev) < 1e-4);
}

TEST_CASE("inertial: a moving new clip: the output starts at the old velocity and T is its own") {
    const double dt = 1.0 / 60.0, h = 1e-5, blend = 0.4;
    // The new clip runs 200 cm/s one way while the old output comes 50 cm/s the other: the offset
    // closes at 250 cm/s, and it turns at -1.5 - 2.0 rad/s.
    const MovingSwitch s{-50.0, 200.0, -1.5, 2.0};
    auto b = makeBlend();
    b->begin(s.oldAt(0.0), s.oldAt(-dt), dt, s.newAt(0.0), s.newAt(-dt), dt, blend);
    for (int i = 0; i < kJ; ++i) {
        CHECK(b->translationCurve(static_cast<std::size_t>(i)).v0 == doctest::Approx(-250.0).epsilon(1e-6));
        CHECK(b->rotationCurve(static_cast<std::size_t>(i)).v0 == doctest::Approx(-3.5).epsilon(1e-6));
        CHECK(b->translationCurve(static_cast<std::size_t>(i)).t1 == doctest::Approx(blend)); // 5 * 30 / 250 > 0.4
    }
    Pose out{};
    b->apply(s.newAt(0.0), 0.0, out);
    CHECK(poseGap(out, s.oldAt(0.0)) < 1e-4);
    b->apply(s.newAt(h), h, out);
    // The output a moment later is where the old output would be: same velocity, rotation and translation.
    const Pose oldNext = s.oldAt(h), oldNow = s.oldAt(0.0);
    for (int i = 0; i < kJ; ++i) {
        const double vel = dist(out[i].translation, oldNow[i].translation) / h;
        CHECK(vel == doctest::Approx(50.0).epsilon(1e-3));
        CHECK(dist(out[i].translation, oldNext[i].translation) / h < 0.05);
        CHECK(angleBetween(out[i].rotation, oldNext[i].rotation) / h < 1e-3);
        CHECK(angleBetween(out[i].rotation, oldNow[i].rotation) / h == doctest::Approx(1.5).epsilon(1e-3));
    }
}

TEST_CASE("inertial: clips moving together: no fake rate, the blend keeps its time") {
    const double dt = 1.0 / 60.0, h = 1e-5, blend = 0.3;
    const MovingSwitch s{-600.0, -600.0, -3.0, -3.0}; // a foot moving at 600 cm/s in both clips
    auto b = makeBlend();
    b->begin(s.oldAt(0.0), s.oldAt(-dt), dt, s.newAt(0.0), s.newAt(-dt), dt, blend);
    for (int i = 0; i < kJ; ++i) {
        CHECK(std::fabs(b->translationCurve(static_cast<std::size_t>(i)).v0) < 1e-6);
        CHECK(std::fabs(b->rotationCurve(static_cast<std::size_t>(i)).v0) < 1e-6);
        CHECK(b->translationCurve(static_cast<std::size_t>(i)).t1 == doctest::Approx(blend));
        CHECK(b->rotationCurve(static_cast<std::size_t>(i)).t1 == doctest::Approx(blend));
    }
    CHECK(b->endSeconds() == doctest::Approx(blend));
    Pose out{};
    b->apply(s.newAt(h), h, out);
    const Pose oldNext = s.oldAt(h);
    for (int i = 0; i < kJ; ++i) {
        CHECK(dist(out[i].translation, oldNext[i].translation) / h < 0.05);
        CHECK(angleBetween(out[i].rotation, oldNext[i].rotation) / h < 1e-3);
    }
    // The same switch with the new clip treated as still (the old API) cuts T to 5 * 30 / 600.
    auto c = makeBlend();
    c->begin(s.oldAt(0.0), s.oldAt(-dt), dt, s.newAt(0.0), kStill, 0.0, blend);
    CHECK(c->translationCurve(0).t1 == doctest::Approx(0.25));
}

TEST_CASE("inertial: the old velocity along the offset, still new clip, different axes") {
    const double dt = 1.0 / 60.0, h = 1e-5;
    const MovingSwitch s{-60.0, 0.0, -1.2, 0.0};
    auto b = makeBlend();
    b->begin(s.oldAt(0.0), s.oldAt(-dt), dt, s.newAt(0.0), kStill, 0.0, 0.5);
    Pose out{};
    b->apply(s.newAt(0.0), h, out);
    const Pose oldNext = s.oldAt(h);
    for (int i = 0; i < kJ; ++i) {
        CHECK(dist(out[i].translation, oldNext[i].translation) / h < 0.05);
        CHECK(angleBetween(out[i].rotation, oldNext[i].rotation) / h < 1e-3);
    }
}

TEST_CASE("inertial: once the offsets end the output is the new clip exactly") {
    const double dt = 1.0 / 60.0;
    const OldMotion m = towardMotion();
    auto b = makeBlend();
    Pose nw = newStatic();
    b->begin(m.at(0.0), m.at(-dt), dt, nw, kStill, 0.0, 0.25);
    const double end = b->endSeconds();
    CHECK(end <= 0.25 + 1e-12);
    Pose out{};
    int frames = 0;
    while (b->active() && frames < 1000) {
        b->apply(nw, dt, out);
        ++frames;
    }
    CHECK(frames == static_cast<int>(std::ceil((end - 1e-9) / dt)));
    CHECK(exactlyEqual(out, nw));
    for (int k = 0; k < 30; ++k) {
        for (int i = 0; i < kJ; ++i) nw[i].translation.y += 0.5;
        b->apply(nw, dt, out);
        CHECK(exactlyEqual(out, nw));
        CHECK_FALSE(b->active());
    }
}

TEST_CASE("inertial: active() matches the offset") {
    const double dt = 1.0 / 120.0;
    // Shortened curves: the old output rushes at the new pose, every curve ends well before the blend
    // time, and active() goes false with them (not at the blend time).
    Pose nw{}, prev{}, prevPrev{};
    for (int i = 0; i < kJ; ++i) {
        nw[i].rotation = aboutAxis(0.0, 1.0, 0.0, 0.2);
        prev[i].rotation = mul(aboutAxis(1.0, 0.0, 0.0, 0.1), nw[i].rotation);
        prevPrev[i].rotation = mul(aboutAxis(1.0, 0.0, 0.0, 0.1 + 20.0 * dt), nw[i].rotation); // 20 rad/s in
        prev[i].translation = TwistVec{2.0, 0.0, 0.0};
        prevPrev[i].translation = TwistVec{2.0 + 400.0 * dt, 0.0, 0.0}; // 400 cm/s in
    }
    auto b = makeBlend();
    b->begin(prev, prevPrev, dt, nw, kStill, 0.0, 1.0);
    CHECK(b->endSeconds() < 0.1);
    CHECK(b->blendSeconds() == 1.0);
    Pose out{};
    int steps = 0;
    while (steps < 200) {
        b->apply(nw, dt, out);
        ++steps;
        if (b->active()) CHECK(poseGap(out, nw) > 0.0);
        else break;
    }
    CHECK(exactlyEqual(out, nw));
    CHECK(b->elapsed() < 0.1 + dt);

    // A blend under the shortest curve: active() stays true until the floored curve ends.
    auto c = makeBlend();
    c->begin(prev, prev, dt, nw, kStill, 0.0, 5e-5);
    CHECK(c->endSeconds() == doctest::Approx(kInertialMinTime));
    c->apply(nw, 6e-5, out);
    CHECK(c->active());
    CHECK(poseGap(out, nw) > 1e-6);
    c->apply(nw, 5e-5, out);
    CHECK_FALSE(c->active());
    CHECK(exactlyEqual(out, nw));

    // No offset at all: nothing to blend.
    auto d = makeBlend();
    d->begin(nw, nw, dt, nw, kStill, 0.0, 0.3);
    CHECK_FALSE(d->active());
}

TEST_CASE("inertial: the offset never grows past its start, for any start velocity") {
    const double dt = 1.0 / 60.0;
    const Pose nw = newStatic();
    const double speeds[] = {-5000.0, -300.0, -40.0, 0.0, 40.0, 300.0, 5000.0};
    const TwistVec dirs[] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, {0.6, -0.8, 0.0}};
    for (const double sp : speeds) {
        for (const TwistVec& dv : dirs) {
            OldMotion m{};
            for (int i = 0; i < kJ; ++i) {
                m.vel[i] = TwistVec{dv.x * sp, dv.y * sp, dv.z * sp};
                m.spin[i] = sp / 100.0;
            }
            const Pose prev = m.at(0.0);
            auto b = makeBlend();
            b->begin(prev, m.at(-dt), dt, nw, kStill, 0.0, 0.3);
            double startMove[kJ], startTurn[kJ];
            for (int i = 0; i < kJ; ++i) {
                startMove[i] = dist(prev[i].translation, nw[i].translation);
                startTurn[i] = angleBetween(prev[i].rotation, nw[i].rotation);
            }
            Pose out{};
            for (int k = 0; k < 40; ++k) {
                b->apply(nw, dt, out);
                for (int i = 0; i < kJ; ++i) {
                    CHECK(dist(out[i].translation, nw[i].translation) <= startMove[i] + 1e-9);
                    CHECK(angleBetween(out[i].rotation, nw[i].rotation) <= startTurn[i] + 1e-7);
                }
            }
            CHECK(exactlyEqual(out, nw));
        }
    }
}

TEST_CASE("inertial: chained switches: Continue from the blended output, or the poses given") {
    const double dt = 1.0 / 60.0;
    const OldMotion m = towardMotion();
    const Pose clipB = newStatic();
    Pose clipC = newStatic();
    for (int i = 0; i < kJ; ++i) {
        clipC[i].translation.x += 40.0;
        clipC[i].rotation = aboutAxis(0.0, 1.0, 0.3, -0.9);
    }
    auto b = makeBlend();
    CHECK_FALSE(b->begin(m.at(0.0), m.at(-dt), dt, clipB, kStill, 0.0, 0.4, InertialFrom::Continue)); // no apply yet
    Pose out{}, before{};
    for (int k = 0; k < 8; ++k) {
        before = out;
        b->apply(clipB, dt, out);
    }
    REQUIRE(b->active());
    const Pose last = out;
    // The caller passes the raw clip's poses; Continue uses the blender's own outputs.
    CHECK(b->begin(clipB, clipB, dt, clipC, kStill, 0.0, 0.4, InertialFrom::Continue));
    Pose s{};
    b->apply(clipC, 0.0, s);
    CHECK(poseGap(s, last) < 1e-4);
    Pose s2{};
    b->apply(clipC, dt, s2);
    CHECK(poseGap(s2, last) < 3.0 * poseGap(last, before) + 0.02);

    // Given: the poses passed in, even mid-blend.
    auto g = makeBlend();
    g->begin(m.at(0.0), m.at(-dt), dt, clipB, kStill, 0.0, 0.4);
    g->apply(clipB, dt, out);
    CHECK_FALSE(g->begin(clipB, clipB, dt, clipC, kStill, 0.0, 0.4, InertialFrom::Given));
    g->apply(clipC, 0.0, s);
    CHECK(poseGap(s, clipB) < 1e-4);

    // begin twice with no apply between: Continue refuses and the second starts from the given poses.
    auto c = makeBlend();
    const Pose prev = m.at(0.0);
    c->begin(prev, m.at(-dt), dt, clipB, kStill, 0.0, 0.4);
    CHECK_FALSE(c->begin(clipB, clipB, dt, clipC, kStill, 0.0, 0.4, InertialFrom::Continue));
    c->apply(clipC, 0.0, s);
    CHECK(poseGap(s, clipB) < 1e-4);

    // A joint count change since the last apply: Continue refuses.
    auto e = makeBlend();
    e->begin(prev, m.at(-dt), dt, clipB, kStill, 0.0, 0.4);
    e->apply(clipB, dt, out);
    const std::span<const InertialJoint> three(clipC.data(), 3);
    CHECK_FALSE(e->begin(clipB, clipB, dt, three, kStill, 0.0, 0.4, InertialFrom::Continue));
}

TEST_CASE("inertial: the history after the joint count grows with dt = 0") {
    const double dt = 1.0 / 60.0;
    Pose far{};
    for (int i = 0; i < kJ; ++i) far[i].translation = TwistVec{1000.0, -500.0, 300.0};
    const Pose still = newStatic();
    auto b = makeBlend();
    Pose out{};
    b->apply(far, dt, out); // the history holds far poses for all four joints
    b->apply(far, dt, out);
    const std::span<const InertialJoint> two(still.data(), 2);
    std::array<InertialJoint, 2> out2{};
    b->apply(two, dt, out2); // two joints now
    b->apply(two, dt, out2);
    b->apply(still, 0.0, out); // back to four, no time step
    Pose target = still;
    for (int i = 0; i < kJ; ++i) target[i].translation.z += 20.0;
    REQUIRE(b->begin(still, still, dt, target, kStill, 0.0, 0.3, InertialFrom::Continue));
    // Joints 2 and 3 have one real output (still), so no velocity: nothing from the stale far poses.
    for (std::size_t i = 0; i < 4; ++i) CHECK(std::fabs(b->translationCurve(i).v0) < 1e-9);
    Pose s{};
    b->apply(target, 0.0, s);
    CHECK(poseGap(s, still) < 1e-4);
}

TEST_CASE("inertial: past the capacity and spans of different lengths, every joint is written") {
    const double dt = 1.0 / 60.0;
    const std::size_t big = static_cast<std::size_t>(kInertialMaxJoints) + 88;
    std::vector<InertialJoint> prev(big), nw(big), out(big);
    for (std::size_t i = 0; i < big; ++i) {
        const double f = static_cast<double>(i);
        prev[i].translation = TwistVec{f, 10.0, 0.0};
        prev[i].rotation = aboutAxis(1.0, 0.0, 0.0, 0.5);
        nw[i].translation = TwistVec{f, 0.0, 5.0};
        nw[i].rotation = aboutAxis(0.0, 0.0, 1.0, -0.3);
    }
    auto b = makeBlend();
    b->begin(prev, prev, dt, nw, kStill, 0.0, 0.3);
    b->apply(nw, 0.0, out);
    for (std::size_t i = 0; i < big; ++i) {
        if (i < static_cast<std::size_t>(kInertialMaxJoints)) CHECK(jointGap(out[i], prev[i]) < 1e-4);
        else CHECK(same(out[i], nw[i])); // unblended, but written
    }

    // outPose shorter than newPose: only outPose's joints; newPose shorter: outPose's tail untouched.
    std::vector<InertialJoint> shortOut(300);
    b->apply(nw, dt, shortOut);
    for (std::size_t i = 0; i < shortOut.size(); ++i) CHECK(std::isfinite(shortOut[i].translation.x));
    InertialJoint sentinel{};
    sentinel.translation = TwistVec{-7.0, -7.0, -7.0};
    std::vector<InertialJoint> longOut(big + 10, sentinel);
    b->apply(nw, dt, longOut);
    for (std::size_t i = big; i < longOut.size(); ++i) CHECK(same(longOut[i], sentinel));

    // prevPose shorter than newPose: the joints past it get no offset.
    auto c = makeBlend();
    const std::span<const InertialJoint> prevShort(prev.data(), 100);
    c->begin(prevShort, prevShort, dt, nw, kStill, 0.0, 0.3);
    c->apply(nw, 0.0, out);
    for (std::size_t i = 0; i < 100; ++i) CHECK(jointGap(out[i], prev[i]) < 1e-4);
    for (std::size_t i = 100; i < big; ++i) CHECK(same(out[i], nw[i]));
}

TEST_CASE("inertial: rotations take the short way round") {
    const double dt = 1.0 / 60.0;
    Pose oldPose{}, newPose{};
    for (int i = 0; i < kJ; ++i) {
        oldPose[i].rotation = aboutZ(350.0 * kPi / 180.0);
        newPose[i].rotation = aboutZ(0.0);
    }
    auto b = makeBlend();
    b->begin(oldPose, oldPose, dt, newPose, kStill, 0.0, 0.3);
    CHECK(b->rotationCurve(0).x0 == doctest::Approx(10.0 * kPi / 180.0).epsilon(1e-9));
    Pose out{};
    b->apply(newPose, 0.0, out);
    CHECK(angleBetween(out[0].rotation, oldPose[0].rotation) < 1e-6);
    double prevAngle = 10.0 * kPi / 180.0;
    for (int k = 0; k < 20; ++k) {
        b->apply(newPose, dt, out);
        const double z = 2.0 * std::atan2(out[0].rotation.z, out[0].rotation.w);
        CHECK(z <= 1e-9);
        CHECK(z >= -10.0 * kPi / 180.0 - 1e-9);
        const double a = angleBetween(out[0].rotation, newPose[0].rotation);
        CHECK(a <= prevAngle + 1e-9);
        prevAngle = a;
    }
}

TEST_CASE("inertial: non-finite and zero dt are safe") {
    const double dt = 1.0 / 60.0;
    const OldMotion m = towardMotion();
    const Pose nw = newStatic();
    const double inf = INFINITY;
    const double nan = std::nan("");
    auto b = makeBlend();
    b->begin(m.at(0.0), m.at(-dt), nan, nw, m.at(-dt), inf, 0.3); // bad dts: rates 0
    CHECK(b->active());
    CHECK(b->translationCurve(0).v0 == 0.0);
    CHECK(b->rotationCurve(0).v0 == 0.0);
    Pose out{};
    for (const double d : {nan, inf, -inf, -1.0, 0.0}) {
        b->apply(nw, d, out);
        CHECK(b->elapsed() == 0.0);
        CHECK(poseGap(out, m.at(0.0)) < 1e-4);
    }
    Pose broken = nw;
    broken[1].translation.x = nan;
    b->apply(broken, dt, out);
    for (int i = 0; i < kJ; ++i) {
        CHECK(std::isfinite(out[i].translation.x));
        CHECK(std::isfinite(out[i].rotation.w));
    }
    for (const double t : {nan, inf, 0.0, -1.0}) {
        auto c = makeBlend();
        c->begin(m.at(0.0), m.at(-dt), dt, nw, kStill, 0.0, t);
        CHECK_FALSE(c->active());
        Pose o{};
        c->apply(nw, dt, o);
        CHECK(exactlyEqual(o, nw));
    }
    Pose junk = m.at(0.0);
    junk[0].translation.y = inf;
    junk[2].rotation = TwistQuat{0.0, 0.0, 0.0, 0.0};
    auto d2 = makeBlend();
    d2->begin(junk, junk, dt, nw, junk, dt, 0.3);
    Pose o{};
    d2->apply(nw, dt, o);
    for (int i = 0; i < kJ; ++i) {
        CHECK(std::isfinite(o[i].translation.x));
        CHECK(std::isfinite(o[i].translation.y));
        CHECK(std::isfinite(o[i].rotation.w));
    }
    CHECK(o[0].translation.y == nw[0].translation.y);
}

TEST_CASE("inertial: 30, 60 and 120 steps per second agree at the same times") {
    const MovingSwitch s{-50.0, 120.0, -1.0, 1.5};
    const int rates[] = {30, 60, 120};
    Pose at[3][10];
    for (int r = 0; r < 3; ++r) {
        const double dt = 1.0 / rates[r];
        auto b = makeBlend();
        b->begin(s.oldAt(0.0), s.oldAt(-dt), dt, s.newAt(0.0), s.newAt(-dt), dt, 0.3);
        const int per = rates[r] / 30;
        Pose out{};
        for (int k = 0; k < 10 * per; ++k) {
            b->apply(s.newAt((k + 1) * dt), dt, out);
            if ((k + 1) % per == 0) at[r][(k + 1) / per - 1] = out;
        }
    }
    for (int f = 0; f < 10; ++f) {
        CHECK(poseGap(at[0][f], at[1][f]) < 1e-6);
        CHECK(poseGap(at[0][f], at[2][f]) < 1e-6);
    }
}

TEST_CASE("inertial: begin and apply do not allocate") {
    REQUIRE(ftcore_test::allocHookInstalled()); // the count below means nothing without the hook
    const double dt = 1.0 / 60.0;
    const MovingSwitch s{-50.0, 200.0, -1.5, 2.0};
    const Pose prev = s.oldAt(0.0), prevPrev = s.oldAt(-dt), nw = s.newAt(0.0), nwPrev = s.newAt(-dt);
    auto b = makeBlend();
    Pose out{};
    const auto before = ftcore_test::allocStats().count;
    for (int k = 0; k < 600; ++k) {
        if (k % 20 == 0) b->begin(prev, prevPrev, dt, nw, nwPrev, dt, 0.3, InertialFrom::Continue);
        b->apply(nw, dt, out);
    }
    CHECK(ftcore_test::allocStats().count - before == 0);
}
