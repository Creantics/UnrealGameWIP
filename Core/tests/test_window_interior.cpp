// Interior-mapped windows (ftcore/WindowInterior.h): the grid on a face, each window's look, where a view ray lands in
// the room picture and on the curtain, and the HLSL written from the same numbers.

#include "ftcore/WindowInterior.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>

using namespace ftcore;

namespace {
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
}  // namespace

TEST_CASE("windowHash is the fixed integer hash the GPU repeats") {
    CHECK(windowHash(0u) == 0u);
    CHECK(windowHash(1u) != windowHash(2u));
    // A pinned value: changing the hash changes every window in the city, so it must be on purpose.
    CHECK(windowHash(12345u) == 2435775735u);
    CHECK(windowHash(1u) == 1753845952u);
    std::set<std::uint32_t> seen;
    for (std::uint32_t i = 0; i < 1000; ++i) seen.insert(windowHash(i));
    CHECK(seen.size() == 1000u);
}

TEST_CASE("seeds keep to the 24 bits a float holds exactly") {
    for (std::uint32_t i = 0; i < 2000; ++i) {
        const std::uint32_t s = windowSeed(i * 7919u + 3u);
        CHECK(s <= kWindowSeedMask);
        CHECK(static_cast<std::uint32_t>(static_cast<float>(s)) == s);
    }
}

TEST_CASE("each building keeps one bay width inside its range") {
    const WindowGridSpec spec;
    std::set<float> widths;
    for (std::uint32_t seed = 0; seed < 200; ++seed) {
        const float bw = windowBayWidth(spec, seed);
        CHECK(bw >= spec.bayMin - 1e-5f);
        CHECK(bw <= spec.bayMax + 1e-5f);
        widths.insert(bw);
        CHECK(windowBayWidth(spec, seed) == bw);
    }
    CHECK(widths.size() == 7u);
}

TEST_CASE("bays are centred on a face and keep its edges clear") {
    const WindowGridSpec spec;
    const std::uint32_t seed = 77;
    const float bw = windowBayWidth(spec, seed);
    const float face = 20.0f;
    const auto n = static_cast<std::uint32_t>(std::floor((face - 2.0f * spec.edge) / bw));
    const float m0 = (face - static_cast<float>(n) * bw) * 0.5f;
    CHECK(m0 >= spec.edge - 1e-4f);
    CHECK_FALSE(windowCellAt(spec, seed, face, 12.0f, m0 - 0.01f, 1.5f, false).bay);
    CHECK_FALSE(windowCellAt(spec, seed, face, 12.0f, face - m0 + 0.01f, 1.5f, false).bay);
    const WindowCell first = windowCellAt(spec, seed, face, 12.0f, m0 + 0.01f, 1.5f, false);
    CHECK(first.bay);
    CHECK(first.column == 0u);
    const WindowCell last = windowCellAt(spec, seed, face, 12.0f, face - m0 - 0.01f, 1.5f, false);
    CHECK(last.bay);
    CHECK(last.column == n - 1u);
    // Symmetric: the middle of the face is the middle of the bay row.
    const WindowCell mid = windowCellAt(spec, seed, face, 12.0f, face * 0.5f, 1.5f, false);
    if (n % 2u == 1u) CHECK(near(mid.u, 0.5f));
    else CHECK((near(mid.u, 0.0f) || near(mid.u, 1.0f)));
}

TEST_CASE("a face too narrow for one bay has no windows") {
    const WindowGridSpec spec;
    CHECK_FALSE(windowCellAt(spec, 5u, spec.bayMin + 2.0f * spec.edge - 0.1f, 9.0f, 1.0f, 1.5f, false).bay);
}

TEST_CASE("storeys: whole ones only; a street face's ground storey is shops") {
    const WindowGridSpec spec;
    // 10 m face: storeys 0..2 whole, the 4th (9..12 m) rises past the top.
    const WindowCell s2 = windowCellAt(spec, 1u, 20.0f, 10.0f, 10.0f, 7.5f, true);
    CHECK(s2.bay);
    CHECK(s2.storey == 2u);
    CHECK_FALSE(s2.shop);
    CHECK(near(s2.v, 0.5f));
    CHECK_FALSE(windowCellAt(spec, 1u, 20.0f, 10.0f, 10.0f, 9.5f, true).bay);
    CHECK(windowCellAt(spec, 1u, 20.0f, 10.0f, 10.0f, 1.5f, true).shop);
    CHECK_FALSE(windowCellAt(spec, 1u, 20.0f, 10.0f, 10.0f, 1.5f, false).shop);
    CHECK_FALSE(windowCellAt(spec, 1u, 20.0f, 10.0f, 10.0f, 4.5f, true).shop);
}

