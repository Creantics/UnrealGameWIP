// The minimap's model from a world's layout tables on disk, named file by file with the same switches the world build
// (FTBuildMassingWorld) takes, so the minimap is built from exactly the tables the map was built from: the world build
// bakes it itself, FTBuildMiniMap re-bakes a saved map, and the dev switch (-MiniMapStreets= ...) builds one at run
// time. The tables are private and never shipped; a shipped level carries the baked blob (AFTExploreDirector::MiniMapBlob).
#pragma once

#include "CoreMinimal.h"

#include "ftcore/MiniMap.h"

namespace FTMiniMapSource
{
struct FOptions
{
	// -<Prefix>Roads=, -<Prefix>Streets=, -<Prefix>Boxes=, -<Prefix>StandInTowers=, -<Prefix>RoadDoors=, -<Prefix>Height=
	FString Roads, Streets, Boxes, Towers, Doors, Height;
	double Side = 0.0;        // -<Prefix>Side=: the plan's square (m)
	bool bArea = false;       // -MiniMapArea=x0,y0,x1,y1 (plan m); without it the roads' and streets' bounds
	double Area[4] = {};
	double SeaFromX = -1.0e9; // -<Prefix>Sea=: plan x from which ground under the datum is sea
	FString AreaError;        // set when -MiniMapArea= is given but isn't four numbers
};
// Reads the switches above from a command line (a leading '-' on each, so -Streets= never matches -PropStreets=). True
// when roads or streets are given. Side keeps its value unless given.
bool ParseOptions(const TCHAR* Cmd, const TCHAR* Prefix, FOptions& Out);
// Builds the model. Skipped: the road and street kinds the map left out (see ftcore::buildMiniMap), for the log.
bool Build(const FOptions& Options, ftcore::MiniMapModel& Out, FString& Error, FString* Skipped = nullptr);
// The tables named, for a log line.
FString Describe(const FOptions& Options);
} // namespace FTMiniMapSource
