#include "ftcore/UiPartyScreen.h"
#include "ftcore/FightSession.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

struct Pad {
    bool explore = false;
    bool toggle = false;
    bool live = true;
    bool party = false;
    bool map = false;
    float stick = 0.f;
    float mouse = 0.f;

    UiPartyInput in() const {
        UiPartyInput i;
        i.live = live;
        i.explore = explore;
        i.partyDown = party;
        i.mapDown = map;
        i.toggle = toggle;
        i.stickY = stick;
        i.mouseY = mouse;
        return i;
    }
};

UiPartyOut step(UiPartyScreen& s, const Pad& p, float dt = kDt) { return uiPartyScreenStep(s, p.in(), dt); }

// Steps `seconds` with the same input; true when any step picked a row other than the lead's (`pick` the last).
struct Ran {
    int opened = 0, closed = 0, maps = 0, picks = 0;
    UiPartyRow pick = UiPartyRow::Lead;
};
Ran run(UiPartyScreen& s, const Pad& p, float seconds) {
    Ran r;
    for (float t = 0.f; t < seconds - 1e-4f; t += kDt) {
        const UiPartyOut o = step(s, p);
        r.opened += o.opened ? 1 : 0;
        r.closed += o.closed ? 1 : 0;
        r.maps += o.map ? 1 : 0;
        if (o.picked()) {
            ++r.picks;
            r.pick = o.pick;
        }
    }
    return r;
}

// A fresh screen past its first step (the start's keys read as held until seen up).
UiPartyScreen fresh(const Pad& idle) {
    UiPartyScreen s;
    step(s, idle);
    return s;
}

} // namespace

TEST_CASE("ui party: a fight opens on Party's key or the map key going down, on the lead") {
    for (int key = 0; key < 2; ++key) {
        Pad p;
        UiPartyScreen s = fresh(p);
        CHECK_FALSE(s.open);
        (key == 0 ? p.party : p.map) = true;
        const UiPartyOut o = step(s, p);
        CHECK(o.opened);
        CHECK_FALSE(o.closed);
        CHECK(s.open);
        CHECK(s.cursor == UiPartyRow::Lead);
        // Held, it stays open; let go with no flick, it shuts on the lead: nothing picked.
        CHECK(run(s, p, 0.5f).closed == 0);
        (key == 0 ? p.party : p.map) = false;
        const UiPartyOut shut = step(s, p);
        CHECK(shut.closed);
        CHECK(shut.pick == UiPartyRow::Lead);
        CHECK_FALSE(shut.picked());
        CHECK_FALSE(shut.map);
        CHECK_FALSE(s.open);
    }
}

TEST_CASE("ui party: a flick up picks the partner on the release, once") {
    Pad p;
    UiPartyScreen s = fresh(p);
    p.party = true;
    step(s, p);
    p.stick = 0.9f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Partner);
    // Held past the flick: one step, no repeat; the top stays (the Third's row never takes the cursor).
    run(s, p, 1.f);
    CHECK(s.cursor == UiPartyRow::Partner);
    p.stick = 0.f;
    step(s, p);
    p.stick = 0.9f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Partner);
    p.party = false;
    const UiPartyOut o = step(s, p);
    CHECK(o.picked());
    CHECK(o.pick == UiPartyRow::Partner);
    const Ran after = run(s, p, 0.5f);
    CHECK(after.picks == 0);
    CHECK(after.closed == 0);
}

TEST_CASE("ui party: flicking back onto the lead picks nothing") {
    Pad p;
    UiPartyScreen s = fresh(p);
    p.map = true;
    step(s, p);
    p.stick = 0.9f;
    step(s, p);
    p.stick = 0.f;
    step(s, p);
    p.stick = -0.9f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Lead);
    // In a fight there is no Follow row: the bottom stays on the lead.
    p.stick = 0.f;
    step(s, p);
    p.stick = -0.9f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Lead);
    p.map = false;
    const UiPartyOut o = step(s, p);
    CHECK(o.closed);
    CHECK_FALSE(o.picked());
}

