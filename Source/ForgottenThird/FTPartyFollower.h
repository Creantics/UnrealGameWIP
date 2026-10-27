#pragma once

// A party member following the lead on a field step: a skeletal mesh with idle and walk clips that
// the explore director places on the lead's trail every frame (ftcore::PartyTrail). It has no
// controller, no movement component and no collision, so it can never block the lead or the
// autopilot. Its look is the member's entry in the explore character's PartyLooks, or its own
// assets when there is none (FTLook.h).

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"

#include "FTPartyFollower.generated.h"

class UAnimSequence;
class USceneComponent;
class USkeletalMeshComponent;
struct FFTLeadLook;

UCLASS(NotBlueprintable)
class FORGOTTENTHIRD_API AFTPartyFollower : public AActor
{
	GENERATED_BODY()

public:
	AFTPartyFollower();

	// Puts on the member's look: body, clips, pieces and grooms (FTLook::LoadLook). False when there
	// is no body (the follower then stays invisible). `Lead` is ignored by the ground trace.
	bool Setup(const FFTLeadLook& Look, const AActor* Lead);

	// Stands the follower at `Ground` (cm, any height; the ground below it is found), facing the way
	// it moved since the last call, walking when it moved and idle when it didn't.
	void Place(const FVector2D& Ground, float LeadFeetZ, float DeltaSeconds);

private:
	void PlayClip(UAnimSequence* Clip);

	// The body hangs off a plain root, so its facing turn survives Place setting the actor's rotation.
	UPROPERTY(VisibleAnywhere, Category = "Follower")
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere, Category = "Follower")
	TObjectPtr<USkeletalMeshComponent> Body;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> IdleClip;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> WalkClip;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> Pieces;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USceneComponent>> GroomComponents;

	FCollisionQueryParams TraceParams;
	FVector2D LastGround = FVector2D::ZeroVector;
	bool bPlaced = false;
	bool bWalking = false;
	bool bClipStarted = false;
};
