#include "FTCellInstancer.h"

#include "ForgottenThird.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace FTCells
{
int32 CullEnd(EFTCellCull Cull)
{
	switch (Cull)
	{
	case EFTCellCull::Prop:
		return PropCull;
	case EFTCellCull::Dressing:
		return DressingCull;
	case EFTCellCull::Car:
		return CarCull;
	default:
		return 0;
	}
}
} // namespace FTCells

namespace
{
const TCHAR* KindName(EFTCellKind Kind)
{
	switch (Kind)
	{
	case EFTCellKind::Solid:
		return TEXT("Solid");
	case EFTCellKind::Walk:
		return TEXT("Walk");
	case EFTCellKind::NoCollide:
		return TEXT("Dress");
	case EFTCellKind::Hidden:
		return TEXT("Hidden");
	default:
		return TEXT("Piece");
	}
}

// The collision, navigation, visibility and shadow a kind takes, on either an instanced or a plain mesh component.
void SetUpKind(UStaticMeshComponent& Comp, EFTCellKind Kind, bool bCastShadow)
{
	Comp.SetGenerateOverlapEvents(false); // as a static mesh actor's component
	if (Kind == EFTCellKind::NoCollide)
	{
		Comp.SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Comp.SetCanEverAffectNavigation(false);
	}
	else
	{
		Comp.SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	}
	if (Kind == EFTCellKind::Hidden)
	{
		Comp.SetHiddenInGame(true);
	}
	if (!bCastShadow || Kind == EFTCellKind::Hidden)
	{
		Comp.SetCastShadow(false);
	}
}
} // namespace

FFTCellInstancer::FFTCellInstancer(UWorld* InWorld, const FActorSpawnParameters& InSpawn)
	: World(InWorld)
	, Spawn(&InSpawn)
{
}

FIntPoint FFTCellInstancer::CellOf(const FVector& At)
{
	return FIntPoint(FMath::FloorToInt32(At.X / FTCells::CellSize), FMath::FloorToInt32(At.Y / FTCells::CellSize));
}

AActor* FFTCellInstancer::CellAt(const FVector& At)
{
	const FIntPoint Cell = CellOf(At);
	if (AActor** Found = Holders.Find(Cell))
	{
		return *Found;
	}
	AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, *Spawn);
	if (!Holder)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: instancer: couldn't spawn cell %d, %d's holder."), Cell.X, Cell.Y);
		return nullptr;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("Root"));
	Root->CreationMethod = EComponentCreationMethod::Instance;
	Root->SetMobility(EComponentMobility::Static);
	Holder->SetRootComponent(Root);
	Holder->AddInstanceComponent(Root);
	Root->RegisterComponent();
	Holder->SetActorLabel(FString::Printf(TEXT("Cell_%d_%d"), Cell.X, Cell.Y));
	Holders.Add(Cell, Holder);
	++Totals.Cells;
	return Holder;
}

// A project material (under /Game) without the instanced-mesh usage gets it here, and its package is saved by Flush,
// so imported props instance too; an engine or plugin material is left as it is.
bool FFTCellInstancer::AllowInstanced(UMaterialInterface* Material)
{
	if (Material->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes))
	{
		return true;
	}
	UMaterial* Base = Material->GetMaterial();
	if (!Base || !Base->GetPathName().StartsWith(TEXT("/Game/")))
	{
		return false;
	}
	Base->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Base->PostEditChange();
	Base->MarkPackageDirty();
	UsageSaves.Add(Base->GetOutermost());
	return Material->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes);
}

bool FFTCellInstancer::CanInstance(UStaticMesh* Mesh, TArrayView<UMaterialInterface* const> Materials)
{
	const TArray<FStaticMaterial>& Own = Mesh->GetStaticMaterials();
	const int32 Slots = FMath::Max(Own.Num(), Materials.Num());
	bool bAll = true;
	for (int32 S = 0; S < Slots; ++S)
	{
		UMaterialInterface* Material = S < Materials.Num() && Materials[S] ? Materials[S] : (S < Own.Num() ? Own[S].MaterialInterface.Get() : nullptr);
		if (!Material)
		{
			continue; // the engine's default material, which draws instanced
		}
		bool* Known = Instanceable.Find(Material);
		const bool bOk = Known ? *Known : Instanceable.Add(Material, AllowInstanced(Material));
		if (!bOk)
		{
			NotInstanceable.Add(Material->GetPathName());
			bAll = false;
		}
	}
	return bAll;
}

