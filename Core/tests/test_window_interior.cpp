// Interior-mapped windows (ftcore/WindowInterior.h): the grid a face's size cuts, each window's look, where a view ray
// lands in the room picture and on the curtain, and the HLSL written from the same numbers.

#include "ftcore/WindowInterior.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>

using namespace ftcore;

namespace {
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// A repeatable number in [lo, hi) for sample i of stream k.
float rnd(std::uint32_t k, std::uint32_t i, float lo, float hi) {
    const std::uint32_t x = windowHash(windowHash(k * 0x9e3779b9u + 17u) ^ (i * 0x85ebca6bu + 3u));
    return lo + (hi - lo) * static_cast<float>(x >> 8) / 16777216.0f;
}

constexpr float kWidths[] = {1.2f, 1.35f, 1.5f};

// Blank cells: a face's nominal grid is one cell per 3 m storey and 3 m bay; the cells it cuts no window into are blank.
struct Blank {
    double nominal = 0.0, blank = 0.0;
    double share() const { return nominal > 0.0 ? blank / nominal : 0.0; }
};
void addFace(Blank& b, const WindowGridSpec& spec, float w, float h, float ww) {
    const double nominal = std::max(1.0, std::floor(h / 3.0 + 0.5)) * std::max(1.0, std::floor(w / 3.0 + 0.5));
    const WindowFaceGrid g = windowFaceGrid(spec, w, h, ww);
    b.nominal += nominal;
    b.blank += std::max(0.0, nominal - static_cast<double>(g.storeys) * static_cast<double>(g.bays));
}

// Identical neighbours (room, mirror, curtain and lit all the same) in a 5 x 5 block of upper-floor windows.
struct Pairs {
    int straight = 0, diagonal = 0;
};
template <class RoomOf>
Pairs fiveByFive(const WindowGridSpec& spec, std::uint32_t seed, RoomOf roomOf) {
    WindowLook g[5][5];
    for (std::uint32_t r = 0; r < 5; ++r)
        for (std::uint32_t c = 0; c < 5; ++c) {
            g[r][c] = windowLookFor(spec, seed, r + 1u, c, false, 0.35f, 0.35f, 0.8f);
            g[r][c].room = roomOf(seed, r + 1u, c);
        }
    const auto same = [](const WindowLook& a, const WindowLook& b) {
        return a.room == b.room && a.mirror == b.mirror && a.curtain == b.curtain && a.lit == b.lit;
    };
    Pairs p;
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 5; ++c) {
            if (c + 1 < 5 && same(g[r][c], g[r][c + 1])) ++p.straight;
            if (r + 1 < 5 && same(g[r][c], g[r + 1][c])) ++p.straight;
            if (r + 1 < 5 && c + 1 < 5 && same(g[r][c], g[r + 1][c + 1])) ++p.diagonal;
            if (r + 1 < 5 && c >= 1 && same(g[r][c], g[r + 1][c - 1])) ++p.diagonal;
        }
    return p;
}
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

