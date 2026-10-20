#pragma once

// The exploration demo's layout and objectives: shared by the Unreal map builder (which places an
// actor for each spot), the Unreal demo at run time, and a headless test that walks it.
// Header-only, like BattleDemo.h. Positions are metres on the ground plane: +x runs from the start
// toward the gate, +y to the player's left as they set off.

#include "ftcore/Explore.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ftcore::demo {

enum ExploreObjective : std::uint8_t {
    kObjTalk = 0,   // talk to Duy
    kObjShards = 1, // pick up the three light shards
    kObjGate = 2,   // open the old gate
    kObjAmbush = 3, // win the fight beyond it
    kObjCount = 4,
};

struct ExploreSpot {
    InteractKind kind;
    Vec2 pos;
    float radius;
    std::uint8_t objective;
    std::uint32_t needs;
};

inline constexpr std::array<std::uint8_t, kObjCount> kExploreRequired = {1, 3, 1, 1};

// In id order. The ambush sits beyond the gate, so its radius stays clear of the gateway.
inline constexpr std::array<ExploreSpot, 6> kExploreSpots = {{
    {InteractKind::Talk, {8.f, 3.f}, 2.5f, kObjTalk, 0},
    {InteractKind::Pickup, {5.f, 14.f}, 1.8f, kObjShards, 1u << kObjTalk},
    {InteractKind::Pickup, {22.f, 9.f}, 1.8f, kObjShards, 1u << kObjTalk},
    {InteractKind::Pickup, {14.f, -10.f}, 1.8f, kObjShards, 1u << kObjTalk},
    {InteractKind::Gate, {30.f, 0.f}, 3.f, kObjGate, 1u << kObjShards},
    {InteractKind::Encounter, {38.f, 0.f}, 3.f, kObjAmbush, 1u << kObjGate},
}};

inline constexpr Vec2 kExploreStart = {0.f, 0.f};

inline Exploration makeExploration() {
    Exploration e;
    for (std::uint8_t required : kExploreRequired) e.addObjective(required);
    for (const ExploreSpot& s : kExploreSpots) {
        Interactable it;
        it.kind = s.kind;
        it.pos = s.pos;
        it.radius = s.radius;
        it.objective = s.objective;
        it.needs = s.needs;
        e.add(it);
    }
    return e;
}

// One pilot step: walk `pos` toward the pilot target at `speed` for `dt` seconds, facing the way it
// walks; use the target once it has focus. Returns the id it used (or the encounter it entered),
// or -1. An encounter is only reported; the caller runs the fight and calls clearEncounter.
inline int pilotStep(Exploration& e, Vec2& pos, Vec2& facing, float speed, float dt) {
    const int target = e.pilotTarget(pos);
    if (target < 0) return -1;
    const Vec2 goal = e.item(target).pos;
    const float dx = goal.x - pos.x;
    const float dy = goal.y - pos.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist > 1e-4f) facing = {dx / dist, dy / dist};
    if (e.item(target).kind == InteractKind::Encounter) {
        const int hit = e.encounterAt(pos);
        if (hit >= 0) return hit;
    } else if (e.focus(pos, facing) == target) {
        return e.interact(target) == InteractResult::Used ? target : -1;
    }
    const float step = speed * dt < dist ? speed * dt : dist;
    pos.x += facing.x * step;
    pos.y += facing.y * step;
    return -1;
}

// ---- Story field step: reach one marker ----
// A field step from the story (ENG or PLAY) hides the demo spots and shows one goal marker, chosen
// from kFieldGoals by (scene, beat, step). The step is done when the player stands at the marker
// and presses Use, or, as a fallback, when kFieldFallbackSeconds have passed. Every goal is a
// straight, prop-free line from kExploreStart that the pilot walks in well under 10 s.

// The spot id the Unreal marker actor carries (outside kExploreSpots).
inline constexpr int kFieldGoalSpotId = 100;
inline constexpr float kFieldGoalRadius = 1.8f;
inline constexpr float kFieldFallbackSeconds = 300.f;

inline constexpr std::array<Vec2, 6> kFieldGoals = {{
    {6.f, 8.f},
    {10.f, -8.f},
    {12.f, 10.f},
    {20.f, 12.f},
    {22.f, -9.f},
    {16.f, -6.f},
}};

