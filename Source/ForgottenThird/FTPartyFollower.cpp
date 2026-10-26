#include "FTPartyFollower.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"

#include "FTMeshFacing.h"

namespace
{
// Slower than this (cm/s) counts as standing still. A speed, not a distance: at a high frame rate a walking
// follower moves well under a centimetre a frame.
constexpr float kStillSpeed = 10.f;
// The time constant (s) of the follower's turn toward the way it moves, as the lead's facing eases (LocoTuning::faceTime).
constexpr float kFaceTime = 0.15f;
// How fast the speed fed to the anim follows each frame's step (1/s), so uneven frames don't jitter the stride.
constexpr float kSpeedResponse = 12.f;
}

AFTPartyFollower::AFTPartyFollower()
{
	PrimaryActorTick.bCanEverTick = false;
	// The body hangs off its own root: as the root, the actor's turn would overwrite the mesh's facing offset.
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetGenerateOverlapEvents(false);
	Body->SetupAttachment(GetRootComponent());
	SetActorEnableCollision(false);
	TraceParams = FCollisionQueryParams(FName(TEXT("FTPartyFollower")), false);
}

bool AFTPartyFollower::Setup(const FString& Name, const AActor* Lead)
{
	const FString Lower = Name.ToLower();
	const FString Dir = FString::Printf(TEXT("/Game/Characters/%s/%s/SkeletalMeshes/"), *Name, *Lower);
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *(Dir + Lower + TEXT(".") + Lower));
	TraceParams.AddIgnoredActor(this);
	if (Lead)
	{
		TraceParams.AddIgnoredActor(Lead);
	}
	if (!Mesh)
	{
		SetActorHiddenInGame(true);
		return false;
	}
	Body->SetSkeletalMesh(Mesh);
	// Turn the body to face the actor's forward (+X).
	Body->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Mesh), 0.0));
	Body->SetAnimInstanceClass(UFTLocomotionAnimInstance::StaticClass());
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance());
	if (!Anim)
	{
		return true;
	}
	// Retargeted clips first (the mocap sample's, then the template's), then the mesh's own idle and walk.
	const FString Rig = FString::Printf(TEXT("/Game/Characters/%s/Rig/Retargeted/"), *Name);
	auto Load = [&](std::initializer_list<const TCHAR*> Retargeted, const TCHAR* Own) -> UAnimSequence*
	{
		for (const TCHAR* Source : Retargeted)
		{
			const FString Asset = FString::Printf(TEXT("%s_to_%s"), Source, *Lower);
			if (UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *(Rig + Asset + TEXT(".") + Asset), nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				return Clip;
			}
		}
		return Own ? LoadObject<UAnimSequence>(nullptr, *(Dir + Lower + Own + TEXT(".") + Lower + Own), nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
	};
	Anim->SetClip(ftcore::LocoClip::Idle, Load({TEXT("M_Neutral_Stand_Idle_Loop"), TEXT("MM_Idle")}, TEXT("idle")));
	Anim->SetClip(ftcore::LocoClip::Walk, Load({TEXT("M_Neutral_Walk_Loop_F"), TEXT("MF_Unarmed_Walk_Fwd")}, TEXT("walk")));
	Anim->SetClip(ftcore::LocoClip::Run, Load({TEXT("M_Neutral_Run_Loop_F"), TEXT("MF_Unarmed_Jog_Fwd")}, nullptr));
	Anim->SetClip(ftcore::LocoClip::Sprint, Load({TEXT("M_Neutral_Sprint_Loop_F")}, nullptr));
	Anim->SetMovesFromGait(ftcore::LocoClip::Walk);
	Anim->SetMovesFromGait(ftcore::LocoClip::Run);
	Anim->SetMovesFromGait(ftcore::LocoClip::Sprint);
	Anim->SetTurnsFromRun();
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
	FRotator Rot = GetActorRotation();
	UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance());
	const ftcore::Locomotion* Loco = Anim ? &Anim->GetLocomotion() : nullptr;
	if (Loco && Loco->turning() && !ftcore::isLocoStandTurn(Loco->turnClip()))
	{
		// A replayed turn turns the body by its clip's root, as it turned the lead.
		if (Loco->turnsBegun() != TurnsSeen)
		{
			TurnsSeen = Loco->turnsBegun();
			TurnEntryYaw = static_cast<float>(Rot.Yaw) - Loco->turnYaw();
		}
		Rot.Yaw = TurnEntryYaw + Loco->turnYaw();
	}
	else if (bWantWalk)
	{
		// Ease toward the trail rather than snapping to each frame's step, so a weaving lead doesn't jerk it round.
		const float Want = FMath::RadiansToDegrees(FMath::Atan2(Move.Y, Move.X));
		const float Ease = DeltaSeconds > 0.f ? 1.f - FMath::Exp(-DeltaSeconds / kFaceTime) : 1.f;
		Rot.Yaw += FMath::FindDeltaAngleDegrees(static_cast<float>(Rot.Yaw), Want) * Ease;
	}
	SetActorLocationAndRotation(FVector(Ground.X, Ground.Y, Z), Rot, false, nullptr, ETeleportType::TeleportPhysics);

	if (DeltaSeconds > 0.f)
	{
		Speed += ((bWantWalk ? Step : 0.f) - Speed) * (1.f - FMath::Exp(-DeltaSeconds * kSpeedResponse));
	}
	if (Anim)
	{
		Anim->SetGroundSpeed(Speed);
		Anim->SetIntent(WantSpeed, StopDistance);
	}
}

void AFTPartyFollower::ReplayTurn(float Delta)
{
	if (UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Body->GetAnimInstance()))
	{
		Anim->ReplayTurn(Delta);
	}
}
