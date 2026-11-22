// Street frontage from the modular building kit (ftcore/BuildingLots.h).

#include "ftcore/BuildingLots.h"

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

KitTable kitTable() {
    KitTable kit;
    std::string err;
    const bool ok = parseKitTable(readFile(std::string(FTCORE_PROJECT_DATA_DIR) + "/Look/building_kit_pieces.csv"), kit, &err);
    REQUIRE_MESSAGE(ok, err);
    return kit;
}

// A row of n boxes 9 m wide along a street edge at y = 622.5 (their street face), east to west, the ground falling
// 0.03 m a metre to the east.
std::vector<MassingBox> row(int n, double x0 = 747.0) {
    std::vector<MassingBox> out;
    for (int i = 0; i < n; ++i) {
        MassingBox b;
        b.id = "R-L" + std::to_string(i + 1);
        b.kind = "kit";
        b.cx = x0 + 4.5 + 9.0 * i;
        b.cy = 615.0833;
        b.sx = 9.0;
        b.sy = 14.8333;
        b.z0 = 8.0 - 0.03 * (b.cx - x0);
        b.z1 = b.z0 + 11.0;
        out.push_back(b);
    }
    return out;
}

FrontageRun westRun(double x0, double x1) {
    FrontageRun r;
    r.id = "TEST-RUN";
    r.ax = x1;
    r.ay = 622.5;
    r.bx = x0;
    r.by = 622.5;
    r.maxDepth = 14.0;
    return r;
}

double widthOf(const KitPiece& p) {
    const KitSnap* r = p.snap("R");
    return r ? r->x : p.sx;
}

// Turns a piece-local point by the placement's yaw into the plan frame.
void local(const KitPlacement& p, double lx, double ly, double& px, double& py) {
    const double a = p.yaw * 3.14159265358979323846 / 180.0;
    px = p.x + lx * std::cos(a) - ly * std::sin(a);
    py = p.y + lx * std::sin(a) + ly * std::cos(a);
}

BuildingLot lotAt(double s0, LotType t, int units, std::uint8_t colour) {
    BuildingLot l;
    l.s0 = s0;
    l.s1 = s0 + units * kLotGrid;
    l.type = t;
    l.wallColour = colour;
    return l;
}

} // namespace

TEST_CASE("building kit: the committed table reads with its snaps") {
    const KitTable kit = kitTable();
    CHECK(kit.pieces.size() == 33);
    const int corner = kit.find("SHOP_CORNER_63");
    REQUIRE(corner >= 0);
    const KitSnap* ret = kit.pieces[static_cast<std::size_t>(corner)].snap("RETURN_END");
    REQUIRE(ret != nullptr);
    CHECK(ret->y == doctest::Approx(3.6));
    CHECK(widthOf(kit.pieces[static_cast<std::size_t>(corner)]) == doctest::Approx(6.3));
    CHECK(kit.find("NOT_A_PIECE") == -1);
    for (const KitPiece& p : kit.pieces) CHECK(p.triangles > 0);
}

TEST_CASE("building kit: quoted cells, bad rows") {
    KitTable kit;
    std::string err;
    CHECK(parseKitTable("id,note,size_x,size_y,size_z,min,snaps\nA,\"one, two\",1,1,1,0/0/0,R:1/0/0\n", kit, &err));
    REQUIRE(kit.pieces.size() == 1);
    CHECK(kit.pieces[0].snaps[0].x == doctest::Approx(1.0));
    CHECK_FALSE(parseKitTable("id,size_x,size_y,size_z,min\nA,1,1,1,0/0\n", kit, &err));
    CHECK_FALSE(parseKitTable("id,size_x,size_y,size_z\nA,1,1,1\n", kit, &err));
    std::vector<FrontageRun> runs;
    CHECK(parseFrontageRuns("id,ax,ay,bx,by,max_depth,corner\nR1,10,0,0,0,12,1\n", runs, &err));
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].cornerAtA);
    CHECK(runs[0].yaw() == doctest::Approx(180.0));
    double px = 0.0, py = 0.0;
    runs[0].toPlan(2.0, 3.0, px, py); // 2 m west of A and 3 m in: a run heading west has its buildings south
    CHECK(px == doctest::Approx(8.0));
    CHECK(py == doctest::Approx(-3.0));
    CHECK_FALSE(parseFrontageRuns("id,ax,ay,bx,by\nR1,0,0,1,0\n", runs, &err));
}

