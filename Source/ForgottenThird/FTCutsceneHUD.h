#pragma once

// Canvas UI of the cutscene screen (no UMG): letterbox bars (2.39:1, at least 8% of the height each),
// the step's title and "Step i of n" in the top bar, "Shot i of n" and the skip hint in the bottom
// bar. All text comes from AFTCutsceneGameMode, built once there; no story text. Drawing builds no
// strings.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTCutsceneHUD.generated.h"

UCLASS()
class FORGOTTENTHIRD_API AFTCutsceneHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
