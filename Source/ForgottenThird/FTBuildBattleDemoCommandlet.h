#pragma once

// Builds the battle demo map (/Game/Maps/BattleDemo) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildBattleDemo -NullRHI -unattended -nopause -nosplash
//
// It writes a floor, a sun, a sky light, sky atmosphere, height fog, a player start and an
// AFTBattleDemoActor at the origin (its camera frames the fight), with AFTBattleGameMode as the
// map's game mode so pressing Play starts the battle. It overwrites the map.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildBattleDemoCommandlet.generated.h"

UCLASS()
class UFTBuildBattleDemoCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildBattleDemoCommandlet();

	virtual int32 Main(const FString& Params) override;
};
