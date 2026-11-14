// Modelled trees in place of the stand-in trees (ftcore/TreeMeshes.h).

#include "ftcore/TreeMeshes.h"

#include <ostream>
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

namespace {

std::vector<TreeModel> models() {
    std::vector<TreeModel> m;
    const char* csv =
        "# models\n"
        "kind,asset,weight\n"
        "street,TREE-broadleaf-a,1\n"
        "street,TREE-broadleaf-b,1\n"
        "park,TREE-park-a,3\n"
        "park,TREE-ornamental-a,1\n"
        "cherry,TREE-cherry-young,1\n"
        "cherry_old,TREE-cherry-old,1\n";
    REQUIRE(parseTreeModels(csv, m));
    const double heights[] = {11.0, 9.5, 13.5, 5.0, 6.0, 9.0};
    for (std::size_t i = 0; i < m.size(); ++i) {
        m[i].height = heights[i];
        m[i].crown = heights[i] * 0.7;
    }
    return m;
}

StandInPoint row(StandInKind kind, const char* tag, const char* id, double h, double canopy = 0.0) {
    StandInPoint p;
    p.kind = kind;
    p.tag = tag;
    p.id = id;
    p.h = h;
    p.canopy = canopy;
    p.x = 10.0;
    p.y = 20.0;
    return p;
}

} // namespace

TEST_CASE("tree models: the table reads kind, asset and weight") {
    std::vector<TreeModel> m = models();
    REQUIRE(m.size() == 6);
    CHECK(m[0].kind == "street");
    CHECK(m[2].asset == "TREE-park-a");
    CHECK(m[2].weight == doctest::Approx(3.0));
    std::vector<TreeModel> bad;
    std::string error;
    CHECK_FALSE(parseTreeModels("asset\nx\n", bad, &error));
    CHECK(error.find("line 1") != std::string::npos);
    CHECK_FALSE(parseTreeModels("kind,asset,weight\nstreet,A,heavy\n", bad, &error));
    CHECK(parseTreeModels("Kind,Asset\nSTREET,A\n", bad));
    CHECK(bad.back().kind == "street");
    CHECK(bad.back().weight == doctest::Approx(1.0));
}

TEST_CASE("tree models: a row takes a model of its kind, else street's or park's") {
    const std::vector<TreeModel> m = models();
    const TreePick street = pickTreeModel(row(StandInKind::StreetTree, "street", "s1", 9.0), m.data(), m.size());
    REQUIRE(street.model >= 0);
    CHECK(m[street.model].kind == "street");
    const TreePick forecourt = pickTreeModel(row(StandInKind::ParkTree, "forecourt", "f1", 12.0), m.data(), m.size());
    REQUIRE(forecourt.model >= 0);
    CHECK(m[forecourt.model].kind == "park");
    const TreePick cherry = pickTreeModel(row(StandInKind::StreetTree, "cherry", "c1", 6.0), m.data(), m.size());
    CHECK(cherry.model == 4);
    const TreePick old = pickTreeModel(row(StandInKind::ParkTree, "cherry_old", "c2", 9.0), m.data(), m.size());
    CHECK(old.model == 5);
    // No tag (a table without a kind column): the stand-in kind's.
    const TreePick plain = pickTreeModel(row(StandInKind::StreetTree, "", "p1", 10.0), m.data(), m.size());
    REQUIRE(plain.model >= 0);
    CHECK(m[plain.model].kind == "street");
    // No model at all for the kind.
    const std::vector<TreeModel> none;
    CHECK(pickTreeModel(row(StandInKind::StreetTree, "street", "x", 9.0), none.data(), 0).model == -1);
}

