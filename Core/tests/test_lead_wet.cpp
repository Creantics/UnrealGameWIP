#include "ftcore/LeadWet.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>

namespace lw = ftcore::leadwet;

TEST_CASE("lead wet: dry by default; ft.LeadWet forces, below 0 follows the world") {
    CHECK(lw::weather(0.f, 0.9f) == 0.f); // 0 holds the leads dry, whatever the world
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

TEST_CASE("lead wet: an out-of-range lead comes into the party with a warning; one in range is kept") {
    static_assert(lw::leadInParty(1, 3).lead == 1 && !lw::leadInParty(1, 3).wrapped);
    CHECK(lw::leadInParty(3, 3).lead == 0);
    CHECK(lw::leadInParty(3, 3).wrapped);
    CHECK(lw::leadInParty(7, 3).lead == 1);
    CHECK(lw::leadInParty(7, 3).wrapped);
    CHECK(lw::leadInParty(-1, 3).lead == 0);
    CHECK(lw::leadInParty(-1, 3).wrapped);
    CHECK(lw::leadInParty(5, 0).lead == 0); // no party: the default names walk behind member 0, nothing to warn of
    CHECK_FALSE(lw::leadInParty(5, 0).wrapped);
    // Every party size and every lead index, in range or not: the lead lands in the party, warned exactly when it was out,
    // and the followers behind it are every other member once.
    for (int count = 1; count <= lw::kPartyWetMembers; ++count) {
        for (int lead = -2; lead < 3 * count; ++lead) {
            const lw::PartyLead got = lw::leadInParty(lead, count);
            CAPTURE(count);
            CAPTURE(lead);
            CHECK(got.lead >= 0);
            CHECK(got.lead < count);
            CHECK(got.wrapped == (lead < 0 || lead >= count));
            if (lead >= 0 && lead < count) {
                CHECK(got.lead == lead);
            }
            bool seen[lw::kPartyWetMembers]{};
            seen[got.lead] = true;
            for (int i = 0; i < count - 1; ++i) {
                const int m = lw::followerMember(got.lead, i, count);
                REQUIRE(m >= 0);
                REQUIRE(m < count);
                CHECK_FALSE(seen[m]);
                seen[m] = true;
            }
        }
    }
}

TEST_CASE("lead wet: each follower's walk place indexes its own member, whatever spawns fail, for every lead") {
    // Every party size, every lead, and every pattern of failed spawns over the walk places (a failed spawn is left out of
    // the roster, so a follower's index and its place part): place and member stay together, ft.PartyWet's slot p wets
    // only the follower at place p, and the soak it stores goes to its own member.
    std::size_t cases = 0;
    const auto before = ftcore_test::allocStats().count;
    for (int count = 2; count <= lw::kPartyWetMembers; ++count) {
        const int places = count - 1;
        for (int lead = 0; lead < count; ++lead) {
            for (unsigned fails = 0; fails < (1u << places); ++fails) {
                lw::FollowerRoster roster;
                for (int p = 0; p < places; ++p) {
                    if ((fails >> p) & 1u) {
                        continue; // this spawn failed
                    }
                    REQUIRE(roster.add(p, lead, count) == lw::followerMember(lead, p, count));
                }
                for (int k = 0; k < roster.count; ++k) {
                    const int p = roster.placeOf(k);
                    REQUIRE(((fails >> p) & 1u) == 0u);
                    REQUIRE(roster.memberOf(k) == (lead + 1 + p) % count);
                    REQUIRE(roster.memberOf(k) != lead);
                }
                for (int slot = -1; slot < places; ++slot) {
                    const lw::PartyWetForce force{true, slot, 0.9f};
                    for (int k = 0; k < roster.count; ++k) {
                        const bool named = slot == -1 || slot == roster.placeOf(k);
                        REQUIRE(roster.weatherOf(k, 0.1f, force) == doctest::Approx(named ? 0.9f : 0.1f));
                    }
                }
                lw::PartyWet party;
                for (int k = 0; k < roster.count; ++k) {
                    party.store(roster.memberOf(k), ftcore::WaterWetness{0.1f * static_cast<float>(roster.placeOf(k) + 1), 0.5f, 0.f}, 0.0);
                }
                for (int p = 0; p < places; ++p) {
                    const int m = lw::followerMember(lead, p, count);
                    if ((fails >> p) & 1u) {
                        REQUIRE_FALSE(party.stored(m));
                    } else {
                        REQUIRE(party.of(m, 0.0).wetness == doctest::Approx(0.1f * static_cast<float>(p + 1)));
                    }
                }
                ++cases;
            }
        }
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    CHECK(cases > 0);
    if (ftcore_test::allocHookInstalled()) {
        CHECK(allocs == 0);
    }
    // Out of range and a full roster.
    lw::FollowerRoster roster;
    CHECK(roster.placeOf(0) == -1);
    CHECK(roster.memberOf(-1) == -1);
    CHECK(roster.weatherOf(0, 0.3f, lw::PartyWetForce{true, -1, 1.f}) == doctest::Approx(0.3f));
    for (int i = 0; i < lw::kPartyWetMembers; ++i) {
        roster.add(0, 0, 2);
    }
    CHECK(roster.full());
    CHECK(roster.add(0, 0, 2) == -1);
    CHECK(roster.count == lw::kPartyWetMembers);
}

TEST_CASE("lead wet: the first look eases the skin in from dry; a later look lands it at the member's soak") {
    // A start in full rain: the first look (none worn before) does not snap, so the face eases from dry over kSkinEaseS.
    const float target = lw::skin(1.f);
    REQUIRE(target == doctest::Approx(1.f));
    constexpr float dt = 1.f / 60.f;
    bool snap = lw::skinSnapOnLook(false);
    CHECK_FALSE(snap);
    float shown = 0.f; // WearLook resets it
    shown = lw::shownSkin(shown, target, dt, snap);
    snap = false;
    CHECK(shown == doctest::Approx(dt / lw::kSkinEaseS)); // one frame's step, not the target
    float largest = shown;
    for (int f = 1; f < 60; ++f) { // the first second: never a jump bigger than a frame's step
        const float next = lw::shownSkin(shown, target, dt, snap);
        CHECK(next - shown <= dt / lw::kSkinEaseS + 1e-6f);
        largest = next;
        shown = next;
    }
    CHECK(largest == doctest::Approx(0.5f).epsilon(0.02));
    for (int f = 0; f < 70; ++f) {
        shown = lw::shownSkin(shown, target, dt, snap);
    }
    CHECK(shown == doctest::Approx(target)); // there by kSkinEaseS
    // A later look (a lead switch): the member's own soak lands at once, then eases from there.
    snap = lw::skinSnapOnLook(true);
    CHECK(snap);
    shown = 0.f;
    shown = lw::shownSkin(shown, lw::skin(0.4f), dt, snap);
    CHECK(shown == doctest::Approx(lw::skin(0.4f)));
    snap = false;
    CHECK(lw::shownSkin(shown, 0.f, dt, snap) == doctest::Approx(lw::skin(0.4f) - dt / lw::kSkinEaseS));
    // NaN target: dry, snapped or eased.
    CHECK(lw::shownSkin(0.4f, std::nanf(""), dt, true) == 0.f);
}

#ifdef FTCORE_LEADWET_HAS_RAIN
TEST_CASE("lead wet: the rain drives the weather value unless ft.LeadWet forces it") {
    ftcore::RainSpec spec;
    const ftcore::RainState none{};
    CHECK(lw::weatherFromRain(-1.f, none) == 0.f); // no rain: dry
    const ftcore::RainState full = ftcore::rainSettled(spec, 1.f);
    CHECK(lw::weatherFromRain(-1.f, full) == doctest::Approx(full.wetness));
    CHECK(full.wetness >= lw::kGroomWetOn); // a settled full rain wears the wet groom
    CHECK(lw::groomWet(lw::weatherFromRain(-1.f, full), false));
    CHECK(lw::weatherFromRain(0.f, full) == 0.f);                          // forced dry wins over the rain
    CHECK(lw::weatherFromRain(0.3f, full) == doctest::Approx(0.3f));       // forced value wins
    CHECK(lw::weatherFromRain(-1.f, full, 0.f) == 0.f);                    // under cover
    CHECK(lw::weatherFromRain(-1.f, full, 0.5f) == doctest::Approx(full.wetness * 0.5f));
    CHECK(lw::weatherFromRain(std::nanf(""), full) == doctest::Approx(full.wetness)); // NaN follows the rain
    ftcore::RainState bad = full;
    bad.wetness = std::nanf("");
    CHECK(lw::weatherFromRain(-1.f, bad) == 0.f);
    // It ramps with the rain: starting dry under a full rain, the lead wets over time, then dries after it stops.
    ftcore::RainState s{};
    s.target = 1.f;
    for (int i = 0; i < 60; ++i) s = ftcore::rainStep(spec, s, 0.5f);   // 30 s
    const float wet30 = lw::weatherFromRain(-1.f, s);
    CHECK(wet30 > 0.f);
    CHECK(wet30 < full.wetness);
    for (int i = 0; i < 1200; ++i) s = ftcore::rainStep(spec, s, 0.5f); // 10 min
    CHECK(lw::weatherFromRain(-1.f, s) == doctest::Approx(full.wetness));
    s.target = 0.f;
    for (int i = 0; i < 120; ++i) s = ftcore::rainStep(spec, s, 0.5f);  // 1 min dry
    CHECK(lw::weatherFromRain(-1.f, s) < full.wetness);
}

TEST_CASE("lead wet: cover from the head trace uses the ground cover's rule and bias") {
    const ftcore::RainCoverSpec cover;
    CHECK(lw::coverExposure(false, 0.f, 1.8f, cover) == 1.f);                  // open sky
    CHECK(lw::coverExposure(true, 4.f, 1.8f, cover) == 0.f);                   // a roof 2.2 m over the head
    CHECK(lw::coverExposure(true, 1.8f + cover.biasM * 0.5f, 1.8f, cover) == 1.f); // under the bias: reads as its own
    // The ground point under the same roof agrees.
    CHECK(lw::coverExposure(true, 4.f, 0.f, cover) == ftcore::rainCoverOpen(4.f, 0.f, cover.biasM));
}

TEST_CASE("lead wet: under cover in rain a lead is not wetted and dries only at the rain's own rate") {
    const ftcore::RainSpec spec;
    const ftcore::RainState rain = ftcore::rainSettled(spec, 1.f);
    CHECK(lw::rainSoakSettled(rain, 1.f) == doctest::Approx(rain.wetness)); // loaded into a settled rain: wet
    CHECK(lw::rainSoakSettled(rain, 0.f) == 0.f);                            // ... under a roof: dry
    // Walks under an awning soaked 0.8: one step later it holds its soak but for the slow drying, never a snap.
    const float prior = 0.8f;
    const float a = lw::rainSoakStep(spec, prior, rain, 0.f, 0.25f);
    CHECK(a <= prior);
    CHECK(a == doctest::Approx(prior - 0.25f * spec.dryPerS));
    // A dry lead under cover in the rain stays dry.
    float s = 0.f;
    for (int i = 0; i < 400; ++i) s = lw::rainSoakStep(spec, s, rain, 0.f, 0.25f);
    CHECK(s == 0.f);
    // Out in the open it wets at the rain's rise rate, then holds the soak.
    s = lw::rainSoakStep(spec, 0.f, rain, 1.f, 0.25f);
    CHECK(s == doctest::Approx(0.25f * spec.wetRisePerS));
    for (int i = 0; i < 2000; ++i) s = lw::rainSoakStep(spec, s, rain, 1.f, 0.25f);
    CHECK(s == doctest::Approx(rain.wetness));
    // Bad steps change nothing; a long hitch is clamped.
    CHECK(lw::rainSoakStep(spec, 0.4f, rain, 1.f, std::nanf("")) == doctest::Approx(0.4f));
    CHECK(lw::rainSoakStep(spec, 0.4f, rain, 1.f, -1.f) == doctest::Approx(0.4f));
    CHECK(lw::rainSoakStep(spec, 0.f, rain, 1.f, 60.f) == doctest::Approx(ftcore::kRainMaxStepS * spec.wetRisePerS));
    // The weather value reads the soak.
    CHECK(lw::weather(-1.f, a) == doctest::Approx(a));
    CHECK(lw::weather(0.f, a) == 0.f);
}

TEST_CASE("lead wet: the cover trace runs every quarter second, not every frame") {
    float clock = 0.f;
    CHECK(lw::coverTraceDue(clock, 0.016f)); // first frame
    int n = 0;
    for (int i = 0; i < 60; ++i) n += lw::coverTraceDue(clock, 1.f / 60.f) ? 1 : 0;
    CHECK(n == 4);
    CHECK_FALSE(lw::coverTraceDue(clock, std::nanf("")));
}
#endif
