#include "FTBuildLookLabWorldCommandlet.h"
#include "FTSceneLook.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraActor.h"
#include "Components/LocalFogVolumeComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/LocalFogVolume.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FTLookLabGameMode.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

namespace FTLookLabBuild
{
const TCHAR* const kMapPath = TEXT("/Game/Maps/Look_Lab");

#if WITH_EDITOR
// A Nanite copy of an engine shape at Path, saved, or null with an error logged. The look's opaque surfaces stand on
// these; the water and the glass keep the engine's shapes, since Nanite draws neither, and so does the world's plain
// surface, whose master isn't built for Nanite.
UStaticMesh* NaniteCopy(UStaticMesh* Source, const TCHAR* Path)
{
	const FString File = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
	UPackage* Package = CreatePackage(Path);
	UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Source, Package, *FPackageName::GetShortName(Path));
	Mesh->SetFlags(RF_Public | RF_Standalone);
	FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
	Nanite.bEnabled = true;
	Mesh->SetNaniteSettings(Nanite);
	Mesh->Build(true);
	Mesh->PostEditChange();
	FAssetRegistryModule::AssetCreated(Mesh);
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Mesh, *File, Save))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookLabWorld: couldn't save %s."), *File);
		return nullptr;
	}
	return Mesh;
}
#endif
} // namespace FTLookLabBuild

