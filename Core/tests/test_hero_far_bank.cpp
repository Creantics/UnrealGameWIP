#include <doctest/doctest.h>
#include "ftcore/HeroFarBank.h"

#include <cmath>
#include <cstring>

using namespace ftcore;

namespace {

bool clearOf(double y0, double y1, const HeroSpan* spans, int count) {
    for (int i = 0; i < count; ++i)
        if (y1 > spans[i].y0 + 1e-9 && y0 < spans[i].y1 - 1e-9) return false;
    return true;
}

} // namespace

TEST_CASE("hero far bank: low fronts stand on the far bank, one or two storeys, clear of its exits") {
    const HeroFarBankSpec spec;
    static HeroFarBankLayout l;
    heroFarBankLayout(spec, l);
    CHECK_FALSE(l.overflow);
    CHECK(l.frontCount >= 8);
    CHECK(l.frontCount <= kFarFrontsMax);
    int lit = 0, two = 0;
    for (int i = 0; i < l.frontCount; ++i) {
        const FarFront& f = l.fronts[i];
        const double w = f.y1 - f.y0;
        CHECK(w >= 4.5 - 1e-9);
        CHECK(w <= 6.3 + 1e-9);
        CHECK(f.storeys >= 1);
        CHECK(f.storeys <= 2);
        CHECK(f.ridge - f.ground <= 8.0);
        CHECK(f.ridge > f.eave);
        CHECK(f.y0 >= spec.frontY0 - 1e-9);
        CHECK(f.y1 <= spec.frontY1 + 1e-9);
        CHECK(clearOf(f.y0, f.y1, spec.frontClear, spec.frontClearCount));
        if (i > 0 && l.fronts[i - 1].y1 > f.y0 - 1e-9) CHECK(l.fronts[i - 1].y1 == doctest::Approx(f.y0));
        CHECK(f.ground == doctest::Approx(heroFarGround(spec, 0.5 * (f.y0 + f.y1))));
        for (int m = 0; m < spec.mastCount; ++m)
            if (spec.mastY[m] > f.y0 - spec.mastClear && spec.mastY[m] < f.y1 + spec.mastClear) CHECK(f.storeys == 1);
        lit += f.lit ? 1 : 0;
        two += f.storeys == 2 ? 1 : 0;
    }
    CHECK(lit >= l.frontCount / 2);
    CHECK(two >= 1);
    CHECK(two < l.frontCount);
    // The far bank: west of the water, the back on the far road's kerb (x 888.5).
    CHECK(spec.frontX < spec.canalX0);
    CHECK(spec.frontX - spec.roofDepth >= 888.5 - 0.1);
    for (int i = 0; i < l.pieceCount; ++i) {
        const FarPiece& p = l.pieces[i];
        CHECK(p.at.x == doctest::Approx(spec.frontX));
        REQUIRE(p.front >= 0);
        REQUIRE(p.front < l.frontCount);
        const FarFront& f = l.fronts[p.front];
        const FarKitSize k = farKitSize(p.kit);
        CHECK(p.at.y >= f.y0 - 1e-9);
        CHECK(p.at.y + k.sx <= f.y1 + 1e-9);
        CHECK(p.at.z >= f.ground - 1e-9);
        CHECK(p.at.z <= f.eave + 1e-9);
        CHECK(std::strlen(farKitName(p.kit)) > 0);
    }
    for (int i = 0; i < l.boxCount; ++i) {
        CHECK(l.boxes[i].along > 0.0);
        CHECK(l.boxes[i].depth > 0.0);
        CHECK(l.boxes[i].height > 0.0);
    }
    // Lanterns hang off the faces (or the boats), never inside the canal's far wall.
    for (int i = 0; i < l.lanternCount; ++i) CHECK(l.lanterns[i].hook.x < spec.canalX1);
}

