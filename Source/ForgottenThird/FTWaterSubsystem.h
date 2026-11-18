#pragma once

// A map's water at run time: the bodies its builder saved on the map's director (canal rows and the sea, as
// ftcore::WaterBodies in the engine's frame, metres), read by the lead for its wade and swim, by the followers for
// their footing, and here for the view: while the view's eye is in a body's footprint and at or under its surface,
// the underwater post-process (FTScene::LookUnderwaterMaterial, FTBuildLookMaterials -Underwater) is on, on an unbound
// volume so it shows through whichever camera is the view (the lead's, a still's eye). The material cuts at the
// waterline per pixel, so a half-sunk view is fogged below the line only. The volume is off while the view is dry.
//
// The water answers the lead (LeadWater, told by the lead each tick): an entry splashes (by the fall's speed), a body
// in it sends rings out where it stands and a wake where it moves, and one that leaves is wet (and stained by the
// water's silt) until it dries; its clothes show it and its feet leave wet prints (the lead's). No drips are spawned. The
// rings are ftcore::WaterRipples' last eight; they, the lead's place, its wake and the sun's way under water go to the
// water collection (FTScene::LookWaterCollection) each frame, which every water surface and the underwater view read,
// so any number of water planes show them at no cost per plane. The splash is the effect "water_splash" (Data/Vfx,
// FTBuildVfx).
//
// The sound hooks: OnCameraWater fires when the eye goes under a surface and when it comes back out, with its depth in
// metres and the body's kind ("canal", "pool", "lake", "sea"); CameraDepth() is the depth at any time. OnLeadSplash
// fires when the lead comes into the water (strength 0.25 a step in to 1 a hard fall), OnLeadWaterMove when it starts
// or stops moving in it or changes between wading and swimming (LeadWaterSpeed() is its speed at any time), and
// OnLeadWaterExit when it comes out.
//
// -WaterTune=<Name>=<value>[,<Name>=<value>...] sets scalar parameters of the water collection or of the underwater
// view by name at the start of play, for tuning from stills without a rebuild. It is compiled out of shipping builds.
//
// The canal's sky: on a tier with no screen-space and no Lumen reflections (Low) the engine reflects the open sky in
// every canal with nothing to hide it, so the water reads cream between dark walls. The collection's CanalSky, which the
// canal master multiplies its Specular by, is written here from ftcore::waterCanalSky: r.FT.Water.CanalSkyLow on such a
// tier, 1 on every other (1 in the cvar turns it off). Written only when it moves; logged then.
//
// Rain on the water: with r.FT.Water.RainRings 1 the collection's RainRings is the rain's intensity (ftcore::waterRainRings,
// in steps), by which the sea and canal masters lay rain rings on their surfaces; 0 (the default) leaves them untouched.
// The underwater view's own ripples (the window) take no rain.
//
// The surface model (ftcore::WaterSurfaceModel: Data/Water/water.json's waves and tide, the look's wind and swell, the
// rain's storm, the world clock's hours) is set on the bodies when ft.Water.Surface is 1, so the wade, the swim and the
// eye read the tide and the waves the drawn surface shows. It is 0 until the surfaces draw them.
//
// Cue and Touch are the swimmers' (the lead and each follower, from their movement): a cue sends rings and, for an entry
// or an exit, a splash; touchers are kept for the tick (up to kMaxTouchers) for the surface's field.
//
// The ways out of the canals (ladders and steps, ftcore::WaterExit) are kept with the bodies: the lead climbs a ladder
// it pushes at, and a jump at any other wall is only the fallback.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ftcore/WaterBodies.h"
#include "ftcore/WaterCue.h"
#include "ftcore/WaterWaves.h"

#include "FTWaterSubsystem.generated.h"

