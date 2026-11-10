#pragma once

// A party follower kept out of the sea. A follower is placed, not steered (PartyTrail.h), and has no collision, so the
// hidden walls that hold the lead at a quay's edge don't hold it: its slot, a metre to the side of the lead's path or
// cutting a corner, can go out over the water. There it would stand at the lead's height over open water (the look down
// meets nothing), on the bank too steep to walk between a quay's edge and the bed, or sink to the bed. Instead, where its
// spot has no footing it stands at the furthest point out from
// where the lead walked toward its spot before that way first meets the sea, and a body's width back from it: it walks
// along the shore beside the lead, and never across a stretch of water onto land beyond it.
//
//   if (!shoreFooting(found, groundZ, normalZ, seaLevel, tuning.margin, walkableZ))
//       stand = standShortOfSea(pathPoint, spot, [&](Vec2 p) { return footingAt(p); }, tuning);
//
// Positions are metres on the ground plane. Nothing here allocates; dryAt is called at most
// 1 + maxSteps + halvings times.

#include "ftcore/Explore.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

struct PartyShoreTuning {
    float margin = 0.1f; // metres: ground no higher than the sea level plus this is the sea's
    float step = 0.25f;  // metres: the way out from the anchor is looked along this far a step...
    int maxSteps = 16;   // ...at most this many (a longer way takes longer steps)
    int halvings = 6;    // then the step that met the sea is halved this many times
    float inset = 0.3f;  // metres: it stands this far back from the shore toward the anchor (not past it)
};

// Ground found at `groundZ`, its normal's up part `normalZ` (the cosine of its slope), is footing when it is clear of a
// sea at `seaLevel` (groundZ, seaLevel and margin in one unit) and no steeper than a walk (normalZ at least `walkableZ`).
// Ground not found (a look down that met nothing: open water over a deep bed, or a drop) is no footing; nor is a bank too
// steep to walk, such as the drop from a quay's edge to the bed.
inline bool shoreFooting(bool found, double groundZ, double normalZ, double seaLevel, double margin, double walkableZ) {
    return found && std::isfinite(groundZ) && std::isfinite(normalZ) && groundZ > seaLevel + margin && normalZ >= walkableZ;
}

struct ShoreStand {
    Vec2 at;           // where to stand
    bool dry = false;  // the anchor has footing (else `at` is the anchor and there is nowhere dry on the way)
    bool held = false; // the way met the sea: `at` is short of `spot`
    int looks = 0;     // points looked at (calls to dryAt)
};

// Where a follower whose spot may be over the sea stands: `spot` itself when the whole way out to it from `anchor` (where
// the lead walked) has footing; otherwise the furthest point on that way before it first meets the sea, less `inset`.
// `dryAt(Vec2)` says whether a point has footing. A way that meets the sea and comes out onto land again stops at the
// first shore: past it would be a step across the water.
template <class DryAt>
ShoreStand standShortOfSea(Vec2 anchor, Vec2 spot, DryAt&& dryAt, const PartyShoreTuning& tuning = {}) {
    ShoreStand out;
    out.at = anchor;
    out.looks = 1;
    if (!dryAt(anchor)) return out;
    out.dry = true;
    const float dx = spot.x - anchor.x;
    const float dy = spot.y - anchor.y;
    const float length = std::hypot(dx, dy);
    if (!(length > 0.f)) return out; // on the anchor (a NaN spot too)
    const float step = tuning.step > 0.f ? tuning.step : length;
    const int steps = std::clamp(static_cast<int>(std::ceil(length / step)), 1, std::max(tuning.maxSteps, 1));
    auto along = [&](float f) { return Vec2{anchor.x + dx * f, anchor.y + dy * f}; };
    float dry = 0.f; // the furthest share of the way known to have footing...
    float wet = 1.f; // ...and the nearest known not to
    bool met = false;
    for (int k = 1; k <= steps; ++k) {
        const float f = static_cast<float>(k) / static_cast<float>(steps);
        ++out.looks;
        if (dryAt(along(f))) {
            dry = f;
        } else {
            wet = f;
            met = true;
            break;
        }
    }
    if (!met) {
        out.at = spot;
        return out;
    }
    for (int h = 0; h < tuning.halvings; ++h) {
        const float mid = 0.5f * (dry + wet);
        ++out.looks;
        if (dryAt(along(mid)))
            dry = mid;
        else
            wet = mid;
    }
    out.held = true;
    out.at = along(std::max(dry - std::max(tuning.inset, 0.f) / length, 0.f));
    return out;
}

} // namespace ftcore
