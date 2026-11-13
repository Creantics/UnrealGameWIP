#pragma once

// The Lift's tuning and the blow cap, from Data/Battle/count_lift.json (FightSession def.liftTuning, def.blowCap):
//
//   { "version": 1, "countLoad": 10, "countLoadEvery": 8, "soloCountLoad": 5, "liftThreshold": 120,
//     "liftMultiplier": 1.5, "liftCost": 30, "blowCap": 6 }
//
// Every field but version may be left out (its default stays). Ranges as StrikeCount.h validLiftTuning (checked
// with the default count tuning) and validBlowCap.

#include "ftcore/Api.h"
#include "ftcore/StrikeCount.h"

#include <string>
#include <string_view>

namespace ftcore {

struct CountLiftData {
    LiftTuning lift{};
    float blowCap = kDefaultBlowCap;
};

// False on a refused file: `out` keeps the defaults and *error names each problem, one per line. Refused: a wrong
// version, a field of the wrong type or out of its range, a non-whole count, an unknown field.
FTCORE_API bool importCountLift(std::string_view json, CountLiftData& out, std::string* error = nullptr);

} // namespace ftcore
