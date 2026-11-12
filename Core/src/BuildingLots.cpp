#include "ftcore/BuildingLots.h"

#include "ftcore/BlockGen.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '"')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '"')) s.remove_suffix(1);
    return s;
}

// Splits on commas outside double quotes.
std::vector<std::string_view> cells(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    bool quoted = false;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i < line.size() && line[i] == '"') quoted = !quoted;
        if (i == line.size() || (line[i] == ',' && !quoted)) {
            out.push_back(trimmed(line.substr(start, i - start)));
            start = i + 1;
        }
    }
    return out;
}

bool number(std::string_view s, double& out) {
    if (s.empty()) return false;
    const std::string text(s);
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size() && std::isfinite(out);
}

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

bool triple(std::string_view s, double& a, double& b, double& c) {
    const std::size_t p = s.find('/');
    if (p == std::string_view::npos) return false;
    const std::size_t q = s.find('/', p + 1);
    if (q == std::string_view::npos) return false;
    return number(trimmed(s.substr(0, p)), a) && number(trimmed(s.substr(p + 1, q - p - 1)), b) &&
           number(trimmed(s.substr(q + 1)), c);
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at <= s.size()) {
        const std::size_t p = s.find(sep, at);
        const std::string_view part = trimmed(s.substr(at, p == std::string_view::npos ? std::string_view::npos : p - at));
        if (!part.empty()) out.push_back(part);
        if (p == std::string_view::npos) break;
        at = p + 1;
    }
    return out;
}

// Reads a CSV with a header row; calls row(cellsByColumn, lineNo) for each data row. Returns false on a missing column.
template <std::size_t N, typename Row>
bool readCsv(std::string_view csv, const std::array<std::string_view, N>& names, std::size_t required, Row&& row,
             std::string* error) {
    std::array<int, N> col{};
    col.fill(-1);
    bool header = false;
    std::size_t lineNo = 0, at = 0;
    while (at <= csv.size()) {
        const std::size_t nl = csv.find('\n', at);
        const std::string_view raw = csv.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? csv.size() + 1 : nl + 1;
        ++lineNo;
        std::string_view line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.remove_prefix(3); // a BOM
        if (line.empty() || line.front() == '#') continue;
        const std::vector<std::string_view> c = cells(line);
        if (!header) {
            for (std::size_t k = 0; k < N; ++k)
                for (std::size_t i = 0; i < c.size(); ++i)
                    if (c[i] == names[k]) col[k] = static_cast<int>(i);
            for (std::size_t k = 0; k < required; ++k)
                if (col[k] < 0) return fail(error, "the header has no column " + std::string(names[k]));
            header = true;
            continue;
        }
        std::array<std::string_view, N> v{};
        for (std::size_t k = 0; k < N; ++k)
            if (col[k] >= 0 && static_cast<std::size_t>(col[k]) < c.size()) v[k] = c[static_cast<std::size_t>(col[k])];
        if (!row(v, lineNo)) return false;
    }
    if (!header) return fail(error, "no header row");
    return true;
}

constexpr KitRgb kWall[] = {
    {0.86f, 0.84f, 0.79f}, // off-white
    {0.66f, 0.63f, 0.58f}, // warm grey
    {0.30f, 0.30f, 0.31f}, // charcoal tile
    {0.52f, 0.62f, 0.55f}, // tile green
    {0.55f, 0.62f, 0.70f}, // tile blue
    {0.80f, 0.72f, 0.60f}, // beige
    {0.62f, 0.50f, 0.42f}, // weathered brown
    {0.90f, 0.86f, 0.74f}, // cream
    {0.48f, 0.50f, 0.52f}, // slate
};
constexpr KitRgb kTrim[] = {{0.86f, 0.85f, 0.82f}, {0.75f, 0.76f, 0.76f}, {0.30f, 0.30f, 0.31f}, {0.62f, 0.60f, 0.56f}};
constexpr KitRgb kTimber[] = {{0.42f, 0.30f, 0.22f}, {0.60f, 0.52f, 0.44f}, {0.22f, 0.19f, 0.17f}};
constexpr KitRgb kRoof[] = {{0.42f, 0.45f, 0.52f}, {0.36f, 0.38f, 0.44f}, {0.30f, 0.31f, 0.33f}};
constexpr KitRgb kAccent[] = {{0.72f, 0.12f, 0.10f}, {0.92f, 0.72f, 0.12f}, {0.12f, 0.28f, 0.62f},
                              {0.15f, 0.48f, 0.30f}, {0.92f, 0.92f, 0.90f}, {0.88f, 0.42f, 0.12f}};
