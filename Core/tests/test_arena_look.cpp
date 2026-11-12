#include "ftcore/AimStance.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/ArenaZone.h"
#include "TestAssets.h"
#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include <string>

using namespace ftcore;

namespace {

ArenaTable lookArena() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err), err);
    return t;
}

AimRules lookAim() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/aim.json", text)) FAIL("Data/Battle/aim.json not found");
    AimRules r;
    std::string err;
    REQUIRE_MESSAGE(importAimRules(text, r, &err), err);
    return r;
}

// The tallest standing point of any enemy kind in the table (its height times its look's scale, plus its hover).
float tallestTop(const ArenaTable& t) {
    float top = 0.f;
    for (const ArenaEnemyKind& k : t.enemies) {
        const float scale = k.look.skinned() ? k.look.scale : 1.f;
        const float hover = k.look.skinned() ? k.look.hover : 0.f;
        top = std::fmax(top, arenaEnemyHeight(k) * scale + hover);
    }
    return top;
}

} // namespace

TEST_CASE("arena look: the view pitch reaches near straight up and is held to its limits") {
    const ArenaLookLimits& L = kArenaLook;
    CHECK(L.viewMax >= 80.f);
    CHECK(L.viewMax < 90.f); // never past straight up (the view would flip)
    CHECK(arenaClampViewPitch(L, 85.f) == doctest::Approx(85.f));
    CHECK(arenaClampViewPitch(L, 120.f) == doctest::Approx(L.viewMax));
    CHECK(arenaClampViewPitch(L, -90.f) == doctest::Approx(L.viewMin));
    CHECK(arenaClampViewPitch(L, std::numeric_limits<float>::quiet_NaN()) == 0.f);
}

TEST_CASE("arena look: the boom stops at boomMax and the lens tilts the rest") {
    const ArenaLookLimits& L = kArenaLook;
    for (float v = -60.f; v <= 95.f; v += 2.5f) {
        const ArenaPitchSplit s = arenaSplitPitch(L, v);
        CHECK(s.boom <= L.boomMax + 1e-4f);
        CHECK(s.tilt >= 0.f);
        CHECK(s.boom + s.tilt == doctest::Approx(arenaClampViewPitch(L, v)));
        if (v <= L.boomMax) CHECK(s.tilt == 0.f);
    }
}

TEST_CASE("arena look: at the boom's highest the lens stays over the floor, free and aimed, at the same arm") {
    const ArenaLookLimits& L = kArenaLook;
    const ZoneCamera cam{};
    const AimRules aim = lookAim();
    const float arms[2] = {cam.arm, cam.arm * aim.armScale};
    for (float arm : arms) {
        CAPTURE(arm);
        CHECK(arenaEyeHeight(cam.pivot, arm, cam.up, L.boomMax) >= L.minEye);
        // Looking further up never moves the lens: the boom is held, only the lens tilts.
        const ArenaPitchSplit a = arenaSplitPitch(L, 40.f), b = arenaSplitPitch(L, L.viewMax);
        CHECK(arenaEyeHeight(cam.pivot, arm, cam.up, a.boom) == doctest::Approx(arenaEyeHeight(cam.pivot, arm, cam.up, b.boom)));
        CHECK(arenaEyeBack(arm, cam.up, a.boom) == doctest::Approx(arenaEyeBack(arm, cam.up, b.boom)));
        // And the lens stays behind the lead, never over or inside it.
        CHECK(arenaEyeBack(arm, cam.up, L.boomMax) > 1.f);
    }
}

TEST_CASE("arena look: the tallest boss's top can be put under the crosshair from anywhere it can stand") {
    const ArenaLookLimits& L = kArenaLook;
    const ArenaTable t = lookArena();
    const float top = tallestTop(t);
    REQUIRE(top > 9.f); // the hovering kite is the tallest (9.1 m and a 0.9 m hover)
    const ZoneCamera cam{};
    const AimRules aim = lookAim();
    const float arms[2] = {cam.arm, cam.arm * aim.armScale};
    for (float arm : arms) {
        // From touching its body (the biggest body radius plus the lead's) out to across a big ring.
        for (float away = 2.5f; away <= 40.f; away += 0.5f) {
            CAPTURE(arm);
            CAPTURE(away);
            const float p = arenaLookUpPitch(L, cam.pivot, arm, cam.up, away, top, cam.lockPitch);
            CHECK(p < L.viewMax);
            CHECK(arenaSightHeightAt(L, cam.pivot, arm, cam.up, away, p) == doctest::Approx(top).epsilon(0.002));
        }
    }
    // It needs well over the old 15 degree limit: from the kite's preferred range, aimed, its top is above 30 degrees.
    CHECK(arenaLookUpPitch(L, cam.pivot, cam.arm * aim.armScale, cam.up, 6.5f, top, cam.lockPitch) > 30.f);
}

TEST_CASE("arena look: a low target keeps the lock's own pitch, an unreachable one the top limit") {
    const ArenaLookLimits& L = kArenaLook;
    const ZoneCamera cam{};
    // Under where the lock's own pitch already looks at that range: the lock's pitch, never lower.
    const float under = arenaSightHeightAt(L, cam.pivot, cam.arm, cam.up, 8.f, cam.lockPitch) - 0.5f;
    CHECK(arenaLookUpPitch(L, cam.pivot, cam.arm, cam.up, 8.f, under, cam.lockPitch) == doctest::Approx(cam.lockPitch));
    CHECK(arenaLookUpPitch(L, cam.pivot, cam.arm, cam.up, 0.f, 500.f, cam.lockPitch) == doctest::Approx(L.viewMax));
    // The answer rises with the target's height.
    float last = -90.f;
    for (float h = 1.f; h <= 12.f; h += 1.f) {
        const float p = arenaLookUpPitch(L, cam.pivot, cam.arm, cam.up, 6.f, h, cam.lockPitch);
        CHECK(p >= last);
        last = p;
    }
}
