#pragma once

// Canvas UI of the mini-game screen (no UMG), for AFTMiniGameMode. Four lanes in arrow order (left,
// down, up, right; lanes the round doesn't use are dim), notes falling to a judgement line that
// pulses on the beat, the latest grade, the try count and, between rounds, the outcome (CLEARED,
// AGAIN or ONWARD) with the round's accuracy. Without a story step: "No story step". Drawing builds
// strings only when the try or the outcome changes.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTMiniHUD.generated.h"

class AFTMiniGameMode;
class UFont;

UCLASS()
class FORGOTTENTHIRD_API AFTMiniHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawCentered(const FString& Text, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale);

	float UiScale = 1.f;
	// "Try i of n" and the result's accuracy line, rebuilt when the try or the outcome moves.
	FString TryText;
	FString AccuracyText;
	int32 CachedAttempt = -1;
	int32 CachedOutcome = -1;
};
