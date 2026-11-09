#pragma once

// A navmesh's islands, shared by FTBuildMassingWorld's -Coverage (the islands against the coverage points) and the
// tour's edge stops (the walk aims at a point on the lead's own island). FTGatherNavIslands takes every tile's ground
// polys with their corners (CornerOf maps an engine point, cm, to the frame the caller wants, in metres), leaves out
// those under the sea at SeaLevel (m in that frame, ftcore::kNoSea for none; ftcore::navPolysAboveSea at
// ftcore::kNavSeaMargin), joins a poly to each neighbour its own neighbour list gives, and to the ground polys at the
// ends of an off-mesh link it lists, then floods them into islands (ftcore::navIslands). Polys, Refs and IndexOf hold
// the kept polys; Set.islandOf[i] is kept poly i's island. Allocates: once per call, never on a tick.

#include "CoreMinimal.h"
#include "AI/Navigation/NavigationTypes.h"
#include "Templates/Function.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/WorldChecks.h"

#include <vector>

class ARecastNavMesh;

struct FFTNavIslands
{
	int32 Tiles = 0;    // the navmesh's tiles
	int32 Gathered = 0; // ground polys with corners, before the sea leaves any out
	std::vector<ftcore::MassingVec> Corners; // every gathered poly's corners, in CornerOf's frame
	std::vector<ftcore::NavPolyRef> Polys;   // the kept polys, indexing Corners
	TArray<NavNodeRef> Refs;                 // the kept polys' refs, in Polys' order
	TMap<NavNodeRef, uint32> IndexOf;        // a kept poly's index in Polys
	ftcore::SeaLeftOut UnderSea;
	ftcore::NavIslandSet Set;
};

void FTGatherNavIslands(const ARecastNavMesh& NavMesh, TFunctionRef<ftcore::MassingVec(const FVector&)> CornerOf, double SeaLevel,
	FFTNavIslands& Out);