TEST_CASE("a shopfront is wider and taller than a window and nearly meets the street") {
    const WindowGridSpec spec;
    const WindowOpening w = windowOpening(spec, false);
    const WindowOpening p = windowOpening(spec, true);
    CHECK(p.u1 - p.u0 > w.u1 - w.u0);
    CHECK(p.v1 - p.v0 > w.v1 - w.v0);
    CHECK(near(p.v0, spec.shopSill / spec.storey));
    CHECK(near(p.v1, spec.shopTop / spec.storey));
    CHECK(near(p.u0 + p.u1, 1.0f));
    // On a street face at 0.5 m up, the middle of a bay is shop glass; off the street it is wall (under the sill).
    const std::uint32_t seed = 9;
    const float bw = windowBayWidth(spec, seed);
    const float face = 4.0f * bw + 2.0f * spec.edge;
    const float m0 = (face - 4.0f * bw) * 0.5f;
    CHECK(windowCellAt(spec, seed, face, 6.0f, m0 + 1.5f * bw, 0.5f, true).part == WindowPart::Glass);
    CHECK(windowCellAt(spec, seed, face, 6.0f, m0 + 1.5f * bw, 0.5f, false).part == WindowPart::Wall);
}

TEST_CASE("the opening sits at the sill and the bay's middle") {
    const WindowGridSpec spec;
    const WindowOpening o = windowOpening(spec, false);
    CHECK(near(o.v0, spec.sill / spec.storey));
    CHECK(near(o.v1, (spec.sill + spec.opening) / spec.storey));
    CHECK(near(o.u0 + o.u1, 1.0f));
    CHECK(near(o.u1 - o.u0, spec.fill));
    const std::uint32_t seed = 9;
    const float bw = windowBayWidth(spec, seed);
    const float face = 4.0f * bw + 2.0f * spec.edge;
    const float m0 = (face - 4.0f * bw) * 0.5f;
    const float up = spec.storey + spec.sill;  // storey 1's sill
    // The middle of bay 1, 0.2 m over the sill: glass. Just under the sill: wall. Just over it: the frame.
    CHECK(windowCellAt(spec, seed, face, 6.0f, m0 + 1.5f * bw, up + 0.2f, false).part == WindowPart::Glass);
    CHECK(windowCellAt(spec, seed, face, 6.0f, m0 + 1.5f * bw, up - 0.02f, false).part == WindowPart::Wall);
    CHECK(windowCellAt(spec, seed, face, 6.0f, m0 + 1.5f * bw, up + 0.5f * spec.frame, false).part ==
          WindowPart::Frame);
    // Just inside the opening's left side: the frame; near the bay's edge: wall.
    CHECK(windowCellAt(spec, seed, face, 6.0f, m0 + bw * (1.0f + o.u0) + 0.5f * spec.frame, up + 0.5f, false).part ==
          WindowPart::Frame);
    CHECK(windowCellAt(spec, seed, face, 6.0f, m0 + 1.02f * bw, up + 0.5f, false).part == WindowPart::Wall);
}

