#pragma once

// The default game mode: AFTPawn driven by AFTPlayerController. It has two states on the Demo map.
//
// Title: a fresh start (no story running, no gameplay hand-off pending) or the map option ?Title
// (the battle screen's "Quit to title"). AFTTitleHUD shows New Game, Continue and Settings; the rules
// are ftcore::TitleMenu. New Game starts the story at its start scene (-StoryScene is a headless flag,
// see below), Continue loads the autosave slot. Either then switches to the story state. Settings
// opens UFTSettingsSubsystem's overlay (it takes the input while it is up; the title ignores presses).
//
// Story: AFTStoryHUD with the AFTStoryDirector playing the story. Back from a gameplay screen the
// story is already running: only the director is spawned, and it resumes the story. When the story
// finishes, the end card shows and Space / Enter reopens this map with ?Title.
//
// Headless flags (-StoryAutoPlay, -StoryAutoPlayFast, -StoryAutoPlayHandoff, -StoryScene=,
// -AlphaAutoPlay) skip the title and the return to it: a fresh run starts the story at once
// (-AlphaAutoPlay logs "Title: New Game (autoplay).") and finishing leaves the director's
// behaviour alone (-StoryQuitOnFinish quits). See docs/GAME_FRAME.md.

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "ftcore/StorySave.h"
#include "ftcore/TitleMenu.h"

#include "FTGameMode.generated.h"

class AFTPlayerController;
class UFTStorySubsystem;

UCLASS(Config = Game)
class FORGOTTENTHIRD_API AFTGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AFTGameMode();

	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Story")
	bool bStartStoryOnPlay = true;
	// Empty: the story's start scene.
	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Story")
	FString StartScene;
	// The map the end card returns to (this map, opened with ?Title).
	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Story")
	FString TitleMap = TEXT("Demo");
	// Seconds the end card ignores presses, so the press that ended the story doesn't skip it.
	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Story")
	float EndCardDelay = 0.6f;

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void StartPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	// ---- for AFTTitleHUD (no allocation) ----
	bool IsTitleShown() const { return bTitle; }
	const ftcore::TitleMenu& GetTitleMenu() const { return Menu; }
	// Why Continue is greyed (None: it isn't).
	ftcore::SaveRefusal GetSaveStatus() const { return SaveStatus; }
	// The mouse is over a title item: selects it (a greyed one stays unselected).
	void HoverTitleItem(ftcore::TitleItem Item) { Menu.hover(Item); }

private:
	UFUNCTION()
	void HandleMenuStep(int32 Step);
	UFUNCTION()
	void HandleMenuPress();
	UFUNCTION()
	void HandleMenuChoose(int32 Option);
	UFUNCTION()
	void HandleStoryFinished();
	UFUNCTION()
	void HandleEndPress();

	UFTStorySubsystem* GetStory() const;
	AFTPlayerController* GetPlayer() const;
	// The settings overlay is up (or closed this frame): the title ignores input.
	bool SettingsBlocked() const;
	void OpenSettings();
	void EnterTitle(UFTStorySubsystem& Story);
	void ApplyTitleAction(ftcore::TitleAction Action);
	// Leaves the title state for the story (already started or loaded) and spawns its director.
	void EnterStory();
	void SpawnDirector();
	void WatchForEnd();
	void ReturnToTitle();

	bool bTitle = false;
	bool bHeadless = false;
	bool bWatchingEnd = false;
	bool bEndCard = false;
	bool bReturning = false;
	double EndCardTime = 0.0;
	ftcore::TitleMenu Menu;
	ftcore::SaveRefusal SaveStatus = ftcore::SaveRefusal::Empty;
};
