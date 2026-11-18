#include "ftcore/WetPrints.h"

#include "ftcore/WaterBodies.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;
constexpr float kStrideRate = 0.9f; // strides (a left and a right plant) a second: a walk

// Walks `seconds` out of the water from a soaking, the body drying as WaterWetness does; calls `onLaid` with each print.
template <class F>
void walkDrying(WetPrints& prints, WaterWetness& wet, float& phase, float seconds, F&& onLaid) {
    const int steps = static_cast<int>(seconds / kDt + 0.5f);
    for (int i = 0; i < steps; ++i) {
        wet.step(0.f, 0.f, kDt);
        phase += kStrideRate * kDt;
        if (phase >= 1.f) phase -= 1.f;
        const int laid = prints.step(phase, true, wet.wetness, kDt);
        if (laid >= 0) onLaid(laid);
    }
}

WaterWetness soaked() {
    WaterWetness wet;
    wet.step(1.f, 1.2f, kDt, 1.f, waterSilt(WaterKind::Canal));
    return wet;
}

} // namespace

TEST_CASE("wet prints: laid at each plant of the foot phase, left at the wrap and right at the half, alternating") {
    WetPrints prints;
    WaterWetness wet = soaked();
    float phase = 0.1f;
    std::vector<WetFoot> feet;
    walkDrying(prints, wet, phase, 10.f, [&](int i) { feet.push_back(prints.print(static_cast<std::size_t>(i)).foot); });
    // 0.9 strides a second for 10 s from 0.1: plants at 0.5, 1.0, 1.5 ... up to 9.1 -> 18 plants.
    CHECK(feet.size() == 18u);
    REQUIRE(!feet.empty());
    CHECK(feet.front() == WetFoot::Right);
    for (std::size_t i = 1; i < feet.size(); ++i) CHECK(feet[i] != feet[i - 1]);
    CHECK(prints.laid() == 18u);
    CHECK(prints.live() == 18u);

    // A plant on the exact wrap (the phase landing on 0) is a left plant, laid once.
    WetPrints exact;
    CHECK(exact.step(0.9f, true, 1.f, kDt) < 0);
    const int left = exact.step(0.f, true, 1.f, kDt);
    REQUIRE(left >= 0);
    CHECK(exact.print(static_cast<std::size_t>(left)).foot == WetFoot::Left);
    CHECK(exact.step(0.f, true, 1.f, kDt) < 0);
    CHECK(exact.step(0.05f, true, 1.f, kDt) < 0);
}

TEST_CASE("wet prints: none while not on foot, none from a phase jump, none at wetness 0") {
    WetPrints prints;
    // Off foot (falling, swimming, wading, standing): the phase passes both plants and nothing is laid.
    for (float p : {0.1f, 0.4f, 0.6f, 0.9f, 0.05f, 0.45f, 0.55f}) CHECK(prints.step(p, false, 1.f, kDt) < 0);
    // The first step back on foot only takes up the phase, even across a plant.
    CHECK(prints.step(0.49f, true, 1.f, kDt) < 0);
    CHECK(prints.step(0.51f, true, 1.f, kDt) >= 0);
    // A jump of more than kWetPrintMaxAdvance (a clip handing the body back) lays nothing, from either side.
    CHECK(prints.step(0.95f, true, 1.f, kDt) < 0);
    CHECK(prints.step(0.4f, true, 1.f, kDt) < 0);
    CHECK(prints.laid() == 1u);
    // Dry: the plants pass and nothing is laid.
    WetPrints dry;
    float phase = 0.f;
    int laid = 0;
    for (int i = 0; i < 600; ++i) {
        phase += kStrideRate * kDt;
        if (phase >= 1.f) phase -= 1.f;
        laid += dry.step(phase, true, 0.f, kDt) >= 0 ? 1 : 0;
    }
    CHECK(laid == 0);
    CHECK(dry.laid() == 0u);
    CHECK(dry.live() == 0u);
}

