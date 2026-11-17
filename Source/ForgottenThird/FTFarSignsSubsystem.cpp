#include "FTFarSignsSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ForgottenThird.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Parse.h"
#include "ftcore/FarSigns.h"

namespace
{
constexpr int32 kMaxSigns = 40;
constexpr int32 kMaxBlocks = 4096;
constexpr int32 kGlyphBars = 4;
constexpr float kCardDepthM = 0.12f;

// The palette (ftcore::farSignColour): red, pink, cyan.
const FLinearColor kSignColours[ftcore::kFarSignColours] = {
	FLinearColor(1.f, 0.07f, 0.05f), FLinearColor(1.f, 0.18f, 0.5f), FLinearColor(0.12f, 0.8f, 1.f)};

// A pseudo-random share in [0, 1) from two small integers (the glyph bars' widths and offsets).
float Hash01(int32 A, int32 B)
{
	uint32 H = static_cast<uint32>(A) * 73856093u ^ static_cast<uint32>(B) * 19349663u;
	H ^= H >> 13;
	H *= 0x5bd1e995u;
	H ^= H >> 15;
	return static_cast<float>(H & 0xFFFFu) / 65536.f;
}

void AddBlock(const FBox& Box, ftcore::FarSignBlock* Blocks, int32& Count)
{
	if (Count >= kMaxBlocks || !Box.IsValid)
	{
		return;
	}
	ftcore::FarSignBlock& B = Blocks[Count++];
	B.minX = static_cast<float>(Box.Min.X) * 0.01f;
	B.minY = static_cast<float>(Box.Min.Y) * 0.01f;
	B.maxX = static_cast<float>(Box.Max.X) * 0.01f;
	B.maxY = static_cast<float>(Box.Max.Y) * 0.01f;
	B.baseZ = static_cast<float>(Box.Min.Z) * 0.01f;
	B.topZ = static_cast<float>(Box.Max.Z) * 0.01f;
}

// A block worth a card: a footprint at least kMinFootCm on each side (not a pole, lamp or tree) and within reach of the
// centre in plan, so the block cap isn't spent on the rest of the map.
constexpr double kMinFootCm = 500.0;
void AddCandidate(const FBox& Box, const FVector2D& CentreCm, double ReachCm, ftcore::FarSignBlock* Blocks, int32& Count)
{
	if (!Box.IsValid || Box.Max.X - Box.Min.X < kMinFootCm || Box.Max.Y - Box.Min.Y < kMinFootCm)
	{
		return;
	}
	const double Dx = FMath::Max3(Box.Min.X - CentreCm.X, 0.0, CentreCm.X - Box.Max.X);
	const double Dy = FMath::Max3(Box.Min.Y - CentreCm.Y, 0.0, CentreCm.Y - Box.Max.Y);
	if (Dx * Dx + Dy * Dy > ReachCm * ReachCm)
	{
		return;
	}
	AddBlock(Box, Blocks, Count);
}

UStaticMeshComponent* AddBox(AActor& Holder, UStaticMesh* Cube, UMaterialInterface* Material, const FTransform& At)
{
	UStaticMeshComponent* Box = NewObject<UStaticMeshComponent>(&Holder);
	Box->SetMobility(EComponentMobility::Movable);
	Box->SetStaticMesh(Cube);
	Box->SetMaterial(0, Material);
	Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Box->SetCastShadow(false);
	Box->SetupAttachment(Holder.GetRootComponent());
	Box->SetWorldTransform(At);
	Box->RegisterComponent();
	Holder.AddInstanceComponent(Box);
	return Box;
}
} // namespace

bool UFTFarSignsSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	FTDemoBootNoteWorld(Outer); // this world's boot row, not the last one's (see ForgottenThird.h)
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) &&
		FParse::Param(FTLookSwitches(), TEXT("FarSigns")) && Super::ShouldCreateSubsystem(Outer);
}

void UFTFarSignsSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	Placed = Place(InWorld);
	if (Placed == 0)
	{
		LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddWeakLambda(this, [this](ULevel*, UWorld* Into)
		{
			if (Placed == 0 && Into == GetWorld() && Into)
			{
				Placed = Place(*Into);
			}
		});
	}
}