TEST_CASE("building lots: a 117 m row splits on the grid, obeys the street rules and is deterministic") {
    const std::vector<MassingBox> boxes = row(13);
    const std::vector<FrontageRun> runs = {westRun(747.0, 864.0)};
    FrontagePlan a, b;
    planFrontage(runs, boxes, a);
    planFrontage(runs, boxes, b);
    REQUIRE(a.lots.size() >= 10);
    CHECK(a.replaced.size() == 13);
    std::set<int> types;
    double last = 0.0;
    for (std::size_t i = 0; i < a.lots.size(); ++i) {
        const BuildingLot& l = a.lots[i];
        CHECK(l.s0 >= last - 1e-9);
        CHECK(l.s0 - last < 0.9);
        CHECK(std::fabs(l.width() - l.units() * kLotGrid) < 1e-9);
        CHECK(l.width() >= 4.5 - 1e-9);
        CHECK(l.width() <= 10.8 + 1e-9);
        CHECK(l.setback <= 0.45 + 1e-9);
        CHECK(!l.parents.empty());
        CHECK(l.floorZ > l.footZ);
        CHECK(l.depth == doctest::Approx(14.0));
        last = l.s1;
        types.insert(static_cast<int>(l.type));
        CHECK(lotSignature(l) == lotSignature(b.lots[i]));
        CHECK(l.s0 == b.lots[i].s0);
    }
    CHECK(117.0 - last < 0.9);
    CHECK(types.size() >= 3);
    const FrontageCheck c = checkFrontage(a.lots);
    CHECK_MESSAGE(c.ok, c.firstFault);
}

TEST_CASE("building lots: a corner run starts with the corner shop") {
    MassingBox b;
    b.id = "CORNER";
    b.kind = "kit";
    b.cx = 871.0;
    b.cy = 632.25;
    b.sx = 14.0;
    b.sy = 7.5;
    b.z0 = 4.075;
    b.z1 = 18.7;
    FrontageRun r;
    r.id = "TEST-CORNER";
    r.ax = 878.0;
    r.ay = 636.0;
    r.bx = 864.0;
    r.by = 636.0;
    r.cornerAtA = true;
    FrontagePlan p;
    planFrontage({r}, {b}, p);
    REQUIRE(p.lots.size() == 2);
    CHECK(p.lots[0].type == LotType::Corner);
    CHECK(p.lots[0].depth == doctest::Approx(7.5));
    CHECK(p.lots[0].setback == 0.0);
    CHECK(p.lots[1].type != LotType::Corner);
    CHECK(checkFrontage(p.lots).ok);
}

TEST_CASE("building lots: boxes off the line or across it are left alone") {
    std::vector<MassingBox> boxes = row(2);
    boxes[1].cy -= 1.0; // its face 1 m behind the line
    FrontagePlan p;
    planFrontage({westRun(747.0, 765.0)}, boxes, p);
    CHECK(p.replaced.size() == 1);
}

TEST_CASE("street rules: each check at its boundary") {
    // Equal facades 39 m apart fail, 41 m pass (the lots between differ).
    std::vector<BuildingLot> lots = {lotAt(0.0, LotType::WalkUp, 6, 0), lotAt(39.0, LotType::WalkUp, 6, 0)};
    CHECK(checkFrontage(lots).equalWithin40 == 1);
    lots[1].s0 = 41.0;
    lots[1].s1 = 41.0 + 5.4;
    CHECK(checkFrontage(lots).equalWithin40 == 0);

    // Neighbours: one trait apart fails, two pass.
    std::vector<BuildingLot> pair = {lotAt(0.0, LotType::WalkUp, 6, 0), lotAt(6.0, LotType::WalkUp, 7, 1)};
    CHECK(checkFrontage(pair).neighbourTwins == 1);
    pair[1].floors = 4;
    CHECK(checkFrontage(pair).neighbourTwins == 0);

    // Three equal wall colours in a row.
    std::vector<BuildingLot> three = {lotAt(0.0, LotType::WalkUp, 6, 2), lotAt(6.0, LotType::Pencil, 5, 2),
                                      lotAt(11.0, LotType::Tiled, 8, 2)};
    three[1].floors = 5;
    three[2].floors = 6;
    CHECK(checkFrontage(three).colourTriples == 1);
    three[2].wallColour = 3;
    CHECK(checkFrontage(three).colourTriples == 0);
    CHECK(checkFrontage(three).ok);

    // A 60 m stretch of two types fails; a third type in it passes.
    std::vector<BuildingLot> two;
    for (int i = 0; i < 8; ++i) {
        BuildingLot l = lotAt(i * 9.0, i % 2 ? LotType::Tiled : LotType::WalkUp, 10, static_cast<std::uint8_t>(i % 5));
        l.floors = 3 + i % 3;
        l.upperVariant = static_cast<std::uint8_t>(i % 3);
        two.push_back(l);
    }
    CHECK(checkFrontage(two).poorWindows > 0);
    for (int i = 2; i < 8; i += 3) two[static_cast<std::size_t>(i)].type = LotType::TownHouse;
    CHECK(checkFrontage(two).poorWindows == 0);
}

