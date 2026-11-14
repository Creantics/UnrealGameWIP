#pragma once

// The world wave's host: one ftcore world wave (Core/include/ftcore/WorldWave.h) stepped each frame, and the things it
// cuts in and out.
//
//   ft.WorldWave <x> <y> <z> [reverse]   a wave from that world point (cm) starting now; ft.WorldWave off ends it
//   ft.WorldWaveDress <x> <y> <z> <r>     the proof dressing round that world point (cm), within r cm
//   -WorldWaveAt=<x,y,z,t>                a wave from that world point (cm) starting at t seconds on the wave clock
//   -WorldWaveReverse                     ... run in reverse (the sphere holds the first world)
//   -WorldWaveDress=<x,y,z,r>             the dressing on the first frame, before any -WorldWaveAt wave starts
//
// The wave clock is game time, unless a capture holds it (SetClock): the eye shot's fixed-step walk holds it at -1 until
// its first frame and at (frame - 1) / -WorldEyeWalkFps from then on, so -WorldWaveAt=x,y,z,0 with
// -WorldEyeWalkTo=<6 m on> -WorldEyeWalkSpeed=2 -WorldEyeWalkFps=30 saves the wave's first 3 s as 90 frames. On game
// time, start a wave a few seconds in: the exposure is held from the wave's start, where the eye has adapted to by then.
//
// Each frame, once every actor and the player's camera have ticked (FWorldDelegates::OnWorldPostActorTick), while a wave
// is set it
//   - writes the wave's parameters to MPC_Look (FTWorldWave.h has the names and units), the origin and the streak's tip
//     (StreakHeightCm over it) projected through the first player's view;
//   - keeps an unbound post-process volume on with PP_WorldWave_Ink as its blendable, and from the wave's start holds the
//     auto exposure where it stands (speed 0.001 up and down), so the change reads as the world's, not the eye's;
//   - thins every height fog's density by the camera's share of the second world (cameraInside for Forward, 1 - it for
//     Reverse), back to the map's own when the wave ends; the post-process material whitens the sky by the same share;
//   - logs each cue (no sound yet);
//   - steps every tracked thing (ftcore::updateWorldWaveThing): its visibility, its collision (collisionDelay after the
//     front passes it) and a held arrival's own front (custom primitive data FTWorldWave::kLocalSlot). An arrival that
//     would come in round the player or a lead (AFTExploreCharacter) waits, not drawn and not solid, until its footprint
//     (its collision's box, else its bounds) is clear, then cuts in. Nothing is pushed.
// No heap allocation in the frame's step: the dressing lists the tracked things, the wave's start the pawns and fogs
// (into arrays reserved once).
//
// The proof dressing (Dress). Within the radius of the point: every street tree (a /Game/Props/TREE- mesh, not the
// grate), TQ-04 lamp post and parked kei car (SM_KeiCar) is tagged OldOnly. One in a cell's instanced mesh is split off
// into a one-instance instanced mesh of its own on the same holder (its transform, per-instance data, materials,
// collision, culling, shadow and tags), as the tag is per primitive (custom primitive data FTWorldWave::kTagSlot); a
// fallback actor's component is tagged in place. The lamps' night lenses (the night lights' instanced mesh tagged
// FTScene::NightLampHeadTag) within the radius go with them, cut by visibility alone (their material has no cut). A slot
// whose master is a prop master (FTWorldWave::PropMasters) moves to
// a dynamic instance of the master's twin with the cut (M_FTCharacter_WaveCut, M_FTCharacterTex_WaveCut). The ground,
// the buildings and the clock tower are not touched: untagged is Both (never cut, they take the second world's look
// from the post-process alone). Then it spawns the second world's own things, tagged NewOnly, on the ground in a ring 8 to
// 25 m out: bare trees (TREE-broadleaf-a, TREE-park-a, leaves and billboard at Bare 1) and standing boards (the kit's
// SIGN_VERT_L and SIGN_VERT_M, on twin instances), each set down on a ground trace and turned to face the point. The log
// lists each pick.

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"
#include "Subsystems/WorldSubsystem.h"

#include "ftcore/WorldWave.h"

#include "FTWorldWaveSubsystem.generated.h"

