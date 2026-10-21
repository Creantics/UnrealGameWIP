#pragma once

// Builds the mini-game map (/Game/Maps/Mini by default) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildMiniWorld [-Map=Mini] -NullRHI -unattended
//       -nopause -nosplash
//
// A small lit room (floor, back wall, sun, sky light, one point light), a camera facing the wall
// and a player start. AFTMiniGameMode is the map's game mode; the rhythm round itself is canvas UI. It
// overwrites the map.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildMiniWorldCommandlet.generated.h"

UCLASS()
class UFTBuildMiniWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildMiniWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
