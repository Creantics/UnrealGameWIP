// The dev captures' water spot check (WaterSpotCheck.h) on made-up hit lists: the order of the reasons, the roof
// height, overlaps ignored, the nearest hit told, the land capture's kept railing, the 11-18 spots, the tally, no heap.
#include "ftcore/WaterSpotCheck.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cstdint>

using ftcore::judgeWaterSpot;
using ftcore::WaterSpotHit;
using ftcore::WaterSpotLeg;
using ftcore::WaterSpotReason;
using ftcore::WaterSpotRules;
using ftcore::WaterSpotTally;
using ftcore::WaterSpotVerdict;

namespace {

const WaterSpotRules kStrict{};
const WaterSpotRules kKeepRailing{4.f, false};

template <std::size_t N>
WaterSpotVerdict judge(const WaterSpotHit (&hits)[N], const WaterSpotRules& rules, bool ground = true)
{
    return judgeWaterSpot(hits, N, ground, rules);
}

} // namespace

TEST_CASE("water spot: nothing in the way is accepted; no ground is rejected first")
{
    const WaterSpotVerdict open = judgeWaterSpot(nullptr, 0, true, kStrict);
    CHECK(open.accepted);
    CHECK(open.reason == WaterSpotReason::Ok);
    CHECK_FALSE(open.dryToInBlocked);
    CHECK(open.dryToInAt == doctest::Approx(-1.f));
    CHECK(open.at == doctest::Approx(-1.f));

    const WaterSpotHit all[] = {{WaterSpotLeg::Up, 2.f, true}, {WaterSpotLeg::DryToIn, 0.3f, true}, {WaterSpotLeg::InToFar, 5.f, true}};
    const WaterSpotVerdict none = judge(all, kStrict, false);
    CHECK_FALSE(none.accepted);
    CHECK(none.reason == WaterSpotReason::NoGround);
    CHECK(judgeWaterSpot(nullptr, 0, false, kStrict).reason == WaterSpotReason::NoGround);
}

TEST_CASE("water spot: a roof within 4 m, not one higher")
{
    const WaterSpotHit low[] = {{WaterSpotLeg::Up, 3.5f, true}};
    const WaterSpotVerdict roofed = judge(low, kStrict);
    CHECK_FALSE(roofed.accepted);
    CHECK(roofed.reason == WaterSpotReason::Roofed);
    CHECK(roofed.at == doctest::Approx(3.5f));
    const WaterSpotHit edge[] = {{WaterSpotLeg::Up, 4.f, true}};
    CHECK(judge(edge, kStrict).reason == WaterSpotReason::Roofed);
    const WaterSpotHit high[] = {{WaterSpotLeg::Up, 4.5f, true}};
    CHECK(judge(high, kStrict).accepted);
    // A roof rejects even where the way in may be blocked (the land rules too).
    const WaterSpotHit both[] = {{WaterSpotLeg::DryToIn, 0.4f, true}, {WaterSpotLeg::Up, 2.5f, true}};
    CHECK(judge(both, kKeepRailing).reason == WaterSpotReason::Roofed);
}

TEST_CASE("water spot: overlaps are ignored; a hit inside at the start is at 0; the nearest hit is told")
{
    const WaterSpotHit overlaps[] = {{WaterSpotLeg::Up, 1.f, false}, {WaterSpotLeg::DryToIn, 0.2f, false}, {WaterSpotLeg::InToFar, 1.f, false}};
    CHECK(judge(overlaps, kStrict).accepted);

    const WaterSpotHit inside[] = {{WaterSpotLeg::DryToIn, -0.1f, true}};
    const WaterSpotVerdict v = judge(inside, kStrict);
    CHECK(v.reason == WaterSpotReason::DryToInBlocked);
    CHECK(v.at == doctest::Approx(0.f));

    const WaterSpotHit several[] = {{WaterSpotLeg::InToFar, 9.f, true}, {WaterSpotLeg::InToFar, 2.2f, true}, {WaterSpotLeg::InToFar, 5.f, true}};
    const WaterSpotVerdict out = judge(several, kStrict);
    CHECK(out.reason == WaterSpotReason::InToFarBlocked);
    CHECK(out.at == doctest::Approx(2.2f));
}

