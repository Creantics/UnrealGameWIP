#pragma once

// Builds the beat boss demo's map (/Game/Maps/Demo_BeatBoss by default) from code, so the map is never
// hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildBeatBossWorld [-Map=Demo_BeatBoss] -unattended -nosplash
//
// A lit room (floor, back wall, two pillars, sun, sky light, one point light), the lead and the boss facing
// each other down it, and a camera behind the lead's right shoulder looking past it at the boss. The lead
// is a skeletal mesh actor wearing the test human (turned to face the boss), tagged
// AFTBeatBossGameMode::LeadTag(); the game mode plays its idle. The boss is a large placeholder of engine
// basic shapes (a body, a head with a visor, shoulders and arms), the body tagged
// AFTBeatBossGameMode::BossTag() and every part attached to it, so the game mode can swell, lean, lunge and
// tint it as one. AFTBeatBossGameMode is the map's game mode; the beat line and the rest are canvas UI. It
// overwrites the map.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildBeatBossWorldCommandlet.generated.h"

UCLASS()
class UFTBuildBeatBossWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildBeatBossWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
