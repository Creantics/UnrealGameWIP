#pragma once

// Where a field step's goal marker stands, per story step (Data/Field/goals.json, FieldGoalTable).
//
//   ftcore::FieldGoalTable table;
//   if (!ftcore::importFieldGoalTable(text, table, &error)) ...;     // allocates: at load
//   const ftcore::FieldGoalEntry* e = table.findEntry(sceneId, beat, step);   // no allocation
//
// Ids and numbers only, no text. Positions are metres on the Explore map's ground plane, the same
// frame as ExploreDemo.h (+x from the start toward the gate, +y to the player's left). A goal is
// the marker the player walks to and uses; the optional `via` is a waypoint the autopilot passes
// first (so the step takes a longer, bent walk). Goals and waypoints must lie on walkable ground:
// x in kMinX..kMaxX, y in -kMaxAbsY..kMaxAbsY (the village courtyard, short of the inner wall at
// x = 30).
//
// goals.json:
//   { "version": 1,
//     "default": { "goal": { "x": 6, "y": 8 } },
//     "goals": [ { "scene": "SC-X", "beat": 4, "step": 0, "goal": { "x": 10, "y": -8 },
//                  "via": { "x": 5, "y": -2 } } ] }
//   `default` and `via` are optional. `step` defaults to 0. A duplicate (scene, beat, step) or a
//   missing or out-of-range position is an error; unknown keys are warnings.

#include "ftcore/Api.h"
#include "ftcore/Explore.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct FieldGoalEntry {
    std::string scene;
    std::uint32_t beat = 0, step = 0;
    Vec2 goal;
    Vec2 via;
    bool hasVia = false;
};

struct FieldGoalTable {
    static constexpr float kMinX = -10.f;
    static constexpr float kMaxX = 28.f;
    static constexpr float kMaxAbsY = 24.f;

    FieldGoalEntry fallback; // the "default" goal (valid when hasFallback)
    bool hasFallback = false;
    std::vector<FieldGoalEntry> entries;

    // The entry for (scene, beat, step), or nullptr. No allocation.
    FTCORE_API const FieldGoalEntry* findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importFieldGoalTable(std::string_view json, FieldGoalTable& out, std::string* error = nullptr,
                                     std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
