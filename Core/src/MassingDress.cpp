#include "ftcore/MassingDress.h"

#include "CsvRead.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTiny = 1e-9;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kCell = 16.0;            // m: a grid cell's side
constexpr std::size_t kMaxCells = 2048;   // a grid's cells along each axis, at most
constexpr std::size_t kNone = static_cast<std::size_t>(-1);

using detail::fail;
using detail::isComment;
using detail::parseNumber;
using detail::Record;
using detail::splitCsv;
using detail::trim;
using detail::validId;
using detail::validKind;

// ---- Shared pieces ----

struct Aabb {
    double x0 = kInf, y0 = kInf, x1 = -kInf, y1 = -kInf;
};

void grow(Aabb& a, const Aabb& b) {
    a.x0 = std::min(a.x0, b.x0);
    a.y0 = std::min(a.y0, b.y0);
    a.x1 = std::max(a.x1, b.x1);
    a.y1 = std::max(a.y1, b.y1);
}

Aabb footAabb(const MassingFoot& f) {
    const double a = f.rot * kPi / 180.0;
    const double c = std::fabs(std::cos(a)), s = std::fabs(std::sin(a));
    const double hx = std::max(f.hx, 0.0), hy = std::max(f.hy, 0.0);
    const double ex = c * hx + s * hy, ey = s * hx + c * hy;
    return {f.x - ex, f.y - ey, f.x + ex, f.y + ey};
}

Aabb pointAabb(double x, double y, double r) { return {x - r, y - r, x + r, y + r}; }

// A uniform grid of cells holding item numbers, to find the items near a place without looking at all of them.
class Grid {
public:
    void reset(const Aabb& extent, double cell) {
        Aabb e = extent;
        if (!(e.x0 <= e.x1) || !(e.y0 <= e.y1)) e = {0.0, 0.0, 1.0, 1.0};
        cell = std::max({cell, (e.x1 - e.x0) / static_cast<double>(kMaxCells),
                         (e.y1 - e.y0) / static_cast<double>(kMaxCells), 1e-3});
        x0_ = e.x0;
        y0_ = e.y0;
        cell_ = cell;
        nx_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil((e.x1 - e.x0) / cell)));
        ny_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil((e.y1 - e.y0) / cell)));
        cells_.assign(nx_ * ny_, {});
        stamp_.clear();
        mark_ = 0;
    }
    void insert(std::uint32_t item, const Aabb& a) {
        if (item >= stamp_.size()) stamp_.resize(static_cast<std::size_t>(item) + 1, 0);
        const std::size_t i0 = index(a.x0, x0_, nx_), i1 = index(a.x1, x0_, nx_);
        const std::size_t j0 = index(a.y0, y0_, ny_), j1 = index(a.y1, y0_, ny_);
        for (std::size_t j = j0; j <= j1; ++j)
            for (std::size_t i = i0; i <= i1; ++i) cells_[j * nx_ + i].push_back(item);
    }
    // Calls f(item) once for each item whose cells meet `a`, until f returns true; then true.
    template <class F> bool any(const Aabb& a, F&& f) {
        if (++mark_ == 0) {
            std::fill(stamp_.begin(), stamp_.end(), 0u);
            mark_ = 1;
        }
        const std::size_t i0 = index(a.x0, x0_, nx_), i1 = index(a.x1, x0_, nx_);
        const std::size_t j0 = index(a.y0, y0_, ny_), j1 = index(a.y1, y0_, ny_);
        for (std::size_t j = j0; j <= j1; ++j)
            for (std::size_t i = i0; i <= i1; ++i)
                for (const std::uint32_t item : cells_[j * nx_ + i]) {
                    if (stamp_[item] == mark_) continue;
                    stamp_[item] = mark_;
                    if (f(item)) return true;
                }
        return false;
    }

private:
    std::size_t index(double v, double origin, std::size_t n) const {
        const double f = std::floor((v - origin) / cell_);
        if (!(f > 0.0)) return 0;
        if (f >= static_cast<double>(n - 1)) return n - 1;
        return static_cast<std::size_t>(f);
    }
    double x0_ = 0.0, y0_ = 0.0, cell_ = 1.0;
    std::size_t nx_ = 1, ny_ = 1;
    std::vector<std::vector<std::uint32_t>> cells_;
    std::vector<std::uint32_t> stamp_;
    std::uint32_t mark_ = 0;
};

// A box's solid read once: its axes, half sizes, bottom and top at its -u and +u ends (massingCorners, no nudge; only
// sloped boxes differ between the two) and their extremes.
struct Solid {
    bool valid = false;
    double cx = 0.0, cy = 0.0, c = 1.0, s = 0.0, hx = 0.0, hy = 0.0;
    double b0 = 0.0, b1 = 0.0, t0 = 0.0, t1 = 0.0;
    double bottom = 0.0, lowTop = 0.0, highTop = 0.0;
};

Solid makeSolid(const MassingBox& b) {
    Solid out;
    if (!(b.sx > 0.0) || !(b.sy > 0.0)) return out;
    std::array<MassingVec, 8> k;
    massingCorners(b, 0.0, k);
    const double a = b.rot * kPi / 180.0;
    out.valid = true;
    out.cx = b.cx;
    out.cy = b.cy;
    out.c = std::cos(a);
    out.s = std::sin(a);
    out.hx = 0.5 * b.sx;
    out.hy = 0.5 * b.sy;
    out.b0 = k[0].z;
    out.b1 = k[1].z;
    out.t0 = k[4].z;
    out.t1 = k[5].z;
    out.bottom = std::min({k[0].z, k[1].z, k[2].z, k[3].z});
    out.lowTop = std::min({k[4].z, k[5].z, k[6].z, k[7].z});
    out.highTop = std::max({k[4].z, k[5].z, k[6].z, k[7].z});
    return out;
}

// The solid over a plan point, as massingFloorAt reads it: false when the footprint doesn't hold the point.
bool solidHolds(const Solid& s, double x, double y, double& bottom, double& top) {
    if (!s.valid) return false;
    const double dx = x - s.cx, dy = y - s.cy;
    const double u = dx * s.c + dy * s.s, w = -dx * s.s + dy * s.c;
    if (std::fabs(u) > s.hx || std::fabs(w) > s.hy) return false;
    const double t = u / (2.0 * s.hx) + 0.5;
    bottom = s.b0 + (s.b1 - s.b0) * t;
    top = s.t0 + (s.t1 - s.t0) * t;
    return true;
}

// The distance from (x, y) to the segment a-b, and the segment's nearest point.
double segmentNearest(double x, double y, const MassingVec& a, const MassingVec& b, double& px, double& py) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = 0.0;
    if (len2 > kTiny) t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / len2, 0.0, 1.0);
    px = a.x + t * dx;
    py = a.y + t * dy;
    return std::hypot(x - px, y - py);
}

// One segment of a street's polyline (a street of one point: that point twice).
struct Seg {
    std::size_t street = 0;
    std::size_t index = 0; // the segment's number along its street
    MassingVec a, b;
};

// The boxes `include` marks and the streets, gridded, with the ground: what the three passes read.
class Scene {
public:
    Scene(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
          const std::vector<MassingStreet>* streets)
        : ground_(ground) {
        const std::size_t n = std::min(boxes.size(), include.size());
        solids.resize(boxes.size());
        feet.resize(boxes.size());
        std::vector<Aabb> boxBounds(boxes.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (!include[i]) continue;
            solids[i] = makeSolid(boxes[i]);
            if (!solids[i].valid) continue;
            feet[i] = massingBoxFoot(boxes[i]);
            boxBounds[i] = footAabb(feet[i]);
            grow(extent, boxBounds[i]);
        }
        std::vector<Aabb> segBounds;
        if (streets) {
            for (std::size_t j = 0; j < streets->size(); ++j) {
                const MassingStreet& s = (*streets)[j];
                const double half = 0.5 * s.width;
                const double halfC = 0.5 * massingCarriageway(s);
                maxHalfWidth = std::max(maxHalfWidth, half);
                maxHalfCarriageway = std::max(maxHalfCarriageway, halfC);
                const std::size_t pts = s.points.size();
                for (std::size_t k = 0; k < pts; ++k) {
                    if (pts > 1 && k + 1 == pts) break;
                    Seg seg;
                    seg.street = j;
                    seg.index = k;
                    seg.a = s.points[k];
                    seg.b = pts > 1 ? s.points[k + 1] : s.points[k];
                    segs.push_back(seg);
                    const Aabb b{std::min(seg.a.x, seg.b.x), std::min(seg.a.y, seg.b.y), std::max(seg.a.x, seg.b.x),
                                 std::max(seg.a.y, seg.b.y)};
                    segBounds.push_back(b);
                    grow(extent, b);
                }
            }
        }
        if (!(extent.x0 <= extent.x1)) extent = {0.0, 0.0, 1.0, 1.0};
        extent = {extent.x0 - 64.0, extent.y0 - 64.0, extent.x1 + 64.0, extent.y1 + 64.0};
        boxGrid.reset(extent, kCell);
        for (std::size_t i = 0; i < boxes.size(); ++i)
            if (solids[i].valid) boxGrid.insert(static_cast<std::uint32_t>(i), boxBounds[i]);
        segGrid.reset(extent, kCell);
        for (std::size_t q = 0; q < segs.size(); ++q) segGrid.insert(static_cast<std::uint32_t>(q), segBounds[q]);
    }

