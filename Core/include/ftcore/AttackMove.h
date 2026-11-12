#pragma once

// Attacking on the move: how much of the stick's walk applies, and how fast the lead may turn, in each phase of an
// attack, and how much of the strike plays on the upper body only (the legs keep the walk) rather than on the whole
// body.
//
// The stick's share of the walking pace is set per phase: startupScale through startup, activeScale through active
// (and from `commit` s before contact), recoveryScale through recovery up to the move's cancel point (the `cancel`
// share of its recovery, when another action may start), and from the cancel point on the walk is free again (full
// pace); with recoveryRamp the recovery instead eases from its scale back to full pace over its whole length. The
// arena ramps the walk toward that share at the fighter's brake (slowing) and accel (speeding up), so the pace the
// attack began at slides to the phase's share rather than stopping dead; the attack's own lunge comes on top. The body
// turns at up to turnRate rad/s the whole time (never held). A light goes by the light's numbers; a heavy, a riposte
// and a Release (which the arena host moves by the heavy's) by the heavy's. The numbers come per weapon (AimKind:
// gauntlets and bare hands Focus, guns Fire, the unused magic kit Cast), from Data/Battle/attack_move.json; a gun's
// shot keeps walking (its legs stay on the walk under the upper-body share), a fist's heavy stops through its active
// part and its light slows.
//
// The upper-body share (attackUpperShare): 0 while the body stands (the full-body strike plays) up to 1 once it moves
// at upperFull m/s (the strike from the spine up, the legs on the walk cycle).

#include "ftcore/AimStance.h"
#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"

#include <string>
#include <string_view>

namespace ftcore {

struct AttackMoveTune {
    float startupScale = 0.f;  // the stick's share of the walking pace through startup
    float activeScale = 0.f;   // ... through active (and the last `commit` s of startup)
    float recoveryScale = 0.f; // ... through recovery up to the cancel point (full pace from there)
    float turnRate = 6.f;      // rad/s the body may turn while the attack plays (above 0: the facing is never held)
    float commit = 0.f;        // s before contact from which activeScale holds (0: from contact)
    bool recoveryRamp = false; // the recovery eases from recoveryScale to full pace over its whole length
};

struct AttackMoveRules {
    // By AimKind; the shipped table's values, so a lead with no data file moves as with it.
    AttackMoveTune light[3] = {{0.4f, 0.55f, 0.7f, 6.f}, {0.65f, 0.65f, 0.65f, 7.f, 0.f, true}, {0.4f, 0.55f, 0.7f, 6.f}};
    AttackMoveTune heavy[3] = {{0.3f, 0.f, 0.4f, 4.f}, {0.45f, 0.3f, 0.3f, 5.f, 0.25f, true}, {0.3f, 0.f, 0.4f, 4.f}};
    float upperNone = 0.15f; // m/s at or under which the strike plays on the whole body
    float upperFull = 0.6f;  // m/s at or over which it plays on the upper body only
};

// Reads Data/Battle/attack_move.json: {"light": {"focus"|"fire"|"cast": {startupScale, activeScale, recoveryScale,
// turnRate, commit, recoveryRamp}}, "heavy": {...the same}, "upperNone", "upperFull"}. Absent keys keep `out`'s
// values. False (and `out` unchanged) on a parse error, an unknown key or a value out of range (each scale 0..1,
// turnRate 0.5..60, commit 0..2, recoveryRamp a bool, upperNone < upperFull, both 0..10): no value may hold the
// lead's facing.
FTCORE_API bool importAttackMoveRules(std::string_view json, AttackMoveRules& out, std::string* error = nullptr);

struct AttackMoveOut {
    float speedScale = 1.f; // of the walking pace
    float turnRate = 0.f;   // rad/s (0 only off attack)
};

// What an attack allows `t` seconds into it: the phase's scale (startup, active from `commit` before contact,
// recovery before the cancel point), 1 from the cancel point (activeEnd + attack.cancel of the recovery) on; with
// recoveryRamp the recovery eases from its scale to 1 over its whole length instead, cancel point or not. t past its total: full pace, no turn limit
// from it (turnRate 0 and speedScale 1, as off attack). During an attack turnRate is always above 0.
FTCORE_API AttackMoveOut attackMove(const AttackMoveRules& rules, AimKind kind, bool heavy, const ArenaAttack& attack,
                                    float t);

// Sets in.attackSpeed and in.attackTurn for the attack the fight's last output shows under way, as the arena host
// does for every step, the player's and the autopilot's alike: a light by the light's numbers; a heavy, a riposte and
// a Release by the heavy's. Off attack (or a move the kit has no attack for) leaves both as they are.
FTCORE_API void attackMoveInput(const AttackMoveRules& rules, const ArenaFight& fight, ArenaInput& in);

// The share (0..1) of the strike that plays on the upper body only, for a body moving at `speed` m/s on the ground.
FTCORE_API float attackUpperShare(const AttackMoveRules& rules, float speed);

// `from` turned toward `to` (both unit on the ground) by at most maxRad radians, the shorter way. A zero `to` keeps
// `from`.
FTCORE_API Vec2 turnCapped(Vec2 from, Vec2 to, float maxRad);

} // namespace ftcore
