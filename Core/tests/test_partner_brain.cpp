#include "ftcore/PartnerBrain.h"
#include "AllocCounter.h"
#include "TestAssets.h"
#include <doctest/doctest.h>
#include <iterator>
#include <algorithm>
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

struct Scene {
    PartnerBrain b;
    PartnerInput in;
    std::array<V, kMaxPartnerEnemies> e{};
    std::size_t n = 1;
    // The lead attacks every step; the budget (rule 2) is out of the way unless `budget` asks for the ruled one.
    explicit Scene(std::uint32_t seed = 7, PartnerTuning t = PartnerTuning{}, bool budget = false) {
        if (!budget) {
            t.budgetShare = 1.f;
            t.budgetAllowanceSeconds = 10.f;
        }
        REQUIRE(b.begin(t, seed));
        in.leadAttacking = true;
        in.pos = {0.f, 2.f};
        in.leadPos = {0.f, -2.f};
        e[0].pos = {0.f, 0.f};
        e[0].radius = 0.5f;
    }
    const PartnerIntent& step(double dt = 1.0 / 60.0) {
        in.enemies = std::span<const V>(e.data(), n);
        return b.step(dt, in);
    }
};

// The default fighter's light chain (Data/Battle/arena.json: startup + active + recovery), both sides' strings.
constexpr double kLight[3] = {0.65, 0.63, 0.91};
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

bool same(const PartnerIntent& x, const PartnerIntent& y) {
    return x.act == y.act && x.why == y.why && x.point.x == y.point.x && x.point.y == y.point.y && x.enemy == y.enemy &&
           x.blows == y.blows && x.newString == y.newString && x.partnerBlow == y.partnerBlow &&
           x.breakValue == y.breakValue && x.extendsStagger == y.extendsStagger && x.punish == y.punish &&
           x.rise == y.rise && x.riseHealth == y.riseHealth && x.blowStart == y.blowStart && x.blowIndex == y.blowIndex;
}
constexpr std::size_t kSame = static_cast<std::size_t>(-1);
// The first step where two whole intent streams differ (kSame when every step matches).
std::size_t firstDiff(const std::vector<PartnerIntent>& a, const std::vector<PartnerIntent>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        if (!same(a[i], b[i])) return i;
    return a.size() == b.size() ? kSame : n;
}
// A scripted host: it starts a blow on each step the brain says (blowStart) and runs it for its length (0.9 s unless
// set), and a blow once started always runs out whatever the brain says after it (no host can cancel one); only
// being hurt cuts it short. It guards on Guard when no blow runs, moves at 4.5 m/s, and a blow aimed at it that lands
// unguarded hurts it for 0.5 s (no act).
struct Host {
    std::array<float, kMaxPartnerBlows> lengths{0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
    double blowLeft = 0.0;
    double hurtLeft = 0.0;
    bool guarding = false;
    double attackTime = 0.0;
    int strings = 0;
    int hits = 0;
    int guards = 0;
    int overlaps = 0; // a blow started while one still ran (never)
    std::vector<int> blows;
    std::vector<double> pauses;
    bool running() const { return blowLeft > 1e-6; }
    void feed(PartnerInput& in) const {
        in.blowSeconds = lengths;
        in.canAct = hurtLeft <= 0.0;
        in.doing = hurtLeft > 0.0 ? PartnerDoing::Hurt
                   : running()     ? PartnerDoing::Attacking
                   : guarding      ? PartnerDoing::Guarding
                                   : PartnerDoing::Idle;
    }
    void apply(const PartnerIntent& it, const PartnerBrain& b, PartnerInput& in, double dt) {
        guarding = it.act == PartnerAct::Guard && !running();
        if (it.act == PartnerAct::Attack && it.blowStart) {
            if (running()) ++overlaps;
            blowLeft = lengths[static_cast<std::size_t>(it.blowIndex)];
            if (it.newString) {
                ++strings;
                blows.push_back(b.lastBlows());
                pauses.push_back(b.lastPause());
            }
        }
        if (it.act == PartnerAct::MoveTo || it.act == PartnerAct::StepBack) {
            const float dx = it.point.x - in.pos.x, dy = it.point.y - in.pos.y;
            const float l = std::hypot(dx, dy);
            const float s = std::min(l, 4.5f * static_cast<float>(dt));
            if (l > 1e-5f) {
                in.pos.x += dx / l * s;
                in.pos.y += dy / l * s;
            }
        }
        if (blowLeft > 0.0) {
            attackTime += std::min(dt, blowLeft);
            blowLeft -= dt;
        }
        if (hurtLeft > 0.0) hurtLeft -= dt;
    }
    void land() { // a blow aimed at it lands
        if (guarding) {
            ++guards;
        } else {
            ++hits;
            hurtLeft = 0.5;
            blowLeft = 0.0;
        }
    }
};

// A tuning under which the budget never binds (a 5 s window and 10 s allowance): what the brain would do without it.
PartnerTuning unbound() {
    PartnerTuning t;
    t.budgetWindowSeconds = 5.f;
    t.budgetShare = 1.f;
    t.budgetAllowanceSeconds = 10.f;
    return t;
}

// A 60 s scripted fight against a boss (radius 1 at the origin); the lead (1.8 m from its centre, never moving) chains
// the default fighter's lights back to back (the chain wraps; chainGap is a window, not a rest), and the host says
// each step whether it is attacking. The dummy never winds up. With windups: 1.0 s
// every 5 s from 3 s, aimed at the partner on 3 of each 10 (under the fight's 30% ceiling) and at the lead otherwise;
// the lead guards from a windup aimed at it until 0.4 s after the blow, and starts its chain again after.
struct Fight {
    double leadAttack = 0.0;
    Host host;
    PartnerBrain brain;
    std::vector<int> guardDraws; // per windup aimed at the partner: 1 guard, 0 hit
    std::vector<PartnerIntent> intents; // every step's
    double period = 5.0;      // a windup every this many seconds
    bool partnerAims = true;  // 3 of each 10 at the partner (false: every one at the lead)
    bool leadFights = true;   // false: the lead never attacks
    bool staggered = false;   // the boss staggered throughout (no windups)
    double swapAt = -1.0;     // >= 0: the roles swap at this time (rolesSwapped, the bodies change places)
    std::size_t swapStep = 0; // the step it swapped on
    std::vector<double> partnerStep, leadStep; // each step's attack seconds
    void clear() {
        leadAttack = 0.0;
        host = Host{};
        guardDraws.clear();
        intents.clear();
        partnerStep.clear();
        leadStep.clear();
    }
    bool run(double dt, PartnerOrder order, std::uint32_t seed, bool windups = true, double seconds = 60.0,
             const PartnerTuning& tuning = PartnerTuning{}) {
        if (!brain.begin(tuning, seed)) return false;
        std::array<V, 1> boss{};
        boss[0].kind = V::Kind::Boss;
        boss[0].radius = 1.f;
        PartnerInput in;
        in.order = order;
        in.pos = {0.f, 3.5f};
        in.leadPos = {1.8f, 0.f};
        in.leadTarget = 0;
        in.enemies = std::span<const V>(boss.data(), 1);
        double leadGuard = 0.0, leadInto = 0.0;
        int leadLight = 0;
        const int steps = static_cast<int>(std::lround(seconds / dt));
        for (int k = 0; k < steps; ++k) {
            const double now = k * dt;
            // The boss: windup index w runs [3 + 5w, 4 + 5w).
            const double since = now - 3.0;
            const int w = since >= 0.0 ? static_cast<int>(std::floor(since / period + 1e-9)) : -1;
            const double into = w >= 0 ? since - period * w : -1.0;
            const bool atPartner = partnerAims && w >= 0 && (w % 10 == 2 || w % 10 == 5 || w % 10 == 9);
            const bool winding = windups && w >= 0 && into < 1.0 - 1e-9;
            boss[0].state = winding ? V::State::WindingUp : staggered ? V::State::Staggered : V::State::Idle;
            boss[0].windupLeft = winding ? static_cast<float>(1.0 - into) : 0.f;
            boss[0].aim = winding ? (atPartner ? V::Aim::Partner : V::Aim::Lead) : V::Aim::None;
            // The lead.
            if (winding && !atPartner) leadGuard = 1.4 - into;
            const bool leadOn = leadFights && !(leadGuard > 0.0);
            if (leadOn) {
                leadAttack += dt;
                leadInto += dt;
                if (leadInto >= kLight[leadLight]) {
                    leadInto -= kLight[leadLight];
                    leadLight = (leadLight + 1) % 3;
                }
            } else {
                leadGuard -= dt;
                leadLight = 0;
                leadInto = 0.0;
            }
            boss[0].leadAttacking = leadOn;
            in.leadAttacking = leadOn;
            if (swapAt >= 0.0 && swapStep == 0 && now + 1e-9 >= swapAt) {
                // The brain now drives the other body: idle, where the lead stood; the lead takes the old one.
                brain.rolesSwapped();
                std::swap(in.pos, in.leadPos);
                host.blowLeft = host.hurtLeft = 0.0;
                host.guarding = false;
                swapStep = static_cast<std::size_t>(k);
            }
            host.feed(in);
            const PartnerIntent& it = brain.step(dt, in);
            intents.push_back(it);
            const double was = host.attackTime;
            host.apply(it, brain, in, dt);
            partnerStep.push_back(host.attackTime - was);
            leadStep.push_back(leadOn ? dt : 0.0);
            // The blow lands on the windup's last step.
            if (winding && atPartner && into + dt >= 1.0 - 1e-9) {
                guardDraws.push_back(host.guarding ? 1 : 0);
                host.land();
            }
        }
        return true;
    }
};

// Rule 2's budget over every window of `w` steps (those running past the end too): the largest excess of the
// partner's attack time over share x the lead's plus allow (at most 0 when every window holds).
double worstExcess(const std::vector<double>& partner, const std::vector<double>& lead, std::size_t w,
                   double share = 0.6, double allow = 2.0) {
    double p = 0.0, l = 0.0, worst = -1e9;
    const std::size_t n = partner.size();
    for (std::size_t k = 0; k < n + w; ++k) {
        if (k < n) {
            p += partner[k];
            l += lead[k];
        }
        if (k >= w && k - w < n) {
            p -= partner[k - w];
            l -= lead[k - w];
        }
        worst = std::max(worst, p - share * l - allow);
    }
    return worst;
}

} // namespace