TEST_CASE("ui party: explore's map key, tapped, is the map; held kUiPartyHold, the screen") {
    Pad p;
    p.explore = true;
    UiPartyScreen s = fresh(p);
    // A tap under the hold: the map on letting go, no screen.
    p.map = true;
    Ran r = run(s, p, kUiPartyHold - 0.05f);
    CHECK(r.opened == 0);
    CHECK_FALSE(s.open);
    p.map = false;
    UiPartyOut o = step(s, p);
    CHECK(o.map);
    CHECK_FALSE(o.opened);
    CHECK_FALSE(o.closed);
    CHECK(run(s, p, 0.5f).maps == 0);

    // Held: it opens at the hold, on the lead, and its release is no map.
    p.map = true;
    r = run(s, p, kUiPartyHold + 2.f * kDt);
    CHECK(r.opened == 1);
    CHECK(s.open);
    CHECK(s.cursor == UiPartyRow::Lead);
    p.map = false;
    o = step(s, p);
    CHECK(o.closed);
    CHECK_FALSE(o.map);
    CHECK_FALSE(o.picked());
    CHECK(run(s, p, 0.5f).maps == 0);
}

TEST_CASE("ui party: explore opens at the hold's threshold, not before") {
    Pad p;
    p.explore = true;
    UiPartyScreen s = fresh(p);
    p.map = true;
    float held = 0.f;
    bool opened = false;
    while (!opened && held < 1.f) {
        opened = step(s, p, 0.01f).opened;
        held += 0.01f;
    }
    CHECK(opened);
    CHECK(held >= kUiPartyHold - 1e-3f);
    CHECK(held <= kUiPartyHold + 0.02f);
}

TEST_CASE("ui party: Party's own key opens explore's screen at once, and is never the map") {
    Pad p;
    p.explore = true;
    UiPartyScreen s = fresh(p);
    p.party = true;
    CHECK(step(s, p).opened);
    p.party = false;
    const UiPartyOut o = step(s, p);
    CHECK(o.closed);
    CHECK_FALSE(o.map);
}

TEST_CASE("ui party: explore's rows end with Follow, which a flick down reaches and picks") {
    Pad p;
    p.explore = true;
    UiPartyScreen s = fresh(p);
    p.party = true;
    step(s, p);
    p.mouse = -12.f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Follow);
    // The mouse still moving fast is the same flick; slowed, it counts again, and the bottom stays.
    run(s, p, 0.3f);
    CHECK(s.cursor == UiPartyRow::Follow);
    p.mouse = 0.f;
    step(s, p);
    p.mouse = -12.f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Follow);
    // Up twice: the lead, then the partner; never the Third.
    p.mouse = 0.f;
    step(s, p);
    p.mouse = 12.f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Lead);
    p.mouse = 0.f;
    step(s, p);
    p.mouse = 12.f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Partner);
    p.mouse = 0.f;
    step(s, p);
    p.mouse = 12.f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Partner);
    // Back down to Follow and let go: Follow is picked.
    for (int i = 0; i < 2; ++i) {
        p.mouse = 0.f;
        step(s, p);
        p.mouse = -12.f;
        step(s, p);
    }
    CHECK(s.cursor == UiPartyRow::Follow);
    p.party = false;
    p.mouse = 0.f;
    const UiPartyOut o = step(s, p);
    CHECK(o.picked());
    CHECK(o.pick == UiPartyRow::Follow);
}

TEST_CASE("ui party: a stick pushed as the screen opens moves nothing until it has come back") {
    Pad p;
    UiPartyScreen s = fresh(p);
    p.stick = 0.9f;
    step(s, p);
    p.party = true;
    step(s, p);
    run(s, p, 0.5f);
    CHECK(s.cursor == UiPartyRow::Lead);
    p.stick = 0.1f;
    step(s, p);
    p.stick = 0.9f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Partner);
}

TEST_CASE("ui party: each opening starts on the lead") {
    Pad p;
    UiPartyScreen s = fresh(p);
    p.party = true;
    step(s, p);
    p.stick = 0.9f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Partner);
    p.party = false;
    p.stick = 0.f;
    CHECK(step(s, p).picked());
    p.party = true;
    CHECK(step(s, p).opened);
    CHECK(s.cursor == UiPartyRow::Lead);
}

