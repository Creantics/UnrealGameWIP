#include "ftcore/SceneLook.h"

#include <algorithm>

namespace ftcore {

SceneLook lerp(const SceneLook& a, const SceneLook& b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    if (t <= 0.f) return a;
    if (t >= 1.f) return b;
    SceneLook r;
#define FTCORE_LOOK_LERP(name, def) r.name = a.name + (b.name - a.name) * t;
    FTCORE_LOOK_FIELDS(FTCORE_LOOK_LERP)
#undef FTCORE_LOOK_LERP
    return r;
}

} // namespace ftcore
