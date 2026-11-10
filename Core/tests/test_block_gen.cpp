// The block generator (ftcore/BlockGen.h).

#include "ftcore/BlockGen.h"

#include <ostream>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

// A superblock 116 x 46 m between two 6 m lanes, a 14 m road at its east end and a pocket park.
const char* kPlan = "type,id,kind,class,x0,y0,x1,y1,width,pavement,low,high,start,end,block,value\n"
                    "street,LN-S,core_lane,lane,0,-3,130,-3,6,0,,,,,,\n"
                    "street,LN-N,core_lane,market,0,49,130,49,6,0,,,,,,\n"
                    "street,RD,road,road,137,-10,137,60,14,3.5,,,,,,\n"
                    "band,SB,x,,0,0,116,46,,,LN-S,LN-N,,,0,\n"
                    "band,RW,y,,116,0,130,46,,,,RD,LN-S,LN-N,0,\n"
                    "feature,PK,park,,50,14.83,65,29.17,,,,,,,,\n";

BlockGenResult generate(const char* csv) {
    BlockPlan plan;
    std::string err;
    REQUIRE(importBlockPlan(csv, plan, &err));
    BlockGround g; // flat at 0
    BlockGenResult out;
    blockGenerate(plan, g, out);
    return out;
}

} // namespace

TEST_CASE("blockFnv1a and blockSeed are 32-bit FNV-1a, pinned") {
    CHECK(blockFnv1a("") == 0x811C9DC5u);
    CHECK(blockFnv1a("a") == 0xE40C292Cu);
    CHECK(blockFnv1a("foobar") == 0xBF9CF968u);
    CHECK(blockSeed("CL-046", 0) == 0x580EC7AEu);
    CHECK(blockSeed("CL-048", 3) == 0xF16817AFu);
    CHECK(blockSeed("CL-048", 3) != blockSeed("CL-048", 4));
    const double h = blockHash01(7u, "yard");
    CHECK(h >= 0.0);
    CHECK(h < 1.0);
    CHECK(blockHash01(7u, "yard") == h);
}

TEST_CASE("blockRowCount packs 10-16 m rows with alleys between back-to-back pairs") {
    double d = 0.0;
    CHECK(blockRowCount(58.0 - 2.5 * 0.0, &d) == 4); // 4 rows, 1 alley
    CHECK(d == doctest::Approx(13.875));
    CHECK(blockRowCount(45.5, &d) == 3);
    CHECK(d == doctest::Approx(14.3333).epsilon(1e-3));
    CHECK(blockRowCount(26.0, &d) == 2);
    CHECK(d == doctest::Approx(13.0));
    CHECK(blockRowCount(15.0, &d) == 0);
}

TEST_CASE("blockLotWidths fills the frontage exactly within the class ranges") {
    for (std::uint32_t seed = 1; seed < 60; ++seed) {
        BlockRng rng(seed);
        std::vector<double> w;
        const double len = 40.0 + seed;
        blockLotWidths(len, seed % 2 == 0, seed % 3 == 0, seed % 5 == 0, rng, w);
        double sum = 0.0;
        for (const double x : w) sum += x;
        CHECK(sum == doctest::Approx(len));
        const double lo = seed % 2 == 0 ? kBlockShopMin : kBlockHouseMin;
        const double hi = (seed % 2 == 0 ? kBlockShopMax : kBlockHouseMax) * kBlockCorner;
        for (const double x : w) {
            CHECK(x >= lo * 0.8 - 1e-9);
            CHECK(x <= hi * 1.35 + 1e-9);
        }
    }
}

TEST_CASE("blockFloors keeps each class's range and blockHeight its floor heights") {
    BlockRng rng(42);
    for (int i = 0; i < 300; ++i) {
        const int r = blockFloors(BlockClass::Road, rng), l = blockFloors(BlockClass::Lane, rng), a = blockFloors(BlockClass::Alley, rng);
        CHECK((r >= 3 && r <= 6));
        CHECK((l >= 2 && l <= 4));
        CHECK((a >= 1 && a <= 3));
    }
    CHECK(blockHeight(4, true, 0.0) == doctest::Approx(12.9));
    CHECK(blockHeight(2, false, 0.6) == doctest::Approx(6.6));
}

