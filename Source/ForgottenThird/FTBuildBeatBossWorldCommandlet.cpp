#include "FTBuildBeatBossWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "Animation/SkeletalMeshActor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraActor.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/PointLight.h"
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
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Base = FTScene::LoadSurface();
	USkeletalMesh* Human = LoadObject<USkeletalMesh>(nullptr, kBossLeadMesh);
	if (!Cube || !Sphere || !Cylinder || !Base || !Human)
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

	// A room 22 m deep and 16 m wide: the floor's top at z = 0 (x from -8 m to 14 m), a back wall at x = 13 m
	// and two pillars either side of the boss for depth.
	UMaterialInterface* Floor = MakeBossTint(Package, Base, TEXT("MI_BeatBoss_Floor"), FLinearColor(0.32f, 0.31f, 0.33f));
	UMaterialInterface* Stone = MakeBossTint(Package, Base, TEXT("MI_BeatBoss_Stone"), FLinearColor(0.2f, 0.21f, 0.25f));
	BossBlock(World, Spawn, Cube, Floor, TEXT("Floor"), FVector(300, 0, -50), FVector(22, 16, 1), FRotator::ZeroRotator, false);
	BossBlock(World, Spawn, Cube, Stone, TEXT("Wall"), FVector(1300, 0, 350), FVector(1, 16, 8), FRotator::ZeroRotator, false);
	BossBlock(World, Spawn, Cylinder, Stone, TEXT("PillarLeft"), FVector(900, -520, 300), FVector(1.1f, 1.1f, 6.f), FRotator::ZeroRotator, false);
	BossBlock(World, Spawn, Cylinder, Stone, TEXT("PillarRight"), FVector(900, 560, 300), FVector(1.1f, 1.1f, 6.f), FRotator::ZeroRotator, false);

	// Sun, sky, air and the camera finish: the shared photoreal look.
	FTScene::SpawnLook(World, Spawn, ftcore::SceneLook{}, FRotator(-40, 25, 0));

	APointLight* Lamp = World->SpawnActor<APointLight>(FVector(300, 120, 480), FRotator::ZeroRotator, Spawn);
	Lamp->PointLightComponent->SetMobility(EComponentMobility::Movable);
	Lamp->PointLightComponent->SetIntensity(6000.f);
	Lamp->PointLightComponent->SetLightColor(FLinearColor(1.f, 0.82f, 0.7f));
	Lamp->PointLightComponent->SetAttenuationRadius(1800.f);
	Lamp->SetActorLabel(TEXT("Lamp"));

	// The lead: the test human at the near end, turned to face the boss (+x).
	ASkeletalMeshActor* Lead = World->SpawnActor<ASkeletalMeshActor>(AFTBeatBossGameMode::LeadLocation(), FRotator::ZeroRotator, Spawn);
	USkeletalMeshComponent* Skin = Lead->GetSkeletalMeshComponent();
	Skin->SetMobility(EComponentMobility::Movable);
	Skin->SetSkeletalMesh(Human);
	Skin->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Human), 0.0));
	Lead->SetActorLabel(TEXT("Lead"));
	Lead->Tags.Add(AFTBeatBossGameMode::LeadTag());

	// The boss: a body of basic shapes facing the lead (-x), every part attached to the tagged body. The game
	// mode gives the body and its limbs one material it tints; the visor keeps its own.
	const FVector Body = AFTBeatBossGameMode::BossLocation();
	const double Width = AFTBeatBossGameMode::BossWidth;
	const double Height = AFTBeatBossGameMode::BossHeight;
	const FRotator Facing(0.0, 180.0, 0.0);
	UMaterialInterface* Hide = MakeBossTint(Package, Base, TEXT("MI_BeatBoss_Body"), FLinearColor(0.16f, 0.15f, 0.21f));
	UMaterialInterface* Visor = MakeBossTint(Package, Base, TEXT("MI_BeatBoss_Visor"), FLinearColor(1.f, 0.55f, 0.1f));
	AStaticMeshActor* Boss = BossBlock(World, Spawn, Cylinder, Hide, TEXT("Boss"), Body, FVector(Width / 100.0, Width / 100.0, Height / 100.0), Facing, true);
	Boss->Tags.Add(AFTBeatBossGameMode::BossTag());
	const double Top = Body.Z + Height * 0.5;
	const auto Part = [&](UStaticMesh* Mesh, UMaterialInterface* Material, const TCHAR* Label, const FVector& Offset, const FVector& Scale,
						  const FRotator& Rotation) {
		AStaticMeshActor* Piece = BossBlock(World, Spawn, Mesh, Material, FString(TEXT("Boss_")) + Label, Offset, Scale, Rotation, true);
		Piece->AttachToActor(Boss, FAttachmentTransformRules::KeepWorldTransform);
		return Piece;
	};
	const double FrontX = Body.X - Width * 0.5;
	Part(Sphere, Hide, TEXT("head"), FVector(Body.X, 0.0, Top + 62.0), FVector(1.5), Facing);
	AStaticMeshActor* Slit = Part(Cube, Visor, TEXT("visor"), FVector(Body.X - 66.0, 0.0, Top + 70.0), FVector(0.2, 1.0, 0.16), Facing);
	Slit->Tags.Add(TEXT("BeatBossVisor"));
	for (const double Side : {-1.0, 1.0})
	{
		const double Y = Side * (Width * 0.5 + 45.0);
		Part(Sphere, Hide, Side < 0.0 ? TEXT("shoulder_l") : TEXT("shoulder_r"), FVector(Body.X, Y, Top - 30.0), FVector(1.1), Facing);
		// Arms hang forward a little, towards the lead.
		Part(Cube, Hide, Side < 0.0 ? TEXT("arm_l") : TEXT("arm_r"), FVector(Body.X - 40.0, Y * 1.08, Top - 170.0), FVector(0.6, 0.6, 2.4),
			FRotator(-14.0, 180.0, 0.0));
		Part(Cube, Hide, Side < 0.0 ? TEXT("fist_l") : TEXT("fist_r"), FVector(FrontX + 40.0, Y * 1.1, Top - 300.0), FVector(0.9), Facing);
	}

	// The view: behind the lead's right shoulder, looking past it at the boss; the game mode makes it the view
	// target.
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
