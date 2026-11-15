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

namespace {

UiPartyScreen openOn(UiPartyRow cursor, bool explore = false) {
    UiPartyScreen s;
    s.open = true;
    s.explore = explore;
    s.cursor = cursor;
    return s;
}

} // namespace

TEST_CASE("ui party view: the cursor on the partner (the party still)") {
    const UiPartyScreen s = openOn(UiPartyRow::Partner);
    const UiPartyRowView p = uiPartyRowView(s, UiPartyRow::Partner, UiPartyWhy::Ok, 132.f);
    CHECK(p.ink == doctest::Approx(1.f));
    CHECK(p.underlineW == doctest::Approx(132.f)); // the whole name, not a stub
    CHECK_FALSE(p.dot);
    const UiPartyRowView l = uiPartyRowView(s, UiPartyRow::Lead, UiPartyWhy::Ok, 180.f);
    CHECK(l.ink == doctest::Approx(kUiPartyOtherInk)); // the controlled lead's name off the cursor dims
    CHECK(l.underlineW == 0.f);
    CHECK(l.dot); // and keeps its dot
    const UiPartyRowView t = uiPartyRowView(s, UiPartyRow::Third, UiPartyWhy::Unknown, 90.f);
    CHECK(t.ink == doctest::Approx(kUiPartyThirdInk));
    CHECK(t.underlineW == 0.f);
    CHECK_FALSE(t.dot);
}

TEST_CASE("ui party view: a refused partner under the cursor stays dim and is still underlined (party_refused)") {
    const UiPartyScreen s = openOn(UiPartyRow::Partner);
    const UiPartyRowView p = uiPartyRowView(s, UiPartyRow::Partner, UiPartyWhy::Down, 132.f);
    CHECK(p.ink == doctest::Approx(kUiPartyRefusedInk));
    CHECK(p.underlineW == doctest::Approx(132.f)); // the name's width only, not its word
    CHECK_FALSE(p.dot);
    const UiPartyRowView off = uiPartyRowView(openOn(UiPartyRow::Lead), UiPartyRow::Partner, UiPartyWhy::Wait, 132.f);
    CHECK(off.ink == doctest::Approx(kUiPartyRefusedInk));
    CHECK(off.underlineW == 0.f);
    const UiPartyRowView l = uiPartyRowView(s, UiPartyRow::Lead, UiPartyWhy::Ok, 180.f);
    CHECK(l.ink == doctest::Approx(kUiPartyOtherInk));
    CHECK(l.dot);
}

TEST_CASE("ui party view: the cursor on the lead, the partner not here (party_lone)") {
    const UiPartyScreen s = openOn(UiPartyRow::Lead);
    const UiPartyRowView l = uiPartyRowView(s, UiPartyRow::Lead, UiPartyWhy::Ok, 180.f);
    CHECK(l.ink == doctest::Approx(1.f));
    CHECK(l.underlineW == doctest::Approx(180.f));
    CHECK(l.dot); // cursor and dot together
    const UiPartyRowView p = uiPartyRowView(s, UiPartyRow::Partner, UiPartyWhy::NotHere, 132.f);
    CHECK(p.ink == doctest::Approx(kUiPartyRefusedInk));
    CHECK(p.underlineW == 0.f);
    // Unknown (no session to ask) stands undimmed: the other-row ink, not the refused one.
    CHECK(uiPartyRowView(s, UiPartyRow::Partner, UiPartyWhy::Unknown, 132.f).ink == doctest::Approx(kUiPartyOtherInk));
}

TEST_CASE("ui party view: explore's Follow row takes the cursor like a lead row, a reason does not dim it") {
    const UiPartyScreen on = openOn(UiPartyRow::Follow, true);
    const UiPartyRowView f = uiPartyRowView(on, UiPartyRow::Follow, UiPartyWhy::Down, 120.f);
    CHECK(f.ink == doctest::Approx(1.f));
    CHECK(f.underlineW == doctest::Approx(120.f));
    CHECK_FALSE(f.dot);
    const UiPartyRowView l = uiPartyRowView(on, UiPartyRow::Lead, UiPartyWhy::Unknown, 180.f);
    CHECK(l.ink == doctest::Approx(kUiPartyOtherInk));
    CHECK(l.dot);
    CHECK(uiPartyRowView(openOn(UiPartyRow::Lead, true), UiPartyRow::Follow, UiPartyWhy::Unknown, 120.f).ink ==
          doctest::Approx(kUiPartyOtherInk));
}

