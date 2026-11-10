#include "ftcore/VehicleRules.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace ftcore;

namespace {

constexpr float kKmh = 100000.f / 3600.f;

RepairTuning garages() {
    RepairTuning t;
    t.garageCount = 1;
    std::memcpy(t.garages[0].id, "canal_rd", 9);
    t.garages[0].x = 88500.f;
    t.garages[0].y = 53000.f;
    return t;
}

} // namespace

TEST_CASE("vehicle wear: impacts wear the zone they hit in stages, distance gathers dirt, both stop at stage 4") {
    const WearTuning t;
    CHECK(wearZoneAt(200.f, 10.f) == WearZone::Front);
    CHECK(wearZoneAt(-200.f, 10.f) == WearZone::Rear);
    CHECK(wearZoneAt(10.f, 90.f) == WearZone::Right);
    CHECK(wearZoneAt(10.f, -90.f) == WearZone::Left);

    WearState s;
    addImpact(t, s, 140.f, 200.f, 0.f); // a nudge leaves nothing
    CHECK(s.zone[0] == 0.f);
    addImpact(t, s, 350.f, 200.f, 0.f); // half a stage
    CHECK(wearStage(s, WearZone::Front) == 0);
    addImpact(t, s, 350.f, 200.f, 0.f);
    CHECK(wearStage(s, WearZone::Front) == 1);
    addImpact(t, s, 1750.f, 200.f, 0.f); // a hard hit at 63 km/h: four stages more
    CHECK(wearStage(s, WearZone::Front) == kWearMaxStage);
    CHECK(s.zone[0] == doctest::Approx(4.f));
    CHECK(wearStage(s, WearZone::Rear) == 0);

    addDistance(t, s, 400000.f); // 4 km: one stage of dirt
    CHECK(dirtStage(s) == 1);
    addDistance(t, s, 1e8f);
    CHECK(dirtStage(s) == kWearMaxStage);
}

TEST_CASE("vehicle repair: in a garage, priced per stage, worst zones first when short of money, wash free") {
    const RepairTuning t = garages();
    CHECK(garageAt(t, 88500.f, 53500.f) == 0);
    CHECK(garageAt(t, 88500.f, 54000.f) == -1);

    WearState s;
    s.zone[0] = 3.5f; // front 3
    s.zone[2] = 1.2f; // left 1
    s.zone[3] = 0.4f; // a scuff
    s.dirt = 2.f;
    CHECK(repairQuote(t, s) == 200);

    WearState part = s;
    const RepairResult r = repair(t, part, 120);
    CHECK(r.stagesFixed == 2);
    CHECK(r.spent == 100);
    CHECK(r.washed);
    CHECK(r.minutes == doctest::Approx(30.f));
    CHECK(wearStage(part, WearZone::Front) == 1); // the front took both stages
    CHECK(wearStage(part, WearZone::Left) == 1);
    CHECK(part.dirt == 0.f);

    WearState full = s;
    const RepairResult f = repair(t, full, 1000);
    CHECK(f.spent == 200);
    for (const float z : full.zone) CHECK(z == 0.f); // the scuff polished out too

    WearState broke = s;
    const RepairResult b = repair(t, broke, 0);
    CHECK(b.stagesFixed == 0);
    CHECK(b.washed);
    CHECK(b.minutes == 0.f);
    CHECK(wearStage(broke, WearZone::Front) == 3);
}

TEST_CASE("vehicle seats: the party's own vehicle only, followers walk in or are placed in, exit held and slow") {
    const SeatTuning t;
    PartySeats p;
    const float near[2] = {300.f, 2000.f}; // one 3 m away, one across the street and beyond boardFar
    CHECK_FALSE(tryEnter(t, p, 150.f, 0.f, false, near, 2)); // not the party's car
    CHECK_FALSE(tryEnter(t, p, 250.f, 0.f, true, near, 2));  // too far
    CHECK_FALSE(tryEnter(t, p, 150.f, 300.f, true, near, 2)); // moving
    CHECK(p.phase == SeatPhase::OnFoot);
    REQUIRE(tryEnter(t, p, 150.f, 0.f, true, near, 2));
    CHECK(p.phase == SeatPhase::Entering);
    CHECK(p.count == 3);
    CHECK(p.length == doctest::Approx(300.f / 450.f + 1.2f)); // waits for the near follower
    CHECK_FALSE(tryEnter(t, p, 150.f, 0.f, true, near, 2));   // already entering

    tickSeats(t, p, 1.25f, 0.f, false, true, true);
    CHECK(p.aboard[0]);
    CHECK_FALSE(p.aboard[1]);
    CHECK_FALSE(p.canDrive());
    tickSeats(t, p, 0.7f, 0.f, false, true, true);
    CHECK(p.canDrive());
    CHECK(p.aboard[1]);
    CHECK(p.aboard[2]); // placed in at the door shut

    // Exit: held at speed does nothing; held long enough slow, out the driver's side.
    for (int i = 0; i < 60; ++i) tickSeats(t, p, 1.f / 60.f, 500.f, true, true, true);
    CHECK(p.canDrive());
    tickSeats(t, p, 0.2f, 0.f, true, true, true);
    CHECK(p.canDrive()); // not held long enough
    tickSeats(t, p, 0.15f, 0.f, true, false, true);
    CHECK(p.phase == SeatPhase::Exiting);
    CHECK(p.side == ExitSide::Passenger); // the driver's side blocked
    tickSeats(t, p, 1.f, 0.f, false, true, true);
    CHECK(p.phase == SeatPhase::OnFoot);
    CHECK_FALSE(p.aboard[0]);

    // Both sides blocked: they stay in.
    REQUIRE(tryEnter(t, p, 0.f, 0.f, true, nullptr, 0));
    CHECK(p.length == doctest::Approx(1.2f));
    tickSeats(t, p, 2.f, 0.f, false, true, true);
    tickSeats(t, p, 0.5f, 0.f, true, false, false);
    CHECK(p.canDrive());
    CHECK(p.side == ExitSide::None);

    // Far followers never hold the drive past boardWait.
    PartySeats w;
    const float slow[2] = {1400.f, 1400.f};
    REQUIRE(tryEnter(t, w, 0.f, 0.f, true, slow, 2));
    CHECK(w.length == doctest::Approx(3.f));
}

