// ftcore_roadgraph: writes the road graph CSV (ftcore/RoadGraph.h) for a massing's streets and built roads, and prints
// the drivable loop, the built roads' counts, where each road's ends meet the net and the deck and pier rows each road
// clears. An offline build tool for the private tables; C stdio only.
//
//   ftcore_roadgraph --streets <csv> --roads <csv> --area <x0,y0,x1,y1> [--boxes <csv>] [--min-width <m>] [--out <csv>]
//   ftcore_roadgraph --net <csv> [--r16 <file> --samples <n> --origin <x,y> [--spacing <m>]] [--boxes <csv>] [--out <csv>] [--list-all 1]
//
// The second form builds a road network table (importRoadNet, roadNetBuild) on the ground (an r16 as ftcore_layout's;
// none: flat at 0) and prints its report and notes, its loop, its counts, the box rows it clears (decks and piers) and
// the other box rows standing in its band (buildings in the road's way); --out writes its graph (roadGraphFromNet).
//
// Streets at least --min-width (10) wide with a point inside the area are taken. Speeds: 60 km/h on an expressway,
// 50 on a coast road, link road or causeway, 40 elsewhere. The last line on stdout is ROADGRAPH OK; an error prints
// "ftcore_roadgraph: error: ..." and exits 1.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/MassingProps.h"
#include "ftcore/RoadGraph.h"
#include "ftcore/Roads.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

int die(const std::string& what) {
    std::fprintf(stderr, "ftcore_roadgraph: error: %s\n", what.c_str());
    return 1;
}

bool writeGraph(const std::string& outPath, const RoadGraph& g) {
    std::FILE* f = std::fopen(outPath.c_str(), "wb");
    if (!f) return false;
    const std::string s = std::string("# PRIVATE. Never committed. ftcore_roadgraph output.\n") + roadGraphWrite(g);
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    return true;
}

