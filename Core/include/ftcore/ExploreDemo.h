#pragma once

// The exploration demo's layout and objectives: shared by the Unreal map builder (which places an
// actor for each spot), the Unreal demo at run time, and a headless test that walks it.
// Header-only, like BattleDemo.h. Positions are metres on the ground plane: +x runs from the start
// toward the gate, +y to the player's left as they set off.

#include "ftcore/Explore.h"

#include <array>
#include <cmath>
#include <cstdint>

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

} // namespace ftcore::demo
