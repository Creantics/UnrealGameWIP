// PartnerBrain rules 13-16 (the play camera given): frustum stationing, the engage bound, variety, the StayClose
// target without a lock; and the idle reasons with their seconds, which run with or without a camera.
#include "ftcore/PartnerBrain.h"
#include "ftcore/StateHash.h"
#include <doctest/doctest.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

using namespace ftcore;

namespace {

using V = PartnerEnemyView;
constexpr float kRad = 3.14159265358979f / 180.f;

float len(Vec2 a) { return std::hypot(a.x, a.y); }
Vec2 minus(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }

// A scripted 20 s fight: a lead at the origin facing +x, attacking 1 s on, 1 s off; three enemies, the first
// circling slowly 2.5 m ahead, the second winding up a blow at the partner every 5 s; a host that walks the partner
// to its goal at 4 m/s and runs every blow it starts for its length. The camera 4.5 m behind the lead.
struct Script {
    PartnerBrain b;
    PartnerInput in;
    std::array<V, kMaxPartnerEnemies> e{};
    std::size_t n = 3;
    double t = 0.0;
    double blowLeft = 0.0;
    StateHash out; // the intents, every field today's brain had
    bool windups = true;     // enemy 1's windups at the partner
    bool leadOnE0 = false;   // the lead's attacks land on enemy 0 (its leadAttacking)
    float orbit = 0.f;       // degrees a second the camera orbits the lead (0: fixed behind it)
    bool camera = false;     // the play camera given
    PartnerCameraView view{}; // the play camera, given or not (on-screen time is measured with it)
    // What it did, step by step.
    double firstAct = -1.0, onScreen = 0.0, maxEngage = 0.0, elapsed = 0.0;
    int goalChanges = 0, starts = 0, guards = 0, moves = 0, fallbacks = 0, framedSteps = 0, offFrameGoals = 0;
    Vec2 lastGoal{1e9f, 1e9f};
    bool wasGuard = false;
    double run0 = 0.0, longestIdle = 0.0; // the longest stretch with no action, after the first
    int keptOut = 0;                      // goals inside the lead's wedge or the target's keep-out
    std::vector<std::pair<double, int>> kinds; // action starts: 0 a string, 1 a guard, 2 a reposition

