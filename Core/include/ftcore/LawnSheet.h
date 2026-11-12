#pragma once

// A lawn that follows the ground: one sheet per region sampled from the height field on a regular grid, and the paths
// over it as ribbons that follow the ground too, a little higher, each edge carried down past the lawn by a skirt so
// the lawn meets the path at a clean edge with no gap and no step.
//
// The build's -Lawn=<file> names a table (CSV, '#' comment lines, a header row naming the columns):
//   kind     "region" (a lawn rectangle) or "path" (a paved strip over it)
//   x0, y0   a region's south-west corner, a path's start (plan metres)
//   x1, y1   a region's north-east corner, a path's end
//   width    a path's width (m); a region ignores it
// Plain C++20; reading the table and making the meshes allocate (build time only).

#include "ftcore/Api.h"
#include "ftcore/Backdrop.h"
#include "ftcore/MassingPlan.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct LawnRegion {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};
struct LawnPath {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0, width = 2.0;
};
struct LawnTable {
    std::vector<LawnRegion> regions;
    std::vector<LawnPath> paths;
};

// False, with the reason in *error, on a missing column, a bad number, an unknown kind, an empty region or a path
// with no length or width.
FTCORE_API bool parseLawnTable(std::string_view csv, LawnTable& out, std::string* error);

// Plan-frame vertices (metres) and triangles (indices into them). The triangles' winding is the plan's
// counter-clockwise seen from above; the build turns them to the engine's.
struct LawnMesh {
    std::vector<MassingVec> v;
    std::vector<std::array<std::uint32_t, 3>> tris;
};

// The lawn sheet over a region: a grid of about `step` metres (at least one cell a side), each vertex `lift` m over
// the ground under it (massingGroundAt), so it follows the ground with no steps. Appends to out.
FTCORE_API void lawnSheetMesh(const HeightField& ground, const LawnRegion& region, double step, double lift, LawnMesh& out);

// A path's ribbon: stations about `step` m apart along it, each with its edges and centre `lift` m over the ground under
// them, and a skirt from each edge down to `skirt` m under the ground. Appends to out.
FTCORE_API void lawnPathMesh(const HeightField& ground, const LawnPath& path, double step, double lift, double skirt, LawnMesh& out);

// The greens' planting: -PlantPoints=<file> names a table (CSV, '#' comment lines, a header row naming the columns):
//   asset    the plant model (the engine's asset path or name; the build resolves it)
//   x, y     plan metres
//   z        optional: the foot's height (m); empty stands it on the ground
//   yaw      degrees, plan frame
//   scale    a uniform scale (0 < scale <= 10)
//   kind     "shrub" (drawn to the shrubs' distance) or "cover" (ground cover: grass, weeds, ferns, drawn nearer)
enum class PlantPointKind : std::uint8_t { Shrub, Cover };
struct PlantPoint {
    std::string asset;
    double x = 0.0, y = 0.0, z = 0.0;
    bool hasZ = false;
    double yaw = 0.0, scale = 1.0;
    PlantPointKind kind = PlantPointKind::Shrub;
};
// False, with the reason in *error, on a missing column, a bad number, an empty asset, a bad scale or an unknown kind.
FTCORE_API bool parsePlantPoints(std::string_view csv, std::vector<PlantPoint>& out, std::string* error);

} // namespace ftcore
