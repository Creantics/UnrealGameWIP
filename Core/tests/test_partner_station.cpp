// PartnerBrain stationing (rules 8-12): personal space, the camera wedge, the stations round the target, the side's
// hold, the no-target follow point and the string's start from a station.
#include "ftcore/PartnerBrain.h"
#include "AllocCounter.h"
#include "TestAssets.h"
#include <doctest/doctest.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace ftcore;

namespace {

using V = PartnerEnemyView;
constexpr float kRad = 3.14159265358979f / 180.f;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

float len(Vec2 a) { return std::hypot(a.x, a.y); }
Vec2 minus(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 unit(Vec2 a) {
    const float l = len(a);
    return {a.x / l, a.y / l};
}
float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

// The wedge as the order states it: from the camera to `beyond` past the lead, `half` degrees either side of the
// camera-to-lead line, grown by `margin` (0: the wedge itself).
bool inWedge(Vec2 p, Vec2 cam, Vec2 lead, float margin = 0.f, float half = 25.f, float beyond = 1.f) {
    const Vec2 axis = unit(minus(lead, cam));
    const float reach = len(minus(lead, cam)) + beyond;
    const Vec2 d = minus(p, cam);
    const float along = dot(d, axis);
    if (along <= 0.f || along >= reach + margin) return false;
    return std::fabs(cross(axis, d)) * std::cos(half * kRad) - along * std::sin(half * kRad) < margin;
}
// The angle at the target between the lead and p (degrees).
float angleAt(Vec2 target, Vec2 lead, Vec2 p) {
    const float c = dot(unit(minus(lead, target)), unit(minus(p, target)));
    return std::acos(std::fmax(-1.f, std::fmin(1.f, c))) / kRad;
}

// A lead at the origin facing +x, its target 2.5 m ahead (radius 0.5), a camera 4.5 m from the lead.
struct Pit {
    PartnerBrain b;
    PartnerInput in;
    std::array<V, kMaxPartnerEnemies> e{};
    std::size_t n = 1;
    explicit Pit(PartnerOrder order, PartnerTuning t = PartnerTuning{}, std::uint32_t seed = 3) {
        t.circling = true; // rule 17 on, as these held it (off is the shipped default)
        t.budgetShare = 1.f; // the budget out of the way
        t.budgetAllowanceSeconds = 10.f;
        REQUIRE(b.begin(t, seed));
        in.order = order;
        in.leadAttacking = true;
        in.leadPos = {0.f, 0.f};
        in.leadTarget = 0;
        in.pos = {-3.f, 3.f};
        e[0].pos = {2.5f, 0.f};
        e[0].radius = 0.5f;
        in.station.on = true;
        in.station.leadFacing = {1.f, 0.f};
        camAt(180.f);
    }
    // The camera at `deg` round the lead (180: straight behind it), looking at it.
    void camAt(float deg, float d = 4.5f) {
        in.station.cameraPos = {in.leadPos.x + d * std::cos(deg * kRad), in.leadPos.y + d * std::sin(deg * kRad)};
        in.station.cameraForward = unit(minus(in.leadPos, in.station.cameraPos));
    }
    Vec2 cam() const { return in.station.cameraPos; }
    const PartnerIntent& step(double dt = 1.0 / 60.0) {
        in.enemies = std::span<const V>(e.data(), n);
        return b.step(dt, in);
    }
};

// Is any point of the arc (both sides) free of the grown wedge, personal space and (StayClose) the cap? The test's
// own look, so the arc checks below are asked only where the order's rules leave room.
bool arcFree(const Pit& p, float lo, float hi, bool close, float side = 0.f) {
    const Vec2 t = p.e[0].pos;
    const float ring = p.e[0].radius + 0.9f;
    const Vec2 u = unit(minus(p.in.leadPos, t));
    for (float s : {-1.f, 1.f}) {
        if (side != 0.f && s != side) continue;
        for (float deg = lo; deg <= hi + 1e-3f; deg += 1.f) {
            const float r = s * deg * kRad;
            const Vec2 q{t.x + (u.x * std::cos(r) - u.y * std::sin(r)) * ring, t.y + (u.x * std::sin(r) + u.y * std::cos(r)) * ring};
            if (inWedge(q, p.cam(), p.in.leadPos, 0.5f)) continue;
            if (len(minus(q, p.in.leadPos)) < 1.6f) continue;
            if (close && len(minus(q, p.in.leadPos)) > 3.5f) continue;
            return true;
        }
    }
    return false;
}

// Rules 8 and 9 for one goal.
void checkGoal(const Pit& p, Vec2 g) {
    CAPTURE(g.x);
    CAPTURE(g.y);
    REQUIRE(std::isfinite(g.x));
    REQUIRE(std::isfinite(g.y));
    CHECK_FALSE(inWedge(g, p.cam(), p.in.leadPos));
    CHECK_FALSE(inWedge(g, p.cam(), p.in.leadPos, 0.49f)); // the clearance kept
    CHECK(len(minus(g, p.in.leadPos)) >= 1.6f - 1e-3f);
}

} // namespace

TEST_CASE("partner station: a ring of camera angles: no goal in the wedge or within 1.6 m, StayClose and Spread in their arcs") {
    const Vec2 starts[] = {{-2.5f, 1.5f}, {-2.5f, -1.5f}, {2.5f, 2.8f}, {2.5f, -2.8f}, {0.5f, -3.f}}; // within the leash, off every station
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        const bool close = order == PartnerOrder::StayClose;
        const float lo = close ? 60.f : 120.f, hi = close ? 90.f : 180.f;
        int arcs = 0;
        for (int a = 0; a < 360; a += 10) {
            for (const Vec2 start : starts) {
                Pit p(order);
                p.camAt(static_cast<float>(a));
                p.in.pos = start;
                CAPTURE(close);
                CAPTURE(a);
                CAPTURE(start.x);
                CAPTURE(start.y);
                const PartnerIntent i = p.step();
                REQUIRE(i.stationed);
                REQUIRE(i.act == PartnerAct::MoveTo);
                CHECK(i.why == PartnerWhy::Approach);
                checkGoal(p, i.point);
                if (arcFree(p, lo, hi, close)) {
                    ++arcs;
                    const float ang = angleAt(p.e[0].pos, p.in.leadPos, i.point);
                    CHECK(ang >= lo - 0.5f);
                    CHECK(ang <= hi + 0.5f);
                    if (close) CHECK(len(minus(i.point, p.in.leadPos)) <= 3.5f + 1e-3f);
                    CHECK(len(minus(i.point, p.e[0].pos)) - p.e[0].radius <= 1.8f); // in reach
                    // Rule 12: from the station the string starts.
                    p.in.pos = i.point;
                    const PartnerIntent j = p.step();
                    CHECK(j.act == PartnerAct::Attack);
                    CHECK(j.why == PartnerWhy::Start);
                }
            }
        }
        CHECK(arcs >= 36 * 5 / 2); // the arc checks are not vacuous
    }
}

