#pragma once

// The moving water surface in the game: the tiles (FTWaterSurfaceBuild::BuildTileMesh's mesh, laid by
// ftcore::waterTileLayout) that stand in for each body's flat plane while ft.Water.Surface is on.
//   - A body gets tiles only where its flat plane (a mesh wearing M_Look_Water or M_Look_CanalWater) was found and can be
//     hidden whole; the tiles wear a dynamic instance of that plane's own material (a canal keeps its Flow and Channel).
//     A polygon body, or a plane shared with a body that has no tiles, keeps its flat plane.
//   - One instanced component and one instance per body (16 at most): its edge line is the body's own.
//   - A sea's plane is replaced past the tiles' reach (and beside them along the shore) by plain planes wearing the
//     sea's instance with the vertex waves off: the tide and the waves' normals only.
//   - Built once, the first time the surface is turned on; the waves' parameters are pushed when the sets are rebuilt,
//     the tide when it has moved 2 mm. Nothing is allocated after the build.

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ftcore/WaterBodies.h"
#include "ftcore/WaterDraw.h"
#include "ftcore/WaterWaves.h"

#include "FTWaterSurfaceDraw.generated.h"

class AActor;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UPrimitiveComponent;
class UStaticMeshComponent;

UCLASS(Transient)
class FORGOTTENTHIRD_API UFTWaterSurfaceDraw : public UObject
{
	GENERATED_BODY()

public:
	// Lays the tiles and finds the flat planes; false (and logged) when the tile mesh or the wired masters are missing.
	bool Build(UWorld& World, const ftcore::WaterBodies& Water, const ftcore::WaterWaveSpec& Waves);
	bool IsBuilt() const { return bBuilt; }
	// The tiles drawn and the flat planes hidden (true), or the other way round.
	void SetShown(bool bOn);
	// Every part's waves (after the sets were rebuilt), and its tide.
	void PushWaves(const ftcore::WaterSurfaceModel& Model);
	// The tide of each group, pushed to its parts when it has moved.
	void PushTide(const ftcore::WaterSurfaceModel& Model);
	int32 TileCount() const { return Tiles; }

private:
	void AddFarSea(UWorld& World, const ftcore::WaterBody& Body, const FBox& PlaneBounds, UMaterialInstanceDynamic* Mid, const TArray<ftcore::WaterTile>& Laid,
		int32 BodyIndex);
	void SetPartParams(UMaterialInstanceDynamic& Mid, const ftcore::WaterBody& Body, bool bWaveVerts);

	UPROPERTY()
	TObjectPtr<AActor> Host;
	UPROPERTY()
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> TileParts;
	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> FarParts;
	// One per part of either kind, its draw group in MidGroup.
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Mids;
	TArray<uint8> MidGroup;
	UPROPERTY()
	TArray<TObjectPtr<UPrimitiveComponent>> Flats;
	float TideCm[ftcore::kWaterDrawGroups] = {};
	bool bTideSet[ftcore::kWaterDrawGroups] = {};
	int32 Tiles = 0;
	bool bBuilt = false;
	bool bTried = false;
	bool bShown = false;
};
