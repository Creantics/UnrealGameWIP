#pragma once

// Game mode of the quick-time event screen: what QTE and `co-op QTE` gameplay steps open
// (GameplayScreen::Qte; the rules are ftcore::QteSequence and ftcore::QteScreen in Qte.h). It reads
// UFTGameInstance::StoryCarry, picks the step's sequence from Data/Qte/qte.json (the table's default
// when the step isn't listed, the built-in default when the file is missing or refused), plays it
// with AFTQteHUD and returns to the story the way the stub screen does. No story on play.
//
// Keys, read by polling (so a hold can see its key go up): arrow keys / WASD / pad d-pad or the pad's
// X A Y B for left, down, up, right, the same mapping as the battle screen. A press that is the
// wrong direction is ignored. There is no fail state: a miss plays on, and the one-line result
// (Perfect, Good, Fair or Poor) shows for about a second before the story continues.
//
// With StoryCarry.bAutoPlay the autopilot plays at the ideal times, on a clock compressed to fit
// about 8 s of game time (the screen ends by 10 s at the latest). With -AlphaFast the screen's timers
// run at 8x (the world's own dilation already counts), as on the stub screen.
//
// Logs carry ids and counts only, never text:
//   QTE: done <scene> beat <b> step <s> prompts <n> perfect <p> good <g> miss <m>

#include "CoreMinimal.h"

#include "FTGameMode.h"

#include "ftcore/Qte.h"

#include "FTQteGameMode.generated.h"

UCLASS()
class FORGOTTENTHIRD_API AFTQteGameMode : public AFTGameMode
{
	GENERATED_BODY()

public:
	// A grade popup for the HUD: which prompt, how it went, and when (game seconds on the screen).
	struct FPopup
	{
		int32 Prompt = 0;
		ftcore::QteGrade Grade = ftcore::QteGrade::Pending;
		double Time = 0.0;
	};
	static constexpr int32 MaxPopups = 8;

	AFTQteGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// ---- for AFTQteHUD (no allocation) ----
	// True when the map was opened with a story step to show.
	bool HasStep() const { return bHasStep; }
	// True when the step is a `co-op QTE` (prompts alternate between two leads).
	bool IsCoop() const { return bCoop; }
	const ftcore::QteScreen& GetScreen() const { return Screen; }
	// The popups, newest last; entries past GetPopupCount() are stale.
	int32 GetPopupCount() const { return FMath::Min(PopupTotal, MaxPopups); }
	const FPopup& GetPopup(int32 Index) const { return Popups[(PopupTotal - GetPopupCount() + Index) % MaxPopups]; }

private:
	// Reads Data/Qte/qte.json into Table; false (logged, ids only) when missing or refused.
	bool LoadTable();
	// Polls the four direction key sets and feeds the screen presses and releases.
	void PollKeys();
	void AddPopups(const ftcore::QteResults& Results);
	void Finish();
	void OpenReturnMap();

	ftcore::QteTable Table;
	ftcore::QteScreen Screen;
	bool bHasStep = false;
	bool bCoop = false;
	bool bFast = false;
	bool bReturning = false;
	bool bDown[4] = {};
	FPopup Popups[MaxPopups];
	int32 PopupTotal = 0;
};
