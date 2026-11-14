#include "ftcore/GarmentDetail.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ftcore {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// A stateless hash to [0, 1): the same inputs always give the same value on every platform.
float hash01(uint32_t seed, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = seed * 0x9E3779B9u ^ a * 0x85EBCA6Bu ^ b * 0xC2B2AE35u ^ c * 0x27D4EB2Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) * (1.f / 16777216.f);
}

// Value noise with `period` lattice cells across the tile, so it wraps with the tile. u, v in [0, 1).
float tileNoise(uint32_t seed, uint32_t layer, uint32_t period, float u, float v) {
    const float x = u * static_cast<float>(period);
    const float y = v * static_cast<float>(period);
    const float fx = std::floor(x), fy = std::floor(y);
    const uint32_t x0 = static_cast<uint32_t>(fx) % period, y0 = static_cast<uint32_t>(fy) % period;
    const uint32_t x1 = (x0 + 1) % period, y1 = (y0 + 1) % period;
    float tx = x - fx, ty = y - fy;
    tx = tx * tx * (3.f - 2.f * tx);
    ty = ty * ty * (3.f - 2.f * ty);
    const float a = hash01(seed, layer, x0, y0), b = hash01(seed, layer, x1, y0);
    const float c = hash01(seed, layer, x0, y1), d = hash01(seed, layer, x1, y1);
    const float top = a + (b - a) * tx;
    return top + ((c + (d - c) * tx) - top) * ty;
}

