#pragma once

// The player's settings (ftcore::Settings) for one game instance, and the settings overlay's state.
// The values live in Saved/Settings.json (JSON from ftcore::encodeSettings): read when the game
// instance starts, written when the overlay closes. A missing, unreadable or newer-version file means
// defaults. Changes apply at once: readers poll GetRevision() and re-read Get() when it moves
// (AFTStoryDirector does, for the text speed, auto-advance and skip; AFTStoryHUD reads the subtitle size).
//
// The overlay is opened by the title (its Settings entry) or by the story director (Escape), through
// Open(). While it is open it takes the player controller's menu input (up / down move, left / right
// step the value, Enter / Space / click change it, Escape closes) and the owner's own handlers must
// ignore input: they check BlocksInput(), which stays true for the rest of the frame the overlay
// closed in, so the press that closes it doesn't also reach them. The HUD draws it (FTSettingsOverlay)
// and reports each row's rectangle back here for the mouse. Rules are ftcore::SettingsMenu.
//
// Settings never reach a headless autoplay run: the director ignores them there. Logs carry no text.

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ftcore/Settings.h"

#include "FTSettingsSubsystem.generated.h"

class AFTPlayerController;

UCLASS()
class FORGOTTENTHIRD_API UFTSettingsSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	static constexpr int32 RowCount = static_cast<int32>(ftcore::kSettingsRowCount);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---- the values ----
	const ftcore::Settings& Get() const { return Values; }
	// Bumped by every change, including the load at start-up.
	uint32 GetRevision() const { return Revision; }
	// Saved/Settings.json
	static FString GetFilePath();
	// Writes the file. False when the write failed.
	bool Save() const;

	// ---- the overlay ----
	bool IsOpen() const { return bOpen; }
	// True while open and for the rest of the frame it closed in.
	bool BlocksInput() const { return bOpen || CloseFrame == GFrameCounter; }
	// Opens the overlay on `Controller` (menu input on, cursor shown). Does nothing when already open.
	void Open(AFTPlayerController* Controller);
	// Saves and closes.
	void Close();

	// ---- for the HUD (no allocation) ----
	int32 GetSelectedRow() const { return static_cast<int32>(Menu.selected()); }
	const FString& GetRowLabel(int32 Row) const { return RowLabels[Row]; }
	const FString& GetRowValue(int32 Row) const { return RowValues[Row]; }
	// Every row but Back has a value to step.
	static bool IsRowAdjustable(int32 Row) { return Row != static_cast<int32>(ftcore::SettingsRow::Back); }
	// Where the HUD drew a row, and the column of its value (clicks left of ValueMid but right of
	// ValueLeft step down, any other click on the row steps up).
	void SetRowRect(int32 Row, const FBox2D& Box) { RowRects[Row] = Box; }
	void SetValueColumn(float Left, float Mid)
	{
		ValueLeft = Left;
		ValueMid = Mid;
	}
	// The mouse is at `Mouse`: when it moved since last time, selects the row under it.
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

	void Load();
	void Apply(ftcore::SettingsAction Action);
	void MarkChanged();
	int32 HitRow(const FVector2D& Point) const;

	ftcore::Settings Values;
	ftcore::SettingsMenu Menu;
	TArray<FString> RowLabels;
	TArray<FString> RowValues;
	TArray<FBox2D> RowRects;
	float ValueLeft = 0.f;
	float ValueMid = 0.f;
	FVector2D LastMouse = FVector2D(-1.0, -1.0);
	uint32 Revision = 0;
	uint64 OpenFrame = 0;
	uint64 CloseFrame = MAX_uint64;
	bool bOpen = false;
	bool bBeganMenu = false; // Open turned the controller's menu input on, so Close turns it off
	TWeakObjectPtr<AFTPlayerController> Controller;
};
