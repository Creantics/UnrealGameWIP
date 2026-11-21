#include "FTPartyFollower.h"

#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/CommandLine.h"

#include "FTExploreCharacter.h"
#include "FTLook.h"
#include "FTMeshFacing.h"
#include "ForgottenThird.h"

#include "ftcore/LeadWet.h"
#include "ftcore/PartyShore.h"
#include "ftcore/WaterSurfaceFeet.h"
#include "FTSwimUnits.h"
#include "FTWaterSubsystem.h"
#include "ftcore/PartyTrail.h"

namespace
{
// Slower than this (cm/s) counts as standing still. A speed, not a distance: at a high frame rate a walking
// follower moves well under a centimetre a frame.
constexpr float kStillSpeed = 10.f;
// The time constant (s) of the follower's turn toward the way it moves, as the lead's facing eases (LocoTuning::faceTime).
constexpr float kFaceTime = 0.15f;
// And never faster than this (deg/s): a walking body's turn, not a spin.
constexpr float kMaxFaceRate = 220.f;
// How fast the speed fed to the anim follows each frame's step (1/s), so uneven frames don't jitter the stride.
constexpr float kSpeedResponse = 12.f;
// Asked to face a way, it has once it is this near (degrees): nearer than LocoTuning::faceMin, no turn on the spot starts.
constexpr float kFacedWithin = 40.f;
// As high as a walking body steps (cm, the character movement's MaxStepHeight): a floor further down is fallen to, and
// a change of height past it in one frame (a pop) is logged.
constexpr float kStepHeight = 45.f;
// Kept out of the sea (SetSea): ground no higher than this over the sea level (cm) is the sea's; and with nowhere dry on
// the way out from where the lead walked, it waits where it last had footing while that is within this of its spot (cm),
// past it going on as before.
constexpr double kShoreMargin = 10.0;
constexpr double kShoreLeash = 600.0;
// With no lead to take it from, ground no steeper than this (the up part of its normal: CharacterMovement's default
// 44.765 degrees) is walkable.
constexpr float kShoreWalkableZ = 0.71f;
// The lead's vaults, climbs and catches (cm): the face ahead is looked at while one is ahead within the probes' reach
// and the body goes its way at least this squarely (the cosine), and is the lead's when within kMarkSlack of it (along
// its way); one is forgotten once the body is kMarkPassed past it or kMarkFar from it.
constexpr float kMarkReach = 560.f;
constexpr float kMarkFacing = 0.8f;
constexpr float kMarkSlack = 60.f;
constexpr float kMarkPassed = 100.f;
constexpr float kMarkFar = 2000.f;
// Carried by a catch, the body turns square into the face with this time constant (s), as the lead does.
constexpr float kCatchTurnTime = 0.05f;
// Why the last look ahead started no traversal (LastMiss), for the log.
const TCHAR* const kMissNames[] = {TEXT("not looked for: not going its way, or on the ground at a catch, or in the air at a vault or climb"),
	TEXT("nothing ahead to go over, onto or catch"), TEXT("the face ahead not the lead's"), TEXT("still out of reach"), TEXT("no clip fits")};
}

const USkeletalMeshComponent* AFTPartyFollower::GetBody() const
{
	return Body;
}

AFTPartyFollower::AFTPartyFollower()
{
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Root);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetGenerateOverlapEvents(false);
	SetActorEnableCollision(false);
	TraceParams = FCollisionQueryParams(FName(TEXT("FTPartyFollower")), false);
}

bool AFTPartyFollower::Setup(const FFTLeadLook& Look, const AActor* Lead)
{
	TraceParams.AddIgnoredActor(this);
	if (Lead)
	{
		TraceParams.AddIgnoredActor(Lead);
	}
	if (!FParse::Param(FCommandLine::Get(), TEXT("LocoNoPhaseTrim")))
	{
		LeadCharacter = Cast<ACharacter>(Lead);
	}
	// The lead's vaults, climbs and catches from here on.
	bNoTraversal = FParse::Param(FCommandLine::Get(), TEXT("LocoNoFollowerTraversal"));
	bNoCatchUpRamp = FParse::Param(FCommandLine::Get(), TEXT("LocoNoCatchUpRamp"));
	bSwimFeetEaseAll = FParse::Param(FCommandLine::Get(), TEXT("SwimFeetEaseAll"));
	LeadExplorer = Cast<AFTExploreCharacter>(Lead);
	MarksSeen = LeadExplorer.IsValid() ? LeadExplorer->GetTraversalsBegun() : 0;
	LogName = FString::Printf(TEXT("FTPartyFollower %s"), *Look.Name);
	WaterName = Look.Name.IsEmpty() ? FString(TEXT("Devin")) : Look.Name;
	const FFTLoadedLook Loaded = FTLook::LoadLook(Look, *LogName);
	if (!Loaded.Body)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: no body; it stays hidden."), *LogName);
		SetActorHiddenInGame(true);
		return false;
	}
	Body->SetSkeletalMesh(Loaded.Body);
	// Turn the body to face the actor's forward (+X).
	Body->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Loaded.Body), 0.0));
	Body->SetAnimInstanceClass(UFTLocomotionAnimInstance::StaticClass());
	WornFronts = Loaded.Fronts;
	const int32 Cloth = FTLook::AddPieces(*this, *Body, Loaded.Pieces, Pieces, *LogName);
	FTLook::AddGrooms(*this, *Body, Pieces, Look.Grooms, GroomComponents, *LogName);
	GroomLooks = Look.Grooms;
	FTLook::SwapMaterials(*Body, Pieces, Look.MaterialSwaps, *LogName);
	UE_LOG(LogForgottenThird, Display, TEXT("%s: body %s facing %.0f, %d pieces (%d with cloth), %d grooms, clips from %s."), *LogName,
		*Loaded.Body->GetName(), Body->GetRelativeRotation().Yaw, Pieces.Num(), Cloth, GroomComponents.Num(), *Loaded.ClipFolder);
	// The lead's jumps are replayed on the trail (ReplayJump), and its climbs, vaults and catches gone over again with the
	// traversal clips (-LocoNoFollowerTraversal: not, so none).
	if (UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance()))
	{
		Anim->SetBaseClips(Loaded.Clips, !bNoTraversal);
	}
	return true;
}

void AFTPartyFollower::Place(const FVector2D& Ground, float LeadFeetZ, float DeltaSeconds, float WantSpeed, float StopDistance,
	const FVector2D* PathPoint)
{
	// Before it moves: the pose and the placement are both last frame's. No foot is planted in the air, nor while a
	// traversal carries the body.
	SlideProbe.Sample(*Body, DeltaSeconds, Speed, bAir || bTraversing);
	const FVector Was = GetActorLocation();
	const bool bWasPlaced = bPlaced;
	const bool bSlotMoving = bWasPlaced && DeltaSeconds > 0.f && FVector2D::Distance(Ground, LastSlot) > kStillSpeed * DeltaSeconds;
	// Its slot's velocity (cm/s): off it, the body goes at that plus its rate back (StrayRate).
	const FVector2D SlotGoing = bWasPlaced && DeltaSeconds > 0.f ? (Ground - LastSlot) / DeltaSeconds : FVector2D::ZeroVector;
	LastSlot = Ground;
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance());
	// In the water its order moves it (TickWater); back on the land, the trail again.
	if (TickWater(Ground, DeltaSeconds, Anim))
	{
		bProofPin = false; // its order has it now (-SwimProof=replay)
		return;
	}
#if !UE_BUILD_SHIPPING
	if (bProofPin)
	{
		// -SwimProof=replay: stood where the run put it; let go, it eases back onto its slot from there.
		SetActorLocation(ProofPin, false, nullptr, ETeleportType::TeleportPhysics);
		Stray = FVector2D(ProofPin.X, ProofPin.Y) - Ground;
		StrayWay = Stray.GetSafeNormal();
		StrayRate = FVector2D::ZeroVector;
		Speed = 0.f;
		if (Anim)
		{
			Anim->SetGroundSpeed(0.f);
		}
		return;
	}
