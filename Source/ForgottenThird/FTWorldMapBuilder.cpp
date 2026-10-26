#include "FTWorldMapBuilder.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Polys.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Model.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ForgottenThird.h"
#include "FTExploreDirector.h"
#include "FTExploreGameMode.h"
#include "FTStyleController.h"
#include "FTStyleParameters.h"
#include "FTText.h"

#include "ftcore/ExploreDemo.h"
#include "ftcore/FieldGoals.h"
#include "ftcore/Overworld.h"
#include "ftcore/WorldLayout.h"

#include <string>
#include <string_view>
#include <vector>

namespace
{
// Metres on the ground plane (ftcore) to Unreal centimetres.
FVector WorldM(float X, float Y, float Z = 0.f)
{
	return FVector(X * 100.f, Y * 100.f, Z * 100.f);
}

FVector WorldM(ftcore::Vec2 P, float Z = 0.f)
{
	return WorldM(P.x, P.y, Z);
}

UMaterialInterface* WorldTint(UPackage* Package, UMaterialInterface* Base, const TCHAR* Name, const FLinearColor& Color)
{
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, Name, RF_Public);
	Mic->SetParentEditorOnly(Base);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), Color);
	Mic->PostEditChange();
	return Mic;
}

// The colours of one world kind.
struct FWorldPalette
{
	FLinearColor Ground, Road, Plaza, Building, Roof, Landmark, Anchor;
};

FWorldPalette WorldPaletteFor(ftcore::WorldKind Kind)
{
	switch (Kind)
	{
	case ftcore::WorldKind::Towers:
		return {{0.2f, 0.24f, 0.3f}, {0.35f, 0.38f, 0.42f}, {0.5f, 0.55f, 0.62f}, {0.45f, 0.52f, 0.62f}, {0.3f, 0.35f, 0.45f},
			{0.7f, 0.75f, 0.85f}, {0.55f, 0.7f, 0.95f}};
	case ftcore::WorldKind::Ruins:
		return {{0.5f, 0.47f, 0.4f}, {0.6f, 0.56f, 0.48f}, {0.68f, 0.65f, 0.6f}, {0.6f, 0.58f, 0.55f}, {0.45f, 0.42f, 0.4f},
			{0.8f, 0.78f, 0.72f}, {0.9f, 0.88f, 0.8f}};
	default:
		return {{0.23f, 0.33f, 0.17f}, {0.45f, 0.38f, 0.28f}, {0.55f, 0.55f, 0.58f}, {0.75f, 0.68f, 0.55f}, {0.5f, 0.2f, 0.15f},
			{0.62f, 0.58f, 0.5f}, {0.95f, 0.78f, 0.45f}};
	}
}

// An actor holding instanced mesh components (it is saved with them, as instance components).
AActor* WorldHolder(UWorld* World, const FActorSpawnParameters& Spawn, const TCHAR* Label)
{
	AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("Root"));
	Root->CreationMethod = EComponentCreationMethod::Instance;
	Root->SetMobility(EComponentMobility::Static);
	Holder->SetRootComponent(Root);
	Holder->AddInstanceComponent(Root);
	Root->RegisterComponent();
	Holder->SetActorLabel(Label);
	return Holder;
}

UInstancedStaticMeshComponent* WorldIsm(AActor* Holder, const TCHAR* Name, UStaticMesh* Mesh, UMaterialInterface* Material)
{
	UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(Holder, Name);
	Ism->CreationMethod = EComponentCreationMethod::Instance;
	Ism->SetMobility(EComponentMobility::Static);
	Ism->SetStaticMesh(Mesh);
	Ism->SetMaterial(0, Material);
	Ism->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	Ism->SetupAttachment(Holder->GetRootComponent());
	Holder->AddInstanceComponent(Ism);
	Ism->RegisterComponent();
	return Ism;
}

AStaticMeshActor* WorldBlock(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Mesh, UMaterialInterface* Material,
	const FString& Label, const FVector& Location, const FVector& Scale, const FRotator& Rotation = FRotator::ZeroRotator)
{
	AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(Location, Rotation, Spawn);
	Prop->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Prop->GetStaticMeshComponent()->SetMaterial(0, Material);
	Prop->SetActorScale3D(Scale);
	Prop->SetActorLabel(Label);
	return Prop;
}

