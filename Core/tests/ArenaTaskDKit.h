#pragma once

// A lead's kit with part of Task D's data (11-22: the soft-target limits and the string end), so a test can show which
// part moved a fight. Task D's code with none of its data steps to the bit as before it (test_arena_partner.cpp's
// classic rows against f9d5c7c9, main before Task D).

#include "ftcore/ArenaFight.h"

#include <cstdint>

namespace ftcore_test {

enum class TaskDPart : std::uint8_t {
    None,    // none of it: softTurn and homing -1 (the old path), push 0, the string end's hitStop the light before it's
    HitStop, // + the string end's hitStop (0.10)
    Push,    // + its push and pushTime
    All,     // the kit as given (+ the soft-target limits)
};

inline ftcore::ArenaFighter taskDKit(const ftcore::ArenaFighter& kit, TaskDPart part) {
    if (part == TaskDPart::All) return kit;
    ftcore::ArenaFighter f = kit;
    const ftcore::ArenaAttack d{};
    auto strip = [&d](ftcore::ArenaAttack& a) {
        a.softTurn = d.softTurn;
        a.homing = d.homing;
        a.push = d.push;
        a.pushTime = d.pushTime;
    };
    strip(f.heavy);
    strip(f.riposte);
    strip(f.shot);
    strip(f.jumpLight);
    strip(f.release);
    for (std::uint8_t k = 0; k < f.lightCount; ++k) strip(f.light[k]);
    if (f.lightCount >= 2) {
        ftcore::ArenaAttack& end = f.light[f.lightCount - 1];
        const ftcore::ArenaAttack& given = kit.light[kit.lightCount - 1];
        end.hitStop = part == TaskDPart::None ? f.light[f.lightCount - 2].hitStop : given.hitStop;
        if (part == TaskDPart::Push) {
            end.push = given.push;
            end.pushTime = given.pushTime;
        }
    }
    return f;
}

inline ftcore::ArenaFighter preFeatureKit(const ftcore::ArenaFighter& kit) { return taskDKit(kit, TaskDPart::None); }

} // namespace ftcore_test
