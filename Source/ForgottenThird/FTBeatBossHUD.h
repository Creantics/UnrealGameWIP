#pragma once

// Canvas UI of the beat boss demo (no UMG), drawn from AFTBeatBossGameMode (docs/BEAT_BOSS_DEMO.md):
//   - the beat line across the bottom of the screen: ftcore::BeatLine laid out in seconds, so a beat is
//     as wide in every bar and the markers move at one speed into the hit line on the left, where the
//     playhead stands. Bar lines are tall, beats short, an empty beat a gap with a small cross; each bar is
//     labelled with its number and meter (a meter the loop breaks in orange, a finisher bar in gold, a
//     count bar with its numerals). A marker is the boss's move at its impact: a strike cyan, a counter red
//     and spiked with its answer written over it, a hold gold with a tail to its end, the Release orange;
//     its size is its stem's, its button glyph is under it, and a move not yet told is dim. Counters
//     locked out after a whiff are grey and crossed. Rests are diamonds on the line, green when open;
//   - in a scripted silence the line dims and freezes under SILENCE and a draining bar; a count's
//     numerals pop in the middle of the screen;
//   - the boss's hp at the top with the phase's floor marked, PHASE n/N and the bar; over the boss, the
//     next counter's answer and a ring that closes as its impact comes; the lead's hp, the Load gauge,
//     the streak, the multiplier and the open rests under the line;
//   - PERFECT / GOOD / MISS (and TOO EARLY, LOCKED, rewards, Load) popping at the hit line, the boss's
//     damage over it, a red edge flash when the lead is hit, banners for a finisher, a stagger, a
//     Release and a phase;
//   - the result (VICTORY or DEFEAT with the counts) and the retry prompt.
// Drawing builds no strings: every word is a static FTUiText, and numbers and the few composed labels
// are built once, when the first fight begins.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTBeatBossHUD.generated.h"

class AFTBeatBossGameMode;
class UFont;

UCLASS()
class FORGOTTENTHIRD_API AFTBeatBossHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	// Every composed label, built once.
	void BuildTexts(const AFTBeatBossGameMode& Mode);
	const FString& Number(int32 Value) const;

	// Text with a drop shadow; Align 0 left, 1 centred, 2 right.
	void Text(const FString& Str, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale, int32 Align = 0);
	void Ring(float X, float Y, float Radius, const FLinearColor& Color, float Thickness, int32 Segments = 32);
	void Disc(float X, float Y, float Radius, const FLinearColor& Color);
	void Spikes(float X, float Y, float Radius, const FLinearColor& Color, float Thickness);
	void Frame(float X, float Y, float W, float H, const FLinearColor& Color, float Thickness);
	void Bar(float X, float Y, float W, float H, float Fraction, const FLinearColor& Fill, const FLinearColor& Back);
	void Diamond(float X, float Y, float Radius, const FLinearColor& Color, bool bFilled);

	void DrawBoss(const AFTBeatBossGameMode& Mode);
	void DrawBeatLine(const AFTBeatBossGameMode& Mode);
	void DrawLeadPanel(const AFTBeatBossGameMode& Mode);
	void DrawCount(const AFTBeatBossGameMode& Mode);
	void DrawPopups(const AFTBeatBossGameMode& Mode);
	void DrawBanners(const AFTBeatBossGameMode& Mode);
	void DrawResult(const AFTBeatBossGameMode& Mode);

	float UiScale = 1.f;
	float FontScale = 1.f; // UiScale with the fonts' boost
	bool bBuilt = false;

	TArray<FString> Numbers;      // "0" .. "999"
	TArray<FString> Damage;       // "-0" .. "-999"
	TArray<FString> Multipliers;  // "x0" .. "x9"
	TArray<FString> LoadGains;    // "+0 LOAD" .. "+100 LOAD"
	TArray<FString> PhaseLabels;  // "PHASE 1/2", ...
	TArray<FString> RetryLabels;  // "... Retry from phase 1", ...
	// Meters as written ("4/4", "5/4", "6/8"), by count x 16 + unit.
	TArray<FString> Meters;
	// The result's counts, built when a result appears.
	FString StatsText;
	int32 StatsFor = -1;
};
