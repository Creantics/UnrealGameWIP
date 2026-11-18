// The block generator (ftcore/BlockGen.h).

#include "ftcore/BlockGen.h"

#include "CsvRead.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>

namespace ftcore {

namespace {

using detail::fail;
using detail::isComment;
using detail::parseNumber;
using detail::Record;
using detail::splitCsv;
using detail::trim;

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

bool lower(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != b[i]) return false;
    }
    return true;
}

bool parseClass(std::string_view s, BlockClass& out) {
    if (lower(s, "road")) out = BlockClass::Road;
    else if (lower(s, "lane")) out = BlockClass::Lane;
    else if (lower(s, "market")) out = BlockClass::Market;
    else if (lower(s, "alley")) out = BlockClass::Alley;
    else return false;
    return true;
}

bool parseFeature(std::string_view s, BlockFeatureKind& out) {
    static constexpr BlockFeatureKind all[] = {BlockFeatureKind::Park,     BlockFeatureKind::Forecourt, BlockFeatureKind::Plaza,
                                               BlockFeatureKind::Precinct, BlockFeatureKind::Hall,      BlockFeatureKind::Podium,
                                               BlockFeatureKind::Court,    BlockFeatureKind::Gate};
    for (const BlockFeatureKind k : all)
        if (lower(s, blockFeatureName(k))) {
            out = k;
            return true;
        }
    return false;
}

// The band's rows: w extent, the side they face (-1 low, +1 high), the street or alley they face and its class.
struct Row {
    double w0 = 0.0, w1 = 0.0;
    int face = -1;
    std::string faceId;
    BlockClass cls = BlockClass::Lane;
    bool internal = false; // faces a mid-block alley
};

struct Lot {
    std::size_t box = 0; // index into out.boxes
    double u0 = 0.0, u1 = 0.0;
    std::size_t row = 0;
    BlockClass cls = BlockClass::Lane;
    bool shop = false, corner = false, end = false, merged = false, slab = false, walkup = false, internal = false;
    int floors = 0;
    double parapet = 0.0, height = 0.0, yard = 0.0;
    bool alongX = true;
    int face = -1;
    double faceW = 0.0, backW = 0.0;
    std::uint32_t seed = 0;
};

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A street's buffer bounds (an axis-aligned street's not grown past its ends).
BlockRect streetRect(const BlockPlanStreet& s) {
    const double h = s.width * 0.5;
    const double hx = s.y0 == s.y1 ? 0.0 : h, hy = s.x0 == s.x1 ? 0.0 : h;
    return {std::min(s.x0, s.x1) - hx, std::min(s.y0, s.y1) - hy, std::max(s.x0, s.x1) + hx, std::max(s.y0, s.y1) + hy};
}

const BlockPlanStreet* findStreet(const BlockPlan& plan, const std::string& id) {
    if (id.empty()) return nullptr;
    for (const BlockPlanStreet& s : plan.streets)
        if (s.id == id) return &s;
    return nullptr;
}

void sampleGround(const BlockGround& g, BlockBox& b) {
    const BlockRect r = b.rect();
    const double in = 0.25;
    double lo = 1e18, hi = -1e18;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const double x = r.x0 + in + (r.w() - 2 * in) * 0.5 * i;
            const double y = r.y0 + in + (r.h() - 2 * in) * 0.5 * j;
            const double z = g.at(x, y);
            lo = std::min(lo, z);
            hi = std::max(hi, z);
        }
    b.ground = g.at(b.cx, b.cy);
    b.terrMin = lo;
    b.terrMax = hi;
}

// Sets a box's z from the ground: base 0.3 m under the lowest ground, top height over the centre's.
void standBox(const BlockGround& g, BlockBox& b, double height, double bury = 0.3) {
    sampleGround(g, b);
    b.z0 = b.terrMin - bury;
    b.z1 = b.ground + height;
    b.sink = b.terrMax - b.z0;
}

BlockBox makeBox(const std::string& id, const std::string& row, const std::string& kind, const BlockRect& r) {
    BlockBox b;
    b.id = id;
    b.row = row;
    b.kind = kind;
    b.cx = (r.x0 + r.x1) * 0.5;
    b.cy = (r.y0 + r.y1) * 0.5;
    b.sx = r.w();
    b.sy = r.h();
    return b;
}

BlockRect uwRect(bool alongX, double u0, double u1, double w0, double w1) {
    return alongX ? BlockRect{u0, w0, u1, w1} : BlockRect{w0, u0, w1, u1};
}

std::string classNote(const Lot& l) {
    std::string n = "R9 class=";
    n += blockClassName(l.cls);
    n += " floors=" + std::to_string(l.floors);
    n += l.shop ? " shop=1" : " shop=0";
    if (l.slab) n += " slab";
    if (l.walkup) n += " walkup";
    if (l.yard > 0.0) n += " yard=" + fmt("%.1f", l.yard);
    return n;
}

double parapetOf(std::uint32_t v) {
    static constexpr double p[3] = {0.0, 0.6, 1.2};
    return p[v % 3];
}

bool inAny(const std::vector<BlockRect>& rs, double x, double y) {
    for (const BlockRect& r : rs)
        if (r.contains(x, y)) return true;
    return false;
}

} // namespace

std::uint32_t blockFnv1a(std::string_view bytes, std::uint32_t h) {
    for (const char c : bytes) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x01000193u;
    }
    return h;
}

std::uint32_t blockSeed(std::string_view streetId, std::uint32_t blockIndex) {
    std::uint32_t h = blockFnv1a(streetId);
    const char sep[1] = {0x1F};
    h = blockFnv1a(std::string_view(sep, 1), h);
    char b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<char>((blockIndex >> (8 * i)) & 0xFFu);
    return blockFnv1a(std::string_view(b, 4), h);
}

namespace {
std::uint32_t mix32(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}
} // namespace

double blockHash01(std::uint32_t seed, std::string_view key) {
    // both mixed: seeds a few bits apart (ids differing in one character) must still spread
    const std::uint32_t h = mix32(blockFnv1a(key) ^ mix32(seed));
    return static_cast<double>(h >> 8) / 16777216.0;
}

std::string_view blockClassName(BlockClass c) {
    switch (c) {
    case BlockClass::Road: return "road";
    case BlockClass::Lane: return "lane";
    case BlockClass::Market: return "market";
    case BlockClass::Alley: return "alley";
    }
    return "lane";
}

std::string_view blockFeatureName(BlockFeatureKind k) {
    switch (k) {
    case BlockFeatureKind::Park: return "park";
    case BlockFeatureKind::Forecourt: return "forecourt";
    case BlockFeatureKind::Plaza: return "plaza";
    case BlockFeatureKind::Precinct: return "precinct";
    case BlockFeatureKind::Hall: return "hall";
    case BlockFeatureKind::Podium: return "podium";
    case BlockFeatureKind::Court: return "court";
    case BlockFeatureKind::Gate: return "gate";
    }
    return "park";
}

void BlockGround::setFrame(double ox, double oy, double deg) {
    const double r = deg * 3.14159265358979323846 / 180.0;
    frameX = ox, frameY = oy, frameCos = std::cos(r), frameSin = std::sin(r);
}

double BlockGround::at(double lx, double ly) const {
    double x = 0.0, y = 0.0;
    toWorld(lx, ly, x, y);
    double z = field.grid ? massingGroundAt(field.grid, field.samplesX, field.samplesY, field.spacing, field.originX,
                                            field.originY, x, y)
                          : 0.0;
    if (!zones.empty() && zoneOn.size() == zones.size()) z = massingFloorAt(zones, zoneOn, x, y, z, 1e9);
    return z;
}

