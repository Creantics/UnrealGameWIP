#pragma once

// Canvas UI of the beat fight screen (no UMG), drawn from AFTBeatGameMode (docs/BEAT_SCREEN.md):
//   - each open prompt as a marker over its enemy (the enemy stand-in's place, projected), a ring that
//     closes in on it as ftcore::BeatFight::approach() runs from 1 to 0, and the arrow inside. A strike is
//     a cyan ring; a counter reads as a threat: red, spiked, and greyed with a cross while counters are
//     locked out; a hold is gold and shows a bar as long as the hold, which then fills as holdProgress()
//     runs; a finisher's strikes are gold with an inner ring. An enemy's next prompts follow to the right
//     of the first, smaller;
//   - the enemies' hp (a panel each, the one whose turn it is framed), the lead's hp, the chain and the
//     multiplier, and the lead-in's beats counted down to the first bar;
//   - PERFECT / GOOD / MISS, LATE and LOCKED popups over the enemy, a flash and LOCKED when counters are
//     locked out, a flash when the lead is hit, and a cue when a finisher starts;
//   - the result (VICTORY or DEFEAT with the counts) and, after a defeat, the Retry and Give up prompt.
// Drawing builds no strings: every word is a static FTUiText, and the numbers are rebuilt only when they
// change.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTBeatHUD.generated.h"

class AFTBeatGameMode;
class UFont;

namespace ftcore
{
struct BeatPrompt;
}

// A number as text, rebuilt only when the number changes (the HUD's BeatNumberOf does it).
struct FBeatNumberText
{
	int32 Value = MIN_int32;
	FString Text;
};

UCLASS()
class FORGOTTENTHIRD_API AFTBeatHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawStrCentered(const FString& Text, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale);
	void DrawRing(float X, float Y, float Radius, const FLinearColor& Color, float Thickness);
	void DrawSpikes(float X, float Y, float Radius, const FLinearColor& Color, float Thickness);
	void DrawFrame(float X, float Y, float W, float H, const FLinearColor& Color, float Thickness);
	void DrawBar(float X, float Y, float W, float H, float Fraction, const FLinearColor& Fill, const FLinearColor& Back);
	// Direction: ftcore::Arrow order (Left, Down, Up, Right).
	void DrawGlyph(int32 Direction, float X, float Y, float Radius, const FLinearColor& Color, float Thickness);
	// Where enemy Enemy's markers hang on the screen: its stand-in's anchor projected, else a place on the screen.
	FVector2D EnemyPoint(const AFTBeatGameMode& Mode, int32 Enemy, int32 Count);
	// One prompt's marker. Approach is ftcore::BeatFight::approach() (0 to 1), HoldFill a held hold's progress.
	void DrawMarker(float X, float Y, float Scale, const ftcore::BeatPrompt& Prompt, float Approach, bool bLocked, bool bFirst, float HoldFill);
	void DrawPrompts(const AFTBeatGameMode& Mode);
	void DrawEnemyPanels(const AFTBeatGameMode& Mode);
	void DrawLeadPanel(const AFTBeatGameMode& Mode);
	void DrawChain(const AFTBeatGameMode& Mode);
	void DrawLeadIn(const AFTBeatGameMode& Mode);
	void DrawPopups(const AFTBeatGameMode& Mode);
	void DrawFlashes(const AFTBeatGameMode& Mode);
	void DrawResult(const AFTBeatGameMode& Mode);

	float UiScale = 1.f;

	FBeatNumberText LeadHpText;
	FBeatNumberText ChainText;
	FBeatNumberText MultiplierText;
	FBeatNumberText LeadInText;
	// The result's counts, built when the result appears (and again after a retry's).
	FString StatsText;
	bool bStatsBuilt = false;
};
