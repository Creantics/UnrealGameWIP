#include "FTPlaceVerticalCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AI/NavigationSystemBase.h"
#include "AssetCompilingManager.h"
#include "Components/StaticMeshComponent.h"
#include "EditorWorldUtils.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTExploreDirector.h"
#include "FTNavIslands.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationSystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/MassingPlan.h"
#include "ftcore/WorldChecks.h"

#include <string>
#include <string_view>
#include <vector>
#endif

UFTPlaceVerticalCommandlet::UFTPlaceVerticalCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

#if WITH_EDITOR
// A named namespace, not an anonymous one: unity builds put this file in one translation unit with others.
namespace FTPlaceVerticalLocal
{
FName VerticalTag()
{
	return FName(TEXT("FTVertical"));
}

enum class EPieceCollide : uint8
{
	None,
	Block,
	Hidden,
};

struct FPiece
{
	FString Id;
	FString Kind;
	double Cx = 0.0, Cy = 0.0, Cz = 0.0;
	double Sx = 0.0, Sy = 0.0, Sz = 0.0;
	double Rot = 0.0, Pitch = 0.0;
	FString Material;
	EPieceCollide Collide = EPieceCollide::Block;
	bool IsProbe() const { return Kind == TEXT("probe"); }
};

bool KnownKind(const FString& Kind)
{
	static const TCHAR* const Kinds[] = {TEXT("tread"), TEXT("landing"), TEXT("deck"), TEXT("ramp"), TEXT("rail"), TEXT("post"),
		TEXT("stringer"), TEXT("pier"), TEXT("collider"), TEXT("probe")};
	for (const TCHAR* K : Kinds)
	{
		if (Kind == K)
		{
			return true;
		}
	}
	return false;
}

bool ReadNumber(const FString& Cell, double& Out)
{
	const FString Trimmed = Cell.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || !FCString::IsNumeric(*Trimmed))
	{
		return false;
	}
	Out = FCString::Atod(*Trimmed);
	return FMath::IsFinite(Out);
}

// The table: a header naming the columns (any order), then a row per piece; blank lines and lines starting with # are
// skipped. Every row is checked before anything is placed.
bool ReadPieces(const FString& File, TArray<FPiece>& Out, FString& Error)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		Error = FString::Printf(TEXT("couldn't read %s"), *File);
		return false;
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, true);
	TMap<FString, int32> Column;
	int32 LineNo = 0;
	TSet<FString> Ids;
	static const TCHAR* const Needed[] = {TEXT("id"), TEXT("kind"), TEXT("cx"), TEXT("cy"), TEXT("cz"), TEXT("sx"), TEXT("sy"), TEXT("sz"),
		TEXT("rot"), TEXT("pitch"), TEXT("material"), TEXT("collide")};
	for (const FString& Raw : Lines)
	{
		++LineNo;
		const FString Line = Raw.TrimStartAndEnd();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}
		TArray<FString> Cells;
		Line.ParseIntoArray(Cells, TEXT(","), false);
		if (Column.Num() == 0)
		{
			for (int32 I = 0; I < Cells.Num(); ++I)
			{
				Column.Add(Cells[I].TrimStartAndEnd().ToLower(), I);
			}
			for (const TCHAR* Name : Needed)
			{
				if (!Column.Contains(Name))
				{
					Error = FString::Printf(TEXT("%s: the header has no %s column"), *File, Name);
					return false;
				}
			}
			continue;
		}
		auto Cell = [&](const TCHAR* Name) -> FString
		{
			const int32 I = Column.FindChecked(Name);
			return Cells.IsValidIndex(I) ? Cells[I].TrimStartAndEnd() : FString();
		};
		FPiece P;
		P.Id = Cell(TEXT("id"));
		P.Kind = Cell(TEXT("kind")).ToLower();
		P.Material = Cell(TEXT("material"));
		const FString Collide = Cell(TEXT("collide"));
		const bool bNumbers = ReadNumber(Cell(TEXT("cx")), P.Cx) && ReadNumber(Cell(TEXT("cy")), P.Cy) && ReadNumber(Cell(TEXT("cz")), P.Cz) &&
			ReadNumber(Cell(TEXT("sx")), P.Sx) && ReadNumber(Cell(TEXT("sy")), P.Sy) && ReadNumber(Cell(TEXT("sz")), P.Sz) &&
			ReadNumber(Cell(TEXT("rot")), P.Rot) && ReadNumber(Cell(TEXT("pitch")), P.Pitch);
		if (P.Id.IsEmpty() || !KnownKind(P.Kind) || !bNumbers)
		{
			Error = FString::Printf(TEXT("%s:%d: a row needs an id, a known kind and numbers in cx..pitch"), *File, LineNo);
			return false;
		}
		if (Collide == TEXT("0"))
		{
			P.Collide = EPieceCollide::None;
		}
		else if (Collide == TEXT("1"))
		{
			P.Collide = EPieceCollide::Block;
		}
		else if (Collide == TEXT("2"))
		{
			P.Collide = EPieceCollide::Hidden;
		}
		else
		{
			Error = FString::Printf(TEXT("%s:%d (%s): collide is 0, 1 or 2"), *File, LineNo, *P.Id);
			return false;
		}
		if (!P.IsProbe() && (P.Sx <= 0.0 || P.Sy <= 0.0 || P.Sz <= 0.0 || FMath::Abs(P.Pitch) >= 60.0))
		{
			Error = FString::Printf(TEXT("%s:%d (%s): a piece needs a positive size and a pitch under 60 degrees"), *File, LineNo, *P.Id);
			return false;
		}
		if (Ids.Contains(P.Id))
		{
			Error = FString::Printf(TEXT("%s:%d: the id %s is used twice"), *File, LineNo, *P.Id);
			return false;
		}
		Ids.Add(P.Id);
		Out.Add(MoveTemp(P));
	}
	if (Column.Num() == 0)
	{
		Error = FString::Printf(TEXT("%s has no header"), *File);
		return false;
	}
	return true;
}

