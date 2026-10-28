#include "FTBuildExploreWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Builders/CubeBuilder.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FTExploreDirector.h"
#include "FTExploreGameMode.h"
#include "FTExploreSpot.h"
#include "FTSceneLook.h"
#include "FTStyleController.h"
#include "FTStyleParameters.h"
#include "FTWorldMapBuilder.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Components/BrushComponent.h"
#include "Engine/Polys.h"
#include "Model.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/ExploreDemo.h"
#endif

namespace
{
const TCHAR* const kExploreMapFolder = TEXT("/Game/Maps");

#if WITH_EDITOR
// A tint of the world surface material, saved inside the map package; a Roughness of 0 or more replaces the
// master's.
UMaterialInterface* MakeTint(UPackage* Package, UMaterialInterface* Base, const TCHAR* Name, const FLinearColor& Color,
	float Roughness = -1.f)
{
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, Name, RF_Public);
	Mic->SetParentEditorOnly(Base);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), Color);
	if (Roughness >= 0.f)
	{
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), Roughness);
	}
	Mic->PostEditChange();
	return Mic;
}

AStaticMeshActor* Block(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Mesh, UMaterialInterface* Material,
	const FString& Label, const FVector& Location, const FVector& Scale, const FRotator& Rotation = FRotator::ZeroRotator)
{
	AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(Location, Rotation, Spawn);
	Prop->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Prop->GetStaticMeshComponent()->SetMaterial(0, Material);
	Prop->SetActorScale3D(Scale);
	Prop->SetActorLabel(Label);
	return Prop;
}

// The layout is in metres on the ground plane, like ftcore/ExploreDemo.h; z in metres too.
FVector M(float X, float Y, float Z = 0.f)
{
	return FVector(X * 100.f, Y * 100.f, Z * 100.f);
}

FRotator Yaw(float Degrees)
{
	return FRotator(0.f, Degrees, 0.f);
}
#endif
}

