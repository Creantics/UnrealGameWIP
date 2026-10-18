#pragma once

// The emotion map: turns a VO line's direction (emotion_primary, emotion_secondary, intensity 1-5)
// into one expression pose for FacePlayer::setExpression (newengine docs/rigging_plan.md §7.1-7.2).
//
// Each emotion word names one of the face set's expression presets (the 14 base poses), a weight
// for it and optional modifier shapes, e.g. tender = warm 0.7 + browInnerUp 0.3:
//     pose(word) = weight * base + modifiers
// resolveEmotion composes a line's pose:
//     k   = min(0.2 * intensity, cap)                       (intensity clamped to 1..5)
//     out = k * pose(primary)                               on every shape
//         + kSecondaryWeight * pose(secondary)              on Upper and Eyes shapes only
// clamped to [0, 1]. The secondary is what leaks through, so it shows in the brows, lids and
// cheeks, and it isn't scaled by intensity. `cap` is the character's containment cap (1 = none).
// Play the result at full intensity: player.setExpression(&out, 5.f, fade).
//
// Not covered here (rigging_plan §7.2): per-character containment shape changes beyond the cap,
// micro-expression flashes, the listening state, emphasis brow flicks and pause blink slots.

#include "ftcore/Api.h"
#include "ftcore/Face.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr float kSecondaryWeight = 0.35f;

struct EmotionDef {
    std::string name;
    int base = -1;            // index into FaceShapeSet::expressions
    float weight = 1.f;       // of the base pose
    FaceWeights modifiers{};  // added on top, indexed like FaceShapeSet::shapes
};

struct EmotionMap {
    std::vector<EmotionDef> emotions;

    FTCORE_API const EmotionDef* find(std::string_view name) const;
    // Adds `name` = weight * the face set's expression `base` + modifiers. False (and why) on a
    // duplicate name, an unknown base expression or an unknown modifier shape.
    FTCORE_API bool add(const FaceShapeSet& set, std::string_view name, std::string_view base, float weight,
                        std::span<const FaceShapeWeight> modifiers = {}, std::string* error = nullptr);
};

// 0.2 per intensity step (1..5, clamped), capped at `cap`.
FTCORE_API float emotionIntensityWeight(int intensity, float cap = 1.f);

// Writes the composed pose into out.weights (out.name is left alone). An empty secondary means
// none. False when a non-empty name isn't in the map: that part is then left out. No allocation.
FTCORE_API bool resolveEmotion(const FaceShapeSet& set, const EmotionMap& map, std::string_view primary,
                               std::string_view secondary, int intensity, float cap, FacePreset& out);

// Loads emotion_map.json: {"emotions": [{"name", "base", "weight"?, "add"?: {shape: weight}}]}.
// Names resolve against `set`, which must be finished. False (and `*error`) on bad JSON or a bad
// entry; `out` is then empty. Load time only.
FTCORE_API bool importEmotionMap(std::string_view json, const FaceShapeSet& set, EmotionMap& out,
                                 std::string* error = nullptr);

} // namespace ftcore