TEST_CASE("partner brain: partner_brain.json is the defaults; the importer keeps defaults for missing fields and refuses bad ones") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/partner_brain.json", text));
    PartnerTuning t;
    t.leashMeters = 9.f;
    std::string err;
    REQUIRE(importPartnerBrain(text, t, &err));
    const PartnerTuning d;
    CHECK(t.leashMeters == d.leashMeters);
    CHECK(t.returnMeters == d.returnMeters);
    CHECK(t.reachMeters == doctest::Approx(d.reachMeters));
    CHECK(t.standoffMeters == doctest::Approx(d.standoffMeters));
    CHECK(t.blowsMin == d.blowsMin);
    CHECK(t.blowsMax == d.blowsMax);
    CHECK(t.blowSeconds == doctest::Approx(d.blowSeconds));
    CHECK(t.pauseMin == doctest::Approx(d.pauseMin));
    CHECK(t.pauseMax == doctest::Approx(d.pauseMax));
    CHECK(t.guardWarning == doctest::Approx(d.guardWarning));
    CHECK(t.guardChance == doctest::Approx(d.guardChance));
    CHECK(t.guardHold == doctest::Approx(d.guardHold));
    CHECK(t.downSeconds == d.downSeconds);
    CHECK(t.riseHealth == doctest::Approx(d.riseHealth));
    CHECK(t.blowCapShare == doctest::Approx(d.blowCapShare));
    CHECK(t.overScale == doctest::Approx(d.overScale));
    CHECK(t.budgetWindowSeconds == d.budgetWindowSeconds);
    CHECK(t.budgetShare == doctest::Approx(d.budgetShare));
    CHECK(t.budgetAllowanceSeconds == doctest::Approx(d.budgetAllowanceSeconds));
    CHECK(d.budgetWindowSeconds == 20.f);
    CHECK(d.budgetShare == 0.6f);
    CHECK(d.budgetAllowanceSeconds == 2.f);
    CHECK(validPartnerTuning(d));

    REQUIRE(importPartnerBrain(R"({"version":1,"leashMeters":6,"blowsMax":4,"guardChance":1})", t, &err));
    CHECK(t.leashMeters == 6.f);
    CHECK(t.blowsMax == 4);
    CHECK(t.guardChance == 1.f);
    CHECK(t.pauseMin == doctest::Approx(d.pauseMin));

    for (const char* bad : {
             R"({"version":2})",
             R"({})",
             R"([])",
             R"({"version":1,"extra":1})",
             R"({"version":1,"leashMeters":0.5})",
             R"({"version":1,"leashMeters":11})",
             R"({"version":1,"reachMeters":5})",
             R"({"version":1,"blowsMin":0})",
             R"({"version":1,"blowsMax":7})",
             R"({"version":1,"blowsMax":2.5})",
             R"({"version":1,"blowSeconds":0})",
             R"({"version":1,"pauseMax":11})",
             R"({"version":1,"guardWarning":-0.1})",
             R"({"version":1,"guardChance":1.1})",
             R"({"version":1,"guardHold":3})",
             R"({"version":1,"downSeconds":0})",
             R"({"version":1,"riseHealth":0})",
             R"({"version":1,"blowCapShare":1.5})",
             R"({"version":1,"overScale":"0.5"})",
         }) {
        CAPTURE(bad);
        t.leashMeters = 9.f;
        CHECK_FALSE(importPartnerBrain(bad, t, &err));
        CHECK_FALSE(err.empty());
        CHECK(t.leashMeters == d.leashMeters);
    }
    // A broken cross-field pair: both its fields take their defaults, every other field is kept, a note says so.
    struct PairCase {
        const char* json;
        const char* note; // the start of the one note
    };
    const PairCase pairs[] = {
        {R"({"version":1,"returnMeters":5,"guardChance":0.9})", "returnMeters, leashMeters"},
        {R"({"version":1,"standoffMeters":2,"guardChance":0.9})", "standoffMeters, reachMeters"},
        {R"({"version":1,"blowsMin":4,"guardChance":0.9})", "blowsMin, blowsMax"},
        {R"({"version":1,"pauseMin":2.5,"guardChance":0.9})", "pauseMin, pauseMax"},
        {R"({"version":1,"leashMeters":2.5,"returnMeters":2,"guardChance":0.9})", "leashMeters, personalSpaceMeters"},
        {R"({"version":1,"stationGiveUpSeconds":8,"guardChance":0.9})", "stationGiveUpSeconds, stationAnySeconds"},
    };
    for (const PairCase& c : pairs) {
        CAPTURE(c.json);
        std::vector<std::string> notes;
        REQUIRE(importPartnerBrain(c.json, t, &err, &notes));
        REQUIRE(notes.size() == 1u);
        CHECK(notes[0].rfind(c.note, 0) == 0);
        CHECK(t.guardChance == 0.9f); // kept
        if (std::string(c.note).rfind("leash", 0) != 0) CHECK(t.leashMeters == d.leashMeters);
        if (std::string(c.note).rfind("leash", 0) != 0) CHECK(t.returnMeters == d.returnMeters);
        CHECK(t.standoffMeters == d.standoffMeters);
        CHECK(t.blowsMin == d.blowsMin);
        CHECK(t.pauseMin == d.pauseMin);
        CHECK(t.stationGiveUpSeconds == d.stationGiveUpSeconds);
        CHECK(validPartnerTuning(t));
    }
    {
        // A reset that breaks another pair: leash 4.5 is under personal space 4 + 1, so that pair goes back (leash 4),
        // which leaves returnMeters 4.2 over the leash: that pair goes back too.
        std::vector<std::string> notes;
        REQUIRE(importPartnerBrain(R"({"version":1,"leashMeters":4.5,"returnMeters":4.2,"personalSpaceMeters":4})", t,
                                   &err, &notes));
        CHECK(notes.size() == 2u);
        CHECK(t.leashMeters == d.leashMeters);
        CHECK(t.returnMeters == d.returnMeters);
        CHECK(t.personalSpaceMeters == d.personalSpaceMeters);
        CHECK(validPartnerTuning(t));
    }
    {
        std::vector<std::string> notes{"stale"};
        REQUIRE(importPartnerBrain(R"({"version":1,"leashMeters":6})", t, &err, &notes));
        CHECK(notes.empty()); // a clean file: no notes
    }
    // Bad tuning on begin: false, and the defaults run.
    PartnerBrain b;
    PartnerTuning wrong;
    wrong.pauseMax = 0.5f;
    CHECK_FALSE(b.begin(wrong, 1));
    // Before begin it waits.
    PartnerBrain idle;
    PartnerInput in;
    CHECK(idle.step(0.1, in).act == PartnerAct::Wait);
    CHECK(idle.step(0.1, in).why == PartnerWhy::NotBegun);
}

TEST_CASE("partner brain: rule 7, a partner blow's damage over 60% of the lead's light is halved") {
    CHECK(partnerBlowDamage(5.f, 12.f) == doctest::Approx(5.f));
    CHECK(partnerBlowDamage(7.2f, 12.f) == doctest::Approx(7.2f));
    CHECK(partnerBlowDamage(12.f, 12.f) == doctest::Approx(7.2f + 2.4f));
    CHECK(partnerBlowDamage(20.f, 10.f) == doctest::Approx(6.f + 7.f));
    PartnerTuning t;
    t.overScale = 0.f;
    CHECK(partnerBlowDamage(20.f, 10.f, t) == doctest::Approx(6.f));
}

TEST_CASE("partner brain: rule 1, Spread takes the nearest standing enemy the lead is not attacking") {
    Scene s;
    s.n = 3;
    s.in.order = PartnerOrder::Spread;
    s.in.pos = {0.f, 0.f};
    s.e[0].pos = {1.f, 0.f}; // nearest
    s.e[1].pos = {2.f, 0.f};
    s.e[2].pos = {0.f, 2.1f};
    s.e[0].leadAttacking = true;
    const PartnerIntent& a = s.step();
    CHECK(a.act == PartnerAct::Attack);
    CHECK(a.enemy == 1); // 1.5 m to its edge; enemy 2 is 1.6 m
    CHECK(s.b.target() == 1);

    // The lead on every standing enemy: the nearest.
    Scene all;
    all.n = 2;
    all.in.order = PartnerOrder::Spread;
    all.in.pos = {0.f, 0.f};
    all.e[0].pos = {1.5f, 0.f};
    all.e[1].pos = {1.f, 0.f};
    all.e[0].leadAttacking = all.e[1].leadAttacking = true;
    CHECK(all.step().enemy == 1);
    // The lead on none: the nearest.
    Scene none;
    none.n = 2;
    none.in.order = PartnerOrder::Spread;
    none.in.pos = {0.f, 0.f};
    none.e[0].pos = {1.5f, 0.f};
    none.e[1].pos = {1.f, 0.f};
    CHECK(none.step().enemy == 1);

    // A Down enemy is never a target and does not count: one standing and attacked by the lead is "only one".
    Scene down;
    down.n = 2;
    down.in.order = PartnerOrder::Spread;
    down.in.pos = {0.f, 0.f};
    down.e[0].pos = {1.f, 0.f};
    down.e[0].leadAttacking = true;
    down.e[1].pos = {0.5f, 0.f};
    down.e[1].state = V::State::Down;
    CHECK(down.step().enemy == 0);
    // All down: no target.
    Scene gone;
    gone.e[0].state = V::State::Down;
    gone.in.order = PartnerOrder::Spread;
    const PartnerIntent& g = gone.step();
    CHECK(g.act == PartnerAct::Wait);
    CHECK(g.why == PartnerWhy::NoTarget);

    // The target is kept through a string even when another comes nearer.
    s.e[2].pos = {0.f, 0.6f};
    CHECK(s.step().enemy == 1);
    CHECK(s.b.stringing());
}

TEST_CASE("partner brain: with one enemy and Spread it still fights, the enemy the lead is on") {
    Scene s;
    s.in.order = PartnerOrder::Spread;
    s.e[0].leadAttacking = true;
    s.in.pos = {0.f, 5.f};
    const PartnerIntent& m = s.step();
    CHECK(m.act == PartnerAct::MoveTo);
    CHECK(m.why == PartnerWhy::Approach);
    CHECK(m.enemy == 0);
    CHECK(m.point.y == doctest::Approx(0.5f + 0.9f));
    s.in.pos = {0.f, 1.5f};
    const PartnerIntent& a = s.step();
    CHECK(a.act == PartnerAct::Attack);
    CHECK(a.enemy == 0);
    CHECK(a.newString);
}

TEST_CASE("partner brain: rule 1, StayClose takes the lead's target and is pulled back at 4.1 m") {
    Scene s;
    s.n = 2;
    s.in.order = PartnerOrder::StayClose;
    s.in.leadPos = {0.f, 0.f};
    s.e[0].pos = {0.f, 1.f};
    s.e[1].pos = {0.f, -1.f};
    s.in.pos = {0.f, -2.5f}; // near enemy 1, but the lead is on 0
    s.in.leadTarget = 0;
    const PartnerIntent& m = s.step();
    CHECK(m.act == PartnerAct::MoveTo);
    CHECK(m.enemy == 0);
    // No lead target: nothing to fight.
    s.in.leadTarget = -1;
    CHECK(s.step().why == PartnerWhy::NoTarget);
    s.in.leadTarget = 1;
    s.e[1].state = V::State::Down;
    CHECK(s.step().why == PartnerWhy::NoTarget);
    s.e[1].state = V::State::Idle;

    // 3.9 m: not pulled; 4.1 m: pulled back to the lead, and held until within 3 m.
    s.in.leadTarget = -1;
    s.in.pos = {3.9f, 0.f};
    CHECK(s.step().why == PartnerWhy::NoTarget);
    s.in.pos = {4.1f, 0.f};
    const PartnerIntent& back = s.step();
    CHECK(back.act == PartnerAct::MoveTo);
    CHECK(back.why == PartnerWhy::Leash);
    CHECK(back.point.x == doctest::Approx(0.f));
    s.in.pos = {3.5f, 0.f};
    CHECK(s.step().why == PartnerWhy::Leash);
    s.in.pos = {2.9f, 0.f};
    CHECK(s.step().why == PartnerWhy::NoTarget);
    // A pull back ends a string.
    s.in.leadTarget = 0;
    s.in.pos = {0.f, 2.f};
    REQUIRE(s.step().newString);
    s.in.pos = {0.f, 4.1f};
    CHECK(s.step().why == PartnerWhy::Leash);
    CHECK_FALSE(s.b.stringing());
    // Closing on a target never takes it past returnMeters of the lead.
    Scene far;
    far.in.order = PartnerOrder::StayClose;
    far.in.leadPos = {0.f, 0.f};
    far.in.pos = {0.f, 1.f};
    far.e[0].pos = {6.f, 0.f};
    far.in.leadTarget = 0;
    const PartnerIntent& c = far.step();
    CHECK(c.why == PartnerWhy::Approach);
    CHECK(std::hypot(c.point.x, c.point.y) <= 3.f + 1e-4f);
    // Spread has no leash.
    Scene sp;
    sp.in.order = PartnerOrder::Spread;
    sp.in.leadPos = {0.f, -20.f};
    sp.in.pos = {0.f, 1.5f};
    CHECK(sp.step().act == PartnerAct::Attack);
}

TEST_CASE("partner brain: rule 2, strings of 2 or 3 blows on the host's blow lengths, a 1.2..2.0 s pause after") {
    Scene s(11);
    s.in.order = PartnerOrder::Spread;
    s.in.pos = {0.f, 1.5f};
    s.in.blowSeconds = {0.65f, 0.63f, 0.91f}; // the default fighter's light chain
    const double dt = 0.05;
    int twos = 0, threes = 0;
    double now = 0.0;
    PartnerIntent a = s.step(dt);
    for (int k = 0; k < 40; ++k) {
        REQUIRE(a.act == PartnerAct::Attack);
        REQUIRE(a.newString);
        CHECK(a.blowStart);
        CHECK(a.blowIndex == 0);
        CHECK(a.partnerBlow);
        CHECK(a.breakValue == 0.f);
        CHECK_FALSE(a.extendsStagger);
        CHECK_FALSE(a.punish);
        const int blows = a.blows;
        CHECK((blows == 2 || blows == 3));
        (blows == 2 ? twos : threes) += 1;
        const double pause = s.b.lastPause();
        CHECK(pause >= 1.2 - 1e-6);
        CHECK(pause <= 2.0 + 1e-6);
        // What the host says it does changes nothing: the blows' lengths are the string's clock.
        s.in.doing = k % 2 == 0 ? PartnerDoing::Attacking : PartnerDoing::Idle;
        std::array<double, kMaxPartnerBlows> starts{};
        starts[0] = now;
        int begun = 1;
        for (;;) {
            now += dt;
            const PartnerIntent& c = s.step(dt);
            if (c.why != PartnerWhy::String) {
                a = c;
                break;
            }
            REQUIRE(c.act == PartnerAct::Attack);
            CHECK_FALSE(c.newString);
            if (c.blowStart) {
                REQUIRE(begun < blows);
                CHECK(c.blowIndex == begun);
                starts[static_cast<std::size_t>(begun++)] = now;
            }
            REQUIRE(now - starts[0] < 5.0);
        }
        CHECK(begun == blows);
        // Each blow starts on the first step at or after the one before it ends; the string ends on the first step
        // at or after its last blow ends.
        for (int i = 1; i <= blows; ++i) {
            const double end = starts[static_cast<std::size_t>(i - 1)] + kLight[i - 1];
            const double next = i < blows ? starts[static_cast<std::size_t>(i)] : now;
            CHECK(next >= end - 1e-9);
            CHECK(next < end + dt - 1e-9);
        }
        // The pause runs from that step.
        REQUIRE(a.why == PartnerWhy::Pause);
        double paused = dt;
        for (;;) {
            now += dt;
            const PartnerIntent& p = s.step(dt);
            if (p.why != PartnerWhy::Pause) {
                a = p; // the step that ends the pause starts the next string
                break;
            }
            CHECK(p.act == PartnerAct::Wait);
            paused += dt;
            REQUIRE(paused < 3.0);
        }
        CHECK(paused >= pause - 1e-6);
        CHECK(paused <= pause + dt + 1e-6);
        REQUIRE(s.b.stringing());
    }
    CHECK(twos > 5);
    CHECK(threes > 5);

    // No lengths from the host, or lengths out of 0.1..2 (or not numbers): each blow is the tuning's blowSeconds,
    // and a host that reports Attacking throughout makes the string no longer.
    for (int k = 0; k < 3; ++k) {
        CAPTURE(k);
        Scene f(3);
        f.in.order = PartnerOrder::Spread;
        f.in.pos = {0.f, 1.5f};
        if (k == 1) f.in.blowSeconds = {0.05f, 2.5f, kNaN, -1.f, 0.f, 9.f};
        if (k == 2) f.in.doing = PartnerDoing::Attacking;
        const PartnerIntent& fa = f.step(0.01);
        REQUIRE(fa.newString);
        const double full = fa.blows * 0.65;
        double t = 0.0;
        while (f.b.stringing()) {
            f.step(0.01);
            t += 0.01;
            REQUIRE(t < 5.0);
        }
        CHECK(t == doctest::Approx(full).epsilon(0.02));
    }
    // The lengths are read when the string starts: changed mid-string, they wait for the next.
    Scene r(3);
    r.in.order = PartnerOrder::Spread;
    r.in.pos = {0.f, 1.5f};
    r.in.blowSeconds = {0.3f, 0.3f, 0.3f};
    REQUIRE(r.step(0.01).newString);
    r.in.blowSeconds = {2.f, 2.f, 2.f};
    double t = 0.0;
    while (r.b.stringing()) {
        r.step(0.01);
        t += 0.01;
        REQUIRE(t < 10.0);
    }
    CHECK(t == doctest::Approx(r.b.lastBlows() * 0.3).epsilon(0.05));
}