UStaticMeshComponent* FFTCellInstancer::SpawnActor(UStaticMesh* Mesh, TArrayView<UMaterialInterface* const> Materials, const FTransform& Transform,
	EFTCellKind Kind, const FFTCellOptions& Options)
{
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Transform.GetLocation(), Transform.Rotator(), *Spawn);
	if (!Actor)
	{
		return nullptr;
	}
	UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent();
	Comp->SetStaticMesh(Mesh);
	for (int32 S = 0; S < Materials.Num(); ++S)
	{
		if (Materials[S])
		{
			Comp->SetMaterial(S, Materials[S]);
		}
	}
	SetUpKind(*Comp, Kind, Options.bCastShadow);
	if (!Options.Tag.IsNone())
	{
		Actor->Tags.Add(Options.Tag);
	}
	Actor->SetActorScale3D(Transform.GetScale3D());
	if (!Options.Label.IsEmpty())
	{
		Actor->SetActorLabel(Options.Label);
	}
	++Totals.FallbackActors;
	return Comp;
}

FFTCellHandle FFTCellInstancer::Add(UStaticMesh* Mesh, TArrayView<UMaterialInterface* const> Materials, const FTransform& Transform,
	TArrayView<const float> CustomData, EFTCellKind Kind, const FFTCellOptions& Options)
{
	FFTCellHandle Handle;
	if (!Mesh)
	{
		return Handle;
	}
	if (!CanInstance(Mesh, Materials))
	{
		Handle.Component = SpawnActor(Mesh, Materials, Transform, Kind, Options);
		for (int32 D = 0; Handle.Component && D < CustomData.Num(); ++D)
		{
			Handle.Component->SetDefaultCustomPrimitiveDataFloat(D, CustomData[D]);
		}
		return Handle;
	}
	FKey Key;
	Key.Cell = CellOf(Transform.GetLocation());
	Key.Mesh = Mesh;
	Key.Materials.Append(Materials.GetData(), Materials.Num());
	while (Key.Materials.Num() > 0 && !Key.Materials.Last())
	{
		Key.Materials.Pop(EAllowShrinking::No);
	}
	Key.Kind = Kind;
	Key.Cull = Options.Cull;
	Key.bCastShadow = Options.bCastShadow && Kind != EFTCellKind::Hidden;
	Key.Tag = Options.Tag;
	int32* Found = GroupOf.Find(Key);
	const int32 G = Found ? *Found : Groups.Num();
	if (!Found)
	{
		GroupOf.Add(Key, G);
		Groups.AddDefaulted_GetRef().Key = MoveTemp(Key);
	}
	FGroup& Group = Groups[G];
	Handle.Group = G;
	Handle.Index = Group.Transforms.Add(Transform);
	TArray<float>& Data = Group.Data.AddDefaulted_GetRef();
	Data.Append(CustomData.GetData(), CustomData.Num());
	++Totals.PerKind[static_cast<int32>(Kind)];
	return Handle;
}

void FFTCellInstancer::SetCustomData(const FFTCellHandle& Handle, int32 Slot, float Value)
{
	if (Handle.Component)
	{
		Handle.Component->SetDefaultCustomPrimitiveDataFloat(Slot, Value);
		return;
	}
	if (!Groups.IsValidIndex(Handle.Group) || !Groups[Handle.Group].Data.IsValidIndex(Handle.Index) || Slot < 0)
	{
		return;
	}
	FGroup& Group = Groups[Handle.Group];
	if (Handle.Index < Group.Made)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: instancer: custom data set after its instance was made; ignored."));
		return;
	}
	TArray<float>& Data = Group.Data[Handle.Index];
	if (Data.Num() <= Slot)
	{
		Data.SetNumZeroed(Slot + 1);
	}
	Data[Slot] = Value;
}

