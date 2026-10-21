#pragma once

// Canvas UI of the banter screen (no UMG), for AFTBanterGameMode. With a banter file: a box at the
// bottom with the speaker and the line (wrapped once per change), set to the left for the first
// speaker in the file and to the right for the second (a third and fourth alternate again), a thin
// bar that runs down while the line holds, the line count and the key prompt. Without a banter file:
// exactly the stub card (title, label, "Step i of n", "continue"). Without a story step: "No story
// step". Drawing builds no strings.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTBanterHUD.generated.h"

class AFTBanterGameMode;
class UFont;

UCLASS()
class FORGOTTENTHIRD_API AFTBanterHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawCard(const AFTBanterGameMode& Mode);
	void DrawBanter(const AFTBanterGameMode& Mode);
	void DrawCentered(const FString& Text, const FLinearColor& Color, float Y, UFont* Font, float Scale);
	// Splits Src into Lines at spaces so each fits MaxWidth (and at newlines).
	void Wrap(const FString& Src, UFont* Font, float Scale, float MaxWidth);

	float UiScale = 1.f;
	TArray<FString> Lines;
	uint32 WrappedSerial = MAX_uint32;
	float WrappedWidth = -1.f;
	// "Line i of n", rebuilt only when the serial moves.
	FString CountText;
	uint32 CountSerial = MAX_uint32;
};
