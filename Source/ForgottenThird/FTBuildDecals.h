#pragma once

// FTBuildMassingWorld's road wear: wraps ftcore/RoadDecals.h. -RoadDecals[=<density>] scatters cracks, stains, manhole
// covers, patches and kerb grime over every -Roads carriageway and pavement slab (density 1 by default), each a 1 m plate
// lying a few millimetres over its slab's top, an instance of its cell (FFTCellInstancer: no collision, no shadow, the
// props' cull distance). One masked material, made in the map's package, draws every kind from the instance's custom
// data (kind, seed, size) with ftcore::roadDecalHlsl's noise, no textures, and darkens and smooths with the look
// collection's Wetness. Without -RoadDecals nothing here runs.

#include "CoreMinimal.h"

#include "ftcore/Roads.h"

#include <vector>

class FFTCellInstancer;
class UPackage;

namespace FTBuildDecals
{
// Lays the decals of Builds (one per road of Roads; a network's all in Builds[0]) into Cells, before its Flush. Side:
// the plan's side (ftcore::massingEnginePoint).
FORGOTTENTHIRD_API void Lay(UPackage* Package, FFTCellInstancer& Cells, const std::vector<ftcore::RoadSpec>& Roads,
	const std::vector<ftcore::RoadBuild>& Builds, double Side);
} // namespace FTBuildDecals
