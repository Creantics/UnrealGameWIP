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
#include "FTExploreDirector.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "FTActionButtons.h"
#include "FTArenaFighter.h"
#include "FTArenaHUD.h"
#include "FTPauseSubsystem.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include <cmath>
#include <string>
#include <vector>

static_assert(AFTArenaGameMode::MaxEnemies == 6, "EnemyFigures[6] in FTArenaGameMode.h must follow ftcore::kMaxArenaEnemies");
static_assert(AFTArenaFighter::TellSegments == 14, "Tell[14] in FTArenaFighter.h must follow TellSegments");

namespace
{
// The shortest tell the arena allows (s): an enemy's startup is its telegraph.
constexpr float kArenaMinTell = 0.4f;
// After the outcome, the autopilot (without -ArenaQuitOnFinish) fights again this much later (s).
constexpr double kArenaAutoAgain = 4.0;
// A press to fight again counts only once the result has been up this long (s), so a held attack doesn't skip it.
constexpr double kArenaAgainDelay = 0.6;
// How long a hit tints a body (s).
constexpr double kArenaHitTint = 0.16;
// The floor tell's flash after a move goes active (s).
constexpr double kArenaTellFlash = 0.3;
// Camera: pitch limits, the pitch while locked, how fast it turns to a lock, stick and mouse rates.
constexpr float kArenaPitchMin = -50.f;
constexpr float kArenaPitchMax = 10.f;
constexpr float kArenaLockTurn = 7.f;
constexpr float kArenaStickTurn = 140.f; // degrees a second at full stick
constexpr float kArenaMouseTurn = 2.5f; // degrees per unit of mouse axis (as the engine's yaw scale)
// A right-stick flick switches the lock: past this, after returning under the rest.
constexpr float kArenaFlickOut = 0.7f;
constexpr float kArenaFlickRest = 0.3f;
// After a view is taken, the fight holds this long (wall seconds) so the frame it renders is that moment.
constexpr double kArenaShotHold = 1.0;
// A -ArenaShot run gives up this long after the first view (wall seconds).
constexpr double kArenaShotGiveUp = 200.0;

// Body colours: the player's, and an enemy's by its kind's index in the table.
const FLinearColor kArenaPlayerColor(0.18f, 0.42f, 0.85f);
const FLinearColor kArenaKindColors[6] = {
	FLinearColor(0.55f, 0.12f, 0.1f),
	FLinearColor(0.45f, 0.2f, 0.6f),
	FLinearColor(0.25f, 0.38f, 0.3f),
	FLinearColor(0.6f, 0.42f, 0.12f),
	FLinearColor(0.2f, 0.45f, 0.55f),
	FLinearColor(0.4f, 0.4f, 0.42f),
};
const FLinearColor kArenaDeadColor(0.12f, 0.12f, 0.13f);
const FLinearColor kArenaOpenColor(1.f, 0.85f, 0.35f);
const FLinearColor kArenaHitColor(1.f, 0.95f, 0.9f);

float ArenaYaw(const ftcore::Vec2& Facing)
{
	return FMath::RadiansToDegrees(FMath::Atan2(Facing.y, Facing.x));
}

float ArenaLength(const ftcore::Vec2& V)
{
	return FMath::Sqrt(V.x * V.x + V.y * V.y);
}

ftcore::Vec2 ArenaLerp(const ftcore::Vec2& A, const ftcore::Vec2& B, float T)
{
	return ftcore::Vec2{A.x + (B.x - A.x) * T, A.y + (B.y - A.y) * T};
}

// How far through its part an attack is (0..1), from the time since it started.
float ArenaPartFraction(const ftcore::ArenaAttack& Attack, ftcore::ArenaPart Part, float Time)
{
	switch (Part)
	{
	case ftcore::ArenaPart::Startup:
		return Attack.startup > 0.f ? FMath::Clamp(Time / Attack.startup, 0.f, 1.f) : 1.f;
	case ftcore::ArenaPart::Active:
		return Attack.active > 0.f ? FMath::Clamp((Time - Attack.startup) / Attack.active, 0.f, 1.f) : 1.f;
	case ftcore::ArenaPart::Recovery:
		return Attack.recovery > 0.f ? FMath::Clamp((Time - Attack.startup - Attack.active) / Attack.recovery, 0.f, 1.f) : 1.f;
	default:
		return 0.f;
	}
}

// 1 at Time, falling to 0 over Life seconds.
float ArenaModePulse(double Now, double Time, double Life)
{
	const double Age = Now - Time;
	return (Age >= 0.0 && Age < Life) ? static_cast<float>(1.0 - Age / Life) : 0.f;
}

bool ArenaAnyJustPressed(const APlayerController& PC, const FKey& A, const FKey& B, const FKey& C = EKeys::Invalid,
	const FKey& D = EKeys::Invalid)
{
	return PC.WasInputKeyJustPressed(A) || PC.WasInputKeyJustPressed(B) || (C.IsValid() && PC.WasInputKeyJustPressed(C))
		|| (D.IsValid() && PC.WasInputKeyJustPressed(D));
}

// The demo's lineup without -ArenaLineup: arena.json's enemy kind ids.
const TCHAR* const kArenaDefaultWave = TEXT("brute,skirmisher,breaker");

const TCHAR* ArenaPilotName(bool bAutoPlay, bool bIdle)
{
	return bIdle ? TEXT("idle") : (bAutoPlay ? TEXT("autoplay") : TEXT("player"));
}
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
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		ActiveSince[I] = -1e9;
		EnemyHitTime[I] = -1e9;
	}
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
	}
	bIdle = FParse::Param(Cmd, TEXT("ArenaIdle"));
	bAutoPlay = !bIdle && (FParse::Param(Cmd, TEXT("ArenaAutoPlay")) || !ShotPrefix.IsEmpty());
	bQuitOnFinish = FParse::Param(Cmd, TEXT("ArenaQuitOnFinish"));
	bOverview = FParse::Param(Cmd, TEXT("ArenaOverview"));
	bHoldLoad = FParse::Param(Cmd, TEXT("ArenaHoldLoad"));
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
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: zone %s, %d enemies (%s), ring %.1f m, spawn %.1f m, %d attacking at once, seed %u, %s; shortest tell %.2f s."),
		*FT::ToFString(Spec.id), static_cast<int32>(Def.enemyCount), *Kinds, Def.ringRadius, Def.spawnDistance,
		static_cast<int32>(Def.attackers), Seed, ArenaPilotName(bAutoPlay, bIdle), Shortest);
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

