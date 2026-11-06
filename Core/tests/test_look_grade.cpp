#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "TestAssets.h"
#include "ftcore/LookGrade.h"
#include "ftcore/LookTable.h"

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr FightAccent kNoAccent{};

float hueGap(float a, float b) {
    float d = std::fabs(a - b);
    return d > 180.f ? 360.f - d : d;
}

GradeConstants constantsOf(const SceneLook& look, const FightAccent& accent = kNoAccent) {
    return gradeConstants(look, accent);
}

// rgb graded by `look`.
void graded(const SceneLook& look, const float in[3], float out[3]) {
    const GradeConstants g = constantsOf(look);
    for (int i = 0; i < 3; ++i) out[i] = in[i];
    gradeSrgb(g, out);
}

LookTable shippedLooks() {
    std::string text;
    LookTable t;
    std::string error;
    const bool read = ftcore_test::readProjectData("Look/looks.json", text);
    REQUIRE(read);
    REQUIRE_MESSAGE(importLookTable(text, t, &error), error);
    return t;
}

// Every sample of a 9 x 9 x 9 grid of sRGB values.
template <class F>
void forGrid(F&& f) {
    for (int r = 0; r < 9; ++r)
        for (int g = 0; g < 9; ++g)
            for (int b = 0; b < 9; ++b) {
                const float rgb[3] = {static_cast<float>(r) / 8.f, static_cast<float>(g) / 8.f,
                                      static_cast<float>(b) / 8.f};
                f(rgb);
            }
}

SceneLook withoutTint(SceneLook look) {
    look.shadowTintChroma = 0.f;
    look.highlightTintChroma = 0.f;
    return look;
}

} // namespace

TEST_CASE("grade: the default look is the identity, in the flag and in the maths") {
    const SceneLook look;
    const GradeConstants g = constantsOf(look);
    CHECK(gradeIsIdentity(g));
    float worst = 0.f;
    forGrid([&](const float in[3]) {
        float out[3] = {in[0], in[1], in[2]};
        gradeSrgb(g, out);
        for (int i = 0; i < 3; ++i) worst = std::fmax(worst, std::fabs(out[i] - in[i]));
    });
    CHECK(worst < 2e-4f);
}

TEST_CASE("grade: gradeIsIdentity sees every setting that changes a pixel") {
    SceneLook look;
    CHECK(gradeIsIdentity(constantsOf(look)));
    { SceneLook l = look; l.gradeChroma = 0.9f; CHECK_FALSE(gradeIsIdentity(constantsOf(l))); }
    { SceneLook l = look; l.shadowTintChroma = 0.01f; l.shadowTintHue = 90.f; CHECK_FALSE(gradeIsIdentity(constantsOf(l))); }
    { SceneLook l = look; l.highlightTintChroma = 0.01f; CHECK_FALSE(gradeIsIdentity(constantsOf(l))); }
    // A window with chroma 1 changes nothing; with another chroma it does; with width 0 it is off.
    { SceneLook l = look; l.keepAWidth = 30.f; CHECK(gradeIsIdentity(constantsOf(l))); }
    { SceneLook l = look; l.keepAWidth = 30.f; l.keepAChroma = 1.2f; CHECK_FALSE(gradeIsIdentity(constantsOf(l))); }
    { SceneLook l = look; l.keepBChroma = 1.2f; CHECK(gradeIsIdentity(constantsOf(l))); }
    // The skin floor only matters when something can scale below it.
    { SceneLook l = look; l.skinFloor = 0.9f; CHECK(gradeIsIdentity(constantsOf(l))); }
    { SceneLook l = look; l.skinFloor = 1.2f; CHECK_FALSE(gradeIsIdentity(constantsOf(l))); }
    { SceneLook l = look; l.keepAWidth = 30.f; l.keepAChroma = 1.0f; l.skinFloor = 1.0f; CHECK(gradeIsIdentity(constantsOf(l))); }
    // The fight's accent counts too.
    FightAccent accent;
    accent.hueDeg = 25.f;
    accent.widthDeg = 20.f;
    accent.chroma = 1.3f;
    CHECK_FALSE(gradeIsIdentity(constantsOf(look, accent)));
    accent.chroma = 1.f;
    CHECK(gradeIsIdentity(constantsOf(look, accent)));
}

