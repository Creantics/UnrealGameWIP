#include "FTArenaGameMode.h"

#include "Camera/CameraActor.h"
#include "DrawDebugHelpers.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "FTSceneLook.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "FTExploreCharacter.h"
#include "FTExploreDirector.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "FTArenaFighter.h"
#include "FTArenaHUD.h"
#include "FTArenaRunner.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include <cmath>
#include <string>
#include <vector>

static_assert(AFTArenaFighter::TellSegments == 14, "Tell[14] in FTArenaFighter.h must follow TellSegments");

namespace
{
// The shortest tell the arena allows (s): an enemy's startup is its telegraph.
constexpr float kArenaMinTell = 0.4f;
// After a view is taken, the fight holds this long (wall seconds) so the frame it renders is that moment.
constexpr double kArenaShotHold = 1.0;
// A -ArenaShot run gives up this long after the first view (wall seconds).
constexpr double kArenaShotGiveUp = 200.0;

// The demo's lineup without -ArenaLineup: arena.json's enemy kind ids.
const TCHAR* const kArenaDefaultWave = TEXT("brute,skirmisher,breaker");
} // namespace

FVector AFTArenaGameMode::PlayerStartFor(float Radius)
{
	return FVector(-0.35 * static_cast<double>(Radius) * 100.0, 0.0, 0.0);
}

AFTArenaGameMode::AFTArenaGameMode()
{
	HUDClass = AFTArenaHUD::StaticClass();
	DefaultPawnClass = AFTArenaFighter::StaticClass();
	bHostsStory = false;
	PrimaryActorTick.bCanEverTick = true;
	Runner = CreateDefaultSubobject<UFTArenaRunner>(TEXT("Runner"));
}

void AFTArenaGameMode::StartPlay()
{
	const TCHAR* Cmd = FCommandLine::Get();
	FString Shot;
	if (FParse::Value(Cmd, TEXT("ArenaShot="), Shot) && !Shot.IsEmpty())
	{
		ShotPrefix = Shot;
		float Delay = ShotDelay;
		if (FParse::Value(Cmd, TEXT("ArenaShotDelay="), Delay))
		{
			ShotDelay = FMath::Clamp(Delay, 0.f, 60.f);
		}
		// The list has commas: read it to the next space.
		FString At;
		if (FParse::Value(Cmd, TEXT("ArenaShotAt="), At, false))
		{
			TArray<FString> Times;
			At.ParseIntoArray(Times, TEXT(","));
			for (const FString& Time : Times)
			{
				const double Seconds = FCString::Atod(*Time);
				if (Seconds >= 0.0)
				{
					ShotAt.Add(Seconds);
				}
			}
			ShotAt.Sort();
		}
	}
	bIdle = FParse::Param(Cmd, TEXT("ArenaIdle"));
	bAutoPlay = !bIdle && (FParse::Param(Cmd, TEXT("ArenaAutoPlay")) || !ShotPrefix.IsEmpty());
	bQuitOnFinish = FParse::Param(Cmd, TEXT("ArenaQuitOnFinish"));
	bOverview = FParse::Param(Cmd, TEXT("ArenaOverview"));
	bHoldLoad = FParse::Param(Cmd, TEXT("ArenaHoldLoad"));
	bBossSplit = FParse::Param(Cmd, TEXT("ArenaBossSplit"));
	bBossCam = FParse::Param(Cmd, TEXT("ArenaBossCam"));
	int32 SeedValue = 1;
	if (FParse::Value(Cmd, TEXT("ArenaSeed="), SeedValue))
	{
		Seed = static_cast<uint32>(SeedValue);
	}

	const bool bTable = LoadTable();
	LoadZones(bTable);

	// A zone in a world is held in that world's map. From another map (Demo_Arena), open it with this game mode on
	// the first tick; on it, the explore director that map carries is taken out before it begins play.
	if (bZoneFromFile && !Spec.world.empty() && GetWorld())
	{
		const FString Want = FT::ToFString(Worlds.worlds[Spec.worldIndex].map);
		if (UWorld::RemovePIEPrefix(GetWorld()->GetMapName()) != Want)
		{
			bTravelPending = true;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: zone %s is in %s; opening that map."), *FT::ToFString(Spec.id), *Want);
			AGameModeBase::StartPlay();
			return;
		}
		for (TActorIterator<AFTExploreDirector> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
	}
	// Skips AFTGameMode::StartPlay on purpose, as the other fight screens do: the story must not start here.
	AGameModeBase::StartPlay();

	BuildDef(bTable);

	// The enemies' tells: every startup at least kArenaMinTell (the data's loader doesn't hold it to that).
	float Shortest = 1e9f;
	for (int32 E = 0; E < static_cast<int32>(Def.enemyCount); ++E)
	{
		const ftcore::ArenaEnemyKind& Kind = Def.enemies[static_cast<size_t>(E)];
		for (int32 M = 0; M < static_cast<int32>(Kind.moveCount); ++M)
		{
			Shortest = FMath::Min(Shortest, Kind.moves[static_cast<size_t>(M)].attack.startup);
		}
	}
	if (Shortest < kArenaMinTell)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: an enemy move's tell is %.2f s, under the %.1f s floor."), Shortest, kArenaMinTell);
	}

	FString Kinds;
	for (int32 E = 0; E < static_cast<int32>(Def.enemyCount); ++E)
	{
		const uint8 Kind = Def.enemyKind[static_cast<size_t>(E)];
		Kinds += E > 0 ? TEXT(", ") : TEXT("");
		Kinds += Kind < Table.enemyIds.size() ? FT::ToFString(Table.enemyIds[Kind]) : FString(TEXT("built-in"));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: zone %s, kit %s, %d enemies (%s), ring %.1f m, spawn %.1f m, %d attacking at once, seed %u, %s; shortest tell %.2f s."),
		*FT::ToFString(Spec.id), *KitId, static_cast<int32>(Def.enemyCount), *Kinds, Def.ringRadius, Def.spawnDistance,
		static_cast<int32>(Def.attackers), Seed, UFTArenaRunner::PilotName(bAutoPlay, bIdle), Shortest);
}

