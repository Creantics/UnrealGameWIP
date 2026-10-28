#pragma once

// Game mode of the choice screen: the dialogue loop that CHC and HE gameplay steps open
// (GameplayScreen::Choice; the rules are ftcore::DialogueLoop). It reads UFTGameInstance::StoryCarry,
// loads the step's loop file, Data/Story/loops/<scene>_<beat>_<step>.json (under the game instance's
// StoryDir), plays it with AFTChoiceHUD and returns to the story the way the stub screen does. No
// story on play.
//
// Keys (AFTPlayerController's story actions): Space / left click / pad A or Enter continue; 1-4 / pad
// d-pad pick an option; Backspace / pad B skip to the closing lines (Escape and pad Start open the
// pause menu, which stops the loop and ignores these keys while it is up). With StoryCarry.bAutoPlay
// it skips by itself and finishes after about a second of game time.
//
// When the loop file is missing (or refused) it shows exactly the stub card (the step's title, label
// and "Step i of n") and continues on a press, so the alpha needs no loop data. Opened without a
// valid carry it only says "No story step".
//
// On finish it sets the loop's flags through UFTStorySubsystem::SetFlag, marks the carry done and
// opens ReturnMap. Logs carry ids and counts only, never line text:
//   Choice: done <scene> beat <b> step <s> picks <n> skipped <0|1>

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "ftcore/DialogueLoop.h"
#include "ftcore/StubScreen.h"

#include "FTChoiceGameMode.generated.h"

class AFTPlayerController;
struct FFTStoryCarry;

UCLASS()
class FORGOTTENTHIRD_API AFTChoiceGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxOptions = 4;

	AFTChoiceGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	// ---- for AFTChoiceHUD (no allocation) ----
	// True when the map was opened with a story step.
	bool HasStep() const { return bHasStep; }
	// True when the step's loop file loaded; false shows the stub card instead.
	bool HasLoop() const { return bHasLoop; }
	const ftcore::DialogueLoop& GetLoop() const { return Loop; }
	// Bumped whenever the cached strings below change (the HUD re-wraps then).
	uint32 GetSerial() const { return Serial; }
	const FString& GetSpeaker() const { return Speaker; }
	const FString& GetText() const { return Text; }
	// Option lines as shown ("1  ..."); Num() is the option count.
	const TArray<FString>& GetOptions() const { return Options; }
	bool IsOptionGreyed(int32 Option) const { return Option >= 0 && Option < MaxOptions && Greyed[Option]; }
	// The stub card's text (the fallback), built once in StartPlay.
	const FString& GetTitleText() const { return TitleText; }
	const FString& GetLabelText() const { return LabelText; }
	const FString& GetStepText() const { return StepText; }

private:
	UFUNCTION()
	void HandlePress();
	UFUNCTION()
	void HandleChoose(int32 Option);
	UFUNCTION()
	void HandleSkip();

	// Reads the step's loop file into Def; false (logged, ids only) when it is missing or refused.
	bool LoadLoop(const FFTStoryCarry& Carry);
	// Refreshes the cached strings when the loop moved.
	void CacheLoop();
	bool InputReady() const;
	void Finish();
	void ApplyFlags(const FFTStoryCarry& Carry);
	void OpenReturnMap();

	TWeakObjectPtr<AFTPlayerController> Controller;

	ftcore::DialogueLoopDef Def;
	ftcore::DialogueLoop Loop;
	ftcore::StubScreen Card; // the fallback card, and the no-step state
	bool bHasStep = false;
	bool bHasLoop = false;
	bool bAutoPlay = false;
	bool bAutoSkipped = false;
	bool bFast = false;
	bool bReturning = false;
	double Elapsed = 0.0; // game seconds on the screen (sped up with -AlphaFast)

	uint32 CachedEpoch = MAX_uint32;
	uint32 Serial = 0;
	FString Speaker;
	FString Text;
	TArray<FString> Options;
	bool Greyed[MaxOptions] = {};

	FString TitleText;
	FString LabelText;
	FString StepText;
};
