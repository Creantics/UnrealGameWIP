#pragma once

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

// Field exploration rules, independent of any engine: the things on the map the player can use,
// which one a prompt points at, the objectives they advance, and encounter zones that start a
// fight. Positions are on the ground plane, in any unit as long as it is the same everywhere.
// Fixed capacity: nothing here allocates.

struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};

enum class InteractKind : std::uint8_t {
    Talk,      // a person: using it counts once toward its objective
    Pickup,    // an item: using it removes it
    Gate,      // opens once its requirements are done, then stays open (inactive)
    Encounter, // not used by a button: entering its radius starts a fight
};

enum class InteractResult : std::uint8_t {
    Used,   // it worked
    Locked, // a required objective isn't done yet
    Gone,   // already used up, or not a valid id
};

struct Interactable {
    Vec2 pos;
    float radius = 1.5f;
    InteractKind kind = InteractKind::Pickup;
    // The objective its use advances, or kNoObjective.
    std::uint8_t objective = 0xFF;
    // Bit i set: objective i must be done before this can be used (or, for an encounter, entered).
    std::uint32_t needs = 0;
};

// What a save holds: which interactables are used up and each objective's progress.
struct ExploreState {
    std::uint32_t used = 0;
    std::array<std::uint8_t, 8> progress{};
};

class Exploration {
public:
    static constexpr std::size_t kMaxInteractables = 32;
    static constexpr std::size_t kMaxObjectives = 8;
    static constexpr std::uint8_t kNoObjective = 0xFF;
    // The focus cone: an interactable more than this far off the facing direction is ignored.
    static constexpr float kFocusCos = 0.35f;

    // Returns the new objective's index, or -1 when full. `required` uses complete it.
    FTCORE_API int addObjective(std::uint8_t required);
    // Returns the new interactable's id, or -1 when full.
    FTCORE_API int add(const Interactable& item);

    std::size_t count() const { return count_; }
    std::size_t objectiveCount() const { return objectiveCount_; }
    const Interactable& item(int id) const { return items_[static_cast<std::size_t>(id)]; }

    FTCORE_API bool isUsed(int id) const;
    // Not used up and every required objective done.
    FTCORE_API bool isAvailable(int id) const;

    // The interactable a button press would use: the nearest one that isn't used up and isn't an
    // encounter, inside its radius of `player` and inside the focus cone of `facing` (a zero
    // facing skips the cone). Locked ones can be focused, so the prompt can say why. -1 for none.
    FTCORE_API int focus(Vec2 player, Vec2 facing) const;

    FTCORE_API InteractResult interact(int id);

    // The first available encounter whose radius contains `player`, or -1. It stays available
    // until clearEncounter, so a lost fight can be retried.
    FTCORE_API int encounterAt(Vec2 player) const;
    FTCORE_API void clearEncounter(int id);

    FTCORE_API std::uint8_t progress(int objective) const;
    std::uint8_t required(int objective) const { return required_[static_cast<std::size_t>(objective)]; }
    FTCORE_API bool objectiveDone(int objective) const;
    FTCORE_API bool allDone() const;
    // The first objective that isn't done, or -1 when all are.
    FTCORE_API int currentObjective() const;

    // Where a pilot should go next: the nearest available interactable that advances the current
    // objective (encounters included), measured from `player`. -1 when nothing does.
    FTCORE_API int pilotTarget(Vec2 player) const;

    FTCORE_API ExploreState save() const;
    // Restores a save made from the same set of interactables and objectives.
    FTCORE_API void load(const ExploreState& state);

private:
    void advance(std::uint8_t objective);
    bool requirementsMet(std::uint32_t mask) const;

    std::array<Interactable, kMaxInteractables> items_{};
    std::array<std::uint8_t, kMaxObjectives> required_{};
    std::array<std::uint8_t, kMaxObjectives> progress_{};
    std::uint32_t used_ = 0;
    std::size_t count_ = 0;
    std::size_t objectiveCount_ = 0;
};

} // namespace ftcore
