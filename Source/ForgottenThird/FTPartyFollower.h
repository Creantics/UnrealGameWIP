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
// (ftcore::TraversalSet), carried along the plan's root path, and then eases back onto its slot, its speed going on from
// the clip's as a person's would (ftcore::easeStray; -LocoNoCatchUpRamp: jumping at once to its slot's pace plus its way
// back, as before). Where the lead caught a ledge, a follower in the air at the same face catches it, planned from its
// own flight (TraversalSet::planCatch); till one fits it is held short of the face, as the lead's capsule is, for it has
// no collision of its own. Off a ledge it falls. -LocoNoFollowerTraversal places it as before, for comparing against: on
// the ground below its slot, popping onto and off what the lead went over or caught.

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"

#include "FTLocomotionAnimInstance.h"
#include "FTLook.h"
#include "FTTraversalProbe.h"
#include "ftcore/WaterBodies.h"
#include "ftcore/WaterFollow.h"
#include "ftcore/WaterLoco.h"

#include "FTPartyFollower.generated.h"

class ACharacter;
class AFTExploreCharacter;
class USceneComponent;
class USkeletalMeshComponent;
class UFTWaterSubsystem;
struct FFTLeadLook;

UCLASS(NotBlueprintable)
class FORGOTTENTHIRD_API AFTPartyFollower : public AActor
{
	GENERATED_BODY()

public:
	AFTPartyFollower();

	// Puts on the member's look: body, clips (the retargeted ones from the look's ClipFolder, the traversal clips
	// among them), pieces and grooms (FTLook::LoadLook). False when there is no body (the follower then stays
	// invisible). `Lead` is ignored by the ground trace and the probes, an explore character lead's vaults, climbs and
	// catches from here on are followed, and a character lead's foot phase sets the follower's steps a quarter stride off
	// its own (-LocoNoPhaseTrim: not).
	bool Setup(const FFTLeadLook& Look, const AActor* Lead);

