#include "FTExploreSpot.h"

#include "Animation/AnimSequence.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include "FTMeshFacing.h"

#include "ftcore/ExploreDemo.h"

namespace
{
const TCHAR* const kCube = TEXT("/Engine/BasicShapes/Cube.Cube");
const TCHAR* const kSphere = TEXT("/Engine/BasicShapes/Sphere.Sphere");
const TCHAR* const kCylinder = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
const TCHAR* const kCone = TEXT("/Engine/BasicShapes/Cone.Cone");
// Has a "Color" vector parameter.
const TCHAR* const kShapeMaterial = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

constexpr float kGateSinkSeconds = 1.5f;
constexpr float kPickupFadeSeconds = 0.4f;

const FLinearColor kShardColor(0.55f, 0.85f, 1.0f);
const FLinearColor kGoalColor(1.0f, 0.8f, 0.25f);
const FLinearColor kGateColor(0.42f, 0.36f, 0.3f);
const FLinearColor kBarColor(0.62f, 0.5f, 0.28f);
const FLinearColor kBruteColor(0.48f, 0.33f, 0.25f);
const FLinearColor kShadeColor(0.22f, 0.42f, 0.44f);

template <typename T>
T* Load(const TCHAR* Path)
{
	return LoadObject<T>(nullptr, Path);
}

void Place(UStaticMeshComponent* Part, const TCHAR* MeshPath, const FVector& At, const FVector& Scale, bool bCollide)
{
	Part->SetStaticMesh(Load<UStaticMesh>(MeshPath));
	Part->SetRelativeLocation(At);
	Part->SetRelativeScale3D(Scale);
	Part->SetVisibility(true);
	Part->SetHiddenInGame(false);
	Part->SetCollisionEnabled(bCollide ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
}

void Hide(UStaticMeshComponent* Part)
{
	Part->SetStaticMesh(nullptr);
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}
}

AFTExploreSpot::AFTExploreSpot()
{
	PrimaryActorTick.bCanEverTick = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	Shape = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Shape"));
	Shape->SetupAttachment(Root);
	PartA = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PartA"));
	PartA->SetupAttachment(Root);
	PartB = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PartB"));
	PartB->SetupAttachment(Root);
	Skin = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Skin"));
	Skin->SetupAttachment(Root);
	Skin->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Glow = CreateDefaultSubobject<UPointLightComponent>(TEXT("Glow"));
	Glow->SetupAttachment(Root);
	Glow->SetVisibility(false);
	Glow->SetCastShadows(false);
}

void AFTExploreSpot::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Configure();
}

void AFTExploreSpot::BeginPlay()
{
	Super::BeginPlay();
	Configure();
}

UMaterialInstanceDynamic* AFTExploreSpot::Tint(UStaticMeshComponent* Part, const FLinearColor& Color)
{
	UMaterialInterface* Base = Load<UMaterialInterface>(kShapeMaterial);
	if (!Base)
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
	Mid->SetVectorParameterValue(TEXT("Color"), Color);
	Part->SetMaterial(0, Mid);
	return Mid;
}

