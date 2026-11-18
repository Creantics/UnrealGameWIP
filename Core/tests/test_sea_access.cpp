// Ways out of the sea (ftcore/SeaAccess.h).

#include "ftcore/SeaAccess.h"
#include "ftcore/Roads.h"

#include <ostream>
#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

TEST_CASE("seaLadders: evenly along a face, off the avoided rects, with the rail gap on the face's line") {
    // A quay face along x = 955 from y 300 to 700, the bank to the west (out = -x), deck at 4 m.
    const std::vector<SeaFace> faces{{"QY", 955.0, 300.0, 955.0, 700.0, -1.0, 0.0, 4.0}};
    std::vector<SeaLadder> out;
    REQUIRE(seaLadders(faces, {}, SeaLadderSpec{}, out));
    // 390 m between the margins at 30 m: 14 ladders, 30 m apart.
    REQUIRE(out.size() == 14);
    CHECK(out[0].id == "QY-L1");
    CHECK(out[0].footX == doctest::Approx(955.35));
    CHECK(out[0].footY == doctest::Approx(305.0));
    CHECK(out[0].topX == doctest::Approx(953.8));
    CHECK(out[0].topZ == doctest::Approx(4.0));
    CHECK(out[1].footY - out[0].footY == doctest::Approx(30.0));
    CHECK(out.back().footY == doctest::Approx(695.0));
    CHECK(out[0].gapY0 == doctest::Approx(304.4));
    CHECK(out[0].gapY1 == doctest::Approx(305.6));
    CHECK(out[0].gapX0 == doctest::Approx(955.0));

    // A crane over y 330-340 and the slip at y 680-700 drop the ladders there.
    std::vector<SeaLadder> cut;
    REQUIRE(seaLadders(faces, {{940.0, 330.0, 960.0, 340.0}, {940.0, 680.0, 960.0, 700.0}}, SeaLadderSpec{}, cut));
    CHECK(cut.size() == 12);
    for (const SeaLadder& l : cut) {
        CHECK_FALSE((l.footY >= 330.0 && l.footY <= 340.0));
        CHECK(l.footY < 680.0);
    }

    // Two faces meeting at a jog: the second face's ladder within minGap of the first's is dropped.
    const std::vector<SeaFace> jog{{"A", 955.0, 0.0, 955.0, 10.0, -1.0, 0.0, 4.0}, {"B", 955.0, 12.0, 968.0, 12.0, 0.0, -1.0, 9.0}};
    std::vector<SeaLadder> j;
    REQUIRE(seaLadders(jog, {}, SeaLadderSpec{}, j));
    CHECK(j.size() == 1);
    CHECK_FALSE(seaLadders({{"Z", 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0}}, {}, SeaLadderSpec{}, j));
    SeaLadderSpec bad;
    bad.spacing = 0.5;
    CHECK_FALSE(seaLadders(faces, {}, bad, j));
}

TEST_CASE("seaExitReach: the farthest water cell from a way out") {
    // A strip of water 18 m wide off the face, 400 m long, a pier across it at y 497-503.
    const std::vector<SeaRect> water{{955.0, 300.0, 973.0, 700.0}};
    const std::vector<SeaRect> solid{{955.0, 497.0, 973.0, 503.0}};
    std::vector<SeaFace> faces{{"QY", 955.0, 300.0, 955.0, 700.0, -1.0, 0.0, 4.0}};
    std::vector<SeaLadder> ladders;
    REQUIRE(seaLadders(faces, {}, SeaLadderSpec{}, ladders));
    std::vector<SeaPoint> exits;
    for (const SeaLadder& l : ladders) exits.push_back({l.footX, l.footY});
    SeaReach r;
    REQUIRE(seaExitReach(water, solid, exits, 1.0, 25.0, r));
    CHECK(r.cells == 18 * 400 - 18 * 6);
    // The worst: a far corner of the strip, 5 m past the last ladder and 17 m out (or a mid-gap point 15 m along).
    CHECK(r.worst < 25.0);
    CHECK(r.over == 0);
    // Half the ladders: some water is over 25 m.
    std::vector<SeaPoint> few;
    for (std::size_t i = 0; i < exits.size(); i += 3) few.push_back(exits[i]);
    REQUIRE(seaExitReach(water, solid, few, 1.0, 25.0, r));
    CHECK(r.worst > 25.0);
    CHECK(r.over > 0);
    // Overlapping rects aren't counted twice.
    REQUIRE(seaExitReach({{0.0, 0.0, 10.0, 10.0}, {5.0, 0.0, 15.0, 10.0}}, {}, {{0.0, 0.0}}, 1.0, 25.0, r));
    CHECK(r.cells == 150);
    CHECK_FALSE(seaExitReach(water, solid, {}, 1.0, 25.0, r));
    CHECK_FALSE(seaExitReach(water, solid, exits, 0.1, 25.0, r));
}

