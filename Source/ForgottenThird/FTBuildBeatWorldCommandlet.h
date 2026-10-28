#pragma once

// Builds the beat fight screen map (/Game/Maps/Beat by default) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildBeatWorld [-Map=Beat] -NullRHI -unattended
//       -nopause -nosplash
//
// A lit room (floor, back wall, sun, sky light, one point light), a camera at the near end looking down it,
// a player start behind the camera, and a stand-in for each enemy a beat fight can have
// (ftcore::kMaxBeatEnemies), standing in a shallow arc that faces the camera. A stand-in is a body (an
// engine cylinder in its own tint) with a head and a visor on its front, all engine basic shapes. The body
// is tagged AFTBeatGameMode::StandInTag(index), which is how the game mode finds it, seats as many as the
// fight has enemies and hides the rest. AFTBeatGameMode is the map's game mode; the markers, the hp and
// the result are canvas UI. It overwrites the map. FTStep::Prepare opens this map for a fight in a beat
// world once it exists.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildBeatWorldCommandlet.generated.h"

UCLASS()
class UFTBuildBeatWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildBeatWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
