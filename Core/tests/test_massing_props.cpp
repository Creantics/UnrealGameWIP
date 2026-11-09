// Street furniture along a massing map's streets (ftcore/MassingProps.h): the prop table's reader, the spots along a
// street, the kerb and wall rules, the checks that drop a spot and the mesh placement. Made-up tables only: the real
// ones are kept outside the repo. Ids and numbers only.

#include "ftcore/MassingProps.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

MassingBox flatBox(double cx, double cy, double sx, double sy, double z0, double z1) {
    MassingBox b;
    b.id = "B";
    b.kind = "slab";
    b.cover = true;
    b.cx = cx;
    b.cy = cy;
    b.sx = sx;
    b.sy = sy;
    b.z0 = z0;
    b.z1 = z1;
    return b;
}

MassingStreet street(const std::string& id, const std::string& kind, double width, std::vector<MassingVec> points) {
    MassingStreet s;
    s.id = id;
    s.kind = kind;
    s.width = width;
    s.points = std::move(points);
    return s;
}

MassingPropRule rule(MassingPropSide side, double spacing) {
    MassingPropRule r;
    r.id = "P-1";
    r.asset = "/Game/X.X";
    r.side = side;
    r.spacing = spacing;
    return r;
}

// Places on flat ground at 0 (an empty grid), dry from -1 m.
struct Run {
    std::vector<MassingProp> props;
    std::vector<MassingPropCounts> counts;
};
Run place(const std::vector<MassingStreet>& streets, const std::vector<MassingPropRule>& rules,
          const std::vector<MassingBox>& boxes = {}, double dryAbove = -1.0) {
    Run run;
    const std::vector<bool> include(boxes.size(), true);
    massingPlaceProps(streets, rules, nullptr, 0, 0, 1.0, 0.0, 0.0, boxes, include, dryAbove, run.props, run.counts);
    return run;
}

// A 0..100 street running east along y = 0, 10 m wide.
std::vector<MassingStreet> eastStreet() { return {street("S-1", "street", 10.0, {{0, 0, 0}, {100, 0, 0}})}; }

} // namespace

