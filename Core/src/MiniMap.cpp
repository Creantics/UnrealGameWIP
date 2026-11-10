#include "ftcore/MiniMap.h"

#include "ftcore/MassingPlan.h"
#include "ftcore/MassingProps.h"
#include "ftcore/Roads.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ftcore {
namespace minimap_detail {

constexpr float kPi = 3.14159265358979323846f;

std::string_view trimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool isCommentLine(std::string_view line) {
    line = trimView(line);
    return line.empty() || line.front() == '#' || (line.size() > 1 && line[0] == '"' && line[1] == '#');
}

bool parseDouble(std::string_view s, double& out) {
    s = trimView(s);
    if (s.empty() || s.size() >= 64) return false;
    char buf[64];
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    char* end = nullptr;
    out = std::strtod(buf, &end);
    return end == buf + s.size() && std::isfinite(out);
}

// Splits on commas (the tables read here carry no quoted fields).
void splitFields(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            out.push_back(trimView(line.substr(start, i - start)));
            start = i + 1;
        }
    }
}

// A header-named CSV table: calls row(fields) for each data row. False, with the reason, when a named column is missing.
struct Table {
    std::vector<std::string_view> header;
    std::vector<std::string_view> fields;
    int col(std::string_view name) const {
        for (std::size_t i = 0; i < header.size(); ++i)
            if (header[i] == name) return static_cast<int>(i);
        return -1;
    }
    std::string_view at(int c) const {
        return c >= 0 && static_cast<std::size_t>(c) < fields.size() ? fields[static_cast<std::size_t>(c)]
                                                                       : std::string_view{};
    }
};

template <class Row>
bool eachRow(std::string_view csv, Table& t, Row&& row) {
    bool haveHeader = false;
    std::size_t pos = 0;
    while (pos <= csv.size()) {
        std::size_t nl = csv.find('\n', pos);
        if (nl == std::string_view::npos) nl = csv.size();
        const std::string_view line = csv.substr(pos, nl - pos);
        pos = nl + 1;
        if (isCommentLine(line)) continue;
        if (!haveHeader) {
            splitFields(line, t.header);
            haveHeader = true;
            continue;
        }
        splitFields(line, t.fields);
        if (!row(t)) return false;
        if (nl == csv.size()) break;
    }
    return true;
}

bool fail(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
    return false;
}

Vec2 engine(double side, double x, double y) {
    return {static_cast<float>(x - side * 0.5), static_cast<float>(side * 0.5 - y)};
}

// "x:y[:z];x:y[:z];..." in the plan frame, appended as engine points.
bool parsePoints(std::string_view pts, double side, std::vector<Vec2>& out, std::size_t& count) {
    count = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= pts.size(); ++i) {
        if (i != pts.size() && pts[i] != ';') continue;
        const std::string_view p = trimView(pts.substr(start, i - start));
        start = i + 1;
        if (p.empty()) continue;
        const std::size_t c1 = p.find(':');
        if (c1 == std::string_view::npos) return false;
        std::size_t c2 = p.find(':', c1 + 1);
        if (c2 == std::string_view::npos) c2 = p.size();
        double x = 0.0, y = 0.0;
        if (!parseDouble(p.substr(0, c1), x) || !parseDouble(p.substr(c1 + 1, c2 - c1 - 1), y)) return false;
        out.push_back(engine(side, x, y));
        ++count;
    }
    return count > 0;
}

Vec2 rotate(Vec2 p, float c, float s) { return {p.x * c - p.y * s, p.x * s + p.y * c}; }

float segDistance(Vec2 p, Vec2 a, Vec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    const float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
    return std::sqrt(ex * ex + ey * ey);
}

