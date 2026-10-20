#pragma once

// Canvas dialogue box (no UMG) for the level's AFTStoryDirector: the speaker, the text typing out,
// numbered options once a prompt is out (greyed when they can't be picked), a continue marker, a
// scene title card and an end card. The text is wrapped once per beat (and when the screen width
// or the subtitle size changes); per-frame drawing reuses cached strings and one buffer for the line
// being typed. Also draws the backlog overlay (the director's, scrolled by it; wrapped lines are cached
// until the backlog, the scroll or the width changes), SKIP / AUTO indicators, and the settings overlay.

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
	void DrawIndicators(const AFTStoryDirector& D);
	void DrawBacklog(const AFTStoryDirector& D);
	void RebuildBacklog(const AFTStoryDirector& D, UFont* Font, float Scale, float TextW, float AreaTop, float AreaBottom);
	void Wrap(const FString& Src, UFont* Font, float Scale, float MaxWidth);
	// Wraps Src into OutLines; the optional arrays get each line's first character and length in codepoints.
	void WrapInto(const FString& Src, UFont* Font, float Scale, float MaxWidth, TArray<FString>& OutLines, TArray<int32>* OutStart,
		TArray<int32>* OutChars);

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
	float WrappedScale = -1.f;
	FString Partial; // the line being typed (keeps its capacity)

	// The backlog overlay's visible lines, each already placed: text, colour and top edge.
	TArray<FString> BacklogText;
	TArray<FLinearColor> BacklogColor;
	TArray<float> BacklogY;
	TArray<FString> BacklogWrap; // scratch for one entry
	uint32 BacklogSerial = MAX_uint32;
	int32 BacklogBuiltScroll = -1;
	float BacklogBuiltWidth = -1.f;
	float BacklogBuiltScale = -1.f;
	float BacklogBuiltBottom = -1.f;
	TWeakObjectPtr<class UFTSettingsSubsystem> CachedSettings;
};
