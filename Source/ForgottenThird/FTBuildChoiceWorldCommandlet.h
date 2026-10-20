#pragma once

// Builds the choice-screen map (/Game/Maps/Choice by default) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildChoiceWorld [-Map=Choice] -NullRHI -unattended
//       -nopause -nosplash
//
// A small lit room (floor, back wall, sun, sky light, one point light), a camera facing the wall
// and a player start. AFTChoiceGameMode is the map's game mode; the dialogue loop itself is canvas UI. It
// overwrites the map.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildChoiceWorldCommandlet.generated.h"

UCLASS()
class UFTBuildChoiceWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildChoiceWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
