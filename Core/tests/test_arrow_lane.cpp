#include "ftcore/ArrowLane.h"
#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>

using namespace ftcore;

namespace {
ArrowLaneLayout shipped() {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arrow_lane.json", text));
    ArrowLaneLayout l;
    std::string err;
    REQUIRE_MESSAGE(importArrowLaneLayout(text, l, &err), err);
    return l;
}
} // namespace

TEST_CASE("the safe box is the centred 16:9 rectangle") {
    PxRect b = safeBox(1920.f, 1080.f);
    CHECK(b.x == doctest::Approx(0.f));
    CHECK(b.w == doctest::Approx(1920.f));
    CHECK(b.h == doctest::Approx(1080.f));
    // 21:9 (2560x1080): the middle 75% of the width, full height.
    b = safeBox(2560.f, 1080.f);
    CHECK(b.w == doctest::Approx(1920.f));
    CHECK(b.x == doctest::Approx(320.f));
    CHECK(b.y == doctest::Approx(0.f));
    // 4:3 (1440x1080): the full width, the middle band of the height.
    b = safeBox(1440.f, 1080.f);
    CHECK(b.w == doctest::Approx(1440.f));
    CHECK(b.h == doctest::Approx(810.f));
    CHECK(b.y == doctest::Approx(135.f));
    CHECK(safeBox(0.f, 100.f).w == 0.f);
}

TEST_CASE("a rectangle resolves from its anchor") {
    const PxRect box{100.f, 50.f, 1600.f, 900.f};
    PxRect r = resolve({LaneAnchor::TopRight, -50.f, 10.f, 25.f, 20.f}, box);
    CHECK(r.x == doctest::Approx(100.f + 1600.f - 800.f));
    CHECK(r.y == doctest::Approx(50.f + 90.f));
    CHECK(r.w == doctest::Approx(400.f));
    CHECK(r.h == doctest::Approx(180.f));
    r = resolve({LaneAnchor::BottomLeft, 1.f, -20.f, 10.f, 10.f}, box);
    CHECK(r.x == doctest::Approx(116.f));
    CHECK(r.y == doctest::Approx(50.f + 900.f - 180.f));
    r = resolve({LaneAnchor::TopLeft, 0.f, 0.f, 100.f, 100.f}, box);
    CHECK(r.w == doctest::Approx(1600.f));
    r = resolve({LaneAnchor::BottomRight, -10.f, -10.f, 10.f, 10.f}, box);
    CHECK(r.x == doctest::Approx(100.f + 1600.f - 160.f));
    CHECK(r.y == doctest::Approx(50.f + 900.f - 90.f));
}

TEST_CASE("the shipped layout file loads and is the built-in default") {
    const ArrowLaneLayout l = shipped();
    const ArrowLaneLayout d = defaultArrowLaneLayout();
    CHECK(l.plate.x == d.plate.x);
    CHECK(l.plate.y == d.plate.y);
    CHECK(l.plate.w == d.plate.w);
    CHECK(l.plate.h == d.plate.h);
    CHECK(l.columnPitch == d.columnPitch);
    CHECK(l.targetY == d.targetY);
    CHECK(l.spawnY == d.spawnY);
    CHECK(l.glyphWidth == d.glyphWidth);
    CHECK(l.glyphHeight == d.glyphHeight);
    CHECK(l.word.y == d.word.y);
    CHECK(l.streak.x == d.streak.x);
    CHECK(l.stamp.y == d.stamp.y);
    CHECK(l.caption.y == d.caption.y);
    CHECK(l.times.lookahead == d.times.lookahead);
    CHECK(l.times.laneFadeIn == d.times.laneFadeIn);
    CHECK(l.times.exitEnd == d.times.exitEnd);
    CHECK(l.times.streakEvery == d.times.streakEvery);
    CHECK(l.times.burstToPerfect == d.times.burstToPerfect);
    CHECK(l.colors.plate.a == d.colors.plate.a);
    CHECK(l.colors.rim[3].r == d.colors.rim[3].r);
    CHECK(l.colors.warn.g == d.colors.warn.g);
}