bool AFTArenaGameMode::LoadTable()
{
	const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/arena.json");
	if (!IFileManager::Get().FileExists(*File))
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: no Data/Battle/arena.json; using the built-in fight."));
		return false;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: couldn't read Data/Battle/arena.json; using the built-in fight."));
		return false;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ftcore::importArenaTable(FT::BytesAsText(Bytes), Table, &Error, &Warnings))
	{
		// The loader's errors name paths and keys only.
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: Data/Battle/arena.json refused; using the built-in fight:\n%s"), *FT::ToFString(Error));
		return false;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s"), *FT::ToFString(W));
	}
	return true;
}

void AFTArenaGameMode::LoadZones(bool bTable)
{
	bZoneFromFile = false;
	Zones = ftcore::ArenaZoneTable();
	Worlds = ftcore::WorldTable();
	// The zone when the files can't give one: the plain 12 m ring of the old demo.
	Spec = ftcore::ArenaZoneSpec();
	Spec.id = "built-in";
	Spec.ring = MapRingRadius;
	Spec.spawn = 7.f;

	FString Want;
	FParse::Value(FCommandLine::Get(), TEXT("ArenaStage="), Want, false);
	Want = Want.TrimStartAndEnd();

	const auto ReadData = [](const TCHAR* Relative, TArray<uint8>& Bytes)
	{
		const FString File = FPaths::ProjectDir() / Relative;
		return IFileManager::Get().FileExists(*File) && FFileHelper::LoadFileToArray(Bytes, *File);
	};
	TArray<uint8> Bytes;
	if (!bTable || !ReadData(TEXT("Data/Battle/arenas.json"), Bytes))
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: no usable Data/Battle/arenas.json; using the built-in zone."));
		return;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	{
		TArray<uint8> WorldBytes;
		if (ReadData(TEXT("Data/Field/worlds.json"), WorldBytes) && !ftcore::importWorldTable(FT::BytesAsText(WorldBytes), Worlds, &Error))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: Data/Field/worlds.json refused:\n%s"), *FT::ToFString(Error));
			Worlds = ftcore::WorldTable();
		}
	}
	if (!ftcore::importArenaZoneTable(FT::BytesAsText(Bytes), Table, Worlds, Zones, &Error, &Warnings))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: Data/Battle/arenas.json refused; using the built-in zone:\n%s"), *FT::ToFString(Error));
		Zones = ftcore::ArenaZoneTable();
		return;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s"), *FT::ToFString(W));
	}
	const ftcore::ArenaZoneSpec* Found = Want.IsEmpty() ? Zones.defaultZone() : Zones.find(FT::ToUtf8(Want));
	if (!Found)
	{
		FString Ids;
		for (const ftcore::ArenaZoneSpec& Z : Zones.zones)
		{
			Ids += (Ids.IsEmpty() ? TEXT("") : TEXT(", ")) + FT::ToFString(Z.id);
		}
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaStage: no zone \"%s\" in arenas.json (%s); using the default."), *Want, *Ids);
		Found = Zones.defaultZone();
	}
	if (Found)
	{
		Spec = *Found;
		bZoneFromFile = true;
	}
}