TEST_CASE("seaRailGaps: the quay rail opened at each ladder, its posts and bollards there left out") {
    // A rail 20 m long along y at x 954.85 (0.15 m in from the face at 955), a post on it at y 505 and one at 510, a
    // bollard 0.6 m in at y 505.4, and a bench (kept whatever it is).
    MassingBox bar;
    bar.id = "Q-QR1";
    bar.kind = "barrier";
    bar.cx = 954.85;
    bar.cy = 510.0;
    bar.sx = 20.0;
    bar.sy = 0.06;
    bar.rot = 90.0;
    MassingBox post = bar, post2 = bar, bollard = bar, bench = bar;
    post.id = "Q-QP1";
    post.kind = "post";
    post.cy = 505.0;
    post.sx = post.sy = 0.08;
    post2.id = "Q-QP2";
    post2.kind = "post";
    post2.cy = 510.0;
    post2.sx = post2.sy = 0.08;
    bollard.id = "Q-QB1";
    bollard.kind = "post";
    bollard.cx = 954.4;
    bollard.cy = 505.4;
    bench.id = "BENCH";
    bench.kind = "bench";
    bench.cy = 505.0;
    const std::vector<SeaFace> faces{{"QY", 955.0, 495.0, 955.0, 525.0, -1.0, 0.0, 4.0}};
    std::vector<SeaLadder> ladders;
    SeaLadderSpec spec;
    spec.spacing = 20.0;
    REQUIRE(seaLadders(faces, {}, spec, ladders));
    REQUIRE(ladders.size() == 2); // y 500 and y 520
    SeaRailCounts c;
    const std::vector<MassingBox> got = seaRailGaps({bar, post, post2, bollard, bench}, ladders, 0.2, 0.5, c);
    CHECK(c.cut == 1);
    CHECK(c.dropped == 0); // the ladders are at y 500 and 520: no post or bollard there
    // The gaps widened by 0.2 run 499.2..500.8 and 519.2..520.8; the bar (500..520) keeps 500.8..519.2.
    std::size_t bars = 0;
    for (const MassingBox& b : got) {
        if (b.kind != "barrier") continue;
        ++bars;
        CHECK(b.id == "Q-QR1a");
        CHECK(b.sx == doctest::Approx(18.4));
        CHECK(b.cy == doctest::Approx(510.0));
        CHECK(b.cx == doctest::Approx(954.85));
    }
    CHECK(bars == 1);
    CHECK(got.size() == 5);

    // A ladder at y 505: the post at 505 and the bollard go, the post at 510 stays; the bar is cut in two.
    SeaLadder at505 = ladders[0];
    at505.gapY0 = 504.4;
    at505.gapY1 = 505.6;
    const std::vector<MassingBox> got2 = seaRailGaps({bar, post, post2, bollard, bench}, {at505}, 0.2, 0.5, c);
    CHECK(c.cut == 1);
    CHECK(c.dropped == 2);
    CHECK(got2.size() == 4); // two bar pieces, post2 and the bench
    // A bar off the gap's line (1 m in) is kept whole.
    MassingBox inland = bar;
    inland.cx = 953.8;
    CHECK(seaRailGaps({inland}, {at505}, 0.2, 0.5, c).size() == 1);
    CHECK(c.cut == 0);
}