constexpr KitRgb kSignFg[] = {{0.95f, 0.95f, 0.92f}, {0.98f, 0.85f, 0.25f}, {0.85f, 0.15f, 0.10f}, {0.08f, 0.08f, 0.09f}};

// Wall colours each surface may wear (indices into kWall).
constexpr std::uint8_t kTileWalls[] = {0, 3, 4, 5, 2};
constexpr std::uint8_t kConcreteWalls[] = {0, 1, 8, 2, 7};
constexpr std::uint8_t kStuccoWalls[] = {7, 5, 0, 6};

struct Parcel {
    double s0 = 0.0, s1 = 0.0, depth = 0.0;
    std::vector<std::size_t> boxes;
};

constexpr int kWidths[5][7] = {
    {7, 8, 11, 0, 0, 0, 0},   // corner
    {5, 6, 0, 0, 0, 0, 0},    // pencil
    {6, 7, 8, 9, 10, 11, 12}, // walk-up
    {6, 8, 9, 10, 12, 0, 0},  // tiled
    {6, 7, 8, 0, 0, 0, 0},    // town house
};

bool widthLegal(LotType t, int units) {
    for (int w : kWidths[static_cast<int>(t)])
        if (w == units) return true;
    return false;
}

std::uint32_t mix(std::uint32_t a, std::uint32_t b) {
    std::uint32_t h = a ^ (b * 0x9E3779B9u);
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

int pick(BlockRng& rng, int n) { return n <= 1 ? 0 : static_cast<int>(rng.next() % static_cast<std::uint32_t>(n)); }

// Every trait but the type, width and place, from one seed.
void rollTraits(BuildingLot& lot, std::uint32_t seed) {
    BlockRng rng(seed);
    lot.seed = seed;
    const int base = static_cast<int>(std::lround((lot.oldTop - lot.floorZ - kShopStorey) / kUpperStorey)) + 1;
    int lo = 3, hi = 5;
    switch (lot.type) {
    case LotType::Corner: lo = 3; hi = 5; break;
    case LotType::Pencil: lo = 4; hi = 6; break;
    case LotType::WalkUp: lo = 3; hi = 5; break;
    case LotType::Tiled: lo = 3; hi = 6; break;
    case LotType::TownHouse: lo = 2; hi = 2; break;
    }
    lot.floors = std::clamp(base + pick(rng, 3) - 1 + (lot.type == LotType::Pencil ? 1 : 0), lo, hi);
    lot.setback = lot.type == LotType::Corner ? 0.0 : 0.15 * pick(rng, 4);
    lot.roof = lot.type == LotType::TownHouse ? (pick(rng, 2) ? RoofKind::Hip : RoofKind::Gable) : RoofKind::Flat;
    switch (lot.type) {
    case LotType::Tiled: lot.wall = WallSurface::Tile; break;
    case LotType::WalkUp: lot.wall = WallSurface::Concrete; break;
    case LotType::TownHouse: lot.wall = WallSurface::Stucco; break;
    default: lot.wall = pick(rng, 2) ? WallSurface::Tile : WallSurface::Concrete; break;
    }
    if (lot.wall == WallSurface::Tile) lot.wallColour = kTileWalls[pick(rng, 5)];
    else if (lot.wall == WallSurface::Concrete) lot.wallColour = kConcreteWalls[pick(rng, 5)];
    else lot.wallColour = kStuccoWalls[pick(rng, 4)];
    lot.trimColour = static_cast<std::uint8_t>(pick(rng, 4));
    lot.accent = static_cast<std::uint8_t>(pick(rng, 6));
    lot.timber = static_cast<std::uint8_t>(pick(rng, 3));
    lot.roofColour = static_cast<std::uint8_t>(pick(rng, 3));
    lot.signColour = static_cast<std::uint8_t>(pick(rng, 4));
    lot.groundVariant = static_cast<std::uint8_t>(pick(rng, 3));
    lot.upperVariant = static_cast<std::uint8_t>(pick(rng, 3));
    lot.signSet = static_cast<std::uint8_t>(pick(rng, 4));
    lot.clutterSeed = rng.next();
}

LotType pickType(BlockRng& rng, int units, bool lastOnly, LotType prev, LotType prev2, bool hasPrev, bool hasPrev2) {
    static constexpr LotType kinds[4] = {LotType::Pencil, LotType::WalkUp, LotType::Tiled, LotType::TownHouse};
    static constexpr double weight[4] = {3.0, 3.0, 3.0, 2.0};
    double w[4] = {};
    double total = 0.0;
    for (int k = 0; k < 4; ++k) {
        bool any = false;
        for (int u : kWidths[static_cast<int>(kinds[k])])
            if (u > 0 && u <= units && (u == units || (!lastOnly && units - u >= 5))) any = true;
        if (!any) continue;
        w[k] = weight[k] * (hasPrev && kinds[k] == prev ? 0.15 : 1.0) * (hasPrev2 && kinds[k] == prev2 ? 0.5 : 1.0);
        total += w[k];
    }
    double r = rng.unit() * total;
    for (int k = 0; k < 4; ++k) {
        if (w[k] <= 0.0) continue;
        if (r < w[k]) return kinds[k];
        r -= w[k];
    }
    for (int k = 3; k >= 0; --k)
        if (w[k] > 0.0) return kinds[k];
    return LotType::WalkUp;
}

int pickWidth(BlockRng& rng, LotType t, int units) {
    int legal[7] = {};
    int n = 0;
    for (int u : kWidths[static_cast<int>(t)])
        if (u > 0 && u <= units && (u == units || units - u >= 5)) legal[n++] = u;
    for (int k = 0; k < n; ++k)
        if (legal[k] == units) return units; // finishing the parcel exactly is always taken when legal
    return n ? legal[pick(rng, n)] : 0;
}

} // namespace