TEST_CASE("massing props: the table is read with its defaults and its errors") {
    std::vector<MassingPropRule> rules;
    std::string error;
    const std::string good = "# a comment\n"
                             "id,asset,spacing_m,phase_m,side,inset_m,reach_m,clear_m,turn,dz,cap,kinds,note\n"
                             "LAMP,/Game/Props/A.A,25,5,stagger,0.5,,1.5,90,-0.1,40,street;core_lane,\"a, note\"\n"
                             "BOX,/Game/Props/B.B,12,,wall,0.2,3,,,,,,-\n"
                             "ALL,/Game/Props/C.C,8,,,,,,,,,,\n";
    REQUIRE(importMassingProps(good, rules, &error));
    REQUIRE(rules.size() == 3);
    CHECK(rules[0].id == "LAMP");
    CHECK(rules[0].asset == "/Game/Props/A.A");
    CHECK(near(rules[0].spacing, 25.0));
    CHECK(near(rules[0].phase, 5.0));
    CHECK(rules[0].side == MassingPropSide::Stagger);
    CHECK(near(rules[0].inset, 0.5));
    CHECK(near(rules[0].reach, 0.0));
    CHECK(near(rules[0].clear, 1.5));
    CHECK(near(rules[0].turn, 90.0));
    CHECK(near(rules[0].dz, -0.1));
    CHECK(rules[0].cap == 40);
    REQUIRE(rules[0].kinds.size() == 2);
    CHECK(rules[0].kinds[0] == "street");
    CHECK(rules[0].kinds[1] == "core_lane");
    CHECK(rules[1].side == MassingPropSide::Wall);
    CHECK(near(rules[1].reach, 3.0));
    CHECK(rules[2].side == MassingPropSide::Both); // the default
    CHECK(rules[2].kinds.empty());
    CHECK(rules[2].cap == 0);

    // Only id, asset and spacing_m are required.
    REQUIRE(importMassingProps("id,asset,spacing_m\nP-1,/Game/X.X,4\n", rules, &error));
    CHECK(rules[0].side == MassingPropSide::Both);

    CHECK_FALSE(importMassingProps("id,asset\nP-1,/Game/X.X\n", rules, &error));
    CHECK(error.find("no column spacing_m") != std::string::npos);
    CHECK_FALSE(importMassingProps("", rules, &error));
    CHECK(error.find("no header") != std::string::npos);
    CHECK_FALSE(importMassingProps("id,asset,spacing_m\nP-1,/Game/X.X,0\n", rules, &error));
    CHECK(error.find("line 2") != std::string::npos);
    CHECK(error.find("spacing_m") != std::string::npos);
    CHECK_FALSE(importMassingProps("id,asset,spacing_m\nP-1,,4\n", rules, &error));
    CHECK(error.find("asset") != std::string::npos);
    CHECK_FALSE(importMassingProps("id,asset,spacing_m\np 1,/Game/X.X,4\n", rules, &error));
    CHECK_FALSE(importMassingProps("id,asset,spacing_m\nP-1,/Game/X.X,4\nP-1,/Game/Y.Y,4\n", rules, &error));
    CHECK(error.find("twice") != std::string::npos);
    CHECK_FALSE(importMassingProps("id,asset,spacing_m,side\nP-1,/Game/X.X,4,up\n", rules, &error));
    CHECK(error.find("side") != std::string::npos);
    CHECK_FALSE(importMassingProps("id,asset,spacing_m,phase_m\nP-1,/Game/X.X,4,-1\n", rules, &error));
    CHECK_FALSE(importMassingProps("id,asset,spacing_m,inset_m\nP-1,/Game/X.X,4,-1\n", rules, &error));
    CHECK_FALSE(importMassingProps("id,asset,spacing_m,cap\nP-1,/Game/X.X,4,2.5\n", rules, &error));
    CHECK(error.find("cap") != std::string::npos);
    CHECK_FALSE(importMassingProps("id,asset,spacing_m,turn\nP-1,/Game/X.X,4,left\n", rules, &error));
    CHECK_FALSE(importMassingProps("id,asset,spacing_m,kinds\nP-1,/Game/X.X,4,street;\n", rules, &error));
    CHECK(error.find("kinds") != std::string::npos);
    CHECK_FALSE(importMassingProps("id,asset,spacing_m,kinds\nP-1,/Game/X.X,4,a street\n", rules, &error));
    CHECK_FALSE(importMassingProps("id,asset,spacing_m,spacing_m\nP-1,/Game/X.X,4,4\n", rules, &error));
    CHECK(error.find("twice") != std::string::npos);
    CHECK_FALSE(importMassingProps("id,asset,spacing_m\nP-1,/Game/X.X\n", rules, &error));
    CHECK(error.find("fields") != std::string::npos);
    CHECK(rules.empty()); // cleared first
}

TEST_CASE("massing streets: an optional kind column is read and checked") {
    std::vector<MassingStreet> streets;
    std::string error;
    REQUIRE(importMassingStreets("id,kind,width_m,pts\nS-1,core_lane,6,0:0;10:0\nS-2,,4,1:1\n", streets, &error));
    REQUIRE(streets.size() == 2);
    CHECK(streets[0].kind == "core_lane");
    CHECK(streets[1].kind.empty());
    REQUIRE(importMassingStreets("id,width_m,pts\nS-1,6,0:0;10:0\n", streets, &error));
    CHECK(streets[0].kind.empty());
    CHECK_FALSE(importMassingStreets("id,kind,width_m,pts\nS-1,a lane,6,0:0\n", streets, &error));
    CHECK(error.find("kind") != std::string::npos);
}