UFTBuildLookLabWorldCommandlet::UFTBuildLookLabWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildLookLabWorldCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	using namespace FTLookLabBuild;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!Cube || !Plane || !Sphere)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookLabWorld: /Engine/BasicShapes is missing."));
		return 1;
	}
	UStaticMesh* NaniteCube = NaniteCopy(Cube, TEXT("/Game/Look/SM_Look_Cube"));
	UStaticMesh* NaniteSphere = NaniteCopy(Sphere, TEXT("/Game/Look/SM_Look_Sphere"));
	if (!NaniteCube || !NaniteSphere)
	{
		return 1;
	}
	const auto Load = [](const TCHAR* Package)
	{
		return LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("%s.%s"), Package, *FPackageName::GetShortName(Package)));
	};
	UMaterialInterface* Water = Load(FTScene::LookWaterMaterial);
	UMaterialInterface* Glass = Load(FTScene::LookGlassMaterial);
	UMaterialInterface* Chrome = Load(FTScene::LookChromeMaterial);
	UMaterialInterface* Stone = Load(FTScene::LookWetStoneMaterial);
	UMaterialInterface* Glow = Load(FTScene::LookGlowMaterial);
	UMaterialInterface* Plain = FTScene::LoadSurface();
	if (!Water || !Glass || !Chrome || !Stone || !Glow)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookLabWorld: the look's materials are missing; run FTBuildLookMaterials first."));
		return 1;
	}

	// Start from nothing: loading the old map into the package would clash with the new world.
	const FString File = FPackageName::LongPackageNameToFilename(kMapPath, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookLabWorld: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(kMapPath);
	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, *FPackageName::GetShortName(kMapPath), Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookLabWorld: couldn't create the world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->DefaultGameMode = AFTLookLabGameMode::StaticClass();
	}

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const auto Prop = [&](UStaticMesh* Mesh, UMaterialInterface* Material, const TCHAR* Label, const FVector& Location,
						  const FVector& Scale, const FRotator& Rotation = FRotator::ZeroRotator)
	{
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, Rotation, Spawn);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		if (Material)
		{
			Actor->GetStaticMeshComponent()->SetMaterial(0, Material);
		}
		Actor->SetActorScale3D(Scale);
		Actor->SetActorLabel(Label);
		return Actor;
	};

	// The yard: wet stone 3 m thick, its top at z = 0, round a hole 6 m square (x 400..1000, y -300..300) whose sides are
	// the basin's walls. The basin's floor lies 2 m down; a ramp climbs from it to just under the water at the far wall.
	Prop(NaniteCube, Stone, TEXT("YardNear"), FVector(-300, 0, -150), FVector(14, 30, 3));
	Prop(NaniteCube, Stone, TEXT("YardFar"), FVector(1700, 0, -150), FVector(14, 30, 3));
	Prop(NaniteCube, Stone, TEXT("YardLeft"), FVector(700, -900, -150), FVector(6, 12, 3));
	Prop(NaniteCube, Stone, TEXT("YardRight"), FVector(700, 900, -150), FVector(6, 12, 3));
	Prop(NaniteCube, Stone, TEXT("BasinFloor"), FVector(700, 0, -250), FVector(6, 6, 1));
	// The ramp's top runs from (600, -200) to (1000, -12): 25.2 degrees, 442 cm along, 50 cm thick.
	Prop(NaniteCube, Stone, TEXT("BasinRamp"), FVector(810.6, 0, -128.6), FVector(4.42, 5.98, 0.5), FRotator(25.2, 0, 0));
	Prop(Plane, Water, TEXT("Water"), FVector(700, 0, -10), FVector(6, 6, 1));
	// A dry block on the rim, for the wet stone against the world's plain surface.
	Prop(Cube, Plain, TEXT("DryBlock"), FVector(300, -450, 40), FVector(0.8, 0.8, 0.8));

	// Glass: a pane 3 m wide on the far rim, a strip glowing behind it and a ball beside the basin.
	Prop(Cube, Glass, TEXT("GlassPane"), FVector(1250, -150, 125), FVector(0.02, 3, 2.5));
	Prop(NaniteCube, Glow, TEXT("GlowStrip"), FVector(1550, -150, 150), FVector(0.1, 4, 0.3));
	Prop(NaniteCube, Glow, TEXT("GlowPost"), FVector(1550, -350, 100), FVector(0.1, 0.15, 2));
	Prop(Sphere, Glass, TEXT("GlassBall"), FVector(1150, 450, 80), FVector(1.6));

	// The lamp's alcove: a back wall, two sides and a roof, open toward the yard, with the chrome beside it.
	const FVector Alcove(1900, 700, 0);
	Prop(Cube, Plain, TEXT("AlcoveBack"), Alcove + FVector(150, 0, 175), FVector(0.3, 4, 3.5));
	Prop(Cube, Plain, TEXT("AlcoveLeft"), Alcove + FVector(0, -200, 175), FVector(3, 0.3, 3.5));
	Prop(Cube, Plain, TEXT("AlcoveRight"), Alcove + FVector(0, 200, 175), FVector(3, 0.3, 3.5));
	Prop(Cube, Plain, TEXT("AlcoveRoof"), Alcove + FVector(0, 0, 365), FVector(3.3, 4.3, 0.3));
	Prop(NaniteCube, Chrome, TEXT("ChromeSlab"), Alcove + FVector(-250, -350, 110), FVector(1.5, 0.15, 2.2), FRotator(0, 35, 0));
	Prop(NaniteSphere, Chrome, TEXT("ChromeBall"), Alcove + FVector(-300, 0, 60), FVector(1.2));

	// Sun, sky, air and the camera finish: the shared look, which -Look=<world> then changes. The sun is low, so the
	// water, glass and chrome have a bright sky and a long light to reflect.
	FTScene::SpawnLook(World, Spawn, ftcore::SceneLook{}, FRotator(-22, 150, 0));

	// The practical lamp under the alcove's roof, as SceneLook{} lights it; the world's look relights it.
	const ftcore::SceneLook Shared;
	APointLight* Lamp = World->SpawnActor<APointLight>(Alcove + FVector(20, 0, 280), FRotator::ZeroRotator, Spawn);
	UPointLightComponent* Light = Lamp->PointLightComponent;
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetIntensity(Shared.practicalCandela);
	Light->SetUseTemperature(true);
	Light->SetTemperature(Shared.practicalKelvin);
	Light->SetVolumetricScatteringIntensity(Shared.practicalScatter);
	Light->SetAttenuationRadius(900.f);
	Light->SetSourceRadius(4.f);
	Lamp->Tags.Add(FTScene::PracticalTag);
	Lamp->SetActorLabel(TEXT("PracticalLamp"));
	// Thicker air in the alcove, so the lamp's light shows in it as it scatters (the volume renders into the volumetric
	// fog, which the practicals light).
	ALocalFogVolume* Haze = World->SpawnActor<ALocalFogVolume>(Alcove + FVector(-60, 0, 180), FRotator::ZeroRotator, Spawn);
	Haze->SetActorScale3D(FVector(0.7f, 0.6f, 0.45f));
	Haze->GetComponent()->SetRadialFogExtinction(0.25f);
	Haze->GetComponent()->SetHeightFogExtinction(0.f);
	Haze->SetActorLabel(TEXT("AlcoveHaze"));

	// A stand-in figure on the yard, the size of a lead, and the trick that lifts a character off dark ground: an
	// unshadowed rim light behind it that lights only lighting channel 1, which only characters join.
	AStaticMeshActor* Figure = Prop(Sphere, Plain, TEXT("Figure"), FVector(200, 250, 85), FVector(0.45f, 0.35f, 1.7f));
	Figure->GetStaticMeshComponent()->SetLightingChannels(true, true, false);
	const FVector RimAt(420, 420, 260);
	ASpotLight* Rim = World->SpawnActor<ASpotLight>(RimAt, (FVector(200, 250, 110) - RimAt).Rotation(), Spawn);
	USpotLightComponent* RimLight = Rim->SpotLightComponent;
	RimLight->SetMobility(EComponentMobility::Movable);
	RimLight->SetIntensityUnits(ELightUnits::Candelas);
	RimLight->SetIntensity(400.f);
	RimLight->SetCastShadows(false);
	RimLight->SetLightingChannels(false, true, false);
	RimLight->SetVolumetricScatteringIntensity(0.f);
	RimLight->SetOuterConeAngle(20.f);
	RimLight->SetInnerConeAngle(12.f);
	RimLight->SetAttenuationRadius(800.f);
	Rim->SetActorLabel(TEXT("FigureRim"));

	// The views: the yard from the near side; the water and the glass beyond it; the chrome and the lamp.
	const auto View = [&](int32 Index, const TCHAR* Label, const FVector& Eye, const FVector& Target)
	{
		ACameraActor* Camera = World->SpawnActor<ACameraActor>(Eye, (Target - Eye).Rotation(), Spawn);
		Camera->Tags.Add(AFTLookLabGameMode::ViewTag(Index));
		Camera->SetActorLabel(Label);
	};
	View(1, TEXT("ViewWide"), FVector(-450, -250, 260), FVector(1100, 150, 40));
	View(2, TEXT("ViewWater"), FVector(250, -380, 170), FVector(950, 0, -40));
	View(3, TEXT("ViewChrome"), FVector(1150, 250, 200), FVector(1850, 600, 140));

	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(-600, 0, 100), FRotator::ZeroRotator, Spawn);
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookLabWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildLookLabWorld: saved %s."), *File);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookLabWorld needs an editor build."));
	return 1;
#endif
}
