// The roads' wear (ftcore/RoadDecals.h) over a made-up road, and the backdrop's rock faces and steep share
// (ftcore/Backdrop.h).

#include "ftcore/Backdrop.h"
#include "ftcore/RoadDecals.h"
#include "ftcore/Roads.h"

#include <ostream>
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

const char* const kDecalRoadCsv =
    "# a test road\n"
    "id,kind,width_m,pts,pavement_m,lane_m,speed_kmh,one_way,ground_z,mouth_back_m,mouth_z,mouths,bus_stops,parking,"
    "rail,poles,ends\n"
    "RD-01,road,14,885:410;885:650,3.5,3.5,40,0,4.0,1.5,4.12,"
    "CL-046@10.5@6@L;CL-047@60.5@6@L;CL-048@118.5@6@L@18;CL-049@160.5@6@L;CL-051@215.5@6@L,64@R,LR,R,R,closed\n";

RoadBuild theBuild() {
    std::vector<RoadSpec> roads;
    std::string err;
    REQUIRE_MESSAGE(importRoads(kDecalRoadCsv, roads, &err), err);
    REQUIRE(roads.size() == 1);
    RoadBuild b;
    roadBuild(roads[0], b);
    return b;
}

// The plan point's offset from a box's centre in its own u, w frame.
void toLocal(const MassingBox& b, double x, double y, double& u, double& w) {
    const double r = b.rot * 3.14159265358979323846 / 180.0;
    const double dx = x - b.cx, dy = y - b.cy;
    u = dx * std::cos(r) + dy * std::sin(r);
    w = -dx * std::sin(r) + dy * std::cos(r);
}

// The slab whose inset top holds the decal's footprint (its four corners), or null.
const RoadSolid* holder(const RoadBuild& b, const RoadDecal& d) {
    const double r = d.rot * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
    for (const RoadSolid& solid : b.solids) {
        if (!roadDecalSlab(solid)) continue;
        bool inside = true;
        for (int k = 0; k < 4 && inside; ++k) {
            const double a = (k & 1 ? 0.5 : -0.5) * d.sx, e = (k & 2 ? 0.5 : -0.5) * d.sy;
            double u = 0.0, w = 0.0;
            toLocal(solid.box, d.cx + a * c - e * s, d.cy + a * s + e * c, u, w);
            inside = std::fabs(u) <= 0.5 * solid.box.sx + 1e-6 && std::fabs(w) <= 0.5 * solid.box.sy + 1e-6;
        }
        if (inside) return &solid;
    }
    return nullptr;
}

} // namespace

TEST_CASE("road decals: slabs only, every kind, each inside its slab and just over its top") {
    const RoadBuild b = theBuild();
    std::vector<RoadDecal> out;
    RoadDecalCounts counts;
    RoadDecalSpec spec;
    spec.density = 3.0; // a short road: enough of each to test
    roadDecals(b, spec, out, counts); // the build's: stop-line oil too (tactile is opt-in)
    REQUIRE(!out.empty());
    CHECK(counts.total() == out.size());
    CHECK(counts.slabs > 0);
    CHECK(counts.asphaltM2 > 1000.0);
    CHECK(counts.pavementM2 > 300.0);
    for (std::size_t k = 0; k < kRoadDecalKinds; ++k) {
        if (k == static_cast<std::size_t>(RoadDecalKind::Tactile)) continue; // off by default: the builder lays them
        CHECK_MESSAGE(counts.perKind[k] > 0, "kind ", k);
    }
    std::size_t outside = 0;
    for (const RoadDecal& d : out) {
        CHECK(d.sx >= 0.3 - 1e-9);
        CHECK(d.sy > 0.0);
        CHECK(d.seed >= 0.f);
        CHECK(d.seed < 1.f);
        const RoadSolid* s = holder(b, d);
        if (!s) {
            ++outside;
            continue;
        }
        // Its top over its slab's top there by 1.5 to 5.8 mm (each kind its own lift, under the markings' 6 mm).
        double u = 0.0, w = 0.0;
        toLocal(s->box, d.cx, d.cy, u, w);
        const double slabTop = s->box.z1 + s->box.riseU * u + s->box.riseW * w;
        CHECK(d.top - slabTop >= 0.0014);
        CHECK(d.top - slabTop <= 0.0058);
        // It knows its ground (the material blends its edge to it); tactile strips on the pavement, the rest of the new
        // kinds on the carriageway.
        CHECK(d.pavement == (s->box.kind == "pavement"));
        if (d.kind == RoadDecalKind::Tactile) CHECK(d.pavement);
        if (d.kind == RoadDecalKind::Oil || d.kind == RoadDecalKind::Drain) CHECK(!d.pavement);
        // Manholes and grime only on the crowned halves; patches never on the pavement.
        if (d.kind == RoadDecalKind::Patch || d.kind == RoadDecalKind::Manhole || d.kind == RoadDecalKind::KerbGrime) {
            CHECK(s->box.kind != "pavement");
        }
    }
    // Overlapping slabs (a stub over its road) may hold a footprint between them; nearly all lie inside one.
    CHECK(outside * 20 <= out.size());
}