const KitSnap* KitPiece::snap(std::string_view name) const {
    for (const KitSnap& s : snaps)
        if (s.name == name) return &s;
    return nullptr;
}

int KitTable::find(std::string_view id) const {
    for (std::size_t i = 0; i < pieces.size(); ++i)
        if (pieces[i].id == id) return static_cast<int>(i);
    return -1;
}

bool parseKitTable(std::string_view csv, KitTable& out, std::string* error) {
    out = {};
    static constexpr std::array<std::string_view, 9> names = {"id", "size_x", "size_y", "size_z", "min",
                                                               "group", "triangles", "slots", "snaps"};
    return readCsv(csv, names, 5, [&](const std::array<std::string_view, 9>& v, std::size_t lineNo) {
        const std::string where = "line " + std::to_string(lineNo);
        KitPiece p;
        p.id = std::string(v[0]);
        if (p.id.empty()) return fail(error, where + ": no id");
        if (!number(v[1], p.sx) || !number(v[2], p.sy) || !number(v[3], p.sz) || p.sx <= 0.0 || p.sy <= 0.0 || p.sz <= 0.0)
            return fail(error, where + ": bad size");
        if (!triple(v[4], p.minX, p.minY, p.minZ)) return fail(error, where + ": bad min");
        p.group = std::string(v[5]);
        double tris = 0.0;
        if (!v[6].empty() && number(v[6], tris)) p.triangles = static_cast<int>(tris);
        for (std::string_view s : split(v[7], ';')) p.slots.emplace_back(s);
        for (std::string_view s : split(v[8], ';')) {
            const std::size_t colon = s.find(':');
            KitSnap k;
            if (colon == std::string_view::npos || !triple(s.substr(colon + 1), k.x, k.y, k.z))
                return fail(error, where + ": bad snap '" + std::string(s) + "'");
            k.name = std::string(s.substr(0, colon));
            p.snaps.push_back(std::move(k));
        }
        if (out.find(p.id) >= 0) return fail(error, where + ": a second " + p.id);
        out.pieces.push_back(std::move(p));
        return true;
    }, error);
}

