#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "ftcore/GlassSurface.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace ftcore;

TEST_CASE("glass: a pane mirrors little square-on and everything at a glancing view") {
    CHECK(glassFresnel(1.f) == doctest::Approx(0.04f));
    CHECK(glassFresnel(0.f) == doctest::Approx(1.f));
    CHECK(glassPaneMirror(1.f) == doctest::Approx(0.0769f).epsilon(0.01));
    CHECK(glassPaneMirror(0.f) == doctest::Approx(1.f));
    float last = 0.f;
    for (int k = 0; k <= 20; ++k) {
        const float r = glassPaneMirror(1.f - 0.05f * static_cast<float>(k));
        CHECK(r >= last);
        last = r;
    }
    // 60 degrees off the normal: still mostly see-through; 85 degrees: mostly mirror.
    CHECK(glassPaneMirror(0.5f) < 0.2f);
    CHECK(glassPaneMirror(0.087f) > 0.75f);
}

TEST_CASE("glass: the night flip follows from the two light levels, at the same angle") {
    // By day the street is far brighter than the room: the eye gets the street's mirror image.
    const GlassSeen day = glassSeen(40.f, 0.6f, 0.9f);
    CHECK(day.ratio < 0.5f);
    // At dusk the two are of a size: both show.
    const GlassSeen dusk = glassSeen(6.f, 0.6f, 0.9f);
    CHECK(dusk.ratio > 0.5f);
    CHECK(dusk.ratio < 2.f);
    // At night the lit room wins.
    const GlassSeen night = glassSeen(0.05f, 0.6f, 0.9f);
    CHECK(night.ratio > 10.f);
    // A dark room behind a dark street shows neither.
    const GlassSeen none = glassSeen(0.f, 0.f, 0.9f);
    CHECK(none.ratio == 0.f);
    // A glancing view takes the mirror's side whatever the levels' order.
    CHECK(glassSeen(6.f, 0.6f, 0.1f).ratio < dusk.ratio);
}

TEST_CASE("glass: the pane's colour is a faint green, deeper on an old pane and along a longer path") {
    const std::array<float, 3> thin = glassTint(1.f, 0.006f, 0.f);
    CHECK(thin[1] > thin[2]);
    CHECK(thin[2] > thin[0]);
    CHECK(thin[0] > 0.9f);
    const std::array<float, 3> old = glassTint(1.f, 0.006f, 1.f);
    CHECK(old[0] < thin[0]);
    const std::array<float, 3> slant = glassTint(0.2f, 0.006f, 0.f);
    CHECK(slant[0] < thin[0]);
    CHECK(slant[0] > 0.9f);
}

