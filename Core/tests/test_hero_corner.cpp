#include <doctest/doctest.h>
#include "ftcore/HeroCorner.h"
#include "ftcore/MassingDress.h"
#include "ftcore/Roads.h"
#include "ftcore/WindowFacade.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ftcore;

TEST_CASE("hero corner: the lane, the bays and the eave stand where the plan puts them") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);

    CHECK(l.pave.cx == doctest::Approx(904.5));
    CHECK(l.pave.cy == doctest::Approx(535.0));
    CHECK(l.pave.sx == doctest::Approx(7.0));
    CHECK(l.pave.sy == doctest::Approx(40.0));
    CHECK(l.pave.z1 == doctest::Approx(4.20));
    CHECK(l.pave.z1 - l.pave.z0 == doctest::Approx(0.08));

    // Five 8.1 m bays (nine kit steps) from y 514.75 to 555.25, centred on the lane, x 908..916, each touching the next.
    std::set<std::string> ids;
    for (int i = 0; i < kHeroBays; ++i) {
        const MassingBox& b = l.bays[i];
        CHECK(b.cx - 0.5 * b.sx == doctest::Approx(908.0));
        CHECK(b.cx + 0.5 * b.sx == doctest::Approx(916.0));
        CHECK(b.cy - 0.5 * b.sy == doctest::Approx(514.75 + 8.1 * i));
        CHECK(b.sy == doctest::Approx(8.1));
        CHECK(b.sy / kLotGrid == doctest::Approx(9.0));
        CHECK(b.z1 == doctest::Approx(4.18 + 6.6));
        CHECK(b.z0 < 4.18);
        CHECK(b.rot == 0.0);
        CHECK(massingVisible(massingRole(b)));
        CHECK(massingLookIsBuilding(massingLookSurface(b)));
        int index = -1;
        REQUIRE(heroCornerBay(b.id, &index));
        CHECK(index == i);
        ids.insert(b.id);
    }
    CHECK(ids.size() == static_cast<std::size_t>(kHeroBays));
    CHECK(l.bayLit[0]);
    CHECK_FALSE(l.bayLit[1]);
    CHECK(l.bayLit[2]);
    CHECK(l.bayLit[3]);
    CHECK_FALSE(l.bayLit[4]);
    CHECK(l.eateryBay == 2);

    // The eave: 1.2 m over the lane at 3.65 m (clear of the kit's 3.6 m shop storey), in two runs between the lattice
    // fronts (bays 1 and 3): bay 2 alone, and bays 4 and 5.
    REQUIRE(l.eaveCount == 2);
    for (int r = 0; r < 2; ++r) {
        const MassingBox& e = l.eaves[r];
        CHECK(e.id == "HC-EAVE-" + std::to_string(r + 1));
        CHECK(e.cx - 0.5 * e.sx == doctest::Approx(906.8));
        CHECK(e.cx + 0.5 * e.sx == doctest::Approx(908.0));
        CHECK(e.z0 == doctest::Approx(4.18 + 3.65));
        CHECK(e.z0 > 4.18 + kShopStorey);
    }
    CHECK(l.eaves[0].cy - 0.5 * l.eaves[0].sy == doctest::Approx(522.85));
    CHECK(l.eaves[0].cy + 0.5 * l.eaves[0].sy == doctest::Approx(530.95));
    CHECK(l.eaves[1].cy - 0.5 * l.eaves[1].sy == doctest::Approx(539.05));
    CHECK(l.eaves[1].cy + 0.5 * l.eaves[1].sy == doctest::Approx(555.25));

    // No lattice fronts: one run end to end. Every front a lattice: none.
    HeroCornerSpec none;
    for (bool& k : none.bayKoshi) k = false;
    HeroCornerLayout ln;
    heroCornerLayout(none, ln);
    REQUIRE(ln.eaveCount == 1);
    CHECK(ln.eaves[0].sy == doctest::Approx(40.5));
    HeroCornerSpec all;
    for (bool& k : all.bayKoshi) k = true;
    HeroCornerLayout la;
    heroCornerLayout(all, la);
    CHECK(la.eaveCount == 0);
    // Alternating fronts: the most runs there can be, each a bay wide.
    HeroCornerSpec alt;
    for (int i = 0; i < kHeroBays; ++i) alt.bayKoshi[i] = i % 2 == 1;
    HeroCornerLayout lt;
    heroCornerLayout(alt, lt);
    CHECK(lt.eaveCount == kHeroBays / 2 + 1);
}

