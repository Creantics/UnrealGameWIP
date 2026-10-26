#pragma once

// Canvas UI of the exploration demo (no UMG): the objective list, the prompt for the focused spot,
// notices, banners and the closing card. Everything comes from the level's AFTExploreDirector;
// per-frame drawing uses cached or static strings only.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTExploreHUD.generated.h"

class AFTExploreDirector;
class UFont;

UCLASS()
class FORGOTTENTHIRD_API AFTExploreHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	AFTExploreDirector* FindDirector();

	void DrawObjectives(const AFTExploreDirector& D);
	void DrawPrompt(const AFTExploreDirector& D);
	void DrawFieldObjective(const AFTExploreDirector& D);
	void DrawWorld(const AFTExploreDirector& D);
	void DrawMessages(const AFTExploreDirector& D);

	void DrawStr(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale = 1.f);
	void DrawStrCentered(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale = 1.f);

	TWeakObjectPtr<AFTExploreDirector> CachedDirector;
	float UiScale = 1.f;
};
