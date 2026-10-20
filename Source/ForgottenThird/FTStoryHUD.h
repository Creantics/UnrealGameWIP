#pragma once

// Canvas dialogue box (no UMG) for the level's AFTStoryDirector: the speaker, the text typing out,
// numbered options once a prompt is out (greyed when they can't be picked), a continue marker, a
// scene title card and an end card. The text is wrapped once per beat (and when the screen width
// changes); per-frame drawing reuses cached strings and one buffer for the line being typed.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTStoryHUD.generated.h"

class AFTStoryDirector;
class UFont;

UCLASS()
class FORGOTTENTHIRD_API AFTStoryHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	AFTStoryDirector* FindDirector();

	void DrawBox(const AFTStoryDirector& D);
	void DrawTitle(const AFTStoryDirector& D);
	void DrawBanner(const AFTStoryDirector& D);
	void Wrap(const FString& Src, UFont* Font, float Scale, float MaxWidth);

	void DrawStr(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale = 1.f);
	void DrawStrCentered(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale = 1.f);

	TWeakObjectPtr<AFTStoryDirector> CachedDirector;
	float UiScale = 1.f;

	// The wrapped text of the current beat: each line with its first character's index and its
	// length, both in codepoints (what the presenter counts).
	TArray<FString> Lines;
	TArray<int32> LineStart;
	TArray<int32> LineChars;
	uint32 WrappedSerial = MAX_uint32;
	float WrappedWidth = -1.f;
	FString Partial; // the line being typed (keeps its capacity)
};
