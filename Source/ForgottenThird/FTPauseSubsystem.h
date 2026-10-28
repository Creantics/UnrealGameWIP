#pragma once

// The pause menu of one game instance: its state (ftcore::PauseMenu), what opening it freezes, and the
// pause-aware wall clock. Esc or pad Start pauses any gameplay screen (not the title). AFTPlayerController
// routes the key: it backs out of the topmost layer first (the settings overlay, then a layer of the
// screen itself, the backlog or an aim in progress) and only then calls Open().
//
// While the menu is open:
//   - the world is paused (UGameplayStatics::SetGamePaused), so actors, timers and the walk stand still;
//   - the story is paused (UFTStorySubsystem::SetPaused), since its hold timer ticks outside the world's pause;
//   - the music, the voice and the ambience are paused (UFTAudioSubsystem::SetGamePaused), so the audio
//     cursor the conductor follows stops;
//   - WallSeconds() stands still, so gameplay timing that reads it (the battle clock, the conductor, the
//     cutscene's real-time clock) doesn't count the pause;
//   - the screens ignore their own input: they check BlocksInput() where they check the settings
//     overlay's. It stays true for the rest of the frame the menu closed in, so the press that closes
//     it doesn't also reach them. Screens that read held keys by polling check JustResumed() too: what
//     is still held the frame after is not a press for them.
//
// Input: Open() turns the controller's menu input on (cursor shown, up / down / left / right, click) and
// its pause menu context (every key the menu and the settings overlay need, above the screen's own
// contexts), and listens to the controller's menu events like the settings overlay does. Up / Down pick a
// row, Enter / Space / pad A / click pick it, Backspace / pad B step back, Esc / Start step back through
// AFTPlayerController. Back on the list resumes. Settings opens the settings overlay on top (its Back
// returns here). Quit to Title and Quit Game ask first (No is the default).
//
// Quit to Title closes the menu (unpauses) and opens the title the way the battle's own "Quit to title"
// does: the carry's return map (else the game mode's TitleMap) with the URL option ?Title. Quit Game calls
// UKismetSystemLibrary::QuitGame. Headless runs (-StoryAutoPlay*, -AlphaAutoPlay, -ExploreAutoPlay,
// -BattleAutoPlay, -WorldAutoPlay, -OverworldAutoPlay, a carry on autoplay) never open it. A map load
// closes it and puts every item back on. Logs carry no text: "Pause: opened.", "Pause: resumed.",
// "Pause: settings.", "Pause: quit to title.", "Pause: quit game."

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ftcore/PauseMenu.h"

#include "FTPauseSubsystem.generated.h"

class AFTPlayerController;
class UWorld;

UCLASS()
class FORGOTTENTHIRD_API UFTPauseSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	static constexpr int32 RowCount = static_cast<int32>(ftcore::PauseMenu::kItemCount);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---- the clock ----

	// Seconds since start-up on a clock that stands still while the menu is open: FPlatformTime::Seconds()
	// minus all the time spent paused so far (process-wide, across maps). Gameplay timing that must not
	// count a pause reads this instead of the platform clock. Every timestamp compared with it has to
	// come from it too (the conductor, the battle clock and the tap time of the controller all do).
	static double WallSeconds();

	// A run with no one at the keys (an autoplay flag): the menu never opens.
	static bool IsHeadlessRun();

	// ---- for the screens ----

	// The subsystem of the world `WorldContext` is in; null when there is none.
	static const UFTPauseSubsystem* Get(const UObject* WorldContext);
	// BlocksInput() / JustResumed() of that subsystem, false when there is none.
	static bool IsBlocking(const UObject* WorldContext);
	static bool IsJustResumed(const UObject* WorldContext);

	// ---- the menu ----

	bool IsOpen() const { return bOpen; }
	// True while open and for the rest of the frame it closed in.
	bool BlocksInput() const { return bOpen || CloseFrame == GFrameCounter; }
	// True in the frame the menu closed in and the next one.
	bool JustResumed() const { return CloseFrame != MAX_uint64 && GFrameCounter <= CloseFrame + 1; }
	// Pauses `World` and opens the menu on its first player controller. False when it can't open: already
	// open, headless, no AFTPlayerController, or the world can't be paused.
	bool Open(UWorld* World);
	// Resumes: undoes everything Open did.
	void Close();
	// Esc, pad Start, Backspace or pad B: a step back (the quit prompt closes, else the menu resumes).
	void PressBack();

	// ---- for the HUD (no allocation) ----

	const ftcore::PauseMenu& GetMenu() const { return Menu; }
	// Where the HUD drew a list row and a button of the quit prompt (0 Yes, 1 No), for the mouse.
	void SetRowRect(int32 Row, const FBox2D& Box) { RowRects[Row] = Box; }
	void SetChoiceRect(int32 Choice, const FBox2D& Box) { ChoiceRects[Choice] = Box; }
	// The mouse is at `Mouse`: when it moved since last time, selects the row or button under it.
	void UpdateHover(const FVector2D& Mouse);

private:
	UFUNCTION()
	void HandleStep(int32 Step);
	UFUNCTION()
	void HandleAdjust(int32 Direction);
	UFUNCTION()
	void HandlePress();
	UFUNCTION()
	void HandleCancel();
	void HandlePreLoadMap(const FString& MapName);

	void Apply(ftcore::PauseAction Action);
	void OpenSettings();
	void QuitToTitle();
	void QuitGame();
	// Undoes Open without logging: the controller's input, the pause of the world, the story, the audio
	// and the clock. A settings overlay opened from here is closed.
	void Teardown();
	// The settings overlay is up (or closed this frame): it owns the input.
	bool SettingsBlocked() const;
	int32 HitRow(const FVector2D& Point) const;
	int32 HitChoice(const FVector2D& Point) const;

	ftcore::PauseMenu Menu;
	TArray<FBox2D> RowRects;
	TArray<FBox2D> ChoiceRects;
	FVector2D LastMouse = FVector2D(-1.0, -1.0);
	uint64 OpenFrame = 0;
	uint64 CloseFrame = MAX_uint64;
	bool bOpen = false;
	bool bBeganMenu = false;  // Open turned the controller's menu input on, so closing turns it off
	bool bPausedStory = false; // Open paused the story, so closing resumes it
	TWeakObjectPtr<AFTPlayerController> Controller;
	TWeakObjectPtr<UWorld> PausedWorld;
};