ANavMeshBoundsVolume* WorldNavBounds(UWorld* World, const FActorSpawnParameters& Spawn, const FVector& Centre, const FVector& Size,
	const FString& Label)
{
	ANavMeshBoundsVolume* Bounds = World->SpawnActor<ANavMeshBoundsVolume>(Centre, FRotator::ZeroRotator, Spawn);
	// The builder fills the volume's brush model, so it needs one first (as the editor's volume factory does).
	Bounds->Brush = NewObject<UModel>(Bounds);
	Bounds->Brush->Initialize(nullptr, true);
	Bounds->Brush->Polys = NewObject<UPolys>(Bounds->Brush);
	Bounds->GetBrushComponent()->Brush = Bounds->Brush;
	UCubeBuilder* Box = NewObject<UCubeBuilder>();
	Box->X = Size.X;
	Box->Y = Size.Y;
	Box->Z = Size.Z;
	Box->Build(World, Bounds);
	Bounds->SetActorLabel(Label);
	return Bounds;
}

bool WorldLoadText(const TCHAR* Relative, TArray<uint8>& Bytes)
{
	const FString File = FPaths::ProjectDir() / Relative;
	return IFileManager::Get().FileExists(*File) && FFileHelper::LoadFileToArray(Bytes, *File);
}

// Sun, sky light, atmosphere, fog and an unbound post-process volume.
void WorldSky(UWorld* World, const FActorSpawnParameters& Spawn)
{
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 1000), FRotator(-38, 35, 0), Spawn);
	Sun->GetComponent()->SetMobility(EComponentMobility::Movable);
	Sun->GetComponent()->SetIntensity(6.f);
	if (UDirectionalLightComponent* SunLight = Cast<UDirectionalLightComponent>(Sun->GetComponent()))
	{
		SunLight->SetAtmosphereSunLight(true);
	}
	Sun->SetActorLabel(TEXT("Sun"));
	ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 800), FRotator::ZeroRotator, Spawn);
	Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sky->GetLightComponent()->bRealTimeCapture = true;
	Sky->SetActorLabel(TEXT("SkyLight"));
	ASkyAtmosphere* Atmosphere = World->SpawnActor<ASkyAtmosphere>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	Atmosphere->SetActorLabel(TEXT("SkyAtmosphere"));
	AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(FVector(0, 0, -100), FRotator::ZeroRotator, Spawn);
	Fog->SetActorLabel(TEXT("HeightFog"));
	APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>(FVector(0, 0, 200), FRotator::ZeroRotator, Spawn);
	Volume->bUnbound = true;
	Volume->Priority = 0.f;
	Volume->SetActorLabel(TEXT("PostProcessVolume"));
}

// The style controller with `StyleSpec`, unless it is "none".
void WorldStyle(UWorld* World, const FActorSpawnParameters& Spawn, const FString& StyleSpec)
{
	if (StyleSpec.TrimStartAndEnd().Equals(TEXT("none"), ESearchCase::IgnoreCase))
	{
		return;
	}
	if (UMaterialInterface* StyleMaterial = LoadObject<UMaterialInterface>(nullptr, FTStyle::MaterialObjectPath))
	{
		AFTStyleController* Style = World->SpawnActor<AFTStyleController>(FVector(0, 0, 300), FRotator::ZeroRotator, Spawn);
		Style->StyleSpec = StyleSpec;
		Style->StyleMaterial = StyleMaterial;
		Style->SetActorLabel(TEXT("StyleController"));
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildExploreWorld: %s is missing, so no style controller (run FTBuildStyleMaterials)."),
			FTStyle::MaterialObjectPath);
	}
}

// A new editor world in /Game/Maps/<MapName>, replacing the file; null on failure (logged).
UWorld* WorldCreate(const FString& MapName, UPackage*& Package, FString& File)
{
	if (MapName.IsEmpty() || !FName::IsValidXName(MapName, INVALID_LONGPACKAGE_CHARACTERS))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: '%s' is not a valid map name."), *MapName);
		return nullptr;
	}
	const FString MapPath = FString::Printf(TEXT("/Game/Maps/%s"), *MapName);
	File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: couldn't replace %s."), *File);
		return nullptr;
	}
	Package = CreatePackage(*MapPath);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, *MapName, Package); // Editor, so it gets a navigation system
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: couldn't create the world."));
		return nullptr;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->DefaultGameMode = AFTExploreGameMode::StaticClass();
	}
	return World;
}

// Saves the map and destroys the world; the commandlet's exit code.
int32 WorldSave(UPackage* Package, UWorld* World, const FString& File)
{
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
}

// The blockout's actor count and bounds: lights, volumes and the director don't count toward the size.
FBox WorldBounds(UWorld* World, int32& Actors)
{
	Actors = 0;
	FBox Bounds(ForceInit);
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		++Actors;
		if (It->IsA<AStaticMeshActor>() || It->FindComponentByClass<UInstancedStaticMeshComponent>())
		{
			Bounds += It->GetComponentsBoundingBox(true);
		}
	}
	return Bounds;
}

int32 BuildOverworldMap(const ftcore::WorldTable& Worlds, const FString& Params);
} // namespace