    Script(PartnerOrder order, bool station, bool budgetFree, std::uint32_t seed, PartnerTuning tu = {}) {
        if (budgetFree) {
            tu.budgetShare = 1.f;
            tu.budgetAllowanceSeconds = 10.f;
        }
        REQUIRE(b.begin(tu, seed));
        in.order = order;
        in.leadPos = {0.f, 0.f};
        in.leadTarget = 0;
        in.pos = {-1.5f, 2.5f};
        e[0].pos = {2.5f, 0.f};
        e[1].pos = {1.f, 3.f};
        e[2].pos = {-1.f, -3.f};
        in.station.on = station;
        in.station.leadFacing = {1.f, 0.f};
        in.station.cameraPos = {-4.5f, 0.f};
        in.station.cameraForward = {1.f, 0.f};
        in.station.arenaCentre = {0.f, 0.f};
        in.station.arenaRadius = 12.f;
        aim(180.f, false);
    }
    void useCamera() {
        camera = true;
        aim(180.f, true);
    }
    // The camera 4.5 m from the lead at `deg` round it, 2 m up, pitched 12 degrees down at it; a 90 degree
    // horizontal field of view at 16:9.
    void aim(float deg, bool feed) {
        const float a = deg * kRad, p = 12.f * kRad;
        const Vec2 c{4.5f * std::cos(a), 4.5f * std::sin(a)};
        const Vec2 f{-std::cos(a), -std::sin(a)};
        if (feed) {
            in.station.cameraPos = c;
            in.station.cameraForward = f;
        }
        PartnerCameraView& v = view;
        v.given = true;
        v.pos = {c.x, c.y, 2.f};
        v.forward = {f.x * std::cos(p), f.y * std::cos(p), -std::sin(p)};
        v.up = {f.x * std::sin(p), f.y * std::sin(p), std::cos(p)};
        v.aspect = 16.f / 9.f;
        v.verticalFovDegrees = 2.f * std::atan(1.f / v.aspect) / kRad;
        in.station.camera = camera ? view : PartnerCameraView{};
    }
    void stepOnce(double dt) {
        const double phase = std::fmod(t, 2.0);
        in.leadAttacking = phase < 1.0;
        const float a = static_cast<float>(t) * 9.f * kRad; // 9 degrees a second round the lead
        e[0].pos = {2.5f * std::cos(a), 2.5f * std::sin(a)};
        const double w = std::fmod(t, 5.0);
        if (orbit != 0.f) aim(180.f + orbit * static_cast<float>(t), true);
        e[0].leadAttacking = leadOnE0 && in.leadAttacking;
        if (windups && t > 1.0 && w < 0.6) {
            e[1].state = V::State::WindingUp;
            e[1].aim = V::Aim::Partner;
            e[1].windupLeft = static_cast<float>(0.6 - w);
        } else {
            e[1].state = V::State::Idle;
            e[1].aim = V::Aim::None;
            e[1].windupLeft = 0.f;
        }
        in.doing = blowLeft > 0.0 ? PartnerDoing::Attacking : PartnerDoing::Idle;
        in.enemies = std::span<const V>(e.data(), n);
        const PartnerIntent& i = b.step(dt, in);
        out.add(i.act).add(i.why).add(i.point.x).add(i.point.y).add(i.enemy).add(i.blows).add(i.newString);
        out.add(i.blowStart).add(i.blowIndex).add(i.partnerBlow).add(i.breakValue).add(i.extendsStagger);
        out.add(i.punish).add(i.rise).add(i.riseHealth).add(i.stationed);
        if (i.idle == PartnerIdle::None && firstAct < 0.0) firstAct = t;
        if (i.why == PartnerWhy::Start) {
            ++starts;
            kinds.push_back({t, 0});
        }
        if (i.act == PartnerAct::Guard && !wasGuard) {
            ++guards;
            kinds.push_back({t, 1});
        }
        wasGuard = i.act == PartnerAct::Guard;
        if (i.why == PartnerWhy::Reposition) {
            ++moves;
            kinds.push_back({t, 2});
        }
        if (i.idle == PartnerIdle::None) run0 = 0.0;
        else if (firstAct >= 0.0) longestIdle = std::max(longestIdle, run0 += dt);
        if (i.frameFallback) ++fallbacks;
        if (i.framed) ++framedSteps;
        if (partnerInFrame(view, 0.f, in.pos)) onScreen += dt;
        if (i.act == PartnerAct::MoveTo || i.act == PartnerAct::StepBack) {
            if (len(minus(i.point, lastGoal)) > 0.5f) ++goalChanges;
            lastGoal = i.point;
            if (i.framed && !i.frameFallback && !partnerInFrame(view, 0.1f, i.point)) ++offFrameGoals;
            if (i.framed && !i.frameFallback && inKeepOut(i.point, i.enemy)) ++keptOut;
        }
        maxEngage = std::max(maxEngage, b.save().engageFor);
        elapsed += dt;
        if (i.act == PartnerAct::Attack && i.blowStart) blowLeft = 0.65;
        blowLeft = std::max(0.0, blowLeft - dt);
        if (blowLeft <= 0.0 && (i.act == PartnerAct::MoveTo || i.act == PartnerAct::StepBack)) {
            const Vec2 d = minus(i.point, in.pos);
            const float far = len(d), most = static_cast<float>(4.0 * dt);
            in.pos = far > most ? Vec2{in.pos.x + d.x * most / far, in.pos.y + d.y * most / far} : i.point;
        }
        t += dt;
    }
    // Rule 9's wedge round the camera-to-lead line, or the target's silhouette keep-out (no clearance).
    bool inKeepOut(Vec2 q, int enemy) const {
        const Vec2 cam = in.station.cameraPos;
        const auto inside = [&](Vec2 to, float sinH, float length) {
            const Vec2 ax = minus(to, cam);
            const float l = len(ax);
            const Vec2 u{ax.x / l, ax.y / l}, d = minus(q, cam);
            const float along = u.x * d.x + u.y * d.y, across = std::fabs(u.x * d.y - u.y * d.x);
            return along > 0.f && along < length && across * std::sqrt(1.f - sinH * sinH) - along * sinH < 0.f;
        };
        const PartnerTuning d{};
        if (inside(in.leadPos, std::sin(d.wedgeHalfDegrees * kRad), len(minus(in.leadPos, cam)) + d.wedgeBeyondMeters))
            return true;
        if (enemy < 0 || static_cast<std::size_t>(enemy) >= n) return false;
        const V& v = e[static_cast<std::size_t>(enemy)];
        const float far = len(minus(v.pos, cam));
        return inside(v.pos, v.radius / far, far + v.radius + d.wedgeBeyondMeters);
    }
    void run(double seconds, double dt) {
        const int steps = static_cast<int>(std::lround(seconds / dt));
        for (int k = 0; k < steps; ++k) stepOnce(dt);
    }
};

struct Baseline {
    std::uint64_t intents;
    std::uint64_t state;
};

// cam: 0 none, 1 a whole camera (stationing off, so unused), 2 a camera that is not whole (its aspect).
Baseline baseline(PartnerOrder order, bool station, bool budgetFree, std::uint32_t seed, double dt, int cam = 0) {
    Script s(order, station, budgetFree, seed);
    if (cam == 1) s.in.station.camera = s.view;
    if (cam == 2) {
        s.in.station.camera = s.view;
        s.in.station.camera.aspect = 0.1f;
    }
    s.run(20.0, dt);
    return {s.out.value, s.b.hash()};
}

} // namespace

