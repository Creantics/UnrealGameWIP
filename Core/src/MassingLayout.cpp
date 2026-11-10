#include "ftcore/MassingLayout.h"

#include <algorithm>
#include <cmath>
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

std::vector<Seg> laneSegments(const std::vector<MassingStreet>& streets, const Rect& area, double upTo) {
    std::vector<Seg> out;
    for (std::size_t s = 0; s < streets.size(); ++s) {
        const MassingStreet& st = streets[s];
        if (!(st.width <= upTo)) continue;
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

bool inside(const Rect& area, double x, double y) { return x >= area.x0 && x <= area.x1 && y >= area.y0 && y <= area.y1; }

} // namespace

MassingBuildToCounts massingBuildTo(std::vector<MassingBox>& boxes, const std::vector<MassingStreet>& streets,
                                    const MassingGround& ground, const MassingBuildTo& opts) {
    MassingBuildToCounts counts;
    const Rect area = toRect(opts.area);
    const std::vector<Seg> segs = laneSegments(streets, area, opts.laneUpTo);
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
    return counts;
}

MassingLaneWidth massingLaneWidth(const MassingStreet& lane, const std::vector<MassingBox>& boxes, const MassingRect& area,
                                  double reach, double step) {
    MassingLaneWidth out;
    std::vector<MassingStreet> one{lane};
    const std::vector<Seg> segs = laneSegments(one, toRect(area), std::numeric_limits<double>::infinity());
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
    const std::vector<Seg> segs = laneSegments(streets, area, opts.laneUpTo);
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

} // namespace ftcore
