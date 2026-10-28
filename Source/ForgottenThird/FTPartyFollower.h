#pragma once

// A party member following the lead on a field step: a skeletal mesh that the explore director
// places on the lead's trail every frame (ftcore::PartyTrail), animated by UFTLocomotionAnimInstance
// from how far it moved: idle, walk and run, the spine banking into turns. It has no controller, no
// movement component and no collision, so it can never block the lead or the autopilot. Its look is
// the member's entry in the explore character's PartyLooks, or its own assets when there is none
// (FTLook.h).

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"

#include "FTLocomotionAnimInstance.h"

#include "FTPartyFollower.generated.h"

class ACharacter;
class USceneComponent;
class USkeletalMeshComponent;
struct FFTLeadLook;

UCLASS(NotBlueprintable)
class FORGOTTENTHIRD_API AFTPartyFollower : public AActor
{
	GENERATED_BODY()

public:
	AFTPartyFollower();

	// Puts on the member's look: body, clips (the retargeted ones from the look's ClipFolder), pieces and
	// grooms (FTLook::LoadLook). False when there is no body (the follower then stays invisible). `Lead` is
	// ignored by the ground trace, and a character lead's foot phase sets the follower's steps a quarter
	// stride off its own (-LocoNoPhaseTrim: not).
	bool Setup(const FFTLeadLook& Look, const AActor* Lead);

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
	// For a scene that places the party itself (AFTExploreDirector::SetPartyHeld). Held, it stands where it is, on the
	// ground, its speed dropped to a stand (a replayed jump and a turn on the spot asked for are dropped too); Place can
	// still walk it to a mark. Let go, its next Place counts the move from wherever it stands then.
	void SetHeld(bool bHeld);

private:
	// The body hangs off a plain root, so its facing turn survives Place setting the actor's rotation.
	UPROPERTY(VisibleAnywhere, Category = "Follower")
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere, Category = "Follower")
	TObjectPtr<USkeletalMeshComponent> Body;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> Pieces;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USceneComponent>> GroomComponents;

	FCollisionQueryParams TraceParams;
	TWeakObjectPtr<const ACharacter> LeadCharacter;
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
