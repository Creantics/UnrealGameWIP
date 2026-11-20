#include "ftcore/MapOverlay.h"

#include "CsvRead.h"
#include "ftcore/MassingPlan.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace ftcore {

namespace {

constexpr float kPi = 3.14159265358979f;

std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

// A whole field as a finite number; false for empty or anything else.
bool number(const std::string& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// A kind drawn as a line (a road, a street, a stair...): named along its road, never at a point, so a line the model
// leaves off is unmatched rather than a stray point name (a zone row excepted: zoneId).
bool lineKind(const std::string& k) {
    static const char* const kLine[] = {"road", "street", "lane", "alley", "stair", "terrace", "walk", "bridge",
                                        "footbridge", "causeway", "core_lane", "coast_road", "wall_walk"};
    for (const char* l : kLine)
        if (k == l) return true;
    static const char* const kEnds[] = {"_road", "_street", "_lane", "_stair", "_alley", "_walk"};
    for (const char* e : kEnds) {
        const std::size_t n = std::strlen(e);
        if (k.size() > n && k.compare(k.size() - n, n, e) == 0) return true;
    }
    return false;
}

// A zone row (Z and digits: the build tables' zones): named at its plan point whatever its kind.
bool zoneId(const std::string& id) {
    if (id.size() < 2 || id[0] != 'Z') return false;
    for (std::size_t i = 1; i < id.size(); ++i)
        if (id[i] < '0' || id[i] > '9') return false;
    return true;
}

// The part of segment a-b inside the box (Liang-Barsky); false when none of it is.
bool clipSegment(Vec2& a, Vec2& b, float x0, float y0, float x1, float y1) {
    float t0 = 0.f, t1 = 1.f;
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {a.x - x0, x1 - a.x, a.y - y0, y1 - a.y};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.f) {
            if (q[i] < 0.f) return false;
            continue;
        }
        const float t = q[i] / p[i];
        if (p[i] < 0.f) {
            if (t > t1) return false;
            t0 = std::max(t0, t);
        } else {
            if (t < t0) return false;
            t1 = std::min(t1, t);
        }
    }
    const Vec2 s = a;
    a = Vec2{s.x + t0 * dx, s.y + t0 * dy};
    b = Vec2{s.x + t1 * dx, s.y + t1 * dy};
    return true;
}

// A label's four corners.
void corners(const MapLabel& l, float grow, Vec2 out[4]) {
    const float r = l.angleDeg * kPi / 180.f;
    const float c = std::cos(r), s = std::sin(r);
    const float hx = l.w * 0.5f + grow, hy = l.h * 0.5f + grow;
    const float ox[4] = {-hx, hx, hx, -hx}, oy[4] = {-hy, -hy, hy, hy};
    for (int i = 0; i < 4; ++i) out[i] = Vec2{l.at.x + ox[i] * c - oy[i] * s, l.at.y + ox[i] * s + oy[i] * c};
}

bool separated(const Vec2 a[4], const Vec2 b[4], Vec2 axis) {
    float amin = 1e30f, amax = -1e30f, bmin = 1e30f, bmax = -1e30f;
    for (int i = 0; i < 4; ++i) {
        const float pa = a[i].x * axis.x + a[i].y * axis.y, pb = b[i].x * axis.x + b[i].y * axis.y;
        amin = std::min(amin, pa);
        amax = std::max(amax, pa);
        bmin = std::min(bmin, pb);
        bmax = std::max(bmax, pb);
    }
    return amax <= bmin || bmax <= amin;
}

// The distance from p to segment a-b, and how far along it (0..1) its nearest point lies.
float nearestOn(Vec2 p, Vec2 a, Vec2 b, float& t) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    t = len2 > 0.f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.f, 1.f) : 0.f;
    return std::hypot(a.x + dx * t - p.x, a.y + dy * t - p.y);
}

// Whether a road's points are all in the model.
bool roadOk(const MiniMapModel& m, std::size_t r) {
    const MapRoad& road = m.roads[r];
    return road.count > 0 && static_cast<std::size_t>(road.first) + road.count <= m.points.size();
}

// The point `s` m along road r's centre line (held to its ends).
Vec2 pointAtArc(const MiniMapModel& m, std::size_t r, float s) {
    const MapRoad& road = m.roads[r];
    float at = 0.f;
    for (std::uint32_t k = 0; k + 1 < road.count; ++k) {
        const Vec2 a = m.points[road.first + k], b = m.points[road.first + k + 1];
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        if (s <= at + len && len > 0.f) {
            const float u = std::clamp((s - at) / len, 0.f, 1.f);
            return Vec2{a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u};
        }
        at += len;
    }
    return m.points[road.first + road.count - 1];
}

bool overlapGrown(const MapLabel& a, float growA, const MapLabel& b, float growB) {
    Vec2 ca[4], cb[4];
    corners(a, growA, ca);
    corners(b, growB, cb);
    const float ra = a.angleDeg * kPi / 180.f, rb = b.angleDeg * kPi / 180.f;
    const Vec2 axes[4] = {{std::cos(ra), std::sin(ra)}, {-std::sin(ra), std::cos(ra)}, {std::cos(rb), std::sin(rb)}, {-std::sin(rb), std::cos(rb)}};
    for (const Vec2& ax : axes)
        if (separated(ca, cb, ax)) return false;
    return true;
}

} // namespace

bool mapRoadShown(MapRoadClass c, MapZoom z) {
    switch (c) {
    case MapRoadClass::Main: return true;
    case MapRoadClass::Street: return z != MapZoom::Far;
    case MapRoadClass::Lane:
    case MapRoadClass::Stair: return z == MapZoom::Near;
    }
    return false;
}

int mapRoadNear(const MiniMapModel& m, Vec2 world, float maxM, MapZoom z) {
    int best = -1;
    float bestD = maxM;
    for (std::size_t r = 0; r < m.roads.size(); ++r) {
        if (!mapRoadShown(m.roads[r].cls, z)) continue;
        const float d = mapRoadDistance(m, static_cast<int>(r), world);
        if (d < bestD || (d <= bestD && best >= 0 && m.roads[r].cls < m.roads[static_cast<std::size_t>(best)].cls)) {
            best = static_cast<int>(r);
            bestD = d;
        }
    }
    return best;
}

void mapWaterRuns(const MiniMapModel& m, std::vector<MapRect>& out) {
    out.assign(m.waterRects.begin(), m.waterRects.end());
    if (m.waterCell <= 0.f || m.water.size() < static_cast<std::size_t>(m.waterW) * m.waterH) return;
    const float cell = m.waterCell;
    for (std::uint32_t y = 0; y < m.waterH; ++y) {
        const std::uint8_t* row = m.water.data() + static_cast<std::size_t>(y) * m.waterW;
        std::uint32_t x = 0;
        while (x < m.waterW) {
            if (!row[x]) {
                ++x;
                continue;
            }
            const std::uint32_t x0 = x;
            while (x < m.waterW && row[x]) ++x;
            const float w = static_cast<float>(x - x0) * cell;
            out.push_back(MapRect{m.minX + static_cast<float>(x0) * cell + w * 0.5f, m.minY + (static_cast<float>(y) + 0.5f) * cell,
                                  w * 0.5f, cell * 0.5f, 0.f});
        }
    }
}

MapZoom mapZoomIn(MapZoom z) { return z == MapZoom::Far ? MapZoom::Mid : MapZoom::Near; }
MapZoom mapZoomOut(MapZoom z) { return z == MapZoom::Near ? MapZoom::Mid : MapZoom::Far; }

MapOverlayView mapOverlayView(Vec2 centre, MapZoom z, float screenW, float screenH, float uiScale) {
    MapOverlayView v;
    v.centre = centre;
    v.screenW = screenW > 0.f ? screenW : 1920.f;
    v.screenH = screenH > 0.f ? screenH : 1080.f;
    const float scale = (v.screenH / 1080.f) * (uiScale > 0.f ? uiScale : 1.f);
    v.mPerPx = kMapZoomMPerPx[static_cast<int>(z)] / scale;
    return v;
}

Vec2 mapOverlayToScreen(const MapOverlayView& v, Vec2 w) {
    return Vec2{(w.x - v.centre.x) / v.mPerPx + v.screenW * 0.5f, (w.y - v.centre.y) / v.mPerPx + v.screenH * 0.5f};
}

Vec2 mapOverlayFromScreen(const MapOverlayView& v, Vec2 s) {
    return Vec2{(s.x - v.screenW * 0.5f) * v.mPerPx + v.centre.x, (s.y - v.screenH * 0.5f) * v.mPerPx + v.centre.y};
}

Vec2 mapOverlayClampCentre(const MiniMapModel& m, const MapOverlayView& v) {
    auto hold = [](float c, float lo, float hi, float half) {
        if (hi - lo <= 2.f * half) return (lo + hi) * 0.5f;
        return std::clamp(c, lo + half, hi - half);
    };
    return Vec2{hold(v.centre.x, m.minX, m.maxX, v.screenW * v.mPerPx * 0.5f),
                hold(v.centre.y, m.minY, m.maxY, v.screenH * v.mPerPx * 0.5f)};
}

