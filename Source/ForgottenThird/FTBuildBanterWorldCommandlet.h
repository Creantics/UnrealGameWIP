#pragma once

// Builds the banter-screen map (/Game/Maps/Banter by default) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildBanterWorld [-Map=Banter] -NullRHI -unattended
//       -nopause -nosplash
//
// A small lit room (floor, back wall, sun, sky light, one point light), a camera facing the wall
// and a player start. AFTBanterGameMode is the map's game mode; the banter itself is canvas UI. It
// overwrites the map.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildBanterWorldCommandlet.generated.h"

UCLASS()
class UFTBuildBanterWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildBanterWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