	// Stands the follower at `Ground` (cm, any height; the ground below it is found), turning toward the
	// way it moved since the last call; the anim instance gets the speed. On the lead's trail the
	// follower sets off and stops when the lead does, so the lead's intent (SetIntent) drives its
	// starts and stops; -1 plays none. Carried by a traversal it goes where its clip goes instead, and is
	// eased back onto `Ground` once let go. In the air at a ledge the lead caught, it stops short of the face till it
	// catches it or comes down clear of it. On a map with a sea (SetSea), where its spot has no footing it stands short of
	// the sea instead, on the way out from `PathPoint` (cm: where the lead walked, at its place on the trail) or, with
	// none, from where it last had footing (ftcore::standShortOfSea).
	void Place(const FVector2D& Ground, float LeadFeetZ, float DeltaSeconds, float WantSpeed = -1.f, float StopDistance = -1.f,
		const FVector2D* PathPoint = nullptr);
	// The map's sea level (cm, world Z): from here on the follower never stands on or over the water, nor on ground at or
	// under the sea level, nor on ground too steep for the lead to walk, nor where the look down meets nothing
	// (-LocoNoShore: it does, as before). Call after Setup.
	void SetSea(float SeaLevel);
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
	// dropped too, and the lead's vaults, climbs and catches still ahead are forgotten); Place can still walk it to a mark.
	// Let go, its next Place counts the move from wherever it stands then, and the lead's vaults, climbs and catches from
	// then on are followed.
	void SetHeld(bool bHeld);
	// The water follow: the lead it goes into the water after (AFTExploreCharacter::StepWaterParty plans for the party),
	// its slot in that plan, and what it tells the plan of itself. In the water (its order's phase past Ashore) Place moves
	// it by its order instead of on the trail: the walk to its entry, the walk down or the jump in, its own WaterLoco at
	// the surface and a dive-follow under, the climb out, and set beside the lead after a rescue. Back on the land it
	// eases onto its slot from where it came out.
	const AFTExploreCharacter* GetWaterLead() const { return LeadExplorer.Get(); }
	// The member's body mesh (the capture checks measure the camera's distance to it).
	const USkeletalMeshComponent* GetBody() const;
	void SetWaterSlot(int32 Slot) { WaterSlot = Slot; }
	void FillWaterMember(ftcore::WaterFollowMember& Member) const;
	// -SwimProof=boom (dev, the 11-18 ruling's item 5): put now at a place (cm; its feet's x and y) and held treading there
	// for the camera's sweep, only while it swims at the surface; false (nothing done) otherwise. ReleaseProofHold lets
	// it follow again.
	bool HoldForProof(const FVector2D& At);
	// -SwimProof=boom (dev, the 11-18 rerun): put now with its body's middle (the middle of its bones' box) at Centre (cm)
	// and kept there, over the water, until ReleaseProofHold; the boom run calls it each tick to keep it on the boom line
	// at the camera's height. Only from the surface swim; false (nothing done) otherwise. The water plan counts it held
	// (as a scene holds a member) meanwhile, so no order moves it and its watchdog waits.
	bool HoldForProofAt(const FVector& Centre);
	void ReleaseProofHold() { bProofHeld = false; bProofAir = false; }
	ftcore::WaterFollowPhase GetWaterPhase() const { return WaterPhase; }
	// A start in the water (a lead switch while swimming, AFTExploreCharacter::StartInWater): counted as placed where it
	// is, so the water plan's first step takes it and the placement puts it at its surface slot, never on land.
	void BeginInWater();
	// The member's name in the water follow's lines (and the proofs').
	const FString& GetWaterName() const { return WaterName; }
	// The wet switch, once a tick after Place (AFTExploreDirector::TickFollowers): its own water soak, stepped from the
	// water it is in as UFTWaterSubsystem::LeadWater steps the lead's, shown under Weather (its party weather value,
	// ftcore::leadwet::partyWeather) by FTLook::ShowWet. Dry and never wet costs a few compares.
	void StepWet(float Weather, float DeltaSeconds);
	const ftcore::WaterWetness& GetWetness() const { return Wet; }
	// Its own soak back after a respawn or a lead switch (ftcore::leadwet::PartyWet); the next show lands the skin at once.
	void SetWetness(const ftcore::WaterWetness& Soak)
	{
		Wet = Soak;
		WetState.bSkinSnap = true;
	}
	// The water it was last sensed in, and whether its wet grooms are on (FTLook::ShowWet).
	const ftcore::WaterState& GetWaterNow() const { return WaterNow; }
	bool IsGroomWet() const { return WetState.bGroomWet; }
	// Its surface step (ftcore::waterSurfaceStep) was held, or slid along the edge, this frame or the last.
	bool IsWaterHeld() const { return WaterBlockedFrame != 0 && WaterBlockedFrame + 1 >= GFrameCounter; }
	bool IsWaterTurned() const { return WaterTurnedFrame != 0 && WaterTurnedFrame + 1 >= GFrameCounter; }
	bool IsWaterBlocked() const { return IsWaterHeld() || IsWaterTurned(); }
	// How many surface steps were held, and slid, counted inside the step's own Hold and Slide branches (the -SwimProof
	// pass lines need the edge path to have run, not only the spot reached).
	int32 GetWaterHeldSteps() const { return WaterHeldSteps; }
	int32 GetWaterSlidSteps() const { return WaterSlidSteps; }
	// -SwimProof=replay (dev): stood at At on the land, its trail walk held, until its order takes it into the water or
	// the run lets it go; from there it eases back onto its slot.
	void PinForProof(const FVector& At) { ProofPin = At; bProofPin = true; }
	void UnpinProof() { bProofPin = false; }
	bool IsProofPinned() const { return bProofPin; }

private:
	// Takes the lead's vaults, climbs and catches begun since the last call, and forgets those the body at Feet is past or
	// far from.
	void TakeMarks(const FVector& Feet);
	// Starts a traversal from Feet at the face ahead where the lead began one: on the ground over or onto it, in the air
	// a catch of it, going on at WantSpeed (cm/s; below 0, at its own). False: none ahead, or no clip fits yet; in the
	// air at the face below its top, the body is then held short of it this frame (bAtWall).
	bool TryTraverse(UFTLocomotionAnimInstance& Anim, const FVector& Feet, float WantSpeed);
	// Carries the body along the traversal under way: Feet is where its clip has it now. False once it lets go there,
	// as it does at the clip's end, or just after the landing while its slot (at Ground) moves on.
	bool TickTraversal(UFTLocomotionAnimInstance& Anim, float DeltaSeconds, const FVector2D& Ground, bool bSlotMoving, FVector& Feet);
	// Eases the body back toward its slot at Ground after a traversal let it go off it, its rate back (StrayRate) changing
	// no faster than a person's speed does, returning how far off it still is (cm).
	FVector2D EaseStray(const FVector2D& Ground, float DeltaSeconds);
	// For the log, once a frame: from when it comes off its slot (let go by a traversal, or held short of a face) till
	// it is back on it, how long that takes, its fastest and its hardest change of speed (WasGoing: last frame's Going).
	void TrackCatchUp(bool bCarried, bool bLetGo, const FVector2D& WasGoing, float DeltaSeconds);
	// On a map with a sea: Spot if it has footing, else where to stand short of the sea (Place). Was is where the body
	// stood last frame; LeadFeetZ and Was bound the look down as Place's does.
	FVector2D KeepAshore(const FVector2D& Spot, const FVector& Was, float LeadFeetZ, const FVector2D* PathPoint);

