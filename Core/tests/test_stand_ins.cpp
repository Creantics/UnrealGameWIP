// The strip's stand-in compositions and their point tables (ftcore/StandIns.h).

#include "ftcore/StandIns.h"

#include <ostream>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;

StandInPoint at(StandInKind kind, double x, double y) {
    StandInPoint p;
    p.kind = kind;
    p.x = x;
    p.y = y;
    return p;
}

// The highest point of a part, for the axis-aligned (unrolled, unpitched) ones.
double topOf(const StandInPart& part) { return part.dz + 0.5 * part.sz; }

} // namespace

TEST_CASE("stand-ins: every kind has parts, within the cap, repeatable") {
    for (std::size_t k = 0; k < kStandInKinds; ++k) {
        const StandInPoint p = at(static_cast<StandInKind>(k), 812.4, 533.1);
        StandInPart a[kStandInPartsMax], b[kStandInPartsMax];
        const std::size_t n = standInParts(p, a);
        CAPTURE(standInKindName(p.kind));
        CHECK(n > 0);
        CHECK(n <= kStandInPartsMax);
        CHECK(standInParts(p, b) == n);
        for (std::size_t i = 0; i < n; ++i) {
            CHECK(a[i].dx == b[i].dx);
            CHECK(a[i].dz == b[i].dz);
            CHECK(a[i].sx == b[i].sx);
            CHECK(a[i].sx > 0.f);
            CHECK(a[i].sy > 0.f);
            CHECK(a[i].sz > 0.f);
        }
    }
    CHECK(standInParts(at(StandInKind::StreetTree, 0, 0), nullptr) == 0);
}

TEST_CASE("stand-ins: the per-frame budget: trees 3-4, railing 3, mast 2, back house 1") {
    StandInPart parts[kStandInPartsMax];
    CHECK(standInParts(at(StandInKind::StreetTree, 1, 2), parts) == 3);
    CHECK(standInParts(at(StandInKind::ParkTree, 1, 2), parts) == 4);
    CHECK(standInParts(at(StandInKind::Shrub, 1, 2), parts) == 2);
    CHECK(standInParts(at(StandInKind::Railing, 1, 2), parts) == 3);
    CHECK(standInParts(at(StandInKind::LampMast, 1, 2), parts) == 2);
    CHECK(standInParts(at(StandInKind::HouseBack, 1, 2), parts) == 1);
    CHECK(standInParts(at(StandInKind::House, 1, 2), parts) == 3);
    CHECK(standInParts(at(StandInKind::Landmark, 1, 2), parts) <= 12);
}

TEST_CASE("stand-ins: a street tree's crown stands on a clear trunk; positions differ by seed") {
    StandInPart parts[kStandInPartsMax];
    standInParts(at(StandInKind::StreetTree, 700, 410), parts);
    CHECK(parts[0].shape == StandInShape::Cylinder);
    CHECK(parts[0].collide == StandInCollide::Solid);
    CHECK(parts[0].dz == doctest::Approx(0.5 * parts[0].sz));
    const double crownBottom = parts[1].dz - 0.5 * parts[1].sz;
    CHECK(crownBottom > 2.5);
    CHECK(parts[1].sx >= 4.5f * 0.99f);
    CHECK(parts[1].sx <= 6.3f * 1.01f);
    CHECK(topOf(parts[1]) < 12.0);
    StandInPart other[kStandInPartsMax];
    standInParts(at(StandInKind::StreetTree, 709, 410), other);
    CHECK((other[1].sx != parts[1].sx || other[2].dx != parts[2].dx));
}