TEST_CASE("a face's size cuts its grid: storeys near 3 m, bays near 2.5 m, room for every window") {
    const WindowGridSpec spec;
    // The spec's own window fits the lowest storey.
    CHECK(spec.sill + spec.opening + spec.frame <= spec.storeyMin);
    // An 8.3 m low-rise front 6 m wide: three storeys of 2.77 m, two bays of 2.65 m.
    const WindowFaceGrid g = windowFaceGrid(spec, 6.0f, 8.3f, 1.35f);
    CHECK(g.storeys == 3u);
    CHECK(near(g.storeyHeight, 8.3f / 3.0f));
    CHECK(g.bays == 2u);
    CHECK(near(g.bayWidth, (6.0f - 2.0f * spec.edge) / 2.0f));
    // A 30 m tower face 27 m wide: ten storeys of 3 m, eleven bays of 2.39 m.
    const WindowFaceGrid t = windowFaceGrid(spec, 27.0f, 30.0f, 1.35f);
    CHECK(t.storeys == 10u);
    CHECK(near(t.storeyHeight, 3.0f));
    CHECK(t.bays == 11u);
    // Lower than the least storey, or too narrow for a window and its piers: nothing.
    CHECK(windowFaceGrid(spec, 10.0f, spec.storeyMin - 0.05f, 1.35f).storeys == 0u);
    CHECK(windowFaceGrid(spec, 1.35f + 2.0f * spec.pier + 2.0f * spec.edge - 0.05f, 9.0f, 1.35f).bays == 0u);
    CHECK(windowFaceGrid(spec, 1.35f + 2.0f * spec.pier + 2.0f * spec.edge + 0.05f, 9.0f, 1.35f).bays == 1u);
    // Over many faces: storeys within their range, bays wide enough for the window and its piers, and the grid fills
    // the face between its edges and up to its last storey (a low face may keep a parapet).
    for (std::uint32_t i = 0; i < 20000; ++i) {
        const float w = rnd(1, i, 2.0f, 60.0f), h = rnd(2, i, 2.0f, 60.0f), ww = kWidths[i % 3];
        const WindowFaceGrid f = windowFaceGrid(spec, w, h, ww);
        if (f.storeys == 0u) {
            CHECK(h < spec.storeyMin);
            continue;
        }
        CHECK(f.storeyHeight >= spec.storeyMin - 1e-4f);
        CHECK(f.storeyHeight <= spec.storeyMax + 1e-4f);
        CHECK(static_cast<float>(f.storeys) * f.storeyHeight <= h + 1e-3f);
        if (h >= 2.0f * spec.storeyMin) CHECK(near(static_cast<float>(f.storeys) * f.storeyHeight, h, 1e-3f));
        if (f.bays > 0u) {
            CHECK(f.bayWidth >= ww + 2.0f * spec.pier - 1e-4f);
            CHECK(near(static_cast<float>(f.bays) * f.bayWidth, w - 2.0f * spec.edge, 1e-3f));
        } else {
            CHECK(w - 2.0f * spec.edge < ww + 2.0f * spec.pier);
        }
    }
}

TEST_CASE("every storey x bay cell gets a window, and blank cells stay under 15 percent") {
    const WindowGridSpec spec;
    // Each cell of a grid has glass at its window's middle (or its shopfront's, on a street face's ground storey).
    for (std::uint32_t i = 0; i < 400; ++i) {
        const float w = rnd(3, i, 3.0f, 30.0f), h = rnd(4, i, 3.0f, 40.0f), ww = kWidths[i % 3];
        const bool street = (i & 1u) != 0u;
        const WindowFaceGrid g = windowFaceGrid(spec, w, h, ww);
        for (std::uint32_t s = 0; s < g.storeys; ++s)
            for (std::uint32_t b = 0; b < g.bays; ++b) {
                const bool shop = street && s == 0u;
                const WindowOpening o = windowOpening(spec, shop, g.bayWidth, g.storeyHeight, ww);
                const float along = spec.edge + (static_cast<float>(b) + 0.5f * (o.u0 + o.u1)) * g.bayWidth;
                const float up = (static_cast<float>(s) + 0.5f * (o.v0 + o.v1)) * g.storeyHeight;
                const WindowCell c = windowCellAt(spec, w, h, along, up, street, ww);
                REQUIRE(c.bay);
                CHECK(c.part == WindowPart::Glass);
                CHECK(c.storey == s);
                CHECK(c.column == b);
                CHECK(c.shop == shop);
            }
    }
    // The share of a face's nominal 3 x 3 m cells that get no window, over all faces and over the low-rise fronts.
    Blank all, low;
    for (std::uint32_t i = 0; i < 20000; ++i) {
        addFace(all, spec, rnd(5, i, 3.0f, 40.0f), rnd(6, i, 3.0f, 45.0f), kWidths[i % 3]);
        addFace(low, spec, rnd(7, i, 4.0f, 9.0f), rnd(8, i, 6.0f, 12.0f), kWidths[i % 3]);
    }
    MESSAGE("blank cells: all faces " << all.share() * 100.0 << " %, low-rise " << low.share() * 100.0 << " %");
    CHECK(all.share() <= 0.15);
    CHECK(low.share() <= 0.15);
}

