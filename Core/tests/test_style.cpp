#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "ftcore/StylePack.h"

using ftcore::StyleDial;
using ftcore::StylePack;
using ftcore::StyleSettings;
using ftcore::StyleVec3;
using ftcore::stylePack;

namespace {

bool approx(float a, float b, float eps = 1e-5f) { return a - b <= eps && b - a <= eps; }
bool approx(const StyleVec3& a, const StyleVec3& b, float eps = 1e-5f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

} // namespace

TEST_CASE("None is the identity and the four packs are active") {
    const StyleSettings none;
    CHECK(stylePack(StylePack::None) == none);
    CHECK_FALSE(none.active());
    CHECK_FALSE(none.needsDepth());
    for (StylePack p : {StylePack::Solhaven, StylePack::Lattice, StylePack::Unwritten, StylePack::Restored}) {
        CAPTURE(ftcore::stylePackName(p));
        CHECK(stylePack(p).active());
        CHECK(stylePack(p).needsDepth()); // every pack has an outline
        CHECK(stylePack(p) != none);
    }
    // Hatch modifiers and face fields alone do not make a style active.
    StyleSettings s;
    s.hatchOverlay = 1.f;
    s.hatchMaxLevel = 2.f;
    s.faceFeatures = 1.f;
    s.hatchShadowOnly = 1.f;
    s.hatchCurve = 1.8f;
    s.rimTint = StyleVec3(1.f, 0.5f, 0.f); // a tint without a rim strength draws nothing
    s.inkVoidNear = 2.f;
    CHECK_FALSE(s.active());
    CHECK_FALSE(s.needsDepth());
}

TEST_CASE("The engine-delta fields default to the identity") {
    const StyleSettings none;
    CHECK(none.hatchOverlay == 0.f);
    CHECK(none.hatchMaxLevel == 5.f);
    CHECK(none.hatchShadowOnly == 0.f);
    CHECK(none.hatchCurve == 1.f);
    CHECK(none.rimScale == 0.f);
    CHECK(none.rimTint == StyleVec3(1.f));
    CHECK(none.bloomThresholdScale == 1.f);
    CHECK(none.inkVoid == 0.f);
    CHECK(none.blackPoint == 0.f);
    // Each one, set on its own, switches the style on (the void also needs depth).
    StyleSettings rim;
    rim.rimScale = 0.5f;
    CHECK(rim.active());
    StyleSettings bloom;
    bloom.bloomThresholdScale = 2.f;
    CHECK(bloom.active());
    StyleSettings clip;
    clip.blackPoint = 0.06f;
    CHECK(clip.active());
    StyleSettings ink;
    ink.inkVoid = 1.f;
    CHECK(ink.active());
    CHECK(ink.needsDepth());
    // Restored uses none of them.
    const StyleSettings res = stylePack(StylePack::Restored);
    CHECK(res.hatch == 0.f);
    CHECK(res.hatchOverlay == 0.f);
    CHECK(res.hatchMaxLevel == 5.f);
    CHECK(res.hatchCurve == 1.f);
    CHECK(res.rimScale == 0.f);
    CHECK(res.bloomThresholdScale == 1.f);
    CHECK(res.inkVoid == 0.f);
    CHECK(res.blackPoint == 0.f);
}

TEST_CASE("Pack values follow art direction v2") {
    const StyleSettings sol = stylePack(StylePack::Solhaven);
    CHECK(sol.outlineWidth == 2.4f);
    CHECK(sol.saturation == 1.25f);
    CHECK(sol.outlineColor == StyleVec3(0x14 / 255.f, 0x11 / 255.f, 0x0F / 255.f));
    CHECK(approx(sol.highlightTint, StyleVec3(0.78f + 0.22f, 0.78f + 0.22f * (0xC9 / 255.f),
                                            0.78f + 0.22f * (0x3C / 255.f))));
    CHECK(sol.hatch == 1.f);
    CHECK(sol.hatchScale == 240.f);
    CHECK(sol.hatchDensity == 1.f);
    CHECK(sol.hatchOverlay == 1.f);
    CHECK(sol.hatchMaxLevel == 2.f);
    CHECK(sol.hatchShadowOnly == 1.f);
    CHECK(sol.hatchCurve == 1.f);
    CHECK(sol.rimScale == 1.f);
    CHECK(sol.rimTint == StyleVec3(1.f, 0xC9 / 255.f, 0x3C / 255.f));
    CHECK(sol.bloomThresholdScale == 1.f);

    const StyleSettings lat = stylePack(StylePack::Lattice);
    CHECK(lat.outline == 0.9f);
    CHECK(lat.cel == 0.9f);
    CHECK(lat.celSteps == 2.f);
    CHECK(lat.shadowTint == StyleVec3(0.46f, 0.38f, 0.59f));
    CHECK(lat.bloomScale == 1.8f);
    CHECK(lat.vignetteScale == 1.5f);
    CHECK(lat.hull == 0.8f);
    CHECK(lat.hullWidth == 1.2f);
    CHECK(lat.hullColor == StyleVec3(0x12 / 255.f, 0x07 / 255.f, 0x2B / 255.f));
    CHECK(lat.bloomThresholdScale == 2.5f);
    CHECK(lat.rimScale == 0.35f);
    CHECK(lat.rimTint == StyleVec3(0x19 / 255.f, 0xE3 / 255.f, 0xF0 / 255.f));
    CHECK(lat.blackPoint == 0.f); // the optional clip waits for a look at the picture
    CHECK(lat.hatch == 0.f);

    const StyleSettings unw = stylePack(StylePack::Unwritten);
    CHECK(unw.hatchScale == 280.f);
    CHECK(unw.hatchDensity == 1.4f);
    CHECK(unw.rimScale == 0.f);
    CHECK(unw.monochrome == 1.f);
    CHECK(unw.monoGamma == 1.15f);
    CHECK(unw.paperSky == 1.f);
    CHECK(unw.paperGrain == 0.07f);
    CHECK(unw.hatchCurve == 1.8f);
    CHECK(unw.outlineJitter == 1.1f);

    const StyleSettings res = stylePack(StylePack::Restored);
    CHECK(res.celSteps == 3.f);
    CHECK(res.outlineColor == StyleVec3(0x17 / 255.f, 0x15 / 255.f, 0x1A / 255.f));
}

TEST_CASE("lerp endpoints are exact and t is clamped") {
    const StyleSettings a = stylePack(StylePack::Solhaven);
    const StyleSettings b = stylePack(StylePack::Unwritten);
    CHECK(ftcore::lerp(a, b, 0.f) == a);
    CHECK(ftcore::lerp(a, b, 1.f) == b);
    CHECK(ftcore::lerp(a, b, -3.f) == a);
    CHECK(ftcore::lerp(a, b, 7.f) == b);
    CHECK(ftcore::lerp(a, a, 0.37f) == a);

    const StyleSettings m = ftcore::lerp(a, b, 0.5f);
    CHECK(approx(m.outlineWidth, (2.4f + 1.3f) * 0.5f));
    CHECK(approx(m.monochrome, 0.5f));
    CHECK(approx(m.outlineColor, (a.outlineColor + b.outlineColor) * 0.5f));
}

TEST_CASE("lerp and the dial cover the engine-delta fields") {
    const StyleSettings sol = stylePack(StylePack::Solhaven);
    const StyleSettings lat = stylePack(StylePack::Lattice);
    const StyleSettings unw = stylePack(StylePack::Unwritten);
    const StyleSettings sl = ftcore::lerp(sol, lat, 0.5f);
    CHECK(approx(sl.rimScale, 0.5f * (1.f + 0.35f)));
    CHECK(approx(sl.rimTint, (sol.rimTint + lat.rimTint) * 0.5f));
    CHECK(approx(sl.bloomThresholdScale, 0.5f * (1.f + 2.5f)));
    CHECK(approx(sl.hatchOverlay, 0.5f));
    CHECK(approx(sl.hatchMaxLevel, 0.5f * (2.f + 5.f)));
    CHECK(approx(sl.hatchShadowOnly, 0.5f));
    const StyleSettings su = ftcore::lerp(sol, unw, 0.25f);
    CHECK(approx(su.hatchCurve, 0.75f * 1.f + 0.25f * 1.8f));
    CHECK(approx(su.hatchScale, 0.75f * 240.f + 0.25f * 280.f));
    StyleSettings clip;
    clip.blackPoint = 0.06f;
    CHECK(approx(ftcore::lerp(StyleSettings{}, clip, 0.5f).blackPoint, 0.03f));
    StyleSettings dark;
    dark.inkVoid = 1.f;
    CHECK(approx(ftcore::lerp(StyleSettings{}, dark, 0.25f).inkVoid, 0.25f));
    // The dial: L1 is a third of the Lattice, the rest None.
    const StyleSettings l1 = ftcore::styleFromDial({0.f, 1.f, 0.f, 0.f});
    CHECK(approx(l1.rimScale, 0.35f / 3.f));
    CHECK(approx(l1.bloomThresholdScale, 2.f / 3.f + 2.5f / 3.f));
}

TEST_CASE("lerp keeps the island and the ink void of the side that has one") {
    StyleSettings withIsland = stylePack(StylePack::Unwritten);
    withIsland.island = 1.f;
    withIsland.islandCenter = StyleVec3(3.f, 0.f, -2.f);
    withIsland.islandRadius = 5.f;
    withIsland.inkVoid = 1.f;
    withIsland.inkVoidNear = 4.f;
    withIsland.inkVoidFar = 12.f;
    const StyleSettings none;
    for (float t : {0.25f, 0.5f, 0.75f}) {
        const StyleSettings r = ftcore::lerp(none, withIsland, t);
        CHECK(r.islandCenter == withIsland.islandCenter);
        CHECK(r.islandRadius == 5.f);
        CHECK(r.inkVoidNear == 4.f);
        CHECK(r.inkVoidFar == 12.f);
        CHECK(approx(r.island, t));
        const StyleSettings back = ftcore::lerp(withIsland, none, t);
        CHECK(back.islandCenter == withIsland.islandCenter);
        CHECK(back.inkVoidFar == 12.f);
    }
}

TEST_CASE("lerp weighs the face fields by hatch") {
    // Lattice does not hatch, so Solhaven's face treatment is kept in full at any blend weight.
    const StyleSettings r = ftcore::lerp(stylePack(StylePack::Solhaven), stylePack(StylePack::Lattice), 0.5f);
    CHECK(r.faceFeatures == 1.f);
    CHECK(approx(r.hatch, 0.5f));
    // Both hatch: plain hatch-weighted average where they differ, exact where they agree.
    const StyleSettings su = ftcore::lerp(stylePack(StylePack::Solhaven), stylePack(StylePack::Unwritten), 0.25f);
    CHECK(su.faceFeatures == 1.f);
    CHECK(approx(su.faceHatch, 0.75f * 1.f + 0.25f * 0.6f));
    // faceSnap blends linearly.
    StyleSettings snap;
    snap.faceSnap = 4.f;
    CHECK(approx(ftcore::lerp(StyleSettings{}, snap, 0.5f).faceSnap, 2.f));
}

TEST_CASE("styleFromDial weights") {
    CHECK(ftcore::styleFromDial({}) == StyleSettings{});
    CHECK(ftcore::styleFromDial({3.f, 0.f, 0.f, 0.f}) == stylePack(StylePack::Solhaven));
    CHECK(ftcore::styleFromDial({0.f, 0.f, 3.f, 0.f}) == stylePack(StylePack::Unwritten));
    CHECK(ftcore::styleFromDial({0.f, 0.f, 0.f, 9.f}) == stylePack(StylePack::Restored)); // clamped to 3

    // S3 U1: 3/4 Solhaven, 1/4 Unwritten.
    const StyleSettings s3u1 = ftcore::styleFromDial({3.f, 0.f, 1.f, 0.f});
    CHECK(approx(s3u1.outlineWidth, 0.75f * 2.4f + 0.25f * 1.3f));
    CHECK(approx(s3u1.monochrome, 0.25f));
    CHECK(approx(s3u1.saturation, 0.75f * 1.25f));

    // A lone needle at 1 is a third of the pack, the rest is None.
    const StyleSettings l1 = ftcore::styleFromDial({0.f, 1.f, 0.f, 0.f});
    CHECK(approx(l1.outline, 0.9f / 3.f));
    CHECK(approx(l1.saturation, 2.f / 3.f + 1.3f / 3.f));

    // S3 L1: Lattice does not hatch, so faceFeatures stays Solhaven's (plain weights would give 0.75).
    CHECK(ftcore::styleFromDial({3.f, 1.f, 0.f, 0.f}).faceFeatures == 1.f);
}

TEST_CASE("Ink Solhaven preset") {
    const StyleSettings s = ftcore::inkSolhavenStyle();
    const StyleSettings dial = ftcore::styleFromDial({1.f, 0.f, 3.f, 0.f});
    CHECK(s.saturation == 1.f);
    CHECK(s.monochrome == 0.8f);
    CHECK(s.cel == 1.f);
    CHECK(s.celSteps == 2.f);
    CHECK(s.paperSky == 1.f);
    CHECK(s.outlineWidth == dial.outlineWidth);
    CHECK(approx(s.outlineWidth, 0.25f * 2.4f + 0.75f * 1.3f));
    CHECK(approx(s.hatchOverlay, 0.25f));
}

TEST_CASE("parseStyleSpec") {
    using ftcore::parseStyleSpec;
    REQUIRE(parseStyleSpec("unwritten"));
    CHECK(*parseStyleSpec("unwritten") == stylePack(StylePack::Unwritten));
    CHECK(*parseStyleSpec("SolHaven") == stylePack(StylePack::Solhaven));
    CHECK(*parseStyleSpec("none") == StyleSettings{});
    CHECK(*parseStyleSpec("InkSolhaven") == ftcore::inkSolhavenStyle());
    CHECK(*parseStyleSpec("S3,U1") == ftcore::styleFromDial({3.f, 0.f, 1.f, 0.f}));
    CHECK(*parseStyleSpec("u1 s3") == ftcore::styleFromDial({3.f, 0.f, 1.f, 0.f}));
    CHECK(*parseStyleSpec("S=3;L:0.5") == ftcore::styleFromDial({3.f, 0.5f, 0.f, 0.f}));
    CHECK(*parseStyleSpec("3,0,0.5,0") == ftcore::styleFromDial({3.f, 0.f, 0.5f, 0.f}));
    CHECK(*parseStyleSpec("0/0/0/3") == stylePack(StylePack::Restored));

    CHECK_FALSE(parseStyleSpec(""));
    CHECK_FALSE(parseStyleSpec("sunhaven"));
    CHECK_FALSE(parseStyleSpec("S4"));          // out of range
    CHECK_FALSE(parseStyleSpec("S-1"));
    CHECK_FALSE(parseStyleSpec("S3,S1"));       // repeated needle
    CHECK_FALSE(parseStyleSpec("3,1"));         // bare needles need all four
    CHECK_FALSE(parseStyleSpec("3,0,0,0,0"));
    CHECK_FALSE(parseStyleSpec("S3,1,0,0"));    // named and bare mixed
    CHECK_FALSE(parseStyleSpec("S3x"));
    CHECK_FALSE(parseStyleSpec("S"));
    CHECK_FALSE(parseStyleSpec("S1.000000000000000"));

    const auto dial = ftcore::parseStyleDial("L2 R1");
    REQUIRE(dial);
    CHECK(dial->s == 0.f);
    CHECK(dial->l == 2.f);
    CHECK(dial->u == 0.f);
    CHECK(dial->r == 1.f);
}

TEST_CASE("hatchWeights picks neighbouring levels") {
    const auto paper = ftcore::hatchWeights(1.f);
    CHECK(paper.tam[0] == 0.f);
    CHECK(paper.solid == 0.f);
    const auto ink = ftcore::hatchWeights(0.f);
    CHECK(ink.solid == 1.f);
    const auto capped = ftcore::hatchWeights(0.f, 1.f, 1.f, 2.f);
    CHECK(capped.tam[1] == 1.f);
    CHECK(capped.solid == 0.f);
    const auto mid = ftcore::hatchWeights(0.5f); // d = 2.5
    CHECK(approx(mid.tam[1], 0.5f));
    CHECK(approx(mid.tam[2], 0.5f));
}

TEST_CASE("hatchWeights tone curve keeps the cross to the darks") {
    // The Unwritten: density 1.4, curve 1.8. The cross (level 3) begins at tone 0.50 and is full at 0.375.
    const auto mid = ftcore::hatchWeights(0.5f, 1.4f, 1.8f); // d ~ 2.01
    CHECK(mid.tam[2] > 0.f);
    CHECK(mid.tam[2] < 0.05f);
    CHECK(mid.tam[1] > 0.95f);
    const auto lighter = ftcore::hatchWeights(0.52f, 1.4f, 1.8f); // d ~ 1.87: one direction only
    CHECK(lighter.tam[2] == 0.f);
    CHECK(lighter.tam[1] > 0.f);
    const auto cross = ftcore::hatchWeights(0.375f, 1.4f, 1.8f); // d ~ 3.0
    CHECK(approx(cross.tam[2], 1.f, 0.02f));
    CHECK(cross.solid == 0.f);
    // Without the curve the same density reaches the cross much earlier, at tone 0.71.
    CHECK(ftcore::hatchWeights(0.65f, 1.4f).tam[2] > 0.f);
    CHECK(ftcore::hatchWeights(0.65f, 1.4f, 1.8f).tam[2] == 0.f);
    // Curve 1 is the plain law, and the cap still applies with a curve.
    CHECK(approx(ftcore::hatchWeights(0.5f, 1.f, 1.f).tam[1], ftcore::hatchWeights(0.5f).tam[1]));
    const auto capped = ftcore::hatchWeights(0.f, 1.4f, 1.8f, 2.f);
    CHECK(capped.tam[1] == 1.f);
    CHECK(capped.tam[2] == 0.f);
    CHECK(capped.solid == 0.f);
}

TEST_CASE("Blending allocates nothing") {
    const StyleSettings a = stylePack(StylePack::Solhaven);
    const StyleSettings b = stylePack(StylePack::Lattice);
    float sink = 0.f;
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i < 240; ++i) {
        const float t = static_cast<float>(i) / 240.f;
        sink += ftcore::lerp(a, b, t).outlineWidth;
        sink += ftcore::styleFromDial({3.f * t, 0.f, 3.f - 3.f * t, 0.f}).monochrome;
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(sink > 0.f);
}