TEST_CASE("partner station: the side holds 2 s, then goes to the nearer; the wedge forces a change at once") {
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        CAPTURE(order == PartnerOrder::StayClose);
        Pit p(order);
        p.in.pos = {1.5f, 3.5f}; // on the +y side, within the leash
        PartnerIntent i = p.step();
        REQUIRE(i.act == PartnerAct::MoveTo);
        REQUIRE(i.point.y > 0.f);
        const std::int8_t first = p.b.save().side;
        REQUIRE(first != 0);
        p.in.pos = {1.5f, -3.5f}; // now nearer the -y side
        double held = 0.0;
        for (int k = 0; k < 600; ++k) {
            i = p.step();
            REQUIRE(i.act == PartnerAct::MoveTo);
            checkGoal(p, i.point);
            held += 1.0 / 60.0;
            if (p.b.save().side != first) break;
        }
        CHECK(p.b.save().side == -first);
        CHECK(i.point.y <= 1e-3f);
        CHECK(held >= 2.0 - 1.0 / 60.0 - 1e-6);
        CHECK(held <= 2.0 + 2.0 / 60.0);
    }

    // Forced: a camera that leaves the +y arc no station; the side changes on the next step.
    Pit p(PartnerOrder::StayClose);
    p.in.pos = {1.5f, 3.5f};
    REQUIRE(p.step().point.y > 0.f);
    int found = -1;
    for (int a = 0; a < 360 && found < 0; a += 2) {
        p.camAt(static_cast<float>(a), 4.f);
        if (!arcFree(p, 60.f, 90.f, true, 0.f)) continue;
        // +y side: rotating the lead's direction (-x seen from the target) clockwise.
        if (!arcFree(p, 60.f, 90.f, true, -1.f) && arcFree(p, 60.f, 90.f, true, 1.f)) found = a;
    }
    REQUIRE(found >= 0);
    CAPTURE(found);
    const PartnerIntent i = p.step();
    REQUIRE(i.act == PartnerAct::MoveTo);
    checkGoal(p, i.point);
    CHECK(i.point.y < 0.f);
    const float ang = angleAt(p.e[0].pos, p.in.leadPos, i.point);
    CHECK(ang >= 59.5f);
    CHECK(ang <= 90.5f);
}

TEST_CASE("partner station: no target, it stands 2.0-2.5 m behind the lead and to a side, outside the wedge") {
    int behind = 0;
    for (int a = 0; a < 360; a += 10) {
        for (const Vec2 start : {Vec2{-3.f, 3.f}, Vec2{-3.f, -3.f}, Vec2{4.f, 0.5f}}) {
            Pit p(PartnerOrder::Spread);
            p.n = 0;
            p.in.leadTarget = -1;
            p.camAt(static_cast<float>(a));
            p.in.pos = start;
            CAPTURE(a);
            CAPTURE(start.x);
            const PartnerIntent i = p.step();
            REQUIRE(i.act == PartnerAct::MoveTo);
            CHECK(i.why == PartnerWhy::NoTarget);
            checkGoal(p, i.point);
            if (a >= 120 && a <= 240) { // the camera behind the lead: the follow points are free
                ++behind;
                const float d = len(i.point);
                CHECK(d >= 2.f - 1e-3f);
                CHECK(d <= 2.5f + 1e-3f);
                CHECK(dot(i.point, p.in.station.leadFacing) < 0.f); // behind
                CHECK(std::fabs(cross(p.in.station.leadFacing, i.point)) >= 0.5f); // to a side
            }
            p.in.pos = i.point; // there: it waits
            const PartnerIntent j = p.step();
            CHECK(j.act == PartnerAct::Wait);
            CHECK(j.why == PartnerWhy::NoTarget);
        }
    }
    CHECK(behind == 13 * 3);
}

TEST_CASE("partner station: a string starts only from its station; later blows are not held; the pause goes back") {
    Pit p(PartnerOrder::StayClose);
    p.in.pos = {3.5f, 0.5f}; // in reach (0.62 m from the edge) but on the far side, not a station
    PartnerIntent i = p.step();
    REQUIRE(i.act == PartnerAct::MoveTo);
    CHECK(i.why == PartnerWhy::Approach);
    CHECK(p.b.rhythmDraws() == 0u);
    const Vec2 station = i.point;
    p.in.pos = station;
    i = p.step();
    REQUIRE(i.why == PartnerWhy::Start);
    // Mid-string, off the station: the string runs on.
    p.in.pos = {3.5f, 0.5f};
    bool paused = false;
    for (int k = 0; k < 600 && !paused; ++k) {
        p.in.doing = PartnerDoing::Attacking;
        i = p.step();
        if (i.why == PartnerWhy::Pause) {
            paused = true;
            break;
        }
        CHECK(i.act == PartnerAct::Attack);
    }
    REQUIRE(paused);
    // Nearer the edge than the standoff: StepBack, to the station.
    CHECK(i.act == PartnerAct::StepBack);
    CHECK(len(minus(i.point, station)) < 0.5f);
    checkGoal(p, i.point);
    p.in.pos = {1.5f, -3.f}; // well off (within the leash): MoveTo
    p.in.doing = PartnerDoing::Idle;
    i = p.step();
    CHECK(i.why == PartnerWhy::Pause);
    CHECK(i.act == PartnerAct::MoveTo);
    checkGoal(p, i.point);
}