#endif

	// Meeting a face the lead vaulted or climbed, it goes over or onto it too, as the lead does, and in the air at a
	// ledge the lead caught it catches it: begun from where it is, carried along its clip from this frame.
	bAtWall = false;
	if (!bNoTraversal && !bHeldByScene && DeltaSeconds > 0.f && Anim)
	{
		TakeMarks(Was);
		if (!bTraversing && NumMarks > 0 && (bAir || Speed > kStillSpeed))
		{
			TryTraverse(*Anim, Was, WantSpeed);
		}
	}
	FVector Carried = Was;
	const bool bWasTraversing = bTraversing;
	const bool bCarried = bTraversing && Anim && TickTraversal(*Anim, DeltaSeconds, Ground, bSlotMoving, Carried);
	bTraversing = bCarried;
	// Let go this frame, it is where its clip let it go, at the clip's speed (-LocoNoCatchUpRamp: a step back toward its
	// slot already, as before).
	const bool bLetGo = bWasTraversing && !bCarried;
	// Otherwise on its slot, or on its way back to it.
	FVector2D Spot = bCarried ? FVector2D(Carried.X, Carried.Y) : Ground + (bLetGo && !bNoCatchUpRamp ? Stray : EaseStray(Ground, DeltaSeconds));
	// Not on through a face the lead caught while it may yet catch it: held short of it, as the lead's capsule is stopped
	// there. Landed below it, or risen clear of its top, it eases back onto its slot from there.
	bool bHeldShort = false;
	if (bAtWall && !bCarried)
	{
		const FVector2D In(WallIn.X, WallIn.Y);
		const double Over = FVector2D::DotProduct(Spot - FVector2D(WallFace.X, WallFace.Y), In) + AFTExploreCharacter::CapsuleRadius;
		if (Over > 0.0)
		{
			Spot -= In * Over;
			Stray = Spot - Ground;
			StrayWay = In;
			bHeldShort = true;
		}
	}
	// The lead under water (diving, swimming under, or carried out by a rescue): nobody follows it down, and the Third
	// never dives. Each holds where it last stood dry, at the bank, until the lead is back at the surface; then it eases
	// back onto its slot from there.
	if (!bCarried && bHasLastDry)
	{
		if (const AFTExploreCharacter* Lead = LeadExplorer.Get(); Lead && Lead->IsUnderWater())
		{
			StrayWay = (Ground - LastDry).GetSafeNormal();
			Spot = LastDry;
			Stray = Spot - Ground;
			bHeldShort = true;
		}
	}
	// On a map with a sea it stands on the land: a spot with no footing (out over the water, on ground at or under the sea
	// level, or over a drop the look down doesn't reach) is given up for a point short of the sea on the way out from
	// where the lead walked, so it walks the shore beside the lead (in a jump or a fall too). Not while a traversal
	// carries it.
	if (bShore && !bCarried)
	{
		const FVector2D Kept = KeepAshore(Spot, Was, LeadFeetZ, PathPoint);
		if (!Kept.Equals(Spot, 0.01))
		{
			// Off its slot as when held short of a face: once its slot is on the land again it eases back onto it from
			// here, rather than jumping there. The way it was going is out toward the sea, so the ease is not held up.
			StrayWay = (Spot - Kept).GetSafeNormal();
			Stray = Kept - Ground;
			Spot = Kept;
			bHeldShort = true;
		}
	}
	const FVector2D Move = bWasPlaced ? Spot - LastGround : FVector2D::ZeroVector;
	const float Step = DeltaSeconds > 0.f ? static_cast<float>(Move.Size()) / DeltaSeconds : 0.f;
	const bool bWantWalk = Step > kStillSpeed;
	const FVector2D WasGoing = Going;
	Going = DeltaSeconds > 0.f ? Move / DeltaSeconds : FVector2D::ZeroVector;
	LastGround = Spot;
	bPlaced = true;
	if (DeltaSeconds > 0.f)
	{
		// Let go this frame, or held short of a face, the rate it goes at back is how it moved (at its clip's speed, or
		// along the face) less its slot's, for the next frame's ease to go on from; otherwise it is the ease's own (from
		// how it moved every frame, the last step onto its slot fed back into it and threw it off again).
		if (bLetGo || bHeldShort)
		{
			StrayRate = Going - SlotGoing;
		}
		TrackCatchUp(bCarried, bLetGo, WasGoing, DeltaSeconds);
	}

	double Z = Carried.Z;
	if (!bCarried)
	{
		// The ground below, looked for from above the higher of its feet and the lead's to below the lower
		// (-LocoNoFollowerTraversal: around the lead's alone, as before).
		const double Above = bNoTraversal ? LeadFeetZ : FMath::Max(static_cast<double>(LeadFeetZ), Was.Z);
		const double Below = bNoTraversal ? LeadFeetZ : FMath::Min(static_cast<double>(LeadFeetZ), Was.Z);
		Z = LeadFeetZ;
		const UWorld* World = GetWorld();
		FHitResult Hit;
		bool bInSolid = false;
		if (World && World->LineTraceSingleByChannel(Hit, FVector(Spot.X, Spot.Y, Above + 150.0), FVector(Spot.X, Spot.Y, Below - 300.0),
				ECC_Visibility, TraceParams))
		{
			Z = Hit.ImpactPoint.Z;
			// Begun inside something solid (its slot runs through a wall or a rock), the look stops where it began, above
			// its feet: it looks again from a step above them, and inside even there it keeps its height, rather than
			// rising to where the look began, and higher frame on frame (-LocoNoFollowerTraversal: it rises, as before).
			bInSolid = Hit.bStartPenetrating && !bNoTraversal;
			if (bInSolid)
			{
				const bool bFloor = World->LineTraceSingleByChannel(Hit, FVector(Spot.X, Spot.Y, Was.Z + kStepHeight),
					FVector(Spot.X, Spot.Y, Below - 300.0), ECC_Visibility, TraceParams) && !Hit.bStartPenetrating;
				Z = bFloor ? Hit.ImpactPoint.Z : Was.Z;
				if (!bThroughSolid)
				{
					UE_LOG(LogForgottenThird, Display, TEXT("%s: its slot runs into something solid at (%.0f, %.0f): it keeps %s."), *LogName,
						Spot.X, Spot.Y, bFloor ? TEXT("to the floor a step above its feet or below") : TEXT("its height"));
				}
			}
		}
		bThroughSolid = bInSolid;
		// Off a ledge more than a step down it falls (-LocoNoFollowerTraversal: it drops onto the floor at once).
		if (!bAir && !bNoTraversal && bWasPlaced && DeltaSeconds > 0.f && World && Z < Was.Z - kStepHeight)
		{
			bAir = true;
			AirSpeed = 0.f;
			AirGravity = static_cast<float>(-World->GetGravityZ());
		}
		// In the air (a replayed jump or a fall) it goes up and comes down under gravity from where it was, back onto
		// the ground below.
		if (bAir)
		{
			const double AirZ = Was.Z + (AirSpeed - 0.5f * AirGravity * DeltaSeconds) * DeltaSeconds;
			AirSpeed -= AirGravity * DeltaSeconds;
			if (AirSpeed < 0.f && AirZ <= Z)
			{
				bAir = false;
				AirSpeed = 0.f;
			}
			else
			{
				Z = FMath::Max(AirZ, Z);
			}
		}
	}
	FRotator Rot = GetActorRotation();
	const ftcore::Locomotion* Loco = Anim ? &Anim->GetLocomotion() : nullptr;
	if (bCarried)
	{
		// A traversal keeps the facing it began with; a catch turns the body square into the face, as it turns the lead.
		const float Gap = FMath::FindDeltaAngleDegrees(static_cast<float>(Rot.Yaw), static_cast<float>(TravFrame.Rotator().Yaw));
		Rot.Yaw += Gap * (1.f - FMath::Exp(-DeltaSeconds / kCatchTurnTime));
	}
	else if (Loco && Loco->turning() && Loco->turnPivots())
	{
		// A replayed pivot (running, or on the spot) turns the body by its clip's root, as it turned the lead. A
		// shallower one rode the lead's own motion round, so the body eases along the trail through it as the lead did.
		if (Loco->turnsBegun() != TurnsSeen)
		{
			TurnsSeen = Loco->turnsBegun();
			TurnEntryYaw = static_cast<float>(Rot.Yaw) - Loco->turnYaw();
		}
		Rot.Yaw = TurnEntryYaw + Loco->turnYaw();
	}
	else if (bWantWalk)
	{
		// Ease toward the trail rather than snapping to each frame's step, so a weaving lead doesn't jerk it round,
		// and no faster than a body turns as it walks (setting off well away from its facing, after a stand turn).
		const float Want = FMath::RadiansToDegrees(FMath::Atan2(Move.Y, Move.X));
		const float Ease = DeltaSeconds > 0.f ? 1.f - FMath::Exp(-DeltaSeconds / kFaceTime) : 1.f;
		const float MaxStep = kMaxFaceRate * DeltaSeconds;
		Rot.Yaw += FMath::Clamp(FMath::FindDeltaAngleDegrees(static_cast<float>(Rot.Yaw), Want) * Ease, -MaxStep, MaxStep);
	}
	SetActorLocationAndRotation(FVector(Spot.X, Spot.Y, Z), Rot, false, nullptr, ETeleportType::TeleportPhysics);
	// A change of height no walking body makes in one frame: a pop (as -LocoNoFollowerTraversal's onto and off what the
	// lead went over).
	if (bWasPlaced && DeltaSeconds > 0.f && FMath::Abs(Z - Was.Z) > kStepHeight)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s: Z %s %.0f cm in one frame%s."), *LogName, Z > Was.Z ? TEXT("rose") : TEXT("dropped"),
			FMath::Abs(Z - Was.Z), bCarried ? TEXT(", carried by a traversal") : TEXT(""));
	}
	// Facing where it was asked to, or setting off, it no longer turns on the spot.
	if (bFacing && (bWantWalk || FMath::Abs(FMath::FindDeltaAngleDegrees(static_cast<float>(Rot.Yaw), FaceYaw)) < kFacedWithin))
	{
		bFacing = false;
	}

	// Carried, its speed is its path's (TickTraversal).
	if (!bCarried && DeltaSeconds > 0.f)
	{
		Speed += ((bWantWalk ? Step : 0.f) - Speed) * (1.f - FMath::Exp(-DeltaSeconds * kSpeedResponse));
	}
	if (Anim)
	{
		Anim->SetGroundSpeed(Speed);
		Anim->SetIntent(WantSpeed, StopDistance);
		Anim->SetAir(bAir, AirSpeed);
		Anim->SetFace(bFacing, FaceYaw);
		// It crouches when its lead does.
		Anim->SetCrouch(LeadCharacter.IsValid() && LeadCharacter->bIsCrouched);
		// The lead's foot phase while it holds a gait, so the two don't step together, and its stride, so a
		// follower with a longer one steps faster to keep pace.
		float LeadPhase = -1.f, LeadStride = 0.f;
		const USkeletalMeshComponent* LeadMesh = LeadCharacter.IsValid() ? LeadCharacter->GetMesh() : nullptr;
		if (const UFTLocomotionAnimInstance* LeadAnim = LeadMesh ? Cast<UFTLocomotionAnimInstance>(LeadMesh->GetAnimInstance()) : nullptr)
		{
			const ftcore::Locomotion& LeadLoco = LeadAnim->GetLocomotion();
			if (LeadLoco.state() == ftcore::LocoState::Ground && LeadCharacter->GetVelocity().Size2D() >= LeadLoco.tuning().moveStop)
			{
				LeadPhase = LeadAnim->LeadPhaseAtFrameStart();
				LeadStride = LeadAnim->LeadStrideAtFrameStart();
			}
		}
		Anim->SetLead(LeadPhase, LeadStride);
	}
}

void AFTPartyFollower::TakeMarks(const FVector& Feet)
{
	if (const AFTExploreCharacter* Lead = LeadExplorer.Get(); Lead && Lead->GetTraversalsBegun() != MarksSeen)
	{
		MarksSeen = Lead->GetTraversalsBegun();
		// The oldest goes to make room.
		if (NumMarks == MaxMarks)
		{
			for (int32 I = 1; I < NumMarks; ++I)
			{
				Marks[I - 1] = Marks[I];
			}
			--NumMarks;
		}
		Marks[NumMarks++] = Lead->GetLastTraversal();
	}
	// Those it is past, or far from, it forgets.
	int32 Kept = 0;
	for (int32 I = 0; I < NumMarks; ++I)
	{
		const FFTTraversalMark& Mark = Marks[I];
		const bool bPast = FVector::DotProduct(Feet - Mark.Face, Mark.Way) > kMarkPassed;
		const bool bFar = FVector::Dist2D(Feet, Mark.Face) > kMarkFar;
		if (!bPast && !bFar)
		{
			Marks[Kept++] = Mark;
			continue;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("%s: left the lead's %s at (%.0f, %.0f) unfollowed (%s)."), *LogName,
			Mark.bCatch ? TEXT("catch") : TEXT("vault or climb"), Mark.Face.X, Mark.Face.Y, bFar ? TEXT("too far from it") : kMissNames[LastMiss]);
		LastMiss = 0;
	}
	NumMarks = Kept;
}

