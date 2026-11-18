#include "ftcore/MassingLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAxisTol = 0.01;   // m: a segment this close to an axis runs along it
constexpr double kEdgeTol = 0.02;   // m: boxes this close count as touching, not overlapping
constexpr double kOverlapMin = 1.0; // m a building's span along a lane must share with the segment to be pulled
constexpr double kProbe = 1.0;      // m behind a frontage's line a stretch is looked at for buildings
constexpr double kStandsOver = 0.3; // m: a box whose top is no higher over the ground is no obstacle (a buried wall)
constexpr double kOverhead = 3.0;   // m: a box whose bottom is this far over the ground is no obstacle (a deck)

struct Rect {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

Rect toRect(const MassingRect& r) { return {r.x0, r.y0, r.x1, r.y1}; }

bool overlaps(const Rect& a, const Rect& b, double tol) {
    return a.x0 < b.x1 - tol && b.x0 < a.x1 - tol && a.y0 < b.y1 - tol && b.y0 < a.y1 - tol;
}

// The box's footprint's bounds, for any turn.
Rect boundsOf(const MassingBox& b) {
    const double t = b.rot * kPi / 180.0;
    const double c = std::fabs(std::cos(t)), s = std::fabs(std::sin(t));
    const double hx = 0.5 * (b.sx * c + b.sy * s), hy = 0.5 * (b.sx * s + b.sy * c);
    return {b.cx - hx, b.cy - hy, b.cx + hx, b.cy + hy};
}

bool axisTurn(double rot) {
    const double r = std::fmod(std::fabs(rot), 90.0);
    return r < 1e-6 || 90.0 - r < 1e-6;
}

bool quarterTurned(double rot) {
    const double r = std::fmod(std::fmod(rot, 360.0) + 360.0, 180.0);
    return std::fabs(r - 90.0) < 1e-6;
}

// Sets an axis-turned box's footprint to `r`, keeping its turn.
void setBounds(MassingBox& b, const Rect& r) {
    b.cx = 0.5 * (r.x0 + r.x1);
    b.cy = 0.5 * (r.y0 + r.y1);
    if (quarterTurned(b.rot)) {
        b.sx = r.y1 - r.y0;
        b.sy = r.x1 - r.x0;
    } else {
        b.sx = r.x1 - r.x0;
        b.sy = r.y1 - r.y0;
    }
}

double groundAt(const MassingGround& g, double x, double y) {
    return massingGroundAt(g.grid, g.samplesX, g.samplesY, g.spacing, g.originX, g.originY, x, y);
}

// The lowest and highest ground under a footprint, read every metre or so (edges included).
void groundSpan(const MassingGround& g, const Rect& r, double& lo, double& hi) {
    const int nx = std::max(2, static_cast<int>(std::ceil(r.x1 - r.x0)) + 1);
    const int ny = std::max(2, static_cast<int>(std::ceil(r.y1 - r.y0)) + 1);
    lo = std::numeric_limits<double>::infinity();
    hi = -lo;
    for (int i = 0; i < nx; ++i) {
        const double x = r.x0 + (r.x1 - r.x0) * i / (nx - 1);
        for (int j = 0; j < ny; ++j) {
            const double y = r.y0 + (r.y1 - r.y0) * j / (ny - 1);
            const double h = groundAt(g, x, y);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
}

std::uint32_t hashOf(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (const char ch : s) {
        h ^= static_cast<std::uint8_t>(ch);
        h *= 16777619u;
    }
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    return h;
}

// A lane segment inside the area: along x (alongX) or y, its centre line at `c` on the other axis, running s0..s1.
struct Seg {
    std::size_t street = 0, index = 0;
    bool alongX = true;
    double c = 0.0, s0 = 0.0, s1 = 0.0, half = 0.0;
    double end0 = 0.0, end1 = 0.0; // the unclipped segment's ends along it
};

// Along and across a segment's axis.
double a0(const Rect& r, bool alongX) { return alongX ? r.x0 : r.y0; }
double a1(const Rect& r, bool alongX) { return alongX ? r.x1 : r.y1; }
double p0(const Rect& r, bool alongX) { return alongX ? r.y0 : r.x0; }
double p1(const Rect& r, bool alongX) { return alongX ? r.y1 : r.x1; }
Rect make(bool alongX, double along0, double along1, double perp0, double perp1) {
    if (perp0 > perp1) std::swap(perp0, perp1);
    if (along0 > along1) std::swap(along0, along1);
    return alongX ? Rect{along0, perp0, along1, perp1} : Rect{perp0, along0, perp1, along1};
}

bool laneKind(const MassingStreet& st, const std::vector<std::string>& kinds) {
    if (kinds.empty()) return true;
    for (const std::string& k : kinds)
        if (st.kind == k) return true;
    return false;
}

std::vector<Seg> laneSegments(const std::vector<MassingStreet>& streets, const Rect& area, double upTo,
                              const std::vector<std::string>& kinds) {
    std::vector<Seg> out;
    for (std::size_t s = 0; s < streets.size(); ++s) {
        const MassingStreet& st = streets[s];
        if (!(st.width <= upTo) || !laneKind(st, kinds)) continue;
        for (std::size_t i = 0; i + 1 < st.points.size(); ++i) {
            const MassingVec& p = st.points[i];
            const MassingVec& q = st.points[i + 1];
            Seg seg;
            seg.street = s;
            seg.index = i;
            seg.half = 0.5 * st.width;
            if (std::fabs(p.y - q.y) <= kAxisTol) {
                seg.alongX = true;
                seg.c = 0.5 * (p.y + q.y);
                seg.end0 = std::min(p.x, q.x);
                seg.end1 = std::max(p.x, q.x);
                if (seg.c < area.y0 || seg.c > area.y1) continue;
                seg.s0 = std::max(seg.end0, area.x0);
                seg.s1 = std::min(seg.end1, area.x1);
            } else if (std::fabs(p.x - q.x) <= kAxisTol) {
                seg.alongX = false;
                seg.c = 0.5 * (p.x + q.x);
                seg.end0 = std::min(p.y, q.y);
                seg.end1 = std::max(p.y, q.y);
                if (seg.c < area.x0 || seg.c > area.x1) continue;
                seg.s0 = std::max(seg.end0, area.y0);
                seg.s1 = std::min(seg.end1, area.y1);
            } else {
                continue;
            }
            if (seg.s1 - seg.s0 >= 1.0) out.push_back(seg);
        }
    }
    return out;
}

// Every street segment's strip (an axis segment's rectangle, else its bounds grown by half the width), with its street
// and segment, so a lane can leave its own out.
struct Strip {
    Rect r;
    std::size_t street = 0, index = 0;
};
std::vector<Strip> streetStrips(const std::vector<MassingStreet>& streets) {
    std::vector<Strip> out;
    for (std::size_t s = 0; s < streets.size(); ++s) {
        const MassingStreet& st = streets[s];
        const double h = 0.5 * st.width;
        for (std::size_t i = 0; i + 1 < st.points.size(); ++i) {
            const MassingVec& p = st.points[i];
            const MassingVec& q = st.points[i + 1];
            Rect r{std::min(p.x, q.x), std::min(p.y, q.y), std::max(p.x, q.x), std::max(p.y, q.y)};
            if (std::fabs(p.y - q.y) <= kAxisTol) {
                r.y0 -= h;
                r.y1 += h;
            } else if (std::fabs(p.x - q.x) <= kAxisTol) {
                r.x0 -= h;
                r.x1 += h;
            } else {
                r.x0 -= h;
                r.y0 -= h;
                r.x1 += h;
                r.y1 += h;
            }
            out.push_back({r, s, i});
        }
    }
    return out;
}

struct Scene {
    std::vector<MassingBox>& boxes;
    const MassingGround& ground;
    std::vector<Rect> bounds;
    std::vector<char> building; // may be moved
    std::vector<char> solid;    // stands in the way at street level
    std::vector<Strip> strips;
    std::vector<Rect> clear;

    Scene(std::vector<MassingBox>& b, const MassingGround& g) : boxes(b), ground(g) {}

    void classify(std::size_t i) {
        const MassingBox& b = boxes[i];
        const MassingRole role = massingRole(b);
        const bool visible = massingVisible(role);
        bounds[i] = boundsOf(b);
        building[i] = visible && b.kind != "wall" && massingLookIsBuilding(massingLookSurface(b)) && !b.sloped &&
                      b.riseU == 0.0 && b.riseW == 0.0 && axisTurn(b.rot);
        if (!visible) {
            solid[i] = 0;
            return;
        }
        const double g = groundAt(ground, b.cx, b.cy);
        const double top = b.sloped ? std::max(b.slopeFrom, b.slopeTo) : b.z1;
        solid[i] = top > g + kStandsOver && b.z0 < g + kOverhead;
    }
    void build() {
        bounds.assign(boxes.size(), Rect{});
        building.assign(boxes.size(), 0);
        solid.assign(boxes.size(), 0);
        for (std::size_t i = 0; i < boxes.size(); ++i) classify(i);
    }
    void add(const MassingBox& b) {
        boxes.push_back(b);
        bounds.push_back(Rect{});
        building.push_back(0);
        solid.push_back(0);
        classify(boxes.size() - 1);
    }
    // Anything in `r` (boxes other than `skip`, streets other than the lane's own segment, keep-clear rects).
    bool blocked(const Rect& r, std::size_t skip, const Seg* own) const {
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (i == skip || !(solid[i] || building[i])) continue;
            if (overlaps(r, bounds[i], kEdgeTol)) return true;
        }
        for (const Strip& s : strips) {
            if (own && s.street == own->street && s.index == own->index) continue;
            if (overlaps(r, s.r, kEdgeTol)) return true;
        }
        for (const Rect& c : clear)
            if (overlaps(r, c, kEdgeTol)) return true;
        return false;
    }
};


// A footprint turned by any angle: centre, unit along (u) and its half sizes along u and across (v).
struct Obb {
    double cx = 0.0, cy = 0.0, ux = 1.0, uy = 0.0, hu = 0.0, hv = 0.0;
};

Obb obbOf(const MassingBox& b) {
    const double t = b.rot * kPi / 180.0;
    return {b.cx, b.cy, std::cos(t), std::sin(t), 0.5 * b.sx, 0.5 * b.sy};
}

Obb obbOf(const Rect& r) { return {0.5 * (r.x0 + r.x1), 0.5 * (r.y0 + r.y1), 1.0, 0.0, 0.5 * (r.x1 - r.x0), 0.5 * (r.y1 - r.y0)}; }

Rect boundsOf(const Obb& o) {
    const double hx = std::fabs(o.ux) * o.hu + std::fabs(o.uy) * o.hv, hy = std::fabs(o.uy) * o.hu + std::fabs(o.ux) * o.hv;
    return {o.cx - hx, o.cy - hy, o.cx + hx, o.cy + hy};
}

// Separating axes: the two overlap by more than `tol` on every axis.
bool obbOverlap(const Obb& a, const Obb& b, double tol) {
    const double dx = b.cx - a.cx, dy = b.cy - a.cy;
    const double axes[4][2] = {{a.ux, a.uy}, {-a.uy, a.ux}, {b.ux, b.uy}, {-b.uy, b.ux}};
    for (const auto& ax : axes) {
        const double ra = a.hu * std::fabs(a.ux * ax[0] + a.uy * ax[1]) + a.hv * std::fabs(-a.uy * ax[0] + a.ux * ax[1]);
        const double rb = b.hu * std::fabs(b.ux * ax[0] + b.uy * ax[1]) + b.hv * std::fabs(-b.uy * ax[0] + b.ux * ax[1]);
        if (std::fabs(dx * ax[0] + dy * ax[1]) >= ra + rb - tol) return false;
    }
    return true;
}

double pointRectDistance(double u, double v, double hu, double hv) {
    const double eu = std::max(0.0, std::fabs(u) - hu), ev = std::max(0.0, std::fabs(v) - hv);
    return std::sqrt(eu * eu + ev * ev);
}

double pointSegmentDistance(double x, double y, double ax, double ay, double bx, double by) {
    const double vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
    const double t = l2 > 0.0 ? std::clamp(((x - ax) * vx + (y - ay) * vy) / l2, 0.0, 1.0) : 0.0;
    return std::hypot(x - ax - t * vx, y - ay - t * vy);
}

// The distance between a segment and a turned footprint (0 when they cross).
double segmentObbDistance(const MassingVec& p, const MassingVec& q, const Obb& o) {
    const auto local = [&](double x, double y, double& u, double& v) {
        const double dx = x - o.cx, dy = y - o.cy;
        u = dx * o.ux + dy * o.uy;
        v = -dx * o.uy + dy * o.ux;
    };
    double pu = 0.0, pv = 0.0, qu = 0.0, qv = 0.0;
    local(p.x, p.y, pu, pv);
    local(q.x, q.y, qu, qv);
    // Clip the segment to the rect (Liang-Barsky): any part left inside means they cross.
    double t0 = 0.0, t1 = 1.0;
    bool crosses = true;
    const double d[2] = {qu - pu, qv - pv}, s[2] = {pu, pv}, h[2] = {o.hu, o.hv};
    for (int k = 0; k < 2; ++k) {
        if (std::fabs(d[k]) < 1e-12) {
            if (std::fabs(s[k]) > h[k]) crosses = false;
            continue;
        }
        double a = (-h[k] - s[k]) / d[k], b = (h[k] - s[k]) / d[k];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    if (crosses && t0 <= t1) return 0.0;
    double best = std::min(pointRectDistance(pu, pv, o.hu, o.hv), pointRectDistance(qu, qv, o.hu, o.hv));
    for (const double cu : {-o.hu, o.hu})
        for (const double cv : {-o.hv, o.hv}) best = std::min(best, pointSegmentDistance(cu, cv, pu, pv, qu, qv));
    return best;
}

// Indices by 16 m cell, for the oblique pass's lookups.
struct CellIndex {
    static constexpr double kCell = 16.0;
    std::vector<std::pair<std::int64_t, std::size_t>> items; // (cell key, index), sorted on demand
    bool sorted = true;
    static std::int64_t key(int cx, int cy) {
        return static_cast<std::int64_t>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
                                         static_cast<std::uint32_t>(cy));
    }
    static int cell(double v) { return static_cast<int>(std::floor(v / kCell)); }
    void add(const Rect& r, std::size_t i) {
        for (int cx = cell(r.x0); cx <= cell(r.x1); ++cx)
            for (int cy = cell(r.y0); cy <= cell(r.y1); ++cy) items.emplace_back(key(cx, cy), i);
        sorted = false;
    }
    // Calls f(i) once for each index filed under a cell `r` touches, until f returns true.
    template <class F> void visit(const Rect& r, std::vector<std::size_t>& seen, F&& f) {
        if (!sorted) {
            std::sort(items.begin(), items.end());
            sorted = true;
        }
        seen.clear();
        for (int cx = cell(r.x0); cx <= cell(r.x1); ++cx)
            for (int cy = cell(r.y0); cy <= cell(r.y1); ++cy) {
                const std::int64_t k = key(cx, cy);
                auto it = std::lower_bound(items.begin(), items.end(), std::pair<std::int64_t, std::size_t>{k, 0});
                for (; it != items.end() && it->first == k; ++it) seen.push_back(it->second);
            }
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        for (const std::size_t i : seen)
            if (f(i)) return;
    }
};

bool inside(const Rect& area, double x, double y) { return x >= area.x0 && x <= area.x1 && y >= area.y0 && y <= area.y1; }

} // namespace

MassingBuildToCounts massingBuildTo(std::vector<MassingBox>& boxes, const std::vector<MassingStreet>& streets,
                                    const MassingGround& ground, const MassingBuildTo& opts) {
    MassingBuildToCounts counts;
    const Rect area = toRect(opts.area);
    const std::vector<Seg> segs = laneSegments(streets, area, opts.laneUpTo, opts.kinds);
    counts.lanes = segs.size();
    Scene scene(boxes, ground);
    scene.build();
    scene.strips = streetStrips(streets);
    for (const MassingRect& r : opts.keepClear) scene.clear.push_back(toRect(r));

    const auto settle = [&](std::size_t i, const Rect& r) {
        MassingBox& b = boxes[i];
        setBounds(b, r);
        double lo = 0.0, hi = 0.0;
        groundSpan(ground, r, lo, hi);
        b.z0 = std::min(b.z0, lo - opts.sink);
        scene.classify(i);
    };

    // Cut back the buildings reaching into a lane.
    for (const Seg& seg : segs) {
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (!scene.building[i] || !inside(area, boxes[i].cx, boxes[i].cy)) continue;
            const Rect& b = scene.bounds[i];
            const bool x = seg.alongX;
            if (std::min(a1(b, x), seg.s1) - std::max(a0(b, x), seg.s0) <= kEdgeTol) continue;
            if (!(p0(b, x) < seg.c + seg.half - kEdgeTol && p1(b, x) > seg.c - seg.half + kEdgeTol)) continue;
            const double mid = 0.5 * (p0(b, x) + p1(b, x));
            Rect r = mid >= seg.c ? make(x, a0(b, x), a1(b, x), seg.c + seg.half, p1(b, x))
                                  : make(x, a0(b, x), a1(b, x), p0(b, x), seg.c - seg.half);
            if (p1(r, x) - p0(r, x) < 1.0) continue; // nothing sensible is left: leave it for the table
            settle(i, r);
            ++counts.trimmed;
        }
    }