TEST_CASE("massing props: kerb props stand every spacing along a kerb, facing the centreline") {
    SUBCASE("left") {
        const Run run = place(eastStreet(), {rule(MassingPropSide::Left, 25.0)});
        REQUIRE(run.props.size() == 5); // 0, 25, 50, 75 and the last point, 100
        for (std::size_t k = 0; k < 5; ++k) {
            CHECK(near(run.props[k].x, 25.0 * static_cast<double>(k)));
            CHECK(near(run.props[k].y, 5.0)); // left of east is north
            CHECK(near(run.props[k].z, 0.0));
            CHECK(near(run.props[k].face, -90.0)); // facing south, to the centreline
            CHECK_FALSE(run.props[k].wall);
            CHECK(run.props[k].street == 0);
            CHECK(run.props[k].rule == 0);
        }
        CHECK(run.counts.size() == 1);
        CHECK(run.counts[0].spots == 5);
        CHECK(run.counts[0].placed == 5);
    }
    SUBCASE("right, with a phase") {
        MassingPropRule r = rule(MassingPropSide::Right, 25.0);
        r.phase = 10.0;
        const Run run = place(eastStreet(), {r});
        REQUIRE(run.props.size() == 4); // 10, 35, 60, 85
        CHECK(near(run.props[0].x, 10.0));
        CHECK(near(run.props[3].x, 85.0));
        CHECK(near(run.props[0].y, -5.0));
        CHECK(near(run.props[0].face, 90.0));
    }
    SUBCASE("both, opposite each other") {
        const Run run = place(eastStreet(), {rule(MassingPropSide::Both, 50.0)});
        REQUIRE(run.props.size() == 6);
        CHECK(run.counts[0].spots == 6);
        CHECK(near(run.props[0].y, 5.0));
        CHECK(near(run.props[1].y, -5.0));
        CHECK(near(run.props[1].x, 0.0));
    }
    SUBCASE("stagger, left first") {
        const Run run = place(eastStreet(), {rule(MassingPropSide::Stagger, 25.0)});
        REQUIRE(run.props.size() == 5);
        CHECK(near(run.props[0].y, 5.0));
        CHECK(near(run.props[1].y, -5.0));
        CHECK(near(run.props[2].y, 5.0));
        CHECK(run.counts[0].spots == 5);
    }
    SUBCASE("an inset moves it in; one past the half width puts it on the centreline") {
        MassingPropRule r = rule(MassingPropSide::Left, 50.0);
        r.inset = 1.0;
        r.dz = 0.25;
        Run run = place(eastStreet(), {r});
        REQUIRE(run.props.size() == 3);
        CHECK(near(run.props[0].y, 4.0));
        CHECK(near(run.props[0].z, 0.25));
        r.inset = 7.0;
        run = place(eastStreet(), {r});
        REQUIRE(run.props.size() == 3);
        CHECK(near(run.props[0].y, 0.0));
    }
}

TEST_CASE("massing props: spots follow a bent street, and a street without length has none") {
    std::vector<MassingStreet> streets = {street("S-1", "lane", 2.0, {{0, 0, 0}, {10, 0, 0}, {10, 0, 0}, {10, 10, 0}}),
                                          street("S-2", "lane", 2.0, {{50, 50, 0}}),
                                          street("S-3", "lane", 2.0, {{60, 60, 0}, {60, 60, 0}})};
    const Run run = place(streets, {rule(MassingPropSide::Left, 5.0)});
    REQUIRE(run.props.size() == 5); // 0, 5, the corner, 15, 20; S-2 and S-3 have none
    CHECK(near(run.props[1].x, 5.0));
    CHECK(near(run.props[1].y, 1.0));
    // At the corner the street already runs north: its left is west.
    CHECK(near(run.props[2].x, 9.0));
    CHECK(near(run.props[2].y, 0.0));
    CHECK(near(run.props[2].face, 0.0));
    CHECK(near(run.props[4].x, 9.0));
    CHECK(near(run.props[4].y, 10.0));
    CHECK(run.counts[0].spots == 5);

    // A phase past the end leaves none.
    MassingPropRule far = rule(MassingPropSide::Left, 5.0);
    far.phase = 30.0;
    CHECK(place(streets, {far}).props.empty());
}

