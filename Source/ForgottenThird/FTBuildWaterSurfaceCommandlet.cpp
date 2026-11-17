#include "FTBuildWaterSurfaceCommandlet.h"

#include "Engine/StaticMesh.h"

#include "FTWaterSurfaceBuild.h"
#include "ForgottenThird.h"

UFTBuildWaterSurfaceCommandlet::UFTBuildWaterSurfaceCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildWaterSurfaceCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	(void)Params;
	const UStaticMesh* Mesh = FTWaterSurfaceBuild::BuildTileMesh();
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildWaterSurface: the tile wasn't built or saved."));
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildWaterSurface: saved %s (%d LODs)."), *Mesh->GetPathName(), Mesh->GetNumSourceModels());
	return 0;
#else
	(void)Params;
	return 1;
#endif
}
