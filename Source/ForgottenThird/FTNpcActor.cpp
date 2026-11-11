#include "FTNpcActor.h"

#include "FTNpcSubsystem.h"

#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
constexpr float kHalfHeight = 90.f;
constexpr float kRadius = 34.f;
} // namespace

AFTNpcActor::AFTNpcActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	Capsule->InitCapsuleSize(kRadius, kHalfHeight);
	Capsule->SetCollisionProfileName(TEXT("Pawn"));
	RootComponent = Capsule;

	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Capsule);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cylinder.Succeeded())
	{
		Body->SetStaticMesh(Cylinder.Object);
		// The 1 m engine cylinder, scaled to the capsule.
		Body->SetRelativeScale3D(FVector(kRadius * 2.f / 100.f, kRadius * 2.f / 100.f, kHalfHeight * 2.f / 100.f));
	}

	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(Capsule);
	Label->SetRelativeLocation(FVector(0.f, 0.f, kHalfHeight + 30.f));
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetWorldSize(18.f);
	Label->SetHiddenInGame(UE_BUILD_SHIPPING != 0);
}

FVector AFTNpcActor::GetEyes() const
{
	return GetActorLocation() + FVector(0.f, 0.f, kHalfHeight * 0.75f);
}

void AFTNpcActor::SetLabel(const FString& Text)
{
	Label->SetText(FText::FromString(Text));
}

void AFTNpcActor::BeginPlay()
{
	Super::BeginPlay();
	SetLabel(NpcId);
	if (UFTNpcSubsystem* Npcs = UFTNpcSubsystem::Get(this))
	{
		Npcs->RegisterNpc(this);
	}
}

void AFTNpcActor::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UFTNpcSubsystem* Npcs = UFTNpcSubsystem::Get(this))
	{
		Npcs->UnregisterNpc(this);
	}
	Super::EndPlay(Reason);
}