TEST_CASE("massing props: kinds pick the streets a rule stands along") {
    const std::vector<MassingStreet> streets = {street("S-1", "street", 10.0, {{0, 0, 0}, {100, 0, 0}}),
                                                street("S-2", "stair", 4.0, {{0, 300, 0}, {100, 300, 0}})};
    MassingPropRule r = rule(MassingPropSide::Left, 50.0);
    r.kinds = {"street"};
    Run run = place(streets, {r});
    REQUIRE(run.props.size() == 3);
    for (const MassingProp& p : run.props) CHECK(p.street == 0);
    r.kinds.clear();
    run = place(streets, {r});
    CHECK(run.props.size() == 6);
}

TEST_CASE("massing props: a kerb spot near a box over its floor, or in another street, is dropped") {
    std::vector<MassingStreet> streets = eastStreet();
    std::vector<MassingBox> boxes = {
        flatBox(50, 8, 4, 4, 0, 5),    // a kiosk 1 m off the kerb at x 50
        flatBox(25, 5, 2, 2, -1, 0.2), // a low deck under the kerb at x 25
        flatBox(75, 5, 2, 2, 4, 5),    // a bridge high over the kerb at x 75
    };
    MassingPropRule r = rule(MassingPropSide::Left, 25.0);
    Run run = place(streets, {r}, boxes);
    CHECK(run.props.size() == 5); // the kiosk is 1 m off, past a clear of 0
    CHECK(near(run.props[1].z, 0.2)); // stands on the deck
    r.clear = 1.5;
    run = place(streets, {r}, boxes);
    REQUIRE(run.props.size() == 4);
    CHECK(run.counts[0].blocked == 1);
    for (const MassingProp& p : run.props) CHECK_FALSE(near(p.x, 50.0));

    // A tall box over the kerb itself blocks it whatever the clear: a roof is never a floor.
    boxes = {flatBox(50, 5, 2, 2, 0, 4)};
    r.clear = 0.0;
    run = place(streets, {r}, boxes);
    CHECK(run.props.size() == 4);
    CHECK(run.counts[0].blocked == 1);

    // A cross street at x 50, 8 m wide: the kerb spot there is on its carriageway.
    streets.push_back(street("S-2", "lane", 8.0, {{50, -40, 0}, {50, 40, 0}}));
    r.kinds = {"street"};
    run = place(streets, {r});
    REQUIRE(run.props.size() == 4);
    CHECK(run.counts[0].inRoad == 1);
    CHECK(run.counts[0].spots == 5);
}

TEST_CASE("massing props: a wall prop stands against the nearest tall face in reach, left first") {
    const std::vector<MassingStreet> streets = eastStreet();
    MassingPropRule r = rule(MassingPropSide::Wall, 25.0);
    r.reach = 3.0;
    SUBCASE("a face on the left") {
        const Run run = place(streets, {r}, {flatBox(50, 9, 20, 4, 0, 8)}); // its south face at y 7
        REQUIRE(run.props.size() == 1);
        CHECK(run.props[0].wall);
        CHECK(near(run.props[0].x, 50.0));
        CHECK(near(run.props[0].y, 7.0));
        CHECK(near(run.props[0].face, -90.0)); // out of the face, south
        CHECK(run.counts[0].spots == 5);
        CHECK(run.counts[0].noWall == 4);
        CHECK(run.counts[0].placed == 1);
    }
    SUBCASE("a face on the right when the left has none") {
        const Run run = place(streets, {r}, {flatBox(50, -9, 20, 4, 0, 8)});
        REQUIRE(run.props.size() == 1);
        CHECK(near(run.props[0].y, -7.0));
        CHECK(near(run.props[0].face, 90.0));
    }
    SUBCASE("left first when both have one") {
        const Run run = place(streets, {r}, {flatBox(50, -9, 20, 4, 0, 8), flatBox(50, 9, 20, 4, 0, 8)});
        REQUIRE(run.props.size() == 1);
        CHECK(near(run.props[0].y, 7.0));
    }
    SUBCASE("the nearest face, out of reach or too low, gives none") {
        CHECK(place(streets, {r}, {flatBox(50, 11, 20, 4, 0, 8)}).props.empty()); // its face at 9 > 5 + 3
        Run run = place(streets, {r}, {flatBox(50, 9, 20, 4, 0, 1)});           // a low wall
        CHECK(run.props.empty());
        CHECK(run.counts[0].noWall == 5);
        // The low wall in front (its face at 6) hides the tall face behind it (at 8, in reach).
        run = place(streets, {r}, {flatBox(50, 9.5, 20, 3, 0, 9)});
        CHECK(run.props.size() == 1);
        run = place(streets, {r}, {flatBox(50, 9.5, 20, 3, 0, 9), flatBox(50, 6.5, 20, 1, 0, 1)});
        CHECK(run.props.empty());
    }
    SUBCASE("a box over the centreline is no face") {
        CHECK(place(streets, {r}, {flatBox(50, 0, 4, 30, 0, 8)}).props.empty());
    }
    SUBCASE("a turned box's face sets the facing") {
        MassingBox b = flatBox(50, 12, 10, 10, 0, 8);
        b.rot = 45.0;
        const Run run = place(streets, {r}, {b});
        REQUIRE(run.props.size() == 1);
        CHECK(near(run.props[0].x, 50.0));
        CHECK(near(run.props[0].y, 12.0 - 5.0 * std::sqrt(2.0), 1e-6)); // the corner
        // It enters at the corner; either face meeting there faces south-east or south-west.
        CHECK((near(run.props[0].face, -45.0) || near(run.props[0].face, -135.0)));
    }
}