TEST_CASE("partner station: reviving lets it close on the lead; no room in the arena: as far as it can get") {
    // The lead 0.6 m from its target's centre: every arc station is within 1.6 m of it. A narrow wedge, so the arc
    // is clear of it.
    PartnerTuning narrow;
    narrow.wedgeHalfDegrees = 10.f;
    narrow.wedgeClearMeters = 0.1f;
    {
        Pit p(PartnerOrder::StayClose, narrow);
        p.in.leadPos = {1.9f, 0.f};
        p.in.pos = {2.5f, 3.f};
        p.camAt(180.f);
        const PartnerIntent i = p.step();
        REQUIRE(i.act == PartnerAct::MoveTo);
        CHECK_FALSE(inWedge(i.point, p.cam(), p.in.leadPos, 0.f, 10.f));
        CHECK(len(minus(i.point, p.in.leadPos)) >= 1.6f - 1e-3f);
        const float ang = angleAt(p.e[0].pos, p.in.leadPos, i.point);
        CHECK(ang > 90.f); // out of the arc: personal space wins
    }
    {
        Pit p(PartnerOrder::StayClose, narrow);
        p.in.leadPos = {1.9f, 0.f};
        p.in.pos = {2.5f, 3.f};
        p.camAt(180.f);
        p.in.station.reviving = true;
        const PartnerIntent i = p.step();
        REQUIRE(i.act == PartnerAct::MoveTo);
        CHECK_FALSE(inWedge(i.point, p.cam(), p.in.leadPos, 0.f, 10.f));
        const float ang = angleAt(p.e[0].pos, p.in.leadPos, i.point);
        CHECK(ang >= 59.5f);
        CHECK(ang <= 90.5f);
        CHECK(len(minus(i.point, p.in.leadPos)) < 1.6f);
    }
    // An arena 1 m round the lead: no point 1.6 m out; the goal is on its edge, out of the wedge (a narrow one, so
    // the arena has room outside it; the tuning above).
    for (int a = 0; a < 360; a += 30) {
        Pit p(PartnerOrder::Spread, narrow);
        p.n = 0;
        p.in.leadTarget = -1;
        p.camAt(static_cast<float>(a));
        p.in.station.arenaCentre = {0.f, 0.f};
        p.in.station.arenaRadius = 1.f;
        p.in.pos = {0.f, 0.1f};
        CAPTURE(a);
        const PartnerIntent i = p.step();
        REQUIRE(i.act == PartnerAct::MoveTo);
        CHECK(len(i.point) <= 1.f + 1e-3f);
        CHECK(len(i.point) >= 1.f - 0.02f);
        CHECK_FALSE(inWedge(i.point, p.cam(), p.in.leadPos, 0.f, 10.f));
    }
}

TEST_CASE("partner station: the leash's goal keeps the rules and ends the return") {
    for (int a = 0; a < 360; a += 15) {
        Pit p(PartnerOrder::StayClose);
        p.camAt(static_cast<float>(a));
        p.in.pos = {-5.f, 1.f}; // past the 4 m leash
        CAPTURE(a);
        const PartnerIntent i = p.step();
        REQUIRE(i.why == PartnerWhy::Leash);
        checkGoal(p, i.point);
        CHECK(len(minus(i.point, p.in.leadPos)) <= 3.f);
        p.in.pos = i.point; // there, the return is over
        CHECK(p.step().why != PartnerWhy::Leash);
    }
}

TEST_CASE("partner station: degenerate views (camera on the lead, zero facing, lead on the target) stay finite and legal") {
    for (int k = 0; k < 3; ++k) {
        Pit p(k == 1 ? PartnerOrder::Spread : PartnerOrder::StayClose);
        if (k == 0) { // the camera on the lead: its forward is the wedge's axis
            p.in.station.cameraPos = p.in.leadPos;
            p.in.station.cameraForward = {1.f, 0.f};
        } else if (k == 1) {
            p.in.station.leadFacing = {0.f, 0.f};
            p.n = 0;
            p.in.leadTarget = -1;
        } else {
            p.in.leadPos = p.e[0].pos;
            p.camAt(180.f);
        }
        CAPTURE(k);
        for (int s = 0; s < 120; ++s) {
            const PartnerIntent i = p.step();
            REQUIRE(i.stationed);
            if (i.act == PartnerAct::MoveTo || i.act == PartnerAct::StepBack) {
                REQUIRE(std::isfinite(i.point.x));
                REQUIRE(std::isfinite(i.point.y));
                CHECK(len(minus(i.point, p.in.leadPos)) >= 1.6f - 1e-3f);
                if (k == 0) { // the wedge along the forward, 1 m out
                    CHECK_FALSE((i.point.x > 0.f && i.point.x < 1.5f &&
                                 std::fabs(i.point.y) < i.point.x * std::tan(25.f * kRad) + 0.5f));
                } else {
                    CHECK_FALSE(inWedge(i.point, p.cam(), p.in.leadPos));
                }
                p.in.pos = {p.in.pos.x + (i.point.x - p.in.pos.x) * 0.1f, p.in.pos.y + (i.point.y - p.in.pos.y) * 0.1f};
            }
        }
    }
}

TEST_CASE("partner station: a bad view is no stationing, the same intents as off") {
    for (int k = 0; k < 6; ++k) {
        Pit on(PartnerOrder::Spread), off(PartnerOrder::Spread);
        off.in.station.on = false;
        if (k == 0) on.in.station.cameraPos.x = kNaN;
        if (k == 1) on.in.station.cameraForward.y = std::numeric_limits<float>::infinity();
        if (k == 2) on.in.station.leadFacing.x = kNaN;
        if (k == 3) on.in.station.arenaRadius = -1.f;
        if (k == 4) on.in.station.arenaCentre.y = kNaN;
        if (k == 5) on.in.station.arenaRadius = kNaN;
        CAPTURE(k);
        for (int s = 0; s < 200; ++s) {
            const PartnerIntent a = on.step(), b = off.step();
            CHECK_FALSE(a.stationed);
            CHECK(a.act == b.act);
            CHECK(a.why == b.why);
            CHECK(a.point.x == b.point.x);
            CHECK(a.point.y == b.point.y);
            CHECK(a.blowStart == b.blowStart);
            on.in.pos = off.in.pos = {on.in.pos.x + 0.02f, on.in.pos.y - 0.01f};
        }
        CHECK(on.b.hash() == off.b.hash());
    }
}