    // Pull the near faces out to the lines.
    for (const Seg& seg : segs) {
        const bool x = seg.alongX;
        for (const int side : {1, -1}) {
            const double line = seg.c + side * seg.half;
            for (std::size_t i = 0; i < boxes.size(); ++i) {
                if (!scene.building[i] || !inside(area, boxes[i].cx, boxes[i].cy)) continue;
                const Rect b = scene.bounds[i];
                const double lo = std::max(a0(b, x), seg.s0), hi = std::min(a1(b, x), seg.s1);
                if (hi - lo < kOverlapMin) continue;
                const double nearFace = side > 0 ? p0(b, x) : p1(b, x);
                const double farFace = side > 0 ? p1(b, x) : p0(b, x);
                const double gap = side * (nearFace - line);
                if (side * (farFace - seg.c) <= 0.0) continue; // on the other side
                if (gap <= kEdgeTol || gap > opts.reach) continue;
                const Rect pull = make(x, a0(b, x) + 0.05, a1(b, x) - 0.05, line, nearFace);
                if (scene.blocked(pull, i, &seg)) {
                    ++counts.blocked;
                    continue;
                }
                settle(i, make(x, a0(b, x), a1(b, x), line, farFace));
                ++counts.pulled;
            }
        }
    }

    // Fill the gaps along each frontage.
    std::vector<std::pair<double, double>> cover;
    std::vector<std::size_t> perStreet(streets.size(), 0);
    // Go on from the highest "<street>-IN<n>" already there (an earlier pass's infill), so ids stay unique.
    {
        std::vector<std::pair<std::string, std::size_t>> byId;
        byId.reserve(streets.size());
        for (std::size_t s = 0; s < streets.size(); ++s) byId.emplace_back(streets[s].id, s);
        std::sort(byId.begin(), byId.end());
        for (const MassingBox& b : boxes) {
            const std::size_t at = b.id.rfind("-IN");
            if (at == std::string::npos || at + 3 >= b.id.size()) continue;
            std::size_t n = 0;
            bool digits = true;
            for (std::size_t k = at + 3; k < b.id.size() && digits; ++k) {
                const char c = b.id[k];
                digits = c >= '0' && c <= '9';
                n = n * 10 + static_cast<std::size_t>(c - '0');
            }
            if (!digits) continue;
            const std::string street = b.id.substr(0, at);
            auto it = std::lower_bound(byId.begin(), byId.end(), std::pair<std::string, std::size_t>{street, 0});
            for (; it != byId.end() && it->first == street; ++it) perStreet[it->second] = std::max(perStreet[it->second], n);
        }
    }
    for (const Seg& seg : segs) {
        const bool x = seg.alongX;
        for (const int side : {1, -1}) {
            const double line = seg.c + side * seg.half;
            const Rect band = make(x, seg.s0, seg.s1, line, line + side * kProbe);
            cover.clear();
            const auto take = [&](const Rect& r) {
                if (!overlaps(band, r, kEdgeTol)) return;
                cover.emplace_back(std::max(a0(r, x), seg.s0), std::min(a1(r, x), seg.s1));
            };
            for (std::size_t i = 0; i < boxes.size(); ++i)
                if (scene.solid[i] || scene.building[i]) take(scene.bounds[i]);
            for (const Strip& s : scene.strips)
                if (!(s.street == seg.street && s.index == seg.index)) take(s.r);
            for (const Rect& c : scene.clear) take(c);
            std::sort(cover.begin(), cover.end());
            std::vector<std::pair<double, double>> gaps;
            double at = seg.s0;
            for (const auto& [c0, c1] : cover) {
                if (c0 > at) gaps.emplace_back(at, c0);
                at = std::max(at, c1);
            }
            if (at < seg.s1) gaps.emplace_back(at, seg.s1);
            // How far the area reaches behind the line.
            const double areaDepth = side > 0 ? (x ? area.y1 : area.x1) - line : line - (x ? area.y0 : area.x0);
            for (const auto& [g0, g1] : gaps) {
                const double len = g1 - g0;
                if (len < opts.minGap) continue;
                const int lots = std::max(1, static_cast<int>(std::ceil(len / opts.lotMax - 1e-9)));
                const double w = len / lots;
                for (int k = 0; k < lots; ++k) {
                    const double l0 = g0 + w * k, l1 = g0 + w * (k + 1);
                    double depth = std::min(opts.depthMax, areaDepth);
                    const auto limit = [&](const Rect& r) {
                        if (std::min(a1(r, x), l1 - 0.05) - std::max(a0(r, x), l0 + 0.05) <= 0.0) return;
                        const double nearFace = side > 0 ? p0(r, x) : p1(r, x);
                        const double farFace = side > 0 ? p1(r, x) : p0(r, x);
                        if (side * (farFace - line) <= kEdgeTol) return; // in front of the line
                        depth = std::min(depth, std::max(0.0, side * (nearFace - line)));
                    };
                    for (std::size_t i = 0; i < boxes.size(); ++i)
                        if (scene.solid[i] || scene.building[i]) limit(scene.bounds[i]);
                    for (const Strip& s : scene.strips)
                        if (!(s.street == seg.street && s.index == seg.index)) limit(s.r);
                    for (const Rect& c : scene.clear) limit(c);
                    if (depth < opts.depthMin) continue;
                    const Rect r = make(x, l0, l1, line, line + side * depth);
                    MassingBox b;
                    b.id = streets[seg.street].id + "-IN" + std::to_string(++perStreet[seg.street]);
                    b.row = b.id;
                    b.kind = "kit";
                    b.source = MassingSource::Invented;
                    b.cover = false;
                    b.rot = 0.0;
                    setBounds(b, r);
                    double lo = 0.0, hi = 0.0;
                    groundSpan(ground, r, lo, hi);
                    if (lo < opts.groundMin) continue;
                    const std::uint32_t pick = (hashOf(b.id) >> 8) % 10u;
                    const int storeys = pick < 5u ? 2 : (pick < 9u ? 3 : 4);
                    b.z0 = lo - opts.sink;
                    b.z1 = hi + storeys * opts.storey + 0.5;
                    scene.add(b);
                    ++counts.infill;
                }
            }
        }
    }

