#pragma once

// Game mode of the stub screen: the placeholder a gameplay step with no real screen yet opens
// (unknown tokens in ftcore::GameplayPlan; GameplayScreen::Stub). The choice, banter and other screens
// draw the same card when their step's data file is missing. It
// shows the step (AFTStubHUD) and continues on Space, Enter or pad A (by itself after a second
// with StoryCarry.bAutoPlay), then returns to the story through UFTGameInstance::StoryCarry. No
// story on play. Opened without a valid carry it only says "No story step".

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "ftcore/StubScreen.h"

#include "FTStubGameMode.generated.h"

UCLASS()
class FORGOTTENTHIRD_API AFTStubGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	AFTStubGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// True when the map was opened with a story step to show.
	bool HasStep() const { return bHasStep; }
	// Text for the HUD, built once in StartPlay.
	const FString& GetTitleText() const { return TitleText; }
	const FString& GetLabelText() const { return LabelText; }
	const FString& GetStepText() const { return StepText; }

private:
	void Finish();
	void OpenReturnMap();

	ftcore::StubScreen Screen;
	bool bHasStep = false;
	bool bFast = false;
	bool bReturning = false;
	FString TitleText;
	FString LabelText;
	FString StepText;
};
