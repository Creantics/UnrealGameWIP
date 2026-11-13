#pragma once

// BriefKit: a Brief's stats turned into the arena fields the guns lead fights with.
//
// For each stat-driven field: mult = 1 + k x D(S) (D in thousandths, LicenceStats.h), then the row's stat clamp,
// either on the multiplier ("x": base x clamp(mult)) or on the value ("abs": clamp(base x mult)), then the Brief's
// kit change P (BriefChange, 1 if none), then the final clamp, then one rounding (whole fields half up, others to 4
// decimals half up). All of it is whole-number arithmetic, so every platform builds the same kit.
//
// The bases are the Brief's own kit (BriefDef::kit: the lead's row, then common, then the Brief's non-stat keys),
// held to 0..1000. At every stat 10 each mult is 1, so the kit is the lead's own with only the changes applied.
//
//   stat      field                                         k       stat clamp         final clamp     P
//   Reach     light[i].damage, heavy.damage, riposte.damage  0.040   x 0.80..1.70       1..200          kickDamage
//   Reach     release.damage                                 0.040   x 0.80..1.70       1..200          releaseDamage
//   Reach     light[i].poiseDamage, heavy.poiseDamage        0.050   x 0.80..1.80       1..120          kickPoiseDamage
//   Drive     shot.damage                                    0.050   x 0.80..1.80       1..60           shotDamage
//   Drive     shot.poiseDamage                               0.030   x 0.85..1.45       1..20           shotPoiseDamage
//   Drive     term duration (not a fighter field)            0.030   x 0.85..1.50       0.5..2.0        termDuration
//   Nerve     hp                                             0.035   x 0.85..1.60       90..230         hp
//   Nerve     poise                                          0.040   x 0.85..1.70       15..60          poise
//   Nerve     stamina                                        0.020   x 0.90..1.35       85..140         -
//   Nerve     guard leak (1 - guardAbsorb)                  -0.030   x 0.50..1.15       absorb .80..95  guardLeak
//   Nerve     parryWindow (base + 0.0015 D)                  add     abs 0.12..0.155    0.11..0.17      + parryWindowAdd
//   Footwork  staminaRegen                                   0.025   x 0.90..1.40       40..65          -
//   Footwork  invuln L (dodgeInvulnEnd - dodgeInvulnStart)   0.012   abs 0.32..0.40     0.25..0.40      invuln
//   Footwork  dodgeDistance                                  0.008   abs 3.30..3.80     2.8..3.8        dodgeDistance
//   Footwork  moveSpeed                                      0.004   abs 4.80..5.10     4.7..5.1        -
//   Insight   readPower                                      0.050   abs 0.40..0.95     0.3..1.2        readPower
//   Insight   readStackPower                                 0.030   abs 0.15..0.32     0.1..0.4        readStackPower
//   Insight   readPoise                                      0.060   abs 4..11          3..12           -
//   Insight   loadShare                                      0.030   x 0.90..1.45       0.8..2.0        loadShare
//
// The guard leak is rounded (4 decimals, half up) before absorb = 1 - leak. dodgeInvulnEnd = dodgeInvulnStart + L,
// never past 0.45 or dodgeTime.

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/Brief.h"
#include "ftcore/LicenceStats.h"

namespace ftcore {

struct BriefKitBuild {
    ArenaFighter fighter;
    float termDuration = 1.f; // the multiplier a stated term's duration takes (Drive); read by the words slice
};

FTCORE_API BriefKitBuild buildBriefKit(const BriefDef& brief, const LicenceStats& stats);

} // namespace ftcore
