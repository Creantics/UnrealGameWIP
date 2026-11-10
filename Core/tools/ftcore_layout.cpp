// ftcore_layout: runs the lane layout (ftcore/MassingLayout.h) on a massing's box and street tables and writes the
// boxes it moved or added, and the lane dressing, as CSV, with each lane's wall-to-wall width on stdout. An offline
// build tool for the private tables (the caller merges the rows into its copy of the box table); C stdio only.
//
//   ftcore_layout --boxes <csv> --streets <csv> --r16 <file> --samples <n> --origin <x,y> [--spacing <m>]
//                 --area <x0,y0,x1,y1> [--clear <x0,y0,x1,y1>]... [--lane-up-to <m>] [--out <csv>] [--no-dress]
//
// The r16 is the world's ground: n x n little-endian u16 samples, rows from the south, h = (v - 32768) / 128, sample
// (0, 0) at origin. The out CSV has the box table's columns id,row,src,kind,cx,cy,sx,sy,rot,z0,z1,cover,slope and an
// op column: "set" for a table row moved, "add" for a new one. The last line on stdout is LAYOUT OK; an error prints
// "ftcore_layout: error: ..." and exits 1.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/MassingLayout.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

int fail(const std::string& what) {
    std::fprintf(stderr, "ftcore_layout: error: %s\n", what.c_str());
    return 1;
}

bool parseNumbers(const char* s, double* out, int count) {
    for (int i = 0; i < count; ++i) {
        char* end = nullptr;
        out[i] = std::strtod(s, &end);
        if (end == s) return false;
        s = end;
        if (i + 1 < count) {
            if (*s != ',') return false;
            ++s;
        }
    }
    return *s == '\0';
}

const char* sourceName(MassingSource s) {
    switch (s) {
    case MassingSource::Plan: return "BLD";
    case MassingSource::Invented: return "INV";
    case MassingSource::Zone: return "Z";
    case MassingSource::Open: return "OPEN";
    case MassingSource::Proxy: return "PROXY";
    case MassingSource::Wall: return "WALL";
    }
    return "INV";
}

void writeRow(std::FILE* f, const char* op, const MassingBox& b) {
    std::string slope;
    if (b.sloped) {
        char t[96];
        std::snprintf(t, sizeof t, "x %.4f to %.4f, %.4f thick", b.slopeFrom, b.slopeTo, b.slopeThick);
        slope = t;
    }
    std::fprintf(f, "%s,%s,%s,%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%s,%s\n", op, b.id.c_str(), b.row.c_str(),
                 sourceName(b.source), b.kind.c_str(), b.cx, b.cy, b.sx, b.sy, b.rot, b.z0, b.z1, b.cover ? "True" : "False",
                 slope.c_str());
}

bool same(const MassingBox& a, const MassingBox& b) {
    return a.cx == b.cx && a.cy == b.cy && a.sx == b.sx && a.sy == b.sy && a.z0 == b.z0 && a.z1 == b.z1;
}

} // namespace