int32 FTBuildWorldMap(const FString& WorldId, const FString& Params)
{
	// The world and its layout.
	TArray<uint8> Bytes;
	if (!WorldLoadText(TEXT("Data/Field/worlds.json"), Bytes))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: no Data/Field/worlds.json."));
		return 1;
	}
	ftcore::WorldTable Worlds;
	std::string Error;
	if (!ftcore::importWorldTable(FT::BytesAsText(Bytes), Worlds, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: Data/Field/worlds.json refused: %s"), *FT::ToFString(Error).TrimEnd());
		return 1;
	}
	if (WorldId.Equals(TEXT("overworld"), ESearchCase::IgnoreCase))
	{
		return BuildOverworldMap(Worlds, Params);
	}
	const FTCHARToUTF8 IdUtf8(*WorldId);
	const int32 Index = Worlds.findWorld(std::string_view(IdUtf8.Get(), static_cast<size_t>(IdUtf8.Length())));
	if (Index < 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: no world '%s' in Data/Field/worlds.json."), *WorldId);
		return 1;
	}
	const ftcore::WorldSpec& Spec = Worlds.worlds[static_cast<size_t>(Index)];
	TUniquePtr<ftcore::WorldLayout> Layout = MakeUnique<ftcore::WorldLayout>();
	if (!ftcore::generateWorldLayout(Spec, *Layout, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: world %s: %s"), *WorldId, *FT::ToFString(Error).TrimEnd());
		return 1;
	}
	const FString Id = FT::ToFString(Spec.id);
	const FString MapName = FT::ToFString(Spec.map);
	FString StyleSpec = FT::ToFString(Spec.style);
	FParse::Value(*Params, TEXT("Style="), StyleSpec, false);

	// Field goals to check from every district: the hashed defaults and goals.json's entries.
	std::vector<ftcore::Vec2> Goals(ftcore::demo::kFieldGoals.begin(), ftcore::demo::kFieldGoals.end());
	if (WorldLoadText(TEXT("Data/Field/goals.json"), Bytes))
	{
		ftcore::FieldGoalTable Table;
		if (!ftcore::importFieldGoalTable(FT::BytesAsText(Bytes), Table, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: Data/Field/goals.json refused: %s"), *FT::ToFString(Error).TrimEnd());
			return 1;
		}
		for (const ftcore::FieldGoalEntry& Entry : Table.entries)
		{
			Goals.push_back(Entry.goal);
			if (Entry.hasVia)
			{
				Goals.push_back(Entry.via);
			}
		}
	}

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Cube || !Sphere || !Cylinder || !Base)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: a /Engine/BasicShapes asset is missing."));
		return 1;
	}

	UPackage* Package = nullptr;
	FString File;
	UWorld* World = WorldCreate(MapName, Package, File);
	if (!World)
	{
		return 1;
	}

	const FWorldPalette Palette = WorldPaletteFor(Layout->kind());
	UMaterialInterface* GroundTint = WorldTint(Package, Base, TEXT("MI_World_Ground"), Palette.Ground);
	UMaterialInterface* RoadTint = WorldTint(Package, Base, TEXT("MI_World_Road"), Palette.Road);
	UMaterialInterface* PlazaTint = WorldTint(Package, Base, TEXT("MI_World_Plaza"), Palette.Plaza);
	UMaterialInterface* BuildingTint = WorldTint(Package, Base, TEXT("MI_World_Building"), Palette.Building);
	UMaterialInterface* RoofTint = WorldTint(Package, Base, TEXT("MI_World_Roof"), Palette.Roof);
	UMaterialInterface* LandmarkTint = WorldTint(Package, Base, TEXT("MI_World_Landmark"), Palette.Landmark);
	UMaterialInterface* AnchorTint = WorldTint(Package, Base, TEXT("MI_World_Anchor"), Palette.Anchor);
	UMaterialInterface* BeaconTint = WorldTint(Package, Base, TEXT("MI_World_Beacon"), FLinearColor(1.f, 0.8f, 0.3f));
	UMaterialInterface* WoodTint = WorldTint(Package, Base, TEXT("MI_World_Wood"), FLinearColor(0.3f, 0.2f, 0.12f));
	UMaterialInterface* LeavesTint = WorldTint(Package, Base, TEXT("MI_World_Leaves"), FLinearColor(0.18f, 0.42f, 0.16f));
	UMaterialInterface* RockTint = WorldTint(Package, Base, TEXT("MI_World_Rock"), FLinearColor(0.42f, 0.4f, 0.38f));

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const float Side = Layout->side();
	const float Half = Side * 0.5f;

	// The floor (top at z = 0) and the perimeter walls, 10 m high.
	WorldBlock(World, Spawn, Cube, GroundTint, TEXT("Ground"), WorldM(0, 0, -0.5f), FVector(Side, Side, 1));
	WorldBlock(World, Spawn, Cube, RockTint, TEXT("Wall_N"), WorldM(0, Half, 5), FVector(Side + 2, 2, 10));
	WorldBlock(World, Spawn, Cube, RockTint, TEXT("Wall_S"), WorldM(0, -Half, 5), FVector(Side + 2, 2, 10));
	WorldBlock(World, Spawn, Cube, RockTint, TEXT("Wall_E"), WorldM(Half, 0, 5), FVector(2, Side + 2, 10));
	WorldBlock(World, Spawn, Cube, RockTint, TEXT("Wall_W"), WorldM(-Half, 0, 5), FVector(2, Side + 2, 10));

	int32 Instances = 0;
	// Roads (2 cm above the floor) and plazas (3 cm): steps the character walks over.
	{
		AActor* Holder = WorldHolder(World, Spawn, TEXT("Roads"));
		UInstancedStaticMeshComponent* Roads = WorldIsm(Holder, TEXT("RoadStrips"), Cube, RoadTint);
		UInstancedStaticMeshComponent* Plazas = WorldIsm(Holder, TEXT("PlazaDiscs"), Cylinder, PlazaTint);
		for (size_t I = 0; I < Layout->roadCount(); ++I)
		{
			const ftcore::WorldRoad& Road = Layout->road(I);
			const ftcore::Vec2 A = Layout->district(Road.a).centre;
			const ftcore::Vec2 B = Layout->district(Road.b).centre;
			const float Length = FVector2D(B.x - A.x, B.y - A.y).Size();
			const float Degrees = FMath::RadiansToDegrees(FMath::Atan2(B.y - A.y, B.x - A.x));
			Roads->AddInstance(FTransform(FRotator(0.f, Degrees, 0.f), WorldM((A.x + B.x) * 0.5f, (A.y + B.y) * 0.5f, -0.48f),
				FVector(Length, ftcore::WorldLayout::kRoadWidth, 1.f)));
			++Instances;
		}
		const float Plaza = ftcore::WorldLayout::kPlazaRadius * 2.f;
		for (size_t D = 0; D < Layout->districtCount(); ++D)
		{
			Plazas->AddInstance(FTransform(FRotator::ZeroRotator, WorldM(Layout->district(D).centre, -0.47f), FVector(Plaza, Plaza, 1.f)));
			++Instances;
		}
	}

	// Buildings: a body, with a pitched roof in a town or a flat cap on a tower.
	{
		AActor* Holder = WorldHolder(World, Spawn, TEXT("Buildings"));
		UInstancedStaticMeshComponent* Bodies = WorldIsm(Holder, TEXT("Bodies"), Cube, BuildingTint);
		UInstancedStaticMeshComponent* Roofs = WorldIsm(Holder, TEXT("Roofs"), Cube, RoofTint);
		const bool bTown = Layout->kind() == ftcore::WorldKind::Town;
		for (size_t I = 0; I < Layout->blockCount(); ++I)
		{
			const ftcore::WorldBlock& B = Layout->block(I);
			const float Degrees = FMath::RadiansToDegrees(B.yaw);
			Bodies->AddInstance(FTransform(FRotator(0.f, Degrees, 0.f), WorldM(B.pos, B.height * 0.5f), FVector(B.width, B.depth, B.height)));
			++Instances;
			if (!B.roof)
			{
				continue;
			}
			if (bTown)
			{
				const float Pitch = B.depth * 0.72f;
				Roofs->AddInstance(FTransform(FRotator(0.f, Degrees, 45.f), WorldM(B.pos, B.height), FVector(B.width + 0.4f, Pitch, Pitch)));
			}
			else
			{
				Roofs->AddInstance(FTransform(FRotator(0.f, Degrees, 0.f), WorldM(B.pos, B.height + 0.5f), FVector(B.width + 1.f, B.depth + 1.f, 1.f)));
			}
			++Instances;
		}
	}

	// Trees (a trunk and a crown) and rocks on the open ground.
	{
		AActor* Holder = WorldHolder(World, Spawn, TEXT("Props"));
		UInstancedStaticMeshComponent* Trunks = WorldIsm(Holder, TEXT("Trunks"), Cylinder, WoodTint);
		UInstancedStaticMeshComponent* Crowns = WorldIsm(Holder, TEXT("Crowns"), Sphere, LeavesTint);
		UInstancedStaticMeshComponent* Rocks = WorldIsm(Holder, TEXT("Rocks"), Sphere, RockTint);
		for (size_t I = 0; I < Layout->propCount(); ++I)
		{
			const ftcore::WorldProp& P = Layout->prop(I);
			const float S = P.scale;
			if (P.kind == 0)
			{
				Trunks->AddInstance(FTransform(FRotator::ZeroRotator, WorldM(P.pos, 1.5f * S), FVector(0.4f * S, 0.4f * S, 3.f * S)));
				Crowns->AddInstance(FTransform(FRotator::ZeroRotator, WorldM(P.pos, 4.f * S), FVector(3.2f * S)));
				Instances += 2;
			}
			else
			{
				const float Turn = static_cast<float>((I * 47u) % 360u);
				Rocks->AddInstance(FTransform(FRotator(0.f, Turn, 0.f), WorldM(P.pos, 0.3f * S), FVector(1.6f * S, 1.3f * S, 0.9f * S)));
				++Instances;
			}
		}
	}

	// Landmarks, each with a gold beacon on top: seen over the rooftops, they tell the districts apart.
	int32 Landmarks = 0;
	for (size_t D = 0; D < Layout->districtCount(); ++D)
	{
		const ftcore::WorldDistrict& Dist = Layout->district(D);
		const float H = Dist.landmarkHeight;
		const float W = ftcore::WorldLayout::kLandmarkRadius * 2.f;
		const int32 N = static_cast<int32>(D);
		switch (Dist.landmarkShape)
		{
		case 1: // a pillar with a ring
			WorldBlock(World, Spawn, Cylinder, LandmarkTint, FString::Printf(TEXT("Landmark_%d"), N), WorldM(Dist.landmark, H * 0.5f), FVector(W, W, H));
			WorldBlock(World, Spawn, Cylinder, LandmarkTint, FString::Printf(TEXT("Landmark_%d_Ring"), N), WorldM(Dist.landmark, H * 0.72f),
				FVector(W * 2.2f, W * 2.2f, 1.5f));
			break;
		case 2: // a stepped spire
			WorldBlock(World, Spawn, Cube, LandmarkTint, FString::Printf(TEXT("Landmark_%d"), N), WorldM(Dist.landmark, H * 0.25f), FVector(W, W, H * 0.5f));
			WorldBlock(World, Spawn, Cube, LandmarkTint, FString::Printf(TEXT("Landmark_%d_Mid"), N), WorldM(Dist.landmark, H * 0.65f),
				FVector(W * 0.66f, W * 0.66f, H * 0.3f), FRotator(0.f, 45.f, 0.f));
			WorldBlock(World, Spawn, Cube, LandmarkTint, FString::Printf(TEXT("Landmark_%d_Top"), N), WorldM(Dist.landmark, H * 0.9f),
				FVector(W * 0.33f, W * 0.33f, H * 0.2f));
			break;
		default: // a pillar
			WorldBlock(World, Spawn, Cylinder, LandmarkTint, FString::Printf(TEXT("Landmark_%d"), N), WorldM(Dist.landmark, H * 0.5f), FVector(W, W, H));
			break;
		}
		WorldBlock(World, Spawn, Sphere, BeaconTint, FString::Printf(TEXT("Landmark_%d_Beacon"), N), WorldM(Dist.landmark, H + 3.f), FVector(6.f));
		++Landmarks;
	}

	// The anchor: one large shape floating over the centre, visible from the whole world.
	WorldBlock(World, Spawn, Sphere, AnchorTint, TEXT("Anchor"), WorldM(0, 0, 260), FVector(60.f));
	WorldBlock(World, Spawn, Cylinder, AnchorTint, TEXT("Anchor_Ring"), WorldM(0, 0, 260), FVector(150.f, 150.f, 3.f), FRotator(12.f, 0.f, 0.f));

	WorldSky(World, Spawn);

	AFTExploreDirector* Director = World->SpawnActor<AFTExploreDirector>(FVector(0, 0, 200), FRotator::ZeroRotator, Spawn);
	Director->WorldId = Id;
	Director->SetActorLabel(TEXT("ExploreDirector"));

	const ftcore::WorldDistrict& First = Layout->district(0);
	APlayerStart* Start = World->SpawnActor<APlayerStart>(WorldM(First.origin, 1.f), FRotator(0.f, FMath::RadiansToDegrees(Layout->spawnYaw(0)), 0.f), Spawn);
	Start->SetActorLabel(TEXT("PlayerStart"));

	WorldStyle(World, Spawn, StyleSpec);

	// A navmesh over each district (plaza and building ring), built here and saved with the map. The
	// roads between districts are straight and clear, so nothing walks them by navmesh.
	const float NavSide = (ftcore::WorldLayout::kDistrictRadius + 5.f) * 200.f;
	TArray<ANavMeshBoundsVolume*> NavBounds;
	for (size_t D = 0; D < Layout->districtCount(); ++D)
	{
		NavBounds.Add(WorldNavBounds(World, Spawn, WorldM(Layout->district(D).centre, 5.f), FVector(NavSide, NavSide, 2000.f),
			FString::Printf(TEXT("NavMeshBounds_%d"), static_cast<int32>(D))));
	}
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
	for (ANavMeshBoundsVolume* Bounds : NavBounds)
	{
		Nav->OnNavigationBoundsUpdated(Bounds);
	}
	Nav->Tick(0.f);
	Nav->Build();

	// Every field goal must be reachable from every district's spawn, or a story step would stall.
	int32 Unreachable = 0;
	for (size_t D = 0; D < Layout->districtCount(); ++D)
	{
		const FVector From = WorldM(Layout->district(D).origin, 1.f);
		for (size_t G = 0; G < Goals.size(); ++G)
		{
			const FVector To = WorldM(Layout->toWorld(D, Goals[G]), 1.f);
			const UNavigationPath* Route = Nav->FindPathToLocationSynchronously(World, From, To);
			if (!Route || !Route->IsValid() || Route->IsPartial())
			{
				if (Unreachable < 20)
				{
					UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: district %d: goal %d (%.1f, %.1f) has no navmesh path from the spawn."),
						static_cast<int32>(D), static_cast<int32>(G), Goals[G].x, Goals[G].y);
				}
				++Unreachable;
			}
		}
	}
	if (Unreachable > 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: %d goal paths missing."), Unreachable);
		World->DestroyWorld(false);
		return 1;
	}

	int32 Actors = 0;
	const FBox Bounds = WorldBounds(World, Actors);
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTBuildExploreWorld: world %s (style %s): %d districts, %d roads, %d buildings, %d props, %d instances, %d landmarks, %d actors; bounds %.0f x %.0f m, %.0f m tall; %d goals reachable from each district."),
		*Id, *StyleSpec, static_cast<int32>(Layout->districtCount()), static_cast<int32>(Layout->roadCount()),
		static_cast<int32>(Layout->blockCount()), static_cast<int32>(Layout->propCount()), Instances, Landmarks, Actors,
		Bounds.GetSize().X / 100.0, Bounds.GetSize().Y / 100.0, Bounds.Max.Z / 100.0, static_cast<int32>(Goals.size()));

	return WorldSave(Package, World, File);
}