// Blends colour c over pixel p by coverage a (0..1).
void blend(std::uint32_t& p, std::uint32_t c, float a) {
    if (a <= 0.f) return;
    if (a >= 1.f) {
        p = c;
        return;
    }
    std::uint32_t r = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        const float d = static_cast<float>((p >> sh) & 0xFFu), s = static_cast<float>((c >> sh) & 0xFFu);
        r |= static_cast<std::uint32_t>(d + (s - d) * a + 0.5f) << sh;
    }
    p = r;
}

void fillRect(MapImage& img, const MapRect& r, std::uint32_t colour) {
    const float c = std::cos(r.rot), s = std::sin(r.rot);
    const float ex = std::abs(r.hx * c) + std::abs(r.hy * s), ey = std::abs(r.hx * s) + std::abs(r.hy * c);
    const float inv = 1.f / img.mPerPx;
    const int i0 = std::max(0, static_cast<int>(std::floor((r.cx - ex - img.x0) * inv)));
    const int i1 = std::min(static_cast<int>(img.w) - 1, static_cast<int>(std::floor((r.cx + ex - img.x0) * inv)));
    const int j0 = std::max(0, static_cast<int>(std::floor((r.cy - ey - img.y0) * inv)));
    const int j1 = std::min(static_cast<int>(img.h) - 1, static_cast<int>(std::floor((r.cy + ey - img.y0) * inv)));
    const float soft = 0.5f * img.mPerPx; // half a pixel of edge
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const float wx = img.x0 + (static_cast<float>(i) + 0.5f) * img.mPerPx - r.cx;
            const float wy = img.y0 + (static_cast<float>(j) + 0.5f) * img.mPerPx - r.cy;
            const float u = wx * c + wy * s, v = -wx * s + wy * c; // into the rectangle's own axes
            const float out = std::max(std::abs(u) - r.hx, std::abs(v) - r.hy);
            const float a = std::clamp((soft - out) / (2.f * soft), 0.f, 1.f);
            blend(img.px[static_cast<std::size_t>(j) * img.w + static_cast<std::size_t>(i)], colour, a);
        }
    }
}

void drawCapsule(MapImage& img, Vec2 a, Vec2 b, float radiusM, std::uint32_t colour) {
    const float inv = 1.f / img.mPerPx;
    const float reach = radiusM + img.mPerPx;
    const int i0 = std::max(0, static_cast<int>(std::floor((std::min(a.x, b.x) - reach - img.x0) * inv)));
    const int i1 = std::min(static_cast<int>(img.w) - 1, static_cast<int>(std::floor((std::max(a.x, b.x) + reach - img.x0) * inv)));
    const int j0 = std::max(0, static_cast<int>(std::floor((std::min(a.y, b.y) - reach - img.y0) * inv)));
    const int j1 = std::min(static_cast<int>(img.h) - 1, static_cast<int>(std::floor((std::max(a.y, b.y) + reach - img.y0) * inv)));
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const Vec2 p{img.x0 + (static_cast<float>(i) + 0.5f) * img.mPerPx, img.y0 + (static_cast<float>(j) + 0.5f) * img.mPerPx};
            const float d = segDistance(p, a, b);
            const float cov = std::clamp((radiusM - d) * inv + 0.5f, 0.f, 1.f);
            std::uint32_t& px = img.px[static_cast<std::size_t>(j) * img.w + static_cast<std::size_t>(i)];
            // Overlapping segments of one road don't stack their soft edges darker: keep the stronger cover.
            if (cov > 0.f) blend(px, colour, cov);
        }
    }
}

// Little-endian blob writing and reading.
template <class T>
void put(std::vector<std::uint8_t>& out, T v) {
    std::uint8_t b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    out.insert(out.end(), b, b + sizeof(T));
}

struct Reader {
    const std::uint8_t* p;
    std::size_t left;
    bool ok = true;
    template <class T>
    T get() {
        T v{};
        if (!ok || left < sizeof(T)) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        left -= sizeof(T);
        return v;
    }
    // A count of items of `each` bytes that must fit in what is left.
    std::uint32_t count(std::size_t each) {
        const std::uint32_t n = get<std::uint32_t>();
        if (ok && each > 0 && n > left / each) ok = false;
        return ok ? n : 0;
    }
};

