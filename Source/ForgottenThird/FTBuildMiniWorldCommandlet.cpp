#include "FTBuildMiniWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraActor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FTMiniGameMode.h"
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
const TCHAR* const kMiniMapFolder = TEXT("/Game/Maps");
}

UFTBuildMiniWorldCommandlet::UFTBuildMiniWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildMiniWorldCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString MapName = TEXT("Mini");
	FParse::Value(*Params, TEXT("Map="), MapName);
	if (MapName.IsEmpty() || !FName::IsValidXName(MapName, INVALID_LONGPACKAGE_CHARACTERS))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniWorld: '%s' is not a valid map name."), *MapName);
		return 1;
	}
	const FString MapPath = FString::Printf(TEXT("%s/%s"), kMiniMapFolder, *MapName);

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* Grey = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Cube)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniWorld: /Engine/BasicShapes/Cube is missing."));
		return 1;
	}

	// Start from nothing: loading the old map into the package would clash with the new world (a
	// rerun crashed on a duplicate WorldSettings before this).
	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniWorld: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(*MapPath);

	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, *MapName, Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniWorld: couldn't create the world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->DefaultGameMode = AFTMiniGameMode::StaticClass();
	}

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// A room 12 m wide: the floor's top at z = 0 and a back wall 6 m ahead (+X) of the camera.
	const auto Slab = [&](const TCHAR* Label, const FVector& Location, const FVector& Scale)
	{
		AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator, Spawn);
		Prop->GetStaticMeshComponent()->SetStaticMesh(Cube);
		if (Grey)
		{
			Prop->GetStaticMeshComponent()->SetMaterial(0, Grey);
		}
		Prop->SetActorScale3D(Scale);
		Prop->SetActorLabel(Label);
	};
	Slab(TEXT("Floor"), FVector(300, 0, -50), FVector(12, 12, 1));
	Slab(TEXT("Wall"), FVector(900, 0, 250), FVector(1, 12, 6));

	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 600), FRotator(-40, 25, 0), Spawn);
	Sun->GetComponent()->SetMobility(EComponentMobility::Movable);
	Sun->GetComponent()->SetIntensity(5.f);
	Sun->SetActorLabel(TEXT("Sun"));

	ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 400), FRotator::ZeroRotator, Spawn);
	Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sky->GetLightComponent()->SetIntensity(1.f);
	Sky->SetActorLabel(TEXT("SkyLight"));

	APointLight* Lamp = World->SpawnActor<APointLight>(FVector(500, 0, 300), FRotator::ZeroRotator, Spawn);
	Lamp->PointLightComponent->SetMobility(EComponentMobility::Movable);
	Lamp->PointLightComponent->SetIntensity(4000.f);
	Lamp->PointLightComponent->SetLightColor(FLinearColor(1.f, 0.85f, 0.65f));
	Lamp->PointLightComponent->SetAttenuationRadius(1200.f);
	Lamp->SetActorLabel(TEXT("Lamp"));

	// The view: the camera faces the wall; AFTMiniGameMode makes it the view target.
	ACameraActor* Camera = World->SpawnActor<ACameraActor>(FVector(0, 0, 150), FRotator::ZeroRotator, Spawn);
	Camera->SetActorLabel(TEXT("Camera"));

	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(-100, 0, 100), FRotator::ZeroRotator, Spawn);
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMiniWorld: saved %s."), *File);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMiniWorld needs an editor build."));
	return 1;
#endif
}