MapOverlayView mapOverlayViewIn(const MiniMapModel& m, Vec2 centre, MapZoom z, float screenW, float screenH, float uiScale) {
    MapOverlayView v = mapOverlayView(centre, z, screenW, screenH, uiScale);
    if (z != MapZoom::Far) return v;
    const float keep = 1.f - 2.f * kMapFarMargin;
    const float fit = std::max(std::max(m.maxX - m.minX, 0.f) / (v.screenW * keep), std::max(m.maxY - m.minY, 0.f) / (v.screenH * keep));
    const float mid = mapOverlayView(centre, MapZoom::Mid, screenW, screenH, uiScale).mPerPx;
    v.mPerPx = std::max(fit, mid * kMapFarOverMid);
    return v;
}

int mapSeaSide(const MiniMapModel& m) {
    if (m.waterW == 0 || m.waterH == 0 || m.water.size() < static_cast<std::size_t>(m.waterW) * m.waterH) return 0;
    auto wet = [&](std::uint32_t x) {
        std::uint32_t n = 0;
        for (std::uint32_t y = 0; y < m.waterH; ++y) n += m.water[static_cast<std::size_t>(y) * m.waterW + x] ? 1u : 0u;
        return 2u * n > m.waterH;
    };
    if (wet(m.waterW - 1)) return 1;
    return wet(0) ? -1 : 0;
}

void mapEnterableBuildings(const MiniMapModel& m, std::vector<std::uint8_t>& out, float reach) {
    out.assign(m.buildings.size(), 0);
    // Per door: the best building so far (outside distance, then centre distance).
    constexpr std::uint32_t kNone = 0xFFFFFFFFu;
    std::vector<std::uint32_t> best(m.doors.size(), kNone);
    std::vector<float> bestOut(m.doors.size(), 0.f), bestCentre(m.doors.size(), 0.f);
    for (std::size_t b = 0; b < m.buildings.size(); ++b) {
        const MapRect& r = m.buildings[b];
        const float c = std::cos(r.rot), s = std::sin(r.rot);
        for (std::size_t k = 0; k < m.doors.size(); ++k) {
            const float dx = m.doors[k].x - r.cx, dy = m.doors[k].y - r.cy;
            const float u = dx * c + dy * s, v = -dx * s + dy * c; // into the rect's own axes
            const float outside = std::hypot(std::max(std::abs(u) - r.hx, 0.f), std::max(std::abs(v) - r.hy, 0.f));
            if (outside > reach) continue;
            const float centre = std::hypot(dx, dy);
            if (best[k] == kNone || outside < bestOut[k] - 1e-3f || (outside <= bestOut[k] + 1e-3f && centre < bestCentre[k])) {
                best[k] = static_cast<std::uint32_t>(b);
                bestOut[k] = outside;
                bestCentre[k] = centre;
            }
        }
    }
    for (std::uint32_t b : best)
        if (b != kNone) out[b] = 1;
}

bool mapGreenKind(std::string_view kind) {
    static constexpr std::string_view kGreen[] = {"park", "green", "garden", "grove", "lawn", "orchard", "cemetery", "square", "plaza"};
    for (std::string_view g : kGreen)
        if (kind == g) return true;
    return false;
}