void AFTArenaGameMode::BuildDef(bool bTable)
{
	Def = bTable ? Table.fallback : ftcore::defaultArenaDef();
	if (bTable)
	{
		// The lead's own kit (its attacks and its blocks); a lead without one plays the default fighter.
		const FString Want = FindKitId();
		const std::string WantId = FT::ToUtf8(Want);
		Def.fighter = Table.fighter(WantId);
		bool bOwn = false;
		for (const std::string& Id : Table.fighterIds)
		{
			bOwn = bOwn || Id == WantId;
		}
		KitId = bOwn ? Want : FString(TEXT("default"));
		if (!bOwn && Want != TEXT("default"))
		{
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: no fighter kit \"%s\" in arena.json; the lead plays the default kit."), *Want);
		}
	}
	else
	{
		KitId = TEXT("built-in");
	}
	int32 Attackers = 0;
	const bool bAttackersFlag = FParse::Value(FCommandLine::Get(), TEXT("ArenaAttackers="), Attackers);
	if (bTable)
	{
		// The lineup: -ArenaLineup's enemy kinds, else the zone's bosses (the sand ring's are the three-kind wave:
		// one of each shipped kind, one attacking at a time), so Load, the Release and the Read all come up without
		// a flag.
		FString Lineup;
		if (!FParse::Value(FCommandLine::Get(), TEXT("ArenaLineup="), Lineup, false) || Lineup.TrimStartAndEnd().IsEmpty())
		{
			Lineup = kArenaDefaultWave;
			if (bZoneFromFile && !Spec.bosses.empty())
			{
				Lineup.Empty();
				for (const std::string& Boss : Spec.bosses)
				{
					Lineup += (Lineup.IsEmpty() ? TEXT("") : TEXT(",")) + FT::ToFString(Boss);
				}
			}
		}
		TArray<FString> Ids;
		Lineup.ParseIntoArray(Ids, TEXT(","), true);
		int32 Count = 0;
		for (const FString& Id : Ids)
		{
			const std::string Want = FT::ToUtf8(Id.TrimStartAndEnd());
			int32 Found = -1;
			for (int32 K = 0; K < static_cast<int32>(Table.enemyIds.size()); ++K)
			{
				if (Table.enemyIds[static_cast<size_t>(K)] == Want)
				{
					Found = K;
					break;
				}
			}
			if (Found < 0)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaLineup: no enemy kind \"%s\" in arena.json; skipped."), *Id);
				continue;
			}
			if (Count >= MaxEnemies)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaLineup: more than %d enemies; the rest are left out."), MaxEnemies);
				break;
			}
			Def.enemies[static_cast<size_t>(Count)] = Table.enemies[static_cast<size_t>(Found)];
			Def.enemyKind[static_cast<size_t>(Count)] = static_cast<uint8>(Found);
			++Count;
		}
		if (Count > 0)
		{
			Def.enemyCount = static_cast<uint8>(Count);
			Def.attackers = 1;
		}
	}
	if (bAttackersFlag)
	{
		Def.attackers = static_cast<uint8>(FMath::Clamp(Attackers, 1, static_cast<int32>(Def.enemyCount)));
		Spec.attackers = Def.attackers; // the fit would otherwise put the zone's own back
	}

	// The zone, fitted to these enemies in its world's real layout: its walls, obstacles, ring and spawn. A world
	// that can't give the zone room falls back to the plain zone, with a warning.
	WorldData.reset();
	if (!Spec.world.empty() && Spec.worldIndex < Worlds.worlds.size())
	{
		std::string LayoutError;
		WorldData = std::make_unique<ftcore::WorldLayout>();
		if (!ftcore::generateWorldLayout(Worlds.worlds[Spec.worldIndex], *WorldData, &LayoutError))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: world %s has no layout:\n%s"), *FT::ToFString(Spec.world), *FT::ToFString(LayoutError));
			WorldData.reset();
		}
	}
	std::string FitError;
	std::vector<std::string> FitWarnings;
	if (!ftcore::fitArenaZone(Spec, WorldData.get(), Def, Fit, &FitError, &FitWarnings))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s\nArena: using the plain zone instead."), *FT::ToFString(FitError));
		WorldData.reset();
		Spec.world.clear();
		Spec.district = -1;
		Spec.offset = ftcore::Vec2{};
		Spec.yaw = 0.f;
		Spec.kind = ftcore::ZoneKind::Circle;
		Spec.ring = MapRingRadius;
		Spec.spawn = 7.f;
		FitWarnings.clear();
		ftcore::fitArenaZone(Spec, nullptr, Def, Fit, nullptr, nullptr);
	}
	for (const std::string& W : FitWarnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s"), *FT::ToFString(W));
	}
	Fit.apply(Def);
}

void AFTArenaGameMode::BuildPlainGround()
{
	UWorld* World = GetWorld();
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// The shared look: the sun, sky, air and the camera finish.
	FTScene::SpawnLook(World, Spawn, ftcore::SceneLook{}, FRotator(-38, 35, 0));
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Cube || !World)
	{
		return;
	}
	// A slab 200 m across, its top at z = 0.
	AStaticMeshActor* Ground = World->SpawnActor<AStaticMeshActor>(FVector(0.0, 0.0, -50.0), FRotator::ZeroRotator, Spawn);
	if (!Ground)
	{
		return;
	}
	UStaticMeshComponent* Mesh = Ground->GetStaticMeshComponent();
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetStaticMesh(Cube);
	if (Base)
	{
		UMaterialInstanceDynamic* Paint = UMaterialInstanceDynamic::Create(Base, World);
		Paint->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.34f, 0.32f, 0.29f));
		Mesh->SetMaterial(0, Paint);
	}
	Mesh->SetCastShadow(false);
	Ground->SetActorScale3D(FVector(200.0, 200.0, 1.0));
}

