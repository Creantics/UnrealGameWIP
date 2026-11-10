#include "ftcore/VehicleDrive.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace ftcore;

namespace {

constexpr float kFrame = 1.f / 60.f;
constexpr float kKmh = 100000.f / 3600.f;

// Runs `seconds` of frames with `in`; returns the distance travelled (cm).
float run(const VehicleDrive& car, VehicleState& s, const VehicleInput& in, float seconds) {
    float dist = 0.f;
    const int frames = static_cast<int>(std::lround(seconds / kFrame));
    for (int i = 0; i < frames; ++i) {
        const float x = s.x;
        const float y = s.y;
        car.step(s, in, kFrame);
        dist += std::hypot(s.x - x, s.y - y);
    }
    return dist;
}

VehicleInput throttle(float v = 1.f) {
    VehicleInput in;
    in.throttle = v;
    return in;
}

VehicleInput braking(float v = 1.f) {
    VehicleInput in;
    in.brake = v;
    return in;
}

VehicleDrive atCap(VehicleState& s) {
    VehicleDrive car;
    s = car.parked(0.f, 0.f, 0.f);
    run(car, s, throttle(), 12.f);
    return car;
}

} // namespace

TEST_CASE("vehicle: full throttle reaches the speed cap and never passes it") {
    VehicleDrive car;
    VehicleState s = car.parked(0.f, 0.f, 0.f);
    float firstAt50 = -1.f;
    float peak = 0.f;
    for (int i = 0; i < 60 * 12; ++i) {
        car.step(s, throttle(), kFrame);
        peak = std::max(peak, s.forward);
        if (firstAt50 < 0.f && s.forward >= 50.f * kKmh) firstAt50 = static_cast<float>(i + 1) * kFrame;
    }
    CHECK(firstAt50 > 3.f); // 0-50 km/h in about 3.7 s: a small city car
    CHECK(firstAt50 < 4.5f);
    CHECK(peak <= car.tuning().speedCap + 1e-3f);
    CHECK(s.forward == doctest::Approx(car.tuning().speedCap).epsilon(0.005));
    CHECK(s.y == doctest::Approx(0.f)); // straight ahead along +x
    CHECK(s.side == doctest::Approx(0.f));
}

TEST_CASE("vehicle: the brake stops the car, then backs it up to its reverse speed; throttle backing up brakes first") {
    VehicleState s;
    const VehicleDrive car = atCap(s);
    float dist = 0.f;
    int frames = 0;
    while (s.forward > 0.f && frames < 600) {
        const float x = s.x;
        car.step(s, braking(), kFrame);
        dist += s.x - x;
        ++frames;
    }
    const float v0 = car.tuning().speedCap;
    CHECK(dist == doctest::Approx(v0 * v0 / (2.f * car.tuning().brakeDecel)).epsilon(0.03)); // about 14.6 m from 60 km/h
    CHECK(s.forward > -10.f); // stopped, at most just backing off

    run(car, s, braking(), 6.f);
    CHECK(s.forward < 0.f);
    CHECK(s.forward >= -car.tuning().reverseSpeed - 1e-3f);
    CHECK(s.forward == doctest::Approx(-car.tuning().reverseSpeed).epsilon(0.01));

    // Throttle while rolling backward brakes the car to a stop before it drives forward.
    float lowest = s.forward;
    bool stoppedFirst = false;
    for (int i = 0; i < 120; ++i) {
        const float before = s.forward;
        car.step(s, throttle(), kFrame);
        if (before < 0.f && s.forward >= 0.f) stoppedFirst = std::abs(s.forward) < 50.f;
        lowest = std::min(lowest, s.forward);
    }
    CHECK(stoppedFirst);
    CHECK(lowest >= -car.tuning().reverseSpeed - 1e-3f);
    CHECK(s.forward > 0.f);
}

TEST_CASE("vehicle: released, the car coasts down to a dead stop") {
    VehicleState s;
    const VehicleDrive car = atCap(s);
    const float coast = run(car, s, VehicleInput{}, 60.f);
    CHECK(s.forward == 0.f);
    CHECK(s.side == 0.f);
    CHECK(coast > 5000.f); // it rolls a good way before stopping
    const float x = s.x;
    run(car, s, VehicleInput{}, 1.f);
    CHECK(s.x == x); // and doesn't creep
}