TEST_CASE("ui party view: the Third never takes the cursor's marks, and a closed screen marks nothing") {
    UiPartyScreen s = openOn(UiPartyRow::Third); // not a place the step puts it
    const UiPartyRowView t = uiPartyRowView(s, UiPartyRow::Third, UiPartyWhy::Ok, 90.f);
    CHECK(t.ink == doctest::Approx(kUiPartyThirdInk));
    CHECK(t.underlineW == 0.f);
    CHECK_FALSE(t.dot);
    s = openOn(UiPartyRow::Lead);
    CHECK(uiPartyRowView(s, UiPartyRow::Lead, UiPartyWhy::Ok, 0.f).underlineW == 0.f); // no measured name, no line
    s.open = false;
    for (const UiPartyRow r : {UiPartyRow::Third, UiPartyRow::Partner, UiPartyRow::Lead, UiPartyRow::Follow}) {
        const UiPartyRowView v = uiPartyRowView(s, r, UiPartyWhy::Down, 100.f);
        CHECK(v.ink == doctest::Approx(1.f));
        CHECK(v.underlineW == 0.f);
        CHECK_FALSE(v.dot);
    }
}

TEST_CASE("ui party view: the dot hangs before the name, centred on its capitals") {
    const UiPartyDot d = uiPartyDot(64.f, 900.f, 25.f);
    CHECK(d.r == doctest::Approx(0.5f * kUiPartyDotCap * 25.f));
    CHECK(d.y == doctest::Approx(900.f));
    CHECK(d.x + d.r == doctest::Approx(64.f - kUiPartyDotGap * 25.f)); // its right edge a gap short of the name
    CHECK(d.x - d.r > 0.f);                                             // inside a 64 px margin
    CHECK(uiPartyDot(64.f, 900.f, -3.f).r == 0.f);
}

TEST_CASE("ui party view: allocates nothing") {
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    float sum = 0.f;
    for (int i = 0; i < 100; ++i) {
        const UiPartyScreen s = openOn(i % 2 ? UiPartyRow::Lead : UiPartyRow::Partner);
        sum += uiPartyRowView(s, UiPartyRow::Partner, UiPartyWhy::Down, 100.f).underlineW;
        sum += uiPartyDot(64.f, 900.f, 25.f).r;
    }
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK(sum > 0.f);
}

TEST_CASE("ui party: with no partner in the fight its row never takes the cursor, so nothing is picked") {
    CHECK_FALSE(uiPartyRowSelectable(UiPartyRow::Partner, false, false));
    CHECK_FALSE(uiPartyRowSelectable(UiPartyRow::Partner, true, false));
    CHECK(uiPartyRowSelectable(UiPartyRow::Lead, false, false));
    CHECK(uiPartyRowSelectable(UiPartyRow::Follow, true, false));
    for (int key = 0; key < 2; ++key) {
        Pad p;
        UiPartyScreen s = fresh(p);
        const auto stepAlone = [&s, &p] {
            UiPartyInput i = p.in();
            i.partnerHere = false;
            return uiPartyScreenStep(s, i, kDt);
        };
        (key == 0 ? p.party : p.map) = true;
        CHECK(stepAlone().opened);
        // A flick up: the partner's row is passed over and the Third's never takes it, so the cursor stays on the lead.
        p.stick = 0.9f;
        stepAlone();
        CHECK(s.cursor == UiPartyRow::Lead);
        p.stick = 0.f;
        stepAlone();
        p.mouse = 20.f;
        stepAlone();
        CHECK(s.cursor == UiPartyRow::Lead);
        p.mouse = 0.f;
        // Drawn: the partner's row has no underline and the lead's keeps it (and the dot).
        const UiPartyRowView partner = uiPartyRowView(s, UiPartyRow::Partner, UiPartyWhy::NotHere, 100.f);
        CHECK(partner.underlineW == 0.f);
        CHECK(partner.ink == doctest::Approx(kUiPartyRefusedInk));
        const UiPartyRowView lead = uiPartyRowView(s, UiPartyRow::Lead, UiPartyWhy::Ok, 80.f);
        CHECK(lead.underlineW == doctest::Approx(80.f));
        CHECK(lead.dot);
        (key == 0 ? p.party : p.map) = false;
        const UiPartyOut o = stepAlone();
        CHECK(o.closed);
        CHECK_FALSE(o.picked());
    }
}

