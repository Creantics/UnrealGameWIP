#include "ftcore/TerrainPocket.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ftcore {

namespace {

constexpr double kTpPi = 3.14159265358979323846;
constexpr std::uint8_t kStand = 1; // standable
constexpr std::uint8_t kNode = 2;  // in play and not squeezed
constexpr std::uint8_t kBoxTop = 4;
constexpr std::uint8_t kFwd = 8;   // reached from an anchor
constexpr std::uint8_t kBack = 16; // reaches an anchor
// A cell the lead slides on: a neighbour no more than this (m) higher is still a move (across the slope, or off its foot).
constexpr double kSlideLevel = 0.01;
// A pocket within this (m) of an exempt rect's sides counts as inside it.
constexpr double kExemptSlack = 0.5;
// A stair takes at most this many steps.
constexpr int kStairStepsMax = 80;
constexpr std::uint8_t kSeen = 32; // taken into a pocket group
constexpr std::size_t kMaxCells = 100000000;
constexpr double kNearBox = 1.0;

double groundAt(const MassingGround& g, double x, double y) {
    return massingGroundAt(g.grid, g.samplesX, g.samplesY, g.spacing, g.originX, g.originY, x, y);
}

// A box in its own axes, for the cells under it.
struct BoxFrame {
    const MassingBox* box = nullptr;
    double c = 1.0, s = 0.0;
    double hx = 0.0, hy = 0.0; // plan half extents of its footprint's bounds
    double bottomMin = 0.0;
    double nz = 1.0;
};

BoxFrame frameOf(const MassingBox& b) {
    BoxFrame f;
    f.box = &b;
    const double a = b.rot * kTpPi / 180.0;
    f.c = std::cos(a);
    f.s = std::sin(a);
    f.hx = 0.5 * (std::fabs(f.c) * b.sx + std::fabs(f.s) * b.sy);
    f.hy = 0.5 * (std::fabs(f.s) * b.sx + std::fabs(f.c) * b.sy);
    if (b.sloped) {
        const double lo = std::min(b.slopeFrom, b.slopeTo);
        f.bottomMin = b.slopeThick > 0.0 ? lo - b.slopeThick : b.z0;
        const double g = b.sx > 0.0 ? (b.slopeTo - b.slopeFrom) / b.sx : 0.0;
        f.nz = 1.0 / std::sqrt(1.0 + g * g);
    } else {
        f.bottomMin = b.z0 - 0.5 * (std::fabs(b.riseU) * b.sx + std::fabs(b.riseW) * b.sy);
        f.nz = 1.0 / std::sqrt(1.0 + b.riseU * b.riseU + b.riseW * b.riseW);
    }
    return f;
}

// The box's solid over a plan point (massingCorners' shape, read at the point): false outside its footprint.
bool solidAt(const BoxFrame& f, double x, double y, double& bottom, double& top) {
    const MassingBox& b = *f.box;
    const double dx = x - b.cx, dy = y - b.cy;
    const double u = dx * f.c + dy * f.s, w = -dx * f.s + dy * f.c;
    if (std::fabs(u) > b.sx * 0.5 || std::fabs(w) > b.sy * 0.5) return false;
    if (b.sloped) {
        const double zt = b.slopeFrom + (b.slopeTo - b.slopeFrom) * (u / b.sx + 0.5);
        top = zt;
        bottom = b.slopeThick > 0.0 ? zt - b.slopeThick : b.z0;
    } else {
        const double lift = b.riseU * u + b.riseW * w;
        bottom = b.z0 + lift;
        top = b.z0 + std::max(b.z1 - b.z0, kMassingMinHeight) + lift;
    }
    return true;
}

bool usable(const MassingBox& b) { return b.sx > 0.0 && b.sy > 0.0 && std::isfinite(b.cx) && std::isfinite(b.cy); }

// The boxes by bucket, for the points the skirts test.
struct BoxBuckets {
    double x0 = 0.0, y0 = 0.0, size = 8.0;
    std::size_t nx = 0, ny = 0;
    std::vector<std::vector<std::uint32_t>> cells;
    std::vector<BoxFrame> frames;

    void build(const std::vector<MassingBox>& boxes, const std::vector<bool>& include, const PocketRect& r) {
        x0 = r.minX;
        y0 = r.minY;
        nx = static_cast<std::size_t>(std::max(1.0, std::ceil((r.maxX - r.minX) / size)));
        ny = static_cast<std::size_t>(std::max(1.0, std::ceil((r.maxY - r.minY) / size)));
        cells.assign(nx * ny, {});
        frames.resize(boxes.size());
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            frames[i] = frameOf(boxes[i]);
            if (i >= include.size() || !include[i] || !usable(boxes[i])) continue;
            const BoxFrame& f = frames[i];
            const long i0 = std::max(0L, static_cast<long>(std::floor((boxes[i].cx - f.hx - x0) / size)));
            const long i1 = std::min(static_cast<long>(nx) - 1, static_cast<long>(std::floor((boxes[i].cx + f.hx - x0) / size)));
            const long j0 = std::max(0L, static_cast<long>(std::floor((boxes[i].cy - f.hy - y0) / size)));
            const long j1 = std::min(static_cast<long>(ny) - 1, static_cast<long>(std::floor((boxes[i].cy + f.hy - y0) / size)));
            for (long j = j0; j <= j1; ++j)
                for (long k = i0; k <= i1; ++k)
                    cells[static_cast<std::size_t>(j) * nx + static_cast<std::size_t>(k)].push_back(static_cast<std::uint32_t>(i));
        }
    }
    const std::vector<std::uint32_t>* at(double x, double y) const {
        const double fi = std::floor((x - x0) / size), fj = std::floor((y - y0) / size);
        if (fi < 0.0 || fj < 0.0 || fi >= static_cast<double>(nx) || fj >= static_cast<double>(ny)) return nullptr;
        return &cells[static_cast<std::size_t>(fj) * nx + static_cast<std::size_t>(fi)];
    }
};