TEST_CASE("road decals: kerb grime lies along the low edge and faces the kerb") {
    const RoadBuild b = theBuild();
    std::vector<RoadDecal> out;
    RoadDecalCounts counts;
    RoadDecalSpec spec;
    spec.grimeChance = 1.0;
    roadDecals(b.solids.data(), b.solids.size(), spec, out, counts);
    std::size_t grime = 0;
    for (const RoadDecal& d : out) {
        if (d.kind != RoadDecalKind::KerbGrime) continue;
        ++grime;
        // Turned so its +w is the gutter: its top falls towards +w.
        CHECK(d.riseW < 0.0);
        CHECK(d.riseW == doctest::Approx(-kRoadCrossfall).epsilon(1e-6));
        CHECK(d.sy <= kRoadGrimeWide + 1e-9);
        CHECK(d.sx <= kRoadGrimeRun + 1e-9);
    }
    CHECK(grime > 50); // a 240 m road's two gutters, a piece each 4 m
}

TEST_CASE("road decals: repeatable, density scales them, and the cap holds") {
    const RoadBuild b = theBuild();
    RoadDecalSpec spec;
    std::vector<RoadDecal> a, c;
    RoadDecalCounts ca, cc;
    roadDecals(b.solids.data(), b.solids.size(), spec, a, ca);
    roadDecals(b.solids.data(), b.solids.size(), spec, c, cc);
    REQUIRE(a.size() == c.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].cx == c[i].cx);
        CHECK(a[i].cy == c[i].cy);
        CHECK(a[i].seed == c[i].seed);
    }

    RoadDecalSpec dense = spec;
    dense.density = 4.0;
    std::vector<RoadDecal> d;
    RoadDecalCounts cd;
    roadDecals(b.solids.data(), b.solids.size(), dense, d, cd);
    CHECK(cd.perKind[static_cast<std::size_t>(RoadDecalKind::Crack)] >
          2 * ca.perKind[static_cast<std::size_t>(RoadDecalKind::Crack)]);

    RoadDecalSpec capped = dense;
    capped.maxDecals = 40;
    std::vector<RoadDecal> e;
    RoadDecalCounts ce;
    roadDecals(b.solids.data(), b.solids.size(), capped, e, ce);
    CHECK(e.size() == 40);
    CHECK(ce.capped > 0);
    CHECK(ce.total() == 40);

    RoadDecalSpec none = spec;
    none.density = 0.0;
    std::vector<RoadDecal> f;
    RoadDecalCounts cf;
    roadDecals(b.solids.data(), b.solids.size(), none, f, cf);
    CHECK(f.empty());
    roadDecals(nullptr, 5, spec, f, cf);
    CHECK(f.empty());
}

