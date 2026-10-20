#pragma once

// Canvas UI of the choice screen (no UMG), for AFTChoiceGameMode. With a loop: a box at the bottom
// with the speaker, the line (wrapped once per change), the numbered options during a pick (used
// wrong ones dim) and the key prompt. No score is shown. Without a loop file: exactly the stub card
// (title, label, "Step i of n", "continue"). Without a story step: "No story step". Drawing builds
// no strings.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTChoiceHUD.generated.h"

class AFTChoiceGameMode;
class UFont;

UCLASS()
class FORGOTTENTHIRD_API AFTChoiceHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawCard(const AFTChoiceGameMode& Mode);
	void DrawLoop(const AFTChoiceGameMode& Mode);
	void DrawCentered(const FString& Text, const FLinearColor& Color, float Y, UFont* Font, float Scale);
	// Splits Src into Lines at spaces so each fits MaxWidth (and at newlines).
	void Wrap(const FString& Src, UFont* Font, float Scale, float MaxWidth);

	float UiScale = 1.f;
	TArray<FString> Lines;
	uint32 WrappedSerial = MAX_uint32;
	float WrappedWidth = -1.f;
};