namespace
{
// The overworld (ftcore::generateOverworldLayout, as the director makes it at run time): a floor with
// perimeter walls, straight roads gate to gate, and per world a gate arch with that world's emblem
// behind it, a ring of small blocks in its colours round a landmark with a beacon. Nothing stands on
// the roads, so the walk between gates needs no navmesh. The player start is world 0's gate.
int32 BuildOverworldMap(const ftcore::WorldTable& Worlds, const FString& Params)
{
	TUniquePtr<ftcore::OverworldLayout> Over = MakeUnique<ftcore::OverworldLayout>();
	std::string Error;
	if (!ftcore::generateOverworldLayout(Worlds, *Over, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: overworld: %s"), *FT::ToFString(Error).TrimEnd());
		return 1;
	}
	// The style: -Style=<spec>, or world 0's (the overworld is the way out of home).
	FString StyleSpec = Worlds.worlds.empty() ? FString(TEXT("none")) : FT::ToFString(Worlds.worlds[0].style);
	FParse::Value(*Params, TEXT("Style="), StyleSpec, false);

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Cube || !Sphere || !Cylinder || !Base)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: a /Engine/BasicShapes asset is missing."));
		return 1;
	}
	UPackage* Package = nullptr;
	FString File;
	UWorld* World = WorldCreate(FT::ToFString(Worlds.overworld.map), Package, File);
	if (!World)
	{
		return 1;
	}

