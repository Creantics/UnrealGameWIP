#pragma once

// Briefs: the Lattice lead's licence classes, each a kit laid over that lead's arena.json row (a third overlay,
// after "default" and the lead's own). Data/Battle/briefs.json:
//
//   { "version": 1,
//     "common": { "rounds": 6, "reload": 1.2, "shot": { "stamina": 0 } },
//     "briefs": [ { "id": "bailiff", "base": [14, 7, 12, 9, 8], "growth": [85, 30, 70, 35, 30],
//                   "change": { "kickDamage": 1.10, "poise": 1.25, ... },
//                   "kit": { "shot": { "startup": 0.26 } } }, ... ] }
//
// * common: fighter keys every Brief shares (the cylinder), laid over the lead's row first.
// * briefs[].id: a BriefId name (lower case), each at most once. A file may list only some Briefs.
// * base: the five LicenceStat values at Rating 1 (each 1..50, summing to 50).
// * growth: the five per-Rating growths in hundredths (each 0..100, summing to 250).
// * change: the Brief's kit changes on stat-driven fields (BriefChange; BriefKit.h applies them after the stats).
//   Multipliers 0.5..2, adds as noted. Each key is optional.
// * kit: fighter keys of arena.json's form for fields no stat drives (timings, ranges, pairWindow, releasePoise),
//   listing only what differs from the lead's row plus common. It may not name an id or a weapon, nor a
//   stat-driven field (those go through change).
//
// BriefDef::kit is the lead's row, then common, then the Brief's kit: the bases BriefKit builds from. A fight's kit is
// buildBriefKit(def, stats); at every stat 10 that is the bases with the changes applied. Unknown keys anywhere
// refuse the file.

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/LicenceStats.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

enum class BriefId : std::uint8_t { Counsel, Bailiff, Notary, Proxy, Escrow, Count };      // append only

inline constexpr std::size_t kBriefCount = static_cast<std::size_t>(BriefId::Count);
inline constexpr int kBriefBaseSum = 50;
inline constexpr int kBriefGrowthSum = 250;

FTCORE_API const char* briefName(BriefId b); // "counsel", "bailiff", "notary", "proxy", "escrow"; "none" past Count
// The Brief named `name` (exact, lower case); false for any other text.
FTCORE_API bool briefFromName(std::string_view name, BriefId& out);

// A Brief's kit changes on stat-driven fields (BriefKit.h's P). Multipliers default to 1, adds to 0.
// A change's bounds: multipliers 0.5..2, parryWindowAdd -0.05..0.05. The import refuses values outside them and
// buildBriefKit holds to them as well (a non-finite one counts as no change).
inline constexpr float kBriefChangeMin = 0.5f;
inline constexpr float kBriefChangeMax = 2.f;
inline constexpr float kBriefParryAddMax = 0.05f;

struct BriefChange {
    float hp = 1.f;
    float poise = 1.f;
    float guardLeak = 1.f;       // on 1 - guardAbsorb
    float parryWindowAdd = 0.f;  // seconds, -0.05..0.05
    float kickDamage = 1.f;      // light, heavy and riposte damage (never release)
    float kickPoiseDamage = 1.f; // light and heavy poiseDamage
    float releaseDamage = 1.f;
    float shotDamage = 1.f;
    float shotPoiseDamage = 1.f;
    float termDuration = 1.f;
    float readPower = 1.f;
    float readStackPower = 1.f;
    float loadShare = 1.f;
    float dodgeDistance = 1.f;
    float invuln = 1.f;          // on the dodge's invulnerable length
};

struct BriefDef {
    bool present = false;                       // listed in the file
    std::array<int, kLicenceStats> base{};
    std::array<int, kLicenceStats> growth{};    // hundredths per Rating
    BriefChange change;
    ArenaFighter kit;                           // the lead's row + common + this Brief's kit (BriefKit's bases)
};

struct BriefTable {
    std::array<BriefDef, kBriefCount> briefs{};
    // The Brief's def if the file listed it, else nullptr.
    FTCORE_API const BriefDef* find(BriefId b) const;
};

// False on a refused file: `out` is left empty and *error names each problem with its path, one per line. `lead` is
// the lead's arena kit (ArenaTable::fighter) the kits are laid over.
FTCORE_API bool importBriefTable(std::string_view json, const ArenaFighter& lead, BriefTable& out,
                                 std::string* error = nullptr);

} // namespace ftcore
