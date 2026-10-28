#include "FTBuildBattleDemoCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FTBattleDemoActor.h"
#include "FTBattleGameMode.h"
#include "FTSceneLook.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

namespace
{
const TCHAR* const kBattleMap = TEXT("/Game/Maps/BattleDemo");
}

UFTBuildBattleDemoCommandlet::UFTBuildBattleDemoCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildBattleDemoCommandlet::Main(const FString& /*Params*/)
{
#if WITH_EDITOR
	// Start from nothing: loading the old map into the package would clash with the new world.
	const FString File = FPackageName::LongPackageNameToFilename(kBattleMap, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBattleDemo: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(kBattleMap);

	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, TEXT("BattleDemo"), Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBattleDemo: couldn't create the world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->DefaultGameMode = AFTBattleGameMode::StaticClass();
	}

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Floor: a 40 m x 40 m slab whose top is at z = 0.
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* Grey = FTScene::LoadSurface();
	if (!Cube)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBattleDemo: /Engine/BasicShapes/Cube is missing."));
		World->DestroyWorld(false);
		return 1;
	}
	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, -50), FRotator::ZeroRotator, Spawn);
	Floor->GetStaticMeshComponent()->SetStaticMesh(Cube);
	if (Grey)
	{
		Floor->GetStaticMeshComponent()->SetMaterial(0, Grey);
	}
	Floor->SetActorScale3D(FVector(40, 40, 1));
	Floor->SetActorLabel(TEXT("Floor"));

	// Sun from behind the camera's left shoulder, so both lines are lit from the front.
	FTScene::SpawnLook(World, Spawn, ftcore::SceneLook{}, FRotator(-40, 25, 0));

	// The battle at the origin; its own camera frames the fight once play starts.
	AFTBattleDemoActor* Battle = World->SpawnActor<AFTBattleDemoActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	Battle->SetActorLabel(TEXT("BattleDemo"));

	// The pawn spawns where the battle camera is, looking the same way.
	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(-1000, 0, 200), FRotator(0, 0, 0), Spawn);
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBattleDemo: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildBattleDemo: saved %s."), *File);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBattleDemo needs an editor build."));
	return 1;
#endif
}