// Props the straight walk from the start must clear (circles, metres; the radius covers the prop).
// Mirrors the map builder: Duy, the fountain, houses, the terrace, crates, barrels, lamps, trees.
struct FieldObstacle {
    Vec2 pos;
    float radius;
};
inline constexpr std::array<FieldObstacle, 30> kFieldObstacles = {{
    {{8.f, 3.f}, 0.4f}, // Duy's blocker
    {{14.f, 0.f}, 1.7f}, // fountain
    {{-5.f, 15.f}, 4.5f}, // houses
    {{-5.f, -16.f}, 4.5f},
    {{6.f, -19.f}, 4.2f},
    {{19.f, -19.f}, 4.5f},
    {{15.f, 19.f}, 4.8f},
    {{41.f, 18.f}, 4.5f},
    {{-4.f, -9.5f}, 4.8f}, // terrace and ramp
    {{-1.4f, 13.f}, 0.6f}, // crates
    {{-1.5f, 13.9f}, 0.6f},
    {{22.5f, -15.8f}, 0.5f}, // barrels
    {{23.4f, -16.2f}, 0.5f},
    {{27.5f, -4.f}, 0.3f}, // lamps
    {{27.5f, 4.f}, 0.3f},
    {{-9.f, 23.f}, 1.7f}, // trees
    {{-9.f, -23.f}, 1.7f},
    {{-9.f, 6.f}, 1.7f},
    {{2.f, 23.f}, 1.7f},
    {{26.f, 22.f}, 1.7f},
    {{26.f, -22.f}, 1.7f},
    {{12.f, -23.f}, 1.7f},
    {{47.f, 23.f}, 1.7f},
    {{47.f, -23.f}, 1.7f},
    {{36.f, 11.f}, 1.7f},
    {{36.f, -11.f}, 1.7f},
    {{45.f, 9.f}, 1.7f},
    {{45.f, -9.f}, 1.7f},
    {{30.f, 14.5f}, 0.1f}, // inner wall ends (the gateway itself is open)
    {{30.f, -14.5f}, 0.1f},
}};

// Distance from point p to the segment a-b.
inline float segmentDistance(Vec2 a, Vec2 b, Vec2 p) {
    const float abx = b.x - a.x;
    const float aby = b.y - a.y;
    const float len2 = abx * abx + aby * aby;
    float t = len2 > 1e-8f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / len2 : 0.f;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    const float dx = a.x + abx * t - p.x;
    const float dy = a.y + aby * t - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Which kFieldGoals entry a step uses: deterministic in (scene, beat, step), spread by FNV-1a.
inline std::size_t fieldGoalIndex(std::string_view scene, int beat, int step) {
    std::uint32_t h = 2166136261u;
    for (const char c : scene) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 16777619u;
    }
    h ^= static_cast<std::uint32_t>(beat) * 0x9E3779B1u;
    h *= 16777619u;
    h ^= static_cast<std::uint32_t>(step) * 0x85EBCA6Bu;
    h *= 16777619u;
    h ^= h >> 15;
    return static_cast<std::size_t>(h % kFieldGoals.size());
}

// The player can use the marker: inside its radius and inside the focus cone of `facing` (a zero
// facing skips the cone), the same rule as Exploration::focus.
inline bool fieldGoalInReach(Vec2 pos, Vec2 facing, Vec2 goal) {
    const float dx = goal.x - pos.x;
    const float dy = goal.y - pos.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist > kFieldGoalRadius) return false;
    const float flen = std::sqrt(facing.x * facing.x + facing.y * facing.y);
    if (flen < 1e-4f || dist < 1e-4f) return true;
    return (dx * facing.x + dy * facing.y) / (dist * flen) >= Exploration::kFocusCos;
}

// A press of Use finishes the step: at the marker, or once the fallback time has passed.
inline bool fieldStepUsable(bool reached, float elapsedSeconds) {
    return reached || elapsedSeconds >= kFieldFallbackSeconds;
}

// One pilot step toward the marker; true once it is in reach (the caller then uses it).
inline bool fieldPilotStep(Vec2& pos, Vec2& facing, Vec2 goal, float speed, float dt) {
    const float dx = goal.x - pos.x;
    const float dy = goal.y - pos.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist > 1e-4f) facing = {dx / dist, dy / dist};
    if (fieldGoalInReach(pos, facing, goal)) return true;
    const float step = speed * dt < dist ? speed * dt : dist;
    pos.x += facing.x * step;
    pos.y += facing.y * step;
    return false;
}

} // namespace ftcore::demo
