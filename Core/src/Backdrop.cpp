#include "ftcore/Backdrop.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint32_t mix(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// A repeatable value in [0, 1) for a seed and three integers.
float unit(std::uint32_t seed, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const std::uint32_t h = mix(seed ^ mix(a * 0x9e3779b9u ^ mix(b * 0x85ebca6bu ^ mix(c * 0xc2b2ae35u + 1u))));
    return static_cast<float>(h >> 8) / 16777216.f;
}

// The mean of 8 points round (x, y) at radius r.
double ringMean(const HeightField& f, double x, double y, double r) {
    double sum = 0.0;
    for (int k = 0; k < 8; ++k) {
        const double a = k * (kPi / 4.0);
        sum += f.at(x + r * std::cos(a), y + r * std::sin(a));
    }
    return sum / 8.0;
}

double slopeAt(const HeightField& f, double x, double y, double d) {
    const double gx = (f.at(x + d, y) - f.at(x - d, y)) / (2.0 * d);
    const double gy = (f.at(x, y + d) - f.at(x, y - d)) / (2.0 * d);
    return std::sqrt(gx * gx + gy * gy);
}

} // namespace

std::vector<BackdropPoint> hillDress(const HeightField& field, const HillDressSpec& spec, double x0, double y0, double x1, double y1,
                                     const MassingBox* boxes, std::size_t boxCount) {
    std::vector<BackdropPoint> out;
    if (!field.grid || field.samplesX == 0 || field.samplesY == 0 || spec.maxPoints == 0 || x1 <= x0 || y1 <= y0) {
        return out;
    }
    const double cell = std::max(static_cast<double>(spec.cellM), 1.0);
    const std::size_t nx = static_cast<std::size_t>(std::ceil((x1 - x0) / cell));
    const std::size_t ny = static_cast<std::size_t>(std::ceil((y1 - y0) / cell));
    // The cells a box (plus clearM) covers, by its rotated footprint's bounding rectangle.
    std::vector<std::uint8_t> blocked(nx * ny, 0);
    for (std::size_t b = 0; b < boxCount && boxes; ++b) {
        const MassingBox& box = boxes[b];
        const double c = std::abs(std::cos(box.rot * kPi / 180.0)), s = std::abs(std::sin(box.rot * kPi / 180.0));
        const double hx = 0.5 * (c * box.sx + s * box.sy) + spec.clearM, hy = 0.5 * (s * box.sx + c * box.sy) + spec.clearM;
        const double ix0 = std::floor((box.cx - hx - x0) / cell), ix1 = std::floor((box.cx + hx - x0) / cell);
        const double iy0 = std::floor((box.cy - hy - y0) / cell), iy1 = std::floor((box.cy + hy - y0) / cell);
        if (ix1 < 0 || iy1 < 0 || ix0 >= static_cast<double>(nx) || iy0 >= static_cast<double>(ny)) {
            continue;
        }
        for (std::size_t iy = static_cast<std::size_t>(std::max(iy0, 0.0)); iy <= static_cast<std::size_t>(std::min(iy1, ny - 1.0)); ++iy) {
            for (std::size_t ix = static_cast<std::size_t>(std::max(ix0, 0.0)); ix <= static_cast<std::size_t>(std::min(ix1, nx - 1.0)); ++ix) {
                blocked[iy * nx + ix] = 1;
            }
        }
    }
    auto isBlocked = [&](double x, double y) {
        const double fx = std::floor((x - x0) / cell), fy = std::floor((y - y0) / cell);
        if (fx < 0 || fy < 0 || fx >= static_cast<double>(nx) || fy >= static_cast<double>(ny)) {
            return true;
        }
        return blocked[static_cast<std::size_t>(fy) * nx + static_cast<std::size_t>(fx)] != 0;
    };

    // The pylon line: per step along y, the highest hill ground across x (scanned a cell apart).
    const double step = std::max(static_cast<double>(spec.pylonStepM), cell);
    for (double y = y0 + 0.5 * step; y < y1 && out.size() < spec.maxPoints; y += step) {
        double bestX = 0.0, bestH = -1e9;
        for (double x = x0 + 0.5 * cell; x < x1; x += cell) {
            const double h = field.at(x, y);
            if (h > bestH) {
                bestH = h;
                bestX = x;
            }
        }
        if (bestH < spec.minHeightM) {
            continue;
        }
        // Off the very top if a box holds it: the nearest clear cell along x within 5 cells.
        double at = bestX;
        bool found = !isBlocked(at, y);
        for (int k = 1; k <= 5 && !found; ++k) {
            for (const double cand : {bestX - k * cell, bestX + k * cell}) {
                if (!found && !isBlocked(cand, y) && field.at(cand, y) >= spec.minHeightM) {
                    at = cand;
                    found = true;
                }
            }
        }
        if (!found) {
            continue;
        }
        BackdropPoint p;
        p.x = static_cast<float>(at);
        p.y = static_cast<float>(y);
        p.z = static_cast<float>(field.at(at, y));
        p.scale = 1.f;
        p.yaw = 90.f; // the arms across the line, which runs north-south
        p.kind = BackdropKind::Pylon;
        out.push_back(p);
    }

    for (std::size_t iy = 0; iy < ny && out.size() < spec.maxPoints; ++iy) {
        for (std::size_t ix = 0; ix < nx && out.size() < spec.maxPoints; ++ix) {
            const auto ux = static_cast<std::uint32_t>(ix), uy = static_cast<std::uint32_t>(iy);
            const double x = x0 + (ix + unit(spec.seed, ux, uy, 1)) * cell;
            const double y = y0 + (iy + unit(spec.seed, ux, uy, 2)) * cell;
            if (x >= x1 || y >= y1 || blocked[iy * nx + ix]) {
                continue;
            }
            const double h = field.at(x, y);
            if (h < spec.minHeightM || slopeAt(field, x, y, 2.0) > spec.maxSlope) {
                continue;
            }
            const bool crest = h - ringMean(field, x, y, spec.ringM) >= spec.crestRiseM;
            const float roll = unit(spec.seed, ux, uy, 3);
            const float tree = crest ? spec.crestTree : spec.slopeTree;
            const float scrub = crest ? spec.crestScrub : spec.slopeScrub;
            if (roll >= tree + scrub) {
                continue;
            }
            BackdropPoint p;
            p.x = static_cast<float>(x);
            p.y = static_cast<float>(y);
            p.z = static_cast<float>(h);
            p.scale = 0.6f + unit(spec.seed, ux, uy, 4);
            p.yaw = 360.f * unit(spec.seed, ux, uy, 5);
            p.kind = roll < tree ? BackdropKind::Tree : BackdropKind::Scrub;
            out.push_back(p);
        }
    }
    return out;
}