bool AFTPartyFollower::TryTraverse(UFTLocomotionAnimInstance& Anim, const FVector& Feet, float WantSpeed)
{
	// Only toward one of the lead's ahead within the probes' reach, going its way: on the ground a vault or a climb, in
	// the air a catch.
	const FVector Fwd = GetActorForwardVector().GetSafeNormal2D();
	bool bAhead = false;
	for (int32 I = 0; I < NumMarks && !bAhead; ++I)
	{
		const double Ahead = FVector::DotProduct(Marks[I].Face - Feet, Marks[I].Way);
		bAhead = Marks[I].bCatch == bAir && Ahead > 0.0 && Ahead <= kMarkReach && FVector::DotProduct(Fwd, Marks[I].Way) > kMarkFacing;
	}
	const UWorld* World = GetWorld();
	if (!bAhead || !World)
	{
		return false;
	}
	// What is ahead, found as the lead finds it (with the lead's capsule for the room on a deep top), at the lead's face.
	const ftcore::TraversalSet& Set = Anim.GetTraversal();
	ftcore::TraversalObstacle Obstacle;
	FVector In;
	if (!FTTraversalProbe::Probe(*World, TraceParams, Feet, Fwd, AFTExploreCharacter::CapsuleRadius, AFTExploreCharacter::CapsuleHalfHeight,
			Set.tuning(), bAir, Obstacle, In))
	{
		LastMiss = 1;
		return false;
	}
	const FVector Face = Feet + In * Obstacle.distance;
	int32 Which = INDEX_NONE;
	for (int32 I = 0; I < NumMarks && Which == INDEX_NONE; ++I)
	{
		if (Marks[I].bCatch == bAir && FMath::Abs(FVector::DotProduct(Face - Marks[I].Face, Marks[I].Way)) <= kMarkSlack)
		{
			Which = I;
		}
	}
	if (Which == INDEX_NONE)
	{
		LastMiss = 2;
		return false;
	}
	// In the air, its flight in the face's space as the lead's (AFTExploreCharacter::TryCatch): Core's x into the face, y
	// its left, z up; it goes on at its slot's pace.
	const ftcore::TravPoint Velocity{static_cast<float>(Going.X * In.X + Going.Y * In.Y), static_cast<float>(Going.X * In.Y - Going.Y * In.X), AirSpeed};
	const ftcore::TraversalPlan Plan = bAir ? Set.planCatch(Obstacle, Velocity, AirGravity, WantSpeed >= 0.f ? WantSpeed : Speed)
		: Set.plan(Obstacle, Speed, -1);
	if (!Plan.ok())
	{
		LastMiss = Plan.tooFar ? 3 : 4;
		// Not yet, below the top: it doesn't fly on through the face.
		if (bAir && Obstacle.height > 0.f)
		{
			bAtWall = true;
			WallFace = Face;
			WallIn = In;
		}
		return false;
	}
	const float Off = static_cast<float>(FVector::DotProduct(Face - Marks[Which].Face, Marks[Which].Way));
	for (int32 I = Which + 1; I < NumMarks; ++I)
	{
		Marks[I - 1] = Marks[I];
	}
	--NumMarks;
	LastMiss = 0;
	bTraversing = true;
	TravPlan = Plan;
	TravTime = Plan.start;
	// A catch turns the body square into the face (In), and carries it from its flight: it is in the air no more.
	TravFrame = FTransform(FRotator(0.0, bAir ? In.Rotation().Yaw : GetActorRotation().Yaw, 0.0), Feet);
	Anim.SetTraversal(Plan.clip, TravTime);
	const ftcore::TraversalClipInfo& Clip = Set.clip(static_cast<std::size_t>(Plan.clip));
	if (bAir)
	{
		bAir = false;
		AirSpeed = 0.f;
		const float Lead = Clip.contact - Plan.start;
		const float Flight = Velocity.z * Lead - 0.5f * AirGravity * Lead * Lead;
		UE_LOG(LogForgottenThird, Display,
			TEXT("%s: catch clip %d (edge %.0f up) from %.2f s, %.2f s before the catch, for a %.0f cm top %.0f cm ahead, %.0f deep, at (%.0f, %.0f, %.0f) cm/s; the catch at (%.0f, %.0f), its flight's miss (%.0f, %.0f), %+.0f cm off the lead's face."),
			*LogName, Plan.clip, Clip.ledgeHeight, Plan.start, Lead, Obstacle.height, Obstacle.distance, Obstacle.depth, Velocity.x, Velocity.y,
			Velocity.z, Plan.atContact.x, Plan.atContact.z, Plan.atContact.x - Velocity.x * Lead, Plan.atContact.z - Flight, Off);
		return true;
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("%s: traversal clip %d (%s, edge %.0f up) from %.2f s for a %.0f cm top %.0f cm ahead, %.0f deep (floor beyond %d at %.0f), at %.0f cm/s, %+.0f cm off the lead's face."),
		*LogName, Plan.clip, Clip.move == ftcore::TraversalMove::Up ? TEXT("up") : TEXT("over"), Clip.ledgeHeight, Plan.start, Obstacle.height,
		Obstacle.distance, Obstacle.depth, Obstacle.backFloor ? 1 : 0, Obstacle.backHeight, Speed, Off);
	return true;
}

bool AFTPartyFollower::TickTraversal(UFTLocomotionAnimInstance& Anim, float DeltaSeconds, const FVector2D& Ground, bool bSlotMoving, FVector& Feet)
{
	const ftcore::TraversalSet& Set = Anim.GetTraversal();
	const ftcore::TraversalClipInfo& Clip = Set.clip(static_cast<std::size_t>(TravPlan.clip));
	TravTime += DeltaSeconds * Set.rate(TravPlan, TravTime);
	const float Length = Clip.length;
	const float T = FMath::Min(TravTime, Length);
	// The plan's root in the body's space as it began: x forward, y left (UE's Y is right), z up.
	auto FeetAt = [this, &Set](float At)
	{
		const ftcore::TravPoint R = Set.rootAt(TravPlan, At);
		return TravFrame.TransformPosition(FVector(R.x, -R.y, R.z));
	};
	Feet = FeetAt(T);
	// Its speed is its path's, for the locomotion now and the walk after.
	const float Back = FMath::Max(DeltaSeconds, 1.f / 60.f);
	Speed = static_cast<float>(FVector::Dist2D(Feet, FeetAt(FMath::Max(TravPlan.start, T - Back)))) / Back;
	// While its slot moves on it hands back to its locomotion just after the landing, as a lead that wants to move does;
	// one whose slot stands plays the clip out.
	if (T < Length && !(bSlotMoving && T >= Set.exitTime(TravPlan)))
	{
		Anim.SetTraversal(TravPlan.clip, T);
		return true;
	}
	bTraversing = false;
	Anim.SetTraversal(-1, 0.f);
	// From where its clip lets it go, it eases back onto its slot.
	Stray = FVector2D(Feet.X, Feet.Y) - Ground;
	const FVector Way = TravFrame.GetUnitAxis(EAxis::X);
	StrayWay = FVector2D(Way.X, Way.Y).GetSafeNormal();
	UE_LOG(LogForgottenThird, Display, TEXT("%s: traversal done at %.2f of %.2f s (%s), %.0f cm/s, %.0f cm from the start, %+.0f cm up, %.0f cm off its slot."),
		*LogName, T, Length, T >= Length ? TEXT("clip end") : TEXT("exit"), Speed, FVector::Dist2D(Feet, TravFrame.GetLocation()),
		Feet.Z - TravFrame.GetLocation().Z, Stray.Size());
	return false;
}

FVector2D AFTPartyFollower::EaseStray(const FVector2D& Ground, float DeltaSeconds)
{
	// Back toward its slot as it goes on, no faster than a walk back, and ahead of it the way the traversal went, waiting
	// for it rather than walking back through what it went over (ftcore::easeStray, in metres). Its rate back changes no
	// faster than a person speeds up or brakes (-LocoNoCatchUpRamp: no limit, as before).
	ftcore::PartyStrayTuning Tuning;
	if (bNoCatchUpRamp)
	{
		Tuning.accel = 0.f;
	}
	const FVector2D From = LastGround - Ground;
	ftcore::Vec2 Off{static_cast<float>(Stray.X / 100.0), static_cast<float>(Stray.Y / 100.0)};
	ftcore::Vec2 Rate{static_cast<float>(StrayRate.X / 100.0), static_cast<float>(StrayRate.Y / 100.0)};
	ftcore::easeStray(Off, Rate, {static_cast<float>(From.X / 100.0), static_cast<float>(From.Y / 100.0)},
		{static_cast<float>(StrayWay.X), static_cast<float>(StrayWay.Y)}, DeltaSeconds, Tuning);
	Stray = FVector2D(Off.x, Off.y) * 100.0;
	StrayRate = FVector2D(Rate.x, Rate.y) * 100.0;
	return Stray;
}

void AFTPartyFollower::TrackCatchUp(bool bCarried, bool bLetGo, const FVector2D& WasGoing, float DeltaSeconds)
{
	if (StrayClock >= 0.f)
	{
		StrayClock += DeltaSeconds;
	}
	if (bCarried)
	{
		return;
	}
	if (bLetGo)
	{
		if (StrayClock >= 0.f)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("%s: let go again %.2f s after it came off its slot (its rate back %.0f cm/s then), before it was back on it: %.0f cm/s at most, its speed changing at %.0f cm/s^2 at most."),
				*LogName, StrayClock, StrayRateFrom, StrayTop, StrayTopAccel);
		}
		StrayClock = 0.f;
		StrayTop = StrayTopAccel = 0.f;
		StrayRateFrom = static_cast<float>(StrayRate.Size());
		return;
	}
	if (StrayClock < 0.f)
	{
		// Held short of a face the lead caught, it comes off its slot with no let-go.
		if (Stray.IsZero())
		{
			return;
		}
		StrayClock = 0.f;
		StrayTop = StrayTopAccel = 0.f;
		StrayRateFrom = static_cast<float>(StrayRate.Size());
	}
	StrayTop = FMath::Max(StrayTop, static_cast<float>(Going.Size()));
	StrayTopAccel = FMath::Max(StrayTopAccel, static_cast<float>((Going - WasGoing).Size()) / DeltaSeconds);
	if (Stray.IsZero())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s: back on its slot %.2f s after it came off it (its rate back %.0f cm/s then): %.0f cm/s at most, its speed changing at %.0f cm/s^2 at most."),
			*LogName, StrayClock, StrayRateFrom, StrayTop, StrayTopAccel);
		StrayClock = -1.f;
	}
}

void AFTPartyFollower::ReplayJump(float UpSpeed, float Gravity)
{
	if (bAir || bTraversing || UpSpeed <= 0.f || Gravity <= 0.f)
	{
		return;
	}
	bAir = true;
	AirSpeed = UpSpeed;
	AirGravity = Gravity;
}

void AFTPartyFollower::FaceTo(float Yaw)
{
	bFacing = true;
	FaceYaw = Yaw;
}

void AFTPartyFollower::SetHeld(bool bHeld)
{
	bHeldByScene = bHeld;
	const FVector At = GetActorLocation();
	if (!bHeld)
	{
		// Whatever moved it meanwhile, its next move counts from here, and what the lead went over or onto meanwhile was
		// the scene's to show.
		LastGround = LastSlot = FVector2D(At.X, At.Y);
		MarksSeen = LeadExplorer.IsValid() ? LeadExplorer->GetTraversalsBegun() : MarksSeen;
		return;
	}
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance());
	if (bTraversing && Anim)
	{
		Anim->SetTraversal(-1, 0.f);
	}
	bTraversing = false;
	NumMarks = 0;
	LastMiss = 0;
	Stray = StrayRate = FVector2D::ZeroVector;
	StrayClock = -1.f;
	bAir = false;
	AirSpeed = 0.f;
	bFacing = false;
	Speed = 0.f;
	if (bPlaced && (WaterPhase == ftcore::WaterFollowPhase::Ashore || (WaterPhase == ftcore::WaterFollowPhase::Placed && !bPlacedInWater)))
	{
		// Down on the ground below it, and the anim told it stands (not in the water: no snap to the bed or the bank; a
		// member placed at its surface slot stays there).
		Place(FVector2D(At.X, At.Y), static_cast<float>(At.Z), 0.f);
	}
}

void AFTPartyFollower::BeginInWater()
{
	const FVector At = GetActorLocation();
	bPlaced = true;
	LastGround = LastSlot = FVector2D(At.X, At.Y);
}