FVector PlanToCm(const ftcore::MassingVec& V)
{
	return FVector(V.x, V.y, V.z) * 100.0;
}

// How close (cm) the navmesh must lie to a probe's surface for the probe to count as walkable there: half a metre
// across, a metre up or down (the navmesh floats a little over the collision it is built from).
const FVector kVerticalProbeNavExtent(50.0, 50.0, 100.0);

// The surface a probe meets: a line down the pawn channel (what the lead stands on) from sz metres over the point (4 m
// when sz is 0) to 12 m under it, and whether the navmesh lies on that surface. One log line each; with Rows, a csv row
// too. With Islands (-Islands), each navmesh point's island too: its number (largest first) and plan area, so two
// surfaces a walk should join show whether the navmesh joins them.
int32 RunProbes(UWorld& World, double Side, const TArray<FPiece>& Pieces, const TCHAR* When, TArray<FString>* Rows, const UNavigationSystemV1* Nav,
	const ANavigationData* NavData, int32& OnNav, const FFTNavIslands* Islands = nullptr)
{
	int32 Met = 0;
	OnNav = 0;
	for (const FPiece& P : Pieces)
	{
		if (!P.IsProbe())
		{
			continue;
		}
		const FVector At = PlanToCm(ftcore::massingEnginePoint(Side, P.Cx, P.Cy, P.Cz));
		const double Over = P.Sz > 0.0 ? P.Sz : 4.0;
		FHitResult Hit;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(FTPlaceVerticalProbe), true);
		const bool bHit = World.LineTraceSingleByChannel(Hit, At + FVector(0, 0, Over * 100.0), At - FVector(0, 0, 1200), ECC_Pawn, Query);
		if (bHit)
		{
			++Met;
			const AActor* Actor = Hit.GetActor();
			const double HitZ = Hit.ImpactPoint.Z / 100.0;
			const FString Label = Actor ? Actor->GetActorLabel() : FString(TEXT("(none)"));
			FNavLocation OnMesh;
			const bool bNav = Nav && NavData && Nav->ProjectPointToNavigation(Hit.ImpactPoint, OnMesh, kVerticalProbeNavExtent, NavData);
			OnNav += bNav ? 1 : 0;
			const FString NavZ = bNav ? FString::Printf(TEXT("%.3f"), OnMesh.Location.Z / 100.0) : FString();
			FString Island, IslandCsv = TEXT(",");
			if (bNav && Islands)
			{
				const uint32* Index = Islands->IndexOf.Find(OnMesh.NodeRef);
				if (Index && *Index < Islands->Set.islandOf.size() && Islands->Set.islandOf[*Index] < Islands->Set.islands.size())
				{
					const uint32 N = Islands->Set.islandOf[*Index];
					const ftcore::NavIsland& Of = Islands->Set.islands[N];
					Island = FString::Printf(TEXT(", island %u (%.0f m2, centre %.1f, %.1f, z %.1f to %.1f)"), N, Of.area, Of.cx, Of.cy, Of.zMin, Of.zMax);
					IslandCsv = FString::Printf(TEXT("%u,%.1f"), N, Of.area);
				}
				else
				{
					Island = TEXT(", island none (poly not gathered)");
				}
			}
			UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical probe %s %s: expected %.2f, surface %.2f, gap %+.2f m, on %s, navmesh %s%s%s"), When,
				*P.Id, P.Cz, HitZ, HitZ - P.Cz, *Label, bNav ? TEXT("yes at ") : TEXT("no"), *NavZ, *Island);
			if (Rows)
			{
				Rows->Add(FString::Printf(TEXT("%s,%s,%.3f,%.3f,%.3f,%.3f,%s,%d,%s,%s"), When, *P.Id, P.Cx, P.Cy, P.Cz, HitZ, *Label, bNav ? 1 : 0, *NavZ,
					*IslandCsv));
			}
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTPlaceVertical probe %s %s: expected %.2f, no surface from %+.0f to -12 m"), When, *P.Id, P.Cz, Over);
			if (Rows)
			{
				Rows->Add(FString::Printf(TEXT("%s,%s,%.3f,%.3f,%.3f,,,0,,,"), When, *P.Id, P.Cx, P.Cy, P.Cz));
			}
		}
	}
	return Met;
}