TEST_CASE("grade: gradeConstants packs radians and the tint offsets") {
    SceneLook look;
    look.gradeChroma = 0.7f;
    look.skinFloor = 0.5f;
    look.skinHue = 90.f;
    look.skinWidth = 45.f;
    look.keepAHue = 180.f;
    look.keepAWidth = 90.f;
    look.keepAChroma = 1.25f;
    look.keepBHue = 270.f;
    look.keepBWidth = 10.f;
    look.keepBChroma = 1.1f;
    look.shadowTintHue = 0.f;
    look.shadowTintChroma = 0.02f;
    look.highlightTintHue = 90.f;
    look.highlightTintChroma = 0.01f;
    const FightAccent accent{180.f, 60.f, 1.4f};
    const GradeConstants g = constantsOf(look, accent);
    const float pi = 3.14159265f;
    CHECK(g.c[0][0] == doctest::Approx(0.7f));
    CHECK(g.c[0][1] == doctest::Approx(0.5f));
    CHECK(g.c[0][2] == doctest::Approx(pi / 2.f));
    CHECK(g.c[0][3] == doctest::Approx(pi / 4.f));
    CHECK(g.c[1][0] == doctest::Approx(pi));
    CHECK(g.c[1][1] == doctest::Approx(pi / 2.f));
    CHECK(g.c[1][2] == doctest::Approx(1.25f));
    CHECK(g.c[2][0] == doctest::Approx(1.5f * pi));
    CHECK(g.c[2][2] == doctest::Approx(1.1f));
    CHECK(g.c[3][0] == doctest::Approx(pi));
    CHECK(g.c[3][1] == doctest::Approx(pi / 3.f));
    CHECK(g.c[3][2] == doctest::Approx(1.4f));
    CHECK(g.c[4][0] == doctest::Approx(0.02f));
    CHECK(g.c[4][1] == doctest::Approx(0.f).epsilon(1e-6));
    CHECK(g.c[4][2] == doctest::Approx(0.f).epsilon(1e-6));
    CHECK(g.c[4][3] == doctest::Approx(0.01f));
}

TEST_CASE("grade: each shipped world's grade does something") {
    const LookTable t = shippedLooks();
    for (const LookTable::Entry& e : t.worlds) {
        CAPTURE(e.id);
        CHECK_FALSE(gradeIsIdentity(constantsOf(e.look)));
    }
}