TEST_CASE("massing props: steep ground and wet floors drop a spot; a deck keeps it") {
    const std::vector<MassingStreet> streets = eastStreet();
    const MassingPropRule r = rule(MassingPropSide::Left, 25.0);
    SUBCASE("wet") {
        const Run run = place(streets, {r}, {}, 0.5);
        CHECK(run.props.empty());
        CHECK(run.counts[0].wet == 5);
        // A deck under the whole street holds them dry, on its top.
        const Run deck = place(streets, {r}, {flatBox(50, 0, 120, 20, -1, 1)}, 0.5);
        REQUIRE(deck.props.size() == 5);
        CHECK(near(deck.props[0].z, 1.0));
    }
    SUBCASE("steep") {
        // Ground rising 1 m per m eastward, then a gentle 0.4 m per m.
        constexpr std::size_t nx = 131, ny = 31;
        for (const double rise : {1.0, 0.4}) {
            std::vector<std::uint16_t> grid(nx * ny);
            for (std::size_t iy = 0; iy < ny; ++iy)
                for (std::size_t ix = 0; ix < nx; ++ix)
                    grid[iy * nx + ix] = static_cast<std::uint16_t>(32768.0 + rise * (static_cast<double>(ix) - 10.0) * 128.0 + 1280.0);
            std::vector<MassingProp> props;
            std::vector<MassingPropCounts> counts;
            massingPlaceProps(streets, {r}, grid.data(), nx, ny, 1.0, -10.0, -10.0, {}, {}, -1000.0, props, counts);
            if (rise > 0.5) {
                CHECK(props.empty());
                CHECK(counts[0].steep == 5);
            } else {
                CHECK(props.size() == 5);
                CHECK(near(props[1].z, 10.0 + 0.4 * 25.0, 1e-3));
            }
        }
    }
}

TEST_CASE("massing props: a cap keeps an even spread, rule by rule") {
    MassingPropRule a = rule(MassingPropSide::Left, 25.0);
    a.cap = 2;
    MassingPropRule b = rule(MassingPropSide::Right, 50.0);
    b.id = "P-2";
    const Run run = place(eastStreet(), {a, b});
    REQUIRE(run.props.size() == 5);
    CHECK(near(run.props[0].x, 0.0)); // the 0th and 2nd of 5
    CHECK(near(run.props[1].x, 50.0));
    CHECK(run.counts[0].placed == 2);
    CHECK(run.counts[0].capped == 3);
    CHECK(run.props[2].rule == 1);
    CHECK(run.counts[1].placed == 3);
    CHECK(run.counts[1].capped == 0);
}