TEST_CASE("hero corner: every bay box fronts the lane as a shophouse with a rendered, tiled, board or brick wall") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);
    for (int i = 0; i < kHeroBays; ++i) {
        const MassingBox& b = l.bays[i];
        MassingWindowData data;
        data.streetBits = static_cast<std::uint8_t>(1u << kHeroShopFace);
        for (double& base : data.base) base = spec.baySink;
        INFO("bay " << i + 1 << " id " << b.id);
        CHECK(massingFacadeData(b, data).type == WindowType::Shophouse);
        const MassingLookSurface wall = massingLookSurface(b);
        CHECK(wall != MassingLookSurface::BuildingB);
        CHECK(wall != MassingLookSurface::BuildingC);
    }
}

TEST_CASE("hero corner: six large lanterns, a pair flanking each lit bay's entrance") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);
    REQUIRE(l.lanternCount == static_cast<std::size_t>(kHeroLargeLanterns + kHeroStrings * kHeroStringLanterns));
    // The lit bays in order (1, 3 and 4), their entrances: bay 1's and the eatery's lattice fronts 4.125 m south of
    // their north edges, bay 4's glazed door at its middle.
    REQUIRE(l.pairCount == 3);
    const int bays[] = {0, 2, 3};
    const double entrances[] = {518.725, 534.925, 543.1};
    for (int p = 0; p < 3; ++p) {
        CHECK(l.pairBay[p] == bays[p]);
        CHECK(l.entranceY[p] == doctest::Approx(entrances[p]));
    }
    for (int i = 0; i < kHeroLargeLanterns; ++i) {
        const HeroLantern& lan = l.lanterns[i];
        INFO("lantern " << i);
        CHECK(lan.large);
        CHECK(lan.string == -1);
        CHECK(lan.bay == bays[i / 2]);
        CHECK(lan.pent == spec.bayKoshi[lan.bay]);
        CHECK(lan.scale == doctest::Approx(lan.pent ? spec.pentScale : spec.largeScale));
        const double height = spec.largeHeight * lan.scale, diameter = spec.largeDiameter * lan.scale;
        // Hung from its hook, the mesh at its scale: its centre that times 0.35 m under.
        CHECK(lan.centre.z == doctest::Approx(lan.hook.z - 0.35 * lan.scale));
        CHECK(lan.centre.z + 0.5 * height <= lan.hook.z + 1e-9);
        // Either side of the entrance, outside its opening (a lattice front's 2.25 m, a glazed door's 1.8 m).
        const double half = std::abs(lan.centre.y - entrances[i / 2]);
        CHECK(half == doctest::Approx(0.5 * (lan.pent ? spec.pentPairGap : spec.largePairGap)));
        CHECK(half - 0.5 * diameter > (lan.pent ? 1.125 : 0.9) - 0.3);
        // In its own bay.
        const double y0 = spec.bayY0 + spec.bayWidth * lan.bay;
        CHECK(lan.centre.y - 0.5 * diameter > y0);
        CHECK(lan.centre.y + 0.5 * diameter < y0 + spec.bayWidth);
        // Clear of both strings' cables and small lanterns.
        for (int s = 0; s < kHeroStrings; ++s) CHECK(std::abs(lan.centre.y - spec.stringY[s]) > 1.0);
        if (lan.pent) {
            // Under the pent roof (3.0 m at the face, falling 22 degrees out): its hook under the slope's underside, its
            // paper clear of the fascia 0.85 m out and of the face, its foot over a walker's head.
            CHECK(lan.hook.x == doctest::Approx(908.0 - 0.6));
            CHECK(lan.hook.z < 4.18 + 3.0 - 0.6 * std::tan(22.0 * 3.14159265358979 / 180.0) - 0.04);
            CHECK(lan.centre.x - 0.5 * diameter > 908.0 - 0.85);
            CHECK(lan.centre.x + 0.5 * diameter < 908.0);
            CHECK(lan.centre.z - 0.5 * height >= 4.18 + 1.95);
            continue;
        }
        CHECK(lan.centre.x == doctest::Approx(907.0));
        CHECK(lan.hook.z == doctest::Approx(4.18 + 3.65));
        // It hangs from an eave run's underside, inside its reach (along and out), its top under the slab.
        const MassingBox* over = nullptr;
        for (int r = 0; r < l.eaveCount; ++r) {
            const MassingBox& e = l.eaves[r];
            if (std::abs(lan.centre.y - e.cy) < 0.5 * e.sy - 0.5 * diameter) over = &e;
        }
        REQUIRE(over != nullptr);
        CHECK(lan.hook.z == doctest::Approx(over->z0));
        CHECK(lan.hook.x > over->cx - 0.5 * over->sx);
        // Clear of the shop's spill, which stands 0.4 m out at 2.4 m: the lantern's bottom is over it.
        CHECK(lan.centre.z - 0.5 * height > 4.18 + kMassingShopLightUp);
        // Big: the paper at least 0.8 m tall.
        CHECK(height >= 0.8);
    }
}