void AFTArenaGameMode::BuildZoneDressing()
{
	UWorld* World = GetWorld();
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Rendering/M_WorldSurface.M_WorldSurface"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Base)
	{
		Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}
	if (!World || !Cube || !Cylinder || !Base)
	{
		return;
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Water and the stuff built for the zone, in the world's tints: a wet dark sheet, dark concrete.
	static const FLinearColor WaterTint[4] = {FLinearColor(0.06f, 0.08f, 0.1f), FLinearColor(0.06f, 0.1f, 0.15f), FLinearColor(0.05f, 0.22f, 0.4f), FLinearColor(0.02f, 0.03f, 0.04f)};
	static const FLinearColor StuffTint[4] = {FLinearColor(0.2f, 0.19f, 0.17f), FLinearColor(0.07f, 0.07f, 0.09f), FLinearColor(0.4f, 0.5f, 0.65f), FLinearColor(0.14f, 0.14f, 0.15f)};
	const int32 Style = FMath::Clamp(ZoneStyle, 0, 3);
	const auto Paint = [&](const FLinearColor& Color, float Roughness, float Metallic)
	{
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Base, World);
		Instance->SetVectorParameterValue(TEXT("Color"), Color);
		Instance->SetScalarParameterValue(TEXT("Roughness"), Roughness);
		Instance->SetScalarParameterValue(TEXT("Metallic"), Metallic);
		return Instance;
	};
	UMaterialInstanceDynamic* Water = Paint(WaterTint[Style], 0.06f, 0.4f);
	UMaterialInstanceDynamic* Stuff = Paint(StuffTint[Style], 0.8f, 0.f);
	const auto Place = [&](UStaticMesh* Mesh, UMaterialInterface* Material, const FVector& At, const FRotator& Turn, const FVector& Size, bool bBlocks)
	{
		AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(At, Turn, Spawn);
		if (!Prop)
		{
			return;
		}
		UStaticMeshComponent* Component = Prop->GetStaticMeshComponent();
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Mesh);
		Component->SetMaterial(0, Material);
		if (!bBlocks)
		{
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCastShadow(false);
		}
		Component->SetCanEverAffectNavigation(false);
		Prop->SetActorScale3D(Size); // both shapes are 1 m across and 1 m tall
	};
	const auto Cm = [this](const ftcore::Vec2& P, double HeightM) { return FVector(P.x * 100.0, P.y * 100.0, ZoneZ + HeightM * 100.0); };

	// Shallow sheets: a plaza's paving stands 3 cm over its pad, so these lie 2 cm over that.
	for (int32 W = 0; W < static_cast<int32>(Fit.waterCount); ++W)
	{
		const ftcore::ZoneWater& Sheet = Fit.water[static_cast<size_t>(W)];
		Place(Cylinder, Water, Cm(Sheet.at, 0.04), FRotator::ZeroRotator, FVector(Sheet.radius * 2.0, Sheet.radius * 2.0, 0.02), false);
	}
	// The zone's own cover.
	for (int32 O = 0; O < static_cast<int32>(Fit.zone.obstacleCount); ++O)
	{
		const ftcore::ZoneObstacle& Piece = Fit.zone.obstacles[static_cast<size_t>(O)];
		if (!Piece.authored)
		{
			continue;
		}
		if (Piece.box)
		{
			Place(Cube, Stuff, Cm(Piece.at, 0.55), FRotator(0.0, FMath::RadiansToDegrees(Piece.yaw), 0.0), FVector(Piece.half.x * 2.0, Piece.half.y * 2.0, 1.1), true);
		}
		else
		{
			Place(Cylinder, Stuff, Cm(Piece.at, 0.5), FRotator::ZeroRotator, FVector(Piece.radius * 2.0, Piece.radius * 2.0, 1.0), true);
		}
	}
	// A barrier across each road the wall crosses, just outside it: crates in a row, the way the wall runs.
	int32 Plugs = 0;
	for (int32 M = 0; M < static_cast<int32>(Fit.mouthCount); ++M)
	{
		const ftcore::ZoneMouth& Mouth = Fit.mouths[static_cast<size_t>(M)];
		const ftcore::Vec2 Along{-Mouth.out.y, Mouth.out.x};
		const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Along.y, Along.x));
		const int32 Count = FMath::Max(2, FMath::CeilToInt((Mouth.width + 1.f) / 1.4f));
		for (int32 K = 0; K < Count; ++K)
		{
			const float Off = (static_cast<float>(K) - 0.5f * static_cast<float>(Count - 1)) * 1.4f;
			const double Tall = (K % 2 == 0) ? 1.1 : 1.6;
			const ftcore::Vec2 At{Mouth.at.x + Mouth.out.x * 0.9f + Along.x * Off, Mouth.at.y + Mouth.out.y * 0.9f + Along.y * Off};
			Place(Cube, Stuff, Cm(At, Tall * 0.5), FRotator(0.0, Yaw, 0.0), FVector(1.3, 0.7, Tall), true);
			++Plugs;
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: dressed the zone: %d water sheets, %d road barriers across %d mouths."), static_cast<int32>(Fit.waterCount), Plugs,
		static_cast<int32>(Fit.mouthCount));
}

