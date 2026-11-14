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

TEST_CASE("stand-ins: the per-frame budget: trees 2, railing 3, mast 2, back house 1") {
    StandInPart parts[kStandInPartsMax];
    CHECK(standInParts(at(StandInKind::StreetTree, 1, 2), parts) == 2);
    CHECK(standInParts(at(StandInKind::ParkTree, 1, 2), parts) == 2);
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
    // The canopy: its foot (the fork) on the trunk's top, its leaves from its fork share up.
    REQUIRE(parts[1].shape == StandInShape::Canopy);
    CHECK(parts[1].collide == StandInCollide::None);
    CHECK(parts[1].variant == 0);
    const double fork = parts[1].dz - 0.5 * parts[1].sz;
    CHECK(fork >= 1.5 - 1e-4);
    CHECK(parts[0].sz > fork);
    CHECK(parts[0].sz < fork + 0.3);
    const double crownBottom = fork + standInCanopyForkShare(0) * parts[1].sz;
    CHECK(crownBottom > 2.5);
    CHECK(parts[1].sx >= 4.5f * 0.88f * 0.99f);
    CHECK(parts[1].sx <= 6.3f * 1.12f * 1.01f);
    CHECK(topOf(parts[1]) < 12.0);
    StandInPart other[kStandInPartsMax];
    standInParts(at(StandInKind::StreetTree, 709, 410), other);
    CHECK((other[1].sx != parts[1].sx || other[1].yaw != parts[1].yaw));
}

TEST_CASE("stand-ins: trees vary per instance: four leaf greens, both park canopies, turned and sized by seed") {
    bool paint[kStandInPaints] = {};
    bool variant[kStandInCanopyVariants] = {};
    double minW = 1e9, maxW = 0.0, minYaw = 1e9, maxYaw = -1e9;
    StandInPart parts[kStandInPartsMax];
    for (int i = 0; i < 200; ++i) {
        StandInPoint p = at(i % 2 ? StandInKind::ParkTree : StandInKind::StreetTree, 700 + 3.7 * i, 400 + 1.3 * i);
        p.canopy = 6.0;
        REQUIRE(standInParts(p, parts) == 2);
        paint[static_cast<std::size_t>(parts[1].paint)] = true;
        variant[parts[1].variant] = true;
        CHECK(parts[1].variant == (i % 2 ? parts[1].variant : 0));
        CHECK(parts[1].variant < kStandInCanopyVariants);
        minW = std::min(minW, static_cast<double>(parts[1].sx));
        maxW = std::max(maxW, static_cast<double>(parts[1].sx));
        minYaw = std::min(minYaw, static_cast<double>(parts[1].yaw));
        maxYaw = std::max(maxYaw, static_cast<double>(parts[1].yaw));
    }
    for (StandInPaint leaf : {StandInPaint::Leaf, StandInPaint::LeafLight, StandInPaint::LeafDark, StandInPaint::LeafWarm})
        CHECK(paint[static_cast<std::size_t>(leaf)]);
    CHECK(variant[0]);
    CHECK(variant[1]);
    CHECK(variant[2]);
    CHECK(maxW - minW > 6.0 * 0.15);
    CHECK(maxYaw - minYaw > 270.0);
}