constexpr std::uint32_t kMagic = 0x4D4D5446u; // "FTMM"
constexpr std::uint32_t kVersion = 1;

} // namespace minimap_detail

namespace md = minimap_detail;

bool mapRoadClass(std::string_view kind, MapRoadClass& out) {
    struct K {
        std::string_view kind;
        MapRoadClass cls;
    };
    static constexpr K kinds[] = {
        {"coast_road", MapRoadClass::Main},     {"road", MapRoadClass::Main},          {"bridge", MapRoadClass::Main},
        {"link_road", MapRoadClass::Main},      {"causeway", MapRoadClass::Main},      {"street", MapRoadClass::Street},
        {"link_street", MapRoadClass::Street},  {"market_arcade", MapRoadClass::Street}, {"core_lane", MapRoadClass::Lane},
        {"lane", MapRoadClass::Lane},           {"terrace", MapRoadClass::Lane},       {"alley", MapRoadClass::Lane},
        {"stair", MapRoadClass::Stair},         {"link_stair", MapRoadClass::Stair},
    };
    for (const K& k : kinds) {
        if (k.kind == kind) {
            out = k.cls;
            return true;
        }
    }
    return false;
}

const char* mapRoadClassName(MapRoadClass c) {
    switch (c) {
    case MapRoadClass::Main: return "main road";
    case MapRoadClass::Street: return "street";
    case MapRoadClass::Lane: return "lane";
    case MapRoadClass::Stair: return "stair";
    }
    return "";
}

bool mapIsBuildingKind(std::string_view kind) {
    return kind == "kit" || kind == "box" || kind == "slab" || kind == "shrine" || kind == "terrace";
}