TEST_CASE("blockVaryHeights breaks runs of 4 and gives 4 heights per 60 m") {
    std::vector<double> h(10, 9.0), c;
    for (int i = 0; i < 10; ++i) c.push_back(4.0 + 8.0 * i);
    CHECK(blockVaryHeights(h, c, true) > 0);
    for (std::size_t i = 3; i < h.size(); ++i) {
        const bool run = std::fabs(h[i] - h[i - 1]) < 0.05 && std::fabs(h[i] - h[i - 2]) < 0.05 && std::fabs(h[i] - h[i - 3]) < 0.05;
        CHECK_FALSE(run);
    }
    for (std::size_t i = 0; i < h.size(); ++i) {
        std::set<long> seen;
        std::size_t n = 0;
        for (std::size_t j = i; j < h.size() && c[j] - c[i] <= 60.0; ++j, ++n) seen.insert(std::lround(h[j] * 10.0));
        if (n >= 4) CHECK(seen.size() >= 4);
    }
}

TEST_CASE("importBlockPlan reads streets, bands, features and rejects bad rows") {
    BlockPlan plan;
    std::string err;
    REQUIRE(importBlockPlan(kPlan, plan, &err));
    CHECK(plan.streets.size() == 3);
    CHECK(plan.bands.size() == 2);
    CHECK(plan.features.size() == 1);
    CHECK(plan.streets[1].cls == BlockClass::Market);
    CHECK_FALSE(plan.bands[1].alongX);
    BlockPlan bad;
    CHECK_FALSE(importBlockPlan("type,id,kind,class\nstreet,X,lane,boulevard\n", bad, &err));
    CHECK_FALSE(importBlockPlan("type,id,kind\nband,B,z\n", bad, &err));
}

TEST_CASE("blockGenerate: back-to-back rows, an alley, frontage on both lanes, a park, stable") {
    const BlockGenResult a = generate(kPlan);
    const BlockGenResult b = generate(kPlan);
    REQUIRE(a.boxes.size() == b.boxes.size());
    for (std::size_t i = 0; i < a.boxes.size(); ++i) {
        CHECK(a.boxes[i].id == b.boxes[i].id);
        CHECK(a.boxes[i].sx == b.boxes[i].sx);
        CHECK(a.boxes[i].z1 == b.boxes[i].z1);
    }
    // 46 m deep: three rows of 14.5 m and one 2.5 m alley
    CHECK(a.stats.alleys == 1);
    bool alley = false;
    for (const BlockPlanStreet& s : a.streets)
        if (s.id == "SB-A1") {
            alley = true;
            CHECK(s.kind == "alley");
            CHECK(s.y0 == doctest::Approx(14.5 * 2 + 1.25));
        }
    CHECK(alley);
    // every lot stands inside its band, none overlaps another or the park
    std::vector<BlockRect> lots;
    for (const BlockBox& x : a.boxes)
        if (x.building) lots.push_back(x.rect());
    CHECK(lots.size() == a.stats.lots);
    for (std::size_t i = 0; i < lots.size(); ++i) {
        CHECK(lots[i].x0 >= -1e-6);
        CHECK(lots[i].x1 <= 130.0 + 1e-6);
        CHECK_FALSE(lots[i].overlaps({50, 14.83, 65, 29.17}, 0.01));
        for (std::size_t j = i + 1; j < lots.size(); ++j) CHECK_FALSE(lots[i].overlaps(lots[j], 0.01));
    }
    // the south lane's whole side is built (lots at y 0 from x 0 to 116)
    double covered = 0.0;
    for (const BlockBox& x : a.boxes)
        if (x.building && x.row == "SB" && std::fabs(x.rect().y0) < 1e-6) covered += x.sx;
    CHECK(covered == doctest::Approx(116.0).epsilon(0.001));
    // shop rows on the lanes, house rows on the alley; heights by class
    for (const BlockBox& x : a.boxes) {
        if (!x.building) continue;
        if (x.note.find("class=alley") != std::string::npos) {
            CHECK(x.note.find("shop=0") != std::string::npos);
        }
        CHECK(x.z1 - x.z0 >= 3.0);
        CHECK(x.z0 == doctest::Approx(-0.3));
    }
    // the road row's street trees, 1 m in from the kerb on the lots' side
    std::size_t street = 0;
    for (const BlockTree& t : a.trees)
        if (t.kind == "street") {
            ++street;
            CHECK(t.x == doctest::Approx(137.0 - 7.0 + 3.5 - 1.0));
        }
    CHECK(street >= 4);
    CHECK(a.stats.enterable == static_cast<std::size_t>(std::lround(a.stats.lots / 8.0)));
    for (const BlockDoor& d : a.doors) CHECK(std::hypot(d.ix - d.ox, d.iy - d.oy) == doctest::Approx(2.0));
}

