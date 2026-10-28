#pragma once

// Builds the exploration demo map (/Game/Maps/Explore by default) from code, so the map is never
// hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildExploreWorld [-Map=Explore] [-Style=<spec>]
//       -NullRHI -unattended -nopause -nosplash
//
// With -World=<id> it builds that big world's map instead (FTWorldMapBuilder.h). The light, air and
// camera finish are the shared photoreal look (FTSceneLook.h); -Style=<spec> adds a style controller
// over it (docs/STYLE_PACKS_UE.md).
//
// A walled square of village blockout: houses, trees, a fountain, a raised terrace with a ramp,
// and an inner wall with one gap that the old gate closes, with a courtyard beyond it. The spots
// of ftcore::demo::kExploreSpots are placed as AFTExploreSpot actors and handed to one
// AFTExploreDirector; a player start at kExploreStart faces +X, and AFTExploreGameMode is the
// map's game mode, so pressing Play starts the demo. The props stay off the straight lines the
// autopilot walks between spots. It overwrites the map.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildExploreWorldCommandlet.generated.h"

UCLASS()
class UFTBuildExploreWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildExploreWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