    double groundAt(double x, double y) const {
        return massingGroundAt(ground_.grid, ground_.samplesX, ground_.samplesY, ground_.spacing, ground_.originX,
                               ground_.originY, x, y);
    }
    // massingFloorAt over the boxes `include` marks.
    double floorAt(double x, double y, double ground, double ceiling) {
        double floor = ground;
        std::size_t on = kNone;
        boxGrid.any(pointAabb(x, y, 0.0), [&](std::uint32_t i) {
            double bottom = 0.0, top = 0.0;
            if (!solidHolds(solids[i], x, y, bottom, top)) return false;
            if (top <= ceiling && (top > floor || (top == floor && on != kNone && i < on))) {
                floor = top;
                on = i;
            }
            return false;
        });
        return floor;
    }
    // The floor near a place whose own floor is `base`: the ground, or a top no higher than a step over the higher of
    // the ground and base.
    double floorNear(double x, double y, double base) {
        const double g = groundAt(x, y);
        return floorAt(x, y, g, std::max(g, base) + kMassingPropMaxStep);
    }
    // The first box (other than `skip`) holding (x, y) whose solid stands in the band lo to hi: its highest top corner
    // over lo and its bottom under hi.
    bool standsAt(double x, double y, double lo, double hi, std::size_t skip) {
        return boxGrid.any(pointAabb(x, y, 0.0), [&](std::uint32_t i) {
            double bottom = 0.0, top = 0.0;
            if (i == skip || !solidHolds(solids[i], x, y, bottom, top)) return false;
            return solids[i].highTop > lo && solids[i].bottom < hi;
        });
    }

    std::vector<Solid> solids;
    std::vector<MassingFoot> feet;
    std::vector<Seg> segs;
    Grid boxGrid, segGrid;
    Aabb extent;
    double maxHalfWidth = 0.0, maxHalfCarriageway = 0.0;

private:
    const MassingGround& ground_;
};

// Feet gridded, to find the ones near a new foot.
class Feet {
public:
    void reset(const Aabb& extent) {
        feet_.clear();
        grid_.reset(extent, kCell);
    }
    void add(const MassingFoot& foot) {
        if (!(foot.hx > 0.0) || !(foot.hy > 0.0)) return;
        grid_.insert(static_cast<std::uint32_t>(feet_.size()), footAabb(foot));
        feet_.push_back(foot);
    }
    // Whether a foot with area comes within kMassingPropGap of one held.
    bool crowds(const MassingFoot& foot) {
        if (!(foot.hx > 0.0) || !(foot.hy > 0.0)) return false;
        Aabb a = footAabb(foot);
        a = {a.x0 - kMassingPropGap, a.y0 - kMassingPropGap, a.x1 + kMassingPropGap, a.y1 + kMassingPropGap};
        return grid_.any(a, [&](std::uint32_t k) { return massingFeetOverlap(foot, feet_[k], kMassingPropGap); });
    }

private:
    std::vector<MassingFoot> feet_;
    Grid grid_;
};

