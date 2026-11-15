// Inertial blending (ftcore/Inertial.h): no pop and no velocity step at a clip switch, the new clip
// exactly from the blend time on, no overshoot, chained switches, the short way round, bad dt, frame
// rate independence and no allocation per frame.

#include "ftcore/Inertial.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <memory>

using namespace ftcore;

namespace {

constexpr int kJ = 4;
using Pose = std::array<InertialJoint, kJ>;
constexpr double kPi = 3.14159265358979323846;

TwistQuat aboutZ(double angle) { return TwistQuat{0.0, 0.0, std::sin(0.5 * angle), std::cos(0.5 * angle)}; }
TwistQuat aboutAxis(double x, double y, double z, double angle) {
    const double n = std::sqrt(x * x + y * y + z * z);
    const double s = std::sin(0.5 * angle) / n;
    return TwistQuat{x * s, y * s, z * s, std::cos(0.5 * angle)};
}

// The angle between two rotations (radians, [0, pi]).
double angleBetween(const TwistQuat& a, const TwistQuat& b) {
    double d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    if (d > 1.0) d = 1.0;
    return 2.0 * std::acos(d);
}
double dist(const TwistVec& a, const TwistVec& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}
double poseGap(const Pose& a, const Pose& b) {
    double g = 0.0;
    for (int i = 0; i < kJ; ++i) {
        g = std::fmax(g, dist(a[i].translation, b[i].translation));
        g = std::fmax(g, angleBetween(a[i].rotation, b[i].rotation));
    }
    return g;
}
bool exactlyEqual(const Pose& a, const Pose& b) {
    for (int i = 0; i < kJ; ++i) {
        const InertialJoint& p = a[i];
        const InertialJoint& q = b[i];
        if (p.rotation.x != q.rotation.x || p.rotation.y != q.rotation.y || p.rotation.z != q.rotation.z ||
            p.rotation.w != q.rotation.w || p.translation.x != q.translation.x ||
            p.translation.y != q.translation.y || p.translation.z != q.translation.z)
            return false;
    }
    return true;
}

// The old clip: each joint moving at a constant rate (cm/s, rad/s about its own axis) from a base pose.
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
    // The new pose (newStatic) sits below and behind; the old pose moves toward it.
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
        p[i].rotation = aboutAxis(1.0, 0.5 * i, 0.25, 0.1 + 0.2 * i);
    }
    return p;
}

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
        // At t1 from the polynomial itself (just inside it).
        const double e = c.t1 * (1.0 - 1e-9);
        CHECK(std::fabs(c.rawValue(e)) < 1e-6);
        CHECK(std::fabs(c.velocity(e)) < 1e-4 * (1.0 + std::fabs(v)));
        CHECK(std::fabs(c.acceleration(e)) < 1e-3 * (1.0 + std::fabs(v) / c.t1));
        // The raw polynomial stays in [0, x0] over the whole curve.
        for (int k = 0; k <= 2000; ++k) {
            const double s = c.t1 * k / 2000.0;
            const double r = c.rawValue(s);
            CHECK(r <= 2.0 + 1e-9);
            CHECK(r >= -1e-9);
        }
    }
}

