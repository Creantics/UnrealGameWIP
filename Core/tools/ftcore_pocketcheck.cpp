// ftcore_pocketcheck: finds the stuck pockets (ftcore/TerrainPocket.h) a box table leaves on its height field, places
// the land that the lead can get into on foot but not out of, and with --plinths 1 the retaining plinths and trough fills
// the massing build lays for them (-Plinths) and the pockets left after. An offline build step on the private tables.
//
//   ftcore_pocketcheck --boxes B --r16 H --samples N --origin X,Y --spacing M [--rect X0,Y0,X1,Y1] [--anchor X,Y[;X,Y]]
//                      [--exempt-rect X0,Y0,X1,Y1[;...]] [--cell C] [--jump J] [--plinths 1] [--out-plinths CSV]
//                      [--report R] [--list K]
//
// Checks the boxes the build makes (kit and named rows) over --rect (default: their bounds, 10 m out, within the field).
// The lead comes in at the --anchor points (the rect's standable edge cells without one) and gets out at them, the
// edge or the sea. --jump J lets a jump of J m out (default 0).
// --exempt-rect: pockets lying wholly inside one of these rects (0.5 m slack) are left out and counted as exempt (an
// area the lead can't enter that the boxes don't show, e.g. a railed canal). Lists the K largest pockets (default 40). Exit 0 on POCKETCHECK OK (no pocket; after the plinths with --plinths 1), 1 on
// any pocket, 2 on an error.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/TerrainPocket.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

bool writeFile(const std::string& path, const std::string& s) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    return true;
}

int die(const std::string& what) {
    std::fprintf(stderr, "ftcore_pocketcheck: error: %s\n", what.c_str());
    return 2;
}

std::string num(double v, int places = 2) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", places, v);
    return b;
}

bool parseList(const std::string& s, double out[], int n) {
    std::size_t p = 0;
    for (int i = 0; i < n; ++i) {
        const std::size_t q = s.find(',', p);
        const std::string t = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
        char* end = nullptr;
        out[i] = std::strtod(t.c_str(), &end);
        if (end == t.c_str()) return false;
        if (q == std::string::npos) return i == n - 1;
        p = q + 1;
    }
    return true;
}

std::string pocketLine(std::size_t i, const Pocket& p, const std::vector<MassingBox>& boxes) {
    std::string s = "  pocket " + std::to_string(i + 1) + ": at " + num(p.cx, 1) + "," + num(p.cy, 1) + " (x " + num(p.minX, 1) + ".." +
                    num(p.maxX, 1) + ", y " + num(p.minY, 1) + ".." + num(p.maxY, 1) + "), " + num(p.area, 2) + " m2, width " +
                    num(p.width, 1) + " m, floor " + num(p.floorMin, 2) + " m";
    s += p.hasSpill ? ", spill " + num(p.spill, 2) + " m (+" + num(p.spill - p.floorMin, 2) + ")" : ", no way out";
    if (p.nearBox >= 0 && static_cast<std::size_t>(p.nearBox) < boxes.size()) s += ", by " + boxes[static_cast<std::size_t>(p.nearBox)].id;
    return s + "\n";
}

std::string summary(const char* what, const PocketReport& r, const PocketSpec& spec, const std::vector<MassingBox>& boxes, std::size_t list) {
    std::size_t narrow = 0, wide = 0, near = 0;
    for (const Pocket& p : r.pockets) {
        (p.width > spec.narrowWidth + 1e-9 ? wide : narrow) += 1;
        near += p.nearBox >= 0 ? 1 : 0;
    }
    std::string s = std::string(what) + ": " + std::to_string(r.pockets.size()) + " pockets, " + std::to_string(r.pocketCells) +
                    " cells (" + num(static_cast<double>(r.pocketCells) * spec.cell * spec.cell, 1) + " m2); " + std::to_string(narrow) +
                    " no wider than " + num(spec.narrowWidth, 1) + " m, " + std::to_string(wide) + " wider; " + std::to_string(near) +
                    " by a building. Grid " + std::to_string(r.cellsX) + " x " + std::to_string(r.cellsY) + ", " +
                    std::to_string(r.inPlay) + " cells in play, " + std::to_string(r.reached) + " reached; " +
                    std::to_string(r.exempted) + " exempt.\n";
    for (std::size_t i = 0; i < r.pockets.size() && i < list; ++i) s += pocketLine(i, r.pockets[i], boxes);
    return s;
}

} // namespace