namespace {

// The share of a side view's crown box (along axis 0 = x or 1 = y, against z, from the crown's foot up) the leaves
// cover, and the cells inside the silhouette left open (leaves on both sides along their row): sky seen through.
struct SideView {
    int covered = 0, holes = 0, cells = 0;
};
SideView sideView(const StandInCanopyMesh& m, int axis, double z0) {
    constexpr int kN = 32;
    bool grid[kN][kN] = {};
    for (std::size_t t = 0; t < m.slots.size(); ++t) {
        if (m.slots[t] != 0) continue;
        double u[3], v[3];
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t i = m.triangles[3 * t + static_cast<std::size_t>(k)];
            u[k] = m.positions[3 * i + static_cast<std::size_t>(axis)];
            v[k] = m.positions[3 * i + 2];
        }
        const double area = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
        if (std::abs(area) < 1e-12) continue;
        for (int row = 0; row < kN; ++row) {
            for (int col = 0; col < kN; ++col) {
                const double pu = -0.5 + (col + 0.5) / kN;
                const double pv = z0 + (0.5 - z0) * (row + 0.5) / kN;
                const double a = ((u[1] - pu) * (v[2] - pv) - (u[2] - pu) * (v[1] - pv)) / area;
                const double b = ((u[2] - pu) * (v[0] - pv) - (u[0] - pu) * (v[2] - pv)) / area;
                if (a >= 0 && b >= 0 && a + b <= 1) grid[row][col] = true;
            }
        }
    }
    SideView s;
    s.cells = kN * kN;
    for (int row = 0; row < kN; ++row) {
        int first = kN, last = -1;
        for (int col = 0; col < kN; ++col) {
            if (grid[row][col]) {
                ++s.covered;
                first = std::min(first, col);
                last = std::max(last, col);
            }
        }
        for (int col = first + 1; col < last; ++col) s.holes += grid[row][col] ? 0 : 1;
    }
    return s;
}

} // namespace

TEST_CASE("stand-ins: canopy meshes: the unit box round the fork, broken crowns with sky through, forking limbs") {
    for (std::size_t v = 0; v < kStandInCanopyVariants; ++v) {
        CAPTURE(v);
        StandInCanopyMesh m;
        REQUIRE(standInCanopyMesh(v, m));
        const std::size_t tris = m.triangles.size() / 3;
        REQUIRE(m.slots.size() == tris);
        REQUIRE(m.normals.size() == 3 * tris);
        double lo[3] = {1, 1, 1}, hi[3] = {-1, -1, -1}, leafLow = 1.0;
        for (std::size_t i = 0; i < m.positions.size(); i += 3)
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], static_cast<double>(m.positions[i + static_cast<std::size_t>(k)]));
                hi[k] = std::max(hi[k], static_cast<double>(m.positions[i + static_cast<std::size_t>(k)]));
            }
        CHECK(std::max(-lo[0], hi[0]) == doctest::Approx(0.5));
        CHECK(std::max(-lo[1], hi[1]) == doctest::Approx(0.5));
        CHECK(lo[2] == doctest::Approx(-0.5));
        CHECK(hi[2] == doctest::Approx(0.5));
        std::size_t leaves = 0, bark = 0;
        for (std::size_t t = 0; t < tris; ++t) {
            // Each normal is the triangle's own (wound counter-clockwise from outside) and unit.
            const float* a = &m.positions[3 * m.triangles[3 * t]];
            const float* b = &m.positions[3 * m.triangles[3 * t + 1]];
            const float* c = &m.positions[3 * m.triangles[3 * t + 2]];
            const double n[3] = {(b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]),
                                 (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]),
                                 (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])};
            const float* nn = &m.normals[3 * t];
            CHECK(nn[0] * n[0] + nn[1] * n[1] + nn[2] * n[2] > 0.0);
            CHECK(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2] == doctest::Approx(1.0).epsilon(1e-4));
            if (m.slots[t] == 0) {
                ++leaves;
                for (const float* p : {a, b, c}) leafLow = std::min(leafLow, static_cast<double>(p[2]));
            } else {
                ++bark;
            }
        }
        // Small clumps, not one sphere: 5 (street) or 7 (park) of 80 triangles; limbs 3 (one fork) or 5 (two).
        CHECK(leaves == (v == 0 ? 5u : 7u) * 80u);
        CHECK(bark == (v == 0 ? 3u : 5u) * 10u);
        // The fork share tree() uses is the mesh's.
        CHECK(standInCanopyForkShare(v) == doctest::Approx(leafLow + 0.5).epsilon(1e-5));
        CHECK(standInCanopyForkShare(v) > 0.2);
        CHECK(standInCanopyForkShare(v) < 0.5);
        for (int axis = 0; axis < 2; ++axis) {
            CAPTURE(axis);
            const SideView s = sideView(m, axis, leafLow);
            CHECK(s.covered < 0.8 * s.cells); // a broken outline, not a filled ellipse
            CHECK(s.holes >= 8);              // sky through the crown
        }
    }
    StandInCanopyMesh none;
    CHECK_FALSE(standInCanopyMesh(kStandInCanopyVariants, none));
    CHECK(standInCanopyForkShare(kStandInCanopyVariants) == 0.0);
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
        // The gable infill: a prism from the eaves up to the ridge over the box's footprint, nothing of it under them.
        const StandInPart& gable = parts[2];
        CHECK(gable.shape == StandInShape::Gable);
        CHECK(gable.roll == 0.f);
        CHECK(gable.dz - 0.5 * gable.sz == doctest::Approx(0.0).epsilon(1e-4));
        CHECK(gable.dz + 0.5 * gable.sz == doctest::Approx(rise).epsilon(1e-4));
        CHECK(gable.sx == doctest::Approx(p.len));
        CHECK(gable.sy == doctest::Approx(p.wid));
    }
}