double AFTArenaGameMode::SampleGround(const ftcore::Vec2& P) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0;
	}
	// From far above, down: what stops the line first is a roof, a crown or the plaza's paving, not the ground. Take the
	// paving's top (the figures stand on it) and for anything else skip the actor and look again, until the Landscape.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ArenaGround), false);
	const FVector Top(P.x * 100.0, P.y * 100.0, 60000.0);
	for (int32 Try = 0; Try < 8; ++Try)
	{
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, Top, Top - FVector(0.0, 0.0, 120000.0), ECC_Visibility, Params))
		{
			break;
		}
		const UPrimitiveComponent* Component = Hit.GetComponent();
		if (Component && Component->GetClass()->GetName().Contains(TEXT("Landscape")))
		{
			return Hit.ImpactPoint.Z;
		}
		if (Component && Component->GetName().Contains(TEXT("PlazaDiscs")))
		{
			return Hit.ImpactPoint.Z;
		}
		if (!Hit.GetActor())
		{
			break;
		}
		Params.AddIgnoredActor(Hit.GetActor());
	}
	UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no ground under the zone; the figures stand at 0."));
	return 0.0;
}

APlayerController* AFTArenaGameMode::GetPC() const
{
	return GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
}

void AFTArenaGameMode::Begin()
{
	bBegun = true;
	BeginNow = Now;

	// The fitted zone's ring, and where the player starts in it.
	Fight.begin(Def, Fit.ring, Fit.playerStart, Fit.playerFacing, Seed);

	if (Spec.world.empty())
	{
		BuildPlainGround();
		ZoneZ = 0.0;
	}
	else
	{
		ZoneZ = SampleGround(Fit.zone.centre);
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

	SpawnFigures();
	PutBodiesAtStart();
	PlaceFigures(1.f);
}

void AFTArenaGameMode::SpawnFigures()
{
	if (bFiguresSpawned)
	{
		return;
	}
	bFiguresSpawned = true;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// The player's figure is the pawn the controller has (DefaultPawnClass), else one made and possessed here.
	APlayerController* PC = GetPC();
	PlayerFigure = PC ? Cast<AFTArenaFighter>(PC->GetPawn()) : nullptr;
	if (!PlayerFigure)
	{
		PlayerFigure = World->SpawnActor<AFTArenaFighter>(AFTArenaFighter::StaticClass(), FTransform::Identity, Params);
		if (PC && PlayerFigure)
		{
			if (APawn* Old = PC->GetPawn())
			{
				PC->UnPossess();
				Old->Destroy();
			}
			PC->Possess(PlayerFigure);
		}
	}
	if (PlayerFigure)
	{
		PlayerFigure->Setup(true, Def.fighter.bodyRadius, kArenaPlayerColor);
		const ftcore::ZoneCamera& Rig = Fit.camera;
		PlayerFigure->SetCameraRig(Rig.arm, Rig.pivot, Rig.side, Rig.up, Rig.fov);
	}
	if (PC)
	{
		PC->SetViewTarget(PlayerFigure);
	}

	for (int32 I = 0; I < static_cast<int32>(Def.enemyCount) && I < MaxEnemies; ++I)
	{
		AFTArenaFighter* Enemy = World->SpawnActor<AFTArenaFighter>(AFTArenaFighter::StaticClass(), FTransform::Identity, Params);
		if (!Enemy)
		{
			continue;
		}
		const uint8 Kind = Def.enemyKind[static_cast<size_t>(I)];
		const FLinearColor& Color = kArenaKindColors[(Kind == ftcore::kArenaNoKind ? 0 : Kind) % 6];
		const ftcore::ArenaEnemyKind& EnemyKind = Def.enemies[static_cast<size_t>(I)];
		Enemy->Setup(false, EnemyKind.bodyRadius, Color, ftcore::arenaEnemyHeight(EnemyKind));
		EnemyFigures[I] = Enemy;
	}
}

void AFTArenaGameMode::PutBodiesAtStart()
{
	Bodies = ftcore::ArenaBodies();
	Bodies.player = Fight.playerStart();
	for (size_t I = 0; I < Fight.enemyCount(); ++I)
	{
		Bodies.enemies[I] = Fight.enemyStart(I);
	}
	PrevBodies = Bodies;
	Accumulator = 0.0;
	Fight.tick(0.f, ftcore::ArenaInput(), Bodies);
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		ActiveSince[I] = -1e9;
		EnemyHitTime[I] = -1e9;
		LastPart[I] = 0;
	}
	PlayerHitTime = -1e9;
	ParryTime = -1e9;
	ReleaseTime = -1e9;
	ReleaseSpent = 0;
	OverloadTime = -1e9;
	bPressLight = bPressHeavy = bPressDodge = bPressHeal = bPressLock = bPressRelease = bPressPair = bPartnerDue = false;
	PressSwitch = 0;
	PressRead = -1;
	CameraYaw = ArenaYaw(Fight.playerStartFacing());
	CameraPitch = -16.f;
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
	if (!bBegun)
	{
		Begin();
	}

	const bool bHold = Shots(DeltaSeconds);
	if (!bHold)
	{
		Now += DeltaSeconds;
	}
	PollInput(DeltaSeconds);

	if (!Fight.over() && !bHold)
	{
		Accumulator = FMath::Min(Accumulator + static_cast<double>(DeltaSeconds), static_cast<double>(StepSeconds * MaxStepsPerFrame));
		while (Accumulator >= StepSeconds && !Fight.over())
		{
			StepFight();
			Accumulator -= StepSeconds;
		}
	}
	PlaceFigures(Fight.over() ? 1.f : static_cast<float>(Accumulator / StepSeconds));
	SteerCamera(DeltaSeconds);

	if (Fight.over())
	{
		if (!bResultLogged)
		{
			Finish();
		}
		else if (ShotPrefix.IsEmpty() && !bQuitOnFinish)
		{
			bool bAgain = bAutoPlay && GetResultAge() >= kArenaAutoAgain;
			const APlayerController* PC = GetPC();
			if (!bAgain && PC && !bAutoPlay && GetResultAge() >= kArenaAgainDelay && !UFTPauseSubsystem::IsBlocking(this))
			{
				bAgain = ArenaAnyJustPressed(*PC, EKeys::Enter, EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom);
			}
			if (bAgain)
			{
				Restart();
			}
		}
		return;
	}

	// A hang guard for unattended runs: a fight that outlasts the watchdog ends the run.
	if (bQuitOnFinish && (Fight.time() > WatchdogSeconds || Now - BeginNow > WatchdogSeconds * 2.0))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("ARENA RESULT: Timeout after %.1f s (%s, seed %u): the fight never ended."),
			Fight.time(), ArenaPilotName(bAutoPlay, bIdle), Seed);
		bQuitOnFinish = false;
		FPlatformMisc::RequestExitWithStatus(false, 2, TEXT("Arena"));
	}
}

