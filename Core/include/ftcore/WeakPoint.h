#pragma once

// Weak points and aim mode (see docs/BATTLE_AIM.md).
//
// Every enemy has 0..kMaxWeakPoints weak points: a spot on its figure with a radius and a damage
// multiplier. Aim is a party command: entering it reveals the target's weak points, the player
// moves a reticle over the figure and fires. A shot inside a weak point deals the aim damage times
// that point's multiplier; a shot elsewhere on the body deals it plain; a shot off the body grazes
// for less.
//
// Figure space, the only space Core knows: the unit is the figure's height. x runs across the
// figure as the camera sees it (0 the centre line, negative to the left), y runs up from the feet
// (0) to the top (1). The body is |x| <= halfWidth, 0 <= y <= 1. The renderer maps this onto the
// projected figure; nothing here knows about pixels.
//
// The reticle sways a little on a fixed Lissajous path (AimTuning::sway*), so a shot is a
// timing as well as a position. The aim point at time t is reticle + sway(t), which is what a shot
// at t is judged on. The pilot reads reticleFor() to put the aim point on a weak point exactly.
//
// Nothing here allocates except importWeakPointTable / applyWeakPoints (load time).
//
//   ftcore::AimShot aim;
//   aim.reset(enemy.weakPoints, now, tuning.aim);
//   aim.move(dx, dy);                    // input, in figure units
//   const ftcore::ShotResult r = aim.judge(now);
//
// Data/Battle/weakpoints.json, keyed by the enemy ids of Data/Battle/enemies.json:
//   { "version": 1, "weakpoints": {
//       "brute": { "halfWidth": 0.5,
//                  "points": [ { "id": "core", "x": 0.0, "y": 0.55, "radius": 0.12, "multiplier": 2.5 } ] } } }
//   `halfWidth` in (0, 1], default 0.5. 0..3 points; ids are lowercase [a-z0-9_], 1..15 characters,
//   unique per enemy; x in [-1, 1], y in [0, 1], radius in [0.05, 0.5], multiplier in [1, 4].
//   An enemy id the roster doesn't know is a warning (the entry is kept but never used); an enemy
//   without an entry has no weak points (aim still works at the body multiplier).

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct Combatant;
struct EnemyRoster;
struct EncounterLineup;

inline constexpr std::size_t kMaxWeakPoints = 3;
inline constexpr std::size_t kWeakPointIdCapacity = 16; // bytes, terminator included
// Data limits (the loader refuses anything outside them).
inline constexpr float kMinWeakRadius = 0.05f;
inline constexpr float kMaxWeakRadius = 0.5f;
inline constexpr float kMinWeakMultiplier = 1.0f;
inline constexpr float kMaxWeakMultiplier = 4.0f;
inline constexpr float kDefaultHalfWidth = 0.5f;

struct WeakPoint {
    std::array<char, kWeakPointIdCapacity> idChars{}; // fixed buffer: copying a combatant never allocates
    float x = 0.0f;          // across the figure, in figure heights from its centre line
    float y = 0.5f;          // up from the feet, in figure heights
    float radius = 0.12f;    // in figure heights
    float multiplier = 2.0f; // damage of a shot that lands inside it, times the aim damage

    std::string_view id() const { return std::string_view(idChars.data()); }
    // Copies at most kWeakPointIdCapacity - 1 bytes.
    void setId(std::string_view id) {
        idChars.fill('\0');
        const std::size_t n = id.size() < kWeakPointIdCapacity ? id.size() : kWeakPointIdCapacity - 1;
        for (std::size_t i = 0; i < n; ++i) idChars[i] = id[i];
    }
};

// One enemy's weak points (fixed capacity) and the width of its body.
struct WeakPoints {
    std::array<WeakPoint, kMaxWeakPoints> items{};
    std::uint8_t count = 0;
    float halfWidth = kDefaultHalfWidth;

    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const WeakPoint& operator[](std::size_t i) const { return items[i]; }
    const WeakPoint* begin() const { return items.data(); }
    const WeakPoint* end() const { return items.data() + count; }
    void clear() { count = 0; }
    // Appends a point; false (and nothing added) when full.
    bool add(const WeakPoint& p) {
        if (count >= kMaxWeakPoints) return false;
        items[count++] = p;
        return true;
    }
    // Index of the point with the highest multiplier (first on ties); size() when empty.
    std::size_t best() const {
        std::size_t out = count;
        for (std::size_t i = 0; i < count; ++i)
            if (out == count || items[i].multiplier > items[out].multiplier) out = i;
        return out;
    }
};

