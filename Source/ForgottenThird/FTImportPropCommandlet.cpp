#include "FTImportPropCommandlet.h"

#include "ForgottenThird.h"
#include "FTImportMaterials.h"

#if WITH_EDITOR
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ObjectTools.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "IAssetTools.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

#include "ftcore/PropGloss.h"
#endif

UFTImportPropCommandlet::UFTImportPropCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

#if WITH_EDITOR
namespace
{
// -Fresh: deletes what an earlier import of this file left in <Dest>/<file>/Materials and /Textures, so the re-import
// makes its material instances and textures anew (otherwise an instance kept on a prop master keeps its old values and
// textures, and a proof reads a false result). References to them (the mesh's slots) are nulled until the import fills
// them again. False (logged) if anything there would not delete.
bool DeleteEarlierImport(const FString& Folder)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	Registry.ScanPathsSynchronous({ Folder }, true);
	TArray<UObject*> Objects;
	for (const TCHAR* Sub : { TEXT("Materials"), TEXT("Textures") })
	{
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*(Folder / Sub)), Assets, true);
		for (const FAssetData& Asset : Assets)
		{
			if (UObject* Object = Asset.GetAsset())
			{
				Objects.Add(Object);
			}
		}
	}
	if (Objects.Num() == 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportProp: -Fresh: nothing under %s/Materials or /Textures (a first import)."), *Folder);
		return true;
	}
	const int32 Deleted = ObjectTools::ForceDeleteObjects(Objects, false);
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportProp: -Fresh: deleted %d of %d earlier materials and textures under %s."), Deleted,
		Objects.Num(), *Folder);
	if (Deleted != Objects.Num())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp: -Fresh: %d earlier assets under %s would not delete; nothing imported."),
			Objects.Num() - Deleted, *Folder);
		return false;
	}
	return true;
}
} // namespace
#endif

