#include "FTImportPropCommandlet.h"

#include "ForgottenThird.h"
#include "FTImportMaterials.h"

#if WITH_EDITOR
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "IAssetTools.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#endif

UFTImportPropCommandlet::UFTImportPropCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

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
	const ftcore::AssetRules Rules = FParse::Param(*Params, TEXT("Kit")) ? ftcore::kitRules(PackKind) : ftcore::propRules(PackKind);
	if (!FT::CheckGlbFile(TEXT("FTImportProp"), Source, Rules))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp: %s breaks the asset rules (docs/ASSET_INTAKE.md); nothing imported."), *Source);
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
		if (bNoCollision || bNoNanite)
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
	bOk &= FT::UseProjectMasters(TEXT("FTImportProp"), Materials);
	if (bNanite)
	{
		FT::AllowNanite(TEXT("FTImportProp"), Materials);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTImportProp: %d static meshes, %s."), Meshes, bOk ? TEXT("ok") : TEXT("failed"));
	return bOk ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTImportProp needs an editor build."));
	return 1;
#endif
}