TEST_CASE("vehicle: a road's limit reins the car back and caps it") {
    VehicleState s;
    const VehicleDrive car = atCap(s);
    VehicleInput lane = throttle();
    lane.limit = 10.f * kKmh;
    run(car, s, lane, 1.f);
    CHECK(s.forward < car.tuning().speedCap);
    CHECK(s.forward > lane.limit); // slowing at governorDecel, not snapped
    run(car, s, lane, 4.f);
    CHECK(s.forward == doctest::Approx(lane.limit).epsilon(0.005));
    CHECK(car.topSpeed(s, lane) == doctest::Approx(lane.limit));
    VehicleInput fast = throttle();
    fast.limit = 200.f * kKmh;
    CHECK(car.topSpeed(s, fast) == doctest::Approx(car.tuning().speedCap)); // never above the car's own cap
}

TEST_CASE("vehicle: steering eases to the lock, narrows with speed and turns on the bicycle model's circle") {
    VehicleDrive car;
    VehicleState s = car.parked(0.f, 0.f, 0.f);
    VehicleInput in;
    in.steer = 1.f;
    car.step(s, in, kFrame);
    CHECK(s.steer > 0.f);
    CHECK(s.steer < 35.f * 3.14159265f / 180.f * 0.2f); // eased, not snapped
    run(car, s, in, 0.5f);
    CHECK(s.steer == doctest::Approx(35.f * 3.14159265f / 180.f).epsilon(0.001)); // full lock at a standstill
    CHECK(s.heading == 0.f); // no yaw without speed

    // Creeping at 5 km/h on full right lock: the circle's radius is wheelbase / tan(steer).
    in.throttle = 1.f;
    in.limit = 5.f * kKmh;
    run(car, s, in, 6.f);
    CHECK(s.forward == doctest::Approx(in.limit).epsilon(0.01));
    CHECK(s.yawRate > 0.f); // positive steer turns toward +y
    const float radius = s.forward / s.yawRate;
    CHECK(radius == doctest::Approx(car.tuning().wheelbase / std::tan(s.steer)).epsilon(0.01));
    CHECK(radius < 450.f);

    // At speed the same input asks for a smaller lock, and the yaw stays under the tyres' grip.
    VehicleState f;
    const VehicleDrive fastCar = atCap(f);
    VehicleInput turn = throttle();
    turn.steer = 1.f;
    run(fastCar, f, turn, 1.f);
    CHECK(f.steer < s.steer * 0.6f);
    CHECK(std::abs(f.forward * f.yawRate) <= fastCar.tuning().gripLat * 1.001f);
    CHECK(std::abs(f.side) < 20.f); // gripping, not sliding

    // Let go and the wheels come back to straight faster than they went out.
    run(fastCar, f, throttle(), 0.3f);
    CHECK(f.steer == doctest::Approx(0.f));
}

TEST_CASE("vehicle: the handbrake lets the tail slide and turns the car harder") {
    VehicleState grip;
    const VehicleDrive car = atCap(grip);
    VehicleState slide = grip;
    VehicleInput in;
    in.steer = 1.f;
    VehicleInput hb = in;
    hb.handbrake = true;
    run(car, grip, in, 0.8f);
    run(car, slide, hb, 0.8f);
    CHECK(std::abs(grip.side) < 20.f);
    CHECK(std::abs(slide.side) > 100.f);
    CHECK(slide.heading > grip.heading);
    CHECK(slide.speed() < grip.speed()); // and scrubs speed
}

