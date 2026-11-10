// Round 5 facades (ftcore/WindowFacade.h): types, the side clamp that ends the grazing smear, reveals, shop kinds,
// tints and the HLSL written from the same numbers.

#include "ftcore/WindowFacade.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>

using namespace ftcore;

namespace {

// The share of the glass (sampled across u0..u1 at mid height) whose ray lands on the back wall.
template <class Hit>
float backShare(const WindowOpening& o, Hit hit) {
    int back = 0, all = 0;
    for (int i = 0; i < 200; ++i) {
        const float u = o.u0 + (o.u1 - o.u0) * (static_cast<float>(i) + 0.5f) / 200.0f;
        const float v = 0.5f * (o.v0 + o.v1);
        back += hit(u, v).face == 0 ? 1 : 0;
        ++all;
    }
    return static_cast<float>(back) / static_cast<float>(all);
}

}  // namespace

TEST_CASE("smear: at V3's grazing view r4 sends most glass to a side wall; r5 keeps 70% or more on the back wall") {
    // V3: eye (781, 418, 8.3) on BLD-030_1's west face (x 787), bay about 2.66 m, window 1.35 m: along/in about 1.7.
    const WindowFacadeSpec spec;
    const float bw = 2.66f, sh = 3.0f;
    const WindowOpening o = windowOpening(spec.base, false, bw, sh, 1.35f);
    for (const float sign : {1.0f, -1.0f}) {
        const float dA = 1.7f * sign, dU = -0.1f, dI = 1.0f;
        const float r4 = backShare(o, [&](float u, float v) { return windowRoomHit(spec.base, bw, sh, u, v, dA, dU, dI); });
        const float r5 = backShare(
            o, [&](float u, float v) { return windowFacadeRoomHit(spec, o, bw, sh, u, v, dA, dU, dI); });
        CHECK(r4 < 0.5f);
        CHECK(r5 >= 0.7f);
    }
    // Head-on nothing changes.
    const WindowRoomHit a = windowRoomHit(spec.base, bw, sh, 0.5f, 0.5f, 0.1f, 0.0f, 1.0f);
    const WindowRoomHit b = windowFacadeRoomHit(spec, o, bw, sh, 0.5f, 0.5f, 0.1f, 0.0f, 1.0f);
    CHECK(a.x == doctest::Approx(b.x));
    CHECK(a.y == doctest::Approx(b.y));
}

TEST_CASE("types: each cuts its own grid and window; Legacy keeps r4") {
    const WindowFacadeSpec spec;
    const WindowGridSpec legacy = windowGridFor(spec, WindowType::Legacy);
    CHECK(legacy.storey == spec.base.storey);
    CHECK(legacy.bay == spec.base.bay);
    CHECK(windowWidthFor(spec, WindowType::Legacy, 7u, 1.5f) == 1.5f);
    std::set<float> storeys, bays, widths;
    for (std::uint32_t t = 1; t < kWindowTypes; ++t) {
        const WindowType ty = static_cast<WindowType>(t);
        const WindowGridSpec g = windowGridFor(spec, ty);
        const WindowTypeSpec s = windowTypeSpec(ty);
        storeys.insert(g.storey);
        bays.insert(g.bay);
        CHECK(s.reveal >= 0.05f);
        CHECK(s.reveal <= 0.3f);
        const float w = windowWidthFor(spec, ty, 1234u, 1.35f);
        widths.insert(w);
        // The window always fits its bay, so a face of 20 m cuts bays.
        const WindowFaceGrid fg = windowFaceGrid(g, 20.0f, 12.0f, w);
        CHECK(fg.bays >= 1u);
        CHECK(fg.storeys >= 1u);
        CHECK(w + 2.0f * g.pier <= fg.bayWidth + 1e-3f);
    }
    CHECK(storeys.size() >= 3u);
    CHECK(bays.size() == 4u);
    CHECK(widths.size() == 4u);
    // The office is a ribbon: mullion to mullion.
    const WindowTypeSpec office = windowTypeSpec(WindowType::Office);
    CHECK(office.ribbon);
    CHECK(windowWidthFor(spec, WindowType::Office, 99u, 1.0f) ==
          doctest::Approx(office.bay - 2.0f * office.pier));
    // The jitter stays within its band and varies by seed.
    std::set<int> seen;
    for (std::uint32_t s = 0; s < 64; ++s) {
        const float w = windowWidthFor(spec, WindowType::Apartment, s, 1.0f);
        const float base = windowTypeSpec(WindowType::Apartment).width;
        CHECK(w >= base * (1.0f - spec.widthJitter) - 1e-4f);
        CHECK(w <= base * (1.0f + spec.widthJitter) + 1e-4f);
        seen.insert(static_cast<int>(w * 100.0f));
    }
    CHECK(seen.size() >= 8u);
}

