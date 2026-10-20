#pragma once

// Builds the stub-screen map (/Game/Maps/Stub by default) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildStubWorld [-Map=Stub] -NullRHI -unattended
//       -nopause -nosplash
//
// A small lit room (floor, back wall, sun, sky light, one point light), a camera facing the wall
// and a player start. AFTStubGameMode is the map's game mode; the card itself is canvas UI. It
// overwrites the map.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildStubWorldCommandlet.generated.h"

UCLASS()
class UFTBuildStubWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildStubWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
