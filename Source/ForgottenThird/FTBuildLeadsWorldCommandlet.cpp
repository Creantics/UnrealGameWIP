#include "FTBuildLeadsWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
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
#include "UObject/UnrealType.h"
#endif

namespace
{
const TCHAR* const kLeadsMap = TEXT("/Game/Maps/Leads");

struct FLead
{
	const TCHAR* Label;
	const TCHAR* Folder; // under /Game/Characters
	const TCHAR* Stem;   // the glb's file name: the mesh is <stem>, the clip <stem>idle
	const TCHAR* Expression;
	float Y; // cm; the camera looks along -X, so +Y is on screen left
};

// Left to right as the camera sees them.
const FLead kLeads[] = {
	{TEXT("Duy"), TEXT("Duy"), TEXT("duy"), TEXT("neutral"), 120.f},
	{TEXT("Devin"), TEXT("Devin"), TEXT("devin"), TEXT("amused"), 0.f},
	{TEXT("Third"), TEXT("Third"), TEXT("third"), TEXT("neutral"), -120.f},
};

#if WITH_EDITOR
// A bone's reference-pose position in component space, or false when the bone isn't there.
bool LeadRefBonePosition(const FReferenceSkeleton& Skeleton, const TCHAR* Name, FVector& Out)
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

// The yaw that turns the mesh to face +X, from where its eyes sit relative to its head (the same
// rule as the demo map's).
float LeadFacingYaw(const USkeletalMesh& Mesh)
{
	FVector Head, EyeL, EyeR;
	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	if (!LeadRefBonePosition(Skeleton, TEXT("head"), Head) || !LeadRefBonePosition(Skeleton, TEXT("eye_l"), EyeL)
		|| !LeadRefBonePosition(Skeleton, TEXT("eye_r"), EyeR))
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

UFTBuildLeadsWorldCommandlet::UFTBuildLeadsWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildLeadsWorldCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	// Load everything first, so a missing character fails before the old map is touched.
	USkeletalMesh* Meshes[UE_ARRAY_COUNT(kLeads)] = {};
	UAnimSequence* Idles[UE_ARRAY_COUNT(kLeads)] = {};
	for (int32 i = 0; i < UE_ARRAY_COUNT(kLeads); ++i)
	{
		const FLead& Lead = kLeads[i];
		const FString MeshPath = FString::Printf(TEXT("/Game/Characters/%s/%s/SkeletalMeshes/%s.%s"), Lead.Folder,
			Lead.Stem, Lead.Stem, Lead.Stem);
		const FString IdlePath = FString::Printf(TEXT("/Game/Characters/%s/%s/SkeletalMeshes/%sidle.%sidle"),
			Lead.Folder, Lead.Stem, Lead.Stem, Lead.Stem);
		Meshes[i] = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
		Idles[i] = LoadObject<UAnimSequence>(nullptr, *IdlePath);
		if (!Meshes[i] || !Idles[i])
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLeadsWorld: couldn't load %s. Import the leads first (docs/LEADS.md)."),
				Meshes[i] ? *IdlePath : *MeshPath);
			return 1;
		}
	}

	// Start from nothing: loading the old map into the package would clash with the new world.
	const FString File = FPackageName::LongPackageNameToFilename(kLeadsMap, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLeadsWorld: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(kLeadsMap);

	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, TEXT("Leads"), Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLeadsWorld: couldn't create the world."));
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLeadsWorld: /Engine/BasicShapes/Cube is missing."));
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

	// Sun from the front left of the line-up, low enough to model the faces.
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 500), FRotator(-35, -150, 0), Spawn);
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

	// The leads stand on the Y axis facing +X, each looping its idle clip with one held expression.
	for (int32 i = 0; i < UE_ARRAY_COUNT(kLeads); ++i)
	{
		const FLead& Lead = kLeads[i];
		const float Yaw = LeadFacingYaw(*Meshes[i]);
		AFTCharacterPreview* Preview =
			World->SpawnActor<AFTCharacterPreview>(FVector(0, Lead.Y, 0), FRotator(0, Yaw, 0), Spawn);
		Preview->SetActorLabel(Lead.Label);
		USkeletalMeshComponent* Body = Preview->GetBody();
		Body->SetSkeletalMesh(Meshes[i]);
		Body->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		Body->AnimationData.AnimToPlay = Idles[i];
		Body->AnimationData.bSavedLooping = true;
		Body->AnimationData.bSavedPlaying = true;
		Preview->CycleExpressions = {FName(Lead.Expression)};
		Preview->bAutoCycle = true;
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildLeadsWorld: %s at y %.0f, yaw %.1f, plays %s, face %s."), Lead.Label,
			Lead.Y, Yaw, *Idles[i]->GetName(), Lead.Expression);
	}

	// A camera 4.2 m out at chest height with a 50 degree field of view takes in all three, head to
	// toe; it becomes the player's view when play starts. The player start sits just behind it.
	ACameraActor* Camera = World->SpawnActor<ACameraActor>(FVector(420, 0, 100), FRotator(-2, 180, 0), Spawn);
	Camera->GetCameraComponent()->SetFieldOfView(50.f);
	Camera->GetCameraComponent()->SetConstraintAspectRatio(false);
	// AutoActivateForPlayer is private and has no setter; it is an editable property, so set it by name.
	if (FByteProperty* Auto = FindFProperty<FByteProperty>(ACameraActor::StaticClass(), TEXT("AutoActivateForPlayer")))
	{
		*Auto->ContainerPtrToValuePtr<uint8>(Camera) = static_cast<uint8>(EAutoReceiveInput::Player0);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildLeadsWorld: ACameraActor has no AutoActivateForPlayer; the camera won't take over the view."));
	}
	Camera->SetActorLabel(TEXT("LeadsCamera"));

	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(470, 0, 100), FRotator(0, 180, 0), Spawn);
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLeadsWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildLeadsWorld: saved %s."), *File);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLeadsWorld needs an editor build."));
	return 1;
#endif
}