void AFTArenaGameMode::PollInput(float DeltaSeconds)
{
	APlayerController* PC = GetPC();
	if (!PC || bAutoPlay || bIdle || UFTPauseSubsystem::IsBlocking(this) || UFTPauseSubsystem::IsJustResumed(this))
	{
		bGuardHeld = false;
		Stick = ftcore::Vec2();
		return;
	}
	using namespace FTActionButtons;
	const int32 AttackB = static_cast<int32>(ftcore::ActionButton::Attack);
	const int32 GuardB = static_cast<int32>(ftcore::ActionButton::Guard);
	const int32 DodgeB = static_cast<int32>(ftcore::ActionButton::Dodge);
	const int32 HeavyB = static_cast<int32>(ftcore::ActionButton::Jump);

	bPressLight |= ArenaAnyJustPressed(*PC, Key(AttackB, 0), Key(AttackB, 1), EKeys::LeftMouseButton, EKeys::Gamepad_RightShoulder);
	bPressHeavy |= ArenaAnyJustPressed(*PC, Key(HeavyB, 0), Key(HeavyB, 1), EKeys::Gamepad_RightTrigger);
	bPressDodge |= ArenaAnyJustPressed(*PC, Key(DodgeB, 0), Key(DodgeB, 1), EKeys::SpaceBar);
	// The partner's Read: 1 / 2 / 3, or on a pad the d-pad's left, up and right while the cards are up, picked or
	// not, until the move goes active (they switch the target and heal at any other time; the d-pad's down stays
	// the partner's pair). Core refuses a second pick, so after one those three do nothing until the cards go.
	const bool bCardsOpen = Fight.readCards().dealt();
	if (PC->WasInputKeyJustPressed(EKeys::One) || (bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Left)))
	{
		PressRead = 0;
	}
	else if (PC->WasInputKeyJustPressed(EKeys::Two) || (bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Up)))
	{
		PressRead = 1;
	}
	else if (PC->WasInputKeyJustPressed(EKeys::Three) || (bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Right)))
	{
		PressRead = 2;
	}
	bPressHeal |= PC->WasInputKeyJustPressed(EKeys::H) || (!bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Up));
	bPressLock |= ArenaAnyJustPressed(*PC, EKeys::Tab, EKeys::MiddleMouseButton, EKeys::Gamepad_RightThumbstick);
	bPressRelease |= ArenaAnyJustPressed(*PC, EKeys::R, EKeys::Gamepad_LeftTrigger);
	bPressPair |= ArenaAnyJustPressed(*PC, EKeys::F, EKeys::Gamepad_DPad_Down);
	bGuardHeld = PC->IsInputKeyDown(Key(GuardB, 0)) || PC->IsInputKeyDown(Key(GuardB, 1)) || PC->IsInputKeyDown(EKeys::RightMouseButton)
		|| PC->IsInputKeyDown(EKeys::Gamepad_LeftShoulder);
	if (PC->WasInputKeyJustPressed(EKeys::Q) || (!bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Left)))
	{
		PressSwitch = -1;
	}
	if (PC->WasInputKeyJustPressed(EKeys::E) || (!bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Right)))
	{
		PressSwitch = 1;
	}

	// The left stick or WASD: forward and right, at most 1 long.
	float Forward = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY);
	float Right = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX);
	Forward += (PC->IsInputKeyDown(EKeys::W) ? 1.f : 0.f) - (PC->IsInputKeyDown(EKeys::S) ? 1.f : 0.f);
	Right += (PC->IsInputKeyDown(EKeys::D) ? 1.f : 0.f) - (PC->IsInputKeyDown(EKeys::A) ? 1.f : 0.f);
	const float Length = FMath::Sqrt(Forward * Forward + Right * Right);
	if (Length > 1.f)
	{
		Forward /= Length;
		Right /= Length;
	}
	else if (Length < 0.15f)
	{
		Forward = Right = 0.f;
	}
	const float Yaw = FMath::DegreesToRadians(CameraYaw);
	const float C = FMath::Cos(Yaw);
	const float S = FMath::Sin(Yaw);
	Stick = ftcore::Vec2{C * Forward - S * Right, S * Forward + C * Right};

	// The view: the mouse and the right stick turn the camera unless something is locked; locked, a flick of
	// the right stick switches the target.
	const float LookX = PC->GetInputAnalogKeyState(EKeys::Gamepad_RightX);
	const float LookY = PC->GetInputAnalogKeyState(EKeys::Gamepad_RightY);
	if (Fight.lock() != ftcore::kArenaNobody)
	{
		if (!bStickFlicked && FMath::Abs(LookX) > kArenaFlickOut)
		{
			bStickFlicked = true;
			PressSwitch = LookX > 0.f ? 1 : -1;
		}
		else if (bStickFlicked && FMath::Abs(LookX) < kArenaFlickRest)
		{
			bStickFlicked = false;
		}
	}
	else
	{
		float MouseX = 0.f;
		float MouseY = 0.f;
		PC->GetInputMouseDelta(MouseX, MouseY);
		CameraYaw += MouseX * kArenaMouseTurn + LookX * kArenaStickTurn * DeltaSeconds;
		CameraPitch = FMath::Clamp(CameraPitch + MouseY * kArenaMouseTurn + LookY * kArenaStickTurn * 0.6f * DeltaSeconds,
			kArenaPitchMin, kArenaPitchMax);
		bStickFlicked = false;
	}
}