    // The frontages of the segments at an angle.
    if (opts.oblique) {
        CellIndex boxIndex, stripIndex;
        for (std::size_t i = 0; i < boxes.size(); ++i)
            if (scene.solid[i] || scene.building[i]) boxIndex.add(scene.bounds[i], i);
        struct Piece {
            MassingVec p, q;
            double half = 0.0;
        };
        std::vector<Piece> pieces;
        for (const MassingStreet& st : streets)
            for (std::size_t i = 0; i + 1 < st.points.size(); ++i) {
                const MassingVec& p = st.points[i];
                const MassingVec& q = st.points[i + 1];
                const double h = 0.5 * st.width;
                stripIndex.add({std::min(p.x, q.x) - h, std::min(p.y, q.y) - h, std::max(p.x, q.x) + h, std::max(p.y, q.y) + h},
                               pieces.size());
                pieces.push_back({p, q, h});
            }
        std::vector<std::size_t> seen;
        // The lowest and highest ground under a turned footprint, read every metre or so (edges included).
        const auto spanOf = [&](const Obb& o, double& lo, double& hi) {
            const int nu = std::max(1, static_cast<int>(std::ceil(2.0 * o.hu))), nv = std::max(1, static_cast<int>(std::ceil(2.0 * o.hv)));
            lo = std::numeric_limits<double>::infinity();
            hi = -lo;
            for (int a = 0; a <= nu; ++a)
                for (int c = 0; c <= nv; ++c) {
                    const double fu = o.hu * (2.0 * a / nu - 1.0), fv = o.hv * (2.0 * c / nv - 1.0);
                    const double g = groundAt(ground, o.cx + o.ux * fu - o.uy * fv, o.cy + o.uy * fu + o.ux * fv);
                    lo = std::min(lo, g);
                    hi = std::max(hi, g);
                }
        };
        const auto isFree = [&](const Obb& o) {
            const Rect r = boundsOf(o);
            bool hit = false;
            boxIndex.visit(r, seen, [&](std::size_t i) { return hit = obbOverlap(o, obbOf(boxes[i]), kEdgeTol); });
            if (hit) return false;
            stripIndex.visit(r, seen, [&](std::size_t i) {
                const Piece& pc = pieces[i];
                return hit = segmentObbDistance(pc.p, pc.q, o) < pc.half - kEdgeTol;
            });
            if (hit) return false;
            for (const Rect& c : scene.clear)
                if (obbOverlap(o, obbOf(c), kEdgeTol)) return false;
            return true;
        };
        for (std::size_t s = 0; s < streets.size(); ++s) {
            const MassingStreet& st = streets[s];
            if (!(st.width <= opts.laneUpTo) || !laneKind(st, opts.kinds)) continue;
            const double h = 0.5 * st.width;
            for (std::size_t i = 0; i + 1 < st.points.size(); ++i) {
                const MassingVec& p = st.points[i];
                const MassingVec& q = st.points[i + 1];
                if (std::fabs(p.x - q.x) <= kAxisTol || std::fabs(p.y - q.y) <= kAxisTol) continue; // done above
                const double len = std::hypot(q.x - p.x, q.y - p.y);
                if (len < opts.minGap) continue;
                const double ux = (q.x - p.x) / len, uy = (q.y - p.y) / len;
                const double heading = std::atan2(uy, ux) * 180.0 / kPi;
                for (const int side : {1, -1}) {
                    const double nx = -uy * side, ny = ux * side; // away from the centre line
                    // A cell along a..b, d deep behind the line.
                    const auto cellAt = [&](double a, double b, double d) {
                        const double sm = 0.5 * (a + b), off = h + 0.5 * d;
                        return Obb{p.x + ux * sm + nx * off, p.y + uy * sm + ny * off, ux, uy, 0.5 * (b - a), 0.5 * d};
                    };
                    const int steps = static_cast<int>(std::floor(len));
                    int run = -1;
                    for (int k = 0; k <= steps; ++k) {
                        bool ok = false;
                        if (k < steps) {
                            const Obb probe = cellAt(k + 0.05, k + 0.95, kProbe);
                            ok = inside(area, probe.cx, probe.cy) && isFree(probe);
                        }
                        if (ok) {
                            if (run < 0) run = k;
                            continue;
                        }
                        if (run < 0) continue;
                        const double g0 = run, g1 = k;
                        run = -1;
                        if (g1 - g0 < opts.minGap) continue;
                        const int lots = std::max(1, static_cast<int>(std::ceil((g1 - g0) / opts.lotMax - 1e-9)));
                        const double w = (g1 - g0) / lots;
                        for (int l = 0; l < lots; ++l) {
                            const double l0 = g0 + w * l, l1 = g0 + w * (l + 1);
                            double depth = 0.0;
                            for (double d = 1.0; d <= opts.depthMax + 1e-9; d += 1.0) {
                                const Obb c = cellAt(l0 + 0.05, l1 - 0.05, d);
                                if (!inside(area, c.cx, c.cy) || !isFree(c)) break;
                                double glo = 0.0, ghi = 0.0;
                                spanOf(c, glo, ghi);
                                if (ghi - glo > opts.obliqueRise || glo < opts.groundMin) break;
                                depth = d;
                            }
                            if (depth < opts.depthMin) continue;
                            const Obb o = cellAt(l0, l1, depth);
                            MassingBox b;
                            b.id = st.id + "-IN" + std::to_string(++perStreet[s]);
                            b.row = b.id;
                            b.kind = "kit";
                            b.source = MassingSource::Invented;
                            b.cover = false;
                            b.cx = o.cx;
                            b.cy = o.cy;
                            b.sx = l1 - l0;
                            b.sy = depth;
                            b.rot = heading;
                            double lo = 0.0, hi = 0.0;
                            spanOf(o, lo, hi);
                            const std::uint32_t pick = (hashOf(b.id) >> 8) % 10u;
                            const int storeys = pick < 5u ? 2 : (pick < 9u ? 3 : 4);
                            b.z0 = lo - opts.sink;
                            b.z1 = hi + storeys * opts.storey + 0.5;
                            scene.add(b);
                            boxIndex.add(scene.bounds.back(), boxes.size() - 1);
                            ++counts.infill;
                            ++counts.oblique;
                        }
                    }
                }
            }
        }
    }
    return counts;
}