bool importBlockPlan(std::string_view csv, BlockPlan& out, std::string* error) {
    std::vector<Record> records;
    if (!splitCsv(csv, records, error)) return false;
    std::vector<std::string> head;
    for (const Record& rec : records) {
        if (isComment(rec)) continue;
        if (head.empty()) {
            for (const std::string& h : rec.fields) head.emplace_back(trim(h));
            continue;
        }
        auto col = [&](std::string_view name) -> std::string_view {
            for (std::size_t i = 0; i < head.size(); ++i)
                if (head[i] == name) return i < rec.fields.size() ? trim(rec.fields[i]) : std::string_view();
            return {};
        };
        auto num = [&](std::string_view name, double& v, double def) -> bool {
            const std::string_view t = col(name);
            if (t.empty()) {
                v = def;
                return true;
            }
            return parseNumber(t, v);
        };
        const std::string_view type = col("type");
        const std::string id(col("id"));
        const std::string kind(col("kind"));
        double x0, y0, x1, y1, width, pavement, value, top, block, rows, emit;
        if (!num("x0", x0, 0) || !num("y0", y0, 0) || !num("x1", x1, 0) || !num("y1", y1, 0) || !num("width", width, 0) ||
            !num("pavement", pavement, 0) || !num("value", value, 0) || !num("top", top, 0) || !num("block", block, 0) ||
            !num("rows", rows, 0) || !num("emit", emit, 1))
            return fail(error, rec.line, "a bad number");
        if (type == "street") {
            BlockPlanStreet s;
            s.id = id;
            s.kind = kind;
            if (!parseClass(col("class"), s.cls)) return fail(error, rec.line, "a street's class is road, lane, market or alley");
            s.x0 = x0, s.y0 = y0, s.x1 = x1, s.y1 = y1;
            s.width = width;
            s.pavement = pavement;
            s.emit = emit != 0.0;
            if (s.id.empty() || s.width <= 0.0 || 2.0 * s.pavement >= s.width)
                return fail(error, rec.line, "a street needs an id, a width and pavements under half of it");
            out.streets.push_back(std::move(s));
        } else if (type == "band") {
            BlockPlanBand b;
            b.id = id;
            if (kind == "x") b.alongX = true;
            else if (kind == "y") b.alongX = false;
            else return fail(error, rec.line, "a band's kind is x or y");
            b.r = {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
            b.low = std::string(col("low"));
            b.high = std::string(col("high"));
            b.start = std::string(col("start"));
            b.end = std::string(col("end"));
            b.block = static_cast<std::uint32_t>(block);
            b.rows = static_cast<int>(rows);
            b.district = std::string(col("district"));
            if (b.id.empty() || b.r.w() <= 0.0 || b.r.h() <= 0.0 || (b.low.empty() && b.high.empty()))
                return fail(error, rec.line, "a band needs an id, a rect and a street on one side");
            out.bands.push_back(std::move(b));
        } else if (type == "feature") {
            BlockPlanFeature f;
            f.id = id;
            if (!parseFeature(kind, f.kind)) return fail(error, rec.line, "an unknown feature kind");
            f.r = {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
            f.value = value;
            f.top = top;
            f.width = width;
            f.district = std::string(col("district"));
            if (f.id.empty() || f.r.w() <= 0.0 || f.r.h() <= 0.0) return fail(error, rec.line, "a feature needs an id and a rect");
            out.features.push_back(std::move(f));
        } else if (type == "landmark") {
            BlockPlanLandmark l;
            l.id = id;
            l.x = x0;
            l.y = y0;
            l.base = width;
            l.height = value;
            if (l.id.empty() || l.base <= 0.0 || l.height <= 0.0) return fail(error, rec.line, "a landmark needs a base and a height");
            out.landmarks.push_back(std::move(l));
        } else if (type == "drop") {
            if (id.empty()) return fail(error, rec.line, "a drop needs an id");
            out.drops.push_back(id);
        } else {
            return fail(error, rec.line, "a row's type is street, band, feature, landmark or drop");
        }
    }
    if (head.empty()) return fail(error, 1, "no header row");
    return true;
}

int blockRowCount(double depth, double* rowDepth) {
    int best = 0;
    double bestD = 0.0;
    for (int n = 2; n <= 6; ++n) {
        const int a = n - 1;
        const double d = (depth - kBlockAlley * a) / n;
        if (d < kBlockRowMin - 1e-9 || d > kBlockRowMax + 1e-9) continue;
        if (best == 0 || std::fabs(d - kBlockRowAim) < std::fabs(bestD - kBlockRowAim)) {
            best = n;
            bestD = d;
        }
    }
    if (rowDepth) *rowDepth = bestD;
    return best;
}

void blockLotWidths(double len, bool shop, bool cornerLow, bool cornerHigh, BlockRng& rng, std::vector<double>& out) {
    const double lo = shop ? kBlockShopMin : kBlockHouseMin;
    const double hi = shop ? kBlockShopMax : kBlockHouseMax;
    const std::size_t first = out.size();
    if (len <= 0.0) return;
    double used = 0.0;
    for (;;) {
        const bool corner = (out.size() == first && cornerLow);
        const double k = corner ? kBlockCorner : 1.0;
        double w = rng.range(lo, hi) * k;
        const double rest = len - used;
        if (rest <= w + lo * 0.5) {
            // the last lot (a corner at the high end gets its x1.3)
            if (rest >= lo * (cornerHigh ? kBlockCorner * 0.8 : 0.8) || out.size() == first) {
                out.push_back(rest);
            } else {
                // too short: spread over the lots before it
                const std::size_t n = out.size() - first;
                for (std::size_t i = first; i < out.size(); ++i) out[i] += rest / static_cast<double>(n);
            }
            break;
        }
        if (cornerHigh && rest - w < hi * kBlockCorner && rest - w > 0.0) {
            // leave a corner-sized last lot
            const double last = std::min(rest - lo * 0.8, rng.range(lo, hi) * kBlockCorner);
            if (last >= lo && rest - last >= lo) {
                w = rest - last;
                if (w > hi * k) {
                    out.push_back(w * 0.5);
                    out.push_back(w * 0.5);
                } else {
                    out.push_back(w);
                }
                out.push_back(last);
                break;
            }
        }
        out.push_back(w);
        used += w;
    }
}

int blockFloors(BlockClass c, BlockRng& rng) {
    int lo = 2, mode = 3, hi = 4;
    if (c == BlockClass::Road) lo = 3, mode = 4, hi = 6;
    else if (c == BlockClass::Alley) lo = 1, mode = 2, hi = 3;
    double total = 0.0;
    double w[8] = {};
    for (int f = lo; f <= hi; ++f) {
        const int d = std::abs(f - mode);
        w[f - lo] = d == 0 ? 3.0 : (d == 1 ? 2.0 : 1.0);
        total += w[f - lo];
    }
    double r = rng.unit() * total;
    for (int f = lo; f <= hi; ++f) {
        r -= w[f - lo];
        if (r < 0.0) return f;
    }
    return hi;
}

double blockHeight(int floors, bool shop, double parapet) {
    return (shop ? kBlockShopFloor : kBlockFloor) + kBlockFloor * (floors - 1) + parapet;
}

std::size_t blockVaryHeights(std::vector<double>& heights, const std::vector<double>& centres, bool shop) {
    (void)shop;
    const std::size_t n = heights.size();
    std::size_t changes = 0;
    auto same = [](double a, double b) { return std::fabs(a - b) < 0.05; };
    // no run of more than 3 identical
    for (std::size_t i = 3; i < n; ++i)
        if (same(heights[i], heights[i - 1]) && same(heights[i], heights[i - 2]) && same(heights[i], heights[i - 3])) {
            heights[i] += (i % 2) ? 0.6 : -0.6;
            ++changes;
        }
    // at least 4 heights in every 60 m holding 4 lots
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t j = i;
        while (j < n && centres[j] - centres[i] <= 60.0) ++j;
        if (j - i < 4) continue;
        for (int pass = 0; pass < 8; ++pass) {
            std::size_t distinct = 0;
            for (std::size_t a = i; a < j; ++a) {
                bool seen = false;
                for (std::size_t b = i; b < a; ++b)
                    if (same(heights[a], heights[b])) seen = true;
                if (!seen) ++distinct;
            }
            if (distinct >= 4) break;
            // change the last duplicate to a height the window lacks
            for (std::size_t a = j; a-- > i;) {
                bool dup = false;
                for (std::size_t b = i; b < j; ++b)
                    if (b != a && same(heights[a], heights[b])) dup = true;
                if (!dup) continue;
                static constexpr double steps[] = {0.6, -0.6, 1.2, -1.2, 3.0, -3.0, 3.6, 2.4};
                for (const double s : steps) {
                    const double h = heights[a] + s;
                    if (h < 3.0) continue;
                    bool clash = false;
                    for (std::size_t b = i; b < j; ++b)
                        if (same(h, heights[b])) clash = true;
                    // keep the no-run rule
                    if (!clash) {
                        heights[a] = h;
                        ++changes;
                        break;
                    }
                }
                break;
            }
        }
    }
    return changes;
}

BlockRect blockFootprint(const MassingBox& b) {
    const double r = b.rot * 3.14159265358979323846 / 180.0;
    const double c = std::fabs(std::cos(r)), s = std::fabs(std::sin(r));
    const double hx = (b.sx * c + b.sy * s) * 0.5, hy = (b.sx * s + b.sy * c) * 0.5;
    return {b.cx - hx, b.cy - hy, b.cx + hx, b.cy + hy};
}

bool blockOnStreet(const MassingStreet& s, double x, double y, double grow) {
    const double h = s.width * 0.5 + grow;
    if (s.points.size() == 1) return std::hypot(x - s.points[0].x, y - s.points[0].y) <= h;
    for (std::size_t i = 1; i < s.points.size(); ++i) {
        const double ax = s.points[i - 1].x, ay = s.points[i - 1].y, bx = s.points[i].x, by = s.points[i].y;
        const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
        double t = l2 > 0.0 ? ((x - ax) * dx + (y - ay) * dy) / l2 : 0.0;
        t = clampd(t, 0.0, 1.0);
        if (std::hypot(x - (ax + t * dx), y - (ay + t * dy)) <= h) return true;
    }
    return false;
}

bool blockSweepKeep(const MassingBox& b, const BlockRect& rect, const std::vector<MassingStreet>& streets,
                    const std::vector<BlockBox>& generated) {
    const BlockRect f = blockFootprint(b);
    const bool zone = b.source == MassingSource::Zone || b.kind == "zone" || b.kind == "zone_slope";
    const bool exempt = zone || b.kind == "kerb" || b.kind == "marking" || b.id.rfind("V1-CLF", 0) == 0 ||
                        b.id.rfind("V1-KB", 0) == 0 || (b.source == MassingSource::Plan && b.kind == "wall") ||
                        b.kind == "deck";
    if (exempt) return true;
    if (!f.overlaps(rect, 0.0)) {
        // outside the slice: kept unless a generated building stands on it
        const bool building = b.kind == "kit" || b.kind == "box" || b.kind == "slab";
        if (!building) return true;
        for (const BlockBox& g : generated)
            if (g.building && f.overlaps(g.rect(), 0.05)) return false;
        return true;
    }
    if (b.kind == "gutter") {
        for (const MassingStreet& s : streets)
            if (blockOnStreet(s, b.cx, b.cy, -0.25)) return true;
        return false;
    }
    return false;
}

void blockGenerate(const BlockPlan& plan, const BlockGround& ground, BlockGenResult& out) {
    BlockGenStats& st = out.stats;
    std::vector<Lot> lots;
    std::vector<BlockRect> cuts; // feature rects that cut rows
    for (const BlockPlanFeature& f : plan.features)
        if (f.kind != BlockFeatureKind::Gate) cuts.push_back(f.r);
    std::vector<BlockRect> streetCuts;
    for (const BlockPlanStreet& s : plan.streets) streetCuts.push_back(streetRect(s));
    int slabsPerCell[64] = {};
    auto cellOf = [](double x, double y) {
        const int cx = static_cast<int>(std::floor(x / kBlockCell)) & 7;
        const int cy = static_cast<int>(std::floor(y / kBlockCell)) & 7;
        return cx * 8 + cy;
    };

    for (const BlockPlanStreet& s : plan.streets)
        if (s.emit) out.streets.push_back(s);

    // pass 1: every band's rows, and an alley between each pair of rows (back to back or not)
    std::vector<std::vector<Row>> bandRows(plan.bands.size());
    const std::size_t alleyFirst = out.streets.size();
    for (std::size_t bi = 0; bi < plan.bands.size(); ++bi) {
        const BlockPlanBand& band = plan.bands[bi];
        const bool ax = band.alongX;
        const double u0 = ax ? band.r.x0 : band.r.y0, u1 = ax ? band.r.x1 : band.r.y1;
        const double w0 = ax ? band.r.y0 : band.r.x0, w1 = ax ? band.r.y1 : band.r.x1;
        const double depth = w1 - w0;
        const BlockPlanStreet* lowS = findStreet(plan, band.low);
        const BlockPlanStreet* highS = findStreet(plan, band.high);
        std::vector<Row>& rows = bandRows[bi];
        if (lowS && highS) {
            double d = 0.0;
            int n = band.rows > 0 ? band.rows : blockRowCount(depth, &d);
            double alleyW = kBlockAlley;
            if (band.rows > 0) d = (depth - kBlockAlley * (n - 1)) / n;
            if (n == 0) {
                n = 2;
                alleyW = 0.0;
                d = depth * 0.5;
                st.notes.push_back(band.id + ": no row count fits " + fmt("%.2f", depth) + " m; two rows back to back");
            }
            double pos = w0;
            for (int i = 0; i < n; ++i) {
                Row r;
                r.w0 = pos;
                r.w1 = pos + d;
                pos += d;
                if (i == 0) {
                    r.face = -1;
                    r.faceId = lowS->id;
                    r.cls = lowS->cls;
                } else if (i == n - 1) {
                    r.face = +1;
                    r.faceId = highS->id;
                    r.cls = highS->cls;
                } else {
                    // alley k runs between rows k-1 and k: an odd row faces the one above it, an even one the one below
                    r.face = i % 2 == 1 ? +1 : -1;
                    r.faceId = band.id + "-A" + std::to_string(i % 2 == 1 ? i + 1 : i);
                    r.cls = BlockClass::Alley;
                    r.internal = true;
                }
                rows.push_back(r);
                if (i < n - 1 && alleyW > 0.0) {
                    BlockPlanStreet a;
                    a.id = band.id + "-A" + std::to_string(i + 1);
                    a.kind = "alley";
                    a.cls = BlockClass::Alley;
                    a.width = alleyW;
                    const double wc = pos + alleyW * 0.5;
                    if (ax) a.x0 = u0, a.x1 = u1, a.y0 = a.y1 = wc;
                    else a.y0 = u0, a.y1 = u1, a.x0 = a.x1 = wc;
                    out.streets.push_back(a);
                    ++st.alleys;
                    pos += alleyW;
                }
            }
        } else if (lowS || highS) {
            const BlockPlanStreet* s = lowS ? lowS : highS;
            const double d = std::min(kBlockRowMax, depth);
            Row r;
            r.face = lowS ? -1 : +1;
            r.w0 = lowS ? w0 : w1 - d;
            r.w1 = r.w0 + d;
            r.faceId = s->id;
            r.cls = s->cls;
            rows.push_back(r);
        } else {
            st.notes.push_back(band.id + ": neither edge street is in the plan; left empty");
        }
    }

    // cut the alleys clear through: an end touching no street runs on along its axis to the nearest street within
    // kBlockAlleyReach m (through the next band's rows), unless a feature stands in the way
    std::vector<BlockRect> alleyCuts;
    for (std::size_t k = alleyFirst; k < out.streets.size(); ++k) {
        BlockPlanStreet& a = out.streets[k];
        const bool alongX = a.y0 == a.y1;
        auto touches = [&](double x, double y) {
            for (std::size_t si = 0; si < plan.streets.size(); ++si)
                if (streetCuts[si].contains(x, y)) return true;
            for (std::size_t j = alleyFirst; j < out.streets.size(); ++j)
                if (j != k && streetRect(out.streets[j]).contains(x, y)) return true;
            return false;
        };
        auto blocked = [&](double x, double y) {
            for (const BlockRect& c : cuts)
                if (c.contains(x, y)) return true;
            return false;
        };
        for (int end = 0; end < 2; ++end) {
            double& e = alongX ? (end == 0 ? a.x0 : a.x1) : (end == 0 ? a.y0 : a.y1);
            const double other = alongX ? a.y0 : a.x0;
            const double dir = (end == 0) == ((alongX ? a.x0 : a.y0) <= (alongX ? a.x1 : a.y1)) ? -1.0 : 1.0;
            auto at = [&](double s, double& x, double& y) {
                x = alongX ? e + dir * s : other;
                y = alongX ? other : e + dir * s;
            };
            double x = 0.0, y = 0.0;
            at(0.5, x, y);
            if (touches(x, y)) continue;
            double reach = -1.0;
            for (double s = 0.5; s <= kBlockAlleyReach; s += 0.25) {
                at(s, x, y);
                if (blocked(x, y)) break;
                if (touches(x, y)) {
                    reach = s - 0.25;
                    break;
                }
            }
            if (reach < 0.0) {
                st.notes.push_back(a.id + ": an alley end reaches no street within " + fmt("%.0f", kBlockAlleyReach) + " m");
                continue;
            }
            e += dir * reach;
            ++st.alleysCut;
        }
        alleyCuts.push_back(streetRect(a));
    }

    // pass 2: the lots
    for (std::size_t bi = 0; bi < plan.bands.size(); ++bi) {
        const BlockPlanBand& band = plan.bands[bi];
        const bool ax = band.alongX;
        const double u0 = ax ? band.r.x0 : band.r.y0, u1 = ax ? band.r.x1 : band.r.y1;
        const std::string& seedStreet = !band.low.empty() ? band.low : band.high;
        const std::uint32_t seed = blockSeed(seedStreet, band.block);
        const std::vector<Row>& rows = bandRows[bi];
        for (std::size_t ri = 0; ri < rows.size(); ++ri) {
            const Row& row = rows[ri];
            BlockRng rng(seed ^ static_cast<std::uint32_t>((ri + 1) * 0x9E3779B9u));
            // free u intervals
            std::vector<std::pair<double, double>> free{{u0, u1}};
            std::vector<std::pair<bool, bool>> corner{{!band.start.empty(), !band.end.empty()}};
            auto cutBy = [&](const BlockRect& c, bool isStreet) {
                const BlockRect rr = uwRect(ax, u0, u1, row.w0, row.w1);
                if (!rr.overlaps(c)) return;
                const double c0 = ax ? c.x0 : c.y0, c1 = ax ? c.x1 : c.y1;
                std::vector<std::pair<double, double>> nf;
                std::vector<std::pair<bool, bool>> nc;
                for (std::size_t k = 0; k < free.size(); ++k) {
                    const auto [a, b] = free[k];
                    if (c1 <= a || c0 >= b) {
                        nf.push_back(free[k]);
                        nc.push_back(corner[k]);
                        continue;
                    }
                    if (c0 > a) {
                        nf.push_back({a, c0});
                        nc.push_back({corner[k].first, isStreet});
                    }
                    if (c1 < b) {
                        nf.push_back({c1, b});
                        nc.push_back({isStreet, corner[k].second});
                    }
                }
                free.swap(nf);
                corner.swap(nc);
            };
            for (const BlockRect& c : cuts) cutBy(c, false);
            for (std::size_t si = 0; si < plan.streets.size(); ++si) {
                const BlockPlanStreet& s = plan.streets[si];
                if (s.id == row.faceId) continue;
                cutBy(streetCuts[si], true);
            }
            for (std::size_t k = 0; k < alleyCuts.size(); ++k)
                if (out.streets[alleyFirst + k].id != row.faceId) cutBy(alleyCuts[k], true);
            const bool shop = row.cls != BlockClass::Alley;
            const std::size_t rowFirst = lots.size();
            for (std::size_t k = 0; k < free.size(); ++k) {
                const double a = free[k].first, b = free[k].second;
                if (b - a < 2.5) {
                    if (b - a > 0.05) st.notes.push_back(band.id + ": a " + fmt("%.1f", b - a) + " m gap left open");
                    continue;
                }
                std::vector<double> widths;
                blockLotWidths(b - a, shop, corner[k].first, corner[k].second, rng, widths);
                double u = a;
                const std::size_t first = lots.size();
                for (std::size_t i = 0; i < widths.size(); ++i) {
                    Lot l;
                    l.u0 = u;
                    l.u1 = (i + 1 == widths.size()) ? b : u + widths[i];
                    u = l.u1;
                    l.row = ri;
                    l.cls = row.cls;
                    l.shop = shop;
                    l.internal = row.internal;
                    l.corner = (i == 0 && corner[k].first) || (i + 1 == widths.size() && corner[k].second);
                    l.end = i == 0 || i + 1 == widths.size(); // a yard here would open onto a side
                    l.alongX = ax;
                    l.face = row.face;
                    l.faceW = row.face < 0 ? row.w0 : row.w1;
                    l.backW = row.face < 0 ? row.w1 : row.w0;
                    l.seed = rng.next();
                    lots.push_back(l);
                }
                // merges: ~10% of lots in groups of 2-3; one forced on a kerbed run of 40 m or more
                bool any = false;
                for (std::size_t i = first; i + 1 < lots.size(); ++i) {
                    if (lots[i].corner) continue;
                    const bool force = !any && row.cls == BlockClass::Road && b - a >= 40.0 && i + 2 >= first + (lots.size() - first) / 2;
                    if (!force && rng.unit() >= 0.045) continue;
                    std::size_t g = 2 + (rng.next() % 2);
                    while (g > 1 && (i + g > lots.size() || lots[i + g - 1].corner)) --g;
                    if (g < 2) continue;
                    lots[i].u1 = lots[i + g - 1].u1;
                    lots[i].merged = true;
                    lots.erase(lots.begin() + static_cast<std::ptrdiff_t>(i + 1), lots.begin() + static_cast<std::ptrdiff_t>(i + g));
                    any = true;
                    ++st.merged;
                }
            }
            // heights
            std::vector<double> heights, centres;
            for (std::size_t i = rowFirst; i < lots.size(); ++i) {
                Lot& l = lots[i];
                const double cu = (l.u0 + l.u1) * 0.5;
                const double cx = ax ? cu : (row.w0 + row.w1) * 0.5, cy = ax ? (row.w0 + row.w1) * 0.5 : cu;
                if (l.merged && row.cls == BlockClass::Road && slabsPerCell[cellOf(cx, cy)] < kBlockSlabsPerCell) {
                    l.slab = true;
                    ++slabsPerCell[cellOf(cx, cy)];
                    l.floors = 8 + static_cast<int>(rng.next() % 7);
                    ++st.slabs;
                } else if (l.merged) {
                    l.walkup = true;
                    l.floors = 3 + static_cast<int>(rng.next() % 3);
                    ++st.walkups;
                } else {
                    l.floors = blockFloors(row.cls, rng);
                }
                l.parapet = parapetOf(rng.next());
                heights.push_back(blockHeight(l.floors, l.shop, l.parapet));
                centres.push_back(cu);
            }
            blockVaryHeights(heights, centres, shop);
            for (std::size_t i = rowFirst; i < lots.size(); ++i) lots[i].height = heights[i - rowFirst];
            for (std::size_t i = rowFirst; i < lots.size(); ++i) {
                Lot& l = lots[i];
                std::string id = "R9-" + band.id + "-R" + std::to_string(ri + 1) + "-L" + std::to_string(i - rowFirst + 1);
                BlockBox bx = makeBox(id, band.id, "kit", uwRect(ax, l.u0, l.u1, row.w0, row.w1));
                bx.building = true;
                l.box = out.boxes.size();
                out.boxes.push_back(bx);
                ++st.lots;
                if (l.shop) ++st.shopLots;
                else ++st.houseLots;
                if (l.corner) ++st.corners;
            }
        }
    }

    // yards: 15-25% of house lots (20% aimed) with a row 12 m deep or more
    {
        std::vector<std::pair<double, std::size_t>> cand;
        std::size_t houses = 0;
        for (std::size_t i = 0; i < lots.size(); ++i) {
            const Lot& l = lots[i];
            if (l.shop || l.merged) continue;
            ++houses;
            if (!l.end && std::fabs(l.backW - l.faceW) >= 12.0) cand.push_back({blockHash01(l.seed, "yard"), i});
        }
        std::sort(cand.begin(), cand.end());
        const std::size_t want = std::min(cand.size(), static_cast<std::size_t>(std::lround(houses * 0.2)));
        for (std::size_t k = 0; k < want; ++k) {
            Lot& l = lots[cand[k].second];
            const double d = std::fabs(l.backW - l.faceW);
            l.yard = clampd(d - 10.0, 2.0, 5.0);
            const double s = l.face < 0 ? 1.0 : -1.0; // from the face toward the back
            const double newBack = l.backW - s * l.yard;
            BlockBox& b = out.boxes[l.box];
            const double fw = std::min(l.faceW, newBack), bw = std::max(l.faceW, newBack);
            const BlockRect r = uwRect(l.alongX, l.u0, l.u1, fw, bw);
            b.cx = (r.x0 + r.x1) * 0.5, b.cy = (r.y0 + r.y1) * 0.5, b.sx = r.w(), b.sy = r.h();
            // the 2 m wall at the yard's back line
            const double ww = l.backW - s * 0.1;
            BlockBox wall = makeBox(b.id + "-YW", b.row, "wall", uwRect(l.alongX, l.u0, l.u1, ww - 0.1, ww + 0.1));
            standBox(ground, wall, 2.0, 0.2);
            wall.note = "R9 yard wall";
            out.boxes.push_back(wall);
            l.backW = newBack;
            ++st.yards;
        }
    }

    // stand the lots
    for (Lot& l : lots) {
        BlockBox& b = out.boxes[l.box];
        standBox(ground, b, l.height);
        b.note = classNote(l);
    }

    // features
    std::vector<BlockRect> gates;
    for (const BlockPlanFeature& f : plan.features)
        if (f.kind == BlockFeatureKind::Gate) gates.push_back(f.r);
    struct Extra {
        std::size_t box;
        double bias;
        const char* room;
    };
    std::vector<Extra> extras;
    for (const BlockPlanFeature& f : plan.features) {
        const std::string base = "R9-F-" + f.id;
        switch (f.kind) {
        case BlockFeatureKind::Park:
        case BlockFeatureKind::Forecourt:
        case BlockFeatureKind::Plaza: {
            BlockBox p = makeBox(base, f.id, "landing", f.r);
            standBox(ground, p, 0.15, 0.2);
            p.note = std::string("R9 ") + std::string(blockFeatureName(f.kind));
            out.boxes.push_back(p);
            break;
        }
        case BlockFeatureKind::Court: break;
        case BlockFeatureKind::Hall:
        case BlockFeatureKind::Podium: {
            BlockBox p = makeBox(base, f.id, "kit", f.r);
            p.building = true;
            const int floors = f.value > 0.0 ? static_cast<int>(f.value) : 2;
            double h = blockHeight(floors, true, 0.0);
            sampleGround(ground, p);
            if (f.top > 0.0) h = f.top - p.ground;
            standBox(ground, p, h);
            p.note = std::string("R9 class=") + (f.kind == BlockFeatureKind::Hall ? "hall" : "podium") + " floors=" +
                     std::to_string(floors) + " shop=1";
            extras.push_back({out.boxes.size(), f.kind == BlockFeatureKind::Hall ? 1e9 : 1.5, "interior_lobby"});
            out.boxes.push_back(p);
            if (f.kind == BlockFeatureKind::Podium && f.width > 0.0) {
                BlockTower t;
                t.id = "R9-T-" + f.id;
                t.kind = "glass";
                t.cx = p.cx;
                t.cy = p.cy;
                t.z0 = p.z1;
                t.sx = t.sy = f.width;
                t.floors = 30 + static_cast<int>(blockHash01(blockFnv1a(f.id), "floors") * 21.0);
                t.height = t.floors * 3.6;
                t.note = "on " + p.id;
                out.towers.push_back(t);
            }
            break;
        }
        case BlockFeatureKind::Precinct: {
            const double t = 0.3, h = 2.2;
            const BlockRect& r = f.r;
            // the four sides, each split around gates standing on it
            struct Side {
                bool alongX;
                double c, a, b;
            };
            const Side sides[4] = {{true, r.y0 + t * 0.5, r.x0, r.x1},
                                   {true, r.y1 - t * 0.5, r.x0, r.x1},
                                   {false, r.x0 + t * 0.5, r.y0 + t, r.y1 - t},
                                   {false, r.x1 - t * 0.5, r.y0 + t, r.y1 - t}};
            int n = 0;
            for (const Side& sd : sides) {
                std::vector<std::pair<double, double>> parts{{sd.a, sd.b}};
                for (const BlockRect& g : gates) {
                    const bool on = sd.alongX ? (g.y0 <= sd.c + 1.0 && g.y1 >= sd.c - 1.0)
                                              : (g.x0 <= sd.c + 1.0 && g.x1 >= sd.c - 1.0);
                    if (!on) continue;
                    const double g0 = sd.alongX ? g.x0 : g.y0, g1 = sd.alongX ? g.x1 : g.y1;
                    std::vector<std::pair<double, double>> np;
                    for (const auto& pr : parts) {
                        if (g1 <= pr.first || g0 >= pr.second) {
                            np.push_back(pr);
                            continue;
                        }
                        if (g0 > pr.first) np.push_back({pr.first, g0});
                        if (g1 < pr.second) np.push_back({g1, pr.second});
                    }
                    parts.swap(np);
                }
                for (const auto& pr : parts) {
                    if (pr.second - pr.first < 0.3) continue;
                    const BlockRect wr = sd.alongX ? BlockRect{pr.first, sd.c - t * 0.5, pr.second, sd.c + t * 0.5}
                                                   : BlockRect{sd.c - t * 0.5, pr.first, sd.c + t * 0.5, pr.second};
                    BlockBox w = makeBox(base + "-W" + std::to_string(++n), f.id, "wall", wr);
                    standBox(ground, w, h, 0.2);
                    w.note = "R9 precinct wall";
                    out.boxes.push_back(w);
                }
            }
            break;
        }
        case BlockFeatureKind::Gate: {
            const BlockRect& r = f.r;
            const bool longY = r.h() >= r.w();
            const double p = 0.9, ph = 5.5;
            const BlockRect a = longY ? BlockRect{(r.x0 + r.x1) * 0.5 - p * 0.5, r.y0, (r.x0 + r.x1) * 0.5 + p * 0.5, r.y0 + p}
                                      : BlockRect{r.x0, (r.y0 + r.y1) * 0.5 - p * 0.5, r.x0 + p, (r.y0 + r.y1) * 0.5 + p * 0.5};
            const BlockRect b = longY ? BlockRect{a.x0, r.y1 - p, a.x1, r.y1} : BlockRect{r.x1 - p, a.y0, r.x1, a.y1};
            BlockBox pa = makeBox(base + "-P1", f.id, "gate", a), pb = makeBox(base + "-P2", f.id, "gate", b);
            standBox(ground, pa, ph, 0.2);
            standBox(ground, pb, ph, 0.2);
            pa.note = pb.note = "R9 gate pillar";
            const BlockRect beamR = longY ? BlockRect{a.x0 - 0.1, r.y0 - 0.6, a.x1 + 0.1, r.y1 + 0.6}
                                          : BlockRect{r.x0 - 0.6, a.y0 - 0.1, r.x1 + 0.6, a.y1 + 0.1};
            BlockBox beam = makeBox(base + "-B", f.id, "gate", beamR);
            sampleGround(ground, beam);
            const double top = std::max(pa.z1, pb.z1);
            beam.z0 = top - 0.2;
            beam.z1 = top + 0.6;
            beam.sink = 0.0;
            beam.note = "R9 gate beam";
            out.boxes.push_back(pa);
            out.boxes.push_back(pb);
            out.boxes.push_back(beam);
            break;
        }
        }
    }

    // enterable: one lot in eight, biased to shops, the market and the precinct
    {
        struct Pick {
            double score;
            std::size_t box;
            int lot; // -1: an extra
            const char* room;
        };
        std::vector<Pick> picks;
        for (std::size_t i = 0; i < lots.size(); ++i) {
            const Lot& l = lots[i];
            double bias = 0.5;
            if (l.cls == BlockClass::Market) bias = 3.0;
            else if (l.shop) bias = 2.0;
            const char* room = l.slab ? "interior_lobby" : (l.shop ? "interior_shop" : "interior_flat");
            picks.push_back({blockHash01(l.seed, "enter") * bias, l.box, static_cast<int>(i), room});
        }
        for (const Extra& e : extras) picks.push_back({e.bias >= 1e9 ? 1e9 : blockHash01(blockFnv1a(out.boxes[e.box].id), "enter") * e.bias, e.box, -1, e.room});
        std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.score > b.score; });
        const std::size_t want = static_cast<std::size_t>(std::lround(static_cast<double>(lots.size()) / 8.0));
        for (std::size_t k = 0; k < std::min(want, picks.size()); ++k) {
            const Pick& p = picks[k];
            BlockBox& b = out.boxes[p.box];
            const BlockRect r = b.rect();
            // the front: a lot's street face; an extra's face toward a forecourt or plaza, else a street
            bool alongX = true;
            int face = -1;
            double fw = 0.0, bw = 0.0;
            if (p.lot >= 0) {
                const Lot& l = lots[static_cast<std::size_t>(p.lot)];
                alongX = l.alongX;
                face = l.face;
                fw = l.faceW;
                bw = l.backW;
            } else {
                struct Cand {
                    bool ax;
                    int face;
                    double fw, bw, px, py;
                };
                const Cand cs[4] = {{true, -1, r.y0, r.y1, b.cx, r.y0 - 3.0},
                                    {true, +1, r.y1, r.y0, b.cx, r.y1 + 3.0},
                                    {false, -1, r.x0, r.x1, r.x0 - 3.0, b.cy},
                                    {false, +1, r.x1, r.x0, r.x1 + 3.0, b.cy}};
                int best = 3, bestRank = 99;
                for (int c = 0; c < 4; ++c) {
                    int rank = 99;
                    for (const BlockPlanFeature& f : plan.features)
                        if ((f.kind == BlockFeatureKind::Forecourt || f.kind == BlockFeatureKind::Plaza) && f.r.contains(cs[c].px, cs[c].py))
                            rank = 0;
                    if (rank > 0)
                        for (const BlockPlanStreet& s : out.streets) {
                            MassingStreet ms;
                            ms.width = s.width;
                            ms.points = {MassingVec{s.x0, s.y0, 0.0}, MassingVec{s.x1, s.y1, 0.0}};
                            if (blockOnStreet(ms, cs[c].px, cs[c].py)) rank = std::min(rank, s.cls == BlockClass::Alley ? 2 : 1);
                        }
                    if (rank < bestRank) bestRank = rank, best = c;
                }
                alongX = cs[best].ax;
                face = cs[best].face;
                fw = cs[best].fw;
                bw = cs[best].bw;
            }
            const double s = static_cast<double>(face); // outward along w
            const double cu = alongX ? b.cx : b.cy;
            const double halfU = (alongX ? b.sx : b.sy) * 0.5;
            BlockDoor d;
            d.boxId = b.id;
            d.roomKind = p.room;
            const double ow = fw + s * 0.32, iw = fw - s * 1.68;
            if (alongX) d.ox = cu, d.oy = ow, d.ix = cu, d.iy = iw;
            else d.ox = ow, d.oy = cu, d.ix = iw, d.iy = cu;
            d.z = ground.at(d.ox, d.oy);
            const double ru = std::min(3.0, halfU - 0.5);
            const double rw0 = fw - s * 1.18, rw1 = bw + s * 0.5;
            d.room = uwRect(alongX, cu - ru, cu + ru, std::min(rw0, rw1), std::max(rw0, rw1));
            out.doors.push_back(d);
            b.note += " door";
            ++st.enterable;
        }
    }

    // pitched-roof houses: ~60% of house lots of 3 floors or fewer
    for (const Lot& l : lots) {
        if (l.shop || l.slab || l.floors > 3) continue;
        if (blockHash01(l.seed, "roof") >= 0.6) continue;
        const BlockBox& b = out.boxes[l.box];
        BlockHouse h;
        h.id = "R9-H-" + b.id.substr(3);
        h.boxId = b.id;
        h.cx = b.cx, h.cy = b.cy, h.sx = b.sx, h.sy = b.sy;
        h.eave = b.z1;
        h.pitch = 25.0 + 10.0 * blockHash01(l.seed, "pitch");
        h.ridge = l.alongX ? 'u' : 'w';
        h.hidden = l.internal;
        out.houses.push_back(h);
        ++st.pitched;
        if (h.hidden) ++st.hiddenHouses;
    }

    // trees
    {
        std::vector<BlockRect> buildings;
        for (const BlockBox& b : out.boxes)
            if (b.building) buildings.push_back(b.rect());
        std::vector<MassingStreet> ms;
        for (const BlockPlanStreet& s : out.streets) {
            MassingStreet m;
            m.id = s.id;
            m.width = s.width;
            m.points = {MassingVec{s.x0, s.y0, 0.0}, MassingVec{s.x1, s.y1, 0.0}};
            ms.push_back(m);
        }
        int n = 0;
        auto add = [&](const char* kind, const std::string& host, double x, double y, double h, double c) {
            BlockTree t;
            t.id = "R9-TR-" + std::to_string(++n);
            t.kind = kind;
            t.host = host;
            t.x = x, t.y = y, t.z = ground.at(x, y), t.height = h, t.canopy = c;
            out.trees.push_back(t);
        };
        // street trees every 10 m on roads 14 m or wider, 1 m in from the kerb, on the sides the lots face
        for (const BlockPlanStreet& s : plan.streets) {
            if (s.cls != BlockClass::Road || s.width < 14.0 - 1e-9) continue;
            const double dx = s.x1 - s.x0, dy = s.y1 - s.y0, len = std::hypot(dx, dy);
            if (len <= 0.0) continue;
            const double tx = dx / len, ty = dy / len, nx = -ty, ny = tx;
            for (int side = -1; side <= 1; side += 2) {
                const double off = s.width * 0.5 - s.pavement + 1.0;
                const double wall = s.width * 0.5;
                // a side with lots within 2 m behind its wall line
                bool lotted = false;
                for (double a = 0.0; a <= len && !lotted; a += 5.0)
                    lotted = inAny(buildings, s.x0 + tx * a + nx * side * (wall + 1.0), s.y0 + ty * a + ny * side * (wall + 1.0));
                if (!lotted) continue;
                for (double a = 5.0; a <= len - 2.0; a += 10.0) {
                    const double x = s.x0 + tx * a + nx * side * off, y = s.y0 + ty * a + ny * side * off;
                    const double wx = s.x0 + tx * a + nx * side * (wall + 0.5), wy = s.y0 + ty * a + ny * side * (wall + 0.5);
                    bool skip = false;
                    for (const MassingStreet& m : ms)
                        if (m.id != s.id && blockOnStreet(m, wx, wy, 2.0)) skip = true;
                    for (const BlockDoor& d : out.doors)
                        if (std::hypot(d.ox - wx, d.oy - wy) < 2.5) skip = true;
                    if (!skip) add("street", s.id, x, y, 7.0, 5.5);
                }
            }
        }
        for (const BlockPlanFeature& f : plan.features) {
            const BlockRect& r = f.r;
            if (f.kind == BlockFeatureKind::Park) {
                const double ix = std::min(3.0, r.w() * 0.25), iy = std::min(3.0, r.h() * 0.25);
                add("park", f.id, r.x0 + ix, r.y0 + iy, 8.0, 6.0);
                add("park", f.id, r.x1 - ix, r.y0 + iy, 8.0, 6.0);
                add("park", f.id, r.x0 + ix, r.y1 - iy, 8.0, 6.0);
                add("park", f.id, r.x1 - ix, r.y1 - iy, 8.0, 6.0);
            } else if (f.kind == BlockFeatureKind::Plaza) {
                std::vector<BlockRect> keep;
                for (const BlockPlanLandmark& l : plan.landmarks)
                    keep.push_back({l.x - l.base * 0.5 - 3.0, l.y - l.base * 0.5 - 3.0, l.x + l.base * 0.5 + 3.0, l.y + l.base * 0.5 + 3.0});
                for (double x = r.x0 + 2.5; x <= r.x1 - 2.5; x += 8.0)
                    for (const double y : {r.y0 + 2.5, r.y1 - 2.5})
                        if (!inAny(keep, x, y)) add("park", f.id, x, y, 8.0, 6.0); // a plaza's: park trees
            } else if (f.kind == BlockFeatureKind::Precinct) {
                std::vector<BlockRect> keep;
                for (const BlockPlanFeature& g : plan.features) {
                    if (g.kind == BlockFeatureKind::Hall && g.r.overlaps(r))
                        keep.push_back({g.r.x0 - 1.5, g.r.y0 - 1.5, g.r.x1 + 1.5, g.r.y1 + 1.5});
                    if (g.kind == BlockFeatureKind::Gate && g.r.overlaps({r.x0 - 1, r.y0 - 1, r.x1 + 1, r.y1 + 1})) {
                        // a clear path from the gate in to the hall
                        if (g.r.h() >= g.r.w()) keep.push_back({r.x0, g.r.y0 + 0.5, r.x1, g.r.y1 - 0.5});
                        else keep.push_back({g.r.x0 + 0.5, r.y0, g.r.x1 - 0.5, r.y1});
                    }
                }
                std::vector<BlockRect> fore;
                for (const BlockPlanFeature& g : plan.features)
                    if (g.kind == BlockFeatureKind::Forecourt && g.r.overlaps(r)) fore.push_back(g.r);
                for (double x = r.x0 + 3.0; x <= r.x1 - 2.5; x += 5.0)
                    for (double y = r.y0 + 3.0; y <= r.y1 - 2.5; y += 5.0)
                        if (!inAny(keep, x, y)) add(inAny(fore, x, y) ? "forecourt" : "precinct", f.id, x, y, 9.0, 8.0);
                // canopy share of the precinct (1 m samples)
                std::size_t in = 0, all = 0;
                for (double x = r.x0 + 0.5; x < r.x1; x += 1.0)
                    for (double y = r.y0 + 0.5; y < r.y1; y += 1.0) {
                        ++all;
                        for (const BlockTree& t : out.trees)
                            if ((t.kind == "precinct" || t.kind == "forecourt") && std::hypot(t.x - x, t.y - y) <= t.canopy * 0.5) {
                                ++in;
                                break;
                            }
                    }
                st.notes.push_back(f.id + ": canopy " + fmt("%.0f", 100.0 * static_cast<double>(in) / static_cast<double>(all ? all : 1)) + "%");
            }
        }
    }

    // r10 dressing, so the features read from above: roof gardens on bare podiums, market stalls, the precinct gate's
    // roof and path in, the park's cross paths
    {
        int n = 0, m = 0;
        auto flat = [&](const std::string& id, const std::string& row, const char* kind, const BlockRect& r, double lift,
                        const char* note) {
            BlockBox b = makeBox(id, row, kind, r);
            standBox(ground, b, lift, 0.2);
            b.note = note;
            out.boxes.push_back(b);
            ++st.marks;
        };
        // podium roofs: park trees in planters where no tower stands and the bare run is over kBlockApronMax m
        for (const BlockPlanFeature& f : plan.features) {
            if (f.kind != BlockFeatureKind::Podium) continue;
            const BlockBox* pod = nullptr;
            for (const BlockBox& b : out.boxes)
                if (b.id == "R9-F-" + f.id) pod = &b;
            if (!pod) continue;
            const BlockRect r = pod->rect();
            const double tw = f.width;
            const double run = tw > 0.0 ? std::max(r.w() - tw, r.h() - tw) * 0.5 : std::max(r.w(), r.h());
            if (run <= kBlockApronMax) continue;
            const BlockRect keep{pod->cx - tw * 0.5 - 2.0, pod->cy - tw * 0.5 - 2.0, pod->cx + tw * 0.5 + 2.0, pod->cy + tw * 0.5 + 2.0};
            const double z = pod->z1;
            const std::string host = pod->id;
            std::size_t planted = 0;
            for (double x = r.x0 + 3.5; x <= r.x1 - 3.0; x += 7.0)
                for (double y = r.y0 + 3.5; y <= r.y1 - 3.0; y += 7.0) {
                    if (tw > 0.0 && keep.contains(x, y)) continue;
                    BlockTree tr;
                    tr.id = "R10-TR-" + std::to_string(++n);
                    tr.kind = "park";
                    tr.host = host;
                    tr.x = x, tr.y = y, tr.z = z, tr.height = 5.0, tr.canopy = 4.5;
                    out.trees.push_back(tr);
                    ++planted;
                }
            st.roofTrees += planted;
            st.notes.push_back(f.id + ": a roof garden of " + std::to_string(planted) + " trees on a " + fmt("%.0f", run) +
                               " m bare roof");
        }
        // market stalls along a market lane's footways, every 5 m, clear of crossings and doors
        for (const BlockPlanStreet& s : out.streets) {
            if (s.cls != BlockClass::Market) continue;
            const bool ax = s.y0 == s.y1;
            const double a0 = ax ? std::min(s.x0, s.x1) : std::min(s.y0, s.y1), a1 = ax ? std::max(s.x0, s.x1) : std::max(s.y0, s.y1);
            const double c = ax ? s.y0 : s.x0;
            const double foot = s.pavement > 0.0 ? s.pavement : 1.5;
            const double off = s.width * 0.5 - foot * 0.5; // the footway's middle
            const double deep = std::min(1.2, foot - 0.2), len = 2.4;
            for (int side = -1; side <= 1; side += 2)
                for (double a = a0 + 4.0; a + len <= a1 - 2.0; a += 5.0) {
                    const double mid = a + len * 0.5, w = c + side * off;
                    const double mx = ax ? mid : w, my = ax ? w : mid;
                    bool clear = true;
                    for (const BlockPlanStreet& o : out.streets)
                        if (&o != &s && streetRect(o).overlaps({mx - 3.0, my - 3.0, mx + 3.0, my + 3.0})) clear = false;
                    for (const BlockPlanStreet& o : plan.streets)
                        if (o.id != s.id && streetRect(o).overlaps({mx - 3.0, my - 3.0, mx + 3.0, my + 3.0})) clear = false;
                    for (const BlockDoor& d : out.doors)
                        if (std::hypot(d.ox - mx, d.oy - my) < 2.5) clear = false;
                    if (!clear) continue;
                    const BlockRect r = ax ? BlockRect{a, w - deep * 0.5, a + len, w + deep * 0.5}
                                           : BlockRect{w - deep * 0.5, a, w + deep * 0.5, a + len};
                    flat("R10-MS-" + std::to_string(++m), s.id, "stand", r, 2.4, "R10 market stall market=1");
                    ++st.stalls;
                    --st.marks;
                }
        }
        // the precinct gates: a plank roof over the gate and a pale path in to the hall
        for (const BlockPlanFeature& g : plan.features) {
            if (g.kind != BlockFeatureKind::Gate) continue;
            const BlockPlanFeature* pr = nullptr;
            for (const BlockPlanFeature& f : plan.features)
                if (f.kind == BlockFeatureKind::Precinct && g.r.overlaps({f.r.x0 - 1, f.r.y0 - 1, f.r.x1 + 1, f.r.y1 + 1})) pr = &f;
            if (!pr) continue;
            const bool longY = g.r.h() >= g.r.w();
            const BlockRect roof = longY ? BlockRect{(g.r.x0 + g.r.x1) * 0.5 - 1.6, g.r.y0 - 1.0, (g.r.x0 + g.r.x1) * 0.5 + 1.6, g.r.y1 + 1.0}
                                         : BlockRect{g.r.x0 - 1.0, (g.r.y0 + g.r.y1) * 0.5 - 1.6, g.r.x1 + 1.0, (g.r.y0 + g.r.y1) * 0.5 + 1.6};
            BlockBox rb = makeBox("R10-GR-" + g.id, g.id, "deck", roof);
            sampleGround(ground, rb);
            rb.z0 = rb.ground + 6.1;
            rb.z1 = rb.ground + 6.5;
            rb.note = "R10 gate roof precinct_gate=1";
            out.boxes.push_back(rb);
            ++st.marks;
            // the path: from the gate to the nearest hall's face (or the precinct's far side)
            const BlockRect& R = pr->r;
            double far = longY ? ((g.r.x0 + g.r.x1) * 0.5 > (R.x0 + R.x1) * 0.5 ? R.x0 + 0.3 : R.x1 - 0.3)
                               : ((g.r.y0 + g.r.y1) * 0.5 > (R.y0 + R.y1) * 0.5 ? R.y0 + 0.3 : R.y1 - 0.3);
            for (const BlockPlanFeature& h : plan.features)
                if (h.kind == BlockFeatureKind::Hall && h.r.overlaps(R)) {
                    if (longY) far = (g.r.x0 > h.r.x1) ? h.r.x1 : h.r.x0;
                    else far = (g.r.y0 > h.r.y1) ? h.r.y1 : h.r.y0;
                }
            const BlockRect path = longY ? BlockRect{std::min(far, g.r.x0), g.r.y0 + 1.0, std::max(far, g.r.x1), g.r.y1 - 1.0}
                                         : BlockRect{g.r.x0 + 1.0, std::min(far, g.r.y0), g.r.x1 - 1.0, std::max(far, g.r.y1)};
            flat("R10-GP-" + g.id, pr->id, "marking", path, 0.2, "R10 precinct path precinct_gate=1");
        }
        // parks: two pale cross paths
        for (const BlockPlanFeature& f : plan.features) {
            if (f.kind != BlockFeatureKind::Park) continue;
            const BlockRect& r = f.r;
            const double cx = (r.x0 + r.x1) * 0.5, cy = (r.y0 + r.y1) * 0.5;
            flat("R10-PP-" + f.id + "-X", f.id, "marking", {r.x0, cy - 0.75, r.x1, cy + 0.75}, 0.2, "R10 park path park=1");
            flat("R10-PP-" + f.id + "-Y", f.id, "marking", {cx - 0.75, r.y0, cx + 0.75, cy - 0.75}, 0.2, "R10 park path park=1");
            flat("R10-PP-" + f.id + "-Z", f.id, "marking", {cx - 0.75, cy + 0.75, cx + 0.75, r.y1}, 0.2, "R10 park path park=1");
        }
    }

    // the landmark
    for (const BlockPlanLandmark& l : plan.landmarks) {
        BlockTower t;
        t.id = "R9-LM-" + l.id;
        t.kind = "landmark";
        t.cx = l.x, t.cy = l.y;
        t.z0 = ground.at(l.x, l.y);
        t.sx = t.sy = l.base;
        t.height = l.height;
        t.note = "placed";
        out.towers.push_back(t);
    }

    // districts: each box takes its band's or feature's district (default: that row's own id)
    static constexpr const char* kColours[] = {"#B8473A", "#2F6E73", "#C99A2E", "#5B4B8A", "#3E7D3A", "#8A8F98"};
    static constexpr const char* kSigns[] = {"vertical_blade", "fascia_band", "lantern_row", "painted_wall", "neon_box", "hanging_board"};
    static constexpr const char* kPaving[] = {"granite_sett", "brick_herringbone", "concrete_slab", "basalt_cobble", "terracotta_tile", "asphalt_patch"};
    // a district value "name@k" (k 0..5) takes palette slot k, so a second run's wards don't repeat the first's cues
    auto slotOf = [](const std::string& v, std::string& id) -> int {
        const std::size_t at = v.rfind('@');
        id = v;
        if (at == std::string::npos || at + 2 != v.size() || v[at + 1] < '0' || v[at + 1] > '5') return -1;
        id = v.substr(0, at);
        return v[at + 1] - '0';
    };
    auto districtOf = [&](const std::string& row) -> std::string {
        std::string v, id;
        for (const BlockPlanBand& b : plan.bands)
            if (b.id == row) v = b.district.empty() ? b.id : b.district;
        for (const BlockPlanFeature& f : plan.features)
            if (v.empty() && f.id == row) v = f.district.empty() ? f.id : f.district;
        if (v.empty()) return {};
        slotOf(v, id);
        return id;
    };
    auto addDistrict = [&](const std::string& value) {
        std::string id;
        const int slot = slotOf(value, id);
        for (const BlockDistrict& d : out.districts)
            if (d.id == id) return;
        const std::size_t n = slot >= 0 ? static_cast<std::size_t>(slot) : out.districts.size() % 6;
        out.districts.push_back({id, kColours[n], kSigns[n], kPaving[n], 0});
    };
    for (const BlockPlanBand& b : plan.bands) addDistrict(b.district.empty() ? b.id : b.district);
    for (const BlockPlanFeature& f : plan.features) addDistrict(f.district.empty() ? f.id : f.district);
    for (BlockBox& b : out.boxes) {
        b.district = districtOf(b.row);
        if (b.district.empty()) continue;
        b.note += (b.note.empty() ? "district=" : " district=") + b.district;
        if (b.building && b.kind == "kit")
            for (BlockDistrict& d : out.districts)
                if (d.id == b.district) ++d.lots;
    }
    if (out.districts.size() > 6) st.notes.push_back("over 6 districts: the cue palettes repeat");
    // road-class plan streets too short to read as a main street
    for (const BlockPlanStreet& s : plan.streets) {
        const double len = std::hypot(s.x1 - s.x0, s.y1 - s.y0);
        if (s.cls == BlockClass::Road && len < kBlockMainStraight)
            st.notes.push_back(s.id + ": a road only " + fmt("%.0f", len) + " m long (a main street runs " +
                               fmt("%.0f", kBlockMainStraight) + " m straight)");
    }
}