TEST_CASE("water spot: the way in is rejected by the strict rules, kept (and told) by the land rules")
{
    const WaterSpotHit railing[] = {{WaterSpotLeg::DryToIn, 0.39f, true}, {WaterSpotLeg::DryToIn, 0.8f, true}};
    const WaterSpotVerdict strict = judge(railing, kStrict);
    CHECK_FALSE(strict.accepted);
    CHECK(strict.reason == WaterSpotReason::DryToInBlocked);
    CHECK(strict.at == doctest::Approx(0.39f));
    CHECK(strict.dryToInBlocked);

    const WaterSpotVerdict kept = judge(railing, kKeepRailing);
    CHECK(kept.accepted);
    CHECK(kept.reason == WaterSpotReason::Ok);
    CHECK(kept.dryToInBlocked);
    CHECK(kept.dryToInAt == doctest::Approx(0.39f));
    CHECK(kept.at == doctest::Approx(-1.f));

    // Blocked both ways: the way in is the reason under the strict rules, the water under the land rules.
    const WaterSpotHit both[] = {{WaterSpotLeg::InToFar, 3.f, true}, {WaterSpotLeg::DryToIn, 0.5f, true}};
    CHECK(judge(both, kStrict).reason == WaterSpotReason::DryToInBlocked);
    CHECK(judge(both, kKeepRailing).reason == WaterSpotReason::InToFarBlocked);
}

TEST_CASE("water spot: the 11-18 captures' spots")
{
    // Watchdog: the dry edge indoors (a ceiling 3 m up), a glass front 0.34 m ahead, a blocker 2.2 m out in the water.
    const WaterSpotHit watchdog[] = {{WaterSpotLeg::Up, 3.f, true}, {WaterSpotLeg::DryToIn, 0.34f, true}, {WaterSpotLeg::InToFar, 2.2f, true}};
    CHECK(judge(watchdog, kStrict).reason == WaterSpotReason::Roofed);
    CHECK_FALSE(judge(watchdog, kKeepRailing).accepted);
    // Without the ceiling it is still rejected both ways.
    const WaterSpotHit glass[] = {{WaterSpotLeg::DryToIn, 0.34f, true}, {WaterSpotLeg::InToFar, 2.2f, true}};
    CHECK(judge(glass, kStrict).reason == WaterSpotReason::DryToInBlocked);
    CHECK(judge(glass, kKeepRailing).reason == WaterSpotReason::InToFarBlocked);
    // Land: a two-bar railing 0.39 m ahead, open water: kept by the land rules (its held-up jump), not by the strict.
    const WaterSpotHit land[] = {{WaterSpotLeg::DryToIn, 0.39f, true}};
    CHECK(judge(land, kKeepRailing).accepted);
    CHECK(judge(land, kStrict).reason == WaterSpotReason::DryToInBlocked);
}

TEST_CASE("water spot: the tally and the names; no heap")
{
    WaterSpotTally tally;
    const WaterSpotHit roof[] = {{WaterSpotLeg::Up, 1.f, true}};
    const WaterSpotHit in[] = {{WaterSpotLeg::DryToIn, 0.3f, true}};
    const WaterSpotHit out[] = {{WaterSpotLeg::InToFar, 1.f, true}};
    const std::uint64_t before = ftcore_test::allocStats().count;
    tally.add(judge(roof, kStrict));
    tally.add(judge(in, kStrict));
    tally.add(judge(in, kStrict));
    tally.add(judge(out, kStrict));
    tally.add(judgeWaterSpot(nullptr, 0, false, kStrict));
    tally.add(judgeWaterSpot(nullptr, 0, true, kStrict));
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(tally.of(WaterSpotReason::Roofed) == 1);
    CHECK(tally.of(WaterSpotReason::DryToInBlocked) == 2);
    CHECK(tally.of(WaterSpotReason::InToFarBlocked) == 1);
    CHECK(tally.of(WaterSpotReason::NoGround) == 1);
    CHECK(tally.of(WaterSpotReason::Ok) == 1);
    CHECK(tally.total() == 6);
    for (int r = 0; r < ftcore::kWaterSpotReasons; ++r) {
        CHECK(ftcore::waterSpotReasonName(static_cast<WaterSpotReason>(r))[0] != '?');
    }
}