// The check's grid and its groups.
struct Grid {
    std::size_t nx = 0, ny = 0;
    double x0 = 0.0, y0 = 0.0, cell = 0.5;
    std::vector<float> floor;
    std::vector<std::uint8_t> flags;
    std::vector<std::uint32_t> queue;
    std::vector<std::vector<std::uint32_t>> groups; // each pocket's cells, in the report's order

    double cx(std::size_t i) const { return x0 + (static_cast<double>(i) + 0.5) * cell; }
    double cy(std::size_t j) const { return y0 + (static_cast<double>(j) + 0.5) * cell; }
};

bool buildGrid(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
               const PocketRect& rect, const PocketSpec& spec, Grid& g, std::string* error) {
    const double w = rect.maxX - rect.minX, h = rect.maxY - rect.minY;
    if (!(spec.cell > 0.0) || !(w > 0.0) || !(h > 0.0) || !std::isfinite(w) || !std::isfinite(h)) {
        if (error) *error = "the pocket rect has no area";
        return false;
    }
    const double nxD = std::ceil(w / spec.cell), nyD = std::ceil(h / spec.cell);
    if (nxD * nyD > static_cast<double>(kMaxCells)) {
        if (error) *error = "the pocket rect is over 100 million cells";
        return false;
    }
    g.nx = static_cast<std::size_t>(nxD);
    g.ny = static_cast<std::size_t>(nyD);
    g.x0 = rect.minX;
    g.y0 = rect.minY;
    g.cell = spec.cell;
    const std::size_t n = g.nx * g.ny;
    g.floor.assign(n, 0.0f);
    g.flags.assign(n, 0);
    g.groups.clear();
    for (std::size_t j = 0; j < g.ny; ++j)
        for (std::size_t i = 0; i < g.nx; ++i) g.floor[j * g.nx + i] = static_cast<float>(groundAt(ground, g.cx(i), g.cy(j)));
    // The ground's slope from the cells either side.
    for (std::size_t j = 0; j < g.ny; ++j) {
        for (std::size_t i = 0; i < g.nx; ++i) {
            const std::size_t iw = i > 0 ? i - 1 : i, ie = i + 1 < g.nx ? i + 1 : i;
            const std::size_t js = j > 0 ? j - 1 : j, jn = j + 1 < g.ny ? j + 1 : j;
            const double gx = (static_cast<double>(g.floor[j * g.nx + ie]) - g.floor[j * g.nx + iw]) /
                              (static_cast<double>(ie - iw) * g.cell + (ie == iw ? 1.0 : 0.0));
            const double gy = (static_cast<double>(g.floor[jn * g.nx + i]) - g.floor[js * g.nx + i]) /
                              (static_cast<double>(jn - js) * g.cell + (jn == js ? 1.0 : 0.0));
            if (1.0 / std::sqrt(1.0 + gx * gx + gy * gy) >= spec.walkableZ) g.flags[j * g.nx + i] = kStand;
        }
    }
    // The boxes, lowest bottom first.
    std::vector<BoxFrame> frames;
    for (std::size_t i = 0; i < boxes.size() && i < include.size(); ++i)
        if (include[i] && usable(boxes[i])) frames.push_back(frameOf(boxes[i]));
    std::sort(frames.begin(), frames.end(), [](const BoxFrame& a, const BoxFrame& b) { return a.bottomMin < b.bottomMin; });
    for (const BoxFrame& f : frames) {
        const MassingBox& b = *f.box;
        const double fi0 = std::ceil((b.cx - f.hx - g.x0) / g.cell - 0.5), fi1 = std::floor((b.cx + f.hx - g.x0) / g.cell - 0.5);
        const double fj0 = std::ceil((b.cy - f.hy - g.y0) / g.cell - 0.5), fj1 = std::floor((b.cy + f.hy - g.y0) / g.cell - 0.5);
        if (fi1 < 0.0 || fj1 < 0.0 || fi0 >= static_cast<double>(g.nx) || fj0 >= static_cast<double>(g.ny)) continue;
        const std::size_t i0 = static_cast<std::size_t>(std::max(0.0, fi0)), j0 = static_cast<std::size_t>(std::max(0.0, fj0));
        const std::size_t i1 = std::min(g.nx - 1, static_cast<std::size_t>(fi1)), j1 = std::min(g.ny - 1, static_cast<std::size_t>(fj1));
        const bool flatTop = f.nz >= spec.walkableZ;
        for (std::size_t j = j0; j <= j1; ++j) {
            for (std::size_t i = i0; i <= i1; ++i) {
                double bottom = 0.0, top = 0.0;
                if (!solidAt(f, g.cx(i), g.cy(j), bottom, top)) continue;
                const std::size_t k = j * g.nx + i;
                const double fl = g.floor[k];
                if (top > fl && bottom < fl + spec.capsuleHeight) {
                    g.floor[k] = static_cast<float>(top);
                    g.flags[k] = static_cast<std::uint8_t>(kBoxTop | (flatTop ? kStand : 0));
                }
            }
        }
    }
    // In play: not water, not squeezed.
    const auto wall = [&](std::size_t k, std::size_t m) { return g.floor[m] > g.floor[k] + spec.maxStep; };
    for (std::size_t j = 0; j < g.ny; ++j) {
        for (std::size_t i = 0; i < g.nx; ++i) {
            const std::size_t k = j * g.nx + i;
            if (g.floor[k] < spec.seaLevel) continue;
            const bool squeezeX = i > 0 && i + 1 < g.nx && wall(k, k - 1) && wall(k, k + 1);
            const bool squeezeY = j > 0 && j + 1 < g.ny && wall(k, k - g.nx) && wall(k, k + g.nx);
            if (!squeezeX && !squeezeY) g.flags[k] |= kNode;
        }
    }
    return true;
}