TEST_CASE("hero corner: lamp slots under the strings, between each pair and over each lit entrance") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);
    REQUIRE(l.lampCount == kHeroLampSlots);
    CHECK(kHeroLampSlots == 8);
    int strings = 0, pairs = 0, entrances = 0;
    for (int k = 0; k < l.lampCount; ++k) {
        const HeroLampSlot& s = l.lamps[k];
        INFO("slot " << k);
        CHECK(s.gain > 0.f);
        CHECK(s.gain <= 1.f);
        // Over the lane, inside the corner's no-street-lamp box (which the build checks lamps against).
        CHECK(s.at.x < 908.0);
        CHECK(s.at.x > 901.0);
        CHECK(heroCornerKeepsLampOut(spec, s.at.x, s.at.y));
        if (s.kind == HeroLampKind::String) {
            CHECK(s.at.x == doctest::Approx(l.stringLamp[s.index].x));
            CHECK(s.at.z == doctest::Approx(l.stringLamp[s.index].z));
            CHECK(s.gain == doctest::Approx(spec.stringLampGain));
            ++strings;
            continue;
        }
        REQUIRE(s.index >= 0);
        REQUIRE(s.index < l.pairCount);
        CHECK(s.at.y == doctest::Approx(l.entranceY[s.index]));
        const bool pent = spec.bayKoshi[l.pairBay[s.index]];
        if (s.kind == HeroLampKind::Pair) {
            // Between its two lanterns, just under their hooks.
            const HeroLantern& a = l.lanterns[2 * s.index];
            CHECK(s.at.x == doctest::Approx(a.hook.x));
            CHECK(s.at.z == doctest::Approx(a.hook.z - spec.pairLampUnder));
            CHECK(s.gain == doctest::Approx(spec.pairLampGain));
            ++pairs;
        } else {
            // Close to the face, under the pent (whose underside at 0.35 m out is about 2.8 m up) or the eave.
            CHECK(s.at.x == doctest::Approx(908.0 - spec.entranceLampOut));
            CHECK(s.at.z < 4.18 + (pent ? 2.8 : 3.65));
            CHECK(s.at.z > 4.18 + 2.5);  // over the lattice front's head beam
            CHECK(s.gain == doctest::Approx(spec.entranceLampGain));
            ++entrances;
        }
    }
    CHECK(strings == 2);
    CHECK(pairs == 3);
    CHECK(entrances == 3);
}

