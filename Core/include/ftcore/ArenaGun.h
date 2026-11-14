#pragma once

// The lead's gun between shots, for the host: the reload clip posed by the fight's reload progress (so it spans Core's
// reload whatever the clip's length, and stands still when the fight holds), the dry-press flinch, and the sound cue each
// cylinder event plays (Data/Audio/cues.json, the sfx bus). A missing clip (length 0) gives a layer of weight 0.

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"

namespace ftcore {

struct ArenaGunRules {
    float reloadFade = 0.1f;  // share of the reload the clip fades in over at its start and out over at its end
    float flinchFade = 0.05f; // s, the flinch's fade out at its clip's end
};

inline constexpr ArenaGunRules kArenaGun{};

struct ArenaGunLayer {
    float time = 0.f;   // s into the clip
    float weight = 0.f; // 0..1
};

// reloading: ArenaFight::reloading(); progress: ArenaFight::reloadProgress(); clip: the reload clip's length (s).
FTCORE_API ArenaGunLayer arenaReloadLayer(const ArenaGunRules& rules, bool reloading, float progress, float clip);

// age: seconds since the last dry press (below 0: none since the fight began); clip: the flinch clip's length (s).
FTCORE_API ArenaGunLayer arenaFlinchLayer(const ArenaGunRules& rules, float age, float clip);

// The cue a cylinder event plays: ReloadStarted "SFX-GUN-RELOAD" (none for an instant reload, amount 0), Reloaded
// "SFX-GUN-LOADED", DryPress "SFX-GUN-DRY"; nullptr for any other event. A literal: valid forever.
FTCORE_API const char* arenaGunCue(const ArenaEvent& event);

} // namespace ftcore
