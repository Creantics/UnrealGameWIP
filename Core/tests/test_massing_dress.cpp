// Dressing for a massing map (ftcore/MassingDress.h): kerbs along streets with pavements, a quay's rail, posts, bollards
// and lamps, and pieces set against the faces of buildings from a frontage table. Made-up tables only: the real ones are
// kept outside the repo. Ids and numbers only.

#include "ftcore/MassingDress.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

MassingBox flatBox(const std::string& id, double cx, double cy, double sx, double sy, double z0, double z1,
                   const std::string& kind = "slab") {
    MassingBox b;
    b.id = id;
    b.kind = kind;
    b.cover = true;
    b.cx = cx;
    b.cy = cy;
    b.sx = sx;
    b.sy = sy;
    b.z0 = z0;
    b.z1 = z1;
    return b;
}

MassingStreet street(const std::string& id, const std::string& kind, double width, double pavement,
                     std::vector<MassingVec> points) {
    MassingStreet s;
    s.id = id;
    s.kind = kind;
    s.width = width;
    s.pavement = pavement;
    s.points = std::move(points);
    return s;
}

// A ground grid, 1 m apart from (ox, oy), its heights h(x, y) at the samples (in steps of 1/128 m).
struct Ground {
    std::vector<std::uint16_t> samples;
    std::size_t nx = 0, ny = 0;
    double ox = 0.0, oy = 0.0;
    MassingGround view() const { return {samples.data(), nx, ny, 1.0, ox, oy}; }
};
template <class H> Ground makeGround(std::size_t nx, std::size_t ny, double ox, double oy, H h) {
    Ground g;
    g.nx = nx;
    g.ny = ny;
    g.ox = ox;
    g.oy = oy;
    g.samples.resize(nx * ny);
    for (std::size_t r = 0; r < ny; ++r)
        for (std::size_t c = 0; c < nx; ++c) {
            const double v = 32768.0 + 128.0 * h(ox + static_cast<double>(c), oy + static_cast<double>(r));
            g.samples[r * nx + c] = static_cast<std::uint16_t>(std::lround(v));
        }
    return g;
}

struct Kerbs {
    std::vector<MassingBox> boxes;
    MassingKerbCounts counts;
};
Kerbs kerbs(const std::vector<MassingStreet>& streets, const std::vector<MassingBox>& boxes = {},
            const MassingGround& ground = {}, double dryAbove = -1.0) {
    Kerbs k;
    const std::vector<bool> include(boxes.size(), true);
    massingKerbs(streets, ground, boxes, include, dryAbove, k.boxes, k.counts);
    return k;
}

// A 9 m street (a 6 m carriageway between 1.5 m pavements) east along y = 0 from x 0 to `length`.
MassingStreet lane(const std::string& id, double length) {
    return street(id, "core_lane", 9.0, 1.5, {{0, 0, 0}, {length, 0, 0}});
}

std::size_t countPrefix(const std::vector<MassingBox>& boxes, const std::string& prefix) {
    std::size_t n = 0;
    for (const MassingBox& b : boxes)
        if (b.id.compare(0, prefix.size(), prefix) == 0) ++n;
    return n;
}

// Sea east of x = 20 (3 m down), land west of it (2 m up), from (0, 0) to (60, 40).
Ground shore() { return makeGround(61, 41, 0.0, 0.0, [](double x, double) { return x <= 20.0 ? 2.0 : -3.0; }); }

MassingPropRule lampRule() {
    MassingPropRule r;
    r.id = "LAMP";
    r.asset = "/Game/L.L";
    r.spacing = 25.0;
    r.dz = 0.1;
    r.footX = 0.4;
    r.footY = 0.4;
    return r;
}

struct Quay {
    std::vector<MassingBox> boxes;
    std::vector<MassingProp> lamps;
    MassingQuayCounts counts;
};
Quay quay(const std::vector<MassingBox>& boxes, const std::vector<MassingFoot>* taken = nullptr) {
    Quay q;
    const Ground g = shore();
    const std::vector<bool> include(boxes.size(), true);
    massingQuay(g.view(), boxes, include, 0.5, lampRule(), 7, taken, q.boxes, q.lamps, q.counts);
    return q;
}

// The quay used below: 10 m deep along x 10..20, 20 m long along y 10..30, its top at 4; its east edge is on the sea.
MassingBox quayDeck() { return flatBox("Q-1", 15, 20, 10, 20, -3, 4, "quay"); }

MassingFrontRule front(const std::string& id, double width, std::vector<std::string> kinds) {
    MassingFrontRule r;
    r.id = id;
    r.asset = "/Game/" + id + "." + id;
    r.width = width;
    r.depth = 1.0;
    r.height = 3.5;
    r.out = 1.0;
    r.kinds = std::move(kinds);
    return r;
}

struct Front {
    std::vector<MassingProp> props;
    std::vector<MassingFrontCounts> counts;
    MassingFrontTotals totals;
};
// The first box is the host; every box is included.
Front frontage(const std::vector<MassingFrontRule>& rules, const std::vector<MassingStreet>& streets,
               const std::vector<MassingBox>& boxes, double dryAbove = -1.0, const MassingGround& ground = {},
               const std::vector<MassingFoot>* taken = nullptr) {
    Front f;
    const std::vector<bool> include(boxes.size(), true);
    std::vector<bool> hosts(boxes.size(), false);
    if (!hosts.empty()) hosts[0] = true;
    massingFrontage(rules, streets, ground, boxes, include, hosts, dryAbove, taken, f.props, f.counts, f.totals);
    return f;
}

// A host 20 m along x and 10 m along y at the origin, 8 m high; a street 10 m wide (6 m carriageway) along y = `y`.
MassingBox host(double z0 = 0.0, double z1 = 8.0) { return flatBox("H-1", 0, 0, 20, 10, z0, z1); }
std::vector<MassingStreet> northStreet(double y = 12.0) {
    return {street("S-1", "street", 10.0, 2.0, {{-30, y, 0}, {30, y, 0}})};
}

std::size_t placedBy(const Front& f, std::size_t rule) {
    std::size_t n = 0;
    for (const MassingProp& p : f.props)
        if (p.rule == rule) ++n;
    return n;
}

} // namespace

TEST_CASE("massing dress: a box's foot is its footprint") {
    MassingBox b = flatBox("A", 3, 4, 6, 2, 0, 1);
    b.rot = 30.0;
    const MassingFoot f = massingBoxFoot(b);
    CHECK(near(f.x, 3.0));
    CHECK(near(f.y, 4.0));
    CHECK(near(f.hx, 3.0));
    CHECK(near(f.hy, 1.0));
    CHECK(near(f.rot, 30.0));
}

TEST_CASE("massing kerbs: both edges of a carriageway, in even pieces, as pole concrete") {
    const Kerbs k = kerbs({lane("S-1", 30)});
    CHECK(k.counts.streets == 1);
    REQUIRE(k.boxes.size() == 6);
    CHECK(k.counts.pieces == 6);
    CHECK(near(k.counts.built, 60.0));
    CHECK(near(k.counts.crossing + k.counts.boxed + k.counts.wet + k.counts.shortPieces, 0.0));
    const MassingBox& first = k.boxes[0];
    CHECK(first.id == "S-1-KL1");
    CHECK(first.row == "S-1");
    CHECK(first.kind == "kerb");
    CHECK(first.source == MassingSource::Invented);
    CHECK_FALSE(first.cover);
    CHECK(near(first.cx, 5.0));
    CHECK(near(first.cy, 3.1)); // half the carriageway plus half a kerb
    CHECK(near(first.sx, 10.0));
    CHECK(near(first.sy, kMassingKerbWidth));
    CHECK(near(first.rot, 0.0));
    CHECK(near(first.z0, -kMassingKerbSink));
    CHECK(near(first.z1, kMassingKerbHeight));
    CHECK(massingLookSurface(first) == MassingLookSurface::PoleConcrete);
    CHECK(k.boxes[2].id == "S-1-KL3");
    CHECK(k.boxes[3].id == "S-1-KR1");
    CHECK(near(k.boxes[3].cy, -3.1));
    CHECK(near(k.boxes[5].cx, 25.0));

    // No pavements, or no length: no kerbs.
    MassingStreet bare = lane("S-2", 30);
    bare.pavement = 0.0;
    MassingStreet dot = lane("S-3", 0);
    const Kerbs none = kerbs({bare, dot});
    CHECK(none.boxes.empty());
    CHECK(none.counts.streets == 0);
}