// Whether the lead moves from cell a to its side neighbour b.
inline bool canMove(const Grid& g, const PocketSpec& spec, std::size_t a, std::size_t b) {
    const std::uint8_t fa = g.flags[a], fb = g.flags[b];
    if (!(fa & kNode) || !(fb & kNode)) return false;
    const double dz = static_cast<double>(g.floor[b]) - g.floor[a];
    if (!(fa & kStand)) return dz <= kSlideLevel; // sliding: down, or along the slope (not up it)
    if (dz <= kPocketFlatRise) return true;
    if (!(fb & kStand)) return false;
    if (dz <= spec.maxStep) return true;
    return spec.jumpRise > spec.maxStep && dz <= spec.jumpRise;
}

template <typename F>
inline void eachSide(const Grid& g, std::size_t k, F&& f) {
    const std::size_t i = k % g.nx, j = k / g.nx;
    if (i > 0) f(k - 1);
    if (i + 1 < g.nx) f(k + 1);
    if (j > 0) f(k - g.nx);
    if (j + 1 < g.ny) f(k + g.nx);
}

void analyse(const MassingGround& ground, const std::vector<MassingBox>& boxes, const PocketSpec& spec,
             const std::vector<PocketPoint>& anchors, Grid& g, PocketReport& out) {
    (void)ground;
    out = PocketReport{};
    out.cellsX = g.nx;
    out.cellsY = g.ny;
    const std::size_t n = g.nx * g.ny;
    std::vector<std::uint32_t>& q = g.queue;
    q.clear();
    q.reserve(std::min<std::size_t>(n, 1u << 22));
    // The ways out: standable cells on the edge, and the cells under the points. The ways in: the points (the edge too
    // when there are none).
    std::vector<std::uint32_t> seeds, ins;
    const auto seed = [&](std::size_t k) {
        if ((g.flags[k] & kNode) && (g.flags[k] & kStand)) seeds.push_back(static_cast<std::uint32_t>(k));
    };
    for (std::size_t i = 0; i < g.nx; ++i) {
        seed(i);
        seed((g.ny - 1) * g.nx + i);
    }
    for (std::size_t j = 1; j + 1 < g.ny; ++j) {
        seed(j * g.nx);
        seed(j * g.nx + g.nx - 1);
    }
    for (const PocketPoint& p : anchors) {
        const double fi = std::floor((p.x - g.x0) / g.cell), fj = std::floor((p.y - g.y0) / g.cell);
        if (fi < 0.0 || fj < 0.0 || fi >= static_cast<double>(g.nx) || fj >= static_cast<double>(g.ny)) continue;
        const std::size_t k = static_cast<std::size_t>(fj) * g.nx + static_cast<std::size_t>(fi);
        if (g.flags[k] & kNode) ins.push_back(static_cast<std::uint32_t>(k));
    }
    if (ins.empty()) ins = seeds;
    seeds.insert(seeds.end(), ins.begin(), ins.end());
    for (std::size_t k = 0; k < n; ++k)
        if (g.flags[k] & kNode) ++out.inPlay;
    // Forward: where the lead gets to.
    for (const std::uint32_t s : ins) {
        if (g.flags[s] & kFwd) continue;
        g.flags[s] |= kFwd;
        q.push_back(s);
    }
    for (std::size_t h = 0; h < q.size(); ++h) {
        const std::size_t a = q[h];
        eachSide(g, a, [&](std::size_t b) {
            if (!(g.flags[b] & kFwd) && canMove(g, spec, a, b)) {
                g.flags[b] |= kFwd;
                q.push_back(static_cast<std::uint32_t>(b));
            }
        });
    }
    out.reached = q.size();
    // Backward: where the lead gets back to an anchor from.
    q.clear();
    for (const std::uint32_t s : seeds) {
        if (g.flags[s] & kBack) continue;
        g.flags[s] |= kBack;
        q.push_back(s);
    }
    // The sea is a way out too: a cell beside water drops into it.
    for (std::size_t k = 0; k < n; ++k) {
        if (!(g.flags[k] & kNode) || (g.flags[k] & kBack)) continue;
        bool wet = false;
        eachSide(g, k, [&](std::size_t m) { wet = wet || g.floor[m] < spec.seaLevel; });
        if (!wet) continue;
        g.flags[k] |= kBack;
        q.push_back(static_cast<std::uint32_t>(k));
    }
    for (std::size_t h = 0; h < q.size(); ++h) {
        const std::size_t b = q[h];
        eachSide(g, b, [&](std::size_t a) {
            if (!(g.flags[a] & kBack) && canMove(g, spec, a, b)) {
                g.flags[a] |= kBack;
                q.push_back(static_cast<std::uint32_t>(a));
            }
        });
    }
    // The pockets: reached, not reaching back, side-connected.
    std::vector<BoxFrame> buildings;
    for (const MassingBox& b : boxes)
        if (pocketBuilding(b)) buildings.push_back(frameOf(b));
    std::vector<std::uint16_t> dist;
    for (std::size_t k0 = 0; k0 < n; ++k0) {
        const std::uint8_t f0 = g.flags[k0];
        if (!(f0 & kFwd) || (f0 & kBack) || (f0 & kSeen)) continue;
        std::vector<std::uint32_t> cells;
        cells.push_back(static_cast<std::uint32_t>(k0));
        g.flags[k0] |= kSeen;
        for (std::size_t h = 0; h < cells.size(); ++h) {
            eachSide(g, cells[h], [&](std::size_t m) {
                const std::uint8_t fm = g.flags[m];
                if ((fm & kFwd) && !(fm & kBack) && !(fm & kSeen)) {
                    g.flags[m] |= kSeen;
                    cells.push_back(static_cast<std::uint32_t>(m));
                }
            });
        }
        Pocket p;
        p.cells = cells.size();
        p.area = static_cast<double>(cells.size()) * g.cell * g.cell;
        p.minX = p.minY = std::numeric_limits<double>::max();
        p.maxX = p.maxY = std::numeric_limits<double>::lowest();
        p.floorMin = std::numeric_limits<double>::max();
        double sx = 0.0, sy = 0.0;
        for (const std::uint32_t k : cells) {
            const double x = g.cx(k % g.nx), y = g.cy(k / g.nx);
            sx += x;
            sy += y;
            p.minX = std::min(p.minX, x - 0.5 * g.cell);
            p.maxX = std::max(p.maxX, x + 0.5 * g.cell);
            p.minY = std::min(p.minY, y - 0.5 * g.cell);
            p.maxY = std::max(p.maxY, y + 0.5 * g.cell);
            p.floorMin = std::min(p.floorMin, static_cast<double>(g.floor[k]));
            eachSide(g, k, [&](std::size_t m) {
                if ((g.flags[m] & kBack) && (g.flags[m] & kNode)) {
                    const double fm = g.floor[m];
                    if (!p.hasSpill || fm < p.spill) p.spill = fm;
                    p.hasSpill = true;
                }
            });
        }
        p.cx = sx / static_cast<double>(cells.size());
        p.cy = sy / static_cast<double>(cells.size());
        // Its width: twice its floor's deepest cell's steps in from the floor's edge (city block, a cell a step); its
        // floor, the cells the lead stands on (all of them when it stands on none: a slide).
        {
            std::vector<std::uint32_t> local; // indices into sorted, by distance
            std::vector<std::uint32_t> sorted;
            for (const std::uint32_t k : cells)
                if (g.flags[k] & kStand) sorted.push_back(k);
            if (sorted.empty()) sorted = cells;
            std::sort(sorted.begin(), sorted.end());
            dist.assign(sorted.size(), 0);
            const auto indexOf = [&](std::size_t m) -> std::size_t {
                const auto it = std::lower_bound(sorted.begin(), sorted.end(), static_cast<std::uint32_t>(m));
                return (it != sorted.end() && *it == m) ? static_cast<std::size_t>(it - sorted.begin()) : sorted.size();
            };
            for (std::size_t s = 0; s < sorted.size(); ++s) {
                const std::size_t k = sorted[s], i = k % g.nx, j = k / g.nx;
                const bool edge = i == 0 || j == 0 || i + 1 == g.nx || j + 1 == g.ny || indexOf(k - 1) == sorted.size() ||
                                  indexOf(k + 1) == sorted.size() || indexOf(k - g.nx) == sorted.size() ||
                                  indexOf(k + g.nx) == sorted.size();
                if (edge) {
                    dist[s] = 1;
                    local.push_back(static_cast<std::uint32_t>(s));
                }
            }
            std::uint16_t deepest = local.empty() ? 0 : 1;
            for (std::size_t h = 0; h < local.size(); ++h) {
                const std::size_t s = local[h];
                eachSide(g, sorted[s], [&](std::size_t m) {
                    const std::size_t t = indexOf(m);
                    if (t < sorted.size() && dist[t] == 0) {
                        dist[t] = static_cast<std::uint16_t>(std::min<int>(dist[s] + 1, 65535));
                        deepest = std::max(deepest, dist[t]);
                        local.push_back(static_cast<std::uint32_t>(t));
                    }
                });
            }
            p.width = 2.0 * static_cast<double>(deepest) * g.cell;
        }
        // A building within kNearBox of it.
        for (std::size_t bi = 0; bi < buildings.size() && p.nearBox < 0; ++bi) {
            const BoxFrame& f = buildings[bi];
            const MassingBox& b = *f.box;
            if (b.cx + f.hx < p.minX - kNearBox || b.cx - f.hx > p.maxX + kNearBox || b.cy + f.hy < p.minY - kNearBox ||
                b.cy - f.hy > p.maxY + kNearBox)
                continue;
            const std::size_t step = std::max<std::size_t>(1, cells.size() / 4000);
            for (std::size_t c = 0; c < cells.size(); c += step) {
                const std::size_t k = cells[c];
                if (massingFootprintGap(b, g.cx(k % g.nx), g.cy(k / g.nx)) <= kNearBox) {
                    p.nearBox = static_cast<int>(f.box - boxes.data());
                    break;
                }
            }
        }
        // An exempt rect holding all of it: not reported.
        bool exempt = false;
        for (const PocketRect& r : spec.exempt)
            exempt = exempt || (p.minX >= r.minX - kExemptSlack && p.maxX <= r.maxX + kExemptSlack && p.minY >= r.minY - kExemptSlack &&
                                p.maxY <= r.maxY + kExemptSlack);
        if (exempt) {
            ++out.exempted;
            continue;
        }
        out.pocketCells += cells.size();
        out.pockets.push_back(p);
        g.groups.push_back(std::move(cells));
    }
    // Largest first (the groups kept in the same order).
    std::vector<std::size_t> order(out.pockets.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return out.pockets[a].cells > out.pockets[b].cells; });
    std::vector<Pocket> pockets;
    std::vector<std::vector<std::uint32_t>> groups;
    pockets.reserve(order.size());
    groups.reserve(order.size());
    for (const std::size_t i : order) {
        pockets.push_back(out.pockets[i]);
        groups.push_back(std::move(g.groups[i]));
    }
    out.pockets = std::move(pockets);
    g.groups = std::move(groups);
}