TEST_CASE("reveal: glass sits in, jambs and bottom show at an angle, nothing outside the opening") {
    const WindowOpening o{0.25f, 0.75f, 0.3f, 0.8f};
    const float bw = 2.5f, sh = 3.0f, frame = 0.06f, rev = 0.12f;
    // Outside the opening: wall, untouched.
    const WindowRevealHit w = windowReveal(frame, rev, o, bw, sh, 0.1f, 0.5f, 1.0f, 0.0f, 1.0f);
    CHECK(w.part == WindowFacadePart::Wall);
    // Head-on, the middle is glass at the same point.
    const WindowRevealHit g = windowReveal(frame, rev, o, bw, sh, 0.5f, 0.55f, 0.0f, 0.0f, 1.0f);
    CHECK(g.part == WindowFacadePart::Glass);
    CHECK(g.gu == doctest::Approx(0.5f));
    // Looking along +u (eye to the left), the right edge of the opening shows the right jamb (facing -along).
    const WindowRevealHit j = windowReveal(frame, rev, o, bw, sh, 0.74f, 0.55f, 1.0f, 0.0f, 1.0f);
    CHECK(j.part == WindowFacadePart::JambMinus);
    const WindowRevealHit jl = windowReveal(frame, rev, o, bw, sh, 0.26f, 0.55f, -1.0f, 0.0f, 1.0f);
    CHECK(jl.part == WindowFacadePart::JambPlus);
    // Looking down (eye above), the bottom of the opening shows the reveal's bottom.
    const WindowRevealHit bt = windowReveal(frame, rev, o, bw, sh, 0.5f, 0.31f, 0.0f, -1.0f, 1.0f);
    CHECK(bt.part == WindowFacadePart::RevealBottom);
    const WindowRevealHit hd = windowReveal(frame, rev, o, bw, sh, 0.5f, 0.79f, 0.0f, 1.0f, 1.0f);
    CHECK(hd.part == WindowFacadePart::RevealHead);
    // The strip that shows is the reveal times the slope: 0.12 m at 45 degrees.
    int jamb = 0;
    for (int i = 0; i < 1000; ++i) {
        const float u = o.u0 + (o.u1 - o.u0) * (static_cast<float>(i) + 0.5f) / 1000.0f;
        jamb += windowReveal(frame, rev, o, bw, sh, u, 0.55f, 1.0f, 0.0f, 1.0f).part == WindowFacadePart::JambMinus;
    }
    CHECK(static_cast<float>(jamb) / 1000.0f * (o.u1 - o.u0) * bw == doctest::Approx(rev).epsilon(0.02));
}

TEST_CASE("shops: six kinds, neighbours never share one, every kind seen") {
    std::set<std::uint32_t> kinds;
    for (std::uint32_t seed = 1; seed < 200; ++seed) {
        for (std::uint32_t c = 0; c < 12; ++c) {
            const WindowShopKind k = windowShopKind(seed, c);
            kinds.insert(static_cast<std::uint32_t>(k));
            CHECK(k != windowShopKind(seed, c + 1u));
            const std::uint32_t pic = windowShopPicture(k, seed, c);
            CHECK(pic >= kWindowRooms);
            CHECK(pic < kWindowRooms + kWindowShops);
        }
    }
    CHECK(kinds.size() == kWindowShopKinds);
}

TEST_CASE("tints: plain finishes take any colour, textured ones only neutrals; roof finishes differ") {
    std::set<std::uint32_t> plain, textured;
    for (std::uint32_t s = 0; s < 400; ++s) {
        plain.insert(windowWallTintFor(s, true));
        const std::uint32_t t = windowWallTintFor(s, false);
        textured.insert(t);
        CHECK_FALSE(windowWallTint(t).pastel);
    }
    CHECK(plain.size() == kWindowWallTintCount);
    CHECK(textured.size() >= 4u);
    CHECK(windowWallTint(0).strength == 0.0f);
    CHECK(windowWallTint(99).strength == 0.0f);
    std::set<int> roofs;
    for (std::uint32_t i = 0; i < kWindowRoofFinishCount; ++i) {
        const WindowTint& r = windowRoofFinish(i);
        roofs.insert(static_cast<int>(r.r * 100.0f) * 10000 + static_cast<int>(r.g * 100.0f) * 100 +
                     static_cast<int>(r.b * 100.0f));
    }
    CHECK(roofs.size() == kWindowRoofFinishCount);
}

