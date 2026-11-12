#pragma once

// Impact feedback for the arena host: the camera jolt a blow gives, and the tells an enemy winds up out of the
// player's view. Pure queries and a small value type; no fight state, nothing allocates.
//
// * Jolt (arenaJoltOf, ArenaJolt). A blow that lands kicks the camera's view by a few degrees and lets it settle:
//   rotation only, never the arm's length or the field of view, so the camera never pulls out. How hard is data:
//   each attack's `jolt` (arena.json, degrees 0..kArenaMaxJolt), the player's moves when they hit an enemy, an
//   enemy's move when it hits the player (the kit's `hurtJolt` when that move names none). A blocked blow kicks
//   half as hard. The player's setting scales it (Settings::cameraShake, 0..1; 0 turns it off). A new kick replaces
//   the one running only when it is stronger than what is left of it, so a blow that lands on three enemies
//   kicks once, not three times.
// * Off-frame tells (arenaOffFrameTells). An enemy in an attack's startup whose direction from the camera, on
//   the ground plane, lies outside the view's horizontal half-angle less a margin. Each comes with its bearing
//   (degrees from the view's forward, + to the right: Unreal's ground plane has x forward and y right) and how
//   far its startup has run (0..1), so the host can draw a marker at the screen's edge and sound a stinger
//   once per windup. The marker is the tell; the stinger only doubles it, so the tell reads with sound off.
// * Pace (arenaPaceText). The fight's time-to-kill per enemy slot and its punish windows (ArenaStats), as one
//   line of numbers for the headless run's check lines. It judges nothing: the numbers wait for a play report.

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

inline constexpr float kArenaMaxJolt = 3.f;       // degrees: the most any attack's jolt may ask for
inline constexpr float kArenaJoltLength = 0.22f;  // s a kick takes to settle
inline constexpr float kArenaJoltCycles = 1.5f;   // swings over that time
inline constexpr float kArenaJoltYawShare = 0.35f; // the side-to-side part of a kick, as a share of its pitch

// The jolt a fight event asks for, in degrees before the player's setting: a Hit by the player (its move's
// jolt), a Hit on the player (the enemy move's jolt, else the kit's hurtJolt), a Blocked blow (half of the
// enemy move's). 0 for any other event or an unknown move.
FTCORE_API float arenaJoltOf(const ArenaDef& def, const ArenaEvent& e);

// A camera kick and its settling. Offsets in degrees; add them to the view, never to the control rotation.
struct ArenaJolt {
    float amount = 0.f; // degrees at the kick
    float age = 0.f;    // s since the kick
    float side = 1.f;   // +1 or -1: which way the yaw part swings first

    // Kicks by degrees * scale (each clamped: degrees to 0..kArenaMaxJolt, scale to 0..1, NaN as 0), when that is
    // more than what is left of the kick running. side < 0 swings left first.
    FTCORE_API void kick(float degrees, float scale, float sideSign);
    // Advances by dt seconds (NaN or below 0: no change).
    FTCORE_API void step(float dt);
    // What is left of the kick: amount * envelope, 0 once it has settled.
    FTCORE_API float left() const;
    // The view's offsets now: pitch (+ up) and yaw (+ right). Both 0 once settled.
    FTCORE_API float pitch() const;
    FTCORE_API float yaw() const;
    bool active() const { return left() > 0.f; }
};

// The view the off-frame test is made against: the camera's place and facing on the ground plane (metres), and its
// horizontal half-angle in degrees (half the field of view the camera actually uses).
struct ArenaView {
    Vec2 eye{};
    Vec2 forward{1.f, 0.f};
    float halfAngle = 45.f;
    float margin = 6.f; // degrees inside the edge that already count as out of frame (the body's half-width)
};

// An enemy as the off-frame test sees it.
struct ArenaTellBody {
    Vec2 pos{};
    bool winding = false; // in an attack's startup
    float fill = 0.f;     // how far the startup has run, 0..1
};

struct ArenaOffFrameTell {
    std::uint8_t enemy = kArenaNobody;
    float bearing = 0.f; // degrees from the view's forward, + right, -180..180
    float fill = 0.f;
};

struct ArenaOffFrameTells {
    std::array<ArenaOffFrameTell, kMaxArenaEnemies> items{};
    std::size_t count = 0;
};

// The winding bodies out of `view`, in index order. A body at the eye counts as in frame. At most
// kMaxArenaEnemies bodies are read.
FTCORE_API ArenaOffFrameTells arenaOffFrameTells(const ArenaView& view, const ArenaTellBody* bodies, std::size_t count);

// Enemy i of the fight as the off-frame test sees it, at `pos`: winding while its output is in an attack's
// startup, fill from its move's startup time.
FTCORE_API ArenaTellBody arenaTellBody(const ArenaFight& fight, std::size_t i, Vec2 pos);

// The bearing, in degrees (+ right, -180..180), of `to` from `eye` looking along `forward`.
FTCORE_API float arenaBearing(Vec2 eye, Vec2 forward, Vec2 to);

// "pace: kills 3, ttk 0 9.4 s, 1 12.0 s, 2 n/a; punish 4/6 windows used" into buf (always NUL-terminated when
// cap > 0); returns its length. Time-to-kill per enemy slot is the mean over its kills (counted from the player's
// first blow on it), n/a for a slot never killed. With no kill and no window it reads "pace: n/a".
FTCORE_API std::size_t arenaPaceText(const ArenaStats& stats, std::size_t enemyCount, char* buf, std::size_t cap);

} // namespace ftcore
