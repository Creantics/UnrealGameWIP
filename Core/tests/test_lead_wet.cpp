#include "ftcore/LeadWet.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <array>
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

TEST_CASE("lead wet: a switch that is off forces nothing, whatever slot and wetness it holds") {
    // Off by default (slot 0, wetness 0): follower 0 takes the lead's weather, not the 0 the switch holds.
    CHECK(lw::partyWeather(0.6f, 0, lw::PartyWetForce{}) == doctest::Approx(0.6f));
    // Off with a slot and wetness filled in (as a rejected parse might leave them): still the lead's.
    const lw::PartyWetForce stale{false, 1, 0.9f};
    CHECK(lw::partyWeather(0.2f, 1, stale) == doctest::Approx(0.2f));
    const lw::PartyWetForce staleAll{false, -1, 1.f};
    for (int slot = 0; slot < 3; ++slot) {
        CHECK(lw::partyWeather(0.3f, slot, staleAll) == doctest::Approx(0.3f));
    }
    // Text the parser turns down leaves every follower on the lead's weather.
    CHECK(lw::partyWeather(0.5f, 0, lw::parsePartyWet("x")) == doctest::Approx(0.5f));
    static_assert(lw::partyWeather(0.5f, 1, lw::PartyWetForce{false, 1, 0.f}) == 0.5f);
}

