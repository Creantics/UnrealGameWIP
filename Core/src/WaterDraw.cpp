#include "ftcore/WaterDraw.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

// A body standing for a group, for the tide's gain and lag.
WaterBody groupBody(WaterDrawGroup g, const WaterWaveSpec& spec) {
    WaterBody b;
    switch (g) {
    case WaterDrawGroup::Sea: b.kind = WaterKind::Sea; break;
    case WaterDrawGroup::River: b.kind = WaterKind::Canal; b.flow = {spec.riverFlowMin + 1.f, 0.f}; break;
    case WaterDrawGroup::Canal: b.kind = WaterKind::Canal; break;
    case WaterDrawGroup::Lake: b.kind = WaterKind::Lake; break;
    case WaterDrawGroup::Pool: b.kind = WaterKind::Pool; break;
    }
    return b;
}

std::size_t tilesAcross(float lengthM, float tileM) {
    if (!(lengthM > 0.f)) return 0;
    const float n = std::ceil(lengthM / tileM - 1e-4f);
    return n < 1.f ? 1 : static_cast<std::size_t>(std::min(n, 1.0e6f));
}

// Lays an nx by ny grid of tiles over a rectangle in a frame (origin o, x axis (c, s)), its x from x0 to x1 and y from
// y0 to y1.
std::size_t layGrid(const WaterTile& proto, Vec2 o, float c, float s, float x0, float x1, float y0, float y1, float tileM,
                    WaterTile* out, std::size_t cap, std::size_t at) {
    const std::size_t nx = tilesAcross(x1 - x0, tileM);
    const std::size_t ny = tilesAcross(y1 - y0, tileM);
    if (nx == 0 || ny == 0) return 0;
    const float sx = (x1 - x0) / static_cast<float>(nx);
    const float sy = (y1 - y0) / static_cast<float>(ny);
    for (std::size_t i = 0; i < nx && at < cap; ++i) {
        for (std::size_t j = 0; j < ny && at < cap; ++j) {
            const float lx = x0 + (static_cast<float>(i) + 0.5f) * sx;
            const float ly = y0 + (static_cast<float>(j) + 0.5f) * sy;
            WaterTile t = proto;
            t.centre = {o.x + lx * c - ly * s, o.y + lx * s + ly * c};
            t.cosYaw = c;
            t.sinYaw = s;
            t.size = {sx, sy};
            out[at++] = t;
        }
    }
    return nx * ny;
}

} // namespace

const char* waterDrawGroupName(WaterDrawGroup g) {
    switch (g) {
    case WaterDrawGroup::Sea: return "sea";
    case WaterDrawGroup::River: return "river";
    case WaterDrawGroup::Canal: return "canal";
    case WaterDrawGroup::Lake: return "lake";
    case WaterDrawGroup::Pool: return "pool";
    }
    return "?";
}

WaterDrawGroup waterDrawGroup(const WaterBody& body, const WaterWaveSpec& spec) {
    switch (body.kind) {
    case WaterKind::Sea: return WaterDrawGroup::Sea;
    case WaterKind::Lake: return WaterDrawGroup::Lake;
    case WaterKind::Pool: return WaterDrawGroup::Pool;
    case WaterKind::Canal: return waterWaveClass(body, spec) == WaterWaveClass::River ? WaterDrawGroup::River : WaterDrawGroup::Canal;
    }
    return WaterDrawGroup::Canal;
}

WaterWaveClass waterDrawGroupClass(WaterDrawGroup g) {
    switch (g) {
    case WaterDrawGroup::Sea: return WaterWaveClass::Sea;
    case WaterDrawGroup::River:
    case WaterDrawGroup::Lake: return WaterWaveClass::River;
    case WaterDrawGroup::Canal:
    case WaterDrawGroup::Pool: return WaterWaveClass::Canal;
    }
    return WaterWaveClass::Canal;
}

WaterDrawParams waterDrawParams(const WaterSurfaceModel& model, WaterDrawGroup g) {
    WaterDrawParams p;
    const WaterWaveClass c = waterDrawGroupClass(g);
    const SeaWaves& set = model.sets.set[static_cast<int>(c)];
    for (int i = 0; i < kSeaWaveCount; ++i) packSeaWave(set.waves[i], p.waves[i]);
    p.q = set.q;
    p.fadeCm = waterFadeCm(model.sets, c);
    p.lapShare = model.sets.lapShare;
    p.tideM = waterTideM(model.spec.tide, groupBody(g, model.spec.waves), c, model.totalHours);
    return p;
}

