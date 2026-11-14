#include "FTLumenCardsReportCommandlet.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DistanceFieldAtlas.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "MeshCardBuild.h"
#include "MeshCardRepresentation.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#include "FTSceneLook.h"
#include "ForgottenThird.h"

namespace
{
struct FMeshRow
{
	UStaticMesh* Mesh = nullptr;
	FString Family;
	int32 Components = 0;
	int64 InstancesA = 0, InstancesB = 0, InstancesC = 0;
	bool bSample = false;
	FMatrix SampleToWorld = FMatrix::Identity;
	// Read after the mesh compiled, then after the card and distance-field queues finished.
	bool bCardsAtLoad = false, bDfAtLoad = false;
	bool bCards = false, bDf = false;
	int32 Cards = 0;
	FBox3f CardBounds = FBox3f(ForceInit);
};

FString FamilyOf(const UStaticMesh& Mesh)
{
	const FString Path = Mesh.GetPathName();
	if (Path.StartsWith(TEXT("/Engine/")))
	{
		return TEXT("engine");
	}
	if (Path.StartsWith(TEXT("/Game/Maps/")))
	{
		return TEXT("map-built");
	}
	if (Path.StartsWith(TEXT("/Game/Props/PLANT-")) || Path.StartsWith(TEXT("/Game/Props/TREE-")))
	{
		return TEXT("plant");
	}
	if (Path.StartsWith(TEXT("/Game/Props/TQ-")))
	{
		return TEXT("imported-prop");
	}
	if (Path.StartsWith(TEXT("/Game/Props/BK-")))
	{
		return TEXT("kit");
	}
	return TEXT("other");
}

// The engine's test (LumenMeshCards.cpp IsMatrixOrthogonal), repeated here: Lumen captures no cards for a primitive
// whose mesh-cards-to-world matrix fails it.
bool IsOrthogonal(const FMatrix& M)
{
	if (M.GetScaleVector().GetAbsMin() < KINDA_SMALL_NUMBER)
	{
		return false;
	}
	FVector X, Y, Z;
	M.GetUnitAxes(X, Y, Z);
	return FMath::Abs(X | Y) < KINDA_SMALL_NUMBER && FMath::Abs(X | Z) < KINDA_SMALL_NUMBER && FMath::Abs(Y | Z) < KINDA_SMALL_NUMBER;
}

void ReadMesh(const UStaticMesh& Mesh, bool& bCards, bool& bDf, int32* Cards, FBox3f* Bounds)
{
	const FStaticMeshRenderData* Render = Mesh.GetRenderData();
	const FStaticMeshLODResources* Lod = Render && Render->LODResources.Num() > 0 ? &Render->LODResources[0] : nullptr;
	bCards = Lod && Lod->CardRepresentationData;
	bDf = Lod && Lod->DistanceFieldData;
	if (Cards)
	{
		*Cards = bCards ? Lod->CardRepresentationData->MeshCardsBuildData.CardBuildData.Num() : 0;
	}
	if (Bounds && bCards)
	{
		*Bounds = FBox3f(Lod->CardRepresentationData->MeshCardsBuildData.Bounds);
	}
}

float LargestFace(const FBox3f& Bounds, const FVector& Scale)
{
	const FVector3f S = FVector3f(Bounds.GetSize()) * FVector3f(Scale);
	return FMath::Max3(S.Y * S.Z, S.X * S.Z, S.X * S.Y);
}

#if WITH_EDITOR
// -TestMap (the header's note): the building box, its component and its index there.
struct FBoxSample
{
	UHierarchicalInstancedStaticMeshComponent* Source = nullptr;
	int32 Instance = INDEX_NONE;
	FVector Size = FVector::ZeroVector; // cube scale (the engine cube is 1 m a side)
	double Volume = 0.0;
};

// The biggest building-sized engine-cube instance in a state-A, ray-traced, drawn HISM of two or more instances: every
// side 3 m or more, 6 m tall or more, no side over 40 m (the quay blocks and ground slabs are bigger).
FBoxSample FindBuildingBox(const UWorld& World)
{
	FBoxSample Best;
	for (AActor* Actor : World.PersistentLevel->Actors)
	{
		if (!Actor)
		{
			continue;
		}
		TInlineComponentArray<UHierarchicalInstancedStaticMeshComponent*> Components(Actor);
		for (UHierarchicalInstancedStaticMeshComponent* Component : Components)
		{
			const UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
			if (!Mesh || Mesh->GetPathName() != TEXT("/Engine/BasicShapes/Cube.Cube") || !Component->bAffectDistanceFieldLighting ||
				!Component->bAffectDynamicIndirectLighting || !Component->bVisibleInRayTracing || Component->bHiddenInGame ||
				Component->GetInstanceCount() < 2)
			{
				continue;
			}
			for (int32 I = 0; I < Component->GetInstanceCount(); ++I)
			{
				FTransform At;
				Component->GetInstanceTransform(I, At, true);
				const FVector Scale = At.GetScale3D().GetAbs();
				const FVector Cm = Scale * 100.0;
				if (Cm.GetMin() < 300.0 || Cm.Z < 600.0 || Cm.GetMax() > 4000.0)
				{
					continue;
				}
				const double Volume = Cm.X * Cm.Y * Cm.Z;
				if (Volume > Best.Volume)
				{
					Best.Source = Component;
					Best.Instance = I;
					Best.Size = Scale;
					Best.Volume = Volume;
				}
			}
		}
	}
	return Best;
}

// Every plain property of Source that differs from a new HISM's (arrays and object references left out; materials
// logged on their own).
void LogFlagDiff(const UHierarchicalInstancedStaticMeshComponent& Source)
{
	const UObject* Defaults = UHierarchicalInstancedStaticMeshComponent::StaticClass()->GetDefaultObject();
	int32 Count = 0;
	for (TFieldIterator<FProperty> It(UHierarchicalInstancedStaticMeshComponent::StaticClass()); It; ++It)
	{
		const FProperty* Property = *It;
		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || CastField<FArrayProperty>(Property) ||
			CastField<FObjectPropertyBase>(Property) || CastField<FMapProperty>(Property) || CastField<FSetProperty>(Property))
		{
			continue;
		}
		const void* Ours = Property->ContainerPtrToValuePtr<void>(&Source);
		const void* Engine = Property->ContainerPtrToValuePtr<void>(Defaults);
		if (Property->Identical(Ours, Engine, PPF_None))
		{
			continue;
		}
		FString OursText, EngineText;
		Property->ExportTextItem_Direct(OursText, Ours, nullptr, nullptr, PPF_None);
		Property->ExportTextItem_Direct(EngineText, Engine, nullptr, nullptr, PPF_None);
		UE_LOG(LogForgottenThird, Display, TEXT("FTLumenCardsReport test flags: %s = %s (new HISM: %s)"), *Property->GetName(),
			*OursText.Left(200), *EngineText.Left(200));
		++Count;
	}
	for (int32 S = 0; S < Source.GetNumMaterials(); ++S)
	{
		const UMaterialInterface* Material = Source.GetMaterial(S);
		UE_LOG(LogForgottenThird, Display, TEXT("FTLumenCardsReport test flags: material %d = %s"), S,
			Material ? *Material->GetPathName() : TEXT("none"));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTLumenCardsReport test flags: %d properties differ from a new HISM's."), Count);
}

// The test map (the header's note). Returns 0 once it is saved.
int32 WriteTestMap(const FString& MapPath, const FBoxSample& Box, const AWorldSettings* SourceSettings)
{
	UHierarchicalInstancedStaticMeshComponent& Source = *Box.Source;
	UStaticMesh* Cube = Source.GetStaticMesh();
	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTLumenCardsReport: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(*MapPath);
	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, *FPackageName::GetShortName(MapPath), Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTLumenCardsReport: couldn't create the test world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	if (AWorldSettings* Settings = World->GetWorldSettings(); Settings && SourceSettings)
	{
		Settings->DefaultGameMode = SourceSettings->DefaultGameMode;
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	const FVector Size = Box.Size;
	const double Width = Size.X * 100.0, Depth = Size.Y * 100.0, Height = Size.Z * 100.0;
	const double Pitch = Width + 2000.0; // 20 m between neighbours
	const int32 Ways = 5;
	const auto Place = [&](int32 I) { return FTransform(FRotator::ZeroRotator, FVector(I * Pitch, 0.0, Height * 0.5), Size); };
	const int32 Floats = Source.NumCustomDataFloats;
	TArray<float> Custom;
	for (int32 F = 0; F < Floats; ++F)
	{
		const int32 At = Box.Instance * Floats + F;
		Custom.Add(Source.PerInstanceSMCustomData.IsValidIndex(At) ? Source.PerInstanceSMCustomData[At] : 0.f);
	}
	const auto Materials = [&](UStaticMeshComponent& Component)
	{
		for (int32 S = 0; S < Source.GetNumMaterials(); ++S)
		{
			Component.SetMaterial(S, Source.GetMaterial(S));
		}
	};

	AStaticMeshActor* Ground = World->SpawnActor<AStaticMeshActor>(FVector((Ways - 1) * Pitch * 0.5, 0.0, -10.0), FRotator::ZeroRotator, Spawn);
	Ground->GetStaticMeshComponent()->SetStaticMesh(Cube);
	Ground->SetActorScale3D(FVector(((Ways - 1) * Pitch + Width + 6000.0) / 100.0, (Depth + 10000.0) / 100.0, 0.2));
	Materials(*Ground->GetStaticMeshComponent());
	Ground->SetActorLabel(TEXT("CardTest_ground"));

	// (a) a static mesh actor, engine defaults.
	AStaticMeshActor* Plain = World->SpawnActor<AStaticMeshActor>(Place(0).GetLocation(), FRotator::ZeroRotator, Spawn);
	Plain->GetStaticMeshComponent()->SetStaticMesh(Cube);
	Plain->SetActorScale3D(Size);
	Materials(*Plain->GetStaticMeshComponent());
	Plain->SetActorLabel(TEXT("CardTest_a_actor"));

	const auto MakeHism = [&](int32 Way, const TCHAR* Label, bool bOurFlags, int32 GroupId, bool bTwo)
	{
		AActor* Holder = World->SpawnActor<AActor>(FVector(Way * Pitch, 0.0, 0.0), FRotator::ZeroRotator, Spawn);
		USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("Root"));
		Root->SetMobility(EComponentMobility::Static);
		Holder->SetRootComponent(Root);
		Holder->AddInstanceComponent(Root);
		Root->RegisterComponent();
		Holder->SetActorLabel(Label);
		UHierarchicalInstancedStaticMeshComponent* Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(Holder, TEXT("Box"));
		Component->CreationMethod = EComponentCreationMethod::Instance;
		Component->SetStaticMesh(Cube);
		Materials(*Component);
		if (bOurFlags)
		{
			Component->SetMobility(Source.Mobility);
			Component->SetGenerateOverlapEvents(Source.GetGenerateOverlapEvents());
			Component->SetCollisionProfileName(Source.GetCollisionProfileName());
			Component->SetCanEverAffectNavigation(Source.CanEverAffectNavigation());
			Component->SetHiddenInGame(Source.bHiddenInGame);
			Component->SetCastShadow(Source.CastShadow);
			Component->SetCullDistances(Source.InstanceStartCullDistance, Source.InstanceEndCullDistance);
			Component->SetVisibleInRayTracing(Source.bVisibleInRayTracing);
			Component->SetAffectDistanceFieldLighting(Source.bAffectDistanceFieldLighting);
			Component->SetAffectDynamicIndirectLighting(Source.bAffectDynamicIndirectLighting);
			Component->SetNumCustomDataFloats(Floats);
			Component->bAutoRebuildTreeOnInstanceChanges = Source.bAutoRebuildTreeOnInstanceChanges;
			Component->ComponentTags = Source.ComponentTags;
		}
		Component->RayTracingGroupId = GroupId;
		Component->SetupAttachment(Root);
		Holder->AddInstanceComponent(Component);
		Component->RegisterComponent();
		TArray<FTransform> Transforms = {Place(Way)};
		if (bTwo)
		{
			// A 1 m cube under the ground slab: never seen, but it makes the component a many-instance one.
			Transforms.Add(FTransform(FRotator::ZeroRotator, FVector(Way * Pitch, 0.0, -400.0), FVector::OneVector));
		}
		const TArray<int32> Ids = Component->AddInstances(Transforms, true, true);
		if (bOurFlags && Floats > 0)
		{
			for (const int32 Id : Ids)
			{
				Component->SetCustomData(Id, Custom);
			}
		}
		Component->BuildTreeIfOutdated(false, true);
		Component->MarkRenderStateDirty();
	};
	MakeHism(1, TEXT("CardTest_b_hism_default_1"), false, -1, false);
	MakeHism(2, TEXT("CardTest_c_hism_ours_2"), true, -1, true);
	MakeHism(3, TEXT("CardTest_d_hism_default_2"), false, -1, true);
	MakeHism(4, TEXT("CardTest_e_hism_ours_2_rtgroup"), true, 7001, true);

	FTScene::SpawnLook(World, Spawn, ftcore::SceneLook{}, FRotator(-35, 150, 0));
	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(-3000.0, -3000.0, 100.0), FRotator::ZeroRotator, Spawn);
	Start->SetActorLabel(TEXT("PlayerStart"));

