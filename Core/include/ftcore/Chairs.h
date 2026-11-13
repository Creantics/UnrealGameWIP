#pragma once

// What a person in one of the lead's chairs does for the lead in an arena fight (Places.h holds the chairs).
//
// Data/Battle/chairs.json maps a person id to one effect:
//
//   { "version": 1, "chairs": [ { "person": 1, "key": "chair_a01", "kind": "loadGain", "a": 0.25, "b": 4 }, ... ] }
//
// * person: a PersonId, 1..65535 (0 is no one), unique.
// * key: the neutral word a test switch uses (lower case, unique). Never a name.
// * kind and its two numbers:
//   - loadGain:    a = added Load gain (0..0.5), b = the ally's radius in m (1..15): while the ally is alive and
//                  within b of the lead, every store of Load but the host's gift gains 1 + a.
//   - healCharges: a = whole heal charges added at the fight's begin (0..3), b unused (0).
//   - takeHalf:    a = seconds added to the take half window (0..0.2), b = the share of the ally's max hp it heals
//                  on each take half (0..0.1).
//   - releaseMark: a = the bonus a Release's mark adds to the next joined count on its target (0..0.5), b = the
//                  mark's seconds (1..15): the joined blows on the marked enemy take x (1 + a).
//
// chairsFor gives the effects of a PlaceTable's Named chairs, in chair order, as a fixed array the fight can copy
// with no allocation. Totals: each kind sums a (and b where it is a value, not a radius) over the Named chairs of
// that kind, then clamps at its cap below.

#include "ftcore/Api.h"
#include "ftcore/Places.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class ChairKind : std::uint8_t { None, HealCharges, LoadGain, TakeHalf, ReleaseMark }; // append only

// "none", "healCharges", "loadGain", "takeHalf", "releaseMark". Literals.
FTCORE_API const char* chairKindName(ChairKind k);

struct ChairEffect {
    ChairKind kind = ChairKind::None;
    float a = 0.f;
    float b = 0.f;
};

struct ChairRow {
    PersonId person = kNoPerson;
    std::string key;
    ChairEffect effect;
};

struct ChairTable {
    std::vector<ChairRow> rows;
    FTCORE_API const ChairRow* findKey(std::string_view key) const;  // nullptr if none
    FTCORE_API const ChairRow* findPerson(PersonId person) const;    // nullptr if none
};

// False on a refused file: `out` is empty and *error names each problem with its path, one per line. Refused: a
// wrong version, an unknown kind, a value out of its range, a non-whole a for healCharges, a duplicate person or
// key, person 0 (or past 65535), a missing or empty key.
FTCORE_API bool importChairTable(std::string_view json, ChairTable& out, std::string* error = nullptr);

inline constexpr int kChairHealChargesCap = 3;           // total added charges
inline constexpr float kChairLoadGainCap = 0.5f;         // total added gain
inline constexpr float kChairTakeHalfWindowCap = 0.2f;   // total added window, s
inline constexpr float kChairTakeHalfHealCap = 0.15f;    // total ally heal share
inline constexpr float kChairReleaseMarkCap = 0.5f;      // total mark bonus (StrikeCount.h kMaxReleaseMark)
inline constexpr float kChairReleaseMarkSecondsCap = 15.f; // the mark's seconds

// The effects of a fighter's Named chairs, in chair order (fixed size: the fight copies it, no allocation).
struct ArenaChairs {
    std::array<ChairEffect, kMaxPlaces> named{};
    std::uint8_t count = 0;

    // The totals, each clamped at its cap. loadGainRadius: the widest b among the loadGain chairs (0: none).
    FTCORE_API int healCharges() const;
    FTCORE_API float loadGain() const;
    FTCORE_API float loadGainRadius() const;
    FTCORE_API float takeHalfWindow() const;
    FTCORE_API float takeHalfHeal() const;
    // The mark a Release leaves: the summed bonus (capped) and the longest seconds among the releaseMark chairs
    // (0: none).
    FTCORE_API float releaseMark() const;
    FTCORE_API float releaseMarkSeconds() const;
};

// Named chairs only; a person with no row adds nothing (and takes no slot).
FTCORE_API ArenaChairs chairsFor(const PlaceTable& places, const ChairTable& table);

} // namespace ftcore
