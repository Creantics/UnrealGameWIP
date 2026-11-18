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

bool mapLoadNames(const MiniMapModel& m, std::string_view csv, MapNames& out, std::string* error, double planSide) {
    out = MapNames{};
    out.roadName.assign(m.roads.size(), kMapNoName);
    std::vector<detail::Record> recs;
    if (!detail::splitCsv(csv, recs, error)) return false;
    std::size_t first = 0;
    while (first < recs.size() && !recs[first].fields.empty() && !recs[first].fields[0].empty() && recs[first].fields[0][0] == '#')
        ++first;
    if (first >= recs.size()) return detail::fail(error, 1, "no header");
    int col[7] = {-1, -1, -1, -1, -1, -1, -1};
    const char* want[7] = {"id", "kind", "name", "sign_form", "district", "x", "y"};
    const auto& head = recs[first].fields;
    for (std::size_t i = 0; i < head.size(); ++i)
        for (int k = 0; k < 7; ++k)
            if (trimmed(head[i]) == want[k]) col[k] = static_cast<int>(i);
    if (col[0] < 0 || col[1] < 0 || col[2] < 0)
        return detail::fail(error, recs[first].line, "the header needs id, kind and name");
    std::unordered_map<std::string, std::vector<int>> byId;
    for (std::size_t r = 0; r < m.roads.size(); ++r) byId[m.roads[r].id].push_back(static_cast<int>(r));
    std::unordered_map<std::string, std::uint16_t> byName;
    std::vector<std::string> roadDistrictId(m.roads.size()); // joined to the districts once all rows are read
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
        const auto roads = byId.find(id);
        if (roads == byId.end()) {
            Vec2 at{};
            if ((zoneId(id) || !lineKind(kind)) && point(rec, at)) {
                out.places.push_back(MapNames::Spot{idx, at});
            } else {
                ++out.unmatched;
            }
            continue;
        }
        if (!water && out.nameKind[idx] != MapNameKind::Water) out.nameKind[idx] = MapNameKind::Road;
        const std::string district = field(rec, col[4]);
        for (int road : roads->second) {
            out.roadName[static_cast<std::size_t>(road)] = idx;
            roadDistrictId[static_cast<std::size_t>(road)] = district;
        }
    }
    // Roads to their districts, and each district without a point of its own: the mean of its named roads' midpoints.
    out.roadDistrict.assign(m.roads.size(), kMapNoName);
    std::vector<double> sx(out.districts.size(), 0.0), sy(out.districts.size(), 0.0);
    std::vector<std::size_t> sn(out.districts.size(), 0);
    for (std::size_t r = 0; r < m.roads.size(); ++r) {
        if (roadDistrictId[r].empty()) continue;
        for (std::size_t d = 0; d < out.districts.size(); ++d) {
            if (out.districts[d].id != roadDistrictId[r]) continue;
            out.roadDistrict[r] = static_cast<std::uint16_t>(d);
            const MapRoad& road = m.roads[r];
            if (road.count == 0 || road.first + road.count > m.points.size()) break;
            const Vec2 pa = m.points[road.first], pb = m.points[road.first + road.count - 1];
            sx[d] += 0.5 * (static_cast<double>(pa.x) + static_cast<double>(pb.x));
            sy[d] += 0.5 * (static_cast<double>(pa.y) + static_cast<double>(pb.y));
            ++sn[d];
            break;
        }
    }
    for (std::size_t d = 0; d < out.districts.size(); ++d) {
        if (sn[d] == 0 || out.districts[d].hasAnchor) continue; // a row's own point wins
        const double k = static_cast<double>(sn[d]);
        out.districts[d].anchor = Vec2{static_cast<float>(sx[d] / k), static_cast<float>(sy[d] / k)};
        out.districts[d].hasAnchor = true;
    }
    return true;
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
                   MapLabel& out, float margin, float pad) {
    if (name == kMapNoName) return false;
    const float x0 = margin, y0 = margin, x1 = v.screenW - margin, y1 = v.screenH - margin;
    float best = -1.f;
    Vec2 ba{}, bb{};
    const std::size_t roads = std::min(m.roads.size(), n.roadName.size());
    for (std::size_t r = 0; r < roads; ++r) {
        if (n.roadName[r] != name) continue;
        const MapRoad& road = m.roads[r];
        for (std::uint32_t k = 0; k + 1 < road.count; ++k) {
            const std::size_t i = road.first + k;
            if (i + 1 >= m.points.size()) break;
            Vec2 a = mapOverlayToScreen(v, m.points[i]), b = mapOverlayToScreen(v, m.points[i + 1]);
            if (!clipSegment(a, b, x0, y0, x1, y1)) continue;
            const float len = std::hypot(b.x - a.x, b.y - a.y);
            if (len > best) {
                best = len;
                ba = a;
                bb = b;
            }
        }
    }
    if (best < 0.f) return false;
    out = MapLabel{};
    out.at = Vec2{(ba.x + bb.x) * 0.5f, (ba.y + bb.y) * 0.5f};
    out.angleDeg = best > 0.f ? mapUprightDeg(std::atan2(bb.y - ba.y, bb.x - ba.x) * 180.f / kPi) : 0.f;
    out.w = w;
    out.h = h;
    out.name = name;
    out.fits = best >= w + 2.f * pad;
    return true;
}