TEST_CASE("inertial: no pop at the switch, and the output starts at the old velocity") {
    const double dt = 1.0 / 60.0;
    const OldMotion m = towardMotion();
    const Pose prev = m.at(0.0), prevPrev = m.at(-dt), nw = newStatic();
    auto b = makeBlend();
    b->begin(prev, prevPrev, dt, nw, 0.4);
    CHECK(b->active());
    Pose out{};
    b->apply(nw, 0.0, out);
    CHECK(poseGap(out, prev) < 1e-4);

    // A tiny step: the output's velocity is the old velocity (translation and rotation). The old
    // motion points toward the new pose here (the part away from it is dropped by design).
    auto c = makeBlend();
    c->begin(prev, prevPrev, dt, nw, 0.4);
    const double h = 1e-5;
    Pose step{};
    c->apply(nw, h, step);
    const Pose oldNext = m.at(h);
    for (int i = 0; i < kJ; ++i) {
        const double vx = (step[i].translation.x - prev[i].translation.x) / h;
        const double vy = (step[i].translation.y - prev[i].translation.y) / h;
        const double ox = (oldNext[i].translation.x - prev[i].translation.x) / h;
        const double oy = (oldNext[i].translation.y - prev[i].translation.y) / h;
        // Along the offset direction the rate matches the old one.
        const TwistVec d{prev[i].translation.x - nw[i].translation.x, prev[i].translation.y - nw[i].translation.y,
                         prev[i].translation.z - nw[i].translation.z};
        const double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const double along = (vx * d.x + vy * d.y) / len;
        const double oldAlong = (ox * d.x + oy * d.y) / len;
        CHECK(along == doctest::Approx(oldAlong).epsilon(1e-3));
        const double turned = angleBetween(step[i].rotation, prev[i].rotation) / h;
        const double oldTurned = angleBetween(oldNext[i].rotation, prev[i].rotation) / h;
        CHECK(turned == doctest::Approx(oldTurned).epsilon(1e-3));
    }

    // Purely along the offset: the full velocity vector matches.
    Pose a{}, aa{}, z{};
    for (int i = 0; i < kJ; ++i) {
        a[i].translation = TwistVec{30.0, 0.0, 0.0};
        aa[i].translation = TwistVec{30.0 + 50.0 * dt, 0.0, 0.0}; // moving at -50 cm/s in x
        z[i].translation = TwistVec{0.0, 0.0, 0.0};
    }
    auto e = makeBlend();
    e->begin(a, aa, dt, z, 0.5);
    Pose s{};
    e->apply(z, h, s);
    CHECK((s[0].translation.x - 30.0) / h == doctest::Approx(-50.0).epsilon(1e-3));
    CHECK(std::fabs(s[0].translation.y) < 1e-12);
}

TEST_CASE("inertial: at and after the blend time the output is the new clip exactly") {
    const double dt = 1.0 / 60.0;
    const OldMotion m = towardMotion();
    auto b = makeBlend();
    Pose nw = newStatic();
    b->begin(m.at(0.0), m.at(-dt), dt, nw, 0.25);
    Pose out{};
    int frames = 0;
    while (b->active() && frames < 1000) {
        b->apply(nw, dt, out);
        ++frames;
    }
    CHECK(frames == 15); // 0.25 s at 60 steps per second
    CHECK(exactlyEqual(out, nw));
    for (int k = 0; k < 30; ++k) {
        for (int i = 0; i < kJ; ++i) nw[i].translation.y += 0.5; // the new clip moves on
        b->apply(nw, dt, out);
        CHECK(exactlyEqual(out, nw));
        CHECK_FALSE(b->active());
    }
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
                m.spin[i] = sp / 100.0; // toward and away from the target rotation
            }
            const Pose prev = m.at(0.0);
            auto b = makeBlend();
            b->begin(prev, m.at(-dt), dt, nw, 0.3);
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

TEST_CASE("inertial: a switch in the middle of a blend starts from the blended output") {
    const double dt = 1.0 / 60.0;
    const OldMotion m = towardMotion();
    const Pose clipB = newStatic();
    Pose clipC = newStatic();
    for (int i = 0; i < kJ; ++i) {
        clipC[i].translation.x += 40.0;
        clipC[i].rotation = aboutAxis(0.0, 1.0, 0.3, -0.9);
    }
    auto b = makeBlend();
    b->begin(m.at(0.0), m.at(-dt), dt, clipB, 0.4);
    Pose out{}, before{};
    for (int k = 0; k < 8; ++k) {
        before = out;
        b->apply(clipB, dt, out);
    }
    REQUIRE(b->active());
    const Pose last = out;
    // The caller passes the raw clip's poses: the state's own outputs are used instead.
    b->begin(clipB, clipB, dt, clipC, 0.4);
    Pose s{};
    b->apply(clipC, 0.0, s);
    CHECK(poseGap(s, last) < 1e-4);
    // The next frame carries on at about the blended output's velocity along the new offset.
    Pose s2{};
    b->apply(clipC, dt, s2);
    CHECK(poseGap(s2, last) < 0.5 * poseGap(last, clipC) + 1e-9);
    CHECK(poseGap(s2, last) < 3.0 * poseGap(last, before) + 0.02);

    // begin twice with no apply between: the second starts from the same pose.
    auto c = makeBlend();
    const Pose prev = m.at(0.0);
    c->begin(prev, m.at(-dt), dt, clipB, 0.4);
    c->begin(prev, m.at(-dt), dt, clipC, 0.4);
    Pose t{};
    c->apply(clipC, 0.0, t);
    CHECK(poseGap(t, prev) < 1e-4);
}