TEST_CASE("partner frame: baseline (report)") {
    int k = 0;
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread})
        for (const bool station : {false, true})
            for (const bool free : {false, true})
                for (const double dt : {1.0 / 60.0, 0.25}) {
                    const Baseline b = baseline(order, station, free, 5, dt);
                    MESSAGE("row " << k++ << " intents 0x" << std::hex << b.intents << " state 0x" << b.state);
                }
}

namespace {
const std::array<std::string, kPartnerIdleCount> kIdleNames = {"none",      "noBudget", "noTarget", "moving",
                                                               "staggered", "called",   "pause",    "other"};

void report(const std::string& name, Script& s) {
    MESSAGE(name << ": first act " << s.firstAct << " s, on screen " << s.onScreen / s.elapsed << ", starts " << s.starts
                 << ", guards " << s.guards << ", moves " << s.moves << ", goal changes " << s.goalChanges
                 << ", fallback steps " << s.fallbacks << ", off-frame goals " << s.offFrameGoals << ", kept-out goals "
                 << s.keptOut << ", engage clock max " << s.maxEngage << ", longest idle " << s.longestIdle);
    std::string shares;
    for (std::size_t k = 0; k < kPartnerIdleCount; ++k)
        shares += " " + kIdleNames[k] + " " +
                  std::to_string(s.b.idleSeconds(static_cast<PartnerIdle>(k)) / s.elapsed).substr(0, 5);
    MESSAGE("    shares:" << shares);
}

// The pinned output of the brain before rules 13-16 (the baseline report's rows, Debug and Release alike).
constexpr std::array<Baseline, 16> kToday = {{
    {13445692613458923181ull, 13138403762025505184ull}, {5437146891790057014ull, 13311928752071444546ull},
    {1547416502585513044ull, 8417886831165652980ull},   {13015864882144990991ull, 17788129973728066943ull},
    {17985158057644685914ull, 2345904445577399390ull},  {12225964072062923046ull, 11606700117422463235ull},
    {13892082800472484288ull, 424531398021521978ull},   {14531939777227021406ull, 15970527748931333887ull},
    {8142716690069406524ull, 17237324410577967092ull},  {13028351920319554605ull, 7145685466416907862ull},
    {6182294181758894789ull, 4736492203090409686ull},   {10353265618600513183ull, 3897729725821978421ull},
    {14330626647911497627ull, 9193964610844952317ull},  {4045883630965868929ull, 7889206512943461700ull},
    {5186579624760122492ull, 11231368162861533754ull},  {8554039395834609380ull, 17039174856404500635ull},
}};

// A level camera at height z looking along `yaw` degrees; 60 degrees vertical, 3:2.
PartnerCameraView levelCamera(float z, float yaw) {
    PartnerCameraView v;
    v.given = true;
    v.pos = {0.f, 0.f, z};
    v.forward = {std::cos(yaw * kRad), std::sin(yaw * kRad), 0.f};
    v.up = {0.f, 0.f, 1.f};
    v.verticalFovDegrees = 60.f;
    v.aspect = 1.5f;
    return v;
}
Vec2 along(float yaw, float ahead, float side) { // ahead along yaw, side to its left
    const float c = std::cos(yaw * kRad), s = std::sin(yaw * kRad);
    return {ahead * c - side * s, ahead * s + side * c};
}

int distinctKinds(const Script& s, double from, double to) {
    bool seen[3] = {false, false, false};
    for (const auto& k : s.kinds)
        if (k.first >= from && k.first < to) seen[k.second] = true;
    return int{seen[0]} + int{seen[1]} + int{seen[2]};
}
} // namespace

