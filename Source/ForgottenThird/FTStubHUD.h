#pragma once

// Canvas UI of the stub screen (no UMG): a centred card with the step's title, its label and
// "Step i of n" from AFTStubGameMode (text built once there), and a "continue" prompt. Opened
// without a story step it says "No story step". Drawing builds no strings.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTStubHUD.generated.h"

UCLASS()
class FORGOTTENTHIRD_API AFTStubHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawCentered(const FString& Text, const FLinearColor& Color, float Y, UFont* Font, float Scale);

	float UiScale = 1.f;
};