TEST_CASE("HLSL: the r5 nodes carry the inputs, outputs and numbers the master wires") {
    const WindowFacadeSpec spec;
    const std::string face = windowFacadeFaceHlsl();
    for (const char* name : {"TOut", "NzOut", "TopOut", "ViewOut", "StreetOut", "FaceHOut", "FaceWOut", "UpOut"})
        CHECK(face.find(name) != std::string::npos);
    const std::string win = windowFacadeHlsl(spec);
    for (const char* name : {"Type", "Seed", "Street", "WinW", "FaceH", "FaceW", "Up", "Along", "ViewT", "Night",
                             "LitShare", "ShopLit", "CurtainShare", "Curtain =", "Emit =", "kRev", "kBand", "kFascia",
                             "rxMax", "return float4"})
        CHECK(win.find(name) != std::string::npos);
    // No float literal lost its point (an HLSL int would truncate).
    CHECK(win.find("0.25 * (u1 - u0)") != std::string::npos);
    const std::string comp = windowFacadeComposeHlsl();
    for (const char* name : {"Tint", "RoofFin", "Top", "kTint", "kRoof", "NormalOut", "EmissiveOut", "WallAlbedo"})
        CHECK(comp.find(name) != std::string::npos);
    // Deterministic.
    CHECK(win == windowFacadeHlsl(spec));
}

TEST_CASE("styles: three or more per family, the same up a column, loggias only where they belong") {
    const WindowFacadeSpec spec;
    for (std::uint32_t s = 0; s < kWindowStyles; ++s)
        CHECK(windowStyleWeight(WindowType::Legacy, static_cast<WindowStyle>(s)) == (s == 0u ? 16u : 0u));
    for (std::uint32_t t = 1; t < kWindowTypes; ++t) {
        const WindowType ty = static_cast<WindowType>(t);
        std::uint32_t total = 0, styles = 0;
        for (std::uint32_t s = 0; s < kWindowStyles; ++s) {
            total += windowStyleWeight(ty, static_cast<WindowStyle>(s));
            styles += windowStyleWeight(ty, static_cast<WindowStyle>(s)) > 0u ? 1u : 0u;
        }
        CHECK(total == kWindowStyleWeightTotal);
        CHECK(styles >= 3u);
        // Over many buildings and columns every weighted style shows, and a building mixes styles.
        std::set<int> seen;
        std::uint32_t mixed = 0;
        for (std::uint32_t seed = 1; seed < 200; ++seed) {
            std::set<int> own;
            for (std::uint32_t c = 0; c < 8; ++c) {
                const WindowStyle cs = windowColumnStyle(ty, seed * 7919u, c);
                seen.insert(static_cast<int>(cs));
                own.insert(static_cast<int>(cs));
                CHECK(windowStyleWeight(ty, cs) > 0u);
                // A column keeps its style up every storey (but a loggia's ground storey and a shop).
                for (std::uint32_t st = 1; st < 6; ++st)
                    CHECK(windowStyleFor(ty, seed * 7919u, c, st, false) ==
                          (cs == WindowStyle::Loggia && ty == WindowType::Office ? WindowStyle::Plain : cs));
                CHECK(windowStyleFor(ty, seed * 7919u, c, 0u, true) == WindowStyle::Plain);
                CHECK(windowStyleFor(ty, seed * 7919u, c, 0u, false) != WindowStyle::Loggia);
            }
            mixed += own.size() >= 2u ? 1u : 0u;
        }
        CHECK(seen.size() == styles);
        CHECK(mixed > 150u);
    }
    // Apartments and shophouses have balconies; offices never.
    CHECK(windowStyleWeight(WindowType::Apartment, WindowStyle::Loggia) > 0u);
    CHECK(windowStyleWeight(WindowType::Shophouse, WindowStyle::Loggia) > 0u);
    CHECK(windowStyleWeight(WindowType::Office, WindowStyle::Loggia) == 0u);
}