TEST_CASE("partner station: save and load carry the side and its timer; the hash covers them") {
    Pit p(PartnerOrder::Spread);
    p.in.pos = {2.5f, 5.f};
    for (int s = 0; s < 30; ++s) p.step();
    const PartnerBrainSave saved = p.b.save();
    CHECK(saved.side != 0);
    CHECK(saved.sideFor > 0.4);
    PartnerBrain q;
    q.load(saved);
    CHECK(q.hash() == p.b.hash());
    p.in.pos = {2.5f, -5.f};
    for (int s = 0; s < 200; ++s) {
        const PartnerIntent a = p.step();
        p.in.enemies = std::span<const V>(p.e.data(), p.n);
        const PartnerIntent b = q.step(1.0 / 60.0, p.in);
        CHECK(a.point.x == b.point.x);
        CHECK(a.point.y == b.point.y);
    }
    CHECK(q.hash() == p.b.hash());
    PartnerBrainSave other = saved;
    other.side = static_cast<std::int8_t>(-saved.side);
    CHECK(other.hash() != saved.hash());
    other = saved;
    other.sideFor += 0.5;
    CHECK(other.hash() != saved.hash());
    other = saved;
    other.t.personalSpaceMeters = 2.f;
    CHECK(other.hash() != saved.hash());
}

TEST_CASE("partner station: partner_brain.json carries the defaults; the importer refuses bad stationing fields and resets broken pairs") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/partner_brain.json", text));
    PartnerTuning t;
    std::string err;
    REQUIRE(importPartnerBrain(text, t, &err));
    const PartnerTuning d;
    CHECK(t.personalSpaceMeters == doctest::Approx(1.6f));
    CHECK(t.wedgeHalfDegrees == doctest::Approx(25.f));
    CHECK(t.wedgeBeyondMeters == doctest::Approx(1.f));
    CHECK(t.wedgeClearMeters == doctest::Approx(0.5f));
    CHECK(t.closeArcMin == doctest::Approx(60.f));
    CHECK(t.closeArcMax == doctest::Approx(90.f));
    CHECK(t.closeLeadMeters == doctest::Approx(3.5f));
    CHECK(t.spreadArcMin == doctest::Approx(120.f));
    CHECK(t.spreadArcMax == doctest::Approx(180.f));
    CHECK(t.sideHoldSeconds == doctest::Approx(2.f));
    CHECK(t.followMin == doctest::Approx(2.f));
    CHECK(t.followMax == doctest::Approx(2.5f));
    CHECK(t.stationTolerance == doctest::Approx(0.5f));
    CHECK(t.personalSpaceMeters == d.personalSpaceMeters);
    CHECK(t.stationTolerance == d.stationTolerance);
    for (const char* bad : {
             R"({"version":1,"personalSpaceMeters":0.4})",
             R"({"version":1,"personalSpaceMeters":5})",
             R"({"version":1,"wedgeHalfDegrees":70})",
             R"({"version":1,"wedgeBeyondMeters":-1})",
             R"({"version":1,"wedgeClearMeters":3})",
             R"({"version":1,"closeArcMax":200})",
             R"({"version":1,"closeLeadMeters":0.5})",
             R"({"version":1,"spreadArcMin":181})",
             R"({"version":1,"sideHoldSeconds":11})",
             R"({"version":1,"followMax":0.4})",
             R"({"version":1,"stationTolerance":0})",
             R"({"version":1,"stationTolerance":"0.5"})",
         }) {
        CAPTURE(bad);
        CHECK_FALSE(importPartnerBrain(bad, t, &err));
        CHECK_FALSE(err.empty());
        CHECK(t.wedgeHalfDegrees == d.wedgeHalfDegrees);
    }
    for (const char* bad : {R"({"version":1,"stationGiveUpSeconds":0.4})", R"({"version":1,"stationAnySeconds":61})",
                            R"({"version":1,"targetHoldSeconds":-1})", R"({"version":1,"targetHoldSeconds":11})"}) {
        CAPTURE(bad);
        CHECK_FALSE(importPartnerBrain(bad, t, &err));
    }
    CHECK(d.stationGiveUpSeconds == 3.f);
    CHECK(d.stationAnySeconds == 6.f);
    CHECK(d.targetHoldSeconds == 1.5f);
    CHECK(t.stationGiveUpSeconds == d.stationGiveUpSeconds);
    // Cross-field pairs: each arc's min over its max, the follow pair, the tolerance over half the reach: that pair
    // goes back to its defaults (noted), the rest is kept.
    struct PairCase {
        const char* json;
        const char* note;
    };
    const PairCase pairs[] = {
        {R"({"version":1,"closeArcMin":100,"sideHoldSeconds":0.5})", "closeArcMin, closeArcMax"},
        {R"({"version":1,"spreadArcMax":100,"sideHoldSeconds":0.5})", "spreadArcMin, spreadArcMax"},
        {R"({"version":1,"followMin":3,"sideHoldSeconds":0.5})", "followMin, followMax"},
        {R"({"version":1,"reachMeters":0.8,"standoffMeters":0.5,"stationTolerance":0.5,"sideHoldSeconds":0.5})",
         "stationTolerance, reachMeters"},
    };
    for (const PairCase& c : pairs) {
        CAPTURE(c.json);
        std::vector<std::string> notes;
        REQUIRE(importPartnerBrain(c.json, t, &err, &notes));
        REQUIRE(notes.size() == 1u);
        CHECK(notes[0].rfind(c.note, 0) == 0);
        CHECK(t.sideHoldSeconds == 0.5f);
        CHECK(t.closeArcMin == d.closeArcMin);
        CHECK(t.closeArcMax == d.closeArcMax);
        CHECK(t.spreadArcMax == d.spreadArcMax);
        CHECK(t.followMin == d.followMin);
        CHECK(t.stationTolerance == d.stationTolerance);
        CHECK(t.reachMeters == d.reachMeters);
        CHECK(validPartnerTuning(t));
    }
    {
        std::vector<std::string> notes;
        REQUIRE(importPartnerBrain(R"({"version":1,"reachMeters":0.8,"standoffMeters":0.5,"stationTolerance":0.4})", t,
                                   &err, &notes));
        CHECK(notes.empty()); // standoff within the reach, tolerance within half of it
        CHECK(t.reachMeters == 0.8f);
    }
    REQUIRE(importPartnerBrain(R"({"version":1,"closeArcMin":40,"closeArcMax":70,"sideHoldSeconds":0.5})", t, &err));
    CHECK(t.closeArcMin == 40.f);
    CHECK(t.sideHoldSeconds == 0.5f);
}