TEST_CASE("road decals: the box and data the engine lays them by") {
    RoadDecal d;
    d.kind = RoadDecalKind::Manhole;
    d.cx = 10.0;
    d.cy = -4.0;
    d.sx = 0.7;
    d.sy = 0.7;
    d.rot = 30.0;
    d.top = 4.2;
    d.riseW = -0.02;
    d.seed = 0.25f;
    const MassingBox box = roadDecalBox(d, 0.02);
    CHECK(box.z1 == doctest::Approx(4.2));
    CHECK(box.z0 == doctest::Approx(4.18));
    CHECK(box.riseW == doctest::Approx(-0.02));
    CHECK(box.rot == doctest::Approx(30.0));
    // roadCubeCentre's top face centre lies over (cx, cy) at z1.
    const MassingVec mid = roadCubeCentre(box, 0.02);
    const MassingCubePose pose = massingCubePose(box, 0.02);
    CHECK(pose.scale.y == doctest::Approx(0.7 / std::cos(std::atan(-0.02))));
    CHECK(std::hypot(mid.x - 10.0, mid.y + 4.0) < 0.001);

    float data[kRoadDecalData] = {};
    roadDecalData(d, data);
    CHECK(data[0] == doctest::Approx(2.0));
    d.pavement = true;
    roadDecalData(d, data);
    CHECK(data[0] == doctest::Approx(10.0)); // kind plus 8 on a pavement
    d.pavement = false;
    CHECK(data[1] == doctest::Approx(0.25));
    CHECK(data[2] == doctest::Approx(0.7));
    CHECK(data[3] == doctest::Approx(0.7));

    const char* code = roadDecalHlsl();
    REQUIRE(code != nullptr);
    for (const char* name : {"LocalPos", "Kind", "Seed", "SizeU", "SizeW", "Wetness", "WorldPos", "RoughOut", "MaskOut",
                             "return C;", "C = lerp(Under, C, Blend);", "WSoak", "Kd == 6"}) {
        CHECK_MESSAGE(std::strstr(code, name) != nullptr, name);
    }
    // Spliced once: the gutter width is a number, and the same pointer comes back.
    CHECK(std::strstr(code, "GUTTER_M") == nullptr);
    CHECK(roadDecalHlsl() == code);
}

TEST_CASE("road decals: bays get oil, gutters get drains, street kinds by tint") {
    const RoadBuild b = theBuild();
    std::vector<RoadDecal> plain, full;
    RoadDecalCounts cp, cf;
    RoadDecalSpec spec;
    roadDecals(b.solids.data(), b.solids.size(), spec, plain, cp);
    roadDecals(b, spec, full, cf);
    CHECK(!RoadDecalSpec{}.tactile); // off by default: the builder lays tactile strips at the ramps
    const auto n = [](const RoadDecalCounts& c, RoadDecalKind k) { return c.perKind[static_cast<std::size_t>(k)]; };
    CHECK(n(cp, RoadDecalKind::Drain) > 4); // two gutters of a 240 m road, one each 30-45 m
    CHECK(n(cp, RoadDecalKind::Tactile) == 0);
    CHECK(n(cf, RoadDecalKind::Tactile) == 0); // the builder lays tactile solids at the ramps; the decal is opt-in
    CHECK(n(cf, RoadDecalKind::Oil) >= n(cp, RoadDecalKind::Oil));
    CHECK(full.size() >= plain.size());
    for (const RoadDecal& d : full) {
        if (d.kind == RoadDecalKind::Drain) CHECK(d.riseW < 0.0); // turned so its +w is the gutter, as kerb grime
    }

    CHECK(roadStreetKind(RoadTint::Asphalt) == 1);
    CHECK(roadStreetKind(RoadTint::AsphaltPatch) == 1);
    CHECK(roadStreetKind(RoadTint::Pavement) == 2);
    CHECK(roadStreetKind(RoadTint::Kerb) == 3);
    CHECK(roadStreetKind(RoadTint::LineWhite) == 4);
    CHECK(roadStreetKind(RoadTint::LineYellow) == 4);
    CHECK(roadStreetKind(RoadTint::Steel) == 0);
}

