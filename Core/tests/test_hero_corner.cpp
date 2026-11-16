#include <doctest/doctest.h>
#include "ftcore/HeroCorner.h"
#include "ftcore/MassingDress.h"
#include "ftcore/WindowFacade.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace ftcore;

TEST_CASE("hero corner: the lane, the bays and the eave stand where the plan puts them") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);

    CHECK(l.pave.cx == doctest::Approx(904.5));
    CHECK(l.pave.cy == doctest::Approx(535.0));
    CHECK(l.pave.sx == doctest::Approx(7.0));
    CHECK(l.pave.sy == doctest::Approx(40.0));
    CHECK(l.pave.z1 == doctest::Approx(4.20));
    CHECK(l.pave.z1 - l.pave.z0 == doctest::Approx(0.08));

    // Five 8 m bays from y 515 to 555, x 908..916, each touching the next.
    std::set<std::string> ids;
    for (int i = 0; i < kHeroBays; ++i) {
        const MassingBox& b = l.bays[i];
        CHECK(b.cx - 0.5 * b.sx == doctest::Approx(908.0));
        CHECK(b.cx + 0.5 * b.sx == doctest::Approx(916.0));
        CHECK(b.cy - 0.5 * b.sy == doctest::Approx(515.0 + 8.0 * i));
        CHECK(b.sy == doctest::Approx(8.0));
        CHECK(b.z1 == doctest::Approx(4.18 + 7.0));
        CHECK(b.z0 < 4.18);
        CHECK(b.rot == 0.0);
        CHECK(massingVisible(massingRole(b)));
        CHECK(massingLookIsBuilding(massingLookSurface(b)));
        int index = -1;
        REQUIRE(heroCornerBay(b.id, &index));
        CHECK(index == i);
        ids.insert(b.id);
    }
    CHECK(ids.size() == static_cast<std::size_t>(kHeroBays));
    CHECK(l.bayLit[0]);
    CHECK_FALSE(l.bayLit[1]);
    CHECK(l.bayLit[2]);
    CHECK(l.bayLit[3]);
    CHECK_FALSE(l.bayLit[4]);
    CHECK(l.eateryBay == 2);

    // The eave: 1.2 m over the lane at 3.4 m, the whole row long.
    CHECK(l.eave.cx - 0.5 * l.eave.sx == doctest::Approx(906.8));
    CHECK(l.eave.cx + 0.5 * l.eave.sx == doctest::Approx(908.0));
    CHECK(l.eave.z0 == doctest::Approx(4.18 + 3.4));
    CHECK(l.eave.sy == doctest::Approx(40.0));
}

TEST_CASE("hero corner: every bay fronts the lane as a shophouse, so the build gives it shop bays and one spill light") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);
    for (int i = 0; i < kHeroBays; ++i) {
        const MassingBox& b = l.bays[i];
        MassingWindowData data;
        data.streetBits = static_cast<std::uint8_t>(1u << kHeroShopFace);
        for (double& base : data.base) base = spec.baySink;
        INFO("bay " << i + 1 << " id " << b.id);
        CHECK(massingFacadeData(b, data).type == WindowType::Shophouse);
        const MassingLookSurface wall = massingLookSurface(b);
        CHECK(wall != MassingLookSurface::BuildingB);
        CHECK(wall != MassingLookSurface::BuildingC);

        // The shop face is the west one: its spill point is 0.4 m out over the lane, facing west, one per 8 m bay.
        std::vector<MassingShopLight> lights;
        REQUIRE(massingShopLights(b, data, kMassingShopLightSpacing, lights) == 1);
        CHECK(lights[0].faceIndex == kHeroShopFace);
        CHECK(lights[0].x == doctest::Approx(908.0 - kMassingShopLightOut));
        CHECK(lights[0].y == doctest::Approx(b.cy));
        CHECK(lights[0].z == doctest::Approx(b.z0 + spec.baySink + kMassingShopLightUp));
        CHECK(std::abs(std::abs(lights[0].face) - 180.0) < 1e-6);
        // Under the eave: the spill sits below its slab.
        CHECK(lights[0].z < l.eave.z0);
    }
}

TEST_CASE("hero corner: six large lanterns in pairs under the eave") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);
    REQUIRE(l.lanternCount == static_cast<std::size_t>(kHeroLargeLanterns + kHeroStrings * kHeroStringLanterns));
    const double pairs[] = {524.0, 532.0, 548.0};
    for (int i = 0; i < kHeroLargeLanterns; ++i) {
        const HeroLantern& lan = l.lanterns[i];
        CHECK(lan.large);
        CHECK(lan.string == -1);
        CHECK(lan.centre.x == doctest::Approx(907.0));
        CHECK(lan.centre.z == doctest::Approx(4.18 + 3.0));
        CHECK(std::abs(lan.centre.y - pairs[i / 2]) == doctest::Approx(0.8));
        // It hangs from the eave's underside, inside the eave's reach, its top under the slab.
        CHECK(lan.hook.z == doctest::Approx(l.eave.z0));
        CHECK(lan.hook.x > l.eave.cx - 0.5 * l.eave.sx);
        CHECK(lan.centre.z + 0.5 * spec.largeHeight <= lan.hook.z + 1e-9);
        // Clear of the shop's spill, which stands 0.4 m out at 2.4 m: the lantern's bottom is over it.
        CHECK(lan.centre.z - 0.5 * spec.largeHeight > 4.18 + kMassingShopLightUp);
    }
}

