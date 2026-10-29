#pragma once

// The world surface library (Data/Field/surfaces.json): photographed surfaces under a free licence, which
// -run=FTBuildSurfaces turns into textured materials, and the surface each world kind's shapes wear. A role
// is one kind of shape the world map builder places (ground, road, plaza, building, roof, landmark, wood,
// rock); a role a kind doesn't list keeps the builder's flat tint.
//
//   ftcore::SurfaceTable table;
//   if (!ftcore::importSurfaceTable(text, table, &error)) ...;                      // allocates: at load
//   if (const ftcore::SurfaceSpec* s = table.surfaceFor(WorldKind::Town, SurfaceRole::Roof)) ...; // no allocation
//
// surfaces.json (ids and numbers only):
//   { "version": 1,
//     "surfaces": [ { "id": "clay_roof_tiles_02", "tileCm": 250, "res": "2k",
//                     "source": "https://polyhaven.com/a/clay_roof_tiles_02", "license": "CC0" } ],
//     "kinds": { "town": { "roof": { "surface": "clay_roof_tiles_02", "tint": [1, 1, 1] } } } }
//   `tileCm` is the real width one copy of the texture covers (the source's own scale). `res` is 1k, 2k or 4k
//   (2k when absent): the size of the files tools/fetch_surfaces.py fetches and the import reads. `license`
//   must be CC0 and `source` an https page. `roughness` (optional, 0..1) is a constant roughness in place of
//   the packed map's, for a surface whose file has an empty roughness channel; -run=FTBuildSurfaces refuses it
//   on a map that has its own. A kind is town, towers or ruins (WorldKind). `tint` (optional, three numbers in
//   0..2, [1, 1, 1] when absent) multiplies the texture's colour. Unknown keys are warnings; an unknown kind or
//   role, or anything else wrong, is an error.

#include "ftcore/Api.h"
#include "ftcore/WorldLayout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class SurfaceRole : std::uint8_t { Ground, Road, Plaza, Building, Roof, Landmark, Wood, Rock };

inline constexpr std::size_t kSurfaceRoleCount = 8;
inline constexpr std::size_t kWorldKindCount = 3;

// "ground", "road", ... as surfaces.json writes them.
FTCORE_API std::string_view surfaceRoleName(SurfaceRole role);
FTCORE_API bool parseSurfaceRole(std::string_view name, SurfaceRole& out);

struct SurfaceSpec {
    static constexpr float kMinTileCm = 10.f;
    static constexpr float kMaxTileCm = 10000.f;

    std::string id;          // lower-case letters, digits and '_'
    float tileCm = 200.f;
    std::uint32_t resK = 2;  // 1, 2 or 4: the texture files are this many thousand pixels across ("2k")
    std::string source;      // the page it came from
    float roughness = -1.f;  // 0..1 in place of the packed map's roughness; negative: the map's
};

struct SurfaceUse {
    int surface = -1; // index into SurfaceTable::surfaces; -1: the role keeps the builder's tint
    std::array<float, 3> tint{1.f, 1.f, 1.f};
};

struct SurfaceTable {
    std::vector<SurfaceSpec> surfaces;
    std::array<std::array<SurfaceUse, kSurfaceRoleCount>, kWorldKindCount> uses{}; // [kind][role]

    // The surface with this id, or -1.
    FTCORE_API int find(std::string_view id) const;
    // What a kind's role wears. No allocation.
    FTCORE_API const SurfaceUse& use(WorldKind kind, SurfaceRole role) const;
    // The surface a kind's role wears, or null when it keeps the builder's tint. No allocation.
    FTCORE_API const SurfaceSpec* surfaceFor(WorldKind kind, SurfaceRole role) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importSurfaceTable(std::string_view json, SurfaceTable& out, std::string* error = nullptr,
                                   std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
