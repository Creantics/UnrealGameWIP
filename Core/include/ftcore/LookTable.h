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
//     "render": { "r.Lumen.HardwareRayTracing": 1 },
//     "worlds": { "solhaven": { "about": "a line in light and colour", "sunKelvin": 6000, "gradeChroma": 0.82 } } }
//   A world's id is lower-case letters, digits and '_'. Each other key is a SceneLook field (see
//   FTCORE_LOOK_FIELDS in SceneLook.h) with a number in that field's range; a field a world leaves out keeps
//   the SceneLook default. `about` is a note and is ignored. An unknown key is a warning; a value that isn't a
//   number or is out of range, or a missing or wrong version, is an error and the whole table is refused.
//   `render` is optional: engine console variables the game sets at start, name -> number, in file order. It
//   must be an object; each key starts with "r." and is 3..128 characters of letters, digits, '.' and '_', with
//   no duplicates (case-insensitive); each value is a finite number. A bad key or value is an error naming
//   render.<key> and the whole table is refused.
//   `dusk` is optional: world id -> the dusk row (ftcore::DuskLook, NightLook.h): the sun's elevation a dusk still
//   stands at (stillDeg), the bell (startDeg, peakDeg, endDeg), the sun's, sky's, fill's and fog's colours
//   (sunTintR/G/B, skyGlowR/G/B, fillTintR/G/B, fogTintR/G/B), fillScale, exposureBias, when the shop, window and
//   street lights come on (lightsOnDeg, lightsFullDeg) and the daylight in a room (dayEndDeg, dayFullDeg). A key a
//   row leaves out keeps the DuskLook default; `status` and `about` are notes (strings) and are kept as read.
//   `glass` is optional: name -> number, the glass system's values a look may set (letters and digits, 2..64
//   characters); `status` and `about` are notes. A bad key or value in either is an error and the table is refused.
//   `renderTiers` is optional: the render quality tiers, set over `render`:
//     { "about": "...", "play": "standard", "cutscene": "high",
//       "low": { "r.X": 0 }, "standard": { "r.X": 2 }, "high": { "r.X": 2 } }
//   Each other key is a tier (lower-case letters, digits and '_') with console variables as in `render`. Every tier
//   sets the same console variables (so a switch leaves none behind); `play` and `cutscene` name tiers. `about` is a
//   note. A bad tier, a missing pick or tiers that set different variables is an error and the table is refused.
//   `maps` (optional, an array of non-empty strings) scopes the tiers: a map takes the play tier only when its name
//   contains one of them (case-insensitive, so "World_Solhaven" covers World_Solhaven_Strip and a PIE prefix); every
//   other map keeps the settings it had before any tier. Without `maps` no map takes a tier.

#include "ftcore/Api.h"
#include "ftcore/NightLook.h"
#include "ftcore/SceneLook.h"

#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct LookTable {
    struct RenderSetting {
        std::string name; // an engine console variable, such as "r.Lumen.HardwareRayTracing"
        float value;
    };
    struct Entry {
        std::string id;
        SceneLook look;
    };
    struct DuskRow {
        std::string world;
        std::string status; // a note, such as "proposed, unruled"
        DuskLook look;
    };
    struct GlassValue {
        std::string name;
        float value;
    };
    std::vector<Entry> worlds;
    std::vector<RenderSetting> render; // in file order
    std::vector<DuskRow> dusk;
    std::vector<GlassValue> glass;
    std::string glassStatus;
    struct RenderTier {
        std::string name;
        std::vector<RenderSetting> settings; // in file order
    };
    std::vector<RenderTier> renderTiers;
    std::string playTier;     // the tier the game starts in; empty when there are no tiers
    std::string cutsceneTier; // the tier a cutscene runs in
    std::vector<std::string> tierMaps; // renderTiers.maps: the map names the tiers apply on

    // The world with this id (case-insensitive), or null. No allocation.
    FTCORE_API const SceneLook* find(std::string_view id) const;
    // The world's dusk row, or null. No allocation.
    FTCORE_API const DuskRow* findDusk(std::string_view world) const;
    // The glass value of this name, or `fallback`. No allocation.
    FTCORE_API float glassValue(std::string_view name, float fallback) const;
    // The render tier of this name (case-insensitive), or null. No allocation.
    FTCORE_API const RenderTier* findTier(std::string_view name) const;
    // True when the map of this name takes the tiers: there are tiers and its name contains one of tierMaps
    // (case-insensitive). No allocation.
    FTCORE_API bool tiersOnMap(std::string_view mapName) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importLookTable(std::string_view json, LookTable& out, std::string* error = nullptr,
                                std::vector<std::string>* warnings = nullptr);

// The world a map belongs to: "solhaven", "lattice" or "unwritten" when the map's name or path contains that
// word (case-insensitive), else empty. No allocation.
FTCORE_API std::string_view lookWorldForMap(std::string_view mapName);

} // namespace ftcore
