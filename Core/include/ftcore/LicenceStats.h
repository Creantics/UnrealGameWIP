#pragma once

// The Lattice lead's five licence stats and the stat curve that turns one into effect.
//
// * Stats are whole numbers 1..kStatMax. Stat 10 is the lead's own arena kit exactly (no effect either way).
// * D(S), the stat's points of effect above 10: S - 10 up to 10, then (S - 10) * H / (H + S - 10) with the knee
//   H = 30 (diminishing returns). It is kept in thousandths, truncated toward zero, so every later step is whole-number
//   arithmetic and derived values never land on an accidental .5. D(1) = -9.000, D(22) = 8.571, D(50) = 17.142.

#include "ftcore/Api.h"

#include <array>
#include <cstdint>

namespace ftcore {

enum class LicenceStat : std::uint8_t { Reach, Drive, Nerve, Footwork, Insight, Count }; // append only

inline constexpr std::size_t kLicenceStats = static_cast<std::size_t>(LicenceStat::Count);
inline constexpr int kStatMin = 1;
inline constexpr int kStatMax = 50;
inline constexpr int kStatPlain = 10; // the lead's own kit
inline constexpr int kStatKnee = 30;

struct LicenceStats {
    std::array<int, kLicenceStats> v{kStatPlain, kStatPlain, kStatPlain, kStatPlain, kStatPlain};
    int operator[](LicenceStat s) const { return v[static_cast<std::size_t>(s)]; }
    bool operator==(const LicenceStats&) const = default;
};

FTCORE_API const char* licenceStatName(LicenceStat s); // "reach", "drive", "nerve", "footwork", "insight"
// D(S) in thousandths (truncated). S is held to kStatMin..kStatMax first.
FTCORE_API int licenceCurveMilli(int stat);

} // namespace ftcore
