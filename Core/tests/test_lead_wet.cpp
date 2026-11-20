#include "ftcore/LeadWet.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>

namespace lw = ftcore::leadwet;

TEST_CASE("lead wet: dry by default; ft.LeadWet forces, below 0 follows the world") {
    CHECK(lw::weather(0.f, 0.9f) == 0.f); // the default 0 is dry, whatever the world
    CHECK(lw::weather(0.7f, 0.f) == doctest::Approx(0.7f));
    CHECK(lw::weather(3.f, 0.f) == 1.f);
    CHECK(lw::weather(-1.f, 0.4f) == doctest::Approx(0.4f));
    CHECK(lw::weather(-1.f, std::nanf("")) == 0.f);
    CHECK(lw::weather(std::nanf(""), 0.5f) == doctest::Approx(0.5f)); // NaN is not >= 0: follows the world
}

TEST_CASE("lead wet: skin and hair take the wetter of the weather and the lead's own water") {
    CHECK(lw::surface(0.f, ftcore::WaterWetness{}) == 0.f); // dry by default
    CHECK(lw::surface(0.f, ftcore::WaterWetness{0.7f, 0.5f, 0.f}) == doctest::Approx(0.7f)); // wading, no rain
    CHECK(lw::surface(0.9f, ftcore::WaterWetness{0.2f, 0.3f, 0.f}) == doctest::Approx(0.9f)); // rain
    CHECK(lw::surface(std::nanf(""), ftcore::WaterWetness{std::nanf(""), 0.f, 0.f}) == 0.f);
    // Wading alone wears the wet groom and wets the skin.
    CHECK(lw::groomWet(lw::surface(0.f, ftcore::WaterWetness{0.8f, 1.f, 0.f}), false));
    CHECK(lw::skin(lw::surface(0.f, ftcore::WaterWetness{0.8f, 1.f, 0.f})) > 0.9f);
}

TEST_CASE("lead wet: cloth takes the wetter of the water and the weather") {
    const ftcore::WaterWetness wading{0.8f, 0.6f, 0.3f};
    const auto a = lw::cloth(wading, 0.5f); // the water is wetter: unchanged
    CHECK(a.wetness == doctest::Approx(0.8f));
    CHECK(a.line == doctest::Approx(0.6f));
    CHECK(a.silt == doctest::Approx(0.3f));
    const auto b = lw::cloth(wading, 1.f); // rain: whole body, the stain stays
    CHECK(b.wetness == 1.f);
    CHECK(b.line == doctest::Approx(lw::kRainLineM));
    CHECK(b.silt == doctest::Approx(0.3f));
    const auto c = lw::cloth(ftcore::WaterWetness{}, 0.f); // dry stays dry
    CHECK(c.wetness == 0.f);
    CHECK(c.line == 0.f);
}

TEST_CASE("lead wet: the groom swap holds between its two edges") {
    bool wet = false;
    wet = lw::groomWet(0.45f, wet);
    CHECK_FALSE(wet);
    wet = lw::groomWet(0.5f, wet);
    CHECK(wet);
    wet = lw::groomWet(0.4f, wet); // inside the gap: stays wet
    CHECK(wet);
    wet = lw::groomWet(0.3f, wet);
    CHECK_FALSE(wet);
    // A value hovering at one edge swaps at most once.
    int swaps = 0;
    for (int i = 0; i < 200; ++i) {
        const bool next = lw::groomWet(lw::kGroomWetOn + (i % 2 ? 0.01f : -0.01f), wet);
        swaps += next != wet;
        wet = next;
    }
    CHECK(swaps == 1);
}

TEST_CASE("lead wet: wet skin goes to about 0.25 on the mask, never rougher") {
    CHECK(lw::skin(0.f) == 0.f);
    CHECK(lw::skin(lw::kSkinFrom) == 0.f);
    CHECK(lw::skin(lw::kSkinTo) == doctest::Approx(1.f));
    CHECK(lw::skin(1.f) == doctest::Approx(1.f));
    float last = 0.f;
    for (int i = 0; i <= 100; ++i) { // rises without a step back
        const float s = lw::skin(i / 100.f);
        CHECK(s >= last);
        last = s;
    }
    CHECK(lw::skinRoughness(0.42f, 1.f, 1.f) == doctest::Approx(0.25f));
    CHECK(lw::skinRoughness(0.42f, 1.f, 0.f) == doctest::Approx(0.42f)); // off the mask: as dry
    CHECK(lw::skinRoughness(0.42f, 0.f, 1.f) == doctest::Approx(0.42f)); // dry
    CHECK(lw::skinRoughness(0.15f, 1.f, 1.f) == doctest::Approx(0.15f)); // already smoother: not raised
}