TEST_CASE("inertial: rotations take the short way round") {
    const double dt = 1.0 / 60.0;
    Pose oldPose{}, newPose{};
    for (int i = 0; i < kJ; ++i) {
        oldPose[i].rotation = aboutZ(350.0 * kPi / 180.0);
        newPose[i].rotation = aboutZ(0.0);
    }
    auto b = makeBlend();
    b->begin(oldPose, oldPose, dt, newPose, 0.3);
    CHECK(b->rotationCurve(0).x0 == doctest::Approx(10.0 * kPi / 180.0).epsilon(1e-9));
    Pose out{};
    b->apply(newPose, 0.0, out);
    CHECK(angleBetween(out[0].rotation, oldPose[0].rotation) < 1e-6);
    double prevAngle = 10.0 * kPi / 180.0;
    for (int k = 0; k < 20; ++k) {
        b->apply(newPose, dt, out);
        // The offset turns back toward 0 through -10 .. 0 degrees, never round the long way.
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
    b->begin(m.at(0.0), m.at(-dt), nan, nw, 0.3); // dtPrev bad: start rates 0
    CHECK(b->active());
    CHECK(b->translationCurve(0).v0 == 0.0);
    Pose out{};
    const double bad[] = {nan, inf, -inf, -1.0, 0.0};
    for (const double d : bad) {
        b->apply(nw, d, out);
        CHECK(b->elapsed() == 0.0);
        CHECK(poseGap(out, m.at(0.0)) < 1e-4);
    }
    // A non-finite joint in the new clip holds its last output.
    Pose broken = nw;
    broken[1].translation.x = nan;
    b->apply(broken, dt, out);
    for (int i = 0; i < kJ; ++i) {
        CHECK(std::isfinite(out[i].translation.x));
        CHECK(std::isfinite(out[i].rotation.w));
    }
    // Bad blend times end the blend; bad old poses give no offset.
    for (const double t : {nan, inf, 0.0, -1.0}) {
        auto c = makeBlend();
        c->begin(m.at(0.0), m.at(-dt), dt, nw, t);
        if (std::isfinite(t)) CHECK_FALSE(c->active());
        Pose o{};
        c->apply(nw, dt, o);
        if (!std::isfinite(t) || t <= 0.0) CHECK(exactlyEqual(o, nw));
    }
    Pose junk = m.at(0.0);
    junk[0].translation.y = inf;
    junk[2].rotation = TwistQuat{0.0, 0.0, 0.0, 0.0};
    auto d2 = makeBlend();
    d2->begin(junk, junk, dt, nw, 0.3);
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
    const OldMotion m = towardMotion();
    const Pose nw = newStatic();
    const int rates[] = {30, 60, 120};
    Pose at[3][10];
    for (int r = 0; r < 3; ++r) {
        const double dt = 1.0 / rates[r];
        auto b = makeBlend();
        b->begin(m.at(0.0), m.at(-dt), dt, nw, 0.3);
        const int per = rates[r] / 30;
        Pose out{};
        for (int k = 0; k < 10 * per; ++k) {
            b->apply(nw, dt, out);
            if ((k + 1) % per == 0) at[r][(k + 1) / per - 1] = out;
        }
    }
    for (int f = 0; f < 10; ++f) {
        CHECK(poseGap(at[0][f], at[1][f]) < 1e-6);
        CHECK(poseGap(at[0][f], at[2][f]) < 1e-6);
    }
}

TEST_CASE("inertial: begin and apply do not allocate") {
    const double dt = 1.0 / 60.0;
    const OldMotion m = towardMotion();
    const Pose prev = m.at(0.0), prevPrev = m.at(-dt), nw = newStatic();
    auto b = makeBlend();
    Pose out{};
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    for (int k = 0; k < 600; ++k) {
        if (k % 20 == 0) b->begin(prev, prevPrev, dt, nw, 0.3);
        b->apply(nw, dt, out);
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    if (counting) CHECK(allocs == 0);
}
