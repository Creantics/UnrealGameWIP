// ftcore_roadgraph: writes the road graph CSV (ftcore/RoadGraph.h) for a massing's streets and built roads, and prints
// the drivable loop, the built roads' counts, where each road's ends meet the net and the deck and pier rows each road
// clears. An offline build tool for the private tables; C stdio only.
//
//   ftcore_roadgraph --streets <csv> --roads <csv> --area <x0,y0,x1,y1> [--boxes <csv>] [--min-width <m>] [--out <csv>]
//   ftcore_roadgraph --net <csv> [--r16 <file> --samples <n> --origin <x,y> [--spacing <m>]] [--boxes <csv>] [--out <csv>] [--list-all 1]
//                    [--dump <x0,y0,x1,y1>] (every solid centred in that area, one line each)
//                    [--canal <x0,y0,x1,y1> [--bridge <at>] --quay-out <csv>] (a canal's railing, mast and footbridge
//                    points, roadQuayPoints, written as roadQuayCsv; --bridge: the footbridge's centre along the banks;
//                    without --canal, --quay-out takes the table's first canal row; every canal row's channel solids,
//                    roadCanalBuild, are built and counted)
//   ftcore_roadgraph --check 1 (--net <csv> [--r16 ...] | --roads <csv>) [--boxes <csv>] [--graph <csv>] [--list-all 1]
//   ftcore_roadgraph --yards 1 --boxes <csv> --area <x0,y0,x1,y1> --out <csv> [--net <csv>] [--streets <csv>]
//                    [--r16 ...] [--goals <csv>] [--clear <x0,y0,x1,y1>]... [--drop-ids <a;b>] [--drop-kinds <k1;k2>]
//                    [--drop-clash 1]
//
// The fourth form writes a new box table: the rows of --boxes less those dropped, plus fenced yards with a shed each on
// the open plains of the area (massingYardFill); see runYards. Its last line is YARDS OK.
//
// The third form builds the roads (a network table, or each road of a roads table) and prints the clashes a vehicle
// meets: box rows standing in a carriageway (roadBoxClashes, after the decks and piers the roads clear) and kerb or
// pavement faces across a lane of the graph or lying in a carriageway (roadKerbFaces), one line each, then the counts.
// Any clash: the last line is ROADCHECK FAIL and it exits 1; none: ROADCHECK OK.
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
#include "ftcore/MassingLayout.h"
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