// FNV-1a, 64 bits, then a mixer, so places a step apart pick apart.
constexpr std::uint64_t kFnvBasis = 14695981039346656037ull;
std::uint64_t hashText(std::uint64_t h, std::string_view s) {
    for (const char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}
std::uint64_t hashNumber(std::uint64_t h, std::uint64_t v) {
    for (int k = 0; k < 8; ++k) {
        h ^= (v >> (8 * k)) & 0xffu;
        h *= 1099511628211ull;
    }
    return h;
}
double hashUnit(std::uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

// A box's four side faces: the outward normal, the way along it (the normal a quarter turn counter-clockwise), its
// length and the box's depth across it, and its start (the end the way along starts from).
struct Face {
    double nx = 0.0, ny = 0.0, tx = 0.0, ty = 0.0;
    double length = 0.0, depth = 0.0;
    double x0 = 0.0, y0 = 0.0;
};

Face boxFace(const MassingBox& b, int face) {
    const double a = b.rot * kPi / 180.0;
    const double ux = std::cos(a), uy = std::sin(a);
    const double wx = -uy, wy = ux;
    Face f;
    switch (face) {
    case 0: f.nx = ux; f.ny = uy; f.length = b.sy; f.depth = b.sx; break;
    case 1: f.nx = wx; f.ny = wy; f.length = b.sx; f.depth = b.sy; break;
    case 2: f.nx = -ux; f.ny = -uy; f.length = b.sy; f.depth = b.sx; break;
    default: f.nx = -wx; f.ny = -wy; f.length = b.sx; f.depth = b.sy; break;
    }
    f.tx = -f.ny;
    f.ty = f.nx;
    f.x0 = b.cx + f.nx * 0.5 * f.depth - f.tx * 0.5 * f.length;
    f.y0 = b.cy + f.ny * 0.5 * f.depth - f.ty * 0.5 * f.length;
    return f;
}

double degrees(double y, double x) { return std::atan2(y, x) * 180.0 / kPi; }

// An optional number: empty leaves `out` at its default.
bool optionalNumber(std::string_view s, double& out) {
    if (trim(s).empty()) return true;
    return parseNumber(s, out);
}

// An optional whole number in [lo, 1e9]: empty leaves `out` at its default.
bool optionalWhole(std::string_view s, double lo, std::size_t& out) {
    if (trim(s).empty()) return true;
    double v = 0.0;
    if (!parseNumber(s, v) || v < lo || v != std::floor(v) || v > 1e9) return false;
    out = static_cast<std::size_t>(v);
    return true;
}

} // namespace

MassingFoot massingBoxFoot(const MassingBox& box) {
    MassingFoot f;
    f.x = box.cx;
    f.y = box.cy;
    f.hx = 0.5 * box.sx;
    f.hy = 0.5 * box.sy;
    f.rot = box.rot;
    return f;
}

// ---- Kerbs ----

namespace {

// Whether the straight line from zs at the start of point q0's share to ze at the end of point (q1 - 1)'s (the points
// step apart, each in the middle of its share) is no steeper than kMassingKerbGrade and passes within kMassingKerbFit of
// each point's floor. No allocation.
bool kerbChordFits(const double* floors, std::size_t q0, std::size_t q1, double step, double zs, double ze) {
    const double grade = (ze - zs) / (static_cast<double>(q1 - q0) * step);
    if (!(std::fabs(grade) <= kMassingKerbGrade)) return false;
    for (std::size_t i = q0; i < q1; ++i) {
        const double t = (static_cast<double>(i - q0) + 0.5) * step;
        if (!(std::fabs(floors[i] - (zs + grade * t)) <= kMassingKerbFit)) return false;
    }
    return true;
}

} // namespace


void massingKerbs(const std::vector<MassingStreet>& streets, const MassingGround& ground,
                  const std::vector<MassingBox>& boxes, const std::vector<bool>& include, double dryAbove,
                  std::vector<MassingBox>& out, MassingKerbCounts& counts) {
    out.clear();
    counts = MassingKerbCounts{};
    Scene scene(ground, boxes, include, &streets);
    const double w = kMassingKerbWidth;

    // Within half a carriageway plus a kerb of another street, or square onto another segment of its own.
    const auto crossing = [&](std::size_t self, std::size_t index, double x, double y) {
        return scene.segGrid.any(pointAabb(x, y, scene.maxHalfCarriageway + w), [&](std::uint32_t q) {
            const Seg& g = scene.segs[q];
            const double lim = 0.5 * massingCarriageway(streets[g.street]) + w;
            double px = 0.0, py = 0.0;
            if (g.street != self) return segmentNearest(x, y, g.a, g.b, px, py) < lim;
            if (g.index == index) return false;
            const double dx = g.b.x - g.a.x, dy = g.b.y - g.a.y;
            const double len2 = dx * dx + dy * dy;
            if (len2 <= kTiny) return false;
            const double t = ((x - g.a.x) * dx + (y - g.a.y) * dy) / len2;
            if (t <= 0.0 || t >= 1.0) return false;
            return std::hypot(x - (g.a.x + t * dx), y - (g.a.y + t * dy)) < lim;
        });
    };
    // A box within half a kerb's width standing over the floor.
    const auto boxed = [&](double x, double y, double floor) {
        return scene.boxGrid.any(pointAabb(x, y, 0.5 * w), [&](std::uint32_t i) {
            const Solid& s = scene.solids[i];
            if (!(s.highTop > floor + kMassingStandClear) || !(s.bottom < floor + kMassingPropBody)) return false;
            return massingFootprintGap(boxes[i], x, y) <= 0.5 * w;
        });
    };

    std::vector<double> floors;
    std::vector<std::uint8_t> keep;
    for (std::size_t j = 0; j < streets.size(); ++j) {
        const MassingStreet& s = streets[j];
        const std::size_t n = s.points.size();
        if (!(s.pavement > 0.0) || n < 2) continue;
        bool hasLength = false;
        for (std::size_t k = 0; k + 1 < n; ++k)
            if (std::hypot(s.points[k + 1].x - s.points[k].x, s.points[k + 1].y - s.points[k].y) > kTiny) hasLength = true;
        if (!hasLength) continue;
        ++counts.streets;
        const double off = 0.5 * massingCarriageway(s) + 0.5 * w;
        std::size_t numbers[2] = {0, 0}; // left, right
        for (std::size_t k = 0; k + 1 < n; ++k) {
            const MassingVec& a = s.points[k];
            const MassingVec& b = s.points[k + 1];
            const double len = std::hypot(b.x - a.x, b.y - a.y);
            if (len <= kTiny) continue;
            const double dx = (b.x - a.x) / len, dy = (b.y - a.y) / len;
            const double lx = -dy, ly = dx; // left
            const std::size_t m = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(len / kMassingKerbStep - 1e-9)));
            const double step = len / static_cast<double>(m);
            for (int side = 0; side < 2; ++side) {
                const double sign = side == 0 ? 1.0 : -1.0;
                floors.assign(m, 0.0);
                keep.assign(m, 0);
                for (std::size_t i = 0; i < m; ++i) {
                    const double t = (static_cast<double>(i) + 0.5) * step;
                    const double cx = a.x + dx * t, cy = a.y + dy * t;
                    const double px = cx + lx * sign * off, py = cy + ly * sign * off;
                    const double gc = scene.groundAt(cx, cy);
                    const double centre = scene.floorAt(cx, cy, gc, gc + kMassingPropDeckReach);
                    const double f = scene.floorNear(px, py, centre);
                    floors[i] = f;
                    if (crossing(j, k, px, py)) counts.crossing += step;
                    else if (boxed(px, py, f)) counts.boxed += step;
                    else if (f <= dryAbove) counts.wet += step;
                    else keep[i] = 1;
                }
                // Rows of kept points, split evenly under the longest piece, then into pieces whose lines fit.
                std::size_t i = 0;
                while (i < m) {
                    if (!keep[i]) {
                        ++i;
                        continue;
                    }
                    const std::size_t r0 = i;
                    while (i < m && keep[i]) ++i;
                    const std::size_t r1 = i;
                    const std::size_t count = r1 - r0;
                    const std::size_t parts = std::max<std::size_t>(
                        1, static_cast<std::size_t>(std::ceil(static_cast<double>(count) * step / kMassingKerbLongest - 1e-9)));
                    // The line's height at the boundary b between points b - 1 and b: their floors' mean. At the
                    // row's ends, and where a run starts, the end point's floor carried on half a step along the grade
                    // to its neighbour, when that is no steeper than kMassingKerbGrade (else the floor as it is).
                    const auto outer = [&](std::size_t end, std::size_t next) {
                        const double d = floors[end] - floors[next];
                        return std::fabs(d) <= kMassingKerbGrade * step ? floors[end] + 0.5 * d : floors[end];
                    };
                    const auto edge = [&](std::size_t b) { // b in (r0, r1]
                        if (b < r1) return 0.5 * (floors[b - 1] + floors[b]);
                        return count < 2 ? floors[r1 - 1] : outer(r1 - 1, r1 - 2);
                    };
                    bool joined = false; // the next piece starts at its boundary's height, where the last one ended
                    for (std::size_t p = 0; p < parts; ++p) {
                        const std::size_t p0 = r0 + count * p / parts, p1 = r0 + count * (p + 1) / parts;
                        std::size_t q = p0;
                        while (q < p1) {
                            const std::size_t q0 = q;
                            const bool follows = joined;
                            joined = false;
                            const double zs = follows ? edge(q0) : (q0 + 1 < r1 ? outer(q0, q0 + 1) : floors[q0]);
                            std::size_t e = q0;
                            for (std::size_t t = q0 + 1; t <= p1 && kerbChordFits(floors.data(), q0, t, step, zs, edge(t)); ++t) e = t;
                            if (e == q0 && follows) continue; // cannot follow on: start a run here
                            if (e == q0) {
                                counts.shortPieces += step; // not even its own point fits: a step under it
                                q = q0 + 1;
                                continue;
                            }
                            q = e;
                            const double pieceLen = static_cast<double>(e - q0) * step;
                            if (pieceLen < kMassingKerbShortest - 1e-9) {
                                counts.shortPieces += pieceLen;
                                continue;
                            }
                            if (!follows) ++counts.runs;
                            const double ze = edge(e);
                            const double mid = 0.5 * static_cast<double>(q0 + e) * step;
                            MassingBox box;
                            box.id = s.id + (side == 0 ? "-KL" : "-KR") + std::to_string(++numbers[side]);
                            box.row = s.id;
                            box.kind = "kerb";
                            box.source = MassingSource::Invented;
                            box.cover = false;
                            box.cx = a.x + dx * mid + lx * sign * off;
                            box.cy = a.y + dy * mid + ly * sign * off;
                            box.sx = pieceLen;
                            box.sy = w;
                            box.rot = degrees(dy, dx);
                            box.riseU = (ze - zs) / pieceLen;
                            box.z0 = 0.5 * (zs + ze) - kMassingKerbSink;
                            box.z1 = 0.5 * (zs + ze) + kMassingKerbHeight;
                            out.push_back(std::move(box));
                            ++counts.pieces;
                            counts.built += pieceLen;
                            joined = true;
                        }
                    }
                }
            }
        }
    }
}

// ---- Quay furniture ----

bool massingQuayDeck(const MassingBox& box) {
    if (box.sloped || !massingVisible(massingRole(box))) return false;
    return box.kind == "quay" || box.kind == "pier" || box.kind == "seawall" || box.kind == "deck";
}

