#pragma once

// Builds the arena demo map (/Game/Maps/Demo_Arena by default) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildArenaWorld [-Map=Demo_Arena] -NullRHI -unattended
//       -nopause -nosplash
//
// The map holds only a player start, where the game mode puts the player for the plain zone's ring
// (AFTArenaGameMode::PlayerStartFor). A zone in a world is held in that world's own map (World_Solhaven, ...), which
// the game mode opens; the plain zone gets its ground and look from the game mode at run time. Nothing is built
// into the map. It overwrites the map.

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