TEST_CASE("glass: dirt gathers at the bottom edge and the corners, streaks hang from the top, hands mark the handle") {
    const float w = 1.8f, h = 2.6f;
    float low = 0.f, mid = 0.f, cornerDust = 0.f;
    int streakTop = 0, streakLow = 0;
    for (int k = 0; k < 200; ++k) {
        const float u = 0.1f + 0.8f * static_cast<float>(k) / 199.f;
        low += glassDetail(u, 0.03f, w, h, 0.37f, 0.5f, 0.f, -1.f, 0.f).dust;
        mid += glassDetail(u, 0.55f, w, h, 0.37f, 0.5f, 0.f, -1.f, 0.f).dust;
        streakTop += glassDetail(u, 0.97f, w, h, 0.37f, 0.5f, 0.f, -1.f, 0.f).streak > 0.02f ? 1 : 0;
        streakLow += glassDetail(u, 0.20f, w, h, 0.37f, 0.5f, 0.f, -1.f, 0.f).streak > 0.02f ? 1 : 0;
    }
    CHECK(low > 3.f * mid);
    CHECK(streakTop > 10);
    CHECK(streakTop > streakLow);
    cornerDust = glassDetail(0.02f, 0.5f * 0.03f, w, h, 0.37f, 0.5f, 0.f, -1.f, 0.f).dust;
    CHECK(cornerDust > glassDetail(0.5f, 0.5f, w, h, 0.37f, 0.5f, 0.f, -1.f, 0.f).dust);
    // The open pane carries no film (round 3: a dust floor of about 2% across the whole pane, sunlit, was a milky layer
    // twice as bright as the shop behind it). Streak runs leave at most a trace; their mark is in the roughness.
    float openVeil = 0.f, openMax = 0.f, streakRough = 0.f;
    for (int k = 0; k < 400; ++k) {
        const float u = 0.2f + 0.6f * static_cast<float>(k % 20) / 19.f, v = 0.3f + 0.4f * static_cast<float>(k / 20) / 19.f;
        const GlassDetail d = glassDetail(u, v, w, h, 0.37f, 1.f, 0.f, -1.f, 0.f);
        openVeil += d.veil / 400.f;
        openMax = std::max(openMax, d.veil);
        const GlassDetail t = glassDetail(u, 0.97f - 0.3f * static_cast<float>(k / 20) / 19.f, w, h, 0.37f, 1.f, 0.f, -1.f, 0.f);
        if (t.streak > 0.2f) streakRough = std::max(streakRough, t.rough - 0.02f - 0.30f * t.dust);
    }
    CHECK(openVeil < 0.005f);
    CHECK(openMax < 0.035f);
    CHECK(streakRough >= 0.06f);
    // The seal at the rim, the dark edge just inside it, clear glass past 3 cm.
    CHECK(glassDetail(0.001f, 0.5f, w, h, 0.f, 0.f, 0.f, -1.f, 0.f).seal == doctest::Approx(1.f));
    CHECK(glassDetail(0.012f, 0.5f, w, h, 0.f, 0.f, 0.f, -1.f, 0.f).edge > 0.3f);
    CHECK(glassDetail(0.5f, 0.5f, w, h, 0.f, 0.f, 0.f, -1.f, 0.f).edge == 0.f);
    // Hand marks only where there is a handle, and only near it.
    float near = 0.f, far = 0.f;
    for (int k = 0; k < 100; ++k) {
        const float du = 0.1f * static_cast<float>(k) / 99.f;
        near = std::max(near, glassDetail(0.1f + du, 0.42f, w, h, 0.61f, 0.5f, 0.f, 0.12f, 0.42f).hand);
        far = std::max(far, glassDetail(0.7f + du, 0.9f, w, h, 0.61f, 0.5f, 0.f, 0.12f, 0.42f).hand);
        CHECK(glassDetail(0.1f + du, 0.42f, w, h, 0.61f, 0.5f, 0.f, -1.f, 0.f).hand == 0.f);
    }
    CHECK(near > 0.2f);
    CHECK(far < 0.01f);
    // An old pane is dirtier than a new one, dry glass has no beads, and every value stays in range.
    CHECK(glassDetail(0.5f, 0.05f, w, h, 0.2f, 1.f, 0.f, -1.f, 0.f).dust > glassDetail(0.5f, 0.05f, w, h, 0.2f, 0.f, 0.f, -1.f, 0.f).dust);
    for (int k = 0; k < 400; ++k) {
        const float u = static_cast<float>(k % 20) / 19.f, v = static_cast<float>(k / 20) / 19.f;
        const GlassDetail d = glassDetail(u, v, w, h, 0.9f, 0.7f, 1.f, 0.5f, 0.4f);
        CHECK(glassDetail(u, v, w, h, 0.9f, 0.7f, 0.f, 0.5f, 0.4f).bead == 0.f);
        CHECK(d.rough >= 0.02f);
        CHECK(d.rough <= 1.f);
        CHECK(d.veil >= 0.f);
        CHECK(d.veil <= 1.f);
    }
}

