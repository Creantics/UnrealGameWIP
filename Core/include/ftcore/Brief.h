#pragma once

// Briefs: the Lattice lead's licence classes, each a kit laid over that lead's arena.json row (a third overlay,
// after "default" and the lead's own). Data/Battle/briefs.json:
//
//   { "version": 1,
//     "common": { "rounds": 6, "reload": 1.2, "shot": { "stamina": 0 } },
//     "briefs": [ { "id": "bailiff", "base": [14, 7, 12, 9, 8], "growth": [85, 30, 70, 35, 30],
//                   "kit": { "poise": 31.25, "light": [ { "damage": 12 }, ... ], ... } }, ... ] }
//
// * common: fighter keys every Brief shares (the cylinder), laid over the lead's row first.
// * briefs[].id: a BriefId name (lower case), each at most once. A file may list only some Briefs.
// * base: the five LicenceStat values at Rating 1 (each 1..50, summing to 50).
// * growth: the five per-Rating growths in hundredths (each 0..100, summing to 250).
// * kit: fighter keys of arena.json's form, listing only what differs from the lead's row plus common. It may not
//   name an id or a weapon.
//
// A Brief's kit is final once imported: the lead's row, then common, then its own kit. The stats (base, growth)
// are carried for the Rating slice and change nothing in a fight yet. Unknown keys anywhere refuse the file.

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

enum class LicenceStat : std::uint8_t { Reach, Drive, Nerve, Footwork, Insight, Count }; // append only
enum class BriefId : std::uint8_t { Counsel, Bailiff, Notary, Proxy, Escrow, Count };      // append only

inline constexpr std::size_t kLicenceStats = static_cast<std::size_t>(LicenceStat::Count);
inline constexpr std::size_t kBriefCount = static_cast<std::size_t>(BriefId::Count);
inline constexpr int kBriefBaseSum = 50;
inline constexpr int kBriefGrowthSum = 250;

FTCORE_API const char* briefName(BriefId b); // "counsel", "bailiff", "notary", "proxy", "escrow"; "none" past Count
// The Brief named `name` (exact, lower case); false for any other text.
FTCORE_API bool briefFromName(std::string_view name, BriefId& out);

struct BriefDef {
    bool present = false;                       // listed in the file
    std::array<int, kLicenceStats> base{};
    std::array<int, kLicenceStats> growth{};    // hundredths per Rating
    ArenaFighter kit;                           // the lead's row + common + this Brief's kit
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