TEST_CASE("hero corner: the probe points follow the layout") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);
    HeroProbe probes[kHeroMaxProbes];
    const int n = heroCornerProbes(spec, l, probes, kHeroMaxProbes);
    // 6 lanterns; per lit bay 4 face points, the lane and the back wall, and on the two lattices the noren and the
    // lattice; 2 dark bays.
    CHECK(n == 6 + 3 * 6 + 2 * 2 + 2);
    std::set<std::string> names;
    int noren = 0;
    for (int i = 0; i < n; ++i) {
        names.insert(probes[i].name);
        if (std::string(probes[i].name).rfind("noren", 0) == 0) {
            ++noren;
            CHECK(probes[i].at.y == doctest::Approx(l.entranceY[noren == 1 ? 0 : 1]));
        }
        // A lattice room's back wall is read under the noren's foot (1.55 m), so the probe sees wall, not cloth.
        if (std::string(probes[i].name).rfind("backwall bay1", 0) == 0) CHECK(probes[i].at.z < spec.ground + 1.55);
    }
    CHECK(names.size() == static_cast<std::size_t>(n));
    CHECK(noren == 2);
    CHECK(std::string(probes[0].name) == "lantern bay1 y517.3");
    CHECK(probes[0].at.z == doctest::Approx(l.lanterns[0].centre.z));
    // A short buffer takes what fits.
    CHECK(heroCornerProbes(spec, l, probes, 3) == 3);
}

TEST_CASE("hero corner: the lanterns glow amber, low enough for the paper to read") {
    const HeroCornerSpec spec;
    CHECK(spec.lanternGlow > 0.f);
    CHECK(spec.lanternGlow <= 1.f);  // the body under clipping at the hero exposure (block C clipped at 2.0)
    CHECK(spec.lanternColour[0] == doctest::Approx(1.f));
    CHECK(spec.lanternColour[1] < 0.5f);
    CHECK(spec.lanternColour[2] < spec.lanternColour[1]);
    // A deep amber paper: darker than block C's cream (0.80, 0.60, 0.38), its warmth kept.
    CHECK(spec.lanternBase[0] < 0.8f);
    CHECK(spec.lanternBase[1] < spec.lanternBase[0]);
    CHECK(spec.lanternBase[2] < spec.lanternBase[1]);
}

TEST_CASE("hero corner: no street lamp inside the corner, and the camera's foreground kept clear") {
    const HeroCornerSpec spec;
    CHECK(heroCornerKeepsLampOut(spec, 904.5, 530.0));   // mid-lane
    CHECK(heroCornerKeepsLampOut(spec, 904.5, 506.0));   // between the camera and the lane
    CHECK(heroCornerKeepsLampOut(spec, 897.0, 503.0));   // an edge counts
    CHECK_FALSE(heroCornerKeepsLampOut(spec, 896.9, 530.0));
    CHECK_FALSE(heroCornerKeepsLampOut(spec, 904.5, 560.1));
    CHECK_FALSE(heroCornerKeepsLampOut(spec, 904.5, 480.0));
    HeroCornerLayout l;
    heroCornerLayout(spec, l);
    const MassingBox& c = l.nearClear;
    CHECK(c.kind == "clear");
    CHECK(c.cx - 0.5 * c.sx == doctest::Approx(spec.laneX0));  // off the quay rail west of the lane
    CHECK(c.cx + 0.5 * c.sx == doctest::Approx(spec.laneX1));
    CHECK(c.cy - 0.5 * c.sy == doctest::Approx(503.0));
    CHECK(c.cy + 0.5 * c.sy == doctest::Approx(spec.laneY0));
    // The camera stands over it.
    CHECK(spec.eye[1] > c.cy - 0.5 * c.sy);
    CHECK(spec.eye[1] < c.cy + 0.5 * c.sy);
}