void AFTArenaGameMode::Begin()
{
	if (Spec.world.empty())
	{
		BuildPlainGround();
		ZoneZ = 0.0;
	}
	else
	{
		ZoneZ = UFTArenaRunner::SampleGround(GetWorld(), Fit.zone.centre);
	}
	if (WorldData && Fit.district >= 0)
	{
		const ftcore::WorldDistrict& Home = WorldData->district(static_cast<size_t>(Fit.district));
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: zone anchored on district %d of %s: plaza centre (%.1f, %.1f) m, facing %.1f degrees, landmark at (%.1f, %.1f) m."),
			Fit.district, *FT::ToFString(Spec.world), Home.centre.x, Home.centre.y, FMath::RadiansToDegrees(FMath::Atan2(Home.forward.y, Home.forward.x)),
			Home.landmark.x, Home.landmark.y);
	}
	ZoneStyle = Spec.world == "solhaven" ? 1 : (Spec.world == "lattice" ? 2 : (Spec.world == "unwritten" ? 3 : 0));
	BuildZoneDressing();
	const ftcore::ArenaZone& Z = Fit.zone;
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena: zone %s%s%s: %s of %d vertices, %d obstacles, ring %.1f m (asked %.1f m, %.0f%%), area %.0f m2, ground at %.2f m; camera arm %.1f m."),
		*FT::ToFString(Fit.id), Spec.world.empty() ? TEXT("") : TEXT(" in "), *FT::ToFString(Spec.world),
		Z.shape == ftcore::ZoneShape::Circle ? TEXT("a circle") : TEXT("a polygon"), static_cast<int32>(Z.vertCount),
		static_cast<int32>(Z.obstacleCount), Fit.ring.radius, Fit.askedRing, Fit.scale * 100.f, Z.area(), ZoneZ / 100.0, Fit.camera.arm);

	FFTArenaRunSetup Setup;
	Setup.Table = &Table;
	Setup.Def = Def;
	Setup.Fit = Fit;
	Setup.ZoneId = Spec.id;
	Setup.ZoneZ = ZoneZ;
	Setup.ZoneYaw = Spec.yaw;
	Setup.ZoneStyle = ZoneStyle;
	Setup.KitId = KitId;
	Setup.Lead = FindLead();
	Setup.Seed = Seed;
	Setup.bAutoPlay = bAutoPlay;
	Setup.bIdle = bIdle;
	Setup.bHoldLoad = bHoldLoad;
	Setup.bBossSplitAtStart = bBossSplit;
	Setup.bQuitOnFinish = bQuitOnFinish;
	Setup.bShots = !ShotPrefix.IsEmpty();
	Runner->Begin(Setup);
}

FString AFTArenaGameMode::FindKitId() const
{
	FString Want;
	FParse::Value(FCommandLine::Get(), TEXT("ArenaLead="), Want, false);
	Want = Want.TrimStartAndEnd();
	if (Want.IsEmpty())
	{
		const TArray<FFTLeadLook>& Party = GetDefault<AFTExploreCharacter>()->PartyLooks;
		Want = Party.IsEmpty() ? FString(TEXT("default")) : Party[0].Name;
	}
	return Want.ToLower();
}

const FFTLeadLook* AFTArenaGameMode::FindLead() const
{
	const TArray<FFTLeadLook>& Party = GetDefault<AFTExploreCharacter>()->PartyLooks;
	if (Party.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: no PartyLooks in config; the lead is the test human."));
		return nullptr;
	}
	FString Want;
	FParse::Value(FCommandLine::Get(), TEXT("ArenaLead="), Want, false);
	Want = Want.TrimStartAndEnd();
	if (!Want.IsEmpty())
	{
		for (const FFTLeadLook& Member : Party)
		{
			if (Member.Name.Equals(Want, ESearchCase::IgnoreCase))
			{
				return &Member;
			}
		}
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaLead: no look named \"%s\" in PartyLooks; using %s."), *Want, *Party[0].Name);
	}
	return &Party[0];
}

void AFTArenaGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bTravelPending)
	{
		if (!bTravelStarted && GetWorld())
		{
			bTravelStarted = true;
			UGameplayStatics::OpenLevel(this, FName(*FT::ToFString(ftcore::mapPackage(Worlds.worlds[Spec.worldIndex].map))), true,
				TEXT("game=/Script/ForgottenThird.FTArenaGameMode"));
		}
		return;
	}
	if (!Runner->IsBegun())
	{
		Begin();
	}
	Runner->TickFight(DeltaSeconds, Shots(DeltaSeconds));
	if (bBossCam)
	{
		BossCam();
	}
}

void AFTArenaGameMode::BossCam()
{
	APlayerController* PC = GetPC();
	const AFTArenaFighter* Boss = Runner->GetEnemyFigure(0);
	if (!PC || !Boss)
	{
		return;
	}
	const float Yaw = static_cast<float>(Boss->GetActorRotation().Yaw);
	if (!BossCamera)
	{
		// Square to the boss's first facing, off its left side at half its height: the whole figure in frame, feet
		// included, and a turn reads as the body swinging toward or away from the lens.
		const FVector Target = Boss->GetActorLocation() + FVector(0.0, 0.0, Boss->GetHeight() * 0.5);
		const FVector From = Target - Boss->GetActorRightVector() * 900.0;
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACameraActor* Camera = GetWorld()->SpawnActor<ACameraActor>(From, (Target - From).Rotation(), Params);
		if (!Camera)
		{
			bBossCam = false;
			return;
		}
		Camera->GetCameraComponent()->SetFieldOfView(40.f);
		BossCamera = Camera;
		PC->SetViewTarget(Camera);
		BossCamYaw = Yaw;
		UE_LOG(LogForgottenThird, Display, TEXT("ArenaBossCam: fixed camera 9 m off the boss's left side; t %.2f s yaw %.1f"),
			Runner->GetFight().time(), Yaw);
		return;
	}
	if (FMath::Abs(FMath::FindDeltaAngleDegrees(BossCamYaw, Yaw)) >= 10.f)
	{
		BossCamYaw = Yaw;
		UE_LOG(LogForgottenThird, Display, TEXT("ArenaBossCam: t %.2f s yaw %.1f"), Runner->GetFight().time(), Yaw);
	}
}

APlayerController* AFTArenaGameMode::GetPC() const
{
	return GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
}

bool AFTArenaGameMode::Shots(float DeltaSeconds)
{
	if (ShotPrefix.IsEmpty())
	{
		return false;
	}
	ShotWall += DeltaSeconds;
	if (ShotWall < ShotHoldUntil)
	{
		return true;
	}
	if (bOverview && OverviewPhase < 3 && OverviewShot())
	{
		return true;
	}
	if (bOverview && EdgePhase < 3 && EdgeShot())
	{
		return true;
	}
	// -ArenaShotAt's views, on the fight's clock, once the ring view is in (uncounted: the six views and the quit are
	// unchanged).
	if (NextShotAt < ShotAt.Num() && ShotsTaken > 0 && static_cast<double>(Runner->GetFight().time()) >= ShotAt[NextShotAt])
	{
		TakeShot(*FString::Printf(TEXT("t_%05.2f"), ShotAt[NextShotAt]), false);
		++NextShotAt;
		return true;
	}
	switch (ShotsTaken)
	{
	case 0:
		// The ring before anyone moves.
		if (ShotWall < ShotDelay)
		{
			return true;
		}
		TakeShot(TEXT("1_ring"));
		return true;
	case 1:
		// An enemy winding up, its tell most of the way full, with the lock on it and its Read cards picked.
		for (int32 I = 0; I < static_cast<int32>(Runner->GetFight().enemyCount()); ++I)
		{
			const ftcore::ArenaEnemyOut& E = Runner->GetFight().output().enemies[static_cast<size_t>(I)];
			const ftcore::ArenaEnemyMove* Move = Runner->GetEnemyMove(I);
			if (Move && E.part == ftcore::ArenaPart::Startup && Runner->GetFight().lock() != ftcore::kArenaNobody
				&& E.stateTime >= Move->attack.startup * 0.65f && Runner->GetFight().readFoe() == static_cast<uint8>(I) && Runner->GetFight().readCards().picked())
			{
				TakeShot(TEXT("2_tell"));
				return true;
			}
		}
		break;
	case 2:
		// The moment a move lands: the flash at impact (or a parry).
		for (int32 I = 0; I < static_cast<int32>(Runner->GetFight().enemyCount()); ++I)
		{
			if (Runner->GetNow() - Runner->GetEnemyActiveSince(I) < 0.05 || Runner->GetNow() - Runner->GetParryTime() < 0.05)
			{
				TakeShot(TEXT("3_impact"));
				return true;
			}
		}
		break;
	case 3:
		// The Load gauge past the Release's threshold, a moment after the parry that filled it (its +LOAD and the
		// partner's COUNTER still up); skipped if the fight ends first.
		// Under -ArenaHoldLoad, the first frame overloaded (the autopilot releases at once when in reach).
		if (bHoldLoad ? Runner->GetFight().overloaded()
					  : (Runner->GetFight().load() > ftcore::kArenaReleaseOpenLoad + 10 && Runner->GetNow() - Runner->GetParryTime() >= 0.15 && Runner->GetNow() - Runner->GetParryTime() < 0.5))
		{
			TakeShot(TEXT("4_load"));
			return true;
		}
		if (Runner->IsFinished())
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: the fight ended before the load view; no load or release view."));
			ShotsTaken += 2;
		}
		break;
	case 4:
		// Just after a Release began: its banner and its pose (skipped if the fight ends first).
		if (Runner->GetNow() - Runner->GetReleaseTime() >= 0.12 && Runner->GetNow() - Runner->GetReleaseTime() < 0.4)
		{
			TakeShot(TEXT("5_release"));
			return true;
		}
		if (Runner->IsFinished())
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: no Release in this fight; no release view."));
			++ShotsTaken;
		}
		break;
	case 5:
		if (Runner->IsFinished() && Runner->GetResultAge() >= 0.8)
		{
			TakeShot(TEXT("6_result"));
			return true;
		}
		break;
	default:
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: views taken; quitting."));
		ShotPrefix.Reset();
		FPlatformMisc::RequestExitWithStatus(false, 0, TEXT("Arena"));
		return true;
	}
	if (ShotWall > ShotDelay + kArenaShotGiveUp)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: only %d of 6 views came; quitting."), ShotsTaken);
		ShotPrefix.Reset();
		FPlatformMisc::RequestExitWithStatus(false, 2, TEXT("Arena"));
		return true;
	}
	return false;
}

