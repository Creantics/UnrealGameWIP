#pragma once

// Canvas UI of the title screen (no UMG): New Game and Continue on the Demo map, shown while
// AFTGameMode is in its title state. The menu rules and the selection live in the game mode
// (ftcore::TitleMenu); this only draws them. Continue is greyed when the save slot isn't usable,
// with a short reason underneath. Drawing builds no strings: every label is a static FString.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTTitleHUD.generated.h"

class AFTGameMode;
class UFont;

UCLASS()
class FORGOTTENTHIRD_API AFTTitleHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawCentered(const FString& S, const FLinearColor& Color, float Y, UFont* Font, float Scale);
};