UFTBuildExploreWorldCommandlet::UFTBuildExploreWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildExploreWorldCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	// -World=<id>: one of the big worlds of Data/Field/worlds.json instead of the demo map.
	FString WorldId;
	if (FParse::Value(*Params, TEXT("World="), WorldId) && !WorldId.IsEmpty())
	{
		return FTBuildWorldMap(WorldId, Params);
	}
	FString MapName = TEXT("Explore");
	FParse::Value(*Params, TEXT("Map="), MapName);
	if (MapName.IsEmpty() || !FName::IsValidXName(MapName, INVALID_LONGPACKAGE_CHARACTERS))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: '%s' is not a valid map name."), *MapName);
		return 1;
	}
	const FString MapPath = FString::Printf(TEXT("%s/%s"), kExploreMapFolder, *MapName);
	// -Style=<spec> adds a style controller over the photoreal look (none without it, or with "none").
	// The separator flag is off so a dial ("S3,U0.5") keeps its commas.
	FString StyleSpec;
	FParse::Value(*Params, TEXT("Style="), StyleSpec, false);

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Base = FTScene::LoadSurface();
	if (!Cube || !Sphere || !Cylinder || !Base)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: a /Engine/BasicShapes mesh or the world surface material is missing."));
		return 1;
	}

	// Start from nothing: loading the old map into the package would clash with the new world.
	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(*MapPath);

	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, *MapName, Package); // Editor, so it gets a navigation system
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: couldn't create the world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->DefaultGameMode = AFTExploreGameMode::StaticClass();
	}

	UMaterialInterface* Ground = MakeTint(Package, Base, TEXT("MI_Explore_Ground"), FLinearColor(0.23f, 0.33f, 0.17f));
	UMaterialInterface* Path = MakeTint(Package, Base, TEXT("MI_Explore_Path"), FLinearColor(0.45f, 0.38f, 0.28f));
	UMaterialInterface* Wall = MakeTint(Package, Base, TEXT("MI_Explore_Wall"), FLinearColor(0.42f, 0.4f, 0.37f));
	UMaterialInterface* House = MakeTint(Package, Base, TEXT("MI_Explore_House"), FLinearColor(0.75f, 0.68f, 0.55f));
	UMaterialInterface* Roof = MakeTint(Package, Base, TEXT("MI_Explore_Roof"), FLinearColor(0.5f, 0.2f, 0.15f));
	UMaterialInterface* Wood = MakeTint(Package, Base, TEXT("MI_Explore_Wood"), FLinearColor(0.3f, 0.2f, 0.12f));
	UMaterialInterface* Leaves = MakeTint(Package, Base, TEXT("MI_Explore_Leaves"), FLinearColor(0.18f, 0.42f, 0.16f));
	UMaterialInterface* Stone = MakeTint(Package, Base, TEXT("MI_Explore_Stone"), FLinearColor(0.55f, 0.55f, 0.58f));
	// Still water: dark, and smooth enough to carry the sky's reflection.
	UMaterialInterface* Water = MakeTint(Package, Base, TEXT("MI_Explore_Water"), FLinearColor(0.05f, 0.11f, 0.14f), 0.05f);

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Ground: x -12..50 m, y -26..26 m, top at z = 0. The path and the courtyard floor sit a
	// couple of centimetres above it (a step the character walks over).
	Block(World, Spawn, Cube, Ground, TEXT("Ground"), M(19, 0, -0.5f), FVector(62, 52, 1));
	Block(World, Spawn, Cube, Path, TEXT("Path"), M(15, 0, -0.48f), FVector(34, 3, 1));
	Block(World, Spawn, Cube, Stone, TEXT("Courtyard"), M(40, 0, -0.47f), FVector(18, 14, 1));

	// Outer walls, 3 m high.
	Block(World, Spawn, Cube, Wall, TEXT("Wall_N"), M(19, 26, 1.5f), FVector(62, 1, 3));
	Block(World, Spawn, Cube, Wall, TEXT("Wall_S"), M(19, -26, 1.5f), FVector(62, 1, 3));
	Block(World, Spawn, Cube, Wall, TEXT("Wall_W"), M(-12, 0, 1.5f), FVector(1, 52, 3));
	Block(World, Spawn, Cube, Wall, TEXT("Wall_E"), M(50, 0, 1.5f), FVector(1, 52, 3));
	// The inner wall at x = 30 with a 6 m opening; the gate spot's pillars narrow it to 4 m.
	Block(World, Spawn, Cube, Wall, TEXT("InnerWall_N"), M(30, 14.5f, 2), FVector(1, 23, 4));
	Block(World, Spawn, Cube, Wall, TEXT("InnerWall_S"), M(30, -14.5f, 2), FVector(1, 23, 4));

	// Houses: a body and a pitched roof (a cube rolled 45 degrees).
	struct FHouse
	{
		float X, Y, W, D, Turn;
	};
	const FHouse Houses[] = {
		{-5.f, 15.f, 6.f, 5.f, 0.f},
		{-5.f, -16.f, 6.f, 5.f, 0.f},
		{6.f, -19.f, 5.f, 5.f, 10.f},
		{19.f, -19.f, 6.f, 5.f, -5.f},
		{15.f, 19.f, 7.f, 5.f, 0.f},
		{41.f, 18.f, 6.f, 6.f, 0.f},
	};
	int32 Index = 0;
	for (const FHouse& H : Houses)
	{
		++Index;
		Block(World, Spawn, Cube, House, FString::Printf(TEXT("House_%d"), Index), M(H.X, H.Y, 1.5f), FVector(H.W, H.D, 3), Yaw(H.Turn));
		const float Side = H.D * 0.72f;
		Block(World, Spawn, Cube, Roof, FString::Printf(TEXT("Roof_%d"), Index), M(H.X, H.Y, 3.f), FVector(H.W + 0.4f, Side, Side),
			FRotator(0.f, H.Turn, 45.f));
	}

	// Trees: a trunk and a round crown.
	const FVector2D Trees[] = {
		{-9.f, 23.f}, {-9.f, -23.f}, {-9.f, 6.f}, {2.f, 23.f}, {26.f, 22.f}, {26.f, -22.f}, {12.f, -23.f},
		{47.f, 23.f}, {47.f, -23.f}, {36.f, 11.f}, {36.f, -11.f}, {45.f, 9.f}, {45.f, -9.f},
	};
	Index = 0;
	for (const FVector2D& T : Trees)
	{
		++Index;
		const float X = static_cast<float>(T.X);
		const float Y = static_cast<float>(T.Y);
		Block(World, Spawn, Cylinder, Wood, FString::Printf(TEXT("Trunk_%d"), Index), M(X, Y, 1.5f), FVector(0.4f, 0.4f, 3));
		Block(World, Spawn, Sphere, Leaves, FString::Printf(TEXT("Crown_%d"), Index), M(X, Y, 4.f), FVector(3.2f));
	}

	// The fountain: a low basin, water and a column.
	Block(World, Spawn, Cylinder, Stone, TEXT("Fountain_Basin"), M(14, 0, 0.35f), FVector(3.2f, 3.2f, 0.7f));
	Block(World, Spawn, Cylinder, Water, TEXT("Fountain_Water"), M(14, 0, 0.62f), FVector(2.8f, 2.8f, 0.2f));
	Block(World, Spawn, Cylinder, Stone, TEXT("Fountain_Column"), M(14, 0, 1.3f), FVector(0.5f, 0.5f, 2.6f));

	// A raised terrace (x -8..0, y -12..-7, 1.2 m high) with a ramp up from the north: the ramp's
	// local X points -Y and pitches up 16.7 degrees, so it rises 1.2 m over 4 m (walkable).
	Block(World, Spawn, Cube, Stone, TEXT("Terrace"), M(-4, -9.5f, 0.6f), FVector(8, 5, 1.2f));
	Block(World, Spawn, Cube, Stone, TEXT("Ramp"), M(-4, -5.f, 0.5f), FVector(4.2f, 3, 0.3f), FRotator(16.7f, -90.f, 0.f));
	Block(World, Spawn, Cube, Wall, TEXT("Terrace_Rail"), M(-4, -11.8f, 1.6f), FVector(8, 0.3f, 0.8f));

	// Crates and barrels by the houses.
	Block(World, Spawn, Cube, Wood, TEXT("Crate_1"), M(-1.4f, 13.f, 0.4f), FVector(0.8f), Yaw(15.f));
	Block(World, Spawn, Cube, Wood, TEXT("Crate_2"), M(-1.5f, 13.9f, 0.4f), FVector(0.8f), Yaw(-10.f));
	Block(World, Spawn, Cylinder, Wood, TEXT("Barrel_1"), M(22.5f, -15.8f, 0.5f), FVector(0.7f, 0.7f, 1.f));
	Block(World, Spawn, Cylinder, Wood, TEXT("Barrel_2"), M(23.4f, -16.2f, 0.5f), FVector(0.7f, 0.7f, 1.f));

	// Lamps by the gate.
	for (const float Side : {-1.f, 1.f})
	{
		const TCHAR* const Suffix = Side < 0.f ? TEXT("S") : TEXT("N");
		const FVector At = M(27.5f, 4.f * Side);
		Block(World, Spawn, Cylinder, Wall, FString::Printf(TEXT("LampPost_%s"), Suffix), At + FVector(0, 0, 125), FVector(0.2f, 0.2f, 2.5f));
		Block(World, Spawn, Sphere, Stone, FString::Printf(TEXT("LampGlobe_%s"), Suffix), At + FVector(0, 0, 265), FVector(0.4f));
		APointLight* Lamp = World->SpawnActor<APointLight>(At + FVector(0, 0, 265), FRotator::ZeroRotator, Spawn);
		Lamp->PointLightComponent->SetIntensity(4000.f);
		Lamp->PointLightComponent->SetLightColor(FLinearColor(1.f, 0.8f, 0.55f));
		Lamp->PointLightComponent->SetAttenuationRadius(900.f);
		Lamp->SetActorLabel(FString::Printf(TEXT("Lamp_%s"), Suffix));
	}

	// A ring of standing stones around the ambush, open toward the gate (-X).
	for (int32 I = 0; I < 7; ++I)
	{
		const float Degrees = -140.f + 280.f * static_cast<float>(I) / 6.f;
		const float A = FMath::DegreesToRadians(Degrees);
		Block(World, Spawn, Cube, Wall, FString::Printf(TEXT("Stone_%d"), I + 1),
			M(40.f + 8.f * FMath::Cos(A), 6.5f * FMath::Sin(A), 1.1f), FVector(0.8f, 0.8f, 2.2f), Yaw(Degrees));
	}

	// Light.
	FTScene::SpawnLook(World, Spawn, ftcore::SceneLook{}, FRotator(-38, 35, 0));

	// The spots and their director.
	AFTExploreDirector* Director = World->SpawnActor<AFTExploreDirector>(FVector(0, 0, 200), FRotator::ZeroRotator, Spawn);
	Director->SetActorLabel(TEXT("ExploreDirector"));
	const int32 SpotCount = static_cast<int32>(ftcore::demo::kExploreSpots.size());
	Director->Spots.SetNum(SpotCount);
	for (int32 Id = 0; Id < SpotCount; ++Id)
	{
		const ftcore::demo::ExploreSpot& Def = ftcore::demo::kExploreSpots[static_cast<size_t>(Id)];
		const FTransform At(M(Def.pos.x, Def.pos.y));
		AFTExploreSpot* Spot = World->SpawnActorDeferred<AFTExploreSpot>(AFTExploreSpot::StaticClass(), At, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		Spot->SpotId = Id;
		Spot->FinishSpawning(At);
		Spot->SetActorLabel(FString::Printf(TEXT("ExploreSpot_%d"), Id));
		Director->Spots[Id] = Spot;
	}

	APlayerStart* Start = World->SpawnActor<APlayerStart>(
		M(ftcore::demo::kExploreStart.x, ftcore::demo::kExploreStart.y, 1.f), FRotator::ZeroRotator, Spawn);
	Start->SetActorLabel(TEXT("PlayerStart"));

	StyleSpec.TrimStartAndEndInline();
	if (!StyleSpec.IsEmpty() && !StyleSpec.Equals(TEXT("none"), ESearchCase::IgnoreCase))
	{
		if (UMaterialInterface* StyleMaterial = LoadObject<UMaterialInterface>(nullptr, FTStyle::MaterialObjectPath))
		{
			AFTStyleController* Style = World->SpawnActor<AFTStyleController>(FVector(0, 0, 300), FRotator::ZeroRotator, Spawn);
			Style->StyleSpec = StyleSpec;
			Style->StyleMaterial = StyleMaterial;
			Style->SetActorLabel(TEXT("StyleController"));
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildExploreWorld: style %s."), *StyleSpec);
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildExploreWorld: %s is missing, so no style controller (run FTBuildStyleMaterials)."),
				FTStyle::MaterialObjectPath);
		}
	}

	// Navmesh over the whole walled ground, built here and saved with the map (the game doesn't
	// rebuild it). The field autopilot walks its paths; later, people and followers can too.
	ANavMeshBoundsVolume* NavBounds = World->SpawnActor<ANavMeshBoundsVolume>(M(19, 0, 3.f), FRotator::ZeroRotator, Spawn);
	// The builder fills the volume's brush model, so it needs one first (as the editor's volume factory does).
	NavBounds->Brush = NewObject<UModel>(NavBounds);
	NavBounds->Brush->Initialize(nullptr, true);
	NavBounds->Brush->Polys = NewObject<UPolys>(NavBounds->Brush);
	NavBounds->GetBrushComponent()->Brush = NavBounds->Brush;
	UCubeBuilder* NavBox = NewObject<UCubeBuilder>();
	NavBox->X = 6200.f;
	NavBox->Y = 5200.f;
	NavBox->Z = 1000.f;
	NavBox->Build(World, NavBounds);
	NavBounds->SetActorLabel(TEXT("NavMeshBounds"));
	FNavigationSystem::AddNavigationSystemToWorld(*World, FNavigationSystemRunMode::EditorMode);
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!Nav)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: no navigation system in the new world."));
		World->DestroyWorld(false);
		return 1;
	}
	// A commandlet's editor world keeps the async-load lock, and the bounds wait for a tick.
	Nav->RemoveNavigationBuildLock(ENavigationBuildLock::AsyncLoadLock, UNavigationSystemV1::ELockRemovalRebuildAction::NoRebuild);
	Nav->OnNavigationBoundsUpdated(NavBounds);
	Nav->Tick(0.f);
	Nav->Build();
	// Every spot must be reachable from the start, or the field pilot would walk into a wall.
	int32 Unreachable = 0;
	const FVector StartAt = M(ftcore::demo::kExploreStart.x, ftcore::demo::kExploreStart.y, 1.f);
	for (int32 Id = 0; Id < SpotCount; ++Id)
	{
		const ftcore::demo::ExploreSpot& Def = ftcore::demo::kExploreSpots[static_cast<size_t>(Id)];
		const UNavigationPath* Route = Nav->FindPathToLocationSynchronously(World, StartAt, M(Def.pos.x, Def.pos.y, 1.f));
		if (!Route || !Route->IsValid() || Route->IsPartial())
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: spot %d has no navmesh path from the start."), Id);
			++Unreachable;
		}
	}
	for (size_t Goal = 0; Goal < ftcore::demo::kFieldGoals.size(); ++Goal)
	{
		const ftcore::Vec2 G = ftcore::demo::kFieldGoals[Goal];
		const UNavigationPath* Route = Nav->FindPathToLocationSynchronously(World, StartAt, M(G.x, G.y, 1.f));
		if (!Route || !Route->IsValid() || Route->IsPartial())
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: field goal %d has no navmesh path from the start."), static_cast<int32>(Goal));
			++Unreachable;
		}
	}
	if (Unreachable > 0)
	{
		World->DestroyWorld(false);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildExploreWorld: navmesh built; all %d spots and %d field goals reachable."), SpotCount,
		static_cast<int32>(ftcore::demo::kFieldGoals.size()));

	FAssetRegistryModule::AssetCreated(World);
	Package->MarkPackageDirty();

	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, World, *File, Save);

	World->DestroyWorld(false);

	if (!bSaved)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildExploreWorld: saved %s."), *File);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld needs an editor build."));
	return 1;
#endif
}
