#include "ftcore/GarmentDetail.h"

#include <algorithm>
#include <cmath>

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
    }
    return s;
}

bool makeGarmentHeight(GarmentTile tile, const GarmentTileSettings& s, std::vector<float>& height) {
    height.clear();
    if (!isPowerOfTwo(s.size) || s.size < 32 || s.repeats == 0 || s.repeats > s.size / 4) {
        return false;
    }
    if (tile == GarmentTile::Twill && s.repeats % 3 != 0) {
        return false;  // the 2/1 repeat is three yarns: anything else leaves a seam at the tile edge
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