double FrontageRun::length() const { return std::hypot(bx - ax, by - ay); }

double FrontageRun::yaw() const { return std::atan2(by - ay, bx - ax) * 180.0 / kPi; }

void FrontageRun::toPlan(double s, double y, double& px, double& py) const {
    const double len = length();
    const double dx = len > 0.0 ? (bx - ax) / len : 1.0;
    const double dy = len > 0.0 ? (by - ay) / len : 0.0;
    px = ax + dx * s - dy * y;
    py = ay + dy * s + dx * y;
}

bool parseFrontageRuns(std::string_view csv, std::vector<FrontageRun>& out, std::string* error) {
    out.clear();
    static constexpr std::array<std::string_view, 7> names = {"id", "ax", "ay", "bx", "by", "max_depth", "corner"};
    return readCsv(csv, names, 5, [&](const std::array<std::string_view, 7>& v, std::size_t lineNo) {
        const std::string where = "line " + std::to_string(lineNo);
        FrontageRun r;
        r.id = std::string(v[0]);
        if (r.id.empty()) return fail(error, where + ": no id");
        if (!number(v[1], r.ax) || !number(v[2], r.ay) || !number(v[3], r.bx) || !number(v[4], r.by))
            return fail(error, where + ": bad end points");
        if (r.length() < 3.0) return fail(error, where + ": a run shorter than 3 m");
        if (!v[5].empty() && (!number(v[5], r.maxDepth) || r.maxDepth < 3.0)) return fail(error, where + ": bad max_depth");
        r.cornerAtA = v[6] == "1" || v[6] == "true" || v[6] == "True";
        out.push_back(std::move(r));
        return true;
    }, error);
}

const char* lotTypeName(LotType t) {
    switch (t) {
    case LotType::Corner: return "corner";
    case LotType::Pencil: return "pencil";
    case LotType::WalkUp: return "walkup";
    case LotType::Tiled: return "tiled";
    case LotType::TownHouse: return "townhouse";
    }
    return "?";
}

int BuildingLot::units() const { return static_cast<int>(std::lround(width() / kLotGrid)); }

std::uint32_t lotSignature(const BuildingLot& lot) {
    const unsigned char b[] = {
        static_cast<unsigned char>(lot.type),        static_cast<unsigned char>(lot.floors),
        static_cast<unsigned char>(lot.roof),        static_cast<unsigned char>(lot.wall),
        lot.wallColour,                              lot.trimColour,
        lot.groundVariant,                           lot.upperVariant,
        lot.signSet,                                 static_cast<unsigned char>(lot.units()),
        static_cast<unsigned char>(std::lround(lot.setback / 0.15)),
    };
    return blockFnv1a(std::string_view(reinterpret_cast<const char*>(b), sizeof b));
}

int lotTraitDifferences(const BuildingLot& a, const BuildingLot& b) {
    return (a.type != b.type) + (a.floors != b.floors) + (a.roof != b.roof) + (a.wall != b.wall) +
           (a.wallColour != b.wallColour) + (a.groundVariant != b.groundVariant) + (a.upperVariant != b.upperVariant) +
           (a.signSet != b.signSet) + (std::lround(a.setback / 0.15) != std::lround(b.setback / 0.15));
}