TEST_CASE("partner brain: rule 2, it never starts a string into a windup aimed at it") {
    Scene s;
    s.in.order = PartnerOrder::Spread;
    s.in.pos = {0.f, 1.5f};
    s.e[0].state = V::State::WindingUp;
    s.e[0].aim = V::Aim::Partner;
    s.e[0].windupLeft = 0.2f; // too little warning to guard: it is hit, but never strings into it
    for (int k = 0; k < 12; ++k) {
        const PartnerIntent& h = s.step(1.0 / 60.0);
        CHECK(h.act == PartnerAct::Wait);
        CHECK(h.why == PartnerWhy::HeldOff);
        CHECK_FALSE(s.b.stringing());
        s.e[0].windupLeft -= 1.f / 60.f;
    }
    CHECK(s.b.rhythmDraws() == 0u);
    // Aimed at the lead: it strings.
    s.e[0].aim = V::Aim::Lead;
    CHECK(s.step().newString);
}

TEST_CASE("partner brain: rule 3, it guards 70% of warned windups aimed at it; never with under 0.25 s") {
    int guarded = 0, total = 0;
    for (std::uint32_t seed = 1; seed <= 400; ++seed) {
        Scene s(seed);
        s.in.order = PartnerOrder::Spread;
        s.in.pos = {0.f, 1.5f};
        for (int w = 0; w < 5; ++w) {
            s.e[0].state = V::State::WindingUp;
            s.e[0].aim = V::Aim::Partner;
            s.e[0].windupLeft = 0.6f;
            const PartnerIntent& it = s.step(0.05);
            ++total;
            if (it.act == PartnerAct::Guard) ++guarded;
            s.e[0].state = V::State::Idle;
            for (int k = 0; k < 10; ++k) s.step(0.05); // the hold runs out
        }
        CHECK(s.b.defenceDraws() == 5u);
    }
    const double share = static_cast<double>(guarded) / total;
    CHECK(share > 0.66);
    CHECK(share < 0.74);

    // First seen at 0.24 s: no draw, no guard.
    Scene late;
    late.e[0].state = V::State::WindingUp;
    late.e[0].aim = V::Aim::Partner;
    late.e[0].windupLeft = 0.24f;
    CHECK(late.step().act != PartnerAct::Guard);
    CHECK(late.b.defenceDraws() == 0u);
    // At exactly 0.25 s: a draw.
    late.e[0].state = V::State::Idle;
    late.step();
    late.e[0].state = V::State::WindingUp;
    late.e[0].windupLeft = 0.25f;
    late.step();
    CHECK(late.b.defenceDraws() == 1u);
}

TEST_CASE("partner brain: rule 3, a guard holds through the windup and guardHold after; a new windup draws anew") {
    PartnerTuning t;
    t.guardChance = 1.f;
    Scene s(5, t);
    s.in.order = PartnerOrder::Spread;
    s.in.pos = {0.f, 1.5f};
    // Mid-string: a guard ends the string and its pause runs from there.
    REQUIRE(s.step(0.05).newString);
    s.in.doing = PartnerDoing::Attacking;
    s.step(0.05);
    REQUIRE(s.b.stringing());
    s.e[0].state = V::State::WindingUp;
    s.e[0].aim = V::Aim::Partner;
    s.e[0].windupLeft = 0.5f;
    const PartnerIntent& g = s.step(0.05);
    CHECK(g.act == PartnerAct::Guard);
    CHECK_FALSE(s.b.stringing());
    CHECK(s.b.pausing());
    s.in.doing = PartnerDoing::Guarding;
    for (int k = 0; k < 9; ++k) {
        s.e[0].windupLeft -= 0.05f;
        CHECK(s.step(0.05).act == PartnerAct::Guard);
    }
    CHECK(s.b.defenceDraws() == 1u);
    // windupLeft rising is a new windup: a second draw.
    s.e[0].windupLeft = 0.8f;
    CHECK(s.step(0.05).act == PartnerAct::Guard);
    CHECK(s.b.defenceDraws() == 2u);
    // It ends: held 0.3 s more (6 steps of 0.05), then not.
    s.e[0].state = V::State::Idle;
    int held = 0;
    for (int k = 0; k < 20; ++k)
        if (s.step(0.05).act == PartnerAct::Guard) ++held;
    CHECK(held == 6);
    // Draw failed: it is hit (never parries, ripostes or dodges: there is no intent for it).
    PartnerTuning never;
    never.guardChance = 0.f;
    Scene h(5, never);
    h.in.pos = {0.f, 1.5f};
    REQUIRE(h.step(0.05).newString);
    h.in.doing = PartnerDoing::Attacking;
    h.e[0].state = V::State::WindingUp;
    h.e[0].aim = V::Aim::Partner;
    h.e[0].windupLeft = 0.5f;
    const PartnerIntent& on = h.step(0.05);
    CHECK(on.act == PartnerAct::Attack); // already in its string: it keeps on, and takes the blow
    CHECK(h.b.defenceDraws() == 1u);
}

TEST_CASE("partner brain: rule 5, down at zero, waits, rises after 30 s at 25% until the host stands it") {
    Scene s;
    s.in.pos = {0.f, 1.5f};
    REQUIRE(s.step(0.1).newString);
    s.in.health = 0.f;
    const PartnerIntent& d = s.step(0.1);
    CHECK(d.act == PartnerAct::Wait);
    CHECK(d.why == PartnerWhy::Down);
    CHECK_FALSE(d.rise);
    CHECK(s.b.down());
    CHECK_FALSE(s.b.stringing());
    for (int k = 0; k < 299; ++k) {
        const PartnerIntent& w = s.step(0.1);
        CHECK(w.why == PartnerWhy::Down);
        CHECK_FALSE(w.rise);
    }
    const PartnerIntent& up = s.step(0.1); // 30 s down
    CHECK(up.why == PartnerWhy::Down);
    CHECK(up.rise);
    CHECK(up.riseHealth == doctest::Approx(0.25f));
    CHECK(s.step(0.1).rise); // level: again until the host stands it
    s.in.health = 0.25f;
    const PartnerIntent& stood = s.step(0.1);
    CHECK_FALSE(stood.rise);
    CHECK(stood.act == PartnerAct::Attack);
    CHECK_FALSE(s.b.down());
    CHECK(s.b.downFor() == 0.0);
}

TEST_CASE("partner brain: rule 6, on a staggered target it strings without pauses, adding no break value") {
    Scene s(9);
    s.in.pos = {0.f, 1.5f};
    s.e[0].state = V::State::Staggered;
    int strings = 0;
    for (int k = 0; k < 400; ++k) {
        const PartnerIntent& a = s.step(0.01);
        CHECK(a.act == PartnerAct::Attack);
        CHECK(a.punish);
        CHECK(a.partnerBlow);
        CHECK(a.breakValue == 0.f);
        CHECK_FALSE(a.extendsStagger);
        if (a.newString) ++strings;
        CHECK_FALSE(s.b.pausing());
    }
    CHECK(strings >= 2); // 4 s at most 1.95 s a string
    // The stagger ends: the next string's end brings its pause back.
    s.e[0].state = V::State::Idle;
    int k = 0;
    while (!s.b.pausing()) {
        CHECK_FALSE(s.step(0.01).punish);
        REQUIRE(++k < 300);
    }
    // A stagger that begins in a pause cuts it.
    s.e[0].state = V::State::Staggered;
    const PartnerIntent& cut = s.step(0.01);
    CHECK(cut.newString);
    CHECK(cut.punish);
}

TEST_CASE("partner brain: in a pause it steps back from a target closer than 0.8 m to its edge") {
    Scene s;
    s.in.pos = {0.f, 1.5f};
    REQUIRE(s.step(0.05).newString);
    for (int k = 0; k < 100 && !s.b.pausing(); ++k) s.step(0.05);
    REQUIRE(s.b.pausing());
    s.in.pos = {0.f, 0.9f}; // 0.4 m to the edge after the lunges
    const PartnerIntent& b = s.step(0.05);
    REQUIRE(s.b.pausing());
    CHECK(b.act == PartnerAct::StepBack);
    CHECK(b.why == PartnerWhy::Pause);
    CHECK(b.point.y == doctest::Approx(0.5f + 0.8f));
    s.in.pos = {0.f, 1.4f};
    CHECK(s.step(0.05).act == PartnerAct::Wait);
}

TEST_CASE("partner brain: the same seed gives the same sequence; draws happen at events, not steps") {
    Fight a, b, c, d;
    REQUIRE(a.run(1.0 / 60.0, PartnerOrder::Spread, 42));
    REQUIRE(b.run(1.0 / 60.0, PartnerOrder::Spread, 42));
    REQUIRE(a.intents.size() == 3600u);
    CHECK(firstDiff(a.intents, b.intents) == kSame); // every step's whole intent
    CHECK(a.host.blows == b.host.blows);
    CHECK(a.host.pauses == b.host.pauses);
    CHECK(a.guardDraws == b.guardDraws);
    REQUIRE(c.run(1.0 / 60.0, PartnerOrder::Spread, 43));
    CHECK(c.host.pauses != a.host.pauses);
    CHECK(firstDiff(a.intents, c.intents) != kSame);
    // At half the step: the same draws (one rhythm pair per string, one defence draw per windup aimed at it).
    REQUIRE(d.run(1.0 / 120.0, PartnerOrder::Spread, 42));
    CHECK(a.brain.rhythmDraws() == 2u * static_cast<std::uint32_t>(a.host.strings + (a.brain.drawnAhead() ? 1 : 0)));
    CHECK(d.brain.rhythmDraws() == 2u * static_cast<std::uint32_t>(d.host.strings + (d.brain.drawnAhead() ? 1 : 0)));
    CHECK(a.brain.defenceDraws() == 3u); // 12 windups, 3 at it
    CHECK(a.brain.defenceDraws() == d.brain.defenceDraws());
    CHECK(a.guardDraws == d.guardDraws);
    const std::size_t m = std::min(a.host.blows.size(), d.host.blows.size());
    REQUIRE(m > 10);
    for (std::size_t i = 0; i < m; ++i) {
        CAPTURE(i);
        CHECK(a.host.blows[i] == d.host.blows[i]);
        CHECK(a.host.pauses[i] == doctest::Approx(d.host.pauses[i]));
    }
}

TEST_CASE("partner brain: over a 60 s scripted fight against a dummy boss its attack time is 35..60% of the lead's") {
    for (const PartnerOrder order : {PartnerOrder::Spread, PartnerOrder::StayClose}) {
        for (const std::uint32_t seed : {1u, 42u, 2026u, 7u, 99u}) {
            CAPTURE(static_cast<int>(order));
            CAPTURE(seed);
            Fight f;
            REQUIRE(f.run(1.0 / 60.0, order, seed, false));
            const double share = f.host.attackTime / f.leadAttack;
            MESSAGE("dummy: partner ", f.host.attackTime, " s, lead ", f.leadAttack, " s, share ", share);
            CHECK(f.leadAttack == doctest::Approx(60.0).epsilon(0.001));
            CHECK(share >= 0.35);
            CHECK(share <= 0.60);
        }
    }
}

TEST_CASE("partner brain: over a 60 s fight with windups, the lead held off, its attack time is 35% of the lead's or more, within the budget") {
    // Both with 3 of each 10 windups at the partner and with every one at the lead (the lead held off 12.6 s of 60).
    // The budget allows 0.60 of the lead's plus 2.0 s in each 20 s window: held in every sliding window, not summed.
    for (const bool partnerAims : {true, false}) {
      for (const PartnerOrder order : {PartnerOrder::Spread, PartnerOrder::StayClose}) {
        for (const std::uint32_t seed : {1u, 42u, 2026u, 7u, 99u}) {
            CAPTURE(partnerAims);
            CAPTURE(static_cast<int>(order));
            CAPTURE(seed);
            Fight f;
            f.partnerAims = partnerAims;
            REQUIRE(f.run(1.0 / 60.0, order, seed, true));
            const double share = f.host.attackTime / f.leadAttack;
            MESSAGE("windups: partner ", f.host.attackTime, " s, lead ", f.leadAttack, " s, share ", share,
                    ", strings ", f.host.strings, ", guards ", f.host.guards, ", hits ", f.host.hits);
            CHECK(share >= 0.35);
            CHECK(worstExcess(f.partnerStep, f.leadStep, 1200) <= 1e-3);
            const int atIt = partnerAims ? 3 : 0;
            CHECK(f.brain.defenceDraws() == static_cast<std::uint32_t>(atIt));
            CHECK(f.host.guards + f.host.hits == atIt);
            CHECK(f.host.strings > 10);
            CHECK(f.brain.rhythmDraws() ==
                  2u * static_cast<std::uint32_t>(f.host.strings + (f.brain.drawnAhead() ? 1 : 0)));
        }
      }
    }
}

