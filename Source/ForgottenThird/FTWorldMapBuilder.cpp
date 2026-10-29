#include "FTWorldMapBuilder.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Polys.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
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
#include "FTSceneLook.h"
#include "FTStyleController.h"
#include "FTStyleParameters.h"
#include "FTText.h"

#include "ftcore/ExploreDemo.h"
#include "ftcore/FieldGoals.h"
#include "ftcore/Overworld.h"
#include "ftcore/SurfaceTable.h"
#include "ftcore/Terrain.h"
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

// The colours of one world kind, and how far its ground drifts between warm and cool over broad patches (the ground
// master's "HueDrift").
struct FWorldPalette
{
	FLinearColor Ground, Road, Plaza, Building, Roof, Landmark, Anchor;
	float GroundHue;
};

FWorldPalette WorldPaletteFor(ftcore::WorldKind Kind)
{
	switch (Kind)
	{
	case ftcore::WorldKind::Towers:
		return {{0.2f, 0.24f, 0.3f}, {0.35f, 0.38f, 0.42f}, {0.5f, 0.55f, 0.62f}, {0.45f, 0.52f, 0.62f}, {0.3f, 0.35f, 0.45f},
			{0.7f, 0.75f, 0.85f}, {0.55f, 0.7f, 0.95f}, 0.02f};
	case ftcore::WorldKind::Ruins:
		return {{0.5f, 0.47f, 0.4f}, {0.6f, 0.56f, 0.48f}, {0.68f, 0.65f, 0.6f}, {0.6f, 0.58f, 0.55f}, {0.45f, 0.42f, 0.4f},
			{0.8f, 0.78f, 0.72f}, {0.9f, 0.88f, 0.8f}, 0.05f};
	default:
		return {{0.23f, 0.33f, 0.17f}, {0.45f, 0.38f, 0.28f}, {0.55f, 0.55f, 0.58f}, {0.75f, 0.68f, 0.55f}, {0.5f, 0.2f, 0.15f},
			{0.62f, 0.58f, 0.5f}, {0.95f, 0.78f, 0.45f}, 0.08f};
	}
}

// A world map's road mask is this many pixels across the playable square, about half a metre each over 1 km, so a
// road's fade spans eight of them.
constexpr int32 kWorldRoadMaskSize = 2048;

