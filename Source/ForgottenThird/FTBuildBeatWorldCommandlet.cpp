#include "FTBuildBeatWorldCommandlet.h"

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
#include "FTBeatGameMode.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

namespace
{
const TCHAR* const kBeatMapFolder = TEXT("/Game/Maps");

#if WITH_EDITOR
// A tint of the basic shape material, saved inside the map package.
UMaterialInterface* MakeBeatTint(UPackage* Package, UMaterialInterface* Base, const TCHAR* Name, const FLinearColor& Color)
{
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, Name, RF_Public);
	Mic->SetParentEditorOnly(Base);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), Color);
	Mic->PostEditChange();
	return Mic;
}

// A basic shape in the room. Movable ones are what the game mode seats and hides (the stand-ins).
AStaticMeshActor* BeatBlock(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Mesh, UMaterialInterface* Material,
	const FString& Label, const FVector& Location, const FVector& Scale, const FRotator& Rotation, bool bMovable)
{
	AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(Location, Rotation, Spawn);
	Prop->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	if (bMovable)
	{
		// Before anything attaches to it: a static part can't ride a movable one.
		Prop->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	}
	if (Material)
	{
		Prop->GetStaticMeshComponent()->SetMaterial(0, Material);
	}
	Prop->SetActorScale3D(Scale);
	Prop->SetActorLabel(Label);
	return Prop;
}
#endif
} // namespace

UFTBuildBeatWorldCommandlet::UFTBuildBeatWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildBeatWorldCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString MapName = TEXT("Beat");
	FParse::Value(*Params, TEXT("Map="), MapName);
	if (MapName.IsEmpty() || !FName::IsValidXName(MapName, INVALID_LONGPACKAGE_CHARACTERS))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatWorld: '%s' is not a valid map name."), *MapName);
		return 1;
	}
	const FString MapPath = FString::Printf(TEXT("%s/%s"), kBeatMapFolder, *MapName);

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Cube || !Sphere || !Cylinder || !Base)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatWorld: a /Engine/BasicShapes asset is missing."));
		return 1;
	}

	// Start from nothing: loading the old map into the package would clash with the new world (a rerun
	// crashed on a duplicate WorldSettings before this).
	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatWorld: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(*MapPath);

	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, *MapName, Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatWorld: couldn't create the world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->DefaultGameMode = AFTBeatGameMode::StaticClass();
	}

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// A room 16 m deep and 12 m wide: the floor's top at z = 0 (x from -6 m to 10 m), a back wall at x = 9 m.
	BeatBlock(World, Spawn, Cube, Base, TEXT("Floor"), FVector(200, 0, -50), FVector(16, 12, 1), FRotator::ZeroRotator, false);
	BeatBlock(World, Spawn, Cube, Base, TEXT("Wall"), FVector(900, 0, 250), FVector(1, 12, 6), FRotator::ZeroRotator, false);

	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 600), FRotator(-40, 25, 0), Spawn);
	Sun->GetComponent()->SetMobility(EComponentMobility::Movable);
	Sun->GetComponent()->SetIntensity(5.f);
	Sun->SetActorLabel(TEXT("Sun"));

	ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 400), FRotator::ZeroRotator, Spawn);
	Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sky->GetLightComponent()->SetIntensity(1.f);
	Sky->SetActorLabel(TEXT("SkyLight"));

	APointLight* Lamp = World->SpawnActor<APointLight>(FVector(350, 0, 320), FRotator::ZeroRotator, Spawn);
	Lamp->PointLightComponent->SetMobility(EComponentMobility::Movable);
	Lamp->PointLightComponent->SetIntensity(4000.f);
	Lamp->PointLightComponent->SetLightColor(FLinearColor(1.f, 0.85f, 0.65f));
	Lamp->PointLightComponent->SetAttenuationRadius(1400.f);
	Lamp->SetActorLabel(TEXT("Lamp"));

	// The stand-ins, one per enemy a fight can have, each in its own tint, in the arc a full line-up
	// stands in. The game mode finds each by its tag, seats as many as the fight has enemies (the same arc,
	// centred on the camera) and hides the rest. The body is 150 cm tall and stands on the floor; the head
	// sits on it and the visor on its front shows which way it faces.
	const FLinearColor BodyColors[AFTBeatGameMode::MaxStandIns] = {
		FLinearColor(0.55f, 0.25f, 0.22f), FLinearColor(0.22f, 0.4f, 0.55f), FLinearColor(0.4f, 0.3f, 0.55f), FLinearColor(0.5f, 0.45f, 0.2f)};
	UMaterialInterface* Visor = MakeBeatTint(Package, Base, TEXT("MI_Beat_Visor"), FLinearColor(0.04f, 0.04f, 0.05f));
	for (int32 I = 0; I < AFTBeatGameMode::MaxStandIns; ++I)
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		AFTBeatGameMode::StandInPlace(I, AFTBeatGameMode::MaxStandIns, Location, Rotation);
		const FString Number = FString::FromInt(I + 1);
		UMaterialInterface* Tint = MakeBeatTint(Package, Base, *FString::Printf(TEXT("MI_Beat_Enemy%d"), I + 1), BodyColors[I]);

		AStaticMeshActor* Body = BeatBlock(World, Spawn, Cylinder, Tint, TEXT("Enemy_") + Number, Location, FVector(0.7f, 0.7f, 1.5f), Rotation, true);
		Body->Tags.Add(AFTBeatGameMode::StandInTag(I));
		AStaticMeshActor* Head = BeatBlock(World, Spawn, Sphere, Tint, TEXT("Enemy_") + Number + TEXT("_head"),
			Location + FVector(0.0, 0.0, 93.0), FVector(0.42f), Rotation, true);
		Head->AttachToActor(Body, FAttachmentTransformRules::KeepWorldTransform);
		AStaticMeshActor* Face = BeatBlock(World, Spawn, Cube, Visor, TEXT("Enemy_") + Number + TEXT("_visor"),
			Location + Rotation.RotateVector(FVector(24.0, 0.0, 96.0)), FVector(0.12f, 0.34f, 0.1f), Rotation, true);
		Face->AttachToActor(Body, FAttachmentTransformRules::KeepWorldTransform);
	}

	// The view: the camera looks down the room from its near end; AFTBeatGameMode makes it the view target.
	ACameraActor* Camera = World->SpawnActor<ACameraActor>(AFTBeatGameMode::ViewerLocation(), AFTBeatGameMode::ViewerRotation(), Spawn);
	Camera->SetActorLabel(TEXT("Camera"));

	// Behind the camera: the game mode's default pawn spawns here.
	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(-450, 0, 100), FRotator::ZeroRotator, Spawn);
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildBeatWorld: saved %s."), *File);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatWorld needs an editor build."));
	return 1;
#endif
}