TEST_CASE("hero corner: each string spans 6.7 m, sags 0.4 m and carries nine lanterns 0.75 m apart") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);
    for (int s = 0; s < kHeroStrings; ++s) {
        const HeroPoint& a = l.cable[s][0];
        const HeroPoint& b = l.cable[s][kHeroCablePoints - 1];
        CHECK(a.x == doctest::Approx(908.0));
        CHECK(a.z == doctest::Approx(4.18 + 3.55));
        CHECK(a.z < 4.18 + spec.eaveUp);  // it leaves the facade under the eave's height
        CHECK(a.z > 4.18 + kKoshiStorey + 0.3);  // and over the eatery's pent roof
        CHECK(b.x == doctest::Approx(901.3));
        CHECK(b.z == doctest::Approx(4.18 + 3.8));
        CHECK(a.x - b.x == doctest::Approx(6.7));
        // The middle point is 0.4 m under the chord.
        const HeroPoint& mid = l.cable[s][(kHeroCablePoints - 1) / 2];
        CHECK(mid.z == doctest::Approx(0.5 * (a.z + b.z) - 0.4));
        // The cable never rises over its higher end; its lowest point is the middle's, give or take the polyline.
        double lowest = 1e9;
        for (int k = 0; k < kHeroCablePoints; ++k) {
            CHECK(l.cable[s][k].y == doctest::Approx(spec.stringY[s]));
            CHECK(l.cable[s][k].z <= b.z + 1e-9);
            lowest = std::min(lowest, l.cable[s][k].z);
        }
        CHECK(lowest == doctest::Approx(mid.z).epsilon(0.01));

        // Nine lanterns, 0.75 m apart along the plan, centred on the span, each hung smallDrop under the cable.
        int count = 0;
        double prevX = 0.0;
        for (std::size_t i = 0; i < l.lanternCount; ++i) {
            const HeroLantern& lan = l.lanterns[i];
            if (lan.string != s) continue;
            CHECK_FALSE(lan.large);
            const double t = (908.0 - lan.hook.x) / 6.7;
            CHECK(lan.hook.z == doctest::Approx(heroCableZ(spec, t) - spec.smallDrop));
            CHECK(lan.centre.z == doctest::Approx(lan.hook.z - 0.16));
            if (count > 0) CHECK(prevX - lan.hook.x == doctest::Approx(0.75));
            prevX = lan.hook.x;
            // A walker 2.0 m tall passes under every small lantern.
            CHECK(lan.centre.z - 0.5 * spec.smallHeight > 4.18 + 2.0);
            ++count;
        }
        CHECK(count == kHeroStringLanterns);
        const HeroLantern& firstL = l.lanterns[kHeroLargeLanterns + s * kHeroStringLanterns];
        const HeroLantern& lastL = l.lanterns[kHeroLargeLanterns + s * kHeroStringLanterns + kHeroStringLanterns - 1];
        CHECK(908.0 - firstL.hook.x == doctest::Approx(lastL.hook.x - 901.3));

        // The post stands on the lane's canal edge; the pool's lamp slot sits under the cable's middle.
        CHECK(l.postFoot[s].x == doctest::Approx(901.3));
        CHECK(l.postFoot[s].z == doctest::Approx(4.18));
        CHECK(4.18 + spec.postHeight > b.z);
        CHECK(l.stringLamp[s].x == doctest::Approx(904.65));
        CHECK(l.stringLamp[s].z == doctest::Approx(mid.z - 0.35));
    }
    CHECK(l.stringLampGain == doctest::Approx(0.5f));
}

TEST_CASE("hero corner: the camera stands at the corner's south end looking north down the lane") {
    HeroCornerLayout l;
    heroCornerLayout(HeroCornerSpec{}, l);
    CHECK(l.eye.x == doctest::Approx(904.5));
    CHECK(l.eye.y == doctest::Approx(511.0));
    CHECK(l.eye.z == doctest::Approx(4.18 + 1.6));
    CHECK(l.at.y - l.eye.y == doctest::Approx(40.0));
    const double pitch = std::atan2(l.at.z - l.eye.z, l.at.y - l.eye.y) * 180.0 / 3.14159265358979;
    CHECK(pitch == doctest::Approx(-3.0).epsilon(0.01));
    CHECK(l.lensMm == doctest::Approx(35.0));
    // Every lantern is in front of it and inside the 35 mm frame's width (36 mm across).
    for (std::size_t i = 0; i < l.lanternCount; ++i) {
        const double dy = l.lanterns[i].centre.y - l.eye.y;
        CHECK(dy > 5.0);
        CHECK(std::abs(l.lanterns[i].centre.x - l.eye.x) / dy < 18.0 / 35.0);
    }
}