TEST_CASE("partner brain: rule 2's budget binds and holds in every 20 s window: windups, at the lead, every 4 s and 2 s") {
    // Each case holds the lead off enough for the budget to bite: the same fight unbound (a tuning where it never
    // binds) breaks some window, and the bound one holds Budget at least once and keeps every window. (The dummy
    // fight, the lead always attacking, never needs it: its share is the 35..60% test's.)
    struct Case {
        const char* name;
        bool partnerAims;
        double period;
    };
    const Case cases[] = {
        {"windups", true, 5.0}, {"windups at the lead", false, 5.0}, {"every 4 s at the lead", false, 4.0},
        {"every 2 s at the lead", false, 2.0},
    };
    for (const Case& c : cases) {
        for (const PartnerOrder order : {PartnerOrder::Spread, PartnerOrder::StayClose}) {
            for (const std::uint32_t seed : {1u, 42u, 2026u, 7u, 99u}) {
                CAPTURE(c.name);
                CAPTURE(static_cast<int>(order));
                CAPTURE(seed);
                Fight f, u;
                f.partnerAims = u.partnerAims = c.partnerAims;
                f.period = u.period = c.period;
                REQUIRE(f.run(1.0 / 60.0, order, seed, true));
                REQUIRE(u.run(1.0 / 60.0, order, seed, true, 60.0, unbound()));
                const double worst = worstExcess(f.partnerStep, f.leadStep, 1200);
                const double without = worstExcess(u.partnerStep, u.leadStep, 1200);
                int held = 0;
                for (const PartnerIntent& it : f.intents) held += it.why == PartnerWhy::Budget ? 1 : 0;
                int heldUnbound = 0;
                for (const PartnerIntent& it : u.intents) heldUnbound += it.why == PartnerWhy::Budget ? 1 : 0;
                MESSAGE(std::string(c.name), ": partner ", f.host.attackTime, " s, lead ", f.leadAttack,
                        " s, worst window ", worst, ", unbound ", without, ", held ", held);
                CHECK(worst <= 1e-3);
                CHECK(without > 0.1); // it bites
                CHECK(held > 0);
                CHECK(heldUnbound == 0);
                CHECK(f.host.overlaps == 0); // every blow it began ran out before the next
                CHECK(f.host.strings > 5);   // it still fights
            }
        }
    }
    // At half the step, the same.
    Fight h, hu;
    h.period = hu.period = 2.0;
    h.partnerAims = hu.partnerAims = false;
    REQUIRE(h.run(1.0 / 120.0, PartnerOrder::Spread, 42, true));
    REQUIRE(hu.run(1.0 / 120.0, PartnerOrder::Spread, 42, true, 60.0, unbound()));
    CHECK(worstExcess(h.partnerStep, h.leadStep, 2400) <= 1e-3);
    CHECK(worstExcess(hu.partnerStep, hu.leadStep, 2400) > 0.1);
}

TEST_CASE("partner brain: a lead who never attacks gives the partner at most 2.0 s of attack per 20 s") {
    for (const PartnerOrder order : {PartnerOrder::Spread, PartnerOrder::StayClose}) {
        for (const std::uint32_t seed : {1u, 42u, 2026u}) {
            CAPTURE(static_cast<int>(order));
            CAPTURE(seed);
            Fight f;
            f.leadFights = false;
            REQUIRE(f.run(1.0 / 60.0, order, seed, false));
            CHECK(f.leadAttack == 0.0);
            CHECK(worstExcess(f.partnerStep, f.leadStep, 1200, 0.6, 2.0) <= 1e-3);
            Fight u;
            u.leadFights = false;
            REQUIRE(u.run(1.0 / 60.0, order, seed, false, 60.0, unbound()));
            CHECK(worstExcess(u.partnerStep, u.leadStep, 1200, 0.6, 2.0) > 0.1); // it bites
            CHECK(f.host.attackTime >= 3.0); // 60 s: it attacks in each window, a string at a time
            bool held = false;
            for (const PartnerIntent& it : f.intents)
                if (it.why == PartnerWhy::Budget) {
                    held = true;
                    CHECK(it.act == PartnerAct::Wait);
                    CHECK(it.enemy == 0);
                }
            CHECK(held);
        }
    }
    // A host that never says (leadAttacking left false) is that lead.
    Scene s(5, PartnerTuning{}, true);
    s.in = PartnerInput{};
    s.in.pos = {0.f, 1.5f};
    double attack = 0.0;
    for (int k = 0; k < 1200; ++k) {
        const PartnerIntent& it = s.step(1.0 / 60.0);
        s.in.doing = it.act == PartnerAct::Attack ? PartnerDoing::Attacking : PartnerDoing::Idle;
        if (it.act == PartnerAct::Attack) attack += 1.0 / 60.0;
    }
    CHECK(attack <= 2.0 + 1e-6);
    CHECK(attack >= 1.0);
}

TEST_CASE("partner brain: the staggered rule (strings without pauses) is still bounded by the budget") {
    for (const bool leadFights : {true, false}) {
        for (const std::uint32_t seed : {1u, 42u, 2026u}) {
            CAPTURE(leadFights);
            CAPTURE(seed);
            Fight f;
            f.staggered = true;
            f.leadFights = leadFights;
            REQUIRE(f.run(1.0 / 60.0, PartnerOrder::Spread, seed, false));
            const double worst = worstExcess(f.partnerStep, f.leadStep, 1200);
            Fight u;
            u.staggered = true;
            u.leadFights = leadFights;
            REQUIRE(u.run(1.0 / 60.0, PartnerOrder::Spread, seed, false, 60.0, unbound()));
            CHECK(worstExcess(u.partnerStep, u.leadStep, 1200) > 0.1); // it bites
            MESSAGE("staggered, lead fights ", leadFights, ": partner ", f.host.attackTime, " s, worst ", worst);
            CHECK(worst <= 1e-3);
            bool held = false, paused = false;
            for (const PartnerIntent& it : f.intents) {
                held = held || it.why == PartnerWhy::Budget;
                paused = paused || it.why == PartnerWhy::Pause;
                if (it.act == PartnerAct::Attack) CHECK(it.punish);
            }
            CHECK(held);
            CHECK_FALSE(paused); // rule 6 still: no pause on a staggered target
            if (leadFights) CHECK(f.host.attackTime > 0.5 * 0.6 * f.leadAttack);
        }
    }
}

TEST_CASE("partner brain: held by the budget it waits on its target, still guards, and starts the drawn string once it fits") {
    PartnerTuning t;
    t.guardChance = 1.f;
    t.blowsMin = t.blowsMax = 3; // the first string 1.95 s of the 2.0 s allowance
    Scene s(5, t, true);
    s.in.leadAttacking = false;
    s.in.pos = {0.f, 1.5f};
    const double dt = 0.05;
    double now = 0.0, spent = 0.0;
    const auto step = [&]() -> const PartnerIntent& {
        const PartnerIntent& it = s.step(dt);
        now += dt;
        if (it.act == PartnerAct::Attack) spent += dt;
        return it;
    };
    REQUIRE(step().newString);
    // The string runs on the tuning's blowSeconds (the host gives no lengths), then its pause.
    while (s.b.stringing() || s.b.pausing()) {
        step();
        REQUIRE(now < 6.0);
    }
    const double first = spent;
    CHECK(first <= 2.0 + 1e-6);
    // Ready, in reach, no windup: held.
    const PartnerIntent& held = step();
    REQUIRE(held.why == PartnerWhy::Budget);
    CHECK(held.act == PartnerAct::Wait);
    CHECK(held.enemy == 0);
    CHECK(s.b.drawnAhead());
    const int blows = s.b.lastBlows();
    const std::uint32_t draws = s.b.rhythmDraws();
    CHECK(draws == 4u);
    CHECK_FALSE(s.b.budgetFits(0.65)); // its first blow would not fit
    // A windup at it while held: it guards, then holds again.
    s.e[0].state = V::State::WindingUp;
    s.e[0].aim = V::Aim::Partner;
    s.e[0].windupLeft = 0.8f;
    CHECK(step().act == PartnerAct::Guard);
    for (int k = 0; k < 15; ++k) {
        s.e[0].windupLeft = std::max(0.f, s.e[0].windupLeft - static_cast<float>(dt));
        CHECK(step().act == PartnerAct::Guard);
    }
    s.e[0].state = V::State::Idle;
    s.e[0].aim = V::Aim::None;
    for (int k = 0; k < 6; ++k) CHECK(step().act == PartnerAct::Guard); // guardHold 0.3 s
    CHECK(step().why == PartnerWhy::Budget);
    // It starts as soon as the first string has slid far enough out for one blow (0.65 + 1.95 - 2.0 = 0.6 s of it out
    // of the window that would hold the new blow): at 18 s + its length, its draws already made.
    double startedAt = -1.0;
    while (startedAt < 0.0) {
        const PartnerIntent& it = step();
        if (it.act == PartnerAct::Attack) {
            CHECK(it.newString);
            CHECK(it.blows == blows);
            startedAt = now - dt;
        } else {
            CHECK(it.why == PartnerWhy::Budget);
        }
        REQUIRE(now < 30.0);
    }
    CHECK(s.b.rhythmDraws() == draws);
    CHECK_FALSE(s.b.drawnAhead());
    CHECK(startedAt >= 18.0 + first - 1e-6);
    CHECK(startedAt <= 18.0 + first + 0.2);
}

TEST_CASE("partner brain: while it cannot act it waits, ends a string, and still draws for a windup at it") {
    PartnerTuning t;
    t.guardChance = 1.f;
    Scene s(5, t);
    s.in.pos = {0.f, 1.5f};
    REQUIRE(s.step(0.05).newString);
    s.in.doing = PartnerDoing::Attacking;
    s.step(0.05);
    s.in.canAct = false;
    s.in.doing = PartnerDoing::Hurt;
    const PartnerIntent& c = s.step(0.05);
    CHECK(c.act == PartnerAct::Wait);
    CHECK(c.why == PartnerWhy::CannotAct);
    CHECK_FALSE(s.b.stringing());
    CHECK(s.b.pausing());
    const std::uint32_t rhythm = s.b.rhythmDraws();
    // A windup at it while it cannot act: drawn (the stream never hangs on hitstun), but it cannot guard yet.
    s.e[0].state = V::State::WindingUp;
    s.e[0].aim = V::Aim::Partner;
    s.e[0].windupLeft = 0.6f;
    CHECK(s.step(0.05).why == PartnerWhy::CannotAct);
    CHECK(s.b.defenceDraws() == 1u);
    // Out of reach of its leash too: CannotAct comes first.
    s.in.order = PartnerOrder::StayClose;
    s.in.leadPos = {0.f, -10.f};
    s.e[0].windupLeft = 0.55f;
    CHECK(s.step(0.05).why == PartnerWhy::CannotAct);
    for (int k = 0; k < 40; ++k) CHECK(s.step(0.05).why == PartnerWhy::CannotAct);
    CHECK(s.b.rhythmDraws() == rhythm);
    CHECK(s.b.defenceDraws() == 1u);
    // Able again while the windup is on: it guards on the draw it made.
    s.in.canAct = true;
    s.in.order = PartnerOrder::Spread;
    s.e[0].windupLeft = 0.5f;
    CHECK(s.step(0.05).act == PartnerAct::Guard);
    CHECK(s.b.defenceDraws() == 1u);
    // Down outranks it.
    s.in.canAct = false;
    s.in.health = 0.f;
    CHECK(s.step(0.05).why == PartnerWhy::Down);
}

TEST_CASE("partner brain: two blows aimed at it at once draw once each; it guards when either says guard") {
    PartnerTuning always;
    always.guardChance = 1.f;
    Scene s(5, always);
    s.n = 2;
    s.in.pos = {0.f, 0.f};
    s.e[0].pos = {1.5f, 0.f};
    s.e[1].pos = {-1.5f, 0.f};
    for (V* e : {&s.e[0], &s.e[1]}) {
        e->state = V::State::WindingUp;
        e->aim = V::Aim::Partner;
        e->windupLeft = 0.6f;
    }
    CHECK(s.step(0.05).act == PartnerAct::Guard);
    CHECK(s.b.defenceDraws() == 2u);
    // The first lands; the second still winds up: it keeps guarding, with no new draw.
    s.e[0].state = V::State::Idle;
    s.e[1].windupLeft = 0.5f;
    for (int k = 0; k < 8; ++k) CHECK(s.step(0.05).act == PartnerAct::Guard);
    CHECK(s.b.defenceDraws() == 2u);
    // One with too little warning: one draw, and it guards on the other's.
    Scene late(5, always);
    late.n = 2;
    late.in.pos = {0.f, 0.f};
    late.e[0].pos = {1.5f, 0.f};
    late.e[1].pos = {-1.5f, 0.f};
    late.e[0].state = late.e[1].state = V::State::WindingUp;
    late.e[0].aim = late.e[1].aim = V::Aim::Partner;
    late.e[0].windupLeft = 0.6f;
    late.e[1].windupLeft = 0.2f;
    CHECK(late.step(0.05).act == PartnerAct::Guard);
    CHECK(late.b.defenceDraws() == 1u);
    // At 70% each, it guards when either draw says so: 1 - 0.3 x 0.3 = 91% of the time.
    int guarded = 0;
    const int runs = 600;
    for (int seed = 1; seed <= runs; ++seed) {
        Scene p(static_cast<std::uint32_t>(seed));
        p.n = 2;
        p.in.pos = {0.f, 0.f};
        p.e[0].pos = {1.5f, 0.f};
        p.e[1].pos = {-1.5f, 0.f};
        p.e[0].state = p.e[1].state = V::State::WindingUp;
        p.e[0].aim = p.e[1].aim = V::Aim::Partner;
        p.e[0].windupLeft = p.e[1].windupLeft = 0.6f;
        if (p.step(0.05).act == PartnerAct::Guard) ++guarded;
        REQUIRE(p.b.defenceDraws() == 2u);
    }
    const double share = static_cast<double>(guarded) / runs;
    CHECK(share > 0.87);
    CHECK(share < 0.95);
}