float smoothstep01(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

bool isPowerOfTwo(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

// Jittered cell distances F1 / F2 (cell units) on an n x n cell grid that wraps; x, y in cell units.
void cellDistances(uint32_t seed, uint32_t layer, uint32_t n, float x, float y, float& f1, float& f2) {
    const int cx = static_cast<int>(std::floor(x)), cy = static_cast<int>(std::floor(y));
    const int ni = static_cast<int>(n);
    f1 = 9.f;
    f2 = 9.f;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int gx = cx + dx, gy = cy + dy;
            const uint32_t wx = static_cast<uint32_t>((gx % ni + ni) % ni);
            const uint32_t wy = static_cast<uint32_t>((gy % ni + ni) % ni);
            const float px = static_cast<float>(gx) + hash01(seed, layer, wx, wy);
            const float py = static_cast<float>(gy) + hash01(seed, layer + 1, wx, wy);
            const float d = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
            if (d < f1) {
                f2 = f1;
                f1 = d;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
}

// Pebbled grain: two octaves of fully jittered cells (so pebbles vary in size and shape), warped so the grooves
// wander, shallow rounded tops, a fine crinkle, and a few long thin creases.
float leatherAt(const GarmentTileSettings& s, float u, float v) {
    const uint32_t n = s.repeats;
    const uint32_t n2 = n * 2 + n / 3;  // second octave, not a multiple of the first
    const uint32_t warpPeriod = std::max(n / 3, 1u);
    const float wu = u + (tileNoise(s.seed, 6, warpPeriod, u, v) - 0.5f) * 0.9f / static_cast<float>(n);
    const float wv = v + (tileNoise(s.seed, 7, warpPeriod, u, v) - 0.5f) * 0.9f / static_cast<float>(n);
    float a1 = 0.f, a2 = 0.f, b1 = 0.f, b2 = 0.f;
    cellDistances(s.seed, 1, n, wu * static_cast<float>(n), wv * static_cast<float>(n), a1, a2);
    cellDistances(s.seed, 3, n2, wu * static_cast<float>(n2), wv * static_cast<float>(n2), b1, b2);
    const float big = smoothstep01(0.f, 0.25f, a2 - a1);    // grooves between the large pebbles
    const float fine = smoothstep01(0.f, 0.2f, b2 - b1);   // and the finer ones
    const float dome = 1.f - std::min(a1 * a1, 1.f);        // rounded tops
    const float crinkle = tileNoise(s.seed, 5, n * 4, u, v);
    const float creaseField = tileNoise(s.seed, 8, std::max(n / 5, 2u), wu, wv);
    const float crease = 1.f - smoothstep01(0.f, 0.025f, std::fabs(creaseField - 0.5f));
    return 0.45f * big + 0.25f * fine + 0.15f * dome + 0.15f * crinkle - 0.3f * crease;
}

// 2/1 twill: on a repeats x repeats yarn grid the warp floats over two picks and under one, stepping one
// yarn per pick, which makes the diagonal wale.
float twillAt(const GarmentTileSettings& s, float u, float v) {
    const uint32_t n = s.repeats;
    const float x = u * static_cast<float>(n), y = v * static_cast<float>(n);
    const uint32_t i = static_cast<uint32_t>(x) % n, j = static_cast<uint32_t>(y) % n;
    const float fx = x - std::floor(x), fy = y - std::floor(y);
    const bool warpUp = (i + j) % 3 != 0;
    // A yarn is round across its width and dips where it goes under the crossing yarn.
    const float across = warpUp ? std::sin(kPi * fx) : std::sin(kPi * fy);
    const float along = warpUp ? std::sin(kPi * fy) : std::sin(kPi * fx);
    const float yarn = std::sqrt(std::max(across, 0.f)) * (0.8f + 0.2f * along);
    const float slub = tileNoise(s.seed, 4, std::max(n / 6, 1u), u, v);  // uneven yarn thickness
    return (warpUp ? 1.f : 0.8f) * yarn * (0.9f + 0.1f * slub);
}

// Single jersey: each column is a chain of V-shaped loops, the two legs meeting at the bottom of the loop.
float knitAt(const GarmentTileSettings& s, float u, float v) {
    const uint32_t cols = s.repeats;
    const uint32_t rows = static_cast<uint32_t>(std::lround(static_cast<double>(cols) * 1.3));
    const float x = u * static_cast<float>(cols), y = v * static_cast<float>(rows);
    const float fx = x - std::floor(x), fy = y - std::floor(y);
    const float half = fx < 0.5f ? fx : 1.f - fx;  // mirror the right leg onto the left
    const float legCentre = 0.5f - 0.33f * (1.f - fy);
    const float d = (half - legCentre) / 0.13f;
    const float leg = std::exp(-d * d);
    const float bulge = 0.7f + 0.3f * std::sin(kPi * fy);  // loops sink where they pass through the row below
    const float fibre = tileNoise(s.seed, 5, cols * 4, u, v);
    return leg * bulge * (0.92f + 0.08f * fibre);
}

// --- Pore -------------------------------------------------------------------------------------------------
// Pits on the tile (a torus) as a Poisson-disk set: no two centres closer than kPoreMinSpacing of the mean
// spacing (tile / repeats). Darts first, then a fill pass over a fine jittered lattice, so the set is close to
// maximal and every pit has a neighbour within about 1.25 of the mean spacing. Sizes are in mean-spacing units,
// so the tile scales with `repeats`: radius 0.25 to 0.375 (0.2 to 0.3 mm at the intended 0.8 mm spacing), one pit
// in five 0.4 to 0.45 (0.32 to 0.36 mm), and any two pits shrunk together until their rims keep kPoreRimGap apart.
constexpr float kPoreMinSpacing = 0.75f;  // closest two centres may be, of the mean spacing (0.6 mm at 0.8)
constexpr float kPoreRimGap = 0.075f;     // skin kept between two rims, of the mean spacing
constexpr float kPoreLargeShare = 0.2f;   // one pit in five is larger
constexpr float kPoreWobbleMax = 0.15f;   // outline wobble at most this share of the radius

struct PoreSite {
    float x = 0.f, y = 0.f;  // tile units, 0..1
    float radius = 0.f;      // tile units, before the wobble
    float depth = 0.f;
    float a1 = 0.f, p1 = 0.f, a2 = 0.f, p2 = 0.f;  // outline r (1 + a1 sin(2 theta + p1) + a2 sin(3 theta + p2))
    bool large = false;
};

float wrapSigned(float d) { return d - std::round(d); }

// Buckets of site indices on a g x g grid that wraps.
struct PoreBuckets {
    uint32_t g = 1;
    std::vector<std::vector<uint32_t>> cells;
    explicit PoreBuckets(float cellSize)
        : g(std::max(1u, static_cast<uint32_t>(1.f / cellSize))), cells(static_cast<size_t>(g) * g) {}
    uint32_t cellOf(float c) const {
        const float w = c - std::floor(c);
        return std::min(static_cast<uint32_t>(w * static_cast<float>(g)), g - 1);
    }
    const std::vector<uint32_t>& at(int cx, int cy) const {
        const int gi = static_cast<int>(g);
        return cells[static_cast<size_t>((cy % gi + gi) % gi) * g + static_cast<size_t>((cx % gi + gi) % gi)];
    }
    void add(const PoreSite& p, uint32_t index) { cells[static_cast<size_t>(cellOf(p.y)) * g + cellOf(p.x)].push_back(index); }
};

std::vector<PoreSite> poreSites(const GarmentTileSettings& s, PoreBuckets& buckets) {
    const float fn = static_cast<float>(s.repeats);
    const float dmin = kPoreMinSpacing / fn;
    std::vector<PoreSite> sites;
    const auto fits = [&](float x, float y) {
        const int cx = static_cast<int>(buckets.cellOf(x)), cy = static_cast<int>(buckets.cellOf(y));
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                for (uint32_t k : buckets.at(cx + dx, cy + dy)) {
                    const float ex = wrapSigned(x - sites[k].x), ey = wrapSigned(y - sites[k].y);
                    if (ex * ex + ey * ey < dmin * dmin) {
                        return false;
                    }
                }
            }
        }
        return true;
    };
    const auto place = [&](float x, float y) {
        if (!fits(x, y)) {
            return;
        }
        PoreSite p;
        p.x = x;
        p.y = y;
        buckets.add(p, static_cast<uint32_t>(sites.size()));
        sites.push_back(p);
    };
    const uint32_t darts = 40u * s.repeats * s.repeats;
    for (uint32_t k = 0; k < darts; ++k) {
        place(hash01(s.seed, 101, k, 0), hash01(s.seed, 102, k, 0));
    }
    const uint32_t lattice = 8u * s.repeats;  // the fill pass: every gap that still holds a pit gets one
    for (uint32_t j = 0; j < lattice; ++j) {
        for (uint32_t i = 0; i < lattice; ++i) {
            place((static_cast<float>(i) + hash01(s.seed, 103, i, j)) / static_cast<float>(lattice),
                  (static_cast<float>(j) + hash01(s.seed, 104, i, j)) / static_cast<float>(lattice));
        }
    }
    for (uint32_t k = 0; k < sites.size(); ++k) {
        PoreSite& p = sites[k];
        p.large = hash01(s.seed, 105, k, 0) < kPoreLargeShare;
        const float r = hash01(s.seed, 106, k, 0);
        p.radius = (p.large ? 0.4f + 0.05f * r : 0.25f + 0.125f * r) / fn;
        p.depth = p.large ? 0.85f + 0.15f * hash01(s.seed, 107, k, 0) : 0.6f + 0.3f * hash01(s.seed, 107, k, 0);
        p.a1 = 0.6f * kPoreWobbleMax * hash01(s.seed, 108, k, 0);
        p.a2 = 0.4f * kPoreWobbleMax * hash01(s.seed, 109, k, 0);
        p.p1 = 2.f * kPi * hash01(s.seed, 110, k, 0);
        p.p2 = 2.f * kPi * hash01(s.seed, 111, k, 0);
    }
    // Any two pits whose outlines (at their widest) would come within the rim gap shrink together, in proportion,
    // so the large ones stay larger.
    const float gap = kPoreRimGap / fn;
    for (uint32_t k = 0; k < sites.size(); ++k) {
        const int cx = static_cast<int>(buckets.cellOf(sites[k].x)), cy = static_cast<int>(buckets.cellOf(sites[k].y));
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                for (uint32_t m : buckets.at(cx + dx, cy + dy)) {
                    if (m <= k) {
                        continue;
                    }
                    PoreSite& p = sites[k];
                    PoreSite& q = sites[m];
                    const float ex = wrapSigned(p.x - q.x), ey = wrapSigned(p.y - q.y);
                    const float d = std::sqrt(ex * ex + ey * ey);
                    const float rp = p.radius * (1.f + p.a1 + p.a2), rq = q.radius * (1.f + q.a1 + q.a2);
                    if (rp + rq + gap > d) {
                        const float scale = std::max(d - gap, 0.f) / (rp + rq);
                        p.radius *= scale;
                        q.radius *= scale;
                    }
                }
            }
        }
    }
    return sites;
}