MassingBox plinthBox(const std::string& id, double cx, double cy, double sx, double sy, double rot, double z0, double z1) {
    MassingBox b;
    b.id = id;
    b.row = "PLINTH";
    b.kind = "plinth";
    b.source = MassingSource::Invented;
    b.cover = false;
    b.cx = cx;
    b.cy = cy;
    b.sx = sx;
    b.sy = sy;
    b.rot = rot;
    b.z0 = z0;
    b.z1 = z1;
    return b;
}

double slopeNz(const MassingGround& ground, double x, double y) {
    constexpr double h = 0.5;
    const double gx = (groundAt(ground, x + h, y) - groundAt(ground, x - h, y)) / (2.0 * h);
    const double gy = (groundAt(ground, x, y + h) - groundAt(ground, x, y - h)) / (2.0 * h);
    return 1.0 / std::sqrt(1.0 + gx * gx + gy * gy);
}

// The skirts of one building, appended to out.
void skirtsOf(const MassingGround& ground, const std::vector<MassingBox>& boxes, std::size_t self, const BoxBuckets& buckets,
              const PlinthSpec& spec, std::vector<MassingBox>& out, std::size_t& counter, bool& sloped) {
    const MassingBox& b = boxes[self];
    const double a = b.rot * kTpPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    const auto toPlan = [&](double u, double w, double& x, double& y) {
        x = b.cx + u * c - w * s;
        y = b.cy + u * s + w * c;
    };
    // Sloping ground: the ground within the band spans more than a step.
    {
        double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
        const double hu = 0.5 * b.sx + spec.band, hw = 0.5 * b.sy + spec.band;
        const int nu = std::max(2, static_cast<int>(std::ceil(2.0 * hu))), nw = std::max(2, static_cast<int>(std::ceil(2.0 * hw)));
        for (int iu = 0; iu <= nu; ++iu) {
            for (int iw = 0; iw <= nw; ++iw) {
                double x = 0.0, y = 0.0;
                toPlan(-hu + 2.0 * hu * iu / nu, -hw + 2.0 * hw * iw / nw, x, y);
                const double z = groundAt(ground, x, y);
                lo = std::min(lo, z);
                hi = std::max(hi, z);
            }
        }
        sloped = hi - lo > spec.pocket.maxStep;
        if (!sloped) return;
    }
    // Whether another box already stands at a point up to `top`.
    const auto taken = [&](double x, double y, double top) {
        const std::vector<std::uint32_t>* list = buckets.at(x, y);
        if (!list) return false;
        for (const std::uint32_t i : *list) {
            if (i == self) continue;
            double bottom = 0.0, t = 0.0;
            if (solidAt(buckets.frames[i], x, y, bottom, t) && t >= top - kPocketSkirtMerge && bottom < top) return true;
        }
        return false;
    };
    constexpr double kOut[] = {0.1, 0.5, 1.0, 1.5, 2.0};
    for (int face = 0; face < 4; ++face) {
        // Faces 0, 1: -u, +u (running along w); 2, 3: -w, +w (running along u).
        const bool alongW = face < 2;
        const double sign = (face % 2 == 0) ? -1.0 : 1.0;
        const double length = alongW ? b.sy : b.sx;
        const double half = alongW ? 0.5 * b.sx : 0.5 * b.sy;
        const int pieces = std::max(1, static_cast<int>(std::ceil(length / spec.segment - 1e-9)));
        const double seg = length / pieces;
        struct Piece {
            bool on = false;
            double top = 0.0, bottom = 0.0;
        };
        std::vector<Piece> row(static_cast<std::size_t>(pieces));
        for (int k = 0; k < pieces; ++k) {
            const double t = -0.5 * length + (k + 0.5) * seg;
            double gmin = std::numeric_limits<double>::max(), gmax = std::numeric_limits<double>::lowest();
            double nearMin = std::numeric_limits<double>::max(), farMax = std::numeric_limits<double>::lowest();
            bool steep = false, wet = false;
            for (int it = -1; it <= 1; ++it) {
                const double along = t + it * 0.45 * seg;
                for (std::size_t io = 0; io < sizeof kOut / sizeof kOut[0]; ++io) {
                    const double o = kOut[io] * spec.band / 2.0;
                    const double n = sign * (half + o);
                    double x = 0.0, y = 0.0;
                    if (alongW) toPlan(n, along, x, y);
                    else toPlan(along, n, x, y);
                    const double z = groundAt(ground, x, y);
                    gmin = std::min(gmin, z);
                    gmax = std::max(gmax, z);
                    if (io == 0) {
                        nearMin = std::min(nearMin, z);
                        wet = wet || z < spec.pocket.seaLevel;
                    }
                    if (io + 1 == sizeof kOut / sizeof kOut[0]) farMax = std::max(farMax, z);
                    if (slopeNz(ground, x, y) < spec.pocket.walkableZ) steep = true;
                }
            }
            const bool dip = farMax - nearMin > spec.pocket.maxStep;
            if (wet || !(steep || dip)) continue;
            double x = 0.0, y = 0.0;
            if (alongW) toPlan(sign * (half + 0.5 * spec.band), t, x, y);
            else toPlan(t, sign * (half + 0.5 * spec.band), x, y);
            if (taken(x, y, gmax)) continue;
            row[static_cast<std::size_t>(k)] = Piece{true, gmax, gmin - spec.sink};
        }
        // Runs of pieces with tops within kPocketSkirtMerge join.
        int k = 0;
        while (k < pieces) {
            if (!row[static_cast<std::size_t>(k)].on) {
                ++k;
                continue;
            }
            const int first = k;
            double top = row[static_cast<std::size_t>(k)].top, bottom = row[static_cast<std::size_t>(k)].bottom;
            double lo = top, hi = top;
            ++k;
            while (k < pieces && row[static_cast<std::size_t>(k)].on) {
                const double t2 = row[static_cast<std::size_t>(k)].top;
                if (std::max(hi, t2) - std::min(lo, t2) > kPocketSkirtMerge) break;
                lo = std::min(lo, t2);
                hi = std::max(hi, t2);
                bottom = std::min(bottom, row[static_cast<std::size_t>(k)].bottom);
                ++k;
            }
            top = hi;
            const double t0 = -0.5 * length + first * seg, t1 = -0.5 * length + k * seg;
            const double mid = 0.5 * (t0 + t1), len = t1 - t0;
            double x = 0.0, y = 0.0;
            const double n = sign * (half + 0.5 * spec.band);
            if (alongW) toPlan(n, mid, x, y);
            else toPlan(mid, n, x, y);
            out.push_back(plinthBox("PL-S-" + std::to_string(++counter), x, y, alongW ? spec.band : len, alongW ? len : spec.band, b.rot,
                                    bottom, top));
        }
    }
}