TEST_CASE("one window size per building: the width is the building's, whatever the face") {
    const WindowGridSpec spec;
    for (const float ww : kWidths) {
        std::set<int> widthsMm, heightsMm;
        for (std::uint32_t i = 0; i < 5000; ++i) {
            const float w = rnd(9, i, 3.0f, 40.0f), h = rnd(10, i, 3.0f, 40.0f);
            const WindowFaceGrid g = windowFaceGrid(spec, w, h, ww);
            if (g.storeys == 0u || g.bays == 0u) continue;
            const WindowOpening o = windowOpening(spec, false, g.bayWidth, g.storeyHeight, ww);
            widthsMm.insert(static_cast<int>(std::lround((o.u1 - o.u0) * g.bayWidth * 1000.0f)));
            heightsMm.insert(static_cast<int>(std::lround((o.v1 - o.v0) * g.storeyHeight * 1000.0f)));
        }
        CHECK(widthsMm.size() == 1u);
        CHECK(*widthsMm.begin() == static_cast<int>(std::lround(ww * 1000.0f)));
        CHECK(heightsMm.size() == 1u);
        CHECK(*heightsMm.begin() == static_cast<int>(std::lround(spec.opening * 1000.0f)));
    }
}

TEST_CASE("a street face's ground storey is a darker band with shopfronts under a fascia") {
    const WindowGridSpec spec;
    const float w = 12.0f, h = 9.0f, ww = 1.35f;
    const WindowFaceGrid g = windowFaceGrid(spec, w, h, ww);
    REQUIRE(g.storeys == 3u);
    REQUIRE(g.bays >= 2u);
    const float sh = g.storeyHeight, bw = g.bayWidth;
    const float top = std::min(spec.shopTop, sh - spec.fascia);
    // Between two shopfronts: band wall. Over them, at either end of the face too: fascia. Inside one: glass.
    CHECK(windowCellAt(spec, w, h, spec.edge + bw, 1.2f, true, ww).part == WindowPart::Band);
    CHECK(windowCellAt(spec, w, h, 0.1f, 1.2f, true, ww).part == WindowPart::Band);
    CHECK(windowCellAt(spec, w, h, 0.1f, top + 0.5f * spec.fascia, true, ww).part == WindowPart::Fascia);
    CHECK(windowCellAt(spec, w, h, w * 0.5f + 0.37f, top + 0.5f * spec.fascia, true, ww).part == WindowPart::Fascia);
    CHECK(windowCellAt(spec, w, h, spec.edge + 0.5f * bw, 1.2f, true, ww).part == WindowPart::Glass);
    CHECK(windowCellAt(spec, w, h, spec.edge + 0.5f * bw, 1.2f, true, ww).shop);
    CHECK(windowCellAt(spec, w, h, spec.edge + 0.5f * bw, 0.3f, true, ww).part == WindowPart::Glass);
    // The shopfront nearly meets the street and fills most of its bay.
    const WindowOpening p = windowOpening(spec, true, bw, sh, ww);
    CHECK(near(p.v0 * sh, spec.shopSill));
    CHECK(near(p.v1 * sh, top));
    CHECK(near(p.u1 - p.u0, spec.shopFill));
    // Off the street the same points are plain wall or an upper-floor window, and the storey over the band is plain.
    CHECK(windowCellAt(spec, w, h, spec.edge + bw, 1.2f, false, ww).part == WindowPart::Wall);
    CHECK(windowCellAt(spec, w, h, 0.1f, top + 0.5f * spec.fascia, false, ww).part == WindowPart::Wall);
    CHECK_FALSE(windowCellAt(spec, w, h, spec.edge + 0.5f * bw, 1.2f, false, ww).shop);
    CHECK(windowCellAt(spec, w, h, spec.edge + bw, sh + 0.3f, true, ww).part == WindowPart::Wall);
    CHECK_FALSE(windowCellAt(spec, w, h, spec.edge + bw, sh + 0.3f, true, ww).band);
    // A street face too narrow for a shopfront still has its band and fascia.
    CHECK(windowCellAt(spec, 2.0f, h, 1.0f, 1.0f, true, ww).part == WindowPart::Band);
    CHECK(windowCellAt(spec, 2.0f, h, 1.0f, top + 0.1f, true, ww).part == WindowPart::Fascia);
    // Over the last storey (a parapet) nothing.
    const WindowCell over = windowCellAt(spec, w, 4.6f, spec.edge + 0.5f * bw, 4.3f, false, ww);
    CHECK_FALSE(over.bay);
    CHECK(over.part == WindowPart::Wall);
}