bool loadGround(const std::string& r16Path, double samples, const double origin[2], double spacing,
                std::vector<std::uint16_t>& grid, MassingGround& ground, std::string& err) {
    if (r16Path.empty()) return true;
    std::string text;
    if (!readFile(r16Path, text)) {
        err = "can't read " + r16Path;
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(samples);
    if (n < 2 || text.size() != n * n * 2) {
        err = "the r16 is not samples x samples u16";
        return false;
    }
    grid.resize(n * n);
    for (std::size_t k = 0; k < n * n; ++k)
        grid[k] = static_cast<std::uint16_t>(static_cast<std::uint8_t>(text[2 * k]) |
                                             (static_cast<std::uint8_t>(text[2 * k + 1]) << 8));
    ground = MassingGround{grid.data(), n, n, spacing, origin[0], origin[1]};
    return true;
}

// One build's clashes: prints them (the first 60 of each kind unless listAll) and adds to the counts.
void checkBuild(const char* what, const RoadBuild& b, const std::vector<MassingBox>& boxes, const RoadGraph* graph,
                bool listAll, std::size_t& boxClashes, std::size_t& faces) {
    std::vector<MassingBox> kept = boxes;
    for (const std::size_t i : roadClearedBoxes(b, kept, 0.0)) kept[i].kind = "underpass";
    const std::vector<std::size_t> clash = roadBoxClashes(b, kept);
    std::size_t shown = 0;
    for (const std::size_t i : clash) {
        const MassingBox& x = kept[i];
        if (++shown <= 60 || listAll)
            std::printf("  box in carriageway (%s): %s (%s) at (%.2f, %.2f) %.1fx%.1f z %.2f..%.2f\n", what, x.id.c_str(),
                        x.kind.c_str(), x.cx, x.cy, x.sx, x.sy, x.z0, x.z1);
    }
    const std::vector<RoadFace> f = roadKerbFaces(b, graph);
    shown = 0;
    for (const RoadFace& r : f)
        if (++shown <= 60 || listAll)
            std::printf("  face (%s): %s at (%.2f, %.2f): %s\n", what, r.piece.c_str(), r.x, r.y, r.fault.c_str());
    // The builder hides the clashing rows (FTBuildRoads::Prepare); any still clashing after that fail the check.
    for (const std::size_t i : clash) kept[i].kind = "underpass";
    const std::size_t left = roadBoxClashes(b, kept).size();
    std::printf("%s: box rows in a carriageway %zu (hidden by the builder, %zu left), kerb faces %zu\n", what,
                clash.size(), left, f.size());
    boxClashes += left;
    faces += f.size();
}

int runCheck(const std::string& netPath, const std::string& roadsPath, const std::string& r16Path, double samples,
             const double origin[2], double spacing, const std::string& boxesPath, const std::string& graphPath,
             bool listAll) {
    std::string text, err;
    std::vector<MassingBox> boxes;
    if (!boxesPath.empty()) {
        if (!readFile(boxesPath, text)) return die("can't read " + boxesPath);
        if (!importMassingBoxes(text, boxes, &err)) return die(boxesPath + ": " + err);
    }
    RoadGraph graph;
    if (!graphPath.empty()) {
        text.clear();
        if (!readFile(graphPath, text)) return die("can't read " + graphPath);
        if (!importRoadGraph(text, graph, &err)) return die(graphPath + ": " + err);
    }
    const RoadGraph* g = graphPath.empty() ? nullptr : &graph;
    std::size_t boxClashes = 0, faces = 0;
    if (!netPath.empty()) {
        std::vector<RoadNetStreet> streets;
        text.clear();
        if (!readFile(netPath, text)) return die("can't read " + netPath);
        if (!importRoadNet(text, streets, &err)) return die(netPath + ": " + err);
        std::vector<std::uint16_t> grid;
        MassingGround ground;
        if (!loadGround(r16Path, samples, origin, spacing, grid, ground, err)) return die(err);
        RoadBuild b;
        RoadNetReport rep;
        roadNetBuild(streets, ground, b, &rep);
        std::printf("net: %zu kerb and pavement pieces pruned in the build, %zu corner aprons\n", rep.pruned,
                    rep.aprons);
        for (const std::string& n : rep.notes)
            if (listAll && n.rfind("pruned", 0) == 0) std::printf("  note: %s\n", n.c_str());
        checkBuild("net", b, boxes, g, listAll, boxClashes, faces);
    } else {
        std::vector<RoadSpec> roads;
        text.clear();
        if (!readFile(roadsPath, text)) return die("can't read " + roadsPath);
        if (!importRoads(text, roads, &err)) return die(roadsPath + ": " + err);
        for (const RoadSpec& r : roads) {
            RoadBuild b;
            roadBuild(r, b);
            checkBuild(r.id.c_str(), b, boxes, g, listAll, boxClashes, faces);
        }
    }
    std::printf("check: box rows left in a carriageway %zu, kerb and pavement faces %zu (graph lanes %zu)\n", boxClashes,
                faces, graph.lanes.size());
    std::printf(boxClashes + faces == 0 ? "ROADCHECK OK\n" : "ROADCHECK FAIL\n");
    return boxClashes + faces == 0 ? 0 : 1;
}

// The yards form: a new box table from --boxes, less the rows dropped (--drop-ids, --drop-kinds, and with
// --drop-clash 1 the rows standing in the network's carriageways), plus yards on the open plains of the area
// (massingYardFill: keeping off the boxes left, the lanes of --streets, the network's bands and furniture, the
// --clear rects and 15 m round each goal and spawn of --goals). Rows kept are copied as they are.
struct YardArgs {
    std::string boxesPath, netPath, streetsPath, goalsPath, outPath, r16Path;
    double samples = 0.0, origin[2] = {0.0, 0.0}, spacing = 1.0;
    MassingRect area;
    std::vector<MassingRect> clear;
    std::vector<std::string> dropIds, dropKinds;
    bool dropClash = false;
};

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::size_t p = 0;
    while (p <= s.size()) {
        std::size_t q = s.find(';', p);
        if (q == std::string::npos) q = s.size();
        if (q > p) out.push_back(s.substr(p, q - p));
        p = q + 1;
    }
    return out;
}

bool listed(const std::vector<std::string>& v, const std::string& s) {
    for (const std::string& x : v)
        if (x == s) return true;
    return false;
}