// Grid-aligned fill boxes over a pocket's cells whose floor is under `level`.
void fillsOf(const MassingGround& ground, const Grid& g, const std::vector<std::uint32_t>& cells, double level, double sink,
             std::vector<MassingBox>& out, std::size_t& counter, std::size_t& made) {
    struct Run {
        std::size_t i0, i1, j;
    };
    std::vector<std::uint32_t> under;
    for (const std::uint32_t k : cells)
        if (static_cast<double>(g.floor[k]) < level) under.push_back(k);
    std::sort(under.begin(), under.end());
    std::vector<Run> runs;
    for (std::size_t h = 0; h < under.size();) {
        const std::size_t k = under[h], j = k / g.nx, i = k % g.nx;
        std::size_t e = h + 1;
        while (e < under.size() && under[e] == under[e - 1] + 1 && under[e] / g.nx == j) ++e;
        runs.push_back(Run{i, i + (e - h) - 1, j});
        h = e;
    }
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
        if (a.i0 != b.i0) return a.i0 < b.i0;
        if (a.i1 != b.i1) return a.i1 < b.i1;
        return a.j < b.j;
    });
    for (std::size_t h = 0; h < runs.size();) {
        std::size_t e = h + 1;
        while (e < runs.size() && runs[e].i0 == runs[h].i0 && runs[e].i1 == runs[h].i1 && runs[e].j == runs[e - 1].j + 1) ++e;
        const double x0 = g.x0 + static_cast<double>(runs[h].i0) * g.cell, x1 = g.x0 + static_cast<double>(runs[h].i1 + 1) * g.cell;
        const double y0 = g.y0 + static_cast<double>(runs[h].j) * g.cell, y1 = g.y0 + static_cast<double>(runs[e - 1].j + 1) * g.cell;
        double lo = std::numeric_limits<double>::max();
        for (double y = y0; y <= y1 + 1e-9; y += g.cell * 0.5)
            for (double x = x0; x <= x1 + 1e-9; x += g.cell * 0.5) lo = std::min(lo, groundAt(ground, x, y));
        out.push_back(plinthBox("PL-F-" + std::to_string(++counter), 0.5 * (x0 + x1), 0.5 * (y0 + y1), x1 - x0, y1 - y0, 0.0,
                                std::min(lo, level) - sink, level));
        ++made;
        h = e;
    }
}