	FAssetRegistryModule::AssetCreated(World);
	Package->MarkPackageDirty();
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, World, *File, Save);
	World->DestroyWorld(false);
	if (!bSaved)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTLumenCardsReport: couldn't save %s."), *File);
		return 1;
	}

	// Eyes in the plan frame of a 1000 m square (engine metres X, Y to plan x = X + 500, y = 500 - Y).
	const auto Plan = [](const FVector& Cm) { return FString::Printf(TEXT("%.1f,%.1f,%.1f"), Cm.X / 100.0 + 500.0, 500.0 - Cm.Y / 100.0, Cm.Z / 100.0); };
	const double Row = (Ways - 1) * Pitch + Width;
	const FVector Mid((Ways - 1) * Pitch * 0.5, 0.0, Height * 0.5);
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTLumenCardsReport test map %s saved: box %s instance %d of %d (owner %s), %.0f x %.0f x %.0f cm, %d custom floats, ")
		TEXT("pitch %.0f cm. Wide eye %s at %s (35 mm)."),
		*MapPath, *Source.GetName(), Box.Instance, Source.GetInstanceCount(), *GetNameSafe(Source.GetOwner()), Width, Depth, Height,
		Floats, Pitch, *Plan(Mid + FVector(0.0, -Row * 1.05, Height * 0.3)), *Plan(Mid));
	for (int32 Way = 0; Way < Ways; ++Way)
	{
		const FVector Face(Way * Pitch, -Depth * 0.5, 800.0);
		UE_LOG(LogForgottenThird, Display, TEXT("FTLumenCardsReport test map way %c: close eye %s at %s (35 mm)."), TCHAR('a' + Way),
			*Plan(Face + FVector(-600.0, -2500.0, -630.0)), *Plan(Face));
	}
	return 0;
}
#endif
} // namespace

