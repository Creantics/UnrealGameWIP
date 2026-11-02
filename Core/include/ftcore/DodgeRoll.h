#pragma once

// A dodge roll on demand: whether one may start, which kind it is, how fast it goes, when the body can't be
// hurt, and the stamina and cooldown it leaves behind. It holds no clip and no body: the host plays a roll
// clip at the dodge's speed, asks invulnerable() when a hit lands, and ticks it every frame. Explore and the
// arena can both drive one.
//
//   ftcore::DodgeRoll dodge;               // dodge.setTuning(t) from Data/Movement/movement.json
//   ftcore::DodgeState s = dodge.fresh();  // full stamina, ready
//   if (dodge.start(s, groundSpeed, hasDirection, clipSeconds)) play a roll at s.speed for s.length;
//   every frame: dodge.tick(s, dt); a hit while dodge.invulnerable(s) does nothing.
//
// The kind follows the body's speed: at longFrom or faster a Long roll (at the body's speed, but no slower than
// longSpeed), slower a Short one (at shortSpeed), both costing staminaCost. With less stamina than that left the roll is Tired: slow, spending what is left, and only
// tiredInvuln of the cover. With staminaCost 0 a dodge costs nothing and is never tired. needsDirection set,
// a dodge with no direction asked doesn't start.
//
// The body can't be hurt from invulnStart to invulnEnd into the roll (seconds; a Tired roll keeps the first
// tiredInvuln share of that). Another may start cooldown after one ends. Stamina comes back at staminaRegen
// per second once regenDelay has gone since it was last spent, up to staminaMax.
//
// Seconds and cm/s. Fixed size: nothing allocates.

#include "ftcore/Api.h"

#include <cstdint>

namespace ftcore {

enum class DodgeKind : std::uint8_t { None, Short, Long, Tired };

struct DodgeTuning {
    float length = 0.8f;        // how long a dodge lasts when the host gives no clip length (s)
    float invulnStart = 0.05f;  // the body can't be hurt from this far into the roll...
    float invulnEnd = 0.4f;     // ...to this (s)
    float cooldown = 0.25f;     // after a dodge ends, the next may start this long after (s)
    float staminaCost = 25.f;   // each dodge spends this (0: dodges cost nothing)
    float staminaMax = 100.f;
    float staminaRegen = 30.f;  // stamina back per second...
    float regenDelay = 1.f;     // ...once this long has gone since it was spent (s)
    float longFrom = 500.f;     // a body this fast or faster rolls Long (cm/s)
    float shortSpeed = 550.f;   // the speed each kind rolls at (cm/s; a Long one at least this, keeping the body's)
    float longSpeed = 650.f;
    float tiredSpeed = 300.f;
    float tiredInvuln = 0.f;    // the share of the cover a Tired roll keeps (0 to 1)
    float needsDirection = 0.f; // 1: a dodge needs a direction asked (0: none rolls the way the body faces)
};

struct DodgeState {
    DodgeKind kind = DodgeKind::None; // the dodge going on (None: none)
    float time = 0.f;      // how far into it (s)
    float length = 0.f;    // how long it lasts (s)
    float speed = 0.f;     // the speed it rolls at (cm/s)
    float stamina = 0.f;
    float cooldown = 0.f;  // left before another may start (s)
    float sinceSpent = 0.f; // since stamina was last spent (s)
    bool active() const { return kind != DodgeKind::None; }
};

class DodgeRoll {
public:
    void setTuning(const DodgeTuning& t) { tuning_ = t; }
    const DodgeTuning& tuning() const { return tuning_; }

    // Full stamina, ready to dodge.
    FTCORE_API DodgeState fresh() const;
    // Whether a dodge may start now: none going on, the cooldown gone, and a direction if one is needed.
    FTCORE_API bool canStart(const DodgeState& s, bool hasDirection) const;
    // The kind a dodge starting now would be, for a body at ground speed `speed` (None when it can't start).
    FTCORE_API DodgeKind kindFor(const DodgeState& s, float speed, bool hasDirection) const;
    // Starts a dodge when canStart: picks its kind, spends the stamina, and lasts `length` seconds (0: the
    // tuning's). False, and `s` unchanged, otherwise.
    FTCORE_API bool start(DodgeState& s, float speed, bool hasDirection, float length = 0.f) const;
    // Moves it on by dt: the dodge's time (ending it at its length), the cooldown and the stamina coming back.
    FTCORE_API void tick(DodgeState& s, float dt) const;
    // Ends a dodge now (the host's clip was cut short); the cooldown starts from here.
    FTCORE_API void stop(DodgeState& s) const;
    // Whether a hit now does nothing.
    FTCORE_API bool invulnerable(const DodgeState& s) const;

private:
    DodgeTuning tuning_;
};

} // namespace ftcore
