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
// Pit sizes in cell units, for the intended tile (1.6 cm, 24 cells: 0.667 mm a cell): radius 0.225 to 0.45 is a
// pit of 0.3 to 0.6 mm across.
constexpr float kPoreRadiusMin = 0.225f;
constexpr float kPoreRadiusMax = 0.45f;
constexpr float kPoreNeighbourShare = 0.38f;  // a radius reaches at most this share of the way to the nearest pit
constexpr float kPoreJitter = 0.7f;           // site offset range across a cell (+-0.35)
constexpr float kPorePresence = 0.84f;        // share of cells with a pit

struct PoreSite {
    float x = 0.f, y = 0.f;  // offset inside its cell, 0..1
    float radius = 0.f;      // cell units; 0 = no pit in this cell
    float depth = 0.f;
    float cosA = 1.f, sinA = 0.f, stretch = 1.f;  // the outline: an ellipse turned by A, axes r * stretch and r / stretch
};

// One site per cell of an n x n grid that wraps; radii trimmed so no pit reaches a neighbour.
std::vector<PoreSite> poreSites(const GarmentTileSettings& s) {
    const uint32_t n = s.repeats;
    std::vector<PoreSite> sites(static_cast<size_t>(n) * n);
    for (uint32_t j = 0; j < n; ++j) {
        for (uint32_t i = 0; i < n; ++i) {
            PoreSite& p = sites[static_cast<size_t>(j) * n + i];
            p.x = 0.5f + (hash01(s.seed, 101, i, j) - 0.5f) * kPoreJitter;
            p.y = 0.5f + (hash01(s.seed, 102, i, j) - 0.5f) * kPoreJitter;
            if (hash01(s.seed, 103, i, j) >= kPorePresence) {
                continue;
            }
            const float r = hash01(s.seed, 104, i, j);
            p.radius = kPoreRadiusMin + (kPoreRadiusMax - kPoreRadiusMin) * r * r;  // more small pits than big ones
            p.depth = 0.6f + 0.4f * hash01(s.seed, 105, i, j);
            const float a = kPi * hash01(s.seed, 106, i, j);
            p.cosA = std::cos(a);
            p.sinA = std::sin(a);
            p.stretch = 1.f + 0.2f * hash01(s.seed, 107, i, j);
        }
    }
    const int ni = static_cast<int>(n);
    std::vector<float> trimmed(sites.size(), 0.f);
    for (int j = 0; j < ni; ++j) {
        for (int i = 0; i < ni; ++i) {
            const PoreSite& p = sites[static_cast<size_t>(j) * n + static_cast<size_t>(i)];
            if (p.radius <= 0.f) {
                continue;
            }
            float nearest = 9.f;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    const int gx = i + dx, gy = j + dy;
                    const PoreSite& q = sites[static_cast<size_t>((gy % ni + ni) % ni) * n + static_cast<size_t>((gx % ni + ni) % ni)];
                    if (q.radius <= 0.f) {
                        continue;
                    }
                    const float ex = static_cast<float>(dx) + q.x - p.x, ey = static_cast<float>(dy) + q.y - p.y;
                    nearest = std::min(nearest, std::sqrt(ex * ex + ey * ey));
                }
            }
            // The outline's long axis is r * stretch: trim that, not r.
            const float r = std::min(p.radius, kPoreNeighbourShare * nearest / p.stretch);
            trimmed[static_cast<size_t>(j) * n + static_cast<size_t>(i)] = r < 0.8f * kPoreRadiusMin ? 0.f : r;
        }
    }
    for (size_t k = 0; k < sites.size(); ++k) {
        sites[k].radius = trimmed[k];  // trimmed against the untrimmed set, so a dropped pit only frees room
    }
    return sites;
}