TEST_CASE("ui party: the partner leaving the fight while the cursor is on its row puts it back on the lead") {
    Pad p;
    UiPartyScreen s = fresh(p);
    p.party = true;
    step(s, p);
    p.stick = 0.9f;
    step(s, p);
    REQUIRE(s.cursor == UiPartyRow::Partner);
    p.stick = 0.f;
    UiPartyInput gone = p.in();
    gone.partnerHere = false;
    uiPartyScreenStep(s, gone, kDt);
    CHECK(s.cursor == UiPartyRow::Lead);
    CHECK(uiPartyRowView(s, UiPartyRow::Partner, UiPartyWhy::NotHere, 100.f).underlineW == 0.f);
    gone.partyDown = false;
    const UiPartyOut o = uiPartyScreenStep(s, gone, kDt);
    CHECK(o.closed);
    CHECK_FALSE(o.picked());
    // Back in the fight, the row takes the cursor again (a refused row that is here still picks: the close refuses it).
    p.party = true;
    step(s, p);
    p.stick = 0.9f;
    step(s, p);
    CHECK(s.cursor == UiPartyRow::Partner);
}

TEST_CASE("ui party: the probe's script picks the partner once from a screen that had no input, then reopens on the lead") {
    for (const float dt : {1.f / 120.f, 1.f / 60.f, 1.f / 30.f}) {
        for (const bool here : {true, false}) {
            CAPTURE(dt);
            CAPTURE(here);
            UiPartyScreen s; // as a fight begins under the autopilot: no input yet
            int picks = 0, opens = 0, closes = 0;
            UiPartyRow pick = UiPartyRow::Lead;
            float pickedAt = -10.f;
            bool openOnLeadAtOne = false;
            const int steps = static_cast<int>(5.f / dt);
            for (int i = 0; i < steps; ++i) {
                const float t = -2.5f + static_cast<float>(i) * dt;
                UiPartyInput in; // the autopilot's: not live
                in.partnerHere = here;
                const bool scripted = uiPartyProbeInput(t, in);
                CHECK(scripted == (t >= kUiPartyProbeFrom && t < kUiPartyProbeTo));
                const UiPartyOut o = uiPartyScreenStep(s, in, dt);
                opens += o.opened ? 1 : 0;
                closes += o.closed ? 1 : 0;
                if (o.picked()) {
                    ++picks;
                    pick = o.pick;
                    pickedAt = t;
                }
                if (t >= 1.f && t < 1.f + dt) openOnLeadAtOne = s.open && s.cursor == UiPartyRow::Lead;
            }
            CHECK(opens == 2);
            CHECK(closes == 2);
            CHECK(openOnLeadAtOne);
            CHECK_FALSE(s.open);
            if (here) {
                CHECK(picks == 1);
                CHECK(pick == UiPartyRow::Partner);
                CHECK(pickedAt >= 0.f);
                CHECK(pickedAt < dt + 1e-4f);
            } else {
                CHECK(picks == 0);
            }
        }
    }
}