TEST_CASE("hero corner: a bay's id is told apart from the rest") {
    int index = -1;
    CHECK(heroCornerBay("HC-BAY-1a", &index));
    CHECK(index == 0);
    CHECK(heroCornerBay("HC-BAY-5c", &index));
    CHECK(index == 4);
    CHECK_FALSE(heroCornerBay("HC-BAY-6a", &index));
    CHECK_FALSE(heroCornerBay("HC-BAY-", &index));
    CHECK_FALSE(heroCornerBay("HC-PAVE", &index));
    CHECK_FALSE(heroCornerBay("YD-0175", &index));
    CHECK(heroCornerBay("HC-BAY-3a", nullptr));
}

namespace {

std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

} // namespace

TEST_CASE("hero corner: the bays as kit lots fill each bay's box with real shop fronts and flat uppers") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);
    HeroLots lots;
    heroCornerLots(spec, lots);
    KitTable kit;
    std::string err;
    REQUIRE_MESSAGE(parseKitTable(readText(std::string(FTCORE_PROJECT_DATA_DIR) + "/Look/building_kit_pieces.csv"), kit, &err), err);

    // The run lies along the bays' lane face, north to south, so its inward side is east (into the bays).
    CHECK(lots.run.ax == doctest::Approx(908.0));
    CHECK(lots.run.ay == doctest::Approx(555.25));
    CHECK(lots.run.by == doctest::Approx(514.75));
    double ix = 0.0, iy = 0.0;
    lots.run.toPlan(0.0, 1.0, ix, iy);
    CHECK(ix == doctest::Approx(909.0));
    CHECK(iy == doctest::Approx(555.25));

    const char* const ground[kHeroBays] = {"SHOP_KOSHI_54", "SHOP_SHUTTER_63", "SHOP_KOSHI_54", "SHOP_GLAZED_81", "SHOP_SHUTTER_63"};
    CHECK(lots.eatery == spec.eateryBay);
    int glazed = 0;
    for (int i = 0; i < kHeroBays; ++i) {
        const BuildingLot& lot = lots.lots[i];
        const MassingBox& b = l.bays[i];
        INFO("bay " << i + 1);
        CHECK(lot.units() == 9);
        CHECK(lot.floors == 2);
        CHECK(lot.flatUppers);
        CHECK(lot.signSet == 0);
        // The shop storeys stand 3.6 m to the box's top; a lattice front's timber storey is 3.0, so it stands 0.6 lower.
        CHECK(lot.top() == doctest::Approx(b.z1 - (spec.bayKoshi[i] ? kShopStorey - kKoshiStorey : 0.0)));
        CHECK(lots.lit[i] == spec.bayLit[i]);
        // The lot's span along the run is the bay's in plan.
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
        lots.run.toPlan(lot.s0, 0.0, x0, y0);
        lots.run.toPlan(lot.s1, 0.0, x1, y1);
        CHECK(std::max(y0, y1) == doctest::Approx(b.cy + 0.5 * b.sy));
        CHECK(std::min(y0, y1) == doctest::Approx(b.cy - 0.5 * b.sy));
        if (i > 0) CHECK(lotTraitDifferences(lot, lots.lots[i - 1]) >= 2);

        std::vector<KitPlacement> out;
        REQUIRE_MESSAGE(facadePlacements(kit, lots.run, lot, i, out, &err), err);
        int shop = 0;
        for (const KitPlacement& p : out) {
            if (p.role != PlacementRole::Piece) continue;
            const std::string id = kit.pieces[static_cast<std::size_t>(p.piece)].id;
            if (id == ground[i]) ++shop;
            // Nothing pushes out under the eave: no balconies, no corridor bays, no awnings.
            CHECK(id != "BAY_BALC_27");
            CHECK(id != "BAY_CORR_27");
            CHECK(id.rfind("AWNING", 0) != 0);
            // Every piece stands in its own bay's span.
            CHECK(p.y >= b.cy - 0.5 * b.sy - 0.6);
            CHECK(p.y <= b.cy + 0.5 * b.sy + 0.6);
        }
        CHECK(shop == 1);
        // The lattice bays are town houses, the other lit ones glazed, the dark ones shuttered.
        CHECK(spec.bayKoshi[i] == (lot.type == LotType::TownHouse));
        if (!spec.bayKoshi[i]) CHECK(spec.bayLit[i] == (lot.type == LotType::Tiled));
        if (lot.type == LotType::Tiled) ++glazed;
    }
    // Two of the three lit fronts are lattices, one glazed at most; the eatery is one of the lattices.
    CHECK(glazed <= 1);
    CHECK(spec.bayKoshi[spec.eateryBay]);
}

