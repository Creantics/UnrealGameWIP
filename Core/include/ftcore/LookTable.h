#pragma once

// The per-world scene looks (Data/Look/looks.json): each world's light, air, camera finish and grade as
// SceneLook values. A world not listed keeps the default SceneLook.
//
//   ftcore::LookTable table;
//   if (!ftcore::importLookTable(text, table, &error, &warnings)) ...;       // allocates: at load
//   const ftcore::SceneLook* look = table.find(ftcore::lookWorldForMap(mapName));   // no allocation
//
// looks.json:
//   { "version": 1,
//     "worlds": { "solhaven": { "about": "a line in light and colour", "sunKelvin": 6000, "gradeChroma": 0.82 } } }
//   A world's id is lower-case letters, digits and '_'. Each other key is a SceneLook field (see
//   FTCORE_LOOK_FIELDS in SceneLook.h) with a number in that field's range; a field a world leaves out keeps
//   the SceneLook default. `about` is a note and is ignored. An unknown key is a warning; a value that isn't a
//   number or is out of range, or a missing or wrong version, is an error and the whole table is refused.

#include "ftcore/Api.h"
#include "ftcore/SceneLook.h"

#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct LookTable {
    struct Entry {
        std::string id;
        SceneLook look;
    };
    std::vector<Entry> worlds;

    // The world with this id (case-insensitive), or null. No allocation.
    FTCORE_API const SceneLook* find(std::string_view id) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importLookTable(std::string_view json, LookTable& out, std::string* error = nullptr,
                                std::vector<std::string>* warnings = nullptr);

// The world a map belongs to: "solhaven", "lattice" or "unwritten" when the map's name or path contains that
// word (case-insensitive), else empty. No allocation.
FTCORE_API std::string_view lookWorldForMap(std::string_view mapName);

} // namespace ftcore
