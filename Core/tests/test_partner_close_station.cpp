// Rule 19: the partner's station under the close arena camera (PartnerCloseStation.h), alone and in the brain.
// The lens in these scenes stands as the close preset puts it: 2.6 m behind the lead, 0.8 m over a shoulder, aimed so
// the target sits 0.2 of the half-width off centre, 30 degrees each side (a 60 degree horizontal field).
#include "ftcore/PartnerBrain.h"
#include "ftcore/PartnerCloseStation.h"
#include "AllocCounter.h"
#include <doctest/doctest.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

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
// centre), the forward turned so the target sits at `aimNdc`.
void csLens(Vec2 lead, Vec2 target, float shoulder, float aimNdc, Vec2& cam, Vec2& fwd) {
    const Vec2 d = csUnit(csSub(target, lead));
    cam = csAdd(csAdd(lead, csMul(d, -2.6f)), csMul(csRight(d), shoulder));
    const Vec2 aim = csUnit(csSub(target, cam));
    fwd = csTurn(aim, -std::atan(aimNdc * std::tan(kCsHalf * kCsDeg)) / kCsDeg);
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