std::vector<BackdropBox> skylineBand(const SkylineSpec& spec) {
    std::vector<BackdropBox> out;
    if (spec.x1 <= spec.x0 || spec.y1 <= spec.y0 || spec.count == 0) {
        return out;
    }
    out.reserve(spec.count);
    const float minW = std::max(spec.minW, 1.f), maxW = std::max(spec.maxW, minW);
    const float minH = std::max(spec.minH, 1.f), maxH = std::max(spec.maxH, minH);
    // The farthest corner from the near point, for the height's growth.
    double farDist = 1.0;
    for (const double cx : {spec.x0, spec.x1}) {
        for (const double cy : {spec.y0, spec.y1}) {
            farDist = std::max(farDist, std::hypot(cx - spec.nearX, cy - spec.nearY));
        }
    }
    double nearDist = farDist;
    {
        const double qx = std::clamp(spec.nearX, spec.x0, spec.x1), qy = std::clamp(spec.nearY, spec.y0, spec.y1);
        nearDist = std::hypot(qx - spec.nearX, qy - spec.nearY);
    }
    for (std::size_t i = 0; i < spec.count; ++i) {
        const auto u = static_cast<std::uint32_t>(i);
        BackdropBox b;
        b.sx = minW + (maxW - minW) * unit(spec.seed, u, 0, 1);
        b.sy = minW + (maxW - minW) * unit(spec.seed, u, 0, 2);
        b.rot = (unit(spec.seed, u, 0, 3) < 0.7f ? 0.f : 90.f) + 8.f * (unit(spec.seed, u, 0, 4) - 0.5f);
        // Inside the band by the footprint's larger half (any yaw).
        const double half = 0.5 * std::hypot(b.sx, b.sy);
        const double bx0 = spec.x0 + half, bx1 = std::max(spec.x1 - half, bx0);
        const double by0 = spec.y0 + half, by1 = std::max(spec.y1 - half, by0);
        b.cx = static_cast<float>(bx0 + (bx1 - bx0) * unit(spec.seed, u, 0, 5));
        b.cy = static_cast<float>(by0 + (by1 - by0) * unit(spec.seed, u, 0, 6));
        const double d = std::hypot(b.cx - spec.nearX, b.cy - spec.nearY);
        const float t = static_cast<float>(std::clamp((d - nearDist) / std::max(farDist - nearDist, 1.0), 0.0, 1.0));
        const float r = unit(spec.seed, u, 0, 7);
        // Most blocks rise with the distance; one in eight is a tower.
        const float share = r < 0.125f ? 0.75f + 0.25f * (r / 0.125f) : (0.15f + 0.55f * t) * (0.6f + 0.4f * r);
        b.h = minH + (maxH - minH) * std::clamp(share, 0.f, 1.f);
        out.push_back(b);
    }
    return out;
}

