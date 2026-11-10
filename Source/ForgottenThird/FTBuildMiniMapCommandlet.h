#pragma once

// Bakes a world map's HUD minimap model (ftcore/MiniMap.h) into the level's AFTExploreDirector::MiniMapBlob, so a run
// needs no tables:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildMiniMap -Map=/Game/Maps/World_Solhaven_Strip
//       -Tables=<layout table folder> -Area=x0,y0,x1,y1 [-Sea=<plan x>] [-Side=<m>] [-Rev=<n>] -unattended -nopause -nosplash
//
// The folder holds one each of the roads, streets, boxes, towers and doors tables and one height map (.json + .r16);
// -Side defaults to the director's PlanSide; of several revisions it takes the newest (at most -Rev). It saves the map in place.

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