namespace {

// Faults found; blame lists, per lot, the rules it should be re-rolled for (1: traits, 2: type too).
FrontageCheck checkWithBlame(const std::vector<BuildingLot>& lots, std::vector<int>* blame) {
    FrontageCheck c;
    if (blame) blame->assign(lots.size(), 0);
    auto fault = [&](const std::string& why, std::size_t lot, int level) {
        if (c.firstFault.empty()) c.firstFault = why;
        c.ok = false;
        if (blame) (*blame)[lot] = std::max((*blame)[lot], level);
    };
    const std::size_t n = lots.size();
    for (std::size_t i = 0; i < n; ++i) {
        const double ci = 0.5 * (lots[i].s0 + lots[i].s1);
        const std::uint32_t si = lotSignature(lots[i]);
        for (std::size_t j = i + 1; j < n && lots[j].run == lots[i].run; ++j) {
            const double cj = 0.5 * (lots[j].s0 + lots[j].s1);
            if (std::fabs(cj - ci) < 40.0 && lotSignature(lots[j]) == si) {
                ++c.equalWithin40;
                fault("lots " + std::to_string(i) + " and " + std::to_string(j) + " share a facade within 40 m", j, 1);
            }
        }
        if (i + 1 < n && lots[i + 1].run == lots[i].run && lotTraitDifferences(lots[i], lots[i + 1]) < 2) {
            ++c.neighbourTwins;
            fault("lots " + std::to_string(i) + " and " + std::to_string(i + 1) + " differ in fewer than two traits", i + 1, 1);
        }
        if (i + 2 < n && lots[i + 2].run == lots[i].run && lots[i + 1].wallColour == lots[i].wallColour &&
            lots[i + 2].wallColour == lots[i].wallColour) {
            ++c.colourTriples;
            fault("lots " + std::to_string(i) + " to " + std::to_string(i + 2) + " share one wall colour", i + 2, 1);
        }
        // A 60 m window from this lot's start, when the run still has 60 m; a run shorter than 60 m is one window
        // that needs min(3, lots) types.
        std::size_t runEnd = i;
        while (runEnd + 1 < n && lots[runEnd + 1].run == lots[i].run) ++runEnd;
        const bool firstOfRun = i == 0 || lots[i - 1].run != lots[i].run;
        std::size_t runStart = i;
        while (runStart > 0 && lots[runStart - 1].run == lots[i].run) --runStart;
        const double runSpan = lots[runEnd].s1 - lots[runStart].s0;
        const bool full = lots[runEnd].s1 - lots[i].s0 >= 60.0;
        if (!full && !(firstOfRun && runSpan < 60.0)) continue;
        bool seen[5] = {};
        int types = 0, count = 0;
        std::size_t last = i;
        for (std::size_t j = i; j <= runEnd && lots[j].s0 < lots[i].s0 + 60.0; ++j) {
            ++count;
            last = j;
            if (!seen[static_cast<int>(lots[j].type)]) {
                seen[static_cast<int>(lots[j].type)] = true;
                ++types;
            }
        }
        if (types < std::min(3, count)) {
            ++c.poorWindows;
            // Blame a non-corner lot in the window whose type repeats, last first.
            std::size_t victim = last;
            for (std::size_t j = last + 1; j-- > i;) {
                int same = 0;
                for (std::size_t k = i; k <= last; ++k) same += lots[k].type == lots[j].type;
                if (same > 1 && lots[j].type != LotType::Corner) {
                    victim = j;
                    break;
                }
            }
            fault("the 60 m window from lot " + std::to_string(i) + " has " + std::to_string(types) + " types", victim, 2);
        }
    }
    return c;
}

} // namespace

FrontageCheck checkFrontage(const std::vector<BuildingLot>& lots) { return checkWithBlame(lots, nullptr); }