bool mapLoadNames(const MiniMapModel& m, std::string_view csv, MapNames& out, std::string* error, double planSide) {
    out = MapNames{};
    out.roadName.assign(m.roads.size(), kMapNoName);
    out.roadAnchor.assign(m.roads.size(), Vec2{});
    out.roadHasAnchor.assign(m.roads.size(), 0);
    std::vector<detail::Record> recs;
    if (!detail::splitCsv(csv, recs, error)) return false;
    std::size_t first = 0;
    while (first < recs.size() && !recs[first].fields.empty() && !recs[first].fields[0].empty() && recs[first].fields[0][0] == '#')
        ++first;
    if (first >= recs.size()) return detail::fail(error, 1, "no header");
    int col[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    const char* want[8] = {"id", "kind", "name", "sign_form", "district", "x", "y", "built"};
    const auto& head = recs[first].fields;
    for (std::size_t i = 0; i < head.size(); ++i)
        for (int k = 0; k < 8; ++k)
            if (trimmed(head[i]) == want[k]) col[k] = static_cast<int>(i);
    if (col[0] < 0 || col[1] < 0 || col[2] < 0)
        return detail::fail(error, recs[first].line, "the header needs id, kind and name");
    std::unordered_map<std::string, std::vector<int>> byId;
    for (std::size_t r = 0; r < m.roads.size(); ++r) byId[m.roads[r].id].push_back(static_cast<int>(r));
    std::unordered_map<std::string, std::uint16_t> byName;
    std::vector<std::string> roadDistrictId(m.roads.size()); // joined to the districts once all rows are read
    // Zone rows of a road kind (they may share a named road's run) stay in places until the end, which drops the ones
    // that name a segment instead.
    struct Zone {
        std::uint16_t name = kMapNoName;
        Vec2 at{};
        std::string district;
        std::size_t place = 0;
    };
    std::vector<Zone> zones;
    constexpr std::size_t kNoPlace = static_cast<std::size_t>(-1);
    std::size_t seaPlace = kNoPlace;
    int seaRank = 3;
    auto field = [](const detail::Record& rec, int c) { return c >= 0 && static_cast<std::size_t>(c) < rec.fields.size() ? trimmed(rec.fields[static_cast<std::size_t>(c)]) : std::string(); };
    // A row's point, in engine metres; false when it has none.
    auto point = [&](const detail::Record& rec, Vec2& at) {
        float px = 0.f, py = 0.f;
        if (!number(field(rec, col[5]), px) || !number(field(rec, col[6]), py)) return false;
        if (planSide > 0.0) {
            const MassingVec e = massingEnginePoint(planSide, static_cast<double>(px), static_cast<double>(py), 0.0);
            at = Vec2{static_cast<float>(e.x), static_cast<float>(e.y)};
        } else {
            at = Vec2{px, py};
        }
        return true;
    };
    for (std::size_t r = first + 1; r < recs.size(); ++r) {
        const auto& rec = recs[r];
        if (!rec.fields.empty() && !rec.fields[0].empty() && rec.fields[0][0] == '#') continue;
        const std::string id = field(rec, col[0]), kind = field(rec, col[1]), name = field(rec, col[2]);
        if (id.empty() || name.empty()) return detail::fail(error, rec.line, "a row needs an id and a name");
        std::uint16_t idx;
        const auto found = byName.find(name);
        if (found != byName.end()) {
            idx = found->second;
        } else {
            if (out.names.size() >= kMapNoName) return detail::fail(error, rec.line, "too many names");
            idx = static_cast<std::uint16_t>(out.names.size());
            byName.emplace(name, idx);
            out.names.push_back(name);
            out.signForms.emplace_back();
            out.nameKind.push_back(MapNameKind::Place); // until a road row or a water row says otherwise
        }
        const std::string sign = field(rec, col[3]);
        if (out.signForms[idx].empty() && !sign.empty()) out.signForms[idx] = sign;
        if (kind == "district") {
            Vec2 at{};
            const bool given = point(rec, at);
            out.districts.push_back(MapNames::District{id, idx, at, given});
            out.nameKind[idx] = MapNameKind::Road; // never drawn as a place
            continue;
        }
        const bool water = kind == "river" || kind == "lake" || kind == "canal" || kind == "sea" || kind == "harbour" || kind == "bay";
        if (water) out.nameKind[idx] = MapNameKind::Water;
        const int seaKind = kind == "sea" ? 0 : kind == "bay" ? 1 : kind == "harbour" ? 2 : 3;
        const auto roads = byId.find(id);
        if (roads == byId.end()) {
            Vec2 at{};
            const bool placed = point(rec, at);
            if (seaKind < seaRank) { // the open sea's name: the first sea row, else bay, else harbour
                if (seaPlace != kNoPlace) seaPlace = kNoPlace; // the one it beats goes back to its point
                seaRank = seaKind;
                out.seaName = idx;
                if (placed) {
                    seaPlace = out.places.size();
                    out.places.push_back(MapNames::Spot{idx, at, false});
                }
                continue;
            }
            if ((zoneId(id) || !lineKind(kind)) && placed) {
                MapRoadClass zc{};
                if (zoneId(id) && !water && mapRoadClass(kind, zc)) zones.push_back(Zone{idx, at, field(rec, col[4]), out.places.size()});
                const bool unbuilt = field(rec, col[7]) == "no"; // not in the world yet: no park drawn for it either
                out.places.push_back(MapNames::Spot{idx, at, mapGreenKind(kind) && !unbuilt, unbuilt});
            } else if (!(seaKind < 3)) {
                ++out.unmatched;
            }
            continue;
        }
        if (!water && out.nameKind[idx] != MapNameKind::Water) out.nameKind[idx] = MapNameKind::Road;
        const std::string district = field(rec, col[4]);
        Vec2 anchor{};
        const bool anchored = point(rec, anchor);
        for (int road : roads->second) {
            const std::size_t ri = static_cast<std::size_t>(road);
            out.roadName[ri] = idx;
            roadDistrictId[ri] = district;
            if (anchored) {
                out.roadAnchor[ri] = anchor;
                out.roadHasAnchor[ri] = 1;
            }
        }
    }
    auto districtOf = [&](const std::string& did) {
        if (did.empty()) return kMapNoName;
        for (std::size_t d = 0; d < out.districts.size(); ++d)
            if (out.districts[d].id == did) return static_cast<std::uint16_t>(d);
        return kMapNoName;
    };
    // Roads to their districts, and each district without a point of its own: the mean of its named roads' midpoints.
    out.roadDistrict.assign(m.roads.size(), kMapNoName);
    std::vector<double> sx(out.districts.size(), 0.0), sy(out.districts.size(), 0.0);
    std::vector<std::size_t> sn(out.districts.size(), 0);
    for (std::size_t r = 0; r < m.roads.size(); ++r) {
        const std::uint16_t d = districtOf(roadDistrictId[r]);
        if (d == kMapNoName) continue;
        out.roadDistrict[r] = d;
        if (!roadOk(m, r)) continue;
        const MapRoad& road = m.roads[r];
        const Vec2 pa = m.points[road.first], pb = m.points[road.first + road.count - 1];
        sx[d] += 0.5 * (static_cast<double>(pa.x) + static_cast<double>(pb.x));
        sy[d] += 0.5 * (static_cast<double>(pa.y) + static_cast<double>(pb.y));
        ++sn[d];
    }
    for (std::size_t d = 0; d < out.districts.size(); ++d) {
        if (sn[d] == 0 || out.districts[d].hasAnchor) continue; // a row's own point wins
        const double k = static_cast<double>(sn[d]);
        out.districts[d].anchor = Vec2{static_cast<float>(sx[d] / k), static_cast<float>(sy[d] / k)};
        out.districts[d].hasAnchor = true;
    }
    // Each road's bounds, once (the picks test them every frame).
    out.roadBox.assign(m.roads.size() * 4, 0.f);
    for (std::size_t r = 0; r < m.roads.size(); ++r) {
        float* box = &out.roadBox[r * 4];
        box[0] = box[1] = 1e30f;
        box[2] = box[3] = -1e30f;
        if (!roadOk(m, r)) continue;
        for (std::uint32_t k = 0; k < m.roads[r].count; ++k) {
            const Vec2 p = m.points[m.roads[r].first + k];
            box[0] = std::min(box[0], p.x);
            box[1] = std::min(box[1], p.y);
            box[2] = std::max(box[2], p.x);
            box[3] = std::max(box[3], p.y);
        }
    }
    // Zones that share a named road's straight run: split the run between the anchors on it (MapOverlay.h).
    std::vector<std::uint8_t> drop(out.places.size(), 0);
    if (seaPlace != kNoPlace) drop[seaPlace] = 1; // named over the open sea instead
    if (!zones.empty()) {
        struct Run {
            std::uint32_t road = 0, k0 = 0, k1 = 0; // pieces [k0, k1)
            float s0 = 0.f, s1 = 0.f;              // m along the road
        };
        std::vector<Run> runs;
        const float bendCos = std::cos(kMapAlongBendDeg * kPi / 180.f);
        for (std::size_t r = 0; r < m.roads.size(); ++r) {
            const std::uint16_t nm = out.roadName[r];
            if (nm == kMapNoName || out.nameKind[nm] == MapNameKind::Water || !roadOk(m, r) || m.roads[r].count < 2) continue;
            const MapRoad& road = m.roads[r];
            float s = 0.f;
            bool open = false;
            Run cur{};
            Vec2 dir{};
            for (std::uint32_t k = 0; k + 1 < road.count; ++k) {
                const Vec2 a = m.points[road.first + k], b = m.points[road.first + k + 1];
                const float len = std::hypot(b.x - a.x, b.y - a.y);
                if (len > 0.f) {
                    const Vec2 d{(b.x - a.x) / len, (b.y - a.y) / len};
                    if (open && d.x * dir.x + d.y * dir.y < bendCos) {
                        runs.push_back(cur);
                        open = false;
                    }
                    if (!open) {
                        cur = Run{static_cast<std::uint32_t>(r), k, k, s, s};
                        dir = d;
                        open = true;
                    }
                }
                s += len;
                if (open) {
                    cur.k1 = k + 1;
                    cur.s1 = s;
                }
            }
            if (open) runs.push_back(cur);
        }
        // The distance from p to a run, and the m along the road of its nearest point.
        auto onRun = [&](const Run& run, Vec2 p, float& along) {
            const MapRoad& road = m.roads[run.road];
            float best = 1e30f, s = run.s0;
            along = run.s0;
            for (std::uint32_t k = run.k0; k < run.k1; ++k) {
                const Vec2 a = m.points[road.first + k], b = m.points[road.first + k + 1];
                const float len = std::hypot(b.x - a.x, b.y - a.y);
                float u = 0.f;
                const float d = nearestOn(p, a, b, u);
                if (d < best) {
                    best = d;
                    along = s + u * len;
                }
                s += len;
            }
            return best;
        };
        std::vector<int> zoneRun(zones.size(), -1);
        std::vector<float> zoneAt(zones.size(), 0.f);
        for (std::size_t z = 0; z < zones.size(); ++z) {
            float bestD = kMapZoneReachM;
            const Vec2 p = zones[z].at;
            for (std::size_t q = 0; q < runs.size(); ++q) {
                const Run& run = runs[q];
                if (out.roadName[run.road] == zones[z].name) continue;
                const float* box = &out.roadBox[static_cast<std::size_t>(run.road) * 4];
                if (p.x < box[0] - bestD || p.x > box[2] + bestD || p.y < box[1] - bestD || p.y > box[3] + bestD) continue;
                float along = 0.f;
                const float d = onRun(run, p, along);
                const bool higher = zoneRun[z] >= 0 && m.roads[run.road].cls < m.roads[runs[static_cast<std::size_t>(zoneRun[z])].road].cls;
                if (d < bestD - 1e-4f || (d <= bestD + 1e-4f && (zoneRun[z] < 0 || higher))) {
                    bestD = d;
                    zoneRun[z] = static_cast<int>(q);
                    zoneAt[z] = along;
                }
            }
        }
        struct Anchor {
            float at = 0.f;
            std::size_t zone = 0; // into zones; the road's own anchor: zones.size()
        };
        std::vector<Anchor> anchors;
        for (std::size_t q = 0; q < runs.size(); ++q) {
            anchors.clear();
            for (std::size_t z = 0; z < zones.size(); ++z)
                if (zoneRun[z] == static_cast<int>(q)) anchors.push_back(Anchor{zoneAt[z], z});
            if (anchors.empty()) continue;
            const Run& run = runs[q];
            const std::size_t zn = anchors.size();
            bool roadAnchored = false;
            if (out.roadHasAnchor[run.road]) {
                float along = 0.f;
                if (onRun(run, out.roadAnchor[run.road], along) <= kMapZoneReachM) {
                    anchors.push_back(Anchor{along, zones.size()});
                    roadAnchored = true;
                }
            }
            std::stable_sort(anchors.begin(), anchors.end(), [](const Anchor& x, const Anchor& y) { return x.at < y.at; });
            float lo = run.s0, hi = run.s1;
            if (!roadAnchored) { // the zones keep the half of the run nearer them
                float mean = 0.f;
                for (const Anchor& an : anchors) mean += an.at;
                mean /= static_cast<float>(zn);
                const float mid = 0.5f * (run.s0 + run.s1);
                if (mean >= mid) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            for (std::size_t i = 0; i < anchors.size(); ++i) {
                if (anchors[i].zone >= zones.size()) continue;
                const float sa = i == 0 ? lo : std::max(lo, 0.5f * (anchors[i - 1].at + anchors[i].at));
                const float sb = i + 1 == anchors.size() ? hi : std::min(hi, 0.5f * (anchors[i].at + anchors[i + 1].at));
                if (!(sb > sa)) continue;
                const Zone& zone = zones[anchors[i].zone];
                MapNames::Segment seg;
                seg.road = run.road;
                seg.s0 = sa;
                seg.s1 = sb;
                seg.name = zone.name;
                seg.district = districtOf(zone.district);
                if (seg.district == kMapNoName) seg.district = out.roadDistrict[run.road];
                // Bounds: the ends and every point between them.
                const Vec2 pa = pointAtArc(m, run.road, sa), pb = pointAtArc(m, run.road, sb);
                seg.x0 = std::min(pa.x, pb.x);
                seg.y0 = std::min(pa.y, pb.y);
                seg.x1 = std::max(pa.x, pb.x);
                seg.y1 = std::max(pa.y, pb.y);
                const MapRoad& road = m.roads[run.road];
                float s = 0.f;
                for (std::uint32_t k = 0; k < road.count; ++k) {
                    const Vec2 p = m.points[road.first + k];
                    if (s > sa && s < sb) {
                        seg.x0 = std::min(seg.x0, p.x);
                        seg.y0 = std::min(seg.y0, p.y);
                        seg.x1 = std::max(seg.x1, p.x);
                        seg.y1 = std::max(seg.y1, p.y);
                    }
                    if (k + 1 < road.count) s += std::hypot(m.points[road.first + k + 1].x - p.x, m.points[road.first + k + 1].y - p.y);
                }
                out.segments.push_back(seg);
                drop[zone.place] = 1;
                out.nameKind[zone.name] = MapNameKind::Road;
            }
        }
        std::stable_sort(out.segments.begin(), out.segments.end(), [](const MapNames::Segment& x, const MapNames::Segment& y) {
            return x.road != y.road ? x.road < y.road : x.s0 < y.s0;
        });
    }
    out.roadSegments.assign(m.roads.size() + 1, 0);
    for (const MapNames::Segment& seg : out.segments) ++out.roadSegments[seg.road + 1];
    for (std::size_t r = 0; r < m.roads.size(); ++r) out.roadSegments[r + 1] += out.roadSegments[r];
    std::size_t kept = 0;
    for (std::size_t i = 0; i < out.places.size(); ++i)
        if (!drop[i]) out.places[kept++] = out.places[i];
    out.places.resize(kept);
    // Each name's tier: water and places by kind; a road's name by the best class carrying it; a district's name last.
    out.nameTier.assign(out.names.size(), MapTier::Stair);
    for (std::size_t i = 0; i < out.names.size(); ++i)
        if (out.nameKind[i] != MapNameKind::Road) out.nameTier[i] = out.nameKind[i] == MapNameKind::Water ? MapTier::Water : MapTier::Place;
    auto raise = [&](std::uint16_t nm, MapRoadClass c) {
        if (nm == kMapNoName || nm >= out.names.size() || out.nameKind[nm] != MapNameKind::Road) return;
        const MapTier t = c == MapRoadClass::Main ? MapTier::Main : c == MapRoadClass::Street ? MapTier::Street
                        : c == MapRoadClass::Lane ? MapTier::Lane : MapTier::Stair;
        if (t < out.nameTier[nm]) out.nameTier[nm] = t;
    };
    for (std::size_t r = 0; r < m.roads.size(); ++r) raise(out.roadName[r], m.roads[r].cls);
    for (const MapNames::Segment& seg : out.segments) raise(seg.name, m.roads[seg.road].cls);
    for (const MapNames::District& d : out.districts) out.nameTier[d.name] = MapTier::District;
    return true;
}

bool mapTierShown(MapTier t, MapZoom z) {
    switch (t) {
    case MapTier::District: return true;
    case MapTier::Water:
    case MapTier::Main:
    case MapTier::Street: return z != MapZoom::Far;
    case MapTier::Place:
    case MapTier::Lane: return z == MapZoom::Near;
    case MapTier::Stair: return false;
    }
    return false;
}

MapTier mapTierLowest(MapZoom z) {
    return z == MapZoom::Far ? MapTier::District : z == MapZoom::Mid ? MapTier::Street : MapTier::Lane;
}

bool mapTierMustDraw(MapTier t, MapZoom z) {
    if (!mapTierShown(t, z)) return false;
    int shown = 0;
    for (int i = 0; i < kMapTiers; ++i) shown += mapTierShown(static_cast<MapTier>(i), z) ? 1 : 0;
    return shown == 1 || static_cast<int>(t) < static_cast<int>(mapTierLowest(z));
}

const char* mapTierName(MapTier t) {
    static const char* const kNames[kMapTiers] = {"district", "water", "main", "street", "place", "lane", "stair"};
    const int i = static_cast<int>(t);
    return i >= 0 && i < kMapTiers ? kNames[i] : "?";
}

MapTier mapNameTier(const MapNames& n, std::uint16_t name) {
    if (name < n.nameTier.size()) return n.nameTier[name];
    if (name < n.nameKind.size() && n.nameKind[name] != MapNameKind::Road)
        return n.nameKind[name] == MapNameKind::Water ? MapTier::Water : MapTier::Place;
    return MapTier::Main;
}

MapTier mapRankTier(MapLabelRank r) {
    switch (r) {
    case MapLabelRank::Cursor:
    case MapLabelRank::District: return MapTier::District;
    case MapLabelRank::Water: return MapTier::Water;
    case MapLabelRank::Main: return MapTier::Main;
    case MapLabelRank::Street: return MapTier::Street;
    case MapLabelRank::Place: return MapTier::Place;
    case MapLabelRank::Lane: return MapTier::Lane;
    }
    return MapTier::Lane;
}

std::size_t mapCapCutAboveLowest(const MapLabelPick* picks, std::size_t count, MapZoom z, std::size_t cap) {
    if (!picks) return 0;
    const MapTier lowest = mapTierLowest(z);
    std::size_t cut = 0;
    for (std::size_t i = cap; i < count; ++i)
        if (picks[i].rank == MapLabelRank::Cursor || mapRankTier(picks[i].rank) != lowest) ++cut;
    return cut;
}

float mapRoadAlong(const MiniMapModel& m, int road, Vec2 world, float* distance) {
    if (distance) *distance = 1e30f;
    if (road < 0 || static_cast<std::size_t>(road) >= m.roads.size() || !roadOk(m, static_cast<std::size_t>(road))) return 0.f;
    const MapRoad& r = m.roads[static_cast<std::size_t>(road)];
    float best = 1e30f, along = 0.f, s = 0.f;
    if (r.count == 1) {
        best = std::hypot(world.x - m.points[r.first].x, world.y - m.points[r.first].y);
    }
    for (std::uint32_t k = 0; k + 1 < r.count; ++k) {
        const Vec2 a = m.points[r.first + k], b = m.points[r.first + k + 1];
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        float u = 0.f;
        const float d = nearestOn(world, a, b, u);
        if (d < best) {
            best = d;
            along = s + u * len;
        }
        s += len;
    }
    if (distance) *distance = best;
    return along;
}

MapPlaceName mapNameAt(const MiniMapModel& m, const MapNames& n, int road, Vec2 world) {
    MapPlaceName out;
    if (road < 0 || static_cast<std::size_t>(road) >= m.roads.size()) return out;
    const std::size_t r = static_cast<std::size_t>(road);
    out.name = mapRoadName(n, road);
    out.district = mapRoadDistrict(n, road);
    if (r + 1 >= n.roadSegments.size() || n.roadSegments[r] == n.roadSegments[r + 1]) return out;
    const float s = mapRoadAlong(m, road, world);
    for (std::uint32_t i = n.roadSegments[r]; i < n.roadSegments[r + 1] && i < n.segments.size(); ++i) {
        const MapNames::Segment& seg = n.segments[i];
        if (s >= seg.s0 && s <= seg.s1) {
            out.name = seg.name;
            if (seg.district != kMapNoName) out.district = seg.district;
            break;
        }
    }
    return out;
}

MapHere mapHereAt(const MiniMapModel& m, const MapNames& n, Vec2 world, float reach) {
    MapHere out;
    float bestD = reach;
    const std::size_t roads = std::min(m.roads.size(), n.roadName.size());
    const bool boxes = n.roadBox.size() >= roads * 4;
    for (std::size_t r = 0; r < roads; ++r) {
        const std::uint16_t nm = n.roadName[r];
        if (nm == kMapNoName || (nm < n.nameKind.size() && n.nameKind[nm] == MapNameKind::Water)) continue;
        if (boxes) {
            const float* box = &n.roadBox[r * 4];
            if (world.x < box[0] - bestD || world.x > box[2] + bestD || world.y < box[1] - bestD || world.y > box[3] + bestD) continue;
        }
        const float d = mapRoadDistance(m, static_cast<int>(r), world);
        const bool higher = out.road >= 0 && m.roads[r].cls < m.roads[static_cast<std::size_t>(out.road)].cls;
        if (d < bestD - 1e-3f || (d <= bestD + 1e-3f && (out.road < 0 || higher))) {
            bestD = d;
            out.road = static_cast<int>(r);
        }
    }
    if (out.road >= 0) {
        const MapPlaceName here = mapNameAt(m, n, out.road, world);
        out.name = here.name;
        out.district = here.district;
    }
    if (out.district == kMapNoName) { // the district whose anchor is nearest
        float bestA = 1e30f;
        for (std::size_t d = 0; d < n.districts.size(); ++d) {
            if (!n.districts[d].hasAnchor) continue;
            const float a = std::hypot(world.x - n.districts[d].anchor.x, world.y - n.districts[d].anchor.y);
            if (a < bestA) {
                bestA = a;
                out.district = static_cast<std::uint16_t>(d);
            }
        }
    }
    return out;
}

std::uint16_t mapCursorDistrict(const MiniMapModel& m, const MapNames& n, Vec2 world) {
    return mapHereAt(m, n, world, 1e30f).district;
}

std::uint16_t mapRoadDistrict(const MapNames& n, int road) {
    return road >= 0 && static_cast<std::size_t>(road) < n.roadDistrict.size() ? n.roadDistrict[static_cast<std::size_t>(road)] : kMapNoName;
}

std::uint16_t mapRoadName(const MapNames& n, int road) {
    return road >= 0 && static_cast<std::size_t>(road) < n.roadName.size() ? n.roadName[static_cast<std::size_t>(road)] : kMapNoName;
}

float mapUprightDeg(float deg) {
    if (!std::isfinite(deg)) return 0.f;
    float d = std::fmod(deg, 180.f); // (-180, 180)
    if (d < -90.f) d += 180.f;
    if (d >= 90.f) d -= 180.f;
    return d;
}

bool mapLabelAlong(const MiniMapModel& m, const MapNames& n, std::uint16_t name, const MapOverlayView& v, float w, float h,
                   MapLabel& out, float margin, float pad, float* runPx) {
    if (name == kMapNoName) return false;
    const float x0 = margin, y0 = margin, x1 = v.screenW - margin, y1 = v.screenH - margin;
    const float bendCos = std::cos(kMapAlongBendDeg * kPi / 180.f);
    float best = -1.f;
    Vec2 ba{}, bb{};
    // A run: clipped segments of one road that meet end to end and each turn less than the bend limit from the run's
    // first direction. Its length is its chord, so a wiggle never counts as straight.
    bool open = false;
    Vec2 ra{}, rb{}, dir{};
    auto endRun = [&]() {
        if (!open) return;
        open = false;
        const float chord = std::hypot(rb.x - ra.x, rb.y - ra.y);
        if (chord > best) {
            best = chord;
            ba = ra;
            bb = rb;
        }
    };
    // One stretch of centre line (world m) that carries the name: clipped to the screen, then joined to the open run
    // or starting one.
    auto piece = [&](Vec2 wa, Vec2 wb) {
        Vec2 a = mapOverlayToScreen(v, wa), b = mapOverlayToScreen(v, wb);
        if (!clipSegment(a, b, x0, y0, x1, y1)) {
            endRun();
            return;
        }
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        if (len <= 0.f) { // a point on screen still places a label, as before, when nothing longer does
            if (best < 0.f) {
                best = 0.f;
                ba = bb = a;
            }
            return;
        }
        const Vec2 d{(b.x - a.x) / len, (b.y - a.y) / len};
        if (open && std::hypot(a.x - rb.x, a.y - rb.y) <= kMapAlongJoinPx && d.x * dir.x + d.y * dir.y >= bendCos) {
            rb = b;
            return;
        }
        endRun();
        open = true;
        ra = a;
        rb = b;
        dir = d;
    };
    const std::size_t roads = std::min(m.roads.size(), n.roadName.size());
    for (std::size_t r = 0; r < roads; ++r) {
        const MapRoad& road = m.roads[r];
        const bool own = n.roadName[r] == name;
        std::uint32_t g0 = 0, g1 = 0; // the road's segments
        if (r + 1 < n.roadSegments.size()) {
            g0 = n.roadSegments[r];
            g1 = std::min(n.roadSegments[r + 1], static_cast<std::uint32_t>(n.segments.size()));
        }
        bool any = own;
        for (std::uint32_t g = g0; g < g1 && !any; ++g) any = n.segments[g].name == name;
        if (!any) continue;
        float s = 0.f; // m along the road
        for (std::uint32_t k = 0; k + 1 < road.count; ++k) {
            const std::size_t i = road.first + k;
            if (i + 1 >= m.points.size()) break;
            const Vec2 a = m.points[i], b = m.points[i + 1];
            if (g0 == g1) { // no segments: the whole piece is the road's
                piece(a, b);
                continue;
            }
            const float len = std::hypot(b.x - a.x, b.y - a.y);
            const float end = s + len;
            float cur = s;
            for (;;) { // the piece in parts, split where a segment starts or ends
                std::uint16_t here = n.roadName[r];
                float next = end;
                for (std::uint32_t g = g0; g < g1; ++g) {
                    const MapNames::Segment& seg = n.segments[g];
                    if (seg.s1 <= cur) continue;
                    if (seg.s0 <= cur) {
                        here = seg.name;
                        next = std::min(next, seg.s1);
                    } else {
                        next = std::min(next, seg.s0);
                    }
                    break;
                }
                const float u0 = len > 0.f ? (cur - s) / len : 0.f, u1 = len > 0.f ? (next - s) / len : 1.f;
                if (here == name) {
                    piece(Vec2{a.x + (b.x - a.x) * u0, a.y + (b.y - a.y) * u0}, Vec2{a.x + (b.x - a.x) * u1, a.y + (b.y - a.y) * u1});
                } else {
                    endRun();
                }
                if (next >= end) break;
                cur = next;
            }
            s = end;
        }
        endRun(); // a run never crosses into another road
    }
    if (best < 0.f) return false;
    out = MapLabel{};
    out.at = Vec2{(ba.x + bb.x) * 0.5f, (ba.y + bb.y) * 0.5f};
    out.angleDeg = best > 0.f ? mapUprightDeg(std::atan2(bb.y - ba.y, bb.x - ba.x) * 180.f / kPi) : 0.f;
    out.w = w;
    out.h = h;
    out.name = name;
    out.fits = best >= w + 2.f * pad;
    if (runPx) *runPx = best;
    return true;
}

float mapLabelRunWholePx(const MiniMapModel& m, const MapNames& n, std::uint16_t name, const MapOverlayView& v) {
    if (name == kMapNoName || !(v.mPerPx > 0.f)) return -1.f;
    // The bounds of every road that carries the name (its own, or a segment of it), then a view round them all.
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    const std::size_t roads = std::min(m.roads.size(), n.roadName.size());
    for (std::size_t r = 0; r < roads; ++r) {
        bool any = n.roadName[r] == name;
        if (r + 1 < n.roadSegments.size())
            for (std::uint32_t g = n.roadSegments[r]; g < n.roadSegments[r + 1] && g < n.segments.size() && !any; ++g)
                any = n.segments[g].name == name;
        if (!any) continue;
        const MapRoad& road = m.roads[r];
        for (std::uint32_t k = 0; k < road.count && road.first + k < m.points.size(); ++k) {
            const Vec2 p = m.points[road.first + k];
            bx0 = std::min(bx0, p.x);
            by0 = std::min(by0, p.y);
            bx1 = std::max(bx1, p.x);
            by1 = std::max(by1, p.y);
        }
    }
    if (bx0 > bx1) return -1.f;
    MapOverlayView whole = v;
    whole.centre = Vec2{0.5f * (bx0 + bx1), 0.5f * (by0 + by1)};
    whole.screenW = (bx1 - bx0) / v.mPerPx + 64.f;
    whole.screenH = (by1 - by0) / v.mPerPx + 64.f;
    MapLabel l;
    float run = -1.f;
    if (!mapLabelAlong(m, n, name, whole, 0.f, 0.f, l, 0.f, 0.f, &run)) return -1.f;
    return run;
}

bool mapLabelsOverlap(const MapLabel& a, const MapLabel& b, float gap) { return overlapGrown(a, gap * 0.5f, b, gap * 0.5f); }

void mapLabelCorners(const MapLabel& l, float grow, Vec2 out[4]) { corners(l, grow, out); }

bool mapLabelOnRoad(const MiniMapModel& m, int road, const MapOverlayView& v, float widthPx, const MapLabel& l, float grow) {
    if (road < 0 || static_cast<std::size_t>(road) >= m.roads.size() || !roadOk(m, static_cast<std::size_t>(road))) return false;
    const MapRoad& r = m.roads[static_cast<std::size_t>(road)];
    const float wd = std::max(widthPx, 0.f);
    const float reachL = std::hypot(l.w * 0.5f + grow, l.h * 0.5f + grow); // the label's half diagonal, for a quick reject
    for (std::uint32_t k = 0; k + 1 < r.count; ++k) {
        const Vec2 a = mapOverlayToScreen(v, m.points[r.first + k]);
        const Vec2 b = mapOverlayToScreen(v, m.points[r.first + k + 1]);
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::hypot(dx, dy);
        MapLabel piece;
        piece.at = Vec2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        piece.angleDeg = len > 0.f ? std::atan2(dy, dx) * 180.f / kPi : 0.f;
        piece.w = len + wd;
        piece.h = wd;
        const float reachP = std::hypot(piece.w * 0.5f, piece.h * 0.5f);
        if (std::hypot(piece.at.x - l.at.x, piece.at.y - l.at.y) > reachL + reachP) continue;
        if (overlapGrown(l, grow, piece, 0.f)) return true;
    }
    return false;
}

bool mapLabelKeepInside(MapLabel& l, float screenW, float screenH, float grow) {
    Vec2 k[4];
    corners(l, grow, k);
    float x0 = k[0].x, x1 = k[0].x, y0 = k[0].y, y1 = k[0].y;
    for (int i = 1; i < 4; ++i) {
        x0 = std::min(x0, k[i].x);
        x1 = std::max(x1, k[i].x);
        y0 = std::min(y0, k[i].y);
        y1 = std::max(y1, k[i].y);
    }
    const float in = kMapLabelInsetPx;
    if (x1 - x0 > screenW - 2.f * in || y1 - y0 > screenH - 2.f * in) return false;
    const float dx = x0 < in ? in - x0 : (x1 > screenW - in ? screenW - in - x1 : 0.f);
    const float dy = y0 < in ? in - y0 : (y1 > screenH - in ? screenH - in - y1 : 0.f);
    l.at.x += dx;
    l.at.y += dy;
    return true;
}

bool mapRoadCased(const MapRoad& road, const MapOverlayView& v, const MapRoadInk& ink) {
    return v.mPerPx > 0.f && road.width / v.mPerPx >= ink.casePx * ink.scale;
}

float mapRoadDrawnPx(const MapRoad& road, const MapOverlayView& v, const MapRoadInk& ink) {
    if (mapRoadCased(road, v, ink)) return road.width / v.mPerPx;
    const int c = std::clamp(static_cast<int>(road.cls), 0, kMapRoadClasses - 1);
    return std::max(ink.linePx[c] * ink.scale, ink.minPx);
}

float mapRoadLitPx(const MapRoad& road, const MapOverlayView& v, const MapRoadInk& ink) {
    const float own = v.mPerPx > 0.f ? road.width / v.mPerPx : 0.f;
    return std::max(own * ink.litShare, ink.litMinPx * ink.scale);
}

bool mapRoadLit(const MapNames& n, std::size_t r, int picked, std::uint16_t pickedName) {
    if (picked < 0) return false;
    if (r == static_cast<std::size_t>(picked)) return true;
    return pickedName != kMapNoName && r < n.roadName.size() && n.roadName[r] == pickedName;
}

void mapRoadOffEnds(const MiniMapModel& m, std::vector<std::uint8_t>& out, float touchM) {
    out.assign(m.roads.size(), 0);
    if (m.points.empty()) return;
    float x0 = m.points[0].x, y0 = m.points[0].y, x1 = x0, y1 = y0;
    for (const Vec2& p : m.points) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    const auto off = [&](const Vec2& p) { return p.x - x0 <= touchM || x1 - p.x <= touchM || p.y - y0 <= touchM || y1 - p.y <= touchM; };
    for (std::size_t r = 0; r < m.roads.size(); ++r) {
        const MapRoad& road = m.roads[r];
        if (road.count == 0 || static_cast<std::size_t>(road.first) + road.count > m.points.size()) continue;
        std::uint8_t ends = 0;
        if (off(m.points[road.first])) ends |= kMapRoadOffFirst;
        if (off(m.points[road.first + road.count - 1])) ends |= kMapRoadOffLast;
        out[r] = ends;
    }
}

namespace {
// Road r as the label tests take it: false when it is not tested (too short, not whole, neither lit nor drawn at z);
// else the width of its ink in px and its bounds in metres (x0, y0, x1, y1).
bool labelRoad(const MiniMapModel& m, const MapNames& n, const MapOverlayView& v, MapZoom z, const MapRoadInk& ink,
               std::size_t r, int picked, std::uint16_t pickedName, bool litOnly, float& wd, float box[4]) {
    const MapRoad& road = m.roads[r];
    if (road.count < 2 || !roadOk(m, r)) return false;
    const bool lit = mapRoadLit(n, r, picked, pickedName);
    const bool shown = !litOnly && mapRoadShown(road.cls, z);
    if (!lit && !shown) return false;
    wd = shown ? mapRoadDrawnPx(road, v, ink) : 0.f;
    if (lit) wd = std::max(wd, mapRoadLitPx(road, v, ink));
    if (n.roadBox.size() >= m.roads.size() * 4) {
        for (std::size_t k = 0; k < 4; ++k) box[k] = n.roadBox[r * 4 + k];
        return true;
    }
    box[0] = box[1] = 1e30f;
    box[2] = box[3] = -1e30f;
    for (std::uint32_t k = 0; k < road.count; ++k) {
        const Vec2 p = m.points[road.first + k];
        box[0] = std::min(box[0], p.x);
        box[1] = std::min(box[1], p.y);
        box[2] = std::max(box[2], p.x);
        box[3] = std::max(box[3], p.y);
    }
    return true;
}

// mapLabelOnAnyRoad over `count` roads by index (rising), or over every road when `roads` is null.
int labelOnRoads(const MiniMapModel& m, const MapNames& n, const MapOverlayView& v, MapZoom z, const MapRoadInk& ink,
                 const MapLabel& l, float grow, int picked, std::uint16_t pickedName, bool litOnly,
                 const std::uint32_t* roads, std::size_t count) {
    if (!(v.mPerPx > 0.f)) return -1;
    const Vec2 cw = mapOverlayFromScreen(v, l.at);
    const float reachPx = std::hypot(l.w * 0.5f + grow, l.h * 0.5f + grow);
    const std::size_t total = roads ? count : m.roads.size();
    for (std::size_t i = 0; i < total; ++i) {
        const std::size_t r = roads ? roads[i] : i;
        float wd = 0.f;
        float box[4]{};
        if (!labelRoad(m, n, v, z, ink, r, picked, pickedName, litOnly, wd, box)) continue;
        const float reachM = (reachPx + wd * 0.5f + 1.f) * v.mPerPx;
        if (cw.x < box[0] - reachM || cw.x > box[2] + reachM || cw.y < box[1] - reachM || cw.y > box[3] + reachM) continue;
        if (mapLabelOnRoad(m, static_cast<int>(r), v, wd, l, grow)) return static_cast<int>(r);
    }
    return -1;
}
} // namespace

int mapLabelOnAnyRoad(const MiniMapModel& m, const MapNames& n, const MapOverlayView& v, MapZoom z, const MapRoadInk& ink,
                      const MapLabel& l, float grow, int picked, std::uint16_t pickedName, bool litOnly) {
    return labelOnRoads(m, n, v, z, ink, l, grow, picked, pickedName, litOnly, nullptr, 0);
}

int mapRoadsNearLabels(const MiniMapModel& m, const MapNames& n, const MapOverlayView& v, MapZoom z, const MapRoadInk& ink,
                       Vec2 lo, Vec2 hi, float w, float h, float grow, int picked, std::uint16_t pickedName,
                       std::uint32_t* out, int cap) {
    if (!(v.mPerPx > 0.f)) return 0;
    const Vec2 a = mapOverlayFromScreen(v, lo), b = mapOverlayFromScreen(v, hi);
    const float reachPx = std::hypot(w * 0.5f + grow, h * 0.5f + grow);
    int k = 0;
    for (std::size_t r = 0; r < m.roads.size(); ++r) {
        float wd = 0.f;
        float box[4]{};
        if (!labelRoad(m, n, v, z, ink, r, picked, pickedName, false, wd, box)) continue;
        const float reachM = (reachPx + wd * 0.5f + 1.f) * v.mPerPx;
        if (b.x < box[0] - reachM || a.x > box[2] + reachM || b.y < box[1] - reachM || a.y > box[3] + reachM) continue;
        if (k >= cap) return -1;
        out[k++] = static_cast<std::uint32_t>(r);
    }
    return k;
}

int mapLabelOnRoads(const MiniMapModel& m, const MapNames& n, const MapOverlayView& v, MapZoom z, const MapRoadInk& ink,
                    const MapLabel& l, float grow, int picked, std::uint16_t pickedName, const std::uint32_t* roads,
                    int count) {
    if (count < 0) return mapLabelOnAnyRoad(m, n, v, z, ink, l, grow, picked, pickedName);
    return labelOnRoads(m, n, v, z, ink, l, grow, picked, pickedName, false, roads, static_cast<std::size_t>(count));
}

void mapLabelMoves(const MapLabel& l, Vec2 out[kMapLabelMoves]) {
    // Half widths -2..2 across, heights -2..2 up and down, by distance; ties keep this order (up first, left first).
    // Sorted in place: no allocation.
    struct Move {
        float dx = 0.f, dy = 0.f, d = 0.f;
        int order = 0;
    };
    std::array<Move, kMapLabelMoves> moves{};
    int k = 0;
    for (int uy = -2; uy <= 2; ++uy)
        for (int ux = -2; ux <= 2; ++ux) {
            if (ux == 0 && uy == 0) continue;
            Move& mv = moves[static_cast<std::size_t>(k)];
            mv.dx = static_cast<float>(ux) * 0.5f * l.w;
            mv.dy = static_cast<float>(uy) * l.h;
            mv.d = std::hypot(mv.dx, mv.dy);
            mv.order = (std::abs(uy) * 2 + (uy > 0 ? 1 : 0)) * 16 + std::abs(ux) * 2 + (ux > 0 ? 1 : 0);
            ++k;
        }
    std::sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) { return a.d != b.d ? a.d < b.d : a.order < b.order; });
    for (int i = 0; i < kMapLabelMoves; ++i) out[i] = Vec2{moves[static_cast<std::size_t>(i)].dx, moves[static_cast<std::size_t>(i)].dy};
}