TEST_CASE("styles: openings and depths") {
    const WindowFacadeSpec spec;
    const float bw = 2.6f, sh = 2.9f, ww = 1.5f;
    const WindowOpening plain = windowStyleOpening(spec, WindowType::Apartment, WindowStyle::Plain, bw, sh, ww);
    const WindowOpening ref = windowOpening(windowGridFor(spec, WindowType::Apartment), false, bw, sh, ww);
    CHECK(plain.u0 == doctest::Approx(ref.u0));
    CHECK(plain.v0 == doctest::Approx(ref.v0));
    CHECK(plain.v1 == doctest::Approx(ref.v1));
    // A loggia runs from the floor to the head, wider than the window; a small window sits at the head.
    const WindowOpening log = windowStyleOpening(spec, WindowType::Apartment, WindowStyle::Loggia, bw, sh, ww);
    CHECK(log.v0 * sh < 0.1f);
    CHECK(log.v1 == doctest::Approx(plain.v1));
    CHECK(log.u1 - log.u0 > plain.u1 - plain.u0);
    const WindowOpening sm = windowStyleOpening(spec, WindowType::Apartment, WindowStyle::Small, bw, sh, ww);
    CHECK((sm.u1 - sm.u0) * bw == doctest::Approx(spec.smallWidth));
    CHECK((sm.v1 - sm.v0) * sh == doctest::Approx(spec.smallHeight));
    CHECK(sm.v1 == doctest::Approx(plain.v1));
    // A ribbon's small style keeps the ribbon.
    const WindowOpening rib = windowStyleOpening(spec, WindowType::Office, WindowStyle::Small, 1.8f, 3.6f, 1.72f);
    const WindowOpening ribP = windowStyleOpening(spec, WindowType::Office, WindowStyle::Plain, 1.8f, 3.6f, 1.72f);
    CHECK(rib.u0 == doctest::Approx(ribP.u0));
    CHECK(rib.v0 == doctest::Approx(ribP.v0));
    // Depths: real reveals, a loggia a metre deep, Legacy flat.
    for (std::uint32_t t = 1; t < kWindowTypes; ++t) {
        const WindowType ty = static_cast<WindowType>(t);
        CHECK(windowStyleReveal(spec, ty, WindowStyle::Plain) >= 0.12f);
        CHECK(windowStyleReveal(spec, ty, WindowStyle::Loggia) >= 1.0f);
        CHECK(windowTypeSpec(ty).frame > spec.base.frame);
    }
    CHECK(windowStyleReveal(spec, WindowType::Legacy, WindowStyle::Plain) == 0.0f);
    // The loggia's rail is lower than its opening; the sill lip is deeper than r5's.
    CHECK(spec.railHeight < (log.v1 - log.v0) * sh);
    CHECK(spec.lipHeight > 0.06f);
    CHECK(spec.lipOut > 0.05f);
}

TEST_CASE("lit share: rises up the building, capped, Legacy flat") {
    const WindowFacadeSpec spec;
    const float base = 0.35f;
    float last = 0.0f;
    for (std::uint32_t st = 0; st < 12; ++st) {
        const float l = windowLitShare(spec, WindowType::Apartment, base, spec.upperLit, st, 12u);
        CHECK(l >= last);
        CHECK(l <= spec.litMax);
        last = l;
    }
    CHECK(windowLitShare(spec, WindowType::Apartment, base, spec.upperLit, 1u, 12u) == doctest::Approx(base));
    CHECK(last == doctest::Approx(std::min(base * (1.0f + spec.upperLit), spec.litMax)));
    CHECK(last > base * 1.5f);
    CHECK(windowLitShare(spec, WindowType::Apartment, 0.8f, 2.0f, 11u, 12u) == doctest::Approx(spec.litMax));
    CHECK(windowLitShare(spec, WindowType::Legacy, base, spec.upperLit, 11u, 12u) == base);
    // Two storeys or one: no rise.
    CHECK(windowLitShare(spec, WindowType::Shophouse, base, spec.upperLit, 1u, 2u) == doctest::Approx(base));
}

TEST_CASE("HLSL: r6 styles, lit rise, sill shadow and the wall cap are wired") {
    const WindowFacadeSpec spec;
    const std::string win = windowFacadeHlsl(spec);
    for (const char* name : {"kStyE", "kFr", "UpperLit", "loggia", "grille", "railZ", "sillSh", "frostGlow", "litP",
                             "hs = "})
        CHECK(win.find(name) != std::string::npos);
    const std::string comp = windowFacadeComposeHlsl();
    for (const char* name : {"Shade", "Metal", "min(lerp(WallAlbedo"}) CHECK(comp.find(name) != std::string::npos);
}

TEST_CASE("HLSL: the shop front fragments are spliced once, r5's kind block is gone") {
    const std::string win = windowFacadeHlsl(WindowFacadeSpec{});
    const auto count = [&](const char* what) {
        std::size_t n = 0;
        for (std::size_t at = win.find(what); at != std::string::npos; at = win.find(what, at + 1)) ++n;
        return n;
    };
    for (const char* decl : {"uint kind =", "uint kh =", "float3 own =", "float sp =", "float3 fc =", "bool panel =",
                             "bool lantern =", "float3 signLight =", "float shopSill =", "struct FTShop"})
        CHECK_MESSAGE(count(decl) == 1u, decl);
    for (const char* gone : {"uint kL", "bool oddC", "bool awning", "float sH ="}) CHECK_MESSAGE(count(gone) == 0u, gone);
    CHECK(count("uint pick = shopPick;") == 1u);
    CHECK(count("px = shop ? shopShift + px * shopZoom : px;") == 1u);
    CHECK(count("!shop && (smallWin") == 1u);
    CHECK(win.find("struct FTShop") < win.find("float v0 = (shop ? shopSill"));
}