bool mapLabelsOverlap(const MapLabel& a, const MapLabel& b, float gap) {
    Vec2 ca[4], cb[4];
    corners(a, gap * 0.5f, ca);
    corners(b, gap * 0.5f, cb);
    const float ra = a.angleDeg * kPi / 180.f, rb = b.angleDeg * kPi / 180.f;
    const Vec2 axes[4] = {{std::cos(ra), std::sin(ra)}, {-std::sin(ra), std::cos(ra)}, {std::cos(rb), std::sin(rb)}, {-std::sin(rb), std::cos(rb)}};
    for (const Vec2& ax : axes)
        if (separated(ca, cb, ax)) return false;
    return true;
}

void mapPlaceLabels(const MapLabel* candidates, std::size_t count, MapLabelSet& out, float gap) {
    out.count = 0;
    if (!candidates) return;
    // By priority, then input order: one pass per priority level (256 at most), no sort buffer.
    for (int p = 0; p < 256 && out.count < kMaxMapLabels; ++p) {
        for (std::size_t i = 0; i < count && out.count < kMaxMapLabels; ++i) {
            const MapLabel& c = candidates[i];
            if (c.priority != p) continue;
            if (!(c.w > 0.f && c.h > 0.f)) continue;
            bool clear = true;
            for (std::size_t k = 0; k < out.count && clear; ++k) clear = !mapLabelsOverlap(c, out.labels[k], gap);
            if (clear) out.labels[out.count++] = c;
        }
    }
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
                          MapLabelPick* out, std::size_t cap, std::uint8_t* seen, float margin) {
    if (!out || cap == 0) return 0;
    const std::size_t names = n.names.size();
    if (seen && names > 0) std::memset(seen, 0, names);
    std::size_t count = 0;
    auto take = [&](std::uint16_t name, MapLabelRank rank, bool along, Vec2 at) {
        if (count >= cap || name == kMapNoName || name >= names) return;
        if (seen) {
            if (seen[name]) return;
            seen[name] = 1;
        }
        out[count++] = MapLabelPick{name, rank, along, at};
    };
    const Vec2 lo = mapOverlayFromScreen(v, Vec2{-margin, -margin});
    const Vec2 hi = mapOverlayFromScreen(v, Vec2{v.screenW + margin, v.screenH + margin});
    auto pointIn = [&](Vec2 p) { return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y; };
    auto roadIn = [&](std::size_t r) {
        const MapRoad& road = m.roads[r];
        if (road.count == 0 || road.first + road.count > m.points.size()) return false;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (std::uint32_t k = 0; k < road.count; ++k) {
            const Vec2 p = m.points[road.first + k];
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
        return x1 >= lo.x && x0 <= hi.x && y1 >= lo.y && y0 <= hi.y;
    };
    const std::size_t roads = std::min(m.roads.size(), n.roadName.size());
    if (picked >= 0 && static_cast<std::size_t>(picked) < roads) take(n.roadName[static_cast<std::size_t>(picked)], MapLabelRank::Cursor, true, Vec2{});
    for (const MapNames::District& d : n.districts)
        if (d.hasAnchor && pointIn(d.anchor)) take(d.name, MapLabelRank::District, false, d.anchor);
    auto kindOf = [&](std::uint16_t name) { return name < n.nameKind.size() ? n.nameKind[name] : MapNameKind::Road; };
    // Water: at its points (every zoom), then along its roads (canals, from Mid).
    for (const MapNames::Spot& p : n.places)
        if (kindOf(p.name) == MapNameKind::Water && pointIn(p.at)) take(p.name, MapLabelRank::Water, false, p.at);
    if (z != MapZoom::Far)
        for (std::size_t r = 0; r < roads; ++r) {
            const std::uint16_t name = n.roadName[r];
            if (name != kMapNoName && kindOf(name) == MapNameKind::Water && (!seen || !seen[name]) && roadIn(r)) take(name, MapLabelRank::Water, true, Vec2{});
        }
    // Roads by class, main first.
    const MapRoadClass order[3] = {MapRoadClass::Main, MapRoadClass::Street, MapRoadClass::Lane};
    const MapLabelRank rankOf[3] = {MapLabelRank::Main, MapLabelRank::Street, MapLabelRank::Lane};
    for (int c = 0; c < 3; ++c) {
        if (c == 2 && z == MapZoom::Near)
            for (const MapNames::Spot& p : n.places)
                if (kindOf(p.name) == MapNameKind::Place && pointIn(p.at)) take(p.name, MapLabelRank::Place, false, p.at);
        if (!mapRoadLabelled(order[c], z)) continue;
        for (std::size_t r = 0; r < roads; ++r) {
            const std::uint16_t name = n.roadName[r];
            if (m.roads[r].cls != order[c] || name == kMapNoName || kindOf(name) == MapNameKind::Water || (seen && seen[name])) continue;
            if (roadIn(r)) take(name, rankOf[c], true, Vec2{});
        }
    }
    return count;
}

} // namespace ftcore
