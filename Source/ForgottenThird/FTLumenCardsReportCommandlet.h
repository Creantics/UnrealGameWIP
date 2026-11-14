#pragma once

// Why a map's meshes get no Lumen mesh cards (surface cache coverage diagnostic, 2026-11-14):
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTLumenCardsReport -Map=/Game/Maps/World_Solhaven_Strip
//       [-Csv=<path>] [-Rebuild=auto|<mesh object path>] -NullRHI -unattended -nopause -nosplash
//
// Per static mesh placed in the map: instance counts by Lumen state (A in Lumen, B ray-traced only, C neither), whether
// the mesh's LOD 0 render data holds card representation data and distance field data (read once the mesh itself is
// compiled, then again after the card and distance-field queues finish, so a late build shows as "late"), the card
// count, the build settings that govern both, the largest card-bounds face of a sample state-A instance against the
// engine's minimum (r.LumenScene.SurfaceCache.MeshCardsMinSize squared), and that instance's transform: scale and
// whether it passes the engine's orthogonality test (LumenMeshCards.cpp IsMatrixOrthogonal). A summary line per family
// (engine shapes, map-built, plants, imported props, building kit, other) goes to the log.
//
// -Rebuild: duplicates ONE mesh with no cards into the transient package (auto: the first map-built, else engine
// shape, with none), forces its distance field on and runs the full static-mesh build, then reports whether it has
// cards. Nothing is saved.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTLumenCardsReportCommandlet.generated.h"

UCLASS()
class UFTLumenCardsReportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTLumenCardsReportCommandlet();

	virtual int32 Main(const FString& Params) override;
};
