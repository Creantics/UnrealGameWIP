// A lawn that follows the ground (ftcore/LawnSheet.h).

#include "ftcore/LawnSheet.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {
// A 20 x 20 field, 1 m apart from (0, 0), rising 0.1 m a metre east (the field's encoding: 32768 + 128 per metre).
std::vector<std::uint16_t> slope() {
    std::vector<std::uint16_t> g(400);
    for (int j = 0; j < 20; ++j)
        for (int i = 0; i < 20; ++i) g[static_cast<std::size_t>(j * 20 + i)] = static_cast<std::uint16_t>(32768 + 128 * 0.1 * i + 0.5);
    return g;
}
double cross(const LawnMesh& m, std::size_t t) {
    const MassingVec& a = m.v[m.tris[t][0]];
    const MassingVec& b = m.v[m.tris[t][1]];
    const MassingVec& c = m.v[m.tris[t][2]];
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
} // namespace

TEST_CASE("lawn table: regions and paths, with its errors") {
    LawnTable t;
    std::string e;
    CHECK(parseLawnTable("# lawn\nkind,x0,y0,x1,y1,width\nregion,10,20,0,0,\npath,1,1,9,1,2.5\n", t, &e));
    REQUIRE(t.regions.size() == 1);
    CHECK(t.regions[0].x0 == 0.0);
    CHECK(t.regions[0].y1 == 20.0);
    REQUIRE(t.paths.size() == 1);
    CHECK(t.paths[0].width == 2.5);
    CHECK_FALSE(parseLawnTable("kind,x0,y0,x1\n", t, &e));
    CHECK_FALSE(parseLawnTable("kind,x0,y0,x1,y1\nbed,0,0,1,1\n", t, &e));
    CHECK(e.find("unknown kind") != std::string::npos);
    CHECK_FALSE(parseLawnTable("kind,x0,y0,x1,y1\nregion,0,0,0,5\n", t, &e));
    CHECK_FALSE(parseLawnTable("kind,x0,y0,x1,y1,width\npath,0,0,0,0,2\n", t, &e));
    CHECK_FALSE(parseLawnTable("kind,x0,y0,x1,y1\nregion,a,0,1,1\n", t, &e));
}

TEST_CASE("plant points: kinds, optional z, and errors") {
    std::vector<PlantPoint> p;
    std::string e;
    CHECK(parsePlantPoints("# plants\nasset,x,y,z,yaw,scale,kind\nPLANT-fern-a,1.5,2,,90,1.1,cover\nPLANT-boxwood-a,3,4,5.25,0,0.9,shrub\n", p, &e));
    REQUIRE(p.size() == 2);
    CHECK(p[0].kind == PlantPointKind::Cover);
    CHECK_FALSE(p[0].hasZ);
    CHECK(p[1].hasZ);
    CHECK(p[1].z == 5.25);
    CHECK_FALSE(parsePlantPoints("asset,x,y,z,yaw,scale\n", p, &e));
    CHECK_FALSE(parsePlantPoints("asset,x,y,z,yaw,scale,kind\nA,1,1,,0,0,shrub\n", p, &e));
    CHECK_FALSE(parsePlantPoints("asset,x,y,z,yaw,scale,kind\nA,1,1,,0,1,tree\n", p, &e));
    CHECK_FALSE(parsePlantPoints("asset,x,y,z,yaw,scale,kind\n,1,1,,0,1,shrub\n", p, &e));
}

TEST_CASE("lawn sheet follows the ground with no steps, wound counter-clockwise") {
    const std::vector<std::uint16_t> g = slope();
    const HeightField f{g.data(), 20, 20, 1.0, 0.0, 0.0};
    LawnMesh m;
    lawnSheetMesh(f, {2.0, 2.0, 12.0, 6.0}, 1.0, 0.05, m);
    CHECK(m.v.size() == 11u * 5u);
    CHECK(m.tris.size() == 10u * 4u * 2u);
    for (const MassingVec& v : m.v) CHECK(v.z == doctest::Approx(0.1 * v.x + 0.05).epsilon(0.01));
    for (std::size_t t = 0; t < m.tris.size(); ++t) CHECK(cross(m, t) > 0.0);
    // A region smaller than a step is still one cell.
    LawnMesh s;
    lawnSheetMesh(f, {3.0, 3.0, 3.2, 3.3}, 1.0, 0.0, s);
    CHECK(s.tris.size() == 2u);
}

TEST_CASE("path ribbon: over the lawn, skirts under the ground, wound counter-clockwise on top") {
    const std::vector<std::uint16_t> g = slope();
    const HeightField f{g.data(), 20, 20, 1.0, 0.0, 0.0};
    LawnMesh m;
    lawnPathMesh(f, {2.0, 10.0, 14.0, 10.0, 2.0}, 1.0, 0.08, 0.1, m);
    REQUIRE(m.v.size() == 13u * 5u);
    CHECK(m.tris.size() == 12u * 4u * 2u);
    CHECK(m.v[1].y == doctest::Approx(11.0)); // left edge, north of an eastward path
    CHECK(m.v[0].z == doctest::Approx(0.2 - 0.1).epsilon(0.02));
    CHECK(m.v[2].z == doctest::Approx(0.2 + 0.08).epsilon(0.02));
    // The top's triangles (k 1 and 2) face up.
    for (std::size_t s = 0; s < 12; ++s)
        for (std::size_t k = 1; k < 3; ++k) {
            CHECK(cross(m, (s * 4 + k) * 2) > 0.0);
            CHECK(cross(m, (s * 4 + k) * 2 + 1) > 0.0);
        }
}
