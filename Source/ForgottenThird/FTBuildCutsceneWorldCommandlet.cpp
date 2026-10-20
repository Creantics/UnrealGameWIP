#include "FTBuildCutsceneWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "CineCameraActor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "FTCutsceneDirector.h"
#include "FTCutsceneGameMode.h"
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
const TCHAR* const kCutsceneMapFolder = TEXT("/Game/Maps");

#if WITH_EDITOR
// A tint of the basic shape material, saved inside the map package.
UMaterialInterface* MakeCutsceneTint(UPackage* Package, UMaterialInterface* Base, const TCHAR* Name, const FLinearColor& Color)
{
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, Name, RF_Public);
	Mic->SetParentEditorOnly(Base);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), Color);
	Mic->PostEditChange();
	return Mic;
}

AStaticMeshActor* CutsceneBlock(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Mesh, UMaterialInterface* Material,
	const FString& Label, const FVector& Location, const FVector& Scale, const FRotator& Rotation = FRotator::ZeroRotator)
{
	AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(Location, Rotation, Spawn);
	Prop->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Prop->GetStaticMeshComponent()->SetMaterial(0, Material);
	Prop->SetActorScale3D(Scale);
	Prop->SetActorLabel(Label);
	return Prop;
}

// Metres to centimetres, like the Core cutscene math (z up).
FVector CutsceneM(float X, float Y, float Z = 0.f)
{
	return FVector(X * 100.f, Y * 100.f, Z * 100.f);
}
#endif
} // namespace