TEST_CASE("massing kerbs: a crossing carriageway and a bend's inside stay open") {
    // A 4 m street with no pavements crossing at x = 15: kerbs stop 2 m + a kerb's width short of its centreline.
    const Kerbs k = kerbs({lane("S-1", 30), street("S-2", "stair", 4.0, 0.0, {{15, -20, 0}, {15, 20, 0}})});
    CHECK(k.counts.streets == 1);
    CHECK(near(k.counts.crossing, 8.0));
    REQUIRE(k.boxes.size() == 8);
    for (const MassingBox& b : k.boxes) {
        CHECK(std::fabs(b.cx - 15.0) - 0.5 * b.sx >= 2.2 - 0.25);
        CHECK(near(b.sx, 6.5));
    }
    CHECK(countPrefix(k.boxes, "S-1-KL") == 4);
    CHECK(countPrefix(k.boxes, "S-1-KR") == 4);

    // A left turn: the inside kerbs stop where the other leg's carriageway begins; the outside ones run to the corner.
    const Kerbs bend = kerbs({street("B-1", "core_lane", 9.0, 1.5, {{0, 0, 0}, {20, 0, 0}, {20, 20, 0}})});
    CHECK(near(bend.counts.crossing, 6.0));
    CHECK(near(bend.counts.built, 74.0));
    for (const MassingBox& b : bend.boxes) {
        if (near(b.cy, 3.1)) CHECK(b.cx + 0.5 * b.sx <= 17.0 + 1e-9);     // first leg, inside
        if (near(b.cx, 16.9)) CHECK(b.cy - 0.5 * b.sx >= 3.0 - 1e-9);     // second leg, inside
    }
    double outside = 0.0;
    for (const MassingBox& b : bend.boxes)
        if (near(b.cy, -3.1) || near(b.cx, 23.1)) outside += b.sx;
    CHECK(near(outside, 40.0));
    // Pieces are numbered along each side in segment order.
    CHECK(bend.boxes.back().id == "B-1-KR4");
    CHECK(near(bend.boxes.back().rot, 90.0));
}

TEST_CASE("massing kerbs: a box standing on the line cuts it, one overhead doesn't") {
    const std::vector<MassingBox> boxes = {flatBox("W", 10, 5, 4, 4, 0, 3), flatBox("O", 10, -5, 4, 4, 5, 8)};
    const Kerbs k = kerbs({lane("S-1", 20)}, boxes);
    CHECK(near(k.counts.boxed, 4.0));
    CHECK(countPrefix(k.boxes, "S-1-KL") == 2);
    CHECK(countPrefix(k.boxes, "S-1-KR") == 2);
    for (const MassingBox& b : k.boxes)
        if (b.id.compare(0, 6, "S-1-KL") == 0) CHECK(near(b.sx, 8.0));

    // A box leaving 0.5 m and 1 m of kerb: the short end is left out.
    const Kerbs shortEnd = kerbs({lane("S-1", 10)}, {flatBox("W", 4.8, 5, 8.4, 4, 0, 3)});
    CHECK(near(shortEnd.counts.boxed, 8.5));
    CHECK(near(shortEnd.counts.shortPieces, 0.5));
    REQUIRE(countPrefix(shortEnd.boxes, "S-1-KL") == 1);
    CHECK(shortEnd.boxes[0].id == "S-1-KL1");
    CHECK(near(shortEnd.boxes[0].cx, 9.5));
    CHECK(near(shortEnd.boxes[0].sx, 1.0));
}

TEST_CASE("massing kerbs: none on wet ground; a grade tilts one piece to it") {
    // Ground 2 m under the water west of x = 9, 1 m up from x = 10.
    const Ground wet = makeGround(41, 21, -5.0, -10.0, [](double x, double) { return x <= 9.0 ? -2.0 : 1.0; });
    const Kerbs k = kerbs({lane("S-1", 30)}, {}, wet.view(), 0.5);
    CHECK(near(k.counts.wet, 20.0));
    REQUIRE(k.boxes.size() == 4);
    for (const MassingBox& b : k.boxes) {
        CHECK(b.cx - 0.5 * b.sx >= 10.0 - 1e-9);
        CHECK(near(b.z0, 1.0 - kMassingKerbSink));
        CHECK(near(b.z1, 1.0 + kMassingKerbHeight));
    }

    CHECK(k.counts.runs == 2);
    for (const MassingBox& b : k.boxes) CHECK(b.riseU == 0.0);

    // Ground climbing 1/32 m per m: one piece a side, tilted to the grade, its line on the ground at its middle.
    const Ground slope = makeGround(41, 21, -5.0, -10.0, [](double x, double) { return x / 32.0; });
    const Kerbs s = kerbs({lane("S-1", 12)}, {}, slope.view());
    REQUIRE(s.boxes.size() == 2);
    CHECK(s.counts.runs == 2);
    for (const MassingBox& b : s.boxes) {
        CHECK(near(b.sx, 12.0));
        CHECK(near(b.riseU, 1.0 / 32.0));
        CHECK(b.riseW == 0.0);
        CHECK(near(b.z0, 6.0 / 32.0 - kMassingKerbSink));
        CHECK(near(b.z1, 6.0 / 32.0 + kMassingKerbHeight));
    }
    CHECK(near(s.counts.built, 24.0));
}

TEST_CASE("massing kerbs: a diagonal street's pieces take its yaw and follow its grade, joined without a step") {
    // A street 30 degrees north of east, 40 m long, over ground curving up: 0.003 x^2 + 0.03 y (grades 0.015 to 0.2
    // along it), so one line no longer fits a 10 m part and pieces join end to end.
    const double toRad = 3.14159265358979323846 / 180.0;
    const double c = std::cos(30.0 * toRad), sn = std::sin(30.0 * toRad);
    const auto h = [](double x, double y) { return 0.003 * x * x + 0.03 * y; };
    const Ground curve = makeGround(71, 61, -15.0, -15.0, h);
    const Kerbs k = kerbs({street("D-1", "core_lane", 9.0, 1.5, {{0, 0, 0}, {40.0 * c, 40.0 * sn, 0}})}, {}, curve.view());
    CHECK(k.counts.runs == 2);
    CHECK(k.boxes.size() > 8);
    CHECK(near(k.counts.built, 80.0));
    const MassingBox* last = nullptr;
    for (const MassingBox& b : k.boxes) {
        // Each piece's yaw follows the segment (Unreal's yaw is the rotation's negative).
        CHECK(near(b.rot, 30.0));
        CHECK(near(massingCubePose(b, b.z1 - b.z0).yaw, -30.0));
        CHECK(std::fabs(b.riseU) <= kMassingKerbGrade);
        CHECK(b.riseU > 0.0);
        // Its top runs kMassingKerbHeight over the ground along its length.
        for (const double f : {-0.5, -0.25, 0.0, 0.25, 0.5}) {
            const double u = f * b.sx;
            const double top = b.z1 + b.riseU * u;
            CHECK(std::fabs(top - kMassingKerbHeight - h(b.cx + c * u, b.cy + sn * u)) <= kMassingKerbFit + 0.015);
        }
        // A piece on the same side as the last starts where it ended, at its height.
        if (last && last->id.compare(0, 6, b.id.substr(0, 6)) == 0) {
            const double endX = last->cx + c * 0.5 * last->sx, endY = last->cy + sn * 0.5 * last->sx;
            const double startX = b.cx - c * 0.5 * b.sx, startY = b.cy - sn * 0.5 * b.sx;
            CHECK(near(endX, startX));
            CHECK(near(endY, startY));
            CHECK(near(last->z1 + last->riseU * 0.5 * last->sx, b.z1 - b.riseU * 0.5 * b.sx));
        }
        last = &b;
    }
}

TEST_CASE("massing kerbs: a step in the floor breaks the run; no piece climbs it") {
    // Ground 0.8 m up east of x = 11, rising between x 10 and 11: steeper than kMassingKerbGrade. Each side keeps
    // x 0..9.5 low and 11..20 high; the points at 9.75, 10.25 and 10.75 fit no line.
    const Ground step = makeGround(41, 21, -5.0, -10.0, [](double x, double) { return x >= 11.0 ? 0.8 : 0.0; });
    const Kerbs k = kerbs({lane("S-1", 20)}, {}, step.view());
    CHECK(k.counts.runs == 4);
    CHECK(k.boxes.size() == 4);
    CHECK(near(k.counts.shortPieces, 3.0));
    for (const MassingBox& b : k.boxes) {
        CHECK(std::fabs(b.riseU) <= kMassingKerbGrade);
        CHECK((near(b.z1, kMassingKerbHeight) || near(b.z1, 0.8 + kMassingKerbHeight, 0.01)));
    }
}