TEST_CASE("window looks spread over rooms, tints and curtains in the asked shares") {
    std::array<int, kWindowRooms> rooms{};
    std::array<int, kWindowCurtains> curtains{};
    int lit = 0, curtained = 0, mirrored = 0;
    const int total = 4000;
    for (int i = 0; i < total; ++i) {
        const WindowLook w = windowLookFor(1234u + static_cast<std::uint32_t>(i / 40), static_cast<std::uint32_t>(i % 8),
                                           static_cast<std::uint32_t>((i / 8) % 5), false, 0.35f, 0.4f, 0.8f);
        REQUIRE(w.room < kWindowRooms);
        REQUIRE(w.tint < kWindowTints);
        ++rooms[w.room];
        lit += w.lit ? 1 : 0;
        mirrored += w.mirror ? 1 : 0;
        if (w.curtain >= 0) {
            REQUIRE(w.curtain < static_cast<int>(kWindowCurtains));
            ++curtains[static_cast<std::size_t>(w.curtain)];
            ++curtained;
            CHECK(w.drawn >= 0.25f);
            CHECK(w.drawn <= 1.0f);
        }
    }
    for (int r : rooms) CHECK(r > total / static_cast<int>(kWindowRooms) / 2);
    for (int c : curtains) CHECK(c > 0);
    CHECK(std::abs(lit - total * 35 / 100) < total / 20);
    CHECK(std::abs(curtained - total * 40 / 100) < total / 20);
    CHECK(std::abs(mirrored - total / 2) < total / 20);
    // Shares of 0 and 1.
    CHECK_FALSE(windowLookFor(5u, 1u, 1u, false, 0.0f, 0.0f, 1.0f).lit);
    CHECK(windowLookFor(5u, 1u, 1u, false, 1.0f, 1.0f, 0.0f).lit);
    CHECK(windowLookFor(5u, 1u, 1u, false, 1.0f, 1.0f, 0.0f).curtain >= 0);
    CHECK(windowLookFor(5u, 1u, 1u, false, 0.0f, 0.0f, 1.0f).curtain == -1);
}

TEST_CASE("shops take the shop pictures, no curtains, and their own lit share") {
    std::array<int, kWindowShops> shops{};
    int lit = 0;
    const int total = 2000;
    for (int i = 0; i < total; ++i) {
        const WindowLook w = windowLookFor(900u + static_cast<std::uint32_t>(i / 10), 0u,
                                           static_cast<std::uint32_t>(i % 10), true, 0.0f, 1.0f, 0.8f);
        REQUIRE(w.room >= kWindowRooms);
        REQUIRE(w.room < kWindowRooms + kWindowShops);
        ++shops[w.room - kWindowRooms];
        CHECK(w.curtain == -1);
        lit += w.lit ? 1 : 0;
    }
    for (int c : shops) CHECK(c > total / static_cast<int>(kWindowShops) / 2);
    CHECK(std::abs(lit - total * 80 / 100) < total / 20);
}

TEST_CASE("neighbouring windows differ") {
    int same = 0;
    for (std::uint32_t c = 0; c < 50; ++c)
        same += windowLookFor(42u, 3u, c, false, 0.35f, 0.4f, 0.8f).room ==
                        windowLookFor(42u, 3u, c + 1, false, 0.35f, 0.4f, 0.8f).room
                    ? 1
                    : 0;
    CHECK(same < 20);
}

TEST_CASE("a straight-on look meets the back wall, drawn at half size in the picture") {
    const WindowGridSpec spec;
    const WindowRoomHit c = windowRoomHit(spec, 3.0f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f);
    CHECK(c.face == 0);
    CHECK(near(c.x, 0.5f));
    CHECK(near(c.y, 0.5f));
    const WindowRoomHit q = windowRoomHit(spec, 3.0f, 0.3f, 0.7f, 0.0f, 0.0f, 1.0f);
    CHECK(q.face == 0);
    CHECK(near(q.x, 0.5f + (0.3f - 0.5f) * 0.5f));
    CHECK(near(q.y, 0.5f + (0.7f - 0.5f) * 0.5f));
}

TEST_CASE("looking down meets the floor, up the ceiling, sideways the side walls") {
    const WindowGridSpec spec;
    CHECK(windowRoomHit(spec, 3.0f, 0.5f, 0.4f, 0.0f, -1.0f, 1.0f).face == 1);
    CHECK(windowRoomHit(spec, 3.0f, 0.5f, 0.6f, 0.0f, 1.0f, 1.0f).face == 2);
    CHECK(windowRoomHit(spec, 3.0f, 0.4f, 0.5f, -1.0f, 0.0f, 1.0f).face == 3);
    CHECK(windowRoomHit(spec, 3.0f, 0.6f, 0.5f, 1.0f, 0.0f, 1.0f).face == 4);
    // The floor seen from above lands in the picture's lower band, between the back wall's foot and the front.
    const WindowRoomHit f = windowRoomHit(spec, 3.0f, 0.5f, 0.4f, 0.0f, -1.0f, 1.0f);
    CHECK(f.y >= 0.0f);
    CHECK(f.y < 0.25f + 1e-4f);
}