TEST_CASE("building lots: a 720 m street of 80 boxes repairs to a clean check") {
    const std::vector<MassingBox> boxes = row(80, 0.0);
    FrontagePlan p;
    planFrontage({westRun(0.0, 720.0)}, boxes, p);
    CHECK(p.lots.size() >= 70);
    const FrontageCheck c = checkFrontage(p.lots);
    CHECK_MESSAGE(c.ok, c.firstFault);
}

TEST_CASE("facade grammar: every type and width fills its ground storey exactly and stands on the lot") {
    const KitTable kit = kitTable();
    const FrontageRun run = westRun(747.0, 864.0);
    static constexpr LotType kinds[5] = {LotType::Corner, LotType::Pencil, LotType::WalkUp, LotType::Tiled, LotType::TownHouse};
    static constexpr int widths[5][7] = {{7, 8, 11}, {5, 6}, {6, 7, 8, 9, 10, 11, 12}, {6, 8, 9, 10, 12}, {6, 7, 8}};
    int cases = 0;
    for (int t = 0; t < 5; ++t)
        for (int u : widths[t]) {
            if (u == 0) continue;
            for (int v = 0; v < 3; ++v)
                for (int sign = 0; sign < 4; ++sign) {
                    BuildingLot lot = lotAt(9.0, kinds[t], u, 0);
                    lot.depth = 14.0;
                    lot.floorZ = 8.0;
                    lot.footZ = 7.5;
                    lot.floors = kinds[t] == LotType::TownHouse ? 2 : 4;
                    lot.roof = kinds[t] == LotType::TownHouse ? (v ? RoofKind::Hip : RoofKind::Gable) : RoofKind::Flat;
                    lot.groundVariant = static_cast<std::uint8_t>(v);
                    lot.upperVariant = static_cast<std::uint8_t>(v);
                    lot.signSet = static_cast<std::uint8_t>(sign);
                    lot.clutterSeed = 0x12345u * static_cast<std::uint32_t>(u + v);
                    lot.seed = 77u;
                    std::vector<KitPlacement> out;
                    std::string err;
                    REQUIRE_MESSAGE(facadePlacements(kit, run, lot, 3, out, &err), err);
                    double ground = 0.0, upper = 0.0;
                    for (const KitPlacement& p : out) {
                        CHECK(p.lot == 3);
                        if (p.piece < 0 || p.role != PlacementRole::Piece) continue;
                        const KitPiece& k = kit.pieces[static_cast<std::size_t>(p.piece)];
                        const bool course = k.group == "ground" || k.group == "upper";
                        if (course && p.z == doctest::Approx(lot.floorZ)) ground += widthOf(k);
                        if (course && p.z == doctest::Approx(lot.floorZ + lot.groundStorey())) upper += widthOf(k);
                    }
                    CHECK(ground == doctest::Approx(lot.width()));
                    CHECK(upper == doctest::Approx(lot.width()));
                    ++cases;
                }
        }
    CHECK(cases == 20 * 12);
}