TEST_CASE("blockGenerate pins one small layout") {
    const BlockGenResult a = generate(kPlan);
    // the pinned layout: lot counts and the first lot of the south row
    CHECK(a.stats.lots == a.stats.shopLots + a.stats.houseLots);
    const BlockBox* first = nullptr;
    for (const BlockBox& x : a.boxes)
        if (x.id == "R9-SB-R1-L1") first = &x;
    REQUIRE(first != nullptr);
    CHECK(first->rect().x0 == doctest::Approx(0.0));
    CHECK(first->rect().y0 == doctest::Approx(0.0));
    CHECK(first->sy == doctest::Approx(14.5));
    CHECK(first->sx >= kBlockShopMin * 0.8);
    CHECK(first->sx <= kBlockShopMax * kBlockCorner * 1.35);
}

TEST_CASE("blockSweepKeep keeps zones, walls, kerbs and gutters on kept streets") {
    const BlockRect rect{0, 0, 100, 100};
    std::vector<MassingStreet> streets(1);
    streets[0].id = "LN";
    streets[0].width = 6.0;
    streets[0].points = {MassingVec{0, 50, 0}, MassingVec{100, 50, 0}};
    std::vector<BlockBox> gen;
    BlockBox g;
    g.cx = 150, g.cy = 50, g.sx = 10, g.sy = 10, g.building = true;
    gen.push_back(g);
    MassingBox b;
    b.cx = 20, b.cy = 20, b.sx = 8, b.sy = 12, b.kind = "kit", b.source = MassingSource::Invented;
    CHECK_FALSE(blockSweepKeep(b, rect, streets, gen));
    b.kind = "zone";
    b.source = MassingSource::Zone;
    CHECK(blockSweepKeep(b, rect, streets, gen));
    b.kind = "wall";
    b.source = MassingSource::Plan;
    CHECK(blockSweepKeep(b, rect, streets, gen));
    b.source = MassingSource::Invented;
    CHECK_FALSE(blockSweepKeep(b, rect, streets, gen));
    b.kind = "gutter";
    b.cy = 50.0;
    CHECK(blockSweepKeep(b, rect, streets, gen));
    b.cy = 60.0;
    CHECK_FALSE(blockSweepKeep(b, rect, streets, gen));
    b.id = "V1-CLF-0001";
    CHECK(blockSweepKeep(b, rect, streets, gen));
    // outside the slice: kept unless a generated building stands on it
    MassingBox o;
    o.cx = 152, o.cy = 52, o.sx = 6, o.sy = 6, o.kind = "kit";
    CHECK_FALSE(blockSweepKeep(o, rect, streets, gen));
    o.cx = 180;
    CHECK(blockSweepKeep(o, rect, streets, gen));
}