int main(int argc, char** argv) {
    std::string boxesPath, streetsPath, r16Path, outPath;
    double samples = 0.0, origin[2] = {0.0, 0.0}, spacing = 1.0, area[4] = {0.0, 0.0, 0.0, 0.0}, upTo = 6.0;
    bool haveArea = false, dress = true;
    std::vector<MassingRect> clear;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--no-dress") {
            dress = false;
        } else if (!more) {
            return fail("'" + a + "' needs a value");
        } else if (a == "--boxes") {
            boxesPath = argv[++i];
        } else if (a == "--streets") {
            streetsPath = argv[++i];
        } else if (a == "--r16") {
            r16Path = argv[++i];
        } else if (a == "--out") {
            outPath = argv[++i];
        } else if (a == "--samples") {
            if (!parseNumbers(argv[++i], &samples, 1)) return fail("bad --samples");
        } else if (a == "--spacing") {
            if (!parseNumbers(argv[++i], &spacing, 1)) return fail("bad --spacing");
        } else if (a == "--lane-up-to") {
            if (!parseNumbers(argv[++i], &upTo, 1)) return fail("bad --lane-up-to");
        } else if (a == "--origin") {
            if (!parseNumbers(argv[++i], origin, 2)) return fail("bad --origin");
        } else if (a == "--area") {
            if (!parseNumbers(argv[++i], area, 4)) return fail("bad --area");
            haveArea = true;
        } else if (a == "--clear") {
            double c[4];
            if (!parseNumbers(argv[++i], c, 4)) return fail("bad --clear");
            clear.push_back({c[0], c[1], c[2], c[3]});
        } else {
            return fail("unknown argument '" + a + "'");
        }
    }
    if (boxesPath.empty() || streetsPath.empty() || r16Path.empty() || samples < 2.0 || !haveArea)
        return fail("needs --boxes, --streets, --r16, --samples and --area");

    std::string text, error;
    std::vector<MassingBox> boxes;
    if (!readFile(boxesPath, text)) return fail("can't read " + boxesPath);
    if (!importMassingBoxes(text, boxes, &error)) return fail(boxesPath + ": " + error);
    text.clear();
    std::vector<MassingStreet> streets;
    if (!readFile(streetsPath, text)) return fail("can't read " + streetsPath);
    if (!importMassingStreets(text, streets, &error)) return fail(streetsPath + ": " + error);
    text.clear();
    if (!readFile(r16Path, text)) return fail("can't read " + r16Path);
    const std::size_t n = static_cast<std::size_t>(samples);
    if (text.size() != n * n * 2) return fail("the r16 is not samples x samples u16");
    std::vector<std::uint16_t> grid(n * n);
    for (std::size_t k = 0; k < n * n; ++k)
        grid[k] = static_cast<std::uint16_t>(static_cast<std::uint8_t>(text[2 * k]) | (static_cast<std::uint8_t>(text[2 * k + 1]) << 8));
    const MassingGround ground{grid.data(), n, n, spacing, origin[0], origin[1]};

    const std::vector<MassingBox> before = boxes;
    MassingBuildTo opts;
    opts.area = {area[0], area[1], area[2], area[3]};
    opts.laneUpTo = upTo;
    opts.keepClear = clear;
    const MassingBuildToCounts c = massingBuildTo(boxes, streets, ground, opts);
    std::printf("build-to: %zu lane segments, %zu faces pulled, %zu left (something between), %zu cut out of a lane, %zu infill\n",
                c.lanes, c.pulled, c.blocked, c.trimmed, c.infill);

    std::vector<MassingBox> dressing;
    if (dress) {
        MassingLaneDress d;
        d.area = opts.area;
        d.laneUpTo = upTo;
        d.keepClear = clear;
        const MassingLaneDressCounts dc = massingLaneDress(streets, ground, d, dressing);
        std::printf("dressing: %zu gutter runs, %zu drains, %zu patches, %zu mouth bars, %zu left out over a step\n", dc.gutters,
                    dc.drains, dc.patches, dc.marks, dc.uneven);
    }

    for (const MassingStreet& s : streets) {
        if (!(s.width <= upTo)) continue;
        const MassingLaneWidth w = massingLaneWidth(s, boxes, opts.area, 12.0, 1.0);
        if (w.samples == 0) continue;
        std::printf("lane %s %.1f m: wall to wall %.2f..%.2f m over %zu of %zu samples\n", s.id.c_str(), s.width, w.minWall,
                    w.maxWall, w.bothSides, w.samples);
    }

    if (!outPath.empty()) {
        std::FILE* f = std::fopen(outPath.c_str(), "wb");
        if (!f) return fail("can't write " + outPath);
        std::fprintf(f, "op,id,row,src,kind,cx,cy,sx,sy,rot,z0,z1,cover,slope\n");
        std::size_t set = 0;
        for (std::size_t i = 0; i < before.size(); ++i)
            if (!same(before[i], boxes[i])) {
                writeRow(f, "set", boxes[i]);
                ++set;
            }
        for (std::size_t i = before.size(); i < boxes.size(); ++i) writeRow(f, "add", boxes[i]);
        for (const MassingBox& b : dressing) writeRow(f, "add", b);
        std::fclose(f);
        std::printf("wrote %zu moved rows and %zu new rows to %s\n", set, boxes.size() - before.size() + dressing.size(), outPath.c_str());
    }
    std::printf("LAYOUT OK\n");
    return 0;
}