TEST_CASE("ui party: under Toggle the release keeps it open and the next press confirms") {
    for (int explore = 0; explore < 2; ++explore) {
        Pad p;
        p.toggle = true;
        p.explore = explore == 1;
        UiPartyScreen s = fresh(p);
        p.map = true;
        run(s, p, kUiPartyHold + 0.05f);
        REQUIRE(s.open);
        p.map = false;
        CHECK(run(s, p, 1.f).closed == 0);
        CHECK(s.open);
        p.stick = 0.9f;
        step(s, p);
        p.stick = 0.f;
        step(s, p);
        CHECK(s.cursor == UiPartyRow::Partner);
        p.map = true;
        const UiPartyOut o = step(s, p);
        CHECK(o.closed);
        CHECK(o.pick == UiPartyRow::Partner);
        // The confirming press's hold and release open nothing and are no map.
        const Ran held = run(s, p, 1.f);
        CHECK(held.opened == 0);
        p.map = false;
        CHECK_FALSE(step(s, p).map);
        CHECK_FALSE(s.open);
    }
}

TEST_CASE("ui party: taken away, it shuts with no pick, and a key still held is no press") {
    Pad p;
    UiPartyScreen s = fresh(p);
    p.party = true;
    step(s, p);
    p.stick = 0.9f;
    step(s, p);
    REQUIRE(s.cursor == UiPartyRow::Partner);
    p.live = false;
    const UiPartyOut o = step(s, p);
    CHECK(o.closed);
    CHECK_FALSE(o.picked());
    CHECK_FALSE(s.open);
    // Back with Party still down: no opening until it is let go and pressed again.
    p.live = true;
    p.stick = 0.f;
    CHECK(run(s, p, 0.5f).opened == 0);
    p.party = false;
    CHECK_FALSE(step(s, p).closed);
    p.party = true;
    CHECK(step(s, p).opened);
}

TEST_CASE("ui party: explore's map hold cut by a pause is neither the screen nor the map") {
    Pad p;
    p.explore = true;
    UiPartyScreen s = fresh(p);
    p.map = true;
    run(s, p, 0.1f);
    p.live = false;
    step(s, p);
    p.live = true;
    const Ran r = run(s, p, 1.f);
    CHECK(r.opened == 0);
    p.map = false;
    CHECK_FALSE(step(s, p).map);
}

TEST_CASE("ui party: a key down at the start is no press") {
    Pad p;
    p.party = true;
    p.map = true;
    UiPartyScreen s;
    CHECK(run(s, p, 0.5f).opened == 0);
    p.party = p.map = false;
    p.explore = true;
    CHECK_FALSE(step(s, p).map);
}

TEST_CASE("ui party: the cursor's rows") {
    CHECK_FALSE(uiPartyRowSelectable(UiPartyRow::Third, false));
    CHECK_FALSE(uiPartyRowSelectable(UiPartyRow::Third, true));
    CHECK(uiPartyRowSelectable(UiPartyRow::Partner, false));
    CHECK(uiPartyRowSelectable(UiPartyRow::Lead, false));
    CHECK_FALSE(uiPartyRowSelectable(UiPartyRow::Follow, false));
    CHECK(uiPartyRowSelectable(UiPartyRow::Follow, true));
}

TEST_CASE("ui party: the reasons and the session's refusals") {
    CHECK_FALSE(uiPartyRefused(UiPartyWhy::Unknown));
    CHECK_FALSE(uiPartyRefused(UiPartyWhy::Ok));
    CHECK(uiPartyRefused(UiPartyWhy::Down));
    CHECK(uiPartyRefused(UiPartyWhy::NotHere));
    CHECK(uiPartyRefused(UiPartyWhy::FightOver));
    CHECK(uiPartyRefused(UiPartyWhy::Wait));
    CHECK(uiPartyWhyOf(LeadSwitch::Ok) == UiPartyWhy::Ok);
    CHECK(uiPartyWhyOf(LeadSwitch::Same) == UiPartyWhy::Ok);
    CHECK(uiPartyWhyOf(LeadSwitch::NotInFight) == UiPartyWhy::NotHere);
    CHECK(uiPartyWhyOf(LeadSwitch::Down) == UiPartyWhy::Down);
    CHECK(uiPartyWhyOf(LeadSwitch::FightOver) == UiPartyWhy::FightOver);
    CHECK(uiPartyWhyOf(LeadSwitch::Cooldown) == UiPartyWhy::Wait);
    CHECK(uiPartyWhyOf(LeadSwitch::Lone) == UiPartyWhy::NotHere);
}