UFTBuildCutsceneWorldCommandlet::UFTBuildCutsceneWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildCutsceneWorldCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString MapName = TEXT("Cutscene");
	FParse::Value(*Params, TEXT("Map="), MapName);
	if (MapName.IsEmpty() || !FName::IsValidXName(MapName, INVALID_LONGPACKAGE_CHARACTERS))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCutsceneWorld: '%s' is not a valid map name."), *MapName);
		return 1;
	}
	const FString MapPath = FString::Printf(TEXT("%s/%s"), kCutsceneMapFolder, *MapName);

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Cube || !Sphere || !Cylinder || !Base)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCutsceneWorld: a /Engine/BasicShapes asset is missing."));
		return 1;
	}

	// Start from nothing: loading the old map into the package would clash with the new world (a
	// rerun crashed on a duplicate WorldSettings in the stub builder before this).
	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCutsceneWorld: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(*MapPath);

	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, *MapName, Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCutsceneWorld: couldn't create the world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->DefaultGameMode = AFTCutsceneGameMode::StaticClass();
	}

	UMaterialInterface* Floor = MakeCutsceneTint(Package, Base, TEXT("MI_Cutscene_Floor"), FLinearColor(0.35f, 0.35f, 0.37f));
	UMaterialInterface* Wall = MakeCutsceneTint(Package, Base, TEXT("MI_Cutscene_Wall"), FLinearColor(0.5f, 0.47f, 0.43f));
	UMaterialInterface* Stone = MakeCutsceneTint(Package, Base, TEXT("MI_Cutscene_Stone"), FLinearColor(0.6f, 0.6f, 0.62f));
	UMaterialInterface* Wood = MakeCutsceneTint(Package, Base, TEXT("MI_Cutscene_Wood"), FLinearColor(0.35f, 0.24f, 0.14f));
	UMaterialInterface* SubjectTints[] = {
		MakeCutsceneTint(Package, Base, TEXT("MI_Cutscene_SubjA"), FLinearColor(0.2f, 0.35f, 0.75f)),
		MakeCutsceneTint(Package, Base, TEXT("MI_Cutscene_SubjB"), FLinearColor(0.75f, 0.25f, 0.2f)),
		MakeCutsceneTint(Package, Base, TEXT("MI_Cutscene_SubjC"), FLinearColor(0.25f, 0.6f, 0.3f)),
	};

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// The set: a 20 m floor (top at z = 0, x and y in -10..10), a back wall at x = -8, four pillars and
	// a few boxes, kept off the sight lines from the spots to the subjects.
	CutsceneBlock(World, Spawn, Cube, Floor, TEXT("Floor"), CutsceneM(0, 0, -0.5f), FVector(20, 20, 1));
	CutsceneBlock(World, Spawn, Cube, Wall, TEXT("BackWall"), CutsceneM(-8, 0, 2.5f), FVector(0.5f, 16, 5));
	const FVector2D Pillars[] = {{-6.f, -6.f}, {-6.f, 6.f}, {5.f, -7.f}, {5.f, 7.f}};
	int32 Index = 0;
	for (const FVector2D& P : Pillars)
	{
		++Index;
		CutsceneBlock(World, Spawn, Cylinder, Stone, FString::Printf(TEXT("Pillar_%d"), Index),
			CutsceneM(static_cast<float>(P.X), static_cast<float>(P.Y), 2.5f), FVector(0.8f, 0.8f, 5));
	}
	CutsceneBlock(World, Spawn, Cube, Wood, TEXT("Table"), CutsceneM(2.5f, 3.f, 0.4f), FVector(1.2f, 0.8f, 0.8f));
	CutsceneBlock(World, Spawn, Cube, Wood, TEXT("Box_1"), CutsceneM(-4.f, -4.f, 0.5f), FVector(1.f), FRotator(0, 20, 0));
	CutsceneBlock(World, Spawn, Cube, Wood, TEXT("Box_2"), CutsceneM(-3.5f, 4.5f, 0.75f), FVector(1.5f, 1.f, 1.5f), FRotator(0, -10, 0));
	CutsceneBlock(World, Spawn, Cube, Wood, TEXT("Box_3"), CutsceneM(1.f, -5.5f, 0.4f), FVector(0.8f), FRotator(0, 35, 0));

	// Subjects: capsule placeholders, the body tagged with the id. The body is 1.45 m tall with its
	// bottom on the floor (the director stands a subject at the bottom of its bounds); the head's
	// centre is at 1.6 m, the height a close shot looks at.
	struct FSubject
	{
		const TCHAR* Id;
		float X, Y;
	};
	const FSubject Subjects[] = {{TEXT("subj_a"), -1.f, -1.2f}, {TEXT("subj_b"), -1.f, 1.2f}, {TEXT("subj_c"), 2.5f, 2.f}};
	Index = 0;
	for (const FSubject& Def : Subjects)
	{
		UMaterialInterface* Tint = SubjectTints[Index++];
		AStaticMeshActor* Body = CutsceneBlock(World, Spawn, Cylinder, Tint, Def.Id, CutsceneM(Def.X, Def.Y, 0.725f), FVector(0.5f, 0.5f, 1.45f));
		Body->Tags.Add(FName(Def.Id));
		AStaticMeshActor* Head = CutsceneBlock(World, Spawn, Sphere, Tint, FString::Printf(TEXT("%s_head"), Def.Id),
			CutsceneM(Def.X, Def.Y, 1.6f), FVector(0.28f));
		Head->AttachToActor(Body, FAttachmentTransformRules::KeepWorldTransform);
	}

	// Camera spots: where a wide shot stands; the other framings take their direction from them.
	struct FSpot
	{
		const TCHAR* Id;
		float X, Y, Z;
	};
	const FSpot Spots[] = {
		{TEXT("spot_a"), 9.f, 0.f, 2.5f},  // front, high: the establishing view
		{TEXT("spot_b"), 6.f, 5.f, 1.7f},  // front right
		{TEXT("spot_c"), 3.f, -5.f, 1.6f}, // right side of the pair
		{TEXT("spot_d"), 3.f, 6.f, 1.6f},  // left side of the pair
		{TEXT("spot_e"), 7.f, -1.f, 1.6f}, // front, low
		{TEXT("spot_f"), 4.f, 5.f, 1.4f},  // by the table
	};
	for (const FSpot& Def : Spots)
	{
		ATargetPoint* Point = World->SpawnActor<ATargetPoint>(CutsceneM(Def.X, Def.Y, Def.Z), FRotator::ZeroRotator, Spawn);
		Point->Tags.Add(FName(Def.Id));
		Point->SetActorLabel(Def.Id);
	}

	// Light.
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 800), FRotator(-45, 30, 0), Spawn);
	Sun->GetComponent()->SetMobility(EComponentMobility::Movable);
	Sun->GetComponent()->SetIntensity(5.f);
	Sun->SetActorLabel(TEXT("Sun"));
	ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 600), FRotator::ZeroRotator, Spawn);
	Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sky->GetLightComponent()->SetIntensity(1.f);
	Sky->SetActorLabel(TEXT("SkyLight"));
	APointLight* Lamp = World->SpawnActor<APointLight>(CutsceneM(1.f, 0.f, 3.5f), FRotator::ZeroRotator, Spawn);
	Lamp->PointLightComponent->SetMobility(EComponentMobility::Movable);
	Lamp->PointLightComponent->SetIntensity(5000.f);
	Lamp->PointLightComponent->SetLightColor(FLinearColor(1.f, 0.85f, 0.65f));
	Lamp->PointLightComponent->SetAttenuationRadius(1500.f);
	Lamp->SetActorLabel(TEXT("Lamp"));

	// The director and its camera (starting at spot_a, looking at the pair).
	ACineCameraActor* Camera = World->SpawnActor<ACineCameraActor>(CutsceneM(9.f, 0.f, 2.5f), FRotator(-8.f, 180.f, 0.f), Spawn);
	Camera->SetActorLabel(TEXT("CutsceneCamera"));
	AFTCutsceneDirector* Director = World->SpawnActor<AFTCutsceneDirector>(CutsceneM(0.f, 0.f, 3.f), FRotator::ZeroRotator, Spawn);
	Director->Camera = Camera;
	Director->SetActorLabel(TEXT("CutsceneDirector"));

	// Out of every shot: the game mode's default pawn spawns here.
	APlayerStart* Start = World->SpawnActor<APlayerStart>(CutsceneM(-9.f, -9.f, 1.f), FRotator::ZeroRotator, Spawn);
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCutsceneWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCutsceneWorld: saved %s."), *File);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCutsceneWorld needs an editor build."));
	return 1;
#endif
}