// Height at (u, v): 1 on the skin, which domes a little toward the middle of each patch between pits (the
// pebbles), less a paraboloid bowl in each pit, with a hard rim so the pit's outline reads.
float poreAt(const GarmentTileSettings& s, const std::vector<PoreSite>& sites, const PoreBuckets& buckets, float u, float v) {
    const float fn = static_cast<float>(s.repeats);
    const uint32_t warp = std::max(s.repeats / 2, 1u);
    const float x = u + 0.04f / fn * (tileNoise(s.seed, 112, warp, u, v) - 0.5f);
    const float y = v + 0.04f / fn * (tileNoise(s.seed, 113, warp, u, v) - 0.5f);
    const int cx = static_cast<int>(buckets.cellOf(x)), cy = static_cast<int>(buckets.cellOf(y));
    float pit = 0.f;
    float edge = 9.f;  // distance to the nearest rim, mean-spacing units (negative inside a pit)
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            for (uint32_t k : buckets.at(cx + dx, cy + dy)) {
                const PoreSite& p = sites[k];
                const float ex = wrapSigned(x - p.x), ey = wrapSigned(y - p.y);
                const float dist = std::sqrt(ex * ex + ey * ey);
                if (dist > 1.2f / fn || p.radius <= 0.f) {
                    continue;
                }
                const float theta = std::atan2(ey, ex);
                const float r = p.radius * (1.f + p.a1 * std::sin(2.f * theta + p.p1) + p.a2 * std::sin(3.f * theta + p.p2));
                edge = std::min(edge, (dist - r) * fn);
                const float t = dist / r;
                if (t < 1.f) {
                    pit = std::max(pit, p.depth * (1.f - t * t));
                }
            }
        }
    }
    const float dome = 0.15f * smoothstep01(0.f, 0.3f, edge);
    const float fine = 0.012f * (tileNoise(s.seed, 120, s.repeats * 3, u, v) - 0.5f);
    return 1.f + dome + fine - 0.85f * pit;
}