void AFTPartyFollower::SetSea(float SeaLevel)
{
	bShore = !FParse::Param(FCommandLine::Get(), TEXT("LocoNoShore"));
	ShoreLevel = SeaLevel;
	// No steeper than the lead walks (Setup gave it the lead).
	const ACharacter* Lead = LeadCharacter.Get();
	const UCharacterMovementComponent* LeadMove = Lead ? Lead->GetCharacterMovement() : nullptr;
	ShoreWalkableZ = LeadMove ? LeadMove->GetWalkableFloorZ() : kShoreWalkableZ;
}

FVector2D AFTPartyFollower::KeepAshore(const FVector2D& Spot, const FVector& Was, float LeadFeetZ, const FVector2D* PathPoint)
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return Spot;
	}
	// The ground below a point, looked for as Place looks for it: from above the higher of its feet and the lead's to
	// below the lower. Footing is ground found clear of the sea and no steeper than the lead walks (not the bank from a
	// quay's edge down to the bed, beyond the hidden wall). A look begun inside something solid is the spot's own case
	// at the spot (Place keeps its height there) and no footing on the way out (it stands short of that too).
	const double Above = FMath::Max(static_cast<double>(LeadFeetZ), Was.Z) + 150.0;
	const double Below = FMath::Min(static_cast<double>(LeadFeetZ), Was.Z) - 300.0;
	// Ground under a canal's or a pool's surface is no footing either (the map's water bodies): it stays on the bank.
	const UFTWaterSubsystem* const WaterSystem = World->GetSubsystem<UFTWaterSubsystem>();
	const ftcore::WaterBodies* const Bodies = WaterSystem && WaterSystem->HasBodies() ? &WaterSystem->Bodies() : nullptr;
	auto Footing = [this, World, Above, Below, Bodies](const FVector2D& At, bool bSolidIsFooting)
	{
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, FVector(At.X, At.Y, Above), FVector(At.X, At.Y, Below), ECC_Visibility, TraceParams))
		{
			return false;
		}
		if (Hit.bStartPenetrating)
		{
			return bSolidIsFooting;
		}
		const ftcore::Vec2 Plan{static_cast<float>(At.X * 0.01), static_cast<float>(At.Y * 0.01)};
		if (Bodies && Bodies->overWater(Plan, static_cast<float>(Hit.ImpactPoint.Z * 0.01), static_cast<float>(kShoreMargin * 0.01)))
		{
			return false;
		}
		// Nor is a flight of steps down a canal's wall, dry treads and all: it waits at the bank's top while the lead
		// goes down to the water.
		if (Bodies && Bodies->onSteps(Plan, static_cast<float>(Hit.ImpactPoint.Z * 0.01)))
		{
			return false;
		}
		return ftcore::shoreFooting(true, Hit.ImpactPoint.Z, Hit.ImpactNormal.Z, ShoreLevel, kShoreMargin, ShoreWalkableZ);
	};
	// The bank beside a spot that is in a canal: the point level with it on the line along the wall through the top of
	// the nearest way out (a ladder's or a flight's), or that top itself where the line has no footing there.
	auto BankBeside = [&Footing, Bodies, &Spot](FVector2D& Beside)
	{
		const int32 Exit = Bodies ? Bodies->nearestExit(ftcore::Vec2{static_cast<float>(Spot.X * 0.01), static_cast<float>(Spot.Y * 0.01)}, 40.f) : -1;
		if (Exit < 0)
		{
			return false;
		}
		const ftcore::WaterExit& Out = Bodies->exit(static_cast<std::size_t>(Exit));
		const FVector2D Top(Out.top.x * 100.0, Out.top.y * 100.0);
		const FVector2D Along(-Out.out.y, Out.out.x);
		const FVector2D Level = Top + Along * FVector2D::DotProduct(Spot - Top, Along);
		Beside = Footing(Level, false) ? Level : Top;
		return true;
	};
	if (Footing(Spot, true))
	{
		if (bShoreHeld)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("%s: its spot is on the land again at (%.0f, %.0f)."), *LogName, Spot.X, Spot.Y);
		}
		bShoreHeld = false;
		LastDry = Spot;
		bHasLastDry = true;
		return Spot;
	}
	// Out from where the lead walked at its place on the trail; with none given, from where it last had footing.
	const FVector2D From = PathPoint ? *PathPoint : bHasLastDry ? LastDry : FVector2D(Was.X, Was.Y);
	const ftcore::ShoreStand Stand = ftcore::standShortOfSea(ftcore::Vec2{static_cast<float>(From.X * 0.01), static_cast<float>(From.Y * 0.01)},
		ftcore::Vec2{static_cast<float>(Spot.X * 0.01), static_cast<float>(Spot.Y * 0.01)},
		[&Footing](ftcore::Vec2 P) { return Footing(FVector2D(P.x * 100.0, P.y * 100.0), false); });
	FVector2D At = Spot;
	const TCHAR* How = TEXT("stands short of it");
	constexpr double kBankReach = 4000.0; // cm: further than this from its last footing, the lead was set down elsewhere
	if (Stand.dry)
	{
		At = FVector2D(Stand.at.x * 100.0, Stand.at.y * 100.0);
		LastDry = At;
		bHasLastDry = true;
	}
	else if (bHasLastDry && Bodies)
	{
		// The lead itself is in the water (a canal, a pool, the sea): it keeps beside the lead along the bank. Of the two
		// ways from where it last had footing that hold one of that point's coordinates, it takes the one that ends
		// nearer its spot, as far as the footing goes and no more than a stride a call; with neither nearer, or the lead
		// far off (set down somewhere else), it waits.
		constexpr double kBankStride = 25.0;  // cm a call
		FVector2D Best = LastDry;
		double BestGap = FVector2D::Distance(LastDry, Spot);
		const bool bNear = BestGap <= kBankReach;
		for (const FVector2D& Target : {FVector2D(Spot.X, LastDry.Y), FVector2D(LastDry.X, Spot.Y)})
		{
			const FVector2D Way = Target - LastDry;
			if (!bNear || Way.IsNearlyZero(1.0))
			{
				continue;
			}
			const FVector2D Along = LastDry + Way.GetSafeNormal() * FMath::Min(Way.Size(), kBankStride);
			const ftcore::ShoreStand Bank = ftcore::standShortOfSea(ftcore::Vec2{static_cast<float>(LastDry.X * 0.01), static_cast<float>(LastDry.Y * 0.01)},
				ftcore::Vec2{static_cast<float>(Along.X * 0.01), static_cast<float>(Along.Y * 0.01)},
				[&Footing](ftcore::Vec2 P) { return Footing(FVector2D(P.x * 100.0, P.y * 100.0), false); });
			const FVector2D To(Bank.at.x * 100.0, Bank.at.y * 100.0);
			const double Gap = FVector2D::Distance(To, Spot);
			if (Bank.dry && Gap < BestGap - 1.0)
			{
				Best = To;
				BestGap = Gap;
			}
		}
		At = Best;
		LastDry = Best;
		How = TEXT("keeps to the bank beside the lead");
		// The lead was set down in a canal far from where this one last stood: the bank beside it.
		if (FVector2D Beside; !bNear && BankBeside(Beside))
		{
			At = Beside;
			LastDry = At;
			How = TEXT("stands on the bank beside the lead, by the nearest way out");
		}
	}
	else if (bHasLastDry && FVector2D::Distance(LastDry, Spot) <= kShoreLeash)
	{
		// Nowhere dry on the way (the lead itself is off the land): it waits where it last stood on it.
		At = LastDry;
		How = TEXT("waits where it last had footing");
	}
	else if (FVector2D Beside; BankBeside(Beside))
	{
		// It has stood nowhere yet and the lead was set down in a canal: the bank beside it.
		At = Beside;
		LastDry = At;
		bHasLastDry = true;
		How = TEXT("stands on the bank beside the lead, by the nearest way out");
	}
	else
	{
		How = TEXT("has no footing near: it goes on as before");
	}
	if (!bShoreHeld)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s: its spot at (%.0f, %.0f) has no footing (over the sea at %.0f cm, or a drop): it %s at (%.0f, %.0f)."),
			*LogName, Spot.X, Spot.Y, ShoreLevel, How, At.X, At.Y);
	}
	bShoreHeld = true;
	return At;
}

void AFTPartyFollower::ReplayTurn(float Delta)
{
	if (UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance()))
	{
		Anim->ReplayTurn(Delta);
	}
}

// ---- In the water (ftcore::WaterFollowParty's orders, the lead's GetWaterOrder) ----

namespace
{
// The walk to an entry and down a flight of steps (cm/s), the jump in (s, and its rise over the line, cm); the climb
// out's times and speeds are the water follow's tuning (ftcore::WaterFollowTuning, the lead's).
constexpr float kWaterWalkSpeed = 220.f;
constexpr float kWaterStepsSpeed = 120.f;
constexpr float kWaterJumpSeconds = 0.75f;
constexpr float kWaterJumpRise = 60.f;
// Near enough (cm) to an entry's point to count as there (an exit's waiting place: the tuning's atExit).
constexpr double kWaterAtEntry = 30.0;
// Under water its velocity follows the order's with this time constant (s); at the surface its height eases to the
// swim's with this one (a breach, a dive's start).
constexpr float kWaterUnderEase = 0.5f;
constexpr float kWaterRiseEase = 0.35f;
// The body's height (m) for the water's state.
constexpr float kWaterBodyHeight = 1.8f;

}

void AFTPartyFollower::FillWaterMember(ftcore::WaterFollowMember& Member) const
{
	using namespace FTSwimUnits;
	Member = ftcore::WaterFollowMember{};
	// The party hidden (P or LB) clears its slot; the director's lens hide (between the camera and the lead) is visual
	// only, so a swimmer keeps its phase and place through it.
	const AFTExploreCharacter* WaterLead = LeadExplorer.Get();
	const bool bPartyHidden = WaterLead && WaterLead->IsPartyHidden();
	Member.active = bPlaced && !bPartyHidden;
	Member.lensHidden = IsHidden() && !bPartyHidden;
	const FVector At = GetActorLocation();
	Member.position = ToSwim(At + FVector(0.0, 0.0, ToCm(ftcore::kWaterSwimDraft)));
	Member.held = bHeldByScene;
#if !UE_BUILD_SHIPPING
	Member.held = Member.held || bProofAir; // -SwimProof=boom: kept over the water by the run, as a scene would hold it
#endif
	Member.traversing = bTraversing;
	Member.atEntry = WaterPhase == ftcore::WaterFollowPhase::ToEntry && bWaterAtEntry;
	Member.entered = WaterPhase == ftcore::WaterFollowPhase::Entering && bWaterMoveDone;
	Member.atExit = WaterPhase == ftcore::WaterFollowPhase::ToExit && bWaterAtExit;
	Member.climbed = WaterPhase == ftcore::WaterFollowPhase::Climbing && bWaterMoveDone;
	Member.surfaceZ = WaterNow.body >= 0 ? WaterNow.surface : ToMetres(At.Z);
	Member.bedZ = WaterBedZ;
	Member.edge = WaterEdge;
	Member.edgeOut = WaterEdgeOut;
	Member.hasEdge = bWaterHasEdge;
	// Its surface step turned or held this frame or the last (the party may step before or after it does).
	Member.blocked = IsWaterHeld();
	Member.turned = IsWaterTurned();
}

