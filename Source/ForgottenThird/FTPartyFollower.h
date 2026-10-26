#pragma once

// A party member following the lead on a field step: a skeletal mesh that the explore director
// places on the lead's trail every frame (ftcore::PartyTrail), animated by UFTLocomotionAnimInstance
// from how far it moved: idle, walk and run, the spine banking into turns. It has no controller, no
// movement component and no collision, so it can never block the lead or the autopilot.

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"

#include "FTLocomotionAnimInstance.h"

#include "FTPartyFollower.generated.h"

class UAnimSequence;
class USkeletalMeshComponent;

UCLASS(NotBlueprintable)
class FORGOTTENTHIRD_API AFTPartyFollower : public AActor
{
	GENERATED_BODY()

public:
	AFTPartyFollower();

	// Loads the member's mesh from /Game/Characters/<Name>/<name>/SkeletalMeshes/ and its clips from
	// /Game/Characters/<Name>/Rig/Retargeted/ (FTBuildRig -RetargetFrom), falling back to the mesh's
	// own <name>idle and <name>walk. False when the mesh is missing (the follower then stays
	// invisible). `Lead` is ignored by the ground trace.
	bool Setup(const FString& Name, const AActor* Lead);

	// Stands the follower at `Ground` (cm, any height; the ground below it is found), turning toward the
	// way it moved since the last call; the anim instance gets the speed. On the lead's trail the
	// follower sets off and stops when the lead does, so the lead's intent (SetIntent) drives its
	// starts and stops; -1 plays none.
	void Place(const FVector2D& Ground, float LeadFeetZ, float DeltaSeconds, float WantSpeed = -1.f, float StopDistance = -1.f);

private:
	UPROPERTY(VisibleAnywhere, Category = "Follower")
	TObjectPtr<USkeletalMeshComponent> Body;

	FCollisionQueryParams TraceParams;
	FVector2D LastGround = FVector2D::ZeroVector;
	float Speed = 0.f;
	bool bPlaced = false;
	FFTFootSlideProbe SlideProbe;
};