TEST_CASE("massing props: a mesh is placed by its bounds, a wall prop's back on its face") {
    MassingProp p;
    p.x = 500;
    p.y = 500;
    p.z = 2;
    p.face = 90; // north in the plan
    MassingKitPlace out;
    REQUIRE(massingPropPlacement(p, 1000.0, {-1, -0.5, 0}, {1, 0.5, 3}, 0.0, 0.0, out));
    CHECK(near(out.yaw, -90.0));
    CHECK(near(out.at.x, 0.0));
    CHECK(near(out.at.y, 0.0));
    CHECK(near(out.at.z, 2.0));
    CHECK(near(out.scale.x, 1.0));

    // Off-centre bounds: their centre (1, 0.5), turned -90, lands on the point; their bottom (-1) on z.
    REQUIRE(massingPropPlacement(p, 1000.0, {0, 0, -1}, {2, 1, 2}, 0.0, 0.0, out));
    CHECK(near(out.at.x, -0.5));
    CHECK(near(out.at.y, 1.0));
    CHECK(near(out.at.z, 3.0));

    // A wall prop facing east sits its half depth plus the inset out from the face.
    p.face = 0;
    p.wall = true;
    REQUIRE(massingPropPlacement(p, 1000.0, {0, 0, 0}, {2, 1, 2}, 0.0, 0.2, out));
    CHECK(near(out.yaw, 0.0));
    CHECK(near(out.at.x, 0.2)); // the centre at 1.2, less the bounds' centre 1
    CHECK(near(out.at.y, -0.5));
    // Turned a quarter, its depth along the facing is its Y.
    REQUIRE(massingPropPlacement(p, 1000.0, {-1, -0.5, 0}, {1, 0.5, 2}, 90.0, 0.0, out));
    CHECK(near(out.yaw, 90.0));
    CHECK(near(out.at.x, 0.5));
    CHECK(near(out.at.y, 0.0));

    const MassingKitPlace before = out;
    CHECK_FALSE(massingPropPlacement(p, 1000.0, {1, 0, 0}, {0, 1, 1}, 0.0, 0.0, out));
    CHECK(near(out.at.x, before.at.x));
}

TEST_CASE("massing props: line_m keeps a prop to a frontage; with none near, the spot is open") {
    std::vector<MassingPropRule> rules;
    std::string error;
    REQUIRE(importMassingProps("id,asset,spacing_m,line_m\nP-1,/Game/X.X,4,3.5\nP-2,/Game/X.X,4,\n", rules, &error));
    CHECK(near(rules[0].line, 3.5));
    CHECK(rules[1].line == 0.0);
    CHECK_FALSE(importMassingProps("id,asset,spacing_m,line_m\nP-1,/Game/X.X,4,-1\n", rules, &error));
    CHECK(error.find("line_m") != std::string::npos);

    // Left kerb spots at x 0, 50 and 100 (y 5); a wall 1 m behind the one at x 50.
    const std::vector<MassingStreet> streets = eastStreet();
    std::vector<MassingBox> boxes = {flatBox(50, 8, 20, 4, 0, 10)};
    MassingPropRule r = rule(MassingPropSide::Left, 50.0);
    Run run = place(streets, {r}, boxes);
    CHECK(run.props.size() == 3); // no line: every spot
    r.line = 2.0;
    run = place(streets, {r}, boxes);
    REQUIRE(run.props.size() == 1);
    CHECK(near(run.props[0].x, 50.0));
    CHECK(run.counts[0].open == 2);
    r.line = 0.5; // the wall is 1 m off
    run = place(streets, {r}, boxes);
    CHECK(run.props.empty());
    CHECK(run.counts[0].open == 3);

    // A low box (a planter) or one high overhead (a bridge) is no frontage.
    r.line = 2.0;
    boxes = {flatBox(50, 8, 20, 4, 0, 1.5)};
    CHECK(place(streets, {r}, boxes).counts[0].open == 3);
    boxes = {flatBox(50, 8, 20, 4, 3, 10)};
    CHECK(place(streets, {r}, boxes).counts[0].open == 3);
    boxes = {flatBox(50, 8, 20, 4, 0, 10)};
    boxes[0].sloped = true;
    CHECK(place(streets, {r}, boxes).counts[0].open == 3);

    // The same test on its own.
    const std::vector<bool> all = {true};
    boxes = {flatBox(50, 8, 20, 4, 0, 10)};
    CHECK(massingBuiltFaceWithin(boxes, all, 50, 5, 0.0, 1.0));
    CHECK_FALSE(massingBuiltFaceWithin(boxes, all, 50, 5, 0.0, 0.9));
    CHECK_FALSE(massingBuiltFaceWithin(boxes, all, 50, 5, 8.0, 1.0)); // the floor up at 8 m: under 2.5 m of wall left
    CHECK_FALSE(massingBuiltFaceWithin(boxes, {false}, 50, 5, 0.0, 1.0));
}

