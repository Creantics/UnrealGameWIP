#pragma once

// Game mode of the exploration demo map: AFTExploreCharacter as the pawn, AFTExploreHUD for the
// objectives and prompts, and no story on play (AFTExploreDirector in the level runs the demo).

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "FTExploreGameMode.generated.h"

UCLASS()
class FORGOTTENTHIRD_API AFTExploreGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	AFTExploreGameMode();

	virtual void StartPlay() override;
};
