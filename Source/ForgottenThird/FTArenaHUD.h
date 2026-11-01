#pragma once

// Canvas UI of the arena demo (no UMG), drawn from AFTArenaGameMode (docs/ARENA_DEMO.md):
//   - the player's panel, bottom left: hp, stamina, the Load gauge (a mark at a third, where the Release opens;
//     RELEASE READY above it, and OVERLOADED with the locked verbs at the top), poise and the heal charges left;
//   - over each living enemy its hp and poise, and the target's (the locked enemy's, else the nearest's) as a
//     wide bar at the top of the screen;
//   - an enemy's tell: while a move starts up, a bar over the enemy fills toward the hit, labelled with the
//     answer (PARRY for a parryable move, BLOCK for one that can only be blocked, DODGE! in red for an
//     unblockable one) and a ring closes in on its body; the moment the move goes active the bar, the ring and
//     a burst flash white. An enemy winding up off the screen shows as an arrow at the edge, in the tell's colour;
//   - the Read: while a move's cards are dealt, three cards on its tell bar (bottom centre while the enemy is
//     off the screen), each with its key (1 2 3; the pad's d-pad left, up, right) and the shape it names; the
//     picked card turns green when right and red when wrong. The Read stacks are pips in the panel;
//   - the lock-on marker: brackets around the locked enemy;
//   - popups (damage, BLOCKED, PARRIED, EVADED, STAGGERED, GUARD BROKEN, heals, READ, MISREAD, READ COUNTER)
//     over the body they happened to,
//     a red edge when the player is hit and a gold one on a parry;
//   - banners: RELEASE with the Load it spent (and an orange edge), and OVERLOADED;
//   - the result (VICTORY or DEFEAT with the counts) and the fight-again prompt, and the key hint.
// Drawing builds no strings: every word is a static FTUiText, and the numbers are rebuilt only when they change.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTArenaHUD.generated.h"

class AFTArenaGameMode;
class UFont;

// A number as text, rebuilt only when the number changes.
struct FArenaNumberText
{
	int32 Value = MIN_int32;
	int32 Second = MIN_int32;
	FString Text;
};

UCLASS()
class FORGOTTENTHIRD_API AFTArenaHUD : public AHUD
{
	GENERATED_BODY()

public:
	AFTArenaHUD();

	virtual void DrawHUD() override;

private:
	void DrawStrCentered(const FString& Text, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale);
	void DrawRing(float X, float Y, float Radius, const FLinearColor& Color, float Thickness);
	void DrawFrame(float X, float Y, float W, float H, const FLinearColor& Color, float Thickness);
	void DrawBar(float X, float Y, float W, float H, float Fraction, const FLinearColor& Fill, const FLinearColor& Back);
	// A world point on the screen; false when it is behind the camera.
	bool ToScreen(const FVector& World, FVector2D& Out);
	bool OnScreen(const FVector2D& P, float Margin) const;

	void DrawPlayerPanel(const AFTArenaGameMode& Mode);
	void DrawEnemies(const AFTArenaGameMode& Mode);
	void DrawTargetBar(const AFTArenaGameMode& Mode);
	void DrawTell(const AFTArenaGameMode& Mode, int32 Enemy, const FVector2D& Over, const FVector2D& Chest, bool bOnScreen);
	// The open Read deal's cards, centred on X with their bottom at Bottom.
	void DrawReadCards(const AFTArenaGameMode& Mode, float X, float Bottom);
	void DrawEdgeArrow(const FVector2D& Toward, const FLinearColor& Color);
	void DrawLock(const AFTArenaGameMode& Mode);
	void DrawPopups(const AFTArenaGameMode& Mode);
	void DrawFlashes(const AFTArenaGameMode& Mode);
	void DrawBanners(const AFTArenaGameMode& Mode);
	void DrawResult(const AFTArenaGameMode& Mode);

	float UiScale = 1.f;

	FArenaNumberText HpText;
	FArenaNumberText TargetText;
	FArenaNumberText LoadText;
	FArenaNumberText ReleaseText;
	// One per popup slot, keyed by the popup's number (GetPopupTotal() when it was made).
	FArenaNumberText PopupText[16];
	FString StatsText;
	bool bStatsBuilt = false;
};
