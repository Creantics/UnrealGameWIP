#include "ftcore/WaterStrand.h"

#include <cmath>

namespace ftcore {

WaterWayOut waterWayOutFromExit(const WaterBodies& water, const WaterExit& exit) {
    WaterWayOut way;
    way.kind = exit.kind == WaterExitKind::Steps ? WaterWayKind::Steps : WaterWayKind::Ladder;
    way.at = exit.foot;
    way.topZ = exit.topZ;
    const int body = water.bodyAt(exit.foot);
    way.bottomZ = body >= 0 ? water.body(static_cast<std::size_t>(body)).bedAt(exit.foot) : exit.topZ;
    return way;
}

bool waterWayUsable(const WaterWayOut& way, float surfaceZ, const WaterStrandSpec& spec) {
    if (way.kind == WaterWayKind::Mantle) return way.topZ - surfaceZ <= spec.mantleOverM;
    return way.bottomZ <= surfaceZ - spec.underM;
}

WaterStrandReport waterStrandCheck(const WaterBodies& water, const WaterSpec& spec, const WaterWayOut* ways, int count,
                                   const WaterStrandSpec& strand) {
    WaterStrandReport r;
    const float reach2 = strand.reachM * strand.reachM;
    const int levels = strand.levels < 1 ? 1 : strand.levels;
    auto list = [&](int way, int body, float level) {
        if (r.listed < kWaterStrandKept) r.first[r.listed++] = {way, body, level};
    };
    auto keep = [&](int way, int body, float level) {
        list(way, body, level);
        ++r.stranded;
    };
    for (int i = 0; i < count; ++i)
        if (water.bodyAt(ways[i].at) < 0) ++r.unplaced;

    for (std::size_t b = 0; b < water.count(); ++b) {
        const WaterBody& body = water.body(b);
        const WaterWaveClass c = waterWaveClass(body, spec.waves);
        float lo = 0.f, hi = 0.f;
        waterTideBand(spec.tide, body, c, lo, hi);
        bool any = false;
        for (int i = 0; i < count; ++i) {
            if (water.bodyAt(ways[i].at) != static_cast<int>(b)) continue;
            any = true;
            ++r.ways;
            for (int l = 0; l < levels; ++l) {
                const float z = body.level + (levels == 1 ? lo : lo + (hi - lo) * static_cast<float>(l) / static_cast<float>(levels - 1));
                ++r.checks;
                if (waterWayUsable(ways[i], z, strand)) continue;
                bool near = false;
                for (int j = 0; j < count && !near; ++j) {
                    if (j == i || water.bodyAt(ways[j].at) != static_cast<int>(b)) continue;
                    const float dx = ways[j].at.x - ways[i].at.x, dy = ways[j].at.y - ways[i].at.y;
                    near = dx * dx + dy * dy <= reach2 && waterWayUsable(ways[j], z, strand);
                }
                if (!near) keep(i, static_cast<int>(b), z);
            }
        }
        if (!any) {
            ++r.bodiesWithout;
            list(-1, static_cast<int>(b), body.level);
        }
    }
    return r;
}

namespace {

// Step l of n across a body's tide band, low water first (one step: low water, the hardest for a ladder).
float bandLevel(const WaterBody& body, const WaterSpec& spec, int l, int n) {
    float lo = 0.f, hi = 0.f;
    waterTideBand(spec.tide, body, waterWaveClass(body, spec.waves), lo, hi);
    return body.level + (n == 1 ? lo : lo + (hi - lo) * static_cast<float>(l) / static_cast<float>(n - 1));
}

} // namespace

WaterCoverageReport waterExitCoverage(const WaterBodies& water, const WaterSpec& spec, const WaterEntryPoint* points, int pointCount,
                                      const WaterWayOut* ways, int wayCount, const WaterStrandSpec& strand) {
    WaterCoverageReport r;
    const int bodies = static_cast<int>(water.count());
    int levels = strand.levels < 1 ? 1 : strand.levels;
    if (levels > kWaterCoverageMaxLevels) levels = kWaterCoverageMaxLevels;
    r.levels = levels;
    const float reach2 = strand.reachM * strand.reachM;
    for (int p = 0; p < pointCount; ++p) {
        const int b = points[p].body;
        if (b < 0 || b >= bodies) {
            ++r.unplaced;
            continue;
        }
        ++r.points;
        const WaterBody& body = water.body(static_cast<std::size_t>(b));
        for (int l = 0; l < levels; ++l) {
            const float z = bandLevel(body, spec, l, levels);
            float best2 = -1.f;
            for (int w = 0; w < wayCount; ++w) {
                if (water.bodyAt(ways[w].at) != b || !waterWayUsable(ways[w], z, strand)) continue;
                const float dx = ways[w].at.x - points[p].at.x, dy = ways[w].at.y - points[p].at.y;
                const float d2 = dx * dx + dy * dy;
                if (best2 < 0.f || d2 < best2) best2 = d2;
            }
            const float dist = best2 < 0.f ? kWaterCoverageNoWayM : std::sqrt(best2);
            if (best2 < 0.f || best2 > reach2) ++r.uncovered;
            WaterCoverageLevel& worst = r.worst[l];
            if (worst.point < 0 || dist > worst.distM) worst = {p, b, z, dist};
        }
    }
    return r;
}

} // namespace ftcore