bool mapLabelOffRoads(const MiniMapModel& m, const MapNames& n, const MapOverlayView& v, MapZoom z, const MapRoadInk& ink,
                      MapLabel& l, float grow, int picked, std::uint16_t pickedName) {
    if (mapLabelOnAnyRoad(m, n, v, z, ink, l, grow, picked, pickedName) < 0) return true;
    Vec2 moves[kMapLabelMoves];
    mapLabelMoves(l, moves);
    for (const Vec2& mv : moves) {
        MapLabel t = l;
        t.at.x += mv.x;
        t.at.y += mv.y;
        if (!mapLabelKeepInside(t, v.screenW, v.screenH, grow)) return false;
        if (mapLabelOnAnyRoad(m, n, v, z, ink, t, grow, picked, pickedName) >= 0) continue;
        l = t;
        return true;
    }
    return false;
}

bool mapLabelMoveClear(const MiniMapModel& m, const MapNames& n, const MapOverlayView& v, MapZoom z, const MapRoadInk& ink,
                       MapLabel& l, float halo, float gap, const MapLabel* labels, std::size_t labelCount,
                       const MapLabel* blockers, std::size_t blockerCount, int picked, std::uint16_t pickedName) {
    const float h = std::max(halo, 0.f), g = std::max(gap, 0.f);
    const auto clear = [&](const MapLabel& t) {
        for (std::size_t k = 0; labels && k < labelCount; ++k)
            if (overlapGrown(t, g * 0.5f + h, labels[k], g * 0.5f + h)) return false;
        for (std::size_t k = 0; blockers && k < blockerCount; ++k)
            if (overlapGrown(t, g * 0.5f + h, blockers[k], g * 0.5f)) return false;
        return mapLabelOnAnyRoad(m, n, v, z, ink, t, h, picked, pickedName, true) < 0;
    };
    if (clear(l)) return true;
    Vec2 moves[kMapLabelMoves];
    mapLabelMoves(l, moves);
    for (const Vec2& mv : moves) {
        MapLabel t = l;
        t.at.x += mv.x;
        t.at.y += mv.y;
        if (!mapLabelKeepInside(t, v.screenW, v.screenH, h)) return false;
        if (!clear(t)) continue;
        l = t;
        return true;
    }
    return false;
}

