// The minimap's model from a world's layout tables on disk: the bake (FTBuildMiniMap) and the dev switch
// (-MiniMapTables=) share this. The tables are private and never shipped; a shipped level carries the baked blob
// (AFTExploreDirector::MiniMapBlob).
#pragma once

#include "CoreMinimal.h"

#include "ftcore/MiniMap.h"

namespace FTMiniMapSource
{
struct FOptions
{
	FString Dir;          // the tables: *_roads_*, *_streets_*, *_boxes_*, *towers*, *_doors_*.csv and one height .json
	double Side = 0.0;    // the plan's square (m)
	double Area[4] = {};  // plan x0, y0, x1, y1 (m)
	double SeaFromX = -1.0e9; // plan x from which ground under the datum is sea
		int32 MaxRev = MAX_int32; // <Prefix>Rev=: the newest table revision to use (a folder keeps several)
};
// Reads -<Prefix>Tables=, -<Prefix>Area=x0,y0,x1,y1, -<Prefix>Sea=, -<Prefix>Side= from a command line. False when
// the tables or the area aren't given; Side keeps its value unless given.
bool ParseOptions(const TCHAR* Cmd, const TCHAR* Prefix, FOptions& Out);
bool Build(const FOptions& Options, ftcore::MiniMapModel& Out, FString& Error);
} // namespace FTMiniMapSource
