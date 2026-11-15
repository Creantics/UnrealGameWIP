// ftcore_lotplan: plans kit buildings on street frontage runs and checks the street rules, offline (ftcore/BuildingLots.h).
//
//   ftcore_lotplan <boxes.csv> <kit.csv> <runs.csv> [lots_out.csv]
//
// Prints each lot (run, span, type, storeys, roof, wall, colours), the street-rule check and a census of pieces, boxes
// and triangles; lots_out.csv gets one row per lot. Exit 0, 1 when the street rules fail, 2 on an input error.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/BuildingLots.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace ftcore;

bool readFile(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[1 << 16];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    std::fclose(f);
    return true;
}

int die(const std::string& why) {
    std::fprintf(stderr, "ftcore_lotplan: %s\n", why.c_str());
    return 2;
}

const char* roofName(RoofKind r) { return r == RoofKind::Flat ? "flat" : r == RoofKind::Gable ? "gable" : "hip"; }
const char* wallName(WallSurface w) { return w == WallSurface::Tile ? "tile" : w == WallSurface::Concrete ? "concrete" : "stucco"; }

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) return die("usage: ftcore_lotplan <boxes.csv> <kit.csv> <runs.csv> [lots_out.csv]");
    std::string text, err;
    std::vector<MassingBox> boxes;
    if (!readFile(argv[1], text) || !importMassingBoxes(text, boxes, &err)) return die(std::string("boxes: ") + err);
    KitTable kit;
    if (!readFile(argv[2], text) || !parseKitTable(text, kit, &err)) return die(std::string("kit: ") + err);
    std::vector<FrontageRun> runs;
    if (!readFile(argv[3], text) || !parseFrontageRuns(text, runs, &err)) return die(std::string("runs: ") + err);

    FrontagePlan plan;
    planFrontage(runs, boxes, plan);
    std::vector<KitPlacement> placements;
    for (std::size_t i = 0; i < plan.lots.size(); ++i) {
        const BuildingLot& l = plan.lots[i];
        if (!facadePlacements(kit, plan.runs[static_cast<std::size_t>(l.run)], l, static_cast<int>(i), placements, &err))
            return die("grammar: " + err);
    }
    std::FILE* out = argc > 4 ? std::fopen(argv[4], "wb") : nullptr;
    if (out) std::fprintf(out, "lot,run,s0,s1,depth,floor_z,top,type,floors,roof,wall,wall_colour,setback,signature,parents\n");
    for (std::size_t i = 0; i < plan.lots.size(); ++i) {
        const BuildingLot& l = plan.lots[i];
        std::string parents;
        for (const std::string& p : l.parents) parents += (parents.empty() ? "" : ";") + p;
        std::printf("%3zu %-12s %7.2f-%7.2f %5.1fm d%5.1f %-9s %d st %-5s %-8s wall %u sb %.2f top %6.2f [%s]\n", i,
                    plan.runs[static_cast<std::size_t>(l.run)].id.c_str(), l.s0, l.s1, l.width(), l.depth,
                    lotTypeName(l.type), l.floors, roofName(l.roof), wallName(l.wall), l.wallColour, l.setback, l.top(),
                    parents.c_str());
        if (out)
            std::fprintf(out, "%zu,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%d,%s,%s,%u,%.2f,%08x,%s\n", i,
                         plan.runs[static_cast<std::size_t>(l.run)].id.c_str(), l.s0, l.s1, l.depth, l.floorZ, l.top(),
                         lotTypeName(l.type), l.floors, roofName(l.roof), wallName(l.wall), l.wallColour, l.setback,
                         lotSignature(l), parents.c_str());
    }
    if (out) std::fclose(out);
    const FrontageCheck c = checkFrontage(plan.lots);
    const KitCensus n = kitCensus(kit, plan, placements);
    std::printf("replaced boxes %zu, repairs %d\n", plan.replaced.size(), plan.repairs);
    std::printf("census: %d lots (corner %d, pencil %d, walkup %d, tiled %d, townhouse %d), %d pieces, %d boxes, %lld triangles\n",
                n.lots, n.types[0], n.types[1], n.types[2], n.types[3], n.types[4], n.pieces, n.boxes, n.triangles);
    std::printf("street rules: %s (equal within 40 m %d, neighbour twins %d, poor 60 m windows %d, colour triples %d)%s%s\n",
                c.ok ? "PASS" : "FAIL", c.equalWithin40, c.neighbourTwins, c.poorWindows, c.colourTriples,
                c.ok ? "" : ": ", c.firstFault.c_str());
    return c.ok ? 0 : 1;
}