TEST_CASE("importSeaAccess: faces, rects, exits and levels from the -SeaFaces table") {
    const std::string text = "# the Strip's sea\n"
                             "face, QY, 955, 300, 955, 700, -2, 0, 4\n"
                             "avoid,940,355,960,365\r\n"
                             "water,955,0,1022,1000  # the whole bay\n"
                             "solid,955,497,1015,503\n"
                             "exit,957,687\n"
                             "bed,-4.5\n"
                             "\n"
                             "sea,0.1";
    SeaAccessTable t;
    std::string err;
    REQUIRE(importSeaAccess(text, t, &err));
    REQUIRE(t.faces.size() == 1);
    CHECK(t.faces[0].id == "QY");
    CHECK(t.faces[0].outX == doctest::Approx(-1.0));
    CHECK(t.faces[0].topZ == doctest::Approx(4.0));
    CHECK(t.avoid.size() == 1);
    CHECK(t.water.size() == 1);
    CHECK(t.water[0].maxX == doctest::Approx(1022.0));
    CHECK(t.solid.size() == 1);
    REQUIRE(t.exits.size() == 1);
    CHECK(t.exits[0].y == doctest::Approx(687.0));
    CHECK(t.bedZ == doctest::Approx(-4.5));
    CHECK(t.seaZ == doctest::Approx(0.1));
    CHECK_FALSE(importSeaAccess("ladder,1,2\n", t, &err));
    CHECK(err.find("line 1") != std::string::npos);
    CHECK_FALSE(importSeaAccess("exit,1\n", t, &err));
    CHECK_FALSE(importSeaAccess("\nexit,1,x\n", t, &err));
    CHECK(err.find("line 2") != std::string::npos);
    CHECK_FALSE(importSeaAccess("face,A,0,0,0,10,0,0,4\n", t, &err));
    CHECK_FALSE(importSeaAccess("water,10,0,0,10\n", t, &err));
    CHECK(importSeaAccess("", t, &err));
    CHECK(t.faces.empty());
    REQUIRE(importSeaAccess("\xEF\xBB\xBF# a BOM first\nexit,1,2\n", t, &err));
    CHECK(t.exits.size() == 1);
}

TEST_CASE("roadSeaLaddersBuild: a canal ladder in each face and a way out per ladder") {
    const std::vector<SeaFace> faces{{"QY", 955.0, 300.0, 955.0, 340.0, -1.0, 0.0, 4.0}, {"LOW", 955.0, 400.0, 955.0, 410.0, -1.0, 0.0, 0.2}};
    std::vector<SeaLadder> ladders;
    REQUIRE(seaLadders(faces, {}, SeaLadderSpec{}, ladders));
    REQUIRE(ladders.size() == 3); // two on QY (30 m between its margins), one on LOW
    RoadBuild build;
    const std::size_t added = roadSeaLaddersBuild(ladders, -3.0, 0.0, build);
    CHECK(added == 2); // LOW's top is no higher than the sea
    REQUIRE(build.exits.size() == 2);
    const RoadWaterExit& e = build.exits[0];
    CHECK(e.id == "QY-L1");
    CHECK(e.canal == "SEA");
    CHECK(e.kind == RoadExitKind::Ladder);
    CHECK(e.footX == doctest::Approx(955.35));
    CHECK(e.topX == doctest::Approx(953.8));
    CHECK(e.topZ == doctest::Approx(4.0));
    CHECK(e.outX == doctest::Approx(-1.0));
    // Per ladder: 2 x (stile, hoop top, hoop leg, 2 brackets) + rungs from -2.7 to under 3.98 every 0.3 m (23).
    CHECK(build.solids.size() == 2 * (10 + 23));
    for (const RoadSolid& s : build.solids) {
        CHECK(s.box.kind == "canal_ladder");
        CHECK_FALSE(s.collide);
        CHECK(std::abs(s.box.rot) == doctest::Approx(90.0));
    }
    // A stile stands 0.12 m out of the face into the water, from the bed to 1 m over the deck.
    const RoadSolid& stile = build.solids[0];
    CHECK(stile.box.cx == doctest::Approx(955.12));
    CHECK(stile.box.z0 == doctest::Approx(-3.0));
    CHECK(stile.box.z1 == doctest::Approx(5.0));
}