// -Tour: the stops, read with ftcore::importTourStops, written over the explore director's saved ones. False (with
// Error) on an unreadable or bad table, or a map without exactly one director in its persistent level.
bool VerticalReadTour(const FString& File, std::vector<ftcore::TourStop>& Stops, FString& Error)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		Error = FString::Printf(TEXT("couldn't read %s"), *File);
		return false;
	}
	std::string Why;
	if (!ftcore::importTourStops(std::string_view(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num())), Stops, &Why))
	{
		Error = FString::Printf(TEXT("%s: %s"), *File, UTF8_TO_TCHAR(Why.c_str()));
		return false;
	}
	return true;
}

AFTExploreDirector* VerticalDirector(UWorld& World, FString& Error)
{
	AFTExploreDirector* Found = nullptr;
	int32 Count = 0;
	for (AActor* Actor : World.PersistentLevel->Actors)
	{
		if (AFTExploreDirector* Director = Cast<AFTExploreDirector>(Actor))
		{
			Found = Director;
			++Count;
		}
	}
	if (Count != 1)
	{
		Error = FString::Printf(TEXT("the persistent level has %d explore directors, not one"), Count);
		return nullptr;
	}
	return Found;
}

void VerticalWriteTour(AFTExploreDirector& Director, double Side, const std::vector<ftcore::TourStop>& Stops)
{
	Director.Modify();
	Director.TourStopIds.Reset();
	Director.TourStopKinds.Reset();
	Director.TourStopTargets.Reset();
	Director.TourStopHasStart.Reset();
	Director.TourStopStarts.Reset();
	Director.TourStopEdges.Reset();
	Director.TourStopStills.Reset();
	for (const ftcore::TourStop& Stop : Stops)
	{
		Director.TourStopIds.Add(UTF8_TO_TCHAR(Stop.id.c_str()));
		Director.TourStopKinds.Add(static_cast<uint8>(Stop.kind));
		Director.TourStopTargets.Add(PlanToCm(ftcore::massingEnginePoint(Side, Stop.x, Stop.y, Stop.z)));
		Director.TourStopHasStart.Add(Stop.hasStart);
		Director.TourStopStarts.Add(Stop.hasStart ? PlanToCm(ftcore::massingEnginePoint(Side, Stop.startX, Stop.startY, Stop.startZ)) : FVector::ZeroVector);
		Director.TourStopEdges.Add(static_cast<uint8>(Stop.edge));
		Director.TourStopStills.Add(Stop.still);
	}
}