TEST_CASE("tree models: only the models the height reaches within the limits are picked") {
    const std::vector<TreeModel> m = models();
    // A 5 m park row: the 13.5 m park tree would need 0.37, so the ornamental, always.
    int ornamental = 0;
    for (int i = 0; i < 40; ++i) {
        const std::string id = "p" + std::to_string(i);
        const TreePick p = pickTreeModel(row(StandInKind::ParkTree, "park", id.c_str(), 5.0), m.data(), m.size());
        ornamental += p.model == 3 ? 1 : 0;
        CHECK_FALSE(p.clamped);
    }
    CHECK(ornamental == 40);
    // A 12 m park row: both reach it only the park tree.
    for (int i = 0; i < 40; ++i) {
        const std::string id = "q" + std::to_string(i);
        CHECK(pickTreeModel(row(StandInKind::ParkTree, "park", id.c_str(), 12.0), m.data(), m.size()).model == 2);
    }
    // A 3 m street row: none fits; the nearest (9.5 m), clamped at the least scale.
    const TreePick small = pickTreeModel(row(StandInKind::StreetTree, "street", "tiny", 3.0), m.data(), m.size());
    CHECK(small.clamped);
    CHECK(small.model == 1);
    CHECK(small.scaleZ <= 0.6 * 1.06 + 1e-9);
    CHECK(small.scaleZ >= 0.6 * 0.94 - 1e-9);
}

TEST_CASE("tree models: weights share the fitting rows out, the same each build") {
    std::vector<TreeModel> m = models();
    m[3].height = 9.0; // an ornamental that reaches 9-12 m rows too
    int counts[6] = {};
    for (int i = 0; i < 400; ++i) {
        const std::string id = "w" + std::to_string(i);
        const TreePick p = pickTreeModel(row(StandInKind::ParkTree, "park", id.c_str(), 10.0), m.data(), m.size());
        REQUIRE(p.model >= 0);
        ++counts[p.model];
        const TreePick again = pickTreeModel(row(StandInKind::ParkTree, "park", id.c_str(), 10.0), m.data(), m.size());
        CHECK(again.model == p.model);
        CHECK(again.yawDeg == doctest::Approx(p.yawDeg));
        CHECK(again.scaleZ == doctest::Approx(p.scaleZ));
    }
    // 3 : 1.
    CHECK(counts[2] > 250);
    CHECK(counts[2] < 350);
    CHECK(counts[3] > 50);
    // A weight of 0 leaves a model out.
    m[3].weight = 0.0;
    for (int i = 0; i < 50; ++i) {
        const std::string id = "z" + std::to_string(i);
        CHECK(pickTreeModel(row(StandInKind::ParkTree, "park", id.c_str(), 10.0), m.data(), m.size()).model == 2);
    }
}

TEST_CASE("tree models: the scale follows the height, the crown the canopy within the spread") {
    const std::vector<TreeModel> m = models();
    const TreePickSpec spec;
    for (int i = 0; i < 60; ++i) {
        const std::string id = "k" + std::to_string(i);
        const TreePick p = pickTreeModel(row(StandInKind::ParkTree, "cherry_old", id.c_str(), 9.0, 20.0), m.data(), m.size());
        REQUIRE(p.model == 5);
        CHECK(p.scaleZ == doctest::Approx(1.0).epsilon(spec.jitter + 1e-6));
        // The canopy asks 20 / 6.3: held to the spread over the height's scale.
        CHECK(p.scaleXY == doctest::Approx(p.scaleZ * (1.0 + spec.crownSpread)));
        CHECK(p.yawDeg >= 0.0);
        CHECK(p.yawDeg < 360.0);
    }
    // Without a height: the row's scale times the model's.
    StandInPoint s = row(StandInKind::StreetTree, "street", "nh", 0.0);
    s.scale = 0.8f;
    const TreePick p = pickTreeModel(s, m.data(), m.size());
    CHECK(p.scaleZ == doctest::Approx(0.8).epsilon(spec.jitter + 1e-6));
}

TEST_CASE("tree models: a stand-in table's kind text comes through as the tag") {
    std::vector<StandInPoint> points;
    REQUIRE(importStandIns("id,kind,x,y,height_m\na,Street,1,2,7\nb,cherry_old,3,4,9\nc,cherry,5,6,6\n", StandInKind::Count, points));
    REQUIRE(points.size() == 3);
    CHECK(points[0].tag == "street");
    CHECK(points[0].kind == StandInKind::StreetTree);
    CHECK(points[1].tag == "cherry_old");
    CHECK(points[1].kind == StandInKind::ParkTree);
    CHECK(points[2].kind == StandInKind::StreetTree);
    CHECK(treeKindOf(points[2]) == "cherry");
}