	// The body hangs off a plain root, so its facing turn survives Place setting the actor's rotation.
	UPROPERTY(VisibleAnywhere, Category = "Follower")
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere, Category = "Follower")
	TObjectPtr<USkeletalMeshComponent> Body;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> Pieces;
	// The look's garment pairs, both halves held (FTLook::PrepareFronts), for switching open or closed.
	UPROPERTY(Transient)
	FFTWornFronts WornFronts;
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
	FVector2D Going = FVector2D::ZeroVector; // its last step (cm/s, level), for a catch's plan as the lead's velocity is
	bool bFacing = false;     // asked to turn on the spot to FaceYaw
	float FaceYaw = 0.f;
	bool bThroughSolid = false; // its slot was inside something solid when last placed (for the log)
	// SetSea: the sea level (cm) it keeps out of; where it last stood with footing; and whether it is held short of the
	// sea now (for the log).
	bool bShore = false;
	float ShoreLevel = 0.f;
	float ShoreWalkableZ = 0.71f; // the steepest ground it stands on (the up part of its normal), as the lead walks
	FVector2D LastDry = FVector2D::ZeroVector;
	bool bHasLastDry = false;
	bool bShoreHeld = false;
	FFTFootSlideProbe SlideProbe;
	// -LocoNoFollowerTraversal.
	bool bNoTraversal = false;
	// -LocoNoCatchUpRamp.
	bool bNoCatchUpRamp = false;
	// The lead's vaults, climbs and catches still ahead of the body, the oldest first; how many the lead had begun when
	// last looked; and why the last look ahead started none (for the log, when one is gone past unfollowed).
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
	// How far off its slot the body still is since a traversal let it go (cm), the way that traversal went, and the rate
	// it goes at back (cm/s: its velocity less its slot's, from how it moved as a traversal let it go or a face held it
	// short, and the ease's own from there).
	FVector2D Stray = FVector2D::ZeroVector;
	FVector2D StrayWay = FVector2D(1.0, 0.0);
	FVector2D StrayRate = FVector2D::ZeroVector;
	// Since it came off its slot (s; below 0, on it), its rate back then, its fastest (cm/s) and its hardest change of
	// speed (cm/s^2).
	float StrayClock = -1.f;
	float StrayRateFrom = 0.f;
	float StrayTop = 0.f;
	float StrayTopAccel = 0.f;
	// In the air this frame at a ledge the lead caught, below its top with no catch fitting yet: where its way in meets
	// the face, and that way (level, unit). Having no collision, the body is held short of it, as the lead's capsule is.
	bool bAtWall = false;
	FVector WallFace = FVector::ZeroVector;
	FVector WallIn = FVector::ForwardVector;
	// The water follow (TickWater: true when its order moved the body this frame, Place then does nothing more).
	bool TickWater(const FVector2D& Ground, float DeltaSeconds, UFTLocomotionAnimInstance* Anim);
	void WaterWalk(const FVector2D& To, float WalkSpeed, float DeltaSeconds, UFTLocomotionAnimInstance* Anim);
	void SenseWater(const UFTWaterSubsystem& System, const FVector& At, bool bSwimming);
	bool FindWaterEdge(const UFTWaterSubsystem& System, const FVector& At, const FVector& LeadAt);
	// The bank's edge on the way from From (cm) to the lead, in the lead's own water (other water passed over): Edge and
	// the way in there (m). False with no water that way within 20 m.
	bool MarchWaterEdge(const ftcore::WaterBodies& Bodies, const FVector2D& From, const FVector& LeadAt, ftcore::Vec2& Edge, ftcore::Vec2& Out) const;
	// A jump from Edge (m, along Out) cut off from the lead: the jump's line from the bank to its landing, or the swim's
	// from the landing to the lead at the surface, meets a solid (a Visibility trace), or the swim's crosses land or other
	// water. 11-21 replay 6: a block in the sea between the quay and the lead.
	bool WaterJumpCutOff(const UFTWaterSubsystem& System, ftcore::Vec2 Edge, ftcore::Vec2 Out, double BankZ, const FVector& LeadAt) const;
	// The water's state for a swimmer at the surface at P (cm): its feet by the one surface rule over the bed there, the
	// bed traced from 60 cm over the surface (a deck lower than that blocks) down 30 m. No surface there, or the trace
	// starting inside a solid (a pier, a quay's block), is Dry. Allocation-free (the member query params).
	ftcore::WaterState SampleSwimAt(const UFTWaterSubsystem& System, const FVector2D& P, float Sink) const;
	// From P (cm) toward To in 50 cm steps, no further than MaxCm (nor past To): the first point the swimmer is in the water
	// (ftcore::waterIn) and in To's own body when To is in one, into Out. False (Out untouched) when there is none that near.
	bool IntoWaterToward(const UFTWaterSubsystem& System, const FVector2D& P, const FVector2D& To, double MaxCm, FVector2D& Out) const;
	void StepFollowerWaterLoco(ftcore::WaterLocoInput& In, float DeltaSeconds, UFTLocomotionAnimInstance* Anim);
	FString WaterName;
	int32 WaterSlot = -1;
	ftcore::WaterFollowPhase WaterPhase = ftcore::WaterFollowPhase::Ashore; // the order's phase it last acted on
	ftcore::SwimState WaterSwimWas = ftcore::SwimState::Surface;
	ftcore::WaterLoco WaterLocoBody;
	ftcore::WaterLocoPose WaterPose;
	ftcore::WaterState WaterNow;
	ftcore::WaterWetness Wet;                   // its own water soak (StepWet)
	UPROPERTY(Transient)
	FFTWetState WetState;                       // what ShowWet last wrote on it
	TArray<FFTGroomLook> GroomLooks;            // the look's grooms, beside GroomComponents (their wet grooms)
	bool bWaterLocoReady = false;
	TWeakObjectPtr<UAnimInstance> WaterClipsAnim;
	float WaterBedZ = -10.f;                    // m: the bed under it, last looked at
	ftcore::Vec2 WaterEdge{};                   // m: the bank's edge toward the lead, and the way in there
	ftcore::Vec2 WaterEdgeOut{1.f, 0.f};
	bool bWaterHasEdge = false;                 // water was found toward the lead (else no jump in)
	bool bWaterEdgeCutOff = false;              // that edge's jump is cut off from the lead by a solid
	ftcore::WaterEntryEdge WaterAltEdges[ftcore::kWaterEntryAlts]; // then: clear edges further along the bank, nearest first
	int32 WaterAltEdgeCount = 0;
	int32 WaterEdgeLogged = -2;                 // the cut-off state last logged (-1 clear, else the alternatives found)
	float WaterEdgeAge = 1.f;                   // s since the edge was last looked for
	// -SwimProof (AFTExploreCharacter::TickSwimProof; dev): slot 0 is held stuck until it is rejoined, then its feet
	// height is logged four times a second for 2 s, then its spread over those 2 s (cm): the lowest and highest feet, and
	// the most they stood off the height the surface swim held them at that frame (bProofWant: a surface frame was seen).
	bool bProofFreed = false;
	float ProofLogLeft = 0.f;
	float ProofLogAt = 0.f;
	double ProofZMin = 0.0;
	double ProofZMax = 0.0;
	double ProofWantOff = 0.0;
	bool bProofWant = false;
	int32 ProofFrames = 0;
	bool bProofHeld = false; // HoldForProof: treading where it was put
	bool bProofAir = false; // HoldForProofAt: kept where the boom run puts it (its own swim step skipped)
	// Placed in the water (the rejoin at its surface slot, or treading while the lead swims): a held scene leaves it there.
	bool bPlacedInWater = false;
	FVector WaterVelocity = FVector::ZeroVector; // cm/s
	// The surface swim's ride under the feet when last placed or stepped (cm: waterSwimFeetZ there, the waves and the
	// bob), the frame it was taken, and whether the feet's target was on it (not held up by the bed): the next frame's
	// step carries the feet by the ride's move (ftcore::waterFeetCarry) only from the frame before, and only on the ride.
	double WaterRideZ = 0.0;
	uint64 WaterRideFrame = 0;
	bool bWaterOnRide = false;
	// The surface step's slide (ftcore::waterSurfaceStep): the side it turns to at an edge, kept while it goes round, and
	// the last frame its step was turned or held (the party's watchdog counts that, ftcore::WaterFollowMember::blocked).
	float WaterSlideSide = 1.f;
	uint64 WaterBlockedFrame = 0; // held (WaterStepKind::Hold)
	uint64 WaterTurnedFrame = 0;  // slid along the edge (WaterStepKind::Slide)
	int32 WaterHeldSteps = 0;     // the steps held, and slid, since spawn
	int32 WaterSlidSteps = 0;
	bool bWaterRejoinWarned = false; // the rejoin slot's no-water warning, once until a slot is found again
	FVector2D WaterRejoinPlacedAt = FVector2D::ZeroVector; // cm: where the last rejoin put it (FillWaterMember's placedAt)
	bool bWaterRejoinPlaced = false;
	FVector ProofPin = FVector::ZeroVector; // PinForProof (dev)
	bool bProofPin = false;
	// -SwimFeetEaseAll: the surface step eases the whole way to its target, the ride not carried (the fallback; the step
	// before 11-18, which trailed the waves and the bob by 3.3 cm on the open sea).
	bool bSwimFeetEaseAll = false;
	// A placed move (the entry's walk or jump, the climb out): from, to, its time and length, and whether it is over.
	bool bWaterMove = false;
	bool bWaterMoveDone = false;
	FVector WaterMoveFrom = FVector::ZeroVector;
	FVector WaterMoveTo = FVector::ZeroVector;
	float WaterMoveTime = 0.f;
	float WaterMoveLength = 1.f;
	// The climb out on Core's path (-PartyExitFast: the swim to the foot, up, across), taken when the climb starts.
	bool bWaterClimbPath = false;
	ftcore::WaterClimbPath WaterClimb;
	bool bWaterAtEntry = false;
	bool bWaterAtExit = false;
};