	const FWorldPalette Land = WorldPaletteFor(ftcore::WorldKind::Town);
	UMaterialInterface* GroundTint = WorldTint(Package, Base, TEXT("MI_Over_Ground"), Land.Ground);
	UMaterialInterface* RoadTint = WorldTint(Package, Base, TEXT("MI_Over_Road"), Land.Road);
	UMaterialInterface* GateTint = WorldTint(Package, Base, TEXT("MI_Over_Gate"), FLinearColor(0.35f, 0.32f, 0.3f));
	UMaterialInterface* BeaconTint = WorldTint(Package, Base, TEXT("MI_Over_Beacon"), FLinearColor(1.f, 0.8f, 0.3f));
	UMaterialInterface* WoodTint = WorldTint(Package, Base, TEXT("MI_Over_Wood"), FLinearColor(0.3f, 0.2f, 0.12f));
	UMaterialInterface* LeavesTint = WorldTint(Package, Base, TEXT("MI_Over_Leaves"), FLinearColor(0.18f, 0.42f, 0.16f));
	UMaterialInterface* RockTint = WorldTint(Package, Base, TEXT("MI_Over_Rock"), FLinearColor(0.42f, 0.4f, 0.38f));

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const float Side = Over->side();
	const float Half = Side * 0.5f;
	WorldBlock(World, Spawn, Cube, GroundTint, TEXT("Ground"), WorldM(0, 0, -0.5f), FVector(Side, Side, 1));
	WorldBlock(World, Spawn, Cube, RockTint, TEXT("Wall_N"), WorldM(0, Half, 5), FVector(Side + 2, 2, 10));
	WorldBlock(World, Spawn, Cube, RockTint, TEXT("Wall_S"), WorldM(0, -Half, 5), FVector(Side + 2, 2, 10));
	WorldBlock(World, Spawn, Cube, RockTint, TEXT("Wall_E"), WorldM(Half, 0, 5), FVector(2, Side + 2, 10));
	WorldBlock(World, Spawn, Cube, RockTint, TEXT("Wall_W"), WorldM(-Half, 0, 5), FVector(2, Side + 2, 10));