// One stair out of a pocket: from a cell beside it that has a way out (its floor the stair's top) straight into it, a step
// every cell down by `rise`, three cells wide, until a cell of the pocket lies no lower than the step it would carry. Tries
// the lowest such sides first. False when no side gives one.
bool stairOf(const MassingGround& ground, const Grid& g, const std::vector<std::uint32_t>& cells, double rise, double sink,
             std::vector<MassingBox>& out, std::size_t& counter) {
    std::vector<std::uint32_t> in(cells);
    std::sort(in.begin(), in.end());
    const auto inPocket = [&](std::size_t k) { return std::binary_search(in.begin(), in.end(), static_cast<std::uint32_t>(k)); };
    struct Side {
        std::size_t s, p;
        float z;
    };
    std::vector<Side> sides;
    for (const std::uint32_t p : cells)
        eachSide(g, p, [&](std::size_t m) {
            if ((g.flags[m] & kBack) && (g.flags[m] & kNode)) sides.push_back(Side{m, p, g.floor[m]});
        });
    std::sort(sides.begin(), sides.end(), [](const Side& a, const Side& b) { return a.z != b.z ? a.z < b.z : a.p < b.p; });
    const std::size_t tries = std::min<std::size_t>(sides.size(), 64);
    const std::size_t stride = std::max<std::size_t>(1, sides.size() / std::max<std::size_t>(tries, 1));
    for (std::size_t t = 0; t < sides.size(); t += stride) {
        const Side& sd = sides[t];
        const long long nx = static_cast<long long>(g.nx);
        const long long d = static_cast<long long>(sd.p) - static_cast<long long>(sd.s);
        const bool alongX = d == 1 || d == -1;
        long long c = static_cast<long long>(sd.p);
        double top = sd.z;
        std::vector<std::pair<long long, double>> steps;
        bool done = false;
        for (int k = 0; k < kStairStepsMax; ++k) {
            const long long i = c % nx, j = c / nx;
            if (c < 0 || j >= static_cast<long long>(g.ny) || i - (alongX ? 0 : 1) < 0 || i + (alongX ? 0 : 1) >= nx ||
                j - (alongX ? 1 : 0) < 0 || j + (alongX ? 1 : 0) >= static_cast<long long>(g.ny))
                break;
            const std::size_t uc = static_cast<std::size_t>(c);
            if (!inPocket(uc) && !steps.empty()) break; // left the pocket before reaching its floor
            const double f = g.floor[uc];
            if (!steps.empty() && f >= top - 1e-6) {
                // The lead stands here and steps up onto the last step (no more than `rise` over it).
                done = (g.flags[uc] & kStand) && (g.flags[uc] & kNode) && f <= top + rise + 1e-6;
                break;
            }
            steps.emplace_back(c, top);
            top -= rise;
            c += d;
        }
        if (!done) continue;
        for (const auto& [cell, z] : steps) {
            const std::size_t uc = static_cast<std::size_t>(cell);
            const double x = g.cx(uc % g.nx), y = g.cy(uc / g.nx);
            const double sxw = alongX ? g.cell : 3.0 * g.cell, syw = alongX ? 3.0 * g.cell : g.cell;
            double lo = z;
            for (int a = -1; a <= 1; ++a)
                for (int b = -1; b <= 1; ++b) lo = std::min(lo, groundAt(ground, x + 0.5 * a * sxw, y + 0.5 * b * syw));
            out.push_back(plinthBox("PL-X-" + std::to_string(++counter), x, y, sxw, syw, 0.0, lo - sink, z));
        }
        return true;
    }
    return false;
}

} // namespace