bool mapIsWater(const MiniMapModel& m, Vec2 p) {
    for (const MapRect& r : m.waterRects) {
        const float c = std::cos(r.rot), s = std::sin(r.rot);
        const float dx = p.x - r.cx, dy = p.y - r.cy;
        if (std::abs(dx * c + dy * s) <= r.hx && std::abs(-dx * s + dy * c) <= r.hy) return true;
    }
    if (m.waterCell <= 0.f || m.waterW == 0 || m.water.size() < static_cast<std::size_t>(m.waterW) * m.waterH) return false;
    const float fx = (p.x - m.minX) / m.waterCell, fy = (p.y - m.minY) / m.waterCell;
    if (!(fx >= 0.f && fy >= 0.f && fx < static_cast<float>(m.waterW) && fy < static_cast<float>(m.waterH))) return false;
    return m.water[static_cast<std::size_t>(fy) * m.waterW + static_cast<std::size_t>(fx)] != 0;
}

bool mapLabelMeetsDisc(const MapLabel& l, float grow, const MapDisc& d) {
    const float r = l.angleDeg * kPi / 180.f;
    const float c = std::cos(r), s = std::sin(r);
    const float dx = d.at.x - l.at.x, dy = d.at.y - l.at.y;
    const float qu = std::max(std::abs(dx * c + dy * s) - (l.w * 0.5f + grow), 0.f);
    const float qv = std::max(std::abs(-dx * s + dy * c) - (l.h * 0.5f + grow), 0.f);
    return qu * qu + qv * qv < d.r * d.r;
}

