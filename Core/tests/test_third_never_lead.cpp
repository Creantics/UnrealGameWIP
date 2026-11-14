// The Third is a party member, never playable (ruled 2026-11-14): no switch, takeover or begin makes the third slot
// the controlled lead. Lead switching and takeover are between the two leads only; the third slot only gives gifts.
#include "ftcore/FightSession.h"

#include <doctest/doctest.h>

using namespace ftcore;

static_assert(!isLead(kThirdSlot));
static_assert(isLead(kLeadA) && isLead(kLeadB));

TEST_CASE("the third slot: a switch request naming it is refused, and no fall hands it control") {
    constexpr double kFrame = 1.0 / 60.0;
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    s.tick(kFrame);
    const FighterId was = s.activeLead();
    REQUIRE(isLead(was));
    // Refused at once: no request waits for the step's close, and the close reports nothing.
    CHECK_FALSE(s.setActiveLead(kThirdSlot));
    CHECK(s.switchRequested() == kNoFighter);
    CHECK_FALSE(s.setActiveLead(kNoFighter));
    const FightTick& t = s.tick(kFrame);
    CHECK(t.switched == kNoFighter);
    CHECK(t.switchRefused == kNoFighter);
    CHECK(s.activeLead() == was);
    // A good request, then the third's in the same step: the third's is refused and the good one still stands.
    const FighterId other = was == kLeadA ? kLeadB : kLeadA;
    CHECK(s.setActiveLead(other));
    CHECK_FALSE(s.setActiveLead(kThirdSlot));
    CHECK(s.switchRequested() == other);
    CHECK(s.tick(kFrame).switched == other);
    CHECK(s.activeLead() == other);
    // The controlled lead falls with the other up: control passes to the other lead, never the third slot.
    s.setLeadHealth(other, 0.f);
    const FightTick& fell = s.tick(kFrame);
    CHECK(fell.tookOver == was);
    CHECK(isLead(s.activeLead()));
    // Both down: a defeat, no takeover, and the controlled lead still one of the two.
    s.setLeadHealth(was, 0.f);
    const FightTick& lost = s.tick(kFrame);
    CHECK(lost.defeated);
    CHECK(lost.tookOver == kNoFighter);
    CHECK(isLead(s.activeLead()));
    CHECK_FALSE(s.setActiveLead(kThirdSlot));
    // A one-lead fight: the third slot is never in it either.
    FightSessionDef one;
    one.inFight = {true, false};
    REQUIRE(s.begin(one, 0.0).ok());
    CHECK(s.activeLead() == kLeadA);
    CHECK_FALSE(s.setActiveLead(kThirdSlot));
    CHECK_FALSE(s.setActiveLead(kLeadB)); // not in this fight
}