MassingLaneWidth massingLaneWidth(const MassingStreet& lane, const std::vector<MassingBox>& boxes, const MassingRect& area,
                                  double reach, double step) {
    MassingLaneWidth out;
    std::vector<MassingStreet> one{lane};
    const std::vector<Seg> segs = laneSegments(one, toRect(area), std::numeric_limits<double>::infinity(), {});
    std::vector<Rect> walls;
    for (const MassingBox& b : boxes) {
        if (!massingVisible(massingRole(b)) || !massingLookIsBuilding(massingLookSurface(b)) || b.kind == "wall") continue;
        walls.push_back(boundsOf(b));
    }
    bool any = false;
    for (const Seg& seg : segs) {
        const bool x = seg.alongX;
        for (double s = seg.s0 + 0.5 * step; s < seg.s1; s += step) {
            ++out.samples;
            double near[2] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
            for (const Rect& r : walls) {
                if (!(a0(r, x) <= s && s <= a1(r, x))) continue;
                for (int k = 0; k < 2; ++k) {
                    const int side = k == 0 ? 1 : -1;
                    const double nearFace = side > 0 ? p0(r, x) : p1(r, x);
                    const double farFace = side > 0 ? p1(r, x) : p0(r, x);
                    if (side * (farFace - seg.c) <= 0.0) continue;
                    const double d = std::max(0.0, side * (nearFace - seg.c));
                    if (d <= reach) near[k] = std::min(near[k], d);
                }
            }
            if (!std::isfinite(near[0]) || !std::isfinite(near[1])) continue;
            const double wall = near[0] + near[1];
            ++out.bothSides;
            out.minWall = any ? std::min(out.minWall, wall) : wall;
            out.maxWall = any ? std::max(out.maxWall, wall) : wall;
            any = true;
        }
    }
    return out;
}

