#include "ftcore/ArenaGun.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

ArenaGunLayer arenaReloadLayer(const ArenaGunRules& rules, bool reloading, float progress, float clip) {
    ArenaGunLayer out;
    if (!reloading || !(clip > 0.f) || !std::isfinite(clip) || !std::isfinite(progress)) return out;
    const float p = std::clamp(progress, 0.f, 1.f);
    out.time = p * clip;
    const float edge = std::min(p, 1.f - p);
    out.weight = rules.reloadFade > 0.f ? std::clamp(edge / rules.reloadFade, 0.f, 1.f) : 1.f;
    return out;
}

ArenaGunLayer arenaFlinchLayer(const ArenaGunRules& rules, float age, float clip) {
    ArenaGunLayer out;
    if (!(age >= 0.f) || !(clip > 0.f) || !std::isfinite(clip) || !(age < clip)) return out;
    out.time = age;
    out.weight = rules.flinchFade > 0.f ? std::clamp((clip - age) / rules.flinchFade, 0.f, 1.f) : 1.f;
    return out;
}

const char* arenaGunCue(const ArenaEvent& event) {
    switch (event.type) {
    case ArenaEventType::ReloadStarted: return event.amount > 0 ? "SFX-GUN-RELOAD" : nullptr;
    case ArenaEventType::Reloaded: return "SFX-GUN-LOADED";
    case ArenaEventType::DryPress: return "SFX-GUN-DRY";
    default: return nullptr;
    }
}

} // namespace ftcore