void UFTFarSignsSubsystem::Deinitialize()
{
	FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
	LevelAddedHandle.Reset();
	Super::Deinitialize();
}

int32 UFTFarSignsSubsystem::Place(UWorld& World)
{
	const TCHAR* Switches = FTLookSwitches();
	float CentreX = 404.5f;
	float CentreY = -25.f;
	FString At;
	if (FParse::Value(Switches, TEXT("FarSignsAt="), At, false)) // false: keep the comma (x,y)
	{
		FString Xs, Ys;
		if (At.Split(TEXT(","), &Xs, &Ys))
		{
			CentreX = FCString::Atof(*Xs);
			CentreY = FCString::Atof(*Ys);
		}
	}
	int32 Want = 30;
	FParse::Value(Switches, TEXT("FarSignsCount="), Want);
	Want = FMath::Clamp(Want, 1, kMaxSigns);
	ftcore::FarSignSpec Spec;
	Spec.farM = 140.f;
	FParse::Value(Switches, TEXT("FarSignsNear="), Spec.nearM);
	FParse::Value(Switches, TEXT("FarSignsFar="), Spec.farM);
	float Strength = 25.f;
	FParse::Value(Switches, TEXT("FarSignsLum="), Strength);

	// The plan boxes of the tall static meshes: a plain component's bounds, an instanced one's per instance. Meshes
	// shorter than the spec's least height are passed over before their instances are walked.
	TArray<ftcore::FarSignBlock> Blocks;
	Blocks.SetNumUninitialized(kMaxBlocks);
	int32 BlockCount = 0;
	int32 MeshesSeen = 0;
	const float MinTallCm = Spec.minHeightM * 100.f;
	const FVector2D CentreCm(CentreX * 100.f, CentreY * 100.f);
	const double ReachCm = Spec.farM * 100.0;
	for (TActorIterator<AActor> It(&World); It && BlockCount < kMaxBlocks; ++It)
	{
		const AActor* Actor = *It;
		if (!Actor || Actor->IsA<APawn>() || Actor->ActorHasTag(SignTag))
		{
			continue;
		}
		for (const UActorComponent* Comp : Actor->GetComponents())
		{
			const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Comp);
			if (!Mesh || !Mesh->GetStaticMesh() || !Mesh->IsRegistered())
			{
				continue;
			}
			++MeshesSeen;
			if (const UInstancedStaticMeshComponent* Ism = Cast<UInstancedStaticMeshComponent>(Mesh))
			{
				const FBox Local = Mesh->GetStaticMesh()->GetBoundingBox();
				const int32 Instances = Ism->GetInstanceCount();
				for (int32 I = 0; I < Instances && BlockCount < kMaxBlocks; ++I)
				{
					FTransform T;
					if (!Ism->GetInstanceTransform(I, T, true))
					{
						continue;
					}
					if (Local.GetSize().Z * FMath::Abs(T.GetScale3D().Z) >= MinTallCm)
					{
						AddCandidate(Local.TransformBy(T), CentreCm, ReachCm, Blocks.GetData(), BlockCount);
					}
				}
			}
			else if (Mesh->Bounds.BoxExtent.Z * 2.0 >= MinTallCm)
			{
				AddCandidate(Mesh->Bounds.GetBox(), CentreCm, ReachCm, Blocks.GetData(), BlockCount);
			}
		}
	}

	ftcore::FarSign Signs[kMaxSigns];
	const int32 Count = static_cast<int32>(ftcore::placeFarSigns(Blocks.GetData(), static_cast<std::size_t>(BlockCount), CentreX,
		CentreY, Spec, Signs, static_cast<std::size_t>(Want)));
	if (Count == 0)
	{
		// Where things are, to set -FarSignsAt: the player start and the tallest static mesh's box.
		FBox Tallest(ForceInit);
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			for (const UActorComponent* Comp : It->GetComponents())
			{
				const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Comp);
				if (Mesh && Mesh->IsRegistered() && Mesh->Bounds.BoxExtent.X < 4000.0 && Mesh->Bounds.BoxExtent.Y < 4000.0 &&
					(!Tallest.IsValid || Mesh->Bounds.BoxExtent.Z > Tallest.GetExtent().Z))
				{
					Tallest = Mesh->Bounds.GetBox();
				}
			}
			if (It->GetName().StartsWith(TEXT("PlayerStart")))
			{
				UE_LOG(LogForgottenThird, Display, TEXT("FTFarSigns: player start at %s (cm)."), *It->GetActorLocation().ToString());
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTFarSigns: tallest small-footprint box %s (cm)."), *Tallest.ToString());
		UE_LOG(LogForgottenThird, Display, TEXT("FTFarSigns: none placed (%d tall boxes from %d static meshes round %.1f,%.1f)."),
			BlockCount, MeshesSeen, CentreX, CentreY);
		return 0;
	}

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* SignMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Look/M_Look_Sign.M_Look_Sign"));
	if (!Cube || !SignMaterial)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTFarSigns: the cube or /Game/Look/M_Look_Sign is missing; no cards."));
		return 0;
	}
	UMaterialInstanceDynamic* Lit[ftcore::kFarSignColours] = {};
	for (int32 C = 0; C < ftcore::kFarSignColours; ++C)
	{
		Lit[C] = UMaterialInstanceDynamic::Create(SignMaterial, this);
		Lit[C]->SetVectorParameterValue(TEXT("SignColor"), kSignColours[C]);
		Lit[C]->SetScalarParameterValue(TEXT("SignStrength"), Strength);
	}
	UMaterialInstanceDynamic* Dark = UMaterialInstanceDynamic::Create(SignMaterial, this);
	Dark->SetVectorParameterValue(TEXT("SignColor"), FLinearColor(0.02f, 0.02f, 0.025f));
	Dark->SetScalarParameterValue(TEXT("SignStrength"), 0.f);

	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transient;
	AActor* Holder = World.SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	if (!Holder)
	{
		return 0;
	}
	Holder->Tags.Add(SignTag);
	USceneComponent* Root = NewObject<USceneComponent>(Holder);
	Root->SetMobility(EComponentMobility::Movable);
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();
	Holder->AddInstanceComponent(Root);

	int32 PerColour[ftcore::kFarSignColours] = {};
	for (int32 S = 0; S < Count; ++S)
	{
		const ftcore::FarSign& Sign = Signs[S];
		const FQuat Turn(FRotator(0.0, Sign.yawDeg, 0.0));
		const FVector Centre(Sign.x * 100.f, Sign.y * 100.f, Sign.z * 100.f);
		// The cube is a metre on a side: the card is kCardDepthM deep along its face's normal (local X).
		AddBox(*Holder, Cube, Lit[FMath::Clamp(Sign.colour, 0, ftcore::kFarSignColours - 1)],
			FTransform(Turn, Centre, FVector(kCardDepthM, Sign.widthM, Sign.heightM)));
		++PerColour[FMath::Clamp(Sign.colour, 0, ftcore::kFarSignColours - 1)];
		// Glyph marks: dark bars of varied width standing a little proud of the face, spaced down the card.
		const FVector Out = Turn.GetForwardVector();
		const FVector Side = Turn.GetRightVector();
		for (int32 G = 0; G < kGlyphBars; ++G)
		{
			const float Width = Sign.widthM * (0.3f + 0.45f * Hash01(S, G));
			const float Shift = (Sign.widthM - Width) * (Hash01(S + 101, G) - 0.5f) * 0.8f;
			const float Height = FMath::Max(0.06f, Sign.heightM * 0.045f);
			const float Down = Sign.heightM * (0.35f - 0.2f * static_cast<float>(G));
			const FVector BarAt = Centre + Out * ((0.5f * kCardDepthM + 0.01f) * 100.f) + Side * (Shift * 100.f) +
				FVector(0.0, 0.0, Down * 100.f);
			AddBox(*Holder, Cube, Dark, FTransform(Turn, BarAt, FVector(0.02f, Width, Height)));
		}
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTFarSigns: %d cards (red %d, pink %d, cyan %d) from %d tall boxes round %.1f,%.1f m, strength %.1f."), Count,
		PerColour[0], PerColour[1], PerColour[2], BlockCount, CentreX, CentreY, Strength);
	return Count;
}
