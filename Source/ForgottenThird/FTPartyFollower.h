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
	// Plays a running turn by `Delta` degrees from here, as the lead did at this spot on its trail
	// (ftcore::PartyTrail::turnsReached). Call before Place.
	void ReplayTurn(float Delta);
	// Jumps from here as the lead did at this spot (ftcore::PartyTrail::jumpsReached): a hop at UpSpeed
	// (cm/s) under Gravity (cm/s^2, positive), landing back on the ground below it.
	void ReplayJump(float UpSpeed, float Gravity);
	// Standing, turns on the spot to face Yaw (degrees, world), as the lead turned to face something.
	void FaceTo(float Yaw);

private:
	UPROPERTY(VisibleAnywhere, Category = "Follower")
	TObjectPtr<USkeletalMeshComponent> Body;

	FCollisionQueryParams TraceParams;
	FVector2D LastGround = FVector2D::ZeroVector;
	float Speed = 0.f;
	bool bPlaced = false;
	uint32 TurnsSeen = 0;
	float TurnEntryYaw = 0.f; // the yaw a replayed turn began at
	bool bAir = false;        // in a replayed jump: this high above the ground (cm) and rising this fast
	float AirHeight = 0.f;
	float AirSpeed = 0.f;
	float AirGravity = 0.f;
	bool bFacing = false;     // asked to turn on the spot to FaceYaw
	float FaceYaw = 0.f;
	FFTFootSlideProbe SlideProbe;
};