namespace minimap_detail {

bool readRoads(std::string_view csv, double side, MiniMapModel& m, std::string* error, const char* table) {
    Table t;
    std::string err;
    const bool ok = eachRow(csv, t, [&](const Table& tb) {
        const int cId = tb.col("id"), cKind = tb.col("kind"), cWidth = tb.col("width_m"), cPts = tb.col("pts");
        if (cId < 0 || cKind < 0 || cWidth < 0 || cPts < 0) {
            err = std::string(table) + ": needs the columns id, kind, width_m and pts";
            return false;
        }
        const std::string_view id = tb.at(cId), kind = tb.at(cKind);
        if (kind == "canal") {
            const std::size_t first = m.points.size();
            std::size_t n = 0;
            if (!parsePoints(tb.at(cPts), side, m.points, n)) {
                err = std::string(table) + ": bad pts on " + std::string(id);
                return false;
            }
            Vec2 lo = m.points[first], hi = m.points[first];
            for (std::size_t i = first; i < m.points.size(); ++i) {
                lo = {std::min(lo.x, m.points[i].x), std::min(lo.y, m.points[i].y)};
                hi = {std::max(hi.x, m.points[i].x), std::max(hi.y, m.points[i].y)};
            }
            m.points.resize(first);
            m.waterRects.push_back({(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, 0.f});
            return true;
        }
        MapRoadClass cls{};
        if (!mapRoadClass(kind, cls)) return true; // a kind the map leaves out
        double width = 0.0;
        if (!parseDouble(tb.at(cWidth), width) || width <= 0.0) {
            err = std::string(table) + ": bad width_m on " + std::string(id);
            return false;
        }
        MapRoad r;
        r.id = std::string(id);
        r.cls = cls;
        r.width = static_cast<float>(width);
        r.first = static_cast<std::uint32_t>(m.points.size());
        std::size_t n = 0;
        if (!parsePoints(tb.at(cPts), side, m.points, n)) {
            err = std::string(table) + ": bad pts on " + r.id;
            return false;
        }
        r.count = static_cast<std::uint32_t>(n);
        m.roads.push_back(std::move(r));
        return true;
    });
    return ok || fail(error, err);
}

} // namespace minimap_detail

bool buildMiniMap(const MiniMapSource& src, MiniMapModel& out, std::string* error) {
    out = MiniMapModel{};
    if (!(src.side > 0.0) || !(src.areaX1 > src.areaX0) || !(src.areaY1 > src.areaY0))
        return md::fail(error, "minimap: needs the plan side and an area");
    const double side = src.side;
    const Vec2 a = md::engine(side, src.areaX0, src.areaY1), b = md::engine(side, src.areaX1, src.areaY0);
    out.minX = a.x;
    out.minY = a.y;
    out.maxX = b.x;
    out.maxY = b.y;
    if (!src.roads.empty() && !md::readRoads(src.roads, side, out, error, "roads")) return false;
    if (!src.streets.empty() && !md::readRoads(src.streets, side, out, error, "streets")) return false;

    auto inArea = [&](Vec2 p, float pad) {
        return p.x >= out.minX - pad && p.x <= out.maxX + pad && p.y >= out.minY - pad && p.y <= out.maxY + pad;
    };
    if (!src.boxes.empty()) {
        std::vector<MassingBox> boxes;
        if (!importMassingBoxes(src.boxes, boxes, error)) return false;
        for (const MassingBox& bx : boxes) {
            if (!mapIsBuildingKind(bx.kind) || bx.z1 - bx.z0 < kMapBuildingMinHeight) continue;
            const Vec2 c = md::engine(side, bx.cx, bx.cy);
            if (!inArea(c, 50.f)) continue;
            out.buildings.push_back({c.x, c.y, static_cast<float>(bx.sx * 0.5), static_cast<float>(bx.sy * 0.5),
                                     static_cast<float>(-bx.rot) * md::kPi / 180.f});
        }
    }
    if (!src.towers.empty()) {
        md::Table t;
        std::string err;
        const bool ok = md::eachRow(src.towers, t, [&](const md::Table& tb) {
            double cx = 0, cy = 0, sx = 0, sy = 0, rot = 0;
            if (!md::parseDouble(tb.at(tb.col("cx")), cx) || !md::parseDouble(tb.at(tb.col("cy")), cy) ||
                !md::parseDouble(tb.at(tb.col("sx")), sx) || !md::parseDouble(tb.at(tb.col("sy")), sy)) {
                err = "towers: bad row " + std::string(tb.at(tb.col("id")));
                return false;
            }
            if (!md::parseDouble(tb.at(tb.col("rot")), rot)) rot = 0.0;
            const Vec2 c = md::engine(side, cx, cy);
            if (tb.at(tb.col("kind")) == "landmark") {
                out.hasLandmark = true;
                out.landmark = c;
            }
            out.buildings.push_back({c.x, c.y, static_cast<float>(sx * 0.5), static_cast<float>(sy * 0.5),
                                     static_cast<float>(-rot) * md::kPi / 180.f});
            return true;
        });
        if (!ok) return md::fail(error, err);
    }
    if (!src.doors.empty()) {
        std::vector<MassingFoot> feet;
        if (!importRoadDoors(src.doors, 0.0, feet, error)) return false;
        for (const MassingFoot& f : feet) {
            const Vec2 c = md::engine(side, f.x, f.y);
            if (inArea(c, 0.f)) out.doors.push_back(c);
        }
    }
    if (src.height && src.samplesX > 1 && src.samplesY > 1 && src.waterCell > 0.f && src.spacing > 0.0) {
        out.waterCell = src.waterCell;
        out.waterW = static_cast<std::uint32_t>(std::ceil((out.maxX - out.minX) / src.waterCell));
        out.waterH = static_cast<std::uint32_t>(std::ceil((out.maxY - out.minY) / src.waterCell));
        out.water.assign(static_cast<std::size_t>(out.waterW) * out.waterH, 0);
        for (std::uint32_t j = 0; j < out.waterH; ++j) {
            for (std::uint32_t i = 0; i < out.waterW; ++i) {
                const double ex = out.minX + (i + 0.5) * src.waterCell, ey = out.minY + (j + 0.5) * src.waterCell;
                const double px = ex + side * 0.5, py = side * 0.5 - ey;
                if (px < src.seaFromX) continue;
                const long ix = std::lround((px - src.originX) / src.spacing), iy = std::lround((py - src.originY) / src.spacing);
                if (ix < 0 || iy < 0 || static_cast<std::size_t>(ix) >= src.samplesX || static_cast<std::size_t>(iy) >= src.samplesY)
                    continue;
                const double h = (static_cast<double>(src.height[static_cast<std::size_t>(iy) * src.samplesX + static_cast<std::size_t>(ix)]) - 32768.0) / 128.0;
                if (h < 0.0) out.water[static_cast<std::size_t>(j) * out.waterW + i] = 1;
            }
        }
    }
    mapBuildGrid(out);
    return true;
}

void mapBuildGrid(MiniMapModel& m, float gridCell) {
    m.gridCell = gridCell;
    m.gridW = static_cast<std::uint32_t>(std::max(1.f, std::ceil((m.maxX - m.minX) / gridCell)));
    m.gridH = static_cast<std::uint32_t>(std::max(1.f, std::ceil((m.maxY - m.minY) / gridCell)));
    m.grid.assign(static_cast<std::size_t>(m.gridW) * m.gridH, -1);
    for (std::size_t ri = 0; ri < m.roads.size(); ++ri) {
        const MapRoad& r = m.roads[ri];
        const float reach = r.width * 0.5f;
        for (std::uint32_t k = 0; k < r.count; ++k) {
            const Vec2 pa = m.points[r.first + k];
            const Vec2 pb = m.points[r.first + std::min(k + 1, r.count - 1)];
            const int i0 = std::max(0, static_cast<int>(std::floor((std::min(pa.x, pb.x) - reach - m.minX) / gridCell)));
            const int i1 = std::min(static_cast<int>(m.gridW) - 1, static_cast<int>(std::floor((std::max(pa.x, pb.x) + reach - m.minX) / gridCell)));
            const int j0 = std::max(0, static_cast<int>(std::floor((std::min(pa.y, pb.y) - reach - m.minY) / gridCell)));
            const int j1 = std::min(static_cast<int>(m.gridH) - 1, static_cast<int>(std::floor((std::max(pa.y, pb.y) + reach - m.minY) / gridCell)));
            for (int j = j0; j <= j1; ++j) {
                for (int i = i0; i <= i1; ++i) {
                    const Vec2 c{m.minX + (static_cast<float>(i) + 0.5f) * gridCell, m.minY + (static_cast<float>(j) + 0.5f) * gridCell};
                    if (md::segDistance(c, pa, pb) > reach) continue;
                    std::int32_t& cell = m.grid[static_cast<std::size_t>(j) * m.gridW + static_cast<std::size_t>(i)];
                    if (cell < 0 || static_cast<int>(r.cls) < static_cast<int>(m.roads[static_cast<std::size_t>(cell)].cls))
                        cell = static_cast<std::int32_t>(ri);
                }
            }
        }
    }
}

void saveMiniMap(const MiniMapModel& m, std::vector<std::uint8_t>& out) {
    out.clear();
    md::put(out, md::kMagic);
    md::put(out, md::kVersion);
    md::put(out, m.minX);
    md::put(out, m.minY);
    md::put(out, m.maxX);
    md::put(out, m.maxY);
    md::put(out, static_cast<std::uint32_t>(m.points.size()));
    for (const Vec2& p : m.points) {
        md::put(out, p.x);
        md::put(out, p.y);
    }
    md::put(out, static_cast<std::uint32_t>(m.roads.size()));
    for (const MapRoad& r : m.roads) {
        md::put(out, static_cast<std::uint32_t>(r.id.size()));
        out.insert(out.end(), r.id.begin(), r.id.end());
        md::put(out, static_cast<std::uint8_t>(r.cls));
        md::put(out, r.width);
        md::put(out, r.first);
        md::put(out, r.count);
    }
    for (const std::vector<MapRect>* rects : {&m.buildings, &m.waterRects}) {
        md::put(out, static_cast<std::uint32_t>(rects->size()));
        for (const MapRect& r : *rects) {
            md::put(out, r.cx);
            md::put(out, r.cy);
            md::put(out, r.hx);
            md::put(out, r.hy);
            md::put(out, r.rot);
        }
    }
    md::put(out, m.waterCell);
    md::put(out, m.waterW);
    md::put(out, m.waterH);
    out.insert(out.end(), m.water.begin(), m.water.end());
    md::put(out, static_cast<std::uint32_t>(m.doors.size()));
    for (const Vec2& p : m.doors) {
        md::put(out, p.x);
        md::put(out, p.y);
    }
    md::put(out, static_cast<std::uint8_t>(m.hasLandmark ? 1 : 0));
    md::put(out, m.landmark.x);
    md::put(out, m.landmark.y);
}

bool loadMiniMap(const std::uint8_t* data, std::size_t size, MiniMapModel& out, std::string* error) {
    out = MiniMapModel{};
    md::Reader rd{data, data ? size : 0};
    if (rd.get<std::uint32_t>() != md::kMagic || !rd.ok) return md::fail(error, "minimap: not a minimap blob");
    if (rd.get<std::uint32_t>() != md::kVersion) return md::fail(error, "minimap: unknown blob version");
    out.minX = rd.get<float>();
    out.minY = rd.get<float>();
    out.maxX = rd.get<float>();
    out.maxY = rd.get<float>();
    out.points.resize(rd.count(8));
    for (Vec2& p : out.points) {
        p.x = rd.get<float>();
        p.y = rd.get<float>();
    }
    out.roads.resize(rd.count(17));
    for (MapRoad& r : out.roads) {
        const std::uint32_t n = rd.count(1);
        if (!rd.ok) break;
        r.id.assign(reinterpret_cast<const char*>(rd.p), n);
        rd.p += n;
        rd.left -= n;
        const std::uint8_t cls = rd.get<std::uint8_t>();
        if (cls >= kMapRoadClasses) rd.ok = false;
        r.cls = static_cast<MapRoadClass>(cls);
        r.width = rd.get<float>();
        r.first = rd.get<std::uint32_t>();
        r.count = rd.get<std::uint32_t>();
        if (r.count == 0 || r.first > out.points.size() || r.count > out.points.size() - r.first) rd.ok = false;
    }
    for (std::vector<MapRect>* rects : {&out.buildings, &out.waterRects}) {
        rects->resize(rd.count(20));
        for (MapRect& r : *rects) {
            r.cx = rd.get<float>();
            r.cy = rd.get<float>();
            r.hx = rd.get<float>();
            r.hy = rd.get<float>();
            r.rot = rd.get<float>();
        }
    }
    out.waterCell = rd.get<float>();
    out.waterW = rd.get<std::uint32_t>();
    out.waterH = rd.get<std::uint32_t>();
    const std::size_t cells = static_cast<std::size_t>(out.waterW) * out.waterH;
    if (!rd.ok || cells > rd.left) return md::fail(error, "minimap: truncated blob");
    out.water.assign(rd.p, rd.p + cells);
    rd.p += cells;
    rd.left -= cells;
    out.doors.resize(rd.count(8));
    for (Vec2& p : out.doors) {
        p.x = rd.get<float>();
        p.y = rd.get<float>();
    }
    out.hasLandmark = rd.get<std::uint8_t>() != 0;
    out.landmark.x = rd.get<float>();
    out.landmark.y = rd.get<float>();
    if (!rd.ok || !(out.maxX > out.minX) || !(out.maxY > out.minY)) {
        out = MiniMapModel{};
        return md::fail(error, "minimap: truncated or bad blob");
    }
    mapBuildGrid(out);
    return true;
}

void rasterMiniMap(const MiniMapModel& m, float mPerPx, MapImage& img, std::uint32_t maxSide, bool powerOfTwo) {
    const float spanX = m.maxX - m.minX, spanY = m.maxY - m.minY;
    float mpp = std::max(mPerPx, 0.01f);
    mpp = std::max({mpp, spanX / static_cast<float>(maxSide), spanY / static_cast<float>(maxSide)});
    img.mPerPx = mpp;
    img.x0 = m.minX;
    img.y0 = m.minY;
    img.w = std::max(1u, static_cast<std::uint32_t>(std::ceil(spanX / mpp)));
    img.h = std::max(1u, static_cast<std::uint32_t>(std::ceil(spanY / mpp)));
    if (powerOfTwo) {
        auto up = [maxSide](std::uint32_t n) {
            std::uint32_t p = 1;
            while (p < n && p < maxSide) p <<= 1;
            return p;
        };
        img.w = up(img.w);
        img.h = up(img.h);
        img.mPerPx = mpp = std::max({mpp, spanX / static_cast<float>(img.w), spanY / static_cast<float>(img.h)});
    }
    img.px.assign(static_cast<std::size_t>(img.w) * img.h, kMapGround);
    if (m.waterCell > 0.f && m.waterW > 0) {
        for (std::uint32_t j = 0; j < img.h; ++j) {
            const float wy = (static_cast<float>(j) + 0.5f) * mpp;
            if (wy >= spanY) break; // the power-of-two pad stays ground
            const std::uint32_t cj = std::min(m.waterH - 1, static_cast<std::uint32_t>(wy / m.waterCell));
            for (std::uint32_t i = 0; i < img.w; ++i) {
                const float wx = (static_cast<float>(i) + 0.5f) * mpp;
                if (wx >= spanX) break;
                const std::uint32_t ci = std::min(m.waterW - 1, static_cast<std::uint32_t>(wx / m.waterCell));
                if (m.water[static_cast<std::size_t>(cj) * m.waterW + ci]) img.px[static_cast<std::size_t>(j) * img.w + i] = kMapWater;
            }
        }
    }
    for (const MapRect& r : m.waterRects) md::fillRect(img, r, kMapWater);
    for (const MapRect& r : m.buildings) md::fillRect(img, r, kMapBuilding);
    for (int cls = kMapRoadClasses - 1; cls >= 0; --cls) {
        for (const MapRoad& r : m.roads) {
            if (static_cast<int>(r.cls) != cls) continue;
            const float radius = std::max(r.width * 0.5f, kMapRoadMinPx[cls] * 0.5f * mpp);
            if (r.count == 1) {
                md::drawCapsule(img, m.points[r.first], m.points[r.first], radius, kMapRoadColor[cls]);
                continue;
            }
            for (std::uint32_t k = 0; k + 1 < r.count; ++k)
                md::drawCapsule(img, m.points[r.first + k], m.points[r.first + k + 1], radius, kMapRoadColor[cls]);
        }
    }
}

float mapZoomRadius(MapMode mode, const MiniMapModel& m) {
    switch (mode) {
    case MapMode::OnFoot: return kMapRadiusOnFoot;
    case MapMode::InVan: return kMapRadiusInVan;
    case MapMode::Overlay: return 0.5f * std::max(m.maxX - m.minX, m.maxY - m.minY);
    }
    return kMapRadiusOnFoot;
}

MapLayers mapLayers(MapMode mode) {
    MapLayers l;
    l.doors = mode == MapMode::OnFoot;
    l.stairs = mode != MapMode::InVan;
    return l;
}

bool mapRotates(MapMode mode) { return mode != MapMode::Overlay; }

namespace minimap_detail {
// The view's turn: the heading up when rotating, else none.
void viewTurn(const MapView& v, float& c, float& s) {
    if (!v.rotate) {
        c = 1.f;
        s = 0.f;
        return;
    }
    const float t = -(90.f + v.yawDeg) * kPi / 180.f;
    c = std::cos(t);
    s = std::sin(t);
}
} // namespace minimap_detail

Vec2 mapToView(const MapView& v, Vec2 world) {
    float c = 1.f, s = 0.f;
    md::viewTurn(v, c, s);
    const float inv = v.radius > 0.f ? 1.f / v.radius : 0.f;
    return md::rotate({(world.x - v.centre.x) * inv, (world.y - v.centre.y) * inv}, c, s);
}

Vec2 mapFromView(const MapView& v, Vec2 view) {
    float c = 1.f, s = 0.f;
    md::viewTurn(v, c, s);
    const Vec2 d = md::rotate(view, c, -s);
    return {v.centre.x + d.x * v.radius, v.centre.y + d.y * v.radius};
}

Vec2 mapNorth(const MapView& v) {
    float c = 1.f, s = 0.f;
    md::viewTurn(v, c, s);
    return md::rotate({0.f, -1.f}, c, s);
}

Vec2 mapHeadingDir(const MapView& v, float yawDeg) {
    float c = 1.f, s = 0.f;
    md::viewTurn(v, c, s);
    const float y = yawDeg * md::kPi / 180.f;
    return md::rotate({std::cos(y), std::sin(y)}, c, s);
}

MapPin mapPin(const MapView& v, Vec2 world, float edge) {
    MapPin pin;
    pin.at = mapToView(v, world);
    const float dx = world.x - v.centre.x, dy = world.y - v.centre.y;
    pin.distance = std::sqrt(dx * dx + dy * dy);
    const float len = std::sqrt(pin.at.x * pin.at.x + pin.at.y * pin.at.y);
    if (len > edge && len > 0.f) {
        pin.at = {pin.at.x / len * edge, pin.at.y / len * edge};
        pin.pinned = true;
    }
    return pin;
}

Vec2 mapImageUv(const MapImage& img, Vec2 world) {
    const float w = static_cast<float>(img.w) * img.mPerPx, h = static_cast<float>(img.h) * img.mPerPx;
    return {w > 0.f ? (world.x - img.x0) / w : 0.f, h > 0.f ? (world.y - img.y0) / h : 0.f};
}

int mapRoadAt(const MiniMapModel& m, Vec2 world) {
    if (m.gridCell <= 0.f || m.grid.empty()) return -1;
    const float fi = (world.x - m.minX) / m.gridCell, fj = (world.y - m.minY) / m.gridCell;
    if (!(fi >= 0.f) || !(fj >= 0.f) || fi >= static_cast<float>(m.gridW) || fj >= static_cast<float>(m.gridH)) return -1;
    return m.grid[static_cast<std::size_t>(fj) * m.gridW + static_cast<std::size_t>(fi)];
}

float mapRoadDistance(const MiniMapModel& m, int road, Vec2 world) {
    if (road < 0 || static_cast<std::size_t>(road) >= m.roads.size()) return 1e30f;
    const MapRoad& r = m.roads[static_cast<std::size_t>(road)];
    float best = 1e30f;
    for (std::uint32_t k = 0; k < r.count; ++k)
        best = std::min(best, md::segDistance(world, m.points[r.first + k], m.points[r.first + std::min(k + 1, r.count - 1)]));
    return best;
}

int mapRoadAtSticky(const MiniMapModel& m, Vec2 world, int previous) {
    if (previous >= 0 && static_cast<std::size_t>(previous) < m.roads.size() &&
        mapRoadDistance(m, previous, world) <= m.roads[static_cast<std::size_t>(previous)].width * 0.5f + kMapRoadHold)
        return previous;
    return mapRoadAt(m, world);
}

} // namespace ftcore
