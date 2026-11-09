#pragma once

// One skeletal mesh stood on a game map from the command line, looping one clip in place: a character put on a
// world map to be looked at (an enemy on the look map before it has a spawner), usually with FTWorldEyeShotSubsystem
// taking the still in the same run.
//
//   UnrealEditor-Cmd ForgottenThird.uproject /Game/Maps/<map> -game -RenderOffScreen -unattended -nosound
//       -PlaceSkin=<mesh object path> -PlaceAt=<x,y,z> [-PlaceAnim=<clip object path>] [-PlaceFace=<x,y,z>]
//       [-PlaceSide=1000]   (plus the -WorldEyeShot switches for the still)
//
// -PlaceAt and -PlaceFace are in the plan frame of a square -PlaceSide metres across (ftcore::massingEnginePoint),
// z in metres, as the massing tables and -WorldEye use. The mesh stands with its soles at -PlaceAt and turns to
// face -PlaceFace (the eye, say), its own forward read off the reference pose (FTMeshFacing); without -PlaceFace it
// faces +X. For a few seconds after begin play a line down from 2 m above -PlaceAt looks for the ground, so a
// streamed-in floor still catches it; the plan z stands when nothing is hit. No AI, no collision, no spawner. A mesh
// or clip that doesn't load ends the game with 1.

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "FTPlaceSkeletalSubsystem.generated.h"

class ASkeletalMeshActor;

UCLASS()
class FORGOTTENTHIRD_API UFTPlaceSkeletalSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bSnapping; }

private:
	UPROPERTY()
	TObjectPtr<ASkeletalMeshActor> Placed;

	// Where the soles go before the ground is found, in engine centimetres.
	FVector Base = FVector::ZeroVector;
	FCollisionQueryParams GroundQuery;
	float Searched = 0.f;
	bool bSnapping = false;
};
