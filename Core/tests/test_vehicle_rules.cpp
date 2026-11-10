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
constexpr float kFrame = 1.f / 60.f;

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
    CHECK(broke.zone[3] == doctest::Approx(0.4f)); // no free polish when nothing was paid for
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

TEST_CASE("vehicle pedestrians: they step aside with time, else are met slowed, a fall only above 10 km/h, never above 25") {
    const PedestrianTuning t;
    // At the 25 km/h cap a pedestrian 8 m ahead on the centre line: the van stops short, they step aside anyway.
    PedOutcome o = pedestrianReact(t, 694.f, 800.f, 0.f);
    CHECK(o.react == PedReact::Dodge);
    CHECK(o.sidestep == doctest::Approx(124.f));
    o = pedestrianReact(t, 694.f, 800.f, -30.f);
    CHECK(o.react == PedReact::Dodge);
    CHECK(o.sidestep == doctest::Approx(-94.f)); // away from the side they were on

    // Stepping out 2 m ahead at the cap: braked to about 7 km/h by contact, a stumble. At the bumper: a fall.
    o = pedestrianReact(t, 694.f, 200.f, 0.f);
    CHECK(o.react == PedReact::Stumble);
    CHECK(o.down == doctest::Approx(0.8f));
    CHECK(o.contact < t.stumbleBelow);
    o = pedestrianReact(t, 694.f, 30.f, 0.f);
    CHECK(o.react == PedReact::KnockDown);
    CHECK(o.down == doctest::Approx(2.5f));
    CHECK(o.contact >= t.stumbleBelow);
    // At 80 km/h the hard clamp inside nearDistance means they are met at no more than 25.
    o = pedestrianReact(t, 80.f * kKmh, 30.f, 0.f);
    CHECK(o.react == PedReact::KnockDown);
    CHECK(o.contact <= t.nearCap);

    // The bumper's corner (inside the body's width, far outside the cone's angle) is in the path.
    CHECK(pedestrianReact(t, 694.f, 10.f, 70.f).react == PedReact::KnockDown);
    CHECK(pedestrianReact(t, 694.f, 0.f, 0.f).react == PedReact::KnockDown);
    CHECK(pedestrianReact(t, 694.f, -20.f, 0.f).react == PedReact::KnockDown); // just behind the bumper line
    // In the clearance band only within the cone; beside or behind the body, clear, or a stopped car: nothing.
    CHECK(pedestrianReact(t, 694.f, 50.f, 120.f).react == PedReact::None);
    CHECK(pedestrianReact(t, 694.f, 100.f, 300.f).react == PedReact::None);
    CHECK(pedestrianReact(t, 694.f, -100.f, 0.f).react == PedReact::None);
    CHECK(pedestrianReact(t, 694.f, 800.f, 130.f).react == PedReact::None);
    CHECK(pedestrianReact(t, 0.f, 50.f, 0.f).react == PedReact::None);
    CHECK(pedestrianReact(t, NAN, 50.f, 0.f).react == PedReact::None);
    CHECK(pedestrianReact(t, 694.f, INFINITY, 0.f).react == PedReact::None);

    // Anyone stepping out inside the zone (2 to 8 m ahead at the cap) dodges or stumbles.
    for (float a = 200.f; a <= 800.f; a += 10.f) {
        const PedReact r = pedestrianReact(t, t.nearCap, a, 0.f).react;
        CHECK((r == PedReact::Dodge || r == PedReact::Stumble));
    }
    // Over every speed and spot: a fall only at or above stumbleBelow, and no contact above nearCap.
    for (float v = 0.f; v <= 100.f * kKmh; v += 50.f) {
        for (float a = -60.f; a <= 1500.f; a += 15.f) {
            for (float side = -130.f; side <= 130.f; side += 13.f) {
                o = pedestrianReact(t, v, a, side);
                CHECK(o.contact <= t.nearCap + 1e-3f);
                if (o.react == PedReact::KnockDown) CHECK(o.contact >= t.stumbleBelow);
                if (o.react == PedReact::Stumble) CHECK(o.contact < t.stumbleBelow);
            }
        }
    }
}

