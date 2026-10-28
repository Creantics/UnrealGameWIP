#include "FTBuildDemoWorldCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PostProcessComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Engine/CollisionProfile.h"
#include "FTCharacterPreview.h"
#include "FTMeshFacing.h"
#include "FTPhysicsComponent.h"
#include "FTSceneLook.h"
#include "FTStyleController.h"
#include "FTStyleParameters.h"
#include "FTVfxComponent.h"
#include "NiagaraSystem.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/StylePack.h"

#include <optional>
#include <string_view>
#endif

namespace
{
const TCHAR* const kMapFolder = TEXT("/Game/Maps");

#if WITH_EDITOR
// The static pack instance for a named preset, so the editor viewport shows the style without playing.
// Empty for "none" and for dial specs (those only show in play, through the controller's dynamic instance).
FString StaticInstancePath(const FString& Spec)
{
	const FString Name = Spec.TrimStartAndEnd().ToLower();
	const TCHAR* const Packs[][2] = {
		{TEXT("solhaven"), TEXT("Solhaven")},
		{TEXT("lattice"), TEXT("Lattice")},
		{TEXT("unwritten"), TEXT("Unwritten")},
		{TEXT("restored"), TEXT("Restored")},
		{TEXT("inksolhaven"), TEXT("InkSolhaven")},
	};
	for (const auto& Pack : Packs)
	{
		if (Name == Pack[0])
		{
			return FString::Printf(TEXT("/Game/Rendering/MI_Style_%s.MI_Style_%s"), Pack[1], Pack[1]);
		}
	}
	return FString();
}

AStaticMeshActor* SpawnProp(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Mesh, UMaterialInterface* Material,
	const TCHAR* Label, const FVector& Location, const FVector& Scale, float Yaw = 0.f)
{
	AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(Location, FRotator(0, Yaw, 0), Spawn);
	Prop->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	if (Material)
	{
		Prop->GetStaticMeshComponent()->SetMaterial(0, Material);
	}
	Prop->SetActorScale3D(Scale);
	Prop->SetActorLabel(Label);
	return Prop;
}

// A prop that simulates from the start, with a /Game/Physics material on its body.
AStaticMeshActor* PhysSpawnSimProp(UWorld* World, const FActorSpawnParameters& Spawn, UStaticMesh* Mesh, UMaterialInterface* Material,
	UPhysicalMaterial* PhysMaterial, const TCHAR* Label, const FVector& Location, const FVector& Scale, float Yaw = 0.f)
{
	AStaticMeshActor* Prop = SpawnProp(World, Spawn, Mesh, Material, Label, Location, Scale, Yaw);
	UStaticMeshComponent* Body = Prop->GetStaticMeshComponent();
	// Static actors drop the simulate flag, so make it movable first.
	Body->SetMobility(EComponentMobility::Movable);
	Body->SetCollisionProfileName(UCollisionProfile::PhysicsActor_ProfileName);
	Body->SetPhysMaterialOverride(PhysMaterial);
	Body->SetSimulatePhysics(true);
	return Prop;
}
#endif
}

