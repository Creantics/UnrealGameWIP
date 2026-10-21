#pragma once

// Builds the quick-time event screen map (/Game/Maps/Qte by default) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildQteWorld [-Map=Qte] -NullRHI -unattended
//       -nopause -nosplash
//
// A small lit room (floor, back wall, sun, sky light, one point light), a camera facing the wall
// and a player start. AFTQteGameMode is the map's game mode; the prompts are canvas UI. It
// overwrites the map.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildQteWorldCommandlet.generated.h"

UCLASS()
class UFTBuildQteWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildQteWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