bool parsePocketRects(const std::string& text, std::vector<PocketRect>& out) {
    out.clear();
    std::size_t p = 0;
    while (p < text.size()) {
        std::size_t q = text.find(';', p);
        if (q == std::string::npos) q = text.size();
        double v[4];
        std::size_t at = p;
        for (int i = 0; i < 4; ++i) {
            const std::size_t comma = i < 3 ? text.find(',', at) : q;
            if (comma == std::string::npos || comma > q) return false;
            const std::string t = text.substr(at, comma - at);
            char* end = nullptr;
            v[i] = std::strtod(t.c_str(), &end);
            if (t.empty() || end != t.c_str() + t.size() || !std::isfinite(v[i])) return false;
            at = comma + 1;
        }
        out.push_back(PocketRect{std::min(v[0], v[2]), std::min(v[1], v[3]), std::max(v[0], v[2]), std::max(v[1], v[3])});
        p = q + 1;
    }
    return true;
}

bool pocketBuilding(const MassingBox& box) {
    if (!(box.kind == "kit" || box.kind == "box" || box.kind == "slab")) return false;
    if (!massingVisible(massingRole(box)) || box.sloped || box.riseU != 0.0 || box.riseW != 0.0) return false;
    return box.z1 - box.z0 >= kPocketBuildingHeight && box.sx > 0.0 && box.sy > 0.0;
}

