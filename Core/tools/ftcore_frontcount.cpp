// ftcore_frontcount: counts what a frontage table places on a map's boxes, offline, as the map's builder would (an
// estimate for a table's budget, not the game).
//
//   ftcore_frontcount <boxes.csv> <prop_streets.csv> <frontage.csv> <ground.r16> <samplesX> <samplesY> <spacing>
//                     <originX> <originY> [kit.csv]
//
// The hosts are as the builder's: boxes that are seen, not stood in for by a kit row (the kit table's "box" column), not
// sloped and wearing a building finish. Box tops are taken as given (the builder nudges some up a few cm), and the room
// the kerbs, quays and roads take is not counted, so the counts are an upper bound. Prints one line per rule (placed,
// faces, capped) and the paving pads.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool readFile(const char* path, std::string& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    out.clear();
    char buf[1 << 16];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    std::fclose(f);
    return true;
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 10) {
        std::fprintf(stderr, "usage: ftcore_frontcount <boxes.csv> <prop_streets.csv> <frontage.csv> <ground.r16> <samplesX> "
                             "<samplesY> <spacing> <originX> <originY> [kit.csv]\n");
        return 2;
    }
    std::string boxesCsv, streetsCsv, frontCsv, groundRaw, kitCsv, error;
    if (!readFile(argv[1], boxesCsv) || !readFile(argv[2], streetsCsv) || !readFile(argv[3], frontCsv) ||
        !readFile(argv[4], groundRaw) || (argc > 10 && !readFile(argv[10], kitCsv))) {
        std::fprintf(stderr, "ftcore_frontcount: can't read an input file\n");
        return 1;
    }
    std::vector<ftcore::MassingBox> boxes;
    std::vector<ftcore::MassingStreet> streets;
    std::vector<ftcore::MassingFrontRule> rules;
    if (!ftcore::importMassingBoxes(boxesCsv, boxes, &error) || !ftcore::importMassingStreets(streetsCsv, streets, &error) ||
        !ftcore::importMassingFront(frontCsv, rules, &error)) {
        std::fprintf(stderr, "ftcore_frontcount: %s\n", error.c_str());
        return 1;
    }
    const std::size_t sx = std::strtoul(argv[5], nullptr, 10), sy = std::strtoul(argv[6], nullptr, 10);
    if (groundRaw.size() != sx * sy * 2) {
        std::fprintf(stderr, "ftcore_frontcount: the ground holds %zu bytes, not %zu x %zu samples\n", groundRaw.size(), sx, sy);
        return 1;
    }
    std::vector<std::uint16_t> grid(sx * sy);
    for (std::size_t i = 0; i < grid.size(); ++i)
        grid[i] = static_cast<std::uint16_t>(static_cast<unsigned char>(groundRaw[2 * i]) |
                                             (static_cast<unsigned char>(groundRaw[2 * i + 1]) << 8));
    const ftcore::MassingGround ground{grid.data(), sx, sy, std::strtod(argv[7], nullptr), std::strtod(argv[8], nullptr),
                                       std::strtod(argv[9], nullptr)};

    std::vector<bool> replaced(boxes.size(), false);
    if (!kitCsv.empty()) {
        const std::vector<std::string> ids = kitBoxes(kitCsv);
        for (std::size_t i = 0; i < boxes.size(); ++i)
            for (const std::string& id : ids)
                if (boxes[i].id == id) replaced[i] = true;
    }
    std::vector<bool> visible(boxes.size(), false), hosts(boxes.size(), false);
    std::size_t hostCount = 0;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        visible[i] = ftcore::massingVisible(ftcore::massingRole(boxes[i]));
        hosts[i] = visible[i] && !replaced[i] && !boxes[i].sloped &&
                   ftcore::massingLookIsBuilding(ftcore::massingLookSurface(boxes[i]));
        hostCount += hosts[i] ? 1 : 0;
    }
    std::vector<ftcore::MassingProp> pieces;
    std::vector<ftcore::MassingFrontCounts> counts;
    ftcore::MassingFrontTotals totals;
    std::vector<ftcore::MassingBox> pads;
    ftcore::massingFrontage(rules, streets, ground, boxes, visible, hosts, 0.5, nullptr, pieces, counts, totals, &pads);
    std::printf("boxes %zu, hosts %zu, faces %zu, street faces %zu (%zu dressed)\n", boxes.size(), hostCount, totals.faces,
                totals.streetFaces, totals.streetDressed);
    std::vector<std::size_t> kept(rules.size(), 0);
    for (const ftcore::MassingProp& p : pieces) kept[p.rule < kept.size() ? p.rule : 0] += 1;
    for (std::size_t r = 0; r < rules.size(); ++r)
        std::printf("%s: %zu kept on %zu faces (placed %zu, capped %zu, tried %zu, in road %zu, blocked %zu, crowded %zu)\n",
                    rules[r].id.c_str(), kept[r], counts[r].faces, counts[r].placed, counts[r].capped, counts[r].tried,
                    counts[r].inRoad, counts[r].blocked, counts[r].crowded);
    std::printf("pieces %zu, paving pads %zu (%zu left out uneven)\n", pieces.size(), pads.size(), totals.padsUneven);
    return 0;
}