// --- Crease -----------------------------------------------------------------------------------------------
// Short fold creases, `repeats` of them on the tile, each 5 to 30 mm long (more short than long), its direction
// wandering within +-30 degrees of the tile's u axis, about a third of them with one branch splitting off at 25
// to 40 degrees. Grooves 0.3 to 0.6 mm wide and tapered at both ends. Each crease has a rank in [0, 1) that the
// master compares against the fold map: a crease shows where the fold density is above its rank.
constexpr float kCreaseTileMm = 60.f;  // the intended tile: 6 cm
constexpr float kCreaseStepMm = 0.3f;
constexpr float kCreaseWander = 30.f * kPi / 180.f;
constexpr float kCreaseBranchShare = 0.35f;

struct CreaseSeg {
    float ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f;  // mm, unwrapped
    float wa = 0.f, wb = 0.f;                      // half-width at each end, mm
    float da = 0.f, db = 0.f;                      // depth at each end
    float rank = 0.f;
};

struct CreaseWalk {
    float x = 0.f, y = 0.f, theta = 0.f, length = 0.f, depth = 0.f, width = 0.f, rank = 0.f;
    float centre = 0.f, range = 0.f;  // the direction stays within centre +- range
};

// Lays out one crease as segments; maxAngle gets the largest turn from the u axis (degrees).
void walkCrease(const GarmentTileSettings& s, const CreaseWalk& w, uint32_t id, uint32_t layer, std::vector<CreaseSeg>& out,
                float& maxAngle) {
    const uint32_t steps = std::max(2u, static_cast<uint32_t>(std::ceil(w.length / kCreaseStepMm)));
    const float step = w.length / static_cast<float>(steps);
    float x = w.x, y = w.y, theta = w.theta;
    const auto envelope = [](float t) { return std::sqrt(std::max(std::sin(kPi * t), 0.f)); };
    for (uint32_t k = 0; k < steps; ++k) {
        theta += (hash01(s.seed, layer, id, k) - 0.5f) * 0.3f;
        theta = std::clamp(theta, w.centre - w.range, w.centre + w.range);
        maxAngle = std::max(maxAngle, std::fabs(theta) * 180.f / kPi);
        const float t0 = static_cast<float>(k) / static_cast<float>(steps);
        const float t1 = static_cast<float>(k + 1) / static_cast<float>(steps);
        CreaseSeg g;
        g.ax = x;
        g.ay = y;
        x += std::cos(theta) * step;
        y += std::sin(theta) * step;
        g.bx = x;
        g.by = y;
        g.wa = w.width * (0.35f + 0.65f * envelope(t0));
        g.wb = w.width * (0.35f + 0.65f * envelope(t1));
        g.da = w.depth * envelope(t0);
        g.db = w.depth * envelope(t1);
        g.rank = w.rank;
        out.push_back(g);
    }
}

