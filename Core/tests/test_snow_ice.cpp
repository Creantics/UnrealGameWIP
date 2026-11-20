// Snow and ice underfoot (ftcore/SnowIce.h): trudge by depth, slips on frozen water only, harmless ice patches.

#include "ftcore/SnowIce.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace ftcore;

TEST_CASE("snow and ice: the default tuning is valid; broken tunings are refused") {
    const SnowIceTuning t;
    CHECK(validSnowIceTuning(t));
    SnowIceTuning bad = t;
    bad.trudgeFullM = bad.trudgeStartM; // no depth range
    CHECK_FALSE(validSnowIceTuning(bad));
    bad = t;
    bad.trudgeMinScale = 0.f; // snow that stops a lead dead
    CHECK_FALSE(validSnowIceTuning(bad));
    bad = t;
    bad.patchGrip = bad.iceGrip * 0.5f; // a patch worse than frozen water is no longer harmless
    CHECK_FALSE(validSnowIceTuning(bad));
    bad = t;
    bad.snowGrip = 1.5f;
    CHECK_FALSE(validSnowIceTuning(bad));
    bad = t;
    bad.slipS = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(validSnowIceTuning(bad));
}

TEST_CASE("snow and ice: trudge slows by depth, from none to the slowest, never faster with more snow") {
    const SnowIceTuning t;
    CHECK(trudgeScale(t, 0.f) == 1.f);
    CHECK(trudgeScale(t, t.trudgeStartM) == 1.f);
    CHECK(trudgeScale(t, t.trudgeFullM) == doctest::Approx(t.trudgeMinScale));
    CHECK(trudgeScale(t, 5.f) == doctest::Approx(t.trudgeMinScale));
    // Halfway through the range: halfway down.
    const float mid = 0.5f * (t.trudgeStartM + t.trudgeFullM);
    CHECK(trudgeScale(t, mid) == doctest::Approx(0.5f * (1.f + t.trudgeMinScale)));
    // Monotone, inside [minScale, 1].
    float last = 1.f;
    for (int i = 0; i <= 200; ++i) {
        const float d = 0.005f * static_cast<float>(i);
        const float s = trudgeScale(t, d);
        INFO("depth " << d << " scale " << s);
        CHECK(s <= last);
        CHECK(s >= t.trudgeMinScale);
        CHECK(s <= 1.f);
        last = s;
    }
    // Bad reads are no snow.
    CHECK(trudgeScale(t, -1.f) == 1.f);
    CHECK(trudgeScale(t, std::numeric_limits<float>::quiet_NaN()) == 1.f);
    CHECK(trudgeScale(t, std::numeric_limits<float>::infinity()) == 1.f);
}

TEST_CASE("snow and ice: the footing under a step") {
    const SnowIceTuning t;
    CHECK(footingAt(t, 0.f, false, 0.f, false) == Footing::Ground);
    CHECK(footingAt(t, 0.02f, false, 0.f, false) == Footing::Ground); // a dusting
    CHECK(footingAt(t, 0.3f, false, 0.f, false) == Footing::Snow);
    CHECK(footingAt(t, 0.f, false, 0.f, true) == Footing::IcePatch);
    CHECK(footingAt(t, 0.3f, false, 0.f, true) == Footing::Snow); // deep snow over a patch
    // Water: thin ice is open water; thick enough holds; snow on held ice walks like snow.
    CHECK(footingAt(t, 0.f, true, 0.f, false) == Footing::OpenWater);
    CHECK(footingAt(t, 0.f, true, t.iceHoldM * 0.5f, false) == Footing::OpenWater);
    CHECK(footingAt(t, 0.f, true, t.iceHoldM, false) == Footing::FrozenWater);
    CHECK(footingAt(t, 0.3f, true, 0.5f, false) == Footing::Snow);
    CHECK(footingAt(t, 0.3f, true, 0.01f, false) == Footing::OpenWater); // snow on thin ice doesn't hold
    CHECK(footingAt(t, 0.f, true, std::numeric_limits<float>::quiet_NaN(), false) == Footing::OpenWater);
    // Grip: ground best, open water none, a patch never worse than frozen water.
    CHECK(footingGrip(t, Footing::Ground) == 1.f);
    CHECK(footingGrip(t, Footing::OpenWater) == 0.f);
    CHECK(footingGrip(t, Footing::Snow) == doctest::Approx(t.snowGrip));
    CHECK(footingGrip(t, Footing::IcePatch) >= footingGrip(t, Footing::FrozenWater));
    CHECK(footingGrip(t, Footing::FrozenWater) > 0.f);
}

TEST_CASE("snow and ice: frozen water slips on a sharp turn or a stop at speed; nothing else ever slips") {
    const SnowIceTuning t;
    const float fast = t.slipSpeedMS + 1.f;
    const float slow = t.slipSpeedMS - 0.5f;
    CHECK(stepSlips(t, Footing::FrozenWater, fast, t.slipTurnDeg, false));
    CHECK(stepSlips(t, Footing::FrozenWater, fast, -t.slipTurnDeg, false)); // either way
    CHECK(stepSlips(t, Footing::FrozenWater, fast, 0.f, true));             // a stop
    CHECK(stepSlips(t, Footing::FrozenWater, t.slipSpeedMS, 90.f, false));  // at the line
    CHECK_FALSE(stepSlips(t, Footing::FrozenWater, fast, t.slipTurnDeg - 1.f, false));
    CHECK_FALSE(stepSlips(t, Footing::FrozenWater, slow, 180.f, true)); // walking: never
    CHECK_FALSE(stepSlips(t, Footing::FrozenWater, std::numeric_limits<float>::quiet_NaN(), 180.f, true));
    CHECK_FALSE(stepSlips(t, Footing::FrozenWater, fast, std::numeric_limits<float>::quiet_NaN(), false));
    // Harmless patches (and snow, ground, open water): no slip at any speed, turn or stop.
    const Footing others[] = {Footing::Ground, Footing::Snow, Footing::IcePatch, Footing::OpenWater};
    for (const Footing f : others) {
        for (int s = 0; s <= 12; ++s) {
            for (int turn = -180; turn <= 180; turn += 30) {
                INFO("footing " << static_cast<int>(f) << " speed " << s << " turn " << turn);
                CHECK_FALSE(stepSlips(t, f, static_cast<float>(s), static_cast<float>(turn), false));
                CHECK_FALSE(stepSlips(t, f, static_cast<float>(s), static_cast<float>(turn), true));
            }
        }
    }
}