TEST_CASE("partner frame: no camera, or one not whole, or one with stationing off, is today's brain to the bit") {
    std::size_t k = 0;
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread})
        for (const bool station : {false, true})
            for (const bool free : {false, true})
                for (const double dt : {1.0 / 60.0, 0.25}) {
                    CAPTURE(k);
                    for (const int cam : {0, station ? 2 : 1}) {
                        CAPTURE(cam);
                        const Baseline b = baseline(order, station, free, 5, dt, cam);
                        CHECK(b.intents == kToday[k].intents);
                        CHECK(b.state == kToday[k].state);
                    }
                    ++k;
                }
}

TEST_CASE("partner frame: a station just outside each side of the shrunk frame is refused; the margin is kept") {
    // tan(30) = 0.57735 up, x1.5 across; the margin 0.1 leaves 0.8 of each half.
    const float tanV = std::tan(30.f * kRad), tanH = 1.5f * tanV, lim = 0.8f, d = 6.f;
    for (const float yaw : {0.f, 37.f, -140.f}) {
        CAPTURE(yaw);
        const PartnerCameraView mid = levelCamera(0.9f, yaw); // feet and head well inside: the sides rule
        const float edgeSide = lim * d * tanH;
        for (const float side : {1.f, -1.f}) { // left and right
            CHECK(partnerInFrame(mid, 0.1f, along(yaw, d, side * (edgeSide - 0.02f))));
            CHECK_FALSE(partnerInFrame(mid, 0.1f, along(yaw, d, side * (edgeSide + 0.02f))));
        }
        // Bottom: a camera 1.6 m up sees the feet last; top: one 0.2 m up sees the head (at 1.8) last.
        const float nearest = 1.6f / (lim * tanV);
        const PartnerCameraView high = levelCamera(1.6f, yaw), low = levelCamera(0.2f, yaw);
        CHECK(partnerInFrame(high, 0.1f, along(yaw, nearest * 1.01f, 0.f)));
        CHECK_FALSE(partnerInFrame(high, 0.1f, along(yaw, nearest * 0.99f, 0.f)));
        CHECK(partnerInFrame(low, 0.1f, along(yaw, nearest * 1.01f, 0.f)));
        CHECK_FALSE(partnerInFrame(low, 0.1f, along(yaw, nearest * 0.99f, 0.f)));
        // The margin: 0.9 of the half-width is in the whole frame and in a 0.04 margin's, not in a 0.1 margin's.
        const Vec2 wide = along(yaw, d, 0.9f * d * tanH);
        CHECK(partnerInFrame(mid, 0.f, wide));
        CHECK(partnerInFrame(mid, 0.04f, wide));
        CHECK_FALSE(partnerInFrame(mid, 0.1f, wide));
        // Behind the camera; a margin out of range.
        CHECK_FALSE(partnerInFrame(mid, 0.1f, along(yaw, -d, 0.f)));
        CHECK_FALSE(partnerInFrame(mid, 0.5f, along(yaw, d, 0.f)));
    }
    // A pitched camera: 2 m up, 20 degrees down; where its centre ray meets the ground is in; just short of the
    // point where the feet meet the shrunk frame's bottom edge is not.
    PartnerCameraView p = levelCamera(2.f, 0.f);
    const float pitch = 20.f * kRad;
    p.forward = {std::cos(pitch), 0.f, -std::sin(pitch)};
    p.up = {std::sin(pitch), 0.f, std::cos(pitch)};
    const float centre = 2.f / std::tan(pitch);
    CHECK(partnerInFrame(p, 0.1f, {centre, 0.f}));
    const float foot = 2.f / std::tan(pitch + std::atan(lim * tanV));
    CHECK(partnerInFrame(p, 0.1f, {foot + 0.05f, 0.f}));
    CHECK_FALSE(partnerInFrame(p, 0.1f, {foot - 0.05f, 0.f}));
    // Not given, or not whole: never in frame.
    PartnerCameraView bad = levelCamera(0.9f, 0.f);
    bad.given = false;
    CHECK_FALSE(partnerInFrame(bad, 0.1f, {d, 0.f}));
    bad = levelCamera(0.9f, 0.f);
    bad.forward = {2.f, 0.f, 0.f};
    CHECK_FALSE(partnerInFrame(bad, 0.1f, {d, 0.f}));
    bad = levelCamera(0.9f, 0.f);
    bad.up = bad.forward;
    CHECK_FALSE(partnerInFrame(bad, 0.1f, {d, 0.f}));
}

