#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "ftcore/WaterBodies.h"
#include "ftcore/WaterCue.h"

using namespace ftcore;

namespace {

WaterCueEvent cue(WaterCueKind kind, float strength) {
    WaterCueEvent c;
    c.kind = kind;
    c.strength = strength;
    c.at[0] = 3.f;
    c.at[1] = 4.f;
    return c;
}

} // namespace

TEST_CASE("A follower's entry splashes and rings; the lead's own entry and exit are the stir's") {
    WaterRipples rings;
    CHECK(waterCueStir(rings, cue(WaterCueKind::Entry, 0.8f), false) == doctest::Approx(0.8f));
    CHECK(rings.live() == 2);
    rings.clear();
    CHECK(waterCueStir(rings, cue(WaterCueKind::Entry, 0.8f), true) == 0.f);
    CHECK(waterCueStir(rings, cue(WaterCueKind::Exit, 0.9f), true) == 0.f);
    CHECK(rings.live() == 0);
    CHECK(waterCueStir(rings, cue(WaterCueKind::Exit, 0.9f), false) == doctest::Approx(0.3f));
    CHECK(rings.live() == 1);
}

TEST_CASE("Strokes, kicks and the head send rings at the cue's place; a resting hand sends none") {
    WaterRipples rings;
    const auto before = ftcore_test::allocStats().count;
    for (WaterCueKind k : {WaterCueKind::CatchLeft, WaterCueKind::CatchRight, WaterCueKind::Kick, WaterCueKind::Gasp, WaterCueKind::HeadUnder,
                           WaterCueKind::HeadOut, WaterCueKind::Pour})
        CHECK(waterCueStir(rings, cue(k, 0.5f), true) == 0.f);
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(rings.live() == 7);
    rings.advance(0.1f); // a ring shows once it has risen
    CHECK(rings.ring(0).p.x == doctest::Approx(3.f));
    rings.clear();
    CHECK(waterCueStir(rings, cue(WaterCueKind::HandRest, 1.f), false) == 0.f);
    CHECK(waterCueStir(rings, cue(WaterCueKind::Pour, 0.f), false) == 0.f);
    CHECK(rings.live() == 0);
}
