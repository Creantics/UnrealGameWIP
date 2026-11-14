#pragma once

// Re-bakes a saved world map's HUD minimap model (ftcore/MiniMap.h) into the level's AFTExploreDirector::MiniMapBlob,
// so a run needs no tables. The world build (FTBuildMassingWorld) already bakes it from the tables it builds the map
// from; this is for a map saved before that, and takes the same switches, so the build's table arguments paste in:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildMiniMap -Map=/Game/Maps/World_Solhaven_Strip
//       -Roads=<csv> -Streets=<csv> [-Boxes=<csv>] [-StandInTowers=<csv>] [-RoadDoors=<csv>] [-Height=<json>]
//       [-Sea=<plan x>] [-Side=<m>] [-MiniMapArea=x0,y0,x1,y1] -unattended -nopause -nosplash
//
// -Side defaults to the director's PlanSide; without -MiniMapArea the map covers every road and street. It saves the map
// in place.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildMiniMapCommandlet.generated.h"

UCLASS()
class UFTBuildMiniMapCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildMiniMapCommandlet();

	virtual int32 Main(const FString& Params) override;
};