namespace {

// The host's feed for the partner's row from a real session: its word (uiPartyLeadWhy on the session's answer and the
// body the host has for that lead) and so its presence, the controlled lead from the session, as FTArenaRunner's
// StepPartyScreen gives them.
struct SessionFeed {
    UiPartyWhy why = UiPartyWhy::Unknown;
    bool here = false;
};
SessionFeed feedFrom(const FightSession& s, bool body) {
    const FighterId partner = s.activeLead() == kLeadA ? kLeadB : kLeadA;
    SessionFeed f;
    f.why = uiPartyLeadWhy(s.leadSwitchReason(partner), body);
    f.here = uiPartyPartnerHere(f.why);
    return f;
}
UiPartyInput sessionIn(const Pad& p, const FightSession& s, const SessionFeed& f) {
    UiPartyInput i = p.in();
    i.partnerHere = f.here;
    i.lead = static_cast<int>(s.activeLead());
    return i;
}
// Opens the screen, flicks up once and lets go: the cursor's row after the flick, and whether the release picked the
// partner.
struct Tried {
    UiPartyRow cursor = UiPartyRow::Lead;
    UiPartyRowView partner;
    bool picked = false;
};
Tried tryPartner(const FightSession& s, const SessionFeed& f) {
    Pad p;
    UiPartyScreen screen;
    uiPartyScreenStep(screen, sessionIn(p, s, f), kDt);
    p.party = true;
    uiPartyScreenStep(screen, sessionIn(p, s, f), kDt);
    p.stick = 0.9f;
    uiPartyScreenStep(screen, sessionIn(p, s, f), kDt);
    Tried t;
    t.cursor = screen.cursor;
    t.partner = uiPartyRowView(screen, UiPartyRow::Partner, f.why, 100.f);
    p.stick = 0.f;
    p.party = false;
    const UiPartyOut o = uiPartyScreenStep(screen, sessionIn(p, s, f), kDt);
    t.picked = o.picked() && o.pick == UiPartyRow::Partner;
    return t;
}

} // namespace

TEST_CASE("ui party: the partner's row from a real session: in the fight with a body, joined this step, Lone, down") {
    constexpr double frame = 1.0 / 60.0;
    SUBCASE("in the fight with a body: Ok, the cursor takes it at full ink and the release picks it") {
        FightSession s;
        REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
        s.tick(frame);
        const SessionFeed f = feedFrom(s, true);
        CHECK(f.why == UiPartyWhy::Ok);
        CHECK(f.here);
        const Tried t = tryPartner(s, f);
        CHECK(t.cursor == UiPartyRow::Partner);
        CHECK(t.partner.ink == doctest::Approx(1.f));
        CHECK(t.partner.underlineW == doctest::Approx(100.f));
        CHECK(t.picked);
        // The host's rule: the same session with no body for the partner is NOT HERE, never the cursor's.
        const SessionFeed none = feedFrom(s, false);
        CHECK(none.why == UiPartyWhy::NotHere);
        CHECK_FALSE(tryPartner(s, none).picked);
    }
    SUBCASE("joined this step: NOT HERE before the close, Ok once the join has landed and the carry ended") {
        FightSessionDef d;
        d.inFight = {true, false};
        d.loneLead = kLeadA;
        FightSession s;
        REQUIRE(s.begin(d, 0.0).ok());
        s.tick(frame);
        // The host's body for the partner: in the session's fight and not Lone (FTArenaRunner::PartnerBody).
        const auto body = [&s] { return s.inFight(kLeadB) && !s.lone(); };
        REQUIRE(s.joinFight(kLeadB, 1.f));
        s.endLone(LoneEnd::PartnerCalled);
        const SessionFeed before = feedFrom(s, body());
        CHECK(before.why == UiPartyWhy::NotHere); // NotInFight until the close
        CHECK_FALSE(before.here);
        CHECK(tryPartner(s, before).cursor == UiPartyRow::Lead);
        CHECK_FALSE(tryPartner(s, before).picked);
        REQUIRE(s.tick(frame).joined == kLeadB);
        const SessionFeed after = feedFrom(s, body());
        CHECK(after.why == UiPartyWhy::Ok);
        CHECK(after.here);
        CHECK(tryPartner(s, after).picked);
    }
    SUBCASE("Lone: NOT HERE from the session (and from the host, with no body), never the cursor's") {
        FightSessionDef d;
        d.loneLead = kLeadA;
        FightSession s;
        REQUIRE(s.begin(d, 0.0).ok());
        s.tick(frame);
        REQUIRE(s.lone());
        for (const bool body : {true, false}) {
            CAPTURE(body);
            const SessionFeed f = feedFrom(s, body);
            CHECK(f.why == UiPartyWhy::NotHere);
            CHECK_FALSE(f.here);
            const Tried t = tryPartner(s, f);
            CHECK(t.cursor == UiPartyRow::Lead);
            CHECK(t.partner.underlineW == 0.f);
            CHECK_FALSE(t.picked);
        }
    }
    SUBCASE("down: DOWN, the row takes the cursor dimmed and still picks; the close refuses it") {
        FightSession s;
        REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
        s.tick(frame);
        s.setLeadHealth(kLeadB, 0.f);
        s.tick(frame);
        REQUIRE(s.down(kLeadB));
        const SessionFeed f = feedFrom(s, true);
        CHECK(f.why == UiPartyWhy::Down);
        CHECK(f.here);
        const Tried t = tryPartner(s, f);
        CHECK(t.cursor == UiPartyRow::Partner);
        CHECK(t.partner.ink == doctest::Approx(kUiPartyRefusedInk));
        CHECK(t.partner.underlineW == doctest::Approx(100.f));
        REQUIRE(t.picked);
        REQUIRE(s.setActiveLead(kLeadB));
        const FightTick& tick = s.tick(frame);
        CHECK(tick.switchRefused == kLeadB);
        CHECK(uiPartyWhyOf(tick.switchRefusal) == UiPartyWhy::Down);
    }
}