TEST_CASE("the shipped rail: 1.0 s lookahead, about 497 px/s at 1080, and it holds on 21:9") {
    const ArrowLaneLayout l = shipped();
    const LaneGeometry g = laneGeometry(l, 1920.f, 1080.f, l.times.lookahead);
    CHECK(g.pxPerSec == doctest::Approx(0.46f * 1080.f).epsilon(0.001));
    CHECK(g.targetY == doctest::Approx(0.44f * 1080.f));
    CHECK(g.glyphH == doctest::Approx(64.8f));
    CHECK(g.plate.x == doctest::Approx(1920.f * (1.f - 0.357f)));
    CHECK(g.plate.y == doctest::Approx(0.35f * 1080.f));
    CHECK(g.plate.y + g.plate.h == doctest::Approx(0.93f * 1080.f));
    // The plate's right edge sits left of the enemy panels (324 px column at 1080, from the screen edge).
    CHECK(g.plate.x + g.plate.w < 1920.f - 324.f);
    // Four columns fit inside the plate.
    CHECK(g.columnCentre(3) + g.glyphW * 0.5f < g.plate.x + g.plate.w);
    CHECK(g.columnCentre(0) - g.glyphW * 0.5f > g.plate.x);

    // 21:9: the same rail inside the middle 16:9, shifted by the side bars.
    const LaneGeometry w = laneGeometry(l, 2560.f, 1080.f, l.times.lookahead);
    CHECK(w.plate.x == doctest::Approx(g.plate.x + 320.f));
    CHECK(w.plate.w == doctest::Approx(g.plate.w));
    CHECK(w.pxPerSec == doctest::Approx(g.pxPerSec));
    // The enemy panels stay at the screen's edge, outside the safe box: no overlap.
    CHECK(w.plate.x + w.plate.w < 2560.f - 324.f);
}

TEST_CASE("steps of a combo are far enough apart on the lane") {
    const ArrowLaneLayout l = shipped();
    const LaneGeometry g = laneGeometry(l, 1920.f, 1080.f, l.times.lookahead);
    // A 250 ms same-lane step is 124 px against a 65 px glyph, a 125 ms burst 62 px.
    CHECK(g.windowPx(0.25) == doctest::Approx(124.f).epsilon(0.01));
    CHECK(g.windowPx(0.125) == doctest::Approx(62.f).epsilon(0.01));
    CHECK(g.windowPx(0.125) < g.glyphH + 4.f);
    // The Perfect window at scale 1.0 (45 ms) is 22 px each way and the Good window (135 ms) 67 px.
    CHECK(g.windowPx(0.045) == doctest::Approx(22.f).epsilon(0.03));
    CHECK(g.windowPx(0.135) == doctest::Approx(67.f).epsilon(0.03));
    // Notes: on the target at dt 0, entering at the bottom at the lookahead, above it when late.
    CHECK(g.noteY(0.0) == doctest::Approx(g.targetY));
    CHECK(g.noteY(1.0) == doctest::Approx(0.90f * 1080.f));
    CHECK(g.noteY(-0.1) < g.targetY);
}

TEST_CASE("a note fades in over its first tenth of a second of travel") {
    const ArrowLaneLayout l = shipped();
    const LaneGeometry g = laneGeometry(l, 1920.f, 1080.f, 1.0);
    CHECK(g.noteAlpha(l.times, 1.0) == doctest::Approx(0.f));
    CHECK(g.noteAlpha(l.times, 0.95) == doctest::Approx(0.5f));
    CHECK(g.noteAlpha(l.times, 0.9) == doctest::Approx(1.f));
    CHECK(g.noteAlpha(l.times, 0.2) == doctest::Approx(1.f));
    CHECK(g.noteAlpha(l.times, -0.1) == doctest::Approx(1.f));
}