ftcore::ArenaInput AFTArenaGameMode::TakeInput()
{
	ftcore::ArenaInput In;
	In.move = Stick;
	const float Yaw = FMath::DegreesToRadians(CameraYaw);
	In.view = ftcore::Vec2{FMath::Cos(Yaw), FMath::Sin(Yaw)};
	In.light = bPressLight;
	In.heavy = bPressHeavy;
	In.dodge = bPressDodge;
	In.heal = bPressHeal;
	In.guard = bGuardHeld;
	In.lock = bPressLock;
	In.lockSwitch = PressSwitch;
	In.release = bPressRelease;
	In.pair = bPressPair;
	In.read = PressRead;
	// Each press goes to one step.
	bPressLight = bPressHeavy = bPressDodge = bPressHeal = bPressLock = bPressRelease = bPressPair = false;
	PressSwitch = 0;
	PressRead = -1;
	return In;
}

void AFTArenaGameMode::StepFight()
{
	ftcore::ArenaInput In;
	if (bAutoPlay)
	{
		In = Fight.autopilot();
		// The partner's press, one step after the parry (well inside the pair window).
		In.pair = bPartnerDue;
		bPartnerDue = false;
		if (bHoldLoad && !Fight.overloaded())
		{
			In.release = false;
		}
	}
	else if (!bIdle)
	{
		In = TakeInput();
	}
	PrevBodies = Bodies;
	const ftcore::ArenaOutput& Out = Fight.tick(StepSeconds, In, Bodies);
	ftcore::arenaStep(Fight, Bodies, StepSeconds, Fit.zone);
	HandleEvents(Out.events);
	TrackParts(Out);
}