TEST_CASE("glass: the maths allocates nothing; the shader block names what the materials read") {
    const auto before = ftcore_test::allocStats().count;
    float sum = 0.f;
    for (int k = 0; k < 64; ++k) {
        const float c = static_cast<float>(k) / 63.f;
        sum += glassPaneMirror(c) + glassTint(c, 0.006f, 0.3f)[1] + glassSeen(3.f, 1.f, c).ratio +
               glassDetail(c, 1.f - c, 2.f, 2.5f, 0.4f, 0.5f, 0.6f, 0.2f, 0.4f).rough;
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(sum > 0.f);
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
    const std::string code = glassHlsl();
    for (const char* name : {"gMirror", "gTint", "gDust", "gStreak", "gHand", "gBead", "gSeal", "gEdge", "gRough", "gVeil", "gWave"})
        CHECK(code.find(name) != std::string::npos);
    // No function definitions and no return: the block sits inside a Custom node's body.
    CHECK(code.find("return") == std::string::npos);
}

TEST_CASE("glass: a front's panes are cut at its mullions; the room behind is shader code with a light of its own") {
    const float cuts[] = {0.33f, 1.64f, 2.80f, 3.16f, 4.05f, 4.94f, 5.30f, 6.47f, 7.77f};
    const GlassPane first = glassPaneAt(0.5f, cuts, 9);
    CHECK(first.left == doctest::Approx(0.33f));
    CHECK(first.width == doctest::Approx(1.31f));
    const GlassPane door = glassPaneAt(3.5f, cuts, 9);
    CHECK(door.left == doctest::Approx(3.16f));
    CHECK(door.width == doctest::Approx(0.89f));
    const GlassPane last = glassPaneAt(7.7f, cuts, 9);
    CHECK(last.left == doctest::Approx(6.47f));
    CHECK(last.width == doctest::Approx(1.30f));
    // Outside the glass: the nearest pane. No table: one unit pane.
    CHECK(glassPaneAt(-1.f, cuts, 9).left == doctest::Approx(0.33f));
    CHECK(glassPaneAt(9.f, cuts, 9).left == doctest::Approx(6.47f));
    CHECK(glassPaneAt(1.f, nullptr, 0).width == doctest::Approx(1.f));
    const std::string room = glassPictureHlsl();
    for (const char* name : {"iA", "iFloor", "iV", "iShop", "iSeed", "iLit", "iDay", "iDayLevel", "iLamp", "iEyeM", "GoodsMap", "iCol"})
        CHECK(room.find(name) != std::string::npos);
    // Goods are cut from the shelf-front picture, not painted blocks.
    CHECK(room.find("frac(iG * 7.1)") == std::string::npos);
    // The room never reads the hour: its light is the daylight share and its lamps.
    CHECK(room.find("Night") == std::string::npos);
    CHECK(room.find("return") == std::string::npos);
}

TEST_CASE("glass: each shelf row takes a band of the goods picture, rows differ, v runs from the shelf up") {
    const auto before = ftcore_test::allocStats().count;
    int bands[kGlassGoodsBands] = {};
    float lastStart = -1.f;
    bool startsDiffer = false;
    for (int row = 0; row < 40; ++row) {
        const GlassGoodsRow r = glassGoodsRow(row, 0.37f);
        REQUIRE(r.band >= 0);
        REQUIRE(r.band < kGlassGoodsBands);
        CHECK(r.start >= 0.f);
        CHECK(r.start < 1.f);
        ++bands[r.band];
        if (lastStart >= 0.f && r.start != doctest::Approx(lastStart)) startsDiffer = true;
        lastStart = r.start;
    }
    for (int b = 0; b < kGlassGoodsBands; ++b) CHECK(bands[b] > 0);
    CHECK(startsDiffer);
    // Band 0 is the picture's top front: its shelf (up 0) at v 0.25, its top (up 1) at v 0; band 3's shelf at v 1.
    CHECK(glassGoodsV(0, 0.f) == doctest::Approx(0.25f));
    CHECK(glassGoodsV(0, 1.f) == doctest::Approx(0.f));
    CHECK(glassGoodsV(3, 0.f) == doctest::Approx(1.f));
    CHECK(glassGoodsV(9, 2.f) == doctest::Approx(0.75f));
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("glass: the bay's stock stands clear of the glass, the walls and the door, stacked items on kept bases") {
    const auto before = ftcore_test::allocStats().count;
    GlassBayItem items[kGlassBayStockMax];
    const int all = glassBayStock(7.5f, 1.f, 0.f, items, kGlassBayStockMax);
    CHECK(all >= 10);
    for (int i = 0; i < all; ++i) {
        CHECK(items[i].x >= 0.25f);
        CHECK(items[i].x <= 7.5f - 0.25f);
        CHECK(items[i].y >= 0.3f);
        CHECK(items[i].on < i);
        if (items[i].on >= 0) CHECK(std::abs(items[i].x - items[items[i].on].x) < 0.3f);
    }
    // A door over the crates (share 0.4 of 7.5 m = 3 m): they and the basket on them go, nothing left inside the span.
    const int withDoor = glassBayStock(7.5f, 2.6f, 3.6f, items, kGlassBayStockMax);
    CHECK(withDoor < all);
    for (int i = 0; i < withDoor; ++i) {
        CHECK((items[i].x <= 2.3f || items[i].x >= 3.9f));
        CHECK(std::strcmp(items[i].id, "wicker_basket_01") != 0);
        if (items[i].on >= 0) CHECK(items[i].on < i);
    }
    CHECK(glassBayStock(7.5f, 1.f, 0.f, items, 3) == 3);
    CHECK(glassBayStock(7.5f, 1.f, 0.f, nullptr, 8) == 0);
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
}
