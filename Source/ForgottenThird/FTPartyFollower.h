#pragma once

// A party member following the lead on a field step: a skeletal mesh that the explore director
// places on the lead's trail every frame (ftcore::PartyTrail), animated by UFTLocomotionAnimInstance
// from how far it moved: idle, walk and run, the spine banking into turns. It has no controller, no
// movement component and no collision, so it can never block the lead or the autopilot. Its look is
// the member's entry in the explore character's PartyLooks, or its own assets when there is none
// (FTLook.h).
//
// Where the lead vaults or climbs (AFTExploreCharacter::GetLastTraversal), a follower that meets the same face goes
// over or onto it as the lead does, from its own probe ahead (FTTraversalProbe) and its own clips' plan
// (ftcore::TraversalSet), carried along the plan's root path, and then eases back onto its slot. Off a ledge it falls.
// -LocoNoFollowerTraversal places it as before, for comparing against: on the ground below its slot, popping onto and
// off what the lead went over.

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"

#include "FTLocomotionAnimInstance.h"
#include "FTTraversalProbe.h"

#include "FTPartyFollower.generated.h"

class ACharacter;
class AFTExploreCharacter;
class USceneComponent;
class USkeletalMeshComponent;
struct FFTLeadLook;

UCLASS(NotBlueprintable)
class FORGOTTENTHIRD_API AFTPartyFollower : public AActor
{
	GENERATED_BODY()

public:
	AFTPartyFollower();

	// Puts on the member's look: body, clips (the retargeted ones from the look's ClipFolder, the traversal clips
	// among them), pieces and grooms (FTLook::LoadLook). False when there is no body (the follower then stays
	// invisible). `Lead` is ignored by the ground trace and the probes, an explore character lead's vaults and climbs
	// from here on are followed, and a character lead's foot phase sets the follower's steps a quarter stride off its
	// own (-LocoNoPhaseTrim: not).
	bool Setup(const FFTLeadLook& Look, const AActor* Lead);

	// Stands the follower at `Ground` (cm, any height; the ground below it is found), turning toward the
	// way it moved since the last call; the anim instance gets the speed. On the lead's trail the
	// follower sets off and stops when the lead does, so the lead's intent (SetIntent) drives its
	// starts and stops; -1 plays none. Carried by a traversal it goes where its clip goes instead, and is
	// eased back onto `Ground` once let go.
	void Place(const FVector2D& Ground, float LeadFeetZ, float DeltaSeconds, float WantSpeed = -1.f, float StopDistance = -1.f);
	// Plays a running turn by `Delta` degrees from here, as the lead did at this spot on its trail
	// (ftcore::PartyTrail::turnsReached). Call before Place.
	void ReplayTurn(float Delta);
	// Jumps from here as the lead did at this spot (ftcore::PartyTrail::jumpsReached): a hop at UpSpeed
	// (cm/s) under Gravity (cm/s^2, positive), landing back on the ground below it. Not in the air already, nor
	// carried by a traversal.
	void ReplayJump(float UpSpeed, float Gravity);
	// Standing, turns on the spot to face Yaw (degrees, world), as the lead turned to face something.
	void FaceTo(float Yaw);
	// For a scene that places the party itself (AFTExploreDirector::SetPartyHeld). Held, it stands where it is, on the
	// ground, its speed dropped to a stand (a replayed jump, a turn on the spot asked for and a traversal under way are
	// dropped too, and the lead's vaults and climbs still ahead are forgotten); Place can still walk it to a mark. Let go,
	// its next Place counts the move from wherever it stands then, and the lead's vaults and climbs from then on are
	// followed.
	void SetHeld(bool bHeld);

private:
	// Takes the lead's vaults and climbs begun since the last call, and forgets those the body at Feet is past or far
	// from.
	void TakeMarks(const FVector& Feet);
	// Starts a traversal from Feet over or onto the face ahead where the lead began one (false: none ahead, or no clip
	// fits yet).
	bool TryTraverse(UFTLocomotionAnimInstance& Anim, const FVector& Feet);
	// Carries the body along the traversal under way: Feet is where its clip has it now. False once it lets go there,
	// as it does at the clip's end, or just after the landing while its slot (at Ground) moves on.
	bool TickTraversal(UFTLocomotionAnimInstance& Anim, float DeltaSeconds, const FVector2D& Ground, bool bSlotMoving, FVector& Feet);
	// Eases the body back toward its slot at Ground after a traversal let it go off it, returning how far off it still
	// is (cm).
	FVector2D EaseStray(const FVector2D& Ground, float DeltaSeconds);

	// The body hangs off a plain root, so its facing turn survives Place setting the actor's rotation.
	UPROPERTY(VisibleAnywhere, Category = "Follower")
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere, Category = "Follower")
	TObjectPtr<USkeletalMeshComponent> Body;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> Pieces;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USceneComponent>> GroomComponents;

	FString LogName; // "FTPartyFollower <member>"
	FCollisionQueryParams TraceParams;
	TWeakObjectPtr<const ACharacter> LeadCharacter;
	TWeakObjectPtr<const AFTExploreCharacter> LeadExplorer;
	FVector2D LastGround = FVector2D::ZeroVector; // where the body stood on the ground last placed
	FVector2D LastSlot = FVector2D::ZeroVector;   // and where its slot was
	float Speed = 0.f;
	bool bPlaced = false;
	bool bHeldByScene = false;
	uint32 TurnsSeen = 0;
	float TurnEntryYaw = 0.f; // the yaw a replayed turn began at
	bool bAir = false;        // in a replayed jump or a fall: going up this fast (cm/s) under this gravity (cm/s^2)
	float AirSpeed = 0.f;
	float AirGravity = 0.f;
	bool bFacing = false;     // asked to turn on the spot to FaceYaw
	float FaceYaw = 0.f;
	bool bThroughSolid = false; // its slot was inside something solid when last placed (for the log)
	FFTFootSlideProbe SlideProbe;
	// -LocoNoFollowerTraversal.
	bool bNoTraversal = false;
	// The lead's vaults and climbs still ahead of the body, the oldest first; how many the lead had begun when last
	// looked; and why the last look ahead started none (for the log, when one is gone past unfollowed).
	static constexpr int32 MaxMarks = 4;
	FFTTraversalMark Marks[MaxMarks];
	int32 NumMarks = 0;
	uint32 MarksSeen = 0;
	int32 LastMiss = 0;
	// The traversal under way, as the lead's (AFTExploreCharacter::TickTraversal): its plan, clip time, and the body's
	// feet and facing as it began.
	bool bTraversing = false;
	ftcore::TraversalPlan TravPlan;
	float TravTime = 0.f;
	FTransform TravFrame;
	// How far off its slot the body still is since a traversal let it go (cm), and the way that traversal went.
	FVector2D Stray = FVector2D::ZeroVector;
	FVector2D StrayWay = FVector2D(1.0, 0.0);
};