TEST_CASE("a window sits at the sill in its bay's middle, inside its frame") {
    const WindowGridSpec spec;
    const float w = 12.0f, h = 9.0f, ww = 1.35f;
    const WindowFaceGrid g = windowFaceGrid(spec, w, h, ww);
    const float sh = g.storeyHeight, bw = g.bayWidth;
    const WindowOpening o = windowOpening(spec, false, bw, sh, ww);
    CHECK(near(o.u0 + o.u1, 1.0f));
    CHECK(near((o.u1 - o.u0) * bw, ww));
    CHECK(near(o.v0 * sh, spec.sill));
    CHECK(near(o.v1 * sh, spec.sill + spec.opening));
    const float mid = spec.edge + 1.5f * bw;
    const float sill = sh + spec.sill;  // storey 1's sill
    CHECK(windowCellAt(spec, w, h, mid, sill + 0.2f, false, ww).part == WindowPart::Glass);
    CHECK(windowCellAt(spec, w, h, mid, sill - 0.02f, false, ww).part == WindowPart::Wall);
    CHECK(windowCellAt(spec, w, h, mid, sill + 0.5f * spec.frame, false, ww).part == WindowPart::Frame);
    CHECK(windowCellAt(spec, w, h, spec.edge + bw * (1.0f + o.u0) + 0.5f * spec.frame, sill + 0.5f, false, ww).part ==
          WindowPart::Frame);
    CHECK(windowCellAt(spec, w, h, spec.edge + 1.02f * bw, sill + 0.5f, false, ww).part == WindowPart::Wall);
    CHECK_FALSE(windowCellAt(spec, w, h, spec.edge - 0.01f, sill + 0.5f, false, ww).bay);
    CHECK_FALSE(windowCellAt(spec, w, h, w - spec.edge + 0.01f, sill + 0.5f, false, ww).bay);
    CHECK(windowCellAt(spec, w, h, w - spec.edge - 0.01f, sill + 0.5f, false, ww).column == g.bays - 1u);
}

TEST_CASE("window looks spread over rooms, tints, mirrors and lights in the asked shares") {
    const WindowGridSpec spec;
    std::array<int, kWindowRooms> rooms{};
    std::array<int, kWindowCurtains> curtains{};
    std::array<int, kWindowTints> tints{};
    int lit = 0, curtained = 0, mirrored = 0;
    const int total = 20000;
    for (int i = 0; i < total; ++i) {
        const WindowLook w = windowLookFor(spec, 1234u + static_cast<std::uint32_t>(i / 40),
                                           1u + static_cast<std::uint32_t>(i % 8),
                                           static_cast<std::uint32_t>((i / 8) % 5), false, 0.35f, 0.35f, 0.8f);
        REQUIRE(w.room < kWindowRooms);
        REQUIRE(w.tint < kWindowTints);
        ++rooms[w.room];
        ++tints[w.tint];
        lit += w.lit ? 1 : 0;
        mirrored += w.mirror ? 1 : 0;
        if (w.curtain >= 0) {
            REQUIRE(w.curtain < static_cast<int>(kWindowCurtains));
            ++curtains[static_cast<std::size_t>(w.curtain)];
            ++curtained;
        }
    }
    for (int r : rooms) CHECK(r > total / static_cast<int>(kWindowRooms) * 3 / 4);
    for (int t : tints) CHECK(t > total / static_cast<int>(kWindowTints) * 3 / 4);
    for (int c : curtains) CHECK(c > curtained / static_cast<int>(kWindowCurtains) * 3 / 4);
    CHECK(std::abs(lit - total * 35 / 100) < total / 50);
    CHECK(std::abs(curtained - total * 35 / 100) < total / 50);
    CHECK(std::abs(mirrored - total / 2) < total / 50);
    // Shares of 0 and 1.
    CHECK_FALSE(windowLookFor(spec, 5u, 1u, 1u, false, 0.0f, 0.0f, 1.0f).lit);
    CHECK(windowLookFor(spec, 5u, 1u, 1u, false, 1.0f, 1.0f, 0.0f).lit);
    CHECK(windowLookFor(spec, 5u, 1u, 1u, false, 1.0f, 1.0f, 0.0f).curtain >= 0);
    CHECK(windowLookFor(spec, 5u, 1u, 1u, false, 0.0f, 0.0f, 1.0f).curtain == -1);
}

