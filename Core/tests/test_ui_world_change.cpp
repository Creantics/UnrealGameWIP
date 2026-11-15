#include "ftcore/UiWorldChange.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 30.f;
constexpr std::int8_t kSol = static_cast<std::int8_t>(UiWorld::Solhaven);
constexpr std::int8_t kLat = static_cast<std::int8_t>(UiWorld::Lattice);

UiWorldChangeIn at(std::int8_t world, bool on = true, bool moving = true, bool drawn = true) {
    UiWorldChangeIn in;
    in.world = world;
    in.on = on;
    in.moving = moving;
    in.drawn = drawn;
    return in;
}

void run(UiWorldChange& s, const UiWorldChangeIn& in, float seconds) {
    for (float t = 0.f; t < seconds - 1e-4f; t += kDt) uiWorldChangeStep(s, in, kDt);
}

} // namespace

TEST_CASE("ui world change: off by default, the HUD does nothing as the wave passes") {
    CHECK_FALSE(UiWorldChangeIn{}.on);
    UiWorldChange s;
    uiWorldChangeStep(s, at(kSol, false), kDt);
    CHECK(s.shown == kSol); // the first world: taken silently
    CHECK_FALSE(s.running);
    for (int i = 0; i < 60; ++i) {
        CAPTURE(i);
        uiWorldChangeStep(s, at(i < 30 ? kSol : kLat, false), kDt);
        CHECK_FALSE(s.running);
        CHECK(s.shown == kSol);
        CHECK(uiWorldChangeMix(s, 0.f) == 1.f);
        CHECK(uiWorldChangeMix(s, 1.f) == 1.f);
    }
    uiWorldChangeStep(s, at(kLat, false, true, false), kDt); // nothing drawn: the ink changes unseen
    CHECK(s.shown == kLat);
    CHECK_FALSE(s.running);
}

TEST_CASE("ui world change: switched on, the crossing's frame starts a 0.3 s sweep") {
    UiWorldChange s;
    run(s, at(kSol), 1.f);
    CHECK_FALSE(s.running);
    uiWorldChangeStep(s, at(kLat), kDt);
    CHECK(s.running);
    CHECK(s.from == kSol);
    CHECK(s.shown == kLat);
    CHECK(uiWorldChangeMix(s, 0.f) > 0.f);
    CHECK(uiWorldChangeMix(s, 0.f) > uiWorldChangeMix(s, 1.f)); // the first point leads
    float last = uiWorldChangeMix(s, 0.5f);
    for (int i = 0; i < 6; ++i) {
        uiWorldChangeStep(s, at(kLat), kDt);
        CAPTURE(i);
        CHECK(uiWorldChangeMix(s, 0.5f) >= last);
        last = uiWorldChangeMix(s, 0.5f);
    }
    CHECK(s.running); // seven frames, about 0.23 s
    CHECK(uiWorldChangeMix(s, 0.f) == 1.f);
    CHECK(uiWorldChangeMix(s, 1.f) < 1.f);
    run(s, at(kLat), 0.2f);
    CHECK_FALSE(s.running);
    CHECK(s.shown == kLat);
    CHECK(s.from == kUiWorldNone);
    CHECK(uiWorldChangeMix(s, 1.f) == 1.f);
    // Edge-detected: the same world on later frames starts nothing.
    run(s, at(kLat), 2.f);
    CHECK_FALSE(s.running);
}

TEST_CASE("ui world change: nothing for a settled edge, a hidden HUD or no world") {
    UiWorldChange s;
    uiWorldChangeStep(s, at(kUiWorldNone), kDt);
    CHECK(s.shown == kUiWorldNone);
    uiWorldChangeStep(s, at(kLat), kDt);
    CHECK(s.shown == kLat); // the first world: no envelope
    CHECK_FALSE(s.running);
    run(s, at(kSol, true, false), 1.f); // a settled edge (a load, a teleport) with the rows drawn: it waits
    CHECK_FALSE(s.running);
    CHECK(s.shown == kLat);
    uiWorldChangeStep(s, at(kSol, true, false, false), kDt);
    CHECK(s.shown == kSol);
    CHECK_FALSE(s.running);
    uiWorldChangeStep(s, at(kLat, true, true, false), kDt); // a crossing with nothing drawn: unseen
    CHECK(s.shown == kLat);
    CHECK_FALSE(s.running);
    uiWorldChangeStep(s, at(kUiWorldNone), kDt); // no world read: kept
    CHECK(s.shown == kLat);
    CHECK_FALSE(uiWorldChangeValid(static_cast<std::int8_t>(kUiWorldCount)));
}

TEST_CASE("ui world change: the camera backing out runs the sweep back without a jump") {
    UiWorldChange s;
    run(s, at(kSol), 0.5f);
    uiWorldChangeStep(s, at(kLat), kDt);
    run(s, at(kLat), 2.f * kDt);
    CHECK(s.running);
    const float xs[] = {0.f, 0.5f, 1.f};
    float lattice[3] = {};
    for (int i = 0; i < 3; ++i) lattice[i] = uiWorldChangeMix(s, xs[i]); // the Lattice ink's share
    uiWorldChangeStep(s, at(kSol), 0.f);
    CHECK(s.running);
    CHECK(s.shown == kSol);
    CHECK(s.from == kLat);
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(1.f - uiWorldChangeMix(s, xs[i]) == doctest::Approx(lattice[i]));
    }
    run(s, at(kSol), 0.4f);
    CHECK_FALSE(s.running);
    CHECK(s.shown == kSol);
}