void AFTArenaGameMode::HandleEvents(const ftcore::ArenaEvents& Events)
{
	for (const ftcore::ArenaEvent& E : Events)
	{
		switch (E.type)
		{
		case ftcore::ArenaEventType::Hit:
			if (E.target == ftcore::kArenaPlayer)
			{
				PlayerHitTime = Now;
			}
			else if (E.target < MaxEnemies)
			{
				EnemyHitTime[E.target] = Now;
			}
			break;
		case ftcore::ArenaEventType::Parried:
			ParryTime = Now;
			bPartnerDue = bAutoPlay;
			break;
		case ftcore::ArenaEventType::CounterStagger:
			if (E.target < MaxEnemies)
			{
				EnemyHitTime[E.target] = Now;
			}
			break;
		case ftcore::ArenaEventType::Released:
			ReleaseTime = Now;
			ReleaseSpent = E.amount;
			continue; // a banner, not a popup
		case ftcore::ArenaEventType::Overloaded:
			OverloadTime = Now;
			continue; // a banner, not a popup
		case ftcore::ArenaEventType::ReadCounter:
			if (E.target < MaxEnemies)
			{
				EnemyHitTime[E.target] = Now;
			}
			break;
		case ftcore::ArenaEventType::AttackStarted:
		case ftcore::ArenaEventType::CardsDealt: // the cards themselves are the display
		case ftcore::ArenaEventType::LockChanged:
		case ftcore::ArenaEventType::Victory:
		case ftcore::ArenaEventType::Defeat:
			continue; // no popup
		default:
			break;
		}
		FPopup& P = Popups[PopupTotal % MaxPopups];
		P.Type = E.type;
		P.Source = E.source;
		P.Target = E.target;
		P.Move = E.move;
		P.Amount = E.amount;
		P.At = ToWorld(E.at);
		P.Time = Now;
		++PopupTotal;
	}
}