TEST_CASE("partner brain: with no enemies it has no target in either order, and the leash still pulls") {
    for (const PartnerOrder order : {PartnerOrder::Spread, PartnerOrder::StayClose}) {
        CAPTURE(static_cast<int>(order));
        Scene s;
        s.n = 0;
        s.in.order = order;
        s.in.leadTarget = 0; // past the empty span
        for (int k = 0; k < 30; ++k) {
            const PartnerIntent& it = s.step(0.05);
            CHECK(it.act == PartnerAct::Wait);
            CHECK(it.why == PartnerWhy::NoTarget);
            CHECK(it.enemy == -1);
        }
        CHECK(s.b.rhythmDraws() == 0u);
        CHECK(s.b.defenceDraws() == 0u);
        CHECK(s.b.target() == -1);
    }
    Scene far;
    far.n = 0;
    far.in.order = PartnerOrder::StayClose;
    far.in.pos = {0.f, 5.f};
    CHECK(far.step(0.05).why == PartnerWhy::Leash);
    // An empty span by default (no view set at all).
    PartnerBrain b;
    REQUIRE(b.begin(PartnerTuning{}, 1));
    CHECK(b.step(0.05, PartnerInput{}).why == PartnerWhy::NoTarget);
}

TEST_CASE("partner brain: views past the sixth are ignored: never a target, never drawn for") {
    PartnerBrain b;
    REQUIRE(b.begin(PartnerTuning{}, 3));
    std::array<V, 8> e{};
    for (std::size_t i = 0; i < 6; ++i) e[i].pos = {10.f + static_cast<float>(i), 0.f};
    e[6].pos = {0.f, 0.6f}; // both far nearer
    e[7].pos = {0.f, -0.6f};
    PartnerInput in;
    in.order = PartnerOrder::Spread;
    in.enemies = std::span<const V>(e.data(), e.size());
    const PartnerIntent& it = b.step(0.05, in);
    CHECK(it.why == PartnerWhy::Approach);
    CHECK(it.enemy == 0);
    e[7].state = V::State::WindingUp;
    e[7].aim = V::Aim::Partner;
    e[7].windupLeft = 0.8f;
    CHECK(b.step(0.05, in).act != PartnerAct::Guard);
    CHECK(b.defenceDraws() == 0u);
    in.order = PartnerOrder::StayClose;
    in.leadTarget = 7;
    CHECK(b.step(0.05, in).why == PartnerWhy::NoTarget);
    in.leadTarget = 6;
    CHECK(b.step(0.05, in).why == PartnerWhy::NoTarget);
    in.leadTarget = 5;
    CHECK(b.step(0.05, in).enemy == 5);
}

TEST_CASE("partner brain: a view whose position or radius is not a number is absent") {
    Scene s;
    s.n = 2;
    s.in.order = PartnerOrder::Spread;
    s.in.pos = {0.f, 0.f};
    s.e[0].pos = {kNaN, 0.f};
    s.e[1].pos = {3.f, 0.f};
    CHECK(s.step(0.05).enemy == 1);
    s.e[0].state = V::State::WindingUp;
    s.e[0].aim = V::Aim::Partner;
    s.e[0].windupLeft = 0.8f;
    CHECK(s.step(0.05).act != PartnerAct::Guard);
    CHECK(s.b.defenceDraws() == 0u);
    // Alone, in either order, each way of being absent: no target.
    const float inf = std::numeric_limits<float>::infinity();
    for (const PartnerOrder order : {PartnerOrder::Spread, PartnerOrder::StayClose}) {
        for (int k = 0; k < 4; ++k) {
            CAPTURE(k);
            Scene one;
            one.in.order = order;
            one.in.leadTarget = 0;
            if (k == 0) one.e[0].pos = {0.f, kNaN};
            if (k == 1) one.e[0].pos = {inf, 0.f};
            if (k == 2) one.e[0].radius = kNaN;
            if (k == 3) one.e[0].radius = -1.f;
            CHECK(one.step(0.05).why == PartnerWhy::NoTarget);
        }
    }
    // A target that turns NaN mid-string is dropped, with no pause on it.
    Scene m;
    m.in.pos = {0.f, 1.5f};
    REQUIRE(m.step(0.05).newString);
    m.in.doing = PartnerDoing::Attacking;
    m.step(0.05);
    m.e[0].pos.x = kNaN;
    CHECK(m.step(0.05).why == PartnerWhy::NoTarget);
    CHECK_FALSE(m.b.stringing());
    CHECK_FALSE(m.b.pausing());
    // Back to a number: a target again, once the blow it was in has run out (0.65 s from its start).
    m.e[0].pos.x = 0.f;
    m.in.doing = PartnerDoing::Idle;
    double t = 0.15;
    for (;;) {
        const PartnerIntent& w = m.step(0.05);
        if (w.why != PartnerWhy::Committed) {
            CHECK(w.newString);
            break;
        }
        CHECK(w.act == PartnerAct::Wait);
        CHECK(w.enemy == 0);
        t += 0.05;
        REQUIRE(t < 1.0);
    }
    CHECK(t >= 0.65 - 1e-6);
    CHECK(t < 0.7 + 1e-6);
}

TEST_CASE("partner brain: begin() reused across two fights gives the same stream as a fresh brain") {
    Fight reused;
    REQUIRE(reused.run(1.0 / 60.0, PartnerOrder::StayClose, 9));
    // Leave it Down past its rise, mid-watch, before the second fight.
    PartnerInput down;
    down.health = 0.f;
    reused.brain.step(0.1, down);
    for (int k = 0; k < 160; ++k) reused.brain.step(0.25, down);
    REQUIRE(reused.brain.down());
    REQUIRE(reused.brain.intent().rise);
    reused.clear();
    REQUIRE(reused.run(1.0 / 60.0, PartnerOrder::Spread, 42));
    Fight fresh;
    REQUIRE(fresh.run(1.0 / 60.0, PartnerOrder::Spread, 42));
    CHECK(firstDiff(reused.intents, fresh.intents) == kSame);
    CHECK(reused.brain.rhythmDraws() == fresh.brain.rhythmDraws());
    CHECK(reused.brain.defenceDraws() == fresh.brain.defenceDraws());
    CHECK(reused.host.blows == fresh.host.blows);
    CHECK(reused.host.pauses == fresh.host.pauses);
    CHECK(reused.guardDraws == fresh.guardDraws);
    CHECK(reused.host.attackTime == fresh.host.attackTime);
}

TEST_CASE("partner brain: the importer refuses each field just past either bound, and an empty file keeps defaults") {
    struct Bound {
        const char* key;
        const char* below;
        const char* above;
    };
    const Bound bounds[] = {
        {"leashMeters", "0.99", "10.01"},   {"returnMeters", "0.49", "10.01"}, {"reachMeters", "0.49", "4.01"},
        {"standoffMeters", "-0.01", "4.01"}, {"blowsMin", "0", "7"},           {"blowsMax", "0", "7"},
        {"blowSeconds", "0.09", "2.01"},    {"pauseMin", "-0.01", "10.01"},    {"pauseMax", "-0.01", "10.01"},
        {"guardWarning", "-0.01", "2.01"},  {"guardChance", "-0.01", "1.01"},  {"guardHold", "-0.01", "2.01"},
        {"downSeconds", "0.99", "600.01"},  {"riseHealth", "0.009", "1.01"},   {"blowCapShare", "-0.01", "1.01"},
        {"overScale", "-0.01", "1.01"},     {"budgetWindowSeconds", "4.99", "30.01"}, {"budgetShare", "-0.01", "1.01"},
        {"budgetAllowanceSeconds", "-0.01", "10.01"},
    };
    const PartnerTuning d;
    PartnerTuning t;
    std::string err;
    for (const Bound& b : bounds) {
        for (const char* v : {b.below, b.above}) {
            const std::string json = std::string(R"({"version":1,")") + b.key + "\":" + v + "}";
            CAPTURE(json);
            t.leashMeters = 9.f;
            CHECK_FALSE(importPartnerBrain(json, t, &err));
            CHECK(err.find(b.key) != std::string::npos);
            CHECK(t.leashMeters == d.leashMeters);
        }
    }
    // Each bound itself is taken, where the other fields' defaults allow it.
    for (const std::string ok : {
             R"({"version":1,"guardWarning":0})",     R"({"version":1,"guardWarning":2})",
             R"({"version":1,"guardChance":0})",      R"({"version":1,"guardHold":0})",
             R"({"version":1,"guardHold":2})",        R"({"version":1,"downSeconds":1})",
             R"({"version":1,"downSeconds":600})",    R"({"version":1,"riseHealth":0.01})",
             R"({"version":1,"riseHealth":1})",       R"({"version":1,"blowCapShare":0})",
             R"({"version":1,"blowCapShare":1})",     R"({"version":1,"overScale":0})",
             R"({"version":1,"overScale":1})",        R"({"version":1,"budgetWindowSeconds":5})",
             R"({"version":1,"budgetWindowSeconds":30})", R"({"version":1,"budgetShare":0})",
             R"({"version":1,"budgetShare":1})",      R"({"version":1,"budgetAllowanceSeconds":0})",
             R"({"version":1,"budgetAllowanceSeconds":10})", R"({"version":1,"standoffMeters":0})",
             R"({"version":1,"blowSeconds":0.1})",    R"({"version":1,"blowSeconds":2})",
             R"({"version":1,"leashMeters":10})",     R"({"version":1,"reachMeters":4})",
         }) {
        CAPTURE(ok);
        CHECK(importPartnerBrain(ok, t, &err));
    }
    // Empty or blank text: refused, and the defaults.
    for (const char* blank : {"", "   ", "\n"}) {
        CAPTURE(blank);
        t.leashMeters = 9.f;
        t.budgetShare = 0.3f;
        err.clear();
        CHECK_FALSE(importPartnerBrain(blank, t, &err));
        CHECK_FALSE(err.empty());
        CHECK(t.leashMeters == d.leashMeters);
        CHECK(t.budgetShare == d.budgetShare);
    }
}

TEST_CASE("partner brain: a target that leaves the span mid-string or mid-pause is dropped, never read past it") {
    Scene s;
    s.n = 2;
    s.in.order = PartnerOrder::Spread;
    s.in.pos = {0.f, 0.f};
    s.e[0].pos = {3.f, 0.f};
    s.e[1].pos = {1.f, 0.f};
    REQUIRE(s.step(0.05).enemy == 1);
    s.in.doing = PartnerDoing::Attacking;
    s.step(0.05);
    REQUIRE(s.b.stringing());
    s.n = 1; // the host's span is shorter now
    const PartnerIntent& a = s.step(0.05);
    CHECK_FALSE(s.b.pausing());
    CHECK(a.enemy == 0);
    CHECK(a.why == PartnerWhy::Approach);
    // Mid-pause.
    Scene p;
    p.n = 2;
    p.in.pos = {0.f, 0.f};
    p.e[0].pos = {3.f, 0.f};
    p.e[1].pos = {1.f, 0.f};
    REQUIRE(p.step(0.05).enemy == 1);
    for (int k = 0; k < 100 && !p.b.pausing(); ++k) p.step(0.05);
    REQUIRE(p.b.pausing());
    p.n = 1;
    CHECK(p.step(0.05).why == PartnerWhy::Approach);
    // Down mid-string.
    Scene d;
    d.in.pos = {0.f, 1.5f};
    REQUIRE(d.step(0.05).newString);
    d.e[0].state = V::State::Down;
    CHECK(d.step(0.05).why == PartnerWhy::NoTarget);
    CHECK_FALSE(d.b.pausing());
}

