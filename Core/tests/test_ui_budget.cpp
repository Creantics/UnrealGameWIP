#include "ftcore/UiBudget.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr std::size_t K(UiMarkKind k) { return static_cast<std::size_t>(k); }

ArenaHudFrame fullFrame() {
    ArenaHudFrame f;
    f.lead.hp = f.lead.hpMax = 100;
    f.lead.loadMax = 90;
    f.lead.releaseReady = true;
    f.lead.heals = 2;
    f.lead.roundsMax = 6;
    f.partner.present = true;
    f.partner.hp = f.partner.hpMax = 80;
    f.boss.present = true;
    f.boss.hp = f.boss.hpMax = 500;
    f.boss.slot = 0;
    f.target.present = true;
    f.target.hp = f.target.hpMax = 40;
    f.target.enemy = 1;
    return f;
}

UiBudgetInput busy(const ArenaHudFrame& f) {
    UiBudgetInput in;
    in.dt = 1.f / 60.f;
    in.activity = true;
    in.bossEvent = true;
    in.verbsAvailable = uiVerbsAvailable(f);
    in.want = uiMarkWants(f, in.verbsAvailable);
    return in;
}

} // namespace

TEST_CASE("ui budget: absent data asks for no mark") {
    ArenaHudFrame f; // nothing present
    const auto w = uiMarkWants(f, uiVerbsAvailable(f));
    for (std::uint8_t n : w) CHECK(n == 0);

    // The third place with no data (planned) draws nothing, though it is "present".
    f.lead.hp = f.lead.hpMax = 10;
    f.third.present = true;
    f.third.planned = true;
    CHECK(uiMarkWants(f, 0)[K(UiMarkKind::Party)] == 0);
    f.third.planned = false;
    CHECK(uiMarkWants(f, 0)[K(UiMarkKind::Party)] == 1);

    // No cylinder, no rounds mark; no boss hp, no boss line; the target that is the boss is not a second line.
    ArenaHudFrame g = fullFrame();
    g.lead.roundsMax = 0;
    g.boss.hpMax = 0;
    g.target.enemy = g.boss.slot;
    const auto wg = uiMarkWants(g, 0);
    CHECK(wg[K(UiMarkKind::Rounds)] == 0);
    CHECK(wg[K(UiMarkKind::Boss)] == 0);
    g.boss.hpMax = 500;
    CHECK(uiMarkWants(g, 0)[K(UiMarkKind::Target)] == 0);
}

TEST_CASE("ui budget: verbs available come from the data") {
    ArenaHudFrame f = fullFrame();
    CHECK(uiVerbsAvailable(f) == 0xF);
    f.lead.releaseReady = false;
    f.lead.heals = 0;
    f.partner.down = true;
    CHECK(uiVerbsAvailable(f) == uiVerbBit(UiVerb::Guard));
    f.lead.guardBroken = true;
    CHECK(uiVerbsAvailable(f) == 0);
}

TEST_CASE("ui budget: at rest only the lead and the party show") {
    const UiBudgetRules rules;
    UiBudgetState st;
    ArenaHudFrame f = fullFrame();
    UiBudgetInput in = busy(f);
    in.want[K(UiMarkKind::Popup)] = 2;
    in.want[K(UiMarkKind::Tell)] = 1;
    uiBudgetStep(st, in, rules);
    CHECK_FALSE(st.out.rest);

    // Nothing changes for the rest time (the data still asks for everything).
    in.activity = false;
    in.bossEvent = false;
    in.changed = 0;
    for (int i = 0; i < 60 * 5; ++i) uiBudgetStep(st, in, rules);
    const UiBudgetOut& o = st.out;
    CHECK(o.rest);
    CHECK(o.allowed(UiMarkKind::Lead) == 1);
    CHECK(o.allowed(UiMarkKind::Party) == 1);
    CHECK(o.marks == 2);
    CHECK(o.bossAlpha == 0.f);
    CHECK(o.verbAlpha == 0.f);
    CHECK(o.verbsShown == 0);
    CHECK(o.keyRing == -1);

    // One change wakes it.
    in.changed = 1;
    uiBudgetStep(st, in, rules);
    CHECK_FALSE(st.out.rest);
}