TEST_CASE("hero corner: one shop slot per lit bay, deep and high in the room, pitched out through its front") {
    const HeroCornerSpec spec;
    HeroCornerLayout l;
    heroCornerLayout(spec, l);
    MassingShopLight lights[kHeroBays];
    REQUIRE(heroCornerShopLights(spec, lights) == 3);
    // Pitched down less than the city's shops (-55), so its cone reaches out through the front onto the lane.
    CHECK(spec.shopLightPitch < 0.f);
    CHECK(spec.shopLightPitch > -55.f);
    CHECK(spec.shopLightGain > 0.f);
    const int lit[] = {0, 2, 3};
    for (int k = 0; k < 3; ++k) {
        const MassingBox& b = l.bays[lit[k]];
        // Behind the shop face (x 908), past the room's front metre, inside a lattice room's 2.8 m depth (0.12 m behind the
        // face).
        CHECK(lights[k].x > 909.0);
        CHECK(lights[k].x < 908.0 + 0.12 + 2.8 - 0.5);
        CHECK(lights[k].y == doctest::Approx(b.cy));
        CHECK(lights[k].z == doctest::Approx(4.18 + spec.shopLightUp));
        // Under a lattice room's 2.45 m ceiling and its 0.07 m panels.
        CHECK(lights[k].z < 4.18 + 2.45 - 0.07);
        CHECK(lights[k].z < 4.18 + spec.eaveUp);
        CHECK(lights[k].z < 4.18 + kKoshiStorey);
        CHECK(lights[k].faceIndex == kHeroShopFace);
        CHECK(lights[k].face == doctest::Approx(180.0));
    }
}

TEST_CASE("hero corner: the quay masts in the corner's view become rail pieces; the rest stay masts") {
    const ftcore::HeroCornerSpec spec;
    std::vector<ftcore::StandInPoint> pts(4);
    pts[0].kind = ftcore::StandInKind::LampMast;  // the lane's bank, beside bay 2
    pts[0].x = 900.75; pts[0].y = 524.0; pts[0].z = 4.18; pts[0].hasZ = true; pts[0].rot = 0.0; pts[0].id = "LM-12";
    pts[1].kind = ftcore::StandInKind::LampMast;  // the far bank, up the lane
    pts[1].x = 892.25; pts[1].y = 611.5; pts[1].z = 4.18; pts[1].hasZ = true; pts[1].rot = 180.0; pts[1].id = "LM-8";
    pts[2].kind = ftcore::StandInKind::LampMast;  // the next canal, out of the corner's view
    pts[2].x = 900.75; pts[2].y = 689.0;
    pts[3].kind = ftcore::StandInKind::StreetTree;  // not a mast
    pts[3].x = 895.0; pts[3].y = 530.0;
    CHECK(ftcore::heroCornerClearMasts(spec, pts) == 2);
    for (int i = 0; i < 2; ++i) {
        CHECK(pts[i].kind == ftcore::StandInKind::Railing);
        CHECK(pts[i].len == doctest::Approx(ftcore::kQuayMastPlate));
        CHECK(pts[i].hasTop);
        CHECK(pts[i].top == doctest::Approx(4.18 + ftcore::kQuayRailHigh));
        CHECK(std::fmod(pts[i].rot, 180.0) == doctest::Approx(90.0));  // along the north-south rail
    }
    CHECK(pts[0].id == "LM-12-RAIL");
    CHECK(pts[2].kind == ftcore::StandInKind::LampMast);
    CHECK(pts[3].kind == ftcore::StandInKind::StreetTree);
}