TEST_CASE("partner brain: a long run of every branch allocates nothing per step") {
    Scene s(77);
    s.n = kMaxPartnerEnemies;
    for (std::size_t i = 0; i < s.n; ++i) s.e[i].pos = {static_cast<float>(i) - 2.5f, 1.f};
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    int acts[5] = {};
    for (int i = 0; i < 20000; ++i) {
        s.in.order = (i / 900) % 2 == 0 ? PartnerOrder::Spread : PartnerOrder::StayClose;
        s.in.health = (i / 4000) % 5 == 4 ? 0.f : 0.6f;
        s.in.canAct = i % 97 > 3;
        s.in.doing = (i / 30) % 3 == 0 ? PartnerDoing::Attacking : PartnerDoing::Idle;
        s.in.pos = {std::sin(i * 0.01f) * 5.f, std::cos(i * 0.013f) * 3.f};
        s.in.leadTarget = (i / 200) % 7 - 1;
        for (std::size_t k = 0; k < s.n; ++k) {
            V& e = s.e[k];
            const int phase = (i / 50 + static_cast<int>(k)) % 6;
            e.state = phase == 0 ? V::State::WindingUp : phase == 1 ? V::State::Staggered
                      : phase == 5 && k == 3 ? V::State::Down
                                             : V::State::Idle;
            e.aim = k % 2 == 0 ? V::Aim::Partner : V::Aim::Lead;
            e.windupLeft = 1.f - static_cast<float>(i % 50) * 0.02f;
            e.leadAttacking = (i / 70 + static_cast<int>(k)) % 3 == 0;
        }
        acts[static_cast<int>(s.step(1.0 / 60.0).act)] += 1;
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    if (counting) CHECK(allocs == 0);
    for (int a : acts) CHECK(a > 0);
}

namespace {
// A scripted scene through the run-out host, the ruled budget on: `bad` replaces step `at`'s time. Every step's intent
// and brain hash.
struct Stream {
    std::vector<PartnerIntent> intents;
    std::vector<std::uint64_t> hashes;
};
void script(Scene& s, int k) {
    s.in.leadAttacking = (k / 240) % 3 != 2; // held off a third of the time: the budget binds
    const bool winding = (k / 300) % 4 == 3;
    s.e[0].state = winding ? V::State::WindingUp : V::State::Idle;
    s.e[0].aim = winding ? V::Aim::Partner : V::Aim::None;
    s.e[0].windupLeft = winding ? 1.f - static_cast<float>(k % 300) / 300.f : 0.f;
}
Scene& setUp(Scene& s) {
    s.n = 2;
    s.in.order = PartnerOrder::Spread;
    s.in.pos = {0.f, 1.5f};
    s.e[1].pos = {2.f, 1.5f};
    return s;
}
Stream streamWith(double bad, int at) {
    Stream out;
    Scene s(31, PartnerTuning{}, true);
    setUp(s);
    Host h;
    for (int k = 0; k < 1500; ++k) {
        const double dt = k == at ? bad : 1.0 / 60.0;
        script(s, k);
        h.feed(s.in);
        const PartnerIntent& it = s.step(dt);
        out.intents.push_back(it);
        out.hashes.push_back(s.b.hash());
        h.apply(it, s.b, s.in, std::isfinite(dt) && dt > 0.0 ? std::min(dt, kMaxPartnerStep) : 0.0);
    }
    return out;
}
} // namespace

TEST_CASE("partner brain: a step of inf, 1e30, NaN or under 0 returns, and goes on as the step it counts as") {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    struct Case {
        double bad;
        double as;
    };
    const Case cases[] = {{inf, 0.0},  {1e30, kMaxPartnerStep}, {nan, 0.0},
                          {-1.0, 0.0}, {-inf, 0.0},             {0.9, kMaxPartnerStep}};
    for (const int at : {7, 400, 1000}) { // in a string, held, wherever the script has it
        for (const Case& c : cases) {
            CAPTURE(at);
            CAPTURE(c.bad);
            const Stream got = streamWith(c.bad, at);
            const Stream want = streamWith(c.as, at);
            REQUIRE(got.intents.size() == 1500u);
            CHECK(firstDiff(got.intents, want.intents) == kSame);
            CHECK(got.hashes == want.hashes);
        }
    }
    // The script does differ by its steps (the comparison can fail).
    CHECK(streamWith(kMaxPartnerStep, 400).hashes != streamWith(0.0, 400).hashes);
    // Huge steps on their own: each returns, the clock moved by kMaxPartnerStep.
    PartnerBrain b;
    REQUIRE(b.begin(PartnerTuning{}, 3));
    PartnerInput in;
    b.step(1e300, in);
    b.step(inf, in);
    CHECK(b.save().now == doctest::Approx(kMaxPartnerStep));
    CHECK(b.save().slot == 5);
}

TEST_CASE("partner brain: load(save()) mid-string goes on exactly as the brain it came from") {
    const Stream whole = streamWith(1.0 / 60.0, 0);
    // The same script, saved at step 333 and loaded into a second brain that takes over from there.
    Scene s(31, PartnerTuning{}, true);
    setUp(s);
    Host h;
    PartnerBrainSave kept{};
    bool stringing = false;
    std::vector<std::uint64_t> hashes;
    PartnerBrain other;
    for (int k = 0; k < 1500; ++k) {
        script(s, k);
        h.feed(s.in);
        s.in.enemies = std::span<const V>(s.e.data(), s.n);
        if (k == 333) {
            kept = s.b.save();
            stringing = s.b.stringing();
            other.load(kept);
            CHECK(other.hash() == s.b.hash());
        }
        PartnerBrain& b = k >= 333 ? other : s.b;
        const PartnerIntent& it = b.step(1.0 / 60.0, s.in);
        hashes.push_back(b.hash());
        h.apply(it, b, s.in, 1.0 / 60.0);
    }
    CHECK(stringing);
    CHECK(hashes == whole.hashes);
    // A save is its state: one step on, the hash differs.
    PartnerBrain moved;
    moved.load(kept);
    PartnerInput in;
    moved.step(1.0 / 60.0, in);
    CHECK(moved.hash() != kept.hash());
}

TEST_CASE("partner brain: a blow begun runs out: its target felled mid-blow, it waits (Committed), then strings on the next") {
    Scene s(5, PartnerTuning{}, true);
    setUp(s); // the second enemy 1.5 m from its edge: in reach
    Host h;
    const double dt = 1.0 / 60.0;
    std::vector<double> mine, lead;
    double now = 0.0, startedNext = -1.0;
    bool committed = false;
    for (int k = 0; k < 600 && startedNext < 0.0; ++k) {
        if (k == 20) s.e[0].state = V::State::Down; // 0.33 s into its first 0.9 s blow
        h.feed(s.in);
        const double left = s.b.committedLeft(); // before the step: the blow still to run
        const PartnerIntent& it = s.step(dt);
        if (k == 0) {
            REQUIRE(it.newString);
            REQUIRE(it.enemy == 0);
        }
        if (k >= 20 && it.why == PartnerWhy::Committed) {
            committed = true;
            CHECK(it.act == PartnerAct::Wait);
            CHECK(it.enemy == 1);
            CHECK(left > 0.0);
            CHECK(h.running());
        }
        if (k > 0 && it.newString) {
            CHECK(it.enemy == 1);
            CHECK_FALSE(h.running());
            startedNext = now;
        }
        const double was = h.attackTime;
        h.apply(it, s.b, s.in, dt);
        mine.push_back(h.attackTime - was);
        lead.push_back(dt);
        now += dt;
    }
    CHECK(committed);
    CHECK(startedNext >= 0.9 - 1e-9);
    CHECK(startedNext < 0.9 + dt + 1e-9);
    CHECK(h.overlaps == 0);
    CHECK(worstExcess(mine, lead, 1200) <= 1e-3);
}

TEST_CASE("partner brain: a guard cuts a string on a staggered target: the blow runs out, no pause, then the next string") {
    PartnerTuning t;
    t.guardChance = 1.f;
    Scene s(5, t, true);
    s.n = 2;
    s.in.order = PartnerOrder::Spread;
    s.in.pos = {0.f, 1.5f};
    s.e[0].state = V::State::Staggered;
    s.e[1].pos = {6.f, 1.5f}; // out of reach: it only winds up at the partner
    Host h;
    const double dt = 1.0 / 60.0;
    std::vector<double> mine, lead;
    double now = 0.0, next = -1.0;
    bool guarded = false, committed = false, paused = false;
    for (int k = 0; k < 600 && next < 0.0; ++k) {
        const bool winding = k >= 3 && k < 21; // 0.3 s of warning from 0.05 s
        s.e[1].state = winding ? V::State::WindingUp : V::State::Idle;
        s.e[1].aim = winding ? V::Aim::Partner : V::Aim::None;
        s.e[1].windupLeft = winding ? static_cast<float>(21 - k) * static_cast<float>(dt) : 0.f;
        h.feed(s.in);
        const PartnerIntent& it = s.step(dt);
        guarded = guarded || it.act == PartnerAct::Guard;
        committed = committed || it.why == PartnerWhy::Committed;
        paused = paused || it.why == PartnerWhy::Pause;
        if (it.act == PartnerAct::Guard) CHECK(h.running()); // the host cannot guard yet: its blow runs out
        if (k > 0 && it.newString) {
            CHECK(it.punish);
            CHECK_FALSE(h.running());
            next = now;
        }
        const double was = h.attackTime;
        h.apply(it, s.b, s.in, dt);
        mine.push_back(h.attackTime - was);
        lead.push_back(dt);
        now += dt;
    }
    CHECK(guarded);
    CHECK(committed); // the guard (0.3 s, then 0.3 s held) ends before the 0.9 s blow
    CHECK_FALSE(paused);
    CHECK(next >= 0.9 - 1e-9);
    CHECK(next < 0.9 + dt + 1e-9);
    CHECK(h.attackTime == doctest::Approx(0.9 + dt).epsilon(0.05)); // the first blow whole, and the next one's first step
    CHECK(h.overlaps == 0);
    CHECK(worstExcess(mine, lead, 1200) <= 1e-3);
}

TEST_CASE("partner brain: every field of its save moves the hash: a change to any one of them is seen") {
    PartnerBrain b;
    REQUIRE(b.begin(PartnerTuning{}, 7));
    const PartnerBrainSave base = b.save();
    using Edit = void (*)(PartnerBrainSave&);
    const Edit edits[] = {
        [](PartnerBrainSave& s) { s.t.leashMeters += 1.f; },
        [](PartnerBrainSave& s) { s.t.returnMeters += 1.f; },
        [](PartnerBrainSave& s) { s.t.reachMeters += 1.f; },
        [](PartnerBrainSave& s) { s.t.standoffMeters += 1.f; },
        [](PartnerBrainSave& s) { s.t.blowsMin += 1; },
        [](PartnerBrainSave& s) { s.t.blowsMax += 1; },
        [](PartnerBrainSave& s) { s.t.blowSeconds += 0.1f; },
        [](PartnerBrainSave& s) { s.t.pauseMin += 0.1f; },
        [](PartnerBrainSave& s) { s.t.pauseMax += 0.1f; },
        [](PartnerBrainSave& s) { s.t.guardWarning += 0.1f; },
        [](PartnerBrainSave& s) { s.t.guardChance -= 0.1f; },
        [](PartnerBrainSave& s) { s.t.guardHold += 0.1f; },
        [](PartnerBrainSave& s) { s.t.downSeconds += 1.f; },
        [](PartnerBrainSave& s) { s.t.riseHealth += 0.1f; },
        [](PartnerBrainSave& s) { s.t.blowCapShare += 0.1f; },
        [](PartnerBrainSave& s) { s.t.overScale += 0.1f; },
        [](PartnerBrainSave& s) { s.t.budgetWindowSeconds += 1.f; },
        [](PartnerBrainSave& s) { s.t.budgetShare -= 0.1f; },
        [](PartnerBrainSave& s) { s.t.budgetAllowanceSeconds += 1.f; },
        [](PartnerBrainSave& s) { s.t.personalSpaceMeters += 0.1f; },
        [](PartnerBrainSave& s) { s.t.wedgeHalfDegrees += 1.f; },
        [](PartnerBrainSave& s) { s.t.wedgeBeyondMeters += 0.1f; },
        [](PartnerBrainSave& s) { s.t.wedgeClearMeters += 0.1f; },
        [](PartnerBrainSave& s) { s.t.closeArcMin += 1.f; },
        [](PartnerBrainSave& s) { s.t.closeArcMax += 1.f; },
        [](PartnerBrainSave& s) { s.t.closeLeadMeters += 0.1f; },
        [](PartnerBrainSave& s) { s.t.spreadArcMin += 1.f; },
        [](PartnerBrainSave& s) { s.t.spreadArcMax -= 1.f; },
        [](PartnerBrainSave& s) { s.t.sideHoldSeconds += 0.1f; },
        [](PartnerBrainSave& s) { s.t.followMin += 0.1f; },
        [](PartnerBrainSave& s) { s.t.followMax += 0.1f; },
        [](PartnerBrainSave& s) { s.t.stationTolerance += 0.1f; },
        [](PartnerBrainSave& s) { s.t.stationGiveUpSeconds += 0.5f; },
        [](PartnerBrainSave& s) { s.t.stationAnySeconds += 0.5f; },
        [](PartnerBrainSave& s) { s.t.targetHoldSeconds += 0.5f; },
        [](PartnerBrainSave& s) { s.t.frustumMargin += 0.05f; },
        [](PartnerBrainSave& s) { s.t.frustumGiveUpSeconds += 0.5f; },
        [](PartnerBrainSave& s) { s.t.engageSeconds += 0.5f; },
        [](PartnerBrainSave& s) { s.t.repositionEverySeconds += 0.5f; },
        [](PartnerBrainSave& s) { s.t.repositionMeters += 0.5f; },
        [](PartnerBrainSave& s) { s.rhythm += 1; },
        [](PartnerBrainSave& s) { s.defence += 1; },
        [](PartnerBrainSave& s) { s.rhythmDraws += 1; },
        [](PartnerBrainSave& s) { s.defenceDraws += 1; },
        [](PartnerBrainSave& s) { s.phase = PartnerBrainSave::Phase::Pause; },
        [](PartnerBrainSave& s) { s.target += 2; },
        [](PartnerBrainSave& s) { s.blows += 1; },
        [](PartnerBrainSave& s) { s.pause += 1.0; },
        [](PartnerBrainSave& s) { s.clock += 1.0; },
        [](PartnerBrainSave& s) { s.returning = !s.returning; },
        [](PartnerBrainSave& s) { s.drawn = !s.drawn; },
        [](PartnerBrainSave& s) { s.lengths[kMaxPartnerBlows - 1] += 0.5; },
        [](PartnerBrainSave& s) { s.begunBlows += 1; },
        [](PartnerBrainSave& s) { s.committedEnd += 0.5; },
        [](PartnerBrainSave& s) { s.spentPartner[PartnerBrainSave::kSlots - 1] += 0.05f; },
        [](PartnerBrainSave& s) { s.spentLead[3] += 0.05f; },
        [](PartnerBrainSave& s) { s.slot += 1; },
        [](PartnerBrainSave& s) { s.now += 1.0; },
        [](PartnerBrainSave& s) { s.down = !s.down; },
        [](PartnerBrainSave& s) { s.downFor += 1.0; },
        [](PartnerBrainSave& s) { s.watch[kMaxPartnerEnemies - 1].winding = true; },
        [](PartnerBrainSave& s) { s.watch[0].left += 0.5f; },
        [](PartnerBrainSave& s) { s.watch[1].guard = true; },
        [](PartnerBrainSave& s) { s.watch[2].hold += 0.5; },
        [](PartnerBrainSave& s) { s.out.act = PartnerAct::Attack; },
        [](PartnerBrainSave& s) { s.out.why = PartnerWhy::Committed; },
        [](PartnerBrainSave& s) { s.out.point.x += 1.f; },
        [](PartnerBrainSave& s) { s.out.point.y += 1.f; },
        [](PartnerBrainSave& s) { s.out.enemy += 2; },
        [](PartnerBrainSave& s) { s.out.blows += 1; },
        [](PartnerBrainSave& s) { s.out.newString = !s.out.newString; },
        [](PartnerBrainSave& s) { s.out.blowStart = !s.out.blowStart; },
        [](PartnerBrainSave& s) { s.out.blowIndex += 2; },
        [](PartnerBrainSave& s) { s.out.partnerBlow = !s.out.partnerBlow; },
        [](PartnerBrainSave& s) { s.out.breakValue += 1.f; },
        [](PartnerBrainSave& s) { s.out.extendsStagger = !s.out.extendsStagger; },
        [](PartnerBrainSave& s) { s.out.punish = !s.out.punish; },
        [](PartnerBrainSave& s) { s.out.rise = !s.out.rise; },
        [](PartnerBrainSave& s) { s.out.riseHealth += 0.25f; },
        [](PartnerBrainSave& s) { s.out.stationed = !s.out.stationed; },
        [](PartnerBrainSave& s) { s.begun = !s.begun; },
        [](PartnerBrainSave& s) { s.side = 1; },
        [](PartnerBrainSave& s) { s.sideKey += 1; },
        [](PartnerBrainSave& s) { s.sideFor += 0.5; },
        [](PartnerBrainSave& s) { s.swaps += 1; },
        [](PartnerBrainSave& s) { s.sideTarget += 2; },
        [](PartnerBrainSave& s) { s.offStation += 0.5; },
        [](PartnerBrainSave& s) { s.targetFor += 0.5; },
        [](PartnerBrainSave& s) { s.targetReached = !s.targetReached; },
    };
    // One row per field: the 40 tunables (rule 17's switch, a bool padded to a float's size, is a framed row below); rhythm .. downFor (20); a watch's 4; the intent's 16 (its point as x and
    // y); begun; side, sideKey, sideFor, swaps, sideTarget, offStation, targetFor, targetReached (8).
    static_assert(sizeof(PartnerTuning) == 41 * sizeof(float), "a new tunable needs a row here");
    CHECK(std::size(edits) == 40u + 20u + 4u + 16u + 1u + 8u);
    int i = 0;
    for (const Edit e : edits) {
        CAPTURE(i++);
        PartnerBrainSave s = base;
        e(s);
        CHECK(s.hash() != base.hash());
        PartnerBrain c;
        c.load(s);
        CHECK(c.hash() == s.hash());
    }
    // -0 and +0 are one state.
    PartnerBrainSave z = base;
    z.now = -0.0;
    CHECK(z.hash() == base.hash());

    // Rules 13-16's fields: seen once framed; unframed (no camera ever given) they leave the hash as it was.
    PartnerBrainSave fb = base;
    fb.framed = true;
    CHECK(fb.hash() != base.hash());
    const Edit framedEdits[] = {
        [](PartnerBrainSave& s) { s.frameRing = 1; },
        [](PartnerBrainSave& s) { s.frameTarget += 2; },
        [](PartnerBrainSave& s) { s.frameAngle += 5.f; },
        [](PartnerBrainSave& s) { s.frameRadius += 0.5f; },
        [](PartnerBrainSave& s) { s.frameLast.x += 1.f; },
        [](PartnerBrainSave& s) { s.frameLast.y += 1.f; },
        [](PartnerBrainSave& s) { s.noFrame += 0.5; },
        [](PartnerBrainSave& s) { s.engageFor += 0.5; },
        [](PartnerBrainSave& s) { s.lastStrike += 1.0; },
        [](PartnerBrainSave& s) { s.lastGuard += 1.0; },
        [](PartnerBrainSave& s) { s.lastMove += 1.0; },
        [](PartnerBrainSave& s) { s.guarding = !s.guarding; },
        [](PartnerBrainSave& s) { s.out.framed = !s.out.framed; },
        [](PartnerBrainSave& s) { s.out.frameFallback = !s.out.frameFallback; },
        [](PartnerBrainSave& s) { s.t.circling = !s.t.circling; },
    };
    int j = 0;
    for (const Edit e : framedEdits) {
        CAPTURE(j++);
        PartnerBrainSave s = fb;
        e(s);
        CHECK(s.hash() != fb.hash());
        PartnerBrain c;
        c.load(s);
        CHECK(c.hash() == s.hash());
        PartnerBrainSave u = base; // not framed: unseen
        e(u);
        CHECK(u.hash() == base.hash());
    }
    // The report (idle seconds, fallback seconds, the intent's idle) is never hashed.
    PartnerBrainSave r = fb;
    r.idleSeconds[static_cast<std::size_t>(PartnerIdle::Pause)] += 3.0;
    r.fallbackSeconds += 1.0;
    r.out.idle = PartnerIdle::NoBudget;
    CHECK(r.hash() == fb.hash());
}

namespace {
bool acts(const PartnerIntent& it) { return it.act == PartnerAct::Attack || it.act == PartnerAct::MoveTo; }
} // namespace

TEST_CASE("partner brain: rolesSwapped: the new body acts within 0.5 s, mid-string or mid-pause, where carrying on would idle") {
    const double dt = 1.0 / 60.0;
    for (const bool inPause : {false, true}) {
        for (const std::uint32_t seed : {3u, 7u, 42u, 2026u}) {
            CAPTURE(inPause);
            CAPTURE(seed);
            Scene s(seed);
            Host h;
            int k = 0;
            // Into a string (and, for the pause case, on until its pause begins).
            for (; k < 600; ++k) {
                h.feed(s.in);
                h.apply(s.step(dt), s.b, s.in, dt);
                if (inPause ? s.b.pausing() : (s.b.stringing() && s.b.committedLeft() > 0.3)) break;
            }
            REQUIRE(k < 600);
            const std::uint32_t draws = s.b.rhythmDraws();
            PartnerBrain carried;
            carried.load(s.b.save()); // the same brain, never told
            s.b.rolesSwapped();
            CHECK(s.b.swaps() == 1u);
            CHECK_FALSE(s.b.stringing());
            CHECK_FALSE(s.b.pausing());
            CHECK(s.b.committedLeft() == 0.0);
            CHECK(s.b.target() == -1);
            // The bodies change places: the brain's is the old lead's, idle, 1.5 m off the target's edge (in reach).
            std::swap(s.in.pos, s.in.leadPos);
            Host fresh, old = h;
            PartnerInput other = s.in;
            double first = -1.0, firstCarried = -1.0;
            for (int j = 0; j < 60; ++j) {
                fresh.feed(s.in);
                const PartnerIntent& it = s.step(dt);
                if (first < 0.0 && acts(it)) first = j * dt;
                fresh.apply(it, s.b, s.in, dt);
                // The carried brain on the same new body: it runs out the old string or pause first.
                old.feed(other);
                other.enemies = std::span<const V>(s.e.data(), s.n);
                const PartnerIntent& c = carried.step(dt, other);
                if (firstCarried < 0.0 && acts(c) && c.blowStart) firstCarried = j * dt;
                old.apply(c, carried, other, dt);
            }
            MESSAGE("first act after the swap ", first, " s; carried on, the first new blow ", firstCarried, " s");
            CHECK(first >= 0.0);
            CHECK(first <= 0.5);
            CHECK((firstCarried < 0.0 || firstCarried > 0.5)); // without it the new body would wait
            CHECK(fresh.strings >= 1);                          // a fresh string from the new position
            CHECK(s.b.rhythmDraws() >= draws + 2u);             // drawn afresh
        }
    }
}

TEST_CASE("partner brain: rolesSwapped keeps the budget ring: every 20 s window holds across the swap") {
    struct Case {
        bool partnerAims;
        double period;
    };
    const Case cases[] = {{true, 5.0}, {false, 5.0}, {false, 2.0}};
    for (const Case& c : cases) {
        for (const double at : {11.0, 23.37, 40.0}) {
            for (const PartnerOrder order : {PartnerOrder::Spread, PartnerOrder::StayClose}) {
                for (const std::uint32_t seed : {1u, 42u, 2026u}) {
                    CAPTURE(c.partnerAims);
                    CAPTURE(c.period);
                    CAPTURE(at);
                    CAPTURE(static_cast<int>(order));
                    CAPTURE(seed);
                    Fight f;
                    f.partnerAims = c.partnerAims;
                    f.period = c.period;
                    f.swapAt = at;
                    REQUIRE(f.run(1.0 / 60.0, order, seed, true));
                    REQUIRE(f.swapStep > 0);
                    CHECK(f.brain.swaps() == 1u);
                    CHECK(worstExcess(f.partnerStep, f.leadStep, 1200) <= 1e-3);
                    CHECK(f.host.overlaps == 0);
                    // It fights on after the swap.
                    int after = 0;
                    for (std::size_t i = f.swapStep; i < f.intents.size(); ++i)
                        after += f.intents[i].newString ? 1 : 0;
                    CHECK(after >= 3);
                }
            }
        }
    }
}

TEST_CASE("partner brain: rolesSwapped is in save, load and the hash") {
    const double dt = 1.0 / 60.0;
    Scene s(11);
    Host h;
    for (int k = 0; k < 150; ++k) {
        h.feed(s.in);
        h.apply(s.step(dt), s.b, s.in, dt);
    }
    PartnerBrain unswapped;
    unswapped.load(s.b.save());
    s.b.rolesSwapped();
    CHECK(s.b.hash() != unswapped.hash());
    CHECK(s.b.save().swaps == 1u);
    // A brain loaded from the swapped save does exactly what the swapped one does from there.
    PartnerBrain copy;
    copy.load(s.b.save());
    CHECK(copy.hash() == s.b.hash());
    CHECK(copy.swaps() == 1u);
    std::swap(s.in.pos, s.in.leadPos);
    PartnerInput in2 = s.in;
    Host h1, h2;
    bool same1 = true;
    for (int k = 0; k < 600; ++k) {
        h1.feed(s.in);
        const PartnerIntent& a = s.step(dt);
        h1.apply(a, s.b, s.in, dt);
        h2.feed(in2);
        in2.enemies = std::span<const V>(s.e.data(), s.n);
        const PartnerIntent& b = copy.step(dt, in2);
        h2.apply(b, copy, in2, dt);
        same1 = same1 && same(a, b) && s.b.hash() == copy.hash();
    }
    CHECK(same1);
    // Before begin it does nothing.
    PartnerBrain idle;
    idle.rolesSwapped();
    CHECK(idle.swaps() == 0u);
}

TEST_CASE("partner brain: rolesSwapped with the budget spent: a MoveTo within 0.5 s, Budget holds, then an Attack when the budget allows") {
    const double dt = 1.0 / 60.0;
    for (const PartnerOrder order : {PartnerOrder::Spread, PartnerOrder::StayClose}) {
        CAPTURE(static_cast<int>(order));
        PartnerTuning t; // the budget as shipped: 0.6 x the lead's attack time plus 2 s, any 20 s
        PartnerBrain b;
        REQUIRE(b.begin(t, 9));
        std::array<V, 1> e{};
        e[0].pos = {2.5f, 0.f};
        e[0].radius = 0.5f;
        PartnerInput in;
        in.order = order;
        in.leadPos = {-1.f, 0.f}; // out of reach (3 m from the edge)
        in.leadTarget = 0;
        in.leadAttacking = false; // the lead never attacks: the allowance is all it has
        in.pos = {1.2f, 0.f};     // in reach
        Host h;
        std::vector<double> partner, lead;
        std::vector<PartnerIntent> its;
        const auto stepOnce = [&]() {
            h.feed(in);
            in.enemies = std::span<const V>(e.data(), 1);
            const PartnerIntent it = b.step(dt, in);
            h.apply(it, b, in, dt);
            const PartnerBrainSave sv = b.save();
            partner.push_back(std::clamp(sv.committedEnd - (sv.now - dt), 0.0, dt));
            lead.push_back(0.0);
            its.push_back(it);
            return it;
        };
        // Spend it: strings until the budget holds one.
        int k = 0;
        for (; k < 1800; ++k)
            if (stepOnce().why == PartnerWhy::Budget) break;
        REQUIRE(k < 1800);
        // Let any blow begun run out, then swap: the new body is the lead's, out of reach.
        for (int j = 0; j < 120; ++j) stepOnce();
        b.rolesSwapped();
        std::swap(in.pos, in.leadPos);
        h.blowLeft = h.hurtLeft = 0.0;
        const std::size_t at = its.size();
        for (int j = 0; j < 1800; ++j) stepOnce();
        double move = -1.0, hold = -1.0, attack = -1.0;
        for (std::size_t i = at; i < its.size(); ++i) {
            const double when = static_cast<double>(i - at) * dt;
            if (move < 0.0 && its[i].act == PartnerAct::MoveTo) move = when;
            if (hold < 0.0 && its[i].why == PartnerWhy::Budget) hold = when;
            if (attack < 0.0 && its[i].act == PartnerAct::Attack && its[i].newString) attack = when;
        }
        MESSAGE("after the swap: MoveTo " << move << " s, Budget " << hold << " s, Attack " << attack << " s");
        CHECK(move >= 0.0);
        CHECK(move <= 0.5);
        CHECK(hold > move);      // in reach, the budget holds it
        CHECK(attack > hold);    // and lets it go once the window frees
        CHECK(attack <= 20.0);
        CHECK(worstExcess(partner, lead, 1200, 0.6, 2.0) <= 1e-3);
        CHECK(h.overlaps == 0);
    }
}

// Rule 2's spentSeconds: attack time outside its strings (a paired counter's 1.0 s), on the partner's side.
namespace {

// The worst excess over share x lead + allow of the windows of `w` steps that would hold any of a blow of `len`
// beginning at step `k`, counting `partner` and `lead` before k (the lead attacking no more).
double worstAtBlow(const std::vector<double>& partner, const std::vector<double>& lead, std::size_t k, double len,
                   double dt, std::size_t w) {
    const std::size_t m = static_cast<std::size_t>(std::ceil(len / dt - 1e-9));
    const std::size_t n = k + m;
    std::vector<double> p(n + 1, 0.0), l(n + 1, 0.0); // prefix sums
    double rest = len;
    for (std::size_t j = 0; j < n; ++j) {
        double pj = 0.0;
        if (j < k) {
            pj = partner[j];
        } else {
            pj = std::min(dt, rest);
            rest -= pj;
        }
        p[j + 1] = p[j] + pj;
        l[j + 1] = l[j] + (j < k ? lead[j] : 0.0);
    }
    double worst = -1e9;
    for (std::size_t e = k; e + 1 < n + w; ++e) { // windows (e - w, e], clipped to the fight and the blow's end
        const std::size_t from = e + 1 > w ? e + 1 - w : 0;
        if (from > n - 1) break;
        const std::size_t to = std::min(e, n - 1) + 1;
        worst = std::max(worst, (p[to] - p[from]) - 0.6 * (l[to] - l[from]) - 2.0);
    }
    return worst;
}

struct CounterRun {
    std::vector<double> partner, lead; // each step's attack seconds (the counters' where they ran)
    std::vector<double> ctr;           // each step's counter seconds alone (also in partner)
    std::vector<std::pair<std::size_t, std::size_t>> spans; // each counter's first and last step with its time
    std::vector<std::size_t> landed;   // each counter's landing step
    double own = 0.0;                  // the partner's own blows
    double worstAtStart = -1e9;        // the worst window as each of its blows began
    int held = 0;
    int strings = 0;
    int overlaps = 0;
};

// 60 s against a boss: the lead attacks 3 s of each 5, a paired counter lands every 3 s from 2 s (1.0 s, back from
// now into the steps its own blows left free). `tell`: the brain is given them (spentSeconds); else it never is.
CounterRun runCounters(bool tell) {
    const double dt = 1.0 / 60.0;
    CounterRun r;
    PartnerBrain brain;
    REQUIRE(brain.begin(PartnerTuning{}, 42));
    Host host;
    std::array<V, 1> boss{};
    boss[0].kind = V::Kind::Boss;
    boss[0].radius = 1.f;
    PartnerInput in;
    in.pos = {0.f, 2.5f};
    in.leadPos = {1.8f, 0.f};
    in.leadTarget = 0;
    in.enemies = std::span<const V>(boss.data(), 1);
    for (int k = 0; k < 3600; ++k) {
        const bool leadOn = k % 300 < 180;
        in.leadAttacking = leadOn;
        boss[0].leadAttacking = leadOn;
        const bool counter = k >= 120 && (k - 120) % 180 == 0;
        if (counter) {
            double left = 1.0;
            std::size_t first = r.partner.size(), last = 0;
            for (std::size_t j = r.partner.size(); j-- > 0 && left > 1e-12;) {
                const double p = std::min(std::max(0.0, dt - r.partner[j]), left);
                r.partner[j] += p;
                r.ctr[j] += p;
                left -= p;
                if (p > 0.0) {
                    first = j;
                    last = std::max(last, j);
                }
            }
            if (left > 1e-12 && !r.partner.empty()) {
                r.partner.back() += left;
                r.ctr.back() += left;
                last = r.partner.size() - 1;
                first = std::min(first, last);
            }
            r.spans.emplace_back(first, last);
            r.landed.push_back(static_cast<std::size_t>(k));
        }
        in.spentSeconds = tell && counter ? 1.f : 0.f;
        host.feed(in);
        const PartnerIntent& it = brain.step(dt, in);
        if (it.why == PartnerWhy::Budget) ++r.held;
        if (it.act == PartnerAct::Attack && it.blowStart)
            r.worstAtStart =
                std::max(r.worstAtStart, worstAtBlow(r.partner, r.lead, r.partner.size(),
                                                     host.lengths[static_cast<std::size_t>(it.blowIndex)], dt, 1200));
        const double was = host.attackTime;
        host.apply(it, brain, in, dt);
        r.partner.push_back(host.attackTime - was);
        r.ctr.push_back(0.0);
        r.lead.push_back(leadOn ? dt : 0.0);
    }
    r.own = host.attackTime;
    r.strings = host.strings;
    r.overlaps = host.overlaps;
    return r;
}

std::size_t firstString(const std::vector<PartnerIntent>& v) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].act == PartnerAct::Attack && v[i].newString) return i;
    return kSame;
}

} // namespace