namespace {
// The least slide of l (px) along its own line (axis 0) or straight across it (axis 1) that clears every disc, keeps
// the grown box whole on the screen and lies within reach: the ends of each disc's blocked slides, just outside, tried
// nearest first, ties to `prefer`'s side. False when none does.
bool clearSlide(const MapLabel& l, const MapDisc* discs, std::size_t count, float grow, int axis, float reach, float prefer,
                float screenW, float screenH, Vec2& move) {
    constexpr float kEps = 0.5f;
    const float r = l.angleDeg * kPi / 180.f;
    const float c = std::cos(r), s = std::sin(r);
    const Vec2 dir = axis == 0 ? Vec2{c, s} : Vec2{-s, c};
    const float halfA = axis == 0 ? l.w * 0.5f + grow : l.h * 0.5f + grow;
    const float halfS = axis == 0 ? l.h * 0.5f + grow : l.w * 0.5f + grow;
    // Disc k blocks the open interval of slides (lo, hi); empty (lo == hi) when it lies wholly beside the box's path.
    auto blocked = [&](std::size_t k, float& lo, float& hi) {
        const float dx = discs[k].at.x - l.at.x, dy = discs[k].at.y - l.at.y;
        const float cu = dx * c + dy * s, cv = -dx * s + dy * c;
        const float onAxis = axis == 0 ? cu : cv, beside = std::abs(axis == 0 ? cv : cu) - halfS;
        const float rr = discs[k].r;
        lo = hi = 0.f;
        if (beside >= rr) return;
        const float ext = beside <= 0.f ? rr : std::sqrt(rr * rr - beside * beside);
        lo = onAxis - halfA - ext;
        hi = onAxis + halfA + ext;
    };
    auto clear = [&](float t) {
        if (std::abs(t) > reach) return false;
        for (std::size_t k = 0; k < count; ++k) {
            float lo = 0.f, hi = 0.f;
            blocked(k, lo, hi);
            if (t > lo && t < hi) return false;
        }
        MapLabel moved = l;
        moved.at.x += dir.x * t;
        moved.at.y += dir.y * t;
        Vec2 q[4];
        corners(moved, grow, q);
        for (const Vec2& p : q)
            if (p.x < kMapLabelInsetPx || p.y < kMapLabelInsetPx || p.x > screenW - kMapLabelInsetPx || p.y > screenH - kMapLabelInsetPx)
                return false;
        return true;
    };
    bool found = false;
    float best = 0.f;
    auto consider = [&](float t) {
        if (found && (std::abs(t) > std::abs(best) || (std::abs(t) == std::abs(best) && t * prefer <= best * prefer))) return;
        if (!clear(t)) return;
        best = t;
        found = true;
    };
    for (std::size_t k = 0; k < count; ++k) {
        float lo = 0.f, hi = 0.f;
        blocked(k, lo, hi);
        if (lo == hi) continue;
        consider(lo - kEps);
        consider(hi + kEps);
    }
    if (found) move = Vec2{dir.x * best, dir.y * best};
    return found;
}
} // namespace

