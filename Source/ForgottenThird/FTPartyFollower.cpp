#include "FTPartyFollower.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Misc/CommandLine.h"

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
	const FString Who = FString::Printf(TEXT("FTPartyFollower %s"), *Look.Name);
	const FFTLoadedLook Loaded = FTLook::LoadLook(Look, *Who);
	if (!Loaded.Body)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: no body; it stays hidden."), *Who);
		SetActorHiddenInGame(true);
		return false;
	}
	Body->SetSkeletalMesh(Loaded.Body);
	// Turn the body to face the actor's forward (+X).
	Body->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Loaded.Body), 0.0));
	Body->SetAnimInstanceClass(UFTLocomotionAnimInstance::StaticClass());
	const int32 Cloth = FTLook::AddPieces(*this, *Body, Look.PiecePaths, Pieces, *Who);
	FTLook::AddGrooms(*this, *Body, Pieces, Look.Grooms, GroomComponents, *Who);
	UE_LOG(LogForgottenThird, Display, TEXT("%s: body %s facing %.0f, %d pieces (%d with cloth), %d grooms, clips from %s."), *Who,
		*Loaded.Body->GetName(), Body->GetRelativeRotation().Yaw, Pieces.Num(), Cloth, GroomComponents.Num(), *Loaded.ClipFolder);
	// The lead's jumps are replayed on the trail (ReplayJump); its climbs and vaults aren't, so no traversal clips.
	if (UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance()))
	{
		Anim->SetBaseClips(Loaded.Clips, false);
	}
	return true;
}

void AFTPartyFollower::Place(const FVector2D& Ground, float LeadFeetZ, float DeltaSeconds, float WantSpeed, float StopDistance)
{
	const FVector2D Move = bPlaced ? Ground - LastGround : FVector2D::ZeroVector;
	const float Step = DeltaSeconds > 0.f ? static_cast<float>(Move.Size()) / DeltaSeconds : 0.f;
	const bool bWantWalk = Step > kStillSpeed;
	LastGround = Ground;
	bPlaced = true;
	// Before it moves: the pose and the placement are both last frame's.
	SlideProbe.Sample(*Body, DeltaSeconds, Speed);

	double Z = LeadFeetZ;
	if (const UWorld* World = GetWorld())
	{
		FHitResult Hit;
		const FVector From(Ground.X, Ground.Y, LeadFeetZ + 150.0);
		const FVector To(Ground.X, Ground.Y, LeadFeetZ - 300.0);
		if (World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, TraceParams))
		{
			Z = Hit.ImpactPoint.Z;
		}
	}
	// A replayed jump: a hop off the ground below, back down onto it.
	if (bAir && DeltaSeconds > 0.f)
	{
		AirHeight += AirSpeed * DeltaSeconds - 0.5f * AirGravity * DeltaSeconds * DeltaSeconds;
		AirSpeed -= AirGravity * DeltaSeconds;
		if (AirHeight <= 0.f && AirSpeed < 0.f)
		{
			bAir = false;
			AirHeight = AirSpeed = 0.f;
		}
	}
	Z += AirHeight;
	FRotator Rot = GetActorRotation();
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance());
	const ftcore::Locomotion* Loco = Anim ? &Anim->GetLocomotion() : nullptr;
	if (Loco && Loco->turning())
	{
		// A replayed turn (running, or on the spot) turns the body by its clip's root, as it turned the lead.
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
	SetActorLocationAndRotation(FVector(Ground.X, Ground.Y, Z), Rot, false, nullptr, ETeleportType::TeleportPhysics);
	// Facing where it was asked to, or setting off, it no longer turns on the spot.
	if (bFacing && (bWantWalk || FMath::Abs(FMath::FindDeltaAngleDegrees(static_cast<float>(Rot.Yaw), FaceYaw)) < kFacedWithin))
	{
		bFacing = false;
	}

	if (DeltaSeconds > 0.f)
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

void AFTPartyFollower::ReplayJump(float UpSpeed, float Gravity)
{
	if (bAir || UpSpeed <= 0.f || Gravity <= 0.f)
	{
		return;
	}
	bAir = true;
	AirHeight = 0.f;
	AirSpeed = UpSpeed;
	AirGravity = Gravity;
}

void AFTPartyFollower::FaceTo(float Yaw)
{
	bFacing = true;
	FaceYaw = Yaw;
}

void AFTPartyFollower::ReplayTurn(float Delta)
{
	if (UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance()))
	{
		Anim->ReplayTurn(Delta);
	}
}
