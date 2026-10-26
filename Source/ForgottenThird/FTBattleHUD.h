#pragma once

// Canvas UI of the battle demo (no UMG): turn order, phase text, HP and AP panels, the skill menu,
// the target marker, the note lane with its hit line, the enemy attack's tells (a ring closing on
// the defender), telegraphs, defence keys and the judgement line, the aim reticle with the target's
// weak points and the shot timer, grade, damage and heal popups, an enemy's phase banner with the
// next skill of each enemy in the turn order, and the outcome with the results.
// Everything comes from the level's AFTBattleDemoActor; per-frame drawing uses cached or static
// strings only.

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "FTBattleHUD.generated.h"

class AFTBattleDemoActor;
class UFont;

UCLASS()
class FORGOTTENTHIRD_API AFTBattleHUD : public AHUD
{
	GENERATED_BODY()

public:
	// Seconds of chart visible below the hit line.
	UPROPERTY(EditAnywhere, Category = "Battle HUD")
	double Lookahead = 1.6;

	virtual void DrawHUD() override;

private:
	AFTBattleDemoActor* FindDemo();

	void DrawTurnOrder(const AFTBattleDemoActor& Demo);
	void DrawPhase(const AFTBattleDemoActor& Demo);
	void DrawPanels(const AFTBattleDemoActor& Demo);
	void DrawPanel(const AFTBattleDemoActor& Demo, int32 Id, float X, float Y, bool bActive);
	void DrawTargetMarker(const AFTBattleDemoActor& Demo);
	void DrawSkillMenu(const AFTBattleDemoActor& Demo);
	void DrawLane(const AFTBattleDemoActor& Demo);
	void DrawDefence(const AFTBattleDemoActor& Demo);
	void DrawJudgement(const AFTBattleDemoActor& Demo);
	void DrawAim(const AFTBattleDemoActor& Demo);
	void DrawPopups(const AFTBattleDemoActor& Demo);
	void DrawMessages(const AFTBattleDemoActor& Demo);

	// Text helpers (Scale is on top of the resolution scale).
	void DrawStr(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale = 1.f);
	void DrawStrCentered(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale = 1.f);
	void DrawArrow(int32 Lane, float X, float Y, float Radius, const FLinearColor& Color, float Thickness);
	void DrawFrame(float X, float Y, float W, float H, const FLinearColor& Color, float Thickness);
	void DrawRing(float X, float Y, float Radius, const FLinearColor& Color, float Thickness);

	TWeakObjectPtr<AFTBattleDemoActor> CachedDemo;
	float UiScale = 1.f;
	// Lane geometry of this frame.
	float LaneX0 = 0.f;
	float LaneWidth = 0.f;
	float HitY = 0.f;
};