TEST_CASE("lead wet: the shown skin eases over two seconds, both ways") {
    float s = 0.f;
    for (int i = 0; i < 59; ++i) {
        s = lw::ease(s, 1.f, 1.f / 30.f); // 59 frames at 30 Hz: not yet there
    }
    CHECK(s < 1.f);
    CHECK(s > 0.9f);
    s = lw::ease(s, 1.f, 1.f);
    CHECK(s == 1.f); // never past the target
    CHECK(lw::ease(1.f, 0.f, 1.f) == doctest::Approx(0.5f));
    CHECK(lw::ease(0.2f, 0.f, 10.f) == 0.f);
    CHECK(lw::ease(0.3f, 1.f, std::nanf("")) == doctest::Approx(0.3f)); // NaN dt holds
    CHECK(lw::ease(0.3f, 1.f, -1.f) == doctest::Approx(0.3f));
    CHECK(lw::ease(0.f, 0.f, 0.016f) == 0.f); // dry stays dry
}

TEST_CASE("lead wet: MetaHuman skin and wet-less hair follow the wet amount") {
    CHECK(lw::mhRoughness(1.2f, 0.f) == doctest::Approx(1.2f));
    CHECK(lw::mhRoughness(1.2f, 1.f) == doctest::Approx(0.6f)); // half of dry
    CHECK(lw::mhSpecular(1.f, 1.f) == doctest::Approx(1.15f));
    CHECK(lw::mhSpecular(1.f, 0.f) == doctest::Approx(1.f));
    CHECK(lw::hairMelanin(0.7f, 1.f) == doctest::Approx(0.9f));
    CHECK(lw::hairMelanin(0.95f, 1.f) == 1.f);
    CHECK(lw::hairMelanin(0.7f, 0.f) == doctest::Approx(0.7f));
    CHECK(lw::hairRoughness(0.6f, 1.f) == doctest::Approx(0.36f));
    CHECK(lw::hairRoughness(0.6f, 0.f) == doctest::Approx(0.6f));
}

TEST_CASE("lead wet: the per-frame mapping allocates nothing") {
    const auto before = ftcore_test::allocStats().count;
    float acc = 0.f;
    bool wet = false;
    for (int frame = 0; frame < 1000; ++frame) {
        const float w = lw::weather(-1.f, static_cast<float>(frame % 100) / 100.f);
        acc += lw::cloth(ftcore::WaterWetness{}, w).wetness + lw::skin(w);
        wet = lw::groomWet(w, wet);
    }
    CHECK(acc > 0.f);
    if (ftcore_test::allocHookInstalled()) {
        CHECK(ftcore_test::allocStats().count - before == 0);
    }
}

TEST_CASE("lead wet: the party switch reads <slot>,<wetness> and is off for anything else") {
    const lw::PartyWetForce one = lw::parsePartyWet("1,0.9");
    CHECK(one.on);
    CHECK(one.slot == 1);
    CHECK(one.wetness == doctest::Approx(0.9f));
    const lw::PartyWetForce all = lw::parsePartyWet(" -1 , 1 ");
    CHECK(all.on);
    CHECK(all.slot == -1);
    CHECK(all.wetness == doctest::Approx(1.f));
    CHECK(lw::parsePartyWet("0,2").wetness == doctest::Approx(1.f)); // clamped
    CHECK(lw::parsePartyWet("2,.5").wetness == doctest::Approx(0.5f));
    for (const char* bad : {"", "x", "1", "1,", "1;0.5", "-2,0.5", "1,0.5z", ",0.5"}) {
        CHECK_FALSE(lw::parsePartyWet(bad).on);
    }
    CHECK_FALSE(lw::parsePartyWet(nullptr).on);
    static_assert(lw::parsePartyWet("0,1").on);
}

TEST_CASE("lead wet: a follower takes the lead's weather unless the switch names it") {
    const lw::PartyWetForce off{};
    CHECK(lw::partyWeather(0.f, 0, off) == 0.f);   // dry by default
    CHECK(lw::partyWeather(0.4f, 2, off) == doctest::Approx(0.4f));
    const lw::PartyWetForce one = lw::parsePartyWet("1,0.9");
    CHECK(lw::partyWeather(0.f, 1, one) == doctest::Approx(0.9f));
    CHECK(lw::partyWeather(0.f, 0, one) == 0.f);   // the one beside it stays dry
    const lw::PartyWetForce all = lw::parsePartyWet("-1,0.7");
    for (int slot = 0; slot < 3; ++slot) {
        CHECK(lw::partyWeather(0.f, slot, all) == doctest::Approx(0.7f));
    }
    // A follower's own water soak starts dry: the cloth then is the weather's alone.
    const ftcore::WaterWetness soak = lw::cloth(ftcore::WaterWetness{}, lw::partyWeather(0.f, 1, one));
    CHECK(soak.wetness == doctest::Approx(0.9f));
    CHECK(lw::cloth(ftcore::WaterWetness{}, lw::partyWeather(0.f, 0, one)).wetness == 0.f);
}