TEST_CASE("the picture point moves smoothly with the eye (no seams where faces meet)") {
    const WindowGridSpec spec;
    // Sweep the view from straight on to steeply down; the picture point must never jump.
    WindowRoomHit prev = windowRoomHit(spec, 3.0f, 0.5f, 0.45f, 0.0f, 0.0f, 1.0f);
    bool sawFloor = false;
    for (int i = 1; i <= 400; ++i) {
        const float down = -2.0f * static_cast<float>(i) / 400.0f;
        const WindowRoomHit h = windowRoomHit(spec, 3.0f, 0.5f, 0.45f, 0.0f, down, 1.0f);
        CHECK(std::fabs(h.y - prev.y) < 0.01f);
        CHECK(h.x >= 0.0f);
        CHECK(h.x <= 1.0f);
        CHECK(h.y >= 0.0f);
        CHECK(h.y <= 1.0f);
        sawFloor = sawFloor || h.face == 1;
        prev = h;
    }
    CHECK(sawFloor);
}

TEST_CASE("the curtain plane shifts a little with the eye and covers its drawn share from the top") {
    const WindowGridSpec spec;
    const WindowOpening o = windowOpening(spec, false);
    const float mu = 0.5f * (o.u0 + o.u1), mv = 0.5f * (o.v0 + o.v1);
    const WindowCurtainHit straight = windowCurtainHit(spec, 3.0f, mu, mv, 0.0f, 0.0f, 1.0f, 0.5f);
    CHECK(near(straight.x, 0.5f));
    CHECK(near(straight.y, 0.5f));
    CHECK(straight.covered);  // drawn halfway: the middle line is its lower edge
    CHECK_FALSE(windowCurtainHit(spec, 3.0f, mu, o.v0 + 0.01f, 0.0f, 0.0f, 1.0f, 0.5f).covered);
    CHECK(windowCurtainHit(spec, 3.0f, mu, o.v0 + 0.01f, 0.0f, 0.0f, 1.0f, 1.0f).covered);
    // Looking from the right, the plane 0.1 m in shifts left by 0.1 m across the bay.
    const WindowCurtainHit side = windowCurtainHit(spec, 3.0f, mu, mv, -1.0f, 0.0f, 1.0f, 0.5f);
    CHECK(near(side.x, 0.5f - (spec.curtainDepth / 3.0f) / (o.u1 - o.u0), 1e-3f));
}

TEST_CASE("the HLSL carries the grid's numbers and the hash") {
    const WindowGridSpec spec;
    const std::string h = windowInteriorHlsl(spec);
    CHECK(h.find("0x7feb352du") != std::string::npos);
    CHECK(h.find("0x846ca68bu") != std::string::npos);
    CHECK(h.find(std::to_string(0x9e3779b9u) + "u") != std::string::npos);
    CHECK(h.find("float storeyH = 3.0;") != std::string::npos);
    CHECK(h.find("(uint)max(Seed, 0.0)") != std::string::npos);
    CHECK(h.find("asuint") == std::string::npos);
    CHECK(h.find("Street > 0.5") != std::string::npos);
    CHECK(h.find("% 8u") != std::string::npos);
    CHECK(h.find("8u + hh % 4u") != std::string::npos);
    CHECK(h.find(") / 3.0);") != std::string::npos);  // three atlas rows
    CHECK(h.find("% 6u") != std::string::npos);
    CHECK(h.find("Texture2DSample(Rooms, RoomsSampler") != std::string::npos);
    CHECK(h.find("Texture2DSample(Curtains, CurtainsSampler") != std::string::npos);
    CHECK(h.find("Glow =") != std::string::npos);
    CHECK(h.find("return float4(c, inGlass ? 1.0 : (inOpen ? 2.0 : 0.0));") != std::string::npos);
    // No early return: past the hash's own, the only return is the last line, after both texture reads.
    const std::size_t last = h.find("return float4");
    CHECK(h.find("return", h.find("return x;") + 1) == last);
    CHECK(h.rfind("return") == last);
    CHECK(h.find("Texture2DSample(Curtains") < last);
    // Every literal it writes is a float or an unsigned literal, never a bare int that HLSL would divide as int.
    CHECK(h.find("/ 4;") == std::string::npos);
}