bool AFTArenaGameMode::OverviewShot()
{
	APlayerController* PC = GetPC();
	if (!PC || ShotWall < ShotDelay)
	{
		return ShotWall < ShotDelay;
	}
	if (OverviewPhase == 0)
	{
		// High over the south-west of the zone, looking at its middle; if the scene is in the way (a building), the
		// camera comes in along the line until it is clear.
		const float Distance = FMath::Max(Fit.ring.radius * 2.6f, 30.f);
		const FVector Target(Fit.zone.centre.x * 100.0, Fit.zone.centre.y * 100.0, ZoneZ + 200.0);
		FVector From = Target + FVector(-0.55 * Distance, -0.55 * Distance, 0.75 * Distance) * 100.0;
		FHitResult Block;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaOverview), false);
		if (GetWorld()->SweepSingleByChannel(Block, Target, From, FQuat::Identity, ECC_Camera, FCollisionShape::MakeSphere(80.f), Query))
		{
			From = Target + (From - Target) * FMath::Max(0.25f, Block.Time * 0.9f);
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACameraActor* Camera = GetWorld()->SpawnActor<ACameraActor>(From, (Target - From).Rotation(), Params);
		if (!Camera)
		{
			OverviewPhase = 2;
			return false;
		}
		Camera->GetCameraComponent()->SetFieldOfView(70.f);
		OverviewCamera = Camera;
		// The walls are invisible in play; for this picture they are drawn: green where the zone ends, red for an
		// obstacle, yellow across a road the wall meets.
		const float LineZ = static_cast<float>(ZoneZ) + 60.f;
		const auto Line = [&](const ftcore::Vec2& A, const ftcore::Vec2& B, const FColor& Colour, float Thick)
		{
			DrawDebugLine(GetWorld(), FVector(A.x * 100.0, A.y * 100.0, LineZ), FVector(B.x * 100.0, B.y * 100.0, LineZ), Colour, false, 6.f, 0, Thick);
		};
		const ftcore::ArenaZone& Z = Fit.zone;
		const int32 Sides = Z.shape == ftcore::ZoneShape::Circle ? 72 : static_cast<int32>(Z.vertCount);
		for (int32 I = 0; I < Sides; ++I)
		{
			ftcore::Vec2 A, B;
			if (Z.shape == ftcore::ZoneShape::Circle)
			{
				const float A0 = UE_TWO_PI * static_cast<float>(I) / 72.f, A1 = UE_TWO_PI * static_cast<float>(I + 1) / 72.f;
				A = ftcore::Vec2{Z.centre.x + Z.radius * FMath::Cos(A0), Z.centre.y + Z.radius * FMath::Sin(A0)};
				B = ftcore::Vec2{Z.centre.x + Z.radius * FMath::Cos(A1), Z.centre.y + Z.radius * FMath::Sin(A1)};
			}
			else
			{
				A = Z.verts[static_cast<size_t>(I)];
				B = Z.verts[static_cast<size_t>((I + 1) % Z.vertCount)];
			}
			Line(A, B, FColor(60, 255, 90), 25.f);
		}
		for (int32 O = 0; O < static_cast<int32>(Z.obstacleCount); ++O)
		{
			const ftcore::ZoneObstacle& Piece = Z.obstacles[static_cast<size_t>(O)];
			for (int32 K = 0; K < 8; ++K)
			{
				ftcore::Vec2 A, B;
				if (Piece.box)
				{
					const float Cx[4] = {-1.f, 1.f, 1.f, -1.f}, Cy[4] = {-1.f, -1.f, 1.f, 1.f};
					if (K >= 4)
					{
						break;
					}
					const auto Corner = [&](int32 N)
					{
						const float Lx = Cx[N] * Piece.half.x, Ly = Cy[N] * Piece.half.y;
						return ftcore::Vec2{Piece.at.x + Lx * FMath::Cos(Piece.yaw) - Ly * FMath::Sin(Piece.yaw), Piece.at.y + Lx * FMath::Sin(Piece.yaw) + Ly * FMath::Cos(Piece.yaw)};
					};
					A = Corner(K);
					B = Corner((K + 1) % 4);
				}
				else
				{
					const float A0 = UE_TWO_PI * static_cast<float>(K) / 8.f, A1 = UE_TWO_PI * static_cast<float>(K + 1) / 8.f;
					A = ftcore::Vec2{Piece.at.x + Piece.radius * FMath::Cos(A0), Piece.at.y + Piece.radius * FMath::Sin(A0)};
					B = ftcore::Vec2{Piece.at.x + Piece.radius * FMath::Cos(A1), Piece.at.y + Piece.radius * FMath::Sin(A1)};
				}
				Line(A, B, FColor(255, 70, 60), 20.f);
			}
		}
		for (int32 M = 0; M < static_cast<int32>(Fit.mouthCount); ++M)
		{
			const ftcore::ZoneMouth& Mouth = Fit.mouths[static_cast<size_t>(M)];
			const ftcore::Vec2 Along{-Mouth.out.y * Mouth.width * 0.5f, Mouth.out.x * Mouth.width * 0.5f};
			Line(ftcore::Vec2{Mouth.at.x - Along.x, Mouth.at.y - Along.y}, ftcore::Vec2{Mouth.at.x + Along.x, Mouth.at.y + Along.y}, FColor(255, 220, 40), 30.f);
		}
		PC->SetViewTarget(Camera);
		if (AHUD* Hud = PC->GetHUD())
		{
			Hud->bShowHUD = false;
		}
		OverviewPhase = 1;
		ShotHoldUntil = ShotWall + 1.5;
		return true;
	}
	if (OverviewPhase == 1)
	{
		// The screenshot is taken on the next frame, so the camera stays until the hold after it has passed.
		TakeShot(TEXT("0_overview"), false);
		OverviewPhase = 2;
		return true;
	}
	PC->SetViewTarget(Runner->GetPlayerFigure());
	if (AHUD* Hud = PC->GetHUD())
	{
		Hud->bShowHUD = true;
	}
	if (OverviewCamera)
	{
		OverviewCamera->Destroy();
	}
	OverviewPhase = 3;
	ShotHoldUntil = ShotWall + 0.7;
	return true;
}

