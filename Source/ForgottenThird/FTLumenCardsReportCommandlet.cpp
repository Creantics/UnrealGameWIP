#include "FTLumenCardsReportCommandlet.h"

#include "AssetCompilingManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DistanceFieldAtlas.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "MeshCardBuild.h"
#include "MeshCardRepresentation.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

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
		Copy->bGenerateMeshDistanceField = true;
		for (int32 L = 0; L < Copy->GetNumSourceModels(); ++L)
		{
			Copy->GetSourceModel(L).BuildSettings.DistanceFieldResolutionScale = FMath::Max(1.f, Copy->GetSourceModel(L).BuildSettings.DistanceFieldResolutionScale);
		}
		Copy->Build(false);
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
			TEXT("after a full build with the DF forced on: card data %d (%d cards, bounds %s), DF %d."),
			*RebuildMesh->GetPathName(), *FamilyOf(*RebuildMesh), RebuildMesh->GetNumSourceModels(),
			RebuildMesh->GetRenderData() ? RebuildMesh->GetRenderData()->LODResources.Num() : 0, bBefore, CardsBefore, bDfBefore,
			bAfter, CardsAfter, *Bounds.GetSize().ToString(), bDfAfter);
	}
	return 0;
#else
	return 1;
#endif
}