void AFTExploreSpot::Configure()
{
	if (SpotId == ftcore::demo::kFieldGoalSpotId)
	{
		// The story field step's goal marker: a glowing column with a bobbing orb and a flat ring.
		Hide(Shape);
		Hide(PartA);
		Hide(PartB);
		Skin->SetSkeletalMesh(nullptr);
		Place(PartA, kCylinder, FVector(0, 0, 3), FVector(2.4, 2.4, 0.04), false);
		Tint(PartA, kGoalColor * 0.5f);
		Place(PartB, kCylinder, FVector(0, 0, 110), FVector(0.12, 0.12, 2.2), false);
		Tint(PartB, kGoalColor * 0.6f);
		Place(Shape, kSphere, FVector(0, 0, 240), FVector(0.4), false);
		ShapeMaterial = Tint(Shape, kGoalColor);
		Glow->SetRelativeLocation(FVector(0, 0, 200));
		Glow->SetLightColor(kGoalColor);
		Glow->SetIntensity(5000.f);
		Glow->SetAttenuationRadius(600.f);
		Glow->SetVisibility(true);
		BaseColor = kGoalColor;
		return;
	}
	if (SpotId < 0 || SpotId >= static_cast<int32>(ftcore::demo::kExploreSpots.size()))
	{
		return;
	}
	const ftcore::InteractKind Kind = ftcore::demo::kExploreSpots[static_cast<size_t>(SpotId)].kind;
	Hide(Shape);
	Hide(PartA);
	Hide(PartB);
	Skin->SetSkeletalMesh(nullptr);
	Glow->SetVisibility(false);
	ShapeMaterial = nullptr;

	switch (Kind)
	{
	case ftcore::InteractKind::Talk:
	{
		// Duy, facing back toward the start (-X). The capsule-sized cylinder blocks but isn't drawn.
		USkeletalMesh* Duy = Load<USkeletalMesh>(TEXT("/Game/Characters/Duy/duy/SkeletalMeshes/duy.duy"));
		if (Duy)
		{
			Skin->SetSkeletalMesh(Duy);
			Skin->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Duy) + 180.0, 0.0));
			if (UAnimSequence* Idle = Load<UAnimSequence>(TEXT("/Game/Characters/Duy/duy/SkeletalMeshes/duyidle.duyidle")))
			{
				Skin->SetAnimationMode(EAnimationMode::AnimationSingleNode);
				Skin->AnimationData.AnimToPlay = Idle;
				Skin->AnimationData.bSavedLooping = true;
				Skin->AnimationData.bSavedPlaying = true;
				if (GetWorld() && GetWorld()->IsGameWorld())
				{
					Skin->PlayAnimation(Idle, true);
				}
			}
		}
		Place(Shape, kCylinder, FVector(0, 0, 90), FVector(0.7, 0.7, 1.8), true);
		Shape->SetVisibility(false);
		BaseColor = FLinearColor::White;
		break;
	}
	case ftcore::InteractKind::Pickup:
		Place(Shape, kSphere, FVector(0, 0, 100), FVector(0.32), false);
		ShapeMaterial = Tint(Shape, kShardColor);
		Place(PartA, kCone, FVector(0, 0, 4), FVector(0.4, 0.4, 0.08), false); // a small plinth glow
		Tint(PartA, kShardColor * 0.4f);
		Glow->SetRelativeLocation(FVector(0, 0, 100));
		Glow->SetLightColor(kShardColor);
		Glow->SetIntensity(2500.f);
		Glow->SetAttenuationRadius(350.f);
		Glow->SetVisibility(true);
		BaseColor = kShardColor;
		break;
	case ftcore::InteractKind::Gate:
		// The gap is 4 m wide along Y; the pillars sit just outside it.
		Place(PartA, kCube, FVector(0, -250, 225), FVector(1.0, 1.0, 4.5), true);
		Place(PartB, kCube, FVector(0, 250, 225), FVector(1.0, 1.0, 4.5), true);
		Tint(PartA, kGateColor);
		Tint(PartB, kGateColor);
		Place(Shape, kCube, FVector(0, 0, 150), FVector(0.4, 4.0, 3.0), true);
		ShapeMaterial = Tint(Shape, kBarColor);
		BaseColor = kBarColor;
		break;
	case ftcore::InteractKind::Encounter:
		Place(PartA, kCube, FVector(400, -150, 75), FVector(1.5), false);
		Place(PartB, kCube, FVector(380, 170, 55), FVector(1.1), false);
		Tint(PartA, kBruteColor);
		Tint(PartB, kShadeColor);
		BaseColor = FLinearColor::White;
		break;
	}
}