TEST_CASE("partner station: stationed steps allocate nothing") {
    Pit p(PartnerOrder::Spread);
    p.in.station.arenaRadius = 12.f;
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    for (int s = 0; s < 600; ++s) {
        p.camAt(static_cast<float>(s % 360));
        p.n = (s / 120) % 2 == 0 ? 1u : 0u;
        p.in.leadTarget = p.n == 1u ? 0 : -1;
        const PartnerIntent& i = p.step();
        if (i.act == PartnerAct::MoveTo)
            p.in.pos = {p.in.pos.x + (i.point.x - p.in.pos.x) * 0.2f, p.in.pos.y + (i.point.y - p.in.pos.y) * 0.2f};
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    if (counting) CHECK(allocs == 0u);
}

TEST_CASE("partner station: StayClose keeps its station within 3.5 m of the lead when the arc runs past it") {
    PartnerTuning narrow; // a narrow wedge from the side, out of the arc's way
    narrow.wedgeHalfDegrees = 10.f;
    narrow.wedgeClearMeters = 0.1f;
    Pit p(PartnerOrder::StayClose, narrow);
    p.in.leadPos = {-1.1f, 0.f}; // 3.6 m from the target: past about 75 degrees the arc is over 3.5 m from the lead
    p.camAt(270.f);
    p.in.pos = {2.2f, 1.9f}; // nearest the arc at about 80 degrees
    const PartnerIntent i = p.step();
    REQUIRE(i.act == PartnerAct::MoveTo);
    CHECK(len(minus(i.point, p.in.leadPos)) <= 3.5f + 1e-3f);
    CHECK(len(minus(i.point, p.in.leadPos)) >= 3.3f); // the nearest the partner within the cap, not the arc's start
    const float ang = angleAt(p.e[0].pos, p.in.leadPos, i.point);
    CHECK(ang >= 59.5f);
    CHECK(ang <= 90.5f);
    CHECK_FALSE(inWedge(i.point, p.cam(), p.in.leadPos, 0.f, 10.f));
}

namespace {
// A walking partner: 4 m/s toward the intent's point on MoveTo or StepBack, for `seconds` at 60 Hz. `lead` moves the
// lead (and the camera with it) before each step; `each` sees every intent.
template <class Lead, class Each> void walk(Pit& p, float seconds, Lead lead, Each each) {
    const float dt = 1.f / 60.f;
    for (int k = 0; k < static_cast<int>(seconds * 60.f); ++k) {
        const float now = static_cast<float>(k) * dt;
        lead(now);
        const PartnerIntent& i = p.step(dt);
        each(now, i);
        if (i.act == PartnerAct::MoveTo || i.act == PartnerAct::StepBack) {
            const Vec2 d = minus(i.point, p.in.pos);
            const float l = len(d), s = std::fmin(l, 4.f * dt);
            if (l > 1e-6f) p.in.pos = {p.in.pos.x + d.x / l * s, p.in.pos.y + d.y / l * s};
        }
    }
}
} // namespace

TEST_CASE("partner station: a walking StayClose partner never yo-yos on the leash, for a far target or a boss") {
    SUBCASE("far target: 7 m from the lead, no station within the cap") {
        Pit p(PartnerOrder::StayClose);
        p.e[0].pos = {7.f, 0.f};
        p.in.pos = {-1.f, -2.5f};
        int leash = 0, moves = 0;
        walk(
            p, 10.f,
            [&](float now) {
                p.in.leadPos = {0.f, 0.4f * std::sin(1.5f * now)}; // drifts about a centimetre a step
                p.camAt(180.f);
            },
            [&](float, const PartnerIntent& i) {
                REQUIRE(i.stationed);
                if (i.why == PartnerWhy::Leash) ++leash;
                if (i.act == PartnerAct::MoveTo) {
                    ++moves;
                    CHECK(len(minus(i.point, p.in.leadPos)) <= 3.5f + 1e-3f);
                }
                CHECK(len(minus(p.in.pos, p.in.leadPos)) <= 4.f);
            });
        CHECK(leash == 0);
        CHECK(moves > 0);
    }
    SUBCASE("boss: radius 3 m, the lead 1.2 m off its edge; the arc has nothing within 3.5 m of the lead") {
        Pit p(PartnerOrder::StayClose);
        p.e[0].pos = {4.2f, 0.f};
        p.e[0].radius = 3.f;
        p.in.pos = {-1.f, 2.5f};
        int leash = 0, starts = 0;
        walk(
            p, 10.f,
            [&](float now) {
                p.in.leadPos = {0.f, 0.4f * std::sin(1.5f * now)};
                p.camAt(180.f);
            },
            [&](float, const PartnerIntent& i) {
                if (i.why == PartnerWhy::Leash) ++leash;
                if (i.act == PartnerAct::Attack && i.newString) ++starts;
                if (i.act == PartnerAct::MoveTo) checkGoal(p, i.point);
            });
        CHECK(leash == 0);
        CHECK(starts >= 1);
    }
}

TEST_CASE("partner station: with the camera orbiting 30 degrees a second a walking partner reaches its station and strings start; the side holds 2 s") {
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        CAPTURE(order == PartnerOrder::StayClose);
        Pit p(order);
        p.in.pos = {-2.5f, 1.5f};
        float firstStart = -1.f, lastChange = -10.f, minGap = 100.f;
        int side = 0, changes = 0, starts = 0, leash = 0, forced = 0;
        const bool close = order == PartnerOrder::StayClose;
        const float lo = close ? 60.f : 120.f, hi = close ? 90.f : 180.f;
        walk(
            p, 12.f, [&](float now) { p.camAt(180.f + 30.f * now); },
            [&](float now, const PartnerIntent& i) {
                if (i.why == PartnerWhy::Leash) ++leash;
                if (i.act == PartnerAct::MoveTo) checkGoal(p, i.point);
                if (i.act == PartnerAct::Attack && i.newString) {
                    ++starts;
                    if (firstStart < 0.f) firstStart = now;
                }
                const int s = p.b.save().side;
                if (s != 0 && side != 0 && s != side) {
                    ++changes;
                    // Inside the hold only the wedge (or 8) may force it: the old side's arc had no station left.
                    if (now - lastChange < 2.f - 1e-3f) {
                        ++forced;
                        CHECK_FALSE(arcFree(p, lo + 3.f, hi - 3.f, close, static_cast<float>(side)));
                    }
                    minGap = std::fmin(minGap, now - lastChange);
                    lastChange = now;
                } else if (s != 0 && side == 0) {
                    lastChange = now;
                }
                side = s;
            });
        CAPTURE(changes);
        CAPTURE(minGap);
        CHECK(leash == 0);
        CHECK(firstStart >= 0.f);
        CHECK(firstStart <= 3.f);
        CHECK(starts >= 2);
        CHECK(changes <= 6); // a full orbit and a third: no flicker
        MESSAGE("orbit: changes " << changes << " (forced " << forced << "), min gap " << minGap << " s, first start "
                                  << firstStart << " s, starts " << starts);
    }
}

