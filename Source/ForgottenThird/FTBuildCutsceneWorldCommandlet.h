#pragma once

// Builds the cutscene-screen map (/Game/Maps/Cutscene by default) from code, so the map is never
// hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildCutsceneWorld [-Map=Cutscene] -NullRHI
//       -unattended -nopause -nosplash
//
// A blockout set: a floor, a back wall, boxes and four pillars, lit by a sun, a sky light and a lamp.
// Camera spots spot_a..spot_f are target points tagged with their ids; subjects subj_a..subj_c are
// tinted capsule placeholders (a cylinder body tagged with the id, a sphere head attached to it). An
// AFTCutsceneDirector with its cine camera plays the shots. AFTCutsceneGameMode is the map's game
// mode. It overwrites the map.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildCutsceneWorldCommandlet.generated.h"

UCLASS()
class UFTBuildCutsceneWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildCutsceneWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