void massingQuay(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                 double dryAbove, const MassingPropRule& lamp, std::size_t lampRule,
                 const std::vector<MassingFoot>* taken, std::vector<MassingBox>& outBoxes,
                 std::vector<MassingProp>& outLamps, MassingQuayCounts& counts) {
    outBoxes.clear();
    outLamps.clear();
    counts = MassingQuayCounts{};
    Scene scene(ground, boxes, include, nullptr);
    Aabb extent = scene.extent;
    if (taken)
        for (const MassingFoot& f : *taken) grow(extent, footAabb(f));
    Feet feet;
    feet.reset(extent);
    if (taken)
        for (const MassingFoot& f : *taken) feet.add(f);

    const auto addBox = [&](const MassingBox& deck, const char* tag, std::size_t number, const char* kind, double cx,
                            double cy, double sx, double sy, double rot, double z0, double z1) {
        MassingBox box;
        box.id = deck.id + tag + std::to_string(number);
        box.row = deck.id;
        box.kind = kind;
        box.source = MassingSource::Invented;
        box.cover = false;
        box.cx = cx;
        box.cy = cy;
        box.sx = sx;
        box.sy = sy;
        box.rot = rot;
        box.z0 = z0;
        box.z1 = z1;
        outBoxes.push_back(std::move(box));
    };

    std::vector<std::uint8_t> ok;
    std::vector<MassingVec> posts;
    const std::size_t n = std::min(boxes.size(), include.size());
    for (std::size_t d = 0; d < n; ++d) {
        if (!include[d] || !massingQuayDeck(boxes[d]) || !scene.solids[d].valid) continue;
        const MassingBox& deck = boxes[d];
        const double top = scene.solids[d].highTop;
        std::size_t rails = 0, postCount = 0, bollards = 0;
        posts.clear();
        bool hasRun = false;
        for (int fi = 0; fi < 4; ++fi) {
            const Face f = boxFace(deck, fi);
            const double rot = degrees(f.ty, f.tx);
            const std::size_t m =
                std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(f.length / kMassingQuayStep - 1e-9)));
            const double step = f.length / static_cast<double>(m);
            ok.assign(m, 0);
            for (std::size_t i = 0; i < m; ++i) {
                const double s = (static_cast<double>(i) + 0.5) * step;
                const double ex = f.x0 + f.tx * s, ey = f.y0 + f.ty * s;
                const double px = ex + f.nx * kMassingQuayProbe, py = ey + f.ny * kMassingQuayProbe;
                if (scene.groundAt(px, py) > dryAbove) continue;
                if (scene.standsAt(px, py, dryAbove, kInf, d)) continue;
                counts.seaward += step;
                const double ix = ex - f.nx * kMassingRailIn, iy = ey - f.ny * kMassingRailIn;
                if (scene.standsAt(ix, iy, top + kMassingStandClear, top + kMassingPropBody, d)) {
                    counts.blocked += step;
                    continue;
                }
                ok[i] = 1;
            }
            std::size_t i = 0;
            while (i < m) {
                if (!ok[i]) {
                    ++i;
                    continue;
                }
                const std::size_t r0 = i;
                while (i < m && ok[i]) ++i;
                const double s0 = std::max(static_cast<double>(r0) * step, kMassingQuayEnd);
                const double s1 = std::min(static_cast<double>(i) * step, f.length - kMassingQuayEnd);
                const double len = s1 - s0;
                if (len < kMassingQuayShortest - 1e-9) continue;
                hasRun = true;
                ++counts.runs;
                counts.railed += len;
                const auto at = [&](double s, double in, double& x, double& y) {
                    x = f.x0 + f.tx * s - f.nx * in;
                    y = f.y0 + f.ty * s - f.ny * in;
                };
                double x = 0.0, y = 0.0;
                at(0.5 * (s0 + s1), kMassingRailIn, x, y);
                addBox(deck, "-QR", ++rails, "barrier", x, y, len, kMassingRailBar, rot,
                       top + kMassingRailTop - kMassingRailBar, top + kMassingRailTop);
                const std::size_t gaps =
                    std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(len / kMassingRailPostGap - 1e-9)));
                for (std::size_t k = 0; k <= gaps; ++k) {
                    at(s0 + len * static_cast<double>(k) / static_cast<double>(gaps), kMassingRailIn, x, y);
                    bool twice = false;
                    for (const MassingVec& p : posts)
                        if (std::hypot(p.x - x, p.y - y) < 0.05) twice = true;
                    if (twice) continue;
                    posts.push_back({x, y, 0.0});
                    addBox(deck, "-QP", ++postCount, "post", x, y, kMassingRailPost, kMassingRailPost, rot, top - 0.05,
                           top + kMassingRailTop - 0.5 * kMassingRailBar);
                    ++counts.posts;
                }
                const std::size_t nb = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(len / kMassingBollardGap)));
                for (std::size_t k = 0; k < nb; ++k) {
                    at(s0 + len * (static_cast<double>(k) + 0.5) / static_cast<double>(nb), kMassingBollardIn, x, y);
                    addBox(deck, "-QB", ++bollards, "post", x, y, kMassingBollardSide, kMassingBollardSide, rot,
                           top - 0.05, top + kMassingBollardHeight);
                    MassingFoot foot;
                    foot.x = x;
                    foot.y = y;
                    foot.hx = foot.hy = 0.5 * kMassingBollardSide;
                    foot.rot = rot;
                    feet.add(foot);
                    ++counts.bollards;
                }
                if (len < 0.5 * kMassingQuayLampGap - 1e-9) continue;
                const std::size_t nl = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(len / kMassingQuayLampGap)));
                for (std::size_t k = 0; k < nl; ++k) {
                    at(s0 + len * (static_cast<double>(k) + 0.5) / static_cast<double>(nl), kMassingQuayLampIn, x, y);
                    MassingProp prop;
                    prop.rule = lampRule;
                    prop.street = d;
                    prop.x = x;
                    prop.y = y;
                    prop.z = top + lamp.dz;
                    prop.face = degrees(-f.ny, -f.nx);
                    prop.wall = false;
                    if (scene.standsAt(x, y, top + kMassingStandClear, top + kMassingPropBody, d)) {
                        ++counts.lampsBlocked;
                        continue;
                    }
                    const MassingFoot foot = massingPropFoot(prop, lamp);
                    if (feet.crowds(foot)) {
                        ++counts.lampsCrowded;
                        continue;
                    }
                    feet.add(foot);
                    outLamps.push_back(prop);
                    ++counts.lamps;
                }
            }
        }
        if (hasRun) ++counts.decks;
    }
}

// ---- Frontage ----

bool importMassingFront(std::string_view csv, std::vector<MassingFrontRule>& out, std::string* error) {
    out.clear();
    std::vector<Record> records;
    if (!splitCsv(csv, records, error)) return false;

    enum Col { Id, Asset, Width, Depth, Height, Out, Gap, Turn, Dz, Weight, Run, Cap, Scale, Kinds, Count };
    constexpr const char* kNames[Count] = {"id",    "asset", "width_m", "depth_m", "height_m", "out_m", "gap_m",
                                           "turn",  "dz",    "weight",  "run",     "cap",      "scale",
                                           "kinds"};
    constexpr std::size_t kRequired = 5; // id, asset, width_m, depth_m, height_m
    std::size_t col[Count] = {};
    std::size_t width = 0;
    bool haveHeader = false;
    std::unordered_set<std::string> ids;

    for (const Record& r : records) {
        if (isComment(r)) continue;
        if (!haveHeader) {
            for (std::size_t c = 0; c < Count; ++c) {
                col[c] = r.fields.size();
                for (std::size_t f = 0; f < r.fields.size(); ++f) {
                    if (trim(r.fields[f]) != kNames[c]) continue;
                    if (col[c] != r.fields.size()) return fail(error, r.line, std::string("column ") + kNames[c] + " twice");
                    col[c] = f;
                }
                if (c < kRequired && col[c] == r.fields.size())
                    return fail(error, r.line, std::string("no column ") + kNames[c]);
            }
            width = r.fields.size();
            haveHeader = true;
            continue;
        }
        if (r.fields.size() != width)
            return fail(error, r.line,
                        std::to_string(r.fields.size()) + " fields where the header has " + std::to_string(width));
        const auto field = [&](Col c) -> std::string_view {
            return col[c] < r.fields.size() ? std::string_view(r.fields[col[c]]) : std::string_view();
        };
        MassingFrontRule rule;
        rule.id = std::string(trim(field(Id)));
        if (!validId(rule.id)) return fail(error, r.line, "id must be A-Z, 0-9, '_' or '-'");
        if (!ids.insert(rule.id).second) return fail(error, r.line, "id " + rule.id + " twice");
        rule.asset = std::string(trim(field(Asset)));
        if (rule.asset.empty()) return fail(error, r.line, rule.id + ": asset is empty");
        if (!parseNumber(field(Width), rule.width) || !(rule.width > 0.0))
            return fail(error, r.line, rule.id + ": width_m must be a number > 0");
        if (!parseNumber(field(Depth), rule.depth) || !(rule.depth > 0.0))
            return fail(error, r.line, rule.id + ": depth_m must be a number > 0");
        if (!parseNumber(field(Height), rule.height) || !(rule.height > 0.0))
            return fail(error, r.line, rule.id + ": height_m must be a number > 0");
        rule.out = rule.depth;
        if (!optionalNumber(field(Out), rule.out) || !(rule.out > 0.0))
            return fail(error, r.line, rule.id + ": out_m must be a number > 0");
        if (!optionalNumber(field(Gap), rule.gap) || rule.gap < 0.0)
            return fail(error, r.line, rule.id + ": gap_m must be a number >= 0");
        if (!optionalNumber(field(Turn), rule.turn)) return fail(error, r.line, rule.id + ": turn must be a number");
        if (!optionalNumber(field(Dz), rule.dz)) return fail(error, r.line, rule.id + ": dz must be a number");
        if (!optionalNumber(field(Weight), rule.weight) || !(rule.weight > 0.0))
            return fail(error, r.line, rule.id + ": weight must be a number > 0");
        if (!optionalWhole(field(Run), 1.0, rule.run))
            return fail(error, r.line, rule.id + ": run must be a whole number >= 1");
        if (!optionalWhole(field(Cap), 0.0, rule.cap))
            return fail(error, r.line, rule.id + ": cap must be a whole number >= 0");
        if (!optionalNumber(field(Scale), rule.scale) || !(rule.scale > 0.0))
            return fail(error, r.line, rule.id + ": scale must be a number > 0");
        std::string_view kinds = trim(field(Kinds));
        while (!kinds.empty()) {
            const std::size_t semi = kinds.find(';');
            const std::string_view one = trim(kinds.substr(0, semi));
            if (!validKind(one))
                return fail(error, r.line, rule.id + ": each of kinds must be a-z, A-Z, 0-9, '_' or '-'");
            rule.kinds.emplace_back(one);
            if (semi == std::string_view::npos) break;
            kinds = trim(kinds.substr(semi + 1));
            if (kinds.empty()) return fail(error, r.line, rule.id + ": kinds ends in ';'");
        }
        out.push_back(std::move(rule));
    }
    if (!haveHeader) return fail(error, 1, "no header row");
    return true;
}

