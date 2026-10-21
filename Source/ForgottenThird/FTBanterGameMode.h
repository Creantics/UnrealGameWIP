#pragma once

// Game mode of the banter screen: what BNT gameplay steps open (GameplayScreen::Banter; the rules are
// ftcore::BanterSequence in Banter.h). It reads UFTGameInstance::StoryCarry, loads the step's banter
// file, Data/Story/banter/<scene>_<beat>_<step>.json (under the game instance's StoryDir), plays it
// with AFTBanterHUD and returns to the story the way the stub screen does. No story on play.
//
// The lines run on their own: each holds for a time set by its length (or its `hold`), then the next
// one shows. Keys (AFTPlayerController's story actions): Space / left click / pad A or Enter / pad
// Start show the next line at once; Escape / Backspace / pad B skip the rest. With
// StoryCarry.bAutoPlay it skips by itself after the input grace. With -AlphaFast the timers run at
// 8x, as on the stub screen.
//
// When the banter file is missing (or refused) it shows exactly the stub card (the step's title,
// label and "Step i of n") and continues on a press, so the alpha needs no banter data. Opened
// without a valid carry it only says "No story step".
//
// On finish it marks the carry done and opens ReturnMap. Logs carry ids and counts only, never line
// text:
//   Banter: done <scene> beat <b> step <s> lines <n> skipped <0|1>

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "ftcore/Banter.h"
#include "ftcore/StubScreen.h"

#include "FTBanterGameMode.generated.h"

class AFTPlayerController;
struct FFTStoryCarry;

UCLASS()
class FORGOTTENTHIRD_API AFTBanterGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	AFTBanterGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	// ---- for AFTBanterHUD (no allocation) ----
	// True when the map was opened with a story step.
	bool HasStep() const { return bHasStep; }
	// True when the step's banter file loaded; false shows the stub card instead.
	bool HasBanter() const { return bHasBanter; }
	const ftcore::BanterSequence& GetBanter() const { return Banter; }
	// Bumped whenever the cached strings below change (the HUD re-wraps then).
	uint32 GetSerial() const { return Serial; }
	const FString& GetSpeaker() const { return Speaker; }
	const FString& GetText() const { return Text; }
	// The stub card's text (the fallback), built once in StartPlay.
	const FString& GetTitleText() const { return TitleText; }
	const FString& GetLabelText() const { return LabelText; }
	const FString& GetStepText() const { return StepText; }

private:
	UFUNCTION()
	void HandlePress();
	UFUNCTION()
	void HandleSkip();

	// Reads the step's banter file into Def; false (logged, ids only) when it is missing or refused.
	bool LoadBanter(const FFTStoryCarry& Carry);
	// Refreshes the cached strings when the line changed.
	void CacheLine();
	bool InputReady() const;
	void Finish();
	void OpenReturnMap();

	TWeakObjectPtr<AFTPlayerController> Controller;

	ftcore::BanterDef Def;
	ftcore::BanterSequence Banter;
	ftcore::StubScreen Card; // the fallback card, and the no-step state
	bool bHasStep = false;
	bool bHasBanter = false;
	bool bAutoPlay = false;
	bool bFast = false;
	bool bReturning = false;
	double Elapsed = 0.0; // game seconds on the screen (sped up with -AlphaFast)

	uint32 CachedEpoch = MAX_uint32;
	uint32 Serial = 0;
	FString Speaker;
	FString Text;

	FString TitleText;
	FString LabelText;
	FString StepText;
};