TEST_CASE("massing quay: which boxes are decks") {
    CHECK(massingQuayDeck(flatBox("Q", 0, 0, 4, 4, 0, 1, "quay")));
    CHECK(massingQuayDeck(flatBox("P", 0, 0, 4, 4, 0, 1, "pier")));
    CHECK(massingQuayDeck(flatBox("W", 0, 0, 4, 4, 0, 1, "seawall")));
    CHECK(massingQuayDeck(flatBox("D", 0, 0, 4, 4, 0, 1, "deck")));
    CHECK_FALSE(massingQuayDeck(flatBox("S", 0, 0, 4, 4, 0, 1, "slab")));
    CHECK_FALSE(massingQuayDeck(flatBox("B", 0, 0, 4, 4, 0, 1, "breakwater")));
    MassingBox sloped = flatBox("Q", 0, 0, 4, 4, 0, 1, "quay");
    sloped.sloped = true;
    CHECK_FALSE(massingQuayDeck(sloped));
    MassingBox zone = flatBox("Q", 0, 0, 4, 4, 0, 1, "quay");
    zone.source = MassingSource::Zone;
    CHECK_FALSE(massingQuayDeck(zone));
}

TEST_CASE("massing quay: a rail, posts, bollards and a lamp along the seaward edge") {
    const Quay q = quay({quayDeck()});
    CHECK(q.counts.decks == 1);
    CHECK(q.counts.runs == 1);
    CHECK(near(q.counts.seaward, 20.0));
    CHECK(near(q.counts.blocked, 0.0));
    CHECK(near(q.counts.railed, 19.7));
    REQUIRE(countPrefix(q.boxes, "Q-1-QR") == 1);
    const MassingBox& rail = q.boxes[0];
    CHECK(rail.id == "Q-1-QR1");
    CHECK(rail.kind == "barrier");
    CHECK(rail.row == "Q-1");
    CHECK(near(rail.cx, 19.85));
    CHECK(near(rail.cy, 20.0));
    CHECK(near(rail.sx, 19.7));
    CHECK(near(rail.sy, kMassingRailBar));
    CHECK(near(rail.rot, 90.0));
    CHECK(near(rail.z1, 4.0 + kMassingRailTop));
    CHECK(near(rail.z0, 4.0 + kMassingRailTop - kMassingRailBar));
    CHECK(massingLookSurface(rail) == MassingLookSurface::GalvSteel);

    CHECK(q.counts.posts == 11);
    CHECK(countPrefix(q.boxes, "Q-1-QP") == 11);
    for (const MassingBox& b : q.boxes) {
        if (b.id.compare(0, 6, "Q-1-QP") != 0) continue;
        CHECK(near(b.cx, 19.85));
        CHECK(b.cy >= 10.15 - 1e-9);
        CHECK(b.cy <= 29.85 + 1e-9);
        CHECK(near(b.z0, 3.95));
        CHECK(b.z1 > rail.z0);
        CHECK(b.kind == "post");
        CHECK(massingLookSurface(b) == MassingLookSurface::GalvSteel);
    }

    CHECK(q.counts.bollards == 2);
    REQUIRE(countPrefix(q.boxes, "Q-1-QB") == 2);
    const MassingBox& bollard = q.boxes[q.boxes.size() - 2];
    CHECK(bollard.id == "Q-1-QB1");
    CHECK(near(bollard.cx, 19.5));
    CHECK(near(bollard.cy, 10.15 + 19.7 / 4.0));
    CHECK(near(bollard.sx, kMassingBollardSide));
    CHECK(near(bollard.z1, 4.0 + kMassingBollardHeight));

    CHECK(q.counts.lamps == 1);
    REQUIRE(q.lamps.size() == 1);
    CHECK(q.lamps[0].rule == 7);
    CHECK(q.lamps[0].street == 0);
    CHECK(near(q.lamps[0].x, 18.5));
    CHECK(near(q.lamps[0].y, 20.0));
    CHECK(near(q.lamps[0].z, 4.1));
    CHECK(near(std::cos(q.lamps[0].face * 3.14159265358979323846 / 180.0), -1.0)); // facing in, west
    CHECK_FALSE(q.lamps[0].wall);
}

TEST_CASE("massing quay: a box on the edge splits the run, a lamp's spot can be taken") {
    // A kiosk standing on the edge's rail line: two runs, too short for lamps.
    const Quay q = quay({quayDeck(), flatBox("K", 19, 20, 2, 2, 4, 7)});
    CHECK(near(q.counts.blocked, 2.0));
    CHECK(q.counts.runs == 2);
    CHECK(countPrefix(q.boxes, "Q-1-QR") == 2);
    CHECK(q.counts.posts == 12);
    CHECK(q.counts.bollards == 2);
    CHECK(q.counts.lamps == 0);
    // One no higher than the deck doesn't count.
    CHECK(quay({quayDeck(), flatBox("K", 19, 20, 2, 2, 0, 4.04)}).counts.runs == 1);

    // A box where the lamp would stand, or a foot already there.
    const Quay blocked = quay({quayDeck(), flatBox("L", 18.5, 20, 1, 1, 4, 7)});
    CHECK(blocked.counts.lampsBlocked == 1);
    CHECK(blocked.counts.lamps == 0);
    CHECK(blocked.counts.runs == 1);
    std::vector<MassingFoot> taken(1);
    taken[0].x = 18.5;
    taken[0].y = 20.0;
    taken[0].hx = taken[0].hy = 0.2;
    const Quay crowded = quay({quayDeck()}, &taken);
    CHECK(crowded.counts.lampsCrowded == 1);
    CHECK(crowded.lamps.empty());
}

TEST_CASE("massing quay: a pier at sea is railed all round, one post at each corner; its mouth breaks the quay's rail") {
    const Quay q = quay({flatBox("P-1", 30, 20, 10, 4, -3, 4, "pier")});
    CHECK(q.counts.runs == 4);
    CHECK(countPrefix(q.boxes, "P-1-QR") == 4);
    CHECK(q.counts.posts == 14);
    CHECK(q.counts.bollards == 4);
    CHECK(q.counts.lamps == 0);

    // A pier on the quay's edge: the edge is not seaward where the pier stands off it.
    const Quay mouth = quay({quayDeck(), flatBox("P-2", 25, 20, 10, 4, -3, 4, "pier")});
    REQUIRE(countPrefix(mouth.boxes, "Q-1-QR") == 2);
    for (const MassingBox& b : mouth.boxes) {
        if (b.id.compare(0, 6, "Q-1-QR") != 0) continue;
        CHECK(std::fabs(b.cy - 20.0) - 0.5 * b.sx >= 2.0 - 1e-9);
    }
}

TEST_CASE("massing frontage: the table is read with its defaults and its errors") {
    std::vector<MassingFrontRule> rules;
    std::string error;
    const std::string good = "# a comment\n"
                             "id,asset,width_m,depth_m,height_m,out_m,gap_m,turn,dz,weight,run,cap,kinds,note\n"
                             "SHOP,/Game/A.A,6.85,12,4.2,1,,90,,2,,,core_lane;street,x\n"
                             "SIGN,/Game/B.B,1,0.3,2,,6,,5.2,,3,40,open,\n";
    REQUIRE(importMassingFront(good, rules, &error));
    REQUIRE(rules.size() == 2);
    CHECK(rules[0].id == "SHOP");
    CHECK(rules[0].asset == "/Game/A.A");
    CHECK(near(rules[0].width, 6.85));
    CHECK(near(rules[0].depth, 12.0));
    CHECK(near(rules[0].height, 4.2));
    CHECK(near(rules[0].out, 1.0));
    CHECK(near(rules[0].gap, 0.3));
    CHECK(near(rules[0].turn, 90.0));
    CHECK(near(rules[0].dz, 0.0));
    CHECK(near(rules[0].weight, 2.0));
    CHECK(rules[0].run == 1);
    CHECK(rules[0].cap == 0);
    REQUIRE(rules[0].kinds.size() == 2);
    CHECK(rules[0].kinds[1] == "street");
    CHECK(near(rules[1].out, 0.3)); // depth_m
    CHECK(near(rules[1].gap, 6.0));
    CHECK(near(rules[1].dz, 5.2));
    CHECK(rules[1].run == 3);
    CHECK(rules[1].cap == 40);
    REQUIRE(rules[1].kinds.size() == 1);
    CHECK(rules[1].kinds[0] == "open");

    const std::string head = "id,asset,width_m,depth_m,height_m,out_m,run,cap,kinds\n";
    const auto fails = [&](const std::string& csv, const std::string& word) {
        std::string why;
        const bool ok = importMassingFront(csv, rules, &why);
        CHECK_FALSE(ok);
        CHECK(why.find(word) != std::string::npos);
    };
    fails("id,asset,width_m,depth_m\nA,/Game/A.A,1,1\n", "height_m");
    fails(head + "A,/Game/A.A,0,1,1,,,,\n", "width_m");
    fails(head + "A,/Game/A.A,1,1,1,0,,,\n", "out_m");
    fails(head + "A,/Game/A.A,1,1,1,,0,,\n", "run");
    fails(head + "A,/Game/A.A,1,1,1,,1.5,,\n", "run");
    fails(head + "A,/Game/A.A,1,1,1,,,-1,\n", "cap");
    fails(head + "A,/Game/A.A,1,1,1,,,,street;\n", "kinds");
    fails(head + "A,,1,1,1,,,,\n", "asset");
    fails(head + "A,/Game/A.A,1,1,1,,,,\nA,/Game/A.A,1,1,1,,,,\n", "twice");
    fails(head + "A,/Game/A.A,1,1,1\n", "fields");
}

