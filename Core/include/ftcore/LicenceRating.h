#pragma once

// The Lattice lead's licence Rating (his level, 1..30), its xp curve, fight xp, and the free stat points
// (allowances) it earns. Only xp and the allowance spends are stored; the Rating, the unspent allowances and the
// stats are always derived from them and the Brief, so a re-brief recomputes everything at the current Rating.
//
// * need(R) = 100 + 20 (R - 1): xp from R to R + 1. T(R) = 100 (R - 1) + 10 (R - 1)(R - 2): xp to hold R.
//   T(30) = 11020 is the xp cap; xp past it is dropped.
// * Fight xp = round(need(R*) x q x c), R* the fight's pacing target, q by the fight's kind (standard 0.35, elite or
//   race 0.6, tutorial 0.5, boss 1.25), c = clamp(1 + 0.35 (R* - R), 0.5, 2.0) the catch-up. Whole-number
//   arithmetic, half up.
// * Allowances: floor(R / 2) earned (0 at Rating 1, 15 at 30); each adds 1 to one stat, never past kStatMax (a
//   spend that would pass it is refused and the point stays unspent).
// * A Brief's stat S(R) = min(50, base + floor(growth x (R - 1) / 100) + spent), growth in hundredths.
//
// Save: 9 bytes, {format version 1, Brief (BriefId; Count = none), xp as uint16 little-endian, five spends}.

#include "ftcore/Api.h"
#include "ftcore/Brief.h"
#include "ftcore/LicenceStats.h"

#include <array>
#include <cstdint>

namespace ftcore {

inline constexpr int kMaxRating = 30;
inline constexpr int kRatingNeedBase = 100;
inline constexpr int kRatingNeedStep = 20;
inline constexpr int kAllowanceEvery = 2;

FTCORE_API int ratingNeed(int rating);    // xp from `rating` to the next (defined at 30 too: 680)
FTCORE_API int ratingXpTotal(int rating); // T(R); rating held to 1..kMaxRating
inline constexpr int kMaxRatingXp = 100 * (kMaxRating - 1) + 10 * (kMaxRating - 1) * (kMaxRating - 2); // 11020
FTCORE_API int ratingForXp(int xp);       // 1..kMaxRating
FTCORE_API int ratingAllowances(int rating);

enum class FightXpKind : std::uint8_t { Standard, Elite, Tutorial, Boss, Count }; // append only (race = Elite)
// Xp a won fight gives: target is the fight's pacing Rating, rating the lead's true Rating going in (both 1..30).
FTCORE_API int fightXp(int target, FightXpKind kind, int rating);

struct LicenceRating {
    int xp = 0;
    std::array<std::uint8_t, kLicenceStats> spent{};

    FTCORE_API int rating() const;
    FTCORE_API int spentTotal() const;
    FTCORE_API int unspent() const; // allowances earned at the true Rating less those spent (never negative)
    // Adds xp up to the cap; returns the xp actually gained (0 once capped, or for a negative amount).
    FTCORE_API int addXp(int amount);
    // Puts one allowance into `stat` for `brief`. False (nothing changes) when none is unspent or the stat is
    // already at kStatMax at the true Rating.
    FTCORE_API bool place(LicenceStat stat, const BriefDef& brief);
    void refund() { spent = {}; }
    // The Brief's five stats at `statsRating` (the true Rating, or a fight's floor) with the spends added.
    FTCORE_API LicenceStats statsFor(const BriefDef& brief, int statsRating) const;
    LicenceStats statsFor(const BriefDef& brief) const { return statsFor(brief, rating()); }
};

inline constexpr std::uint8_t kLicenceSaveVersion = 1;
inline constexpr std::size_t kLicenceSaveBytes = 9;
using LicenceSaveBytes = std::array<std::uint8_t, kLicenceSaveBytes>;

FTCORE_API LicenceSaveBytes encodeLicence(BriefId brief, const LicenceRating& r);
// False (outputs untouched) for another version, a Brief past Count, xp past kMaxRatingXp or a spend past kStatMax.
// Spends that add up to more than the Rating has earned are refunded (all to unspent) rather than refused.
FTCORE_API bool decodeLicence(const LicenceSaveBytes& bytes, BriefId& brief, LicenceRating& r);

} // namespace ftcore