// ---- route tables ----

BlockRole blockStreetRole(const MassingStreet& s) {
    if (s.pavement > 0.0 && s.width >= kBlockMainWidth - 1e-9) return BlockRole::Main;
    if (s.kind != "alley" && s.width > kBlockAlleyMax + 1e-9) return BlockRole::Lane;
    return BlockRole::Alley;
}

std::string_view blockRoleName(BlockRole r) {
    switch (r) {
    case BlockRole::Main: return "road"; // the class names: road | lane | alley
    case BlockRole::Lane: return "lane";
    case BlockRole::Alley: return "alley";
    }
    return "alley";
}

double blockStraightRun(const MassingStreet& s) {
    double best = 0.0, run = 0.0, px = 0.0, py = 0.0;
    bool have = false;
    const double cosTurn = std::cos(kBlockTurnDeg * 3.14159265358979323846 / 180.0);
    for (std::size_t i = 1; i < s.points.size(); ++i) {
        const double dx = s.points[i].x - s.points[i - 1].x, dy = s.points[i].y - s.points[i - 1].y;
        const double l = std::hypot(dx, dy);
        if (l < 1e-9) continue;
        const double ux = dx / l, uy = dy / l;
        if (have && ux * px + uy * py >= cosTurn) run += l;
        else run = l;
        px = ux, py = uy, have = true;
        best = std::max(best, run);
    }
    return best;
}