TEST_CASE("curtains: at most 15 percent fully drawn, mean draw at most 40 percent") {
    const WindowGridSpec spec;
    int curtained = 0, full = 0, windows = 0;
    double drawSum = 0.0;
    for (std::uint32_t seed = 0; seed < 1000; ++seed)
        for (std::uint32_t s = 1; s <= 6; ++s)
            for (std::uint32_t c = 0; c < 5; ++c) {
                const WindowLook w = windowLookFor(spec, seed * 977u + 11u, s, c, false, 0.35f, 0.35f, 0.8f);
                ++windows;
                if (w.curtain < 0) continue;
                ++curtained;
                drawSum += w.drawn;
                if (w.drawn >= 0.95f) {
                    ++full;
                    CHECK(w.drawn == 1.0f);
                } else {
                    CHECK(w.drawn >= spec.drawMin);
                    CHECK(w.drawn <= spec.drawMin + spec.drawSpan + 1e-5f);
                }
            }
    REQUIRE(curtained > 0);
    const double fullShare = static_cast<double>(full) / curtained;
    const double meanCurtained = drawSum / curtained;
    const double meanAll = drawSum / windows;
    MESSAGE(windows << " windows, " << curtained << " curtained: fully drawn " << fullShare * 100.0
                    << " % of curtained (" << 100.0 * full / windows << " % of all), mean draw " << meanCurtained
                    << " curtained, " << meanAll << " over all windows");
    CHECK(fullShare <= 0.15);
    CHECK(meanCurtained <= 0.40);
    CHECK(meanAll <= 0.40);
}

TEST_CASE("shops take the shop pictures, no curtains, and their own lit share") {
    const WindowGridSpec spec;
    std::array<int, kWindowShops> shops{};
    int lit = 0;
    const int total = 4000;
    for (int i = 0; i < total; ++i) {
        const WindowLook w = windowLookFor(spec, 900u + static_cast<std::uint32_t>(i / 10), 0u,
                                           static_cast<std::uint32_t>(i % 10), true, 0.0f, 1.0f, 0.8f);
        REQUIRE(w.room >= kWindowRooms);
        REQUIRE(w.room < kWindowRooms + kWindowShops);
        ++shops[w.room - kWindowRooms];
        CHECK(w.curtain == -1);
        lit += w.lit ? 1 : 0;
    }
    for (int c : shops) CHECK(c > total / static_cast<int>(kWindowShops) * 3 / 4);
    CHECK(std::abs(lit - total * 80 / 100) < total / 40);
}

TEST_CASE("no window shows the same room as the one beside, above or below it") {
    int clashes = 0, pairs = 0;
    for (std::uint32_t seed = 0; seed < 2000; ++seed) {
        const std::uint32_t sd = windowSeed(seed);
        const bool street = (seed & 1u) != 0u;
        for (std::uint32_t s = 0; s < 12; ++s)
            for (std::uint32_t c = 0; c < 12; ++c) {
                const std::uint32_t r = windowRoomFor(sd, s, c, street && s == 0u);
                if (c + 1 < 12) {
                    ++pairs;
                    clashes += r == windowRoomFor(sd, s, c + 1u, street && s == 0u) ? 1 : 0;
                }
                if (s + 1 < 12) {
                    ++pairs;
                    clashes += r == windowRoomFor(sd, s + 1u, c, false) ? 1 : 0;
                }
            }
    }
    CHECK(pairs > 500000);
    CHECK(clashes == 0);
}