MassingFoot massingFrontFoot(const MassingProp& prop, const MassingFrontRule& rule) {
    const double a = prop.face * kPi / 180.0;
    const double out = std::max(rule.out, 0.0);
    MassingFoot foot;
    foot.x = prop.x + std::cos(a) * 0.5 * out;
    foot.y = prop.y + std::sin(a) * 0.5 * out;
    foot.hx = 0.5 * out;
    foot.hy = 0.5 * std::max(rule.width, 0.0);
    foot.rot = prop.face;
    return foot;
}

void massingFrontage(const std::vector<MassingFrontRule>& rules, const std::vector<MassingStreet>& streets,
                     const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                     const std::vector<bool>& hosts, double dryAbove, const std::vector<MassingFoot>* taken,
                     std::vector<MassingProp>& out, std::vector<MassingFrontCounts>& counts,
                     MassingFrontTotals& totals, std::vector<MassingBox>* outPads,
                     std::vector<std::uint8_t>* outFronting) {
    out.clear();
    counts.assign(rules.size(), MassingFrontCounts{});
    totals = MassingFrontTotals{};
    if (outPads) outPads->clear();
    if (outFronting) outFronting->clear();
    if (rules.empty()) return;
    Scene scene(ground, boxes, include, &streets);
    Aabb extent = scene.extent;
    if (taken)
        for (const MassingFoot& f : *taken) grow(extent, footAabb(f));
    for (std::size_t h = 0; h < boxes.size() && h < hosts.size(); ++h)
        if (hosts[h]) grow(extent, footAabb(massingBoxFoot(boxes[h])));

    // The two layers' rules, narrowest first.
    std::vector<std::size_t> layers[2];
    for (std::size_t r = 0; r < rules.size(); ++r) layers[rules[r].dz >= kMassingPropBody ? 1 : 0].push_back(r);
    for (auto& layer : layers)
        std::stable_sort(layer.begin(), layer.end(),
                         [&](std::size_t a, std::size_t b) { return rules[a].width < rules[b].width; });

    // The kind of the street a face fronts at (x, y), 0.5 m out of it along (nx, ny).
    const std::string_view open = "open";
    const auto kindAt = [&](double x, double y, double nx, double ny) -> std::string_view {
        double best = kInf;
        std::size_t bestStreet = kNone;
        scene.segGrid.any(pointAabb(x, y, scene.maxHalfWidth + kMassingFrontReach), [&](std::uint32_t q) {
            const Seg& g = scene.segs[q];
            double px = 0.0, py = 0.0;
            const double edge = segmentNearest(x, y, g.a, g.b, px, py) - 0.5 * streets[g.street].width;
            if (edge > kMassingFrontReach) return false;
            if ((px - x) * nx + (py - y) * ny <= 0.0) return false;
            if (edge < best || (edge == best && g.street < bestStreet)) {
                best = edge;
                bestStreet = g.street;
            }
            return false;
        });
        return bestStreet == kNone ? open : std::string_view(streets[bestStreet].kind);
    };
    const auto inRoad = [&](double x, double y) {
        return scene.segGrid.any(pointAabb(x, y, scene.maxHalfCarriageway), [&](std::uint32_t q) {
            const Seg& g = scene.segs[q];
            double px = 0.0, py = 0.0;
            return segmentNearest(x, y, g.a, g.b, px, py) < 0.5 * massingCarriageway(streets[g.street]);
        });
    };
    const auto holds = [](const MassingFrontRule& rule, std::string_view kind) {
        return rule.kinds.empty() || std::find(rule.kinds.begin(), rule.kinds.end(), kind) != rule.kinds.end();
    };

    enum class Fail { Pass, NoFit, NoWall, Shallow, Blocked, InRoad, Steep, Wet, Crowded };
    Feet feet[2];
    std::vector<std::size_t> faceOf; // per piece placed: host * 4 + face
    std::vector<std::uint8_t> fronting;
    std::vector<std::size_t> order;

    const auto walk = [&](const std::vector<double>& accept) {
        out.clear();
        faceOf.clear();
        counts.assign(rules.size(), MassingFrontCounts{});
        totals = MassingFrontTotals{};
        fronting.assign(boxes.size() * 4, 0);
        for (int l = 0; l < 2; ++l) feet[l].reset(extent);
        if (taken)
            for (const MassingFoot& f : *taken) feet[0].add(f);
        for (std::size_t h = 0; h < boxes.size() && h < hosts.size(); ++h) {
            if (!hosts[h] || boxes[h].sloped) continue;
            const Solid host = makeSolid(boxes[h]);
            if (!host.valid) continue;
            ++totals.hosts;
            const std::uint64_t hostHash = hashText(kFnvBasis, boxes[h].id);
            for (int fi = 0; fi < 4; ++fi) {
                ++totals.faces;
                const Face f = boxFace(boxes[h], fi);
                const double faceDeg = degrees(f.ny, f.nx);
                const auto point = [&](double s, double outward, double& x, double& y) {
                    x = f.x0 + f.tx * s + f.nx * outward;
                    y = f.y0 + f.ty * s + f.ny * outward;
                };
                double mx = 0.0, my = 0.0;
                point(0.5 * f.length, 0.5, mx, my);
                if (kindAt(mx, my, f.nx, f.ny) != open) {
                    ++totals.streetFaces;
                    fronting[h * 4 + static_cast<std::size_t>(fi)] = 1;
                }
                for (int l = 0; l < 2; ++l) {
                    const std::vector<std::size_t>& layer = layers[l];
                    if (layer.empty()) continue;
                    const double narrowest = rules[layer.front()].width;
                    const double end = f.length - kMassingFrontMargin;
                    double s = kMassingFrontMargin;
                    std::uint64_t place = 0;
                    std::size_t runRule = kNone, runLeft = 0;
                    MassingProp prop;
                    // Whether rule r takes the place at s, failing on the first check it fails.
                    const auto tryRule = [&](std::size_t r) -> Fail {
                        const MassingFrontRule& rule = rules[r];
                        if (s + rule.width > end + 1e-9) return Fail::NoFit;
                        const double mid = s + 0.5 * rule.width;
                        double fx = 0.0, fy = 0.0, ox = 0.0, oy = 0.0;
                        point(mid, 0.0, fx, fy);
                        point(mid, 0.5, ox, oy);
                        const double floor = scene.floorNear(ox, oy, host.bottom);
                        const double lo = floor + rule.dz, hi = lo + rule.height;
                        if (host.lowTop < hi || host.bottom > lo + kMassingFrontLift) return Fail::NoWall;
                        if (rule.depth - rule.out > f.depth - kMassingFrontBack) return Fail::Shallow;
                        const double reach = std::max(rule.out, kMassingFrontStrip);
                        MassingFoot strip;
                        point(mid, 0.5 * (0.05 + reach), strip.x, strip.y);
                        strip.hx = 0.5 * (reach - 0.05);
                        strip.hy = 0.5 * rule.width;
                        strip.rot = faceDeg;
                        const bool blocked = scene.boxGrid.any(footAabb(strip), [&](std::uint32_t i) {
                            if (i == h) return false;
                            const Solid& o = scene.solids[i];
                            if (!(o.highTop > lo + kMassingStandClear) || !(o.bottom < hi)) return false;
                            return massingFeetOverlap(strip, scene.feet[i], 0.0);
                        });
                        if (blocked) return Fail::Blocked;
                        if (l == 0) {
                            double x = 0.0, y = 0.0;
                            point(s, rule.out, x, y);
                            bool road = inRoad(x, y);
                            point(s + rule.width, rule.out, x, y);
                            road = road || inRoad(x, y);
                            point(mid, rule.out, x, y);
                            road = road || inRoad(x, y);
                            if (road) return Fail::InRoad;
                            double lowF = floor, highF = floor;
                            for (const double at : {s, s + rule.width}) {
                                point(at, 0.5, x, y);
                                const double g = scene.floorNear(x, y, host.bottom);
                                lowF = std::min(lowF, g);
                                highF = std::max(highF, g);
                            }
                            if (highF - lowF > kMassingFrontSpread) return Fail::Steep;
                        }
                        if (floor <= dryAbove) return Fail::Wet;
                        prop.rule = r;
                        prop.street = h;
                        prop.x = fx;
                        prop.y = fy;
                        prop.z = lo;
                        prop.face = faceDeg;
                        prop.wall = true;
                        if (feet[l].crowds(massingFrontFoot(prop, rule))) return Fail::Crowded;
                        return Fail::Pass;
                    };
                    while (s + narrowest <= end + 1e-9) {
                        const std::uint64_t placeHash =
                            hashNumber(hashNumber(hashNumber(hostHash, static_cast<std::uint64_t>(fi)),
                                                  static_cast<std::uint64_t>(l)),
                                       place);
                        ++place;
                        double kx = 0.0, ky = 0.0;
                        point(std::min(s + 1.0, end), 0.5, kx, ky);
                        const std::string_view kind = kindAt(kx, ky, f.nx, f.ny);
                        const auto eligible = [&](std::size_t r) {
                            if (!holds(rules[r], kind)) return false;
                            return accept[r] >= 1.0 || hashUnit(hashText(placeHash, rules[r].id)) < accept[r];
                        };
                        order.clear();
                        const bool running = runLeft > 0 && runRule != kNone && eligible(runRule);
                        if (running) order.push_back(runRule);
                        double total = 0.0;
                        for (const std::size_t r : layer)
                            if (eligible(r)) total += rules[r].weight;
                        if (total > 0.0) {
                            double x = hashUnit(placeHash) * total;
                            std::size_t picked = kNone;
                            for (const std::size_t r : layer) {
                                if (!eligible(r)) continue;
                                picked = r;
                                x -= rules[r].weight;
                                if (x < 0.0) break;
                            }
                            if (picked != kNone && std::find(order.begin(), order.end(), picked) == order.end())
                                order.push_back(picked);
                            for (const std::size_t r : layer)
                                if (eligible(r) && std::find(order.begin(), order.end(), r) == order.end())
                                    order.push_back(r);
                        }
                        std::size_t won = kNone;
                        for (const std::size_t r : order) {
                            const Fail why = tryRule(r);
                            if (why == Fail::NoFit) continue;
                            MassingFrontCounts& c = counts[r];
                            ++c.tried;
                            switch (why) {
                            case Fail::Pass: won = r; break;
                            case Fail::NoWall: ++c.noWall; break;
                            case Fail::Shallow: ++c.shallow; break;
                            case Fail::Blocked: ++c.blocked; break;
                            case Fail::InRoad: ++c.inRoad; break;
                            case Fail::Steep: ++c.steep; break;
                            case Fail::Wet: ++c.wet; break;
                            case Fail::Crowded: ++c.crowded; break;
                            case Fail::NoFit: break;
                            }
                            if (won != kNone) break;
                        }
                        if (won == kNone) {
                            runLeft = 0;
                            s += kMassingFrontStep;
                            continue;
                        }
                        ++counts[won].placed;
                        feet[l].add(massingFrontFoot(prop, rules[won]));
                        out.push_back(prop);
                        faceOf.push_back(h * 4 + static_cast<std::size_t>(fi));
                        if (running && won == runRule) {
                            --runLeft;
                        } else {
                            runRule = won;
                            runLeft = rules[won].run - 1;
                        }
                        s += rules[won].width + rules[won].gap;
                    }
                }
            }
        }
    };

    std::vector<double> accept(rules.size(), 1.0);
    walk(accept);
    bool again = false;
    for (std::size_t r = 0; r < rules.size(); ++r) {
        if (rules[r].cap > 0 && counts[r].placed > rules[r].cap) {
            accept[r] = static_cast<double>(rules[r].cap) / static_cast<double>(counts[r].placed);
            again = true;
        }
    }
    if (again) walk(accept);

    // Thin any rule still over its cap, evenly.
    std::vector<std::uint8_t> drop(out.size(), 0);
    bool dropped = false;
    std::vector<std::size_t> mine;
    for (std::size_t r = 0; r < rules.size(); ++r) {
        if (rules[r].cap == 0 || counts[r].placed <= rules[r].cap) continue;
        mine.clear();
        for (std::size_t k = 0; k < out.size(); ++k)
            if (out[k].rule == r) mine.push_back(k);
        std::vector<std::uint8_t> kept(mine.size(), 0);
        for (std::size_t k = 0; k < rules[r].cap; ++k) kept[k * mine.size() / rules[r].cap] = 1;
        for (std::size_t k = 0; k < mine.size(); ++k)
            if (!kept[k]) drop[mine[k]] = 1;
        counts[r].capped = mine.size() - rules[r].cap;
        counts[r].placed = rules[r].cap;
        dropped = true;
    }
    if (dropped) {
        std::size_t w = 0;
        for (std::size_t k = 0; k < out.size(); ++k) {
            if (drop[k]) continue;
            out[w] = out[k];
            faceOf[w] = faceOf[k];
            ++w;
        }
        out.resize(w);
        faceOf.resize(w);
    }

    // Faces each rule dressed, and the faces dressed on the ground layer.
    std::vector<std::size_t> lastFace(rules.size(), kNone);
    std::vector<std::uint8_t> dressed(boxes.size() * 4, 0);
    for (std::size_t k = 0; k < out.size(); ++k) {
        const std::size_t r = out[k].rule;
        if (lastFace[r] != faceOf[k]) {
            lastFace[r] = faceOf[k];
            ++counts[r].faces;
        }
        if (rules[r].dz < kMassingPropBody) dressed[faceOf[k]] = 1;
    }
    for (std::size_t k = 0; k < dressed.size(); ++k) {
        if (!dressed[k]) continue;
        ++totals.dressed;
        if (fronting[k]) ++totals.streetDressed;
    }
    if (outFronting) *outFronting = fronting;

    // The ground pieces' paving pads.
    if (!outPads) return;
    std::vector<std::size_t> padsOf(boxes.size(), 0);
    for (std::size_t k = 0; k < out.size(); ++k) {
        const MassingProp& p = out[k];
        const MassingFrontRule& rule = rules[p.rule];
        if (rule.dz >= kMassingPropBody) continue;
        const std::size_t h = faceOf[k] / 4;
        const double a = p.face * kPi / 180.0;
        const double nx = std::cos(a), ny = std::sin(a), tx = -ny, ty = nx;
        const double half = 0.5 * (rule.width + rule.gap);
        const auto front = [&](double reach) {
            const double fx = p.x + nx * reach, fy = p.y + ny * reach;
            return inRoad(fx, fy) || inRoad(fx + tx * half, fy + ty * half) || inRoad(fx - tx * half, fy - ty * half);
        };
        double reach = rule.out + kMassingFrontPadReach;
        while (reach - kMassingFrontPadStep >= rule.out - 1e-9 && front(reach)) reach -= kMassingFrontPadStep;
        reach = std::max(reach, rule.out);
        const double floor = p.z - rule.dz;
        const double cx = p.x + nx * 0.5 * reach, cy = p.y + ny * 0.5 * reach;
        // The plane through the floors at a 3 x 3 grid over the pad (u out of the face, w along it): with the grid
        // even about the middle, least squares gives the mean and each axis's grade on its own.
        const double du = kMassingFrontPadGrid * reach, dw = kMassingFrontPadGrid * 2.0 * half;
        double zs[9];
        double sum = 0.0, su = 0.0, sw = 0.0;
        for (int g = 0; g < 9; ++g) {
            const double u = (g % 3 - 1) * du, v = (g / 3 - 1) * dw;
            zs[g] = scene.floorNear(cx + nx * u + tx * v, cy + ny * u + ty * v, floor);
            sum += zs[g];
            su += zs[g] * u;
            sw += zs[g] * v;
        }
        const double mean = sum / 9.0;
        const double riseU = du > kTiny ? su / (6.0 * du * du) : 0.0, riseW = dw > kTiny ? sw / (6.0 * dw * dw) : 0.0;
        double off = 0.0;
        for (int g = 0; g < 9; ++g)
            off = std::max(off, std::fabs(zs[g] - (mean + riseU * (g % 3 - 1) * du + riseW * (g / 3 - 1) * dw)));
        if (!(off <= kMassingFrontPadFit)) {
            ++totals.padsUneven;
            continue;
        }
        MassingBox pad;
        pad.id = boxes[h].id + "-FP" + std::to_string(++padsOf[h]);
        pad.row = boxes[h].id;
        pad.kind = "landing";
        pad.source = MassingSource::Invented;
        pad.cover = false;
        pad.cx = cx;
        pad.cy = cy;
        pad.sx = reach;
        pad.sy = 2.0 * half;
        pad.rot = p.face;
        pad.riseU = riseU;
        pad.riseW = riseW;
        pad.z0 = mean - kMassingFrontPadSink;
        pad.z1 = mean + kMassingFrontPadTop;
        outPads->push_back(pad);
    }
}

