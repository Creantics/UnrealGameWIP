#pragma once

// Builds the moving water surface's tile (UFTWaterSubsystem::TileMeshPath; FTWaterSurfaceBuild::BuildTileMesh):
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildWaterSurface -unattended -nopause -nosplash
//
// A 16 m square with three LODs (25 cm, 1 m and 4 m cells, the cell baked in UV1.x) and a skirt. The masters' surface
// inputs are FTBuildLookMaterials' (-Water rebuilds them).

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildWaterSurfaceCommandlet.generated.h"

UCLASS()
class UFTBuildWaterSurfaceCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildWaterSurfaceCommandlet();

	virtual int32 Main(const FString& Params) override;
};