TEST_CASE("vehicle pedestrians: the cap is a braking curve, so a van at 80 km/h is down to 25 before them") {
    const PedestrianTuning t;
    CHECK(pedestrianCap(t, 500.f, 300.f) == doctest::Approx(t.nearCap));
    CHECK(pedestrianCap(t, kNobodyAhead, 0.f) == 0.f);
    CHECK(pedestrianCap(t, INFINITY, 0.f) == 0.f);
    // Bad numbers fail safe: a cap still applies.
    CHECK(pedestrianCap(t, NAN, 300.f) == doctest::Approx(t.nearCap));
    CHECK(pedestrianCap(t, -1.f, 0.f) == doctest::Approx(1.f));       // at the bumper, in the path: stop
    CHECK(pedestrianCap(t, -1.f, 300.f) == doctest::Approx(t.nearCap)); // at the bumper line, to the side
    CHECK(pedestrianCap(t, 200.f, NAN) == pedestrianCap(t, 200.f, 0.f)); // side unknown: in the path
    // Past nearDistance it rises as the distance a firm brake needs to come down to nearCap.
    const float far = pedestrianCap(t, t.nearDistance + t.brakeMargin + 2000.f, 300.f);
    CHECK(far == doctest::Approx(std::sqrt(t.nearCap * t.nearCap + 2.f * t.brakeDecel * 2000.f)));
    // In the path it stops short of them, and is never 0 (which would read as no limit).
    CHECK(pedestrianCap(t, 200.f, 0.f) < t.nearCap);
    CHECK(pedestrianCap(t, 50.f, 0.f) > 0.f);

    VehicleTuning vt;
    vt.speedCap = 80.f * kKmh;
    VehicleDrive car;
    car.setTuning(vt);
    constexpr float kNose = 260.f; // rear axle to the front bumper
    // Pedestrians on the pavement (2 m to the side) at 60 m: slowed from farther out the faster it goes.
    float startAt[2] = {0.f, 0.f};
    const float speeds[2] = {80.f * kKmh, 40.f * kKmh};
    for (int k = 0; k < 2; ++k) {
        VehicleState s = car.parked(0.f, 0.f, 0.f);
        s.forward = speeds[k];
        const float ped = 6000.f;
        float atNear = -1.f;
        float atPed = -1.f;
        float peakDecel = 0.f;
        for (int i = 0; i < 1200 && atPed < 0.f; ++i) {
            const float ahead = ped - (s.x + kNose);
            if (atNear < 0.f && ahead <= t.nearDistance) atNear = s.forward;
            if (ahead <= 0.f) atPed = s.forward;
            VehicleInput in;
            in.throttle = 1.f;
            applySpeedLimits(t, in, speeds[k], ahead, 200.f);
            const float before = s.forward;
            car.step(s, in, kFrame);
            if (startAt[k] == 0.f && s.forward < before - 1.f) startAt[k] = ahead;
            if (ahead > t.nearDistance + 50.f) peakDecel = std::max(peakDecel, (before - s.forward) / kFrame);
        }
        CAPTURE(k);
        CAPTURE(atNear);
        CHECK(atNear <= t.nearCap * 1.02f);  // down to 25 km/h by the last metres (the clamp only trims the frame lag)
        CHECK(atPed >= 0.f);
        CHECK(atPed <= t.nearCap + 1e-3f);   // never past them faster than 25 km/h
        CHECK(peakDecel <= t.brakeDecel * 1.05f); // a firm brake, not a wall
    }
    CHECK(startAt[0] > startAt[1] + 1000.f); // the zone scales with speed
    CHECK(startAt[0] == doctest::Approx(t.nearDistance + t.brakeMargin + (speeds[0] * speeds[0] - t.nearCap * t.nearCap) / (2.f * t.brakeDecel)).epsilon(0.05));

    // One standing in its path: it stops short of them and stays stopped with the throttle held.
    VehicleState s = car.parked(0.f, 0.f, 0.f);
    s.forward = 80.f * kKmh;
    float ahead = 0.f;
    for (int i = 0; i < 900; ++i) {
        ahead = 6000.f - (s.x + kNose);
        VehicleInput in;
        in.throttle = 1.f;
        applySpeedLimits(t, in, 0.f, ahead, 0.f);
        car.step(s, in, kFrame);
    }
    CHECK(s.forward == 0.f);
    CHECK(ahead > 0.f);
    CHECK(ahead <= t.stopGap + 30.f);
}

