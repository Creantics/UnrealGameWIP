#include "ftcore/UiWorldChange.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 30.f;
constexpr std::int8_t kSol = static_cast<std::int8_t>(UiWorld::Solhaven);
constexpr std::int8_t kLat = static_cast<std::int8_t>(UiWorld::Lattice);

UiWorldChangeIn at(std::int8_t world, bool on = true, bool moving = true, bool drawn = true) {
    UiWorldChangeIn in;
    in.world = world;
    in.on = on;
    in.moving = moving;
    in.drawn = drawn;
    return in;
}

void run(UiWorldChange& s, const UiWorldChangeIn& in, float seconds) {
    for (float t = 0.f; t < seconds - 1e-4f; t += kDt) uiWorldChangeStep(s, in, kDt);
}

} // namespace

TEST_CASE("ui world change: off by default, the HUD does nothing as the wave passes") {
    CHECK_FALSE(UiWorldChangeIn{}.on);
    UiWorldChange s;
    uiWorldChangeStep(s, at(kSol, false), kDt);
    CHECK(s.shown == kSol); // the first world: taken silently
    CHECK_FALSE(s.running);
    for (int i = 0; i < 60; ++i) {
        CAPTURE(i);
        uiWorldChangeStep(s, at(i < 30 ? kSol : kLat, false), kDt);
        CHECK_FALSE(s.running);
        CHECK(s.shown == kSol);
        CHECK(uiWorldChangeMix(s, 0.f) == 1.f);
        CHECK(uiWorldChangeMix(s, 1.f) == 1.f);
    }
    uiWorldChangeStep(s, at(kLat, false, true, false), kDt); // nothing drawn: the ink changes unseen
    CHECK(s.shown == kLat);
    CHECK_FALSE(s.running);
}

TEST_CASE("ui world change: switched on, the crossing's frame starts a 0.3 s sweep") {
    UiWorldChange s;
    run(s, at(kSol), 1.f);
    CHECK_FALSE(s.running);
    uiWorldChangeStep(s, at(kLat), kDt);
    CHECK(s.running);
    CHECK(s.from == kSol);
    CHECK(s.shown == kLat);
    CHECK(uiWorldChangeMix(s, 0.f) > 0.f);
    CHECK(uiWorldChangeMix(s, 0.f) > uiWorldChangeMix(s, 1.f)); // the first point leads
    float last = uiWorldChangeMix(s, 0.5f);
    for (int i = 0; i < 6; ++i) {
        uiWorldChangeStep(s, at(kLat), kDt);
        CAPTURE(i);
        CHECK(uiWorldChangeMix(s, 0.5f) >= last);
        last = uiWorldChangeMix(s, 0.5f);
    }
    CHECK(s.running); // seven frames, about 0.23 s
    CHECK(uiWorldChangeMix(s, 0.f) == 1.f);
    CHECK(uiWorldChangeMix(s, 1.f) < 1.f);
    run(s, at(kLat), 0.2f);
    CHECK_FALSE(s.running);
    CHECK(s.shown == kLat);
    CHECK(s.from == kUiWorldNone);
    CHECK(uiWorldChangeMix(s, 1.f) == 1.f);
    // Edge-detected: the same world on later frames starts nothing.
    run(s, at(kLat), 2.f);
    CHECK_FALSE(s.running);
}

TEST_CASE("ui world change: nothing for a settled edge, a hidden HUD or no world") {
    UiWorldChange s;
    uiWorldChangeStep(s, at(kUiWorldNone), kDt);
    CHECK(s.shown == kUiWorldNone);
    uiWorldChangeStep(s, at(kLat), kDt);
    CHECK(s.shown == kLat); // the first world: no envelope
    CHECK_FALSE(s.running);
    run(s, at(kSol, true, false), 1.f); // a settled edge (a load, a teleport) with the rows drawn: it waits
    CHECK_FALSE(s.running);
    CHECK(s.shown == kLat);
    uiWorldChangeStep(s, at(kSol, true, false, false), kDt);
    CHECK(s.shown == kSol);
    CHECK_FALSE(s.running);
    uiWorldChangeStep(s, at(kLat, true, true, false), kDt); // a crossing with nothing drawn: unseen
    CHECK(s.shown == kLat);
    CHECK_FALSE(s.running);
    uiWorldChangeStep(s, at(kUiWorldNone), kDt); // no world read: kept
    CHECK(s.shown == kLat);
    CHECK_FALSE(uiWorldChangeValid(static_cast<std::int8_t>(kUiWorldCount)));
}

TEST_CASE("ui world change: the camera backing out runs the sweep back without a jump") {
    UiWorldChange s;
    run(s, at(kSol), 0.5f);
    uiWorldChangeStep(s, at(kLat), kDt);
    run(s, at(kLat), 2.f * kDt);
    CHECK(s.running);
    const float xs[] = {0.f, 0.5f, 1.f};
    float lattice[3] = {};
    for (int i = 0; i < 3; ++i) lattice[i] = uiWorldChangeMix(s, xs[i]); // the Lattice ink's share
    uiWorldChangeStep(s, at(kSol), 0.f);
    CHECK(s.running);
    CHECK(s.shown == kSol);
    CHECK(s.from == kLat);
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(1.f - uiWorldChangeMix(s, xs[i]) == doctest::Approx(lattice[i]));
    }
    run(s, at(kSol), 0.4f);
    CHECK_FALSE(s.running);
    CHECK(s.shown == kSol);
}

TEST_CASE("ui world change: stepping allocates nothing") {
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    UiWorldChange s;
    float sum = 0.f;
    for (int i = 0; i < 300; ++i) {
        uiWorldChangeStep(s, at((i / 20) % 2 ? kLat : kSol), kDt);
        sum += uiWorldChangeMix(s, 0.25f);
    }
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK(sum > 0.f);
}
