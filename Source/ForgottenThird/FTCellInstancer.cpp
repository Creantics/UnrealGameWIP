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
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/MassingPlan.h"

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
	case EFTCellCull::Interior:
		return InteriorCull;
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
	double CellM = 0.0;
	if (FParse::Value(FCommandLine::Get(), TEXT("CellM="), CellM) && CellM >= 50.0)
	{
		CellSize = CellM * 100.0;
	}
}

void FFTCellInstancer::SetGround(const ftcore::MassingGround& InGround, double InSide)
{
	Ground = InGround;
	Side = InSide;
}

FIntPoint FFTCellInstancer::CellOf(const FVector& At) const
{
	return FIntPoint(FMath::FloorToInt32(At.X / CellSize), FMath::FloorToInt32(At.Y / CellSize));
}

bool FFTCellInstancer::GroundFlush(const UStaticMesh& Mesh, const FTransform& Transform) const
{
	if (!Ground.grid || Ground.samplesX < 2 || Ground.samplesY < 2)
	{
		return false;
	}
	const FBox Bounds = Mesh.GetBoundingBox();
	const FVector Size = Bounds.GetSize();
	const FVector Scale = Transform.GetScale3D().GetAbs();
	if (Size.Z * Scale.Z > FTCells::FlushThick)
	{
		return false;
	}
	const FQuat Turn = Transform.GetRotation();
	if (Turn.GetUpVector().Z < FTCells::FlushUpright)
	{
		return false;
	}
	// The top face: its middle, and its two half sides in the world.
	const FVector Middle = Bounds.GetCenter();
	const FVector Top = Transform.TransformPosition(FVector(Middle.X, Middle.Y, Bounds.Max.Z));
	const FVector HalfX = Turn.GetForwardVector() * (0.5 * Size.X * Scale.X);
	const FVector HalfY = Turn.GetRightVector() * (0.5 * Size.Y * Scale.Y);
	const int32 CountX = FMath::Clamp(FMath::CeilToInt32(Size.X * Scale.X / FTCells::FlushSampleGap) + 1, 2, 9);
	const int32 CountY = FMath::Clamp(FMath::CeilToInt32(Size.Y * Scale.Y / FTCells::FlushSampleGap) + 1, 2, 9);
	for (int32 IX = 0; IX < CountX; ++IX)
	{
		for (int32 IY = 0; IY < CountY; ++IY)
		{
			const FVector At = Top + HalfX * (2.0 * IX / (CountX - 1) - 1.0) + HalfY * (2.0 * IY / (CountY - 1) - 1.0);
			// The engine's point back in the plan (ftcore::massingEnginePoint's inverse).
			const double GroundZ = ftcore::massingGroundAt(Ground.grid, Ground.samplesX, Ground.samplesY, Ground.spacing, Ground.originX,
				Ground.originY, At.X / 100.0 + Side * 0.5, Side * 0.5 - At.Y / 100.0);
			if (At.Z - GroundZ * 100.0 > FTCells::FlushRise)
			{
				return false;
			}
		}
	}
	return true;
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
	if (!Base->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes))
	{
		Base->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
		Base->PostEditChange();
		Base->MarkPackageDirty();
		UsageSaves.Add(Base->GetOutermost());
	}
	// A material instance keeps a copy of its base's usage flags, refreshed only on load; refresh the chain from the
	// instance nearest the base outward so this run sees the new usage (the instances themselves need no save).
	TArray<UMaterialInstance*, TInlineAllocator<4>> Chain;
	for (UMaterialInstance* Inst = Cast<UMaterialInstance>(Material); Inst; Inst = Cast<UMaterialInstance>(Inst->Parent))
	{
		Chain.Add(Inst);
	}
	for (int32 I = Chain.Num() - 1; I >= 0; --I)
	{
		Chain[I]->UpdateOverridableBaseProperties();
	}
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
	// Solid and Walk collide alike (SetUpKind): one group for both.
	Key.Kind = Kind == EFTCellKind::Walk ? EFTCellKind::Solid : Kind;
	Key.Cull = Options.Cull;
	Key.bCastShadow = Options.bCastShadow && Kind != EFTCellKind::Hidden;
	if (Key.bCastShadow && Kind == EFTCellKind::Walk && GroundFlush(*Mesh, Transform))
	{
		Key.bCastShadow = false;
		++Totals.FlushNoShadow;
	}
	int32* Found = GroupOf.Find(Key);
	const int32 G = Found ? *Found : Groups.Num();
	if (!Found)
	{
		GroupOf.Add(Key, G);
		Groups.AddDefaulted_GetRef().Key = MoveTemp(Key);
	}
	FGroup& Group = Groups[G];
	if (!Options.Tag.IsNone())
	{
		Group.Tags.AddUnique(Options.Tag);
	}
	Unmerged.Add(MakeTuple(G, static_cast<uint8>(Kind), Options.Tag));
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
			AActor* Holder = CellAt(FVector((Key.Cell.X + 0.5) * CellSize, (Key.Cell.Y + 0.5) * CellSize, 0.0));
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
			// Small dressing: nothing a ray, a distance field or the bounce lighting needs to know of.
			if (Key.Cull == EFTCellCull::Dressing || Key.Cull == EFTCellCull::Interior ||
				(Key.Kind == EFTCellKind::NoCollide && !Key.bCastShadow))
			{
				Comp->SetVisibleInRayTracing(false);
				Comp->SetAffectDistanceFieldLighting(false);
				Comp->SetAffectDynamicIndirectLighting(false);
				++Totals.LightComponents;
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
		for (const FName& Tag : Group.Tags)
		{
			Comp->ComponentTags.AddUnique(Tag);
		}
		if (!Comp->bVisibleInRayTracing)
		{
			Totals.LightInstances += Count;
		}
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
	Totals.UnmergedGroups = Unmerged.Num();
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: instancer: %.0f m cells; %d components (%d before Solid/Walk and tags shared "
		"groups); %d light components of %d instances (no ray tracing, distance field or indirect lighting scene); %d ground-flush walked "
		"pieces cast no shadow."), CellSize / 100.0, Totals.Components, Totals.UnmergedGroups, Totals.LightComponents, Totals.LightInstances,
		Totals.FlushNoShadow);
	// The census per cell: every instance and component made so far, one line a cell, west to east, then north to south.
	{
		TMap<FIntPoint, FIntPoint> PerCell; // X instances, Y components
		for (const FGroup& Group : Groups)
		{
			if (!Group.Component)
			{
				continue;
			}
			FIntPoint& Count = PerCell.FindOrAdd(Group.Key.Cell, FIntPoint::ZeroValue);
			Count.X += Group.Made;
			Count.Y += 1;
		}
		TArray<FIntPoint> CellIds;
		PerCell.GetKeys(CellIds);
		CellIds.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });
		for (const FIntPoint& Cell : CellIds)
		{
			const FIntPoint& Count = PerCell[Cell];
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: instancer: Cell_%d_%d: %d instances, %d components."), Cell.X, Cell.Y, Count.X,
				Count.Y);
		}
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