TEST_CASE("partner frame: every station it walks to is in the shrunk frame and outside both keep-outs") {
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread})
        for (const bool free : {false, true})
            for (const bool lock : {true, false})
                for (const float orbit : {0.f, 30.f})
                    for (const double dt : {1.0 / 60.0, 0.25}) {
                        CAPTURE(static_cast<int>(order));
                        CAPTURE(free);
                        CAPTURE(lock);
                        CAPTURE(orbit);
                        CAPTURE(dt);
                        Script s(order, true, free, 5);
                        if (!lock) {
                            s.in.leadTarget = -1;
                            s.leadOnE0 = true;
                        }
                        s.useCamera();
                        s.orbit = orbit;
                        s.run(20.0, dt);
                        CHECK(s.framedSteps > 0);
                        CHECK(s.offFrameGoals == 0);
                        CHECK(s.keptOut == 0);
                    }
}

TEST_CASE("partner frame: with no valid station it keeps the last for frustumGiveUpSeconds, then falls back and says so") {
    for (const double dt : {1.0 / 60.0, 0.25}) {
        CAPTURE(dt);
        // No enemies: rule 11's ring round the lead, chosen every step.
        Script s(PartnerOrder::Spread, true, false, 5);
        s.n = 0;
        s.useCamera();
        s.run(5.0, dt);
        CHECK(s.fallbacks == 0);
        CHECK(s.b.fallbackSeconds() == 0.0);
        // The camera turns away from the fight: nothing it could stand on is in frame.
        s.view.forward = {-s.view.forward.x, -s.view.forward.y, s.view.forward.z};
        s.view.up = {-s.view.up.x, -s.view.up.y, s.view.up.z};
        s.in.station.camera = s.view;
        double first = -1.0;
        const double turned = s.t;
        for (int k = 0; k < static_cast<int>(std::lround(6.0 / dt)); ++k) {
            const int before = s.fallbacks;
            s.stepOnce(dt);
            if (s.fallbacks > before && first < 0.0) first = s.t - dt - turned;
        }
        CHECK(first >= 3.0 - dt - 1e-6);
        CHECK(first <= 3.0 + dt + 1e-6);
        CHECK(s.b.fallbackSeconds() == doctest::Approx(6.0 - first).epsilon(0.02));
        // A fresh brain that never held one falls back from its first step, and says so.
        Script f(PartnerOrder::Spread, true, false, 5);
        f.n = 0;
        f.useCamera();
        f.view.forward = s.view.forward;
        f.view.up = s.view.up;
        f.in.station.camera = f.view;
        f.stepOnce(dt);
        CHECK(f.fallbacks == 1);
    }
}