TEST_CASE("ui party: a takeover under the open screen puts the cursor on the new lead, so the release sends nothing") {
    constexpr double frame = 1.0 / 60.0;
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    s.tick(frame);
    Pad p;
    UiPartyScreen screen;
    const auto stepIt = [&] { return uiPartyScreenStep(screen, sessionIn(p, s, feedFrom(s, true)), kDt); };
    stepIt();
    p.party = true;
    CHECK(stepIt().opened);
    p.stick = 0.9f;
    stepIt();
    REQUIRE(screen.cursor == UiPartyRow::Partner);
    p.stick = 0.f;
    stepIt();
    // The controlled lead falls with the partner up: the session hands control over without the screen's confirm.
    s.setLeadHealth(kLeadA, 0.f);
    REQUIRE(s.tick(frame).tookOver == kLeadB);
    stepIt();
    CHECK(screen.cursor == UiPartyRow::Lead);
    p.party = false;
    const UiPartyOut o = stepIt();
    CHECK(o.closed);
    CHECK_FALSE(o.picked());
}

TEST_CASE("ui party: a lead change while open moves the cursor to Lead; the first lead said, or a closed screen, moves nothing") {
    Pad p;
    UiPartyScreen s = fresh(p);
    const auto stepLead = [&s, &p](int lead) {
        UiPartyInput i = p.in();
        i.lead = lead;
        return uiPartyScreenStep(s, i, kDt);
    };
    // Not said (-1) before, then said: no change.
    p.party = true;
    stepLead(-1);
    p.stick = 0.9f;
    stepLead(0);
    REQUIRE(s.cursor == UiPartyRow::Partner);
    p.stick = 0.f;
    stepLead(0);
    CHECK(s.cursor == UiPartyRow::Partner);
    // Tab swapped the roles while the cursor rests on the partner: back to the lead, the release picks nothing.
    stepLead(1);
    CHECK(s.cursor == UiPartyRow::Lead);
    p.party = false;
    CHECK_FALSE(stepLead(1).picked());
    // A change while shut: the next opening starts on the lead anyway, and nothing moves.
    stepLead(0);
    p.party = true;
    CHECK(stepLead(0).opened);
    CHECK(s.cursor == UiPartyRow::Lead);
}