TEST_CASE("hero corner: each string spans 6.7 m, sags 0.4 m and carries nine lanterns 0.75 m apart") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);
    for (int s = 0; s < kHeroStrings; ++s) {
        const HeroPoint& a = l.cable[s][0];
        const HeroPoint& b = l.cable[s][kHeroCablePoints - 1];
        CHECK(a.x == doctest::Approx(908.0));
        CHECK(a.z == doctest::Approx(4.18 + 3.6));
        CHECK(b.x == doctest::Approx(901.3));
        CHECK(b.z == doctest::Approx(4.18 + 3.8));
        CHECK(a.x - b.x == doctest::Approx(6.7));
        // The middle point is 0.4 m under the chord.
        const HeroPoint& mid = l.cable[s][(kHeroCablePoints - 1) / 2];
        CHECK(mid.z == doctest::Approx(0.5 * (a.z + b.z) - 0.4));
        // The cable never rises over its higher end; its lowest point is the middle's, give or take the polyline.
        double lowest = 1e9;
        for (int k = 0; k < kHeroCablePoints; ++k) {
            CHECK(l.cable[s][k].y == doctest::Approx(spec.stringY[s]));
            CHECK(l.cable[s][k].z <= b.z + 1e-9);
            lowest = std::min(lowest, l.cable[s][k].z);
        }
        CHECK(lowest == doctest::Approx(mid.z).epsilon(0.01));

        // Nine lanterns, 0.75 m apart along the plan, centred on the span, each hung smallDrop under the cable.
        int count = 0;
        double prevX = 0.0;
        for (std::size_t i = 0; i < l.lanternCount; ++i) {
            const HeroLantern& lan = l.lanterns[i];
            if (lan.string != s) continue;
            CHECK_FALSE(lan.large);
            const double t = (908.0 - lan.hook.x) / 6.7;
            CHECK(lan.hook.z == doctest::Approx(heroCableZ(spec, t) - spec.smallDrop));
            CHECK(lan.centre.z == doctest::Approx(lan.hook.z - 0.16));
            if (count > 0) CHECK(prevX - lan.hook.x == doctest::Approx(0.75));
            prevX = lan.hook.x;
            // A walker 2.0 m tall passes under every small lantern.
            CHECK(lan.centre.z - 0.5 * spec.smallHeight > 4.18 + 2.0);
            ++count;
        }
        CHECK(count == kHeroStringLanterns);
        const HeroLantern& firstL = l.lanterns[kHeroLargeLanterns + s * kHeroStringLanterns];
        const HeroLantern& lastL = l.lanterns[kHeroLargeLanterns + s * kHeroStringLanterns + kHeroStringLanterns - 1];
        CHECK(908.0 - firstL.hook.x == doctest::Approx(lastL.hook.x - 901.3));

        // The post stands on the lane's canal edge; the pool's lamp slot sits under the cable's middle.
        CHECK(l.postFoot[s].x == doctest::Approx(901.3));
        CHECK(l.postFoot[s].z == doctest::Approx(4.18));
        CHECK(4.18 + spec.postHeight > b.z);
        CHECK(l.stringLamp[s].x == doctest::Approx(904.65));
        CHECK(l.stringLamp[s].z == doctest::Approx(mid.z - 0.35));
    }
    CHECK(l.stringLampGain == doctest::Approx(0.5f));
}

TEST_CASE("hero corner: the camera stands at the corner's south end looking north down the lane") {
    HeroCornerLayout l;
    heroCornerLayout(HeroCornerSpec{}, l);
    CHECK(l.eye.x == doctest::Approx(904.5));
    CHECK(l.eye.y == doctest::Approx(511.0));
    CHECK(l.eye.z == doctest::Approx(4.18 + 1.6));
    CHECK(l.at.y - l.eye.y == doctest::Approx(40.0));
    const double pitch = std::atan2(l.at.z - l.eye.z, l.at.y - l.eye.y) * 180.0 / 3.14159265358979;
    CHECK(pitch == doctest::Approx(-3.0).epsilon(0.01));
    CHECK(l.lensMm == doctest::Approx(35.0));
    // Every lantern is in front of it.
    for (std::size_t i = 0; i < l.lanternCount; ++i) CHECK(l.lanterns[i].centre.y > l.eye.y + 10.0);
}

TEST_CASE("hero corner: a bay's id is told apart from the rest") {
    int index = -1;
    CHECK(heroCornerBay("HC-BAY-1a", &index));
    CHECK(index == 0);
    CHECK(heroCornerBay("HC-BAY-5c", &index));
    CHECK(index == 4);
    CHECK_FALSE(heroCornerBay("HC-BAY-6a", &index));
    CHECK_FALSE(heroCornerBay("HC-BAY-", &index));
    CHECK_FALSE(heroCornerBay("HC-PAVE", &index));
    CHECK_FALSE(heroCornerBay("YD-0175", &index));
    CHECK(heroCornerBay("HC-BAY-3a", nullptr));
}

TEST_CASE("lamp slot gain: an empty table keeps every lamp's light; a slot past the table keeps it too") {
    CHECK(lampSlotGain(nullptr, 0, 0) == 1.f);
    CHECK(lampSlotGain(nullptr, 0, 63) == 1.f);
    const float gains[] = {1.f, 0.5f};
    CHECK(lampSlotGain(gains, 2, 0) == 1.f);
    CHECK(lampSlotGain(gains, 2, 1) == 0.5f);
    CHECK(lampSlotGain(gains, 2, 2) == 1.f);
}