TEST_CASE("ui budget: the boss line fades at rest and returns on a boss event") {
    const UiBudgetRules rules;
    UiBudgetState st;
    ArenaHudFrame f = fullFrame();
    UiBudgetInput in = busy(f);
    for (int i = 0; i < 30; ++i) uiBudgetStep(st, in, rules);
    CHECK(st.out.bossAlpha > 0.9f);

    // The lead keeps acting but the boss does nothing: its line goes after the rest time.
    in.bossEvent = false;
    for (int i = 0; i < 60 * 5; ++i) uiBudgetStep(st, in, rules);
    CHECK_FALSE(st.out.rest);
    CHECK(st.out.bossAlpha == 0.f);
    CHECK(st.out.allowed(UiMarkKind::Boss) == 0);

    in.bossEvent = true;
    for (int i = 0; i < 12; ++i) uiBudgetStep(st, in, rules);
    CHECK(st.out.bossAlpha > 0.5f);
    CHECK(st.out.allowed(UiMarkKind::Boss) == 1);
}

TEST_CASE("ui budget: never more than the mark cap in action, highest priority kept") {
    const UiBudgetRules rules;
    ArenaHudFrame f = fullFrame();
    // Every mix of the widget's own wants.
    for (int tells = 0; tells <= 4; ++tells) {
        for (int pops = 0; pops <= 8; pops += 2) {
            for (int foes = 0; foes <= 6; foes += 3) {
                for (int extra = 0; extra < 8; ++extra) {
                    UiBudgetState st;
                    UiBudgetInput in = busy(f);
                    in.want[K(UiMarkKind::Tell)] = static_cast<std::uint8_t>(tells);
                    in.want[K(UiMarkKind::Popup)] = static_cast<std::uint8_t>(pops);
                    in.want[K(UiMarkKind::Foe)] = static_cast<std::uint8_t>(foes);
                    in.want[K(UiMarkKind::Cards)] = (extra & 1) ? 1 : 0;
                    in.want[K(UiMarkKind::Lock)] = (extra & 2) ? 1 : 0;
                    in.want[K(UiMarkKind::WeakPoint)] = (extra & 4) ? 1 : 0;
                    in.want[K(UiMarkKind::Wall)] = 1;
                    in.prompted = static_cast<std::int8_t>(UiVerb::Heal);
                    for (int t = 0; t < 20; ++t) uiBudgetStep(st, in, rules);
                    const UiBudgetOut& o = st.out;
                    CHECK(o.marks <= rules.marks);
                    CHECK(o.allowed(UiMarkKind::Lead) == 1);
                    CHECK(o.allowed(UiMarkKind::Tell) == tells); // a tell is never dropped
                    int sum = 0;
                    for (std::uint8_t a : o.allow) sum += a;
                    CHECK(sum == o.marks);
                    // The verbs drawn are as many as granted, available ones only.
                    int shown = 0;
                    for (unsigned b = o.verbsShown; b; b &= b - 1) ++shown;
                    CHECK(shown == o.allowed(UiMarkKind::Verb));
                    CHECK((o.verbsShown & ~in.verbsAvailable) == 0);
                    // The prompted verb carries the only key ring whenever any verb shows.
                    if (shown > 0) CHECK(o.keyRing == static_cast<std::int8_t>(UiVerb::Heal));
                    CHECK(st.maxMarks <= rules.marks);
                }
            }
        }
    }
}

TEST_CASE("ui budget: a verb that cannot be used is not drawn, and no key ring without a prompt") {
    const UiBudgetRules rules;
    UiBudgetState st;
    ArenaHudFrame f = fullFrame();
    f.lead.heals = 0;
    f.lead.releaseReady = false;
    UiBudgetInput in = busy(f);
    in.prompted = static_cast<std::int8_t>(UiVerb::Heal); // asked for, but none left
    for (int i = 0; i < 20; ++i) uiBudgetStep(st, in, rules);
    CHECK((st.out.verbsShown & uiVerbBit(UiVerb::Heal)) == 0);
    CHECK((st.out.verbsShown & uiVerbBit(UiVerb::Release)) == 0);
    CHECK(st.out.keyRing == -1);
}

