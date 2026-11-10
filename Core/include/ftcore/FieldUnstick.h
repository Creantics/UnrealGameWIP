#pragma once

// Getting the lead out of places it can't walk out of: a slope too steep to stand on and a wedge (a wall against a
// steep bank) it falls into and can't leave.
//
// Each host frame: where the body is and what it stands on. On a slope steeper than the walkable limit the lead
// slides down it (sliding(), slideDir(): the host pushes the body downhill at slideSpeed and may still jump). While on
// walkable ground the position goes into a small ring of recent safe points, one every ringSpacing metres. Two
// rescues step the lead back to a safe point (the newest one at least backoff metres away, else the oldest): no
// walkable ground and under stuckMove metres of net movement (height counted) for stuckTime seconds, or falling (in
// the air, not on any ground) for longer than fallTime. A rescue fades the screen for fadeTime seconds (fade()).

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

struct UnstickRules {
    float walkableZ = 0.7071f; // a floor's normal z (cos of the slope) at or over which it is walkable (45 degrees)
    float slideSpeed = 4.f;    // m/s down a too-steep slope
    float stuckTime = 1.5f;
    float stuckMove = 0.5f;
    float fallTime = 4.f;
    float ringSpacing = 1.f;
    float backoff = 1.5f;
    float fadeTime = 0.35f;
};

// Reads Data/Movement/unstick.json (the tunables above by name). Absent keys keep `out`'s values; false and `out`
// unchanged on a parse error, an unknown key or a value out of range.
FTCORE_API bool importUnstickRules(std::string_view json, UnstickRules& out, std::string* error = nullptr);

struct UnstickPoint {
    Vec2 at;
    float z = 0.f;
};

class FTCORE_API FieldUnstick {
public:
    static constexpr std::uint8_t kRing = 8;

    void setRules(const UnstickRules& r) { rules_ = r; }
    const UnstickRules& rules() const { return rules_; }
    // Forgets the ring and the timers (a teleport the host made, a map change).
    void reset();

    // One frame. onGround: the body touches some floor (walking or resting on a slope); floorNormal: that floor's
    // normal (x, y on the ground, z up; unit); inAir: the body is falling or jumping. Returns true on the frame a
    // rescue fires; rescueTo() is then where to put the feet.
    bool tick(float dt, Vec2 at, float z, bool onGround, Vec2 floorNormalXY, float floorNormalZ, bool inAir);

    // Sliding down a too-steep floor this frame, and the downhill way (unit on the ground).
    bool sliding() const { return sliding_; }
    Vec2 slideDir() const { return slideDir_; }
    const UnstickPoint& rescueTo() const { return rescueTo_; }
    // 1 at the rescue, fading to 0 over fadeTime: the host's screen fade.
    float fade() const { return fadeLeft_ > 0.f && rules_.fadeTime > 0.f ? fadeLeft_ / rules_.fadeTime : 0.f; }
    std::uint8_t safeCount() const { return count_; }
    // Seconds without walkable ground and seconds in the air, as counted now.
    float stuckFor() const { return stuckFor_; }
    float fallingFor() const { return fallFor_; }

private:
    void push(Vec2 at, float z);
    bool rescue(Vec2 at);

    UnstickRules rules_{};
    UnstickPoint ring_[kRing] = {};
    std::uint8_t head_ = 0; // the next slot
    std::uint8_t count_ = 0;
    float stuckFor_ = 0.f;
    float fallFor_ = 0.f;
    Vec2 anchor_{};
    float anchorZ_ = 0.f;
    bool anchored_ = false;
    bool sliding_ = false;
    Vec2 slideDir_{};
    UnstickPoint rescueTo_{};
    float fadeLeft_ = 0.f;
};

} // namespace ftcore