void heightShapeMask(const HeightField& field, double centreX, double centreY, double sideM, std::size_t size, bool northUp, float ringM,
                     float fullPerMetre, std::uint8_t* out) {
    if (!out || size == 0) {
        return;
    }
    const double ring = std::max(static_cast<double>(ringM), 0.5);
    const double full = std::max(static_cast<double>(fullPerMetre), 1e-4);
    for (std::size_t iy = 0; iy < size; ++iy) {
        const double v = (static_cast<double>(iy) + 0.5) / static_cast<double>(size) - 0.5;
        const double y = northUp ? centreY - v * sideM : centreY + v * sideM;
        for (std::size_t ix = 0; ix < size; ++ix) {
            const double x = centreX + ((static_cast<double>(ix) + 0.5) / static_cast<double>(size) - 0.5) * sideM;
            double shape = 0.0;
            if (field.grid && field.samplesX > 0 && field.samplesY > 0) {
                const double h = field.at(x, y);
                const double nearRise = (h - ringMean(field, x, y, ring)) / (full * ring);
                const double farRise = (h - ringMean(field, x, y, 3.0 * ring)) / (full * 3.0 * ring);
                shape = std::clamp(0.5 * (nearRise + farRise), -1.0, 1.0);
            }
            out[iy * size + ix] = static_cast<std::uint8_t>(std::lround(128.0 + 127.0 * shape));
        }
    }
}


bool backdropRockFace(const MassingBox& box) {
    return box.kind == "cliff" || box.kind == "terrace";
}

double heightSteepShare(const HeightField& field, double x0, double y0, double x1, double y1, double step, double minSlope) {
    if (!field.grid || field.samplesX == 0 || field.samplesY == 0 || !(x1 > x0) || !(y1 > y0) || !(step > 0.0)) {
        return 0.0;
    }
    const double d = std::max(field.spacing, 0.5);
    std::size_t all = 0, steep = 0;
    for (double y = y0; y <= y1; y += step) {
        for (double x = x0; x <= x1; x += step) {
            ++all;
            steep += slopeAt(field, x, y, d) >= minSlope ? 1u : 0u;
        }
    }
    return all ? static_cast<double>(steep) / static_cast<double>(all) : 0.0;
}