float poreAt(const GarmentTileSettings& s, const std::vector<PoreSite>& sites, float u, float v) {
    const uint32_t n = s.repeats;
    const float fn = static_cast<float>(n);
    // Warp the lookup (periodic noise, so the tile still wraps): pits drift off the grid and their outlines go
    // irregular, a coarse sway plus a finer wobble.
    const uint32_t coarse = std::max(n / 4, 1u), fine = std::max(n / 2, 1u);
    const float x = u * fn + 0.5f * (tileNoise(s.seed, 110, coarse, u, v) - 0.5f) + 0.25f * (tileNoise(s.seed, 112, fine, u, v) - 0.5f);
    const float y = v * fn + 0.5f * (tileNoise(s.seed, 111, coarse, u, v) - 0.5f) + 0.25f * (tileNoise(s.seed, 113, fine, u, v) - 0.5f);
    const int cx = static_cast<int>(std::floor(x)), cy = static_cast<int>(std::floor(y));
    const int ni = static_cast<int>(n);
    float pit = 0.f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int gx = cx + dx, gy = cy + dy;
            const PoreSite& p = sites[static_cast<size_t>((gy % ni + ni) % ni) * n + static_cast<size_t>((gx % ni + ni) % ni)];
            if (p.radius <= 0.f) {
                continue;
            }
            const float ex = x - (static_cast<float>(gx) + p.x), ey = y - (static_cast<float>(gy) + p.y);
            const float a = (ex * p.cosA + ey * p.sinA) / (p.radius * p.stretch);
            const float b = (ey * p.cosA - ex * p.sinA) * p.stretch / p.radius;
            const float t2 = a * a + b * b;
            if (t2 < 1.f) {
                const float bowl = (1.f - t2) * (1.f - t2);  // soft at the rim, rounded at the floor
                pit = std::max(pit, p.depth * bowl);
            }
        }
    }
    // The skin between: a very faint, broad undulation and a finer one.
    const float skin = 0.05f * (tileNoise(s.seed, 120, std::max(n / 8, 1u), u, v) - 0.5f) +
                       0.03f * (tileNoise(s.seed, 121, std::max(n / 2, 1u), u, v) - 0.5f) +
                       0.015f * (tileNoise(s.seed, 122, n * 3, u, v) - 0.5f);
    return 1.f + skin - pit;
}

// --- Crease -----------------------------------------------------------------------------------------------
constexpr float kCreaseTileMm = 24.f;  // the intended tile: 2.4 cm

struct CreaseLine {
    float u = 0.f;  // where the line crosses the tile, 0..1
};
struct CreaseLink {
    uint32_t line = 0;             // runs from this line to the next one (wrapping)
    float v0 = 0.f, length = 0.f;  // start and extent along v, tile units
    float depth = 0.f;
};

void creaseLayout(const GarmentTileSettings& s, std::vector<CreaseLine>& lines, std::vector<CreaseLink>& links) {
    const uint32_t k = s.repeats;
    // Gaps of (1 + d) / k with the d in +- pairs, shuffled: each gap between 0.7 and 1.3 of the mean (2.1 to 3.9 mm
    // at 8 lines on 2.4 cm) and the sum exactly one tile, so the gap that closes around the tile edge is in range too.
    std::vector<float> d(k, 0.f);
    for (uint32_t i = 0; i + 1 < k; i += 2) {
        const float e = 0.3f * hash01(s.seed, 200, i, 0);
        d[i] = e;
        d[i + 1] = -e;
    }
    for (uint32_t i = k; i > 1; --i) {
        const uint32_t j = static_cast<uint32_t>(hash01(s.seed, 201, i, 0) * static_cast<float>(i)) % i;
        std::swap(d[i - 1], d[j]);
    }
    lines.assign(k, CreaseLine{});
    float u = hash01(s.seed, 202, 0, 0);
    for (uint32_t i = 0; i < k; ++i) {
        lines[i].u = u - std::floor(u);
        u += (1.f + d[i]) / static_cast<float>(k);
    }
    // About one connector per line: from line i at v0 to line i + 1, 6 to 10 mm further along v (a shallow angle,
    // about 15 to 30 degrees off the lines).
    links.clear();
    for (uint32_t i = 0; i < k; ++i) {
        for (uint32_t c = 0; c < 2; ++c) {
            if (hash01(s.seed, 203, i, c) >= 0.5f) {
                continue;
            }
            CreaseLink l;
            l.line = i;
            l.v0 = hash01(s.seed, 204, i, c);
            l.length = (6.f + 4.f * hash01(s.seed, 205, i, c)) / kCreaseTileMm;
            l.depth = 0.35f + 0.3f * hash01(s.seed, 206, i, c);
            links.push_back(l);
        }
    }
}

// Where line i sits at v (tile units): its crossing plus a slow wander of up to about +-0.6 mm and a fine wobble
// of about +-0.06 mm, periodic in v.
float creaseLineU(const GarmentTileSettings& s, const CreaseLine& line, uint32_t i, float v) {
    const float w = 0.7f * (tileNoise(s.seed, 210 + i, 3, line.u, v) - 0.5f) + 0.3f * (tileNoise(s.seed, 230 + i, 8, line.u, v) - 0.5f);
    const float wobble = tileNoise(s.seed, 240 + i, 24, line.u, v) - 0.5f;
    return line.u + (1.2f * w + 0.12f * wobble) / kCreaseTileMm;
}