TEST_CASE("5 x 5 blocks: identical neighbours (room, mirror, curtain, lit), mean and worst over many seeds") {
    const WindowGridSpec spec;
    const int seeds = 20000;
    double rawSum = 0.0, newSum = 0.0, newDiagSum = 0.0;
    int rawWorst = 0, newWorst = 0, newStraight = 0;
    for (int i = 0; i < seeds; ++i) {
        const std::uint32_t sd = windowSeed(static_cast<std::uint32_t>(i) * 2654435761u + 7u);
        const Pairs raw = fiveByFive(spec, sd, [](std::uint32_t s, std::uint32_t st, std::uint32_t c) {
            return windowRawRoom(s, st, c, false);
        });
        const Pairs now = fiveByFive(spec, sd, [](std::uint32_t s, std::uint32_t st, std::uint32_t c) {
            return windowRoomFor(s, st, c, false);
        });
        rawSum += raw.straight + raw.diagonal;
        newSum += now.straight + now.diagonal;
        newDiagSum += now.diagonal;
        newStraight += now.straight;
        rawWorst = std::max(rawWorst, raw.straight + raw.diagonal);
        newWorst = std::max(newWorst, now.straight + now.diagonal);
    }
    MESSAGE("5x5 identical neighbours of 72 pairs, over " << seeds << " seeds: hash alone mean " << rawSum / seeds
                                                          << " worst " << rawWorst << "; with the neighbour rule mean "
                                                          << newSum / seeds << " worst " << newWorst
                                                          << " (all diagonal: " << newDiagSum / seeds << ")");
    CHECK(newStraight == 0);
    CHECK(newSum < rawSum * 0.8);
    CHECK(newSum / seeds < 0.6);
    CHECK(newWorst <= 8);
}

TEST_CASE("straight on, the eye looks a little down onto the back wall's furnished middle") {
    const WindowGridSpec spec;
    const WindowRoomHit c = windowRoomHit(spec, 2.5f, 3.0f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f);
    CHECK(c.face == 0);
    CHECK(near(c.x, 0.5f));
    CHECK(near(c.y, 0.5f + (0.5f + spec.viewBiasY - 0.5f) * 0.5f));
    // Under a quarter of the storey the tilted ray meets the floor first.
    CHECK(windowRoomHit(spec, 2.5f, 3.0f, 0.5f, 0.2f, 0.0f, 0.0f, 1.0f).face == 1);
}

TEST_CASE("the eased ray never meets the ceiling and keeps the side walls to the edges") {
    const WindowGridSpec spec;
    for (std::uint32_t i = 0; i < 20000; ++i) {
        const float u = rnd(11, i, 0.0f, 1.0f), v = rnd(12, i, 0.0f, 1.0f);
        const float da = rnd(13, i, -50.0f, 50.0f), du = rnd(14, i, -50.0f, 50.0f), di = rnd(15, i, 0.0f, 10.0f);
        const WindowRoomHit h = windowRoomHit(spec, rnd(16, i, 1.85f, 3.8f), rnd(17, i, 2.4f, 3.8f), u, v, da, du, di);
        CHECK(h.face != 2);
        CHECK(h.x >= -1e-5f);
        CHECK(h.x <= 1.0f + 1e-5f);
        CHECK(h.y >= -1e-5f);
        CHECK(h.y <= 1.0f + 1e-5f);
    }
    CHECK(windowRoomHit(spec, 2.5f, 3.0f, 0.5f, 0.6f, 0.0f, 1.0f, 1.0f).face == 0);
    CHECK(windowRoomHit(spec, 2.5f, 3.0f, 0.5f, 0.4f, 0.0f, -1.0f, 1.0f).face == 1);
    CHECK(windowRoomHit(spec, 2.5f, 3.0f, 0.2f, 0.5f, -5.0f, 0.0f, 1.0f).face == 3);
    CHECK(windowRoomHit(spec, 2.5f, 3.0f, 0.8f, 0.5f, 5.0f, 0.0f, 1.0f).face == 4);
}