class APawn;
class APostProcessVolume;
class UExponentialHeightFogComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UMaterialParameterCollection;
class UPrimitiveComponent;
class UStaticMesh;

UCLASS()
class FORGOTTENTHIRD_API UFTWorldWaveSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& InCollection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	// A wave from Origin (world cm) starting at StartTime on the wave clock. A wave already set is replaced; the tracked
	// things start it from the state the side beyond its front has.
	void StartWave(const FVector& Origin, double StartTime, bool bReverse);
	// Ends the wave: the collection cleared, the volume off, the fog and the exposure back, every tracked thing as the
	// first world has it.
	void StopWave();
	bool HasWave() const { return bWave; }

	// The proof dressing round Origin (world cm) within Radius (cm); returns how many things it tracks now.
	int32 Dress(const FVector& Origin, double Radius);

	// Holds the wave clock at Seconds (a fixed-step capture); ReleaseClock goes back to game time.
	void SetClock(double Seconds);
	void ReleaseClock();
	double GetClock() const;

private:
	// One tagged thing the wave cuts.
	struct FTracked
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		ftcore::WorldWaveThing Thing;
		FVector FootCentre = FVector::ZeroVector; // its footprint for the pawn test: a box's centre and half size, cm
		FVector FootExtent = FVector::ZeroVector;
		TEnumAsByte<ECollisionEnabled::Type> Collision = ECollisionEnabled::QueryAndPhysics; // when solid
		bool bDrawn = true;
		bool bSolid = true;
		bool bHeld = false;
		float Local = 0.f; // the local front last written (cm), 0 for none
	};
	struct FFog
	{
		TWeakObjectPtr<UExponentialHeightFogComponent> Component;
		float Density = 0.f; // the map's own
		float Written = 0.f;
	};

	void PostActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds);
	void Step(UWorld& World);
	// The volume with the ink material and the exposure hold, spawned on the first wave.
	void EnsureVolume(UWorld& World);
	void SetExposureHold(bool bHold);
	void GatherPawns(UWorld& World);
	bool PawnInFootprint(const FTracked& Item, const APawn* PlayerPawn) const;
	// A thing as one side of the front has it (drawn and solid on its own world's side), its wave state reset.
	void ResetThing(FTracked& Item, bool bOldSide);
	// Tracks a primitive with its tag (the custom primitive data set here), its pivot, radius and footprint from Mesh at
	// WorldTransform.
	void Track(UPrimitiveComponent& Component, ftcore::WorldTag Tag, const UStaticMesh& Mesh, const FTransform& WorldTransform);
	// The material a dressed prop's slot takes: a twin instance when its master is a prop master, else Material itself.
	UMaterialInterface* CutMaterialFor(UMaterialInterface* Material);
	void ClearCollection(UWorld& World);

	UPROPERTY()
	TObjectPtr<UMaterialParameterCollection> MpcLook;
	UPROPERTY()
	TObjectPtr<APostProcessVolume> Volume;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Instances; // the dressing's twin and bare instances
	UPROPERTY()
	TMap<TObjectPtr<UMaterialInterface>, TObjectPtr<UMaterialInterface>> CutFor; // a dressed slot's material to its cut one

	ftcore::WorldWaveParams Params;
	ftcore::WorldWaveState State;
	ftcore::WorldWaveFrame Frame;
	FVector OriginCm = FVector::ZeroVector;
	bool bWave = false;
	bool bExposureHeld = false;
	bool bHoldClock = false;
	double HeldClock = 0.0;

	// The switches' dressing and wave, run on the first frame once every actor has begun play (OnWorldBeginPlay comes
	// before the actors' BeginPlay).
	bool bBootPending = false;
	bool bBootDress = false;
	bool bBootWave = false;
	bool bBootReverse = false;
	FVector BootDressAt = FVector::ZeroVector;
	double BootDressRadius = 0.0;
	FVector BootWaveAt = FVector::ZeroVector;
	double BootWaveTime = 0.0;

	TArray<FTracked> Tracked;
	TArray<TWeakObjectPtr<APawn>> Pawns;
	TArray<FFog> Fogs;

	FName VectorParams[4];
	FName ScalarParams[6];
	FDelegateHandle PostTickHandle;
};