int runYards(const YardArgs& a) {
    std::string text, err;
    if (!readFile(a.boxesPath, text)) return die("can't read " + a.boxesPath);
    std::vector<MassingBox> boxes;
    if (!importMassingBoxes(text, boxes, &err)) return die(a.boxesPath + ": " + err);
    const std::string boxText = text;
    std::vector<std::uint16_t> grid;
    MassingGround ground;
    if (!loadGround(a.r16Path, a.samples, a.origin, a.spacing, grid, ground, err)) return die(err);
    RoadBuild b;
    if (!a.netPath.empty()) {
        std::vector<RoadNetStreet> net;
        text.clear();
        if (!readFile(a.netPath, text)) return die("can't read " + a.netPath);
        if (!importRoadNet(text, net, &err)) return die(a.netPath + ": " + err);
        roadNetBuild(net, ground, b);
    }
    std::vector<MassingStreet> lanes;
    if (!a.streetsPath.empty()) {
        text.clear();
        if (!readFile(a.streetsPath, text)) return die("can't read " + a.streetsPath);
        if (!importMassingStreets(text, lanes, &err)) return die(a.streetsPath + ": " + err);
    }
    MassingYards opts;
    opts.area = a.area;
    opts.keepClear = a.clear;
    if (!a.goalsPath.empty()) {
        text.clear();
        if (!readFile(a.goalsPath, text)) return die("can't read " + a.goalsPath);
        std::vector<std::string> head;
        std::size_t p = 0;
        while (p < text.size()) {
            std::size_t q = text.find('\n', p);
            if (q == std::string::npos) q = text.size();
            std::string line = text.substr(p, q - p);
            p = q + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == '"') continue;
            std::vector<std::string> f;
            std::size_t s0 = 0;
            while (s0 <= line.size()) {
                std::size_t s1 = line.find(',', s0);
                if (s1 == std::string::npos) s1 = line.size();
                f.push_back(line.substr(s0, s1 - s0));
                s0 = s1 + 1;
            }
            if (head.empty()) {
                head = f;
                continue;
            }
            auto num = [&](const char* name, double& v) {
                for (std::size_t i = 0; i < head.size() && i < f.size(); ++i)
                    if (head[i] == name && !f[i].empty()) {
                        v = std::atof(f[i].c_str());
                        return true;
                    }
                return false;
            };
            double x = 0.0, y = 0.0;
            if (num("x", x) && num("y", y)) opts.keepClear.push_back({x - 15.0, y - 15.0, x + 15.0, y + 15.0});
            if (num("spawn_x", x) && num("spawn_y", y)) opts.keepClear.push_back({x - 15.0, y - 15.0, x + 15.0, y + 15.0});
        }
    }
    // The rows dropped.
    std::vector<bool> drop(boxes.size(), false);
    std::size_t byId = 0, byKind = 0, byClash = 0;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (listed(a.dropIds, boxes[i].id)) {
            drop[i] = true;
            ++byId;
        } else if (listed(a.dropKinds, boxes[i].kind)) {
            drop[i] = true;
            ++byKind;
        }
    }
    if (a.dropClash) {
        std::vector<MassingBox> kept = boxes;
        for (const std::size_t i : roadClearedBoxes(b, kept, 0.0)) kept[i].kind = "underpass";
        for (const std::size_t i : roadBoxClashes(b, kept))
            if (!drop[i]) {
                drop[i] = true;
                ++byClash;
            }
    }
    std::vector<MassingBox> left;
    std::vector<std::string> gone;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (drop[i])
            gone.push_back(boxes[i].id);
        else
            left.push_back(boxes[i]);
    }
    const std::size_t before = left.size();
    std::vector<MassingFoot> taken = b.band;
    taken.insert(taken.end(), b.furniture.begin(), b.furniture.end());
    const MassingYardCounts n = massingYardFill(left, lanes, taken, ground, opts);
    // The table: the kept rows as they are, then the yards.
    const char* const kHead = "id,row,src,name,kind,cx,cy,sx,sy,rot,z0,z1,ground,terr_min,terr_max,sink_m,cover,note,slope";
    std::string out;
    bool headSeen = false;
    std::size_t p = 0;
    while (p < boxText.size()) {
        std::size_t q = boxText.find('\n', p);
        if (q == std::string::npos) q = boxText.size();
        std::string line = boxText.substr(p, q - p);
        p = q + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == '"') {
            out += line + "\n";
            continue;
        }
        if (!headSeen) {
            if (line != kHead) return die("the box table's header is not " + std::string(kHead));
            headSeen = true;
            out += "# r6: rows dropped and yards added by ftcore_roadgraph --yards (note R6-YARD).\n" + line + "\n";
            continue;
        }
        const std::string id = line.substr(0, line.find(','));
        if (listed(gone, id)) continue;
        out += line + "\n";
    }
    char buf[512];
    for (std::size_t i = before; i < left.size(); ++i) {
        const MassingBox& x = left[i];
        const double g = x.z0 + 0.1;
        std::snprintf(buf, sizeof buf, "%s,%s,INV,,%s,%.3f,%.3f,%.3f,%.3f,0.0,%.3f,%.3f,%.3f,%.3f,%.3f,0.1,False,R6-YARD,\n",
                      x.id.c_str(), x.row.c_str(), x.kind.c_str(), x.cx, x.cy, x.sx, x.sy, x.z0, x.z1, g, g, g);
        out += buf;
    }
    std::FILE* f = std::fopen(a.outPath.c_str(), "wb");
    if (!f) return die("can't write " + a.outPath);
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    std::printf("yards: dropped %zu rows (%zu by id, %zu by kind, %zu in a carriageway):", gone.size(), byId, byKind,
                byClash);
    for (std::size_t i = 0; i < gone.size() && i < 80; ++i) std::printf(" %s", gone[i].c_str());
    std::printf("%s\n", gone.size() > 80 ? " ..." : "");
    std::printf("yards: %zu yards (%.0f m2), %zu sheds, %zu fence pieces: %zu rows added; %zu rows out\n", n.yards, n.area,
                n.sheds, n.fences, left.size() - before, left.size());
    std::printf("YARDS OK\n");
    return 0;
}