TEST_CASE("hero far bank: posts every 2.5 m at most on the near quay, a rope sagging between") {
    const HeroFarBankSpec spec;
    static HeroFarBankLayout l;
    heroFarBankLayout(spec, l);
    CHECK(l.postCount >= 15);
    CHECK(l.ropeCount >= 12);
    for (int i = 0; i < l.postCount; ++i) {
        const QuayPost& p = l.posts[i];
        CHECK(p.foot.x == doctest::Approx(spec.ropeX));
        CHECK(p.foot.x > spec.nearRailX);
        CHECK(p.foot.x < spec.canalX1 + 0.5);
        CHECK(p.foot.z == doctest::Approx(spec.nearGround));
        CHECK(p.foot.y >= spec.ropeY0 - 1e-9);
        CHECK(p.foot.y <= spec.ropeY1 + 1e-9);
        for (int c = 0; c < spec.ropeClearCount; ++c)
            CHECK_FALSE((p.foot.y > spec.ropeClear[c].y0 && p.foot.y < spec.ropeClear[c].y1));
    }
    for (int i = 0; i < l.ropeCount; ++i) {
        const RopeSpan& r = l.ropes[i];
        const double pitch = r.pt[kRopePoints - 1].y - r.pt[0].y;
        CHECK(pitch >= 1.6);
        CHECK(pitch <= spec.postPitch + 1e-9);
        CHECK(r.pt[0].z == doctest::Approx(spec.nearGround + spec.ropeUp));
        CHECK(r.pt[kRopePoints - 1].z == doctest::Approx(spec.nearGround + spec.ropeUp));
        CHECK(r.pt[kRopePoints / 2].z == doctest::Approx(spec.nearGround + spec.ropeUp - spec.ropeSag));
        for (int p = 1; p < kRopePoints; ++p) CHECK(r.pt[p].y > r.pt[p - 1].y);
        // Each rope runs post to post, never over a gap.
        CHECK(clearOf(r.pt[0].y, r.pt[kRopePoints - 1].y, spec.ropeClear, spec.ropeClearCount));
    }
    CHECK(heroRopeZ(spec, 0.5) == doctest::Approx(spec.nearGround + spec.ropeUp - spec.ropeSag));
}

TEST_CASE("hero far bank: boats float on the water by the far quay, each with a lantern and a lamp slot") {
    const HeroFarBankSpec spec;
    static HeroFarBankLayout l;
    heroFarBankLayout(spec, l);
    CHECK(l.boatCount == 3);
    for (int i = 0; i < l.boatCount; ++i) {
        const FarBoat& b = l.boats[i];
        CHECK(b.centre.z == doctest::Approx(spec.waterZ));
        CHECK(b.centre.x - 0.5 * b.beam >= spec.waterX0);
        CHECK(b.centre.x + 0.5 * b.beam <= spec.waterX1);
        // Clear of the footbridge (y 560.8..563.2) and the far ladder at 582.5.
        CHECK(clearOf(b.centre.y - 0.5 * b.length, b.centre.y + 0.5 * b.length, spec.frontClear + 2, 2));
        REQUIRE(b.lamp >= 0);
        REQUIRE(b.lamp < l.lampCount);
        CHECK(l.lamps[b.lamp].at.z < b.lanternHook.z);
        CHECK(b.lanternHook.x < spec.waterX1);
        if (i > 0) CHECK(b.centre.y - l.boats[i - 1].centre.y >= b.length);
    }
}

TEST_CASE("hero far bank: trees between the street trees, out of the lane mouths, deterministic") {
    const HeroFarBankSpec spec;
    static HeroFarBankLayout a, b;
    heroFarBankLayout(spec, a);
    heroFarBankLayout(spec, b);
    CHECK(a.treeCount >= 5);
    for (int i = 0; i < a.treeCount; ++i) {
        const FarTree& t = a.trees[i];
        CHECK(t.foot.x == doctest::Approx(spec.treeX));
        CHECK(t.model >= 0);
        CHECK(t.model < kFarTreeModels);
        CHECK(t.height >= spec.treeHeight - spec.treeHeightVar - 1e-9);
        CHECK(t.height <= spec.treeHeight + spec.treeHeightVar + 1e-9);
        for (int k = 0; k < spec.oldTreeCount; ++k) CHECK(std::fabs(t.foot.y - spec.oldTreeY[k]) >= spec.treeClear);
        CHECK(clearOf(t.foot.y, t.foot.y, spec.treeMouth, spec.treeMouthCount));
        if (i > 0) CHECK(t.foot.y - a.trees[i - 1].foot.y >= spec.treePitch - 1e-9);
    }
    REQUIRE(a.frontCount == b.frontCount);
    REQUIRE(a.pieceCount == b.pieceCount);
    REQUIRE(a.boxCount == b.boxCount);
    for (int i = 0; i < a.frontCount; ++i) {
        CHECK(a.fronts[i].y0 == b.fronts[i].y0);
        CHECK(a.fronts[i].storeys == b.fronts[i].storeys);
        CHECK(a.fronts[i].shop == b.fronts[i].shop);
    }
    for (int i = 0; i < a.treeCount; ++i) CHECK(a.trees[i].height == b.trees[i].height);
    HeroFarBankSpec other = spec;
    other.seed = 0x1234u;
    static HeroFarBankLayout c;
    heroFarBankLayout(other, c);
    CHECK_FALSE(c.overflow);
}
