#include "ftcore/PartnerBrain.h"
#include "AllocCounter.h"
#include "TestAssets.h"
#include <doctest/doctest.h>
#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

using V = PartnerEnemyView;

struct Scene {
    PartnerBrain b;
    PartnerInput in;
    std::array<V, kMaxPartnerEnemies> e{};
    std::size_t n = 1;
    explicit Scene(std::uint32_t seed = 7, PartnerTuning t = PartnerTuning{}) {
        REQUIRE(b.begin(t, seed));
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
double chainSeconds(int blows) {
    double s = 0.0;
    for (int i = 0; i < blows; ++i) s += kLight[i % 3];
    return s;
}

// A scripted host: the ally runs the brain's strings for the light chain's time, guards on Guard, moves at 4.5 m/s,
// and a blow aimed at it that lands unguarded hurts it for 0.5 s (no act).
struct Host {
    double attackLeft = 0.0;
    double hurtLeft = 0.0;
    bool guarding = false;
    double attackTime = 0.0;
    int strings = 0;
    int hits = 0;
    int guards = 0;
    std::vector<int> blows;
    std::vector<double> pauses;
    void feed(PartnerInput& in) const {
        in.canAct = hurtLeft <= 0.0;
        in.doing = hurtLeft > 0.0 ? PartnerDoing::Hurt
                   : attackLeft > 0.0 ? PartnerDoing::Attacking
                   : guarding          ? PartnerDoing::Guarding
                                       : PartnerDoing::Idle;
    }
    void apply(const PartnerIntent& it, const PartnerBrain& b, PartnerInput& in, double dt) {
        guarding = it.act == PartnerAct::Guard;
        if (guarding) attackLeft = 0.0;
        if (it.act == PartnerAct::Attack && it.newString) {
            attackLeft = chainSeconds(it.blows);
            ++strings;
            blows.push_back(b.lastBlows());
            pauses.push_back(b.lastPause());
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
        if (attackLeft > 0.0) {
            attackTime += std::min(dt, attackLeft);
            attackLeft -= dt;
        }
        if (hurtLeft > 0.0) hurtLeft -= dt;
    }
    void land() { // a blow aimed at it lands
        if (guarding) {
            ++guards;
        } else {
            ++hits;
            hurtLeft = 0.5;
            attackLeft = 0.0;
        }
    }
};

// A 60 s scripted fight against a boss (radius 1 at the origin); the lead (1.8 m from its centre, never moving) chains
// the default fighter's lights back to back (the chain wraps; chainGap is a window, not a rest). The dummy never winds
// up. With windups: 1.0 s every 5 s from 3 s, aimed at the partner on 3 of each 10 (under the fight's 30% ceiling)
// and at the lead otherwise; the lead guards from a windup aimed at it until 0.4 s after the blow.
struct Fight {
    double leadAttack = 0.0;
    Host host;
    PartnerBrain brain;
    std::vector<int> guardDraws; // per windup aimed at the partner: 1 guard, 0 hit
    bool run(double dt, PartnerOrder order, std::uint32_t seed, bool windups = true, double seconds = 60.0) {
        if (!brain.begin(PartnerTuning{}, seed)) return false;
        std::array<V, 1> boss{};
        boss[0].kind = V::Kind::Boss;
        boss[0].radius = 1.f;
        PartnerInput in;
        in.order = order;
        in.pos = {0.f, 3.5f};
        in.leadPos = {1.8f, 0.f};
        in.leadTarget = 0;
        in.enemies = std::span<const V>(boss.data(), 1);
        double leadGuard = 0.0;
        const int steps = static_cast<int>(std::lround(seconds / dt));
        for (int k = 0; k < steps; ++k) {
            const double now = k * dt;
            // The boss: windup index w runs [3 + 5w, 4 + 5w).
            const double since = now - 3.0;
            const int w = since >= 0.0 ? static_cast<int>(std::floor(since / 5.0 + 1e-9)) : -1;
            const double into = w >= 0 ? since - 5.0 * w : -1.0;
            const bool atPartner = w >= 0 && (w % 10 == 2 || w % 10 == 5 || w % 10 == 9);
            const bool winding = windups && w >= 0 && into < 1.0 - 1e-9;
            boss[0].state = winding ? V::State::WindingUp : V::State::Idle;
            boss[0].windupLeft = winding ? static_cast<float>(1.0 - into) : 0.f;
            boss[0].aim = winding ? (atPartner ? V::Aim::Partner : V::Aim::Lead) : V::Aim::None;
            // The lead.
            if (winding && !atPartner) leadGuard = 1.4 - into;
            const bool leadOn = !(leadGuard > 0.0);
            if (leadOn) leadAttack += dt;
            else leadGuard -= dt;
            boss[0].leadAttacking = leadOn;
            host.feed(in);
            const PartnerIntent& it = brain.step(dt, in);
            host.apply(it, brain, in, dt);
            // The blow lands on the windup's last step.
            if (winding && atPartner && into + dt >= 1.0 - 1e-9) {
                guardDraws.push_back(host.guarding ? 1 : 0);
                host.land();
            }
        }
        return true;
    }
};

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
             R"({"version":1,"returnMeters":5})",
             R"({"version":1,"reachMeters":5})",
             R"({"version":1,"standoffMeters":2})",
             R"({"version":1,"blowsMin":0})",
             R"({"version":1,"blowsMin":4})",
             R"({"version":1,"blowsMax":7})",
             R"({"version":1,"blowsMax":2.5})",
             R"({"version":1,"blowSeconds":0})",
             R"({"version":1,"pauseMin":2.5})",
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

TEST_CASE("partner brain: rule 2, strings of 2 or 3 blows, a 1.2..2.0 s pause after, on the host's clock") {
    Scene s(11);
    s.in.order = PartnerOrder::Spread;
    s.in.pos = {0.f, 1.5f};
    const double dt = 0.05;
    int twos = 0, threes = 0;
    PartnerIntent a = s.step(dt);
    for (int k = 0; k < 40; ++k) {
        REQUIRE(a.act == PartnerAct::Attack);
        REQUIRE(a.newString);
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
        // The host reports Attacking for 2.0 s, past blows x blowSeconds (1.3 / 1.95 s): the string lasts as long.
        s.in.doing = PartnerDoing::Attacking;
        double t = dt;
        for (; t < 2.0 - 1e-9; t += dt) {
            const PartnerIntent& c = s.step(dt);
            REQUIRE(c.act == PartnerAct::Attack);
            CHECK_FALSE(c.newString);
            CHECK(c.why == PartnerWhy::String);
        }
        s.in.doing = PartnerDoing::Idle;
        // The pause runs from the first step the host stops reporting it.
        double paused = 0.0;
        for (;;) {
            const PartnerIntent& p = s.step(dt);
            if (p.why != PartnerWhy::Pause) {
                a = p; // the step that ends the pause starts the next string
                break;
            }
            CHECK(p.act == PartnerAct::Wait);
            paused += dt;
            REQUIRE(paused < 3.0);
        }
        CHECK(paused >= pause - dt - 1e-6);
        CHECK(paused <= pause + dt + 1e-6);
        REQUIRE(s.b.stringing());
    }
    CHECK(twos > 5);
    CHECK(threes > 5);

    // A host that never reports Attacking: the fallback clock, blows x blowSeconds.
    Scene f(3);
    f.in.order = PartnerOrder::Spread;
    f.in.pos = {0.f, 1.5f};
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
    // A host that never stops: 2 x that.
    Scene g(3);
    g.in.order = PartnerOrder::Spread;
    g.in.pos = {0.f, 1.5f};
    REQUIRE(g.step(0.01).newString);
    g.in.doing = PartnerDoing::Attacking;
    t = 0.0;
    while (g.b.stringing()) {
        g.step(0.01);
        t += 0.01;
        REQUIRE(t < 10.0);
    }
    CHECK(t == doctest::Approx(2.0 * full).epsilon(0.02));
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
    s.in.doing = PartnerDoing::Attacking;
    s.step(0.05);
    s.in.doing = PartnerDoing::Idle;
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
    CHECK(a.host.blows == b.host.blows);
    CHECK(a.host.pauses == b.host.pauses);
    CHECK(a.guardDraws == b.guardDraws);
    REQUIRE(c.run(1.0 / 60.0, PartnerOrder::Spread, 43));
    CHECK(c.host.pauses != a.host.pauses);
    // At half the step: the same draws (one rhythm pair per string, one defence draw per windup aimed at it).
    REQUIRE(d.run(1.0 / 120.0, PartnerOrder::Spread, 42));
    CHECK(a.brain.rhythmDraws() == 2u * static_cast<std::uint32_t>(a.host.strings));
    CHECK(d.brain.rhythmDraws() == 2u * static_cast<std::uint32_t>(d.host.strings));
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

TEST_CASE("partner brain: over a 60 s scripted fight against a dummy boss its attack time is 35..55% of the lead's") {
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
            CHECK(share <= 0.55);
        }
    }
}

TEST_CASE("partner brain: a 60 s fight with windups keeps every rule (its share reported, not asserted)") {
    for (const std::uint32_t seed : {1u, 42u, 2026u}) {
        CAPTURE(seed);
        Fight f;
        REQUIRE(f.run(1.0 / 60.0, PartnerOrder::Spread, seed, true));
        MESSAGE("windups: partner ", f.host.attackTime, " s, lead ", f.leadAttack, " s, share ",
                f.host.attackTime / f.leadAttack, ", strings ", f.host.strings, ", guards ", f.host.guards, ", hits ",
                f.host.hits);
        CHECK(f.brain.defenceDraws() == 3u);
        CHECK(f.host.guards + f.host.hits == 3);
        CHECK(f.host.strings > 10);
        CHECK(f.brain.rhythmDraws() == 2u * static_cast<std::uint32_t>(f.host.strings));
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
    p.in.doing = PartnerDoing::Attacking;
    p.step(0.05);
    p.in.doing = PartnerDoing::Idle;
    p.step(0.05);
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