TEST_CASE("backdrop: rock faces and the steep share") {
    MassingBox cliff;
    cliff.kind = "cliff";
    MassingBox terrace;
    terrace.kind = "terrace";
    MassingBox wall;
    wall.kind = "wall";
    MassingBox seawall;
    seawall.kind = "seawall";
    CHECK(backdropRockFace(cliff));
    CHECK(backdropRockFace(terrace));
    CHECK(!backdropRockFace(wall));
    CHECK(!backdropRockFace(seawall));

    // A 21 x 21 field 1 m apart: flat for x < 10, then a 1 in 1 ramp (massingHeightFromValue's encoding).
    std::vector<std::uint16_t> grid(21 * 21);
    for (std::size_t y = 0; y < 21; ++y) {
        for (std::size_t x = 0; x < 21; ++x) {
            const double h = x < 10 ? 0.0 : static_cast<double>(x - 10);
            grid[y * 21 + x] = static_cast<std::uint16_t>(32768.0 + h * 128.0);
        }
    }
    const HeightField field{grid.data(), 21, 21, 1.0, 0.0, 0.0};
    const double all = heightSteepShare(field, 1.0, 1.0, 19.0, 19.0, 1.0, 0.7);
    CHECK(all > 0.35);
    CHECK(all < 0.6);
    CHECK(heightSteepShare(field, 1.0, 1.0, 8.0, 19.0, 1.0, 0.7) == doctest::Approx(0.0));
    CHECK(heightSteepShare(field, 12.0, 1.0, 19.0, 19.0, 1.0, 0.7) == doctest::Approx(1.0));
    CHECK(heightSteepShare(HeightField{}, 0.0, 0.0, 1.0, 1.0, 1.0, 0.7) == doctest::Approx(0.0));
    CHECK(heightSteepShare(field, 5.0, 5.0, 4.0, 9.0, 1.0, 0.7) == doctest::Approx(0.0));
}

TEST_CASE("backdrop: scrub clumps are low, lumpy, varied and repeatable") {
    BackdropLump a[kBackdropClumpMax], b[kBackdropClumpMax];
    BackdropPoint pylon;
    pylon.kind = BackdropKind::Pylon;
    CHECK(backdropClump(pylon, a) == 0);
    CHECK(backdropClump(pylon, nullptr) == 0);
    bool tints[kBackdropScrubTints] = {};
    for (int i = 0; i < 60; ++i) {
        BackdropPoint p;
        p.x = 12.5f * static_cast<float>(i);
        p.y = -7.f * static_cast<float>(i);
        p.scale = 0.6f + 0.016f * static_cast<float>(i);
        p.kind = i % 2 ? BackdropKind::Tree : BackdropKind::Scrub;
        const std::size_t n = backdropClump(p, a);
        const bool tree = i % 2;
        CHECK(n == (tree ? 4u : 2u));
        CHECK(backdropClump(p, b) == n);
        for (std::size_t k = 0; k < n; ++k) {
            CHECK(a[k].sx == b[k].sx);
            CHECK(a[k].yaw == b[k].yaw);
            CHECK(a[k].trunk == (tree && k == 0));  // a tree's first part is its trunk, nothing else is
            CHECK(a[k].tint < kBackdropScrubTints);
            CHECK(a[k].dz > 0.f);
            if (a[k].trunk) {
                CHECK(a[k].sz >= 3.f * p.scale - 1e-4f);  // a trunk tall enough to read under the crown
                CHECK(a[k].sx < 0.25f * a[k].sz);         // and slim
                CHECK(a[k].dz < 0.5f * a[k].sz);          // its foot sunk into the ground
                continue;
            }
            tints[a[k].tint] = true;
            CHECK(a[k].sz <= (tree ? 5.f : 1.8f));  // crowns to 5 m deep, bushes to 1.8 m
            CHECK(a[k].sz >= 0.45f * a[k].sx);      // rounded, never flat discs
            CHECK(a[k].sz <= a[k].sx);              // nor spires
            if (tree) {
                CHECK(a[k].dz - 0.5f * a[k].sz < a[0].sz);  // the crown sits on the trunk's top, overlapping it
                CHECK(a[k].dz > 0.6f * a[0].sz);
                if (k == 1) CHECK(a[k].sx >= 3.5f * p.scale - 1e-4f);  // crowns read as trees
            } else {
                CHECK(a[k].dz < 0.5f * a[k].sz);  // bushes sunk into the ground
            }
        }
        const std::size_t far = tree ? 2 : 1;
        CHECK(a[far].dx * a[far].dx + a[far].dy * a[far].dy > 0.f); // lumps spread, not stacked
    }
    for (bool t : tints) {
        CHECK(t);
    }
}