int main(int argc, char** argv) {
    std::string boxesPath, r16Path, outPlinths, reportPath, anchorText;
    double samples = 0.0, origin[2] = {0.0, 0.0}, spacing = 1.0, rectV[4] = {0, 0, 0, 0};
    bool haveRect = false, plinths = false;
    std::size_t list = 40;
    PlinthSpec spec;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--boxes") boxesPath = v;
        else if (k == "--r16") r16Path = v;
        else if (k == "--samples") samples = std::atof(v.c_str());
        else if (k == "--origin") {
            if (!parseList(v, origin, 2)) return die("--origin is X,Y");
        } else if (k == "--spacing") spacing = std::atof(v.c_str());
        else if (k == "--rect") {
            if (!parseList(v, rectV, 4)) return die("--rect is X0,Y0,X1,Y1");
            haveRect = true;
        } else if (k == "--anchor") anchorText = v;
        else if (k == "--exempt-rect") {
            if (!parsePocketRects(v, spec.pocket.exempt)) return die("--exempt-rect is X0,Y0,X1,Y1[;X0,Y0,X1,Y1]");
        }
        else if (k == "--cell") spec.pocket.cell = std::atof(v.c_str());
        else if (k == "--jump") spec.pocket.jumpRise = std::atof(v.c_str());
        else if (k == "--plinths") plinths = v == "1";
        else if (k == "--out-plinths") outPlinths = v;
        else if (k == "--report") reportPath = v;
        else if (k == "--list") list = static_cast<std::size_t>(std::atol(v.c_str()));
        else return die("unknown switch " + k);
    }
    if (boxesPath.empty() || r16Path.empty() || samples < 2.0) return die("needs --boxes, --r16 and --samples");

    std::string text, err;
    if (!readFile(boxesPath, text)) return die("can't read " + boxesPath);
    std::vector<MassingBox> boxes;
    if (!importMassingBoxes(text, boxes, &err)) return die(boxesPath + ": " + err);
    std::string raw;
    if (!readFile(r16Path, raw)) return die("can't read " + r16Path);
    const std::size_t n = static_cast<std::size_t>(samples);
    if (raw.size() != n * n * 2) return die("the r16 is not samples x samples u16");
    std::vector<std::uint16_t> grid(n * n);
    for (std::size_t k = 0; k < n * n; ++k)
        grid[k] = static_cast<std::uint16_t>(static_cast<std::uint8_t>(raw[2 * k]) | (static_cast<std::uint8_t>(raw[2 * k + 1]) << 8));
    const MassingGround ground{grid.data(), n, n, spacing, origin[0], origin[1]};

    std::vector<bool> include(boxes.size());
    for (std::size_t i = 0; i < boxes.size(); ++i) include[i] = massingVisible(massingRole(boxes[i]));
    const PocketRect rect = haveRect ? PocketRect{rectV[0], rectV[1], rectV[2], rectV[3]} : pocketRectOf(ground, boxes, include, 10.0);
    std::vector<PocketPoint> anchors;
    for (std::size_t p = 0; p < anchorText.size();) {
        const std::size_t q = anchorText.find(';', p);
        const std::string one = anchorText.substr(p, q == std::string::npos ? std::string::npos : q - p);
        double xy[2];
        if (!parseList(one, xy, 2)) return die("--anchor is X,Y[;X,Y]");
        anchors.push_back(PocketPoint{xy[0], xy[1]});
        if (q == std::string::npos) break;
        p = q + 1;
    }

    std::string out = "ftcore_pocketcheck: " + boxesPath + "\nrect x " + num(rect.minX, 1) + ".." + num(rect.maxX, 1) + ", y " +
                      num(rect.minY, 1) + ".." + num(rect.maxY, 1) + "; cell " + num(spec.pocket.cell, 2) + " m, step " +
                      num(spec.pocket.maxStep, 2) + " m, jump " + num(spec.pocket.jumpRise, 2) + " m, walkable z " +
                      num(spec.pocket.walkableZ, 2) + ", capsule h " + num(spec.pocket.capsuleHeight, 2) + " m, sea " +
                      num(spec.pocket.seaLevel, 2) + " m\n";
    bool failed = false;
    if (!plinths) {
        PocketReport report;
        if (!findPockets(ground, boxes, include, rect, anchors, spec.pocket, report, &err)) return die(err);
        out += summary("before", report, spec.pocket, boxes, list);
        failed = !report.pockets.empty();
    } else {
        std::vector<MassingBox> made;
        PlinthStats stats;
        if (!planPlinths(ground, boxes, include, rect, anchors, spec, made, stats, &err)) return die(err);
        out += "before: " + std::to_string(stats.pocketsBefore) + " pockets, " + std::to_string(stats.pocketCellsBefore) + " cells\n";
        out += "plinths: " + std::to_string(stats.slopedBuildings) + " buildings on sloping ground, " + std::to_string(stats.skirts) +
               " skirt boxes, " + std::to_string(stats.fills) + " fill boxes over " + std::to_string(stats.filledPockets) +
               " pocket fills (" + std::to_string(stats.wideFilled) + " wide), " + std::to_string(stats.stairs) + " stair steps out of " + std::to_string(stats.stairedPockets) +
               " pockets, " + std::to_string(stats.passes) + " passes\n";
        std::vector<MassingBox> all(boxes);
        all.insert(all.end(), made.begin(), made.end());
        out += summary("after", stats.after, spec.pocket, all, list);
        failed = !stats.after.pockets.empty();
        if (!outPlinths.empty()) {
            std::string csv = "id,src,kind,cx,cy,sx,sy,rot,z0,z1,cover,slope\n";
            for (const MassingBox& b : made)
                csv += b.id + ",INV," + b.kind + "," + num(b.cx, 3) + "," + num(b.cy, 3) + "," + num(b.sx, 3) + "," + num(b.sy, 3) + "," +
                       num(b.rot, 3) + "," + num(b.z0, 3) + "," + num(b.z1, 3) + ",False,\n";
            if (!writeFile(outPlinths, csv)) return die("can't write " + outPlinths);
        }
    }
    out += failed ? "POCKETCHECK FAIL\n" : "POCKETCHECK OK\n";
    std::fputs(out.c_str(), stdout);
    if (!reportPath.empty() && !writeFile(reportPath, out)) return die("can't write " + reportPath);
    return failed ? 1 : 0;
}
