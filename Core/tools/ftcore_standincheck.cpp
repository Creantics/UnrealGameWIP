// ftcore_standincheck: the corridor rule (ftcore/StandIns.h, standInCorridorCheck) on the stand-in point tables and the
// street tables, offline: no stand-in part may stand in a street's or an alley's clear way. The massing build runs the
// same check when it places the stand-ins (FTBuildStandIns.h); this runs it before an Unreal process starts.
//
//   ftcore_standincheck --streets S[;S2...] [--trees T[;..]] [--houses H[;..]] [--towers W[;..]] [--points P[;..]]
//                       [--r16 H --samples N --origin X,Y --spacing M] [--skip-kind canal] [--clearance C] [--slack S]
//                       [--report R] [--list K]
//
// --streets: street and road tables (id, width_m, pts, kind, pavement_m: importStandInCorridors). --trees rows default
// to street trees, --houses to house roofs; --towers and --points need a kind column. With the height field a row
// without z stands on the ground and a row with one is taken that far over it; without it every row stands on the
// ground. Lists the K deepest hits (default 40). Exit 0 on STANDINCHECK OK, 1 on any hit, 2 on an error.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/MassingPlan.h" // massingGroundAt
#include "ftcore/StandIns.h"

#include <algorithm>
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

int die(const std::string& what) {
    std::fprintf(stderr, "ftcore_standincheck: error: %s\n", what.c_str());
    return 2;
}

std::string num(double v, int places = 2) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", places, v);
    return b;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    for (std::size_t p = 0; p <= s.size();) {
        const std::size_t q = s.find(';', p);
        const std::string one = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (!one.empty()) out.push_back(one);
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string streetsArg, tables[4], r16Path, reportPath, skipKind = "canal";
    const StandInKind defaults[4] = {StandInKind::StreetTree, StandInKind::House, StandInKind::Count, StandInKind::Count};
    double samples = 0.0, origin[2] = {0.0, 0.0}, spacing = 1.0;
    std::size_t list = 40;
    StandInCorridorSpec spec;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--streets") streetsArg = v;
        else if (k == "--trees") tables[0] = v;
        else if (k == "--houses") tables[1] = v;
        else if (k == "--towers") tables[2] = v;
        else if (k == "--points") tables[3] = v;
        else if (k == "--r16") r16Path = v;
        else if (k == "--samples") samples = std::atof(v.c_str());
        else if (k == "--origin") {
            const std::size_t comma = v.find(',');
            if (comma == std::string::npos) return die("--origin is X,Y");
            origin[0] = std::atof(v.substr(0, comma).c_str());
            origin[1] = std::atof(v.substr(comma + 1).c_str());
        } else if (k == "--spacing") spacing = std::atof(v.c_str());
        else if (k == "--skip-kind") skipKind = v;
        else if (k == "--clearance") spec.clearance = std::atof(v.c_str());
        else if (k == "--slack") spec.slack = std::atof(v.c_str());
        else if (k == "--report") reportPath = v;
        else if (k == "--list") list = static_cast<std::size_t>(std::atol(v.c_str()));
        else return die("unknown switch " + k);
    }
    if (streetsArg.empty()) return die("needs --streets");

    std::vector<StandInCorridor> corridors;
    std::vector<std::string> streetIds; // per corridor
    for (const std::string& path : split(streetsArg)) {
        std::string text, err;
        if (!readFile(path, text)) return die("can't read " + path);
        if (!importStandInCorridors(text, corridors, &streetIds, skipKind, &err)) return die(path + ": " + err);
    }

    std::vector<StandInPoint> points;
    for (int t = 0; t < 4; ++t) {
        for (const std::string& path : split(tables[t])) {
            std::string text, err;
            if (!readFile(path, text)) return die("can't read " + path);
            if (!importStandIns(text, defaults[t], points, &err)) return die(path + ": " + err);
        }
    }

    std::vector<std::uint16_t> grid;
    std::size_t n = 0;
    if (!r16Path.empty()) {
        std::string raw;
        if (!readFile(r16Path, raw)) return die("can't read " + r16Path);
        n = static_cast<std::size_t>(samples);
        if (n < 2 || raw.size() != n * n * 2) return die("the r16 is not samples x samples u16");
        grid.resize(n * n);
        for (std::size_t k = 0; k < n * n; ++k)
            grid[k] = static_cast<std::uint16_t>(static_cast<std::uint8_t>(raw[2 * k]) | (static_cast<std::uint8_t>(raw[2 * k + 1]) << 8));
    }
    std::vector<double> foot(points.size(), 0.0);
    if (!grid.empty()) {
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (points[i].hasZ) foot[i] = points[i].z - massingGroundAt(grid.data(), n, n, spacing, origin[0], origin[1], points[i].x, points[i].y);
        }
    }

    std::vector<StandInCorridorHit> hits(points.size() * kStandInPartsMax + 1);
    const std::size_t found =
        standInCorridorCheck(points.data(), foot.data(), points.size(), corridors.data(), corridors.size(), spec, hits.data(), hits.size());
    hits.resize(std::min(found, hits.size()));
    std::sort(hits.begin(), hits.end(), [](const StandInCorridorHit& a, const StandInCorridorHit& b) { return a.depth > b.depth; });

    std::string out = "standincheck: " + std::to_string(points.size()) + " points, " + std::to_string(corridors.size()) +
                      " corridor segments, clearance " + num(spec.clearance, 1) + " m, slack " + num(spec.slack) + " m: " +
                      std::to_string(found) + " parts in a corridor.\n";
    StandInPart parts[kStandInPartsMax];
    for (std::size_t h = 0; h < hits.size() && h < list; ++h) {
        const StandInPoint& p = points[hits[h].point];
        standInParts(p, parts);
        const StandInBounds b = standInBounds(p, parts[hits[h].part]);
        out += "  " + std::string(standInKindName(p.kind)) + " " + (p.id.empty() ? std::string("(no id)") : p.id) + " part " +
               std::to_string(hits[h].part) + " at " + num(b.cx, 1) + "," + num(b.cy, 1) + " (" + num(2.0 * b.hx, 1) + " x " +
               num(2.0 * b.hy, 1) + " m): " + num(hits[h].depth) + " m into " + streetIds[hits[h].corridor] + "\n";
    }
    out += found == 0 ? "STANDINCHECK OK\n" : "STANDINCHECK FAILED\n";
    std::fputs(out.c_str(), stdout);
    if (!reportPath.empty()) {
        std::FILE* f = std::fopen(reportPath.c_str(), "wb");
        if (!f) return die("can't write " + reportPath);
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    }
    return found == 0 ? 0 : 1;
}
