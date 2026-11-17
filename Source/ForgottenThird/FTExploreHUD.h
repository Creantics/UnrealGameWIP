#pragma once

// Canvas UI of the exploration demo (no UMG): the objective list, the prompt for the focused spot,
// notices, banners and the closing card. Everything comes from the level's AFTExploreDirector;
// per-frame drawing uses cached or static strings only. While a cutscene plays in place
// (AFTCutsceneDirector::IsPlayingInPlace) it draws that cutscene's letterbox (FT::DrawCutsceneHUD)
// instead.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "ftcore/UiScreens.h"

#include "FTExploreHUD.generated.h"

class AFTExploreDirector;
class UFTMiniMap;
class UFont;

UCLASS()
class FORGOTTENTHIRD_API AFTExploreHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void DrawHUD() override;
	// M: the minimap's whole-map overlay on a world map (nothing elsewhere).
	void ToggleMapOverlay();
	// A review capture's HUD: the aim reticle alone (no objectives, prompts or controls line).
	bool bReticleOnly = false;
	// The screens open over play (ftcore/UiScreens.h): the map overlay (Lead's controller's HUD, when an explore HUD),
	// the party screen (Lead's), the pause menu and the settings. Each play HUD part asks ftcore::uiHudDraws with it.
	static ftcore::UiScreenMask ScreensOpen(const class AFTExploreCharacter* Lead);

private:
	AFTExploreDirector* FindDirector();

	void DrawObjectives(const AFTExploreDirector& D);
	void DrawPrompt(const AFTExploreDirector& D);
	void DrawFieldObjective(const AFTExploreDirector& D);
	void DrawWorld(const AFTExploreDirector& D);
	void DrawMessages(const AFTExploreDirector& D, bool bNotice = true); // bNotice false: the new HUD shows the notices
	// The centre reticle while the lead aims (right mouse held), faded in with the aim stance.
	void DrawAimReticle();

	void DrawStr(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale = 1.f);
	void DrawStrCentered(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale = 1.f);

	TWeakObjectPtr<AFTExploreDirector> CachedDirector;
	float UiScale = 1.f;

	// The corner minimap of a world map, made at BeginPlay so its one-off raster runs with the level's load, not on the
	// first frame (null when the level has none); drawn only on a world map.
	void TryMakeMiniMap(const AFTExploreDirector& D);
	UPROPERTY(Transient)
	TObjectPtr<UFTMiniMap> MiniMap;
	bool bMiniMapTried = false;

	// The new UI's party screen (FT::MakeExplorePartyWidget), added at BeginPlay when FT::NewUiEnabled (null under
	// -FTOldUi).
	TSharedPtr<class SWidget> NewUiWidget;
	// The new UI's explore HUD (FT::MakeExploreHudWidget): the party stack, the objective, the prompt at its spot and the
	// notices; the Canvas panels for those are not drawn while it stands.
	TSharedPtr<class SWidget> ExploreHudWidget;
};