TEST_CASE("partner frame: StayClose by a fighting lead with no lock: today idles, framed it acts within 2 s") {
    for (const double dt : {1.0 / 60.0, 0.25})
        for (const bool free : {false, true}) {
            CAPTURE(dt);
            CAPTURE(free);
            Script today(PartnerOrder::StayClose, true, free, 5), framed(PartnerOrder::StayClose, true, free, 5);
            for (Script* s : {&today, &framed}) {
                s->in.leadTarget = -1;
                s->leadOnE0 = true;
            }
            framed.useCamera();
            today.run(20.0, dt);
            framed.run(20.0, dt);
            CHECK(today.starts == 0);
            CHECK(today.b.idleSeconds(PartnerIdle::NoTarget) >= 0.8 * today.elapsed);
            CHECK(framed.firstAct >= 0.0);
            CHECK(framed.firstAct <= 2.0);
            CHECK(framed.starts >= 5);
            CHECK(framed.b.idleSeconds(PartnerIdle::NoTarget) <= 0.1 * framed.elapsed);
        }
}

TEST_CASE("partner frame: in reach with its next blow in the budget it acts within engageSeconds") {
    PartnerTuning slow; // a pause that today would stand through
    slow.pauseMin = 5.f;
    slow.pauseMax = 6.f;
    for (const double dt : {1.0 / 60.0, 0.25})
        for (const bool free : {false, true})
            for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
                CAPTURE(dt);
                CAPTURE(free);
                CAPTURE(static_cast<int>(order));
                Script today(order, true, free, 5, slow), framed(order, true, free, 5, slow);
                framed.useCamera();
                today.run(20.0, dt);
                framed.run(20.0, dt);
                CHECK(today.longestIdle >= 4.5);
                CHECK(framed.maxEngage <= static_cast<double>(PartnerTuning{}.engageSeconds) + dt + 1e-6);
                CHECK(framed.longestIdle < today.longestIdle);
                CHECK(framed.starts > today.starts);
            }
}

TEST_CASE("partner frame: idle seconds sum to the time stepped, each to its reason") {
    for (const bool cam : {false, true})
        for (const bool free : {false, true})
            for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
                Script s(order, true, free, 5);
                if (cam) s.useCamera();
                s.run(20.0, 1.0 / 60.0);
                double sum = 0.0;
                for (std::size_t k = 0; k < kPartnerIdleCount; ++k) sum += s.b.idleSeconds(static_cast<PartnerIdle>(k));
                CHECK(sum == doctest::Approx(20.0).epsilon(1e-9));
                CHECK(s.b.idleSeconds(PartnerIdle::None) > 0.0);
            }
    // Called, staggered (hurt, and down), no target.
    PartnerBrain b;
    REQUIRE(b.begin(PartnerTuning{}, 1));
    PartnerInput in;
    in.canAct = false;
    in.called = true;
    CHECK(b.step(0.25, in).idle == PartnerIdle::Called);
    CHECK(b.step(0.25, in).idle == PartnerIdle::Called);
    in.called = false;
    in.doing = PartnerDoing::Hurt;
    CHECK(b.step(0.25, in).idle == PartnerIdle::Staggered);
    in.canAct = true;
    in.doing = PartnerDoing::Idle;
    in.health = 0.f;
    CHECK(b.step(0.25, in).idle == PartnerIdle::Staggered);
    in.health = 1.f;
    for (int k = 0; k < 4; ++k) CHECK(b.step(0.25, in).idle == PartnerIdle::NoTarget);
    CHECK(b.idleSeconds(PartnerIdle::Called) == doctest::Approx(0.5));
    CHECK(b.idleSeconds(PartnerIdle::Staggered) == doctest::Approx(0.5));
    CHECK(b.idleSeconds(PartnerIdle::NoTarget) == doctest::Approx(1.0));
    CHECK(b.idleSeconds(static_cast<PartnerIdle>(kPartnerIdleCount)) == 0.0);
}