bool AFTPartyFollower::FindWaterEdge(const ftcore::WaterBodies& Bodies, const FVector& At, const FVector& LeadAt)
{
	// The bank's edge on the way from where it stands to the lead: the last point short of the lead's own water (other
	// water on the way, a canal, is passed over), marched in 25 cm steps up to 20 m; the way in there is toward the lead.
	const int LeadBody = Bodies.bodyAt(FTSwimUnits::ToPlan(LeadAt));
	const FVector2D From(At.X, At.Y);
	FVector2D Way = FVector2D(LeadAt.X, LeadAt.Y) - From;
	const double Far = Way.Size();
	Way = Far > 1.0 ? Way / Far : FVector2D(1.0, 0.0);
	WaterEdge = {FTSwimUnits::ToMetres(From.X), FTSwimUnits::ToMetres(From.Y)};
	WaterEdgeOut = {static_cast<float>(Way.X), static_cast<float>(Way.Y)};
	for (double Along = 0.0; Along <= FMath::Min(Far, 2000.0); Along += 25.0)
	{
		const FVector2D P = From + Way * Along;
		const int Here = Bodies.bodyAt(ftcore::Vec2{FTSwimUnits::ToMetres(P.X), FTSwimUnits::ToMetres(P.Y)});
		if (LeadBody >= 0 ? Here == LeadBody : Here >= 0)
		{
			const FVector2D Edge = From + Way * FMath::Max(0.0, Along - 30.0);
			WaterEdge = {FTSwimUnits::ToMetres(Edge.X), FTSwimUnits::ToMetres(Edge.Y)};
			return true;
		}
	}
	return false; // no water toward the lead: it stays ashore (no jump onto land)
}

ftcore::WaterState AFTPartyFollower::SampleSwimAt(const UFTWaterSubsystem& System, const FVector2D& P, float Sink) const
{
	using namespace FTSwimUnits;
	const ftcore::Vec2 Plan{ToMetres(P.X), ToMetres(P.Y)};
	float Surface = 0.f;
	const UWorld* World = GetWorld();
	if (!World || !System.Bodies().surfaceHeight(Plan, System.WaterTime(), Surface))
	{
		return ftcore::WaterState{};
	}
	FHitResult Bed;
	const double Top = ToCm(Surface) + 60.0;
	const bool bBed = World->LineTraceSingleByChannel(Bed, FVector(P.X, P.Y, Top), FVector(P.X, P.Y, Top - 3060.0), ECC_Visibility, TraceParams);
	if (bBed && Bed.bStartPenetrating)
	{
		return ftcore::WaterState{}; // inside a solid
	}
	const float BedZ = bBed ? ToMetres(Bed.ImpactPoint.Z) : Surface - 30.f;
	ftcore::WaterCapsule Capsule;
	Capsule.p = Plan;
	Capsule.feetZ = ftcore::waterSurfaceFeetZ(ftcore::waterSwimFeetZ(Surface, System.WaterTime()), Sink, BedZ);
	Capsule.height = kWaterBodyHeight;
	Capsule.swimming = true;
	Capsule.groundZ = BedZ;
	Capsule.hasGround = true;
	return ftcore::waterState(System.Bodies(), Capsule, System.WaterTime());
}

bool AFTPartyFollower::IntoWaterToward(const UFTWaterSubsystem& System, const FVector2D& P, const FVector2D& To, double MaxCm, FVector2D& Out) const
{
	const FVector2D Way = To - P;
	const double Far = FMath::Min(Way.Size(), MaxCm);
	if (Far <= 0.0)
	{
		return false;
	}
	const FVector2D Dir = Way.GetSafeNormal();
	for (double Along = 0.0; Along <= Far; Along += 50.0)
	{
		const FVector2D Q = P + Dir * Along;
		if (ftcore::waterIn(SampleSwimAt(System, Q, 0.f)))
		{
			Out = Q;
			return true;
		}
	}
	return false;
}

bool AFTPartyFollower::HoldForProof(const FVector2D& At)
{
#if UE_BUILD_SHIPPING
	(void)At;
	return false;
#else
	using namespace FTSwimUnits;
	UWorld* World = GetWorld();
	const UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	float Surface = 0.f;
	if (!System || WaterPhase != ftcore::WaterFollowPhase::Surface
		|| !System->Bodies().surfaceHeight(ftcore::Vec2{ToMetres(At.X), ToMetres(At.Y)}, System->WaterTime(), Surface))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("SwimProof: %s not held at (%.0f, %.0f): phase %d%s."), *LogName, At.X, At.Y, static_cast<int32>(WaterPhase),
			System && WaterPhase == ftcore::WaterFollowPhase::Surface ? TEXT(", no water there") : TEXT(" (not at the surface)"));
		return false;
	}
	// At the surface swim's own height there (the one rule), the bed looked at first.
	const float Draft = ftcore::waterSwimFeetZ(Surface, System->WaterTime());
	SenseWater(*System, FVector(At.X, At.Y, ToCm(Draft)), true);
	const FVector P(At.X, At.Y, ToCm(ftcore::waterSurfaceFeetZ(Draft, 0.f, WaterBedZ)));
	SetActorLocation(P, false, nullptr, ETeleportType::TeleportPhysics);
	WaterVelocity = FVector::ZeroVector;
	WaterRideZ = ToCm(Draft);
	WaterRideFrame = GFrameCounter;
	bWaterOnRide = ftcore::waterFeetOnRide(Draft, 0.f, WaterBedZ);
	bProofHeld = true;
	UE_LOG(LogForgottenThird, Display, TEXT("SwimProof: %s held for the sweep at (%.0f, %.0f, %.0f) cm."), *LogName, P.X, P.Y, P.Z);
	return true;
#endif
}

bool AFTPartyFollower::HoldForProofAt(const FVector& Centre)
{
#if UE_BUILD_SHIPPING
	(void)Centre;
	return false;
#else
	if (!bProofAir && WaterPhase != ftcore::WaterFollowPhase::Surface)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("SwimProof: %s not held on the boom line: phase %d (not at the surface)."), *LogName, static_cast<int32>(WaterPhase));
		return false;
	}
	// The middle of its bones' box (the camera's measure is to the bones) put at Centre: the actor moves by the gap.
	FBox Bones(ForceInit);
	const FTransform ToWorld = Body->GetComponentTransform();
	for (const FTransform& Bone : Body->GetComponentSpaceTransforms())
	{
		Bones += ToWorld.TransformPosition(Bone.GetLocation());
	}
	const FVector Mid = Bones.IsValid ? Bones.GetCenter() : GetActorLocation();
	SetActorLocation(GetActorLocation() + (Centre - Mid), false, nullptr, ETeleportType::TeleportPhysics);
	WaterVelocity = FVector::ZeroVector;
	if (!bProofAir)
	{
		const FVector Feet = GetActorLocation();
		UE_LOG(LogForgottenThird, Display, TEXT("SwimProof: %s held on the boom line: its middle at (%.0f, %.0f, %.0f) cm, its feet at (%.0f, %.0f, %.0f) cm."), *LogName,
			Centre.X, Centre.Y, Centre.Z, Feet.X, Feet.Y, Feet.Z);
	}
	bProofAir = true;
	return true;
#endif
}

void AFTPartyFollower::SenseWater(const UFTWaterSubsystem& System, const FVector& At, bool bSwimming)
{
	using namespace FTSwimUnits;
	ftcore::WaterCapsule Capsule;
	Capsule.p = ToPlan(At);
	Capsule.feetZ = ToMetres(At.Z);
	Capsule.height = kWaterBodyHeight;
	Capsule.swimming = bSwimming;
	FHitResult Bed;
	const bool bBed = GetWorld()->LineTraceSingleByChannel(Bed, At + FVector(0.0, 0.0, 100.0), At - FVector(0.0, 0.0, 600.0), ECC_Visibility, TraceParams);
	WaterBedZ = bBed ? ToMetres(Bed.ImpactPoint.Z) : Capsule.feetZ - 6.f;
	Capsule.groundZ = WaterBedZ;
	Capsule.hasGround = true;
	WaterNow = ftcore::waterState(System.Bodies(), Capsule, System.WaterTime());
}

void AFTPartyFollower::StepWet(float Weather, float DeltaSeconds)
{
	// In the water only while its order has it there (on land WaterNow may still hold the last swim's state).
	// The one in-water test (ftcore::waterIn), the same the surface step keeps it to.
	const bool bIn = WaterPhase != ftcore::WaterFollowPhase::Ashore && ftcore::waterIn(WaterNow);
	const UWorld* World = GetWorld();
	if (const UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr; System && System->HasBodies())
	{
		const ftcore::WaterBodies& Bodies = System->Bodies();
		const ftcore::WaterKind Kind = WaterNow.body >= 0 && static_cast<std::size_t>(WaterNow.body) < Bodies.count()
			? Bodies.body(static_cast<std::size_t>(WaterNow.body)).kind
			: ftcore::WaterKind::Pool;
		Wet.step(bIn ? WaterNow.immersion : 0.f, kWaterBodyHeight, DeltaSeconds, 1.f, ftcore::waterSilt(Kind));
	}
	FTLook::ShowWet(*this, WetState, ftcore::leadwet::cloth(Wet, Weather), ftcore::leadwet::surface(Weather, Wet),
		static_cast<float>(GetActorLocation().Z), GroomComponents, GroomLooks, DeltaSeconds, *LogName);
}

void AFTPartyFollower::StepFollowerWaterLoco(ftcore::WaterLocoInput& In, float DeltaSeconds, UFTLocomotionAnimInstance* Anim)
{
	if (!bWaterLocoReady)
	{
		bWaterLocoReady = true;
		WaterLocoBody.reset(AFTExploreCharacter::SharedWaterLocoTuning());
	}
	if (Anim && WaterClipsAnim.Get() != Anim)
	{
		WaterClipsAnim = Anim;
		AFTExploreCharacter::LoadWaterClips(*Anim, Body->GetSkeletalMeshAsset(), WaterName, *LogName);
	}
	In.water = WaterNow;
	In.yaw = static_cast<float>(GetActorRotation().Yaw);
	In.velocity = FTSwimUnits::ToSwim(WaterVelocity);
	WaterPose = WaterLocoBody.step(In, DeltaSeconds);
	if (Anim)
	{
		Anim->SetWater(WaterPose.waterWeight, WaterPose.layers, WaterPose.layerCount, WaterPose.lean);
	}
	if (WaterPose.changed)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s: WaterLoco %s to %s at (%.0f, %.0f, %.0f) cm."), *LogName, ANSI_TO_TCHAR(ftcore::waterLocoStateName(WaterPose.from)),
			ANSI_TO_TCHAR(ftcore::waterLocoStateName(WaterPose.state)), GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z);
	}
}