TEST_CASE("stand-ins: a back house is one gable prism on its eaves, its ridge along the longer side") {
    StandInPoint p = at(StandInKind::HouseBack, 750, 600);
    p.len = 10.0;
    p.wid = 8.0;
    p.pitch = 30.0;
    StandInPart parts[kStandInPartsMax];
    REQUIRE(standInParts(p, parts) == 1);
    CHECK(parts[0].shape == StandInShape::Gable);
    CHECK(parts[0].roll == 0.f);
    CHECK(parts[0].yaw == doctest::Approx(0.0));
    CHECK(parts[0].sx == doctest::Approx(10.0));
    CHECK(parts[0].sy == doctest::Approx(8.0));
    CHECK(parts[0].sz == doctest::Approx(4.0 * std::tan(30.0 * kDeg)));
    CHECK(parts[0].dz - 0.5 * parts[0].sz == doctest::Approx(0.0));

    // The alley house of round 10 (6.1 m along its table ridge, 13.2 m across): the ridge turns to the long side, so
    // the gable is 2 m over the eaves, not 6.6 m, and its footprint is still the box's.
    StandInPoint narrow = at(StandInKind::HouseBack, 750.066, 446.25);
    narrow.len = 6.133;
    narrow.wid = 13.167;
    narrow.pitch = 34.3;
    REQUIRE(standInParts(narrow, parts) == 1);
    CHECK(parts[0].yaw == doctest::Approx(90.0));
    CHECK(parts[0].sx == doctest::Approx(13.167));
    CHECK(parts[0].sy == doctest::Approx(6.133));
    CHECK(parts[0].sz == doctest::Approx(0.5 * 6.133 * std::tan(34.3 * kDeg)));
    CHECK(parts[0].sz < 2.2f);
    const StandInBounds b = standInBounds(narrow, parts[0]);
    CHECK(b.bottom == doctest::Approx(0.0));
    // A steep table pitch is held at 40 degrees.
    narrow.pitch = 60.0;
    standInParts(narrow, parts);
    CHECK(parts[0].sz == doctest::Approx(0.5 * 6.133 * std::tan(40.0 * kDeg)));
}

TEST_CASE("stand-ins: no roof part reaches under its eaves by more than the overhang") {
    for (int i = 0; i < 60; ++i) {
        StandInPoint p = at(i % 2 == 0 ? StandInKind::House : StandInKind::HouseBack, 700 + 3.7 * i, 400 + 5.1 * i);
        p.len = 5.0 + 0.4 * i;
        p.wid = 22.0 - 0.3 * i;
        p.pitch = i % 3 == 0 ? 0.0 : 15.0 + 0.5 * i;
        p.rot = 15.0 * i;
        StandInPart parts[kStandInPartsMax];
        const std::size_t n = standInParts(p, parts);
        for (std::size_t k = 0; k < n; ++k) {
            CAPTURE(i);
            CAPTURE(k);
            CHECK(standInBounds(p, parts[k]).bottom >= -kStandInEaveDrop);
        }
    }
}