void AFTArenaGameMode::TrackParts(const ftcore::ArenaOutput& Out)
{
	for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()) && I < MaxEnemies; ++I)
	{
		const ftcore::ArenaEnemyOut& E = Out.enemies[static_cast<size_t>(I)];
		const uint8 Part = E.brain == ftcore::ArenaBrain::Attack ? static_cast<uint8>(E.part) : 0;
		if (Part != LastPart[I])
		{
			if (Part == static_cast<uint8>(ftcore::ArenaPart::Active))
			{
				ActiveSince[I] = Now;
			}
			else if (Part == static_cast<uint8>(ftcore::ArenaPart::Startup))
			{
				ActiveSince[I] = -1e9;
			}
			LastPart[I] = Part;
		}
	}
}

const ftcore::ArenaEnemyMove* AFTArenaGameMode::GetEnemyMove(int32 Index) const
{
	if (!bBegun || Index < 0 || Index >= static_cast<int32>(Fight.enemyCount()))
	{
		return nullptr;
	}
	const ftcore::ArenaEnemyOut& E = Fight.output().enemies[static_cast<size_t>(Index)];
	if (E.brain != ftcore::ArenaBrain::Attack)
	{
		return nullptr;
	}
	const ftcore::ArenaEnemyKind& Kind = Fight.def().enemies[static_cast<size_t>(Index)];
	return E.move < Kind.moveCount ? &Kind.moves[E.move] : nullptr;
}