void AFTPartyFollower::WaterWalk(const FVector2D& To, float WalkSpeed, float DeltaSeconds, UFTLocomotionAnimInstance* Anim)
{
	const FVector At = GetActorLocation();
	FVector2D Way = To - FVector2D(At.X, At.Y);
	const double Gap = Way.Size();
	const double Step = FMath::Min(Gap, static_cast<double>(WalkSpeed * DeltaSeconds));
	Way = Gap > 1.0 ? Way / Gap : FVector2D::ZeroVector;
	const FVector2D Spot = FVector2D(At.X, At.Y) + Way * Step;
	double Z = At.Z;
	FHitResult Hit;
	if (GetWorld()->LineTraceSingleByChannel(Hit, FVector(Spot.X, Spot.Y, At.Z + 150.0), FVector(Spot.X, Spot.Y, At.Z - 300.0), ECC_Visibility, TraceParams)
		&& !Hit.bStartPenetrating)
	{
		Z = Hit.ImpactPoint.Z;
	}
	FRotator Rot = GetActorRotation();
	if (Gap > 5.0 && DeltaSeconds > 0.f)
	{
		const float Want = FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(Way.Y), static_cast<float>(Way.X)));
		const float MaxStep = kMaxFaceRate * DeltaSeconds;
		Rot.Yaw += FMath::Clamp(FMath::FindDeltaAngleDegrees(static_cast<float>(Rot.Yaw), Want) * (1.f - FMath::Exp(-DeltaSeconds / kFaceTime)), -MaxStep, MaxStep);
	}
	SetActorLocationAndRotation(FVector(Spot.X, Spot.Y, Z), Rot, false, nullptr, ETeleportType::TeleportPhysics);
	bPlaced = true;
	if (DeltaSeconds > 0.f)
	{
		const float WalkSpeedNow = static_cast<float>(Step / DeltaSeconds);
		Speed += (WalkSpeedNow - Speed) * (1.f - FMath::Exp(-DeltaSeconds * kSpeedResponse));
	}
	if (Anim)
	{
		Anim->SetGroundSpeed(Speed);
		Anim->SetIntent(-1.f, -1.f);
		Anim->SetAir(false, 0.f);
	}
}

bool AFTPartyFollower::TickWater(const FVector2D& Ground, float DeltaSeconds, UFTLocomotionAnimInstance* Anim)
{
	using namespace FTSwimUnits;
	using ftcore::WaterFollowPhase;
	const AFTExploreCharacter* Lead = LeadExplorer.Get();
	const ftcore::WaterFollowOrder* Order = Lead && !bHeldByScene ? Lead->GetWaterOrder(WaterSlot) : nullptr;
	const UWorld* World = GetWorld();
	const UFTWaterSubsystem* System = World ? World->GetSubsystem<UFTWaterSubsystem>() : nullptr;
	const WaterFollowPhase Phase = Order && System && System->HasBodies() ? Order->phase : WaterFollowPhase::Ashore;
	const WaterFollowPhase Was = WaterPhase;
	const bool bNew = Phase != Was;
	WaterPhase = Phase;
	if (bNew)
	{
		bWaterMove = false;
		bWaterMoveDone = false;
		bWaterAtEntry = false;
		bWaterAtExit = false;
	}
	const FVector At = GetActorLocation();
#if !UE_BUILD_SHIPPING
	// -SwimProof=boom: kept on the boom line by the run (HoldForProofAt) while the plan has it at the surface; any other
	// phase (a dive, an exit) ends the hold and it swims again.
	if (bProofAir)
	{
		if (Phase == WaterFollowPhase::Surface)
		{
			return true;
		}
		bProofAir = false;
	}
	// -SwimProof=land|watchdog (AFTExploreCharacter::TickSwimProof): slot 0 is held (it does not walk to its entry, or it treads
	// where it is) until the plan rejoins it, then its feet height is logged four times a second for 2 s.
	static const FString ProofKind = []() { FString K; FParse::Value(FCommandLine::Get(), TEXT("SwimProof="), K); return K; }();
	if (Phase == WaterFollowPhase::Placed && Order && Order->rejoin && !ProofKind.IsEmpty()
		&& (WaterSlot == 0 || ProofKind == TEXT("switch") || ProofKind == TEXT("start")) && !bProofFreed)
	{
		bProofFreed = true;
		ProofLogLeft = 2.f;
		ProofLogAt = 0.f;
		ProofFrames = 0;
		bProofWant = false;
		ProofWantOff = 0.0;
	}
	// -SwimProof=replay -SwimProofReplay=4: the run-4 rejoin again, slot 0's walk to the water held as the land kind's is.
	static const bool bProofReplay4 = []() { int32 R = 0; return FParse::Value(FCommandLine::Get(), TEXT("SwimProofReplay="), R) && R == 4; }();
	const bool bProofStuck = WaterSlot == 0 && !bProofFreed
		&& (((ProofKind == TEXT("land") || (ProofKind == TEXT("replay") && bProofReplay4)) && Phase == WaterFollowPhase::ToEntry)
			|| (ProofKind == TEXT("watchdog") && Phase == WaterFollowPhase::Surface));
	if (ProofLogLeft > 0.f && Phase != WaterFollowPhase::Placed)
	{
		// Each frame after the rejoin: the feet's lowest and highest (the surface swim adds how far they stand off its
		// target that frame, ProofWantOff).
		ProofZMin = ProofFrames == 0 ? At.Z : FMath::Min(ProofZMin, At.Z);
		ProofZMax = ProofFrames == 0 ? At.Z : FMath::Max(ProofZMax, At.Z);
		++ProofFrames;
		ProofLogAt -= DeltaSeconds;
		ProofLogLeft -= DeltaSeconds;
		if (ProofLogAt <= 0.f)
		{
			ProofLogAt = 0.25f;
			UE_LOG(LogForgottenThird, Display, TEXT("SwimProof %s: %s after the rejoin, phase %d, feet z %.1f cm at (%.0f, %.0f)."), *ProofKind, *LogName,
				static_cast<int32>(Phase), At.Z, At.X, At.Y);
		}
		if (ProofLogLeft <= 0.f)
		{
			// Steady (ruled 11-18, second ruling): within 3 cm of the swim's steady target, bob included, from the first
			// frame. The raw feet (final, low, high, spread against the final) are logged beside it.
			const double OffFinal = FMath::Max(At.Z - ProofZMin, ProofZMax - At.Z);
			const TCHAR* Verdict = !bProofWant ? TEXT("NO SURFACE FRAMES (no target)")
				: ProofWantOff > 3.0 ? TEXT("NOT STEADY") : TEXT("STEADY (within 3 cm of the target)");
			UE_LOG(LogForgottenThird, Display,
				TEXT("SwimProof %s: %s feet over 2 s after the rejoin (%d frames): final %.1f cm, low %.1f, high %.1f, most off the final %.1f cm, most off the swim's target %.1f cm: %s."),
				*ProofKind, *LogName, ProofFrames, At.Z, ProofZMin, ProofZMax, OffFinal, ProofWantOff, Verdict);
		}
	}
#endif

	if (Phase == WaterFollowPhase::Ashore)
	{
		if (Was != WaterFollowPhase::Ashore)
		{
			bPlacedInWater = false;
			// Out on the land (its climb over, or the plan let it go; or back from its way to an entry): it eases back onto its slot from where it stands.
			LastDry = FVector2D(At.X, At.Y);
			bHasLastDry = true;
			Stray = LastDry - Ground;
			StrayWay = Stray.GetSafeNormal();
			StrayRate = FVector2D::ZeroVector;
			WaterVelocity = FVector::ZeroVector;
			UE_LOG(LogForgottenThird, Display, TEXT("%s: out of the water at (%.0f, %.0f, %.0f) cm, %.0f cm off its slot."), *LogName, At.X, At.Y, At.Z, Stray.Size());
		}
		// The water's layer fades out over the locomotion (WaterLoco on dry ground).
		if (bWaterLocoReady && WaterPose.waterWeight > 0.f)
		{
			WaterNow = ftcore::WaterState{};
			ftcore::WaterLocoInput In;
			StepFollowerWaterLoco(In, DeltaSeconds, Anim);
		}
		// Ready for the lead's next swim: where it would go in from. Only while an order may pick an entry (in a slot, the
		// lead swimming), four times a second.
		WaterEdgeAge += DeltaSeconds;
		if (Order && System && System->HasBodies() && Lead && Lead->IsSwimming())
		{
			if (WaterEdgeAge >= 0.25f)
			{
				WaterEdgeAge = 0.f;
				bWaterHasEdge = FindWaterEdge(System->Bodies(), At, Lead->GetActorLocation());
			}
		}
		else
		{
			bWaterHasEdge = false;
			WaterEdgeAge = 1.f;
		}
		return false;
	}
	const ftcore::WaterBodies& Bodies = System->Bodies();
	const float Time = System->WaterTime();
	ftcore::WaterLocoInput In;
	In.swim = Order->swim;
	const bool bWasUnder = WaterSwimWas == ftcore::SwimState::Under || WaterSwimWas == ftcore::SwimState::Diving;
	const bool bUnder = Order->swim == ftcore::SwimState::Under || Order->swim == ftcore::SwimState::Diving;
	In.dived = bUnder && !bWasUnder;
	In.surfaced = !bUnder && bWasUnder;
	WaterSwimWas = Order->swim;
	// The feet's height swimming at a place: the swim's draft and bob, over the bed.
	auto SwimFeetZ = [&Bodies, Time](const FVector2D& P, double Fallback, bool* bFound = nullptr)
	{
		float Surface = 0.f;
		const bool bHas = Bodies.surfaceHeight(ftcore::Vec2{ToMetres(P.X), ToMetres(P.Y)}, Time, Surface);
		if (bFound)
		{
			*bFound = bHas;
		}
		return bHas ? ToCm(ftcore::waterSwimFeetZ(Surface, Time)) : Fallback;
	};

	switch (Phase)
	{
	case WaterFollowPhase::ToEntry:
	{
		if (bTraversing)
		{
			return false; // the traversal carries it; the entry starts once it ends
		}
#if !UE_BUILD_SHIPPING
		if (bProofStuck)
		{
			if (Anim)
			{
				Speed = 0.f;
				Anim->SetGroundSpeed(0.f);
			}
			return true; // held where it stands (-SwimProof=land): its walk is given up twice
		}
#endif
		const FVector2D To(ToCm(Order->entry.point.x), ToCm(Order->entry.point.y));
		WaterWalk(To, kWaterWalkSpeed, DeltaSeconds, Anim);
		bWaterAtEntry = FVector2D::Distance(To, FVector2D(GetActorLocation().X, GetActorLocation().Y)) <= kWaterAtEntry;
		return true;
	}
	case WaterFollowPhase::Entering:
	{
		const bool bJump = Order->entry.kind != ftcore::WaterFollowEntryKind::Steps;
		if (!bWaterMove)
		{
			// Into the water: down the steps to their foot, or a jump from the edge to swimming depth a metre or more out.
			bWaterMove = true;
			WaterMoveTime = 0.f;
			WaterMoveFrom = At;
			FVector2D Into(ToCm(Order->entry.into.x), ToCm(Order->entry.into.y));
			if (bJump)
			{
				const FVector2D Out(Order->entry.into.x - Order->entry.point.x, Order->entry.into.y - Order->entry.point.y);
				const FVector2D Way = Out.GetSafeNormal();
				for (int32 Try = 0; Try < 10 && Bodies.depth(ftcore::Vec2{ToMetres(Into.X), ToMetres(Into.Y)}) <= ftcore::kWaterSwimDepth; ++Try)
				{
					Into += Way * 50.0;
				}
				// Never onto a solid standing out of the water (a pier, a block): on toward the lead, up to 5 m, until in
				// the water. None that near: the landing stays (the surface step and the watchdog take it from there).
				if (Lead && !ftcore::waterIn(SampleSwimAt(*System, Into, 0.f)))
				{
					const FVector2D From = Into;
					if (IntoWaterToward(*System, From, FVector2D(Lead->GetActorLocation().X, Lead->GetActorLocation().Y), 500.0, Into))
					{
						UE_LOG(LogForgottenThird, Display, TEXT("%s: the jump's landing (%.0f, %.0f) cm is out of the water; moved on %.0f cm to (%.0f, %.0f) cm."), *LogName,
							From.X, From.Y, FVector2D::Distance(From, Into), Into.X, Into.Y);
					}
					else
					{
						UE_LOG(LogForgottenThird, Warning, TEXT("%s: the jump's landing (%.0f, %.0f) cm is out of the water and none is within 5 m toward the lead; kept."), *LogName,
							From.X, From.Y);
					}
				}
			}
			WaterMoveTo = FVector(Into.X, Into.Y, SwimFeetZ(Into, At.Z));
			WaterMoveLength = bJump ? kWaterJumpSeconds : FMath::Max(0.5f, static_cast<float>(FVector::Dist(WaterMoveFrom, WaterMoveTo) / kWaterStepsSpeed));
			UE_LOG(LogForgottenThird, Display, TEXT("%s: %s into the water from (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f) cm."), *LogName,
				bJump ? TEXT("jumps") : TEXT("walks down the steps"), At.X, At.Y, At.Z, WaterMoveTo.X, WaterMoveTo.Y, WaterMoveTo.Z);
		}
		WaterMoveTime += DeltaSeconds;
		const float T = FMath::Clamp(WaterMoveTime / WaterMoveLength, 0.f, 1.f);
		FVector P = FMath::Lerp(WaterMoveFrom, WaterMoveTo, T);
		if (bJump)
		{
			P.Z += 4.0 * kWaterJumpRise * T * (1.f - T);
		}
		WaterVelocity = DeltaSeconds > 0.f ? (P - At) / DeltaSeconds : FVector::ZeroVector;
		bWaterMoveDone = T >= 1.f;
		const FVector2D Way(WaterMoveTo.X - WaterMoveFrom.X, WaterMoveTo.Y - WaterMoveFrom.Y);
		const float Yaw = Way.SizeSquared() > 1.0 ? FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(Way.Y), static_cast<float>(Way.X))) : static_cast<float>(GetActorRotation().Yaw);
		SetActorLocationAndRotation(P, FRotator(0.0, Yaw, 0.0), false, nullptr, ETeleportType::TeleportPhysics);
		SenseWater(*System, P, bWaterMoveDone);
		In.falling = bJump && !bWaterMoveDone;
		StepFollowerWaterLoco(In, DeltaSeconds, Anim);
		if (Anim)
		{
			Speed = bJump ? 0.f : kWaterStepsSpeed;
			Anim->SetGroundSpeed(Speed);
			Anim->SetIntent(-1.f, -1.f);
			Anim->SetAir(bJump && !bWaterMoveDone, static_cast<float>(WaterVelocity.Z));
		}
		return true;
	}
	case WaterFollowPhase::Surface:
	case WaterFollowPhase::DiveWait:
	case WaterFollowPhase::ToExit:
	case WaterFollowPhase::ExitWait:
	{
		// WaterLoco moves it: toward the order's target at its speed (the stroke's cap, or the sprint's), easing in over the
		// last 60 cm, treading there.
		SenseWater(*System, At, true);
		const ftcore::WaterLocoTuning& Tune = WaterLocoBody.tuning();
		const FVector2D Gap(ToCm(Order->target.x) - At.X, ToCm(Order->target.y) - At.Y);
		const double Far = Gap.Size();
		const bool bSprint = Order->speed > Tune.swimSpeed;
		const float Cap = bSprint ? Tune.swimSprintSpeed : Tune.swimSpeed;
		float Share = Cap > 0.f ? FMath::Clamp(Order->speed / Cap, 0.f, 1.f) : 0.f;
		Share *= static_cast<float>(FMath::Clamp(Far / 60.0, 0.0, 1.0));
		if (Far < 5.0)
		{
			Share = 0.f;
		}
#if !UE_BUILD_SHIPPING
		if ((bProofStuck || bProofHeld) && Phase == WaterFollowPhase::Surface)
		{
			Share = 0.f; // held treading where it is (-SwimProof=watchdog, or put in the boom's way for -SwimProof=boom)
		}
#endif
		const FVector2D Way = Far > 1.0 ? Gap / Far : FVector2D::ZeroVector;
		In.move = {static_cast<float>(Way.X) * Share, static_cast<float>(Way.Y) * Share};
		In.sprint = bSprint;
		StepFollowerWaterLoco(In, DeltaSeconds, Anim);
		// The step kept in the water (ftcore::waterSurfaceStep, the one in-water test): taken, slid along the edge (a bank,
		// a block standing out of it), or held, the watchdog told so it rejoins one pinned there.
		const float Sink = WaterPose.sink;
		const FVector2D From(At.X, At.Y);
		const ftcore::WaterSurfaceStep Kept = ftcore::waterSurfaceStep(
			ftcore::Vec2{WaterPose.velocity.x * DeltaSeconds, WaterPose.velocity.y * DeltaSeconds}, WaterSlideSide,
			[this, System, &From, Sink](ftcore::Vec2 D) { return SampleSwimAt(*System, From + FVector2D(ToCm(D.x), ToCm(D.y)), Sink); });
		WaterSlideSide = Kept.side;
		if (Kept.kind == ftcore::WaterStepKind::Hold)
		{
			WaterBlockedFrame = GFrameCounter;
			if (WaterHeldSteps++ == 0)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("%s: first surface step held at the edge at (%.0f, %.0f) cm."), *LogName, From.X, From.Y);
			}
		}
		else if (Kept.kind == ftcore::WaterStepKind::Slide)
		{
			WaterTurnedFrame = GFrameCounter;
			if (WaterSlidSteps++ == 0)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("%s: first surface step slid along the edge at (%.0f, %.0f) cm."), *LogName, From.X, From.Y);
			}
		}
		const FVector2D Step(ToCm(Kept.step.x), ToCm(Kept.step.y));
		// The one surface rule (ftcore/WaterSurfaceFeet.h): the draft and bob plus the splash's sink, never nearer the
		// bed than kWaterBedFloat. The rejoin's placement uses the same rule, so it starts where this holds it. The feet
		// ride with the surface's own move since the last frame (the waves and the bob) and ease only the gap off it
		// (ftcore::waterFeetCarry); -SwimFeetEaseAll eases the whole way.
		bool bRideFound = false;
		const double Ride = SwimFeetZ(FVector2D(At.X, At.Y) + Step, At.Z, &bRideFound);
		const bool bOnRide = bRideFound && ftcore::waterFeetOnRide(ToMetres(Ride), WaterPose.sink, WaterBedZ);
		const double Want = ToCm(ftcore::waterSurfaceFeetZ(ToMetres(Ride), WaterPose.sink, WaterBedZ));
		const double RideMove = !bSwimFeetEaseAll && bOnRide && bWaterOnRide && WaterRideFrame + 1 == GFrameCounter ? Ride - WaterRideZ : 0.0;
		const double Z = ToCm(ftcore::waterFeetCarry(ToMetres(At.Z), ToMetres(RideMove), ToMetres(Want), DeltaSeconds, kWaterRiseEase));
		WaterRideZ = Ride;
		WaterRideFrame = GFrameCounter;
		bWaterOnRide = bOnRide;
		const FVector P(At.X + Step.X, At.Y + Step.Y, Z);