void massingRoofs(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                  const std::vector<bool>& hosts, std::vector<MassingBox>& out, MassingRoofCounts& counts,
                  const std::vector<bool>* courts) {
    out.clear();
    counts = MassingRoofCounts{};
    Scene scene(ground, boxes, include, nullptr);
    std::vector<MassingFoot> clutter;
    std::vector<std::array<double, 2>> posts;
    for (std::size_t h = 0; h < boxes.size() && h < hosts.size(); ++h) {
        if (!hosts[h]) continue;
        const MassingBox& b = boxes[h];
        const Solid host = makeSolid(b);
        const bool court = courts != nullptr && h < courts->size() && (*courts)[h];
        if (b.sloped || !host.valid || host.highTop - host.lowTop > 1e-6 || b.sx < kMassingRoofSide ||
            b.sy < kMassingRoofSide || b.sx * b.sy < kMassingRoofArea ||
            (!court && host.highTop - scene.groundAt(b.cx, b.cy) < kMassingRoofHigh)) {
            ++counts.bare;
            continue;
        }
        const double top = host.highTop;
        std::size_t nRail = 0, nPost = 0, nTank = 0, nUnit = 0, nMast = 0, nBar = 0;
        std::size_t nRackPost = 0, nRackBar = 0, nPole = 0, nArm = 0;
        const std::size_t before = out.size();
        const auto add = [&](const char* tag, std::size_t& n, const char* kind, double x, double y, double sx, double sy,
                             double rot, double z0, double z1) {
            MassingBox o;
            o.id = b.id + tag + std::to_string(++n);
            o.row = b.id;
            o.kind = kind;
            o.source = MassingSource::Invented;
            o.cover = false;
            o.cx = x;
            o.cy = y;
            o.sx = sx;
            o.sy = sy;
            o.rot = rot;
            o.z0 = z0;
            o.z1 = z1;
            out.push_back(o);
        };
        const auto covered = [&](double x, double y) {
            return scene.standsAt(x, y, top + kMassingStandClear, top + kMassingRailTop, h);
        };
        const auto shared = [&](double x, double y) {
            if (scene.groundAt(x, y) > top - kMassingRoofShared) return true;
            return scene.boxGrid.any(pointAabb(x, y, 0.0), [&](std::uint32_t i) {
                double bottom = 0.0, t = 0.0;
                if (i == h || !solidHolds(scene.solids[i], x, y, bottom, t)) return false;
                return t > top - kMassingRoofShared && bottom < top;
            });
        };

        // The rails along the edges that drop away.
        posts.clear();
        for (int fi = 0; fi < 4; ++fi) {
            const Face f = boxFace(b, fi);
            const double lo = kMassingQuayEnd, hi = f.length - kMassingQuayEnd;
            if (hi - lo < kMassingQuayShortest) continue;
            const std::size_t steps =
                std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil((hi - lo) / kMassingQuayStep - 1e-9)));
            const double step = (hi - lo) / static_cast<double>(steps);
            const double rot = degrees(f.ty, f.tx);
            const auto at = [&](double s, double outward, double& x, double& y) {
                x = f.x0 + f.tx * s + f.nx * outward;
                y = f.y0 + f.ty * s + f.ny * outward;
            };
            const auto run = [&](double s0, double s1) {
                if (s1 - s0 < kMassingQuayShortest - 1e-9) return;
                double x = 0.0, y = 0.0;
                at(0.5 * (s0 + s1), -kMassingRailIn, x, y);
                add("-RR", nRail, "barrier", x, y, s1 - s0, kMassingRailBar, rot, top + kMassingRailTop - kMassingRailBar,
                    top + kMassingRailTop);
                counts.railed += s1 - s0;
                const std::size_t gaps =
                    std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil((s1 - s0) / kMassingRoofPostGap - 1e-9)));
                for (std::size_t k = 0; k <= gaps; ++k) {
                    at(s0 + (s1 - s0) * static_cast<double>(k) / static_cast<double>(gaps), -kMassingRailIn, x, y);
                    bool twin = false;
                    for (const auto& q : posts) twin = twin || std::hypot(q[0] - x, q[1] - y) < 0.05;
                    if (twin) continue;
                    posts.push_back({x, y});
                    add("-RP", nPost, "post", x, y, kMassingRailPost, kMassingRailPost, rot, top - 0.05,
                        top + kMassingRailTop - 0.5 * kMassingRailBar);
                    ++counts.posts;
                }
            };
            double start = -1.0, last = lo;
            for (std::size_t k = 0; k <= steps; ++k) {
                const double s = lo + step * static_cast<double>(k);
                double ox = 0.0, oy = 0.0, ix = 0.0, iy = 0.0;
                at(s, kMassingRoofProbe, ox, oy);
                at(s, -kMassingRoofProbe, ix, iy);
                const bool free = !shared(ox, oy) && !covered(ix, iy);
                if (free && start < 0.0) start = s;
                if (!free && start >= 0.0) {
                    run(start, last);
                    start = -1.0;
                }
                if (free) last = s;
            }
            if (start >= 0.0) run(start, last);
        }

        // The clutter.
        const double ix = 0.5 * b.sx - kMassingRoofEdgeIn, iy = 0.5 * b.sy - kMassingRoofEdgeIn;
        if (ix > 0.0 && iy > 0.0) {
            clutter.clear();
            const std::uint64_t seed = hashText(kFnvBasis, b.id);
            const double a = b.rot * kPi / 180.0, c = std::cos(a), sn = std::sin(a);
            const auto place = [&](std::uint64_t key, double hx, double hy, double& x, double& y) {
                if (hx > ix || hy > iy) return false;
                for (std::uint64_t t = 0; t < 8; ++t) {
                    const std::uint64_t k = hashNumber(hashNumber(seed, key), t);
                    const double u = (2.0 * hashUnit(k) - 1.0) * (ix - hx);
                    const double w = (2.0 * hashUnit(hashNumber(k, 7)) - 1.0) * (iy - hy);
                    x = b.cx + u * c - w * sn;
                    y = b.cy + u * sn + w * c;
                    const MassingFoot foot{x, y, hx, hy, b.rot};
                    bool clash = false;
                    for (const MassingFoot& o : clutter)
                        clash = clash || massingFeetOverlap(foot, o, kMassingRoofClutterGap);
                    if (clash || covered(x, y)) continue;
                    for (const double du : {-hx, hx})
                        for (const double dw : {-hy, hy})
                            clash = clash || covered(x + du * c - dw * sn, y + du * sn + dw * c);
                    if (clash) continue;
                    clutter.push_back(foot);
                    return true;
                }
                return false;
            };
            const double area = b.sx * b.sy;
            double x = 0.0, y = 0.0;
            if (court) {
                // The racks, each along u or w by the hash, a post at either end of its bar.
                const std::size_t racks =
                    std::min(kMassingCourtRacks, static_cast<std::size_t>(area / kMassingCourtRackArea));
                const double end = 0.5 * (kMassingCourtRackLong - kMassingCourtRackPost);
                for (std::size_t k = 0; k < racks; ++k) {
                    const bool across = hashUnit(hashNumber(seed, 500 + k)) < 0.5;
                    const double hx = 0.5 * (across ? kMassingCourtRackDeep : kMassingCourtRackLong);
                    const double hy = 0.5 * (across ? kMassingCourtRackLong : kMassingCourtRackDeep);
                    if (!place(600 + k, hx, hy, x, y)) continue;
                    const double ax = across ? -sn : c, ay = across ? c : sn;
                    for (const double e : {-end, end})
                        add("-CP", nRackPost, "post", x + ax * e, y + ay * e, kMassingCourtRackPost, kMassingCourtRackPost,
                            b.rot, top - 0.05, top + kMassingCourtRackHigh - kMassingCourtRackPost);
                    add("-CB", nRackBar, "barrier", x, y, kMassingCourtRackLong, kMassingCourtRackPost,
                        b.rot + (across ? 90.0 : 0.0), top + kMassingCourtRackHigh - kMassingCourtRackPost,
                        top + kMassingCourtRackHigh);
                    ++counts.racks;
                }
                // The lamp standards, each arm along u or w by the hash.
                const std::size_t standards =
                    std::min(kMassingCourtStandards, static_cast<std::size_t>(area / kMassingCourtStandardArea));
                for (std::size_t k = 0; k < standards; ++k) {
                    if (!place(700 + k, kMassingCourtArmLong, kMassingCourtArmLong, x, y)) continue;
                    const bool across = hashUnit(hashNumber(seed, 800 + k)) < 0.5;
                    const double ax = across ? -sn : c, ay = across ? c : sn;
                    add("-CS", nPole, "mast", x, y, kMassingCourtStandardSide, kMassingCourtStandardSide, b.rot,
                        top - 0.05, top + kMassingCourtStandardHigh);
                    add("-CA", nArm, "barrier", x + ax * 0.5 * kMassingCourtArmLong, y + ay * 0.5 * kMassingCourtArmLong,
                        kMassingCourtArmLong, 0.12, b.rot + (across ? 90.0 : 0.0),
                        top + kMassingCourtStandardHigh - 0.12, top + kMassingCourtStandardHigh);
                    ++counts.standards;
                }
            }
            const double roll = hashUnit(hashNumber(seed, 1));
            const std::size_t tanks = court ? 0 : area >= 60.0 ? (roll < 0.5 ? 2 : 1) : (roll < 0.5 ? 1 : 0);
            const std::size_t units = court ? 0 : std::min<std::size_t>(6, 1 + static_cast<std::size_t>(area / 40.0));
            const bool aerial = !court && hashUnit(hashNumber(seed, 2)) < 0.6;
            for (std::size_t k = 0; k < tanks; ++k)
                if (place(100 + k, 0.5 * kMassingRoofTankSide, 0.5 * kMassingRoofTankSide, x, y)) {
                    add("-RT", nTank, "tank", x, y, kMassingRoofTankSide, kMassingRoofTankSide, b.rot, top - 0.05,
                        top + kMassingRoofTankHeight);
                    ++counts.tanks;
                }
            for (std::size_t k = 0; k < units; ++k) {
                const bool across = hashUnit(hashNumber(seed, 300 + k)) < 0.5;
                const double hx = 0.5 * (across ? kMassingRoofUnitShort : kMassingRoofUnitLong);
                const double hy = 0.5 * (across ? kMassingRoofUnitLong : kMassingRoofUnitShort);
                if (place(200 + k, hx, hy, x, y)) {
                    add("-RU", nUnit, "unit", x, y, 2.0 * hx, 2.0 * hy, b.rot, top - 0.05, top + kMassingRoofUnitHeight);
                    ++counts.units;
                }
            }
            if (aerial && place(400, 0.5 * kMassingRoofBarLong, 0.5 * kMassingRoofBarLong, x, y)) {
                const double tall = 2.4 + hashUnit(hashNumber(seed, 3));
                add("-RM", nMast, "aerial", x, y, kMassingRoofMastSide, kMassingRoofMastSide, b.rot, top - 0.05,
                    top + tall);
                add("-RX", nBar, "aerial", x, y, kMassingRoofBarLong, kMassingRoofMastSide, b.rot + 45.0,
                    top + tall - 0.3 - kMassingRoofMastSide, top + tall - 0.3);
                ++counts.aerials;
            }
        }
        if (out.size() > before) ++(court ? counts.courts : counts.roofs);
    }
}

