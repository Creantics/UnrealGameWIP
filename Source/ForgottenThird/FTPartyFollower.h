#pragma once

// A party member following the lead on a field step: a skeletal mesh with idle and walk clips that
// the explore director places on the lead's trail every frame (ftcore::PartyTrail). It has no
// controller, no movement component and no collision, so it can never block the lead or the
// autopilot.

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"

#include "FTPartyFollower.generated.h"

class UAnimSequence;
class USkeletalMeshComponent;

UCLASS(NotBlueprintable)
class FORGOTTENTHIRD_API AFTPartyFollower : public AActor
{
	GENERATED_BODY()

public:
	AFTPartyFollower();

	// Loads the member's mesh and clips from /Game/Characters/<Name>/<name>/SkeletalMeshes/. False
	// when the mesh is missing (the follower then stays invisible). `Lead` is ignored by the ground
	// trace.
	bool Setup(const FString& Name, const AActor* Lead);

	// Stands the follower at `Ground` (cm, any height; the ground below it is found), facing the way
	// it moved since the last call, walking when it moved and idle when it didn't.
	void Place(const FVector2D& Ground, float LeadFeetZ, float DeltaSeconds);

private:
	void PlayClip(UAnimSequence* Clip);

	UPROPERTY(VisibleAnywhere, Category = "Follower")
	TObjectPtr<USkeletalMeshComponent> Body;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> IdleClip;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> WalkClip;

	FCollisionQueryParams TraceParams;
	FVector2D LastGround = FVector2D::ZeroVector;
	bool bPlaced = false;
	bool bWalking = false;
	bool bClipStarted = false;
};