UFTBuildDemoWorldCommandlet::UFTBuildDemoWorldCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildDemoWorldCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	// -Map=<name> picks the map under /Game/Maps (default Demo), so each style can have its own map.
	FString MapName = TEXT("Demo");
	FParse::Value(*Params, TEXT("Map="), MapName);
	if (MapName.IsEmpty() || !FName::IsValidXName(MapName, INVALID_LONGPACKAGE_CHARACTERS))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: '%s' is not a valid map name."), *MapName);
		return 1;
	}
	const FString MapPath = FString::Printf(TEXT("%s/%s"), kMapFolder, *MapName);

	// -Style=<spec> adds a style controller over the photoreal look (none without it, or with "none"). The
	// separator flag is off so a dial ("S3,U0.5") keeps its commas.
	FString StyleSpec;
	FParse::Value(*Params, TEXT("Style="), StyleSpec, false);
	StyleSpec.TrimStartAndEndInline();
	UMaterialInterface* StyleMaterial = nullptr;
	UMaterialInterface* StaticInstance = nullptr;
	if (!StyleSpec.IsEmpty() && !StyleSpec.Equals(TEXT("none"), ESearchCase::IgnoreCase))
	{
		const FTCHARToUTF8 Utf8(*StyleSpec);
		if (!ftcore::parseStyleSpec(std::string_view(Utf8.Get(), static_cast<size_t>(Utf8.Length()))))
		{
			UE_LOG(LogForgottenThird, Error,
				TEXT("FTBuildDemoWorld: bad -Style=%s (none|solhaven|lattice|unwritten|restored|inksolhaven|dial like S3,U0.5)."),
				*StyleSpec);
			return 1;
		}
		StyleMaterial = LoadObject<UMaterialInterface>(nullptr, FTStyle::MaterialObjectPath);
		if (!StyleMaterial)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: %s is missing; run FTBuildStyleMaterials first."),
				FTStyle::MaterialObjectPath);
			return 1;
		}
		const FString InstancePath = StaticInstancePath(StyleSpec);
		if (!InstancePath.IsEmpty())
		{
			StaticInstance = LoadObject<UMaterialInterface>(nullptr, *InstancePath);
			if (!StaticInstance)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: %s is missing; run FTBuildStyleMaterials first."),
					*InstancePath);
				return 1;
			}
		}
	}

	// Start from nothing: loading the old map into the package would clash with the new world.
	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: couldn't replace %s."), *File);
		return 1;
	}
	UPackage* Package = CreatePackage(*MapPath);

	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, *MapName, Package);
	if (!World)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: couldn't create the world."));
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Floor: a 40 m x 40 m slab whose top is at z = 0.
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* Grey = FTScene::LoadSurface();
	if (!Cube)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: /Engine/BasicShapes/Cube is missing."));
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

	// Sun, low enough to rake across the face.
	FTScene::SpawnLook(World, Spawn, ftcore::SceneLook{}, FRotator(-35, -40, 0));

	// The character stands at the origin; the player starts 2.5 m in front of it at eye height,
	// looking back along -X.
	AFTCharacterPreview* Preview = World->SpawnActor<AFTCharacterPreview>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	Preview->SetActorLabel(TEXT("CharacterPreview"));

	// -Mesh=<object path> puts a skeletal mesh on the preview and turns it to face the player.
	FString MeshPath;
	if (FParse::Value(*Params, TEXT("Mesh="), MeshPath))
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
		if (!Mesh)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: couldn't load skeletal mesh %s."), *MeshPath);
			World->DestroyWorld(false);
			return 1;
		}
		Preview->GetBody()->SetSkeletalMesh(Mesh);
		const float Yaw = FTMeshFacing::FacingYaw(*Mesh);
		Preview->SetActorRotation(FRotator(0, Yaw, 0));
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildDemoWorld: preview uses %s, yaw %.1f."), *Mesh->GetPathName(), Yaw);
	}

	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(250, 0, 160), FRotator(0, 180, 0), Spawn);
	Start->SetActorLabel(TEXT("PlayerStart"));

	// Props for the style to bite on: a small cluster behind the character (the player looks along -X),
	// and two far shapes so the outline taper and the depth effects show. The character stays the
	// centrepiece; nothing stands between it and the player start.
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UStaticMesh* Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (!Sphere || !Cylinder || !Cone)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: a /Engine/BasicShapes mesh is missing."));
		World->DestroyWorld(false);
		return 1;
	}
	// BasicShapes are 100 cm across with their pivot at the centre.
	SpawnProp(World, Spawn, Cube, Grey, TEXT("PropCrate"), FVector(-180, 160, 40), FVector(0.8f), 20.f);
	SpawnProp(World, Spawn, Sphere, Grey, TEXT("PropBall"), FVector(-240, -130, 35), FVector(0.7f));
	SpawnProp(World, Spawn, Cylinder, Grey, TEXT("PropPillar"), FVector(-300, 40, 110), FVector(0.5f, 0.5f, 2.2f));
	SpawnProp(World, Spawn, Cone, Grey, TEXT("PropCone"), FVector(-130, -230, 40), FVector(0.6f, 0.6f, 0.8f));
	SpawnProp(World, Spawn, Cube, Grey, TEXT("PropFarBlock"), FVector(-1500, 450, 150), FVector(3.f), 35.f);
	SpawnProp(World, Spawn, Cylinder, Grey, TEXT("PropFarTower"), FVector(-1200, -700, 300), FVector(1.2f, 1.2f, 6.f));

	// The physics corner: simulating props on the character's left, behind it and clear of the other props,
	// one per material. A stack of three crates (2 cm gaps, so they settle onto each other), a stone block, a
	// metal drum, a sack and a rubber ball dropped from 1.5 m. -NoPhysics leaves it out.
	if (!FParse::Param(*Params, TEXT("NoPhysics")))
	{
		auto LoadPhys = [](const TCHAR* Name) {
			const FString Path = UFTPhysicsComponent::MaterialPath(Name);
			UPhysicalMaterial* Material = LoadObject<UPhysicalMaterial>(nullptr, *Path);
			if (!Material)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: %s is missing; run FTBuildPhysics first (or pass -NoPhysics)."), *Path);
			}
			return Material;
		};
		UPhysicalMaterial* Wood = LoadPhys(TEXT("wood"));
		UPhysicalMaterial* Stone = LoadPhys(TEXT("stone"));
		UPhysicalMaterial* Metal = LoadPhys(TEXT("metal"));
		UPhysicalMaterial* Sack = LoadPhys(TEXT("sack"));
		UPhysicalMaterial* Rubber = LoadPhys(TEXT("rubber"));
		if (!Wood || !Stone || !Metal || !Sack || !Rubber)
		{
			World->DestroyWorld(false);
			return 1;
		}
		const FVector Corner(-450, 550, 0);
		PhysSpawnSimProp(World, Spawn, Cube, Grey, Wood, TEXT("Phys_CrateLow"), Corner + FVector(0, 0, 26), FVector(0.5f));
		PhysSpawnSimProp(World, Spawn, Cube, Grey, Wood, TEXT("Phys_CrateMid"), Corner + FVector(3, -2, 78), FVector(0.5f), 8.f);
		PhysSpawnSimProp(World, Spawn, Cube, Grey, Wood, TEXT("Phys_CrateTop"), Corner + FVector(-2, 3, 130), FVector(0.5f), -12.f);
		PhysSpawnSimProp(World, Spawn, Cube, Grey, Stone, TEXT("Phys_StoneBlock"), Corner + FVector(-110, -90, 31), FVector(0.6f), 25.f);
		PhysSpawnSimProp(World, Spawn, Cylinder, Grey, Metal, TEXT("Phys_MetalDrum"), Corner + FVector(80, -120, 41), FVector(0.4f, 0.4f, 0.8f));
		PhysSpawnSimProp(World, Spawn, Cube, Grey, Sack, TEXT("Phys_Sack"), Corner + FVector(-80, 110, 17), FVector(0.6f, 0.4f, 0.3f), 30.f);
		PhysSpawnSimProp(World, Spawn, Sphere, Grey, Rubber, TEXT("Phys_RubberBall"), Corner + FVector(90, 100, 150), FVector(0.4f));
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildDemoWorld: placed the physics corner (7 simulating props) at (%.0f, %.0f)."),
			Corner.X, Corner.Y);
	}

	// Effects beside the character, off the line between it and the player start (which looks along -X
	// from x = 250): one on each side and one behind. -NoVfx leaves them out.
	if (!FParse::Param(*Params, TEXT("NoVfx")))
	{
		struct FPlacement
		{
			const TCHAR* Effect;
			FVector Location;
		};
		const FPlacement Placements[] = {
			{TEXT("ember_swirl"), FVector(30, 150, 0)},
			{TEXT("heal_ring"), FVector(20, -170, 2)},
			{TEXT("smoke"), FVector(-160, -90, 0)},
		};
		for (const FPlacement& Place : Placements)
		{
			const FName Effect(Place.Effect);
			UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UFTVfxComponent::SystemPath(Effect));
			if (!System)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: %s is missing; run FTBuildVfx first (or pass -NoVfx)."),
					*UFTVfxComponent::SystemPath(Effect));
				World->DestroyWorld(false);
				return 1;
			}
			AFTVfxActor* Actor = World->SpawnActor<AFTVfxActor>(Place.Location, FRotator::ZeroRotator, Spawn);
			Actor->GetVfx()->Effect = Effect;
			Actor->GetVfx()->System = System;
			Actor->SetActorLabel(FString::Printf(TEXT("Vfx_%s"), Place.Effect));
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildDemoWorld: placed %d effects."), static_cast<int32>(UE_ARRAY_COUNT(Placements)));
	}

	if (StyleMaterial)
	{
		AFTStyleController* Style = World->SpawnActor<AFTStyleController>(FVector(0, 0, 300), FRotator::ZeroRotator, Spawn);
		Style->StyleSpec = StyleSpec;
		Style->StyleMaterial = StyleMaterial;
		if (StaticInstance)
		{
			Style->GetPostProcess()->Settings.AddBlendable(StaticInstance, 1.f);
		}
		Style->SetActorLabel(TEXT("StyleController"));
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildDemoWorld: style %s%s."), *StyleSpec,
			StaticInstance ? TEXT("") : TEXT(" (no static instance: the editor viewport shows it only in play)"));
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildDemoWorld: photoreal look, no style controller."));
	}

	FAssetRegistryModule::AssetCreated(World);
	Package->MarkPackageDirty();

	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, World, *File, Save);

	World->DestroyWorld(false);

	if (!bSaved)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildDemoWorld: saved %s."), *File);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildDemoWorld needs an editor build."));
	return 1;
#endif
}