TEST_CASE("massing frontage: a piece's foot is the part out of its face") {
    MassingProp p;
    p.face = 90.0;
    MassingFrontRule r = front("SHOP", 4.0, {});
    r.depth = 12.0;
    r.out = 2.0;
    const MassingFoot f = massingFrontFoot(p, r);
    CHECK(near(f.x, 0.0));
    CHECK(near(f.y, 1.0));
    CHECK(near(f.hx, 1.0));
    CHECK(near(f.hy, 2.0));
    CHECK(near(f.rot, 90.0));
}

TEST_CASE("massing frontage: each face takes the rules its street kind holds") {
    const std::vector<MassingFrontRule> rules = {front("SHOP", 6.0, {"street"}), front("VEND", 2.0, {"open"})};
    const Front f = frontage(rules, northStreet(), {host()});
    CHECK(f.totals.hosts == 1);
    CHECK(f.totals.faces == 4);
    CHECK(f.totals.dressed == 4);
    CHECK(f.totals.streetFaces == 1);
    CHECK(f.totals.streetDressed == 1);
    CHECK(f.counts[0].placed == 3);
    CHECK(f.counts[0].faces == 1);
    CHECK(f.counts[1].placed == 16);
    CHECK(f.counts[1].faces == 3);
    REQUIRE(f.props.size() == 19);
    // The east face (+x) first, then the north face, which fronts the street.
    const MassingProp& shop = f.props[4];
    CHECK(shop.rule == 0);
    CHECK(shop.street == 0);
    CHECK(near(shop.x, 6.7));
    CHECK(near(shop.y, 5.0));
    CHECK(near(shop.z, 0.0));
    CHECK(near(shop.face, 90.0));
    CHECK(shop.wall);
    for (const MassingProp& p : f.props) {
        if (p.rule == 0) CHECK(near(p.face, 90.0));
        else CHECK_FALSE(near(p.face, 90.0));
    }
    // Run again: the same pieces.
    const Front again = frontage(rules, northStreet(), {host()});
    REQUIRE(again.props.size() == f.props.size());
    for (std::size_t k = 0; k < f.props.size(); ++k) {
        CHECK(again.props[k].rule == f.props[k].rule);
        CHECK(near(again.props[k].x, f.props[k].x));
        CHECK(near(again.props[k].y, f.props[k].y));
    }
    // No kinds: every face.
    const Front all = frontage({front("ANY", 2.0, {})}, northStreet(), {host()});
    CHECK(all.counts[0].faces == 4);
    // Hosts not marked, or sloped, take nothing.
    std::vector<MassingFrontRule> one = {front("ANY", 2.0, {})};
    Front none;
    massingFrontage(one, northStreet(), {}, {host()}, {true}, {false}, -1.0, nullptr, none.props, none.counts,
                    none.totals);
    CHECK(none.props.empty());
    CHECK(none.totals.hosts == 0);
    MassingBox sloped = host();
    sloped.sloped = true;
    CHECK(frontage(one, northStreet(), {sloped}).props.empty());
}

TEST_CASE("massing frontage: no wall, shallow, blocked") {
    const std::vector<MassingFrontRule> shop = {front("SHOP", 6.0, {"street"})};
    // A host lower than the piece, or one standing over its foot.
    const Front low = frontage(shop, northStreet(), {host(0.0, 3.0)});
    CHECK(low.counts[0].placed == 0);
    CHECK(low.counts[0].tried > 0);
    CHECK(low.counts[0].noWall == low.counts[0].tried);
    CHECK(frontage(shop, northStreet(), {host(1.0, 8.0)}).counts[0].noWall > 0);
    CHECK(frontage(shop, northStreet(), {host(0.2, 8.0)}).counts[0].placed == 3);

    // A piece sunk deeper than the host less kMassingFrontBack.
    std::vector<MassingFrontRule> deep = shop;
    deep[0].depth = 12.0;
    const Front shallow = frontage(deep, northStreet(), {host()});
    CHECK(shallow.counts[0].placed == 0);
    CHECK(shallow.counts[0].shallow == shallow.counts[0].tried);
    deep[0].depth = 10.0;
    CHECK(frontage(deep, northStreet(), {host()}).counts[0].placed == 3);

    // A box in the strip out of the face, from x -2 to 2: the pieces step past it.
    const Front blocked = frontage(shop, northStreet(), {host(), flatBox("K", 0, 7, 4, 2, 0, 3)});
    CHECK(blocked.counts[0].placed == 2);
    CHECK(blocked.counts[0].blocked == 6);
    for (const MassingProp& p : blocked.props) CHECK(std::fabs(p.x) >= 2.0 + 3.0 - 1e-9);
    // One high overhead doesn't block.
    CHECK(frontage(shop, northStreet(), {host(), flatBox("K", 0, 7, 4, 2, 5, 6)}).counts[0].placed == 3);
}

TEST_CASE("massing frontage: in the road, steep, wet, crowded") {
    const std::vector<MassingFrontRule> shop = {front("SHOP", 6.0, {"street"})};
    // A carriageway reaching y 5..11: the fronts at y 6 stand on it.
    const Front road = frontage(shop, northStreet(8.0), {host()});
    CHECK(road.counts[0].placed == 0);
    CHECK(road.counts[0].inRoad == road.counts[0].tried);

    // Ground climbing 1/4 m per m along the face: 6 m pieces are steep, 1.5 m ones are not.
    const Ground slope = makeGround(41, 41, -20.0, -20.0, [](double x, double) { return x / 4.0; });
    const Front steep = frontage(shop, northStreet(), {host(-10.0, 12.0)}, -100.0, slope.view());
    CHECK(steep.counts[0].placed == 0);
    CHECK(steep.counts[0].steep == steep.counts[0].tried);
    const Front narrow = frontage({front("NARROW", 1.5, {"street"})}, northStreet(), {host(-10.0, 12.0)}, -100.0,
                                  slope.view());
    CHECK(narrow.counts[0].placed > 0);
    CHECK(narrow.counts[0].steep == 0);

    const Front wet = frontage(shop, northStreet(), {host()}, 0.5);
    CHECK(wet.counts[0].placed == 0);
    CHECK(wet.counts[0].wet == wet.counts[0].tried);

    // A foot already taken in front of the face's middle.
    std::vector<MassingFoot> taken(1);
    taken[0].x = 0.0;
    taken[0].y = 5.5;
    taken[0].hx = taken[0].hy = 0.5;
    const Front crowded = frontage(shop, northStreet(), {host()}, -1.0, {}, &taken);
    CHECK(crowded.counts[0].crowded > 0);
    CHECK(crowded.counts[0].placed >= 1);
    for (const MassingProp& p : crowded.props)
        CHECK_FALSE(massingFeetOverlap(massingFrontFoot(p, shop[0]), taken[0], kMassingPropGap));
}