namespace {
// The starvation fallback's hosts. `drift`: the whole fight (lead, camera, target) moves by the partner's step, so its
// station runs from it exactly as fast as it walks (it never gets nearer). `wallR` > 0: a wall round the target that
// the partner cannot enter (every station is inside it; it slides along its edge). `walk` false: it never moves. Returns
// the first string's start time (-1: none in `seconds`), with the spot it started from in `at`.
float starve(Pit& p, float seconds, bool walk, bool drift, float wallR, Vec2& at) {
    const float dt = 1.f / 60.f;
    for (int k = 0; k < static_cast<int>(seconds * 60.f); ++k) {
        const float now = static_cast<float>(k) * dt;
        const PartnerIntent i = p.step(dt);
        if (i.act == PartnerAct::Attack && i.newString) {
            at = p.in.pos;
            return now;
        }
        if (!walk || (i.act != PartnerAct::MoveTo && i.act != PartnerAct::StepBack)) continue;
        const Vec2 d = minus(i.point, p.in.pos);
        const float l = len(d), st = std::fmin(l, 4.f * dt);
        if (l <= 1e-6f) continue;
        const Vec2 m{d.x / l * st, d.y / l * st};
        Vec2 next{p.in.pos.x + m.x, p.in.pos.y + m.y};
        const Vec2 in = minus(next, p.e[0].pos);
        if (wallR > 0.f && len(in) < wallR) // the wall: it slides along it
            next = {p.e[0].pos.x + in.x / len(in) * wallR, p.e[0].pos.y + in.y / len(in) * wallR};
        p.in.pos = next;
        if (drift) {
            p.in.leadPos = {p.in.leadPos.x + m.x, p.in.leadPos.y + m.y};
            p.in.station.cameraPos = {p.in.station.cameraPos.x + m.x, p.in.station.cameraPos.y + m.y};
            p.e[0].pos = {p.e[0].pos.x + m.x, p.e[0].pos.y + m.y};
        }
    }
    return -1.f;
}
} // namespace

TEST_CASE("partner station: a station it cannot reach never starves it: from where it stands after 3 s outside the wedge, after 6 s anywhere outside personal space") {
    const float dt = 1.f / 60.f;
    // In reach, outside personal space: (2.5, 2.2) outside the wedge, (0.9, 1.4) inside it (the camera behind).
    struct Case {
        Vec2 start;
        bool inWedgeSpot;
    };
    const Case cases[] = {{{2.5f, 2.2f}, false}, {{0.9f, 1.4f}, true}};
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        for (const Case& c : cases) {
            for (const int host : {0, 1}) { // 0: a station that runs from it every step; 1: it cannot move at all
                CAPTURE(order == PartnerOrder::StayClose);
                CAPTURE(c.start.x);
                CAPTURE(host);
                Pit p(order);
                p.in.pos = c.start;
                REQUIRE(inWedge(c.start, p.cam(), p.in.leadPos) == c.inWedgeSpot);
                Vec2 at{};
                const float first = starve(p, 10.f, host == 0, host == 0, 0.f, at);
                const float due = c.inWedgeSpot ? 6.f : 3.f;
                CHECK(first >= due - dt - 1e-4f);
                CHECK(first <= due + dt + 1e-4f);
                CHECK(len(minus(at, p.in.leadPos)) >= 1.6f - 1e-3f);
                CHECK(len(minus(at, p.e[0].pos)) - p.e[0].radius <= 1.8f + 1e-4f);
                if (first < 6.f - dt) CHECK_FALSE(inWedge(at, p.cam(), p.in.leadPos));
            }
        }
    }
    // The timers at their most: no string in 20 s.
    for (const Case& c : cases) {
        PartnerTuning t;
        t.stationGiveUpSeconds = 30.f;
        t.stationAnySeconds = 60.f;
        Pit p(PartnerOrder::Spread, t);
        p.in.pos = c.start;
        Vec2 at{};
        CHECK(starve(p, 20.f, false, false, 0.f, at) < 0.f);
    }
    // Inside personal space it never starts from there: (0.8, 0.8) is in reach but 1.13 m from the lead.
    {
        Pit p(PartnerOrder::Spread);
        p.in.pos = {0.8f, 0.8f};
        Vec2 at{};
        CHECK(starve(p, 10.f, false, false, 0.f, at) < 0.f);
    }
}

TEST_CASE("partner station: every station inside a wall: it stops at the wall, then strings start from there by 6 s") {
    const float dt = 1.f / 60.f;
    int early = 0;
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        for (const Vec2 start : {Vec2{-2.5f, 1.5f}, Vec2{-2.5f, -1.5f}, Vec2{1.5f, -3.f}, Vec2{0.5f, 3.5f}}) {
            for (int a = 120; a <= 240; a += 30) {
                CAPTURE(order == PartnerOrder::StayClose);
                CAPTURE(start.x);
                CAPTURE(start.y);
                CAPTURE(a);
                Pit p(order);
                p.camAt(static_cast<float>(a));
                p.in.pos = start;
                Vec2 at{};
                // The wall: 2.1 m round the target's centre; its stations are 1.4 m out, its reach 2.3 m.
                const float first = starve(p, 10.f, true, false, 2.1f, at);
                REQUIRE(first >= 0.f);
                CHECK(first >= 3.f - dt - 1e-4f);
                CHECK(first <= 6.f + dt + 1e-4f);
                CHECK(len(minus(at, p.in.leadPos)) >= 1.6f - 1e-3f);
                CHECK(len(minus(at, p.e[0].pos)) - p.e[0].radius <= 1.8f + 1e-4f);
                if (first < 6.f - dt) {
                    ++early;
                    CHECK_FALSE(inWedge(at, p.cam(), p.in.leadPos));
                }
            }
        }
    }
    CHECK(early > 0);
}