void AFTExploreSpot::MarkUsed(bool bInstant)
{
	if (bUsed)
	{
		return;
	}
	bUsed = true;
	UsedTime = bInstant ? -100.0 : static_cast<double>(Age);
	SetFocused(false);
	if (SpotId < 0 || SpotId >= static_cast<int32>(ftcore::demo::kExploreSpots.size()))
	{
		return;
	}
	const ftcore::InteractKind Kind = ftcore::demo::kExploreSpots[static_cast<size_t>(SpotId)].kind;
	if (Kind == ftcore::InteractKind::Gate)
	{
		// Open at once for walking; the bar's sinking is only for show.
		Shape->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (bInstant)
	{
		if (Kind == ftcore::InteractKind::Pickup || Kind == ftcore::InteractKind::Encounter)
		{
			Hide(Shape);
			Hide(PartA);
			Hide(PartB);
			Glow->SetVisibility(false);
		}
		else if (Kind == ftcore::InteractKind::Gate)
		{
			Shape->SetRelativeLocation(FVector(0, 0, -160));
		}
	}
	else if (Kind == ftcore::InteractKind::Encounter)
	{
		Hide(PartA);
		Hide(PartB);
	}
}

void AFTExploreSpot::SetFocused(bool bInFocus)
{
	if (bFocused == bInFocus)
	{
		return;
	}
	bFocused = bInFocus;
	if (ShapeMaterial)
	{
		ShapeMaterial->SetVectorParameterValue(TEXT("Color"), bFocused ? FLinearColor::LerpUsingHSV(BaseColor, FLinearColor::White, 0.5f) : BaseColor);
	}
	Skin->SetRenderCustomDepth(bFocused);
}

void AFTExploreSpot::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;
	if (SpotId == ftcore::demo::kFieldGoalSpotId)
	{
		if (Shape->GetStaticMesh())
		{
			Shape->SetRelativeLocation(FVector(0, 0, 240.f + 14.f * FMath::Sin(Age * 2.6f)));
		}
		return;
	}
	if (SpotId < 0 || SpotId >= static_cast<int32>(ftcore::demo::kExploreSpots.size()))
	{
		return;
	}
	const ftcore::InteractKind Kind = ftcore::demo::kExploreSpots[static_cast<size_t>(SpotId)].kind;
	const float SinceUsed = bUsed ? static_cast<float>(Age - UsedTime) : -1.f;
	if (Kind == ftcore::InteractKind::Pickup && Shape->GetStaticMesh())
	{
		const float Bob = 12.f * FMath::Sin(Age * 2.2f);
		Shape->SetRelativeLocation(FVector(0, 0, 100.f + Bob));
		Shape->SetRelativeRotation(FRotator(0.0, Age * 90.0, 0.0));
		if (bUsed)
		{
			// Rise, shrink and go out.
			const float T = FMath::Clamp(SinceUsed / kPickupFadeSeconds, 0.f, 1.f);
			Shape->SetRelativeLocation(FVector(0, 0, 100.f + Bob + 80.f * T));
			Shape->SetRelativeScale3D(FVector(0.32f * (1.f - T)));
			Glow->SetIntensity(2500.f * (1.f - T));
			if (T >= 1.f)
			{
				Hide(Shape);
				Hide(PartA);
				Glow->SetVisibility(false);
			}
		}
	}
	else if (Kind == ftcore::InteractKind::Gate && bUsed && SinceUsed >= 0.f)
	{
		const float T = FMath::Clamp(SinceUsed / kGateSinkSeconds, 0.f, 1.f);
		const float Ease = 1.f - (1.f - T) * (1.f - T);
		Shape->SetRelativeLocation(FVector(0, 0, FMath::Lerp(150.f, -160.f, Ease)));
	}
	else if (Kind == ftcore::InteractKind::Encounter && !bUsed && PartA->GetStaticMesh())
	{
		// The waiting enemies bob, like the battle demo's active figure.
		PartA->SetRelativeLocation(FVector(400, -150, 75.f + 8.f * FMath::Abs(FMath::Sin(Age * 3.f))));
		PartB->SetRelativeLocation(FVector(380, 170, 55.f + 8.f * FMath::Abs(FMath::Sin(Age * 3.f + 1.3f))));
	}
}