TEST_CASE("massing frontage: overhead pieces are a layer of their own") {
    MassingFrontRule sign = front("SIGN", 1.0, {"street"});
    sign.depth = sign.out = 0.3;
    sign.height = 1.5;
    sign.dz = 4.0;
    sign.gap = 3.0;
    // On a street whose carriageway reaches the face, under a box standing low in front: the sign still goes up.
    const Front up = frontage({sign}, northStreet(8.0), {host(), flatBox("K", 0, 7, 4, 2, 0, 3)});
    CHECK(up.counts[0].placed == 5);
    CHECK(up.counts[0].inRoad == 0);
    CHECK(up.totals.dressed == 0);
    for (const MassingProp& p : up.props) CHECK(near(p.z, 4.0));
    // Ground pieces and overhead ones don't crowd each other.
    const Front both = frontage({front("SHOP", 6.0, {"street"}), sign}, northStreet(), {host()});
    CHECK(both.counts[0].placed == 3);
    CHECK(both.counts[1].placed == 5);
    CHECK(both.totals.dressed == 1);
}

TEST_CASE("massing frontage: runs keep a picked rule going; a cap thins evenly") {
    // A long host: picked arcades come three in a row.
    MassingFrontRule arc = front("ARC", 4.0, {"street"});
    arc.run = 3;
    const MassingFrontRule bin = front("BIN", 1.0, {"street"});
    const std::vector<MassingStreet> streets = {street("S-1", "street", 10.0, 2.0, {{-60, 12, 0}, {60, 12, 0}})};
    const Front f = frontage({arc, bin}, streets, {flatBox("H-2", 0, 0, 100, 10, 0, 8)});
    CHECK(f.counts[0].placed > 0);
    CHECK(f.counts[1].placed > 0);
    std::vector<std::size_t> streaks;
    std::size_t streak = 0;
    for (const MassingProp& p : f.props) {
        if (p.rule == 0) {
            ++streak;
            continue;
        }
        if (streak > 0) streaks.push_back(streak);
        streak = 0;
    }
    // Only a streak the face's end cuts short (the last, with bins after it at most) may hold fewer.
    REQUIRE(streaks.size() >= 2);
    streaks.pop_back();
    for (const std::size_t n : streaks) CHECK(n % 3 == 0);

    // A cap: no more than it, and the same pieces each time.
    MassingFrontRule vend = front("VEND", 2.0, {"open"});
    vend.cap = 5;
    const Front capped = frontage({vend}, northStreet(), {host()});
    CHECK(capped.counts[0].placed <= 5);
    CHECK(placedBy(capped, 0) == capped.counts[0].placed);
    CHECK(capped.counts[0].placed > 0);
    const Front again = frontage({vend}, northStreet(), {host()});
    REQUIRE(again.props.size() == capped.props.size());
    for (std::size_t k = 0; k < capped.props.size(); ++k) CHECK(near(again.props[k].x, capped.props[k].x));
}

TEST_CASE("massing frontage: ground pieces stand on paving pads cut short of the carriageway") {
    const std::vector<MassingFrontRule> shop = {front("SHOP", 6.0, {"street"})};
    const std::vector<bool> include(1, true), hosts(1, true);
    const auto run = [&](double streetY, std::vector<MassingBox>& pads, std::vector<std::uint8_t>& fronting) {
        Front f;
        massingFrontage(shop, northStreet(streetY), {}, {host()}, include, hosts, -1.0, nullptr, f.props, f.counts,
                        f.totals, &pads, &fronting);
        return f;
    };
    std::vector<MassingBox> pads;
    std::vector<std::uint8_t> fronting;
    // A carriageway reaching y 9..15: the pads run the whole reach, 1 + 1.2 m out of the face at y 5.
    const Front far = run(12.0, pads, fronting);
    REQUIRE(far.props.size() > 0);
    REQUIRE(pads.size() == far.props.size());
    for (std::size_t k = 0; k < pads.size(); ++k) {
        const MassingBox& p = pads[k];
        CHECK(p.id == "H-1-FP" + std::to_string(k + 1));
        CHECK(p.row == "H-1");
        CHECK(p.kind == "landing");
        CHECK(p.source == MassingSource::Invented);
        CHECK_FALSE(p.cover);
        CHECK(massingLookSurface(p) == MassingLookSurface::PavingSlab);
        CHECK(near(p.sx, 1.0 + kMassingFrontPadReach));
        CHECK(near(p.sy, 6.0 + 0.3));
        CHECK(near(p.rot, 90.0));
        CHECK(near(p.cx, far.props[k].x));
        CHECK(near(p.cy, 5.0 + 0.5 * p.sx));
        CHECK(near(p.z1, kMassingFrontPadTop));
        CHECK(near(p.z0, -kMassingFrontPadSink));
        CHECK(near(p.riseU, 0.0));
        CHECK(near(p.riseW, 0.0));
    }
    // Only the +w face (index 1) fronts the street.
    REQUIRE(fronting.size() == 4);
    CHECK(fronting[0] == 0);
    CHECK(fronting[1] == 1);
    CHECK(fronting[2] == 0);
    CHECK(fronting[3] == 0);
    // A carriageway reaching y 7..13: the pads are cut to 1.95 m, their fronts at y 6.95.
    const Front close = run(10.0, pads, fronting);
    REQUIRE(close.props.size() > 0);
    REQUIRE(pads.size() == close.props.size());
    for (const MassingBox& p : pads) CHECK(near(p.sx, 1.95));
    // Overhead pieces take no pad.
    MassingFrontRule sign = front("SIGN", 2.0, {"street"});
    sign.dz = kMassingPropBody;
    Front over;
    massingFrontage({sign}, northStreet(), {}, {host()}, include, hosts, -1.0, nullptr, over.props, over.counts,
                    over.totals, &pads, nullptr);
    CHECK(over.props.size() > 0);
    CHECK(pads.empty());
}

TEST_CASE("massing frontage: paving pads lie flush on sloping ground; one over a step is left out") {
    const std::vector<MassingFrontRule> shop = {front("SHOP", 6.0, {"street"})};
    const std::vector<bool> include(1, true), hosts(1, true);
    // Ground rising 0.06 m per m east and 0.04 m per m north; the host sunk 2 m so every place keeps its wall.
    const auto h = [](double x, double y) { return 0.06 * x + 0.04 * y; };
    const Ground tilt = makeGround(81, 41, -40.0, -10.0, h);
    std::vector<MassingBox> pads;
    Front f;
    massingFrontage(shop, northStreet(), tilt.view(), {host(-2.0)}, include, hosts, -1.0, nullptr, f.props, f.counts,
                    f.totals, &pads, nullptr);
    REQUIRE(f.props.size() > 0);
    REQUIRE(pads.size() == f.props.size());
    CHECK(f.totals.padsUneven == 0);
    double worst = 0.0, levelWorst = 0.0;
    for (const MassingBox& p : pads) {
        // The pad's u runs north (out of the face), its w west (along it).
        CHECK(near(p.riseU, 0.04, 0.005));
        CHECK(near(p.riseW, -0.06, 0.005));
        std::array<MassingVec, 8> c;
        massingCorners(p, 0.0, c);
        for (std::size_t k = 4; k < 8; ++k) {
            const double ground = h(c[k].x, c[k].y) + kMassingFrontPadTop;
            worst = std::max(worst, std::fabs(c[k].z - ground));
            levelWorst = std::max(levelWorst, std::fabs(p.z1 - ground));
        }
    }
    // Every top corner within 2 cm of the ground under it (a level pad would be over 0.15 m off at its ends).
    CHECK(worst <= 0.02);
    CHECK(levelWorst >= 0.15);

    // A 0.5 m step through the pads (north of y = 7, between their back and front): left out, counted.
    const Ground step = makeGround(81, 41, -40.0, -10.0, [](double, double y) { return y >= 7.0 ? 0.5 : 0.0; });
    Front s;
    massingFrontage(shop, northStreet(), step.view(), {host(-2.0)}, include, hosts, -1.0, nullptr, s.props, s.counts,
                    s.totals, &pads, nullptr);
    REQUIRE(s.props.size() > 0);
    CHECK(pads.empty());
    CHECK(s.totals.padsUneven == s.props.size());
}

namespace {
struct Roofs {
    std::vector<MassingBox> boxes;
    MassingRoofCounts counts;
};
// The first box is the host; every box is included.
Roofs roofs(const std::vector<MassingBox>& boxes, const MassingGround& ground = {}) {
    Roofs r;
    const std::vector<bool> include(boxes.size(), true);
    std::vector<bool> hosts(boxes.size(), false);
    if (!hosts.empty()) hosts[0] = true;
    massingRoofs(ground, boxes, include, hosts, r.boxes, r.counts);
    return r;
}
std::size_t ofKind(const Roofs& r, const std::string& kind) {
    std::size_t n = 0;
    for (const MassingBox& b : r.boxes) n += b.kind == kind ? 1 : 0;
    return n;
}
} // namespace

