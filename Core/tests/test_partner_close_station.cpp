// Rule 19: the partner's station under the close arena camera (PartnerCloseStation.h), alone and in the brain.
// The lens in these scenes stands as the close preset puts it: 2.6 m behind the lead, 0.8 m over a shoulder, aimed so
// the target sits 0.2 of the half-width off centre, 30 degrees each side (a 60 degree horizontal field).
#include "ftcore/PartnerBrain.h"
#include "ftcore/PartnerCloseStation.h"
#include "AllocCounter.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>

using namespace ftcore;

namespace {

constexpr float kCsDeg = 3.14159265358979f / 180.f;
constexpr float kCsHalf = 30.f;

Vec2 csSub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 csAdd(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 csMul(Vec2 a, float k) { return {a.x * k, a.y * k}; }
float csLen(Vec2 a) { return std::hypot(a.x, a.y); }
Vec2 csUnit(Vec2 a) { return csMul(a, 1.f / csLen(a)); }
Vec2 csTurn(Vec2 v, float deg) {
    const float c = std::cos(deg * kCsDeg), s = std::sin(deg * kCsDeg);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}
Vec2 csRight(Vec2 f) { return {-f.y, f.x}; } // the lens's level right (x forward, y right at yaw 0)
float csDot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
// The angle at the target between the lead and p (degrees).
float csAngleAt(Vec2 target, Vec2 lead, Vec2 p) {
    const float c = csDot(csUnit(csSub(lead, target)), csUnit(csSub(p, target)));
    return std::acos(std::fmax(-1.f, std::fmin(1.f, c))) / kCsDeg;
}
float csNdc(const PartnerCloseInput& in, Vec2 p) {
    float x = 99.f;
    partnerCloseNdc(in.cameraPos, in.cameraForward, in.halfAngleDegrees, p, x);
    return x;
}

// The close lens on the lead looking at the target: `shoulder` m to the lens's right (positive: the lead left of
// centre), the forward turned so the target sits at `aimNdc` (`half`: the lens's half-width, 30 by default).
void csLens(Vec2 lead, Vec2 target, float shoulder, float aimNdc, Vec2& cam, Vec2& fwd, float half = kCsHalf) {
    const Vec2 d = csUnit(csSub(target, lead));
    cam = csAdd(csAdd(lead, csMul(d, -2.6f)), csMul(csRight(d), shoulder));
    const Vec2 aim = csUnit(csSub(target, cam));
    fwd = csTurn(aim, -std::atan(aimNdc * std::tan(half * kCsDeg)) / kCsDeg);
}

// A target of radius 1 at the origin, the lead 5 m out on -x, the lens over its right shoulder (lead left of centre).
PartnerCloseInput csScene(std::int8_t leadSide = -1, float shoulder = 0.8f, float aimNdc = 0.2f) {
    PartnerCloseInput in;
    in.lead = {-5.f, 0.f};
    in.target = {0.f, 0.f};
    in.targetRadius = 1.f;
    csLens(in.lead, in.target, shoulder, aimNdc, in.cameraPos, in.cameraForward);
    in.halfAngleDegrees = kCsHalf;
    in.leadSide = leadSide;
    in.partner = {-4.f, -3.f};
    return in;
}

// The circle-strafe sweep: the lead circles the target at 3 m/s (dir +1 or -1), the lens on it as the close preset;
// the partner walks 4.5 m/s to its station and stands still while attacking (1.2 s of every 3 s).
struct CsSweep {
    float inCone = 0.f;  // share of steps the partner is inside the lens's own edges
    float clear = 0.f;   // ... and neither overlaps the lead nor stands hidden behind the target
    int flips = 0;
    int flipsAttacking = 0;
    double shortestHold = 1e9; // seconds between two flank changes
};
CsSweep csSweep(float targetRadius, float orbit, float standMin, float standMax, float dir) {
    PartnerCloseInput in;
    in.target = {0.f, 0.f};
    in.targetRadius = targetRadius;
    in.halfAngleDegrees = kCsHalf;
    in.leadSide = -1;
    in.standMin = standMin;
    in.standMax = standMax;
    in.partner = {orbit, 2.f};
    PartnerCloseHold hold;
    CsSweep r;
    const float dt = 1.f / 60.f, w = dir * 3.f / orbit;
    int n = 0, cone = 0, clear = 0;
    std::int8_t last = 0;
    double lastFlipAt = -1.0;
    for (int s = 0; s < 30 * 60; ++s) {
        const float t = static_cast<float>(s) * dt;
        in.lead = {orbit * std::cos(w * t), orbit * std::sin(w * t)};
        csLens(in.lead, in.target, 0.8f, 0.2f, in.cameraPos, in.cameraForward);
        in.attacking = std::fmod(t, 3.f) < 1.2f;
        hold.heldFor += dt;
        const PartnerCloseStation st = partnerCloseStation(in, hold);
        if (hold.flank != last && last != 0) {
            ++r.flips;
            if (in.attacking) ++r.flipsAttacking;
            if (lastFlipAt >= 0.0) r.shortestHold = std::fmin(r.shortestHold, static_cast<double>(t) - lastFlipAt);
            lastFlipAt = static_cast<double>(t);
        }
        last = hold.flank;
        if (st.valid && !in.attacking) {
            const Vec2 v = csSub(st.point, in.partner);
            const float l = csLen(v), step = 4.5f * dt;
            in.partner = l <= step ? st.point : csAdd(in.partner, csMul(v, step / l));
        }
        ++n;
        if (partnerCloseInCone(in.cameraPos, in.cameraForward, kCsHalf, 0.f, in.partner)) {
            ++cone;
            if (!partnerCloseOverlaps(in.cameraPos, in.partner, 0.4f, in.lead, 0.4f) &&
                !partnerCloseHiddenBehind(in.cameraPos, in.partner, in.target, in.targetRadius))
                ++clear;
        }
    }
    r.inCone = static_cast<float>(cone) / static_cast<float>(n);
    r.clear = static_cast<float>(clear) / static_cast<float>(n);
    return r;
}

} // namespace

TEST_CASE("close station: the cone measure on both sides of the screen") {
    const Vec2 cam{0.f, 0.f}, fwd{2.f, 0.f}; // need not be unit
    const float t30 = std::tan(kCsHalf * kCsDeg);
    float x = 0.f;
    REQUIRE(partnerCloseNdc(cam, fwd, kCsHalf, {5.f, 2.5f * t30}, x));
    CHECK(x == doctest::Approx(0.5f)); // +y: the lens's right
    REQUIRE(partnerCloseNdc(cam, fwd, kCsHalf, {5.f, -2.5f * t30}, x));
    CHECK(x == doctest::Approx(-0.5f));
    CHECK_FALSE(partnerCloseNdc(cam, fwd, kCsHalf, {-1.f, 0.f}, x)); // behind the lens
    CHECK_FALSE(partnerCloseNdc(cam, {0.f, 0.f}, kCsHalf, {5.f, 0.f}, x)); // no forward
    for (const float side : {-1.f, 1.f}) {
        CAPTURE(side);
        CHECK(partnerCloseInCone(cam, fwd, kCsHalf, kPartnerCloseConeMargin, {5.f, side * 0.75f * 5.f * t30}));
        CHECK_FALSE(partnerCloseInCone(cam, fwd, kCsHalf, kPartnerCloseConeMargin, {5.f, side * 0.85f * 5.f * t30}));
        CHECK(partnerCloseInCone(cam, fwd, kCsHalf, 0.f, {5.f, side * 0.95f * 5.f * t30}));
        CHECK_FALSE(partnerCloseInCone(cam, fwd, kCsHalf, 0.f, {5.f, side * 1.05f * 5.f * t30}));
    }

    // The station: in the cone with the margin, ahead of the lead, between the screen's edge and the lead on its side.
    struct Case {
        std::int8_t side;
        float shoulder, aim;
        Vec2 partner;
    };
    for (const Case c : {Case{-1, 0.8f, 0.2f, {-4.f, -3.f}}, Case{1, -0.8f, -0.2f, {-4.f, 3.f}}}) {
        CAPTURE(static_cast<int>(c.side));
        PartnerCloseInput in = csScene(c.side, c.shoulder, c.aim);
        in.partner = c.partner;
        const float leadX = csNdc(in, in.lead);
        CHECK(leadX * static_cast<float>(c.side) > 0.f); // the lead on its side of the centre
        PartnerCloseHold hold;
        const PartnerCloseStation st = partnerCloseStation(in, hold);
        REQUIRE(st.valid);
        CHECK(partnerCloseInCone(in.cameraPos, in.cameraForward, kCsHalf, kPartnerCloseConeMargin, st.point));
        CHECK(csNdc(in, st.point) * static_cast<float>(c.side) >= leadX * static_cast<float>(c.side));
        const float a = csAngleAt(in.target, in.lead, st.point);
        CHECK(a >= kPartnerCloseAngleMin - 0.01f);
        CHECK(a <= kPartnerCloseAngleMax + 0.01f);
        CHECK(hold.flank == st.flank);
        CHECK(hold.heldFor == 0.0);
    }
}

TEST_CASE("close station: never hidden behind the lead or the target, never covering the lead") {
    const Vec2 cam{0.f, 0.f};
    CHECK(partnerCloseOverlaps(cam, {6.f, 0.1f}, 0.4f, {3.f, 0.f}, 0.4f));  // behind the lead
    CHECK(partnerCloseOverlaps(cam, {1.5f, 0.1f}, 0.4f, {3.f, 0.f}, 0.4f)); // in front of it, covering it
    CHECK_FALSE(partnerCloseOverlaps(cam, {6.f, 3.f}, 0.4f, {3.f, 0.f}, 0.4f));
    CHECK(partnerCloseHiddenBehind(cam, {8.f, 0.5f}, {5.f, 0.f}, 1.f));
    CHECK_FALSE(partnerCloseHiddenBehind(cam, {3.f, 0.2f}, {5.f, 0.f}, 1.f)); // in front of the target
    CHECK_FALSE(partnerCloseHiddenBehind(cam, {8.f, 4.f}, {5.f, 0.f}, 1.f));

    // Over a sweep of lead bearings, partners and target sizes, every station found is clear of both.
    int found = 0;
    for (int a = 0; a < 360; a += 15)
        for (const float tr : {0.5f, 1.f, 2.5f})
            for (const float px : {-6.f, -2.f, 2.f}) {
                PartnerCloseInput in;
                in.target = {0.f, 0.f};
                in.targetRadius = tr;
                in.lead = csMul({std::cos(static_cast<float>(a) * kCsDeg), std::sin(static_cast<float>(a) * kCsDeg)}, tr + 4.f);
                csLens(in.lead, in.target, 0.8f, 0.2f, in.cameraPos, in.cameraForward);
                in.halfAngleDegrees = kCsHalf;
                in.leadSide = -1;
                in.partner = {px, 3.f};
                PartnerCloseHold hold;
                const PartnerCloseStation st = partnerCloseStation(in, hold);
                if (!st.valid) continue;
                ++found;
                CHECK_FALSE(partnerCloseOverlaps(in.cameraPos, st.point, in.bodyRadius, in.lead, in.bodyRadius));
                CHECK_FALSE(partnerCloseHiddenBehind(in.cameraPos, st.point, in.target, in.targetRadius));
                CHECK(csLen(csSub(st.point, in.lead)) >= in.personalSpace - 1e-4f);
            }
    CHECK(found >= 200); // of 216
}

TEST_CASE("close station: the flank holds 1.5 s and never changes while attacking") {
    // A centred lens (no side preference) and the partner standing on flank -1's ground: -1 is the better flank.
    PartnerCloseInput in = csScene(0, 0.f, 0.f);
    in.partner = {-1.5f, 2.6f};
    {
        PartnerCloseHold unheld;
        const PartnerCloseStation st = partnerCloseStation(in, unheld);
        REQUIRE(st.valid);
        CHECK(st.flank == -1);
        CHECK_FALSE(st.held);
    }
    PartnerCloseHold hold{1, 60.f, 0.5}; // flank +1 held 0.5 s
    PartnerCloseStation st = partnerCloseStation(in, hold);
    REQUIRE(st.valid);
    CHECK(st.flank == 1);
    CHECK(st.held);
    CHECK(hold.heldFor == 0.5); // kept: its clock runs on
    hold.heldFor = 2.0;
    in.attacking = true; // past the hold, but attacking: kept
    st = partnerCloseStation(in, hold);
    REQUIRE(st.valid);
    CHECK(st.flank == 1);
    CHECK(st.held);
    in.attacking = false; // free: the better flank, its clock from zero
    st = partnerCloseStation(in, hold);
    REQUIRE(st.valid);
    CHECK(st.flank == -1);
    CHECK_FALSE(st.held);
    CHECK(hold.flank == -1);
    CHECK(hold.heldFor == 0.0);

    // Over the circle-strafe: no flip while attacking, none within 1.5 s of the last.
    for (const float dir : {1.f, -1.f}) {
        CAPTURE(dir);
        const CsSweep r = csSweep(1.f, 5.f, kPartnerCloseMeleeMin, kPartnerCloseMeleeMax, dir);
        CHECK(r.flipsAttacking == 0);
        CHECK(r.shortestHold + 1e-3 >= kPartnerCloseHoldSeconds);
    }
}

TEST_CASE("close station: a wall rejects the stations in it and behind it") {
    PartnerCloseInput in = csScene();
    PartnerCloseHold h0;
    const PartnerCloseStation clearRun = partnerCloseStation(in, h0);
    REQUIRE(clearRun.valid);
    // A wall through the station found in the open, square to the target-to-station line.
    const Vec2 radial = csUnit(csSub(clearRun.point, in.target));
    const Vec2 along = csRight(radial);
    const std::array<PartnerWall, 1> walls{PartnerWall{csAdd(clearRun.point, csMul(along, 1.f)), csSub(clearRun.point, csMul(along, 1.f)), 0.1f}};
    in.walls = walls;
    PartnerCloseHold h1;
    const PartnerCloseStation walled = partnerCloseStation(in, h1);
    REQUIRE(walled.valid);
    CHECK(csLen(csSub(walled.point, clearRun.point)) > 0.4f);
    // Clear of the wall by its half-thickness and the body, and the lens sees it past the wall.
    const Vec2 a = walls[0].a, b = walls[0].b, ab = csSub(b, a);
    const float t = std::fmax(0.f, std::fmin(1.f, csDot(csSub(walled.point, a), ab) / csDot(ab, ab)));
    CHECK(csLen(csSub(walled.point, csAdd(a, csMul(ab, t)))) >= 0.1f + in.bodyRadius - 1e-4f);
    CHECK(partnerCloseInCone(in.cameraPos, in.cameraForward, kCsHalf, kPartnerCloseConeMargin, walled.point));

    // A wall across the whole view just ahead of the lens: nothing legal, the hold untouched.
    const Vec2 f = csUnit(in.cameraForward);
    const Vec2 mid = csAdd(in.cameraPos, csMul(f, 1.f));
    const std::array<PartnerWall, 1> shut{PartnerWall{csAdd(mid, csMul(csRight(f), 10.f)), csSub(mid, csMul(csRight(f), 10.f)), 0.1f}};
    in.walls = shut;
    PartnerCloseHold h2{1, 45.f, 0.25};
    const PartnerCloseStation none = partnerCloseStation(in, h2);
    CHECK_FALSE(none.valid);
    CHECK(none.held); // the flank it must keep, reported, not valid
    CHECK(h2.flank == 1);
    CHECK(h2.angle == 45.f);
    CHECK(h2.heldFor == 0.25);
    PartnerCloseHold h3;
    CHECK_FALSE(partnerCloseStation(in, h3).valid);
    CHECK(h3.flank == 0);

    // Another body on the station: rejected the same way.
    in.walls = {};
    const std::array<PartnerBody, 1> body{PartnerBody{clearRun.point, 0.6f}};
    in.bodies = body;
    PartnerCloseHold h4;
    const PartnerCloseStation moved = partnerCloseStation(in, h4);
    REQUIRE(moved.valid);
    CHECK(csLen(csSub(moved.point, clearRun.point)) >= 0.6f + in.bodyRadius - 1e-4f);
}

TEST_CASE("close station: the kit's distance from the target") {
    PartnerCloseInput melee = csScene();
    PartnerCloseHold h;
    const PartnerCloseStation m = partnerCloseStation(melee, h);
    REQUIRE(m.valid);
    CHECK(m.offEdge >= kPartnerCloseMeleeMin - 1e-4f);
    CHECK(m.offEdge <= kPartnerCloseMeleeMax + 1e-4f);
    CHECK(csLen(csSub(m.point, melee.target)) - melee.targetRadius == doctest::Approx(m.offEdge).epsilon(1e-4));

    // A caster stands 5-8 m off; with the lead 11 m out it finds one in the cone.
    PartnerCloseInput caster = csScene();
    caster.lead = {-11.f, 0.f};
    csLens(caster.lead, caster.target, 0.8f, 0.2f, caster.cameraPos, caster.cameraForward);
    caster.partner = {-9.f, -3.f};
    caster.standMin = kPartnerCloseCasterMin;
    caster.standMax = kPartnerCloseCasterMax;
    PartnerCloseHold hc;
    const PartnerCloseStation c = partnerCloseStation(caster, hc);
    REQUIRE(c.valid);
    CHECK(c.offEdge >= kPartnerCloseCasterMin - 1e-4f);
    CHECK(c.offEdge <= kPartnerCloseCasterMax + 1e-4f);
    CHECK(partnerCloseInCone(caster.cameraPos, caster.cameraForward, kCsHalf, kPartnerCloseConeMargin, c.point));

    // Not whole: no station, the hold untouched.
    PartnerCloseInput bad = csScene();
    bad.standMin = 3.f;
    bad.standMax = 2.f;
    PartnerCloseHold hb{1, 50.f, 0.3};
    CHECK_FALSE(partnerCloseWhole(bad));
    CHECK_FALSE(partnerCloseStation(bad, hb).valid);
    CHECK(hb.flank == 1);
    bad = csScene();
    bad.lead.x = std::nanf("");
    CHECK_FALSE(partnerCloseWhole(bad));
}

TEST_CASE("close station: a circle-strafe keeps the partner in the close lens 85% of the time or more") {
    struct Case {
        float targetRadius, orbit, lo, hi;
    };
    for (const Case c : {Case{0.5f, 4.f, kPartnerCloseMeleeMin, kPartnerCloseMeleeMax},
                         Case{1.f, 5.f, kPartnerCloseMeleeMin, kPartnerCloseMeleeMax},
                         Case{2.5f, 6.f, kPartnerCloseMeleeMin, kPartnerCloseMeleeMax},
                         Case{1.f, 11.f, kPartnerCloseCasterMin, kPartnerCloseCasterMax}})
        for (const float dir : {1.f, -1.f}) {
            CAPTURE(c.targetRadius);
            CAPTURE(c.orbit);
            CAPTURE(dir);
            const CsSweep r = csSweep(c.targetRadius, c.orbit, c.lo, c.hi, dir);
            MESSAGE("close sweep: target r " << c.targetRadius << " orbit " << c.orbit << " band " << c.lo << "-" << c.hi
                                             << " dir " << dir << ": in cone " << r.inCone << ", clear " << r.clear
                                             << ", flips " << r.flips);
            CHECK(r.inCone >= 0.85f);
        }
}

namespace {

// The brain with the close camera: the lead 5 m from a target of radius 1, the lens as csScene.
struct CsPit {
    PartnerBrain b;
    PartnerInput in;
    std::array<PartnerEnemyView, kMaxPartnerEnemies> e{};
    explicit CsPit(bool close, std::uint32_t seed = 3) {
        PartnerTuning t;
        t.budgetShare = 1.f;
        t.budgetAllowanceSeconds = 10.f;
        REQUIRE(b.begin(t, seed));
        in.order = PartnerOrder::Spread;
        in.leadAttacking = true;
        in.leadPos = {-5.f, 0.f};
        in.leadTarget = 0;
        in.pos = {-4.f, -3.f};
        e[0].pos = {0.f, 0.f};
        e[0].radius = 1.f;
        in.station.on = true;
        in.station.leadFacing = {1.f, 0.f};
        csLens(in.leadPos, e[0].pos, 0.8f, 0.2f, in.station.cameraPos, in.station.cameraForward);
        in.station.close.given = close;
        in.station.close.halfAngleDegrees = kCsHalf;
        in.station.close.leadSide = -1;
    }
    const PartnerIntent& step(double dt = 1.0 / 60.0) {
        in.enemies = std::span<const PartnerEnemyView>(e.data(), 1);
        return b.step(dt, in);
    }
};

} // namespace

TEST_CASE("close station: not given, the brain is as before; its hold is hashed only once given") {
    // Every close field but `given` set: the brain steps exactly as one never told.
    CsPit plain(false), told(false);
    told.in.station.close.halfAngleDegrees = 40.f;
    told.in.station.close.leadSide = 1;
    told.in.station.close.standMin = 5.f;
    told.in.station.close.standMax = 8.f;
    for (int s = 0; s < 600; ++s) {
        const PartnerIntent& a = plain.step();
        const PartnerIntent& b = told.step();
        REQUIRE(a.act == b.act);
        if (a.act == PartnerAct::MoveTo) {
            plain.in.pos = csAdd(plain.in.pos, csMul(csSub(a.point, plain.in.pos), 0.1f));
            told.in.pos = csAdd(told.in.pos, csMul(csSub(b.point, told.in.pos), 0.1f));
        }
        REQUIRE(plain.b.hash() == told.b.hash());
    }
    CHECK_FALSE(plain.b.save().closeUsed);

    // The save: the hold unhashed until closeUsed, then hashed and carried by load.
    PartnerBrainSave base = plain.b.save();
    PartnerBrainSave s = base;
    s.closeHold = {1, 55.f, 0.7};
    CHECK(s.hash() == base.hash());
    s.closeUsed = true;
    CHECK(s.hash() != base.hash());
    PartnerBrainSave u = s;
    u.closeHold.angle = 60.f;
    CHECK(u.hash() != s.hash());
    u = s;
    u.closeHold.heldFor += 0.5;
    CHECK(u.hash() != s.hash());
    PartnerBrain c;
    c.load(s);
    CHECK(c.hash() == s.hash());
}

TEST_CASE("close station: given, the brain walks to a station in the close lens and strings from it") {
    CsPit p(true);
    const PartnerIntent& first = p.step();
    REQUIRE(first.act == PartnerAct::MoveTo);
    PartnerCloseInput view = csScene();
    CHECK(partnerCloseInCone(view.cameraPos, view.cameraForward, kCsHalf, kPartnerCloseConeMargin, first.point));
    const float a = csAngleAt(p.e[0].pos, p.in.leadPos, first.point);
    CHECK(a >= kPartnerCloseAngleMin - 0.01f);
    CHECK(a <= kPartnerCloseAngleMax + 0.01f);
    // In Ready the band is pulled in to reach less the station's tolerance, so a string can start there.
    const PartnerTuning tune{};
    const float off = csLen(csSub(first.point, p.e[0].pos)) - p.e[0].radius;
    CHECK(off <= tune.reachMeters - tune.stationTolerance + 1e-3f);
    CHECK(off >= 0.5f * tune.reachMeters - 1e-3f);
    CHECK(p.b.save().closeUsed);

    bool strung = false;
    for (int s = 0; s < 5 * 60 && !strung; ++s) {
        const PartnerIntent& i = s == 0 ? first : p.step();
        if (i.newString) strung = true;
        if (i.act == PartnerAct::MoveTo) {
            const Vec2 v = csSub(i.point, p.in.pos);
            const float l = csLen(v), step = 4.5f / 60.f;
            p.in.pos = l <= step ? i.point : csAdd(p.in.pos, csMul(v, step / l));
        }
    }
    CHECK(strung);
    CHECK(partnerCloseInCone(view.cameraPos, view.cameraForward, kCsHalf, 0.f, p.in.pos));
}

TEST_CASE("close station: stepping it allocates nothing") {
    const bool counting = ftcore_test::allocHookInstalled();
    CsPit p(true);
    const std::array<PartnerWall, 2> walls{PartnerWall{{-3.f, 6.f}, {3.f, 6.f}, 0.2f}, PartnerWall{{-3.f, -6.f}, {3.f, -6.f}, 0.2f}};
    p.in.station.close.walls = walls;
    const auto before = ftcore_test::allocStats().count;
    for (int s = 0; s < 600; ++s) {
        const float th = static_cast<float>(s) * 0.01f;
        p.in.leadPos = {-5.f * std::cos(th), -5.f * std::sin(th)};
        csLens(p.in.leadPos, p.e[0].pos, 0.8f, 0.2f, p.in.station.cameraPos, p.in.station.cameraForward);
        const PartnerIntent& i = p.step();
        if (i.act == PartnerAct::MoveTo) p.in.pos = csAdd(p.in.pos, csMul(csSub(i.point, p.in.pos), 0.1f));
    }
    const CsSweep r = csSweep(1.f, 5.f, kPartnerCloseMeleeMin, kPartnerCloseMeleeMax, 1.f);
    const auto allocs = ftcore_test::allocStats().count - before;
    CHECK(r.inCone > 0.f);
    if (counting) CHECK(allocs == 0u);
}

TEST_CASE("close station: through the brain, a circle-strafe keeps the partner in the close lens 85% of the time or more") {
    // The lead circles its target at 3 m/s with the close lens on it; the partner walks its intents at 4.5 m/s
    // (strings, guards and waits stand still), as the arena's proofs count it: inside the lens's own edges.
    for (const float dir : {1.f, -1.f}) {
        CAPTURE(dir);
        CsPit p(true);
        const float orbit = 5.f, w = dir * 3.f / orbit, dt = 1.f / 60.f;
        int n = 0, cone = 0, strings = 0;
        for (int s = 0; s < 30 * 60; ++s) {
            const float t = static_cast<float>(s) * dt;
            p.in.leadPos = {-orbit * std::cos(w * t), -orbit * std::sin(w * t)};
            csLens(p.in.leadPos, p.e[0].pos, 0.8f, 0.2f, p.in.station.cameraPos, p.in.station.cameraForward);
            const PartnerIntent& i = p.step(static_cast<double>(dt));
            if (i.newString) ++strings;
            if (i.act == PartnerAct::MoveTo || i.act == PartnerAct::StepBack) {
                const Vec2 v = csSub(i.point, p.in.pos);
                const float l = csLen(v), step = 4.5f * dt;
                if (l > 1e-6f) p.in.pos = l <= step ? i.point : csAdd(p.in.pos, csMul(v, step / l));
            }
            ++n;
            if (partnerCloseInCone(p.in.station.cameraPos, p.in.station.cameraForward, kCsHalf, 0.f, p.in.pos)) ++cone;
        }
        const float share = static_cast<float>(cone) / static_cast<float>(n);
        MESSAGE("close brain sweep dir " << dir << ": in cone " << share << ", strings " << strings);
        CHECK(share >= 0.85f);
        CHECK(strings >= 3);
    }
}

TEST_CASE("close station: at least 3 m ahead of the lens and within 25 degrees of its axis") {
    // The camera lane's lens: 34 degrees of half-width, 0.55 m off the lead's shoulder. Its partner turn is capped
    // well under the half-width, so the station keeps near the axis and ahead of the lead, not at the cone's edge.
    int found = 0;
    for (int a = 0; a < 360; a += 15)
        for (const float tr : {0.5f, 1.f, 2.5f})
            for (const float px : {-6.f, -2.f, 2.f}) {
                PartnerCloseInput in;
                in.target = {0.f, 0.f};
                in.targetRadius = tr;
                in.lead = csMul({std::cos(static_cast<float>(a) * kCsDeg), std::sin(static_cast<float>(a) * kCsDeg)}, tr + 4.f);
                csLens(in.lead, in.target, 0.55f, 0.2f, in.cameraPos, in.cameraForward);
                in.halfAngleDegrees = 34.f;
                in.leadSide = -1;
                in.partner = {px, 3.f};
                PartnerCloseHold hold;
                const PartnerCloseStation st = partnerCloseStation(in, hold);
                if (!st.valid) continue;
                ++found;
                const Vec2 f = csUnit(in.cameraForward);
                const Vec2 r = csSub(st.point, in.cameraPos);
                const float ahead = csDot(r, f);
                CHECK(ahead >= kPartnerCloseMinAhead - 1e-4f);
                CHECK(std::atan2(std::fabs(csDot(r, csRight(f))), ahead) / kCsDeg <= kPartnerCloseAimConeDegrees + 1e-3f);
            }
    CHECK(found >= 200); // of 216

    // Pulled in so near that nothing is 3 m ahead of the lens: no station; the floor off: one.
    PartnerCloseInput in = csScene();
    in.cameraPos = csAdd(in.lead, csMul(csUnit(in.cameraForward), 2.f));
    in.minAhead = 20.f;
    PartnerCloseHold hold;
    CHECK_FALSE(partnerCloseStation(in, hold).valid);
    in.minAhead = 0.f;
    in.aimConeDegrees = 0.f;
    in.cameraPos = csScene().cameraPos;
    CHECK(partnerCloseStation(in, hold).valid);
    in.aimConeDegrees = 86.f; // out of range: not whole
    PartnerCloseHold unheld;
    CHECK_FALSE(partnerCloseStation(in, unheld).valid);
}

TEST_CASE("close station: a pitched lens keeps the partner in the lower two thirds of the frame") {
    PartnerCloseInput in = csScene();
    const float vhalf = std::atan(std::tan(34.f * kCsDeg) / (16.f / 9.f)) / kCsDeg; // 20.8
    // Level and flat agree on x; the aim point under the lens sits below the centre.
    float x2 = 0.f, x3 = 0.f, y3 = 0.f;
    const Vec2 probe{-2.f, -2.f};
    REQUIRE(partnerCloseNdc(in.cameraPos, in.cameraForward, in.halfAngleDegrees, probe, x2));
    in.verticalHalfDegrees = vhalf;
    REQUIRE(partnerCloseLens(in, probe, x3, y3));
    CHECK(x3 == doctest::Approx(x2).epsilon(1e-4));
    CHECK(y3 < 0.f);
    // Pitched up 6 degrees: the station's aim point is in the lower two thirds, inside the frame.
    in.pitchDegrees = 6.f;
    PartnerCloseHold hold;
    const PartnerCloseStation st = partnerCloseStation(in, hold);
    REQUIRE(st.valid);
    REQUIRE(partnerCloseLens(in, st.point, x3, y3));
    CHECK(y3 >= -(1.f - 2.f * kPartnerCloseConeMargin) - 1e-4f);
    CHECK(y3 <= kPartnerCloseTopNdc + 1e-4f);
    CHECK(std::fabs(x3) <= 1.f - 2.f * kPartnerCloseConeMargin + 1e-4f);
    // Pitched up 25 degrees, past the vertical half-angle: the frame's bottom edge is above the horizon, so no ground
    // station shows the partner. A station is still given (the least bad one), below the frame: the camera must
    // keep its pitch under about 12 degrees for this rule to hold the partner (MESSAGE for the camera lane).
    in.pitchDegrees = 25.f;
    PartnerCloseHold hold2;
    const PartnerCloseStation low = partnerCloseStation(in, hold2);
    REQUIRE(low.valid);
    REQUIRE(partnerCloseLens(in, low.point, x3, y3));
    CHECK(y3 < -1.f);
    MESSAGE("pitched 25: the station's aim point at ndc y " << y3);
}

TEST_CASE("close station: a big target takes the wide flanks and keeps the partner clear of the lead 75% of the time") {
    // Radius 2 or more: 60-100 degrees round the target, so the partner stands off the lead's silhouette.
    for (const float tr : {2.f, 2.5f, 3.5f}) {
        CAPTURE(tr);
        PartnerCloseInput in = csScene();
        in.targetRadius = tr;
        PartnerCloseHold hold;
        const PartnerCloseStation st = partnerCloseStation(in, hold);
        REQUIRE(st.valid);
        CHECK(st.tier == 1);
        const float a = csAngleAt(in.target, in.lead, st.point);
        CHECK(a >= kPartnerCloseWideAngleMin - 0.01f);
        CHECK(a <= kPartnerCloseWideAngleMax + 0.01f);
    }
    PartnerCloseInput small = csScene();
    small.targetRadius = 1.9f; // under the line: the usual 40-80
    PartnerCloseHold hs;
    const PartnerCloseStation s = partnerCloseStation(small, hs);
    REQUIRE(s.valid);
    CHECK(csAngleAt(small.target, small.lead, s.point) <= kPartnerCloseAngleMax + 0.01f);

    // The circle-strafe by a big target: in the lens, and clear of the lead's silhouette and the target, 75% or more.
    struct Case {
        float targetRadius, orbit;
    };
    for (const Case c : {Case{2.5f, 6.f}, Case{3.5f, 7.f}})
        for (const float dir : {1.f, -1.f}) {
            CAPTURE(c.targetRadius);
            CAPTURE(dir);
            const CsSweep r = csSweep(c.targetRadius, c.orbit, kPartnerCloseMeleeMin, kPartnerCloseMeleeMax, dir);
            MESSAGE("wide sweep: target r " << c.targetRadius << " dir " << dir << ": in cone " << r.inCone << ", clear "
                                            << r.clear << ", flips " << r.flips);
            CHECK(r.inCone >= 0.85f);
            CHECK(r.clear >= 0.75f);
            CHECK(r.flipsAttacking == 0);
        }
}

TEST_CASE("close station: a caster with the lead next to the target casts from short of its band") {
    // The lead 1 m off the target's edge: no point of the 5-8 m band is in the cone; the farthest legal one short of it.
    PartnerCloseInput in = csScene();
    in.lead = {-2.f, 0.f};
    csLens(in.lead, in.target, 0.8f, 0.2f, in.cameraPos, in.cameraForward);
    in.partner = {-1.f, -3.f};
    in.standMin = kPartnerCloseCasterMin;
    in.standMax = kPartnerCloseCasterMax;
    PartnerCloseHold hold;
    const PartnerCloseStation st = partnerCloseStation(in, hold);
    REQUIRE(st.valid);
    CHECK(st.tier == 2);
    CHECK(st.offEdge < kPartnerCloseCasterMin);
    CHECK(st.offEdge >= in.bodyRadius - 1e-4f); // never inside the target's own body
    CHECK(csLen(csSub(st.point, in.target)) - in.targetRadius == doctest::Approx(st.offEdge).epsilon(1e-4));
    CHECK(partnerCloseInCone(in.cameraPos, in.cameraForward, kCsHalf, kPartnerCloseConeMargin, st.point));
    // The steps short of the band are standMin x (1 - j/8): the one found is one of them.
    const float j = (1.f - st.offEdge / kPartnerCloseCasterMin) * 8.f;
    CHECK(std::fabs(j - std::round(j)) < 1e-3f);
    // The lead far out: the band itself (tier 1).
    PartnerCloseInput away = in;
    away.lead = {-11.f, 0.f};
    csLens(away.lead, away.target, 0.8f, 0.2f, away.cameraPos, away.cameraForward);
    away.partner = {-9.f, -3.f};
    PartnerCloseHold hf;
    const PartnerCloseStation f = partnerCloseStation(away, hf);
    REQUIRE(f.valid);
    CHECK(f.tier == 1);
    // The cone holds none at all: not valid (the brain's own rules then).
    in.minAhead = 20.f;
    PartnerCloseHold hn;
    const PartnerCloseStation none = partnerCloseStation(in, hn);
    CHECK_FALSE(none.valid);
    CHECK(none.tier == 0);
    CHECK(hn.flank == 0);
}

TEST_CASE("close station: never beyond the leash; within it a ring round the lead") {
    // A far target (lead 11 m off its edge), a melee band: its stations are 10 m from the lead.
    PartnerCloseInput in = csScene();
    in.lead = {-12.f, 0.f};
    csLens(in.lead, in.target, 0.8f, 0.2f, in.cameraPos, in.cameraForward);
    in.partner = {-10.f, -2.f};
    PartnerCloseHold h0;
    const PartnerCloseStation loose = partnerCloseStation(in, h0);
    REQUIRE(loose.valid);
    CHECK(loose.tier == 1);
    CHECK(csLen(csSub(loose.point, in.lead)) > 3.5f);
    // Leashed at 3.5 m: nothing of the band is legal; the ring round the lead, toward the target's flank, in the lens.
    in.leashMeters = 3.5f;
    PartnerCloseHold h1;
    const PartnerCloseStation ring = partnerCloseStation(in, h1);
    REQUIRE(ring.valid);
    CHECK(ring.tier == 3);
    CHECK(h1.ring);
    CHECK(csLen(csSub(ring.point, in.lead)) <= in.leashMeters + 1e-4f);
    CHECK(csLen(csSub(ring.point, in.lead)) >= in.personalSpace - 1e-4f);
    CHECK(partnerCloseInCone(in.cameraPos, in.cameraForward, kCsHalf, kPartnerCloseConeMargin, ring.point));
    // Held on the ring, the next call keeps it (no band angle mistaken for the ring's).
    const PartnerCloseStation again = partnerCloseStation(in, h1);
    REQUIRE(again.valid);
    CHECK(again.tier == 3);
    CHECK(csLen(csSub(again.point, ring.point)) < 1e-4f);

    // Every bearing, both kits, leashed: every station within the leash.
    int found = 0;
    for (int a = 0; a < 360; a += 15)
        for (const float lo : {kPartnerCloseMeleeMin, kPartnerCloseCasterMin}) {
            PartnerCloseInput s = csScene();
            s.lead = csMul({std::cos(static_cast<float>(a) * kCsDeg), std::sin(static_cast<float>(a) * kCsDeg)}, 5.f);
            csLens(s.lead, s.target, 0.8f, 0.2f, s.cameraPos, s.cameraForward);
            s.partner = {0.f, 3.f};
            s.standMin = lo;
            s.standMax = lo == kPartnerCloseMeleeMin ? kPartnerCloseMeleeMax : kPartnerCloseCasterMax;
            s.leashMeters = 3.5f;
            PartnerCloseHold h;
            const PartnerCloseStation st = partnerCloseStation(s, h);
            if (!st.valid) continue;
            ++found;
            CHECK(csLen(csSub(st.point, s.lead)) <= s.leashMeters + 1e-4f);
        }
    CHECK(found >= 44); // of 48

    // Through the brain under StayClose: every goal within its cap (3.5 m here) of the lead.
    CsPit p(true);
    p.in.order = PartnerOrder::StayClose;
    p.in.station.close.standMin = kPartnerCloseCasterMin;
    p.in.station.close.standMax = kPartnerCloseCasterMax;
    const PartnerTuning tune{};
    int moves = 0;
    for (int s = 0; s < 600; ++s) {
        const PartnerIntent& i = p.step();
        if (i.act != PartnerAct::MoveTo) continue;
        ++moves;
        CHECK(csLen(csSub(i.point, p.in.leadPos)) <= tune.closeLeadMeters + 1e-3f);
        p.in.pos = csAdd(p.in.pos, csMul(csSub(i.point, p.in.pos), 0.1f));
    }
    CHECK(moves > 0);
}

namespace {

// A host's 3D frame test for the rule: the brain's close measure (partnerInFrameClose, middle and head) through `cam`.
bool csSees(const void* cam, Vec2 p) {
    return partnerInFrameClose(*static_cast<const PartnerCameraView*>(cam), kPartnerFrustumMargin, p);
}
// The flat close lens as a 3D camera, 1.7 m up, pitched `pitch` degrees (+ up), 16:9 with the lens's half-width.
PartnerCameraView csCamera(const PartnerCloseInput& in, float pitch) {
    const Vec2 f = csUnit(in.cameraForward);
    const float c = std::cos(pitch * kCsDeg), s = std::sin(pitch * kCsDeg);
    PartnerCameraView v;
    v.given = true;
    v.pos = {in.cameraPos.x, in.cameraPos.y, 1.7f};
    v.forward = {f.x * c, f.y * c, s};
    v.up = {-f.x * s, -f.y * s, c};
    v.aspect = 16.f / 9.f;
    v.verticalFovDegrees = 2.f * std::atan(std::tan(in.halfAngleDegrees * kCsDeg) / v.aspect) / kCsDeg;
    v.groundZ = 0.f;
    v.bodyHeight = 1.8f;
    return v;
}

} // namespace

TEST_CASE("close station: given a 3D frame test, it is the measure") {
    PartnerCloseInput in = csScene();
    PartnerCameraView cam = csCamera(in, 0.f);
    in.sees = csSees;
    in.seesContext = &cam;
    PartnerCloseHold hold;
    const PartnerCloseStation st = partnerCloseStation(in, hold);
    REQUIRE(st.valid);
    CHECK(partnerInFrameClose(cam, kPartnerFrustumMargin, st.point));
    // A test that sees nothing: no station.
    in.sees = [](const void*, Vec2) { return false; };
    PartnerCloseHold h0;
    CHECK_FALSE(partnerCloseStation(in, h0).valid);

    // The close measure leaves the feet out: a lens 1.7 m up pitched up 6 degrees, a partner 6 m ahead. Its feet are
    // below the frame (ndc y -1.23), its waist (-0.74) and head (-0.27) in it.
    PartnerCameraView up6;
    up6.given = true;
    up6.pos = {0.f, 0.f, 1.7f};
    up6.forward = {std::cos(6.f * kCsDeg), 0.f, std::sin(6.f * kCsDeg)};
    up6.up = {-std::sin(6.f * kCsDeg), 0.f, std::cos(6.f * kCsDeg)};
    up6.aspect = 16.f / 9.f;
    up6.verticalFovDegrees = 2.f * std::atan(std::tan(30.f * kCsDeg) / up6.aspect) / kCsDeg;
    up6.groundZ = 0.f;
    up6.bodyHeight = 1.8f;
    CHECK_FALSE(partnerInFrame(up6, kPartnerFrustumMargin, Vec2{6.f, 0.f}));
    CHECK(partnerInFrameClose(up6, kPartnerFrustumMargin, Vec2{6.f, 0.f}));
    CHECK_FALSE(partnerInFrameClose(up6, kPartnerFrustumMargin, Vec2{2.f, 0.f})); // its waist below the frame too

    // A sweep of bearings and target sizes, level and pitched up: every station passes the frame test (middle and
    // head; feet not tested). Level and at +6 (the camera's usual stand-off pitch) a station nearly everywhere; +12
    // (its cap) about half (MESSAGE); at +28 the frame's bottom clears even the partner's waist near the lens: none.
    in.sees = csSees;
    for (const float pitch : {0.f, 6.f, 12.f, 28.f}) {
        int found = 0, n = 0;
        for (int a = 0; a < 360; a += 15)
            for (const float tr : {1.f, 2.5f}) {
                PartnerCloseInput s = csScene();
                s.targetRadius = tr;
                s.lead = csMul({std::cos(static_cast<float>(a) * kCsDeg), std::sin(static_cast<float>(a) * kCsDeg)}, tr + 4.f);
                csLens(s.lead, s.target, 0.8f, 0.2f, s.cameraPos, s.cameraForward);
                const PartnerCameraView v = csCamera(s, pitch);
                s.sees = csSees;
                s.seesContext = &v;
                PartnerCloseHold h;
                const PartnerCloseStation r = partnerCloseStation(s, h);
                ++n;
                if (!r.valid) continue;
                ++found;
                CHECK(partnerInFrameClose(v, kPartnerFrustumMargin, r.point));
            }
        MESSAGE("3D frame, pitch " << pitch << ": a station in frame for " << found << " of " << n << " scenes");
        if (pitch == 0.f || pitch == 6.f) CHECK(found >= n * 3 / 4);
        if (pitch == 28.f) CHECK(found == 0);
    }
}

namespace {

// The brain's cycle on the rule alone: Ready (the band pulled in to reach less tolerance), a 1.2 s string, a 1.5 s
// pause in the kit band, one hold throughout; the lens 0.55 m off the shoulder at 34 degrees, the lead circling.
struct CsCycle {
    float inFrame = 0.f;
    int flips = 0, strings = 0, none = 0;
};
CsCycle csCycle(float targetRadius, float orbit, float kitLo, float kitHi, float dir) {
    constexpr float kReach = 1.8f, kTol = 0.5f, kHalf = 34.f;
    PartnerCloseInput in;
    in.target = {0.f, 0.f};
    in.targetRadius = targetRadius;
    in.halfAngleDegrees = kHalf;
    in.leadSide = -1;
    in.partner = {orbit, 2.f};
    PartnerCloseHold hold;
    CsCycle r;
    int phase = 0; // 0 Ready, 1 string, 2 pause
    double phaseFor = 0.0;
    const float dt = 1.f / 60.f, w = dir * 3.f / orbit;
    const float readyLo = std::fmin(kitLo, 0.5f * kReach), readyHi = std::fmin(kitHi, kReach - kTol);
    int n = 0, seen = 0;
    std::int8_t last = 0;
    for (int s = 0; s < 30 * 60; ++s) {
        const float t = static_cast<float>(s) * dt;
        in.lead = {orbit * std::cos(w * t), orbit * std::sin(w * t)};
        csLens(in.lead, in.target, 0.55f, 0.2f, in.cameraPos, in.cameraForward, kHalf);
        hold.heldFor += dt;
        phaseFor += dt;
        if (phase != 1) {
            in.standMin = phase == 0 ? readyLo : kitLo;
            in.standMax = phase == 0 ? readyHi : kitHi;
            const PartnerCloseStation st = partnerCloseStation(in, hold);
            if (!st.valid) {
                ++r.none;
            } else {
                const Vec2 v = csSub(st.point, in.partner);
                const float l = csLen(v), step = 4.5f * dt;
                if (l > kTol || phase == 2) in.partner = l <= step ? st.point : csAdd(in.partner, csMul(v, step / l));
                if (phase == 0 && csLen(csSub(st.point, in.partner)) <= kTol &&
                    csLen(in.partner) - targetRadius <= kReach) {
                    phase = 1;
                    phaseFor = 0.0;
                    ++r.strings;
                }
            }
            if (phase == 2 && phaseFor >= 1.5) {
                phase = 0;
                phaseFor = 0.0;
            }
        } else if (phaseFor >= 1.2) {
            phase = 2;
            phaseFor = 0.0;
        }
        if (hold.flank != last && last != 0) ++r.flips;
        last = hold.flank;
        ++n;
        if (partnerCloseInCone(in.cameraPos, in.cameraForward, kHalf, 0.f, in.partner)) ++seen;
    }
    r.inFrame = static_cast<float>(seen) / static_cast<float>(n);
    return r;
}

} // namespace

TEST_CASE("close station: Pause and Ready on opposite flanks do not ping-pong") {
    // Without the switch margin a caster's Pause station (5-8 m) and Ready station (0.9-1.3 m) can each be best on
    // opposite flanks, and the partner crosses between them every cycle. With it: no flank change in 30 s.
    struct Case {
        float targetRadius, orbit, lo, hi;
    };
    for (const Case c : {Case{1.f, 5.f, kPartnerCloseMeleeMin, kPartnerCloseMeleeMax},
                         Case{2.5f, 6.f, kPartnerCloseMeleeMin, kPartnerCloseMeleeMax},
                         Case{1.f, 11.f, kPartnerCloseCasterMin, kPartnerCloseCasterMax}})
        for (const float dir : {1.f, -1.f}) {
            CAPTURE(c.targetRadius);
            CAPTURE(c.orbit);
            CAPTURE(dir);
            const CsCycle r = csCycle(c.targetRadius, c.orbit, c.lo, c.hi, dir);
            MESSAGE("cycle: target r " << c.targetRadius << " orbit " << c.orbit << " band " << c.lo << "-" << c.hi
                                       << " dir " << dir << ": in frame " << r.inFrame << ", flips " << r.flips
                                       << ", strings " << r.strings << ", steps without a station " << r.none);
            CHECK(r.inFrame >= 0.85f);
            CHECK(r.flips <= 1);
            CHECK(r.strings >= 5);
        }
}

TEST_CASE("close station: never inside the target's body, whatever the tier") {
    // The review's case: the lead 1.5 m off the edge of a radius-2 target, leashed at 3.5 m. The lead ring (tier 3)
    // is placed round the lead, so its points can reach into the target: none may stand there.
    {
        PartnerCloseInput in = csScene();
        in.targetRadius = 2.f;
        in.lead = {-3.5f, 0.f};
        csLens(in.lead, in.target, 0.8f, 0.2f, in.cameraPos, in.cameraForward);
        in.partner = {-2.f, 2.f};
        in.leashMeters = 3.5f;
        PartnerCloseHold h;
        const PartnerCloseStation st = partnerCloseStation(in, h);
        if (st.valid) CHECK(csLen(csSub(st.point, in.target)) >= in.targetRadius + in.bodyRadius - 1e-4f);
    }
    // A sweep: every bearing, big targets, the lead 1-2 m off their edge, both kits, leashed at 3.5 m, so the ring's
    // angles and radii are all tried. Without the body test the model put 190 of 432 stations inside the target.
    int found = 0, ring = 0;
    for (int a = 0; a < 360; a += 15)
        for (const float tr : {2.f, 2.5f, 3.5f})
            for (const float off : {1.f, 1.5f, 2.f})
                for (const float lo : {kPartnerCloseMeleeMin, kPartnerCloseCasterMin}) {
                    CAPTURE(a);
                    CAPTURE(tr);
                    CAPTURE(off);
                    CAPTURE(lo);
                    PartnerCloseInput s = csScene();
                    s.targetRadius = tr;
                    s.lead = csMul({std::cos(static_cast<float>(a) * kCsDeg), std::sin(static_cast<float>(a) * kCsDeg)}, tr + off);
                    csLens(s.lead, s.target, 0.8f, 0.2f, s.cameraPos, s.cameraForward);
                    s.partner = csAdd(csMul(s.lead, 0.5f), Vec2{0.f, 2.f});
                    s.standMin = lo;
                    s.standMax = lo == kPartnerCloseMeleeMin ? kPartnerCloseMeleeMax : kPartnerCloseCasterMax;
                    s.leashMeters = 3.5f;
                    PartnerCloseHold h;
                    const PartnerCloseStation st = partnerCloseStation(s, h);
                    if (!st.valid) continue;
                    ++found;
                    if (st.tier == 3) ++ring;
                    CHECK(csLen(csSub(st.point, s.target)) >= tr + s.bodyRadius - 1e-4f);
                    CHECK(csLen(csSub(st.point, s.lead)) <= s.leashMeters + 1e-4f);
                }
    MESSAGE("never inside: " << found << " stations of 648, " << ring << " on the lead ring");
    CHECK(found >= 300); // the model: 384
    CHECK(ring >= 100);  // the ring is exercised (the model: 240)
}

TEST_CASE("close station: a new target starts a new flank hold") {
    // Two targets at one place: the same geometry, so the same flank. Held on the first for a while, the switch
    // to the second clears the hold (its clock starts again), as on a lead switch or a fall.
    CsPit p(true);
    p.e[1] = p.e[0];
    p.in.leadTarget = 0;
    const auto two = [&p]() {
        p.in.enemies = std::span<const PartnerEnemyView>(p.e.data(), 2);
        return p.b.step(1.0 / 60.0, p.in);
    };
    for (int s = 0; s < 180; ++s) {
        const PartnerIntent& i = two();
        if (i.act == PartnerAct::MoveTo) p.in.pos = csAdd(p.in.pos, csMul(csSub(i.point, p.in.pos), 0.1f));
    }
    REQUIRE(p.b.save().target == 0);
    REQUIRE(p.b.save().closeHold.flank != 0);
    REQUIRE(p.b.save().closeHold.heldFor > 0.5);
    const std::int8_t was = p.b.save().closeHold.flank;
    p.e[0].state = PartnerEnemyView::State::Down;
    p.in.leadTarget = 1;
    for (int s = 0; s < 120 && p.b.save().target != 1; ++s) two(); // a string running ends first
    REQUIRE(p.b.save().target == 1);
    MESSAGE("flank on the first target " << static_cast<int>(was) << ", on the second " << static_cast<int>(p.b.save().closeHold.flank));
    CHECK(p.b.save().closeHold.heldFor < 1e-9);
}

namespace {
// 30 s of a close-camera partner on one target with a kit range: its strings, from how far off the edge each starts,
// its farthest Pause or Approach goal, and its StepBacks (bad: one not out to the standoff from inside it).
struct CsRangeRun {
    int strings = 0;
    float minStart = 1e9f, sumStart = 0.f;
    float maxGoal = 0.f;
    int stepBacks = 0, badStepBacks = 0;
    float meanStart() const { return strings > 0 ? sumStart / static_cast<float>(strings) : 0.f; }
};
CsRangeRun csRangeRun(PartnerRange range, float lo, float hi, Vec2 lead, Vec2 start) {
    CsPit p(true);
    p.in.range = range;
    p.in.station.close.standMin = lo;
    p.in.station.close.standMax = hi;
    p.in.leadPos = lead;
    csLens(lead, p.e[0].pos, 0.8f, 0.2f, p.in.station.cameraPos, p.in.station.cameraForward);
    p.in.pos = start;
    const float r = p.e[0].radius;
    const float standoff = partnerEffectiveTuning(PartnerTuning{}, p.in).standoffMeters;
    CsRangeRun k;
    for (int s = 0; s < 30 * 60; ++s) {
        const PartnerIntent& i = p.step();
        const float at = csLen(csSub(p.in.pos, p.e[0].pos)) - r;
        if (i.act == PartnerAct::Attack && i.newString) {
            ++k.strings;
            k.minStart = std::min(k.minStart, at);
            k.sumStart += at;
        }
        if (i.act == PartnerAct::MoveTo || i.act == PartnerAct::StepBack) {
            const float goal = csLen(csSub(i.point, p.e[0].pos)) - r;
            if (i.why == PartnerWhy::Pause || i.why == PartnerWhy::Approach) k.maxGoal = std::max(k.maxGoal, goal);
            if (i.act == PartnerAct::StepBack) {
                ++k.stepBacks;
                if (!(at < standoff && goal + 1e-3f >= standoff)) ++k.badStepBacks;
            }
            p.in.pos = csAdd(p.in.pos, csMul(csSub(i.point, p.in.pos), 0.1f));
        }
    }
    return k;
}
} // namespace

TEST_CASE("close station: the kit's range: unset, invalid, or under the classic camera, the brain as without it") {
    const PartnerTuning tu;
    PartnerInput in;
    in.range = {3.2f, 1.8f};
    CHECK_FALSE(partnerRangeOn(in, tu)); // the classic camera: never read
    CHECK(partnerEffectiveTuning(tu, in).reachMeters == tu.reachMeters);
    in.station.close.given = true;
    REQUIRE(partnerRangeOn(in, tu));
    const PartnerTuning e = partnerEffectiveTuning(tu, in);
    CHECK(e.reachMeters == 3.2f);
    CHECK(e.standoffMeters == 1.8f);
    CHECK(e.leashMeters == doctest::Approx(3.2f + kPartnerRangeLeadMelee)); // the leash holds the band (4.7 m)
    CHECK(e.stationTolerance == tu.stationTolerance);
    const float notNumber = std::nanf("");
    const float endless = HUGE_VALF;
    for (const PartnerRange bad : {PartnerRange{}, PartnerRange{notNumber, 1.f}, PartnerRange{3.f, notNumber}, PartnerRange{3.f, 3.5f},
                                   PartnerRange{0.3f, 0.f}, PartnerRange{0.9f, 0.f}, PartnerRange{3.f, -0.1f},
                                   PartnerRange{kPartnerRangeMax + 0.1f, 1.f}, PartnerRange{endless, 1.f}}) {
        CAPTURE(bad.reach);
        CAPTURE(bad.standoff);
        in.range = bad;
        CHECK_FALSE(partnerRangeOn(in, tu));
        CHECK(partnerEffectiveTuning(tu, in).reachMeters == tu.reachMeters);
        CHECK(partnerEffectiveTuning(tu, in).standoffMeters == tu.standoffMeters);
    }
    in.range = {kPartnerRangeMax, kPartnerCloseCasterMin}; // the far end: a kit that casts from the caster band
    CHECK(partnerRangeOn(in, tu));
    in.range = {1.f, 1.f}; // reach twice the tolerance, standoff equal to it: the near end
    CHECK(partnerRangeOn(in, tu));

    // Stepped: an invalid range under the close camera, and a valid one under the classic camera, step exactly as
    // none (act, point and hash, every step).
    struct Pair {
        bool close;
        PartnerRange range;
    };
    for (const Pair c : {Pair{true, PartnerRange{3.f, 3.5f}}, Pair{true, PartnerRange{notNumber, 1.f}},
                         Pair{false, PartnerRange{3.2f, 1.8f}}, Pair{false, PartnerRange{8.5f, 5.f}}}) {
        CAPTURE(c.close);
        CAPTURE(c.range.reach);
        CsPit plain(c.close), told(c.close);
        told.in.range = c.range;
        int strings = 0;
        for (int s = 0; s < 900; ++s) {
            const PartnerIntent& a = plain.step();
            const PartnerIntent& b = told.step();
            REQUIRE(a.act == b.act);
            REQUIRE(a.point.x == b.point.x);
            REQUIRE(a.point.y == b.point.y);
            if (a.act == PartnerAct::Attack && a.newString) ++strings;
            if (a.act == PartnerAct::MoveTo || a.act == PartnerAct::StepBack) {
                plain.in.pos = csAdd(plain.in.pos, csMul(csSub(a.point, plain.in.pos), 0.1f));
                told.in.pos = csAdd(told.in.pos, csMul(csSub(b.point, told.in.pos), 0.1f));
            }
            REQUIRE(plain.b.hash() == told.b.hash());
        }
        MESSAGE("range " << c.range.reach << "/" << c.range.standoff << (c.close ? ", close" : ", classic") << ": "
                         << strings << " strings in 15 s, alike");
        if (c.close) CHECK(strings >= 1); // the close runs fight (the classic one: the arena test's hits)
    }
}

TEST_CASE("close station: a caster with its kit's range strings from its band, not walked in") {
    // The shipped party kit's range (3.2, 1.8) under a caster band of 5-8 m: they do not overlap, so its band is
    // [1.8, 2.7], Pause and Ready alike, and its strings start once within reach (3.2) on its walk there. Without the
    // range Ready pulls in to 0.9-1.3 m. Checked: its strings start at least 1.3 m off the edge on average, and 0.3 m
    // further off than without the range; no goal of its is past its reach.
    const Vec2 lead{-5.f, 0.f}, start{-4.f, -3.f};
    const CsRangeRun ranged = csRangeRun({3.2f, 1.8f}, kPartnerCloseCasterMin, kPartnerCloseCasterMax, lead, start);
    const CsRangeRun plain = csRangeRun({}, kPartnerCloseCasterMin, kPartnerCloseCasterMax, lead, start);
    MESSAGE("kit range 3.2/1.8: strings " << ranged.strings << ", start off the edge mean " << ranged.meanStart()
                                          << " min " << ranged.minStart << ", farthest goal " << ranged.maxGoal
                                          << "; none: strings " << plain.strings << ", mean " << plain.meanStart());
    REQUIRE(ranged.strings >= 3);
    REQUIRE(plain.strings >= 3);
    CHECK(ranged.meanStart() > plain.meanStart() + 0.3f);
    CHECK(ranged.meanStart() >= 1.3f);
    CHECK(ranged.maxGoal <= 3.2f + 1e-3f); // never a station out of its reach
    CHECK(ranged.badStepBacks == 0);

    // A kit that would cast from the caster band itself (8.5, 5; arena.json caps partnerReach at 4 today): its band
    // is the host's, 5-8 m, and its strings start there.
    const CsRangeRun farKit = csRangeRun({kPartnerRangeMax, kPartnerCloseCasterMin}, kPartnerCloseCasterMin,
                                         kPartnerCloseCasterMax, Vec2{-12.f, 0.f}, Vec2{-10.f, -3.f});
    MESSAGE("kit range 8.5/5: strings " << farKit.strings << ", start off the edge mean " << farKit.meanStart()
                                        << " min " << farKit.minStart << ", farthest goal " << farKit.maxGoal);
    REQUIRE(farKit.strings >= 3);
    CHECK(farKit.meanStart() >= kPartnerCloseCasterMin - 0.5f);
    CHECK(farKit.maxGoal <= kPartnerRangeMax + 1e-3f);
    CHECK(farKit.badStepBacks == 0);
}

namespace {
// 30 s of a close-camera caster with an 8.5 / 5 kit range by a lead in melee (`leadGap` off the target's edge, on
// the lens's side): the target still, or walking a 2 m circle at 0.8 m/s with the lead and the lens following. Each
// string's start off the target's edge; the farthest the partner stood from the lead once its first string began.
struct CsBandRun {
    int strings = 0, inside = 0; // inside: started more than kPartnerRangeStepOut inside the standoff
    int unanswered = 0;           // a string begun with the last inside start not stepped out from
    float minStart = 1e9f, sumStart = 0.f, maxFromLead = 0.f;
    float meanStart() const { return strings > 0 ? sumStart / static_cast<float>(strings) : 0.f; }
};
CsBandRun csBandRun(PartnerOrder order, bool moving, float leadGap, Vec2 start) {
    CsPit p(true);
    p.in.order = order;
    p.in.range = {8.5f, 5.f};
    p.in.station.close.standMin = kPartnerCloseCasterMin;
    p.in.station.close.standMax = kPartnerCloseCasterMax;
    p.in.pos = start;
    const float r = p.e[0].radius;
    CsBandRun k;
    bool owed = false; // an inside start, no StepBack since
    for (int s = 0; s < 30 * 60; ++s) {
        if (moving) {
            const float a = 0.4f * static_cast<float>(s) / 60.f;
            p.e[0].pos = {2.f * std::cos(a) - 2.f, 2.f * std::sin(a)};
        }
        p.in.leadPos = csAdd(p.e[0].pos, Vec2{-(r + leadGap), 0.f});
        csLens(p.in.leadPos, p.e[0].pos, 0.8f, 0.2f, p.in.station.cameraPos, p.in.station.cameraForward);
        const PartnerIntent& i = p.step();
        const float at = csLen(csSub(p.in.pos, p.e[0].pos)) - r;
        if (k.strings > 0) k.maxFromLead = std::max(k.maxFromLead, csLen(csSub(p.in.pos, p.in.leadPos))); // after the walk in
        if (i.act == PartnerAct::Attack && i.newString) {
            ++k.strings;
            k.minStart = std::min(k.minStart, at);
            k.sumStart += at;
            if (owed) ++k.unanswered;
            owed = at < 5.f - kPartnerRangeStepOut - 1e-3f;
            if (owed) ++k.inside;
        }
        if (i.act == PartnerAct::StepBack) owed = false;
        if (i.act == PartnerAct::MoveTo || i.act == PartnerAct::StepBack)
            p.in.pos = csAdd(p.in.pos, csMul(csSub(i.point, p.in.pos), 0.1f));
    }
    return k;
}
} // namespace

TEST_CASE("close station: an 8.5 / 5 caster casts from inside its standoff and steps out between strings") {
    // Ruling (orchestrator, 11-16, second): under the close camera with the kit's range on, the standoff is a goal, not
    // a gate: a string starts anywhere within the reach; after one begun more than kPartnerRangeStepOut inside the
    // standoff, the pause steps the partner out before the next. The walk in from out of reach aims for the band's
    // middle; the leash holds the band. Starts from inside the standoff (1 m off), from the band, and from out of
    // reach; the lead in melee; Spread and StayClose; a still target and a moving one.
    int all = 0;
    for (const PartnerOrder order : {PartnerOrder::Spread, PartnerOrder::StayClose})
        for (const bool moving : {false, true})
            for (const Vec2 start : {Vec2{-1.5f, -1.5f}, Vec2{-4.f, -5.f}, Vec2{-9.f, -9.f}}) {
                CAPTURE(static_cast<int>(order));
                CAPTURE(moving);
                CAPTURE(start.x);
                const CsBandRun k = csBandRun(order, moving, 0.3f, start);
                MESSAGE(std::string(order == PartnerOrder::Spread ? "Spread" : "StayClose") << (moving ? ", moving" : ", still")
                        << ", from " << start.x << "," << start.y << ": strings " << k.strings << ", start off the edge mean "
                        << k.meanStart() << " min " << k.minStart << ", inside the standoff " << k.inside
                        << " (not stepped out from " << k.unanswered << "); farthest from the lead " << k.maxFromLead);
                CHECK(k.strings >= 2);
                CHECK(k.unanswered == 0);
                CHECK(k.minStart <= 8.5f + 1e-3f);
                if (order == PartnerOrder::StayClose) CHECK(k.maxFromLead <= 10.f + 0.5f); // the leash, 10 m at most
                all += k.strings;
            }
    MESSAGE("12 runs: " << all << " strings");
}

TEST_CASE("close station: a step out to the kit's band is not carried to the next target") {
    // An 8.5 / 5 caster 1 m off its target steps out after a string; mid step its target falls and the lead turns to
    // another 7 m off. The step out is dropped: no string on the new target from where the walk had got to, that step.
    CsPit p(true);
    p.in.range = {8.5f, 5.f};
    p.in.station.close.standMin = kPartnerCloseCasterMin;
    p.in.station.close.standMax = kPartnerCloseCasterMax;
    p.in.leadPos = {-1.3f, 0.f};
    p.in.pos = {-1.4f, -1.4f};
    p.e[1].pos = {-3.f, -8.f};
    p.e[1].radius = 1.f;
    csLens(p.in.leadPos, p.e[0].pos, 0.8f, 0.2f, p.in.station.cameraPos, p.in.station.cameraForward);
    const auto step2 = [&]() -> const PartnerIntent& {
        p.in.enemies = std::span<const PartnerEnemyView>(p.e.data(), 2);
        return p.b.step(1.0 / 60.0, p.in);
    };
    bool out = false;
    for (int s = 0; s < 600 && !out; ++s) {
        const PartnerIntent& i = step2();
        out = i.act == PartnerAct::StepBack;
        if (i.act == PartnerAct::MoveTo || i.act == PartnerAct::StepBack)
            p.in.pos = csAdd(p.in.pos, csMul(csSub(i.point, p.in.pos), 0.02f));
    }
    REQUIRE(out);
    CHECK(p.b.save().steppedOut);
    p.e[0].state = PartnerEnemyView::State::Down;
    p.in.leadTarget = 1;
    const PartnerIntent& i = step2();
    CHECK_FALSE(i.act == PartnerAct::Attack);
    CHECK_FALSE(p.b.save().steppedOut);
}

TEST_CASE("close station: a target not tall rushed inside the standoff: over the size cap a string starts only with no band station") {
    // The near field (the order of 11-16): an 8.5 / 5 caster stands over rule 2's size cap (0.85 of the frame's
    // height) but outside rule 1, its target not tall. More than kPartnerRangeStepOut inside the standoff with no
    // station in the band (a 4 degree cone: tier 3-5) it strings from where it stands; with one (tier 1-2) it walks
    // there; outside the standoff it walks too.
    struct Case {
        float half, depth, lateral;
        bool casts;
    };
    for (const Case c : {Case{4.f, 2.9f, 2.f, true}, Case{kCsHalf, 2.9f, 2.f, false}, Case{4.f, 2.55f, -3.f, false}}) {
        CAPTURE(c.half);
        CAPTURE(c.depth);
        CAPTURE(c.lateral);
        CsPit p(true);
        p.in.range = {8.5f, 5.f};
        p.in.station.close.standMin = kPartnerCloseCasterMin;
        p.in.station.close.standMax = kPartnerCloseCasterMax;
        p.in.station.close.halfAngleDegrees = c.half;
        p.in.station.close.verticalHalfDegrees = 20.f;
        const Vec2 cam = p.in.station.cameraPos, f = csUnit(p.in.station.cameraForward);
        p.in.pos = csAdd(csAdd(cam, csMul(f, c.depth)), csMul(Vec2{-f.y, f.x}, c.lateral));
        REQUIRE(partnerCloseNearOk(cam, f, p.in.leadPos, p.in.pos));
        REQUIRE(partnerCloseProjectedHeight(cam, f, 20.f, p.in.pos) > kPartnerCloseSizeCap);
        const float edge = std::hypot(p.in.pos.x, p.in.pos.y) - p.e[0].radius;
        const PartnerIntent& i = p.step();
        if (c.casts) {
            CHECK(edge < 5.f - kPartnerRangeStepOut);
            CHECK(i.closeTier >= 3);
            CHECK(i.why == PartnerWhy::Start);
            CHECK(i.act == PartnerAct::Attack);
            CHECK(i.blowStart);
        } else {
            if (c.half == kCsHalf) CHECK(i.closeTier <= 2);
            else CHECK(edge >= 5.f - kPartnerRangeStepOut);
            CHECK(i.why == PartnerWhy::Approach);
            CHECK(i.act == PartnerAct::MoveTo);
            CHECK_FALSE(i.blowStart);
        }
    }
}

TEST_CASE("close station: the near field stretches the band to the kit's reach, past the host's band") {
    // The near field's rule 3 (the order of 11-16): with the kit's range on under the close lens, the band reaches
    // reach - tolerance whatever the host's own band: a host band of 5-5.3 m gives the station a band of 5-8.5 m gives,
    // there past 5.3 m off the target's edge (without the stretch, 5.3 m at most: a tier 4 and a tier 5 case).
    struct Case {
        float lead, verticalHalf, half;
        Vec2 start;
        int tier;
    };
    for (const Case c : {Case{5.f, 8.f, 30.f, Vec2{2.f, -8.f}, 4}, Case{6.f, 22.f, 12.f, Vec2{-4.f, -6.f}, 5}}) {
        CAPTURE(c.lead);
        CAPTURE(c.half);
        Vec2 goal[2];
        for (int k = 0; k < 2; ++k) {
            CsPit p(true);
            p.in.range = {8.5f, 5.f};
            p.in.leadPos = {-c.lead, 0.f};
            p.in.station.close.standMin = 5.f;
            p.in.station.close.standMax = k == 0 ? 5.3f : 8.5f;
            p.in.station.close.halfAngleDegrees = c.half;
            p.in.station.close.verticalHalfDegrees = c.verticalHalf;
            csLens(p.in.leadPos, p.e[0].pos, 0.8f, 0.2f, p.in.station.cameraPos, p.in.station.cameraForward, c.half);
            p.in.pos = c.start;
            const PartnerIntent& i = p.step();
            CHECK(i.act == PartnerAct::MoveTo);
            CHECK(static_cast<int>(i.closeTier) == c.tier);
            goal[k] = i.point;
        }
        CHECK(std::hypot(goal[0].x - goal[1].x, goal[0].y - goal[1].y) <= 1e-4f);
        CHECK(std::hypot(goal[0].x, goal[0].y) - 1.f > 5.3f + 0.5f);
    }
}

TEST_CASE("close station: the kit's range sizes the leash to its band, a melee kit's stays") {
    const PartnerTuning tu;
    PartnerInput in;
    in.station.close.given = true;
    in.range = {8.5f, 5.f};
    const PartnerTuning caster = partnerEffectiveTuning(tu, in);
    CHECK(partnerRangeOn(in, tu));
    CHECK(caster.leashMeters == doctest::Approx(10.f));
    CHECK(caster.returnMeters == doctest::Approx(10.f - (tu.leashMeters - tu.returnMeters)));
    in.range = {1.8f, 0.8f}; // a melee kit's
    const PartnerTuning melee = partnerEffectiveTuning(tu, in);
    CHECK(melee.leashMeters == tu.leashMeters);
    CHECK(melee.returnMeters == tu.returnMeters);
    in.station.close.given = false; // the classic camera: none of it
    const PartnerTuning classic = partnerEffectiveTuning(tu, in);
    CHECK_FALSE(partnerRangeOn(in, tu));
    CHECK(classic.leashMeters == tu.leashMeters);
}

TEST_CASE("close station: with the kit's range a StepBack only steps out to the standoff") {
    // The lead next to the target (its band has no point in the cone: short of it, maybe inside the standoff), the
    // partner starting inside the standoff: every StepBack is from inside it to a goal at least the standoff off.
    int stepBacks = 0, strings = 0;
    for (const float leadOff : {1.f, 1.5f, 2.5f})
        for (const Vec2 start : {Vec2{-1.2f, -0.9f}, Vec2{-0.5f, 1.3f}, Vec2{-2.f, -2.f}}) {
            CAPTURE(leadOff);
            CAPTURE(start.x);
            const CsRangeRun k = csRangeRun({3.2f, 1.8f}, kPartnerCloseCasterMin, kPartnerCloseCasterMax,
                                            Vec2{-1.f - leadOff, 0.f}, start);
            CHECK(k.badStepBacks == 0);
            CHECK(k.maxGoal <= 3.2f + 1e-3f);
            stepBacks += k.stepBacks;
            strings += k.strings;
        }
    MESSAGE("lead next to the target, 9 runs: " << stepBacks << " StepBacks (all out to the standoff), " << strings
                                                << " strings");
    CHECK(strings >= 9);
}

TEST_CASE("close station: with the kit's range, Pause and Ready do not flap at the band's edges") {
    // The partner starts at an edge of its band (standoff 1.8, reach - tolerance 2.7, reach 3.2 off the target's
    // edge) or just either side of one, and walks to its intents for 20 s. A flap: its (act, why) off a blow turns
    // to another and back within 10 steps; or its goal off a blow jumps more than 0.5 m and back again within 10
    // steps (Pause and Ready share the band: no goal between two stations).
    const PartnerRange range{3.2f, 1.8f};
    const Vec2 dir{-0.8f, -0.6f}; // the start's side of the target: in the lens, the lead's flank
    int flaps = 0, jumps = 0, strings = 0;
    for (const float at : {1.7f, 1.8f, 1.9f, 2.6f, 2.7f, 2.8f, 3.1f, 3.2f, 3.3f}) {
        CAPTURE(at);
        CsPit p(true);
        p.in.range = range;
        p.in.station.close.standMin = kPartnerCloseCasterMin;
        p.in.station.close.standMax = kPartnerCloseCasterMax;
        const float r = p.e[0].radius;
        p.in.pos = csAdd(p.e[0].pos, csMul(dir, r + at));
        struct Was {
            PartnerAct act;
            PartnerWhy why;
            int step;
        };
        Was prev{PartnerAct::Wait, PartnerWhy::NotBegun, -100}, before{PartnerAct::Wait, PartnerWhy::NotBegun, -100};
        bool haveGoal = false;
        Vec2 lastGoal{}, jumpFrom{};
        int jumpStep = -100;
        int runFlaps = 0, runJumps = 0, runStrings = 0;
        for (int s = 0; s < 20 * 60; ++s) {
            const PartnerIntent& i = p.step();
            if (i.act == PartnerAct::Attack) { // a blow: the next run off it starts afresh
                if (i.newString) ++runStrings;
                prev = before = Was{PartnerAct::Wait, PartnerWhy::NotBegun, -100};
                haveGoal = false;
                continue;
            }
            if (i.act != prev.act || i.why != prev.why) {
                if (prev.step >= 0 && i.act == before.act && i.why == before.why && s - before.step <= 10) ++runFlaps;
                before = prev;
                prev = Was{i.act, i.why, s};
            }
            if (i.act == PartnerAct::MoveTo || i.act == PartnerAct::StepBack) {
                if (haveGoal && csLen(csSub(i.point, lastGoal)) > 0.5f) {
                    if (s - jumpStep <= 10 && csLen(csSub(i.point, jumpFrom)) <= 0.5f) ++runJumps; // and back
                    jumpFrom = lastGoal;
                    jumpStep = s;
                }
                lastGoal = i.point;
                haveGoal = true;
                p.in.pos = csAdd(p.in.pos, csMul(csSub(i.point, p.in.pos), 0.1f));
            } else {
                haveGoal = false;
            }
        }
        MESSAGE("start " << at << " m off the edge: flaps " << runFlaps << ", goal jumps " << runJumps << ", strings "
                         << runStrings);
        CHECK(runFlaps == 0);
        CHECK(runJumps == 0);
        CHECK(runStrings >= 3);
        flaps += runFlaps;
        jumps += runJumps;
        strings += runStrings;
    }
    MESSAGE("band edges, 9 runs: " << flaps << " flaps, " << jumps << " goal jumps, " << strings << " strings");
}

TEST_CASE("close station: a flank handed over while attacking never walks the partner before its blow lands") {
    // Review (11-16): the far arc may hand a kept flank over while a string runs (tier 4, the kept flank's far arc
    // empty). The station moves; the partner must not: no walk or step back while a blow it began is running. The
    // lead hugs a target walking a 2 m circle, the lens on the lead; Spread (StayClose's leash may end a string at
    // once by its own rule 1).
    int strings = 0, early = 0, farWhileAttacking = 0, walks = 0;
    for (const std::uint32_t seed : {1u, 2u, 3u, 4u}) {
        CsPit p(true, seed);
        p.in.blowSeconds.fill(0.6f);
        p.in.pos = {-3.f, -3.f};
        const float r = p.e[0].radius;
        double blowEnd = -1.0;
        for (int s = 0; s < 30 * 60; ++s) {
            const float a = 0.4f * static_cast<float>(s) / 60.f;
            p.e[0].pos = {2.f * std::cos(a) - 2.f, 2.f * std::sin(a)};
            p.in.leadPos = csAdd(p.e[0].pos, csMul(csUnit(csSub(p.in.leadPos, p.e[0].pos)), r + 0.45f));
            csLens(p.in.leadPos, p.e[0].pos, 0.8f, 0.2f, p.in.station.cameraPos, p.in.station.cameraForward);
            const PartnerIntent& i = p.step();
            const double now = static_cast<double>(s + 1) / 60.0;
            if (i.blowStart) blowEnd = now + 0.6;
            if (i.act == PartnerAct::Attack && i.newString) ++strings;
            if (i.act == PartnerAct::Attack && i.closeTier == 4) ++farWhileAttacking;
            if (i.act == PartnerAct::MoveTo || i.act == PartnerAct::StepBack) {
                ++walks;
                if (now + 1e-6 < blowEnd) ++early;
                p.in.pos = csAdd(p.in.pos, csMul(csSub(i.point, p.in.pos), 0.1f));
            }
        }
    }
    MESSAGE("4 seeds: " << strings << " strings, " << walks << " walking steps, " << early << " before a blow landed; "
                        << farWhileAttacking << " attacking steps on the far arc");
    CHECK(strings >= 8);
    CHECK(walks > 0);
    CHECK(early == 0);
}

TEST_CASE("close station: a string the lead hides ends no faster than a pause and one blow") {
    // Review (11-16): the cut has no cooldown of its own. A partner the host never moves, standing where the lead
    // hides it from the lens and in reach of the target (no station it reaches, so the fallback starts its strings
    // from there): every string is cut after its first blow, and the next starts no sooner than the shortest pause
    // after that blow (the pause is the cooldown; a still target, never staggered).
    CsPit p(true);
    p.in.blowSeconds.fill(0.6f);
    const PartnerTuning t;
    const Vec2 cam = p.in.station.cameraPos, lead = p.in.leadPos;
    Vec2 spot{};
    bool found = false;
    for (int k = 0; k < 400 && !found; ++k) { // along the lens's line over the lead, the first point in reach
        spot = csAdd(cam, csMul(csSub(lead, cam), 1.05f + 0.005f * static_cast<float>(k)));
        const float off = csLen(csSub(spot, p.e[0].pos)) - p.e[0].radius;
        found = off <= t.reachMeters - 0.3f && off >= 0.5f;
    }
    REQUIRE(found);
    REQUIRE(partnerCloseHiddenBehind(cam, spot, lead, p.in.station.close.bodyRadius));
    REQUIRE(csLen(csSub(spot, lead)) > t.personalSpaceMeters);
    p.in.pos = spot;
    int strings = 0, blows = 0;
    double lastStart = -1.0, shortest = 1e9;
    for (int s = 0; s < 30 * 60; ++s) {
        const PartnerIntent& i = p.step();
        const double now = static_cast<double>(s + 1) / 60.0;
        if (i.blowStart) ++blows;
        if (i.act == PartnerAct::Attack && i.newString) {
            if (lastStart >= 0.0) shortest = std::min(shortest, now - lastStart);
            lastStart = now;
            ++strings;
        }
    }
    MESSAGE("hidden and still: " << strings << " strings, " << blows << " blows in 30 s; shortest start to start "
                                 << shortest << " s (a blow 0.6 s, the shortest pause " << t.pauseMin << " s)");
    REQUIRE(strings >= 3);
    CHECK(blows == strings); // each cut after its first blow
    CHECK(shortest + 1e-6 >= 0.6 + static_cast<double>(t.pauseMin));
}

TEST_CASE("close station: a classic run's whole hash, pinned") {
    // Review (11-16): the close rules leave the classic brain bit-identical; this pins one whole classic run (the
    // close station off, the frame on, the target walking, the partner walked to each goal) at cb7d028a's value.
    CsPit p(false);
    p.in.blowSeconds.fill(0.6f);
    for (int s = 0; s < 20 * 60; ++s) {
        const float a = 0.3f * static_cast<float>(s) / 60.f;
        p.e[0].pos = {1.5f * std::cos(a) - 1.5f, 1.5f * std::sin(a)};
        p.in.leadPos = csAdd(p.e[0].pos, Vec2{-2.2f, 0.f});
        csLens(p.in.leadPos, p.e[0].pos, 0.8f, 0.2f, p.in.station.cameraPos, p.in.station.cameraForward);
        const PartnerIntent& i = p.step();
        if (i.act == PartnerAct::MoveTo || i.act == PartnerAct::StepBack)
            p.in.pos = csAdd(p.in.pos, csMul(csSub(i.point, p.in.pos), 0.1f));
    }
    const std::uint64_t h = p.b.hash();
    MESSAGE("classic whole-run hash " << h);
    CHECK(h == 773648685674569465ull);
}
