#include "ftcore/PathFollow.h"

namespace ftcore {

int advanceWaypoint(const Vec2* points, int count, int current, Vec2 pos, float reach) {
    if (!points || count <= 0) return -1;
    int i = current < 0 ? 0 : (current >= count ? count - 1 : current);
    while (i < count - 1) {
        const Vec2 p = points[i];
        const float dx = pos.x - p.x, dy = pos.y - p.y;
        if (dx * dx + dy * dy <= reach * reach) {
            ++i;
            continue;
        }
        // Passed: the walker is beyond the point, measured along the leg that led to it.
        if (i > 0) {
            const Vec2 prev = points[i - 1];
            const float lx = p.x - prev.x, ly = p.y - prev.y;
            if (dx * lx + dy * ly > 0.f) {
                ++i;
                continue;
            }
        }
        break;
    }
    return i;
}

} // namespace ftcore