TEST_CASE("grade: gradeChroma 0.5 halves the chroma and keeps the hue") {
    SceneLook look;
    look.gradeChroma = 0.5f;
    const float samples[][3] = {{0.8f, 0.3f, 0.3f}, {0.2f, 0.7f, 0.3f}, {0.3f, 0.4f, 0.9f},
                                {0.9f, 0.8f, 0.2f}, {0.6f, 0.2f, 0.7f}, {0.2f, 0.7f, 0.8f}};
    for (const auto& in : samples) {
        float out[3];
        graded(look, in, out);
        CAPTURE(in[0]);
        CAPTURE(in[1]);
        CAPTURE(in[2]);
        const float c0 = oklabChroma(in), c1 = oklabChroma(out);
        CHECK(c1 / c0 == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(hueGap(oklabHueDeg(in), oklabHueDeg(out)) < 0.5f);
    }
}

TEST_CASE("grade: greys stay grey under a chroma change and no tint") {
    SceneLook look;
    look.gradeChroma = 0.3f;
    for (int i = 0; i <= 16; ++i) {
        const float v = static_cast<float>(i) / 16.f;
        const float in[3] = {v, v, v};
        float out[3];
        graded(look, in, out);
        CAPTURE(v);
        CHECK(oklabChroma(out) < 1e-3f);
        for (int c = 0; c < 3; ++c) CHECK(std::fabs(out[c] - v) < 2e-3f);
    }
}

TEST_CASE("grade: skin tones keep their chroma against a global desaturation") {
    const float skin[3] = {0.87f, 0.67f, 0.55f};
    // The sample sits inside the default skin window.
    CHECK(hueGap(oklabHueDeg(skin), 55.f) < 25.f);

    SceneLook plain;
    plain.gradeChroma = 0.5f;
    float out[3];
    graded(plain, skin, out);
    CHECK(oklabChroma(out) / oklabChroma(skin) == doctest::Approx(0.5f).epsilon(0.02));

    SceneLook protectedSkin = plain;
    protectedSkin.skinFloor = 0.9f;
    graded(protectedSkin, skin, out);
    CHECK(oklabChroma(out) / oklabChroma(skin) >= 0.88f);

    // A colour well outside the skin window is not protected.
    const float blue[3] = {0.3f, 0.4f, 0.9f};
    graded(protectedSkin, blue, out);
    CHECK(oklabChroma(out) / oklabChroma(blue) == doctest::Approx(0.5f).epsilon(0.02));
}

TEST_CASE("grade: a keep window holds its colour while the rest is scaled by gradeChroma") {
    SceneLook look;
    look.gradeChroma = 0.5f;
    look.keepAHue = 255.f;
    look.keepAWidth = 40.f;
    look.keepAChroma = 1.f;
    // A blue near hue 255 and a red far from it.
    const float blue[3] = {0.2f, 0.5f, 0.9f};
    const float red[3] = {0.85f, 0.25f, 0.25f};
    REQUIRE(hueGap(oklabHueDeg(blue), 255.f) < 10.f);
    float out[3];
    graded(look, blue, out);
    CHECK(oklabChroma(out) / oklabChroma(blue) == doctest::Approx(1.f).epsilon(0.03));
    graded(look, red, out);
    CHECK(oklabChroma(out) / oklabChroma(red) == doctest::Approx(0.5f).epsilon(0.02));
}

TEST_CASE("grade: a fight's accent window works like a keep window, and off when its width is 0") {
    SceneLook look;
    look.gradeChroma = 0.5f;
    const float red[3] = {0.85f, 0.25f, 0.25f};
    FightAccent accent;
    accent.hueDeg = oklabHueDeg(red);
    accent.widthDeg = 30.f;
    accent.chroma = 1.f;
    float out[3] = {red[0], red[1], red[2]};
    gradeSrgb(constantsOf(look, accent), out);
    CHECK(oklabChroma(out) / oklabChroma(red) == doctest::Approx(1.f).epsilon(0.03));
    accent.widthDeg = 0.f;
    for (int i = 0; i < 3; ++i) out[i] = red[i];
    gradeSrgb(constantsOf(look, accent), out);
    CHECK(oklabChroma(out) / oklabChroma(red) == doctest::Approx(0.5f).epsilon(0.02));
}

TEST_CASE("grade: a window wraps around 0 and 360 degrees") {
    SceneLook look;
    look.gradeChroma = 0.5f;
    // Red is near OKLab hue 29; a window at 350 degrees 60 wide reaches across 0.
    const float red[3] = {0.85f, 0.25f, 0.25f};
    const float hue = oklabHueDeg(red);
    REQUIRE(hue < 40.f);
    look.keepAHue = 360.f - (15.f - hue);
    look.keepAWidth = 60.f;
    look.keepAChroma = 1.f;
    float out[3];
    graded(look, red, out);
    CHECK(oklabChroma(out) / oklabChroma(red) > 0.9f);
}

TEST_CASE("grade: split tone leans the shadows and the highlights toward their hues") {
    SceneLook look;
    look.shadowTintHue = 250.f;
    look.shadowTintChroma = 0.02f;
    look.highlightTintHue = 70.f;
    look.highlightTintChroma = 0.02f;
    const float dark[3] = {0.12f, 0.12f, 0.12f};
    const float light[3] = {0.88f, 0.88f, 0.88f};
    float out[3];
    graded(look, dark, out);
    CHECK(oklabChroma(out) > 0.005f);
    CHECK(hueGap(oklabHueDeg(out), 250.f) < 6.f);
    graded(look, light, out);
    CHECK(oklabChroma(out) > 0.005f);
    CHECK(hueGap(oklabHueDeg(out), 70.f) < 6.f);
    // The dark grey is bluer than it was, the light grey warmer.
    graded(look, dark, out);
    CHECK(out[2] > out[0]);
    graded(look, light, out);
    CHECK(out[0] > out[2]);
}

TEST_CASE("grade: every shipped world keeps every output in 0..1 over the grid") {
    const LookTable t = shippedLooks();
    for (const LookTable::Entry& e : t.worlds) {
        CAPTURE(e.id);
        FightAccent accent;
        accent.hueDeg = 25.f;
        accent.widthDeg = 30.f;
        accent.chroma = 1.6f;
        const GradeConstants g = constantsOf(e.look, accent);
        bool inRange = true;
        forGrid([&](const float in[3]) {
            float out[3] = {in[0], in[1], in[2]};
            gradeSrgb(g, out);
            for (int i = 0; i < 3; ++i) inRange = inRange && out[i] >= 0.f && out[i] <= 1.f && std::isfinite(out[i]);
        });
        CHECK(inRange);
    }
}

TEST_CASE("grade: with the tints zeroed and no chroma boost, a shipped world turns no hue by 3 degrees") {
    const LookTable t = shippedLooks();
    for (const LookTable::Entry& e : t.worlds) {
        CAPTURE(e.id);
        SceneLook look = withoutTint(e.look);
        look.keepAChroma = std::fmin(look.keepAChroma, 1.f);
        look.keepBChroma = std::fmin(look.keepBChroma, 1.f);
        const GradeConstants g = constantsOf(look);
        float worst = 0.f;
        int tested = 0;
        forGrid([&](const float in[3]) {
            if (oklabChroma(in) <= 0.03f) return;
            float out[3] = {in[0], in[1], in[2]};
            gradeSrgb(g, out);
            worst = std::fmax(worst, hueGap(oklabHueDeg(in), oklabHueDeg(out)));
            ++tested;
        });
        CHECK(tested > 100);
        CHECK(worst < 3.f);
    }
}

TEST_CASE("grade: a chroma boost turns a hue only where the boosted colour leaves the sRGB gamut and is clipped") {
    const LookTable t = shippedLooks();
    for (const LookTable::Entry& e : t.worlds) {
        CAPTURE(e.id);
        const GradeConstants g = constantsOf(withoutTint(e.look));
        float worst = 0.f;
        forGrid([&](const float in[3]) {
            if (oklabChroma(in) <= 0.03f) return;
            float out[3] = {in[0], in[1], in[2]};
            gradeSrgb(g, out);
            worst = std::fmax(worst, hueGap(oklabHueDeg(in), oklabHueDeg(out)));
        });
        // The clip of a boosted, nearly-on-the-gamut-edge colour is the only cause; it stays small.
        CHECK(worst < 6.f);
    }
}

TEST_CASE("grade: running the grade allocates nothing") {
    const LookTable t = shippedLooks();
    const SceneLook look = t.worlds[0].look;
    float sink = 0.f;
    const auto before = ftcore_test::allocStats().count;
    const GradeConstants g = gradeConstants(look, kNoAccent);
    for (int i = 0; i < 256; ++i) {
        float rgb[3] = {static_cast<float>(i) / 255.f, 0.4f, 0.6f};
        gradeSrgb(g, rgb);
        sink += rgb[0] + oklabChroma(rgb) + oklabHueDeg(rgb);
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(sink > 0.f);
}

TEST_CASE("look: the grade fields blend linearly") {
    SceneLook a, b;
    b.gradeChroma = 0.5f;
    b.skinFloor = 0.9f;
    b.keepAHue = 200.f;
    b.keepAWidth = 40.f;
    b.keepAChroma = 1.4f;
    b.shadowTintChroma = 0.02f;
    b.highlightTintHue = 80.f;
    b.fringe = 0.4f;
    b.filmToe = 0.65f;
    b.exposureSpeedUp = 5.f;
    const SceneLook mid = lerp(a, b, 0.5f);
    CHECK(mid.gradeChroma == doctest::Approx(0.75f));
    CHECK(mid.skinFloor == doctest::Approx(0.45f));
    CHECK(mid.keepAHue == doctest::Approx(100.f));
    CHECK(mid.keepAWidth == doctest::Approx(20.f));
    CHECK(mid.keepAChroma == doctest::Approx(1.2f));
    CHECK(mid.shadowTintChroma == doctest::Approx(0.01f));
    CHECK(mid.highlightTintHue == doctest::Approx(40.f));
    CHECK(mid.fringe == doctest::Approx(0.2f));
    CHECK(mid.filmToe == doctest::Approx(0.6f));
    CHECK(mid.exposureSpeedUp == doctest::Approx(4.f));
    // Unchanged fields stay at the default.
    CHECK(mid.skinHue == a.skinHue);
    CHECK(mid.localHighlightContrast == a.localHighlightContrast);
}