namespace {

// A style with a distinct colour for every world and token, so no two worlds share one.
UiStyle distinctStyle() {
    UiStyle st;
    for (std::size_t w = 0; w < kUiWorldCount; ++w)
        for (std::size_t t = 0; t < kUiTokenCount; ++t) {
            const float fw = static_cast<float>(w);
            const float ft = static_cast<float>(t);
            st.world[w][t] = UiColor{0.1f + 0.3f * fw, 0.05f * ft, 0.9f - 0.3f * fw, 1.f - 0.2f * fw};
        }
    return st;
}

} // namespace

TEST_CASE("ui world change: the sweep carries Ink, InkDim and Accent together; every other token is the shown world's") {
    CHECK(uiWorldChangeSwept(UiToken::Ink));
    CHECK(uiWorldChangeSwept(UiToken::InkDim));
    CHECK(uiWorldChangeSwept(UiToken::Accent));
    CHECK_FALSE(uiWorldChangeSwept(UiToken::Backing));
    CHECK_FALSE(uiWorldChangeSwept(UiToken::Accent2));
    CHECK_FALSE(uiWorldChangeSwept(UiToken::Danger));
    const UiStyle st = distinctStyle();
    const UiPalette pal = UiPalette::Default;
    const UiToken swept[] = {UiToken::Ink, UiToken::InkDim, UiToken::Accent};

    UiWorldChange s;
    CHECK(uiWorldChangeColor(s, st, pal, UiToken::Ink, 0.f) == uiColor(st, UiWorld::Solhaven, pal, UiToken::Ink)); // none yet
    run(s, at(kSol), 0.5f);
    for (std::size_t t = 0; t < kUiTokenCount; ++t) {
        const UiToken token = static_cast<UiToken>(t);
        CHECK(uiWorldChangeColor(s, st, pal, token, 0.5f) == uiColor(st, UiWorld::Solhaven, pal, token)); // settled
    }
    uiWorldChangeStep(s, at(kLat), kDt);
    run(s, at(kLat), 2.f * kDt);
    REQUIRE(s.running);
    const float xs[] = {0.f, 0.25f, 0.5f}; // reached by 0.1 s in
    for (const float x : xs) {
        CAPTURE(x);
        const float m = uiWorldChangeMix(s, x);
        CHECK(m > 0.f);
        CHECK(m < 1.f);
        for (const UiToken token : swept) {
            CAPTURE(static_cast<int>(token));
            const UiColor a = uiColor(st, UiWorld::Solhaven, pal, token);
            const UiColor c = uiColor(st, UiWorld::Lattice, pal, token);
            const UiColor v = uiWorldChangeColor(s, st, pal, token, x);
            // One mix for all three tokens and every channel.
            CHECK(v.r == doctest::Approx(a.r + (c.r - a.r) * m));
            CHECK(v.g == doctest::Approx(a.g + (c.g - a.g) * m));
            CHECK(v.b == doctest::Approx(a.b + (c.b - a.b) * m));
            CHECK(v.a == doctest::Approx(a.a + (c.a - a.a) * m));
        }
        CHECK(uiWorldChangeColor(s, st, pal, UiToken::Danger, x) == uiColor(st, UiWorld::Lattice, pal, UiToken::Danger));
        CHECK(uiWorldChangeColor(s, st, pal, UiToken::Backing, x) == uiColor(st, UiWorld::Lattice, pal, UiToken::Backing));
    }
    // A palette that overrides a token holds it through the sweep: both ends are the override.
    UiStyle high = st;
    const std::size_t hcAt = static_cast<std::size_t>(UiPalette::HighContrast);
    const std::size_t inkAt = static_cast<std::size_t>(UiToken::Ink);
    const UiColor white{1.f, 1.f, 1.f, 1.f};
    high.paletteColor[hcAt][inkAt] = white;
    high.paletteOverrides[hcAt][inkAt] = true;
    CHECK(uiWorldChangeColor(s, high, UiPalette::HighContrast, UiToken::Ink, 0.5f) == white);
    run(s, at(kLat), 0.4f);
    REQUIRE_FALSE(s.running);
    for (const UiToken token : swept)
        CHECK(uiWorldChangeColor(s, st, pal, token, 1.f) == uiColor(st, UiWorld::Lattice, pal, token)); // done
}

TEST_CASE("ui world change: stepping allocates nothing") {
    const UiStyle st = defaultUiStyle();
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    UiWorldChange s;
    float sum = 0.f;
    for (int i = 0; i < 300; ++i) {
        uiWorldChangeStep(s, at((i / 20) % 2 ? kLat : kSol), kDt);
        sum += uiWorldChangeMix(s, 0.25f) + uiWorldChangeColor(s, st, UiPalette::Default, UiToken::Accent, 0.25f).r;
    }
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK(sum > 0.f);
}