void planFrontage(const std::vector<FrontageRun>& runs, const std::vector<MassingBox>& boxes, FrontagePlan& out) {
    out = {};
    out.runs = runs;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        const FrontageRun& run = runs[r];
        const double len = run.length();
        const double dx = (run.bx - run.ax) / len, dy = (run.by - run.ay) / len;
        std::vector<Parcel> parcels;
        for (std::size_t b = 0; b < boxes.size(); ++b) {
            const MassingBox& box = boxes[b];
            const MassingRole role = massingRole(box);
            if (role == MassingRole::Zone || role == MassingRole::Hidden || role == MassingRole::Open || box.sloped) continue;
            double turn = std::fmod(std::fabs(box.rot - run.yaw()), 90.0);
            if (std::min(turn, 90.0 - turn) > 2.0) continue;
            std::array<MassingVec, 8> c{};
            massingCorners(box, 0.0, c);
            double smin = 1e9, smax = -1e9, tmin = 1e9, tmax = -1e9;
            for (int k = 0; k < 4; ++k) {
                const double px = c[static_cast<std::size_t>(k)].x - run.ax, py = c[static_cast<std::size_t>(k)].y - run.ay;
                const double s = px * dx + py * dy, t = -px * dy + py * dx;
                smin = std::min(smin, s);
                smax = std::max(smax, s);
                tmin = std::min(tmin, t);
                tmax = std::max(tmax, t);
            }
            if (std::fabs(tmin) > 0.4 || tmax - tmin < 2.0) continue;
            const double s0 = std::max(smin, 0.0), s1 = std::min(smax, len);
            if (s1 - s0 < 0.5) continue;
            Parcel p;
            p.s0 = s0;
            p.s1 = s1;
            p.depth = std::min(tmax, run.maxDepth);
            p.boxes.push_back(b);
            parcels.push_back(std::move(p));
        }
        std::sort(parcels.begin(), parcels.end(), [](const Parcel& a, const Parcel& b) { return a.s0 < b.s0; });
        std::vector<Parcel> merged;
        for (Parcel& p : parcels) {
            if (!merged.empty() && p.s0 <= merged.back().s1 + 0.3) {
                Parcel& m = merged.back();
                m.s1 = std::max(m.s1, p.s1);
                m.depth = std::min(m.depth, p.depth);
                m.boxes.insert(m.boxes.end(), p.boxes.begin(), p.boxes.end());
            } else {
                merged.push_back(std::move(p));
            }
        }
        std::uint32_t lotNo = 0;
        for (const Parcel& p : merged) {
            for (std::size_t b : p.boxes) out.replaced.push_back(boxes[b].id);
            BlockRng rng(blockSeed(run.id, static_cast<std::uint32_t>(&p - merged.data())));
            int units = static_cast<int>(std::floor((p.s1 - p.s0) / kLotGrid + 1e-6));
            std::vector<std::pair<LotType, int>> plan;
            bool corner = run.cornerAtA && &p == merged.data() && p.s0 < 0.5;
            while (units >= 5 || (corner && units >= 7)) {
                LotType t;
                int w = 0;
                if (corner) {
                    t = LotType::Corner;
                    w = pickWidth(rng, t, units);
                    if (w == 0) w = std::min(units, 7);
                    corner = false;
                } else {
                    const bool hasPrev = !plan.empty(), hasPrev2 = plan.size() > 1;
                    t = pickType(rng, units, false, hasPrev ? plan.back().first : LotType::Corner,
                                 hasPrev2 ? plan[plan.size() - 2].first : LotType::Corner, hasPrev, hasPrev2);
                    w = pickWidth(rng, t, units);
                    if (w == 0) break;
                }
                plan.emplace_back(t, w);
                units -= w;
            }
            // What is left (under 4.5 m, plus the part of a grid step) becomes even gaps between the lots.
            const double used = [&] {
                double sum = 0.0;
                for (const auto& e : plan) sum += e.second * kLotGrid;
                return sum;
            }();
            const double spare = (p.s1 - p.s0) - used;
            const double gap = plan.size() > 1 ? spare / static_cast<double>(plan.size() - 1) : 0.0;
            double s = p.s0 + (plan.size() > 1 ? 0.0 : 0.5 * spare);
            for (const auto& e : plan) {
                BuildingLot lot;
                lot.run = static_cast<int>(r);
                lot.type = e.first;
                lot.s0 = s;
                lot.s1 = s + e.second * kLotGrid;
                s = lot.s1 + gap;
                lot.depth = p.depth;
                double zHi = -1e9, zLo = 1e9, top = -1e9;
                for (std::size_t b : p.boxes) {
                    const MassingBox& box = boxes[b];
                    std::array<MassingVec, 8> c{};
                    massingCorners(box, 0.0, c);
                    double smin = 1e9, smax = -1e9;
                    for (int k = 0; k < 4; ++k) {
                        const double px = c[static_cast<std::size_t>(k)].x - run.ax, py = c[static_cast<std::size_t>(k)].y - run.ay;
                        smin = std::min(smin, px * dx + py * dy);
                        smax = std::max(smax, px * dx + py * dy);
                    }
                    if (smax <= lot.s0 + 0.05 || smin >= lot.s1 - 0.05) continue;
                    lot.parents.push_back(box.id);
                    zHi = std::max(zHi, box.z0);
                    zLo = std::min(zLo, box.z0);
                    top = std::max(top, box.z1);
                }
                lot.floorZ = zHi + 0.5;
                lot.footZ = zLo;
                lot.oldTop = top;
                rollTraits(lot, blockSeed(run.id, 1000u + lotNo++));
                out.lots.push_back(std::move(lot));
            }
        }
    }
    // Repair: re-roll the blamed lots' traits (and type, for a poor window) until the street rules hold.
    for (int attempt = 1; attempt <= 12; ++attempt) {
        std::vector<int> blame;
        if (checkWithBlame(out.lots, &blame).ok) break;
        for (std::size_t i = 0; i < out.lots.size(); ++i) {
            if (!blame[i]) continue;
            BuildingLot& lot = out.lots[i];
            const std::uint32_t seed = mix(lot.seed, static_cast<std::uint32_t>(attempt));
            if (blame[i] == 2) {
                static constexpr LotType kinds[4] = {LotType::Pencil, LotType::WalkUp, LotType::Tiled, LotType::TownHouse};
                LotType options[4];
                int n = 0;
                for (LotType t : kinds) {
                    if (t == lot.type || !widthLegal(t, lot.units())) continue;
                    bool nearby = false;
                    for (std::size_t j = i >= 2 ? i - 2 : 0; j < std::min(out.lots.size(), i + 3); ++j)
                        nearby = nearby || (j != i && out.lots[j].run == lot.run && out.lots[j].type == t);
                    if (!nearby) options[n++] = t;
                }
                if (n == 0)
                    for (LotType t : kinds)
                        if (t != lot.type && widthLegal(t, lot.units())) options[n++] = t;
                if (n) lot.type = options[seed % static_cast<std::uint32_t>(n)];
            }
            rollTraits(lot, seed);
            ++out.repairs;
        }
    }
}