TEST_CASE("facade grammar: three shop lots in four wear awnings and a sign stack, the rest a fascia sign") {
    const KitTable kit = kitTable();
    const FrontageRun run = westRun(747.0, 864.0);
    int awningLots = 0, fasciaLots = 0;
    for (int sign = 0; sign < 4; ++sign) {
        BuildingLot lot = lotAt(0.0, LotType::WalkUp, 9, 0);
        lot.depth = 14.0;
        lot.floorZ = 8.0;
        lot.footZ = 7.5;
        lot.floors = 4;
        lot.signSet = static_cast<std::uint8_t>(sign);
        std::vector<KitPlacement> out;
        REQUIRE(facadePlacements(kit, run, lot, 0, out));
        int awnings = 0, fascias = 0;
        for (const KitPlacement& p : out) {
            if (p.piece == kit.find("AWNING_27") || p.piece == kit.find("AWNING_36")) ++awnings;
            if (p.piece == kit.find("SIGN_FASCIA_36")) ++fascias;
        }
        if (sign == 0) {
            CHECK(awnings == 0);
        } else {
            CHECK(fascias == 0);
        }
        awningLots += awnings > 0 ? 1 : 0;
        fasciaLots += fascias > 0 ? 1 : 0;
    }
    CHECK(awningLots + fasciaLots >= 3);
    CHECK(awningLots >= 2 * fasciaLots);
}

TEST_CASE("facade grammar: on a run heading west balconies reach north and the pencil sign hangs west") {
    const KitTable kit = kitTable();
    const FrontageRun run = westRun(747.0, 864.0);
    BuildingLot tiled = lotAt(0.0, LotType::Tiled, 9, 0);
    tiled.depth = 14.0;
    tiled.floorZ = 8.0;
    tiled.footZ = 7.5;
    tiled.floors = 3;
    tiled.upperVariant = 0;
    std::vector<KitPlacement> out;
    REQUIRE(facadePlacements(kit, run, tiled, 0, out));
    int balconies = 0;
    for (const KitPlacement& p : out) {
        if (p.piece != kit.find("BAY_BALC_27")) continue;
        ++balconies;
        CHECK(p.yaw == doctest::Approx(180.0));
        double px = 0.0, py = 0.0;
        local(p, 1.35, -1.12, px, py);
        CHECK(py > 622.5 + 1.0);
        CHECK(px <= 864.0 + 1e-9);
        CHECK(px >= 864.0 - 8.1 - 1e-9);
    }
    CHECK(balconies == 6);

    BuildingLot pencil = lotAt(0.0, LotType::Pencil, 5, 0);
    pencil.depth = 14.0;
    pencil.floorZ = 8.0;
    pencil.floors = 4;
    pencil.upperVariant = 0;
    out.clear();
    REQUIRE(facadePlacements(kit, run, pencil, 0, out));
    double bayX = 0.0, signX = 0.0;
    for (const KitPlacement& p : out) {
        if (p.piece == kit.find("BAY_PENCIL_45") && p.z == doctest::Approx(11.6)) bayX = p.x;
        if (p.piece == kit.find("SIGN_VERT_L")) signX = p.x;
    }
    CHECK(bayX == doctest::Approx(864.0));
    CHECK(signX == doctest::Approx(864.0 - 4.05));
}

TEST_CASE("facade grammar: a body box under the roof, a plinth down to the foot, parapets at the top") {
    const KitTable kit = kitTable();
    const FrontageRun run = westRun(747.0, 864.0);
    BuildingLot lot = lotAt(0.0, LotType::WalkUp, 8, 0);
    lot.depth = 12.0;
    lot.floorZ = 8.0;
    lot.footZ = 7.0;
    lot.floors = 4;
    std::vector<KitPlacement> out;
    REQUIRE(facadePlacements(kit, run, lot, 0, out));
    int bodies = 0, plinths = 0, parapets = 0;
    for (const KitPlacement& p : out) {
        if (p.role == PlacementRole::Body) {
            ++bodies;
            CHECK(p.z == doctest::Approx(8.0));
            CHECK(p.z + p.sz == doctest::Approx(lot.top() + 0.03));
        }
        if (p.role == PlacementRole::Plinth) {
            ++plinths;
            CHECK(p.z == doctest::Approx(6.7));
            CHECK(p.z + p.sz == doctest::Approx(8.0));
        }
        if (p.piece == kit.find("PARAPET_09")) {
            ++parapets;
            CHECK(p.z == doctest::Approx(lot.top()));
        }
    }
    CHECK(bodies == 1);
    CHECK(plinths == 1);
    CHECK(parapets >= 2 * 8 + 2 * 12);
    CHECK(lot.top() == doctest::Approx(8.0 + 3.6 + 9.0));
}