TEST_CASE("the lookahead follows the tempo within its clamp") {
    const ArrowLaneTimes t = shipped().times;
    CHECK(laneLookahead(t, 0.0) == doctest::Approx(1.0));
    CHECK(laneLookahead(t, 120.0) == doctest::Approx(1.0)); // two beats
    CHECK(laneLookahead(t, 60.0) == doctest::Approx(1.2));  // clamped from 2.0
    CHECK(laneLookahead(t, 240.0) == doctest::Approx(0.7)); // clamped from 0.5
    CHECK(laneLookahead(t, 100.0) == doctest::Approx(1.2));
    CHECK(laneLookahead(t, 150.0) == doctest::Approx(0.8));
}

TEST_CASE("the glyph keeps its minimum on a small screen and stays inside its column") {
    const ArrowLaneLayout l = shipped();
    const LaneGeometry g = laneGeometry(l, 640.f, 360.f, 1.0);
    CHECK(g.glyphH >= l.minGlyphPx);
    CHECK(g.glyphW <= g.pitch);
}

TEST_CASE("the lane fades in over 200 ms and out inside the 700 ms recovery") {
    const ArrowLaneTimes t = shipped().times;
    CHECK(laneVisibility(t, 0.0, -1.0, false) == doctest::Approx(0.f));
    CHECK(laneVisibility(t, 0.1, -1.0, false) == doctest::Approx(0.5f));
    CHECK(laneVisibility(t, 0.2, -1.0, false) == doctest::Approx(1.f));
    CHECK(laneVisibility(t, 5.0, -1.0, false) == doctest::Approx(1.f));
    // After the last judgement: holds to 450 ms, gone at 700 ms (the recovery's end).
    CHECK(laneVisibility(t, 5.0, 0.0, false) == doctest::Approx(1.f));
    CHECK(laneVisibility(t, 5.0, 0.45, false) == doctest::Approx(1.f));
    CHECK(laneVisibility(t, 5.0, 0.575, false) == doctest::Approx(0.5f));
    CHECK(laneVisibility(t, 5.0, 0.7, false) == doctest::Approx(0.f));
    CHECK(laneVisibility(t, 5.0, 0.9, false) == doctest::Approx(0.f));
    // The enemies fell: gone in 150 ms.
    CHECK(laneVisibility(t, 5.0, 0.075, true) == doctest::Approx(0.5f));
    CHECK(laneVisibility(t, 5.0, 0.15, true) == doctest::Approx(0.f));
    // Monotone down through the exit.
    float last = 2.f;
    for (double a = 0.0; a <= 0.8; a += 0.01) {
        const float v = laneVisibility(t, 5.0, a, false);
        CHECK(v <= last + 1e-6f);
        last = v;
    }
}

TEST_CASE("the exit ends no later than the recovery, whatever the file says") {
    ArrowLaneLayout l;
    std::string err;
    CHECK_FALSE(importArrowLaneLayout(R"({"version":1,"timing":{"exitStart":0.6,"exitFade":0.25,"exitEnd":0.7}})", l, &err));
    CHECK(err.find("exit") != std::string::npos);
}

TEST_CASE("the stamp dims the plate and pops, then shares the exit") {
    const ArrowLaneTimes t = shipped().times;
    CHECK(plateDimming(t, 0.0) == doctest::Approx(1.f));
    CHECK(plateDimming(t, 0.06) == doctest::Approx(0.55f));
    CHECK(stampAlpha(t, 0.0) == doctest::Approx(0.f));
    CHECK(stampAlpha(t, 0.06) == doctest::Approx(1.f));
    CHECK(stampAlpha(t, 0.45) == doctest::Approx(1.f));
    CHECK(stampAlpha(t, 0.7) == doctest::Approx(0.f));
    CHECK(stampScale(t, 0.06) == doctest::Approx(0.8f));
    CHECK(stampScale(t, 0.18) == doctest::Approx(1.f));
    CHECK(stampScale(t, 0.12) == doctest::Approx(0.9f));
}