	int32 Instances = 0;
	{
		AActor* Holder = WorldHolder(World, Spawn, TEXT("Roads"));
		UInstancedStaticMeshComponent* Roads = WorldIsm(Holder, TEXT("RoadStrips"), Cube, RoadTint);
		for (size_t I = 0; I < Over->roadCount(); ++I)
		{
			const ftcore::Vec2 A = Over->node(Over->road(I).a).gate;
			const ftcore::Vec2 B = Over->node(Over->road(I).b).gate;
			const float Length = FVector2D(B.x - A.x, B.y - A.y).Size() + ftcore::OverworldLayout::kRoadWidth;
			const float Degrees = FMath::RadiansToDegrees(FMath::Atan2(B.y - A.y, B.x - A.x));
			Roads->AddInstance(FTransform(FRotator(0.f, Degrees, 0.f), WorldM((A.x + B.x) * 0.5f, (A.y + B.y) * 0.5f, -0.48f),
				FVector(Length, ftcore::OverworldLayout::kRoadWidth, 1.f)));
			++Instances;
		}
	}

	// Per world: a base disc and the emblem's blocks in its kind's colours, the landmark and beacon, and
	// a gate arch 2 m behind the gate (the roads all leave the gate forward, so the arch never stands on one).
	for (size_t N = 0; N < Over->nodeCount(); ++N)
	{
		const ftcore::OverworldNode& Node = Over->node(N);
		const FWorldPalette Palette = WorldPaletteFor(Node.kind);
		const FString Tag = FString::Printf(TEXT("%d"), static_cast<int32>(N));
		UMaterialInterface* PlazaTint = WorldTint(Package, Base, *(TEXT("MI_Over_Plaza_") + Tag), Palette.Plaza);
		UMaterialInterface* BuildingTint = WorldTint(Package, Base, *(TEXT("MI_Over_Building_") + Tag), Palette.Building);
		UMaterialInterface* RoofTint = WorldTint(Package, Base, *(TEXT("MI_Over_Roof_") + Tag), Palette.Roof);
		UMaterialInterface* LandmarkTint = WorldTint(Package, Base, *(TEXT("MI_Over_Landmark_") + Tag), Palette.Landmark);

		AActor* Holder = WorldHolder(World, Spawn, *(TEXT("Emblem_") + Tag));
		UInstancedStaticMeshComponent* Discs = WorldIsm(Holder, TEXT("Base"), Cylinder, PlazaTint);
		UInstancedStaticMeshComponent* Bodies = WorldIsm(Holder, TEXT("Bodies"), Cube, BuildingTint);
		UInstancedStaticMeshComponent* Roofs = WorldIsm(Holder, TEXT("Roofs"), Cube, RoofTint);
		const float Disc = (ftcore::OverworldLayout::kEmblemRadius + 2.f) * 2.f;
		Discs->AddInstance(FTransform(FRotator::ZeroRotator, WorldM(Node.centre, -0.47f), FVector(Disc, Disc, 1.f)));
		++Instances;
		const bool bTown = Node.kind == ftcore::WorldKind::Town;
		for (size_t I = 0; I < Over->blockCount(); ++I)
		{
			const ftcore::WorldBlock& B = Over->block(I);
			if (B.district != N)
			{
				continue;
			}
			const float Degrees = FMath::RadiansToDegrees(B.yaw);
			Bodies->AddInstance(FTransform(FRotator(0.f, Degrees, 0.f), WorldM(B.pos, B.height * 0.5f), FVector(B.width, B.depth, B.height)));
			++Instances;
			if (!B.roof)
			{
				continue;
			}
			if (bTown)
			{
				const float Pitch = B.depth * 0.72f;
				Roofs->AddInstance(FTransform(FRotator(0.f, Degrees, 45.f), WorldM(B.pos, B.height), FVector(B.width + 0.2f, Pitch, Pitch)));
			}
			else
			{
				Roofs->AddInstance(FTransform(FRotator(0.f, Degrees, 0.f), WorldM(B.pos, B.height + 0.25f), FVector(B.width + 0.4f, B.depth + 0.4f, 0.5f)));
			}
			++Instances;
		}

		const float H = Node.landmarkHeight;
		WorldBlock(World, Spawn, Cylinder, LandmarkTint, TEXT("Landmark_") + Tag, WorldM(Node.centre, H * 0.5f), FVector(3.f, 3.f, H));
		WorldBlock(World, Spawn, Sphere, BeaconTint, TEXT("Landmark_") + Tag + TEXT("_Beacon"), WorldM(Node.centre, H + 2.f), FVector(4.f));

		const ftcore::Vec2 Across{-Node.outward.y, Node.outward.x};
		const ftcore::Vec2 Arch{Node.gate.x + Node.outward.x * 2.f, Node.gate.y + Node.outward.y * 2.f};
		const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Across.y, Across.x));
		for (const float Sign : {-1.f, 1.f})
		{
			const ftcore::Vec2 Post{Arch.x + Across.x * 4.5f * Sign, Arch.y + Across.y * 4.5f * Sign};
			WorldBlock(World, Spawn, Cube, GateTint, FString::Printf(TEXT("Gate_%s_Post%s"), *Tag, Sign < 0.f ? TEXT("L") : TEXT("R")),
				WorldM(Post, 3.5f), FVector(1.f, 1.f, 7.f), FRotator(0.f, Yaw, 0.f));
		}
		WorldBlock(World, Spawn, Cube, GateTint, FString::Printf(TEXT("Gate_%s_Lintel"), *Tag), WorldM(Arch, 7.5f), FVector(10.f, 1.2f, 1.2f),
			FRotator(0.f, Yaw, 0.f));
	}

	{
		AActor* Holder = WorldHolder(World, Spawn, TEXT("Props"));
		UInstancedStaticMeshComponent* Trunks = WorldIsm(Holder, TEXT("Trunks"), Cylinder, WoodTint);
		UInstancedStaticMeshComponent* Crowns = WorldIsm(Holder, TEXT("Crowns"), Sphere, LeavesTint);
		UInstancedStaticMeshComponent* Rocks = WorldIsm(Holder, TEXT("Rocks"), Sphere, RockTint);
		for (size_t I = 0; I < Over->propCount(); ++I)
		{
			const ftcore::WorldProp& P = Over->prop(I);
			const float S = P.scale;
			if (P.kind == 0)
			{
				Trunks->AddInstance(FTransform(FRotator::ZeroRotator, WorldM(P.pos, 1.5f * S), FVector(0.4f * S, 0.4f * S, 3.f * S)));
				Crowns->AddInstance(FTransform(FRotator::ZeroRotator, WorldM(P.pos, 4.f * S), FVector(3.2f * S)));
				Instances += 2;
			}
			else
			{
				const float Turn = static_cast<float>((I * 47u) % 360u);
				Rocks->AddInstance(FTransform(FRotator(0.f, Turn, 0.f), WorldM(P.pos, 0.3f * S), FVector(1.6f * S, 1.3f * S, 0.9f * S)));
				++Instances;
			}
		}
	}

	WorldSky(World, Spawn);
	AFTExploreDirector* Director = World->SpawnActor<AFTExploreDirector>(FVector(0, 0, 200), FRotator::ZeroRotator, Spawn);
	Director->bOverworld = true;
	Director->SetActorLabel(TEXT("ExploreDirector"));
	APlayerStart* Start = World->SpawnActor<APlayerStart>(WorldM(Over->node(0).gate, 1.f),
		FRotator(0.f, FMath::RadiansToDegrees(Over->gateYaw(0)), 0.f), Spawn);
	Start->SetActorLabel(TEXT("PlayerStart"));
	WorldStyle(World, Spawn, StyleSpec);

	int32 Actors = 0;
	const FBox Bounds = WorldBounds(World, Actors);
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTBuildExploreWorld: overworld (style %s): %d worlds, %d roads, %d emblem blocks, %d props, %d instances, %d actors; bounds %.0f x %.0f m, %.0f m tall."),
		*StyleSpec, static_cast<int32>(Over->nodeCount()), static_cast<int32>(Over->roadCount()), static_cast<int32>(Over->blockCount()),
		static_cast<int32>(Over->propCount()), Instances, Actors, Bounds.GetSize().X / 100.0, Bounds.GetSize().Y / 100.0, Bounds.Max.Z / 100.0);
	return WorldSave(Package, World, File);
}
} // namespace
#endif