float wrapSigned(float d) { return d - std::round(d); }

// The groove profile, t = distance / extent: deepest on the line, soft at its edges.
float groove(float t) { return t < 1.f ? (1.f - t * t) * (1.f - t * t) : 0.f; }

float creaseAt(const GarmentTileSettings& s, const std::vector<CreaseLine>& lines, const std::vector<CreaseLink>& links,
               float u, float v) {
    const uint32_t k = s.repeats;
    float cut = 0.f;
    for (uint32_t i = 0; i < k; ++i) {
        const float du = wrapSigned(u - creaseLineU(s, lines[i], i, v)) * kCreaseTileMm;  // mm
        const float extent = 0.15f + 0.1f * tileNoise(s.seed, 250 + i, 6, lines[i].u, v); // 0.15 to 0.25 mm
        if (std::fabs(du) >= extent) {
            continue;
        }
        const float present = smoothstep01(0.4f, 0.52f, tileNoise(s.seed, 270 + i, 5, lines[i].u, v));  // the gaps
        const float depth = 0.3f + 0.7f * tileNoise(s.seed, 290 + i, 7, lines[i].u, v);
        cut = std::max(cut, depth * present * groove(std::fabs(du) / extent));
    }
    for (const CreaseLink& l : links) {
        const uint32_t next = (l.line + 1) % k;
        const float v1 = l.v0 + l.length;
        // The two ends in mm, the far one unwrapped so the segment is the short way across.
        const float ax = creaseLineU(s, lines[l.line], l.line, l.v0) * kCreaseTileMm, ay = l.v0 * kCreaseTileMm;
        float bx = creaseLineU(s, lines[next], next, v1 - std::floor(v1)) * kCreaseTileMm;
        bx = ax + wrapSigned((bx - ax) / kCreaseTileMm) * kCreaseTileMm;
        const float by = v1 * kCreaseTileMm;
        // The pixel's copy nearest the segment's middle.
        const float mx = 0.5f * (ax + bx), my = 0.5f * (ay + by);
        const float px = mx + wrapSigned((u * kCreaseTileMm - mx) / kCreaseTileMm) * kCreaseTileMm;
        const float py = my + wrapSigned((v * kCreaseTileMm - my) / kCreaseTileMm) * kCreaseTileMm;
        const float sx = bx - ax, sy = by - ay;
        const float t = std::clamp(((px - ax) * sx + (py - ay) * sy) / (sx * sx + sy * sy), 0.f, 1.f);
        const float ex = px - (ax + sx * t), ey = py - (ay + sy * t);
        const float dist = std::sqrt(ex * ex + ey * ey);
        constexpr float kLinkExtent = 0.13f;
        if (dist < kLinkExtent) {
            cut = std::max(cut, l.depth * std::sin(kPi * t) * groove(dist / kLinkExtent));  // fades into both lines
        }
    }
    const float skin = 0.02f * (tileNoise(s.seed, 310, std::max(k / 2, 1u), u, v) - 0.5f);
    return 1.f + skin - cut;
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
        s.repeats = 24;
        s.normalStrength = 4.f;
        s.seed = 14;
        break;
    case GarmentTile::Crease:
        s.repeats = 8;
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

bool makeGarmentHeight(GarmentTile tile, const GarmentTileSettings& s, std::vector<float>& height) {
    height.clear();
    if (!isPowerOfTwo(s.size) || s.size < 32 || s.repeats == 0 || s.repeats > s.size / 4) {
        return false;
    }
    if (tile == GarmentTile::Twill && s.repeats % 3 != 0) {
        return false;  // the 2/1 repeat is three yarns: anything else leaves a seam at the tile edge
    }
    if (tile == GarmentTile::Crease && s.repeats % 2 != 0) {
        return false;  // the line gaps come in +- pairs so they add up to one tile
    }
    std::vector<PoreSite> sites;
    std::vector<CreaseLine> lines;
    std::vector<CreaseLink> links;
    if (tile == GarmentTile::Pore) {
        sites = poreSites(s);
    } else if (tile == GarmentTile::Crease) {
        creaseLayout(s, lines, links);
    }
    const uint32_t n = s.size;
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
                h = poreAt(s, sites, u, v);
                break;
            case GarmentTile::Crease:
                h = creaseAt(s, lines, links, u, v);
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