TEST_CASE("massing props: feet come no nearer than the gap, turned or not") {
    const MassingFoot a{0, 0, 1, 1, 0};
    MassingFoot b{2, 0, 1, 1, 0};
    CHECK_FALSE(massingFeetOverlap(a, b, 0.0)); // touching
    CHECK(massingFeetOverlap(a, b, kMassingPropGap));
    b.x = 2.1;
    CHECK_FALSE(massingFeetOverlap(a, b, kMassingPropGap));
    b.x = 1.5;
    CHECK(massingFeetOverlap(a, b, 0.0));
    CHECK(massingFeetOverlap(b, a, 0.0));
    // A diamond: a square turned 45 degrees reaches sqrt(2) along x.
    b = {2.3, 0, 1, 1, 45};
    CHECK(massingFeetOverlap(a, b, 0.0));
    b.x = 2.5;
    CHECK_FALSE(massingFeetOverlap(a, b, 0.0));
    // Diagonal neighbours whose bounding circles meet but whose sides don't.
    b = {2.05, 1.5, 1, 1, 0};
    CHECK_FALSE(massingFeetOverlap(a, b, 0.0));
    // A long thin foot (a kerb) across a small one.
    const MassingFoot kerb{0, 0, 6, 0.1, 90};
    CHECK(massingFeetOverlap(kerb, MassingFoot{0.3, 5, 0.25, 0.25, 0}, 0.0));
    CHECK_FALSE(massingFeetOverlap(kerb, MassingFoot{0.5, 5, 0.25, 0.25, 0}, 0.1));
    // A foot of no area is a point.
    CHECK(massingFeetOverlap(a, MassingFoot{0.5, 0.5, 0, 0, 0}, 0.0));
}

TEST_CASE("massing props: a prop's foot lies where its mesh is placed") {
    MassingPropRule r = rule(MassingPropSide::Left, 10.0);
    r.footX = 2.0;
    r.footY = 1.0;
    r.turn = 90.0;
    MassingProp p;
    p.x = 10;
    p.y = 20;
    p.face = 90; // facing north
    MassingFoot f = massingPropFoot(p, r);
    CHECK(near(f.x, 10.0));
    CHECK(near(f.y, 20.0));
    CHECK(near(f.hx, 1.0));
    CHECK(near(f.hy, 0.5));
    CHECK(near(f.rot, 0.0)); // engine yaw -90 + 90: the mesh's X along plan east

    // A wall prop facing north, unturned: its depth (X) set out from the face with the inset.
    r.turn = 0.0;
    r.inset = 0.05;
    r.footX = 1.0;
    r.footY = 3.0;
    p.wall = true;
    f = massingPropFoot(p, r);
    CHECK(near(f.x, 10.0));
    CHECK(near(f.y, 20.55));
    CHECK(near(f.rot, 90.0));
    MassingKitPlace place;
    REQUIRE(massingPropPlacement(p, 1000.0, {-0.5, -1.5, 0}, {0.5, 1.5, 2}, r.turn, r.inset, place));
    const MassingVec centre = massingEnginePoint(1000.0, f.x, f.y, 0.0);
    CHECK(near(place.at.x, centre.x));
    CHECK(near(place.at.y, centre.y));
    // The mesh's X runs along the facing: plan north, the foot's own x at 90 degrees.
    CHECK(near(massingEngineYaw(f.rot), place.yaw));
}

