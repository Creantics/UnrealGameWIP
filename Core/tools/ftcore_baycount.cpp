// ftcore_baycount: counts the ground-floor shop bays facing each street of a massing map, offline (a planning aid for
// modelled shop bays; the rule is in ftcore/BayCount.h).
//
//   ftcore_baycount --boxes B --streets S [--prop-streets PS] --frontage F --r16 H --samples N --origin X,Y
//                   [--spacing M] [--kit K] [--reach R] [--step M] [--top N] [--csv OUT]
//
// B boxes, S the street table (ids, kinds, pavement_m), PS the table the build's frontage reads (default S), F the
// frontage rules, H the raw u16 height field (N x N samples), K the kit table (its "box" column: boxes a kit mesh stands
// in for). Prints the top streets by bay count (default 25), then the totals for all streets, for walked ones (pavement_m
// above 0) and the densest sweep point; --csv writes every street. Exit 0, 1 on an input error, 2 on bad arguments.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/BayCount.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
    std::fprintf(stderr, "ftcore_baycount: %s\n", why.c_str());
    return 1;
}

bool parsePair(const std::string& s, double out[2]) {
    const std::size_t comma = s.find(',');
    if (comma == std::string::npos) return false;
    char* end = nullptr;
    out[0] = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    const std::string rest = s.substr(comma + 1);
    out[1] = std::strtod(rest.c_str(), &end);
    return end != rest.c_str();
}

// The ids in a CSV's "box" column (comment rows start with '#').
std::vector<std::string> kitBoxes(const std::string& csv) {
    std::vector<std::string> ids;
    long column = -1;
    std::size_t at = 0;
    while (at < csv.size()) {
        std::size_t end = csv.find('\n', at);
        if (end == std::string::npos) end = csv.size();
        std::string line = csv.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cells(1);
        for (char c : line) {
            if (c == ',') cells.emplace_back();
            else cells.back() += c;
        }
        if (column < 0) {
            for (std::size_t i = 0; i < cells.size(); ++i)
                if (cells[i] == "box") column = static_cast<long>(i);
            if (column < 0) return ids;
            continue;
        }
        if (static_cast<std::size_t>(column) < cells.size()) ids.push_back(cells[static_cast<std::size_t>(column)]);
    }
    return ids;
}

void printTotals(const char* name, const BayTotals& t) {
    std::printf("%-7s %5zu streets (%zu with bays), %8.1f km, %6zu faces, %7zu bays, %7zu lit, %7zu spill lights\n", name,
                t.streets, t.withBays, t.length / 1000.0, t.faces, t.bays, t.litBays, t.lights);
}

}  // namespace