bool massingStandIn(const MassingBox& box, std::vector<MassingBox>& out) {
    const bool crane = box.kind == "crane", boat = box.kind == "boat";
    if ((!crane && !boat) || box.sloped || !(box.z1 > box.z0) || !(box.sx > 0.0) || !(box.sy > 0.0)) return false;
    const double a = box.rot * kPi / 180.0, c = std::cos(a), sn = std::sin(a);
    std::size_t n = 0;
    // A piece at (u, w) in the box's frame, lu along u by lw along w, z0 to z1 at its middle, rising riseU along u.
    const auto piece = [&](double u, double w, double lu, double lw, double z0, double z1, double riseU) {
        MassingBox p;
        p.id = box.id + "-S" + std::to_string(++n);
        p.row = box.id;
        p.kind = box.kind;
        p.source = MassingSource::Invented;
        p.cover = false;
        p.cx = box.cx + u * c - w * sn;
        p.cy = box.cy + u * sn + w * c;
        p.sx = lu;
        p.sy = lw;
        p.rot = box.rot;
        p.z0 = z0;
        p.z1 = z1;
        p.riseU = riseU;
        out.push_back(std::move(p));
    };
    const double h = box.z1 - box.z0, hu = 0.5 * box.sx, hw = 0.5 * box.sy;
    if (crane) {
        const double leg = kMassingCraneLeg, portal = box.z0 + 0.4 * h, house = portal + 0.14 * h;
        for (const double su : {-1.0, 1.0})
            for (const double sw : {-1.0, 1.0})
                piece(su * (hu - 0.5 * leg), sw * (hw - 0.5 * leg), leg, leg, box.z0, portal - leg, 0.0);
        for (const double sw : {-1.0, 1.0}) piece(0.0, sw * (hw - 0.5 * leg), box.sx, leg, portal - leg, portal, 0.0);
        for (const double su : {-1.0, 1.0})
            piece(su * (hu - 0.5 * leg), 0.0, leg, box.sy - 2.0 * leg, portal - leg, portal, 0.0);
        piece(0.0, 0.0, 0.6 * box.sx, 0.8 * box.sy, portal, house, 0.0);
        const double mast = 0.3 * box.sy;
        for (const double sw : {-1.0, 1.0}) piece(0.0, sw * mast, leg, leg, house, box.z1 - leg, 0.0);
        piece(0.0, 0.0, leg, 2.0 * mast + leg, box.z1 - leg, box.z1, 0.0);
        const double boomTop = box.z0 + 0.62 * h, back = -box.sx, front = hu + 2.5 * box.sx;
        piece(0.5 * (back + front), 0.0, front - back, kMassingCraneBoom, boomTop - kMassingCraneBoom, boomTop, 0.0);
        // The ties: from the cross-head's top (u 0) to the boom's top at its ends, their middles on that line.
        const double head = box.z1, tie = 0.25;
        piece(0.5 * back, 0.0, -back, 0.5, 0.5 * (head + boomTop) - tie, 0.5 * (head + boomTop) + tie,
              (head - boomTop) / -back);
        piece(0.5 * front, 0.0, front, 0.5, 0.5 * (head + boomTop) - tie, 0.5 * (head + boomTop) + tie,
              (boomTop - head) / front);
        return true;
    }
    // A boat: the pieces along its long side.
    const bool alongU = box.sx >= box.sy;
    const double length = alongU ? box.sx : box.sy, beam = alongU ? box.sy : box.sx;
    const auto hull = [&](double along, double lLong, double lBeam, double z0, double z1) {
        if (alongU)
            piece(along, 0.0, lLong, lBeam, z0, z1, 0.0);
        else
            piece(0.0, along, lBeam, lLong, z0, z1, 0.0);
    };
    const double waist = box.z0 + 0.45 * h;
    hull(0.0, 0.85 * length, 0.7 * beam, box.z0, waist);
    hull(0.0, length, beam, waist, box.z1);
    const double cabin = (length > 20.0 ? 0.6 : 0.25) * length;
    hull(-0.5 * length + 0.1 * length + 0.5 * cabin, cabin, 0.7 * beam, box.z1, box.z1 + 2.0 + 0.1 * length);
    return true;
}