TEST_CASE("building kit: palettes and census") {
    CHECK(kitPaletteSize(KitPalette::Wall) == 9);
    CHECK(kitPaletteColour(KitPalette::Accent, 6).r == kitPaletteColour(KitPalette::Accent, 0).r);
    const KitTable kit = kitTable();
    FrontagePlan plan;
    planFrontage({westRun(747.0, 864.0)}, row(13), plan);
    std::vector<KitPlacement> out;
    for (std::size_t i = 0; i < plan.lots.size(); ++i)
        REQUIRE(facadePlacements(kit, plan.runs[0], plan.lots[i], static_cast<int>(i), out));
    const KitCensus c = kitCensus(kit, plan, out);
    CHECK(c.lots == static_cast<int>(plan.lots.size()));
    CHECK(c.pieces + c.boxes == static_cast<int>(out.size()));
    CHECK(c.triangles > 100000);
}

TEST_CASE("street clutter: a pedestrian way's band drops the bicycles parked in it and keeps the planters by the wall") {
    const KitTable kit = kitTable();
    FrontagePlan plan;
    planFrontage({westRun(747.0, 864.0)}, row(13), plan);
    std::vector<KitPlacement> all;
    for (std::size_t i = 0; i < plan.lots.size(); ++i)
        REQUIRE(facadePlacements(kit, plan.runs[0], plan.lots[i], static_cast<int>(i), all));
    const int bicycle = kit.find("BICYCLE"), planters = kit.find("PLANTERS"), vending = kit.find("VENDING");
    REQUIRE(bicycle >= 0);
    auto count = [](const std::vector<KitPlacement>& v, int piece) {
        int n = 0;
        for (const KitPlacement& p : v) n += p.piece == piece ? 1 : 0;
        return n;
    };
    const int bikes = count(all, bicycle), plant = count(all, planters), vend = count(all, vending);
    REQUIRE(bikes > 0);
    REQUIRE(plant > 0);
    // The lane in front: CL-051's line, 6 m wide, its south face the run at y 622.5.
    MassingStreet lane;
    lane.id = "CL-051";
    lane.kind = "core_lane";
    lane.width = 6.0;
    lane.points = {{692.5, 625.5, 0.0}, {876.5, 625.5, 0.0}};
    // Not a pedestrian way, or no band: nothing moves.
    std::vector<KitPlacement> out = all;
    CHECK(dropClutterInWays(kit, {lane}, 1.5, out) == 0);
    CHECK(out.size() == all.size());
    lane.foot = true;
    CHECK(dropClutterInWays(kit, {lane}, 0.0, out) == 0);
    CHECK(out.size() == all.size());
    // A pedestrian way with the crowd's 1.5 m half band: the bicycles 2.2 m out (their far side about 2.6 m from the wall,
    // 0.4 m from the centre line) go; the planters (to 1.04 m out) and the vending machine stay, and so does the rest.
    CHECK(dropClutterInWays(kit, {lane}, 1.5, out) == static_cast<std::size_t>(bikes));
    CHECK(count(out, bicycle) == 0);
    CHECK(count(out, planters) == plant);
    CHECK(count(out, vending) == vend);
    CHECK(out.size() == all.size() - static_cast<std::size_t>(bikes));
    // The kept placements keep their order.
    std::size_t j = 0;
    for (const KitPlacement& p : all)
        if (p.piece != bicycle) {
            REQUIRE(j < out.size());
            CHECK(out[j].piece == p.piece);
            CHECK(out[j].x == p.x);
            ++j;
        }
    // A band that ends 0.2 m short of the bicycles' far side (their foot reaches about y 625.1) keeps them.
    out = all;
    lane.points = {{692.5, 626.8, 0.0}, {876.5, 626.8, 0.0}};
    CHECK(dropClutterInWays(kit, {lane}, 1.5, out) == 0);
}