UFTLumenCardsReportCommandlet::UFTLumenCardsReportCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTLumenCardsReportCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString MapPath;
	if (!FParse::Value(*Params, TEXT("Map="), MapPath) || !FPackageName::IsValidLongPackageName(MapPath))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTLumenCardsReport: needs -Map=/Game/Maps/<map>."));
		return 1;
	}
	FString CsvPath, Rebuild;
	FParse::Value(*Params, TEXT("Csv="), CsvPath);
	FParse::Value(*Params, TEXT("Rebuild="), Rebuild);
	// -AsBuilt: rebuild the copy as the massing builder builds its meshes (its own DF flag, Build(true)) rather than
	// with the DF forced on, to tell the flag from the build path.
	const bool bAsBuilt = FParse::Param(*Params, TEXT("AsBuilt"));

	const IConsoleVariable* GenDf = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GenerateMeshDistanceFields"));
	const IConsoleVariable* MinSize = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LumenScene.SurfaceCache.MeshCardsMinSize"));
	const float MinArea = MinSize ? FMath::Square(MinSize->GetFloat()) : 100.f;
	UE_LOG(LogForgottenThird, Display, TEXT("FTLumenCardsReport: r.GenerateMeshDistanceFields=%d, MeshCardsMinSize=%.1f (min face %.0f cm2)."),
		GenDf ? GenDf->GetInt() : -1, MinSize ? MinSize->GetFloat() : -1.f, MinArea);

	UPackage* Package = LoadPackage(nullptr, *MapPath, LOAD_None);
	UWorld* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
	if (!World || !World->PersistentLevel)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTLumenCardsReport: couldn't load %s."), *MapPath);
		return 1;
	}

	TMap<UStaticMesh*, FMeshRow> Rows;
	struct FShapeCensus
	{
		int64 Total = 0, NoFace = 0, SidesCulled = 0, Thin10 = 0, NonUniform20 = 0, NonOrtho = 0, Thick50 = 0;
		TMap<FString, int64> ThickByMat;
	} Shapes;
	const double MinShapeArea = MinArea;
	for (AActor* Actor : World->PersistentLevel->Actors)
	{
		if (!Actor)
		{
			continue;
		}
		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		for (UStaticMeshComponent* Component : Components)
		{
			UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
			if (!Mesh)
			{
				continue;
			}
			FMeshRow& Row = Rows.FindOrAdd(Mesh);
			Row.Mesh = Mesh;
			++Row.Components;
			const UInstancedStaticMeshComponent* Ism = Cast<UInstancedStaticMeshComponent>(Component);
			const int64 Count = Ism ? Ism->GetInstanceCount() : 1;
			const bool bA = Component->bAffectDistanceFieldLighting && Component->bAffectDynamicIndirectLighting;
			const bool bB = !bA && Component->bVisibleInRayTracing;
			(bA ? Row.InstancesA : bB ? Row.InstancesB : Row.InstancesC) += Count;
			// Engine shapes in state A, per instance: their world size against the card minimum, and the big thick ones.
			if (bA && Mesh->GetPathName().StartsWith(TEXT("/Engine/BasicShapes/")))
			{
				const FVector Unit = Mesh->GetBounds().BoxExtent * 2.0;
				const UMaterialInterface* Mat = Component->GetMaterial(0);
				for (int64 I = 0; I < Count; ++I)
				{
					FTransform At = Component->GetComponentTransform();
					if (Ism)
					{
						Ism->GetInstanceTransform(static_cast<int32>(I), At, true);
					}
					const FMatrix M = At.ToMatrixWithScale();
					const FVector S = M.GetScaleVector().GetAbs();
					FVector W = Unit * S;
					if (W.X > W.Y) Swap(W.X, W.Y);
					if (W.Y > W.Z) Swap(W.Y, W.Z);
					if (W.X > W.Y) Swap(W.X, W.Y);
					++Shapes.Total;
					Shapes.NoFace += W.Y * W.Z <= MinShapeArea;
					Shapes.SidesCulled += W.Y * W.Z > MinShapeArea && W.X * W.Z <= MinShapeArea;
					Shapes.Thin10 += W.X < 10.0;
					Shapes.NonUniform20 += S.GetMin() > 0.0 && S.GetMax() / S.GetMin() > 20.0;
					Shapes.NonOrtho += !IsOrthogonal(M);
					if (W.X >= 50.0 && W.Y * W.Z >= 1e4)
					{
						++Shapes.Thick50;
						Shapes.ThickByMat.FindOrAdd(Mat ? Mat->GetName() : FString(TEXT("none"))) += 1;
					}
				}
			}
			if (bA && !Row.bSample && Count > 0)
			{
				FTransform At = Component->GetComponentTransform();
				if (Ism)
				{
					Ism->GetInstanceTransform(0, At, true);
				}
				Row.SampleToWorld = At.ToMatrixWithScale();
				Row.bSample = true;
			}
		}
	}

	// Meshes compiled (render data exists), card and distance-field queues not yet waited on: what a renderer finds at
	// load. Then everything finished.
	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	for (TPair<UStaticMesh*, FMeshRow>& Pair : Rows)
	{
		Pair.Value.Family = FamilyOf(*Pair.Key);
		ReadMesh(*Pair.Key, Pair.Value.bCardsAtLoad, Pair.Value.bDfAtLoad, nullptr, nullptr);
	}
	FAssetCompilingManager::Get().FinishAllCompilation();
	if (GCardRepresentationAsyncQueue)
	{
		GCardRepresentationAsyncQueue->BlockUntilAllBuildsComplete();
	}
	if (GDistanceFieldAsyncQueue)
	{
		GDistanceFieldAsyncQueue->BlockUntilAllBuildsComplete();
	}

	struct FFamily
	{
		int32 Meshes = 0, WithCards = 0, LateCards = 0, ZeroCards = 0, WithDf = 0, NonOrtho = 0, SmallFace = 0;
		int64 InstancesA = 0, InstancesB = 0, InstancesC = 0, InstancesAWithCards = 0;
	};
	TMap<FString, FFamily> Families;
	TArray<FString> Lines;
	Lines.Add(TEXT("mesh,family,components,inst_a,inst_b,inst_c,cards_at_load,df_at_load,cards_data,cards,df_data,gen_df_flag,two_sided_df,df_res_scale,nanite,")
			  TEXT("card_bounds_cm,sample_scale,largest_face_cm2,face_ok,orthogonal"));
	UStaticMesh* RebuildMesh = nullptr;
	for (TPair<UStaticMesh*, FMeshRow>& Pair : Rows)
	{
		FMeshRow& Row = Pair.Value;
		ReadMesh(*Row.Mesh, Row.bCards, Row.bDf, &Row.Cards, &Row.CardBounds);
		const FVector Scale = Row.SampleToWorld.GetScaleVector();
		const float Face = Row.bCards ? LargestFace(Row.CardBounds, Scale) : 0.f;
		const bool bFaceOk = Face > MinArea;
		const bool bOrtho = !Row.bSample || IsOrthogonal(Row.SampleToWorld);
		const FMeshBuildSettings* Build = Row.Mesh->GetNumSourceModels() > 0 ? &Row.Mesh->GetSourceModel(0).BuildSettings : nullptr;
		FFamily& F = Families.FindOrAdd(Row.Family);
		++F.Meshes;
		F.WithCards += Row.bCards && Row.Cards > 0;
		F.LateCards += Row.bCards && !Row.bCardsAtLoad;
		F.ZeroCards += Row.bCards && Row.Cards == 0;
		F.WithDf += Row.bDf;
		F.NonOrtho += Row.bSample && !bOrtho;
		F.SmallFace += Row.bSample && Row.bCards && !bFaceOk;
		F.InstancesA += Row.InstancesA;
		F.InstancesB += Row.InstancesB;
		F.InstancesC += Row.InstancesC;
		if (Row.bCards && Row.Cards > 0 && bOrtho && (!Row.bSample || bFaceOk))
		{
			F.InstancesAWithCards += Row.InstancesA;
		}
		const FVector3f Size = Row.bCards ? Row.CardBounds.GetSize() : FVector3f::ZeroVector;
		Lines.Add(FString::Printf(TEXT("%s,%s,%d,%lld,%lld,%lld,%d,%d,%d,%d,%d,%d,%d,%.2f,%d,%.0fx%.0fx%.0f,%.3fx%.3fx%.3f,%.0f,%d,%d"),
			*Row.Mesh->GetPathName(), *Row.Family, Row.Components, Row.InstancesA, Row.InstancesB, Row.InstancesC,
			Row.bCardsAtLoad, Row.bDfAtLoad, Row.bCards, Row.Cards, Row.bDf, Row.Mesh->bGenerateMeshDistanceField,
			Build ? Build->bGenerateDistanceFieldAsIfTwoSided : -1, Build ? Build->DistanceFieldResolutionScale : -1.f,
			Row.Mesh->IsNaniteEnabled(), Size.X, Size.Y, Size.Z, Scale.X, Scale.Y, Scale.Z, Face, bFaceOk, bOrtho));
		const bool bWanted = Rebuild.Equals(TEXT("auto"), ESearchCase::IgnoreCase)
			? (Row.InstancesA > 0 && !(Row.bCards && Row.Cards > 0) && (Row.Family == TEXT("map-built") || (!RebuildMesh && Row.Family == TEXT("engine"))))
			: (!Rebuild.IsEmpty() && Row.Mesh->GetPathName() == Rebuild);
		if (bWanted && (!RebuildMesh || FamilyOf(*RebuildMesh) != TEXT("map-built")))
		{
			RebuildMesh = Row.Mesh;
		}
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTLumenCardsReport engine shapes in state A: %lld instances; every face under the card minimum %lld; only the big ")
		TEXT("faces kept (sides culled) %lld; thinnest side under 10 cm %lld; scale ratio over 20 %lld; non-orthogonal %lld; ")
		TEXT("at least 50 cm thick with a face of 1 m2 or more %lld."),
		Shapes.Total, Shapes.NoFace, Shapes.SidesCulled, Shapes.Thin10, Shapes.NonUniform20, Shapes.NonOrtho, Shapes.Thick50);
	Shapes.ThickByMat.ValueSort([](int64 A, int64 B) { return A > B; });
	int32 Shown = 0;
	for (const TPair<FString, int64>& Pair : Shapes.ThickByMat)
	{
		if (++Shown > 15)
		{
			break;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTLumenCardsReport engine shapes 50 cm+ thick on %s: %lld."), *Pair.Key, Pair.Value);
	}
	for (const TPair<FString, FFamily>& Pair : Families)
	{
		const FFamily& F = Pair.Value;
		UE_LOG(LogForgottenThird, Display,
			TEXT("FTLumenCardsReport family %s: %d meshes, %d with cards (%d only after the queue, %d with card data but 0 cards), %d with DF; ")
			TEXT("sample non-orthogonal %d, face under min %d; instances A %lld (with usable cards %lld), B %lld, C %lld."),
			*Pair.Key, F.Meshes, F.WithCards, F.LateCards, F.ZeroCards, F.WithDf, F.NonOrtho, F.SmallFace, F.InstancesA,
			F.InstancesAWithCards, F.InstancesB, F.InstancesC);
	}
	if (!CsvPath.IsEmpty())
	{
		FFileHelper::SaveStringArrayToFile(Lines, *CsvPath);
		UE_LOG(LogForgottenThird, Display, TEXT("FTLumenCardsReport: %d meshes written to %s."), Lines.Num() - 1, *CsvPath);
	}

	FString TestMap;
	if (FParse::Value(*Params, TEXT("TestMap="), TestMap))
	{
		if (!FPackageName::IsValidLongPackageName(TestMap) || TestMap == MapPath)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTLumenCardsReport: -TestMap needs a new /Game/Maps/<map>."));
			return 1;
		}
		const FBoxSample Box = FindBuildingBox(*World);
		if (!Box.Source)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTLumenCardsReport: no building box found for -TestMap."));
			return 1;
		}
		LogFlagDiff(*Box.Source);
		if (WriteTestMap(TestMap, Box, World->GetWorldSettings()) != 0)
		{
			return 1;
		}
	}

	if (!Rebuild.IsEmpty())
	{
		if (!RebuildMesh)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTLumenCardsReport -Rebuild=%s: no state-A mesh without cards matched."), *Rebuild);
			return 0;
		}
		bool bBefore = false, bDfBefore = false;
		int32 CardsBefore = 0;
		ReadMesh(*RebuildMesh, bBefore, bDfBefore, &CardsBefore, nullptr);
		UStaticMesh* Copy = DuplicateObject<UStaticMesh>(RebuildMesh, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("FTCardsAB")));
		if (!bAsBuilt)
		{
			Copy->bGenerateMeshDistanceField = true;
			for (int32 L = 0; L < Copy->GetNumSourceModels(); ++L)
			{
				Copy->GetSourceModel(L).BuildSettings.DistanceFieldResolutionScale = FMath::Max(1.f, Copy->GetSourceModel(L).BuildSettings.DistanceFieldResolutionScale);
			}
		}
		Copy->Build(bAsBuilt);
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (GCardRepresentationAsyncQueue)
		{
			GCardRepresentationAsyncQueue->BlockUntilAllBuildsComplete();
		}
		if (GDistanceFieldAsyncQueue)
		{
			GDistanceFieldAsyncQueue->BlockUntilAllBuildsComplete();
		}
		bool bAfter = false, bDfAfter = false;
		int32 CardsAfter = 0;
		FBox3f Bounds(ForceInit);
		ReadMesh(*Copy, bAfter, bDfAfter, &CardsAfter, &Bounds);
		UE_LOG(LogForgottenThird, Display,
			TEXT("FTLumenCardsReport A/B %s (%s, %d source models, %d LODs): before card data %d (%d cards), DF %d; ")
			TEXT("after a full build %s: card data %d (%d cards, bounds %s), DF %d."),
			*RebuildMesh->GetPathName(), *FamilyOf(*RebuildMesh), RebuildMesh->GetNumSourceModels(),
			RebuildMesh->GetRenderData() ? RebuildMesh->GetRenderData()->LODResources.Num() : 0, bBefore, CardsBefore, bDfBefore,
			bAsBuilt ? TEXT("as the massing builder builds (own DF flag, silent)") : TEXT("with the DF forced on"), bAfter, CardsAfter, *Bounds.GetSize().ToString(), bDfAfter);

		// -Fresh: a new mesh made from the same mesh description the way the builders make theirs (NewObject,
		// CreateMeshDescription, Commit, Build(true), PostEditChange), once as they do and once with the section map
		// set before the build.
		if (FParse::Param(*Params, TEXT("Fresh")) && RebuildMesh->GetMeshDescription(0))
		{
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				const bool bSections = Pass == 1;
				UStaticMesh* Fresh = NewObject<UStaticMesh>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("FTCardsFresh")));
				Fresh->GetStaticMaterials() = RebuildMesh->GetStaticMaterials();
				FStaticMeshSourceModel& Model = Fresh->AddSourceModel();
				Model.BuildSettings = RebuildMesh->GetSourceModel(0).BuildSettings;
				Fresh->bGenerateMeshDistanceField = RebuildMesh->bGenerateMeshDistanceField;
				FMeshDescription Desc = *RebuildMesh->GetMeshDescription(0);
				Fresh->CreateMeshDescription(0, MoveTemp(Desc));
				Fresh->CommitMeshDescription(0);
				const int32 SectionsBefore = Fresh->GetSectionInfoMap().GetSectionNumber(0);
				if (bSections)
				{
					for (int32 S = 0; S < Fresh->GetStaticMaterials().Num(); ++S)
					{
						FMeshSectionInfo Info(S);
						Fresh->GetSectionInfoMap().Set(0, S, Info);
					}
				}
				Fresh->Build(true);
				Fresh->PostEditChange();
				FAssetCompilingManager::Get().FinishAllCompilation();
				if (GCardRepresentationAsyncQueue)
				{
					GCardRepresentationAsyncQueue->BlockUntilAllBuildsComplete();
				}
				bool bFreshCards = false, bFreshDf = false;
				int32 FreshCards = 0;
				ReadMesh(*Fresh, bFreshCards, bFreshDf, &FreshCards, nullptr);
				UE_LOG(LogForgottenThird, Display,
					TEXT("FTLumenCardsReport fresh %s: section map %d before the build, %d after; card data %d (%d cards), DF %d."),
					bSections ? TEXT("with the section map set first") : TEXT("as the builders make it"), SectionsBefore,
					Fresh->GetSectionInfoMap().GetSectionNumber(0), bFreshCards, FreshCards, bFreshDf);
			}
		}
	}
	return 0;
#else
	return 1;
#endif
}
