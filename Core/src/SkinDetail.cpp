#include "ftcore/SkinDetail.h"

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

// Value noise with period `period` lattice cells across the tile, so it wraps with the tile. u, v in [0, 1).
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
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

bool isPowerOfTwo(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

uint8_t toByte(float unit) { return static_cast<uint8_t>(std::clamp(unit * 255.f + 0.5f, 0.f, 255.f)); }

} // namespace

bool makeSkinHeight(const SkinDetailSettings& s, std::vector<float>& height) {
    height.clear();
    if (!isPowerOfTwo(s.size) || s.size < 16 || s.poreCells == 0 || s.poreCells > s.size / 4 || s.poreRadius <= 0.f ||
        s.poreRadius > 0.5f || s.poreDepth < 0.f || s.grooveDepth < 0.f) {
        return false;
    }
    const uint32_t n = s.size;
    const float inv = 1.f / static_cast<float>(n);
    height.assign(static_cast<size_t>(n) * n, 1.f);

    // The groove network: thin lines along four directions whose line counts per tile are whole numbers, so each
    // wraps. A slow noise bends the lines and another breaks them into short runs, so no grid shows.
    struct Dir {
        int kx, ky;
        float lines;
    };
    const float base = static_cast<float>(s.poreCells);
    const Dir dirs[] = {{1, 0, std::round(base * 0.9f)}, {0, 1, std::round(base * 0.8f)},
                        {1, 1, std::round(base * 0.55f)}, {1, -1, std::round(base * 0.6f)}};
    for (uint32_t y = 0; y < n; ++y) {
        const float v = static_cast<float>(y) * inv;
        for (uint32_t x = 0; x < n; ++x) {
            const float u = static_cast<float>(x) * inv;
            float groove = 0.f;
            for (uint32_t d = 0; d < 4; ++d) {
                const Dir& dir = dirs[d];
                const float warp = 0.9f * (tileNoise(s.seed, 10 + d, 8, u, v) - 0.5f);
                const float phase = dir.lines * (static_cast<float>(dir.kx) * u + static_cast<float>(dir.ky) * v) + warp;
                const float line = std::sin(kPi * phase);
                const float run = std::clamp((tileNoise(s.seed, 20 + d, 16, u, v) - 0.35f) * 2.5f, 0.f, 1.f);
                groove = std::max(groove, std::exp(-line * line * 60.f) * run);
            }
            height[static_cast<size_t>(y) * n + x] -= s.grooveDepth * groove;
        }
    }

    // Pores: one per cell at a jittered spot, a few cells left bare, each pit a smooth cup.
    const float cell = static_cast<float>(n) / base;
    const int ni = static_cast<int>(n);
    for (uint32_t cy = 0; cy < s.poreCells; ++cy) {
        for (uint32_t cx = 0; cx < s.poreCells; ++cx) {
            if (hash01(s.seed, 1, cx, cy) < 0.12f) {
                continue;
            }
            const float px = (static_cast<float>(cx) + 0.15f + 0.7f * hash01(s.seed, 2, cx, cy)) * cell;
            const float py = (static_cast<float>(cy) + 0.15f + 0.7f * hash01(s.seed, 3, cx, cy)) * cell;
            const float radius = std::max(1.5f, s.poreRadius * cell * (0.6f + 0.8f * hash01(s.seed, 4, cx, cy)));
            const float depth = s.poreDepth * (0.5f + hash01(s.seed, 5, cx, cy));
            const int r = static_cast<int>(std::ceil(radius));
            const int ix = static_cast<int>(px), iy = static_cast<int>(py);
            for (int dy = -r - 1; dy <= r + 1; ++dy) {
                for (int dx = -r - 1; dx <= r + 1; ++dx) {
                    const float ex = static_cast<float>(ix + dx) + 0.5f - px;
                    const float ey = static_cast<float>(iy + dy) + 0.5f - py;
                    const float t = (ex * ex + ey * ey) / (radius * radius);
                    if (t >= 1.f) {
                        continue;
                    }
                    const int wx = ((ix + dx) % ni + ni) % ni, wy = ((iy + dy) % ni + ni) % ni;
                    const float cup = (1.f - t) * (1.f - t);
                    float& h = height[static_cast<size_t>(wy) * n + static_cast<size_t>(wx)];
                    h = std::min(h, 1.f - depth * cup);
                }
            }
        }
    }
    for (float& h : height) {
        h = std::clamp(h, 0.f, 1.f);
    }
    return true;
}

void skinNormalBgra(const std::vector<float>& height, uint32_t size, float strength, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(size) * size * 4, 0);
    if (height.size() != static_cast<size_t>(size) * size) {
        out.clear();
        return;
    }
    const auto at = [&](uint32_t x, uint32_t y) { return height[static_cast<size_t>(y % size) * size + x % size]; };
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            // Rows run down the image; Unreal's tangent-space green points down the image too.
            const float dx = 0.5f * (at(x + 1, y) - at(x + size - 1, y)) * strength;
            const float dy = 0.5f * (at(x, y + 1) - at(x, y + size - 1)) * strength;
            const float len = std::sqrt(dx * dx + dy * dy + 1.f);
            uint8_t* p = &out[(static_cast<size_t>(y) * size + x) * 4];
            p[0] = toByte(0.5f + 0.5f / len);       // B: Z
            p[1] = toByte(0.5f - 0.5f * dy / len);  // G: Y
            p[2] = toByte(0.5f - 0.5f * dx / len);  // R: X
            p[3] = 255;
        }
    }
}

void skinMaskBgra(const std::vector<float>& height, uint32_t size, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(size) * size * 4, 0);
    if (height.size() != static_cast<size_t>(size) * size) {
        out.clear();
        return;
    }
    const float inv = 1.f / static_cast<float>(size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = static_cast<float>(x) * inv, v = static_cast<float>(y) * inv;
            const float micro = 0.65f * tileNoise(7, 30, 32, u, v) + 0.35f * tileNoise(7, 31, 128, u, v);
            uint8_t* p = &out[(static_cast<size_t>(y) * size + x) * 4];
            p[0] = 0;                                              // B
            p[1] = toByte(0.5f + 0.3f * (micro - 0.5f));           // G: micro roughness around 128
            p[2] = toByte(height[static_cast<size_t>(y) * size + x]); // R: cavity
            p[3] = 255;
        }
    }
}

} // namespace ftcore