TEST_CASE("stand-ins: the gable mesh is a closed prism in the unit box, wound outward") {
    StandInCanopyMesh m;
    standInGableMesh(m);
    REQUIRE(m.positions.size() == 18);
    REQUIRE(m.triangles.size() == 24);
    REQUIRE(m.normals.size() == 24);
    CHECK(m.slots.size() == 8);
    CHECK(m.vertexNormals.empty());
    double volume = 0.0;
    for (std::size_t t = 0; t < m.triangles.size(); t += 3) {
        const float* a = &m.positions[3 * m.triangles[t]];
        const float* b = &m.positions[3 * m.triangles[t + 1]];
        const float* c = &m.positions[3 * m.triangles[t + 2]];
        // The signed volume under each face: positive in all when every face winds outward.
        volume += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
        // Its normal turns away from the prism's middle (0, 0, -1/6).
        const double mid[3] = {(a[0] + b[0] + c[0]) / 3.0, (a[1] + b[1] + c[1]) / 3.0, (a[2] + b[2] + c[2]) / 3.0 + 1.0 / 6.0};
        CHECK(m.normals[t] * mid[0] + m.normals[t + 1] * mid[1] + m.normals[t + 2] * mid[2] > 0.0);
    }
    CHECK(volume == doctest::Approx(0.5)); // half the unit box
    for (std::size_t i = 0; i < m.positions.size(); ++i) CHECK(std::abs(m.positions[i]) <= 0.5f);
}

TEST_CASE("stand-ins: a canopy's vertex normals are unit, outward and round each clump") {
    for (std::size_t v = 0; v < kStandInCanopyVariants; ++v) {
        StandInCanopyMesh m;
        REQUIRE(standInCanopyMesh(v, m));
        REQUIRE(m.vertexNormals.size() == m.positions.size());
        for (std::size_t i = 0; i < m.vertexNormals.size(); i += 3) {
            const double len = std::sqrt(m.vertexNormals[i] * m.vertexNormals[i] + m.vertexNormals[i + 1] * m.vertexNormals[i + 1] +
                                         m.vertexNormals[i + 2] * m.vertexNormals[i + 2]);
            CHECK(len == doctest::Approx(1.0).epsilon(1e-3));
        }
        // Each corner's normal leans the way its triangle faces, and differs from it somewhere (the shading is round).
        std::size_t differ = 0;
        for (std::size_t t = 0; t < m.triangles.size(); t += 3) {
            for (std::size_t k = 0; k < 3; ++k) {
                const float* n = &m.vertexNormals[3 * m.triangles[t + k]];
                const double dot = n[0] * m.normals[t] + n[1] * m.normals[t + 1] + n[2] * m.normals[t + 2];
                if (m.slots[t / 3] == 0) CHECK(dot > 0.3);
                differ += dot < 0.999 ? 1 : 0;
            }
        }
        CHECK(differ > m.triangles.size() / 2);
    }
}