TEST_CASE("partner station: out of reach once 3 s have run it goes to the nearest in-reach point, outside personal space") {
    // A station inside a wall that stops it 2.6 m from the target's centre (out of reach, 2.3 m): after 3 s the goal
    // is the nearest point of the reach ring in the arena, outside personal space; the host then lets it through.
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        CAPTURE(order == PartnerOrder::StayClose);
        Pit p(order);
        p.in.pos = {2.5f, 2.6f}; // out of reach, beside the target
        const float dt = 1.f / 60.f;
        Vec2 goal{}, station{};
        bool seen = false;
        for (int k = 0; k < 400 && !seen; ++k) {
            const PartnerIntent i = p.step(dt);
            if (k == 0) station = i.point;
            REQUIRE(i.act == PartnerAct::MoveTo);
            CHECK(i.why == PartnerWhy::Approach);
            if (static_cast<float>(k) * dt >= 3.f + dt) {
                goal = i.point;
                seen = true;
            }
        }
        REQUIRE(seen);
        CHECK(len(minus(goal, p.e[0].pos)) == doctest::Approx(1.4f).epsilon(1e-3)); // on the ring, in reach
        CHECK(len(minus(goal, p.in.leadPos)) >= 1.6f - 1e-3f);
        CHECK(len(minus(goal, p.in.pos)) <= 1.25f); // the nearest: straight in, or near it
        if (order == PartnerOrder::Spread) CHECK(len(minus(goal, station)) > 0.5f); // not the station (the far side)
        p.in.pos = goal;
        const PartnerIntent j = p.step(dt);
        CHECK(j.act == PartnerAct::Attack); // there, outside the wedge: it starts
        CHECK(j.newString);
    }
}

namespace {
// The grid of rings round the lead the brain's resolver tries (personal space + 0.05 .. + 2.4, every 11.25 degrees):
// does any of it lie in the arena, outside personal space and the grown wedge?
bool roomOnGrid(const Pit& p, float arenaR, float half, float beyond, float clear) {
    for (const float r : {0.05f, 0.5f, 1.f, 1.6f, 2.4f})
        for (int k = 0; k < 32; ++k) {
            const float a = 11.25f * static_cast<float>(k) * kRad;
            const Vec2 q{p.in.leadPos.x + (1.6f + r) * std::cos(a), p.in.leadPos.y + (1.6f + r) * std::sin(a)};
            if (arenaR > 0.f && len(minus(q, p.in.station.arenaCentre)) > arenaR) continue;
            if (inWedge(q, p.cam(), p.in.leadPos, clear, half, beyond)) continue;
            return true;
        }
    return false;
}
} // namespace

TEST_CASE("partner station: when rules collide the arena wins, then personal space, then the wedge, then the arc") {
    SUBCASE("a wedge whose only ways out in the arena are inside personal space: the goal is outside it, in the wedge") {
        PartnerTuning t;
        t.wedgeHalfDegrees = 60.f; // the camera a metre behind: its edges pass 0.87 m from the lead
        t.wedgeBeyondMeters = 5.f;
        int inside = 0;
        for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
            for (int a = 0; a < 360; a += 15) {
                for (const Vec2 start : {Vec2{-0.5f, 2.f}, Vec2{0.5f, -2.f}, Vec2{3.f, 2.5f}}) {
                    CAPTURE(order == PartnerOrder::StayClose);
                    CAPTURE(a);
                    CAPTURE(start.x);
                    Pit p(order, t);
                    p.camAt(static_cast<float>(a), 1.f);
                    p.in.station.arenaCentre = {0.5f, 0.f};
                    p.in.station.arenaRadius = 1.7f;
                    p.in.pos = start;
                    const PartnerIntent i = p.step();
                    if (i.act != PartnerAct::MoveTo) { // within the tolerance of a goal out of reach: it waits
                        CHECK(i.why == PartnerWhy::Approach);
                        continue;
                    }
                    const Vec2 goal = i.point;
                    CHECK(len(minus(goal, p.in.station.arenaCentre)) <= 1.7f + 1e-3f); // the arena first
                    CHECK(len(minus(goal, p.in.leadPos)) >= 1.6f - 1e-3f);             // then personal space
                    if (roomOnGrid(p, 1.7f, 60.f, 5.f, 0.5f)) {
                        CHECK_FALSE(inWedge(goal, p.cam(), p.in.leadPos, 0.49f, 60.f, 5.f)); // then the wedge
                    } else {
                        ++inside;
                        if (a == 180) CHECK(inWedge(goal, p.cam(), p.in.leadPos, 0.f, 60.f, 5.f));
                    }
                }
            }
        }
        MESSAGE("goals personal space kept in the wedge: " << inside);
        CHECK(inside >= 6); // the camera behind (and near it): the collision happened
    }
    SUBCASE("a wall behind the station: the goal stays in the arena, off the arc") {
        for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
            CAPTURE(order == PartnerOrder::StayClose);
            Pit p(order);
            p.in.station.arenaRadius = 3.f; // Spread's arc (the far side, x >= 3.2) is all past it
            p.in.pos = {-1.f, 2.5f};
            const PartnerIntent i = p.step();
            REQUIRE(i.act == PartnerAct::MoveTo);
            CHECK(len(i.point) <= 3.f + 1e-3f);
            checkGoal(p, i.point);
            if (order == PartnerOrder::Spread) CHECK(angleAt(p.e[0].pos, p.in.leadPos, i.point) < 120.f);
            // Walking it, it stations (or falls back) and strings start; it never leaves the arena.
            int starts = 0;
            walk(p, 8.f, [](float) {},
                 [&](float, const PartnerIntent& j) {
                     if (j.act == PartnerAct::MoveTo) CHECK(len(j.point) <= 3.f + 1e-3f);
                     starts += j.act == PartnerAct::Attack && j.newString ? 1 : 0;
                 });
            CHECK(len(p.in.pos) <= 3.f + 1e-3f);
            CHECK(starts >= 1);
        }
    }
    SUBCASE("a target beside the lead: no station is in the arc and outside personal space; personal space wins") {
        for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
            CAPTURE(order == PartnerOrder::StayClose);
            Pit p(order);
            p.e[0].pos = {0.f, 0.9f}; // its ring (1.2 m) is within 1.5 m of the lead from 0 to 90 degrees
            p.e[0].radius = 0.3f;
            p.in.pos = {2.f, 2.5f};
            const PartnerIntent i = p.step();
            REQUIRE(i.act == PartnerAct::MoveTo);
            checkGoal(p, i.point); // outside personal space (and the wedge, which has room here)
            // Walking it, strings start (from a station off the arc, or the fallback) and every goal keeps the rules.
            int starts = 0;
            walk(p, 8.f, [](float) {},
                 [&](float, const PartnerIntent& j) {
                     if (j.act == PartnerAct::MoveTo) checkGoal(p, j.point);
                     starts += j.act == PartnerAct::Attack && j.newString ? 1 : 0;
                 });
            CHECK(starts >= 1);
        }
    }
}