namespace {

// A flat dressing piece over [r], u along x, laid to the ground's run along x. False when the ground under it is not
// that plane to within `fit`.
bool lay(const MassingGround& g, const Rect& r, double fit, MassingBox& out) {
    const int nx = std::clamp(static_cast<int>(std::ceil(r.x1 - r.x0)) + 1, 3, 9);
    const int ny = std::clamp(static_cast<int>(std::ceil(r.y1 - r.y0)) + 1, 3, 9);
    double a = 0.0, b = 0.0;
    for (int j = 0; j < ny; ++j) {
        const double y = r.y0 + (r.y1 - r.y0) * j / (ny - 1);
        a += groundAt(g, r.x0, y);
        b += groundAt(g, r.x1, y);
    }
    a /= ny;
    b /= ny;
    double lo = std::numeric_limits<double>::infinity();
    for (int i = 0; i < nx; ++i) {
        const double t = static_cast<double>(i) / (nx - 1);
        const double x = r.x0 + (r.x1 - r.x0) * t;
        const double plane = a + (b - a) * t;
        for (int j = 0; j < ny; ++j) {
            const double h = groundAt(g, x, r.y0 + (r.y1 - r.y0) * j / (ny - 1));
            if (std::fabs(h - plane) > fit) return false;
            lo = std::min(lo, h);
        }
    }
    setBounds(out, r);
    out.rot = 0.0;
    out.source = MassingSource::Invented;
    out.cover = false;
    if (std::fabs(b - a) < 1e-3) {
        const double mid = 0.5 * (a + b);
        out.sloped = false;
        out.z0 = std::min(lo, mid) - kMassingLaneDressSink;
        out.z1 = mid + kMassingLaneDressTop;
    } else {
        out.sloped = true;
        out.slopeFrom = a + kMassingLaneDressTop;
        out.slopeTo = b + kMassingLaneDressTop;
        out.slopeThick = kMassingLaneDressTop + kMassingLaneDressSink;
        out.z0 = std::min(out.slopeFrom, out.slopeTo) - out.slopeThick;
        out.z1 = std::max(out.slopeFrom, out.slopeTo);
    }
    return true;
}

// [s0, s1] less the cuts, in order.
std::vector<std::pair<double, double>> subtract(double s0, double s1, std::vector<std::pair<double, double>> cuts) {
    std::sort(cuts.begin(), cuts.end());
    std::vector<std::pair<double, double>> out;
    double at = s0;
    for (const auto& [c0, c1] : cuts) {
        if (c1 <= at) continue;
        if (c0 >= s1) break;
        if (c0 > at) out.emplace_back(at, c0);
        at = std::max(at, c1);
    }
    if (at < s1) out.emplace_back(at, s1);
    return out;
}

double segDistance(double px, double py, const MassingVec& a, const MassingVec& b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0.0 ? ((px - a.x) * dx + (py - a.y) * dy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double qx = a.x + t * dx - px, qy = a.y + t * dy - py;
    return std::sqrt(qx * qx + qy * qy);
}

double streetDistance(const MassingStreet& s, double x, double y) {
    double best = std::numeric_limits<double>::infinity();
    if (s.points.size() == 1) return std::hypot(s.points[0].x - x, s.points[0].y - y);
    for (std::size_t i = 0; i + 1 < s.points.size(); ++i) best = std::min(best, segDistance(x, y, s.points[i], s.points[i + 1]));
    return best;
}

} // namespace

MassingLaneDressCounts massingLaneDress(const std::vector<MassingStreet>& streets, const MassingGround& ground,
                                        const MassingLaneDress& opts, std::vector<MassingBox>& out) {
    MassingLaneDressCounts counts;
    const Rect area = toRect(opts.area);
    const std::vector<Seg> segs = laneSegments(streets, area, opts.laneUpTo, opts.kinds);
    const std::vector<Strip> strips = streetStrips(streets);
    std::vector<std::size_t> n(streets.size() * 4, 0);
    const auto next = [&](std::size_t street, int k, const char* tag) {
        return streets[street].id + tag + std::to_string(++n[street * 4 + static_cast<std::size_t>(k)]);
    };
    for (const Seg& seg : segs) {
        const bool x = seg.alongX;
        const MassingStreet& st = streets[seg.street];
        // What the centre line runs through: other streets, keep-clear rects.
        const Rect centre = make(x, seg.s0, seg.s1, seg.c - 0.5 * opts.gutterWidth, seg.c + 0.5 * opts.gutterWidth);
        const Rect strip = make(x, seg.s0, seg.s1, seg.c - seg.half, seg.c + seg.half);
        std::vector<std::pair<double, double>> cuts;
        for (const Strip& s : strips) {
            if (s.street == seg.street && s.index == seg.index) continue;
            if (s.street == seg.street && (s.index + 1 == seg.index || seg.index + 1 == s.index)) continue; // its own bends
            if (overlaps(centre, s.r, 0.0)) cuts.emplace_back(a0(s.r, x) - opts.endGap, a1(s.r, x) + opts.endGap);
        }
        for (const MassingRect& k : opts.keepClear) {
            const Rect r = toRect(k);
            if (overlaps(strip, r, 0.0)) cuts.emplace_back(a0(r, x) - opts.endGap, a1(r, x) + opts.endGap);
        }
        const std::vector<std::pair<double, double>> free = subtract(seg.s0 + opts.endGap, seg.s1 - opts.endGap, cuts);

        // Drain covers on the centre line, every drainEvery along the free stretches; the gutter runs between them.
        std::vector<std::pair<double, double>> drains;
        for (const auto& [f0, f1] : free) {
            for (double s = f0 + 0.5 * opts.drainEvery; s + 0.5 * opts.drainSize <= f1; s += opts.drainEvery) {
                const double h = 0.5 * opts.drainSize;
                MassingBox d;
                d.kind = "drain";
                if (!lay(ground, make(x, s - h, s + h, seg.c - h, seg.c + h), opts.fit, d)) {
                    ++counts.uneven;
                    continue;
                }
                d.id = next(seg.street, 1, "-D");
                d.row = d.id;
                out.push_back(d);
                ++counts.drains;
                drains.emplace_back(s - h - 0.05, s + h + 0.05);
            }
        }
        for (const auto& [f0, f1] : free) {
            for (const auto& [r0, r1] : subtract(f0, f1, drains)) {
                const int runs = std::max(1, static_cast<int>(std::ceil((r1 - r0) / opts.gutterRun - 1e-9)));
                const double w = (r1 - r0) / runs;
                if (w < 0.5) continue;
                for (int k = 0; k < runs; ++k) {
                    MassingBox gt;
                    gt.kind = "gutter";
                    if (!lay(ground, make(x, r0 + w * k, r0 + w * (k + 1), seg.c - 0.5 * opts.gutterWidth, seg.c + 0.5 * opts.gutterWidth),
                             opts.fit, gt)) {
                        ++counts.uneven;
                        continue;
                    }
                    gt.id = next(seg.street, 0, "-G");
                    gt.row = gt.id;
                    out.push_back(gt);
                    ++counts.gutters;
                }
            }
        }

        // Patches beside the gutter, placed by the lane's hash.
        double freeLen = 0.0;
        for (const auto& [f0, f1] : free) freeLen += f1 - f0;
        const int patches = static_cast<int>(std::floor(freeLen / opts.patchEvery + 0.5));
        const double room = seg.half - 0.6 - 0.5 * opts.gutterWidth - 0.05;
        for (int k = 0; k < patches && room >= 0.8; ++k) {
            const std::uint32_t h = hashOf(st.id + "#" + std::to_string(seg.index) + "#" + std::to_string(k));
            const double len = 1.5 + 2.5 * ((h & 0xffu) / 255.0);
            const double across = 0.8 + (room - 0.8) * (((h >> 8) & 0xffu) / 255.0);
            const int side = (h >> 16) & 1u ? 1 : -1;
            // Its middle, spread over the free length.
            double s = (k + 0.25 + 0.5 * (((h >> 20) & 0xffu) / 255.0)) * freeLen / patches;
            for (const auto& [f0, f1] : free) {
                if (s > f1 - f0) {
                    s -= f1 - f0;
                    continue;
                }
                const double mid = std::clamp(f0 + s, f0 + 0.5 * len, f1 - 0.5 * len);
                if (f1 - f0 < len) break;
                const double inner = seg.c + side * (0.5 * opts.gutterWidth + 0.05);
                MassingBox p;
                p.kind = "patch";
                if (!lay(ground, make(x, mid - 0.5 * len, mid + 0.5 * len, inner, inner + side * across), opts.fit, p)) {
                    ++counts.uneven;
                    break;
                }
                bool clash = false;
                for (const auto& [d0, d1] : drains)
                    if (mid + 0.5 * len > d0 && mid - 0.5 * len < d1) clash = true;
                if (clash) break;
                p.id = next(seg.street, 2, "-P");
                p.row = p.id;
                out.push_back(p);
                ++counts.patches;
                break;
            }
        }

        // A give-way bar across each mouth onto a wider street.
        for (const double end : {seg.end0, seg.end1}) {
            if (end < seg.s0 - 1e-6 || end > seg.s1 + 1e-6) continue; // clipped away by the area
            const double ex = x ? end : seg.c, ey = x ? seg.c : end;
            bool mouth = false;
            for (const MassingStreet& other : streets) {
                if (&other == &st || !(other.width > opts.laneUpTo)) continue;
                if (streetDistance(other, ex, ey) <= 0.5 * other.width + 3.0) mouth = true;
            }
            for (const MassingRect& k : opts.keepClear) {
                const Rect r = toRect(k);
                if (ex >= r.x0 - 3.0 && ex <= r.x1 + 3.0 && ey >= r.y0 - 3.0 && ey <= r.y1 + 3.0) mouth = true;
            }
            if (!mouth) continue;
            const double inward = end == seg.end0 ? 1.0 : -1.0;
            const double s = end + inward * (0.5 + 0.5 * opts.markWidth);
            MassingBox m;
            m.kind = "marking";
            if (!lay(ground, make(x, s - 0.5 * opts.markWidth, s + 0.5 * opts.markWidth, seg.c - seg.half + 0.5, seg.c + seg.half - 0.5),
                     opts.fit, m)) {
                ++counts.uneven;
                continue;
            }
            m.id = next(seg.street, 3, "-M");
            m.row = m.id;
            out.push_back(m);
            ++counts.marks;
        }
    }
    return counts;
}

std::size_t massingLanePave(std::vector<MassingStreet>& streets, double upTo, double margin) {
    if (!(upTo > 0.0)) return 0;
    std::size_t n = 0;
    for (MassingStreet& s : streets) {
        if (!(s.width <= upTo)) continue;
        s.width += 2.0 * std::max(margin, 0.0);
        s.pavement = 0.0;
        ++n;
    }
    return n;
}

std::size_t massingDropLanePads(std::vector<MassingBox>& pads, const std::vector<MassingStreet>& streets, double upTo,
                                double reach) {
    if (!(upTo > 0.0)) return 0;
    const auto onLane = [&](const MassingBox& p) {
        for (const MassingStreet& s : streets) {
            if (!(s.width <= upTo)) continue;
            if (streetDistance(s, p.cx, p.cy) <= 0.5 * s.width + reach) return true;
        }
        return false;
    };
    const auto end = std::remove_if(pads.begin(), pads.end(), onLane);
    const std::size_t n = static_cast<std::size_t>(pads.end() - end);
    pads.erase(end, pads.end());
    return n;
}

namespace {

std::uint32_t yardHash(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (const char ch : s) h = (h ^ static_cast<std::uint8_t>(ch)) * 16777619u;
    return h;
}

} // namespace

MassingYardCounts massingYardFill(std::vector<MassingBox>& boxes, const std::vector<MassingStreet>& streets,
                                  const std::vector<MassingFoot>& taken, const MassingGround& ground,
                                  const MassingYards& opts) {
    MassingYardCounts n;
    const double c = opts.cell >= 0.5 ? opts.cell : 2.0;
    const Rect A = toRect(opts.area);
    const int W = static_cast<int>(std::ceil((A.x1 - A.x0) / c)), H = static_cast<int>(std::ceil((A.y1 - A.y0) / c));
    if (W <= 0 || H <= 0) return n;
    const std::size_t cells = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    std::vector<std::uint8_t> busy(cells, 0), street(cells, 0);
    std::vector<double> top(cells, -std::numeric_limits<double>::infinity()); // a flat zone's top over a cell
    auto at = [&](int i, int j) {
        return static_cast<std::size_t>(j) * static_cast<std::size_t>(W) + static_cast<std::size_t>(i);
    };
    auto cx = [&](int i) { return A.x0 + (i + 0.5) * c; };
    auto cy = [&](int j) { return A.y0 + (j + 0.5) * c; };
    // Calls f(i, j) for each cell whose centre lies in a turned rect (half sizes hx, hy).
    auto eachIn = [&](double x, double y, double hx, double hy, double rot, auto&& f) {
        const double t = rot * kPi / 180.0, ct = std::cos(t), st = std::sin(t);
        const double ex = std::fabs(ct) * hx + std::fabs(st) * hy, ey = std::fabs(st) * hx + std::fabs(ct) * hy;
        const int i0 = std::max(0, static_cast<int>(std::floor((x - ex - A.x0) / c)));
        const int i1 = std::min(W - 1, static_cast<int>(std::floor((x + ex - A.x0) / c)));
        const int j0 = std::max(0, static_cast<int>(std::floor((y - ey - A.y0) / c)));
        const int j1 = std::min(H - 1, static_cast<int>(std::floor((y + ey - A.y0) / c)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const double dx = cx(i) - x, dy = cy(j) - y;
                if (std::fabs(dx * ct + dy * st) <= hx && std::fabs(-dx * st + dy * ct) <= hy) f(i, j);
            }
    };
    for (const MassingBox& b : boxes) {
        const MassingRole role = massingRole(b);
        if (role == MassingRole::Hidden) continue;
        if (role == MassingRole::Zone) {
            const bool slope = b.sloped || b.kind.find("slope") != std::string::npos;
            eachIn(b.cx, b.cy, b.sx * 0.5, b.sy * 0.5, b.rot, [&](int i, int j) {
                if (slope)
                    busy[at(i, j)] = 1;
                else
                    top[at(i, j)] = std::max(top[at(i, j)], b.z1);
            });
            continue;
        }
        eachIn(b.cx, b.cy, b.sx * 0.5 + opts.boxGap, b.sy * 0.5 + opts.boxGap, b.rot,
               [&](int i, int j) { busy[at(i, j)] = 1; });
    }
    for (const MassingFoot& f : taken)
        eachIn(f.x, f.y, f.hx + opts.streetBack, f.hy + opts.streetBack, f.rot,
               [&](int i, int j) { busy[at(i, j)] = street[at(i, j)] = 1; });
    for (const MassingStreet& s : streets) {
        const double r = s.width * 0.5 + opts.streetBack;
        for (std::size_t k = 1; k < s.points.size(); ++k) {
            const MassingVec& p = s.points[k - 1];
            const MassingVec& q = s.points[k];
            const double dx = q.x - p.x, dy = q.y - p.y, l2 = std::max(1e-9, dx * dx + dy * dy);
            const int i0 = std::max(0, static_cast<int>(std::floor((std::min(p.x, q.x) - r - A.x0) / c)));
            const int i1 = std::min(W - 1, static_cast<int>(std::floor((std::max(p.x, q.x) + r - A.x0) / c)));
            const int j0 = std::max(0, static_cast<int>(std::floor((std::min(p.y, q.y) - r - A.y0) / c)));
            const int j1 = std::min(H - 1, static_cast<int>(std::floor((std::max(p.y, q.y) + r - A.y0) / c)));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    const double u = std::clamp(((cx(i) - p.x) * dx + (cy(j) - p.y) * dy) / l2, 0.0, 1.0);
                    if (std::hypot(cx(i) - p.x - u * dx, cy(j) - p.y - u * dy) <= r)
                        busy[at(i, j)] = street[at(i, j)] = 1;
                }
        }
    }
    for (const MassingRect& k : opts.keepClear)
        eachIn((k.x0 + k.x1) * 0.5, (k.y0 + k.y1) * 0.5, (k.x1 - k.x0) * 0.5, (k.y1 - k.y0) * 0.5, 0.0,
               [&](int i, int j) { busy[at(i, j)] = 1; });
    // The ground per cell, and the low ground with waterKeep round its edge.
    std::vector<double> gr(cells);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) gr[at(i, j)] = std::max(groundAt(ground, cx(i), cy(j)), top[at(i, j)]);
    auto gAt = [&](double x, double y) {
        const int i = std::clamp(static_cast<int>(std::floor((x - A.x0) / c)), 0, W - 1);
        const int j = std::clamp(static_cast<int>(std::floor((y - A.y0) / c)), 0, H - 1);
        return std::max(groundAt(ground, x, y), top[at(i, j)]);
    };
    {
        const int rk = static_cast<int>(std::ceil(opts.waterKeep / c));
        std::vector<std::uint8_t> low(cells, 0);
        for (std::size_t k = 0; k < cells; ++k) low[k] = gr[k] < opts.groundMin ? 1 : 0;
        for (int j = 0; j < H; ++j)
            for (int i = 0; i < W; ++i) {
                if (!low[at(i, j)]) continue;
                busy[at(i, j)] = 1;
                const bool edge = (i > 0 && !low[at(i - 1, j)]) || (i + 1 < W && !low[at(i + 1, j)]) ||
                                  (j > 0 && !low[at(i, j - 1)]) || (j + 1 < H && !low[at(i, j + 1)]);
                if (!edge) continue;
                for (int b = std::max(0, j - rk); b <= std::min(H - 1, j + rk); ++b)
                    for (int a = std::max(0, i - rk); a <= std::min(W - 1, i + rk); ++a)
                        if ((a - i) * (a - i) + (b - j) * (b - j) <= rk * rk) busy[at(a, b)] = 1;
            }
    }
    // A summed-area table of the taken cells.
    const std::size_t sw = static_cast<std::size_t>(W) + 1;
    std::vector<std::uint32_t> sat(sw * (static_cast<std::size_t>(H) + 1), 0);
    auto S = [&](int i, int j) -> std::uint32_t& {
        return sat[static_cast<std::size_t>(j) * sw + static_cast<std::size_t>(i)];
    };
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) S(i + 1, j + 1) = busy[at(i, j)] + S(i, j + 1) + S(i + 1, j) - S(i, j);
    auto takenIn = [&](int i, int j, int ni, int nj) {
        return S(i + ni, j + nj) - S(i, j + nj) - S(i + ni, j) + S(i, j);
    };

    const double sizes[][2] = {{36, 28}, {28, 36}, {28, 22}, {22, 28}, {22, 18}, {18, 22}, {16, 14}, {14, 16}};
    std::vector<Rect> yards;
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            if (busy[at(i, j)]) continue;
            for (const auto& sz : sizes) {
                const int ni = static_cast<int>(std::ceil(sz[0] / c)), nj = static_cast<int>(std::ceil(sz[1] / c));
                if (i + ni > W || j + nj > H || takenIn(i, j, ni, nj) != 0) continue;
                const Rect R{A.x0 + i * c, A.y0 + j * c, A.x0 + (i + ni) * c, A.y0 + (j + nj) * c};
                bool near = false;
                for (const Rect& y : yards)
                    if (overlaps({R.x0 - opts.gap, R.y0 - opts.gap, R.x1 + opts.gap, R.y1 + opts.gap}, y, 0.0)) near = true;
                if (near) continue;
                double lo = std::numeric_limits<double>::infinity(), hi = -lo;
                for (int b = j; b < j + nj; ++b)
                    for (int a = i; a < i + ni; ++a) {
                        lo = std::min(lo, gr[at(a, b)]);
                        hi = std::max(hi, gr[at(a, b)]);
                    }
                if (hi - lo > opts.maxRise) continue;
                yards.push_back(R);
                ++n.yards;
                n.area += (R.x1 - R.x0) * (R.y1 - R.y0);
                char buf[48];
                std::snprintf(buf, sizeof buf, "%s-%04zu", opts.prefix.c_str(), n.yards);
                const std::string id = buf;
                const std::uint32_t h = yardHash(id);
                // The gate's side: the one with a street nearest outside it (0 south, 1 east, 2 north, 3 west).
                int gate = static_cast<int>(h % 4u), best = 1 << 30;
                const int mi = i + ni / 2, mj = j + nj / 2;
                for (int side = 0; side < 4; ++side) {
                    for (int k = 1; k <= 12; ++k) {
                        const int a = side == 1 ? i + ni - 1 + k : side == 3 ? i - k : mi;
                        const int b = side == 2 ? j + nj - 1 + k : side == 0 ? j - k : mj;
                        if (a < 0 || b < 0 || a >= W || b >= H) break;
                        if (street[at(a, b)]) {
                            if (k < best) {
                                best = k;
                                gate = side;
                            }
                            break;
                        }
                    }
                }
                auto addBox = [&](const std::string& bid, const char* kind, double x0, double y0, double x1, double y1,
                                  double over) {
                    double l = std::numeric_limits<double>::infinity(), u = -l;
                    const double stepY = std::max(0.5, (y1 - y0) / 4.0), stepX = std::max(0.5, (x1 - x0) / 4.0);
                    for (double y = y0; y <= y1 + 1e-9; y += stepY)
                        for (double x = x0; x <= x1 + 1e-9; x += stepX) {
                            const double g = gAt(x, y);
                            l = std::min(l, g);
                            u = std::max(u, g);
                        }
                    MassingBox b;
                    b.id = bid;
                    b.row = id;
                    b.kind = kind;
                    b.source = MassingSource::Invented;
                    b.cx = (x0 + x1) * 0.5;
                    b.cy = (y0 + y1) * 0.5;
                    b.sx = x1 - x0;
                    b.sy = y1 - y0;
                    b.z0 = l - 0.1;
                    b.z1 = u + over;
                    boxes.push_back(std::move(b));
                };
                // The shed across the half away from the gate, 1.5 m in from the fence.
                const double frac = 0.45 + 0.05 * static_cast<double>((h >> 4) % 4u);
                const double in = 1.5, storeys = 2.0 + static_cast<double>((h >> 8) % 2u);
                Rect sh{R.x0 + in, R.y0 + in, R.x1 - in, R.y1 - in};
                if (gate == 0) sh.y0 = R.y1 - (R.y1 - R.y0) * frac;
                if (gate == 2) sh.y1 = R.y0 + (R.y1 - R.y0) * frac;
                if (gate == 1) sh.x1 = R.x0 + (R.x1 - R.x0) * frac;
                if (gate == 3) sh.x0 = R.x1 - (R.x1 - R.x0) * frac;
                addBox(id, "kit", sh.x0, sh.y0, sh.x1, sh.y1, storeys * opts.storey);
                ++n.sheds;
                // The fence: the south and north sides whole, east and west between them; the gate in the middle.
                int piece = 0;
                const double t = opts.fenceThick;
                for (int side = 0; side < 4; ++side) {
                    const bool alongX = side == 0 || side == 2;
                    const double s0 = alongX ? R.x0 : R.y0 + t, s1 = alongX ? R.x1 : R.y1 - t;
                    const double line = side == 0 ? R.y0 : side == 2 ? R.y1 - t : side == 3 ? R.x0 : R.x1 - t;
                    std::pair<double, double> runs[2] = {{s0, s1}, {s1, s1}};
                    if (side == gate) {
                        const double m = (s0 + s1) * 0.5;
                        runs[0] = {s0, m - opts.gateWide * 0.5};
                        runs[1] = {m + opts.gateWide * 0.5, s1};
                    }
                    const bool low = side == gate && best < (1 << 30) && opts.frontHigh > 0.0 && opts.frontPiece > 0.0;
                    for (const auto& run : runs) {
                        const double r0 = run.first, r1 = run.second;
                        if (r1 - r0 < 0.5) continue;
                        if (low) {
                            // From the gate outward: a piece, a gap, a piece ... (the last piece no shorter than 0.5 m).
                            const bool fromHi = r1 <= (s0 + s1) * 0.5 + 1e-9; // the run before the gate ends at it
                            for (double a = 0.0; a + 0.5 <= r1 - r0 + 1e-9; a += opts.frontPiece + opts.frontGap) {
                                const double b = std::min(a + opts.frontPiece, r1 - r0);
                                const double a0 = fromHi ? r1 - b : r0 + a, a1 = fromHi ? r1 - a : r0 + b;
                                std::snprintf(buf, sizeof buf, "-F%d", ++piece);
                                if (alongX)
                                    addBox(id + buf, "wall", a0, line, a1, line + t, opts.frontHigh);
                                else
                                    addBox(id + buf, "wall", line, a0, line + t, a1, opts.frontHigh);
                                ++n.fences;
                            }
                            continue;
                        }
                        const int pieces = std::max(1, static_cast<int>(std::ceil((r1 - r0) / opts.fencePiece)));
                        const double step = (r1 - r0) / pieces;
                        for (int k = 0; k < pieces; ++k) {
                            const double a0 = r0 + k * step, a1 = a0 + step;
                            std::snprintf(buf, sizeof buf, "-F%d", ++piece);
                            if (alongX)
                                addBox(id + buf, "wall", a0, line, a1, line + t, opts.fenceHigh);
                            else
                                addBox(id + buf, "wall", line, a0, line + t, a1, opts.fenceHigh);
                            ++n.fences;
                        }
                    }
                }
                i += ni - 1;
                break;
            }
        }
    return n;
}

} // namespace ftcore