TEST_CASE("massing roofs: rails on the edges that drop away, clutter on the top") {
    const Roofs r = roofs({host()});
    CHECK(r.counts.roofs == 1);
    CHECK(r.counts.bare == 0);
    // Four edges, each from 0.15 m to 0.15 m short of its end: 2 x 9.7 + 2 x 19.7 m.
    CHECK(near(r.counts.railed, 58.8, 1e-6));
    CHECK(ofKind(r, "barrier") == 4);
    CHECK(r.counts.posts == 2 * 5 + 2 * 8 - 4); // the corners' end posts coincide
    CHECK(ofKind(r, "post") == r.counts.posts);
    CHECK(r.counts.tanks >= 1);
    CHECK(r.counts.units == 6);
    CHECK(ofKind(r, "tank") == r.counts.tanks);
    CHECK(ofKind(r, "unit") == r.counts.units);
    CHECK(ofKind(r, "aerial") == 2 * r.counts.aerials);
    std::vector<MassingFoot> clutter;
    for (const MassingBox& b : r.boxes) {
        CHECK(b.row == "H-1");
        CHECK(b.source == MassingSource::Invented);
        CHECK(massingLookSurface(b) == MassingLookSurface::GalvSteel);
        CHECK(b.z1 > 8.0);
        CHECK(b.z0 >= 8.0 - 0.06);
        if (b.kind == "tank" || b.kind == "unit") {
            const MassingFoot f = massingBoxFoot(b);
            CHECK(std::fabs(f.x) + f.hx <= 10.0 - kMassingRoofEdgeIn + 1e-6);
            CHECK(std::fabs(f.y) + f.hy <= 5.0 - kMassingRoofEdgeIn + 1e-6);
            for (const MassingFoot& o : clutter) CHECK_FALSE(massingFeetOverlap(f, o, kMassingRoofClutterGap - 1e-6));
            clutter.push_back(f);
        }
    }
    // Stable.
    const Roofs again = roofs({host()});
    REQUIRE(again.boxes.size() == r.boxes.size());
    for (std::size_t k = 0; k < r.boxes.size(); ++k) {
        CHECK(again.boxes[k].id == r.boxes[k].id);
        CHECK(near(again.boxes[k].cx, r.boxes[k].cx));
    }
}

TEST_CASE("massing roofs: a neighbour or the ground at the top shares the edge; boxes on the top keep clutter off") {
    // A neighbour as high along the +u edge: no rail there.
    const Roofs n = roofs({host(), flatBox("N-1", 15, 0, 10, 10, 0, 8)});
    CHECK(near(n.counts.railed, 58.8 - 9.7, 1e-6));
    // A lower neighbour leaves the rail.
    const Roofs low = roofs({host(), flatBox("N-1", 15, 0, 10, 10, 0, 6)});
    CHECK(near(low.counts.railed, 58.8, 1e-6));
    // The ground at the top beyond the +w edge.
    const Ground hill = makeGround(41, 41, -20.0, -20.0, [](double, double y) { return y >= 5.0 ? 8.0 : 0.0; });
    const Roofs g = roofs({host()}, hill.view());
    CHECK(near(g.counts.railed, 58.8 - 19.7, 1e-6));
    // A box standing on the top keeps the clutter off its footprint.
    const MassingBox tower = flatBox("T-1", 0, 0, 6, 6, 8, 14);
    const Roofs t = roofs({host(), tower});
    for (const MassingBox& b : t.boxes)
        if (b.kind == "tank" || b.kind == "unit" || b.kind == "aerial")
            CHECK_FALSE(massingFeetOverlap(massingBoxFoot(b), massingBoxFoot(tower), 0.0));
}

TEST_CASE("massing roofs: small, low or sloped tops are left bare") {
    CHECK(roofs({host(0.0, 2.0)}).counts.bare == 1);
    CHECK(roofs({host(0.0, 2.0)}).boxes.empty());
    CHECK(roofs({flatBox("S-1", 0, 0, 2.5, 10, 0, 8)}).boxes.empty());
    CHECK(roofs({flatBox("S-1", 0, 0, 3.5, 4, 0, 8)}).boxes.empty());
    MassingBox sloped = host();
    sloped.sloped = true;
    sloped.slopeFrom = 6.0;
    sloped.slopeTo = 8.0;
    CHECK(roofs({sloped}).boxes.empty());
}

TEST_CASE("massing roofs: a court at the ground takes rails where it drops away, racks and standards, no roof clutter") {
    // A 40 x 20 m court, its top at the ground; the ground falls 10 m beyond its +u and +w edges.
    const auto h = [](double x, double y) { return (x > 20.5 || y > 10.5) ? 0.0 : 10.0; };
    const Ground step = makeGround(61, 41, -30.0, -20.0, h);
    const MassingBox court = flatBox("C-1", 0, 0, 40, 20, 4, 10);
    // As a plain roof host it stands under kMassingRoofHigh over the ground: bare.
    const Roofs plain = roofs({court}, step.view());
    CHECK(plain.counts.bare == 1);
    CHECK(plain.boxes.empty());

    const auto courtRoofs = [&](const std::vector<MassingBox>& boxes) {
        Roofs r;
        const std::vector<bool> include(boxes.size(), true);
        std::vector<bool> hosts(boxes.size(), false), courts(boxes.size(), false);
        hosts[0] = courts[0] = true;
        massingRoofs(step.view(), boxes, include, hosts, r.boxes, r.counts, &courts);
        return r;
    };
    const Roofs c = courtRoofs({court});
    CHECK(c.counts.bare == 0);
    CHECK(c.counts.roofs == 0);
    CHECK(c.counts.courts == 1);
    // Rails on the two drop edges only, each 0.15 m short of its ends: 19.7 + 39.7 m.
    CHECK(near(c.counts.railed, 19.7 + 39.7, 1e-6));
    CHECK(c.counts.tanks == 0);
    CHECK(c.counts.units == 0);
    CHECK(c.counts.aerials == 0);
    CHECK(ofKind(c, "tank") == 0);
    CHECK(ofKind(c, "unit") == 0);
    CHECK(ofKind(c, "aerial") == 0);
    // 800 m^2: five racks and one standard.
    CHECK(c.counts.racks == 5);
    CHECK(c.counts.standards == 1);
    CHECK(ofKind(c, "post") == c.counts.posts + 2 * c.counts.racks);
    CHECK(ofKind(c, "barrier") == 2 + c.counts.racks + c.counts.standards);
    CHECK(ofKind(c, "mast") == c.counts.standards);
    for (const MassingBox& b : c.boxes) {
        CHECK(massingLookSurface(b) == MassingLookSurface::GalvSteel);
        CHECK(b.z0 >= 10.0 - 0.06);
        if (b.id.find("-CB") != std::string::npos) CHECK(near(b.z1, 10.0 + kMassingCourtRackHigh));
        if (b.id.find("-CS") != std::string::npos) CHECK(near(b.z1, 10.0 + kMassingCourtStandardHigh));
        if (b.id.find("-CP") != std::string::npos || b.id.find("-CB") != std::string::npos) {
            CHECK(std::fabs(b.cx) <= 20.0 - kMassingRoofEdgeIn + 1e-6);
            CHECK(std::fabs(b.cy) <= 10.0 - kMassingRoofEdgeIn + 1e-6);
        }
    }
    // A retaining wall flush with the court along its +u edge shares that edge: no rail doubles it.
    const Roofs w = courtRoofs({court, flatBox("W-1", 20, 0, 1, 20, 0, 10)});
    CHECK(near(w.counts.railed, 39.7, 1e-6));
    CHECK(w.counts.racks == 5);
}