std::size_t backdropClump(const BackdropPoint& p, BackdropLump* out) {
    if (!out || p.kind == BackdropKind::Pylon) {
        return 0;
    }
    const bool tree = p.kind == BackdropKind::Tree;
    // Seeded by the point's plan position (decimetres).
    const std::uint32_t qx = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(p.x * 10.f)));
    const std::uint32_t qy = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(p.y * 10.f)));
    const float scale = std::clamp(p.scale, 0.4f, 2.f);
    const std::uint8_t baseTint = static_cast<std::uint8_t>(unit(0xc1u, qx, qy, 0u) * static_cast<float>(kBackdropScrubTints));
    auto tintOf = [&](float r) { return static_cast<std::uint8_t>((baseTint + (r < 0.3f ? 1u : 0u)) % kBackdropScrubTints); };
    if (!tree) {
        // Two rounded bush mounds, a fifth under the ground.
        for (std::size_t k = 0; k < 2; ++k) {
            const std::uint32_t kk = static_cast<std::uint32_t>(k) + 1u;
            auto u = [&](std::uint32_t salt) { return unit(0xc1u + salt, qx, qy, kk); };
            const float wide = std::min((1.8f + 1.6f * u(1)) * scale * (k == 0 ? 1.f : 0.6f + 0.3f * u(2)), 3.6f);
            BackdropLump& l = out[k];
            l = BackdropLump{};
            l.sx = wide;
            l.sy = wide * (0.7f + 0.3f * u(3));
            l.sz = std::min(wide * (0.5f + 0.2f * u(4)), 1.8f);
            l.yaw = 360.f * u(5);
            const double a = 2.0 * kPi * static_cast<double>(u(6));
            const float r = k == 0 ? 0.f : (0.3f + 0.2f * u(7)) * out[0].sx;
            l.dx = static_cast<float>(r * std::cos(a));
            l.dy = static_cast<float>(r * std::sin(a));
            l.dz = 0.3f * l.sz; // a fifth of it under the ground
            l.tint = tintOf(u(8));
        }
        return 2;
    }
    // A trunk, sunk 0.3 m, then three crown lumps overlapping its top into one canopy.
    auto t = [&](std::uint32_t salt) { return unit(0xd7u + salt, qx, qy, 0u); };
    BackdropLump& trunk = out[0];
    trunk = BackdropLump{};
    trunk.trunk = true;
    trunk.sx = trunk.sy = (0.3f + 0.15f * t(1)) * scale;
    trunk.sz = (3.0f + 2.5f * t(2)) * scale;
    trunk.dz = 0.5f * trunk.sz - 0.3f;
    trunk.yaw = 360.f * t(3);
    trunk.tint = 0;
    const float top = trunk.sz - 0.3f;
    for (std::size_t k = 1; k < 4; ++k) {
        const std::uint32_t kk = static_cast<std::uint32_t>(k);
        auto u = [&](std::uint32_t salt) { return unit(0xc1u + salt, qx, qy, kk); };
        const float wide = (3.5f + 3.0f * u(1)) * scale * (k == 1 ? 1.f : 0.6f + 0.3f * u(2));
        BackdropLump& l = out[k];
        l = BackdropLump{};
        l.sx = wide;
        l.sy = wide * (0.75f + 0.25f * u(3));
        l.sz = std::min(wide * (0.7f + 0.2f * u(4)), 5.f);
        l.yaw = 360.f * u(5);
        const double a = 2.0 * kPi * static_cast<double>(u(6));
        const float r = k == 1 ? 0.f : (0.25f + 0.15f * u(7)) * out[1].sx;
        l.dx = static_cast<float>(r * std::cos(a));
        l.dy = static_cast<float>(r * std::sin(a));
        l.dz = top + (k == 1 ? 0.35f : 0.15f + 0.15f * u(9)) * l.sz;
        l.tint = tintOf(u(8));
    }
    return 4;
}

void hillPlantMask(const HeightField& field, double centreX, double centreY, double sideM, std::size_t size, bool northUp,
                   float minHeightM, float rampM, float steepFrom, float steepTo, std::uint8_t* out) {
    if (!out || size == 0) {
        return;
    }
    const double ramp = std::max(static_cast<double>(rampM), 0.1);
    const double s0 = static_cast<double>(steepFrom);
    const double sRamp = std::max(static_cast<double>(steepTo) - s0, 0.01);
    for (std::size_t iy = 0; iy < size; ++iy) {
        const double v = (static_cast<double>(iy) + 0.5) / static_cast<double>(size) - 0.5;
        const double y = northUp ? centreY - v * sideM : centreY + v * sideM;
        for (std::size_t ix = 0; ix < size; ++ix) {
            const double x = centreX + ((static_cast<double>(ix) + 0.5) / static_cast<double>(size) - 0.5) * sideM;
            double hill = 0.0;
            if (field.grid && field.samplesX > 0 && field.samplesY > 0) {
                const double t = std::clamp((field.at(x, y) - static_cast<double>(minHeightM)) / ramp, 0.0, 1.0);
                hill = t * t * (3.0 - 2.0 * t);
                if (hill > 0.0) {
                    const double gx = (field.at(x + 2.0, y) - field.at(x - 2.0, y)) / 4.0;
                    const double gy = (field.at(x, y + 2.0) - field.at(x, y - 2.0)) / 4.0;
                    const double st = std::clamp((std::sqrt(gx * gx + gy * gy) - s0) / sRamp, 0.0, 1.0);
                    hill *= 1.0 - st * st * (3.0 - 2.0 * st);
                }
            }
            out[iy * size + ix] = static_cast<std::uint8_t>(std::lround(255.0 * (1.0 - hill)));
        }
    }
}