void creaseSegments(const GarmentTileSettings& s, std::vector<CreaseSeg>& segs, std::vector<GarmentCrease>* info) {
    segs.clear();
    for (uint32_t c = 0; c < s.repeats; ++c) {
        CreaseWalk w;
        w.rank = hash01(s.seed, 400, c, 0);
        w.x = hash01(s.seed, 401, c, 0) * kCreaseTileMm;
        w.y = hash01(s.seed, 402, c, 0) * kCreaseTileMm;
        const float l = hash01(s.seed, 403, c, 0);
        w.length = 5.f + 25.f * l * l;
        w.theta = (hash01(s.seed, 404, c, 0) - 0.5f) * (40.f * kPi / 180.f);
        w.depth = 0.5f + 0.5f * hash01(s.seed, 405, c, 0);
        w.width = 0.15f + 0.15f * hash01(s.seed, 406, c, 0);
        w.range = kCreaseWander;
        float angle = 0.f;
        const size_t first = segs.size();
        walkCrease(s, w, c, 420, segs, angle);
        if (info) {
            info->push_back({w.length, angle, false});
        }
        if (hash01(s.seed, 407, c, 0) >= kCreaseBranchShare) {
            continue;
        }
        // The branch leaves from a point 30 to 70 % along, at 25 to 40 degrees off the crease there.
        const size_t n = segs.size() - first;
        const CreaseSeg at = segs[first + static_cast<size_t>((0.3f + 0.4f * hash01(s.seed, 408, c, 0)) * static_cast<float>(n - 1))];
        CreaseWalk b = w;
        b.x = at.bx;
        b.y = at.by;
        const float side = hash01(s.seed, 409, c, 0) < 0.5f ? -1.f : 1.f;
        b.theta = std::atan2(at.by - at.ay, at.bx - at.ax) + side * (25.f + 15.f * hash01(s.seed, 410, c, 0)) * kPi / 180.f;
        b.centre = b.theta;
        b.range = 15.f * kPi / 180.f;
        b.length = w.length * (0.3f + 0.3f * hash01(s.seed, 411, c, 0));
        b.depth = 0.7f * w.depth;
        b.width = 0.8f * w.width;
        float branchAngle = 0.f;
        walkCrease(s, b, c, 430, segs, branchAngle);
        if (info) {
            info->push_back({b.length, branchAngle, true});
        }
    }
}

// The groove profile, t = distance / half-width: deepest on the line, soft at its edges.
float groove(float t) { return t < 1.f ? (1.f - t * t) * (1.f - t * t) : 0.f; }