TEST_CASE("ui party: a lead not in this fight reads NOT HERE, and its switch is not taken") {
    FightSessionDef d;
    d.inFight = {true, false};
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(uiPartyWhyOf(s.leadSwitchReason(kLeadB)) == UiPartyWhy::NotHere);
    CHECK(uiPartyWhyOf(s.leadSwitchReason(kLeadA)) == UiPartyWhy::Ok);
    CHECK_FALSE(s.setActiveLead(kLeadB)); // nothing asked: no tick refuses it, so the host shows the call itself
}

TEST_CASE("ui party: a Lone Carry's partner reads NOT HERE; the refused switch says so and leaves the carry on") {
    constexpr double frame = 1.0 / 60.0;
    FightSessionDef d;
    d.loneLead = kLeadA;
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    s.tick(frame);
    CHECK(uiPartyWhyOf(s.leadSwitchReason(kLeadB)) == UiPartyWhy::NotHere);
    REQUIRE(s.setActiveLead(kLeadB));
    const FightTick& t = s.tick(frame);
    CHECK(t.switchRefused == kLeadB);
    CHECK(uiPartyWhyOf(t.switchRefusal) == UiPartyWhy::NotHere);
    CHECK(s.lone());
    CHECK(s.activeLead() == kLeadA);
}

TEST_CASE("ui party: a second switch inside the cooldown reads WAIT, row and call alike, then clears") {
    constexpr double frame = 1.0 / 60.0;
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    s.tick(frame);
    REQUIRE(s.setActiveLead(kLeadB));
    CHECK(s.tick(frame).switched == kLeadB);
    CHECK(uiPartyWhyOf(s.leadSwitchReason(kLeadA)) == UiPartyWhy::Wait);
    REQUIRE(s.setActiveLead(kLeadA));
    const FightTick& t = s.tick(frame);
    CHECK(t.switchRefused == kLeadA);
    CHECK(uiPartyWhyOf(t.switchRefusal) == UiPartyWhy::Wait);
    for (int i = 0; i < 70; ++i) s.tick(frame);
    CHECK(uiPartyWhyOf(s.leadSwitchReason(kLeadA)) == UiPartyWhy::Ok);
}

TEST_CASE("ui party: a switch to a downed lead, refused at the close, reads DOWN") {
    constexpr double frame = 1.0 / 60.0;
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadA));
    s.tick(frame);
    s.setLeadHealth(kLeadB, 0.0f);
    s.tick(frame);
    REQUIRE(s.down(kLeadB));
    REQUIRE(s.setActiveLead(kLeadB)); // taken: the close refuses it
    const FightTick& t = s.tick(frame);
    CHECK(t.switchRefused == kLeadB);
    CHECK(uiPartyWhyOf(t.switchRefusal) == UiPartyWhy::Down);
    CHECK(s.activeLead() == kLeadA);
}

TEST_CASE("ui party: a switch asked as the controlled lead falls gives way to the takeover, with nothing to say") {
    constexpr double frame = 1.0 / 60.0;
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadA));
    s.tick(frame);
    REQUIRE(s.setActiveLead(kLeadB));
    s.setLeadHealth(kLeadA, 0.0f);
    const FightTick& t = s.tick(frame);
    CHECK(t.tookOver == kLeadB);
    CHECK(t.switchRefused == kNoFighter); // the HUD draws no SWITCH call
    CHECK(s.activeLead() == kLeadB);
}

TEST_CASE("ui party: stepping allocates nothing") {
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    Pad p;
    p.explore = true;
    UiPartyScreen s = fresh(p);
    p.map = true;
    run(s, p, 0.5f);
    p.stick = 0.9f;
    run(s, p, 0.2f);
    p.map = false;
    run(s, p, 0.2f);
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK_FALSE(s.open);
}