TEST_CASE("vehicle: the same inputs give the same path, however the frames are cut") {
    VehicleDrive car;
    VehicleState a = car.parked(100.f, -50.f, 0.3f);
    VehicleState b = a;
    VehicleInput in = throttle(0.8f);
    in.steer = -0.4f;
    for (int i = 0; i < 240; ++i) car.step(a, in, 1.f / 60.f);
    for (int i = 0; i < 120; ++i) car.step(b, in, 1.f / 30.f);
    CHECK(a.x == doctest::Approx(b.x).epsilon(1e-4));
    CHECK(a.y == doctest::Approx(b.y).epsilon(1e-4));
    CHECK(a.heading == doctest::Approx(b.heading).epsilon(1e-4));
    VehicleState c = car.parked(100.f, -50.f, 0.3f);
    for (int i = 0; i < 240; ++i) car.step(c, in, 1.f / 60.f);
    CHECK(c.x == a.x); // bit for bit on a repeat
    CHECK(c.y == a.y);
    // A long hitch steps at most 32 substeps, not a burst.
    VehicleState d = car.parked(0.f, 0.f, 0.f);
    car.step(d, throttle(), 5.f);
    CHECK(d.forward == doctest::Approx(car.tuning().engineAccel * 32.f * car.tuning().substep).epsilon(0.01));
}

TEST_CASE("vehicle: a wall stops the speed into it and scrapes the speed along it") {
    VehicleDrive car;
    VehicleState s = car.parked(0.f, 0.f, 0.f);
    s.forward = 1000.f;
    CHECK(car.collide(s, -1.f, 0.f) == doctest::Approx(1000.f)); // head on
    CHECK(s.forward == doctest::Approx(-150.f));                  // bounces back at restitution
    CHECK(car.collide(s, -1.f, 0.f) == 0.f);                      // already moving away
    CHECK(car.collide(s, 0.f, 0.f) == 0.f);

    // At 30 degrees to a wall along +x: 500 cm/s into it, the 866 along it loses 0.6 x 500.
    VehicleState g = car.parked(0.f, 0.f, 30.f * 3.14159265f / 180.f);
    g.forward = 1000.f;
    CHECK(car.collide(g, 0.f, -2.f) == doctest::Approx(500.f).epsilon(0.001));
    CHECK(g.velX() == doctest::Approx(866.03f - 300.f).epsilon(0.001));
    CHECK(g.velY() == doctest::Approx(-75.f).epsilon(0.001));
}

TEST_CASE("vehicle: a low kerb is driven up at a cost, a high one is a wall") {
    VehicleDrive car;
    VehicleState s = car.parked(0.f, 0.f, 0.f);
    s.forward = 800.f;
    CHECK(car.mountKerb(s, 12.f));
    CHECK(s.forward == doctest::Approx(800.f - 48.f));
    CHECK_FALSE(car.mountKerb(s, 20.f));
    CHECK(s.forward == doctest::Approx(752.f)); // unchanged: the host collides instead
    CHECK(car.mountKerb(s, 0.f));
    VehicleState slow = car.parked(0.f, 0.f, 0.f);
    slow.forward = 20.f;
    CHECK(car.mountKerb(slow, 15.f));
    CHECK(slow.forward == 0.f); // too slow to climb it
}

TEST_CASE("vehicle: the shipped micro-van loads, and bad tunables are refused") {
    std::string text;
    if (!ftcore_test::readProjectData("Vehicles/microvan.json", text)) FAIL("Data/Vehicles/microvan.json not found");
    VehicleTuning t;
    std::string err;
    REQUIRE(importVehicleTuning(text, t, &err));
    CHECK(t.speedCap == doctest::Approx(80.f * kKmh));
    CHECK(t.reverseSpeed == doctest::Approx(18.f * kKmh));
    CHECK(t.wheelbase == doctest::Approx(190.f));
    CHECK(t.guideSpeedCap == 0.f);

    VehicleTuning keep;
    keep.wheelbase = 123.f;
    CHECK_FALSE(importVehicleTuning(R"({"version":1,"drive":{"wheelbase":300,"grip":5}})", keep, &err));
    CHECK(err == "drive.grip: not a known tunable");
    CHECK(keep.wheelbase == 123.f); // unchanged on failure
    CHECK_FALSE(importVehicleTuning(R"({"version":1,"drive":{"speedCap":1000,"speedCapKmh":40}})", keep, &err));
    CHECK_FALSE(importVehicleTuning(R"({"version":1,"drive":{"maxSteer":90}})", keep, &err));
    CHECK(err == "drive.maxSteer: out of range");
    CHECK_FALSE(importVehicleTuning(R"({"version":2})", keep, &err));
    CHECK_FALSE(importVehicleTuning(R"({"version":1,"engine":{}})", keep, &err));
    CHECK(importVehicleTuning(R"({"version":1,"drive":{"speedCapKmh":36}})", keep, &err));
    CHECK(keep.speedCap == doctest::Approx(1000.f));
    CHECK(keep.wheelbase == 123.f); // members left out keep what was there
}