MassingWindowData massingWindowData(const MassingBox& box, const MassingGround& ground, const std::uint8_t* fronting) {
    MassingWindowData d;
    d.seed = windowSeed(static_cast<std::uint32_t>(hashUnit(hashText(kFnvBasis, box.id)) * 4294967296.0));
    const double height = std::max(box.z1 - box.z0, 0.0);
    for (int f = 0; f < 4; ++f) {
        if (fronting && fronting[f] != 0) d.streetBits = static_cast<std::uint8_t>(d.streetBits | (1u << f));
        const Face face = boxFace(box, f);
        const double out = 0.5 * face.depth + kMassingWindowProbe;
        const double g = massingGroundAt(ground.grid, ground.samplesX, ground.samplesY, ground.spacing, ground.originX,
                                         ground.originY, box.cx + face.nx * out, box.cy + face.ny * out);
        d.base[f] = std::clamp(g - box.z0, 0.0, height);
    }
    return d;
}

std::size_t massingShopLights(const MassingBox& box, const MassingWindowData& data, double spacing,
                              std::vector<MassingShopLight>& out) {
    if (box.sloped) return 0;
    std::size_t added = 0;
    for (int f = 0; f < 4; ++f) {
        if (((data.streetBits >> f) & 1u) == 0) continue;
        const double z = box.z0 + data.base[f] + kMassingShopLightUp;
        if (z > box.z1) continue;
        const Face face = boxFace(box, f);
        if (face.length <= 0.0) continue;
        const int n = spacing > 0.0 ? std::max(1, static_cast<int>(std::ceil(face.length / spacing - 1e-9))) : 1;
        const double ox = face.x0 + face.nx * kMassingShopLightOut, oy = face.y0 + face.ny * kMassingShopLightOut;
        for (int k = 0; k < n; ++k) {
            const double along = (k + 0.5) / n * face.length;
            MassingShopLight light;
            light.x = ox + face.tx * along;
            light.y = oy + face.ty * along;
            light.z = z;
            light.face = degrees(face.ny, face.nx);
            light.faceIndex = f;
            out.push_back(light);
            ++added;
        }
    }
    return added;
}

} // namespace ftcore