namespace {

double polyLength(const MassingStreet& s) {
    double l = 0.0;
    for (std::size_t i = 1; i < s.points.size(); ++i)
        l += std::hypot(s.points[i].x - s.points[i - 1].x, s.points[i].y - s.points[i - 1].y);
    return l;
}

bool marketKind(const std::string& k) { return k.find("market") != std::string::npos; }

} // namespace

bool blockStreetNearRect(const MassingStreet& s, const BlockRect& r, double g) {
    const BlockRect w{r.x0 - g, r.y0 - g, r.x1 + g, r.y1 + g};
    if (s.points.size() == 1) return w.contains(s.points[0].x, s.points[0].y);
    for (std::size_t i = 1; i < s.points.size(); ++i) {
        const double ax = s.points[i - 1].x, ay = s.points[i - 1].y, bx = s.points[i].x, by = s.points[i].y;
        const int n = std::max(1, static_cast<int>(std::ceil(std::hypot(bx - ax, by - ay) / 2.0)));
        for (int k = 0; k <= n; ++k) {
            const double f = static_cast<double>(k) / n;
            if (w.contains(ax + f * (bx - ax), ay + f * (by - ay))) return true;
        }
    }
    return false;
}

void blockRouteTables(const std::vector<MassingStreet>& streets, const std::vector<MassingBox>& boxes,
                      const BlockRect& rect, const BlockGround& ground, std::vector<BlockStreetFlag>& flags,
                      std::vector<BlockPlate>& plates) {
    std::vector<const MassingStreet*> mine;
    for (const MassingStreet& s : streets)
        if (!s.points.empty() && blockStreetNearRect(s, rect, 20.0)) mine.push_back(&s);
    for (const MassingStreet* s : mine) {
        BlockStreetFlag f;
        f.id = s->id;
        f.kind = s->kind;
        const BlockRole r = blockStreetRole(*s);
        f.main = r == BlockRole::Main;
        f.role = f.main ? "road" : (r == BlockRole::Lane ? (marketKind(s->kind) ? "market" : "lane") : "alley");
        f.width = s->width;
        f.length = polyLength(*s);
        f.straight = blockStraightRun(*s);
        if (f.main) {
            f.treeEvery = 10.0, f.lampEvery = 25.0, f.lampKind = "mast", f.signsPer100 = 30.0, f.awning = true;
            f.mouth = "junction";
        } else if (f.role == "market") {
            f.lampEvery = 12.0, f.lampKind = "lantern_string", f.signsPer100 = 40.0, f.awning = true;
            f.mouth = "dropped_kerb";
        } else if (f.role == "lane") {
            f.lampEvery = 30.0, f.lampKind = "wall", f.signsPer100 = 10.0;
            f.mouth = "dropped_kerb";
        } else {
            f.lampKind = "none", f.signsPer100 = 2.0;
            f.mouth = "bollards";
        }
        flags.push_back(f);
    }
    // buildings that may carry a wall plate
    std::vector<const MassingBox*> bld;
    const BlockRect wide{rect.x0 - 30.0, rect.y0 - 30.0, rect.x1 + 30.0, rect.y1 + 30.0};
    for (const MassingBox& b : boxes) {
        if ((b.kind != "kit" && b.kind != "box" && b.kind != "slab") || b.z1 - b.z0 < 2.5) continue;
        if (blockFootprint(b).overlaps(wide, 0.0)) bld.push_back(&b);
    }
    auto onBuilding = [&](double x, double y) {
        for (const MassingBox* b : bld) {
            const double rr = b->rot * 3.14159265358979323846 / 180.0;
            const double c = std::cos(rr), sn = std::sin(rr), dx = x - b->cx, dy = y - b->cy;
            if (std::fabs(dx * c + dy * sn) <= b->sx * 0.5 + 0.3 && std::fabs(-dx * sn + dy * c) <= b->sy * 0.5 + 0.3)
                return true;
        }
        return false;
    };
    auto onAnyStreet = [&](double x, double y) {
        for (const MassingStreet* s : mine)
            if (blockOnStreet(*s, x, y)) return true;
        return false;
    };
    std::size_t n = 0;
    const std::size_t firstPlate = plates.size();
    const BlockRect near20{rect.x0 - 20.0, rect.y0 - 20.0, rect.x1 + 20.0, rect.y1 + 20.0};
    const BlockRect near1{rect.x0 - 1.0, rect.y0 - 1.0, rect.x1 + 1.0, rect.y1 + 1.0};
    for (std::size_t i = 0; i < mine.size(); ++i)
        for (std::size_t j = i + 1; j < mine.size(); ++j) {
            const MassingStreet* a = mine[i];
            const MassingStreet* b = mine[j];
            const BlockRole ra = blockStreetRole(*a), rb = blockStreetRole(*b);
            if (ra == BlockRole::Alley && rb == BlockRole::Alley) continue;
            if (static_cast<int>(ra) < static_cast<int>(rb)) std::swap(a, b); // a: the minor street
            for (std::size_t si = 1; si < a->points.size(); ++si)
                for (std::size_t sj = 1; sj < b->points.size(); ++sj) {
                    const double ax = a->points[si - 1].x, ay = a->points[si - 1].y;
                    const double adx = a->points[si].x - ax, ady = a->points[si].y - ay;
                    const double bx = b->points[sj - 1].x, by = b->points[sj - 1].y;
                    const double bdx = b->points[sj].x - bx, bdy = b->points[sj].y - by;
                    const double la = std::hypot(adx, ady), lb = std::hypot(bdx, bdy);
                    const double den = adx * bdy - ady * bdx;
                    if (la < 1e-9 || lb < 1e-9 || std::fabs(den) < 1e-9 * la * lb) continue;
                    const double ta = ((bx - ax) * bdy - (by - ay) * bdx) / den;
                    const double tb = ((bx - ax) * ady - (by - ay) * adx) / den;
                    // a lane mouth stops short of the main street's centreline: reach over its half width
                    const double ga = (b->width * 0.5 + 2.0) / la, gb = (a->width * 0.5 + 2.0) / lb;
                    if (ta < -ga || ta > 1.0 + ga || tb < -gb || tb > 1.0 + gb) continue;
                    const double jx = ax + ta * adx, jy = ay + ta * ady;
                    if (!near20.contains(jx, jy)) continue; // a mouth on a street just outside the slice
                    bool dup = false;
                    for (std::size_t k = firstPlate; k < plates.size(); ++k)
                        if (plates[k].a == a->id && plates[k].b == b->id &&
                            std::hypot(plates[k].x - jx, plates[k].y - jy) < a->width + b->width + 6.0)
                            dup = true;
                    if (dup) continue;
                    const double uax = adx / la, uay = ady / la, ubx = bdx / lb, uby = bdy / lb;
                    const double oa = b->width * 0.5 + 0.15, ob = a->width * 0.5 + 0.15;
                    double bestX = 0.0, bestY = 0.0;
                    int bestScore = -1;
                    for (int sa = -1; sa <= 1; sa += 2)
                        for (int sb = -1; sb <= 1; sb += 2) {
                            const double cx = jx + sa * uax * oa + sb * ubx * ob;
                            const double cy = jy + sa * uay * oa + sb * uby * ob;
                            if (!near1.contains(cx, cy)) continue;
                            // a corner the minor street really reaches (its centreline runs past it)
                            const double along = (cx - ax) * uax + (cy - ay) * uay;
                            const bool real = along >= -1.0 && along <= la + 1.0;
                            const int score = (onBuilding(cx, cy) ? 4 : (onAnyStreet(cx, cy) ? 0 : 2)) + (real ? 1 : 0);
                            if (score > bestScore) bestScore = score, bestX = cx, bestY = cy;
                        }
                    if (bestScore < 0) continue;
                    bool taken = false; // one plate per corner (a link meeting a road where a lane does)
                    for (std::size_t k = firstPlate; k < plates.size(); ++k)
                        if (std::hypot(plates[k].x - bestX, plates[k].y - bestY) < 2.0) taken = true;
                    if (taken) continue;
                    BlockPlate p;
                    p.id = "R10-NP-" + std::to_string(++n);
                    p.a = a->id;
                    p.b = b->id;
                    p.x = bestX, p.y = bestY;
                    p.z = ground.at(p.x, p.y);
                    p.mount = bestScore >= 4 ? "wall" : "pole";
                    p.rot = std::atan2(uay, uax) * 180.0 / 3.14159265358979323846;
                    plates.push_back(p);
                }
        }
}

