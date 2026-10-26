#include "FTFieldGuard.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
const TCHAR* const kGuardCube = TEXT("/Engine/BasicShapes/Cube.Cube");
const TCHAR* const kGuardCylinder = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
const TCHAR* const kGuardSphere = TEXT("/Engine/BasicShapes/Sphere.Sphere");
// Has a "Color" vector parameter.
const TCHAR* const kGuardShapeMaterial = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

// The figure: a 60 cm wide, 180 cm tall column standing on the actor's origin.
constexpr double kHeight = 180.0;
constexpr double kEyeHeight = 160.0;

const FLinearColor kCalmColor(0.36f, 0.16f, 0.14f);
const FLinearColor kNoticeColor(0.85f, 0.6f, 0.15f);
const FLinearColor kChaseColor(0.8f, 0.1f, 0.08f);
const FLinearColor kVisorColor(0.9f, 0.85f, 0.7f);

UStaticMeshComponent* MakePart(AActor& Owner, const TCHAR* Name, const FVector& At, const FVector& Scale)
{
	UStaticMeshComponent* Part = Owner.CreateDefaultSubobject<UStaticMeshComponent>(Name);
	Part->SetRelativeLocation(At);
	Part->SetRelativeScale3D(Scale);
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetGenerateOverlapEvents(false);
	return Part;
}

// Gives the part its mesh and a tint of its own (at run time, not in the constructor).
UMaterialInstanceDynamic* Dress(AActor& Owner, UStaticMeshComponent* Part, const TCHAR* MeshPath, const FLinearColor& Color)
{
	if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, MeshPath))
	{
		Part->SetStaticMesh(Mesh);
	}
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, kGuardShapeMaterial);
	if (!Base)
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, &Owner);
	Mid->SetVectorParameterValue(TEXT("Color"), Color);
	Part->SetMaterial(0, Mid);
	return Mid;
}

const FLinearColor& StateColor(ftcore::GuardState State)
{
	switch (State)
	{
	case ftcore::GuardState::Notice:
		return kNoticeColor;
	case ftcore::GuardState::Chase:
		return kChaseColor;
	default:
		return kCalmColor;
	}
}
}

AFTFieldGuard::AFTFieldGuard()
{
	PrimaryActorTick.bCanEverTick = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	// The basic shapes are 100 cm and centred on their origin.
	Body = MakePart(*this, TEXT("Body"), FVector(0.0, 0.0, kHeight * 0.5), FVector(0.6, 0.6, kHeight / 100.0));
	Visor = MakePart(*this, TEXT("Visor"), FVector(28.0, 0.0, kEyeHeight), FVector(0.12, 0.4, 0.12));
	Alert = MakePart(*this, TEXT("Alert"), FVector(0.0, 0.0, kHeight + 35.0), FVector(0.25, 0.25, 0.25));
	Body->SetupAttachment(RootComponent);
	Visor->SetupAttachment(RootComponent);
	Alert->SetupAttachment(RootComponent);
	SetActorEnableCollision(false);
	GroundParams = FCollisionQueryParams(FName(TEXT("FTFieldGuardGround")), false);
	SightParams = FCollisionQueryParams(FName(TEXT("FTFieldGuardSight")), false);
}

void AFTFieldGuard::Setup(const AActor* Player)
{
	GroundParams.AddIgnoredActor(this);
	SightParams.AddIgnoredActor(this);
	if (Player)
	{
		GroundParams.AddIgnoredActor(Player);
	}
	BodyTint = Dress(*this, Body, kGuardCylinder, kCalmColor);
	Dress(*this, Visor, kGuardCube, kVisorColor);
	Dress(*this, Alert, kGuardSphere, kChaseColor);
	Alert->SetHiddenInGame(true);
}

void AFTFieldGuard::Place(const FVector2D& Ground, const FVector2D& Facing, ftcore::GuardState State, float FeetZ)
{
	double Z = FeetZ;
	if (const UWorld* World = GetWorld())
	{
		FHitResult Hit;
		const FVector From(Ground.X, Ground.Y, FeetZ + 150.0);
		const FVector To(Ground.X, Ground.Y, FeetZ - 300.0);
		if (World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, GroundParams))
		{
			Z = Hit.ImpactPoint.Z;
		}
	}
	const FRotator Rot(0.0, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0.0);
	SetActorLocationAndRotation(FVector(Ground.X, Ground.Y, Z), Rot, false, nullptr, ETeleportType::TeleportPhysics);
	if (!bTinted || State != Shown)
	{
		if (BodyTint)
		{
			BodyTint->SetVectorParameterValue(TEXT("Color"), StateColor(State));
		}
		const bool bAlert = State == ftcore::GuardState::Notice || State == ftcore::GuardState::Chase;
		Alert->SetHiddenInGame(!bAlert);
		Shown = State;
		bTinted = true;
	}
}

bool AFTFieldGuard::CanSee(const AActor& Player) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const FVector Eye = GetActorLocation() + FVector(0.0, 0.0, kEyeHeight);
	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, Eye, Player.GetActorLocation(), ECC_Visibility, SightParams))
	{
		return true;
	}
	return Hit.GetActor() == &Player;
}
