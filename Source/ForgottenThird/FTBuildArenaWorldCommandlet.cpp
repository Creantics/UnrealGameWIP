#include "FTBuildArenaWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FTArenaGameMode.h"
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
const TCHAR* const kArenaMapFolder = TEXT("/Game/Maps");

#if WITH_EDITOR
// A tint of the world surface material, saved inside the map package.
UMaterialInterface* MakeArenaTint(UPackage* Package, UMaterialInterface* Base, const TCHAR* Name, const FLinearColor& Color)
{
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, Name, RF_Public);
	Mic->SetParentEditorOnly(Base);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), Color);
	Mic->PostEditChange();
	return Mic;
}

AStaticMeshActor* ArenaBlock(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Mesh, UMaterialInterface* Material,
	const FString& Label, const FVector& Location, const FVector& Scale, const FRotator& Rotation)
{
	AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(Location, Rotation, Spawn);
	Prop->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	if (Material)
	{
		Prop->GetStaticMeshComponent()->SetMaterial(0, Material);
	}
	Prop->SetActorScale3D(Scale);
	Prop->SetActorLabel(Label);
	return Prop;
}

// Count blocks around a circle of Radius cm (Z: the blocks' centre), each Depth deep and Height tall, turned
// along the circle and overlapping their neighbours by Overlap.
void ArenaCircle(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Cube, UMaterialInterface* Material, const TCHAR* Label,
	int32 Count, double Radius, double Z, double Depth, double Height, double Overlap)
{
	const double Step = 2.0 * UE_DOUBLE_PI / static_cast<double>(Count);
	const double Chord = 2.0 * Radius * FMath::Sin(Step * 0.5) + Overlap;
	for (int32 I = 0; I < Count; ++I)
	{
		const double A = Step * static_cast<double>(I);
		const FVector At(Radius * FMath::Cos(A), Radius * FMath::Sin(A), Z);
		ArenaBlock(World, Spawn, Cube, Material, FString::Printf(TEXT("%s_%02d"), Label, I), At,
			FVector(Depth / 100.0, Chord / 100.0, Height / 100.0), FRotator(0.0, FMath::RadiansToDegrees(A), 0.0));
	}
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

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Base = FTScene::LoadSurface();
	if (!Cube || !Cylinder || !Base)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildArenaWorld: a /Engine/BasicShapes mesh or the world surface material is missing."));
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

	UMaterialInterface* Ground = MakeArenaTint(Package, Base, TEXT("MI_Arena_Ground"), FLinearColor(0.16f, 0.15f, 0.13f));
	UMaterialInterface* Stone = MakeArenaTint(Package, Base, TEXT("MI_Arena_Stone"), FLinearColor(0.42f, 0.4f, 0.36f));
	UMaterialInterface* Line = MakeArenaTint(Package, Base, TEXT("MI_Arena_Line"), FLinearColor(0.85f, 0.82f, 0.7f));
	UMaterialInterface* Wall = MakeArenaTint(Package, Base, TEXT("MI_Arena_Wall"), FLinearColor(0.3f, 0.27f, 0.24f));
	UMaterialInterface* Pillar = MakeArenaTint(Package, Base, TEXT("MI_Arena_Pillar"), FLinearColor(0.24f, 0.22f, 0.2f));

	const double Ring = static_cast<double>(AFTArenaGameMode::MapRingRadius) * 100.0;

	// The ground 60 m square, its top 2 cm under the disc's; the disc a little wider than the ring, its top at z = 0.
	ArenaBlock(World, Spawn, Cube, Ground, TEXT("Ground"), FVector(0, 0, -52), FVector(60, 60, 1), FRotator::ZeroRotator);
	const double Disc = (Ring + 120.0) * 2.0 / 100.0;
	ArenaBlock(World, Spawn, Cylinder, Stone, TEXT("Disc"), FVector(0, 0, -10), FVector(Disc, Disc, 0.2), FRotator::ZeroRotator);
	// The ring's edge painted on the disc, and a centre mark.
	ArenaCircle(World, Spawn, Cube, Line, TEXT("RingLine"), 64, Ring, 0.5, 14.0, 1.0, 2.0);
	ArenaBlock(World, Spawn, Cylinder, Line, TEXT("CentreMark"), FVector(0, 0, 0.5), FVector(0.6, 0.6, 0.01), FRotator::ZeroRotator);
	// A knee-high wall just outside the ring: nothing leaves the ring, and the camera looks over it.
	ArenaCircle(World, Spawn, Cube, Wall, TEXT("Wall"), 48, Ring + 90.0, 45.0, 40.0, 90.0, 6.0);

	// Sun, sky, air and the camera finish: the shared look.
	FTScene::SpawnLook(World, Spawn, ftcore::SceneLook{}, FRotator(-38, 35, 0));

	// Eight pillars around it, a lamp by each.
	for (int32 I = 0; I < 8; ++I)
	{
		const double A = UE_DOUBLE_PI * 0.25 * static_cast<double>(I) + UE_DOUBLE_PI / 8.0;
		const FVector At((Ring + 300.0) * FMath::Cos(A), (Ring + 300.0) * FMath::Sin(A), 0.0);
		ArenaBlock(World, Spawn, Cube, Pillar, FString::Printf(TEXT("Pillar_%d"), I), At + FVector(0, 0, 225), FVector(0.8, 0.8, 4.5),
			FRotator(0.0, FMath::RadiansToDegrees(A), 0.0));
		APointLight* Lamp = World->SpawnActor<APointLight>(At * 0.92 + FVector(0, 0, 500), FRotator::ZeroRotator, Spawn);
		Lamp->PointLightComponent->SetMobility(EComponentMobility::Movable);
		Lamp->PointLightComponent->SetIntensity(6000.f);
		Lamp->PointLightComponent->SetLightColor(FLinearColor(1.f, 0.82f, 0.6f));
		Lamp->PointLightComponent->SetAttenuationRadius(1800.f);
		Lamp->PointLightComponent->SetCastShadows(false);
		Lamp->SetActorLabel(FString::Printf(TEXT("Lamp_%d"), I));
	}

	// Where the game mode puts the player for the map's ring (the pawn spawns here first).
	APlayerStart* Start = World->SpawnActor<APlayerStart>(
		AFTArenaGameMode::PlayerStartFor(AFTArenaGameMode::MapRingRadius) + FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildArenaWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildArenaWorld: saved %s."), *File);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildArenaWorld needs an editor build."));
	return 1;
#endif
}
