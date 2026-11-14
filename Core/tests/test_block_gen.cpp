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
    CHECK(blockRowCount(58.0, &d) == 4); // 4 rows, 3 alleys
    CHECK(d == doctest::Approx((58.0 - 3 * kBlockAlley) / 4));
    CHECK(blockRowCount(45.5, &d) == 3);
    CHECK(d == doctest::Approx((45.5 - 2 * kBlockAlley) / 3));
    CHECK(blockRowCount(26.0, &d) == 2); // back to back across one alley
    CHECK(d == doctest::Approx((26.0 - kBlockAlley) / 2));
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
    // 46 m deep: three rows and an alley between each pair (the back-to-back pair too), cut clear through the road
    // row to RD's edge at x 130; their west ends reach no street
    const double row = (46.0 - 2 * kBlockAlley) / 3;
    CHECK(a.stats.alleys == 2);
    CHECK(a.stats.alleysCut == 2);
    std::vector<BlockRect> alleys;
    for (const BlockPlanStreet& s : a.streets)
        if (s.id == "SB-A1" || s.id == "SB-A2") {
            CHECK(s.kind == "alley");
            CHECK(s.width == doctest::Approx(kBlockAlley));
            CHECK(s.y0 == doctest::Approx(s.id == "SB-A1" ? row + kBlockAlley * 0.5 : 2 * row + kBlockAlley * 1.5));
            CHECK(s.x0 == doctest::Approx(0.0));
            CHECK(s.x1 == doctest::Approx(130.0).epsilon(0.002));
            alleys.push_back({s.x0, s.y0 - s.width * 0.5, s.x1, s.y0 + s.width * 0.5});
        }
    REQUIRE(alleys.size() == 2);
    for (const BlockBox& x : a.boxes)
        if (x.building)
            for (const BlockRect& r : alleys) CHECK_FALSE(x.rect().overlaps(r, 0.01));
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
    CHECK(first->sy == doctest::Approx((46.0 - 2 * kBlockAlley) / 3));
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

TEST_CASE("blockGenerate: districts by band or feature, cue palettes, short-road notes") {
    std::string csv = std::string(kPlan);
    csv.replace(csv.find(",block,value\n"), 13, ",block,value,district\n");
    csv.replace(csv.find("LN-S,LN-N,,,0,\n"), 15, "LN-S,LN-N,,,0,,D1\n");
    const BlockGenResult a = generate(csv.c_str());
    // SB in D1; RW and PK default to their own ids
    REQUIRE(a.districts.size() == 3);
    CHECK(a.districts[0].id == "D1");
    CHECK(a.districts[1].id == "RW");
    CHECK(a.districts[2].id == "PK");
    CHECK(a.districts[0].colour != a.districts[1].colour);
    CHECK(a.districts[0].sign != a.districts[1].sign);
    CHECK(a.districts[0].paving != a.districts[1].paving);
    std::size_t lots = 0;
    for (const BlockDistrict& d : a.districts) lots += d.lots;
    CHECK(lots == a.stats.lots);
    for (const BlockBox& x : a.boxes) {
        if (x.row == "SB") CHECK(x.district == "D1");
        if (!x.district.empty()) CHECK(x.note.find("district=" + x.district) != std::string::npos);
    }
    // the 70 m road is noted as too short for a main street
    bool noted = false;
    for (const std::string& n : a.stats.notes) noted = noted || n.rfind("RD: a road only 70 m", 0) == 0;
    CHECK(noted);
    // "name@k" takes palette slot k and drops the suffix
    std::string csv2 = csv;
    csv2.replace(csv2.find(",,D1\n"), 5, ",,D1@4\n");
    const BlockGenResult b = generate(csv2.c_str());
    REQUIRE(b.districts.size() == 3);
    CHECK(b.districts[0].id == "D1");
    CHECK(b.districts[0].colour == "#3E7D3A");
    CHECK(b.districts[1].colour == a.districts[1].colour);
    for (const BlockBox& x : b.boxes)
        if (x.row == "SB") CHECK(x.district == "D1");
}

