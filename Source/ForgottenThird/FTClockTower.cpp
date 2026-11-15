#include "FTClockTower.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FTWorldClockSubsystem.h"
#include "Materials/MaterialInterface.h"

#include "ftcore/WorldClock.h"

AFTClockTower::AFTClockTower()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.TickInterval = 1.f;

	TowerMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Tower"));
	TowerMesh->SetMobility(EComponentMobility::Static);
	RootComponent = TowerMesh;
	static const TCHAR* const HourNames[FaceCount] = {TEXT("Hour0"), TEXT("Hour1"), TEXT("Hour2"), TEXT("Hour3")};
	static const TCHAR* const MinuteNames[FaceCount] = {TEXT("Minute0"), TEXT("Minute1"), TEXT("Minute2"), TEXT("Minute3")};
	for (int32 F = 0; F < FaceCount; ++F)
	{
		for (int32 Kind = 0; Kind < 2; ++Kind)
		{
			UStaticMeshComponent* Hand = CreateDefaultSubobject<UStaticMeshComponent>(Kind == 0 ? HourNames[F] : MinuteNames[F]);
			Hand->SetupAttachment(TowerMesh);
			Hand->SetMobility(EComponentMobility::Movable);
			Hand->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Hand->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
			Hand->SetCanEverAffectNavigation(false);
			Hand->SetGenerateOverlapEvents(false);
			(Kind == 0 ? HourHands : MinuteHands).Add(Hand);
		}
	}
	FaceYaw = {0.f, -90.f, 180.f, 90.f};
}

void AFTClockTower::Setup(UStaticMesh* Tower, UStaticMesh* HourHand, UStaticMesh* MinuteHand, double DialHeight, double DialOut,
	UMaterialInterface* Glow, const TArray<FName>& GlowSlots)
{
	TowerMesh->SetStaticMesh(Tower);
	if (Glow && Tower)
	{
		for (const FName& Slot : GlowSlots)
		{
			const int32 Index = TowerMesh->GetMaterialIndex(Slot);
			if (Index != INDEX_NONE)
			{
				TowerMesh->SetMaterial(Index, Glow);
			}
		}
	}
	for (int32 F = 0; F < FaceCount; ++F)
	{
		// The dial's pivot: +Y turned by the face's yaw.
		const FVector Pivot = FRotator(0.0, FaceYaw[F], 0.0).RotateVector(FVector(0.0, DialOut, DialHeight));
		HourHands[F]->SetStaticMesh(HourHand);
		MinuteHands[F]->SetStaticMesh(MinuteHand);
		HourHands[F]->SetRelativeLocation(Pivot);
		MinuteHands[F]->SetRelativeLocation(Pivot);
	}
	ShownMinute = -1.0;
	ShowHours(ftcore::kWorldClockStartHours);
}

void AFTClockTower::ShowHours(double Hours)
{
	const ftcore::ClockHands Hands = ftcore::clockHands(Hours);
	for (int32 F = 0; F < FaceCount; ++F)
	{
		// A hand points up its +Z with its face toward +Y; seen from +Y, clockwise carries +Z toward +X, a negative pitch.
		// The face's yaw then turns it onto its side of the tower.
		HourHands[F]->SetRelativeRotation(FRotator(-Hands.hourDeg, FaceYaw[F], 0.0));
		MinuteHands[F]->SetRelativeRotation(FRotator(-Hands.minuteDeg, FaceYaw[F], 0.0));
	}
}

void AFTClockTower::BeginPlay()
{
	Super::BeginPlay();
	ShownMinute = -1.0;
	Tick(0.f);
}

void AFTClockTower::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const UWorld* World = GetWorld();
	const UFTWorldClockSubsystem* Clock = World ? World->GetSubsystem<UFTWorldClockSubsystem>() : nullptr;
	const double Hours = Clock ? static_cast<double>(Clock->GetHours()) : ftcore::kWorldClockStartHours;
	const double Minute = FMath::FloorToDouble(Hours * 60.0);
	if (Minute != ShownMinute)
	{
		ShownMinute = Minute;
		ShowHours(Hours);
	}
}
