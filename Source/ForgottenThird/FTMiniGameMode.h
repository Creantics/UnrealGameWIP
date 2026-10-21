#pragma once

// Game mode of the mini-game screen: what MINI gameplay steps open (GameplayScreen::Mini; the rules
// are ftcore::MiniScreen in Mini.h). It reads UFTGameInstance::StoryCarry, picks the step's round from
// Data/Mini/mini.json (the table's default when the step isn't listed, the built-in default when the
// file is missing or refused), plays it with AFTMiniHUD and returns to the story the way the stub
// screen does. No story on play.
//
// A short rhythm round: arrow steps on a beat grid scroll down their lanes to a line, and the player
// presses each as it lands. Keys, read by polling: arrow keys / WASD / pad d-pad or the pad's X A Y B
// for left, down, up, right, the same mapping as the battle and QTE screens. There is no fail state:
// a round below the pass mark is retried while tries remain, and after the last try the story
// continues anyway.
//
// With StoryCarry.bAutoPlay the autopilot hits every note on a clock compressed so the round fits
// about 0.6 s of game time, and the screen ends within about a second. With -AlphaFast the screen's
// timers run at 8x (the world's own dilation already counts), as on the stub screen.
//
// Logs carry ids and counts only:
//   Mini: done <scene> beat <b> step <s> tries <t> accuracy <pct> cleared <0|1>

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "ftcore/Mini.h"

#include "FTMiniGameMode.generated.h"

UCLASS()
class FORGOTTENTHIRD_API AFTMiniGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	AFTMiniGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// ---- for AFTMiniHUD (no allocation) ----
	// True when the map was opened with a story step to show.
	bool HasStep() const { return bHasStep; }
	const ftcore::MiniScreen& GetScreen() const { return Screen; }

private:
	// Reads Data/Mini/mini.json into Table; false (logged, ids only) when missing or refused.
	bool LoadTable();
	// Polls the four direction key sets and feeds the screen presses.
	void PollKeys();
	void Finish();
	void OpenReturnMap();

	ftcore::MiniTable Table;
	ftcore::MiniScreen Screen;
	bool bHasStep = false;
	bool bFast = false;
	bool bReturning = false;
	bool bDown[4] = {};
};