TEST_CASE("BlockGround: a local frame samples the turned world point") {
    BlockGround g;
    MassingBox z;
    z.id = "Z";
    z.kind = "zone";
    z.source = MassingSource::Zone;
    z.cx = 100.0, z.cy = 50.0, z.sx = 4.0, z.sy = 4.0, z.z0 = 0.0, z.z1 = 7.0;
    g.setZones({z});
    CHECK(g.at(100.0, 50.0) == doctest::Approx(7.0));
    g.setFrame(100.0, 40.0, 90.0); // local +x runs north from (100, 40)
    double wx = 0.0, wy = 0.0, lx = 0.0, ly = 0.0;
    g.toWorld(10.0, 0.0, wx, wy);
    CHECK(wx == doctest::Approx(100.0));
    CHECK(wy == doctest::Approx(50.0));
    g.toLocal(wx, wy, lx, ly);
    CHECK(lx == doctest::Approx(10.0));
    CHECK(ly == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(g.at(10.0, 0.0) == doctest::Approx(7.0));
    CHECK(g.at(100.0, 50.0) != doctest::Approx(7.0));
}

namespace {
MassingStreet routeStreet(const char* id, const char* kind, double w, double pav, double x0, double y0, double x1, double y1) {
    MassingStreet s;
    s.id = id;
    s.kind = kind;
    s.width = w;
    s.pavement = pav;
    s.points = {MassingVec{x0, y0, 0}, MassingVec{x1, y1, 0}};
    return s;
}
} // namespace

TEST_CASE("blockStreetRole, blockStraightRun and blockRouteTables") {
    MassingStreet bent = routeStreet("B", "road", 14, 3.5, 0, 0, 100, 0);
    bent.points.push_back(MassingVec{200, 3, 0});  // under 5 degrees: still straight
    bent.points.push_back(MassingVec{260, 60, 0}); // 45 degrees: a new run
    CHECK(blockStreetRole(bent) == BlockRole::Main);
    CHECK(blockStraightRun(bent) == doctest::Approx(100.0 + std::hypot(100.0, 3.0)));
    CHECK(blockStreetRole(routeStreet("L", "core_lane", 6, 0, 0, 0, 1, 0)) == BlockRole::Lane);
    CHECK(blockStreetRole(routeStreet("A", "alley", 2.5, 0, 0, 0, 1, 0)) == BlockRole::Alley);
    CHECK(blockStreetRole(routeStreet("N", "core_lane", 3, 0, 0, 0, 1, 0)) == BlockRole::Alley); // 3 m or narrower
    CHECK(blockRoleName(BlockRole::Main) == "road");
    CHECK(blockStreetRole(routeStreet("U", "road", 8, 1, 0, 0, 1, 0)) == BlockRole::Lane); // too narrow for a main

    // two mains at y 0 and y 100, a lane from one to the other (mouths stopping at the kerbs), a market lane
    std::vector<MassingStreet> s = {routeStreet("M1", "road", 14, 3.5, -50, 0, 250, 0),
                                    routeStreet("M2", "road", 14, 3.5, -50, 100, 250, 100),
                                    routeStreet("LN", "core_lane", 6, 0, 50, 7, 50, 93),
                                    routeStreet("MK", "market_arcade", 5, 0, 150, 7, 150, 93)};
    std::vector<MassingBox> boxes;
    MassingBox b; // a corner building east of the lane, north of M1
    b.id = "K", b.kind = "kit", b.cx = 60, b.cy = 17, b.sx = 14, b.sy = 20, b.z0 = 0, b.z1 = 12;
    boxes.push_back(b);
    std::vector<BlockStreetFlag> flags;
    std::vector<BlockPlate> plates;
    blockRouteTables(s, boxes, {0, 0, 200, 100}, BlockGround{}, flags, plates);
    REQUIRE(flags.size() == 4);
    CHECK(flags[0].main);
    CHECK(flags[0].treeEvery == doctest::Approx(10.0));
    CHECK(flags[0].lampKind == "mast");
    CHECK(flags[0].role == "road");
    CHECK(flags[0].mouth == "junction");
    CHECK(flags[2].role == "lane");
    CHECK(flags[2].mouth == "dropped_kerb");
    CHECK(flags[3].role == "market");
    CHECK(flags[3].lampEvery < flags[2].lampEvery);
    CHECK(flags[0].signsPer100 > flags[2].signsPer100);
    // four junctions (each lane's two mouths), the minor street first; the lane's south-east corner sits on K
    REQUIRE(plates.size() == 4);
    bool wall = false;
    for (const BlockPlate& p : plates) {
        CHECK((p.a == "LN" || p.a == "MK"));
        CHECK((p.b == "M1" || p.b == "M2"));
        if (p.a == "LN" && p.b == "M1") {
            CHECK(p.x == doctest::Approx(50 + 3.15));
            CHECK(p.y == doctest::Approx(7.15));
            CHECK(p.mount == "wall");
            wall = true;
        }
    }
    CHECK(wall);
}

TEST_CASE("blockGenerate r10 dressing: roof garden, market stalls, gate roof and path, park paths, place plates") {
    const char* plan = "type,id,kind,class,x0,y0,x1,y1,width,pavement,low,high,start,end,block,value,top\n"
                       "street,LN-S,core_lane,lane,0,-3,130,-3,6,1.5,,,,,,,\n"
                       "street,MK,market_arcade,market,0,49,130,49,6,1.5,,,,,,,\n"
                       "feature,PD,podium,,0,0,40,40,0,,,,,,,2,\n"
                       "feature,TW,podium,,45,0,85,40,32,,,,,,,2,\n"
                       "feature,PR,precinct,,90,0,130,40,,,,,,,,,\n"
                       "feature,HL,hall,,95,10,110,30,,,,,,,,2,\n"
                       "feature,GT,gate,,129,17,130,23,,,,,,,,,\n"
                       "feature,PK,park,,90,41,105,46,,,,,,,,,\n";
    const BlockGenResult a = generate(plan);
    // the towerless podium's 40 m roof is planted, on its roof; the tower's 4 m aprons are not
    std::size_t roof = 0;
    for (const BlockTree& t : a.trees)
        if (t.host == "R9-F-PD") {
            ++roof;
            CHECK(t.kind == "park");
            CHECK(t.z > 5.0);
        }
    CHECK(roof == a.stats.roofTrees);
    CHECK(roof >= 25);
    for (const BlockTree& t : a.trees) CHECK(t.host != "R9-F-TW");
    // stalls on both of the market's footways (y 47.5..48.7 and 49.3..50.5), none on the lane
    std::size_t stalls = 0, gateRoof = 0, gatePath = 0, parkPath = 0;
    for (const BlockBox& b : a.boxes) {
        if (b.kind == "stand") {
            ++stalls;
            CHECK(b.row == "MK");
            CHECK(std::fabs(std::fabs(b.cy - 49.0) - 2.25) < 1e-6);
            CHECK(b.z1 - b.ground == doctest::Approx(2.4));
        }
        if (b.id == "R10-GR-GT") {
            ++gateRoof;
            CHECK(b.kind == "deck");
        }
        if (b.id == "R10-GP-GT") {
            ++gatePath;
            CHECK(b.kind == "marking");
            CHECK(b.rect().x0 == doctest::Approx(110.0)); // from the hall's east face to the gate
            CHECK(b.rect().x1 == doctest::Approx(130.0));
        }
        if (b.note.find("park=1") != std::string::npos) ++parkPath;
    }
    CHECK(stalls == a.stats.stalls);
    CHECK(stalls >= 30);
    CHECK(gateRoof == 1);
    CHECK(gatePath == 1);
    CHECK(parkPath == 3);
    // a pole plate for the park and the precinct, on the side nearest a street
    BlockPlan p;
    REQUIRE(importBlockPlan(plan, p));
    std::vector<MassingStreet> streets(1);
    streets[0].id = "MK";
    streets[0].width = 6.0;
    streets[0].points = {MassingVec{0, 49, 0}, MassingVec{130, 49, 0}};
    std::vector<BlockPlate> plates;
    blockPlacePlates(p.features, streets, BlockGround{}, plates);
    REQUIRE(plates.size() == 2);
    CHECK(plates[0].a == "PR");
    CHECK(plates[0].b == "precinct");
    CHECK(plates[1].a == "PK");
    CHECK(plates[1].y == doctest::Approx(45.0)); // the park's north side, 1 m in
    CHECK(plates[1].mount == "pole");
}