void FFTCellInstancer::Flush()
{
	for (int32 G = 0; G < Groups.Num(); ++G)
	{
		FGroup& Group = Groups[G];
		const int32 Count = Group.Transforms.Num() - Group.Made;
		if (Count <= 0)
		{
			continue;
		}
		int32 Floats = 0;
		for (int32 I = Group.Made; I < Group.Transforms.Num(); ++I)
		{
			Floats = FMath::Max(Floats, Group.Data[I].Num());
		}
		if (!Group.Component)
		{
			const FKey& Key = Group.Key;
			AActor* Holder = CellAt(FVector((Key.Cell.X + 0.5) * FTCells::CellSize, (Key.Cell.Y + 0.5) * FTCells::CellSize, 0.0));
			if (!Holder)
			{
				continue;
			}
			const FName Name(*FString::Printf(TEXT("%s_%s_%d"), KindName(Key.Kind), *Key.Mesh->GetName(), G));
			UHierarchicalInstancedStaticMeshComponent* Comp = NewObject<UHierarchicalInstancedStaticMeshComponent>(Holder, Name);
			Comp->CreationMethod = EComponentCreationMethod::Instance;
			Comp->SetMobility(EComponentMobility::Static);
			Comp->SetStaticMesh(Key.Mesh);
			for (int32 S = 0; S < Key.Materials.Num(); ++S)
			{
				if (Key.Materials[S])
				{
					Comp->SetMaterial(S, Key.Materials[S]);
				}
			}
			SetUpKind(*Comp, Key.Kind, Key.bCastShadow);
			const int32 CullTo = FTCells::CullEnd(Key.Cull);
			if (CullTo > 0)
			{
				Comp->SetCullDistances(FMath::RoundToInt32(CullTo * FTCells::CullFadeStart), CullTo);
			}
			if (!Key.Tag.IsNone())
			{
				Comp->ComponentTags.Add(Key.Tag);
			}
			// Set before any instance goes in: setting it clears the instances' data.
			Comp->SetNumCustomDataFloats(Floats);
			Comp->bAutoRebuildTreeOnInstanceChanges = false;
			Comp->SetupAttachment(Holder->GetRootComponent());
			Holder->AddInstanceComponent(Comp);
			Comp->RegisterComponent();
			Group.Component = Comp;
			++Totals.Components;
		}
		else if (Floats > Group.Component->NumCustomDataFloats)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: instancer: %s takes %d custom data floats after its first flush; %d kept."),
				*Group.Component->GetName(), Floats, Group.Component->NumCustomDataFloats);
		}
		UHierarchicalInstancedStaticMeshComponent* Comp = Group.Component;
		const TArray<FTransform> Adding(Group.Transforms.GetData() + Group.Made, Count);
		const TArray<int32> Indices = Comp->AddInstances(Adding, true, true);
		const int32 Stride = Comp->NumCustomDataFloats;
		if (Stride > 0)
		{
			TArray<float> Values;
			Values.SetNumZeroed(Stride);
			for (int32 I = 0; I < Count && I < Indices.Num(); ++I)
			{
				const TArray<float>& Data = Group.Data[Group.Made + I];
				for (int32 D = 0; D < Stride; ++D)
				{
					Values[D] = D < Data.Num() ? Data[D] : 0.f;
				}
				Comp->SetCustomData(Indices[I], Values);
			}
		}
		Comp->BuildTreeIfOutdated(false, true);
		Comp->MarkRenderStateDirty();
		Group.Made = Group.Transforms.Num();
		Totals.Instances += Count;
	}
	int32 Saved = 0;
	for (UPackage* Package : UsageSaves)
	{
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Save;
		Save.TopLevelFlags = RF_Public | RF_Standalone;
		Save.SaveFlags = SAVE_NoError;
		Saved += UPackage::SavePackage(Package, nullptr, *File, Save) ? 1 : 0;
	}
	if (!UsageSaves.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: instancer: instanced-mesh usage turned on and saved for %d of %d project materials."),
			Saved, UsageSaves.Num());
		UsageSaves.Reset();
	}
	if (!NotInstanceable.IsEmpty())
	{
		TArray<FString> Names = NotInstanceable.Array();
		Names.Sort();
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: instancer: %d materials aren't flagged for instanced meshes, so their pieces "
			"stay actors (%d): %s."), Names.Num(), Totals.FallbackActors, *FString::Join(Names, TEXT(", ")));
	}
}