int kitPaletteSize(KitPalette p) {
    switch (p) {
    case KitPalette::Wall: return static_cast<int>(std::size(kWall));
    case KitPalette::Trim: return static_cast<int>(std::size(kTrim));
    case KitPalette::Timber: return static_cast<int>(std::size(kTimber));
    case KitPalette::Roof: return static_cast<int>(std::size(kRoof));
    case KitPalette::Accent: return static_cast<int>(std::size(kAccent));
    case KitPalette::SignFg: return static_cast<int>(std::size(kSignFg));
    }
    return 0;
}

KitRgb kitPaletteColour(KitPalette p, int index) {
    const int n = kitPaletteSize(p);
    if (n <= 0) return {};
    const int i = ((index % n) + n) % n;
    switch (p) {
    case KitPalette::Wall: return kWall[i];
    case KitPalette::Trim: return kTrim[i];
    case KitPalette::Timber: return kTimber[i];
    case KitPalette::Roof: return kRoof[i];
    case KitPalette::Accent: return kAccent[i];
    case KitPalette::SignFg: return kSignFg[i];
    }
    return {};
}

KitCensus kitCensus(const KitTable& kit, const FrontagePlan& plan, const std::vector<KitPlacement>& placements) {
    KitCensus c;
    c.lots = static_cast<int>(plan.lots.size());
    for (const BuildingLot& l : plan.lots) ++c.types[static_cast<std::size_t>(l.type)];
    for (const KitPlacement& p : placements) {
        if (p.piece >= 0 && static_cast<std::size_t>(p.piece) < kit.pieces.size()) {
            ++c.pieces;
            c.triangles += kit.pieces[static_cast<std::size_t>(p.piece)].triangles;
        } else {
            ++c.boxes;
            c.triangles += 12;
        }
    }
    return c;
}

} // namespace ftcore