TEST_CASE("massing props: a prop never stands on a foot already taken") {
    const std::vector<MassingStreet> streets = eastStreet();
    MassingPropRule a = rule(MassingPropSide::Left, 50.0);
    a.footX = a.footY = 1.0;
    MassingPropRule b = a;
    b.id = "P-2";
    Run run = place(streets, {a, b});
    CHECK(run.counts[0].placed == 3);
    CHECK(run.counts[1].placed == 0);
    CHECK(run.counts[1].crowded == 3); // every spot on one of the first rule's
    // A rule with no foot is never crowded and crowds nothing.
    b.footX = 0.0;
    run = place(streets, {a, b});
    CHECK(run.counts[1].placed == 3);

    // A foot given as taken.
    const std::vector<MassingFoot> taken = {{50, 5.5, 0.5, 0.5, 0}};
    const std::vector<bool> none;
    massingPlaceProps(streets, {a}, nullptr, 0, 0, 1.0, 0.0, 0.0, {}, none, -1.0, run.props, run.counts, &taken);
    CHECK(run.props.size() == 2);
    CHECK(run.counts[0].crowded == 1);

    // Its own spots crowd each other: 0.6 m apart with a 1 m foot, every other one stands.
    a.spacing = 0.6;
    run = place(streets, {a});
    REQUIRE(run.props.size() >= 2);
    CHECK(near(run.props[0].x, 0.0));
    CHECK(near(run.props[1].x, 1.2));
    CHECK(run.counts[0].placed + run.counts[0].crowded == run.counts[0].spots);
}

TEST_CASE("massing props: another street's pavement is not its road") {
    // A cross street at x 50, 10 m wide; left kerb spots of the east street at x 3, 13, ..., 93 (y 5).
    std::vector<MassingStreet> streets = eastStreet();
    streets.push_back(street("S-2", "lane", 10.0, {{50, -40, 0}, {50, 40, 0}}));
    MassingPropRule r = rule(MassingPropSide::Left, 10.0);
    r.phase = 3.0;
    r.kinds = {"street"};
    Run run = place(streets, {r});
    CHECK(run.counts[0].inRoad == 1); // x 53, 3 m off its centreline
    streets[1].pavement = 3.0;        // a 4 m carriageway: x 53 stands on its pavement
    run = place(streets, {r});
    CHECK(run.counts[0].inRoad == 0);
    CHECK(run.props.size() == 10);
}

TEST_CASE("massing props: the gap to the nearest kerb or frontage line") {
    std::vector<MassingStreet> streets = eastStreet();
    const std::vector<bool> all = {true};
    std::vector<MassingBox> boxes = {flatBox(50, 12, 20, 4, 0, 10)}; // a wall face along y 10
    // No pavements: only the frontage counts.
    CHECK(near(massingPropLineGap(streets, boxes, all, 50, 5, 0.0), 5.0));
    streets[0].pavement = 2.0; // kerbs along y 3 and -3
    CHECK(near(massingPropLineGap(streets, boxes, all, 50, 4, 0.0), 1.0));
    CHECK(near(massingPropLineGap(streets, boxes, all, 50, 0, 0.0), 3.0));
    CHECK(near(massingPropLineGap(streets, boxes, all, 50, -5, 0.0), 2.0));
    CHECK(near(massingPropLineGap(streets, boxes, all, 50, 9, 0.0), 1.0)); // the wall nearer than the kerb
    // A wall standing lower than kMassingPropWallHeight over the floor is no frontage.
    CHECK(near(massingPropLineGap(streets, boxes, all, 50, 9, 8.0), 6.0));
    streets[0].pavement = 0.0;
    CHECK(std::isinf(massingPropLineGap(streets, {}, {}, 50, 9, 0.0)));
}
