#include "FTFieldDummy.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "ForgottenThird.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
constexpr float kJoltTime = 0.25f;
constexpr float kJoltDegrees = 8.f;
} // namespace

AFTFieldDummy::AFTFieldDummy()
{
	PrimaryActorTick.bCanEverTick = true;
	Post = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Post"));
	SetRootComponent(Post);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cylinder.Succeeded())
	{
		Post->SetStaticMesh(Cylinder.Object);
	}
	// The engine cylinder is 100 cm across and tall, centred: a post 80 cm across and 180 cm tall standing on its base.
	Post->SetRelativeScale3D(FVector(0.8, 0.8, 1.8));
	Post->SetCollisionProfileName(TEXT("BlockAll"));
}

void AFTFieldDummy::TakeHit(int32 Amount)
{
	++Hits;
	DamageTaken += Amount;
	Jolt = kJoltTime;
	UE_LOG(LogForgottenThird, Display, TEXT("Field: dummy hit for %d (%d taken, %d hits)."), Amount, DamageTaken, Hits);
}

void AFTFieldDummy::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Jolt <= 0.f)
	{
		return;
	}
	Jolt = FMath::Max(0.f, Jolt - DeltaSeconds);
	const float Share = Jolt / kJoltTime;
	Post->SetRelativeRotation(FRotator(kJoltDegrees * Share * FMath::Sin(Share * 3.f * PI), 0.0, 0.0));
}
