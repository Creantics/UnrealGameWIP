#pragma once

// Attacking on the move: how fast the lead may walk and how fast it may turn while an attack plays, and how much of
// the strike plays on the upper body only (the legs keep the walk) rather than on the whole body.
//
// A light never roots the lead: it walks at speedScale of its pace through startup and active, back up to full pace
// over the recovery, and turns toward the stick or the camera at up to turnRate rad/s the whole time. A heavy commits:
// it walks at speedScale until `commit` seconds before its hit frame (the start of active), then stands rooted
// through active, still turning at up to turnRate until the hit frame and not at all after it; the recovery ramps
// back up from standing to full pace. The numbers come per weapon (AimKind: gauntlets Focus, guns Fire, magic Cast),
// from Data/Battle/attack_move.json.
//
// The upper-body share (attackUpperShare): 0 while the body stands (the full-body strike plays, no foot slide) up to
// 1 once it moves at upperFull m/s (the strike from the spine up, the legs on the walk cycle).

#include "ftcore/AimStance.h"
#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"

#include <string>
#include <string_view>

namespace ftcore {

struct AttackMoveTune {
    float speedScale = 0.6f; // share of the walking pace while the attack plays (a heavy: before its commit)
    float turnRate = 6.f;    // rad/s the body may turn while the attack plays (a heavy: up to its hit frame)
    float commit = 0.f;      // a heavy's root: seconds before its hit frame it stops walking (a light: unused)
};

struct AttackMoveRules {
    AttackMoveTune light[3] = {{0.6f, 6.f, 0.f}, {0.6f, 7.f, 0.f}, {0.6f, 6.f, 0.f}};    // by AimKind
    AttackMoveTune heavy[3] = {{0.4f, 4.f, 0.3f}, {0.4f, 5.f, 0.25f}, {0.4f, 4.f, 0.3f}}; // by AimKind
    float upperNone = 0.15f; // m/s at or under which the strike plays on the whole body
    float upperFull = 0.6f;  // m/s at or over which it plays on the upper body only
};

// Reads Data/Battle/attack_move.json: {"light": {"focus"|"fire"|"cast": {speedScale, turnRate}}, "heavy": {... commit},
// "upperNone", "upperFull"}. Absent keys keep `out`'s values. False (and `out` unchanged) on a parse error, an unknown
// key or a value out of range (speedScale 0..1, turnRate 0..60, commit 0..2, upperNone < upperFull, both 0..10).
FTCORE_API bool importAttackMoveRules(std::string_view json, AttackMoveRules& out, std::string* error = nullptr);

struct AttackMoveOut {
    float speedScale = 1.f; // of the walking pace
    float turnRate = 0.f;   // rad/s, 0: the facing holds
    bool rooted = false;    // a heavy's commit: no walking at all
};

// What an attack allows `t` seconds into it (t past its total: full pace, no turn limit from it: turnRate 0 and
// speedScale 1, as off attack).
FTCORE_API AttackMoveOut attackMove(const AttackMoveRules& rules, AimKind kind, bool heavy, const ArenaAttack& attack,
                                    float t);

// The share (0..1) of the strike that plays on the upper body only, for a body moving at `speed` m/s on the ground.
FTCORE_API float attackUpperShare(const AttackMoveRules& rules, float speed);

// `from` turned toward `to` (both unit on the ground) by at most maxRad radians, the shorter way. A zero `to` keeps
// `from`.
FTCORE_API Vec2 turnCapped(Vec2 from, Vec2 to, float maxRad);

} // namespace ftcore