// The ground's instance of the ground master (FTScene::GroundMaterial), MI_World_Ground. Each of its layers wears the
// surface the world's kind gives the layer's role, at the surface's scale, in its tint and with its roughness, and the
// roads come from a new T_RoadMask of ftcore::roadMask's bytes. Null, with a warning and nothing made, when the master,
// a layer's surface or one of their textures is missing.
UMaterialInterface* WorldGround(UPackage* Package, const ftcore::SurfaceTable& Surfaces, const ftcore::WorldLayout& Layout, float Hue)
{
	constexpr int32 Layers = UE_ARRAY_COUNT(FTScene::GroundLayers);
	constexpr int32 Maps = UE_ARRAY_COUNT(FTScene::SurfaceMaps);
	UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr,
		*(FString(FTScene::GroundMaterial) + TEXT(".") + FPackageName::GetShortName(FTScene::GroundMaterial)), nullptr,
		LOAD_NoWarn | LOAD_Quiet);
	if (!Master)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildExploreWorld: %s is missing; run FTBuildGround first. The ground wears one surface and the roads stay bare."),
			FTScene::GroundMaterial);
		return nullptr;
	}
	const ftcore::SurfaceSpec* Specs[Layers] = {};
	UTexture* Textures[Layers][Maps] = {};
	for (int32 L = 0; L < Layers; ++L)
	{
		Specs[L] = Surfaces.surfaceFor(Layout.kind(), FTScene::GroundLayers[L].Role);
		if (!Specs[L])
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildExploreWorld: Data/Field/surfaces.json gives this kind's %s no surface. The ground wears one surface and the roads stay bare."),
				*FT::ToFString(ftcore::surfaceRoleName(FTScene::GroundLayers[L].Role)));
			return nullptr;
		}
		const FString Id = FT::ToFString(Specs[L]->id);
		for (int32 M = 0; M < Maps; ++M)
		{
			Textures[L][M] = FTScene::LoadSurfaceTexture(Id, FTScene::SurfaceMaps[M].Suffix);
			if (!Textures[L][M])
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildExploreWorld: %s is missing; run FTBuildSurfaces first. The ground wears one surface and the roads stay bare."),
					*FTScene::SurfaceTexturePackage(Id, FTScene::SurfaceMaps[M].Suffix));
				return nullptr;
			}
		}
	}

	TArray<uint8> Bytes;
	Bytes.SetNumZeroed(kWorldRoadMaskSize * kWorldRoadMaskSize);
	ftcore::roadMask(Layout, static_cast<std::size_t>(kWorldRoadMaskSize), Bytes.GetData());
	int32 OnRoad = 0;
	for (const uint8 Byte : Bytes)
	{
		OnRoad += Byte >= 128 ? 1 : 0;
	}
	UTexture2D* RoadMask = FTScene::NewRoadMask(Package, TEXT("T_RoadMask"), kWorldRoadMaskSize, Bytes.GetData(), RF_Public);

	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_World_Ground"), RF_Public);
	Mic->SetParentEditorOnly(Master);
	for (int32 L = 0; L < Layers; ++L)
	{
		const FString Layer = FTScene::GroundLayers[L].Name;
		for (int32 M = 0; M < Maps; ++M)
		{
			Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(*(Layer + FTScene::SurfaceMaps[M].Name)), Textures[L][M]);
		}
		const std::array<float, 3>& Tint = Surfaces.use(Layout.kind(), FTScene::GroundLayers[L].Role).tint;
		Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(*(Layer + TEXT("Tint"))), FLinearColor(Tint[0], Tint[1], Tint[2]));
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(*(Layer + TEXT("TileCm"))), Specs[L]->tileCm);
		if (Specs[L]->roughness >= 0.f)
		{
			Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(*(Layer + TEXT("Roughness"))), Specs[L]->roughness);
		}
	}
	Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("RoadMask")), RoadMask);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MaskSideCm")), Layout.side() * 100.f);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("HueDrift")), Hue);
	Mic->PostEditChange();
	FString Worn;
	for (int32 L = 0; L < Layers; ++L)
	{
		Worn += FString::Printf(TEXT("%s%s %s"), L > 0 ? TEXT(", ") : TEXT(""), FTScene::GroundLayers[L].Name, *FT::ToFString(Specs[L]->id));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildExploreWorld: the ground wears %s (%s), its roads from a %d x %d mask (%.1f%% of the square on a road)."),
		FTScene::GroundMaterial, *Worn, kWorldRoadMaskSize, kWorldRoadMaskSize, 100.0 * OnRoad / Bytes.Num());
	return Mic;
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

// A hidden wall that stops pawns but no traces on Visibility, by which the followers and the guards find
// their footing, and not the camera's arm.
AStaticMeshActor* WorldBlocker(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Cube, const FString& Label,
	const FVector& Location, const FVector& Scale, float Yaw = 0.f)
{
	AStaticMeshActor* Wall = WorldBlock(World, Spawn, Cube, nullptr, Label, Location, Scale, FRotator(0.f, Yaw, 0.f));
	UStaticMeshComponent* Mesh = Wall->GetStaticMeshComponent();
	Mesh->SetCollisionProfileName(TEXT("InvisibleWall"));
	Mesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Mesh->SetCastShadow(false);
	Wall->SetActorHiddenInGame(true);
	return Wall;
}

// Water: a plane anything walks or sees through to the ground under it.
AStaticMeshActor* WorldWater(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Plane, UMaterialInterface* Material,
	const FString& Label, const FVector& Location, const FVector& Scale)
{
	AStaticMeshActor* Water = WorldBlock(World, Spawn, Plane, Material, Label, Location, Scale);
	Water->GetStaticMeshComponent()->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Water->GetStaticMeshComponent()->SetCanEverAffectNavigation(false);
	return Water;
}

// A Landscape component is 2 x 63 quads, so the terrain's 757 samples make 6 x 6 of them.
constexpr int32 kWorldLandSubsections = 2;
constexpr int32 kWorldLandSubsectionQuads = 63;
static_assert((ftcore::WorldTerrain::kSamples - 1) % (kWorldLandSubsections * kWorldLandSubsectionQuads) == 0,
	"the terrain's samples must fill whole Landscape components");
// Trees and rocks stay off ground steeper than this (39 degrees): it stands bare.
constexpr float kWorldPropMaxSlope = 0.8f;
// The sea's plane reaches this far (m) out from the square and along its edge both ways, past the horizon.
constexpr float kWorldSeaReach = 20000.f;

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