TEST_CASE("stand-ins: a house roof's halves meet at the ridge, pitched 25-35 degrees, overhanging the eaves") {
    for (int i = 0; i < 20; ++i) {
        StandInPoint p = at(StandInKind::House, 700 + 7.3 * i, 420 + 3.1 * i);
        p.len = 11.0;
        p.wid = 9.0;
        p.rot = 0.0;
        StandInPart parts[kStandInPartsMax];
        REQUIRE(standInParts(p, parts) == 3);
        const StandInPart& left = parts[0];
        const StandInPart& right = parts[1];
        const double pitch = left.roll;
        CHECK(pitch >= 25.0);
        CHECK(pitch <= 35.0);
        CHECK(right.roll == doctest::Approx(-pitch));
        CHECK(left.dy < 0.f);
        CHECK(right.dy > 0.f);
        // The left half's (y < 0) mid-plane reaches the ridge at y = 0: its inner edge is half its slope width in.
        const double c = std::cos(pitch * kDeg), s = std::sin(pitch * kDeg);
        const double rise = 0.5 * p.wid * std::tan(pitch * kDeg);
        const double lift = 0.5 * left.sz / c;
        for (const StandInPart* half : {&left, &right}) {
            const double inward = half->dy < 0.f ? 1.0 : -1.0;
            const double innerY = half->dy + inward * 0.5 * half->sy * c;
            const double innerZ = half->dz - lift + 0.5 * half->sy * s;
            CHECK(innerY == doctest::Approx(0.0).epsilon(1e-4));
            CHECK(innerZ == doctest::Approx(rise).epsilon(1e-4));
            // Its outer edge overhangs the wall (y beyond wid / 2) and dips under the eaves.
            const double outerY = half->dy - inward * 0.5 * half->sy * c;
            const double outerZ = half->dz - lift - 0.5 * half->sy * s;
            CHECK(std::abs(outerY) > 0.5 * p.wid);
            CHECK(outerZ < 0.0);
        }
        // A positive roll lifts a part's +y edge (its inner one on the y < 0 half), so that half falls outward.
        CHECK(left.roll > 0.f);
        // The gable infill: a 45-degree cube whose top corner stays under the ridge.
        const StandInPart& gable = parts[2];
        CHECK(gable.roll == doctest::Approx(45.0));
        CHECK(gable.dz + gable.sz * std::sqrt(0.5) == doctest::Approx(rise).epsilon(1e-4));
        CHECK(gable.sx == doctest::Approx(p.len));
    }
}

TEST_CASE("stand-ins: a back house is one 45-degree gable over the box's width") {
    StandInPoint p = at(StandInKind::HouseBack, 750, 600);
    p.len = 10.0;
    p.wid = 8.0;
    StandInPart parts[kStandInPartsMax];
    REQUIRE(standInParts(p, parts) == 1);
    CHECK(parts[0].roll == doctest::Approx(45.0));
    // Its diamond's half diagonal is half the width: the gable spans the box exactly.
    CHECK(parts[0].sy * std::sqrt(0.5) == doctest::Approx(4.0));
}

TEST_CASE("stand-ins: glass towers are 30-50 floors on 30-45 m, landmark 250-300 m, slender") {
    for (int i = 0; i < 40; ++i) {
        StandInPart parts[kStandInPartsMax];
        const std::size_t n = standInParts(at(StandInKind::GlassTower, 820 + 13.0 * i, 900 - 7.0 * i), parts);
        double top = 0.0, wide = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            top = std::max(top, topOf(parts[k]));
            wide = std::max(wide, static_cast<double>(parts[k].sx));
        }
        CHECK(top >= 30 * 3.5 - 0.01);
        CHECK(top <= 50 * 3.5 + 5.0 + 0.01); // the plant box over the crown
        CHECK(wide >= 30.0);
        CHECK(wide <= 45.0 + 0.41);
        CHECK(parts[1].collide == StandInCollide::Solid);
    }
    StandInPoint given = at(StandInKind::GlassTower, 0, 0);
    given.floors = 40;
    given.len = 36;
    given.wid = 32;
    StandInPart parts[kStandInPartsMax];
    const std::size_t n = standInParts(given, parts);
    double top = 0.0;
    for (std::size_t k = 0; k < n; ++k) top = std::max(top, topOf(parts[k]));
    CHECK(top == doctest::Approx(140.0 + 5.0));

    for (int i = 0; i < 20; ++i) {
        const std::size_t m = standInParts(at(StandInKind::Landmark, 640 + 11.0 * i, 700 + 5.0 * i), parts);
        double peak = 0.0, wide = 0.0;
        for (std::size_t k = 0; k < m; ++k) {
            const double reach = parts[k].roll != 0.f ? parts[k].dz + parts[k].sz * std::sqrt(0.5) : topOf(parts[k]);
            peak = std::max(peak, reach);
            if (k > 0) wide = std::max(wide, static_cast<double>(std::max(parts[k].sx, parts[k].sy)));
        }
        CHECK(peak >= 250.0 - 0.01);
        CHECK(peak <= 300.0 + 0.01);
        CHECK(peak / wide >= 12.0); // slender: at least 12 times as tall as its widest stage
    }
}