class ADirectionalLight;
class APostProcessVolume;
class UMaterialInstanceDynamic;
class UMaterialParameterCollection;
class UNiagaraSystem;
class UFTWaterSurfaceDraw;

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
	// A canal's section (ftcore::WaterBody): the bed on the shelf at its long walls, the shelf's width and the slope's run.
	UPROPERTY()
	float ShelfBed = 0.f;
	UPROPERTY()
	float Shelf = 0.f;
	UPROPERTY()
	float Batter = 0.f;
	UPROPERTY()
	TArray<FVector2D> Points;
	// Free swim under water (ftcore::WaterBody::swimDeep): dived in inside the dive area (DiveMin..DiveMax, the engine's
	// frame, metres), or in the whole body when it has none.
	UPROPERTY()
	bool bSwimDeep = false;
	UPROPERTY()
	bool bHasDiveArea = false;
	UPROPERTY()
	FVector2D DiveMin = FVector2D::ZeroVector;
	UPROPERTY()
	FVector2D DiveMax = FVector2D::ZeroVector;

	static FFTWaterBodyRow From(const ftcore::WaterBody& Body);
	ftcore::WaterBody ToBody() const;
};

// One way out of the water as the map keeps it (the engine's frame, metres): ftcore::WaterExit's fields.
USTRUCT()
struct FFTWaterExitRow
{
	GENERATED_BODY()

	UPROPERTY()
	uint8 Kind = 0;
	UPROPERTY()
	FVector2D Foot = FVector2D::ZeroVector;
	UPROPERTY()
	FVector2D Top = FVector2D::ZeroVector;
	UPROPERTY()
	float TopZ = 0.f;
	UPROPERTY()
	FVector2D Out = FVector2D::ZeroVector;

	static FFTWaterExitRow From(const ftcore::WaterExit& Exit);
	ftcore::WaterExit ToExit() const;
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

	// The moving surface's tile (FTBuildWaterSurface builds it).
	static constexpr const TCHAR* TileMeshPath = TEXT("/Game/Look/SM_WaterTile");

	bool HasBodies() const { return bHasBodies; }
	const ftcore::WaterBodies& Bodies() const { return Water; }
	// The time the surfaces are read at (the world's seconds).
	float WaterTime() const;

	// The lead's water this tick: its state (dry too), its place (cm), its velocity (cm/s) and its height
	// (cm). Splashes, feeds the rings, keeps its wetness and silt and fires the lead's hooks.
	void LeadWater(AActor& Lead, const ftcore::WaterState& State, const FVector& At, const FVector& Velocity, float Height, float DeltaSeconds);
	// How wet the lead is (the wetness, the line's height over its feet in metres, the silt).
	const ftcore::WaterWetness& LeadWetness() const { return Wet; }
	const ftcore::WaterRipples& Ripples() const { return Rings; }
	// The surface model the bodies read (its sets and hours as of this tick).
	const ftcore::WaterSurfaceModel& SurfaceModel() const { return Surface; }

	// A swimmer's event this step; bFromLead for the lead, whose entry and exit LeadWater already splashed.
	void Cue(const ftcore::WaterCue& Event, bool bFromLead);
	// A body in the water this tick (metres, the engine's frame).
	void Touch(const ftcore::WaterToucher& Toucher);
	static constexpr int32 kMaxTouchers = 8;
	int32 TouchCount() const { return Touchers; }
	const ftcore::WaterToucher& Toucher(int32 I) const { return Touched[I]; }

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
	// The model's sets from the look and the rain (rebuilt only when they move) and its hours from the clock.
	void UpdateSurface(bool bForce);
	void Splash(const FVector& WhereCm, float Strength);

	ftcore::WaterBodies Water;
	ftcore::WaterRipples Rings;
	ftcore::WaterStir Stir;
	ftcore::WaterWetness Wet;
	ftcore::WaterSurfaceModel Surface;
	ftcore::WaterToucher Touched[kMaxTouchers];
	int32 Touchers = 0;
	float SurfaceStorm = -1.f;
	float SurfaceWind = -1.f;
	float SurfaceSwell = -1.f;
	bool bSurfaceOn = false;
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
	// The CanalSky and RainRings last written to the water collection (-1: not yet).
	float CanalSkyWritten = -1.f;
	float RainRingsWritten = -1.f;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> Underwater;
	UPROPERTY()
	TObjectPtr<APostProcessVolume> Volume;
	UPROPERTY()
	TObjectPtr<UMaterialParameterCollection> SurfaceParams;
	UPROPERTY()
	TObjectPtr<UNiagaraSystem> SplashSystem;
	// The tiles that draw the surface model while it is on (built the first time it is).
	UPROPERTY()
	TObjectPtr<UFTWaterSurfaceDraw> Draw;
	TWeakObjectPtr<ADirectionalLight> Sun;
};
