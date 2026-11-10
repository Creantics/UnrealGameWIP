#pragma once

// FTBuildMassingWorld's roof dressing, round 6. -RoofScatter (with -Roofs -RoofLayouts) sets
// ftcore::MassingRoofStyle::scatter: tanks of their own sizes, clutter turned a little off the roof's square and
// clustered, huts along the edges as well as in the corners, more empty roofs. -RoofHut=<static mesh object path> lays
// that mesh (the BK-HUT kit piece) in every stair-head hut box (kind "hut") as one instance of a single HISM, in place
// of the hut's cube; -RoofHutTurn=<degrees> turns every hut that much more (180 when the mesh's door is on its +X face,
// not its -X). Without these switches nothing here changes the build.

#include "CoreMinimal.h"

#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"

class AActor;
class UStaticMesh;
class UWorld;
struct FActorSpawnParameters;

namespace FTRoofDress
{
// The command line's roof switches.
struct FOptions
{
	bool bScatter = false; // -RoofScatter
	FString HutMesh;       // -RoofHut=
	double HutTurn = 0.0;  // -RoofHutTurn=
};
FOptions ParseOptions(const TCHAR* Params);

// Sets the style's round 6 flags from the options.
void ApplyStyle(const FOptions& Options, ftcore::MassingRoofStyle& Style);

// The stair-head huts as instances of the hut mesh.
class FHuts
{
public:
	// Loads -RoofHut's mesh. False (logged) when it was given and doesn't load; true, with nothing to do, without it.
	bool Begin(const FOptions& Options);
	// True when Piece is a hut whose mesh instance this will lay (the caller then lays no cube for it). Side: the
	// world's side (m), as ftcore::massingKitPlacement takes it.
	bool Take(const ftcore::MassingBox& Piece, double Side);
	// Spawns the holder and its HISM with every taken hut. Returns the instances added.
	int32 Finish(UWorld* World, const FActorSpawnParameters& Spawn);

private:
	UStaticMesh* Mesh = nullptr;
	double Turn = 0.0;
	ftcore::MassingVec MeshMin, MeshMax;
	TArray<FTransform> Pending;
};
} // namespace FTRoofDress
