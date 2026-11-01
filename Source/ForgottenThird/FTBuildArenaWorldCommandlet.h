#pragma once

// Builds the arena demo map (/Game/Maps/Demo_Arena by default) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildArenaWorld [-Map=Demo_Arena] -NullRHI -unattended
//       -nopause -nosplash
//
// A lit round arena: a stone disc a little wider than AFTArenaGameMode::MapRingRadius (12 m) on a wider ground,
// a pale line painted where ftcore::ArenaFight's ring holds everyone, a knee-high wall of blocks just outside
// it (low enough for the camera to look over), eight pillars with a lamp by each around it, the shared look
// (FTScene::SpawnLook) and a player start where the game mode puts the player (PlayerStartFor). Everything
// that fights is spawned by AFTArenaGameMode, the map's game mode. It overwrites the map.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildArenaWorldCommandlet.generated.h"

UCLASS()
class UFTBuildArenaWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildArenaWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
