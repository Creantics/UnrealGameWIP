#include "FTPartyFollower.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"

#include "FTMeshFacing.h"

namespace
{
// Moves shorter than this (cm) in a frame count as standing still.
constexpr float kMoveEpsilon = 0.5f;
// The walk clip's ground speed (cm/s), as on the explore character.
constexpr float kWalkClipSpeed = 140.f;
}

AFTPartyFollower::AFTPartyFollower()
{
	PrimaryActorTick.bCanEverTick = false;
	Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetGenerateOverlapEvents(false);
	SetRootComponent(Body);
	SetActorEnableCollision(false);
	TraceParams = FCollisionQueryParams(FName(TEXT("FTPartyFollower")), false);
}

bool AFTPartyFollower::Setup(const FString& Name, const AActor* Lead)
{
	const FString Lower = Name.ToLower();
	const FString Dir = FString::Printf(TEXT("/Game/Characters/%s/%s/SkeletalMeshes/"), *Name, *Lower);
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *(Dir + Lower + TEXT(".") + Lower));
	IdleClip = LoadObject<UAnimSequence>(nullptr, *(Dir + Lower + TEXT("idle.") + Lower + TEXT("idle")));
	WalkClip = LoadObject<UAnimSequence>(nullptr, *(Dir + Lower + TEXT("walk.") + Lower + TEXT("walk")));
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
	Body->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	return true;
}

void AFTPartyFollower::PlayClip(UAnimSequence* Clip)
{
	if (Clip)
	{
		Body->PlayAnimation(Clip, true);
	}
}

void AFTPartyFollower::Place(const FVector2D& Ground, float LeadFeetZ, float DeltaSeconds)
{
	const FVector2D Move = bPlaced ? Ground - LastGround : FVector2D::ZeroVector;
	const float Moved = static_cast<float>(Move.Size());
	const bool bWantWalk = Moved > kMoveEpsilon;
	LastGround = Ground;
	bPlaced = true;

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
	if (bWantWalk)
	{
		Rot = FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(Move.Y, Move.X)), 0.0);
	}
	SetActorLocationAndRotation(FVector(Ground.X, Ground.Y, Z), Rot, false, nullptr, ETeleportType::TeleportPhysics);

	if (!bClipStarted || bWantWalk != bWalking)
	{
		PlayClip(bWantWalk ? WalkClip.Get() : IdleClip.Get());
		bWalking = bWantWalk;
		bClipStarted = true;
	}
	const float Speed = DeltaSeconds > 0.f ? Moved / DeltaSeconds : 0.f;
	Body->SetPlayRate(bWalking ? FMath::Clamp(Speed / kWalkClipSpeed, 0.5f, 3.f) : 1.f);
}