TEST_CASE("the streak badge pops on every fourth from two") {
    const ArrowLaneTimes t = shipped().times;
    CHECK_FALSE(streakStep(t, 1));
    CHECK_FALSE(streakStep(t, 2));
    CHECK_FALSE(streakStep(t, 3));
    CHECK(streakStep(t, 4));
    CHECK_FALSE(streakStep(t, 5));
    CHECK(streakStep(t, 8));
    CHECK(streakStep(t, 12));
    CHECK_FALSE(streakStep(t, 0));
    CHECK(streakPopScale(t, 0.0) == doctest::Approx(1.25f));
    CHECK(streakPopScale(t, 0.075) == doctest::Approx(1.125f));
    CHECK(streakPopScale(t, 0.15) == doctest::Approx(1.f));
    CHECK(streakPopScale(t, 3.0) == doctest::Approx(1.f));
    CHECK(streakPopScale(t, -1.0) == doctest::Approx(1.f));
}

TEST_CASE("each grade has its own burst; only a Perfect has the star, and reduce flashes removes it") {
    const ArrowLaneTimes t = shipped().times;
    CHECK(burstLength(t, Grade::Perfect) == doctest::Approx(0.26));
    CHECK(burstLength(t, Grade::Great) == doctest::Approx(0.22));
    CHECK(burstLength(t, Grade::Good) == doctest::Approx(0.18));
    CHECK(burstLength(t, Grade::Miss) == 0.0);
    CHECK(burstRadius(t, Grade::Perfect, 0.0) == doctest::Approx(6.f));
    CHECK(burstRadius(t, Grade::Perfect, 0.26) == doctest::Approx(10.f));
    CHECK(burstRadius(t, Grade::Great, 0.22) == doctest::Approx(8.5f));
    CHECK(burstRadius(t, Grade::Good, 0.18) == doctest::Approx(7.f));
    CHECK(burstRadius(t, Grade::Good, 0.2) == 0.f);
    CHECK(burstRadius(t, Grade::Miss, 0.1) == 0.f);
    CHECK(burstHasStar(Grade::Perfect, false));
    CHECK_FALSE(burstHasStar(Grade::Perfect, true));
    CHECK_FALSE(burstHasStar(Grade::Great, false));
    CHECK_FALSE(burstHasStar(Grade::Good, false));
}

TEST_CASE("the stray cue is quiet for the confirm press at the combo's start") {
    const ArrowLaneTimes t = shipped().times;
    CHECK_FALSE(strayCueShows(t, 0.0));
    CHECK_FALSE(strayCueShows(t, 0.016));
    CHECK_FALSE(strayCueShows(t, 0.29));
    CHECK(strayCueShows(t, 0.3));
    CHECK(strayCueShows(t, 0.8));
}

TEST_CASE("early and late ticks: none on a Perfect, late for an expired note, the sign of the error otherwise") {
    CHECK(pressTick(Grade::Perfect, true, -0.02) == 0);
    CHECK(pressTick(Grade::Great, true, -0.07) == -1);
    CHECK(pressTick(Grade::Great, true, 0.07) == 1);
    CHECK(pressTick(Grade::Good, true, -0.12) == -1);
    CHECK(pressTick(Grade::Good, true, 0.12) == 1);
    CHECK(pressTick(Grade::Miss, false, 0.0) == 1);   // expired unpressed
    CHECK(pressTick(Grade::Miss, true, -0.05) == -1); // a wrong arrow pressed early
    CHECK(pressTick(Grade::Miss, true, 0.05) == 1);
    CHECK(pressTick(Grade::Pending, true, 0.05) == 0);

    const ArrowLaneLayout l = shipped();
    const LaneGeometry g = laneGeometry(l, 1920.f, 1080.f, 1.0);
    // A late press: the arrow was above the target.
    CHECK(tickOffsetPx(g, 0.1, 0.135) == doctest::Approx(-0.1f * g.pxPerSec));
    CHECK(tickOffsetPx(g, -0.1, 0.135) == doctest::Approx(0.1f * g.pxPerSec));
    CHECK(tickOffsetPx(g, 0.5, 0.135) == doctest::Approx(-0.135f * g.pxPerSec)); // clamped
}

