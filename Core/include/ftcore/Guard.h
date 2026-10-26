#pragma once

// Field guards: enemies that walk a route on the field, notice the player, chase, and start a fight
// on contact.
//
//   Idle --(pause over)--> Patrol --(player seen)--> Notice --(reaction time)--> Chase
//     ^                      ^                          |                          |
//     +--(at a waypoint)-----+------- Return <----------+------ (lost sight) ------+
//                                       ^
//                                       +--- (touched the player: cooldown starts)
//
//   ftcore::GuardSet guards;
//   guards.add(spec);                                    // at spawn
//   every frame:
//     sight bit i = guards.inSenseRange(i, player) && <a line trace from guard i to the player is clear>
//     const int touched = guards.step(player, sight, dt);
//     place guard i at guards.position(i), facing guards.facing(i)
//     touched >= 0: start the fight with that guard
//
// * Perception: the player is noticed inside the vision cone (visionRange, visionHalfAngleCos) or
//   inside senseRadius. While chasing only the distance counts (loseRange), so sharp turns don't
//   lose the player. Geometry is the caller's: `sight` has one bit per guard, set when nothing
//   blocks the line between them. Core never knows walls; a caller with none passes ~0u.
// * Movement: a guard is placed, not steered. It walks straight between waypoints at walkSpeed,
//   at chaseSpeed toward the player, and back to its current waypoint at walkSpeed. It doesn't
//   collide with anything, so routes must stay clear of props.
// * Contact (closer than contactRadius) is reported once: the guard turns back to its route and
//   ignores the player for contactCooldown seconds. The caller decides what contact means (the
//   field demo starts a fight) and calls defeat() after a win.
// * A route of one point is a sentry: it stands there facing `facing`.
//
// Positions are metres on the ground plane. Deterministic for a fixed dt. Fixed capacity: nothing
// here allocates.

#include "ftcore/Api.h"
#include "ftcore/Explore.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

enum class GuardState : std::uint8_t { Idle, Patrol, Notice, Chase, Return };
FTCORE_API const char* guardStateName(GuardState s);

// One bit each in GuardEvents::bits.
enum GuardEvent : std::uint8_t {
    kGuardNoticed = 1,      // entered Notice: the player was just seen
    kGuardChaseStarted = 2, // entered Chase
    kGuardLost = 4,         // gave up (entered Return without contact)
    kGuardContact = 8,      // touched the player
};

struct GuardTuning {
    float walkSpeed = 1.5f;          // m/s on the route and on the way back
    float chaseSpeed = 4.f;          // m/s; between the player's walk (3.3) and run (6.2)
    float visionRange = 8.f;         // metres
    float visionHalfAngleCos = 0.64f; // cos of half the cone's angle (about 50 degrees)
    float senseRadius = 2.5f;        // noticed at this distance whatever the facing
    float loseRange = 12.f;          // chasing: lost beyond this distance
    float reactionTime = 0.5f;       // seconds in Notice before the chase
    float loseTime = 2.f;            // seconds out of sight before giving up the chase
    float pauseTime = 1.f;           // seconds at each waypoint
    float contactRadius = 0.8f;      // horizontal distance that counts as touching
    float contactCooldown = 4.f;     // seconds the player is ignored after contact
};

struct GuardSpec {
    static constexpr std::size_t kMaxWaypoints = 8;
    std::array<Vec2, kMaxWaypoints> route{}; // walked in order, then back to the first
    std::uint8_t routeCount = 1;             // 1: a sentry at route[0]
    Vec2 facing{1.f, 0.f};                   // starting facing (a sentry keeps it)
    GuardTuning tuning;
};

struct GuardEvents {
    std::array<std::uint8_t, 8> bits{}; // per guard, GuardEvent bits raised in the last step
};

// What survives a fight: which guards are defeated.
struct GuardSave {
    std::uint32_t defeated = 0;
};

class GuardSet {
public:
    static constexpr std::size_t kMaxGuards = 8;

    // Returns the new guard's id, or -1 when full or the route is empty. A route longer than
    // kMaxWaypoints is cut. The guard starts at route[0], in Idle.
    FTCORE_API int add(const GuardSpec& spec);

    std::size_t count() const { return count_; }
    const GuardSpec& spec(int id) const { return specs_[static_cast<std::size_t>(id)]; }
    FTCORE_API bool active(int id) const; // added and not defeated
    GuardState state(int id) const { return guards_[static_cast<std::size_t>(id)].state; }
    Vec2 position(int id) const { return guards_[static_cast<std::size_t>(id)].pos; }
    Vec2 facing(int id) const { return guards_[static_cast<std::size_t>(id)].facing; }
    float cooldown(int id) const { return guards_[static_cast<std::size_t>(id)].cooldown; }
    int waypoint(int id) const { return guards_[static_cast<std::size_t>(id)].waypoint; }

    // True when `target` is close enough for guard `id` to see it, ignoring walls: inside the cone
    // or sense radius, or within loseRange while chasing. Only these need a line trace.
    FTCORE_API bool inSenseRange(int id, Vec2 target) const;

    // Advances every active guard by `dt` seconds. `sight` bit i: nothing blocks the line from
    // guard i to `target`. Returns the lowest id that touched the target this step, or -1; the
    // events of every guard go to `events` when given.
    FTCORE_API int step(Vec2 target, std::uint32_t sight, float dt, GuardEvents* events = nullptr);

    // Takes the guard off the field for good (a won fight).
    FTCORE_API void defeat(int id);
    // Puts the guard back at its first waypoint, in Idle, with its timers cleared. Its cooldown is kept.
    FTCORE_API void reset(int id);
    // Every guard ignores the player for at least `seconds` (back on the field after a fight).
    FTCORE_API void holdOff(float seconds);

    FTCORE_API GuardSave save() const;
    // Restores the defeated guards of a save made from the same set.
    FTCORE_API void load(const GuardSave& save);

private:
    struct Guard {
        GuardState state = GuardState::Idle;
        Vec2 pos;
        Vec2 facing{1.f, 0.f};
        float timer = 0.f;     // seconds in Idle or Notice
        float lostTimer = 0.f; // Chase: seconds without sight
        float cooldown = 0.f;
        Vec2 lastSeen;         // Chase: where the player was last seen
        int waypoint = 0;      // the route point being walked to (or paused at)
        bool defeated = false;
    };

    bool sees(const Guard& g, const GuardTuning& t, Vec2 target, bool clear) const;

    std::array<GuardSpec, kMaxGuards> specs_{};
    std::array<Guard, kMaxGuards> guards_{};
    std::size_t count_ = 0;
};

} // namespace ftcore