// The map's streaming levels and whether each is loaded: the probes and the navmesh see only what is loaded here.
void VerticalLogStreaming(const UWorld& World)
{
	const TArray<ULevelStreaming*>& Levels = World.GetStreamingLevels();
	UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical: %d streaming levels."), Levels.Num());
	for (const ULevelStreaming* Level : Levels)
	{
		if (Level)
		{
			const ULevel* Loaded = Level->GetLoadedLevel();
			UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical: streaming level %s: %s, %d actors."), *Level->GetWorldAssetPackageName(),
				Loaded ? TEXT("loaded") : TEXT("not loaded"), Loaded ? Loaded->Actors.Num() : 0);
		}
	}
}

int32 NavTiles(const UWorld& World)
{
	int32 Tiles = 0;
	for (TActorIterator<ARecastNavMesh> It(&World); It; ++It)
	{
		Tiles += It->GetNavMeshTilesCount();
	}
	return Tiles;
}
} // namespace FTPlaceVerticalLocal
#endif

int32 UFTPlaceVerticalCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	using namespace FTPlaceVerticalLocal;
	FString MapPath, PiecesFile, ProbeOut;
	if (!FParse::Value(*Params, TEXT("Map="), MapPath) || !FPackageName::IsValidLongPackageName(MapPath) ||
		!FParse::Value(*Params, TEXT("Pieces="), PiecesFile))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: needs -Map=/Game/Maps/<map> and -Pieces=<csv>."));
		return 1;
	}
	FParse::Value(*Params, TEXT("ProbeOut="), ProbeOut);
	FString TourFile;
	FParse::Value(*Params, TEXT("Tour="), TourFile);
	double Side = 1000.0;
	FParse::Value(*Params, TEXT("Side="), Side);
	const bool bNoNav = FParse::Param(*Params, TEXT("NoNav"));
	const bool bRebuildNav = FParse::Param(*Params, TEXT("RebuildNav"));
	const bool bDryRun = FParse::Param(*Params, TEXT("DryRun"));
	const bool bIslands = FParse::Param(*Params, TEXT("Islands"));
	if (bNoNav && bRebuildNav)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: -NoNav and -RebuildNav ask for opposite things."));
		return 1;
	}

	TArray<FPiece> Pieces;
	FString Error;
	if (!ReadPieces(PiecesFile, Pieces, Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: %s."), *Error);
		return 1;
	}
	std::vector<ftcore::TourStop> TourStops;
	if (!TourFile.IsEmpty() && !VerticalReadTour(TourFile, TourStops, Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: -Tour: %s."), *Error);
		return 1;
	}
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Cube)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: /Engine/BasicShapes/Cube is missing."));
		return 1;
	}
	TMap<FString, UMaterialInterface*> Materials;
	for (const FPiece& P : Pieces)
	{
		if (P.IsProbe() || P.Material.IsEmpty() || P.Material == TEXT("none") || Materials.Contains(P.Material))
		{
			continue;
		}
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *P.Material);
		if (!Material)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: %s's material %s doesn't load."), *P.Id, *P.Material);
			return 1;
		}
		Materials.Add(P.Material, Material);
	}

	const FString File = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (!IFileManager::Get().FileExists(*File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: there is no %s."), *File);
		return 1;
	}
	UWorld::InitializationValues Init;
	Init.RequiresHitProxies(false)
		.ShouldSimulatePhysics(false)
		.EnableTraceCollision(true)
		.CreateNavigation(true)
		.CreateAISystem(false)
		.AllowAudioPlayback(false)
		.CreatePhysicsScene(true);
	FScopedEditorWorld Loaded(FStringView(MapPath), Init);
	UWorld* World = Loaded.GetWorld();
	if (!World || !World->PersistentLevel)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: couldn't load %s."), *MapPath);
		return 1;
	}
	int32 ActorsBefore = 0;
	TArray<AActor*> Old;
	for (AActor* Actor : World->PersistentLevel->Actors)
	{
		if (!Actor)
		{
			continue;
		}
		++ActorsBefore;
		if (Actor->Tags.Contains(VerticalTag()))
		{
			Old.Add(Actor);
		}
	}
	const int32 TilesBefore = NavTiles(*World);
	UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical: loaded %s: %d actors in its persistent level, %d placed by an earlier run; navmesh %d tiles."),
		*MapPath, ActorsBefore, Old.Num(), TilesBefore);
	VerticalLogStreaming(*World);
	AFTExploreDirector* Director = nullptr;
	if (!TourFile.IsEmpty())
	{
		Director = VerticalDirector(*World, Error);
		if (!Director)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: -Tour: %s; the map is not changed."), *Error);
			return 1;
		}
	}

	// The navigation system over the saved navmesh, for the probes' navmesh column and for the rebuild.
	FNavigationSystem::AddNavigationSystemToWorld(*World, FNavigationSystemRunMode::EditorMode);
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	const ANavigationData* NavData = Nav ? Nav->GetDefaultNavDataInstance(FNavigationSystem::DontCreate) : nullptr;
	for (TActorIterator<ARecastNavMesh> It(World); It && !NavData; ++It)
	{
		NavData = *It;
	}
	if (!NavData)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTPlaceVertical: %s has no navmesh to probe; the probes' navmesh column reads no."), *MapPath);
	}

	TArray<FString> ProbeRows;
	TArray<FString>* Rows = ProbeOut.IsEmpty() ? nullptr : &ProbeRows;
	int32 Probes = 0;
	for (const FPiece& P : Pieces)
	{
		Probes += P.IsProbe() ? 1 : 0;
	}
	// -Islands: the navmesh's islands (in the plan frame, nothing left out under a sea), gathered once per navmesh probed.
	TUniquePtr<FFTNavIslands> Islands;
	auto GatherIslands = [&Islands, bIslands, Side](const ANavigationData* Data, const TCHAR* When)
	{
		Islands.Reset();
		const ARecastNavMesh* Recast = Cast<const ARecastNavMesh>(Data);
		if (!bIslands || !Recast)
		{
			return;
		}
		const double Began = FPlatformTime::Seconds();
		Islands = MakeUnique<FFTNavIslands>();
		FTGatherNavIslands(*Recast, [Side](const FVector& V) { return ftcore::MassingVec{V.X / 100.0 + Side * 0.5, Side * 0.5 - V.Y / 100.0, V.Z / 100.0}; },
			ftcore::kNoSea, *Islands);
		UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical: the %s navmesh's islands: %d polys in %d tiles, %d islands, gathered in %.1f s."), When,
			Islands->Gathered, Islands->Tiles, static_cast<int32>(Islands->Set.islands.size()), FPlatformTime::Seconds() - Began);
	};
	GatherIslands(NavData, TEXT("saved"));
	int32 NavBefore = 0;
	const int32 MetBefore = RunProbes(*World, Side, Pieces, TEXT("before"), Rows, Nav, NavData, NavBefore, Islands.Get());

	for (AActor* Actor : Old)
	{
		World->EditorDestroyActor(Actor, true);
	}

	FActorSpawnParameters Spawn;
	Spawn.OverrideLevel = World->PersistentLevel;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	TMap<FString, int32> ByKind;
	int32 Placed = 0;
	for (const FPiece& P : Pieces)
	{
		if (P.IsProbe())
		{
			continue;
		}
		const FVector At = PlanToCm(ftcore::massingEnginePoint(Side, P.Cx, P.Cy, P.Cz));
		const FRotator Turn(P.Pitch, ftcore::massingEngineYaw(P.Rot), 0.0);
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(At, Turn, Spawn);
		if (!Actor)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: couldn't spawn %s."), *P.Id);
			return 1;
		}
		UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
		Mesh->SetStaticMesh(Cube);
		if (UMaterialInterface* const* Material = Materials.Find(P.Material))
		{
			Mesh->SetMaterial(0, *Material);
		}
		Actor->SetActorScale3D(FVector(P.Sx, P.Sy, P.Sz));
		switch (P.Collide)
		{
		case EPieceCollide::None:
			Mesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
			Mesh->SetCanEverAffectNavigation(false);
			break;
		case EPieceCollide::Block:
			Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
			break;
		case EPieceCollide::Hidden:
			Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
			Actor->SetActorHiddenInGame(true);
			Mesh->SetCastShadow(false);
			break;
		}
		Actor->SetActorLabel(P.Id);
		Actor->SetFolderPath(TEXT("FTVertical"));
		Actor->Tags.Add(VerticalTag());
		++ByKind.FindOrAdd(P.Kind);
		++Placed;
	}
	FString Kinds;
	for (const TPair<FString, int32>& K : ByKind)
	{
		Kinds += FString::Printf(TEXT("%s%s %d"), Kinds.IsEmpty() ? TEXT("") : TEXT(", "), *K.Key, K.Value);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical: removed %d, placed %d (%s)."), Old.Num(), Placed, Kinds.IsEmpty() ? TEXT("none") : *Kinds);

	int32 NavAfter = 0;
	const int32 MetAfter = RunProbes(*World, Side, Pieces, TEXT("after"), Rows, Nav, NavData, NavAfter);
	UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical: probes %d, surfaces met %d before and %d after (on the saved navmesh: %d and %d)."), Probes,
		MetBefore, MetAfter, NavBefore, NavAfter);

	if (Director)
	{
		const int32 Was = Director->TourStopIds.Num();
		VerticalWriteTour(*Director, Side, TourStops);
		int32 Stills = 0;
		for (const ftcore::TourStop& Stop : TourStops)
		{
			Stills += Stop.still ? 1 : 0;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical: -Tour: the director's %d tour stops replaced by %d from %s (%d end in a still)."), Was,
			static_cast<int32>(TourStops.size()), *TourFile, Stills);
	}

	const bool bChanged = Old.Num() > 0 || Placed > 0;
	int32 TilesAfter = TilesBefore;
	if ((bChanged && !bNoNav) || bRebuildNav)
	{
		if (!Nav)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: %s has no navigation system."), *MapPath);
			return 1;
		}
		// A commandlet's editor world keeps the async-load lock, and the saved bounds wait for a tick.
		Nav->RemoveNavigationBuildLock(ENavigationBuildLock::AsyncLoadLock, UNavigationSystemV1::ELockRemovalRebuildAction::NoRebuild);
		for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
		{
			Nav->OnNavigationBoundsUpdated(*It);
		}
		Nav->Tick(0.f);
		const double BuildStart = FPlatformTime::Seconds();
		Nav->Build();
		TilesAfter = NavTiles(*World);
		UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical: navmesh rebuilt in %.0f s: %d tiles (was %d)."), FPlatformTime::Seconds() - BuildStart,
			TilesAfter, TilesBefore);
		if (TilesAfter == 0)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: the rebuilt navmesh has no tiles; the map is not saved."));
			return 1;
		}
		// The probes once more, on the rebuilt navmesh: whether a walk can now stand on every new surface.
		NavData = Nav->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
		for (TActorIterator<ARecastNavMesh> It(World); It && !NavData; ++It)
		{
			NavData = *It;
		}
		GatherIslands(NavData, TEXT("rebuilt"));
		int32 NavRebuilt = 0;
		const int32 MetRebuilt = RunProbes(*World, Side, Pieces, TEXT("rebuilt"), Rows, Nav, NavData, NavRebuilt, Islands.Get());
		UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical: probes on the rebuilt navmesh: %d of %d met a surface, %d of them on the navmesh."), MetRebuilt,
			Probes, NavRebuilt);
	}
	if (Rows)
	{
		ProbeRows.Insert(TEXT("when,id,x,y,expected_z,surface_z,actor,nav,nav_z,island,island_m2"), 0);
		if (!FFileHelper::SaveStringToFile(FString::Join(ProbeRows, TEXT("\n")) + TEXT("\n"), *ProbeOut, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: couldn't write %s."), *ProbeOut);
			return 1;
		}
	}
	int32 ActorsAfter = 0;
	for (const AActor* Actor : World->PersistentLevel->Actors)
	{
		ActorsAfter += Actor ? 1 : 0;
	}
	if (bDryRun)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical -DryRun: %d actors (was %d), not saved."), ActorsAfter, ActorsBefore);
		return 0;
	}
	FAssetCompilingManager::Get().FinishAllCompilation();
	UPackage* Package = World->GetPackage();
	Package->MarkPackageDirty();
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, World, *File, Save))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTPlaceVertical: saved %s: %d actors (was %d), navmesh %d tiles (was %d)."), *File, ActorsAfter, ActorsBefore,
		TilesAfter, TilesBefore);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTPlaceVertical needs an editor build."));
	return 1;
#endif
}