bool AFTArenaGameMode::EdgeShot()
{
	if (OverviewPhase < 3 || ShotWall < ShotHoldUntil)
	{
		return OverviewPhase < 3;
	}
	if (EdgePhase == 0)
	{
		// The wall point on the player's left (the lane's normal), the player 1.4 m inside it.
		const ftcore::Vec2 Face = Fit.playerFacing;
		const ftcore::Vec2 Left{-Face.y, Face.x};
		float Lo = 0.f, Hi = Fit.zone.radius * 1.05f;
		for (int32 It = 0; It < 24; ++It)
		{
			const float Mid = 0.5f * (Lo + Hi);
			(Fit.zone.contains(ftcore::Vec2{Fit.zone.centre.x + Left.x * Mid, Fit.zone.centre.y + Left.y * Mid}, 0.f) ? Lo : Hi) = Mid;
		}
		const float Inside = FMath::Max(0.f, Lo - 1.4f);
		const ftcore::Vec2 At{Fit.zone.centre.x + Left.x * Inside, Fit.zone.centre.y + Left.y * Inside};
		Runner->PutPlayerAt(At);
		EdgePhase = 1;
		ShotHoldUntil = ShotWall + 2.0; // the camera's lag settles
		return true;
	}
	if (EdgePhase == 1)
	{
		TakeShot(TEXT("0b_edge"), false);
		EdgePhase = 2;
		return true;
	}
	Runner->Restart(); // the fight as it began
	EdgePhase = 3;
	ShotHoldUntil = ShotWall + 1.0;
	return true;
}

void AFTArenaGameMode::TakeShot(const TCHAR* Name, bool bCounted)
{
	const FString File = FString::Printf(TEXT("%s_%s.png"), *ShotPrefix, Name);
	// The game viewport handles HighResShot; the engine's own Exec doesn't.
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *File), *GLog);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: view %s."), *File);
	if (bCounted)
	{
		++ShotsTaken;
	}
	ShotHoldUntil = ShotWall + kArenaShotHold;
}