// The sun's angle in every big world and the overworld.
const FRotator kWorldSun(-38, 35, 0);

// Whether `StyleSpec` (-Style=<spec>) asks for a style controller: the worlds are photoreal without one.
bool WorldStyled(const FString& StyleSpec)
{
	const FString Spec = StyleSpec.TrimStartAndEnd();
	return !Spec.IsEmpty() && !Spec.Equals(TEXT("none"), ESearchCase::IgnoreCase);
}

// The style controller with `StyleSpec`, when one is asked for.
void WorldStyle(UWorld* World, const FActorSpawnParameters& Spawn, const FString& StyleSpec)
{
	if (!WorldStyled(StyleSpec))
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
	// Whatever is still building for the map (its road mask's texture) finishes before it's saved.
	FAssetCompilingManager::Get().FinishAllCompilation();
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
		// Nor do the hidden walls and the water, which reaches past the horizon.
		const AStaticMeshActor* Mesh = Cast<AStaticMeshActor>(*It);
		if (It->IsHidden() || (Mesh && !Mesh->GetStaticMeshComponent()->IsCollisionEnabled()))
		{
			continue;
		}
		if (Mesh || It->FindComponentByClass<UInstancedStaticMeshComponent>())
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
	// Its ground, before the old map is replaced.
	TUniquePtr<ftcore::WorldTerrain> Terrain = MakeUnique<ftcore::WorldTerrain>();
	if (!ftcore::generateWorldTerrain(Spec, *Layout, *Terrain, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: world %s's ground: %s"), *WorldId, *FT::ToFString(Error).TrimEnd());
		return 1;
	}
	const FString Id = FT::ToFString(Spec.id);
	const FString MapName = FT::ToFString(Spec.map);
	// -Style=<spec> adds a style controller over the photoreal look; worlds.json's style is not used yet.
	FString StyleSpec;
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
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Base = FTScene::LoadSurface();
	if (!Cube || !Sphere || !Cylinder || !Plane || !Base)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: a /Engine/BasicShapes mesh or the world surface material is missing."));
		return 1;
	}
	ftcore::SurfaceTable Surfaces;
	if (!FTScene::LoadSurfaceTable(Surfaces))
	{
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
	// A role wears the photographed surface surfaces.json gives it, in its tint, else the palette's flat tint.
	int32 Dressed = 0;
	const auto Dress = [&](ftcore::SurfaceRole Role, const TCHAR* Name, const FLinearColor& Flat)
	{
		FLinearColor Tint;
		if (UMaterialInterface* Surface = FTScene::LoadSurfaceFor(Surfaces, Layout->kind(), Role, Tint))
		{
			++Dressed;
			return WorldTint(Package, Surface, Name, Tint);
		}
		return WorldTint(Package, Base, Name, Flat);
	};
	// The ground wears the ground master: its kind's ground, rock where it's steep, and the roads. Without the master or
	// one of its surfaces it wears the ground's surface alone (or the flat tint), and a road is only its cut.
	UMaterialInterface* GroundTint = WorldGround(Package, Surfaces, *Layout, Palette.GroundHue);
	if (GroundTint)
	{
		// The ground's and the road's surfaces; the rock's counts with the rocks, which wear it too.
		Dressed += 2;
	}
	else
	{
		GroundTint = Dress(ftcore::SurfaceRole::Ground, TEXT("MI_World_Ground"), Palette.Ground);
	}
	UMaterialInterface* PlazaTint = Dress(ftcore::SurfaceRole::Plaza, TEXT("MI_World_Plaza"), Palette.Plaza);
	UMaterialInterface* BuildingTint = Dress(ftcore::SurfaceRole::Building, TEXT("MI_World_Building"), Palette.Building);
	UMaterialInterface* RoofTint = Dress(ftcore::SurfaceRole::Roof, TEXT("MI_World_Roof"), Palette.Roof);
	UMaterialInterface* LandmarkTint = Dress(ftcore::SurfaceRole::Landmark, TEXT("MI_World_Landmark"), Palette.Landmark);
	UMaterialInterface* AnchorTint = WorldTint(Package, Base, TEXT("MI_World_Anchor"), Palette.Anchor);
	UMaterialInterface* BeaconTint = WorldTint(Package, Base, TEXT("MI_World_Beacon"), FLinearColor(1.f, 0.8f, 0.3f));
	UMaterialInterface* WoodTint = Dress(ftcore::SurfaceRole::Wood, TEXT("MI_World_Wood"), FLinearColor(0.3f, 0.2f, 0.12f));
	UMaterialInterface* LeavesTint = WorldTint(Package, Base, TEXT("MI_World_Leaves"), FLinearColor(0.18f, 0.42f, 0.16f));
	UMaterialInterface* RockTint = Dress(ftcore::SurfaceRole::Rock, TEXT("MI_World_Rock"), FLinearColor(0.42f, 0.4f, 0.38f));
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildExploreWorld: %d of %d surface roles wear photographed surfaces."), Dressed,
		static_cast<int32>(ftcore::kSurfaceRoleCount));

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const float Side = Layout->side();
	const float Half = Side * 0.5f;

	// The ground: the terrain's height field as a Landscape, a vertex per sample, at the 16-bit heights
	// ftcore gives (Z scale 100). Its material and scale go on before Import, which builds the components,
	// their collision and their material instances.
	const int32 Samples = static_cast<int32>(ftcore::WorldTerrain::kSamples);
	TArray<uint16> Heights;
	Heights.SetNumUninitialized(Samples * Samples);
	for (int32 Y = 0; Y < Samples; ++Y)
	{
		for (int32 X = 0; X < Samples; ++X)
		{
			Heights[Y * Samples + X] = Terrain->heightmapValue(static_cast<size_t>(X), static_cast<size_t>(Y));
		}
	}
	const float Corner = Terrain->extent() * -50.f;
	ALandscape* Land = World->SpawnActor<ALandscape>(FVector(Corner, Corner, 0.f), FRotator::ZeroRotator, Spawn);
	Land->LandscapeMaterial = GroundTint;
	Land->SetActorRelativeScale3D(FVector(Terrain->spacing() * 100.f, Terrain->spacing() * 100.f, 100.f));
	{
		TMap<FGuid, TArray<uint16>> HeightLayers;
		HeightLayers.Add(FGuid(), MoveTemp(Heights));
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>> PaintLayers;
		PaintLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());
		Land->Import(FGuid::NewGuid(), 0, 0, Samples - 1, Samples - 1, kWorldLandSubsections, kWorldLandSubsectionQuads, HeightLayers,
			TEXT(""), PaintLayers, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
	}
	ULandscapeInfo* LandInfo = Land->GetLandscapeInfo();
	if (!LandInfo)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildExploreWorld: the ground's Landscape has no info after its import."));
		World->DestroyWorld(false);
		return 1;
	}
	LandInfo->UpdateLayerInfoMap(Land);
	Land->SetActorLabel(TEXT("Ground"));

	// The edge: hidden walls from under the abyss's floor to far over the mountains, so the sea, the abyss
	// and the mountains past the square stay scenery.
	WorldBlocker(World, Spawn, Cube, TEXT("Edge_N"), WorldM(0, Half, 25), FVector(Side + 2, 2, 450));
	WorldBlocker(World, Spawn, Cube, TEXT("Edge_S"), WorldM(0, -Half, 25), FVector(Side + 2, 2, 450));
	WorldBlocker(World, Spawn, Cube, TEXT("Edge_E"), WorldM(Half, 0, 25), FVector(2, Side + 2, 450));
	WorldBlocker(World, Spawn, Cube, TEXT("Edge_W"), WorldM(-Half, 0, 25), FVector(2, Side + 2, 450));

	// A blocked cut (a trench, a pit) gets hidden walls round its footprint, `rim` in from its edge (out
	// from it, for a negative rim): four round a rectangle, one per ray round a star. ftcore keeps the cuts
	// off the roads and the pads, so nothing on them meets one. A wall spans the ground along its line,
	// from 5 m under the lowest to 30 m over the highest.
	int32 Rims = 0;
	const auto RimWall = [&](ftcore::Vec2 From, ftcore::Vec2 To)
	{
		const float Length = FVector2D(To.x - From.x, To.y - From.y).Size();
		const int32 Steps = FMath::Max(1, FMath::CeilToInt(Length));
		float Low = Terrain->height(From);
		float High = Low;
		for (int32 Step = 1; Step <= Steps; ++Step)
		{
			const float T = static_cast<float>(Step) / static_cast<float>(Steps);
			const float Along = Terrain->height({From.x + (To.x - From.x) * T, From.y + (To.y - From.y) * T});
			Low = FMath::Min(Low, Along);
			High = FMath::Max(High, Along);
		}
		Low -= 5.f;
		High += 30.f;
		// Half a metre past either end, so neighbouring walls close their corner.
		WorldBlocker(World, Spawn, Cube, FString::Printf(TEXT("Rim_%d"), Rims), WorldM((From.x + To.x) * 0.5f, (From.y + To.y) * 0.5f, (Low + High) * 0.5f),
			FVector(Length + 1.f, 1.f, High - Low), FMath::RadiansToDegrees(FMath::Atan2(To.y - From.y, To.x - From.x)));
		++Rims;
	};
	int32 Cuts = 0;
	for (size_t I = 0; I < Terrain->featureCount(); ++I)
	{
		const ftcore::TerrainFeature& Cut = Terrain->feature(I);
		if (!Cut.blocked)
		{
			continue;
		}
		const float Grow = -Cut.rim;
		switch (Cut.kind)
		{
		case ftcore::TerrainFeature::Kind::Trench:
		{
			const float X0 = Cut.centre.x - Cut.half.x - Grow, X1 = Cut.centre.x + Cut.half.x + Grow;
			const float Y0 = Cut.centre.y - Cut.half.y - Grow, Y1 = Cut.centre.y + Cut.half.y + Grow;
			RimWall({X0, Y0}, {X1, Y0});
			RimWall({X1, Y0}, {X1, Y1});
			RimWall({X1, Y1}, {X0, Y1});
			RimWall({X0, Y1}, {X0, Y0});
			++Cuts;
			break;
		}
		case ftcore::TerrainFeature::Kind::Lake:
		case ftcore::TerrainFeature::Kind::Pit:
		{
			const auto RayEnd = [&Cut, Grow](size_t K)
			{
				const float Angle = 2.f * PI * static_cast<float>(K) / static_cast<float>(ftcore::TerrainFeature::kRays);
				const float R = Cut.ray[K % ftcore::TerrainFeature::kRays] + Grow;
				return ftcore::Vec2{Cut.centre.x + R * FMath::Cos(Angle), Cut.centre.y + R * FMath::Sin(Angle)};
			};
			for (size_t K = 0; K < ftcore::TerrainFeature::kRays; ++K)
			{
				RimWall(RayEnd(K), RayEnd(K + 1));
			}
			++Cuts;
			break;
		}
		default: // the sea lies past the edge's walls
			break;
		}
	}

	// Water: the sea, a plane from its edge of the square out past the horizon (the ground hides it inside
	// the coast), and the pools.
	int32 Waters = 0;
	UMaterialInstanceConstant* WaterTint = nullptr;
	for (size_t I = 0; I < Terrain->featureCount(); ++I)
	{
		const ftcore::TerrainFeature& Water = Terrain->feature(I);
		const bool bSea = Water.kind == ftcore::TerrainFeature::Kind::Sea;
		if (!bSea && Water.kind != ftcore::TerrainFeature::Kind::Pool)
		{
			continue;
		}
		if (!WaterTint)
		{
			// Still water: dark and glossy, so it shows the sky and the shore.
			WaterTint = Cast<UMaterialInstanceConstant>(WorldTint(Package, Base, TEXT("MI_World_Water"), FLinearColor(0.02f, 0.06f, 0.07f)));
			WaterTint->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), 0.05f);
			WaterTint->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Variation")), 0.02f);
			WaterTint->PostEditChange();
		}
		if (bSea)
		{
			// Side 0 is +x, 1 +y, 2 -x, 3 -y.
			const bool bAlongX = Water.side % 2 == 0;
			const float Out = Water.side < 2 ? 1.f : -1.f;
			const ftcore::Vec2 Middle = bAlongX ? ftcore::Vec2{Water.centre.x + Out * kWorldSeaReach * 0.5f, Water.centre.y}
												: ftcore::Vec2{Water.centre.x, Water.centre.y + Out * kWorldSeaReach * 0.5f};
			WorldWater(World, Spawn, Plane, WaterTint, TEXT("Sea"), WorldM(Middle, Water.level),
				bAlongX ? FVector(kWorldSeaReach, 2.f * kWorldSeaReach, 1.f) : FVector(2.f * kWorldSeaReach, kWorldSeaReach, 1.f));
		}
		else
		{
			WorldWater(World, Spawn, Plane, WaterTint, FString::Printf(TEXT("Pool_%d"), Waters), WorldM(Water.centre, Water.level),
				FVector(Water.half.x * 2.f, Water.half.y * 2.f, 1.f));
		}
		++Waters;
	}

	int32 Instances = 0;
	// Plazas, 3 cm over their pads: a step the character walks over. The roads are the ground's own.
	{
		AActor* Holder = WorldHolder(World, Spawn, TEXT("Plazas"));
		UInstancedStaticMeshComponent* Plazas = WorldIsm(Holder, TEXT("PlazaDiscs"), Cylinder, PlazaTint);
		const float Plaza = ftcore::WorldLayout::kPlazaRadius * 2.f;
		for (size_t D = 0; D < Layout->districtCount(); ++D)
		{
			Plazas->AddInstance(FTransform(FRotator::ZeroRotator, WorldM(Layout->district(D).centre, Terrain->padHeight(D) - 0.47f),
				FVector(Plaza, Plaza, 1.f)));
			++Instances;
		}
	}

	// Buildings, on their pads: a body, sunk half a metre so no gap shows at its foot, with a pitched roof in
	// a town or a flat cap on a tower.
	{
		AActor* Holder = WorldHolder(World, Spawn, TEXT("Buildings"));
		UInstancedStaticMeshComponent* Bodies = WorldIsm(Holder, TEXT("Bodies"), Cube, BuildingTint);
		UInstancedStaticMeshComponent* Roofs = WorldIsm(Holder, TEXT("Roofs"), Cube, RoofTint);
		const bool bTown = Layout->kind() == ftcore::WorldKind::Town;
		for (size_t I = 0; I < Layout->blockCount(); ++I)
		{
			const ftcore::WorldBlock& B = Layout->block(I);
			const float Degrees = FMath::RadiansToDegrees(B.yaw);
			const float Floor = Terrain->padHeight(B.district);
			Bodies->AddInstance(FTransform(FRotator(0.f, Degrees, 0.f), WorldM(B.pos, Floor + (B.height - 0.5f) * 0.5f),
				FVector(B.width, B.depth, B.height + 0.5f)));
			++Instances;
			if (!B.roof)
			{
				continue;
			}
			if (bTown)
			{
				const float Pitch = B.depth * 0.72f;
				Roofs->AddInstance(FTransform(FRotator(0.f, Degrees, 45.f), WorldM(B.pos, Floor + B.height), FVector(B.width + 0.4f, Pitch, Pitch)));
			}
			else
			{
				Roofs->AddInstance(FTransform(FRotator(0.f, Degrees, 0.f), WorldM(B.pos, Floor + B.height + 0.5f),
					FVector(B.width + 1.f, B.depth + 1.f, 1.f)));
			}
			++Instances;
		}
	}

	// Trees (a trunk and a crown) and rocks on the open ground, each sunk as far as the slope under it falls
	// across its foot. None stands in water, in or beside a cut, or on bare steep ground.
	int32 Skipped = 0;
	{
		AActor* Holder = WorldHolder(World, Spawn, TEXT("Props"));
		UInstancedStaticMeshComponent* Trunks = WorldIsm(Holder, TEXT("Trunks"), Cylinder, WoodTint);
		UInstancedStaticMeshComponent* Crowns = WorldIsm(Holder, TEXT("Crowns"), Sphere, LeavesTint);
		UInstancedStaticMeshComponent* Rocks = WorldIsm(Holder, TEXT("Rocks"), Sphere, RockTint);
		for (size_t I = 0; I < Layout->propCount(); ++I)
		{
			const ftcore::WorldProp& P = Layout->prop(I);
			const ftcore::Vec2 Fall = Terrain->gradient(P.pos);
			const float Slope = FMath::Sqrt(Fall.x * Fall.x + Fall.y * Fall.y);
			bool bClear = Slope <= kWorldPropMaxSlope && Terrain->waterDepth(P.pos) <= 0.f;
			for (size_t F = 0; bClear && F < Terrain->featureCount(); ++F)
			{
				bClear = !Terrain->feature(F).covers(P.pos, -3.f);
			}
			if (!bClear)
			{
				++Skipped;
				continue;
			}
			const float S = P.scale;
			if (P.kind == 0)
			{
				const float Foot = Terrain->height(P.pos) - 0.2f * S * Slope;
				Trunks->AddInstance(FTransform(FRotator::ZeroRotator, WorldM(P.pos, Foot + 1.5f * S), FVector(0.4f * S, 0.4f * S, 3.f * S)));
				Crowns->AddInstance(FTransform(FRotator::ZeroRotator, WorldM(P.pos, Foot + 4.f * S), FVector(3.2f * S)));
				Instances += 2;
			}
			else
			{
				const float Turn = static_cast<float>((I * 47u) % 360u);
				const float Foot = Terrain->height(P.pos) - 0.8f * S * Slope;
				Rocks->AddInstance(FTransform(FRotator(0.f, Turn, 0.f), WorldM(P.pos, Foot + 0.3f * S), FVector(1.6f * S, 1.3f * S, 0.9f * S)));
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
		const float Z = Terrain->padHeight(D); // the landmark stands in the building ring, on the pad
		const int32 N = static_cast<int32>(D);
		switch (Dist.landmarkShape)
		{
		case 1: // a pillar with a ring
			WorldBlock(World, Spawn, Cylinder, LandmarkTint, FString::Printf(TEXT("Landmark_%d"), N), WorldM(Dist.landmark, Z + H * 0.5f), FVector(W, W, H));
			WorldBlock(World, Spawn, Cylinder, LandmarkTint, FString::Printf(TEXT("Landmark_%d_Ring"), N), WorldM(Dist.landmark, Z + H * 0.72f),
				FVector(W * 2.2f, W * 2.2f, 1.5f));
			break;
		case 2: // a stepped spire
			WorldBlock(World, Spawn, Cube, LandmarkTint, FString::Printf(TEXT("Landmark_%d"), N), WorldM(Dist.landmark, Z + H * 0.25f), FVector(W, W, H * 0.5f));
			WorldBlock(World, Spawn, Cube, LandmarkTint, FString::Printf(TEXT("Landmark_%d_Mid"), N), WorldM(Dist.landmark, Z + H * 0.65f),
				FVector(W * 0.66f, W * 0.66f, H * 0.3f), FRotator(0.f, 45.f, 0.f));
			WorldBlock(World, Spawn, Cube, LandmarkTint, FString::Printf(TEXT("Landmark_%d_Top"), N), WorldM(Dist.landmark, Z + H * 0.9f),
				FVector(W * 0.33f, W * 0.33f, H * 0.2f));
			break;
		default: // a pillar
			WorldBlock(World, Spawn, Cylinder, LandmarkTint, FString::Printf(TEXT("Landmark_%d"), N), WorldM(Dist.landmark, Z + H * 0.5f), FVector(W, W, H));
			break;
		}
		WorldBlock(World, Spawn, Sphere, BeaconTint, FString::Printf(TEXT("Landmark_%d_Beacon"), N), WorldM(Dist.landmark, Z + H + 3.f), FVector(6.f));
		++Landmarks;
	}

	// The anchor: one large shape floating over the centre, visible from the whole world.
	WorldBlock(World, Spawn, Sphere, AnchorTint, TEXT("Anchor"), WorldM(0, 0, 260), FVector(60.f));
	WorldBlock(World, Spawn, Cylinder, AnchorTint, TEXT("Anchor_Ring"), WorldM(0, 0, 260), FVector(150.f, 150.f, 3.f), FRotator(12.f, 0.f, 0.f));

	FTScene::SpawnLook(World, Spawn, ftcore::SceneLook{}, kWorldSun);

	AFTExploreDirector* Director = World->SpawnActor<AFTExploreDirector>(FVector(0, 0, 200), FRotator::ZeroRotator, Spawn);
	Director->WorldId = Id;
	for (size_t D = 0; D < Layout->districtCount(); ++D)
	{
		Director->DistrictFloors.Add(Terrain->padHeight(D) * 100.f);
	}
	Director->SetActorLabel(TEXT("ExploreDirector"));

	const ftcore::WorldDistrict& First = Layout->district(0);
	APlayerStart* Start = World->SpawnActor<APlayerStart>(WorldM(First.origin, Terrain->padHeight(0) + 1.f),
		FRotator(0.f, FMath::RadiansToDegrees(Layout->spawnYaw(0)), 0.f), Spawn);
	Start->SetActorLabel(TEXT("PlayerStart"));

	WorldStyle(World, Spawn, StyleSpec);

	// A navmesh over each district (plaza and building ring), built here and saved with the map, 30 m
	// either side of its pad for the ground round it. The roads between districts are straight and clear,
	// so nothing walks them by navmesh.
	const float NavSide = (ftcore::WorldLayout::kDistrictRadius + 5.f) * 200.f;
	TArray<ANavMeshBoundsVolume*> NavBounds;
	for (size_t D = 0; D < Layout->districtCount(); ++D)
	{
		NavBounds.Add(WorldNavBounds(World, Spawn, WorldM(Layout->district(D).centre, Terrain->padHeight(D)), FVector(NavSide, NavSide, 6000.f),
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
		const ftcore::Vec2 Origin = Layout->district(D).origin;
		const FVector From = WorldM(Origin, Terrain->height(Origin) + 1.f);
		for (size_t G = 0; G < Goals.size(); ++G)
		{
			const ftcore::Vec2 Goal = Layout->toWorld(D, Goals[G]);
			const FVector To = WorldM(Goal, Terrain->height(Goal) + 1.f);
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

	// The ground's range, and how much of the square climbs steeper than 1 in 1 (45 degrees: the character
	// walks up to 44.8).
	float Lowest = Terrain->sample(0, 0);
	float Highest = Lowest;
	int32 Inside = 0;
	int32 Steep = 0;
	for (int32 Y = 0; Y < Samples; ++Y)
	{
		for (int32 X = 0; X < Samples; ++X)
		{
			const float Here = Terrain->sample(static_cast<size_t>(X), static_cast<size_t>(Y));
			Lowest = FMath::Min(Lowest, Here);
			Highest = FMath::Max(Highest, Here);
			const ftcore::Vec2 Middle{(static_cast<float>(X) + 0.5f) * Terrain->spacing() - Terrain->extent() * 0.5f,
				(static_cast<float>(Y) + 0.5f) * Terrain->spacing() - Terrain->extent() * 0.5f};
			if (X + 1 < Samples && Y + 1 < Samples && FMath::Abs(Middle.x) < Half && FMath::Abs(Middle.y) < Half)
			{
				++Inside;
				const ftcore::Vec2 Climb = Terrain->gradient(Middle);
				Steep += Climb.x * Climb.x + Climb.y * Climb.y > 1.f ? 1 : 0;
			}
		}
	}
	float PadLow = Terrain->padHeight(0);
	float PadHigh = PadLow;
	for (size_t D = 1; D < Layout->districtCount(); ++D)
	{
		PadLow = FMath::Min(PadLow, Terrain->padHeight(D));
		PadHigh = FMath::Max(PadHigh, Terrain->padHeight(D));
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTBuildExploreWorld: world %s's ground: %.1f to %.1f m (pads %.1f to %.1f m), %.1f%% of the square steeper than 1 in 1; %d cuts walled in by %d hidden walls; %d water planes; %d props left off steep, wet or cut ground."),
		*Id, Lowest, Highest, PadLow, PadHigh, 100.0 * Steep / FMath::Max(Inside, 1), Cuts, Rims, Waters, Skipped);

	int32 Actors = 0;
	const FBox Bounds = WorldBounds(World, Actors);
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTBuildExploreWorld: world %s (look %s): %d districts, %d roads, %d buildings, %d props, %d instances, %d landmarks, %d actors; bounds %.0f x %.0f m, %.0f m tall; %d goals reachable from each district."),
		*Id, WorldStyled(StyleSpec) ? *StyleSpec : TEXT("photoreal"), static_cast<int32>(Layout->districtCount()), static_cast<int32>(Layout->roadCount()),
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
	// -Style=<spec> adds a style controller over the photoreal look, as for a world.
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

	FTScene::SpawnLook(World, Spawn, ftcore::SceneLook{}, kWorldSun);
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
		TEXT("FTBuildExploreWorld: overworld (look %s): %d worlds, %d roads, %d emblem blocks, %d props, %d instances, %d actors; bounds %.0f x %.0f m, %.0f m tall."),
		WorldStyled(StyleSpec) ? *StyleSpec : TEXT("photoreal"), static_cast<int32>(Over->nodeCount()), static_cast<int32>(Over->roadCount()), static_cast<int32>(Over->blockCount()),
		static_cast<int32>(Over->propCount()), Instances, Actors, Bounds.GetSize().X / 100.0, Bounds.GetSize().Y / 100.0, Bounds.Max.Z / 100.0);
	return WorldSave(Package, World, File);
}
} // namespace
#endif