FTCORE_API bool sameWeakPoints(const WeakPoints& a, const WeakPoints& b);

// Aim as a party command.
struct AimTuning {
    int apCost = 2;               // spent on entering aim; refunded by cancelling before the shot
    float power = 1.4f;           // aim damage, like a skill's power (before the shot's multiplier)
    double timeLimit = 5.0;       // seconds in aim before the shot fires on its own where the reticle is
    float bodyMultiplier = 1.0f;  // a shot on the body that misses every weak point
    float grazeMultiplier = 0.5f; // a shot off the body
    float margin = 0.4f;          // the reticle may leave the body by this much on each side
    float swayX = 0.035f;         // sway amplitude (figure heights) and period (seconds) on each axis
    float swayY = 0.025f;
    double swayPeriodX = 1.3;
    double swayPeriodY = 1.9;
};

enum class ShotKind : std::uint8_t { Weak, Body, Graze };
// "WEAK POINT", "HIT", "GRAZE". A literal: valid forever.
FTCORE_API const char* shotKindName(ShotKind k);

struct AimPoint {
    float x = 0.0f;
    float y = 0.5f;
};

struct ShotResult {
    ShotKind kind = ShotKind::Graze;
    std::uint8_t weakPoint = kMaxWeakPoints; // index of the weak point hit (kMaxWeakPoints: none)
    float multiplier = 0.0f;
    AimPoint at;                             // the aim point the shot was judged on
};

// Where a shot lands at `at` on a figure with `points`: the weak point with the highest multiplier
// whose circle holds it (edge included), else the body, else a graze.
FTCORE_API ShotResult judgeShot(const WeakPoints& points, AimPoint at, const AimTuning& tuning);

class AimShot {
public:
    // Starts aiming at a figure with `points` at time `start`, the reticle at the body's centre.
    FTCORE_API void reset(const WeakPoints& points, double start, const AimTuning& tuning = {});

    // Moves / places the reticle, clamped to the body plus the margin.
    FTCORE_API void move(float dx, float dy);
    FTCORE_API void set(float x, float y);

    AimPoint reticle() const { return reticle_; }
    // The sway offset at t (0 at the start).
    FTCORE_API AimPoint sway(double t) const;
    // Reticle plus sway: what a shot at t is judged on.
    FTCORE_API AimPoint aimPoint(double t) const;
    ShotResult judge(double t) const { return judgeShot(points_, aimPoint(t), tuning_); }
    // The reticle position that puts the aim point at t on weak point i's centre (the body centre
    // when there is no such point).
    FTCORE_API AimPoint reticleFor(std::size_t i, double t) const;

    double start() const { return start_; }
    double deadline() const { return start_ + tuning_.timeLimit; }
    bool expired(double t) const { return t >= deadline(); }
    const WeakPoints& weakPoints() const { return points_; }
    const AimTuning& tuning() const { return tuning_; }
    // The reticle's bounds.
    float minX() const { return -points_.halfWidth - tuning_.margin; }
    float maxX() const { return points_.halfWidth + tuning_.margin; }
    float minY() const { return -tuning_.margin * 0.5f; }
    float maxY() const { return 1.0f + tuning_.margin * 0.5f; }

private:
    WeakPoints points_{};
    AimTuning tuning_{};
    double start_ = 0.0;
    AimPoint reticle_{};
};

// Data/Battle/weakpoints.json in memory.
struct WeakPointEntry {
    std::string enemy; // enemy id
    WeakPoints points;
};

struct WeakPointTable {
    std::vector<WeakPointEntry> entries;
    // The weak points of that enemy id, or nullptr. No allocation.
    FTCORE_API const WeakPoints* find(std::string_view enemyId) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line. Enemy ids are
// checked against `roster` (unknown ones are warnings).
FTCORE_API bool importWeakPointTable(std::string_view json, const EnemyRoster& roster, WeakPointTable& out,
                                     std::string* error = nullptr, std::vector<std::string>* warnings = nullptr);

// Gives foes[i] the weak points of the enemy at lineup.enemies[i] (the order makeEnemies builds
// them in); an enemy without an entry gets none.
FTCORE_API void applyWeakPoints(const WeakPointTable& table, const EnemyRoster& roster, const EncounterLineup& lineup,
                                std::vector<Combatant>& foes);

} // namespace ftcore
