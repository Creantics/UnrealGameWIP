// Internal to Core/src: not part of the public ftcore headers.
#pragma once

// When a foot in a clip is at rest on the floor: the rule the traversal and slide measures share.

#include "ftcore/Traversal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ftcore::detail {

using Part = const TravPoint& (*)(const TraversalFrame&);
inline const TravPoint& ballLeft(const TraversalFrame& f) { return f.ballLeft; }
inline const TravPoint& ballRight(const TraversalFrame& f) { return f.ballRight; }

inline float dist(const TravPoint& a, const TravPoint& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

inline float groundDist(const TravPoint& a, const TravPoint& b) { return std::hypot(a.x - b.x, a.y - b.y); }

// The frames kRestTime spans at a clip's frame time dt (at least 1).
inline std::size_t restFrames(float dt) { return std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(kRestTime / dt - 1e-4f))); }

// At rest from frame i: its net move over the next n frames slower than kRestSpeed, or than kRestShare of
// the root's ground speed over them (a planted foot slips and rolls some at a run; a swinging one passes
// the root). The net move, so a jittering sample or two does not lift it.
inline bool restsFrom(const TraversalFrame* f, std::size_t count, Part part, std::size_t i, std::size_t n, float dt) {
    if (i + n >= count) return false;
    const float limit = std::max(kRestSpeed * static_cast<float>(n) * dt, kRestShare * groundDist(f[i].root, f[i + n].root));
    return dist(part(f[i]), part(f[i + n])) < limit;
}

struct FootRest {
    int foot = -1;          // 0 left, 1 right; -1 none
    std::size_t first = 0;  // the frame its rest begins at
};

// The take-off foot: the last at rest on the floor under frame `from`, from it to frame `before`, and the
// frame that rest begins at (the foot planting).
inline FootRest lastFootAtRest(const TraversalFrame* f, std::size_t count, std::size_t from, std::size_t before, float dt) {
    const Part balls[2] = {ballLeft, ballRight};
    const float floor0 = std::min(f[from].ballLeft.z, f[from].ballRight.z);
    const std::size_t rest = restFrames(dt);
    auto rests = [&](int b, std::size_t i) { return balls[b](f[i]).z - floor0 < kFootRest && restsFrom(f, count, balls[b], i, rest, dt); };
    for (std::size_t i = before; i-- > from;)
        for (int b = 0; b < 2; ++b)
            if (rests(b, i)) {
                while (i > from && rests(b, i - 1)) --i;
                return {b, i};
            }
    return {};
}

} // namespace ftcore::detail