// Stamps every segment into cut (0..1, deepest wins) and rank: the rank of the deepest crease inside a groove, and
// around it, out to twice its half-width plus two pixels, the rank of the nearest crease (so bilinear filtering and the
// smaller mips keep a crease's rank on its soft edges instead of fading it toward 1); 1 further out.
void stampCreases(const GarmentTileSettings& s, const std::vector<CreaseSeg>& segs, std::vector<float>& cut, std::vector<float>& rank) {
    const uint32_t n = s.size;
    const int ni = static_cast<int>(n);
    const float mmPerPx = kCreaseTileMm / static_cast<float>(n);
    const float minHalf = 0.75f * mmPerPx;  // a groove always covers a pixel, at any tile size
    cut.assign(static_cast<size_t>(n) * n, 0.f);
    rank.assign(static_cast<size_t>(n) * n, 1.f);
    std::vector<float> nearest(static_cast<size_t>(n) * n, 1e9f);  // mm to the nearest crease line, outside every groove
    for (const CreaseSeg& g : segs) {
        const float reach = 2.f * std::max(std::max(g.wa, g.wb), minHalf) + 2.f * mmPerPx;
        const int x0 = static_cast<int>(std::floor((std::min(g.ax, g.bx) - reach) / mmPerPx - 0.5f));
        const int x1 = static_cast<int>(std::ceil((std::max(g.ax, g.bx) + reach) / mmPerPx - 0.5f));
        const int y0 = static_cast<int>(std::floor((std::min(g.ay, g.by) - reach) / mmPerPx - 0.5f));
        const int y1 = static_cast<int>(std::ceil((std::max(g.ay, g.by) + reach) / mmPerPx - 0.5f));
        const float sx = g.bx - g.ax, sy = g.by - g.ay;
        const float ss = std::max(sx * sx + sy * sy, 1e-8f);
        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                const float mx = (static_cast<float>(px) + 0.5f) * mmPerPx, my = (static_cast<float>(py) + 0.5f) * mmPerPx;
                const float t = std::clamp(((mx - g.ax) * sx + (my - g.ay) * sy) / ss, 0.f, 1.f);
                const float ex = mx - (g.ax + sx * t), ey = my - (g.ay + sy * t);
                const float half = std::max(g.wa + (g.wb - g.wa) * t, minHalf);
                const float e = std::sqrt(ex * ex + ey * ey);
                const float c = (g.da + (g.db - g.da) * t) * groove(e / half);
                const size_t i = static_cast<size_t>((py % ni + ni) % ni) * n + static_cast<size_t>((px % ni + ni) % ni);
                if (c > cut[i]) {
                    cut[i] = c;
                    rank[i] = g.rank;
                } else if (cut[i] <= 0.f && e <= 2.f * half + 2.f * mmPerPx && e < nearest[i]) {
                    nearest[i] = e;
                    rank[i] = g.rank;
                }
            }
        }
    }
}

bool validSettings(GarmentTile tile, const GarmentTileSettings& s) {
    if (!isPowerOfTwo(s.size) || s.size < 32 || s.repeats == 0 || s.repeats > s.size / 4) {
        return false;
    }
    return tile != GarmentTile::Twill || s.repeats % 3 == 0;  // the 2/1 repeat is three yarns: else a seam at the edge
}

} // namespace

GarmentTileSettings garmentTileDefaults(GarmentTile tile) {
    GarmentTileSettings s;
    switch (tile) {
    case GarmentTile::Leather:
        s.repeats = 22;
        s.normalStrength = 5.f;
        s.seed = 11;
        break;
    case GarmentTile::Twill:
        s.repeats = 48;
        s.normalStrength = 3.f;
        s.seed = 12;
        break;
    case GarmentTile::Knit:
        s.repeats = 32;
        s.normalStrength = 3.f;
        s.seed = 13;
        break;
    case GarmentTile::Pore:
        s.repeats = 20;  // 0.8 mm mean spacing on the 1.6 cm tile
        s.normalStrength = 5.f;
        s.seed = 14;
        break;
    case GarmentTile::Crease:
        s.size = 1024;    // 0.06 mm a pixel: the narrowest groove still spans several
        s.repeats = 110;  // creases on the 6 cm tile
        s.normalStrength = 2.f;
        s.seed = 15;
        break;
    }
    return s;
}

