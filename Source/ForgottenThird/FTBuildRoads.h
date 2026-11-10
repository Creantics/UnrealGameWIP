#pragma once

// FTBuildMassingWorld's roads: wraps ftcore/Roads.h. -Roads=<csv> builds each road of the table (asphalt,
// kerbs, pavements, markings, the junction and its signals, signs, the rail, poles and wires, the bus stop) from the
// engine's cube and cylinder in flat tints, its street lamps as TQ-04 props (so -NightLamps lights them), and hides the
// deck and pier rows over it. -Vehicles parks stand-in cars in its bays and puts scooters and bicycles along it and in
// the lanes (-RoadDoors=<csv> keeps them off the doors). -LaneLamps=<x0,y0,x1,y1> adds lane lamps in that plan box so
// the night pools overlap. Without -Roads nothing here runs, so earlier rounds rebuild as they were.

#include "CoreMinimal.h"
#include "Templates/Function.h"

#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/MassingProps.h"
#include "ftcore/Roads.h"

#include <vector>

class UMaterialInterface;
class UPackage;
class UStaticMesh;
class UWorld;
struct FActorSpawnParameters;

// What the map's builder lends the roads.
struct FFTRoadsHost
{
	UWorld* World = nullptr;
	const FActorSpawnParameters* Spawn = nullptr;
	UPackage* Package = nullptr;
	UMaterialInterface* Base = nullptr; // the world surface master (its Color and Roughness parameters)
	UStaticMesh* Cube = nullptr;
	double Side = 0.0;
	std::vector<ftcore::MassingFoot>* Taken = nullptr; // the props' room: the roads add theirs
	// Lay sets them: Taken[RoadsFrom, RoadsTo) are the roads' bands and furniture, which the two-wheelers along a road's
	// back of pavement stand inside (they keep off the rest).
	size_t RoadsFrom = 0, RoadsTo = 0;
	// Places a TQ-04 street lamp as the builder places a prop (its night light with -NightLamps). False on failure.
	TFunction<bool(UStaticMesh* Mesh, const ftcore::MassingProp& Lamp, const FString& Label)> PlaceLamp;
};

namespace FTBuildRoads
{
// The command line's road switches.
struct FOptions
{
	FString RoadsFile;  // -Roads=
	FString DoorsFile;  // -RoadDoors=
	bool bVehicles = false; // -Vehicles
	bool bLaneLamps = false; // -LaneLamps=x0,y0,x1,y1
	double LaneBox[4] = {0, 0, 0, 0};
	uint32 Seed = 1110;
};
FOptions ParseOptions(const TCHAR* Params);

// Reads and builds the roads (Core only) and hides the rows of Boxes they clear (kind set to "underpass"). False,
// logged, on a bad table. A road network table (ftcore::roadNetTable) is built whole on Ground into Builds[0]; Roads
// then holds one stand-in per network street (kind "net", its id only, an empty build each) for DropRoadStreets.
bool Prepare(const FOptions& Options, const ftcore::MassingGround& Ground, std::vector<ftcore::MassingBox>& Boxes,
	std::vector<ftcore::RoadSpec>& Roads, std::vector<ftcore::RoadBuild>& Builds);

// Takes the roads' own rows (same id) out of a streets table, so -Kerbs doesn't kerb them twice. Returns how many.
size_t DropRoadStreets(const std::vector<ftcore::RoadSpec>& Roads, std::vector<ftcore::MassingStreet>& Streets);

// Lays the roads' solids and lamps, and with -Vehicles parks the cars in the bays. False on failure.
bool Lay(FFTRoadsHost& Host, const FOptions& Options, const std::vector<ftcore::RoadSpec>& Roads,
	const std::vector<ftcore::RoadBuild>& Builds);

// After the props: with -LaneLamps, lamps along the lanes of StreetsFile in its box (clear of every TQ-04 lamp already
// in the world); with -Vehicles, scooters and bicycles along those lanes and the roads. False on failure.
bool Finish(const FFTRoadsHost& Host, const FOptions& Options, const std::vector<ftcore::RoadSpec>& Roads,
	const std::vector<ftcore::RoadBuild>& Builds, const FString& StreetsFile, const ftcore::MassingGround& Ground);
} // namespace FTBuildRoads