int runNet(const std::string& netPath, const std::string& r16Path, double samples, const double origin[2], double spacing,
           const std::string& boxesPath, const std::string& outPath, bool listAll) {
    std::string text, err;
    std::vector<RoadNetStreet> streets;
    if (!readFile(netPath, text)) return die("can't read " + netPath);
    if (!importRoadNet(text, streets, &err)) return die(netPath + ": " + err);
    std::vector<std::uint16_t> grid;
    MassingGround ground;
    if (!r16Path.empty()) {
        text.clear();
        if (!readFile(r16Path, text)) return die("can't read " + r16Path);
        const std::size_t n = static_cast<std::size_t>(samples);
        if (n < 2 || text.size() != n * n * 2) return die("the r16 is not samples x samples u16");
        grid.resize(n * n);
        for (std::size_t k = 0; k < n * n; ++k)
            grid[k] = static_cast<std::uint16_t>(static_cast<std::uint8_t>(text[2 * k]) |
                                                 (static_cast<std::uint8_t>(text[2 * k + 1]) << 8));
        ground = MassingGround{grid.data(), n, n, spacing, origin[0], origin[1]};
    }
    RoadBuild b;
    RoadNetReport rep;
    roadNetBuild(streets, ground, b, &rep);
    std::printf("net: streets %zu, nodes %zu, edges %zu, junctions %zu, bends %zu, ends %zu, pieces %zu, piers %zu\n",
                streets.size(), rep.nodes, rep.edges, rep.junctions, rep.bends, rep.ends, rep.pieces, rep.piers);
    std::printf("net: steep %zu (max grade %.1f%%), pokes %zu, floats %zu, tight %zu, max step %.3f m\n", rep.steep,
                rep.maxGrade * 100.0, rep.pokes, rep.floats, rep.tight, rep.maxStep);
    std::printf("net: the ground at most %.2f m over a road's top; %zu pokes over 0.3 m\n", rep.maxPoke, rep.pokesOver);
    std::printf("net: the steepest piece %s\n", rep.maxGradeAt.c_str());
    for (const std::string& n : rep.notes) std::printf("  note: %s\n", n.c_str());
    const RoadCounts& c = b.counts;
    std::printf("build: solids %zu (collide %zu), markings %zu, zebras %zu, bays %zu, lamps %zu, signals %zu, "
                "junctions %zu\n",
                c.solids, c.collide, c.markings, c.zebras, c.bays, c.lamps, c.signals, c.junctions);
    std::vector<MassingStreet> ms;
    double total = 0.0;
    for (const RoadNetStreet& s : streets) {
        ms.push_back(roadNetStreet(s));
        for (std::size_t k = 1; k < s.pts.size(); ++k)
            total += std::hypot(s.pts[k].x - s.pts[k - 1].x, s.pts[k].y - s.pts[k - 1].y);
    }
    const RoadLoop loop = roadGraphLoop(ms, kRoadGraphSnap);
    std::printf("net length %.0f m; loop %.0f m round; its net %.0f m, %zu nodes, %zu edges; %zu bridges dropped\n", total,
                loop.length, loop.netLength, loop.nodes, loop.edges, loop.bridges);
    if (!boxesPath.empty()) {
        std::vector<MassingBox> boxes;
        text.clear();
        if (!readFile(boxesPath, text)) return die("can't read " + boxesPath);
        if (!importMassingBoxes(text, boxes, &err)) return die(boxesPath + ": " + err);
        const std::vector<std::size_t> gone = roadClearedBoxes(b, boxes, 0.0);
        std::printf("clears %zu rows:", gone.size());
        for (const std::size_t i : gone) std::printf(" %s", boxes[i].id.c_str());
        std::printf("\n");
        std::size_t in = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            bool cleared = false;
            for (const std::size_t gi : gone) cleared = cleared || gi == i;
            if (cleared) continue;
            const MassingFoot f = massingBoxFoot(boxes[i]);
            bool hit = false;
            for (const MassingFoot& band : b.band) hit = hit || massingFeetOverlap(f, band, 0.0);
            if (!hit) continue;
            if (++in <= 40 || listAll)
                std::printf("  in the band: %s (%s) at (%.0f, %.0f) %.0fx%.0f\n", boxes[i].id.c_str(), boxes[i].kind.c_str(),
                            boxes[i].cx, boxes[i].cy, boxes[i].sx, boxes[i].sy);
        }
        std::printf("box rows in the band: %zu\n", in);
    }
    RoadGraph g;
    roadGraphFromNet(streets, b, g);
    std::size_t signalled = 0;
    for (const RoadGraphJunction& j : g.junctions)
        if (!j.phases.empty()) ++signalled;
    std::printf("graph: lanes %zu, junctions %zu (signalled %zu), crossings %zu, bays %zu\n", g.lanes.size(),
                g.junctions.size(), signalled, g.crossings.size(), g.bays.size());
    if (!outPath.empty() && !writeGraph(outPath, g)) return die("can't write " + outPath);
    std::printf("ROADGRAPH OK\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string streetsPath, roadsPath, boxesPath, outPath, netPath, r16Path;
    double samples = 0.0, origin[2] = {0.0, 0.0}, spacing = 1.0;
    double area[4] = {0, 0, 0, 0};
    bool haveArea = false;
    double minWidth = 10.0;
    bool listAll = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--streets") streetsPath = v;
        else if (k == "--roads") roadsPath = v;
        else if (k == "--boxes") boxesPath = v;
        else if (k == "--out") outPath = v;
        else if (k == "--min-width") minWidth = std::atof(v.c_str());
        else if (k == "--net") netPath = v;
        else if (k == "--list-all") listAll = v == "1";
        else if (k == "--r16") r16Path = v;
        else if (k == "--samples") samples = std::atof(v.c_str());
        else if (k == "--spacing") spacing = std::atof(v.c_str());
        else if (k == "--origin") {
            if (std::sscanf(v.c_str(), "%lf,%lf", &origin[0], &origin[1]) != 2) return die("bad --origin");
        }
        else if (k == "--area") haveArea = std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf", &area[0], &area[1], &area[2], &area[3]) == 4;
        else return die("unknown option " + k);
    }
    if (!netPath.empty()) return runNet(netPath, r16Path, samples, origin, spacing, boxesPath, outPath, listAll);
    if (streetsPath.empty() || roadsPath.empty() || !haveArea) return die("--streets, --roads and --area are needed");
    std::string text, err;
    std::vector<MassingStreet> all;
    if (!readFile(streetsPath, text)) return die("can't read " + streetsPath);
    if (!importMassingStreets(text, all, &err)) return die(streetsPath + ": " + err);
    std::vector<RoadSpec> roads;
    text.clear();
    if (!readFile(roadsPath, text)) return die("can't read " + roadsPath);
    if (!importRoads(text, roads, &err)) return die(roadsPath + ": " + err);

    std::vector<MassingStreet> net;
    for (const MassingStreet& s : all) {
        if (s.width < minWidth - 1e-9 || s.points.size() < 2) continue;
        bool built = false; // a road of --roads is drawn from its build, not as a street
        for (const RoadSpec& r : roads) built = built || r.id == s.id;
        if (built) continue;
        bool inside = false;
        for (const MassingVec& p : s.points)
            if (p.x >= area[0] && p.x <= area[2] && p.y >= area[1] && p.y <= area[3]) inside = true;
        if (inside) net.push_back(s);
    }
    RoadGraph g;
    for (const MassingStreet& s : net) {
        double speed = 40.0;
        if (s.kind == "expressway") speed = 60.0;
        else if (s.kind == "coast_road" || s.kind == "link_road" || s.kind == "causeway") speed = 50.0;
        roadGraphStreetLanes(s, speed, false, 0.2, g);
    }
    roadGraphJunctions(net, kRoadGraphSnap, 0.2, g);
    const RoadLoop loop = roadGraphLoop(net, kRoadGraphSnap);
    std::printf("streets %zu (of %zu) at least %.1f m wide in the area\n", net.size(), all.size(), minWidth);
    std::printf("loop %.0f m round; its net %.0f m, %zu nodes, %zu edges; %zu bridges dropped\n", loop.length,
                loop.netLength, loop.nodes, loop.edges, loop.bridges);
    if (!loop.ring.empty()) {
        double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (const RoadPoint& p : loop.ring) {
            x0 = std::fmin(x0, p.x);
            y0 = std::fmin(y0, p.y);
            x1 = std::fmax(x1, p.x);
            y1 = std::fmax(y1, p.y);
        }
        std::printf("loop box x %.0f..%.0f y %.0f..%.0f\n", x0, x1, y0, y1);
    }

    std::vector<MassingBox> boxes;
    if (!boxesPath.empty()) {
        text.clear();
        if (!readFile(boxesPath, text)) return die("can't read " + boxesPath);
        if (!importMassingBoxes(text, boxes, &err)) return die(boxesPath + ": " + err);
    }
    for (const RoadSpec& r : roads) {
        RoadBuild b;
        roadBuild(r, b);
        roadGraphFromRoad(r, b, g);
        const RoadCounts& c = b.counts;
        std::printf("%s: solids %zu (collide %zu), markings %zu, zebras %zu, bays %zu, lamps %zu, signals %zu, signs %zu, "
                    "poles %zu, mouths %zu, junctions %zu\n",
                    r.id.c_str(), c.solids, c.collide, c.markings, c.zebras, c.bays, c.lamps, c.signals, c.signs, c.poles,
                    c.mouths, c.junctions);
        // Its ends against the net's nodes: the nearest street point to each.
        for (int e = 0; e < 2; ++e) {
            const double ex = e == 0 ? r.x0 : r.x1, ey = e == 0 ? r.y0 : r.y1;
            double best = std::numeric_limits<double>::infinity();
            std::string bestId;
            double bx = 0, by = 0;
            for (const MassingStreet& s : net)
                for (std::size_t k = 0; k + 1 < s.points.size(); ++k) {
                    const MassingVec a = s.points[k], q = s.points[k + 1];
                    const double dx = q.x - a.x, dy = q.y - a.y, l2 = dx * dx + dy * dy;
                    double t = l2 > 0 ? ((ex - a.x) * dx + (ey - a.y) * dy) / l2 : 0.0;
                    t = std::fmax(0.0, std::fmin(1.0, t));
                    const double px = a.x + dx * t, py = a.y + dy * t;
                    const double d = std::hypot(px - ex, py - ey);
                    if (d < best) {
                        best = d;
                        bestId = s.id;
                        bx = px;
                        by = py;
                    }
                }
            std::printf("%s end %d (%.0f, %.0f): nearest street %s at (%.0f, %.0f), %.0f m\n", r.id.c_str(), e + 1, ex, ey,
                        bestId.c_str(), bx, by, best);
        }
        if (!boxes.empty()) {
            const std::vector<std::size_t> gone = roadClearedBoxes(b, boxes, 0.0);
            std::printf("%s clears %zu rows:", r.id.c_str(), gone.size());
            for (const std::size_t i : gone) std::printf(" %s", boxes[i].id.c_str());
            std::printf("\n");
        }
    }
    std::size_t signalled = 0;
    for (const RoadGraphJunction& j : g.junctions)
        if (!j.phases.empty()) ++signalled;
    std::printf("graph: lanes %zu, junctions %zu (signalled %zu), crossings %zu, bays %zu\n", g.lanes.size(),
                g.junctions.size(), signalled, g.crossings.size(), g.bays.size());
    if (!outPath.empty() && !writeGraph(outPath, g)) return die("can't write " + outPath);
    std::printf("ROADGRAPH OK\n");
    return 0;
}
