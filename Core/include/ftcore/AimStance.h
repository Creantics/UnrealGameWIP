#pragma once

// The lead's aim (right mouse held, everywhere the leads are playable: the arena, the field and the world maps, in or
// out of a fight) and the attack button's tap and hold. Rules only; the hosts move the camera, turn the body and point
// the weapon by what these say.
//
// AimStance: while aim is held and the body allows it, the stance blends in (blendIn seconds, 0 -> 1) and the host pulls
// the camera in over the right shoulder (armScale, shoulder offset, fov, all lerped by alpha()), shows a centre
// reticle, turns the body to the camera's aim (turnRate) and slows its moves to a strafe (speedScale()). Let go, it
// blends back out (blendOut). Blocked: seated (in a vehicle or boarding it), hanging, traversing and sliding cancel
// the aim at once and it blends out; rolling holds it out for the roll (it comes back if still held after); falling
// keeps it. A host in a menu or a pause passes held = false.
//
// What the lead's attack does while aiming (aimKindOf, from the kit's weapon): guns fire one round per light press at
// the reticle (the kit's shot; the hit test is the shot's lane along the facing toward the reticle point), and kick
// again once the aim lets go, magic casts its bolt at the reticle, melee (the
// gauntlets, kicks) is a focus stance whose light attack lunges toward the reticle point (lungeScale times the attack's
// own lunge, stopping short of the point).
//
// PressHold: the attack button (left mouse) as light on a tap and heavy on a hold. A release before heavyHold seconds
// is a light; holding to heavyHold is a heavy, once per press, and its release does nothing more. With `single` (a gun
// kit aiming) the press is one light at once, held or not: no heavy can come of it, and a heavy waiting is dropped.
//
// The tunables are in Data/Battle/aim.json (importAimRules). Nothing allocates after import.

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/FieldStrike.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

struct AimRules {
    float blendIn = 0.15f;      // s, off -> full aim
    float blendOut = 0.25f;     // s, full aim -> off
    float speedScale = 0.45f;   // share of the move speed while fully aimed (a strafe walk)
    float turnRate = 14.f;      // rad/s the body turns toward the camera's aim
    float armScale = 0.45f;     // share of the camera's boom length while fully aimed
    float shoulderRight = 55.f; // cm, the camera's offset right of the boom while fully aimed
    float shoulderUp = 70.f;    // cm, up
    float fov = 55.f;           // degrees while fully aimed
    float reticleRange = 50.f;  // m, how far the reticle's line is traced
    float lungeScale = 2.f;     // a melee light attack's lunge while aimed, times the attack's own
    float lungeStop = 0.6f;     // the lunge stops this share of the attack's range short of the reticle point
    float heavyHold = 0.25f;    // s held on the attack button for a heavy
};

// Reads Data/Battle/aim.json over `out` (keys absent keep out's values). False with a reason in `error` on a bad
// file: not an object, an unknown key, a value not a number or out of its range.
FTCORE_API bool importAimRules(std::string_view json, AimRules& out, std::string* error);

enum class AimPhase : std::uint8_t { Off, In, Held, Out };

// Whether aim may be held while the body does this (seated, hanging, traversing, sliding, rolling: no).
inline constexpr bool aimAllowed(FieldBody body) {
    return body == FieldBody::Free || body == FieldBody::Falling;
}

class FTCORE_API AimStance {
public:
    void setRules(const AimRules& rules) { rules_ = rules; }
    const AimRules& rules() const { return rules_; }
    // Back to off at once (a new body, a scene change).
    void reset() {
        alpha_ = 0.f;
        phase_ = AimPhase::Off;
    }
    // One frame: held = the aim button is down and the host allows it (no menu, pause or vehicle); body = what the
    // body does. Returns the phase.
    AimPhase tick(float dt, bool held, FieldBody body);
    AimPhase phase() const { return phase_; }
    // 0 (off) .. 1 (fully aimed), eased; the camera blends by it.
    float alpha() const;
    // Whether the lead aims now: held and allowed (In or Held). Attacks go at the reticle while it does.
    bool aiming() const { return phase_ == AimPhase::In || phase_ == AimPhase::Held; }
    // The move speed's scale now: 1 off, speedScale fully aimed.
    float speedScale() const { return 1.f + (rules_.speedScale - 1.f) * alpha(); }
    // Lerps an off value to its aimed value by alpha().
    float blend(float off, float aimed) const { return off + (aimed - off) * alpha(); }

private:
    AimRules rules_{};
    float alpha_ = 0.f; // linear 0..1
    AimPhase phase_ = AimPhase::Off;
};

enum class AimKind : std::uint8_t { Focus, Fire, Cast };

// What an aimed attack does for a kit: guns fire, magic casts, the rest (gauntlets, kicks) take the focus stance.
inline constexpr AimKind aimKindOf(const ArenaFighter& kit) {
    return kit.weapon == ArenaWeapon::Guns ? AimKind::Fire
        : kit.weapon == ArenaWeapon::Magic ? AimKind::Cast
                                           : AimKind::Focus;
}

// The kind of one of the kit's moves (AttackMove's numbers): a guns kit's shot (kArenaShotMove) fires, its kicks take
// the focus numbers; any other kit as aimKindOf.
inline constexpr AimKind aimKindOfMove(const ArenaFighter& kit, std::uint8_t move) {
    return kit.weapon == ArenaWeapon::Guns ? (move == kArenaShotMove ? AimKind::Fire : AimKind::Focus) : aimKindOf(kit);
}

// The attack button as tap = light, hold = heavy.
class FTCORE_API PressHold {
public:
    struct Out {
        bool light = false;
        bool heavy = false;
    };
    // One frame: down = the button is down now. A press shorter than `hold` gives a light on its release; reaching
    // `hold` gives a heavy at once, or, while busy (an attack under way the host can't queue a heavy into), on the
    // first frame it isn't, unless a new press has begun by then. With `single` a new press is a light on its first
    // frame and nothing after; a press already under way gives no heavy while `single` holds (its release is a light
    // unless it already gave one), and a heavy still waiting is dropped.
    Out tick(float dt, bool down, float hold, bool busy = false, bool single = false);
    // Forgets a press under way (the button's input was taken away: a menu, the vehicle).
    void reset() {
        downFor_ = -1.f;
        heavyDone_ = false;
        heavyPending_ = false;
    }
    bool heavyPending() const { return heavyPending_; }
    bool pressing() const { return downFor_ >= 0.f; }

private:
    float downFor_ = -1.f; // s the press has lasted; below 0: up
    bool heavyDone_ = false;
    bool heavyPending_ = false;
};

// How far (radians, signed) the body turns this frame toward the aim, `gap` radians away, at the aim's turnRate.
FTCORE_API float aimTurnStep(const AimRules& rules, float gap, float dt);

} // namespace ftcore