TEST_CASE("partner brain: a paired counter's 1.0 s (spentSeconds) makes the next string wait as a 1.0 s string would") {
    // The ruled budget, the lead not attacking at first: 2.0 s a window. Blows of 1.234 s (no tie with the slots or
    // the lead's steps). A: the host reports 1.0 s of its own attack (doing Attacking, no target: no string, no
    // draw). B: that second as a paired counter, on the step after it. C: neither.
    const double dt = 1.0 / 60.0;
    Scene a(5, PartnerTuning{}, true), b(5, PartnerTuning{}, true), c(5, PartnerTuning{}, true);
    std::vector<PartnerIntent> ia, ib, ic;
    for (int k = 0; k < 600; ++k) {
        for (Scene* s : {&a, &b, &c}) {
            s->n = k < 60 ? 0 : 1;         // the target comes in reach after that second
            s->in.leadAttacking = k >= 90; // the lead's attack lets it go, not the window's end
            s->in.doing = PartnerDoing::Idle;
            s->in.spentSeconds = 0.f;
            s->in.blowSeconds.fill(1.234f);
        }
        if (k < 60) a.in.doing = PartnerDoing::Attacking;
        if (k == 60) b.in.spentSeconds = 1.f;
        ia.push_back(a.step(dt));
        ib.push_back(b.step(dt));
        ic.push_back(c.step(dt));
    }
    CHECK(firstString(ic) == 60u); // with nothing spent, at once
    CHECK(ia[60].why == PartnerWhy::Budget);
    CHECK(ib[60].why == PartnerWhy::Budget);
    const std::size_t at = firstString(ia);
    REQUIRE(at != kSame);
    CHECK(firstString(ib) == at);
    CHECK(at > 90u);
    CHECK(at < 200u);
    // Exactly as A from the counter's step on.
    CHECK(firstDiff(std::vector<PartnerIntent>(ia.begin() + 60, ia.end()),
                    std::vector<PartnerIntent>(ib.begin() + 60, ib.end())) == kSame);
    CHECK(a.b.rhythmDraws() == b.b.rhythmDraws());
}

