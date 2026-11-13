#pragma once

// A map's water at run time: the bodies its builder saved on the map's director (canal rows and the sea, as
// ftcore::WaterBodies in the engine's frame, metres), read by the lead for its wade and swim, by the followers for
// their footing, and here for the view: while the view's eye is in a body's footprint and at or under its surface,
// the underwater post-process (FTScene::LookUnderwaterMaterial, FTBuildLookMaterials -Underwater) is on, on an unbound
// volume so it shows through whichever camera is the view (the lead's, a still's eye). The material cuts at the
// waterline per pixel, so a half-sunk view is fogged below the line only. The volume is off while the view is dry.
//
// The water answers the lead (LeadWater, told by the lead each tick): an entry splashes (by the fall's speed), a body
// in it sends rings out where it stands and a wake where it moves, and one that leaves drips for some seconds. The
// rings are ftcore::WaterRipples' last eight; they, the lead's place, its wake and the sun's way under water go to the
// water collection (FTScene::LookWaterCollection) each frame, which every water surface and the underwater view read,
// so any number of water planes show them at no cost per plane. The splash and the drips are the effects
// "water_splash" and "water_drips" (Data/Vfx, FTBuildVfx).
//
// The sound hooks: OnCameraWater fires when the eye goes under a surface and when it comes back out, with its depth in
// metres and the body's kind ("canal", "pool", "lake", "sea"); CameraDepth() is the depth at any time. OnLeadSplash
// fires when the lead comes into the water (strength 0.25 a step in to 1 a hard fall), OnLeadWaterMove when it starts
// or stops moving in it or changes between wading and swimming (LeadWaterSpeed() is its speed at any time), and
// OnLeadWaterExit when it comes out.
//
// -WaterTune=<Name>=<value>[,<Name>=<value>...] sets scalar parameters of the water collection or of the underwater
// view by name at the start of play, for tuning from stills without a rebuild.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ftcore/WaterBodies.h"

#include "FTWaterSubsystem.generated.h"

class ADirectionalLight;
class APostProcessVolume;
class UMaterialInstanceDynamic;
class UMaterialParameterCollection;
class UNiagaraComponent;
class UNiagaraSystem;

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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFTLeadSplashEvent, float, Strength, float, FallMetresPerSecond, FName, Kind);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFTLeadWaterMoveEvent, bool, bMoving, float, MetresPerSecond, FName, Mode);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFTLeadWaterExitEvent, float, WetLineMetres, FName, Kind);

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

	// The lead's water this tick: its state (dry too), its place (cm), its velocity (cm/s) and its height
	// (cm). Splashes, feeds the rings, starts and stops the drips, keeps its wetness and fires the lead's hooks.
	void LeadWater(AActor& Lead, const ftcore::WaterState& State, const FVector& At, const FVector& Velocity, float Height, float DeltaSeconds);
	// How wet the lead is (the wetness, the line's height over its feet in metres, the drip).
	const ftcore::WaterWetness& LeadWetness() const { return Wet; }
	const ftcore::WaterRipples& Ripples() const { return Rings; }

	// The view's eye went under a surface (true) or came back out (false).
	UPROPERTY(BlueprintAssignable, Category = "Water")
	FFTCameraWaterEvent OnCameraWater;
	// The lead came into the water.
	UPROPERTY(BlueprintAssignable, Category = "Water")
	FFTLeadSplashEvent OnLeadSplash;
	// The lead started or stopped moving in the water, or changed between wading and swimming ("wade", "deep wade", "swim").
	UPROPERTY(BlueprintAssignable, Category = "Water")
	FFTLeadWaterMoveEvent OnLeadWaterMove;
	// The lead came out of the water, wet up to a line that far over its feet.
	UPROPERTY(BlueprintAssignable, Category = "Water")
	FFTLeadWaterExitEvent OnLeadWaterExit;
	UFUNCTION(BlueprintPure, Category = "Water")
	bool IsCameraUnder() const { return bUnder; }
	UFUNCTION(BlueprintPure, Category = "Water")
	float CameraDepth() const { return Depth; }
	UFUNCTION(BlueprintPure, Category = "Water")
	float LeadWaterSpeed() const { return LeadSpeed; }

private:
	void SetView(bool bWet, float SurfaceCm, int32 Body);
	// The rings, the lead's place and wake and the sun under water, to the water collection.
	void PushSurface(float DeltaTime);
	void ApplyTune();

	ftcore::WaterBodies Water;
	ftcore::WaterRipples Rings;
	ftcore::WaterStir Stir;
	ftcore::WaterWetness Wet;
	bool bHasBodies = false;
	bool bUnder = false;
	bool bMaterialMissing = false;
	bool bRingsShown = false;
	bool bLeadMoving = false;
	bool bTuned = false;
	uint8 LeadMode = 0;
	FName LeadKind;
	float Depth = 0.f;
	float LeadSpeed = 0.f;
	float LeadFall = 0.f;
	// The lead's place in the water for the surfaces (cm; Z its radius at the line, 0 out of the water).
	FVector LeadAt = FVector::ZeroVector;
	FVector2D WakeWay = FVector2D(1.0, 0.0);
	float WakeSpeed = 0.f;
	float WakeAmount = 0.f;
	float SunSought = 0.f;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> Underwater;
	UPROPERTY()
	TObjectPtr<APostProcessVolume> Volume;
	UPROPERTY()
	TObjectPtr<UMaterialParameterCollection> SurfaceParams;
	UPROPERTY()
	TObjectPtr<UNiagaraSystem> SplashSystem;
	UPROPERTY()
	TObjectPtr<UNiagaraSystem> DripSystem;
	UPROPERTY()
	TObjectPtr<UNiagaraComponent> Drips;
	TWeakObjectPtr<ADirectionalLight> Sun;
};