#if !UE_BUILD_SHIPPING
		if (ProofLogLeft > 0.f)
		{
			ProofWantOff = FMath::Max(ProofWantOff, FMath::Abs(At.Z - Want)); // -SwimProof: this frame's feet off its target
			bProofWant = true;
		}
#endif
		WaterVelocity = DeltaSeconds > 0.f ? (P - At) / DeltaSeconds : FVector::ZeroVector;
		SetActorLocationAndRotation(P, FRotator(0.0, WaterPose.yaw, 0.0), false, nullptr, ETeleportType::TeleportPhysics);
		if (Phase == WaterFollowPhase::ToExit)
		{
			// At its place in the queue (or with no exit to go to, straight to its own way out).
			bWaterAtExit = Order->exitIndex < 0 || Far <= ToCm(Lead->GetWaterFollowTuning().atExit);
		}
		if (Anim)
		{
			Speed = 0.f;
			Anim->SetGroundSpeed(0.f);
			Anim->SetIntent(-1.f, -1.f);
			Anim->SetAir(false, 0.f);
		}
		return true;
	}
	case WaterFollowPhase::Under:
	{
		// Toward the order's place in the band around the lead (its body's reference point), its velocity easing to the
		// order's; inside the band it drifts to a stop.
		const FVector Ref = At + FVector(0.0, 0.0, ToCm(ftcore::kWaterSwimDraft));
		const FVector Gap = ToEngine(Order->target) - Ref;
		const double Far = Gap.Size();
		const FVector Wanted = Far > 1.0 ? Gap / Far * FMath::Min(ToCm(Order->speed), Far / FMath::Max(DeltaSeconds, 1e-3f)) : FVector::ZeroVector;
		WaterVelocity += (Wanted - WaterVelocity) * (DeltaSeconds > 0.f ? 1.f - FMath::Exp(-DeltaSeconds / kWaterUnderEase) : 1.f);
		FVector P = At + WaterVelocity * DeltaSeconds;
		SenseWater(*System, P, true);
		P.Z = FMath::Max(P.Z, ToCm(WaterBedZ + ftcore::kWaterBedFloat));
		FRotator Rot = GetActorRotation();
		if (WaterVelocity.Size2D() > 20.0 && DeltaSeconds > 0.f)
		{
			const float Want = FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(WaterVelocity.Y), static_cast<float>(WaterVelocity.X)));
			Rot.Yaw += FMath::FindDeltaAngleDegrees(static_cast<float>(Rot.Yaw), Want) * (1.f - FMath::Exp(-DeltaSeconds / kFaceTime));
		}
		SetActorLocationAndRotation(P, Rot, false, nullptr, ETeleportType::TeleportPhysics);
		In.swimVelocity = ToSwim(WaterVelocity);
		In.sprint = Order->sprint;
		StepFollowerWaterLoco(In, DeltaSeconds, Anim);
		if (Anim)
		{
			Speed = 0.f;
			Anim->SetGroundSpeed(0.f);
			Anim->SetAir(false, 0.f);
		}
		return true;
	}
	case WaterFollowPhase::Climbing:
	{
		const bool bHasExit = Order->exitIndex >= 0 && static_cast<std::size_t>(Order->exitIndex) < Bodies.exitCount();
		const ftcore::WaterExit* Exit = bHasExit ? &Bodies.exit(static_cast<std::size_t>(Order->exitIndex)) : nullptr;
		const bool bSteps = Exit && Exit->kind == ftcore::WaterExitKind::Steps;
		const bool bLadder = Exit && Exit->kind == ftcore::WaterExitKind::Ladder;
		const ftcore::WaterFollowTuning& Tun = Lead->GetWaterFollowTuning();
		if (!bWaterMove)
		{
			// Out: up the ladder and over its top, up the steps, or with no exit a mantle onto the bank behind the lead.
			bWaterMove = true;
			WaterMoveTime = 0.f;
			WaterMoveFrom = At;
			if (Exit)
			{
				WaterMoveTo = FVector(ToCm(Exit->top.x), ToCm(Exit->top.y), ToCm(Exit->topZ));
			}
			else
			{
				const double Half = Lead->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
				const FVector LeadFeet = Lead->GetActorLocation() - FVector(0.0, 0.0, Half);
				const FVector Back = -Lead->GetActorForwardVector().GetSafeNormal2D();
				WaterMoveTo = LeadFeet + Back * 150.0 + Lead->GetActorRightVector().GetSafeNormal2D() * (WaterSlot % 2 == 0 ? 100.0 : -100.0);
			}
			WaterMoveLength = bLadder ? Tun.ladderSeconds : bSteps ? FMath::Max(0.5f, static_cast<float>(FVector::Dist(WaterMoveFrom, WaterMoveTo) / ToCm(Tun.stepsSpeed)))
				: Tun.mantleSeconds;
			bWaterClimbPath = Tun.climbPath;
			if (bWaterClimbPath)
			{
				// The path from where it is (feet): a ladder's foot at its own speed, then up and across; a flight one walk.
				WaterClimb = ftcore::waterClimbPath(Tun, Exit, ToSwim(WaterMoveFrom), ToSwim(WaterMoveTo));
				WaterMoveLength = FMath::Max(WaterClimb.seconds(), 0.05f);
			}
			UE_LOG(LogForgottenThird, Display, TEXT("%s: climbs out by %s to (%.0f, %.0f, %.0f) cm in %.2f s%s."), *LogName,
				bLadder ? TEXT("the ladder") : bSteps ? TEXT("the steps") : TEXT("a mantle"), WaterMoveTo.X, WaterMoveTo.Y, WaterMoveTo.Z, WaterMoveLength,
				bWaterClimbPath ? *FString::Printf(TEXT(" on the climb path (swim %.2f, up %.2f, across %.2f s)"), WaterClimb.swim, WaterClimb.climb, WaterClimb.across) : TEXT(""));
		}
		WaterMoveTime += DeltaSeconds;
		const float T = FMath::Clamp(WaterMoveTime / WaterMoveLength, 0.f, 1.f);
		FVector P;
		if (bWaterClimbPath)
		{
			const float Tm = FMath::Min(WaterMoveTime, WaterClimb.seconds());
			P = ToEngine(WaterClimb.at(Tm));
			const int Leg = WaterClimb.leg(Tm);
			if (!bSteps && Leg == 0)
			{
				// The swim to the ladder's foot: the stroke toward it.
				const FVector2D ToFoot(ToCm(WaterClimb.foot.x) - At.X, ToCm(WaterClimb.foot.y) - At.Y);
				const FVector2D Dir = ToFoot.GetSafeNormal();
				In.move = {static_cast<float>(Dir.X), static_cast<float>(Dir.Y)};
			}
			else if (!bSteps)
			{
				const float Up = WaterClimb.climb + WaterClimb.across;
				In.exitMove = bLadder ? (Leg == 1 ? ftcore::WaterExitMove::Ladder : ftcore::WaterExitMove::LadderTop) : ftcore::WaterExitMove::Mantle;
				In.exitProgress = bLadder ? (Leg == 1 ? 0.f : WaterClimb.legProgress(Tm)) : (Up > 0.f ? FMath::Clamp((Tm - WaterClimb.swim) / Up, 0.f, 1.f) : 1.f);
				In.ladderClimbed = ToMetres(P.Z - WaterMoveFrom.Z);
				const FVector Way = (WaterMoveTo - WaterMoveFrom).GetSafeNormal2D();
				In.exitHand = ToSwim(FVector(WaterMoveFrom.X, WaterMoveFrom.Y, WaterMoveTo.Z) + Way * 35.0);
			}
		}
		else if (bSteps)
		{
			P = FMath::Lerp(WaterMoveFrom, WaterMoveTo, T);
		}
		else
		{
			// Up first (the rungs, or the push up on the hands), then across onto the top.
			const float Up = FMath::Clamp(T * 2.f, 0.f, 1.f);
			const float Across = FMath::Clamp(T * 2.f - 1.f, 0.f, 1.f);
			// A ladder's climb starts at its foot: its first sixth swims there from its place in the queue.
			const FVector2D From(WaterMoveFrom.X, WaterMoveFrom.Y);
			const FVector2D Foot = bLadder ? FVector2D(ToCm(Exit->foot.x), ToCm(Exit->foot.y)) : From;
			const FVector2D Top(WaterMoveTo.X, WaterMoveTo.Y);
			const FVector2D Flat = Across > 0.f ? FMath::Lerp(Foot, Top, static_cast<double>(Across)) : FMath::Lerp(From, Foot, static_cast<double>(FMath::Clamp(T * 6.f, 0.f, 1.f)));
			P = FVector(Flat.X, Flat.Y, FMath::Lerp(WaterMoveFrom.Z, WaterMoveTo.Z, static_cast<double>(Up)));
			In.exitMove = bLadder ? (T < 0.5f ? ftcore::WaterExitMove::Ladder : ftcore::WaterExitMove::LadderTop) : ftcore::WaterExitMove::Mantle;
			In.exitProgress = bLadder ? (T < 0.5f ? 0.f : (T - 0.5f) * 2.f) : T;
			In.ladderClimbed = ToMetres(P.Z - WaterMoveFrom.Z);
			const FVector Way = (WaterMoveTo - WaterMoveFrom).GetSafeNormal2D();
			In.exitHand = ToSwim(FVector(WaterMoveFrom.X, WaterMoveFrom.Y, WaterMoveTo.Z) + Way * 35.0);
		}
		WaterVelocity = DeltaSeconds > 0.f ? (P - At) / DeltaSeconds : FVector::ZeroVector;
		bWaterMoveDone = T >= 1.f;
		const FVector2D Way(WaterMoveTo.X - WaterMoveFrom.X, WaterMoveTo.Y - WaterMoveFrom.Y);
		FRotator Rot = GetActorRotation();
		if (Exit)
		{
			Rot.Yaw = FMath::RadiansToDegrees(FMath::Atan2(Exit->out.y, Exit->out.x));
		}
		else if (Way.SizeSquared() > 1.0)
		{
			Rot.Yaw = FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(Way.Y), static_cast<float>(Way.X)));
		}
		SetActorLocationAndRotation(P, Rot, false, nullptr, ETeleportType::TeleportPhysics);
		SenseWater(*System, P, !bSteps);
		StepFollowerWaterLoco(In, DeltaSeconds, Anim);
		if (Anim)
		{
			Speed = bSteps ? static_cast<float>(ToCm(Tun.stepsSpeed)) : 0.f;
			Anim->SetGroundSpeed(Speed);
			Anim->SetIntent(-1.f, -1.f);
			Anim->SetAir(false, 0.f);
		}
		return true;
	}
	case WaterFollowPhase::Placed:
	{
		// The lead was rescued: once it is out of the water, it is put beside the lead (its side by its slot), standing.
		if (Order->rejoin)
		{
			// Placed while the lead swims on (a teleport, the watchdog, a second failed entry): put at its surface slot by the
			// lead, in the water, facing the lead's way; the next step it swims from there.
			// Its feet go where the surface swim will hold them (the one rule, ftcore/WaterSurfaceFeet.h), so there is no
			// ease after the placement: the bed under the slot is looked at first (SenseWater's trace, from the draft's
			// height), and over a column shallower than the draft the feet start just over the bed, not at the draft
			// (placed at the draft alone, they rose 19 cm over 1.1 s, 11-18).
			// The slot itself in the water, else the first point in it on the way to the lead (a slot on a bank or a block
			// would pin it there, and the watchdog place it there again).
			const FVector2D Slot(ToCm(Order->target.x), ToCm(Order->target.y));
			const FVector2D LeadXY(Lead->GetActorLocation().X, Lead->GetActorLocation().Y);
			FVector2D To = Slot;
			if (IntoWaterToward(*System, Slot, LeadXY, FMath::Min(FVector2D::Distance(Slot, LeadXY), 2000.0), To))
			{
				bWaterRejoinWarned = false;
			}
			else if (!bWaterRejoinWarned)
			{
				bWaterRejoinWarned = true;
				UE_LOG(LogForgottenThird, Warning, TEXT("%s: rejoin slot (%.0f, %.0f) cm has no water within 20 m toward the lead; placed at the slot."), *LogName, Slot.X, Slot.Y);
			}
			bool bDraftFound = false;
			const double Draft = SwimFeetZ(To, Lead->GetActorLocation().Z - Lead->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), &bDraftFound);
			SenseWater(*System, FVector(To.X, To.Y, Draft), true);
			const float BedAt = WaterBedZ;
			const FVector P(To.X, To.Y, ToCm(ftcore::waterSurfaceFeetZ(ToMetres(Draft), 0.f, BedAt)));
			SetActorLocationAndRotation(P, FRotator(0.0, Lead->GetActorRotation().Yaw, 0.0), false, nullptr, ETeleportType::TeleportPhysics);
			WaterVelocity = FVector::ZeroVector;
			WaterRideZ = Draft;
			WaterRideFrame = GFrameCounter;
			bWaterOnRide = bDraftFound && ftcore::waterFeetOnRide(ToMetres(Draft), 0.f, BedAt);
			bPlacedInWater = true;
			UE_LOG(LogForgottenThird, Display, TEXT("%s: rejoined the lead in the water at (%.0f, %.0f, %.0f) cm (the draft's feet %.1f, bed %.1f, surface %.1f cm: %s)."),
				*LogName, P.X, P.Y, P.Z, Draft, ToCm(BedAt), Draft + ToCm(ftcore::kWaterSwimDraft),
				P.Z > Draft + 0.5 ? TEXT("the column is shallower than the draft, feet over the bed") : TEXT("at the draft"));
			SenseWater(*System, P, true);
			StepFollowerWaterLoco(In, DeltaSeconds, Anim);
			return true;
		}
		if (Lead->IsSwimming() || Lead->IsUnderWater())
		{
			bPlacedInWater = true;
			SenseWater(*System, At, true);
			StepFollowerWaterLoco(In, DeltaSeconds, Anim);
			return true;
		}
		const double Half = Lead->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector LeadFeet = Lead->GetActorLocation() - FVector(0.0, 0.0, Half);
		const FVector Side = Lead->GetActorRightVector().GetSafeNormal2D() * (WaterSlot % 2 == 0 ? 120.0 : -120.0);
		FVector P = LeadFeet - Lead->GetActorForwardVector().GetSafeNormal2D() * 100.0 + Side;
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, P + FVector(0.0, 0.0, 150.0), P - FVector(0.0, 0.0, 300.0), ECC_Visibility, TraceParams) && !Hit.bStartPenetrating)
		{
			P.Z = Hit.ImpactPoint.Z;
		}
		SetActorLocationAndRotation(P, FRotator(0.0, Lead->GetActorRotation().Yaw, 0.0), false, nullptr, ETeleportType::TeleportPhysics);
		WaterVelocity = FVector::ZeroVector;
		WaterNow = ftcore::WaterState{};
		bPlacedInWater = false;
		StepFollowerWaterLoco(In, DeltaSeconds, Anim);
		if (Anim)
		{
			Speed = 0.f;
			Anim->SetGroundSpeed(0.f);
			Anim->SetAir(false, 0.f);
		}
		return true;
	}
	default:
		return false;
	}
}