TEST_CASE("stand-ins: parts turn with the point's rot") {
    StandInPoint p = at(StandInKind::LampMast, 885, 500);
    p.rot = 90.0;
    StandInPart parts[kStandInPartsMax];
    REQUIRE(standInParts(p, parts) == 2);
    // The head reaches along rot: at 90 degrees, north (+y).
    CHECK(parts[1].dx == doctest::Approx(0.0).epsilon(1e-5));
    CHECK(parts[1].dy == doctest::Approx(0.65));
    CHECK(parts[1].yaw == doctest::Approx(90.0));
    CHECK(parts[0].dz + 0.5 * parts[0].sz == doctest::Approx(8.0));
}

TEST_CASE("stand-ins: point tables by column name, spans from their ends, kinds skipped") {
    const char* csv =
        "# a test table\n"
        "id,kind,cx,cy,z,rot,len,wid,floors,back\n"
        "T1,street_tree,710.5,420,3.2,,,,,\n"
        "H1,house,720,430,,15,10,8,,1\n"
        "H2,House,722,440,,15,10,8,,0\n"
        "G1,glass_tower,800,600,2,0,36,32,44,\n"
        "Q1,bench,700,400,,,,,,\n";
    std::vector<StandInPoint> pts;
    std::string error;
    StandInImportStats st;
    REQUIRE(importStandIns(csv, StandInKind::Count, pts, &error, &st));
    CHECK(error.empty());
    CHECK(st.rows == 5);
    CHECK(st.kept == 4);
    CHECK(st.skipped == 1);
    REQUIRE(pts.size() == 4);
    CHECK(pts[0].kind == StandInKind::StreetTree);
    CHECK(pts[0].hasZ);
    CHECK(pts[0].z == doctest::Approx(3.2));
    CHECK(pts[0].id == "T1");
    CHECK(pts[1].kind == StandInKind::HouseBack);
    CHECK(!pts[1].hasZ);
    CHECK(pts[1].rot == doctest::Approx(15.0));
    CHECK(pts[2].kind == StandInKind::House);
    CHECK(pts[3].floors == 44);
    CHECK(pts[3].len == doctest::Approx(36.0));

    const char* spans =
        "x0,y0,x1,y1,z\n"
        "880,410,880,435,1.5\n";
    std::vector<StandInPoint> rails;
    REQUIRE(importStandIns(spans, StandInKind::Railing, rails, &error));
    REQUIRE(rails.size() == 1);
    CHECK(rails[0].kind == StandInKind::Railing);
    CHECK(rails[0].x == doctest::Approx(880.0));
    CHECK(rails[0].y == doctest::Approx(422.5));
    CHECK(rails[0].len == doctest::Approx(25.0));
    CHECK(rails[0].rot == doctest::Approx(90.0));

    const char* roofs =
        "kind,x,y,z1,sx,sy,rot\n"
        "house,700,500,9.5,12,9,30\n";
    std::vector<StandInPoint> houses;
    REQUIRE(importStandIns(roofs, StandInKind::Count, houses, &error));
    REQUIRE(houses.size() == 1);
    CHECK(houses[0].z == doctest::Approx(9.5));
    CHECK(houses[0].len == doctest::Approx(12.0));
    CHECK(houses[0].wid == doctest::Approx(9.0));
}