int main(int argc, char** argv) {
    std::string boxesPath, streetsPath, propPath, frontPath, r16Path, kitPath, csvPath;
    double samples = 0.0, origin[2] = {0.0, 0.0}, spacing = 1.0;
    std::size_t top = 25;
    BayCountConfig config;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--boxes") boxesPath = v;
        else if (k == "--streets") streetsPath = v;
        else if (k == "--prop-streets") propPath = v;
        else if (k == "--frontage") frontPath = v;
        else if (k == "--r16") r16Path = v;
        else if (k == "--samples") samples = std::atof(v.c_str());
        else if (k == "--origin") {
            if (!parsePair(v, origin)) return die("--origin is X,Y");
        } else if (k == "--spacing") spacing = std::atof(v.c_str());
        else if (k == "--kit") kitPath = v;
        else if (k == "--reach") config.reach = std::atof(v.c_str());
        else if (k == "--step") config.step = std::atof(v.c_str());
        else if (k == "--top") top = static_cast<std::size_t>(std::atol(v.c_str()));
        else if (k == "--csv") csvPath = v;
        else {
            std::fprintf(stderr, "ftcore_baycount: unknown switch %s\n", k.c_str());
            return 2;
        }
    }
    if (boxesPath.empty() || streetsPath.empty() || frontPath.empty() || r16Path.empty() || samples < 2.0) {
        std::fprintf(stderr, "usage: ftcore_baycount --boxes B --streets S [--prop-streets PS] --frontage F --r16 H --samples N "
                             "--origin X,Y [--spacing M] [--kit K] [--reach R] [--step M] [--top N] [--csv OUT]\n");
        return 2;
    }
    std::string boxesText, streetsText, propText, frontText, raw, kitText, error;
    if (!readFile(boxesPath, boxesText)) return die("can't read " + boxesPath);
    if (!readFile(streetsPath, streetsText)) return die("can't read " + streetsPath);
    if (!propPath.empty() && !readFile(propPath, propText)) return die("can't read " + propPath);
    if (!readFile(frontPath, frontText)) return die("can't read " + frontPath);
    if (!readFile(r16Path, raw)) return die("can't read " + r16Path);
    if (!kitPath.empty() && !readFile(kitPath, kitText)) return die("can't read " + kitPath);

    std::vector<MassingBox> boxes;
    std::vector<MassingStreet> streets, propStreets;
    std::vector<MassingFrontRule> rules;
    if (!importMassingBoxes(boxesText, boxes, &error)) return die(boxesPath + ": " + error);
    if (!importMassingStreets(streetsText, streets, &error)) return die(streetsPath + ": " + error);
    if (propPath.empty()) propStreets = streets;
    else if (!importMassingStreets(propText, propStreets, &error)) return die(propPath + ": " + error);
    if (!importMassingFront(frontText, rules, &error)) return die(frontPath + ": " + error);

    const std::size_t n = static_cast<std::size_t>(samples);
    if (raw.size() != n * n * 2) return die("the r16 is not samples x samples u16");
    std::vector<std::uint16_t> grid(n * n);
    for (std::size_t k = 0; k < grid.size(); ++k)
        grid[k] = static_cast<std::uint16_t>(static_cast<std::uint8_t>(raw[2 * k]) |
                                             (static_cast<std::uint8_t>(raw[2 * k + 1]) << 8));
    const MassingGround ground{grid.data(), n, n, spacing, origin[0], origin[1]};

    std::vector<bool> replaced(boxes.size(), false);
    if (!kitText.empty()) {
        const std::vector<std::string> ids = kitBoxes(kitText);
        for (std::size_t i = 0; i < boxes.size(); ++i)
            for (const std::string& id : ids)
                if (boxes[i].id == id) replaced[i] = true;
    }

    BayCountResult r;
    if (!bayCount(boxes, replaced, streets, propStreets, rules, ground, config, r, &error)) return die(error);

    std::printf("boxes %zu, hosts %zu, windowed %zu, street faces %zu (%zu with no bay, %zu unattributed)\n", r.boxes, r.hosts,
                r.windowed, r.streetFaces, r.emptyFaces, r.unattributed);
    std::vector<const BayStreet*> order;
    for (const BayStreet& s : r.streets) order.push_back(&s);
    std::stable_sort(order.begin(), order.end(), [](const BayStreet* a, const BayStreet* b) { return a->bays > b->bays; });
    std::printf("\ntop %zu streets by bays\n%-10s %-12s %8s %6s %5s %6s %6s %6s %6s\n", std::min(top, order.size()), "id", "kind",
                "length", "width", "pave", "faces", "bays", "lit", "lights");
    for (std::size_t k = 0; k < order.size() && k < top; ++k) {
        const BayStreet& s = *order[k];
        std::printf("%-10s %-12s %8.1f %6.1f %5.1f %6zu %6zu %6zu %6zu\n", s.id.c_str(), s.kind.c_str(), s.length, s.width,
                    s.pavement, s.faces, s.bays, s.litBays, s.lights);
    }
    std::printf("\n");
    printTotals("all", r.all);
    printTotals("walked", r.walked);
    if (r.worst.found)
        std::printf("densest: %zu bays within %.0f m of %s at (%.1f, %.1f) (sweep every %.1f m along walked centrelines)\n",
                    r.worst.bays, r.reach, r.worst.street.c_str(), r.worst.x, r.worst.y, config.step);
    else
        std::printf("densest: no walked street to sweep\n");

    if (!csvPath.empty()) {
        std::string csv = "id,kind,length_m,width_m,pavement_m,walked,faces,bays,lit_bays,lights\n";
        char buf[256];
        for (const BayStreet& s : r.streets) {
            std::snprintf(buf, sizeof buf, ",%s,%.1f,%.1f,%.2f,%d,%zu,%zu,%zu,%zu\n", s.kind.c_str(), s.length, s.width,
                          s.pavement, s.pavement > 0.0 ? 1 : 0, s.faces, s.bays, s.litBays, s.lights);
            csv += s.id;
            csv += buf;
        }
        std::FILE* f = std::fopen(csvPath.c_str(), "wb");
        if (!f) return die("can't write " + csvPath);
        std::fwrite(csv.data(), 1, csv.size(), f);
        std::fclose(f);
        std::printf("wrote %s (%zu streets)\n", csvPath.c_str(), r.streets.size());
    }
    return 0;
}
