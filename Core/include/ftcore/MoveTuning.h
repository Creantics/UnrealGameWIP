#pragma once

// The movement moves' tunables, from Data/Movement/movement.json: the slide, the traversals (vaults, climbs,
// rolls and ledge catches), the landings and the dodge roll. Every member is optional and keeps its built-in default when
// left out, so the file only needs the numbers being tuned; a member that is there must be a number within
// its range, and a section or member the reader doesn't know is refused (a misspelt tunable would otherwise
// be silently ignored).
//
//   ftcore::MoveTuning t;
//   std::string error;
//   if (!ftcore::importMoveTuning(text, t, &error)) ...; // t is left as it was
//   slideSet.setTuning(t.slide);
//
// {"version": 1, "slide": {"friction": 400, ...}, "traversal": {...}, "landing": {...}, "dodge": {...}}
//
// Parsing allocates (load time only).

#include "ftcore/Api.h"
#include "ftcore/DodgeRoll.h"
#include "ftcore/Slide.h"
#include "ftcore/Traversal.h"

#include <string>
#include <string_view>

namespace ftcore {

// The landing rules Locomotion applies (LocoTuning's rollMinAir and rollDrop), copied into it by the host, and how
// long the host keeps a roll pressed in the air for the touchdown.
struct LandingTuning {
    float rollMinAir = 0.4f;  // a body touching down after this long in the air may roll out of it...
    float rollDrop = 250.f;   // ...and does, whatever it holds, once it has fallen this far (cm)
    float pressWindow = 0.3f; // a roll pressed in the air this long or less before touchdown rolls out of it (s)
};

struct MoveTuning {
    SlideTuning slide;
    TraversalTuning traversal;
    LandingTuning landing;
    DodgeTuning dodge;
};

// Reads `json` into `out`. On failure `out` is unchanged and `error` (if given) says which member and why.
FTCORE_API bool importMoveTuning(std::string_view json, MoveTuning& out, std::string* error = nullptr);

} // namespace ftcore