TEST_CASE("massing stand-ins: a crane box is built as an open frame with its boom out past its +u side") {
    MassingBox crane = flatBox("BLD-057_2", 950, 420, 12, 10, -1, 44);
    crane.kind = "crane";
    std::vector<MassingBox> out = {flatBox("KEEP", 0, 0, 1, 1, 0, 1)};
    REQUIRE(massingStandIn(crane, out));
    REQUIRE(out.size() == 1 + 15);
    CHECK(out[0].id == "KEEP");
    const double toRad = 3.14159265358979323846 / 180.0;
    double top = -1e9, reach = -1e9;
    for (std::size_t k = 1; k < out.size(); ++k) {
        const MassingBox& p = out[k];
        CHECK(p.id == "BLD-057_2-S" + std::to_string(k));
        CHECK(p.kind == "crane");
        CHECK(p.row == "BLD-057_2");
        CHECK(p.source == MassingSource::Invented);
        CHECK_FALSE(p.cover);
        CHECK(massingLookSurface(p) == MassingLookSurface::GalvSteel);
        CHECK(p.z0 >= -1.0 - 1e-9);
        top = std::max(top, p.z1);
        reach = std::max(reach, p.cx + 0.5 * p.sx);
        // Every piece but the boom and its ties stays inside the footprint.
        if (p.sx < 13.0 && p.riseU == 0.0) {
            CHECK(std::fabs(p.cx - 950.0) + 0.5 * p.sx <= 6.0 + 1e-9);
            CHECK(std::fabs(p.cy - 420.0) + 0.5 * p.sy <= 5.0 + 1e-9);
        }
        // A tie's line runs from the cross-head's top (u 0) to the boom's top at its end.
        if (p.riseU != 0.0) {
            const double mid = 0.5 * (p.z0 + p.z1);
            const double atHead = mid + p.riseU * (950.0 - p.cx);
            CHECK(near(atHead, 44.0));
            const double far = p.cx + (p.cx > 950.0 ? 0.5 : -0.5) * p.sx;
            CHECK(near(mid + p.riseU * (far - p.cx), -1.0 + 0.62 * 45.0));
        }
    }
    CHECK(near(top, 44.0));
    CHECK(near(reach, 950.0 + 6.0 + 30.0)); // the boom 2.5 sx out past the +u side
    // The middle under the portal is open: no piece holds (950, 420) below the portal's beams.
    for (std::size_t k = 1; k < out.size(); ++k) {
        const MassingBox& p = out[k];
        const bool holds = std::fabs(p.cx - 950.0) <= 0.5 * p.sx && std::fabs(p.cy - 420.0) <= 0.5 * p.sy;
        if (holds) CHECK(p.z0 >= -1.0 + 0.4 * 45.0 - kMassingCraneLeg - 1e-9);
    }
    // Turned a quarter, the boom runs north.
    crane.rot = 90.0;
    std::vector<MassingBox> turned;
    REQUIRE(massingStandIn(crane, turned));
    double north = -1e9;
    for (const MassingBox& p : turned) {
        const double a = p.rot * toRad;
        north = std::max(north, p.cy + std::fabs(std::sin(a)) * 0.5 * p.sx + std::fabs(std::cos(a)) * 0.5 * p.sy);
    }
    CHECK(near(north, 420.0 + 36.0));
}

TEST_CASE("massing stand-ins: a boat box is a hull, narrower below, with a cabin on its deck") {
    MassingBox boat = flatBox("BLD-060_2", 950, 450, 3.5, 11, 4, 6);
    boat.kind = "boat";
    std::vector<MassingBox> out;
    REQUIRE(massingStandIn(boat, out));
    REQUIRE(out.size() == 3);
    // The long side is w: the lower hull 0.85 of 11 m by 0.7 of 3.5 m, to 0.45 of the height.
    CHECK(near(out[0].sy, 0.85 * 11.0));
    CHECK(near(out[0].sx, 0.7 * 3.5));
    CHECK(near(out[0].z0, 4.0));
    CHECK(near(out[0].z1, 4.9));
    CHECK(near(out[1].sx, 3.5));
    CHECK(near(out[1].sy, 11.0));
    CHECK(near(out[1].z1, 6.0));
    // The cabin on the deck toward the -w end, inside the hull.
    CHECK(near(out[2].z0, 6.0));
    CHECK(near(out[2].z1, 6.0 + 2.0 + 1.1));
    CHECK(out[2].cy < 450.0);
    CHECK(out[2].cy - 0.5 * out[2].sy >= 450.0 - 5.5 - 1e-9);
    for (const MassingBox& p : out) CHECK(massingLookSurface(p) == MassingLookSurface::GalvSteel);
    // Any other kind, or a sloped box: nothing.
    MassingBox other = boat;
    other.kind = "deck";
    CHECK_FALSE(massingStandIn(other, out));
    MassingBox sloped = boat;
    sloped.sloped = true;
    CHECK_FALSE(massingStandIn(sloped, out));
    CHECK(out.size() == 3);
}

TEST_CASE("massing frontage: a rule's scale reads from its column") {
    std::vector<MassingFrontRule> rules;
    std::string error;
    const std::string good = "id,asset,width_m,depth_m,height_m,scale\n"
                             "A,/Game/A.A,2,1,5,0.6\n"
                             "B,/Game/B.B,2,1,5,\n";
    REQUIRE(importMassingFront(good, rules, &error));
    CHECK(near(rules[0].scale, 0.6));
    CHECK(near(rules[1].scale, 1.0));
    const std::string zero = "id,asset,width_m,depth_m,height_m,scale\n"
                             "A,/Game/A.A,2,1,5,0\n";
    CHECK_FALSE(importMassingFront(zero, rules, &error));
}

TEST_CASE("massing windows: a box's seed, street faces and street floors") {
    const Ground slope = makeGround(41, 41, 0.0, 0.0, [](double x, double) { return 0.25 * x; });
    const MassingBox box = flatBox("W-1", 20, 20, 10, 8, 0, 12);
    const std::uint8_t fronting[4] = {0, 1, 0, 1};
    const MassingWindowData d = massingWindowData(box, slope.view(), fronting);
    CHECK(d.streetBits == 0b1010);
    CHECK(near(d.base[0], 0.25 * 25.5));  // +u: the probe 0.5 m east of the face
    CHECK(near(d.base[1], 5.0));
    CHECK(near(d.base[2], 0.25 * 14.5));
    CHECK(near(d.base[3], 5.0));
    CHECK(d.seed <= kWindowSeedMask);
    CHECK(d.seed == massingWindowData(box, slope.view(), nullptr).seed);
    CHECK(d.seed != massingWindowData(flatBox("W-2", 20, 20, 10, 8, 0, 12), slope.view(), nullptr).seed);
    CHECK(massingWindowData(box, slope.view(), nullptr).streetBits == 0);
    // Standing on another box: the ground lies under the bottom. Sunk deep: at most the box's height.
    const MassingWindowData high = massingWindowData(flatBox("W-3", 20, 20, 10, 8, 20, 30), slope.view(), nullptr);
    CHECK(near(high.base[0], 0.0));
    const MassingWindowData sunk = massingWindowData(flatBox("W-4", 20, 20, 10, 8, -20, -15), slope.view(), nullptr);
    CHECK(near(sunk.base[2], 5.0));
}

TEST_CASE("massing shop lights: along each street face, out of it, over its street floor") {
    MassingBox box = flatBox("S-1", 10, 20, 6, 14, 5, 15);
    MassingWindowData data;
    data.streetBits = 0b0101;  // +u and -u: 14 m long, so two lights each at a spacing of 8
    data.base[0] = 1.0;
    data.base[2] = 0.0;
    std::vector<MassingShopLight> lights;
    REQUIRE(massingShopLights(box, data, kMassingShopLightSpacing, lights) == 4);
    REQUIRE(lights.size() == 4);
    for (int k = 0; k < 2; ++k) {
        const MassingShopLight& east = lights[static_cast<std::size_t>(k)];
        CHECK(east.faceIndex == 0);
        CHECK(near(east.x, 13.0 + kMassingShopLightOut));
        CHECK(near(east.z, 5.0 + 1.0 + kMassingShopLightUp));
        CHECK(near(east.face, 0.0));
        const MassingShopLight& west = lights[static_cast<std::size_t>(k + 2)];
        CHECK(west.faceIndex == 2);
        CHECK(near(west.x, 7.0 - kMassingShopLightOut));
        CHECK(near(west.z, 5.0 + kMassingShopLightUp));
        CHECK(near(std::fabs(west.face), 180.0));
    }
    // A quarter and three quarters along: y 16.5 and 23.5, either way round.
    CHECK(near(std::min(lights[0].y, lights[1].y), 16.5));
    CHECK(near(std::max(lights[0].y, lights[1].y), 23.5));
    CHECK(near(std::min(lights[2].y, lights[3].y), 16.5));
    // Appends; no spacing gives one light per face, at its middle.
    REQUIRE(massingShopLights(box, data, 0.0, lights) == 2);
    CHECK(lights.size() == 6);
    CHECK(near(lights[4].y, 20.0));
    // Turned a quarter: +u faces north.
    box.rot = 90.0;
    lights.clear();
    data.streetBits = 0b0001;
    REQUIRE(massingShopLights(box, data, 0.0, lights) == 1);
    CHECK(near(lights[0].x, 10.0));
    CHECK(near(lights[0].y, 23.0 + kMassingShopLightOut));
    CHECK(near(lights[0].face, 90.0));
    // No street face, a storey too low for the shop band, a sloped box: none.
    lights.clear();
    data.streetBits = 0;
    CHECK(massingShopLights(box, data, 8.0, lights) == 0);
    data.streetBits = 0b1111;
    data.base[0] = data.base[1] = data.base[2] = data.base[3] = 9.0;
    CHECK(massingShopLights(box, data, 8.0, lights) == 0);
    data.base[0] = data.base[1] = data.base[2] = data.base[3] = 0.0;
    box.sloped = true;
    CHECK(massingShopLights(box, data, 8.0, lights) == 0);
    CHECK(lights.empty());
}