TEST_CASE("stand-ins: the corridor rule: nothing stands in a street's or an alley's clear way") {
    // An alley 3 m wide up x 745.5 (no pavements: all of it is kept clear) and a 12 m street with 3 m pavements along y 400.
    // The canal's row (no width, shorter than the header) is left out, as a road table's is.
    const char* table =
        "# streets\n"
        "id,kind,width_m,pts,pavement_m,note\n"
        "SP,alley,3.0,745.5:423.5;745.5:622.5,0,x\n"
        "ST,street,12,700:400:8.5;760:400;800:440,3.0\n"
        "CN,canal,,600:0;600:900\n"
        "WET,canal,10,600:0;600:900,0\n";
    std::vector<StandInCorridor> cor;
    std::vector<std::string> ids;
    std::string error;
    REQUIRE(importStandInCorridors(table, cor, &ids, "canal", &error));
    REQUIRE(cor.size() == 3);
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == "SP");
    CHECK(ids[2] == "ST");
    CHECK(cor[0].half == doctest::Approx(1.5));
    CHECK(cor[1].half == doctest::Approx(3.0)); // the carriageway's half: the pavements may hold trees
    CHECK(cor[2].street == 1);
    CHECK(cor[2].bx == doctest::Approx(800.0));
    std::vector<StandInCorridor> none;
    CHECK_FALSE(importStandInCorridors("id,kind,pts\nA,lane,1:2;3:4\n", none, nullptr, {}, &error));
    CHECK(error.find("line 1") != std::string::npos);
    CHECK_FALSE(importStandInCorridors("id,width_m,pts\nA,4,1:2;x:4\n", none, nullptr, {}, &error));
    CHECK(error.find("line 2") != std::string::npos);
    CHECK_FALSE(importStandInCorridors("# nothing\n", none, nullptr, {}, &error));
    CHECK(none.empty());
    const StandInCorridorSpec spec;

    // The back house on the alley's east side (its wall on the alley's edge, x 747): clear, with its eaves 4.2 m up.
    std::vector<StandInPoint> pts;
    StandInPoint house = at(StandInKind::HouseBack, 750.066, 446.25);
    house.len = 6.133;
    house.wid = 13.167;
    house.z = 12.83;
    house.hasZ = true;
    pts.push_back(house);
    std::vector<double> foot = {12.83 - 8.63};
    StandInCorridorHit hits[8];
    CHECK(standInCorridorCheck(pts.data(), foot.data(), pts.size(), cor.data(), cor.size(), spec, hits, 8) == 0);
    // The same roof on the ground (a row whose eaves came out at the ground's height) and 1 m into the alley: rejected.
    pts[0].x -= 1.0;
    foot[0] = 0.0;
    REQUIRE(standInCorridorCheck(pts.data(), foot.data(), pts.size(), cor.data(), cor.size(), spec, hits, 8) == 1);
    CHECK(hits[0].point == 0);
    CHECK(hits[0].corridor == 0);
    CHECK(hits[0].depth == doctest::Approx(1.0).epsilon(0.01));
    // Up on its eaves again it hangs clear, even over the alley.
    foot[0] = 4.2;
    CHECK(standInCorridorCheck(pts.data(), foot.data(), pts.size(), cor.data(), cor.size(), spec, hits, 8) == 0);

    // Trees: on the street's pavement (4.5 m off its centre line) clear, though the crown spreads over the
    // carriageway; in the carriageway, or in the alley, rejected by the trunk alone (one hit a tree).
    pts.clear();
    StandInPoint tree = at(StandInKind::StreetTree, 730.0, 404.5);
    tree.canopy = 6.0;
    pts.push_back(tree);
    tree.y = 402.0;
    pts.push_back(tree);
    tree.x = 745.5;
    tree.y = 500.0;
    pts.push_back(tree);
    tree.x = 600.0; // in the canal's row, which is left out
    pts.push_back(tree);
    REQUIRE(standInCorridorCheck(pts.data(), nullptr, pts.size(), cor.data(), cor.size(), spec, hits, 8) == 2);
    CHECK(hits[0].point == 1);
    CHECK(hits[0].part == 0);
    CHECK(hits[0].corridor == 1);
    CHECK(hits[1].point == 2);
    CHECK(hits[1].corridor == 0);
    // The count goes on past the room for hits; null hits only count.
    CHECK(standInCorridorCheck(pts.data(), nullptr, pts.size(), cor.data(), cor.size(), spec, hits, 1) == 2);
    CHECK(standInCorridorCheck(pts.data(), nullptr, pts.size(), cor.data(), cor.size(), spec, nullptr, 0) == 2);
    CHECK(standInCorridorCheck(nullptr, nullptr, 0, cor.data(), cor.size(), spec, hits, 8) == 0);

    // Median islands (median_m): a tree on a 4 m island is clear; on a 1 m one, or in a lane, it is not.
    {
        std::vector<StandInCorridor> mc;
        REQUIRE(importStandInCorridors("id,kind,width_m,pts,pavement_m,lanes,median_m\n"
                                       "BV,street,25,0:0;300:0,3.5,4,4\n"
                                       "NM,street,25,0:100;300:100,3.5,4,1\n"
                                       "PL,street,14,0:200;300:200,2.8,,\n",
                                       mc, nullptr, {}, &error));
        REQUIRE(mc.size() == 3);
        CHECK(mc[0].median == doctest::Approx(4.0));
        CHECK(mc[0].half == doctest::Approx(9.0));
        CHECK(mc[1].median == doctest::Approx(1.0));
        CHECK(mc[2].median == 0.0);
        std::vector<StandInCorridor> bad;
        CHECK_FALSE(importStandInCorridors("width_m,pts,median_m\n20,0:0;9:0,-1\n", bad, nullptr, {}, &error));
        std::vector<StandInPoint> mp;
        mp.push_back(at(StandInKind::StreetTree, 150.0, 0.0)); // on the 4 m island
        mp.push_back(at(StandInKind::ParkTree, 150.0, 1.0));   // off its centre, still on it
        mp.push_back(at(StandInKind::StreetTree, 150.0, 5.0)); // in a lane beside it
        mp.push_back(at(StandInKind::StreetTree, 150.0, 100.0)); // on the 1 m median
        for (StandInPoint& p : mp) p.canopy = 6.0;
        REQUIRE(standInCorridorCheck(mp.data(), nullptr, mp.size(), mc.data(), mc.size(), spec, hits, 8) == 2);
        CHECK(hits[0].point == 2);
        CHECK(hits[0].corridor == 0);
        CHECK(hits[1].point == 3);
        CHECK(hits[1].corridor == 1);
        // Not a tree: the island lets only trees stand.
        const StandInPoint mast = at(StandInKind::LampMast, 150.0, 0.0);
        CHECK(standInCorridorCheck(&mast, nullptr, 1, mc.data(), mc.size(), spec, hits, 8) >= 1);
        // A wider minimum takes the 4 m island's leave away.
        StandInCorridorSpec strict;
        strict.medianMin = 5.0;
        CHECK(standInCorridorCheck(mp.data(), nullptr, 2, mc.data(), mc.size(), strict, hits, 8) == 2);
        // Past the street's end the island is not under it.
        mp[0].x = -0.1;
        CHECK(standInCorridorCheck(mp.data(), nullptr, 1, mc.data(), mc.size(), spec, hits, 8) == 1);
    }

    // A glass tower whose corner crosses the street's diagonal leg, and one whose footprint holds an alley's end.
    pts.clear();
    StandInPoint tower = at(StandInKind::GlassTower, 795.0, 410.0);
    tower.len = 30.0;
    tower.wid = 30.0;
    tower.floors = 30;
    pts.push_back(tower);
    CHECK(standInCorridorCheck(pts.data(), nullptr, pts.size(), cor.data(), cor.size(), spec, hits, 8) >= 1);
    pts[0].x = 745.5;
    pts[0].y = 520.0;
    REQUIRE(standInCorridorCheck(pts.data(), nullptr, pts.size(), cor.data(), cor.size(), spec, hits, 8) >= 1);
    CHECK(hits[0].depth == doctest::Approx(1.5)); // the centre line runs through it: the whole half width

    // The depth itself: a 2 x 2 box turned 45 degrees, its corner 0.5 m short of the alley's centre line.
    StandInBounds b;
    b.cx = 745.5 + 0.5 + std::sqrt(2.0);
    b.cy = 500.0;
    b.hx = 1.0;
    b.hy = 1.0;
    b.yaw = 45.0;
    CHECK(standInCorridorDepth(b, cor[0]) == doctest::Approx(1.0));
    b.cx += 5.0;
    CHECK(standInCorridorDepth(b, cor[0]) < 0.0);
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
    CHECK(parts[1].sx >= 6.0 * 0.88 - 1e-4); // the table's canopy, +-12% by seed
    CHECK(parts[1].sx <= 6.0 * 1.12 + 1e-4);
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