TEST_CASE("a jump pair shows the worse grade") {
    CHECK(worseGrade(Grade::Perfect, Grade::Good) == Grade::Good);
    CHECK(worseGrade(Grade::Good, Grade::Perfect) == Grade::Good);
    CHECK(worseGrade(Grade::Great, Grade::Miss) == Grade::Miss);
    CHECK(worseGrade(Grade::Perfect, Grade::Great) == Grade::Great);
    CHECK(worseGrade(Grade::Perfect, Grade::Perfect) == Grade::Perfect);
}

TEST_CASE("the layout file refuses bad numbers and keeps defaults for missing ones") {
    ArrowLaneLayout l;
    l.targetY = 12.f;
    std::string err;
    CHECK(importArrowLaneLayout(R"({"version":1})", l, &err));
    CHECK(l.targetY == defaultArrowLaneLayout().targetY);

    l.targetY = 12.f;
    CHECK_FALSE(importArrowLaneLayout(R"({"version":2})", l, &err));
    CHECK_FALSE(importArrowLaneLayout("[1]", l, &err));
    CHECK_FALSE(importArrowLaneLayout("{", l, &err));
    CHECK_FALSE(importArrowLaneLayout(R"({"version":1,"geometry":{"targetY":"high"}})", l, &err));
    CHECK(err.find("targetY") != std::string::npos);
    CHECK_FALSE(importArrowLaneLayout(R"({"version":1,"geometry":{"targetY":120}})", l, &err));
    CHECK_FALSE(importArrowLaneLayout(R"({"version":1,"geometry":{"targetY":95,"spawnY":90}})", l, &err));
    CHECK_FALSE(importArrowLaneLayout(R"({"version":1,"geometry":{"glyphWidth":9}})", l, &err));
    CHECK_FALSE(importArrowLaneLayout(R"({"version":1,"geometry":{"columnPitch":9}})", l, &err));
    CHECK_FALSE(importArrowLaneLayout(R"({"version":1,"geometry":{"plate":{"anchor":"MID"}}})", l, &err));
    CHECK_FALSE(importArrowLaneLayout(R"({"version":1,"timing":{"lookahead":0}})", l, &err));
    CHECK_FALSE(importArrowLaneLayout(R"({"version":1,"timing":{"lookahead":2.0}})", l, &err)); // above its clamp
    CHECK_FALSE(importArrowLaneLayout(R"({"version":1,"colors":{"plate":[2,0,0,1]}})", l, &err));
    CHECK_FALSE(importArrowLaneLayout(R"({"version":1,"colors":{"rim":[[0,0,0]]}})", l, &err));
    CHECK(l.targetY == 12.f); // untouched by every refusal

    CHECK(importArrowLaneLayout(R"({"version":1,"geometry":{"targetY":50,"plate":{"anchor":"BL","x":3,"y":-50,"w":20,"h":40}},
                                   "colors":{"warn":[0.1,0.2,0.3]}})", l, &err));
    CHECK(l.targetY == 50.f);
    CHECK(l.plate.anchor == LaneAnchor::BottomLeft);
    CHECK(l.plate.h == 40.f);
    CHECK(l.colors.warn.g == doctest::Approx(0.2f));
    CHECK(l.colors.warn.a == 1.f);
}

TEST_CASE("reading the lane geometry and envelopes allocates nothing") {
    if (!ftcore_test::allocHookInstalled()) return;
    const ArrowLaneLayout l = shipped();
    const auto before = ftcore_test::allocStats();
    float sum = 0.f;
    for (int i = 0; i < 1000; ++i) {
        const LaneGeometry g = laneGeometry(l, 1920.f, 1080.f, 1.0);
        sum += g.noteY(0.01 * i) + g.noteAlpha(l.times, 0.001 * i) + laneVisibility(l.times, 0.01 * i, 0.001 * i, false) +
               stampAlpha(l.times, 0.001 * i) + burstRadius(l.times, Grade::Perfect, 0.0002 * i);
    }
    CHECK(sum != 12345.f);
    CHECK(ftcore_test::allocStats().count == before.count);
}