bool mapLabelClearOfDiscs(MapLabel& l, const MapDisc* discs, std::size_t count, float grow, float along, float across, float screenW,
                          float screenH) {
    bool meets = false;
    for (std::size_t k = 0; discs && k < count && !meets; ++k) meets = mapLabelMeetsDisc(l, grow, discs[k]);
    if (!meets) return true;
    Vec2 move{};
    // Along the line, right first in reading; else across it, up first (the box's own -y).
    if (!clearSlide(l, discs, count, grow, 0, along, 1.f, screenW, screenH, move) &&
        !clearSlide(l, discs, count, grow, 1, across, -1.f, screenW, screenH, move))
        return false;
    l.at.x += move.x;
    l.at.y += move.y;
    return true;
}

void mapPlaceLabels(const MapLabel* candidates, std::size_t count, MapLabelSet& out, float gap, const MapLabel* blockers,
                    std::size_t blockerCount, float halo) {
    out.count = 0;
    if (!candidates) return;
    const float grow = gap * 0.5f + std::max(halo, 0.f); // a label and its halo
    // By priority, then input order: one pass per priority level (256 at most), no sort buffer.
    for (int p = 0; p < 256 && out.count < kMaxMapLabels; ++p) {
        for (std::size_t i = 0; i < count && out.count < kMaxMapLabels; ++i) {
            const MapLabel& c = candidates[i];
            if (c.priority != p) continue;
            if (!(c.w > 0.f && c.h > 0.f)) continue;
            bool clear = true;
            for (std::size_t k = 0; blockers && k < blockerCount && clear; ++k) clear = !overlapGrown(c, grow, blockers[k], gap * 0.5f);
            for (std::size_t k = 0; k < out.count && clear; ++k) clear = !overlapGrown(c, grow, out.labels[k], grow);
            if (clear) out.labels[out.count++] = c;
        }
    }
}

bool mapPlaceLabelAdd(MapLabelSet& set, const MapLabel& l, float gap, const MapLabel* blockers, std::size_t blockerCount,
                      float halo) {
    if (set.count >= kMaxMapLabels || !(l.w > 0.f && l.h > 0.f)) return false;
    const float grow = gap * 0.5f + std::max(halo, 0.f); // as mapPlaceLabels
    for (std::size_t k = 0; blockers && k < blockerCount; ++k)
        if (overlapGrown(l, grow, blockers[k], gap * 0.5f)) return false;
    for (std::size_t k = 0; k < set.count; ++k)
        if (overlapGrown(l, grow, set.labels[k], grow)) return false;
    set.labels[set.count++] = l;
    return true;
}

MapLabel mapCaptionBox(float screenW, float screenH, float scale) {
    const float s = scale > 0.f ? scale : 1.f;
    MapLabel b;
    b.w = (kMapCaptionW + 2.f * kMapCaptionPad) * s;
    if (screenW > 0.f) b.w = std::min(b.w, screenW);
    b.h = (kMapCaptionH + 2.f * kMapCaptionPad) * s;
    const float left = (kMapCaptionX - kMapCaptionPad) * s, top = screenH - (kMapCaptionUp + kMapCaptionPad) * s;
    b.at = Vec2{left + b.w * 0.5f, top + b.h * 0.5f};
    return b;
}