TEST_CASE("lead wet: every follower steps its own soak and shows it under its party weather, allocating nothing") {
    // As AFTExploreDirector::TickFollowers and AFTPartyFollower::StepWet run it: two followers (slot 1 the Third), each
    // with its own WaterWetness stepped as the lead's is, then shown through cloth and surface of its party weather.
    constexpr float kHeight = 1.8f; // m, the follower's body (FTPartyFollower.cpp kWaterBodyHeight)
    constexpr float kDt = 1.f / 60.f;
    const float silt = ftcore::waterSilt(ftcore::WaterKind::Canal);
    const lw::PartyWetForce off{};
    const lw::PartyWetForce all = lw::parsePartyWet("-1,0.8");
    const lw::PartyWetForce third = lw::parsePartyWet("1,0.2");
    std::array<ftcore::WaterWetness, 2> wet{};
    std::array<ftcore::WaterWetness, 2> swum{}, dried{};
    std::array<float, 2> swumSkin{}, allCloth{};
    float thirdOwnWins = 0.f;
    const auto before = ftcore_test::allocStats().count;
    // The Third swims for 2 s (1.2 m under) while follower 0 stays on the bank.
    for (int frame = 0; frame < 120; ++frame) {
        for (std::size_t i = 0; i < wet.size(); ++i) {
            wet[i].step(i == 1 ? 1.2f : 0.f, kHeight, kDt, 1.f, silt);
            const float w = lw::partyWeather(0.f, static_cast<int>(i), off);
            swum[i] = lw::cloth(wet[i], w);
            swumSkin[i] = lw::surface(w, wet[i]);
        }
    }
    // Out of the water for 45 s, both still shown each frame.
    for (int frame = 0; frame < 45 * 60; ++frame) {
        for (std::size_t i = 0; i < wet.size(); ++i) {
            wet[i].step(0.f, kHeight, kDt, 1.f, silt);
            dried[i] = lw::cloth(wet[i], lw::partyWeather(0.f, static_cast<int>(i), off));
            allCloth[i] = lw::cloth(wet[i], lw::partyWeather(0.f, static_cast<int>(i), all)).wetness;
        }
        thirdOwnWins = lw::cloth(wet[1], lw::partyWeather(0.f, 1, third)).wetness;
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    CHECK(swum[1].wetness == doctest::Approx(1.f));
    CHECK(swum[1].line == doctest::Approx(1.2f));
    CHECK(swum[1].silt == doctest::Approx(silt));
    CHECK(swumSkin[1] == doctest::Approx(1.f));
    CHECK(swum[0].wetness == 0.f); // the one on the bank stays dry
    CHECK(swumSkin[0] == 0.f);
    CHECK(dried[1].wetness == doctest::Approx(0.5f).epsilon(0.01)); // its own clock: half dry after 45 s
    CHECK(dried[0].wetness == 0.f);
    // The switch for every follower reaches both, the Third included; the Third's own wetter soak still wins over a
    // weaker force.
    CHECK(allCloth[0] == doctest::Approx(0.8f));
    CHECK(allCloth[1] == doctest::Approx(0.8f));
    CHECK(thirdOwnWins == doctest::Approx(0.5f).epsilon(0.01));
    if (ftcore_test::allocHookInstalled()) {
        CHECK(allocs == 0);
    }
}

TEST_CASE("lead wet: the soak stays with the member across a lead switch") {
    // A party of three (lead 0, followers 1 and 2), as SpawnFollowers walks it.
    CHECK(lw::followerMember(0, 0, 3) == 1);
    CHECK(lw::followerMember(0, 1, 3) == 2);
    CHECK(lw::followerMember(1, 0, 3) == 2);
    CHECK(lw::followerMember(1, 1, 3) == 0);
    CHECK(lw::followerMember(2, 1, 3) == 1);
    CHECK(lw::followerMember(0, 2, 3) == -1); // only count - 1 followers
    CHECK(lw::followerMember(3, 0, 3) == -1);
    CHECK(lw::followerMember(-1, 0, 3) == -1);
    CHECK(lw::followerMember(0, -1, 3) == -1);
    CHECK(lw::followerMember(0, 0, 0) == -1);
    static_assert(lw::followerMember(2, 0, 3) == 0);

    // The lead (member 0) has swum; follower 0 (member 1) is dry, follower 1 (member 2) waded to the knee.
    ftcore::WaterWetness lead{1.f, 1.2f, 0.4f};
    const ftcore::WaterWetness knee{0.6f, 0.45f, 0.1f};
    std::array<ftcore::WaterWetness, 2> followers{ftcore::WaterWetness{}, knee};
    lw::PartyWet party;
    const auto before = ftcore_test::allocStats().count;
    // Switch to member 1: as OnLeadChanged and ClearFollowers store them, the old lead's and each follower's.
    party.store(0, lead, 0.0);
    for (int i = 0; i < 2; ++i) {
        party.store(lw::followerMember(0, i, 3), followers[static_cast<std::size_t>(i)], 0.0);
    }
    lead = party.of(1, 0.0); // the new lead takes its own
    for (int i = 0; i < 2; ++i) {
        followers[static_cast<std::size_t>(i)] = party.of(lw::followerMember(1, i, 3), 0.0); // SpawnFollowers
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    CHECK(lead.wetness == 0.f); // member 1 was dry
    CHECK(followers[0].wetness == doctest::Approx(0.6f)); // member 2 walks first now, still knee-wet
    CHECK(followers[0].line == doctest::Approx(0.45f));
    CHECK(followers[1].wetness == doctest::Approx(1.f)); // the old lead, still soaked, with its line and stain
    CHECK(followers[1].line == doctest::Approx(1.2f));
    CHECK(followers[1].silt == doctest::Approx(0.4f));
    // And back: the round trip gives each its own again.
    party.store(1, lead, 0.0);
    for (int i = 0; i < 2; ++i) {
        party.store(lw::followerMember(1, i, 3), followers[static_cast<std::size_t>(i)], 0.0);
    }
    CHECK(party.of(0, 0.0).wetness == doctest::Approx(1.f));
    CHECK(party.of(1, 0.0).wetness == 0.f);
    CHECK(party.of(2, 0.0).silt == doctest::Approx(0.1f));
    // Out of range: store is a no-op and says so, of is dry.
    CHECK_FALSE(party.store(-1, lead, 0.0));
    CHECK_FALSE(party.store(lw::kPartyWetMembers, ftcore::WaterWetness{1.f, 2.f, 1.f}, 0.0));
    CHECK(party.of(lw::kPartyWetMembers, 0.0).wetness == 0.f);
    CHECK(party.of(-1, 0.0).silt == 0.f);
    if (ftcore_test::allocHookInstalled()) {
        CHECK(allocs == 0);
    }
}

TEST_CASE("lead wet: a member away dries by the water's own law; a switch at no time away is exact") {
    lw::PartyWet party;
    CHECK_FALSE(party.stored(0)); // never stored: nothing to give back, so no skin snap
    CHECK(party.of(0, 100.0).wetness == 0.f);
    const ftcore::WaterWetness soaked{1.f, 1.2f, 0.4f};
    const ftcore::WaterWetness knee{0.6f, 0.45f, 0.f};
    REQUIRE(party.store(2, soaked, 10.0));
    REQUIRE(party.store(3, knee, 10.0));
    CHECK(party.stored(2));
    CHECK_FALSE(party.stored(1));
    // No time away (a lead switch stores and takes back in the same frame): bit-identical.
    const ftcore::WaterWetness same = party.of(2, 10.0);
    CHECK(same.wetness == soaked.wetness);
    CHECK(same.line == soaked.line);
    CHECK(same.silt == soaked.silt);
    CHECK(party.of(2, 5.0).wetness == soaked.wetness); // an earlier time holds it
    // Half the dry time away: half as wet, line and stain kept.
    const float half = ftcore::kWaterDryTime * 0.5f;
    const ftcore::WaterWetness back = party.of(2, 10.0 + static_cast<double>(half));
    CHECK(back.wetness == doctest::Approx(0.5f));
    CHECK(back.line == doctest::Approx(1.2f));
    CHECK(back.silt == doctest::Approx(0.4f));
    // The same as a follower drying out of the water in steps (StepWet's law).
    ftcore::WaterWetness stepped = soaked;
    for (int i = 0; i < 45; ++i) {
        stepped.step(0.f, 0.f, half / 45.f);
    }
    CHECK(back.wetness == doctest::Approx(stepped.wetness));
    // Long gone: dry; with no stain the line goes too, a stain keeps it.
    const ftcore::WaterWetness kneeLater = party.of(3, 10.0 + 10.0 * static_cast<double>(ftcore::kWaterDryTime));
    CHECK(kneeLater.wetness == 0.f);
    CHECK(kneeLater.line == 0.f);
    CHECK(party.of(2, 1.0e6).line == doctest::Approx(1.2f));
    // The first look eases in; a later one lands at once.
    static_assert(!lw::skinSnapOnLook(false));
    static_assert(lw::skinSnapOnLook(true));
}
