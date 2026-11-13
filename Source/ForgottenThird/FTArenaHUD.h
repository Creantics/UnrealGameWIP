#pragma once

// Canvas UI of the arena fight (no UMG), drawn from its UFTArenaRunner (docs/ARENA_DEMO.md):
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
//   - the first Arena boss (stone_bearer), under the target bar: its WEIGHT stacks as pips, its state (INVULNERABLE
//     with "R on the call" on its plinth, WEAK POINT OPEN, DOWN, or the CALL of a called blow with the zone it is in,
//     green when the lead stands in it) and the latest answer or rule (INTERCEPT, GUARD, WRONG SIDE, MISSED, BROKEN,
//     SHOCK RING, CRITICAL, LIFTED), fading;
//   - the result (VICTORY or DEFEAT with the counts) and the fight-again prompt, and the key hint.
// Drawing builds no strings: every word is a static FTUiText, and the numbers are rebuilt only when they change.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTArenaHUD.generated.h"

class UFTArenaRunner;
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

	void DrawPlayerPanel(const UFTArenaRunner& Mode);
	void DrawEnemies(const UFTArenaRunner& Mode);
	void DrawTargetBar(const UFTArenaRunner& Mode);
	void DrawBossPanel(const UFTArenaRunner& Mode);
	void DrawTell(const UFTArenaRunner& Mode, int32 Enemy, const FVector2D& Over, const FVector2D& Chest, bool bOnScreen);
	// The open Read deal's cards, centred on X with their bottom at Bottom.
	void DrawReadCards(const UFTArenaRunner& Mode, float X, float Bottom);
	// An arrow at the screen's edge toward Toward (screen space, + right and down). For an off-frame windup: Pulse 1..0
	// swells it and rings it at the windup's onset, Fill (0..1; below 0: none) fills a bar beside it toward the hit, and
	// Word (the answer) is written beside it, so the tell reads with the sound off.
	void DrawEdgeArrow(const FVector2D& Toward, const FLinearColor& Color, float Pulse = 0.f, float Fill = -1.f,
		const FString* Word = nullptr);
	void DrawLock(const UFTArenaRunner& Mode);
	void DrawPopups(const UFTArenaRunner& Mode);
	void DrawFlashes(const UFTArenaRunner& Mode);
	void DrawBanners(const UFTArenaRunner& Mode);
	void DrawResult(const UFTArenaRunner& Mode);
	// The air near the zone's wall, in the world's own look (rain haze, a pulsing blue grid, paper-edge fog), within
	// about 3 m of it and nowhere else: how the player learns where the invisible wall is.
	void DrawEdgeCue(const UFTArenaRunner& Mode);

	float UiScale = 1.f;

	FArenaNumberText HpText;
	FArenaNumberText AllyText;
	FArenaNumberText TargetText;
	FArenaNumberText LoadText;
	FArenaNumberText ReleaseText;
	// One per popup slot, keyed by the popup's number (GetPopupTotal() when it was made).
	FArenaNumberText PopupText[16];
	FString StatsText;
	bool bStatsBuilt = false;
};