TEST_CASE("vehicle: driving allocates nothing") {
    VehicleDrive car;
    VehicleState s = car.parked(0.f, 0.f, 0.f);
    const bool hooked = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (int i = 0; i < 2000; ++i) {
        VehicleInput in;
        in.throttle = i % 300 < 200 ? 1.f : 0.f;
        in.brake = i % 300 >= 250 ? 1.f : 0.f;
        in.steer = std::sin(static_cast<float>(i) * 0.01f);
        in.handbrake = i % 97 == 0;
        car.step(s, in, kFrame);
        if (i % 50 == 0) car.collide(s, -1.f, 0.2f);
        if (i % 70 == 0) car.mountKerb(s, 10.f);
    }
    CHECK(std::isfinite(s.x + s.y + s.heading));
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
}

namespace {

// A rail bike's data: slow off the rail, quick on it.
VehicleDrive railBike() {
    VehicleTuning t;
    t.speedCap = 30.f * kKmh;
    t.guideSpeedCap = 80.f * kKmh;
    t.engineAccel = 450.f;
    VehicleDrive d;
    d.setTuning(t);
    return d;
}

// Frames along a rail on the x axis (offset = y), with `in` otherwise.
void ride(const VehicleDrive& d, VehicleState& s, VehicleInput in, float seconds) {
    for (int i = 0; i < static_cast<int>(std::lround(seconds / kFrame)); ++i) {
        in.guide.present = true;
        in.guide.dirX = 1.f;
        in.guide.dirY = 0.f;
        in.guide.offset = s.y;
        d.step(s, in, kFrame);
    }
}

} // namespace

TEST_CASE("vehicle: a guide surface latches a vehicle that has one, lines it up and lets it run faster") {
    const VehicleDrive bike = railBike();
    VehicleState s = bike.parked(0.f, 40.f, 10.f * 3.14159265f / 180.f);
    VehicleInput in = throttle();
    in.steer = 1.f; // ignored once latched
    ride(bike, s, in, 0.5f);
    CHECK(s.latched);
    CHECK(s.heading == doctest::Approx(0.f).epsilon(0.001));
    CHECK(std::abs(s.y) < 1.f);
    CHECK(s.steer == doctest::Approx(0.f));
    ride(bike, s, in, 15.f);
    CHECK(s.latched);
    CHECK(s.forward == doctest::Approx(80.f * kKmh).epsilon(0.01));
    CHECK(std::abs(s.y) < 0.5f);
    CHECK(s.side == 0.f);

    // Detached, it is the ordinary vehicle again and is reined back to its own cap.
    in.detach = true;
    in.steer = 0.f;
    ride(bike, s, in, 6.f);
    CHECK_FALSE(s.latched);
    CHECK(s.forward == doctest::Approx(30.f * kKmh).epsilon(0.01));

    // Too far from the line, or across it, it doesn't latch; the other way along it, it does.
    VehicleState far = bike.parked(0.f, 100.f, 0.f);
    ride(bike, far, throttle(), 0.2f);
    CHECK_FALSE(far.latched);
    VehicleState across = bike.parked(0.f, 10.f, 40.f * 3.14159265f / 180.f);
    ride(bike, across, VehicleInput{}, 0.1f);
    CHECK_FALSE(across.latched);
    VehicleState back = bike.parked(0.f, -20.f, 3.f);
    ride(bike, back, throttle(), 1.f);
    CHECK(back.latched);
    CHECK(std::abs(back.heading) == doctest::Approx(3.14159265f).epsilon(0.001));
    CHECK(back.x < 0.f);

    // A car without guide data never latches.
    VehicleDrive car;
    VehicleState c = car.parked(0.f, 0.f, 0.f);
    ride(car, c, throttle(), 1.f);
    CHECK_FALSE(c.latched);
}
