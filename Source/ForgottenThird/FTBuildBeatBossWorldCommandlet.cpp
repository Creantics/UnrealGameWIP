#include "FTBuildBeatBossWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "Animation/SkeletalMeshActor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraActor.h"
#include "Components/PointLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/PointLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FTBeatBossGameMode.h"
#include "FTMeshFacing.h"
#include "FTSceneLook.h"
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
const TCHAR* const kBossMapFolder = TEXT("/Game/Maps");
const TCHAR* const kBossLeadMesh = TEXT("/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_human.test_human");

#if WITH_EDITOR
// A tint of the world surface material, saved inside the map package.
UMaterialInterface* MakeBossTint(UPackage* Package, UMaterialInterface* Base, const TCHAR* Name, const FLinearColor& Color)
{
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, Name, RF_Public);
	Mic->SetParentEditorOnly(Base);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), Color);
	Mic->PostEditChange();
	return Mic;
}

// A basic shape (100 cm across, its pivot at its centre). Movable ones are the boss's, which the game mode moves.
AStaticMeshActor* BossBlock(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Mesh, UMaterialInterface* Material,
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

UFTBuildBeatBossWorldCommandlet::UFTBuildBeatBossWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildBeatBossWorldCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString MapName = TEXT("Demo_BeatBoss");
	FParse::Value(*Params, TEXT("Map="), MapName);
	if (MapName.IsEmpty() || !FName::IsValidXName(MapName, INVALID_LONGPACKAGE_CHARACTERS))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatBossWorld: '%s' is not a valid map name."), *MapName);
		return 1;
	}
	const FString MapPath = FString::Printf(TEXT("%s/%s"), kBossMapFolder, *MapName);

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Base = FTScene::LoadSurface();
	USkeletalMesh* Human = LoadObject<USkeletalMesh>(nullptr, kBossLeadMesh);
	if (!Cube || !Cylinder || !Base || !Human)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatBossWorld: a /Engine/BasicShapes mesh, the world surface material or the test human is missing."));
		return 1;
	}

	// Start from nothing: loading the old map into the package would clash with the new world.
	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatBossWorld: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(*MapPath);

	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, *MapName, Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatBossWorld: couldn't create the world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->DefaultGameMode = AFTBeatBossGameMode::StaticClass();
	}

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// A room 20 m long and 14 m wide, walled in: the floor's top at z = 0 (x from -8 m to 14 m), walls round it
	// and two pillars either side of the boss for depth.
	// Dark and neutral: the demo stage is a closed room with a roof, so nothing of the sky shows and the light on the
	// fighters is the lamps' alone (docs/BEAT_BOSS_DEMO.md, "The stage").
	UMaterialInterface* Floor = MakeBossTint(Package, Base, TEXT("MI_BeatBoss_Floor"), FLinearColor(0.07f, 0.065f, 0.075f));
	UMaterialInterface* Stone = MakeBossTint(Package, Base, TEXT("MI_BeatBoss_Stone"), FLinearColor(0.085f, 0.08f, 0.1f));
	BossBlock(World, Spawn, Cube, Floor, TEXT("Floor"), FVector(300, 0, -50), FVector(22, 16, 1), FRotator::ZeroRotator, false);
	BossBlock(World, Spawn, Cube, Stone, TEXT("Wall"), FVector(1300, 0, 750), FVector(1, 16, 16), FRotator::ZeroRotator, false);
	BossBlock(World, Spawn, Cube, Stone, TEXT("WallBehindLead"), FVector(-700, 0, 750), FVector(1, 16, 16), FRotator::ZeroRotator, false);
	BossBlock(World, Spawn, Cube, Stone, TEXT("WallNear"), FVector(300, 720, 750), FVector(21, 1, 16), FRotator::ZeroRotator, false);
	BossBlock(World, Spawn, Cube, Stone, TEXT("WallFar"), FVector(300, -720, 750), FVector(21, 1, 16), FRotator::ZeroRotator, false);
	BossBlock(World, Spawn, Cube, Stone, TEXT("Roof"), FVector(300, 0, 1600), FVector(24, 18, 1), FRotator::ZeroRotator, false);
	BossBlock(World, Spawn, Cylinder, Stone, TEXT("PillarLeft"), FVector(900, -520, 300), FVector(1.1f, 1.1f, 6.f), FRotator::ZeroRotator, false);
	BossBlock(World, Spawn, Cylinder, Stone, TEXT("PillarRight"), FVector(900, 560, 300), FVector(1.1f, 1.1f, 6.f), FRotator::ZeroRotator, false);

	// The air and the camera finish, with no sun, atmosphere or clouds: a very low sky light (the closed room's own
	// bounce), manual exposure so the per-shot lens never changes the brightness, a little bloom and vignette. The game
	// mode sets the same on the camera, which wins over this volume.
	{
		ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 800), FRotator::ZeroRotator, Spawn);
		USkyLightComponent* SkyLight = Sky->GetLightComponent();
		SkyLight->SetMobility(EComponentMobility::Movable);
		SkyLight->bRealTimeCapture = true;
		SkyLight->SetIntensity(0.35f);
		Sky->SetActorLabel(TEXT("SkyLight"));
		APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>(FVector(0, 0, 200), FRotator::ZeroRotator, Spawn);
		Volume->bUnbound = true;
		Volume->Priority = 0.f;
		FPostProcessSettings& Finish = Volume->Settings;
		Finish.bOverride_AutoExposureMethod = true;
		Finish.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
		Finish.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		Finish.AutoExposureApplyPhysicalCameraExposure = false;
		Finish.bOverride_AutoExposureBias = true;
		Finish.AutoExposureBias = AFTBeatBossGameMode::StageExposure();
		Finish.bOverride_BloomIntensity = true;
		Finish.BloomIntensity = 0.35f;
		Finish.bOverride_VignetteIntensity = true;
		Finish.VignetteIntensity = 0.4f;
		Finish.bOverride_FilmGrainIntensity = true;
		Finish.FilmGrainIntensity = 0.f;
		Volume->SetActorLabel(TEXT("PostProcessVolume"));
	}

	// Light: a warm key over the fighters (the only light that casts a shadow), a red rim light behind the boss and a cold
	// one behind the lead, and a kicker from above and behind the boss. The game mode scales them apart for each shot
	// (the key from the rims), so each is tagged.
	const auto Light = [&](const FVector& At, float Intensity, const FLinearColor& Color, float Radius, const TCHAR* Label, FName Tag, bool bShadow) {
		APointLight* Lamp = World->SpawnActor<APointLight>(At, FRotator::ZeroRotator, Spawn);
		Lamp->PointLightComponent->SetMobility(EComponentMobility::Movable);
		Lamp->PointLightComponent->SetIntensity(Intensity);
		Lamp->PointLightComponent->SetLightColor(Color);
		Lamp->PointLightComponent->SetAttenuationRadius(Radius);
		Lamp->PointLightComponent->SetCastShadows(bShadow);
		Lamp->SetActorLabel(Label);
		Lamp->Tags.Add(Tag);
	};
	Light(FVector(-60, 320, 560), 90000.f, FLinearColor(1.f, 0.84f, 0.7f), 2600.f, TEXT("Key"), AFTBeatBossGameMode::KeyTag(), true);
	Light(FVector(560, 0, 260), 22000.f, FLinearColor(1.f, 0.22f, 0.1f), 2000.f, TEXT("RimBoss"), AFTBeatBossGameMode::RimBossTag(), false);
	Light(FVector(-420, -60, 240), 22000.f, FLinearColor(0.3f, 0.5f, 1.f), 2000.f, TEXT("RimLead"), AFTBeatBossGameMode::RimLeadTag(), false);
	Light(FVector(900, -120, 620), 26000.f, FLinearColor(0.9f, 0.85f, 0.8f), 2200.f, TEXT("Kicker"), AFTBeatBossGameMode::KickerTag(), false);

	// The lead: the test human at the near end, turned to face the boss (+x). The boss is the same body, 1.35 times the
	// size, turned to face back (-x); the game mode gives it a dark red hide and plays the fight clips on both. The
	// game mode also places them (AFTBeatBossGameMode::LeadLocation / BossLocation) and makes any missing one.
	const auto Fighter = [&](const FVector& Location, double Yaw, double Scale, const TCHAR* Label, FName Tag) {
		ASkeletalMeshActor* Actor = World->SpawnActor<ASkeletalMeshActor>(Location, FRotator(0.0, Yaw, 0.0), Spawn);
		USkeletalMeshComponent* Skin = Actor->GetSkeletalMeshComponent();
		Skin->SetMobility(EComponentMobility::Movable);
		Skin->SetSkeletalMesh(Human);
		Skin->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Human), 0.0));
		Actor->SetActorScale3D(FVector(Scale));
		Actor->SetActorLabel(Label);
		Actor->Tags.Add(Tag);
		return Actor;
	};
	Fighter(AFTBeatBossGameMode::LeadLocation(), 0.0, 1.0, TEXT("Lead"), AFTBeatBossGameMode::LeadTag());
	Fighter(AFTBeatBossGameMode::BossLocation(), 180.0, AFTBeatBossGameMode::BossScaleFactor, TEXT("Boss"), AFTBeatBossGameMode::BossTag());

	// The view: the wide shot from the side; the game mode makes it the view target and moves it with the shots.
	ACameraActor* Camera = World->SpawnActor<ACameraActor>(AFTBeatBossGameMode::ViewerLocation(), AFTBeatBossGameMode::ViewerRotation(), Spawn);
	Camera->SetActorLabel(TEXT("Camera"));

	// Behind the camera: the game mode's default pawn spawns here.
	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(-700, 0, 100), FRotator::ZeroRotator, Spawn);
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatBossWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildBeatBossWorld: saved %s."), *File);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildBeatBossWorld needs an editor build."));
	return 1;
#endif
}