TEST_CASE("street eyes see the furnished band of the room, never its ceiling") {
    // An eye 1.7 m over the street, 5 to 30 m out and up to 20 m to the side, looking at a window on storeys 1 to 10.
    const WindowGridSpec spec;
    const float w = 27.0f, h = 33.0f, ww = 1.35f;
    const WindowFaceGrid g = windowFaceGrid(spec, w, h, ww);
    REQUIRE(g.storeys == 11u);
    int samples = 0, ceiling = 0, band = 0, back = 0;
    for (std::uint32_t i = 0; i < 50000; ++i) {
        const float out = rnd(20, i, 5.0f, 30.0f), side = rnd(21, i, -20.0f, 20.0f);
        const auto storey = 1u + static_cast<std::uint32_t>(rnd(22, i, 0.0f, 10.0f));
        const auto bay = static_cast<std::uint32_t>(rnd(23, i, 0.0f, static_cast<float>(g.bays)));
        const WindowOpening o = windowOpening(spec, false, g.bayWidth, g.storeyHeight, ww);
        const float u = rnd(24, i, o.u0, o.u1), v = rnd(25, i, o.v0, o.v1);
        const float along = spec.edge + (static_cast<float>(bay) + u) * g.bayWidth;
        const float up = (static_cast<float>(storey) + v) * g.storeyHeight;
        const float eyeAlong = std::clamp(along + side, 0.0f, w);
        const WindowRoomHit r =
            windowRoomHit(spec, g.bayWidth, g.storeyHeight, u, v, along - eyeAlong, up - 1.7f, out);
        ++samples;
        ceiling += r.face == 2 ? 1 : 0;
        back += r.face == 0 ? 1 : 0;
        band += r.y >= 0.2f && r.y <= 0.7f ? 1 : 0;
    }
    MESSAGE("street eyes: " << samples << " samples, ceiling " << ceiling << ", picture y in 0.2..0.7 "
                            << 100.0 * band / samples << " %, back wall " << 100.0 * back / samples << " %");
    CHECK(ceiling == 0);
    CHECK(band >= samples * 9 / 10);
}

TEST_CASE("the picture point moves smoothly with the eye (no seams where faces meet)") {
    const WindowGridSpec spec;
    for (const float v : {0.3f, 0.45f, 0.7f}) {
        WindowRoomHit prev = windowRoomHit(spec, 2.5f, 3.0f, 0.5f, v, 0.0f, 3.0f, 1.0f);
        for (int i = 1; i <= 600; ++i) {
            const float up = 3.0f - 6.0f * static_cast<float>(i) / 600.0f;
            const WindowRoomHit h = windowRoomHit(spec, 2.5f, 3.0f, 0.5f, v, 0.0f, up, 1.0f);
            CHECK(std::fabs(h.y - prev.y) < 0.01f);
            prev = h;
        }
    }
    WindowRoomHit prev = windowRoomHit(spec, 2.5f, 3.0f, 0.7f, 0.5f, -3.0f, 0.0f, 1.0f);
    for (int i = 1; i <= 600; ++i) {
        const float across = -3.0f + 6.0f * static_cast<float>(i) / 600.0f;
        const WindowRoomHit h = windowRoomHit(spec, 2.5f, 3.0f, 0.7f, 0.5f, across, 0.0f, 1.0f);
        CHECK(std::fabs(h.x - prev.x) < 0.01f);
        prev = h;
    }
}

TEST_CASE("the curtain plane shifts a little with the eye and covers its drawn share from the top") {
    const WindowGridSpec spec;
    const float bw = 2.5f, sh = 3.0f, ww = 1.35f;
    const WindowOpening o = windowOpening(spec, false, bw, sh, ww);
    const float mu = 0.5f * (o.u0 + o.u1), mv = 0.5f * (o.v0 + o.v1);
    const float tilt = (spec.curtainDepth / spec.roomDepth) * spec.viewBiasY / (o.v1 - o.v0);
    const WindowCurtainHit straight = windowCurtainHit(spec, bw, sh, ww, mu, mv, 0.0f, 0.0f, 1.0f, 0.5f);
    CHECK(near(straight.x, 0.5f));
    CHECK(near(straight.y, 0.5f + tilt));
    CHECK_FALSE(windowCurtainHit(spec, bw, sh, ww, mu, o.v0 + 0.05f, 0.0f, 0.0f, 1.0f, 0.5f).covered);
    CHECK(windowCurtainHit(spec, bw, sh, ww, mu, o.v1 - 0.05f, 0.0f, 0.0f, 1.0f, 0.5f).covered);
    CHECK(windowCurtainHit(spec, bw, sh, ww, mu, o.v0 + 0.05f, 0.0f, 0.0f, 1.0f, 1.0f).covered);
    // Looking from the right, the plane 0.1 m in shifts left by the eased ray's share of 0.1 m.
    const WindowCurtainHit side = windowCurtainHit(spec, bw, sh, ww, mu, mv, -0.5f, 0.0f, 1.0f, 0.5f);
    const float rx = std::clamp(-0.5f * (spec.roomDepth / bw) * spec.viewSquashX, -spec.viewMaxX, spec.viewMaxX);
    CHECK(near(side.x, 0.5f + (spec.curtainDepth / spec.roomDepth) * rx / (o.u1 - o.u0), 1e-3f));
}