void AFTArenaGameMode::PlaceFigures(float Alpha)
{
	if (!bBegun)
	{
		return;
	}
	const ftcore::ArenaOutput& Out = Fight.output();
	const ftcore::ArenaDef& D = Fight.def();
	Alpha = FMath::Clamp(Alpha, 0.f, 1.f);

	if (PlayerFigure)
	{
		const ftcore::ArenaPlayerOut& P = Out.player;
		FFTArenaPose Pose;
		Pose.Feet = ToWorld(ArenaLerp(PrevBodies.player, Bodies.player, Alpha));
		Pose.Yaw = ArenaYaw(P.facing);
		Pose.Speed = ArenaLength(P.velocity);
		Pose.Tint = PlayerFigure->GetBaseColor();
		const ftcore::ArenaAttack* Attack = nullptr;
		switch (P.action)
		{
		case ftcore::ArenaAction::Light:
			Attack = &D.fighter.light[FMath::Min<size_t>(P.move, ftcore::kMaxLightChain - 1)];
			Pose.Swing = (P.move + 1 >= D.fighter.lightCount) ? EFTArenaSwing::Overhead : ((P.move % 2) == 0 ? EFTArenaSwing::Across : EFTArenaSwing::Backhand);
			break;
		case ftcore::ArenaAction::Heavy:
			Attack = &D.fighter.heavy;
			Pose.Swing = EFTArenaSwing::Overhead;
			break;
		case ftcore::ArenaAction::Riposte:
			Attack = &D.fighter.riposte;
			Pose.Swing = EFTArenaSwing::Thrust;
			break;
		case ftcore::ArenaAction::Release:
			Attack = &D.fighter.release;
			Pose.Swing = EFTArenaSwing::Overhead;
			Pose.Tint = FMath::Lerp(Pose.Tint, FLinearColor(1.f, 0.5f, 0.12f), 0.6f);
			break;
		case ftcore::ArenaAction::Dodge:
		{
			Pose.bDodging = true;
			const ftcore::Vec2& F = P.facing;
			const ftcore::Vec2& Dir = P.dodgeDir;
			Pose.DodgeLocal = FVector2D(Dir.x * F.x + Dir.y * F.y, Dir.y * F.x - Dir.x * F.y);
			Pose.DodgeFraction = D.fighter.dodgeTime > 0.f ? P.actionTime / D.fighter.dodgeTime : 1.f;
			break;
		}
		case ftcore::ArenaAction::Guard:
			Pose.bGuard = true;
			Pose.bParrying = P.parrying;
			break;
		case ftcore::ArenaAction::Heal:
			Pose.bHealing = true;
			Pose.Tint = FMath::Lerp(Pose.Tint, FLinearColor(0.3f, 0.95f, 0.45f), 0.5f);
			break;
		case ftcore::ArenaAction::Hitstun:
			Pose.LeanBack = 14.f;
			break;
		case ftcore::ArenaAction::GuardBreak:
			Pose.LeanBack = 22.f;
			Pose.Wobble = 6.f * FMath::Sin(static_cast<float>(Now) * 16.f);
			break;
		case ftcore::ArenaAction::Downed:
			Pose.bLying = true;
			break;
		default:
			break;
		}
		if (Attack)
		{
			Pose.bAttacking = true;
			Pose.Part = static_cast<uint8>(P.part);
			Pose.PartFraction = ArenaPartFraction(*Attack, P.part, P.actionTime);
		}
		if (P.invulnerable)
		{
			Pose.Tint = FMath::Lerp(Pose.Tint, FLinearColor(0.85f, 0.95f, 1.f), 0.45f);
		}
		Pose.Tint = FMath::Lerp(Pose.Tint, FLinearColor(1.f, 0.2f, 0.15f), ArenaModePulse(Now, PlayerHitTime, kArenaHitTint));
		PlayerFigure->ApplyPose(Pose);
	}

	for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()) && I < MaxEnemies; ++I)
	{
		AFTArenaFighter* Figure = EnemyFigures[I].Get();
		if (!Figure)
		{
			continue;
		}
		const ftcore::ArenaEnemyOut& E = Out.enemies[static_cast<size_t>(I)];
		const ftcore::ArenaEnemyKind& Kind = D.enemies[static_cast<size_t>(I)];
		FFTArenaPose Pose;
		Pose.Feet = ToWorld(ArenaLerp(PrevBodies.enemies[static_cast<size_t>(I)], Bodies.enemies[static_cast<size_t>(I)], Alpha));
		Pose.Yaw = ArenaYaw(E.facing);
		Pose.Speed = ArenaLength(E.velocity);
		Pose.Tint = Figure->GetBaseColor();
		switch (E.brain)
		{
		case ftcore::ArenaBrain::Attack:
			if (E.move < Kind.moveCount)
			{
				const ftcore::ArenaEnemyMove& Move = Kind.moves[E.move];
				Pose.bAttacking = true;
				Pose.Part = static_cast<uint8>(E.part);
				Pose.PartFraction = ArenaPartFraction(Move.attack, E.part, E.stateTime);
				Pose.Swing = (!Move.parryable || Move.unblockable) ? EFTArenaSwing::Overhead
					: ((E.move % 2) == 0 ? EFTArenaSwing::Across : EFTArenaSwing::Backhand);
				const float Flash = ArenaModePulse(Now, ActiveSince[I], kArenaTellFlash);
				if (E.part == ftcore::ArenaPart::Startup || Flash > 0.f)
				{
					Pose.TellReach = (Move.attack.range + D.fighter.bodyRadius) * 100.f;
					Pose.TellHalfAngle = FMath::Acos(FMath::Clamp(Move.attack.arcCos, -1.f, 1.f));
					Pose.TellFraction = E.part == ftcore::ArenaPart::Startup ? Pose.PartFraction : 1.f;
					Pose.TellFlash = E.part == ftcore::ArenaPart::Startup ? 0.f : Flash;
					Pose.bTellUnblockable = Move.unblockable;
				}
			}
			break;
		case ftcore::ArenaBrain::Staggered:
			Pose.LeanBack = 12.f;
			Pose.Wobble = 9.f * FMath::Sin(static_cast<float>(Now) * 18.f);
			break;
		case ftcore::ArenaBrain::Open:
			Pose.LeanBack = 20.f;
			// Open to a riposte: it glows.
			Pose.Tint = FMath::Lerp(Pose.Tint, kArenaOpenColor, 0.35f + 0.25f * FMath::Sin(static_cast<float>(Now) * 12.f));
			break;
		case ftcore::ArenaBrain::Down:
			Pose.bLying = true;
			break;
		case ftcore::ArenaBrain::Dead:
			Pose.bLying = true;
			Pose.Tint = kArenaDeadColor;
			break;
		default:
			break;
		}
		if (E.brain != ftcore::ArenaBrain::Dead)
		{
			Pose.Tint = FMath::Lerp(Pose.Tint, kArenaHitColor, ArenaModePulse(Now, EnemyHitTime[I], kArenaHitTint));
		}
		Figure->ApplyPose(Pose);
	}
}