int32 UFTImportPropCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString Source, Dest = TEXT("/Game/Props"), Pack;
	if (!FParse::Value(*Params, TEXT("Source="), Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp: pass -Source=<file.glb>."));
		return 1;
	}
	FParse::Value(*Params, TEXT("Dest="), Dest);
	FParse::Value(*Params, TEXT("Pack="), Pack);
	if (FPaths::IsRelative(Source))
	{
		Source = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Source);
	}
	if (!FPaths::FileExists(Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp: %s doesn't exist."), *Source);
		return 1;
	}
	if (!FPaths::GetExtension(Source).Equals(TEXT("glb"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp: props come in as .glb (docs/ASSET_INTAKE.md)."));
		return 1;
	}
	const ftcore::AssetPack PackKind = ftcore::assetPackFromName(TCHAR_TO_UTF8(*Pack));
	const bool bNoCollision = FParse::Param(*Params, TEXT("NoCollision"));
	const bool bNoNanite = FParse::Param(*Params, TEXT("NoNanite"));
	const bool bKit = FParse::Param(*Params, TEXT("Kit"));
	const bool bForceNanite = FParse::Param(*Params, TEXT("Nanite")) && !bNoNanite;
	// -Held: a prop carried in a hand keeps the character masters (no grime, no world-fixed noise).
	const bool bHeld = FParse::Param(*Params, TEXT("Held"));
	// The world whose grime the prop masters take: -World=, else -Pack=, else Solhaven (propGrimeFor(None)).
	FString WorldName;
	FParse::Value(*Params, TEXT("World="), WorldName);
	ftcore::AssetPack World = PackKind;
	if (!WorldName.IsEmpty())
	{
		World = ftcore::assetPackFromName(TCHAR_TO_UTF8(*WorldName));
		if (World == ftcore::AssetPack::None)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp: -World=%s is not a world (Solhaven, Lattice, Unwritten, Restored, InkSolhaven)."),
				*WorldName);
			return 1;
		}
	}
	const float Grime = ftcore::propGrimeFor(World);
	const ftcore::AssetRules Rules = bKit ? ftcore::kitRules(PackKind) : ftcore::propRules(PackKind);
	if (!FT::CheckGlbFile(TEXT("FTImportProp"), Source, Rules))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp: %s breaks the asset rules (docs/ASSET_INTAKE.md); nothing imported."), *Source);
		return 1;
	}

	if (FParse::Param(*Params, TEXT("Fresh")) && !DeleteEarlierImport(Dest / FPaths::GetBaseFilename(Source)))
	{
		return 1;
	}

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = Source;
	Task->DestinationPath = Dest;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bSave = true;
	Task->bAsync = false;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.ImportAssetTasks({ Task });

	bool bOk = true;
	TArray<UMaterialInterface*> Materials;
	int32 Meshes = 0;
	bool bNanite = false;
	for (UObject* Object : Task->GetObjects())
	{
		if (!Object)
		{
			continue;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportProp: imported %s (%s)"), *Object->GetPathName(), *Object->GetClass()->GetName());
		if (Cast<USkeletalMesh>(Object))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp: %s is skinned; import it with FTImportCharacter."), *Object->GetName());
			bOk = false;
			continue;
		}
		UStaticMesh* Mesh = Cast<UStaticMesh>(Object);
		if (!Mesh)
		{
			continue;
		}
		++Meshes;
		const int32 Triangles = Mesh->GetNumLODs() > 0 ? Mesh->GetNumTriangles(0) : 0;
		const UBodySetup* Body = Mesh->GetBodySetup();
		const int32 Shapes = Body ? Body->AggGeom.GetElementCount() : 0;
		UE_LOG(LogForgottenThird, Display, TEXT("FTImportProp: %s: %d triangles, %d material slots, %d collision shapes."),
			*Mesh->GetName(), Triangles, Mesh->GetStaticMaterials().Num(), Shapes);
		if (Shapes == 0)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTImportProp: %s has no simple collision."), *Mesh->GetName());
		}
		if (bNoNanite && Mesh->IsNaniteEnabled())
		{
			// -NoNanite: a prop with thin parts (sills, hoods, lanterns) that Nanite's reduced fallback shreds; drawn at full detail.
			FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
			Nanite.bEnabled = false;
			Mesh->SetNaniteSettings(Nanite);
			Mesh->Build(true);
			Mesh->PostEditChange();
			UE_LOG(LogForgottenThird, Display, TEXT("FTImportProp: %s: -NoNanite: Nanite off, %d triangles at LOD0."), *Mesh->GetName(),
				Mesh->GetNumLODs() > 0 ? Mesh->GetNumTriangles(0) : 0);
		}
		bool bFullFallback = false;
		if (bForceNanite && !Mesh->IsNaniteEnabled())
		{
			// -Nanite: a large prop (a tower) drawn through Nanite whatever the importer's default.
			FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
			Nanite.bEnabled = true;
			Mesh->SetNaniteSettings(Nanite);
			Mesh->Build(true);
			Mesh->PostEditChange();
			bFullFallback = true;
			UE_LOG(LogForgottenThird, Display, TEXT("FTImportProp: %s: -Nanite: Nanite on."), *Mesh->GetName());
		}
		if ((bKit || bForceNanite) && Mesh->IsNaniteEnabled())
		{
			// -Kit: a kit piece keeps its raster fallback and ray-tracing proxy at full detail (a reduced one shreds thin
			// sills and rails), and fails when the fallback keeps under 90% of the source's triangles.
			FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
			FMeshRayTracingProxySettings Proxy = Mesh->GetRayTracingProxySettings();
			if (Nanite.FallbackTarget != ENaniteFallbackTarget::PercentTriangles || Nanite.FallbackPercentTriangles < 1.f ||
				Proxy.FallbackTarget != ENaniteFallbackTarget::PercentTriangles || Proxy.FallbackPercentTriangles < 1.f)
			{
				Nanite.FallbackTarget = ENaniteFallbackTarget::PercentTriangles;
				Nanite.FallbackPercentTriangles = 1.f;
				Nanite.FallbackRelativeError = 0.f;
				Mesh->SetNaniteSettings(Nanite);
				Proxy.FallbackTarget = ENaniteFallbackTarget::PercentTriangles;
				Proxy.FallbackPercentTriangles = 1.f;
				Proxy.FallbackRelativeError = 0.f;
				Mesh->SetRayTracingProxySettings(Proxy);
				Mesh->Build(true);
				Mesh->PostEditChange();
				bFullFallback = true;
			}
			const FMeshDescription* Description = Mesh->GetMeshDescription(0);
			const int32 SourceTriangles = Description ? Description->Triangles().Num() : 0;
			const int32 Fallback = Mesh->GetNumLODs() > 0 ? Mesh->GetNumTriangles(0) : 0;
			const double Share = SourceTriangles > 0 ? static_cast<double>(Fallback) / SourceTriangles : 1.0;
			UE_LOG(LogForgottenThird, Display, TEXT("FTImportProp: %s: -Kit: fallback %d of %d source triangles (%.0f%%)%s."), *Mesh->GetName(),
				Fallback, SourceTriangles, 100.0 * Share, bFullFallback ? TEXT(", fallback and ray-tracing proxy set to every triangle") : TEXT(""));
			if (Share < 0.9)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp: %s's fallback keeps under 90%% of its triangles."), *Mesh->GetName());
				bOk = false;
			}
		}
		if (bNoCollision || bNoNanite || bFullFallback)
		{
			// -NoCollision: a piece that never collides (a clock hand): no simple shapes, no complex either.
			UBodySetup* Setup = bNoCollision ? Mesh->GetBodySetup() : nullptr;
			if (Setup)
			{
				Setup->RemoveSimpleCollision();
				Setup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
				Setup->DefaultInstance.SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
				Setup->InvalidatePhysicsData();
				Setup->CreatePhysicsMeshes();
			}
			UPackage* Package = Mesh->GetPackage();
			Package->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
			if (!UPackage::SavePackage(Package, Mesh, *File, Args))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp: couldn't save %s after -NoCollision/-NoNanite."), *File);
				bOk = false;
			}
			const UBodySetup* After = Mesh->GetBodySetup();
			UE_LOG(LogForgottenThird, Display, TEXT("FTImportProp: %s: -NoCollision: %d collision shapes left."), *Mesh->GetName(),
				After ? After->AggGeom.GetElementCount() : 0);
		}
		bNanite |= Mesh->IsNaniteEnabled();
		for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
		{
			Materials.AddUnique(Slot.MaterialInterface);
		}
	}
	if (Meshes == 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp: no static mesh came out of %s."), *Source);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportProp: %s, Grime %.2f."),
		bHeld ? TEXT("-Held: the character masters") : TEXT("the prop masters"), bHeld ? 0.0f : Grime);
	bOk &= FT::UsePropMasters(TEXT("FTImportProp"), Materials, Grime, bHeld);
	if (bNanite)
	{
		FT::AllowNanite(TEXT("FTImportProp"), Materials);
	}
	// The gloss guard, on the values the prop now draws with (no -AllowGloss: a failing material is fixed at its source).
	bOk &= FT::CheckPropGloss(TEXT("FTImportProp"), Materials);
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportProp: %d static meshes, %s."), Meshes, bOk ? TEXT("ok") : TEXT("failed"));
	return bOk ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp needs an editor build."));
	return 1;
#endif
}
