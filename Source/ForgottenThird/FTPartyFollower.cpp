#include "FTPartyFollower.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Misc/CommandLine.h"

#include "FTExploreCharacter.h"
#include "FTLook.h"
#include "FTMeshFacing.h"
#include "ForgottenThird.h"

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
// The lead's vaults, climbs and catches (cm): the face ahead is looked at while one is ahead within the probes' reach
// and the body goes its way at least this squarely (the cosine), and is the lead's when within kMarkSlack of it (along
// its way); one is forgotten once the body is kMarkPassed past it or kMarkFar from it.
constexpr float kMarkReach = 560.f;
constexpr float kMarkFacing = 0.8f;
constexpr float kMarkSlack = 60.f;
constexpr float kMarkPassed = 100.f;
constexpr float kMarkFar = 2000.f;
// Let go by a traversal off its slot, the body eases back onto it with this time constant (s), no faster than this
// (cm/s); ahead of its slot the way the traversal went, within kStrayWait (cm), it waits for its slot rather than going
// back toward it.
constexpr float kStrayTime = 0.4f;
constexpr float kStrayRate = 200.f;
constexpr float kStrayWait = 300.f;
// Carried by a catch, the body turns square into the face with this time constant (s), as the lead does.
constexpr float kCatchTurnTime = 0.05f;
// Why the last look ahead started no traversal (LastMiss), for the log.
const TCHAR* const kMissNames[] = {TEXT("not looked for: not going its way, or on the ground at a catch, or in the air at a vault or climb"),
	TEXT("nothing ahead to go over, onto or catch"), TEXT("the face ahead not the lead's"), TEXT("still out of reach"), TEXT("no clip fits")};
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
	LeadExplorer = Cast<AFTExploreCharacter>(Lead);
	MarksSeen = LeadExplorer.IsValid() ? LeadExplorer->GetTraversalsBegun() : 0;
	LogName = FString::Printf(TEXT("FTPartyFollower %s"), *Look.Name);
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
	const int32 Cloth = FTLook::AddPieces(*this, *Body, Look.PiecePaths, Pieces, *LogName);
	FTLook::AddGrooms(*this, *Body, Pieces, Look.Grooms, GroomComponents, *LogName);
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

void AFTPartyFollower::Place(const FVector2D& Ground, float LeadFeetZ, float DeltaSeconds, float WantSpeed, float StopDistance)
{
	// Before it moves: the pose and the placement are both last frame's. No foot is planted in the air, nor while a
	// traversal carries the body.
	SlideProbe.Sample(*Body, DeltaSeconds, Speed, bAir || bTraversing);
	const FVector Was = GetActorLocation();
	const bool bWasPlaced = bPlaced;
	const bool bSlotMoving = bWasPlaced && DeltaSeconds > 0.f && FVector2D::Distance(Ground, LastSlot) > kStillSpeed * DeltaSeconds;
	LastSlot = Ground;
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance());

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
	const bool bCarried = bTraversing && Anim && TickTraversal(*Anim, DeltaSeconds, Ground, bSlotMoving, Carried);
	bTraversing = bCarried;
	// Otherwise on its slot, or on its way back to it.
	FVector2D Spot = bCarried ? FVector2D(Carried.X, Carried.Y) : Ground + EaseStray(Ground, DeltaSeconds);
	// Not on through a face the lead caught while it may yet catch it: held short of it, as the lead's capsule is stopped
	// there. Landed below it, or risen clear of its top, it eases back onto its slot from there.
	if (bAtWall && !bCarried)
	{
		const FVector2D In(WallIn.X, WallIn.Y);
		const double Over = FVector2D::DotProduct(Spot - FVector2D(WallFace.X, WallFace.Y), In) + AFTExploreCharacter::CapsuleRadius;
		if (Over > 0.0)
		{
			Spot -= In * Over;
			Stray = Spot - Ground;
			StrayWay = In;
		}
	}
	const FVector2D Move = bWasPlaced ? Spot - LastGround : FVector2D::ZeroVector;
	const float Step = DeltaSeconds > 0.f ? static_cast<float>(Move.Size()) / DeltaSeconds : 0.f;
	const bool bWantWalk = Step > kStillSpeed;
	Going = DeltaSeconds > 0.f ? Move / DeltaSeconds : FVector2D::ZeroVector;
	LastGround = Spot;
	bPlaced = true;

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
	if (Stray.IsZero() || DeltaSeconds <= 0.f)
	{
		return Stray;
	}
	// Back toward its slot as it goes on, no faster than a walk back.
	FVector2D Change = Stray * (FMath::Exp(-DeltaSeconds / kStrayTime) - 1.0);
	const double Most = kStrayRate * DeltaSeconds;
	if (Change.SizeSquared() > Most * Most)
	{
		Change = Change.GetSafeNormal() * Most;
	}
	FVector2D Next = Stray + Change;
	// Ahead of its slot the way the traversal went, it doesn't go back toward it (it would walk back through what it went
	// over): it waits for its slot to come on.
	const double Back = FVector2D::DotProduct(Ground + Next - LastGround, StrayWay);
	if (Back < 0.0 && FVector2D::DotProduct(Next, StrayWay) > 0.0 && Next.Size() < kStrayWait)
	{
		Next -= StrayWay * Back;
	}
	Stray = Next.SizeSquared() < 1.0 ? FVector2D::ZeroVector : Next;
	return Stray;
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
	Stray = FVector2D::ZeroVector;
	bAir = false;
	AirSpeed = 0.f;
	bFacing = false;
	Speed = 0.f;
	if (bPlaced)
	{
		// Down on the ground below it, and the anim told it stands.
		Place(FVector2D(At.X, At.Y), static_cast<float>(At.Z), 0.f);
	}
}

void AFTPartyFollower::ReplayTurn(float Delta)
{
	if (UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance()))
	{
		Anim->ReplayTurn(Delta);
	}
}