TEST_CASE("vehicle limits: the lowest real one wins; 0 means none and never wipes a limit") {
    CHECK(combinedLimit(0.f, 0.f) == 0.f);
    CHECK(combinedLimit(1667.f, 0.f) == 1667.f); // no pedestrian: the road's limit stands
    CHECK(combinedLimit(0.f, 694.f) == 694.f);   // no road limit: the pedestrian's
    CHECK(combinedLimit(1667.f, 694.f) == 694.f);
    CHECK(combinedLimit(278.f, 694.f) == 278.f); // 25 never raises a 10 km/h lane
    CHECK(combinedLimit(-5.f, 694.f) == 694.f);
    CHECK(combinedLimit(NAN, 694.f) == 694.f);

    const PedestrianTuning t;
    VehicleInput in;
    applySpeedLimits(t, in, 278.f, 500.f, 300.f);
    CHECK(in.limit == 278.f);
    CHECK(in.limitDecel == 0.f); // the lane's own limit: the governor's rate
    applySpeedLimits(t, in, 1667.f, 1500.f, 300.f);
    CHECK(in.limit < 1667.f);
    CHECK(in.limitDecel == t.brakeDecel);
    CHECK(in.clamp == 0.f);
    applySpeedLimits(t, in, 0.f, 400.f, 300.f);
    CHECK(in.clamp == t.nearCap);
    applySpeedLimits(t, in, 1667.f, NAN, 0.f); // a bad distance: capped and clamped
    CHECK(in.limit == doctest::Approx(t.nearCap));
    CHECK(in.clamp == t.nearCap);
    applySpeedLimits(t, in, 1667.f, kNobodyAhead, 0.f); // nobody ahead
    CHECK(in.limit == 1667.f);
    CHECK(in.limitDecel == 0.f);
    CHECK(in.clamp == 0.f);
}

TEST_CASE("vehicle seats: held stopped while getting in or out; bad follower counts and odd numbers refused") {
    const SeatTuning st;
    PartySeats p;
    REQUIRE(tryEnter(st, p, 100.f, 40.f, true, nullptr, 0));
    VehicleDrive car;
    VehicleState s = car.parked(0.f, 0.f, 0.f);
    s.forward = 40.f;
    for (int i = 0; i < 60; ++i) {
        VehicleInput in;
        in.throttle = 1.f;
        in.steer = 1.f;
        holdForSeats(p, in);
        car.step(s, in, kFrame);
    }
    CHECK(s.forward == 0.f);
    CHECK(s.x < 5.f);
    VehicleInput seated;
    seated.throttle = 1.f;
    p.phase = SeatPhase::Seated;
    holdForSeats(p, seated);
    CHECK(seated.throttle == 1.f);
    CHECK_FALSE(seated.handbrake);
    p.phase = SeatPhase::Exiting;
    holdForSeats(p, seated);
    CHECK(seated.throttle == 0.f);
    CHECK(seated.brake == 0.f);
    CHECK(seated.handbrake);

    const float two[2] = {100.f, 100.f};
    PartySeats q;
    CHECK_FALSE(tryEnter(st, q, 0.f, 0.f, true, two, -1));
    CHECK_FALSE(tryEnter(st, q, 0.f, 0.f, true, two, 3));
    CHECK_FALSE(tryEnter(st, q, 0.f, 0.f, true, nullptr, 2));
    CHECK_FALSE(tryEnter(st, q, NAN, 0.f, true, nullptr, 0));
    CHECK_FALSE(tryEnter(st, q, 0.f, NAN, true, nullptr, 0));
    CHECK(q.phase == SeatPhase::OnFoot);
    const float odd[2] = {NAN, 100.f};
    REQUIRE(tryEnter(st, q, 0.f, 0.f, true, odd, 2));
    CHECK(q.boardAt[1] == -1.f); // a distance that is not a number: placed in at the door shut
    CHECK(std::isfinite(q.length));
    tickSeats(st, q, NAN, 0.f, false, true, true);
    CHECK(q.time == 0.f);
    tickSeats(st, q, 5.f, NAN, false, true, true);
    CHECK(q.canDrive());
}