TEST_CASE("the HLSL carries the grid's numbers, the salts and the hash") {
    const WindowGridSpec spec;
    const std::string h = windowInteriorHlsl(spec);
    CHECK(h.find("0x7feb352du") != std::string::npos);
    CHECK(h.find("0x846ca68bu") != std::string::npos);
    for (const std::uint32_t salt : {0x68bc21ebu, 0x02e5be93u, 0x967a889bu, 0x5bd1e995u, 0x1b873593u, 0xcc9e2d51u,
                                     0x85ebca6bu})
        CHECK(h.find(std::to_string(salt) + "u") != std::string::npos);
    CHECK(h.find("FaceH / 3.0 + 0.5") != std::string::npos);
    CHECK(h.find("usable / 2.5 + 0.5") != std::string::npos);
    CHECK(h.find("(uint)max(Seed, 0.0)") != std::string::npos);
    CHECK(h.find("max(WinW, ") != std::string::npos);
    CHECK(h.find("asuint") == std::string::npos);
    CHECK(h.find("Street > 0.5") != std::string::npos);
    CHECK(h.find("8u + r % 4u : r % 8u") != std::string::npos);
    CHECK(h.find(") / 3.0);") != std::string::npos);  // three atlas rows
    CHECK(h.find("% 6u") != std::string::npos);
    CHECK(h.find("(fascia ? 4.0 : (band ? 3.0 : 0.0))") != std::string::npos);
    // The neighbour rule steps kWindowAvoidSteps times.
    std::size_t steps = 0;
    for (std::size_t at = h.find("W.Next(room, shop)"); at != std::string::npos; at = h.find("W.Next(room, shop)", at + 1))
        ++steps;
    CHECK(steps == kWindowAvoidSteps);
    // Every window reads its room; a curtain hides at most CurtainAlpha of it and lights only with a lit room, at night.
    CHECK(h.find("Texture2DSample(Rooms, RoomsSampler") != std::string::npos);
    CHECK(h.find("Texture2DSample(Curtains, CurtainsSampler") != std::string::npos);
    CHECK(h.find("float ca = covered ? cc.a * CurtainAlpha : 0.0;") != std::string::npos);
    CHECK(h.find("float3 seen = c * (1.0 - ca);") != std::string::npos);
    CHECK(h.find("Curtain = float4(cc.rgb, ca);") != std::string::npos);
    CHECK(h.find("float on = (inGlass && lit) ? Night : 0.0;") != std::string::npos);
    CHECK(h.find("(1.0 - Night)") != std::string::npos);
    CHECK(h.find("on * (seen + cc.rgb * ca * CurtainGlow)") != std::string::npos);
    CHECK(h.find("Emit = inGlass ?") != std::string::npos);
    CHECK(h.find("return float4(seen, part);") != std::string::npos);
    // No early return: past the helper struct, the only return is the last line, after both texture reads.
    const std::size_t body = h.find("FTWin W;");
    REQUIRE(body != std::string::npos);
    const std::size_t last = h.find("return float4");
    CHECK(h.find("return", body) == last);
    CHECK(h.rfind("return") == last);
    CHECK(h.find("Texture2DSample(Curtains") < last);
    // Every literal it writes is a float or an unsigned literal, never a bare int that HLSL would divide as int.
    CHECK(h.find("/ 4;") == std::string::npos);
    CHECK(h.find("/ 6;") == std::string::npos);
}

namespace {
std::uint64_t fnv64(const std::string& s) {
    std::uint64_t h = 14695981039346656037ull;
    for (const char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}
}  // namespace

TEST_CASE("r4 reproducible: the default spec writes the r4 HLSL byte for byte") {
    const std::string h = windowInteriorHlsl(WindowGridSpec{});
    CHECK(h.size() == 4592u);
    CHECK(fnv64(h) == 778640904970305523ull);
}