namespace {
// One 1 x 1 m piece centred on its origin, for the clutter band's exact cases.
KitTable unitKit() {
    KitTable kit;
    KitPiece box;
    box.id = "BOX";
    box.group = "clutter";
    box.sx = box.sy = box.sz = 1.0;
    box.minX = box.minY = -0.5;
    kit.pieces.push_back(box);
    return kit;
}

KitPlacement clutterAt(double x, double y, double yaw = 0.0, int piece = 0,
                       PlacementRole role = PlacementRole::Clutter) {
    KitPlacement p;
    p.piece = piece;
    p.role = role;
    p.x = x;
    p.y = y;
    p.yaw = yaw;
    return p;
}

MassingStreet wayThrough(std::vector<MassingVec> points) {
    MassingStreet st;
    st.id = "W";
    st.kind = "core_lane";
    st.width = 3.0;
    st.foot = true;
    st.points = std::move(points);
    return st;
}
} // namespace

TEST_CASE("street clutter: only clutter with a piece in the kit is weighed; other roles in the band stay") {
    const KitTable kit = unitKit();
    const MassingStreet way = wayThrough({{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}});
    std::vector<KitPlacement> v = {
        clutterAt(5.0, 0.0, 0.0, 0, PlacementRole::Piece),
        clutterAt(5.0, 0.0, 0.0, 0, PlacementRole::Body),
        clutterAt(5.0, 0.0, 0.0, 0, PlacementRole::Plinth),
        clutterAt(5.0, 0.0, 0.0, 0, PlacementRole::Deck),
        clutterAt(5.0, 0.0, 0.0, -1), // no piece
        clutterAt(5.0, 0.0, 0.0, 1),  // past the table's end
        clutterAt(5.0, 0.0),          // the one that goes
    };
    CHECK(dropClutterInWays(kit, {way}, 1.0, v) == 1);
    REQUIRE(v.size() == 6);
    CHECK(v[0].role == PlacementRole::Piece);
    CHECK(v[3].role == PlacementRole::Deck);
    CHECK(v[4].piece == -1);
    CHECK(v[5].piece == 1);
}

TEST_CASE("street clutter: every segment of a way has its band, and none past its ends") {
    const KitTable kit = unitKit();
    // An L: east 10 m, then north 10 m; a zero-length segment at the corner is skipped.
    const MassingStreet way = wayThrough({{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {10.0, 10.0, 0.0}});
    std::vector<KitPlacement> v = {
        clutterAt(5.0, 0.9),   // first leg's band (its foot to y 0.4)
        clutterAt(10.9, 5.0),  // second leg's band only
        clutterAt(5.0, 5.0),   // between the legs, 5 m from each
        clutterAt(10.0, 11.0), // past the second leg's end (its foot from y 10.5)
        clutterAt(-1.0, 0.0),  // past the first leg's start (its foot to x -0.5)
        clutterAt(5.0, 1.5),   // touches the first leg's band only: stays
    };
    CHECK(dropClutterInWays(kit, {way}, 1.0, v) == 2);
    REQUIRE(v.size() == 4);
    CHECK(v[0].x == 5.0);
    CHECK(v[0].y == 5.0);
    CHECK(v[1].y == 11.0);
    CHECK(v[2].x == -1.0);
    CHECK(v[3].y == 1.5);
}

TEST_CASE("street clutter: a turned segment's band is turned with it, not its plan box") {
    const KitTable kit = unitKit();
    const MassingStreet way = wayThrough({{0.0, 0.0, 0.0}, {10.0, 10.0, 0.0}});
    const double nx = -std::sqrt(0.5), ny = std::sqrt(0.5); // the segment's left normal
    auto off = [&](double d, double yaw) { return clutterAt(5.0 + d * nx, 5.0 + d * ny, yaw); };
    // A square foot square to the axes reaches 0.71 m along the normal; turned 45 degrees, 0.5 m. Band 1 m.
    std::vector<KitPlacement> v = {
        off(0.0, 0.0),  // on the line
        off(1.6, 0.0),  // 1.6 - 0.71 = 0.89 m from the line: in
        off(1.6, 45.0), // 1.6 - 0.5 = 1.1 m: out
        off(2.0, 0.0),  // 1.29 m: out, though inside the band's plan box
        off(-1.6, 0.0), // the other side: in
    };
    CHECK(dropClutterInWays(kit, {way}, 1.0, v) == 3);
    REQUIRE(v.size() == 2);
    CHECK(v[0].yaw == 45.0);
    CHECK(v[1].x == doctest::Approx(5.0 + 2.0 * nx));
}