TEST_CASE("vehicle rules: numbers that are not finite change nothing") {
    const WearTuning t;
    WearState w;
    addImpact(t, w, NAN, 100.f, 0.f);
    addImpact(t, w, INFINITY, 100.f, 0.f);
    addImpact(t, w, 900.f, NAN, 0.f);
    addDistance(t, w, NAN);
    for (const float z : w.zone) CHECK(z == 0.f);
    CHECK(w.dirt == 0.f);
    w.zone[0] = NAN;
    CHECK(wearStage(w, WearZone::Front) == 0);
    addImpact(t, w, 950.f, 100.f, 0.f);
    CHECK(wearStage(w, WearZone::Front) == 2);
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

TEST_CASE("lamps: a van parked empty at night stands dark") {
    const LampState empty = lampState(true, 0.f, true, false);
    CHECK_FALSE(empty.head);
    CHECK_FALSE(empty.tail);
    CHECK_FALSE(empty.brake);
    CHECK_FALSE(lampState(true, 0.f, false, false).tail);
}

TEST_CASE("lamps: the night band switches on at 0.55 and off at 0.45") {
    CHECK_FALSE(nightBand(0.f, false));
    CHECK_FALSE(nightBand(0.54f, false));
    CHECK(nightBand(kNightOn, false));
    CHECK(nightBand(0.9f, false));
    CHECK(nightBand(0.5f, true));      // inside the band: stays on
    CHECK_FALSE(nightBand(0.5f, false)); // inside the band: stays off
    CHECK(nightBand(0.46f, true));
    CHECK_FALSE(nightBand(kNightOff, true));
    CHECK_FALSE(nightBand(0.1f, true));
    CHECK(nightBand(NAN, true));
    CHECK_FALSE(nightBand(NAN, false));
}

TEST_CASE("lamps: head and tail at night, brake under brake input or a held handbrake") {
    const LampState day = lampState(false, 0.f, false, true);
    CHECK_FALSE(day.head);
    CHECK_FALSE(day.tail);
    CHECK_FALSE(day.brake);
    const LampState night = lampState(true, 0.f, false, true);
    CHECK(night.head);
    CHECK(night.tail);
    CHECK_FALSE(night.brake);
    CHECK_FALSE(lampState(true, kBrakeLampInput, false, true).brake);
    CHECK(lampState(false, 0.5f, false, false).brake);
    CHECK(lampState(false, 0.f, true, true).brake);
    CHECK_FALSE(lampState(false, 0.f, true, false).brake);
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
        sum += o.down + o.sidestep + pedestrianCap(r.pedestrian, static_cast<float>(i), 0.f);
        if (p.phase == SeatPhase::OnFoot) tryEnter(r.seats, p, 0.f, 0.f, true, nullptr, 0);
        tickSeats(r.seats, p, 0.1f, 0.f, i % 20 > 10, true, true);
    }
    CHECK(std::isfinite(sum));
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
}