float garmentTileCentimetres(GarmentTile tile) {
    switch (tile) {
    case GarmentTile::Pore:
        return 1.6f;
    case GarmentTile::Crease:
        return kCreaseTileMm * 0.1f;
    case GarmentTile::Leather:
    case GarmentTile::Twill:
    case GarmentTile::Knit:
        break;
    }
    return 0.f;
}

bool garmentPores(const GarmentTileSettings& s, std::vector<GarmentPore>& pores) {
    pores.clear();
    if (!validSettings(GarmentTile::Pore, s)) {
        return false;
    }
    PoreBuckets buckets(kPoreMinSpacing / static_cast<float>(s.repeats));
    const float mm = garmentTileCentimetres(GarmentTile::Pore) * 10.f;
    for (const PoreSite& p : poreSites(s, buckets)) {
        pores.push_back({p.x * mm, p.y * mm, p.radius * mm, p.large});
    }
    return true;
}

bool garmentCreases(const GarmentTileSettings& s, std::vector<GarmentCrease>& creases) {
    creases.clear();
    if (!validSettings(GarmentTile::Crease, s)) {
        return false;
    }
    std::vector<CreaseSeg> segs;
    creaseSegments(s, segs, &creases);
    return true;
}

bool garmentCreaseRank(const GarmentTileSettings& s, std::vector<float>& rank) {
    rank.clear();
    if (!validSettings(GarmentTile::Crease, s)) {
        return false;
    }
    std::vector<CreaseSeg> segs;
    std::vector<float> cut;
    creaseSegments(s, segs, nullptr);
    stampCreases(s, segs, cut, rank);
    return true;
}

bool makeGarmentHeight(GarmentTile tile, const GarmentTileSettings& s, std::vector<float>& height) {
    height.clear();
    if (!validSettings(tile, s)) {
        return false;
    }
    const uint32_t n = s.size;
    if (tile == GarmentTile::Crease) {
        std::vector<CreaseSeg> segs;
        std::vector<float> rank;
        creaseSegments(s, segs, nullptr);
        stampCreases(s, segs, height, rank);
        float lo = 1e9f, hi = -1e9f;
        for (size_t i = 0; i < height.size(); ++i) {
            const uint32_t x = static_cast<uint32_t>(i % n), y = static_cast<uint32_t>(i / n);
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(n), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(n);
            height[i] = 1.f - height[i] + 0.02f * (tileNoise(s.seed, 310, 24, u, v) - 0.5f);
            lo = std::min(lo, height[i]);
            hi = std::max(hi, height[i]);
        }
        const float scale = hi > lo ? 1.f / (hi - lo) : 0.f;
        for (float& h : height) {
            h = (h - lo) * scale;
        }
        return true;
    }
    std::vector<PoreSite> sites;
    PoreBuckets buckets(tile == GarmentTile::Pore ? kPoreMinSpacing / static_cast<float>(s.repeats) : 1.f);
    if (tile == GarmentTile::Pore) {
        sites = poreSites(s, buckets);
    }
    const float inv = 1.f / static_cast<float>(n);
    height.assign(static_cast<size_t>(n) * n, 0.f);
    float lo = 1e9f, hi = -1e9f;
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            // Sample at pixel centres so the tile is symmetric about its edges.
            const float u = (static_cast<float>(x) + 0.5f) * inv, v = (static_cast<float>(y) + 0.5f) * inv;
            float h = 0.f;
            switch (tile) {
            case GarmentTile::Leather:
                h = leatherAt(s, u, v);
                break;
            case GarmentTile::Twill:
                h = twillAt(s, u, v);
                break;
            case GarmentTile::Knit:
                h = knitAt(s, u, v);
                break;
            case GarmentTile::Pore:
                h = poreAt(s, sites, buckets, u, v);
                break;
            case GarmentTile::Crease:
                break;
            }
            height[static_cast<size_t>(y) * n + x] = h;
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
    const float scale = hi > lo ? 1.f / (hi - lo) : 0.f;
    for (float& h : height) {
        h = (h - lo) * scale;
    }
    return true;
}

} // namespace ftcore
