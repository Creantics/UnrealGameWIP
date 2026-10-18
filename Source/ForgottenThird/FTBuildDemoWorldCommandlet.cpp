#include "FTBuildDemoWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FTCharacterPreview.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

namespace
{
const TCHAR* const kDemoMap = TEXT("/Game/Maps/Demo");

#if WITH_EDITOR
// A bone's reference-pose position in component space, or false when the bone isn't there.
bool RefBonePosition(const FReferenceSkeleton& Skeleton, const TCHAR* Name, FVector& Out)
{
	int32 Bone = Skeleton.FindBoneIndex(Name);
	if (Bone == INDEX_NONE)
	{
		return false;
	}
	FTransform Pose = FTransform::Identity;
	for (; Bone != INDEX_NONE; Bone = Skeleton.GetParentIndex(Bone))
	{
		Pose = Pose * Skeleton.GetRefBonePose()[Bone];
	}
	Out = Pose.GetLocation();
	return true;
}

// The yaw that turns the mesh to face +X, from where its eyes sit relative to its head. Zero when
// the mesh has no eye or head bones.
float FacingYaw(const USkeletalMesh& Mesh)
{
	FVector Head, EyeL, EyeR;
	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	if (!RefBonePosition(Skeleton, TEXT("head"), Head) || !RefBonePosition(Skeleton, TEXT("eye_l"), EyeL)
		|| !RefBonePosition(Skeleton, TEXT("eye_r"), EyeR))
	{
		return 0.f;
	}
	FVector Forward = (EyeL + EyeR) * 0.5f - Head;
	Forward.Z = 0;
	if (Forward.IsNearlyZero())
	{
		return 0.f;
	}
	return -FMath::RadiansToDegrees(FMath::Atan2(Forward.Y, Forward.X));
}
#endif
}

UFTBuildDemoWorldCommandlet::UFTBuildDemoWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildDemoWorldCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	// Start from nothing: loading the old map into the package would clash with the new world.
	const FString File = FPackageName::LongPackageNameToFilename(kDemoMap, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(kDemoMap);

	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, TEXT("Demo"), Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: couldn't create the world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Floor: a 40 m x 40 m slab whose top is at z = 0.
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* Grey = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Cube)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: /Engine/BasicShapes/Cube is missing."));
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

	// Sun, low enough to rake across the face.
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 500), FRotator(-35, -40, 0), Spawn);
	Sun->GetComponent()->SetMobility(EComponentMobility::Movable);
	Sun->GetComponent()->SetIntensity(6.f);
	if (UDirectionalLightComponent* SunLight = Cast<UDirectionalLightComponent>(Sun->GetComponent()))
	{
		SunLight->SetAtmosphereSunLight(true);
	}
	Sun->SetActorLabel(TEXT("Sun"));

	ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 400), FRotator::ZeroRotator, Spawn);
	Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sky->GetLightComponent()->bRealTimeCapture = true;
	Sky->SetActorLabel(TEXT("SkyLight"));

	ASkyAtmosphere* Atmosphere = World->SpawnActor<ASkyAtmosphere>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	Atmosphere->SetActorLabel(TEXT("SkyAtmosphere"));

	AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(FVector(0, 0, -100), FRotator::ZeroRotator, Spawn);
	Fog->SetActorLabel(TEXT("HeightFog"));

	// The character stands at the origin; the player starts 2.5 m in front of it at eye height,
	// looking back along -X.
	AFTCharacterPreview* Preview = World->SpawnActor<AFTCharacterPreview>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	Preview->SetActorLabel(TEXT("CharacterPreview"));

	// -Mesh=<object path> puts a skeletal mesh on the preview and turns it to face the player.
	FString MeshPath;
	if (FParse::Value(*Params, TEXT("Mesh="), MeshPath))
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
		if (!Mesh)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: couldn't load skeletal mesh %s."), *MeshPath);
			World->DestroyWorld(false);
			return 1;
		}
		Preview->GetBody()->SetSkeletalMesh(Mesh);
		const float Yaw = FacingYaw(*Mesh);
		Preview->SetActorRotation(FRotator(0, Yaw, 0));
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildDemoWorld: preview uses %s, yaw %.1f."), *Mesh->GetPathName(), Yaw);
	}

	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(250, 0, 160), FRotator(0, 180, 0), Spawn);
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildDemoWorld: saved %s."), *File);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld needs an editor build."));
	return 1;
#endif
}