int runNet(const std::string& netPath, const std::string& r16Path, double samples, const double origin[2], double spacing,
           const std::string& boxesPath, const std::string& outPath, bool listAll, const double* dump,
           const RoadCanal* canal, const std::string& quayOut) {
    std::string text, err;
    std::vector<RoadNetStreet> streets;
    if (!readFile(netPath, text)) return die("can't read " + netPath);
    std::vector<RoadCanal> canals;
    if (!importRoadNet(text, streets, &err, &canals)) return die(netPath + ": " + err);
    std::vector<std::uint16_t> grid;
    MassingGround ground;
    if (!loadGround(r16Path, samples, origin, spacing, grid, ground, err)) return die(err);
    RoadBuild b;
    RoadNetReport rep;
    roadNetBuild(streets, ground, b, &rep);
    // The table's canal rows: their channel solids; the first gives the quay points unless --canal does.
    if (!canal && !quayOut.empty() && !canals.empty()) canal = &canals.front();
    if (!canal && !quayOut.empty()) return die("--quay-out needs --canal or a canal row in the table");
    for (const RoadCanal& c : canals)
        std::printf("canal %s: %zu solids (floor %.2f, water %.2f)\n", c.id.c_str(), roadCanalBuild(c, ground, b),
                    c.floorZ, c.waterZ);
    std::printf("net: streets %zu, nodes %zu, edges %zu, junctions %zu, bends %zu, ends %zu, pieces %zu, piers %zu\n",
                streets.size(), rep.nodes, rep.edges, rep.junctions, rep.bends, rep.ends, rep.pieces, rep.piers);
    std::printf("net: steep %zu (max grade %.1f%%), pokes %zu, floats %zu, tight %zu, max step %.3f m, pruned %zu\n",
                rep.steep, rep.maxGrade * 100.0, rep.pokes, rep.floats, rep.tight, rep.maxStep, rep.pruned);
    std::printf("net: the ground at most %.2f m over a road's top; %zu pokes over 0.3 m\n", rep.maxPoke, rep.pokesOver);
    std::printf("net: the steepest piece %s\n", rep.maxGradeAt.c_str());
    std::printf("net: scrambles %zu\n", rep.scrambles);
    for (const std::string& n : rep.notes) std::printf("  note: %s\n", n.c_str());
    if (dump)
        for (const RoadSolid& sd : b.solids) {
            const MassingBox& x = sd.box;
            if (x.cx < dump[0] || x.cx > dump[2] || x.cy < dump[1] || x.cy > dump[3]) continue;
            std::printf("  solid %s %s %d (%.2f, %.2f) %.2fx%.2f rot %.1f z %.3f..%.3f rise %.4f,%.4f\n", x.id.c_str(),
                        sd.tag.c_str(), static_cast<int>(sd.shape), x.cx, x.cy, x.sx, x.sy, x.rot, x.z0, x.z1, x.riseU,
                        x.riseW);
        }
    const RoadCounts& c = b.counts;
    std::size_t inst = 0;
    for (const RoadSolid& sd : b.solids)
        if (sd.instanced) ++inst;
    std::printf("build: solids %zu (collide %zu, instanced %zu, actors %zu), markings %zu, zebras %zu, bays %zu, "
                "lamps %zu, signals %zu, signs %zu, poles %zu, junctions %zu\n",
                c.solids, c.collide, inst, c.solids - inst, c.markings, c.zebras, c.bays, c.lamps, c.signals, c.signs,
                c.poles, c.junctions);
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
    if (canal) {
        std::vector<RoadQuayPoint> qp;
        RoadQuayCounts qc;
        roadQuayPoints(*canal, b, ground, qp, &qc);
        std::printf("quay %s: spans %zu, closing posts %zu, masts %zu (%zu moved), bridge pieces %zu at %.1f, railed %.0f m, "
                    "road feet in the water %zu, rows %zu\n",
                    canal->id.c_str(), qc.spans, qc.posts, qc.masts, qc.mastsMoved, qc.bridge, qc.bridgeAt, qc.railed,
                    qc.roadInWater, qp.size());
        if (qc.roadInWater > 0) return die("the canal reaches over the road's band");
        const std::string csv = roadQuayCsv(qp);
        std::FILE* f = std::fopen(quayOut.c_str(), "wb");
        if (!f) return die("can't write " + quayOut);
        std::fwrite(csv.data(), 1, csv.size(), f);
        std::fclose(f);
    }
    std::printf("ROADGRAPH OK\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string streetsPath, roadsPath, boxesPath, outPath, netPath, r16Path, graphPath;
    bool check = false;
    double samples = 0.0, origin[2] = {0.0, 0.0}, spacing = 1.0;
    double area[4] = {0, 0, 0, 0};
    bool haveArea = false;
    double dump[4] = {0, 0, 0, 0};
    bool haveDump = false;
    double minWidth = 10.0;
    bool listAll = false;
    bool yards = false;
    YardArgs ya;
    RoadCanal canal;
    bool haveCanal = false;
    std::string quayOut;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--yards") yards = v == "1";
        else if (k == "--goals") ya.goalsPath = v;
        else if (k == "--drop-ids") ya.dropIds = splitList(v);
        else if (k == "--drop-kinds") ya.dropKinds = splitList(v);
        else if (k == "--drop-clash") ya.dropClash = v == "1";
        else if (k == "--clear") {
            MassingRect r;
            if (std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf", &r.x0, &r.y0, &r.x1, &r.y1) != 4) return die("bad --clear");
            ya.clear.push_back(r);
        }
        else if (k == "--streets") streetsPath = v;
        else if (k == "--roads") roadsPath = v;
        else if (k == "--boxes") boxesPath = v;
        else if (k == "--out") outPath = v;
        else if (k == "--min-width") minWidth = std::atof(v.c_str());
        else if (k == "--net") netPath = v;
        else if (k == "--list-all") listAll = v == "1";
        else if (k == "--check") check = v == "1";
        else if (k == "--graph") graphPath = v;
        else if (k == "--r16") r16Path = v;
        else if (k == "--samples") samples = std::atof(v.c_str());
        else if (k == "--spacing") spacing = std::atof(v.c_str());
        else if (k == "--origin") {
            if (std::sscanf(v.c_str(), "%lf,%lf", &origin[0], &origin[1]) != 2) return die("bad --origin");
        }
        else if (k == "--dump") haveDump = std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf", &dump[0], &dump[1], &dump[2], &dump[3]) == 4;
        else if (k == "--canal")
            haveCanal = std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf", &canal.x0, &canal.y0, &canal.x1, &canal.y1) == 4;
        else if (k == "--bridge") canal.bridgeAt = std::atof(v.c_str());
        else if (k == "--quay-out") quayOut = v;
        else if (k == "--area") haveArea = std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf", &area[0], &area[1], &area[2], &area[3]) == 4;
        else return die("unknown option " + k);
    }
    if (yards) {
        if (boxesPath.empty() || outPath.empty() || !haveArea) return die("--yards needs --boxes, --area and --out");
        ya.boxesPath = boxesPath;
        ya.netPath = netPath;
        ya.streetsPath = streetsPath;
        ya.outPath = outPath;
        ya.r16Path = r16Path;
        ya.samples = samples;
        ya.origin[0] = origin[0];
        ya.origin[1] = origin[1];
        ya.spacing = spacing;
        ya.area = {area[0], area[1], area[2], area[3]};
        return runYards(ya);
    }
    if (check) {
        if (netPath.empty() && roadsPath.empty()) return die("--check needs --net or --roads");
        return runCheck(netPath, roadsPath, r16Path, samples, origin, spacing, boxesPath, graphPath, listAll);
    }
    if (haveCanal && (netPath.empty() || quayOut.empty())) return die("--canal needs --net and --quay-out");
    if (!quayOut.empty() && netPath.empty()) return die("--quay-out needs --net");
    if (!netPath.empty()) return runNet(netPath, r16Path, samples, origin, spacing, boxesPath, outPath, listAll,
                                         haveDump ? dump : nullptr, haveCanal ? &canal : nullptr, quayOut);
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