TEST_CASE("vehicle pedestrians: capped near them, they step aside with time, else stumble or fall, never worse") {
    const PedestrianTuning t;
    CHECK(pedestrianCap(t, 500.f) == doctest::Approx(25.f * kKmh).epsilon(0.001));
    CHECK(pedestrianCap(t, 900.f) == 0.f);
    CHECK(pedestrianCap(t, -1.f) == 0.f);

    // At the 25 km/h cap a pedestrian 8 m ahead on the centre line has 1.15 s and needs 0.65 s: steps aside.
    PedOutcome o = pedestrianReact(t, 694.f, 800.f, 0.f);
    CHECK(o.react == PedReact::Dodge);
    CHECK(o.sidestep == doctest::Approx(124.f));
    o = pedestrianReact(t, 694.f, 800.f, -30.f);
    CHECK(o.react == PedReact::Dodge);
    CHECK(o.sidestep == doctest::Approx(-94.f)); // away from the side they were on

    // Too close to make it: a fall at 25 km/h, a stumble at walking pace.
    o = pedestrianReact(t, 694.f, 200.f, 0.f);
    CHECK(o.react == PedReact::KnockDown);
    CHECK(o.down == doctest::Approx(2.5f));
    o = pedestrianReact(t, 200.f, 30.f, 0.f);
    CHECK(o.react == PedReact::Stumble);
    CHECK(o.down == doctest::Approx(0.8f));

    // Out of the cone, behind, beside the body, or a stopped car: nothing.
    CHECK(pedestrianReact(t, 694.f, 100.f, 300.f).react == PedReact::None);
    CHECK(pedestrianReact(t, 694.f, -100.f, 0.f).react == PedReact::None);
    CHECK(pedestrianReact(t, 694.f, 800.f, 130.f).react == PedReact::None);
    CHECK(pedestrianReact(t, 0.f, 50.f, 0.f).react == PedReact::None);
}

TEST_CASE("vehicle rules: the shipped provisional file loads, and bad rows are refused") {
    std::string text;
    if (!ftcore_test::readProjectData("Vehicles/vehicle_rules.json", text)) FAIL("Data/Vehicles/vehicle_rules.json not found");
    VehicleRules r;
    std::string err;
    REQUIRE(importVehicleRules(text, r, &err));
    CHECK(r.repair.pricePerStage == 50);
    REQUIRE(r.repair.garageCount == 3);
    CHECK(std::string(r.repair.garages[0].id) == "canal_rd");
    CHECK(r.repair.garages[0].x == doctest::Approx(88500.f));
    CHECK(r.repair.garages[0].radius == doctest::Approx(800.f));
    CHECK(garageAt(r.repair, 88500.f, 41000.f) == -1); // the south end of Canal Road's span is out of reach...
    CHECK(garageAt(r.repair, 88500.f, 53400.f) == 0); // ...the bay is in it
    CHECK(r.pedestrian.nearCap == doctest::Approx(25.f * kKmh));

    VehicleRules keep;
    CHECK_FALSE(importVehicleRules(R"({"version":1,"repair":{"garages":[{"id":"a","x":1}]}})", keep, &err));
    CHECK_FALSE(importVehicleRules(R"({"version":1,"repair":{"garages":[{"id":"","x":1,"y":2}]}})", keep, &err));
    CHECK_FALSE(importVehicleRules(R"({"version":1,"wear":{"heat":1}})", keep, &err));
    CHECK(err == "wear.heat: not a known member");
    CHECK_FALSE(importVehicleRules(R"({"version":1,"pedestrian":{"nearCapKmh":8,"stumbleBelowKmh":10}})", keep, &err));
    CHECK(keep.repair.garageCount == 0); // unchanged on failure
}

TEST_CASE("vehicle rules allocate nothing at run time") {
    const VehicleRules r;
    const bool hooked = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    WearState w;
    PartySeats p;
    float sum = 0.f;
    for (int i = 0; i < 1000; ++i) {
        addImpact(r.wear, w, static_cast<float>(i), 100.f, static_cast<float>(i % 7) - 3.f);
        addDistance(r.wear, w, 50.f);
        if (i % 100 == 0) sum += static_cast<float>(repair(r.repair, w, i).spent);
        const PedOutcome o = pedestrianReact(r.pedestrian, static_cast<float>(i), 400.f, static_cast<float>(i % 50));
        sum += o.down + o.sidestep + pedestrianCap(r.pedestrian, static_cast<float>(i));
        if (p.phase == SeatPhase::OnFoot) tryEnter(r.seats, p, 0.f, 0.f, true, nullptr, 0);
        tickSeats(r.seats, p, 0.1f, 0.f, i % 20 > 10, true, true);
    }
    CHECK(std::isfinite(sum));
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
}