void waterEdgeLine(const WaterBody& body, float out[kWaterEdgeFloats]) {
    out[0] = body.centre.x * 100.f;
    out[1] = body.centre.y * 100.f;
    out[2] = body.cosYaw;
    out[3] = body.sinYaw;
    out[4] = 0.f;
    out[5] = 2.f;
    switch (body.shape) {
    case WaterShape::Box:
        // The long walls only (waterEdgeM): across the shorter side.
        if (body.half.y <= body.half.x) {
            out[2] = -body.sinYaw;
            out[3] = body.cosYaw;
            out[4] = body.half.y * 100.f;
        } else {
            out[4] = body.half.x * 100.f;
        }
        out[5] = 0.f;
        break;
    case WaterShape::HalfPlane: out[5] = 1.f; break;
    case WaterShape::Polygon: break;
    }
}

float waterEdgeLineCm(const float line[kWaterEdgeFloats], float x, float y) {
    const float d = (x - line[0]) * line[2] + (y - line[1]) * line[3];
    if (line[5] > 1.5f) return kWaterNoEdgeM * 100.f;
    if (line[5] > 0.5f) return std::max(d, 0.f);
    return std::max(line[4] - std::fabs(d), 0.f);
}

namespace {

// One body's tiles into out from at (up to cap); returns how many the body needs.
std::size_t layBody(const WaterBodies& water, std::size_t i, const WaterWaveSpec& waves, const WaterTileSpec& spec,
                    const WaterTileBounds& bounds, WaterTile* out, std::size_t cap, std::size_t at) {
    const float tileM = spec.tileM > 0.5f ? spec.tileM : 16.f;
    const WaterBody& b = water.body(i);
    WaterTile proto;
    proto.body = static_cast<std::uint16_t>(i);
    proto.group = waterDrawGroup(b, waves);
    proto.z = b.level;
    switch (b.shape) {
    case WaterShape::Box:
        return layGrid(proto, b.centre, b.cosYaw, b.sinYaw, -b.half.x, b.half.x, -b.half.y, b.half.y, tileM, out, cap, at);
    case WaterShape::HalfPlane: {
        // Along the shore: the bounds' corners projected on the shore's line, (-sin, cos).
        float lo = 0.f, hi = 0.f;
        const Vec2 corners[4] = {bounds.min, {bounds.max.x, bounds.min.y}, bounds.max, {bounds.min.x, bounds.max.y}};
        for (int k = 0; k < 4; ++k) {
            const float a = -(corners[k].x - b.centre.x) * b.sinYaw + (corners[k].y - b.centre.y) * b.cosYaw;
            lo = k == 0 ? a : std::min(lo, a);
            hi = k == 0 ? a : std::max(hi, a);
        }
        return layGrid(proto, b.centre, b.cosYaw, b.sinYaw, -std::max(spec.landOverlapM, 0.f), std::max(spec.seaReachM, 0.f), lo, hi,
                       tileM, out, cap, at);
    }
    case WaterShape::Polygon: break;
    }
    return 0;
}

} // namespace

std::size_t waterTileLayout(const WaterBodies& water, const WaterWaveSpec& waves, const WaterTileSpec& spec,
                            const WaterTileBounds& bounds, WaterTile* out, std::size_t cap) {
    if (!out) cap = 0;
    std::size_t total = 0;
    for (std::size_t i = 0; i < water.count() && i <= 0xFFFFu; ++i) {
        total += layBody(water, i, waves, spec, bounds, out, cap, std::min(total, cap));
    }
    return total;
}

std::size_t waterTileLayoutWhole(const WaterBodies& water, const WaterWaveSpec& waves, const WaterTileSpec& spec,
                                 const WaterTileBounds& bounds, WaterTile* out, std::size_t cap, std::uint32_t* dropped,
                                 std::uint32_t* perBody) {
    if (!out) cap = 0;
    std::uint32_t left = 0;
    std::size_t laid = 0;
    for (std::size_t i = 0; i < water.count() && i < 32; ++i) {
        const std::size_t need = layBody(water, i, waves, spec, bounds, nullptr, 0, 0);
        if (perBody) perBody[i] = static_cast<std::uint32_t>(std::min<std::size_t>(need, 0xFFFFFFFFu));
        if (need == 0) continue;
        if (need > cap - laid) {
            left |= 1u << i;
            continue;
        }
        laid += layBody(water, i, waves, spec, bounds, out, cap, laid);
    }
    if (dropped) *dropped = left;
    return laid;
}

} // namespace ftcore