MapCaption mapCaptionLayout(float screenW, float screenH, float scale, float districtW, float streetW, float lineH, float ruleT) {
    const auto size = [](float v) { return std::isfinite(v) && v > 0.f ? v : 0.f; };
    const float s = std::isfinite(scale) && scale > 0.f ? scale : 1.f;
    districtW = size(districtW);
    streetW = size(streetW);
    lineH = size(lineH);
    ruleT = size(ruleT);
    MapCaption c;
    c.kept = mapCaptionBox(screenW, screenH, s);
    c.text = Vec2{kMapCaptionX * s, screenH - kMapCaptionUp * s};
    const float gap = kMapCaptionGap * s;
    const float lineW = districtW + (districtW > 0.f && streetW > 0.f ? gap : 0.f) + streetW;
    if (!(lineW > 0.f)) {
        c.plate = MapLabel{};
        c.plate.at = c.text;
        return c;
    }
    c.shown = true;
    // The room for the line: the screen less the plate's left margin on both sides, the tail and the pad.
    const float pad = kMapCaptionPad * s, margin = (kMapCaptionX - kMapCaptionPad) * s;
    const float room = screenW - margin - pad - kMapCaptionTail * s - c.text.x;
    if (std::isfinite(room) && lineW > room) c.fit = room > lineW * 1e-3f ? room / lineW : 1e-3f;
    c.districtW = districtW * c.fit;
    c.streetW = streetW * c.fit;
    c.lineH = lineH * c.fit;
    const float g = districtW > 0.f && streetW > 0.f ? gap * c.fit : 0.f;
    c.streetX = c.text.x + c.districtW + g;
    c.dot = Vec2{c.text.x + c.districtW + g * 0.5f, c.text.y + c.lineH * 0.55f};
    c.ruleY = c.text.y + c.lineH + kMapCaptionRuleGap * s;
    c.ruleRight = c.streetX + c.streetW + kMapCaptionTail * s;
    c.ruleBottom = c.ruleY + ruleT;
    const float x0 = c.text.x - pad, y0 = c.text.y - pad, x1 = c.ruleRight + pad, y1 = c.ruleBottom + pad;
    c.plate.w = x1 - x0;
    c.plate.h = y1 - y0;
    c.plate.at = Vec2{0.5f * (x0 + x1), 0.5f * (y0 + y1)};
    // The kept box takes in the plate too.
    const float kx0 = std::min(c.kept.at.x - 0.5f * c.kept.w, x0), ky0 = std::min(c.kept.at.y - 0.5f * c.kept.h, y0);
    const float kx1 = std::max(c.kept.at.x + 0.5f * c.kept.w, x1), ky1 = std::max(c.kept.at.y + 0.5f * c.kept.h, y1);
    c.kept.w = kx1 - kx0;
    c.kept.h = ky1 - ky0;
    c.kept.at = Vec2{0.5f * (kx0 + kx1), 0.5f * (ky0 + ky1)};
    return c;
}

bool mapRoadLabelled(MapRoadClass c, MapZoom z) {
    switch (c) {
    case MapRoadClass::Main:
    case MapRoadClass::Street: return z != MapZoom::Far;
    case MapRoadClass::Lane: return z == MapZoom::Near;
    case MapRoadClass::Stair: return false;
    }
    return false;
}

std::size_t mapLabelPicks(const MiniMapModel& m, const MapNames& n, const MapOverlayView& v, MapZoom z, int picked,
                          MapLabelPick* out, std::size_t cap, std::uint8_t* seen, float margin, std::uint16_t pickedName,
                          const Vec2* seaAt, const Vec2* cursorAt) {
    if (!out || cap == 0) return 0;
    const std::size_t names = n.names.size();
    if (seen && names > 0) std::memset(seen, 0, names);
    std::size_t count = 0;
    auto take = [&](std::uint16_t name, MapLabelRank rank, bool along, Vec2 at, bool unbuilt = false) {
        if (count >= cap || name == kMapNoName || name >= names) return;
        if (seen) {
            if (seen[name]) return;
            seen[name] = 1;
        }
        out[count++] = MapLabelPick{name, rank, along, unbuilt, at};
    };
    const Vec2 lo = mapOverlayFromScreen(v, Vec2{-margin, -margin});
    const Vec2 hi = mapOverlayFromScreen(v, Vec2{v.screenW + margin, v.screenH + margin});
    auto pointIn = [&](Vec2 p) { return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y; };
    auto boxIn = [&](float bx0, float by0, float bx1, float by1) { return bx1 >= lo.x && bx0 <= hi.x && by1 >= lo.y && by0 <= hi.y; };
    const std::size_t roads = std::min(m.roads.size(), n.roadName.size());
    const bool boxes = n.roadBox.size() >= roads * 4;
    auto roadIn = [&](std::size_t r) {
        if (boxes) return boxIn(n.roadBox[r * 4], n.roadBox[r * 4 + 1], n.roadBox[r * 4 + 2], n.roadBox[r * 4 + 3]);
        const MapRoad& road = m.roads[r]; // names not from mapLoadNames: measure here
        if (road.count == 0 || road.first + road.count > m.points.size()) return false;
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        for (std::uint32_t k = 0; k < road.count; ++k) {
            const Vec2 p = m.points[road.first + k];
            bx0 = std::min(bx0, p.x);
            by0 = std::min(by0, p.y);
            bx1 = std::max(bx1, p.x);
            by1 = std::max(by1, p.y);
        }
        return boxIn(bx0, by0, bx1, by1);
    };
    // The cursor's name: the name at the cursor on its road, else the district there. Outside the tier table.
    std::uint16_t cursorName = kMapNoName;
    if (picked >= 0 && static_cast<std::size_t>(picked) < roads)
        cursorName = pickedName != kMapNoName ? pickedName : n.roadName[static_cast<std::size_t>(picked)];
    if (cursorName != kMapNoName) {
        take(cursorName, MapLabelRank::Cursor, true, Vec2{});
    } else if (cursorAt) {
        const std::uint16_t d = mapCursorDistrict(m, n, *cursorAt);
        if (d < n.districts.size()) take(n.districts[d].name, MapLabelRank::Cursor, false, *cursorAt);
    }
    for (const MapNames::District& d : n.districts)
        if (d.hasAnchor && pointIn(d.anchor)) take(d.name, MapLabelRank::District, false, d.anchor);
    auto kindOf = [&](std::uint16_t name) { return name < n.nameKind.size() ? n.nameKind[name] : MapNameKind::Road; };
    // Water, where the zoom draws it: the sea, once, where it is widest on screen; water at its points; then along its
    // roads (canals).
    if (mapTierShown(MapTier::Water, z)) {
        if (seaAt && n.seaName != kMapNoName) take(n.seaName, MapLabelRank::Water, false, *seaAt);
        for (const MapNames::Spot& p : n.places)
            if (kindOf(p.name) == MapNameKind::Water && pointIn(p.at)) take(p.name, MapLabelRank::Water, false, p.at, p.unbuilt);
        for (std::size_t r = 0; r < roads; ++r) {
            const std::uint16_t name = n.roadName[r];
            if (name != kMapNoName && kindOf(name) == MapNameKind::Water && (!seen || !seen[name]) && roadIn(r)) take(name, MapLabelRank::Water, true, Vec2{});
        }
    }
    // Roads' names by tier, main first (places before lanes): a name is taken with its own tier (the best class that
    // carries it) when any road or segment that carries it meets the screen. A road's name not from mapLoadNames takes
    // the road's class.
    const MapTier tierOf[3] = {MapTier::Main, MapTier::Street, MapTier::Lane};
    const MapLabelRank rankOf[3] = {MapLabelRank::Main, MapLabelRank::Street, MapLabelRank::Lane};
    auto nameTier = [&](std::uint16_t name, MapRoadClass cls) {
        if (name < n.nameTier.size()) return n.nameTier[name];
        return cls == MapRoadClass::Main ? MapTier::Main : cls == MapRoadClass::Street ? MapTier::Street
             : cls == MapRoadClass::Lane ? MapTier::Lane : MapTier::Stair;
    };
    for (int c = 0; c < 3; ++c) {
        if (c == 2 && mapTierShown(MapTier::Place, z))
            for (const MapNames::Spot& p : n.places)
                if (kindOf(p.name) == MapNameKind::Place && pointIn(p.at)) take(p.name, MapLabelRank::Place, false, p.at, p.unbuilt);
        if (!mapTierShown(tierOf[c], z)) continue;
        for (std::size_t r = 0; r < roads; ++r) {
            const MapRoadClass cls = m.roads[r].cls;
            const std::uint16_t name = n.roadName[r];
            if (name != kMapNoName && kindOf(name) != MapNameKind::Water && nameTier(name, cls) == tierOf[c] && (!seen || !seen[name]) && roadIn(r))
                take(name, rankOf[c], true, Vec2{});
            if (r + 1 >= n.roadSegments.size()) continue;
            for (std::uint32_t g = n.roadSegments[r]; g < n.roadSegments[r + 1] && g < n.segments.size(); ++g) {
                const MapNames::Segment& seg = n.segments[g];
                if (nameTier(seg.name, cls) == tierOf[c] && (!seen || !seen[seg.name]) && boxIn(seg.x0, seg.y0, seg.x1, seg.y1))
                    take(seg.name, rankOf[c], true, Vec2{});
            }
        }
    }
    return count;
}

} // namespace ftcore