TEST_CASE("partner frame: two kinds of action in every 20 s with the budget free") {
    for (const bool windups : {false, true})
        for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread})
            for (const double dt : {1.0 / 60.0, 0.25}) {
                CAPTURE(windups);
                CAPTURE(static_cast<int>(order));
                CAPTURE(dt);
                Script today(order, true, true, 5), framed(order, true, true, 5);
                today.windups = framed.windups = windups;
                framed.useCamera();
                today.run(60.0, dt);
                framed.run(60.0, dt);
                int worst = 3, worstToday = 3;
                for (double a = 0.0; a + 20.0 <= 60.0 + 1e-9; a += 0.5) {
                    worst = std::min(worst, distinctKinds(framed, a, a + 20.0));
                    worstToday = std::min(worstToday, distinctKinds(today, a, a + 20.0));
                }
                CHECK(worst >= 2);
                if (!windups) CHECK(worstToday == 1); // strings only
            }
}

TEST_CASE("partner frame: determinism, and a save and load mid-fight carries on to the bit") {
    for (const double dt : {1.0 / 60.0, 0.25}) {
        CAPTURE(dt);
        Script a(PartnerOrder::Spread, true, false, 9), b(PartnerOrder::Spread, true, false, 9);
        a.useCamera();
        b.useCamera();
        a.orbit = b.orbit = 30.f;
        a.run(10.0, dt);
        b.run(10.0, dt);
        CHECK(a.out.value == b.out.value);
        CHECK(a.b.hash() == b.b.hash());
        CHECK(a.b.framed());
        Script c = a; // the same script, its brain loaded from a's save
        c.b = PartnerBrain{};
        c.b.load(a.b.save());
        CHECK(c.b.hash() == a.b.hash());
        a.run(10.0, dt);
        c.run(10.0, dt);
        CHECK(c.out.value == a.out.value);
        CHECK(c.b.hash() == a.b.hash());
        for (std::size_t k = 0; k < kPartnerIdleCount; ++k)
            CHECK(c.b.idleSeconds(static_cast<PartnerIdle>(k)) == a.b.idleSeconds(static_cast<PartnerIdle>(k)));
    }
}

TEST_CASE("partner frame: today against framed (report)") {
    for (const bool free : {false, true})
        for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread})
            for (const bool lock : {true, false})
                for (const bool cam : {false, true}) {
                    Script s(order, true, free, 5);
                    if (!lock) {
                        s.in.leadTarget = -1;
                        s.leadOnE0 = true;
                    }
                    if (cam) s.useCamera();
                    s.run(20.0, 1.0 / 60.0);
                    report(std::string(free ? "free " : "governed ") + (order == PartnerOrder::Spread ? "spread " : "close ") +
                               (lock ? "lock " : "nolock ") + (cam ? "framed" : "today"),
                           s);
                }
    for (const bool cam : {false, true}) {
        Script o(PartnerOrder::Spread, true, false, 5);
        if (cam) o.useCamera();
        o.orbit = 30.f;
        o.run(20.0, 1.0 / 60.0);
        report(std::string("orbit 30/s spread governed ") + (cam ? "framed" : "today"), o);
    }
}
