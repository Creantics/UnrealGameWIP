#pragma once

// Canvas UI of the title screen (no UMG): New Game and Continue on the Demo map, shown while the
// default game mode is in its title state. Skeleton: the game-frame stream fills it in.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTTitleHUD.generated.h"

UCLASS()
class FORGOTTENTHIRD_API AFTTitleHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
