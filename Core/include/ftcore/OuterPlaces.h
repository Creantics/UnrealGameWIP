#pragma once

// The outer field's places: districts, rest points and other named-by-id spots that stand outside the
// inner city's district table. Read from Data/Field/ring_places.json; ids and metres only (any names
// live in the text tables, keyed by id).
//
//   {
//     "version": 1,
//     "places": [ { "id": 3, "kind": "district", "x": 1200.0, "y": -340.5, "radius": 250.0 } ]
//   }
//
// kind is "district", "rest_point" or "place"; x and y are plan-frame metres (MassingPlan.h), radius
// metres >= 0. Ids are unique whole numbers 0..65535. Unknown members are refused.

#include "ftcore/Api.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct OuterPlace {
    enum class Kind : std::uint8_t { District, RestPoint, Place };
    std::uint16_t id = 0;
    Kind kind = Kind::Place;
    float x = 0.0f, y = 0.0f;
    float radius = 0.0f;
};

// "district", "rest_point", "place".
FTCORE_API std::string_view outerPlaceKindName(OuterPlace::Kind kind);

// Reads the places in file order. On a failure out is left empty and error says what and where.
FTCORE_API bool importOuterPlaces(std::string_view json, std::vector<OuterPlace>& out, std::string* error = nullptr);

// The inner city's districts take discovery indices 0..15 (WorldLayout::kMaxDistricts); the outer
// districts follow from here in file order.
inline constexpr int kOuterDistrictFirstIndex = 16;

// The discovery index of places[index]: kOuterDistrictFirstIndex plus the number of districts before it
// in the list. -1 when index is out of range or the place is not a district.
FTCORE_API int outerDistrictDiscoveryIndex(const std::vector<OuterPlace>& places, std::size_t index);

} // namespace ftcore