TEST_CASE("stand-ins: bad tables fail with the line") {
    std::vector<StandInPoint> pts;
    std::string error;
    CHECK_FALSE(importStandIns("kind,a,b\ntree,1,2\n", StandInKind::Count, pts, &error));
    CHECK(error.find("line 1") != std::string::npos);
    CHECK_FALSE(importStandIns("x,y\n1,2\n", StandInKind::Count, pts, &error)); // no kind column, no default
    CHECK_FALSE(importStandIns("kind,x,y\ntree,1,nope\n", StandInKind::Count, pts, &error));
    CHECK(error.find("line 2") != std::string::npos);
    CHECK_FALSE(importStandIns("kind,x,y,z\ntree,1\n", StandInKind::Count, pts, &error));
    CHECK_FALSE(importStandIns("# only a comment\n", StandInKind::Count, pts, &error));
    CHECK(pts.empty());
    CHECK(importStandIns("x,y\n1,2\n", StandInKind::Shrub, pts, &error));
    CHECK(pts.size() == 1);
}

TEST_CASE("stand-ins: round 9's tables: trees, houses, towers (G) and the quay points (S)") {
    std::string error;
    const char* trees =
        "# sol_r9_trees.csv\n"
        "id,kind,x,y,z,height_m,canopy_m,host\n"
        "TR-1,street,720,420,1.2,9,6,ST-0079\n"
        "TR-2,precinct,740,600,1.5,,,R9-F-PRECINCT\n"
        "TR-3,forecourt,745,602,1.5,12,9,R9-F-FORECOURT\n";
    std::vector<StandInPoint> pts;
    REQUIRE(importStandIns(trees, StandInKind::StreetTree, pts, &error));
    REQUIRE(pts.size() == 3);
    CHECK(pts[0].kind == StandInKind::StreetTree);
    CHECK(pts[1].kind == StandInKind::ParkTree);
    CHECK(pts[2].kind == StandInKind::ParkTree);
    StandInPart parts[kStandInPartsMax];
    const std::size_t n = standInParts(pts[0], parts);
    CHECK(parts[1].sx == doctest::Approx(6.0));
    CHECK(topOf(parts[1]) == doctest::Approx(9.0)); // the given height is the crown's top
    double peak = 0.0;
    for (std::size_t k = 0; k < n; ++k) peak = std::max(peak, topOf(parts[k]));
    CHECK(peak < 9.0 + 0.2 * 6.0);

    const char* houses =
        "id,box_id,cx,cy,sx,sy,rot,eave_z,pitch_deg,ridge,hidden\n"
        "HR-1,R9-A-R1-L1,710,430,12,8,0,9.4,30,u,0\n"
        "HR-2,R9-A-R1-L2,710,440,8,12,0,9.4,28,w,1\n";
    pts.clear();
    REQUIRE(importStandIns(houses, StandInKind::House, pts, &error));
    REQUIRE(pts.size() == 2);
    CHECK(pts[0].kind == StandInKind::House);
    CHECK(pts[0].hasZ);
    CHECK(pts[0].z == doctest::Approx(9.4));
    CHECK(pts[0].len == doctest::Approx(12.0));
    CHECK(standInParts(pts[0], parts) == 3);
    CHECK(parts[0].roll == doctest::Approx(30.0));
    CHECK(pts[1].kind == StandInKind::HouseBack);
    CHECK(pts[1].len == doctest::Approx(12.0)); // ridge along w: turned a quarter
    CHECK(pts[1].wid == doctest::Approx(8.0));
    CHECK(pts[1].rot == doctest::Approx(90.0));

    const char* towers =
        "id,kind,cx,cy,z0,sx,sy,rot,floors,height_m,note\n"
        "TW-1,glass,820,700,6,36,32,0,40,140,podium 6 m\n"
        "LM-1,landmark,640,760,30,20,20,0,,280,closes ST-0081\n";
    pts.clear();
    REQUIRE(importStandIns(towers, StandInKind::Count, pts, &error));
    REQUIRE(pts.size() == 2);
    CHECK(pts[0].kind == StandInKind::GlassTower);
    CHECK(pts[0].z == doctest::Approx(6.0));
    CHECK(pts[0].h == doctest::Approx(140.0));
    CHECK(pts[1].kind == StandInKind::Landmark);
    const std::size_t m = standInParts(pts[1], parts);
    double reach = 0.0;
    for (std::size_t k = 0; k < m; ++k)
        reach = std::max(reach, parts[k].roll != 0.f ? parts[k].dz + parts[k].sz * std::sqrt(0.5) : topOf(parts[k]));
    CHECK(reach == doctest::Approx(280.0));

    const char* quay =
        "kind,id,group,x,y,z0,z1,rot,length_m,width_m,slope,deck\n"
        "railing_span,R1,QR-1,890,450,1.8,2.9,90,2,0.06,0,QD-1\n"
        "railing_span,R2,FB-01,900,470,2.0,3.1,0,2,0.06,0.5,FB-01\n"
        "lamp_mast,M1,QR-1,890,475,1.8,9.8,180,0.4,0.4,0,QD-1\n"
        "bridge_deck,D1,FB-01,905,470,5.6,6.0,0,20,3,0,FB-01\n"
        "bridge_stair,S1,FB-01,894,470,4.0,4.3,0,6,2.4,0.5,FB-01\n"
        "bridge_pier,P1,FB-01,905,470,0.5,5.6,0,0.8,0.8,0,FB-01\n";
    pts.clear();
    REQUIRE(importStandIns(quay, StandInKind::Count, pts, &error));
    REQUIRE(pts.size() == 6);
    CHECK(pts[0].kind == StandInKind::Railing);
    CHECK(standInParts(pts[0], parts) == 3);
    CHECK(parts[0].dz == doctest::Approx(1.07));
    CHECK(parts[0].pitch == doctest::Approx(0.0));
    standInParts(pts[1], parts);
    CHECK(parts[0].pitch == doctest::Approx(std::atan(0.5) / kDeg));
    CHECK(pts[2].kind == StandInKind::LampMast);
    standInParts(pts[2], parts);
    CHECK(parts[0].sz == doctest::Approx(8.0)); // z1 - z0
    CHECK(parts[1].dx == doctest::Approx(-0.65)); // the head faces rot 180
    CHECK(pts[3].kind == StandInKind::BridgeDeck);
    REQUIRE(standInParts(pts[3], parts) == 1);
    CHECK(parts[0].collide == StandInCollide::Walk);
    CHECK(parts[0].dz + 0.5 * parts[0].sz == doctest::Approx(0.4)); // its top at z1 over z0
    CHECK(pts[4].kind == StandInKind::BridgeStair);
    REQUIRE(standInParts(pts[4], parts) == 1);
    CHECK(parts[0].collide == StandInCollide::Walk);
    CHECK(parts[0].pitch == doctest::Approx(std::atan(0.5) / kDeg));
    CHECK(parts[0].sx == doctest::Approx(6.0 / std::cos(std::atan(0.5))));
    CHECK(pts[5].kind == StandInKind::BridgePier);
    REQUIRE(standInParts(pts[5], parts) == 1);
    CHECK(parts[0].collide == StandInCollide::Solid);
    CHECK(parts[0].sz == doctest::Approx(5.1));
}
