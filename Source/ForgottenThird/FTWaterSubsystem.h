#pragma once

// A map's water at run time: the bodies its builder saved on the map's director (canal rows and the sea, as
// ftcore::WaterBodies in the engine's frame, metres), read by the lead for its wade and swim, by the followers for
// their footing, and here for the view: while the view's eye is in a body's footprint and at or under its surface,
// the underwater post-process (FTScene::LookUnderwaterMaterial, FTBuildLookMaterials -Underwater) is on, on an unbound
// volume so it shows through whichever camera is the view (the lead's, a still's eye). The material cuts at the
// waterline per pixel, so a half-sunk view is fogged below the line only.
//
// The sound hook: OnCameraWater fires when the eye goes under a surface and when it comes back out, with its depth in
// metres and the body's kind ("canal", "pool", "lake", "sea"); CameraDepth() is the depth at any time.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ftcore/WaterBodies.h"

#include "FTWaterSubsystem.generated.h"

class APostProcessVolume;
class UMaterialInstanceDynamic;

// One body as the map keeps it (the engine's frame, metres): ftcore::WaterBody's fields.
USTRUCT()
struct FFTWaterBodyRow
{
	GENERATED_BODY()

	UPROPERTY()
	uint8 Kind = 0;
	UPROPERTY()
	uint8 Shape = 0;
	UPROPERTY()
	bool bWaves = false;
	UPROPERTY()
	float Level = 0.f;
	UPROPERTY()
	float Bed = 0.f;
	UPROPERTY()
	FVector2D Flow = FVector2D::ZeroVector;
	UPROPERTY()
	FVector2D Centre = FVector2D::ZeroVector;
	UPROPERTY()
	FVector2D Half = FVector2D::ZeroVector;
	UPROPERTY()
	float CosYaw = 1.f;
	UPROPERTY()
	float SinYaw = 0.f;
	UPROPERTY()
	float BuoyLine = 0.f;
	UPROPERTY()
	TArray<FVector2D> Points;

	static FFTWaterBodyRow From(const ftcore::WaterBody& Body);
	ftcore::WaterBody ToBody() const;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFTCameraWaterEvent, bool, bUnder, float, DepthMetres, FName, Kind);

UCLASS()
class FORGOTTENTHIRD_API UFTWaterSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bHasBodies; }

	bool HasBodies() const { return bHasBodies; }
	const ftcore::WaterBodies& Bodies() const { return Water; }
	// The time the surfaces are read at (the world's seconds).
	float WaterTime() const;

	// The view's eye went under a surface (true) or came back out (false).
	UPROPERTY(BlueprintAssignable, Category = "Water")
	FFTCameraWaterEvent OnCameraWater;
	UFUNCTION(BlueprintPure, Category = "Water")
	bool IsCameraUnder() const { return bUnder; }
	UFUNCTION(BlueprintPure, Category = "Water")
	float CameraDepth() const { return Depth; }

private:
	void SetView(bool bWet, float SurfaceCm, float Clarity);

	ftcore::WaterBodies Water;
	bool bHasBodies = false;
	bool bUnder = false;
	bool bMaterialMissing = false;
	float Depth = 0.f;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> Underwater;
	UPROPERTY()
	TObjectPtr<APostProcessVolume> Volume;
};