TEST_CASE("lamp slot gain: an empty table keeps every lamp's light; a slot past the table keeps it too") {
    CHECK(lampSlotGain(nullptr, 0, 0) == 1.f);
    CHECK(lampSlotGain(nullptr, 0, 63) == 1.f);
    const float gains[] = {1.f, 0.5f};
    CHECK(lampSlotGain(gains, 2, 0) == 1.f);
    CHECK(lampSlotGain(gains, 2, 1) == 0.5f);
    CHECK(lampSlotGain(gains, 2, 2) == 1.f);
}

TEST_CASE("lamp pool merge: one set gives the pool it had before sets merged") {
    const LampPoolPoint pts[] = {{100.0, 200.0, 300.0}, {400.0, 500.0, 600.0}, {700.0, 800.0, 900.0}};
    const float gains[] = {0.8f, 0.5f};
    const LampPoolSet one{pts, 3, gains, 2};
    std::vector<LampPoolSlot> slots;
    lampPoolMerge(&one, 1, slots);
    REQUIRE(slots.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(slots[i].at.x == pts[i].x);
        CHECK(slots[i].at.y == pts[i].y);
        CHECK(slots[i].at.z == pts[i].z);
        CHECK(slots[i].gain == lampSlotGain(gains, 2, i));
        CHECK(slots[i].set == 0);
        CHECK(slots[i].index == i);
    }
    // No table: every lamp keeps its light. Nothing to clash with in one set.
    const LampPoolSet bare{pts, 3, nullptr, 0};
    lampPoolMerge(&bare, 1, slots);
    REQUIRE(slots.size() == 3);
    CHECK(slots[2].gain == 1.f);
    CHECK(lampPoolClashes(slots, 50.0, nullptr, nullptr) == 0);
    lampPoolMerge(nullptr, 0, slots);
    CHECK(slots.empty());
}

TEST_CASE("lamp pool merge: two sets append in order with their own gains; a light claimed by both is a clash") {
    const LampPoolPoint strip[] = {{0.0, 0.0, 0.0}, {1000.0, 0.0, 0.0}};
    const LampPoolPoint corner[] = {{5000.0, 0.0, 0.0}, {1010.0, 0.0, 0.0}};
    const float stripGains[] = {0.5f};
    const float cornerGains[] = {2.f, 3.f};
    const LampPoolSet sets[] = {{strip, 2, stripGains, 1}, {corner, 2, cornerGains, 2}};
    std::vector<LampPoolSlot> slots;
    lampPoolMerge(sets, 2, slots);
    REQUIRE(slots.size() == 4);
    CHECK(slots[0].gain == 0.5f);
    CHECK(slots[1].gain == 1.f);
    CHECK(slots[2].gain == 2.f);
    CHECK(slots[3].gain == 3.f);
    CHECK(slots[2].set == 1);
    CHECK(slots[3].index == 1);
    std::size_t a = 99, b = 99;
    CHECK(lampPoolClashes(slots, 50.0, &a, &b) == 1);
    CHECK(a == 1);
    CHECK(b == 3);
    CHECK(lampPoolClashes(slots, 5.0, nullptr, nullptr) == 0);
    // Two lights of one set side by side are not a clash.
    const LampPoolPoint pair[] = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}};
    const LampPoolSet same{pair, 2, nullptr, 0};
    lampPoolMerge(&same, 1, slots);
    CHECK(lampPoolClashes(slots, 50.0, nullptr, nullptr) == 0);
}
