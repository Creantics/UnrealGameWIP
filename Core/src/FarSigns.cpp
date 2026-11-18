#include "ftcore/FarSigns.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

int farSignColour(std::size_t index) {
    static constexpr int kPattern[] = {0, 1, 0, 2, 1};
    return kPattern[index % (sizeof(kPattern) / sizeof(kPattern[0]))];
}

namespace {

// Plan distance from (cx, cy) to the block's footprint; negative when the point is inside it.
float blockDistance(const FarSignBlock& b, float cx, float cy) {
    if (cx >= b.minX && cx <= b.maxX && cy >= b.minY && cy <= b.maxY) {
        return -1.f;
    }
    const float dx = std::max({b.minX - cx, 0.f, cx - b.maxX});
    const float dy = std::max({b.minY - cy, 0.f, cy - b.maxY});
    return std::sqrt(dx * dx + dy * dy);
}

bool eligible(const FarSignBlock& b, float d, const FarSignSpec& spec) {
    return d >= spec.nearM && d <= spec.farM && b.topZ - b.baseZ >= spec.minHeightM && b.maxX - b.minX <= spec.maxFootM &&
           b.maxY - b.minY <= spec.maxFootM && b.maxX > b.minX && b.maxY > b.minY;
}

// The point along a face [lo, hi] nearest to c, kept cornerM off its ends (the middle when the face is shorter).
float alongFace(float lo, float hi, float c, float cornerM) {
    if (hi - lo <= 2.f * cornerM) {
        return 0.5f * (lo + hi);
    }
    return std::clamp(c, lo + cornerM, hi - cornerM);
}

FarSign cardOn(const FarSignBlock& b, float cx, float cy, const FarSignSpec& spec) {
    FarSign s;
    const float gapX = std::max(b.minX - cx, cx - b.maxX); // > 0 when the centre is beyond an X face
    const float gapY = std::max(b.minY - cy, cy - b.maxY);
    if (gapX >= gapY) {
        const bool plus = cx > b.maxX;
        s.x = (plus ? b.maxX : b.minX) + (plus ? spec.outM : -spec.outM);
        s.y = alongFace(b.minY, b.maxY, cy, spec.cornerM);
        s.yawDeg = plus ? 0.f : 180.f;
    } else {
        const bool plus = cy > b.maxY;
        s.y = (plus ? b.maxY : b.minY) + (plus ? spec.outM : -spec.outM);
        s.x = alongFace(b.minX, b.maxX, cx, spec.cornerM);
        s.yawDeg = plus ? 90.f : 270.f;
    }
    const float tall = b.topZ - b.baseZ;
    s.heightM = std::min(spec.heightM, 0.45f * tall);
    s.widthM = spec.widthM;
    // The card's middle a little over half the block's height, its top at least half a metre under the roof.
    s.z = std::min(b.baseZ + std::min(0.55f * tall, 12.f), b.topZ - 0.5f - 0.5f * s.heightM);
    return s;
}

} // namespace

std::size_t placeFarSigns(const FarSignBlock* blocks, std::size_t count, float cx, float cy, const FarSignSpec& spec,
                          FarSign* out, std::size_t maxOut) {
    if (!blocks || !out) {
        return 0;
    }
    std::size_t placed = 0;
    float lastD = -1.f;
    std::size_t lastI = 0;
    while (placed < maxOut) {
        // The nearest eligible block after the last one picked (distance, then index, ascending).
        std::size_t best = count;
        float bestD = 0.f;
        for (std::size_t i = 0; i < count; ++i) {
            const float d = blockDistance(blocks[i], cx, cy);
            if (!eligible(blocks[i], d, spec)) {
                continue;
            }
            const bool after = placed == 0 || d > lastD || (d == lastD && i > lastI);
            if (after && (best == count || d < bestD || (d == bestD && i < best))) {
                best = i;
                bestD = d;
            }
        }
        if (best == count) {
            break;
        }
        out[placed] = cardOn(blocks[best], cx, cy, spec);
        out[placed].colour = farSignColour(placed);
        ++placed;
        lastD = bestD;
        lastI = best;
    }
    return placed;
}

} // namespace ftcore