void blockPlacePlates(const std::vector<BlockPlanFeature>& features, const std::vector<MassingStreet>& streets,
                      const BlockGround& ground, std::vector<BlockPlate>& plates) {
    auto segDist = [](double px, double py, double ax, double ay, double bx, double by) {
        const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
        double t = l2 > 0.0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        return std::hypot(px - ax - t * dx, py - ay - t * dy);
    };
    int n = 0;
    for (const BlockPlanFeature& f : features) {
        if (f.kind != BlockFeatureKind::Park && f.kind != BlockFeatureKind::Plaza && f.kind != BlockFeatureKind::Precinct) continue;
        const BlockRect& r = f.r;
        const double cx = (r.x0 + r.x1) * 0.5, cy = (r.y0 + r.y1) * 0.5;
        // the side midpoints, each 1 m in, and the way along the side
        const double sx[4] = {cx, cx, r.x0 + 1.0, r.x1 - 1.0}, sy[4] = {r.y0 + 1.0, r.y1 - 1.0, cy, cy};
        const double ex[4] = {cx, cx, r.x0, r.x1}, ey[4] = {r.y0, r.y1, cy, cy};
        int best = -1;
        double bestD = 0.0;
        for (int k = 0; k < 4; ++k) {
            double d = 1e18;
            for (const MassingStreet& s : streets)
                for (std::size_t i = 1; i < s.points.size(); ++i)
                    d = std::min(d, segDist(ex[k], ey[k], s.points[i - 1].x, s.points[i - 1].y, s.points[i].x, s.points[i].y) -
                                        s.width * 0.5);
            if (best < 0 || d < bestD) best = k, bestD = d;
        }
        BlockPlate p;
        p.id = "R10-PL-" + std::to_string(++n);
        p.a = f.id;
        p.b = std::string(blockFeatureName(f.kind));
        p.mount = "pole";
        p.x = sx[best], p.y = sy[best];
        p.z = ground.at(p.x, p.y);
        p.rot = best < 2 ? 0.0 : 90.0;
        plates.push_back(p);
    }
}

} // namespace ftcore
