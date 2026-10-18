#pragma once

// Game mode of the battle demo map: the default pawn and player controller, AFTBattleHUD for the
// battle UI, and no story on play (AFTBattleDemoActor in the level runs the fight).

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "FTBattleGameMode.generated.h"

UCLASS()
class FORGOTTENTHIRD_API AFTBattleGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	AFTBattleGameMode();

	virtual void StartPlay() override;
};