TEST_CASE("partner station: Spread keeps a target 1.5 s; a fall, or leaving reach once reached, frees it at once") {
    const double dt = 1.0 / 60.0;
    // Two enemies; once the first is chosen the lead attacks it, so the second (free) is preferred.
    const auto setup = [](Pit& p, Vec2 pos) {
        p.n = 2;
        p.e[1].pos = {1.f, 5.f};
        p.e[1].radius = 0.5f;
        p.in.pos = pos;
        p.in.leadTarget = -1;
    };
    SUBCASE("held 1.5 s") {
        Pit p(PartnerOrder::Spread);
        setup(p, {-3.f, -3.f}); // out of reach of both, still (no strings)
        p.step(dt);
        REQUIRE(p.b.target() == 0);
        p.e[0].leadAttacking = true;
        double switched = -1.0;
        for (int k = 1; k < 200 && switched < 0.0; ++k) {
            p.step(dt);
            if (p.b.target() == 1) switched = k * dt;
        }
        CHECK(switched >= 1.5 - 1e-6);
        CHECK(switched <= 1.5 + 2 * dt);
    }
    SUBCASE("a fall frees it at once") {
        Pit p(PartnerOrder::Spread);
        setup(p, {-3.f, -3.f});
        p.step(dt);
        p.e[0].leadAttacking = true;
        for (int k = 0; k < 30; ++k) p.step(dt);
        REQUIRE(p.b.target() == 0);
        p.e[0].state = V::State::Down;
        p.step(dt);
        CHECK(p.b.target() == 1);
    }
    SUBCASE("leaving reach once reached frees it at once; never reached, it is held") {
        for (const bool reached : {true, false}) {
            CAPTURE(reached);
            Pit p(PartnerOrder::Spread);
            setup(p, reached ? Vec2{2.5f, 1.9f} : Vec2{-3.f, -3.f}); // (2.5, 1.9): in reach, off the arc (90 degrees)
            p.step(dt);
            REQUIRE(p.b.target() == 0);
            p.e[0].leadAttacking = true;
            for (int k = 0; k < 20; ++k) p.step(dt);
            REQUIRE(p.b.target() == 0);
            p.e[0].pos = {9.f, -4.f}; // far: out of reach
            p.step(dt);
            CHECK(p.b.target() == (reached ? 1 : 0));
        }
    }
    SUBCASE("the hold is in save, load and the hash") {
        Pit p(PartnerOrder::Spread);
        setup(p, {-3.f, -3.f});
        for (int k = 0; k < 30; ++k) p.step(dt);
        const PartnerBrainSave sv = p.b.save();
        CHECK(sv.targetFor == doctest::Approx(29 * dt));
        CHECK_FALSE(sv.targetReached);
        PartnerBrain c;
        c.load(sv);
        CHECK(c.hash() == p.b.hash());
    }
}

TEST_CASE("partner station: a new target keeps the side while its station there is legal; an order change re-picks at once") {
    const double dt = 1.0 / 60.0;
    const auto start = [](Pit& p) {
        p.n = 2;
        p.e[1].pos = {2.6f, 0.4f};
        p.e[1].radius = 0.5f;
        p.in.pos = {1.5f, 3.5f};
        for (int k = 0; k < 30; ++k) p.step(1.0 / 60.0);
    };
    SUBCASE("kept: the held side's station for the new target is in its arc") {
        Pit p(PartnerOrder::StayClose);
        start(p);
        const PartnerBrainSave before = p.b.save();
        REQUIRE(before.side != 0);
        p.in.pos = {1.5f, -3.5f}; // now nearer the other side: a re-pick would take it
        p.in.leadTarget = 1;
        const PartnerIntent i = p.step(dt);
        REQUIRE(i.enemy == 1);
        CHECK(p.b.save().side == before.side);
        CHECK(p.b.save().sideFor == doctest::Approx(before.sideFor + dt)); // its timer runs on
        CHECK(p.b.save().sideTarget == 1);
    }
    SUBCASE("re-picked: an arena that leaves the held side no station for the new target") {
        Pit p(PartnerOrder::StayClose);
        start(p);
        const PartnerBrainSave before = p.b.save();
        const Vec2 held = p.step(dt).point;
        REQUIRE(held.y > 0.f);
        p.in.station.arenaCentre = {2.6f, -4.f}; // reaches y = 0.1 at most: the +y stations are past it
        p.in.station.arenaRadius = 4.1f;
        p.in.leadTarget = 1;
        const PartnerIntent i = p.step(dt);
        CHECK(p.b.save().side == -before.side);
        CHECK(p.b.save().sideFor == 0.0);
        CHECK(i.point.y < 0.1f);
    }
    SUBCASE("an order change re-picks at once, inside the hold") {
        Pit p(PartnerOrder::StayClose);
        start(p);
        REQUIRE(p.b.save().sideFor < 2.0);
        p.in.order = PartnerOrder::Spread;
        p.step(dt);
        CHECK(p.b.save().sideFor == 0.0);
        CHECK(p.b.save().sideKey == 2);
    }
}

TEST_CASE("partner station: rolesSwapped with stationing on: the first intent is a MoveTo to the station a fresh brain picks") {
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        CAPTURE(order == PartnerOrder::StayClose);
        Pit p(order);
        p.in.pos = {-2.5f, 1.5f};
        PartnerIntent i = p.step();
        REQUIRE(i.act == PartnerAct::MoveTo);
        p.in.pos = i.point; // on its station: a string starts
        i = p.step();
        REQUIRE(i.why == PartnerWhy::Start);
        p.in.doing = PartnerDoing::Attacking;
        p.step();
        REQUIRE(p.b.stringing());
        // The bodies change places; the camera follows the new lead.
        p.b.rolesSwapped();
        std::swap(p.in.pos, p.in.leadPos);
        p.in.doing = PartnerDoing::Idle;
        p.camAt(180.f);
        Pit fresh(order);
        fresh.in = p.in;
        const PartnerIntent f = fresh.step();
        const PartnerIntent g = p.step();
        CHECK(g.stationed);
        CHECK(g.act == f.act);
        CHECK(g.why == f.why);
        CHECK(g.point.x == doctest::Approx(f.point.x));
        CHECK(g.point.y == doctest::Approx(f.point.y));
        CHECK(g.act == PartnerAct::MoveTo);
        checkGoal(p, g.point);
    }
}