TEST_CASE("ui budget: the result card stands alone") {
    const UiBudgetRules rules;
    UiBudgetState st;
    ArenaHudFrame f = fullFrame();
    UiBudgetInput in = busy(f);
    in.want[K(UiMarkKind::Popup)] = 3;
    in.resultOpen = true;
    uiBudgetStep(st, in, rules);
    CHECK(st.out.marks == 0);
    CHECK(st.out.bossAlpha == 0.f);
}

TEST_CASE("ui popups: at most two per body, repeats coalesce, the result clears them") {
    const UiBudgetRules rules;
    UiPopLanes lanes;
    const std::uint32_t a = uiPopPush(lanes, 3, 7, 7, rules); // SHARED 7 on body 3
    CHECK(lanes.pop[a].count == 1);
    const std::uint32_t a2 = uiPopPush(lanes, 3, 7, 7, rules);
    const std::uint32_t a3 = uiPopPush(lanes, 3, 7, 7, rules);
    CHECK(a2 == a);
    CHECK(a3 == a);
    CHECK(lanes.pop[a].count == 3); // "SHARED 7 x3"
    CHECK(uiPopLiveCount(lanes, 3) == 1);

    // A different amount is its own popup; a third kind pushes the oldest off the body.
    const std::uint32_t b = uiPopPush(lanes, 3, 7, 4, rules);
    CHECK(b != a);
    CHECK(uiPopLiveCount(lanes, 3) == 2);
    CHECK(uiPopLane(lanes, b) == 0);
    CHECK(uiPopLane(lanes, a) == 1);
    const std::uint32_t c = uiPopPush(lanes, 3, 1, 12, rules);
    CHECK(uiPopLiveCount(lanes, 3) == 2);
    CHECK(lanes.pop[c].live);
    CHECK(lanes.pop[b].live);
    // The SHARED 7 popup (the oldest) is gone; its slot may be the new one's.
    for (const UiPop& p : lanes.pop) CHECK_FALSE((p.live && p.body == 3 && p.kind == 7 && p.amount == 7));

    // Other bodies keep their own two.
    uiPopPush(lanes, 0, 1, 5, rules);
    uiPopPush(lanes, 0, 2, 0, rules);
    CHECK(uiPopLiveCount(lanes, 0) == 2);
    CHECK(uiPopLiveCount(lanes, 3) == 2);

    // A coalesced repeat restarts its life.
    uiPopTick(lanes, 1.f, 1.5f);
    uiPopPush(lanes, 3, 1, 12, rules);
    uiPopTick(lanes, 0.8f, 1.5f);
    CHECK(lanes.pop[c].live);
    CHECK_FALSE(lanes.pop[b].live);

    uiPopClear(lanes);
    CHECK(uiPopLiveCount(lanes, 0) == 0);
    CHECK(uiPopLiveCount(lanes, 3) == 0);
}

TEST_CASE("ui popups: never more than two on a body under a storm") {
    const UiBudgetRules rules;
    UiPopLanes lanes;
    for (int i = 0; i < 200; ++i) {
        uiPopPush(lanes, static_cast<std::uint8_t>(i % 5), static_cast<std::uint8_t>(i % 3), i % 7, rules);
        for (std::uint8_t body = 0; body < 5; ++body) CHECK(uiPopLiveCount(lanes, body) <= rules.popupsPerBody);
    }
}

TEST_CASE("ui luma floor lifts dark colours and keeps light ones") {
    const UiColor dark{0.6f, 0.2f, 0.15f, 0.8f};
    const UiColor up = uiLumaFloor(dark, 0.45f);
    CHECK(up.a == doctest::Approx(0.8f));
    CHECK(up.r >= dark.r);
    CHECK(up.g > dark.g);
    // A white text over the lifted colour: its contrast against black is at least that of the floor.
    const UiColor black{0.f, 0.f, 0.f, 1.f};
    CHECK(uiContrast(UiColor{up.r, up.g, up.b, 1.f}, black, black) >= (0.45f + 0.05f) / 0.05f - 0.1f);
    const UiColor light{0.95f, 0.93f, 0.9f, 1.f};
    CHECK(uiLumaFloor(light, 0.45f) == light);
}