PocketRect pocketRectOf(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                        double margin) {
    PocketRect r{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
                 std::numeric_limits<double>::lowest()};
    bool any = false;
    for (std::size_t i = 0; i < boxes.size() && i < include.size(); ++i) {
        if (!include[i] || !usable(boxes[i])) continue;
        const BoxFrame f = frameOf(boxes[i]);
        r.minX = std::min(r.minX, boxes[i].cx - f.hx);
        r.maxX = std::max(r.maxX, boxes[i].cx + f.hx);
        r.minY = std::min(r.minY, boxes[i].cy - f.hy);
        r.maxY = std::max(r.maxY, boxes[i].cy + f.hy);
        any = true;
    }
    if (!any) return PocketRect{};
    r.minX -= margin;
    r.minY -= margin;
    r.maxX += margin;
    r.maxY += margin;
    if (ground.grid && ground.samplesX > 1 && ground.samplesY > 1 && ground.spacing > 0.0) {
        r.minX = std::max(r.minX, ground.originX);
        r.minY = std::max(r.minY, ground.originY);
        r.maxX = std::min(r.maxX, ground.originX + static_cast<double>(ground.samplesX - 1) * ground.spacing);
        r.maxY = std::min(r.maxY, ground.originY + static_cast<double>(ground.samplesY - 1) * ground.spacing);
    }
    return r;
}

bool findPockets(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                 const PocketRect& rect, const std::vector<PocketPoint>& anchors, const PocketSpec& spec, PocketReport& out,
                 std::string* error) {
    Grid g;
    if (!buildGrid(ground, boxes, include, rect, spec, g, error)) return false;
    analyse(ground, boxes, spec, anchors, g, out);
    return true;
}

bool planPlinths(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                 const PocketRect& rect, const std::vector<PocketPoint>& anchors, const PlinthSpec& spec,
                 std::vector<MassingBox>& out, PlinthStats& stats, std::string* error) {
    stats = PlinthStats{};
    std::vector<MassingBox> all(boxes);
    std::vector<bool> inc(include);
    inc.resize(all.size(), false);
    Grid g;
    PocketReport report;
    if (!buildGrid(ground, all, inc, rect, spec.pocket, g, error)) return false;
    analyse(ground, all, spec.pocket, anchors, g, report);
    stats.pocketsBefore = report.pockets.size();
    stats.pocketCellsBefore = report.pocketCells;
    const std::size_t first = out.size();
    std::size_t skirtCounter = 0, fillCounter = 0, stairCounter = 0;
    if (spec.skirts) {
        BoxBuckets buckets;
        buckets.build(all, inc, rect);
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (!inc[i] || !pocketBuilding(all[i])) continue;
            bool sloped = false;
            skirtsOf(ground, all, i, buckets, spec, out, skirtCounter, sloped);
            stats.slopedBuildings += sloped ? 1 : 0;
        }
        stats.skirts = out.size() - first;
    }
    for (std::size_t i = first; i < out.size(); ++i) {
        all.push_back(out[i]);
        inc.push_back(true);
    }
    const std::size_t afterSkirts = out.size();
    for (int pass = 0; (spec.fills || spec.stairs) && pass < spec.fillPasses; ++pass) {
        if (!buildGrid(ground, all, inc, rect, spec.pocket, g, error)) return false;
        analyse(ground, all, spec.pocket, anchors, g, report);
        const std::size_t before = out.size();
        for (std::size_t p = 0; p < report.pockets.size(); ++p) {
            const Pocket& pk = report.pockets[p];
            if (!pk.hasSpill) continue;
            if (spec.fills && pk.width <= spec.pocket.narrowWidth + 1e-9 && pk.spill - pk.floorMin <= spec.fillMaxRise) {
                std::size_t made = 0;
                fillsOf(ground, g, g.groups[p], pk.spill, spec.sink, out, fillCounter, made);
                stats.filledPockets += made > 0 ? 1 : 0;
            } else if (spec.stairs) {
                const std::size_t was = out.size();
                if (stairOf(ground, g, g.groups[p], spec.stairRise, spec.sink, out, stairCounter)) {
                    stats.stairs += out.size() - was;
                    ++stats.stairedPockets;
                } else if (spec.fills && pk.spill - pk.floorMin <= spec.fillMaxRise) {
                    // No stair fits (too thin for its rise): filled whatever its width.
                    std::size_t made = 0;
                    fillsOf(ground, g, g.groups[p], pk.spill, spec.sink, out, fillCounter, made);
                    stats.filledPockets += made > 0 ? 1 : 0;
                    stats.wideFilled += made > 0 ? 1 : 0;
                }
            }
        }
        if (out.size() == before) break;
        ++stats.passes;
        for (std::size_t i = before; i < out.size(); ++i) {
            all.push_back(out[i]);
            inc.push_back(true);
        }
    }
    stats.fills = out.size() - afterSkirts - stats.stairs;
    if (!buildGrid(ground, all, inc, rect, spec.pocket, g, error)) return false;
    analyse(ground, all, spec.pocket, anchors, g, report);
    stats.pocketsAfter = report.pockets.size();
    stats.pocketCellsAfter = report.pocketCells;
    for (const Pocket& p : report.pockets) (p.width > spec.pocket.narrowWidth + 1e-9 ? stats.wideLeft : stats.narrowLeft) += 1;
    stats.after = std::move(report);
    return true;
}

} // namespace ftcore