void AFTArenaGameMode::SteerCamera(float DeltaSeconds)
{
	APlayerController* PC = GetPC();
	if (!PC || !bBegun)
	{
		return;
	}
	// Locked: turn to look from the player at the target. The autopilot and an idle run with nothing locked
	// look at the nearest living enemy, so the fight stays in frame.
	int32 Target = Fight.lock() != ftcore::kArenaNobody ? static_cast<int32>(Fight.lock()) : -1;
	if (Target < 0 && (bAutoPlay || bIdle))
	{
		float Best = 1e9f;
		for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()); ++I)
		{
			if (Fight.enemyAlive(static_cast<size_t>(I)))
			{
				const ftcore::Vec2& E = Bodies.enemies[static_cast<size_t>(I)];
				const float Dist = ArenaLength(ftcore::Vec2{E.x - Bodies.player.x, E.y - Bodies.player.y});
				if (Dist < Best)
				{
					Best = Dist;
					Target = I;
				}
			}
		}
	}
	if (Target >= 0 && Target < static_cast<int32>(Fight.enemyCount()))
	{
		const ftcore::Vec2& E = Bodies.enemies[static_cast<size_t>(Target)];
		const ftcore::Vec2 To{E.x - Bodies.player.x, E.y - Bodies.player.y};
		if (ArenaLength(To) > 0.3f)
		{
			const float Want = ArenaYaw(To);
			const float Delta = FMath::FindDeltaAngleDegrees(CameraYaw, Want);
			CameraYaw += Delta * FMath::Min(1.f, kArenaLockTurn * DeltaSeconds);
		}
		CameraPitch = FMath::FInterpTo(CameraPitch, Fit.camera.lockPitch, DeltaSeconds, 4.f);
	}
	CameraYaw = FMath::UnwindDegrees(CameraYaw);
	PC->SetControlRotation(FRotator(CameraPitch, CameraYaw, 0.0));
}

void AFTArenaGameMode::Finish()
{
	bResultLogged = true;
	ResultTime = Now;
	const bool bWon = Fight.phase() == ftcore::ArenaPhase::Victory;
	const ftcore::ArenaStats& S = Fight.stats();
	UE_LOG(LogForgottenThird, Display,
		TEXT("ARENA RESULT: %s in %.1f s (%s, seed %u, %d enemies, hp %d/%d, %d heals left; %d hits, %d taken, %d blocks, %d parries, %d ripostes, %d evades, %d staggers, %d guard breaks, %d heals; %d load stored, %d releases, %d overloads, %d counter staggers; %d reads dealt, %d picked, %d right, %d read counters for %d; dealt %d, took %d)."),
		bWon ? TEXT("Victory") : TEXT("Defeat"), Fight.time(), ArenaPilotName(bAutoPlay, bIdle), Seed,
		static_cast<int32>(Fight.enemyCount()), Fight.hp(), Fight.def().fighter.hp, static_cast<int32>(Fight.healCharges()),
		S.hits, S.hitsTaken, S.blocks, S.parries, S.ripostes, S.evades, S.staggers, S.guardBreaks, S.heals, S.loadStored, S.releases, S.overloads, S.counterStaggers,
		S.readsDealt, S.reads, S.correctReads, S.readCounters, S.readDamage, S.damageDealt, S.damageTaken);
	if (bQuitOnFinish && ShotPrefix.IsEmpty())
	{
		FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(bWon ? 0 : 1), TEXT("Arena"));
	}
}

void AFTArenaGameMode::Restart()
{
	Fight.restart();
	bResultLogged = false;
	PutBodiesAtStart();
	BeginNow = Now;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: fight again."));
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
		for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()); ++I)
		{
			const ftcore::ArenaEnemyOut& E = Fight.output().enemies[static_cast<size_t>(I)];
			const ftcore::ArenaEnemyMove* Move = GetEnemyMove(I);
			if (Move && E.part == ftcore::ArenaPart::Startup && Fight.lock() != ftcore::kArenaNobody
				&& E.stateTime >= Move->attack.startup * 0.65f && Fight.readFoe() == static_cast<uint8>(I) && Fight.readCards().picked())
			{
				TakeShot(TEXT("2_tell"));
				return true;
			}
		}
		break;
	case 2:
		// The moment a move lands: the flash at impact (or a parry).
		for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()); ++I)
		{
			if (Now - ActiveSince[I] < 0.05 || Now - ParryTime < 0.05)
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
		if (bHoldLoad ? Fight.overloaded()
					  : (Fight.load() > ftcore::kArenaReleaseOpenLoad + 10 && Now - ParryTime >= 0.15 && Now - ParryTime < 0.5))
		{
			TakeShot(TEXT("4_load"));
			return true;
		}
		if (bResultLogged)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: the fight ended before the load view; no load or release view."));
			ShotsTaken += 2;
		}
		break;
	case 4:
		// Just after a Release began: its banner and its pose (skipped if the fight ends first).
		if (Now - ReleaseTime >= 0.12 && Now - ReleaseTime < 0.4)
		{
			TakeShot(TEXT("5_release"));
			return true;
		}
		if (bResultLogged)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: no Release in this fight; no release view."));
			++ShotsTaken;
		}
		break;
	case 5:
		if (bResultLogged && GetResultAge() >= 0.8)
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
	PC->SetViewTarget(PlayerFigure);
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
		Bodies.player = At;
		PrevBodies.player = At;
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
	Restart(); // the fight as it began
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
