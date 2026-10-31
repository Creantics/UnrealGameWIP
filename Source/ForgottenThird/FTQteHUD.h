#pragma once

// Canvas UI of the quick-time event screen (no UMG), drawn from AFTQteGameMode: the current prompt
// and the next two, each a ring that closes in on its target (the battle screen's tell frame, as a
// circle) with the button glyph and its key; a second inner ring marks a narrow prompt; a hold shows
// a bar that fills, a mash a row of boxes that fills; a pip per prompt along the top (grey pending,
// gold perfect, cyan good, red miss); PERFECT / GOOD / MISS popups rise from the current prompt; and
// the one-line result at the end. A co-op step tints the prompts by lead and tags them 1 and 2.
// Static strings only: drawing builds none.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTQteHUD.generated.h"

class AFTQteGameMode;

UCLASS()
class FORGOTTENTHIRD_API AFTQteHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawCentered(const FString& Text, const FLinearColor& Color, float Y, UFont* Font, float Scale);
	void DrawStrCentered(const FString& Text, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale);
	void DrawRing(float X, float Y, float Radius, const FLinearColor& Color, float Thickness);
	// The face-button diamond with Button (ftcore::ActionButton) filled (FTActionButtons::DrawGlyph).
	void DrawGlyph(int32 Button, float X, float Y, float Radius, const FLinearColor& Color, float Thickness);
	void DrawPrompt(const AFTQteGameMode& Mode, int32 Index, int32 Slot, float X, float Y);
	void DrawPips(const AFTQteGameMode& Mode, float Y);
	void DrawPopups(const AFTQteGameMode& Mode, float X, float Y);

	float UiScale = 1.f;
};