TEST_CASE("wet prints: strength is the stamp less age over 120 s on the dry clock, fading to exactly 0") {
    WetPrints prints;
    prints.step(0.45f, true, 0.6f, kDt);
    const int i = prints.step(0.55f, true, 0.6f, kDt);
    REQUIRE(i >= 0);
    const std::size_t at = static_cast<std::size_t>(i);
    CHECK(prints.print(at).stamp == doctest::Approx(0.6f));
    CHECK(prints.strength(at) == doctest::Approx(0.6f));
    CHECK(WetPrints::life(0.6f) == doctest::Approx(72.f));
    // 36 s on (standing still, off foot): half gone.
    for (int s = 0; s < 36 * 60; ++s) prints.step(0.f, false, 0.f, kDt);
    CHECK(prints.strength(at) == doctest::Approx(0.3f).epsilon(0.01));
    // A faster drying scale ages it faster, the same as the body: 18 s at x2 is the other 36.
    for (int s = 0; s < 18 * 60; ++s) prints.step(0.f, false, 0.f, kDt, 2.f);
    CHECK(prints.strength(at) < 0.005f);
    for (int s = 0; s < 60; ++s) prints.step(0.f, false, 0.f, kDt);
    CHECK(prints.strength(at) == 0.f);
    CHECK(prints.live() == 0u);
    // A slot never laid has no strength.
    CHECK(prints.strength(at + 1) == 0.f);
    CHECK(prints.strength(kWetPrintCap) == 0.f);
}

TEST_CASE("wet prints: walking off a soaking, no newer print is ever stronger than an older one, all gone by 120 s") {
    WetPrints prints;
    WaterWetness wet = soaked();
    float phase = 0.f;
    std::vector<int> order;
    bool ordered = true;
    for (int i = 0; i < 130 * 60; ++i) {
        wet.step(0.f, 0.f, kDt);
        phase += kStrideRate * kDt;
        if (phase >= 1.f) phase -= 1.f;
        const int laid = prints.step(phase, true, wet.wetness, kDt);
        if (laid >= 0) order.push_back(laid);
        if (i % 30 == 0) {
            for (std::size_t k = 1; k < order.size(); ++k) {
                const float older = prints.strength(static_cast<std::size_t>(order[k - 1]));
                const float newer = prints.strength(static_cast<std::size_t>(order[k]));
                if (newer > older + 1e-6f) ordered = false;
            }
        }
        if (i == 60 * 60) CHECK(prints.live() > 0u);
    }
    CHECK(ordered);
    // The body dried at 90 s and laid no more; the first prints went at 120 s.
    CHECK(wet.wetness == 0.f);
    CHECK(prints.live() == 0u);
    CHECK(order.size() > 140u);
    CHECK(order.size() < 170u);
    // The first prints (wetness 0.9 and over) have drops, the most first; the rest none.
    CHECK(prints.print(static_cast<std::size_t>(order.front())).drops > 0.9f);
    CHECK(prints.print(static_cast<std::size_t>(order[order.size() / 2])).drops == 0.f);
}

TEST_CASE("wet prints: never more than the cap, the oldest replaced, and no heap in the step") {
    WetPrints prints;
    float phase = 0.f;
    std::size_t most = 0;
    const auto before = ftcore_test::allocStats().count;
    // Six plants a second at full wetness, the clock barely moving, for long enough to lay the cap twice and more.
    for (int i = 0; i < static_cast<int>(kWetPrintCap) * 25; ++i) {
        phase += 3.f * kDt;
        if (phase >= 1.f) phase -= 1.f;
        prints.step(phase, true, 1.f, kDt * 0.01f);
        most = std::max(most, prints.live());
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(most == kWetPrintCap);
    CHECK(prints.laid() > 2u * kWetPrintCap);
    // The ring holds the newest kWetPrintCap prints.
    std::uint32_t lowest = prints.laid();
    for (std::size_t i = 0; i < kWetPrintCap; ++i) lowest = std::min(lowest, prints.print(i).serial);
    CHECK(lowest == prints.laid() - static_cast<std::uint32_t>(kWetPrintCap) + 1u);
    prints.clear();
    CHECK(prints.live() == 0u);
    CHECK(prints.laid() == 0u);
}