TEST_CASE("partner brain: with paired counters at the 3 s cooldown every window holds as each blow begins") {
    const CounterRun told = runCounters(true);
    const CounterRun blind = runCounters(false);
    MESSAGE("told: own " << told.own << " s, worst at a start " << told.worstAtStart << ", held " << told.held
                         << "; blind: own " << blind.own << " s, worst " << blind.worstAtStart);
    // Told, every blow it begins fits every window with the counters so far (the brain places a counter by 0.05 s
    // slot, this ledger by step: they may differ by one slot at a window's edge).
    CHECK(told.worstAtStart <= PartnerBrainSave::kSlot + 1e-3);
    CHECK(blind.worstAtStart > 0.5); // never told, it would carry windows over
    CHECK(told.held > 0);
    CHECK(told.own < blind.own);
    CHECK(told.strings > 1); // it still fights
    CHECK(told.overlaps == 0);
}

TEST_CASE("partner brain: with paired counters at the 3 s cooldown the ruled bound holds over every window") {
    // The ruled bound: over any 20 s window, the partner's attack time is at most 0.60 x the lead's plus 2.0 s, plus
    // at most 1.0 s for each paired counter whose time falls in it. At the cooldown, 7 land in some window.
    struct Worst {
        double base = -1e9;       // over 0.60 x lead + 2.0
        double ruled = -1e9;      // over that plus the counter seconds in the window
        double perCounter = -1e9; // over that plus 1.0 s for each counter with time in the window
        std::size_t landed = 0;   // the most counters landed in one window
    };
    const auto worstOf = [](const CounterRun& r) {
        const std::size_t w = 1200; // 20 s of 1/60 s steps
        const std::size_t n = r.partner.size();
        std::vector<double> p(n + 1, 0.0), l(n + 1, 0.0), c(n + 1, 0.0); // prefix sums
        for (std::size_t j = 0; j < n; ++j) {
            p[j + 1] = p[j] + r.partner[j];
            l[j + 1] = l[j] + r.lead[j];
            c[j + 1] = c[j] + r.ctr[j];
        }
        Worst out;
        for (std::size_t e = 0; e + 1 < n + w; ++e) { // windows (e - w, e], clipped to the fight
            const std::size_t from = e + 1 > w ? e + 1 - w : 0;
            if (from >= n) break;
            const std::size_t to = std::min(e, n - 1) + 1; // steps [from, to)
            const double base = (p[to] - p[from]) - 0.6 * (l[to] - l[from]) - 2.0;
            std::size_t ran = 0, landedIn = 0;
            for (std::size_t i = 0; i < r.spans.size(); ++i) {
                if (r.spans[i].second >= from && r.spans[i].first < to) ++ran;
                if (r.landed[i] >= from && r.landed[i] < to) ++landedIn;
            }
            out.base = std::max(out.base, base);
            out.ruled = std::max(out.ruled, base - (c[to] - c[from]));
            out.perCounter = std::max(out.perCounter, base - 1.0 * static_cast<double>(ran));
            out.landed = std::max(out.landed, landedIn);
        }
        return out;
    };
    const CounterRun told = runCounters(true);
    const CounterRun blind = runCounters(false);
    const Worst t = worstOf(told);
    const Worst b = worstOf(blind);
    MESSAGE("told: over the base " << t.base << ", over the ruled " << t.ruled << ", over 1.0 s a counter "
                                   << t.perCounter << "; blind: over the base " << b.base);
    const double slack = PartnerBrainSave::kSlot + 1e-3; // the brain's 0.05 s slots against this ledger's steps
    CHECK(t.landed == 7u);
    CHECK(t.ruled <= slack);
    CHECK(t.perCounter <= slack);
    CHECK(t.base < b.base); // it pays the counters back: never told, it carries windows further over
    CHECK(told.held > 0);
}

TEST_CASE("partner brain: spentSeconds of 0, not finite or under 0 is today's brain; above the window is the window") {
    const auto run = [](float spent, int at) {
        Scene s(5, PartnerTuning{}, true);
        s.in.leadAttacking = false;
        std::vector<std::uint64_t> h;
        for (int k = 0; k < 900; ++k) {
            s.in.spentSeconds = at < 0 || k == at ? spent : 0.f;
            s.in.doing = s.b.committedLeft() > 0.0 ? PartnerDoing::Attacking : PartnerDoing::Idle;
            s.step(1.0 / 60.0);
            h.push_back(s.b.hash());
        }
        return h;
    };
    const std::vector<std::uint64_t> today = run(0.f, -1);
    for (const float bad : {kNaN, -1.f, -std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::infinity(), -0.f}) {
        CAPTURE(bad);
        CHECK(run(bad, -1) == today); // every step's hash: byte-identical
    }
    CHECK(PartnerInput{}.spentSeconds == 0.f); // the default is zero
    // Capped at budgetWindowSeconds (20).
    CHECK(run(1.0e6f, 300) == run(20.f, 300));
    CHECK(run(5.f, 300) != today); // it bites
}