namespace {

struct AviationRoof {
    double cx, cy, sx, sy, rot, top, h;
    bool point;
};

bool aviationClear(const std::vector<AviationLamp>& lamps, std::size_t count, double x, double y, double minD2) {
    for (std::size_t i = 0; i < count; ++i) {
        const double dx = static_cast<double>(lamps[i].x) - x, dy = static_cast<double>(lamps[i].y) - y;
        if (dx * dx + dy * dy < minD2) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<AviationLamp> aviationLamps(const AviationSpec& spec, const MassingBox* boxes, std::size_t boxCount, const BackdropBox* skyline,
                                        std::size_t skylineCount) {
    std::vector<AviationRoof> roofs;
    for (std::size_t b = 0; b < boxCount && boxes; ++b) {
        const MassingBox& box = boxes[b];
        const double h = box.z1 - box.z0;
        if (h < static_cast<double>(spec.minHeightM)) {
            continue;
        }
        const bool slim = box.kind == "mast" || box.kind == "crane";
        if (!slim && box.kind != "kit" && box.kind != "tower") {
            continue;
        }
        roofs.push_back({box.cx, box.cy, box.sx, box.sy, box.rot, box.z1, h, slim});
    }
    // Tallest first; ties by plan position, so the order never depends on the table's.
    std::sort(roofs.begin(), roofs.end(), [](const AviationRoof& a, const AviationRoof& c) {
        if (a.h != c.h) return a.h > c.h;
        if (a.cx != c.cx) return a.cx < c.cx;
        return a.cy < c.cy;
    });
    std::vector<AviationLamp> out;
    const double minD2 = static_cast<double>(spec.minSpacingM) * static_cast<double>(spec.minSpacingM);
    const double inset = std::max(static_cast<double>(spec.insetM), 0.0);
    const double head = static_cast<double>(spec.headM);
    auto put = [&](double x, double y, double z, double h) {
        out.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(h)});
    };
    for (const AviationRoof& r : roofs) {
        if (out.size() >= spec.maxLamps) {
            break;
        }
        const double z = r.top + head;
        if (r.point || r.sx < 2.0 * inset + 0.5 || r.sy < 2.0 * inset + 0.5) {
            if (aviationClear(out, out.size(), r.cx, r.cy, minD2)) {
                put(r.cx, r.cy, z, r.h);
            }
            continue;
        }
        const double c = std::cos(r.rot * kPi / 180.0), s = std::sin(r.rot * kPi / 180.0);
        const double hu = 0.5 * r.sx - inset, hw = 0.5 * r.sy - inset;
        // One corner, then (a long roof) the opposite one; the pair share a roof, so the second is spaced from the
        // other roofs' lamps only.
        const std::size_t before = out.size();
        const double ax = r.cx + c * hu - s * hw, ay = r.cy + s * hu + c * hw;
        if (!aviationClear(out, before, ax, ay, minD2)) {
            continue;
        }
        put(ax, ay, z, r.h);
        if (std::max(r.sx, r.sy) >= static_cast<double>(spec.longRoofM) && out.size() < spec.maxLamps) {
            const double bx = r.cx - c * hu + s * hw, by = r.cy - s * hu - c * hw;
            if (aviationClear(out, before, bx, by, minD2)) {
                put(bx, by, z, r.h);
            }
        }
    }
    // The far band: its tallest blocks, one lamp each at the roof's centre.
    std::vector<std::size_t> tall;
    for (std::size_t i = 0; i < skylineCount && skyline; ++i) {
        if (skyline[i].h >= spec.skylineMinH) {
            tall.push_back(i);
        }
    }
    std::sort(tall.begin(), tall.end(), [&](std::size_t a, std::size_t b) {
        if (skyline[a].h != skyline[b].h) return skyline[a].h > skyline[b].h;
        return a < b;
    });
    std::size_t far = 0;
    for (std::size_t i : tall) {
        if (far >= spec.maxSkyline) {
            break;
        }
        const BackdropBox& k = skyline[i];
        if (aviationClear(out, out.size(), k.cx, k.cy, minD2)) {
            put(k.cx, k.cy, static_cast<double>(k.h) + head, k.h);
            ++far;
        }
    }
    return out;
}

} // namespace ftcore