namespace {
Roofs styledRoofs(const std::vector<MassingBox>& boxes, const MassingRoofStyle* style) {
    Roofs r;
    const std::vector<bool> include(boxes.size(), true);
    std::vector<bool> hosts(boxes.size(), false);
    if (!hosts.empty()) hosts[0] = true;
    massingRoofs({}, boxes, include, hosts, r.boxes, r.counts, nullptr, style);
    return r;
}
} // namespace

TEST_CASE("massing roofs: the layouts off leave the r4 clutter as it was") {
    const MassingRoofStyle off{};
    for (const char* id : {"H-1", "BLD-030_1", "BLD-051_2"}) {
        MassingBox b = host();
        b.id = id;
        const Roofs a = styledRoofs({b}, nullptr), c = styledRoofs({b}, &off);
        REQUIRE(a.boxes.size() == c.boxes.size());
        for (std::size_t k = 0; k < a.boxes.size(); ++k) {
            CHECK(a.boxes[k].id == c.boxes[k].id);
            CHECK(a.boxes[k].cx == c.boxes[k].cx);
            CHECK(a.boxes[k].cy == c.boxes[k].cy);
            CHECK(a.boxes[k].z1 == c.boxes[k].z1);
        }
        CHECK(c.counts.huts == 0);
    }
}

TEST_CASE("massing roofs: layouts by the hash give distinct roofs, some empty, all inside the inset") {
    const MassingRoofStyle on{true};
    std::vector<std::vector<std::string>> kinds;
    std::array<std::size_t, kMassingRoofLayouts> seen{};
    std::array<std::size_t, kMassingRoofFinishes> finishes{};
    std::size_t empty = 0, clutterPieces = 0, huts = 0;
    const int hosts = 48;
    for (int n = 0; n < hosts; ++n) {
        MassingBox b = flatBox("BLD-" + std::to_string(100 + n) + "_1", 0, 0, 14.0 + (n % 3) * 3.0, 9.0 + (n % 2) * 3.0,
                               0.0, 12.0);
        b.rot = 15.0 * (n % 4);
        const Roofs r = styledRoofs({b}, &on);
        CHECK(r.counts.roofs == 1);
        ++seen[static_cast<std::size_t>(massingRoofLayout(b.id))];
        ++finishes[static_cast<std::size_t>(massingRoofFinish(b.id))];
        std::vector<std::string> mine;
        std::vector<MassingFoot> feet;
        const double c = std::cos(b.rot * 3.14159265358979 / 180.0), s = std::sin(b.rot * 3.14159265358979 / 180.0);
        for (const MassingBox& p : r.boxes) {
            if (p.id.find("-RR") != std::string::npos || p.id.find("-RP") != std::string::npos) continue; // rails
            mine.push_back(p.kind);
            ++clutterPieces;
            CHECK(massingLookSurface(p) ==
                  (p.kind == "hut" ? massingLookSurface(b) : MassingLookSurface::GalvSteel));
            // Inside the inset, in the host's frame.
            const MassingFoot f = massingBoxFoot(p);
            const double u = (f.x - b.cx) * c + (f.y - b.cy) * s, w = -(f.x - b.cx) * s + (f.y - b.cy) * c;
            const bool square = near(p.rot, b.rot, 1e-9);
            const double ru = square ? f.hx : 0.0, rw = square ? f.hy : 0.0; // turned pieces: their middle
            CHECK(std::fabs(u) + ru <= 0.5 * b.sx - kMassingRoofEdgeIn + 1e-6);
            CHECK(std::fabs(w) + rw <= 0.5 * b.sy - kMassingRoofEdgeIn + 1e-6);
            // Pieces standing on the roof (not on a hut or stand) keep apart.
            if (p.z0 > b.z1) continue;
            if (p.kind == "post" || p.kind == "aerial") continue;
            for (const MassingFoot& o : feet) CHECK_FALSE(massingFeetOverlap(f, o, kMassingRoofClutterGap - 1e-6));
            feet.push_back(f);
        }
        huts += r.counts.huts;
        std::sort(mine.begin(), mine.end());
        if (mine.empty()) ++empty;
        if (std::find(kinds.begin(), kinds.end(), mine) == kinds.end()) kinds.push_back(mine);
    }
    for (const std::size_t k : seen) CHECK(k > 0);
    for (const std::size_t k : finishes) CHECK(k > 0);
    CHECK(empty > 0);
    CHECK(huts > 0);
    CHECK(kinds.size() >= 6);
    CHECK(static_cast<double>(clutterPieces) / hosts <= 8.0);
}

TEST_CASE("massing roofs: a hut keeps to a corner and carries a tank") {
    const MassingRoofStyle on{true};
    for (int n = 0; n < 200; ++n) {
        MassingBox b = flatBox("H-" + std::to_string(n), 0, 0, 16, 12, 0, 10);
        if (massingRoofLayout(b.id) != MassingRoofLayout::StairHead) continue;
        const Roofs r = styledRoofs({b}, &on);
        REQUIRE(r.counts.huts == 1);
        const MassingBox* hut = nullptr;
        for (const MassingBox& p : r.boxes)
            if (p.kind == "hut") hut = &p;
        REQUIRE(hut != nullptr);
        CHECK(near(hut->z1 - b.z1, kMassingRoofHutHeight, 1e-9));
        // A corner: both its sides on the inset line.
        CHECK(near(std::fabs(hut->cx) + 0.5 * hut->sx, 0.5 * b.sx - kMassingRoofEdgeIn, 1e-6));
        CHECK(near(std::fabs(hut->cy) + 0.5 * hut->sy, 0.5 * b.sy - kMassingRoofEdgeIn, 1e-6));
        bool tankOn = false;
        for (const MassingBox& p : r.boxes)
            tankOn = tankOn || (p.kind == "tank" && near(p.cx, hut->cx, 1e-9) && near(p.z0, hut->z1 - 0.05, 1e-9));
        CHECK(tankOn);
        return;
    }
    FAIL("no StairHead host among 200 ids");
}

TEST_CASE("massingFacadeData: type by height and footprint, all types and tints seen, roof finish as the roof's") {
    std::array<int, kWindowTypes> types{};
    int pastelOnTextured = 0;
    for (int i = 0; i < 200; ++i) {
        const std::string id = "BLD-" + std::to_string(i);
        for (const double h : {8.0, 15.0, 26.0}) {
            MassingBox box = flatBox(id.c_str(), 0, 0, 8, 14, 0, h);
            MassingWindowData d;
            d.seed = static_cast<std::uint32_t>(i * 7919);
            d.streetBits = (i & 1) ? 1u : 0u;
            const MassingFacadeData f = massingFacadeData(box, d);
            ++types[static_cast<std::size_t>(f.type)];
            CHECK(f.type != WindowType::Legacy);
            if (h >= kMassingFacadeTall) CHECK((f.type == WindowType::Office || f.type == WindowType::Walkway));
            if (h < kMassingFacadeMid) CHECK((f.type == WindowType::Apartment || f.type == WindowType::Shophouse));
            CHECK(f.roofFinish == static_cast<std::uint32_t>(massingRoofFinish(box.id)));
            CHECK(f.wallTint < kWindowWallTintCount);
            if (massingLookSurface(box) == MassingLookSurface::BuildingA && windowWallTint(f.wallTint).pastel)
                ++pastelOnTextured;
        }
    }
    for (std::uint32_t t = 1; t < kWindowTypes; ++t) CHECK(types[t] > 0);
    CHECK(pastelOnTextured == 0);
}
