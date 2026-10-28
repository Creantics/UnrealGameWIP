#pragma once

// Game mode of the cutscene screen (GameplayScreen::Cutscene: a CG step of a story gameplay beat) on
// the stage map. The map's AFTCutsceneDirector (or one spawned when the map has none) plays the step's
// shot list from Data/Cutscene/cutscenes.json, takes the skip presses and hands the step back to the
// story when it ends (its logs are in FTCutsceneDirector.h). This mode shows AFTCutsceneHUD
// (letterbox, the step's title, a skip hint). With StoryCarry.bAutoPlay the step skips itself after
// one second of real time. Opened without a valid carry it plays the default list as a preview and
// stays on the map.

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "ftcore/Cutscene.h"

#include "FTCutsceneGameMode.generated.h"

class AFTCutsceneDirector;

UCLASS()
class FORGOTTENTHIRD_API AFTCutsceneGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	AFTCutsceneGameMode();

	virtual void StartPlay() override;

	// True when the map was opened with a story step to play.
	bool HasStep() const { return bHasStep; }
	// The director playing the step; the HUD draws from it (its text included).
	const AFTCutsceneDirector* GetDirector() const { return Director; }

private:
	UPROPERTY()
	TObjectPtr<AFTCutsceneDirector> Director;

	bool bHasStep = false;
};
