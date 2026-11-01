#include "FTBuildArenaWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "FTArenaGameMode.h"
#include "FTArenaStage.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

namespace
{
const TCHAR* const kArenaMapFolder = TEXT("/Game/Maps");

#if WITH_EDITOR
// Adds an expression to the material the way the material editor does.
template <typename T>
T* AddArenaExpression(UMaterial* Material, int32 X, int32 Y)
{
	T* Expression = NewObject<T>(Material, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Expression);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = X;
	Expression->MaterialExpressionEditorY = Y;
	Expression->UpdateMaterialExpressionGuid(true, false);
	return Expression;
}

bool SaveArenaPackage(UPackage* Package, UObject* Asset, const FString& File)
{
	FAssetRegistryModule::AssetCreated(Asset);
	Package->MarkPackageDirty();
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Asset, *File, Save))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildArenaWorld: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildArenaWorld: saved %s."), *File);
	return true;
}

// The glow master the stages' lit bands, cracks and caps wear: Color times Strength into the emissive colour, on
// a black surface.
bool BuildGlowMaterial()
{
	const FString PackageName = FTArenaStage::GlowMaterial;
	const FString File = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildArenaWorld: couldn't replace %s."), *File);
		return false;
	}
	UPackage* Package = CreatePackage(*PackageName);
	UMaterial* Material = NewObject<UMaterial>(Package, FName(*FPackageName::GetShortName(PackageName)), RF_Public | RF_Standalone | RF_Transactional);
	Material->MaterialDomain = MD_Surface;
	Material->BlendMode = BLEND_Opaque;

	UMaterialExpressionVectorParameter* Color = AddArenaExpression<UMaterialExpressionVectorParameter>(Material, -600, 0);
	Color->ParameterName = TEXT("Color");
	Color->DefaultValue = FLinearColor(1.f, 1.f, 1.f, 1.f);
	UMaterialExpressionScalarParameter* Strength = AddArenaExpression<UMaterialExpressionScalarParameter>(Material, -600, 200);
	Strength->ParameterName = TEXT("Strength");
	Strength->DefaultValue = 1.f;
	UMaterialExpressionMultiply* Product = AddArenaExpression<UMaterialExpressionMultiply>(Material, -300, 0);
	Product->A.Connect(0, Color);
	Product->B.Connect(0, Strength);
	Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Product);

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return SaveArenaPackage(Package, Material, File);
}
#endif
} // namespace

UFTBuildArenaWorldCommandlet::UFTBuildArenaWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildArenaWorldCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString MapName = TEXT("Demo_Arena");
	FParse::Value(*Params, TEXT("Map="), MapName);
	if (MapName.IsEmpty() || !FName::IsValidXName(MapName, INVALID_LONGPACKAGE_CHARACTERS))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildArenaWorld: '%s' is not a valid map name."), *MapName);
		return 1;
	}
	const FString MapPath = FString::Printf(TEXT("%s/%s"), kArenaMapFolder, *MapName);

	if (!BuildGlowMaterial())
	{
		return 1;
	}

	// Start from nothing (as FTBuildBeatWorld: loading the old map into the package clashes with the new world).
	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildArenaWorld: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(*MapPath);

	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, *MapName, Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildArenaWorld: couldn't create the world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->DefaultGameMode = AFTArenaGameMode::StaticClass();
	}

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Only the player start (where the game mode puts the player for the default stage's ring; the pawn spawns here
	// first). The stage, its light and its sky are built at run time from Data/Battle/arenas.json.
	APlayerStart* Start = World->SpawnActor<APlayerStart>(
		AFTArenaGameMode::PlayerStartFor(AFTArenaGameMode::MapRingRadius) + FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
	Start->SetActorLabel(TEXT("PlayerStart"));

	const bool bSaved = SaveArenaPackage(Package, World, File);

	World->DestroyWorld(false);

	if (!bSaved)
	{
		return 1;
	}
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildArenaWorld needs an editor build."));
	return 1;
#endif
}
