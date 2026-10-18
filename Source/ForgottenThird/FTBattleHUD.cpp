#include "FTBattleHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"

#include "FTBattleComponent.h"
#include "FTBattleDemoActor.h"
#include "FTConductorComponent.h"

#include "ftcore/Battle.h"

namespace
{
// Lane order Left, Down, Up, Right.
const FLinearColor kLaneColors[4] = {
	FLinearColor(0.78f, 0.35f, 0.95f), FLinearColor(0.25f, 0.75f, 1.0f), FLinearColor(0.35f, 0.95f, 0.45f), FLinearColor(1.0f, 0.35f, 0.35f)};
const FVector2D kLaneDirs[4] = {FVector2D(-1, 0), FVector2D(0, 1), FVector2D(0, -1), FVector2D(1, 0)};

const FLinearColor kGold(1.0f, 0.82f, 0.25f);
const FLinearColor kWhite(1.f, 1.f, 1.f);
const FLinearColor kDim(0.55f, 0.55f, 0.6f);
const FLinearColor kPanel(0.02f, 0.02f, 0.05f, 0.65f);
const FLinearColor kHpBack(0.15f, 0.05f, 0.05f, 0.9f);
const FLinearColor kHpParty(0.3f, 0.85f, 0.4f);
const FLinearColor kHpEnemy(0.9f, 0.3f, 0.25f);
const FLinearColor kAp(0.3f, 0.65f, 1.0f);
const FLinearColor kRed(1.0f, 0.3f, 0.3f);

// Static so drawing never builds strings.
const FString kNow(TEXT("NOW"));
const FString kNext(TEXT("NEXT"));
const FString kPerfect(TEXT("PERFECT"));
const FString kGreat(TEXT("GREAT"));
const FString kGood(TEXT("GOOD"));
const FString kMiss(TEXT("MISS"));
const FString kWrong(TEXT("WRONG"));
const FString kVictory(TEXT("VICTORY"));
const FString kDefeat(TEXT("DEFEAT"));
const FString kAgain(TEXT("Enter / Space / pad A to fight again"));
const FString kChoose(TEXT("choose a skill"));
const FString kAttack(TEXT("attack - hit the arrows!"));
const FString kDefend(TEXT("defend - hit the arrows!"));
const FString kWait(TEXT("..."));
const FString kMenuHint(TEXT("Up/Down or 1-4: skill    Left/Right, Q/E, Tab: target    Enter / Space / pad A: use"));
const FString kLaneHint(TEXT("Arrows / WASD / d-pad / X A Y B"));
const FString kDown(TEXT("DOWN"));

double HudWall() { return FPlatformTime::Seconds(); }

bool IsOver(const ftcore::Battle& B) { return B.phase() == ftcore::Phase::Victory || B.phase() == ftcore::Phase::Defeat; }

float Fade(double Age, double Hold, double Out)
{
	if (Age < 0.0)
	{
		return 0.f;
	}
	if (Age < Hold)
	{
		return 1.f;
	}
	return static_cast<float>(FMath::Clamp(1.0 - (Age - Hold) / Out, 0.0, 1.0));
}

FLinearColor WithAlpha(FLinearColor C, float A)
{
	C.A *= A;
	return C;
}
} // namespace

AFTBattleDemoActor* AFTBattleHUD::FindDemo()
{
	if (!CachedDemo.IsValid())
	{
		for (TActorIterator<AFTBattleDemoActor> It(GetWorld()); It; ++It)
		{
			CachedDemo = *It;
			break;
		}
	}
	return CachedDemo.Get();
}

void AFTBattleHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	AFTBattleDemoActor* Found = FindDemo();
	if (!Found || !Found->GetBattle() || !Found->GetBattle()->GetBattle())
	{
		return;
	}
	const AFTBattleDemoActor& D = *Found;
	UiScale = FMath::Max(0.5f, Canvas->ClipY / 1080.f);

	DrawPanels(D);
	DrawTurnOrder(D);
	DrawPhase(D);
	DrawTargetMarker(D);
	DrawSkillMenu(D);
	DrawLane(D);
	DrawPopups(D);
	DrawMessages(D);
}

// ---- helpers ----

void AFTBattleHUD::DrawStr(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale)
{
	DrawText(S, Color, X, Y, Font, Scale * UiScale);
}

void AFTBattleHUD::DrawStrCentered(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	GetTextSize(S, W, H, Font, Scale * UiScale);
	DrawText(S, Color, X - W * 0.5f, Y, Font, Scale * UiScale);
}

void AFTBattleHUD::DrawFrame(float X, float Y, float W, float H, const FLinearColor& Color, float Thickness)
{
	DrawRect(Color, X, Y, W, Thickness);
	DrawRect(Color, X, Y + H - Thickness, W, Thickness);
	DrawRect(Color, X, Y, Thickness, H);
	DrawRect(Color, X + W - Thickness, Y, Thickness, H);
}

void AFTBattleHUD::DrawArrow(int32 Lane, float X, float Y, float Radius, const FLinearColor& Color, float Thickness)
{
	const FVector2D Dir = kLaneDirs[Lane & 3];
	const FVector2D Perp(-Dir.Y, Dir.X);
	const FVector2D C(X, Y);
	const FVector2D Tip = C + Dir * Radius * 0.75;
	const FVector2D Tail = C - Dir * Radius * 0.75;
	const FVector2D WingA = C + Perp * Radius * 0.7;
	const FVector2D WingB = C - Perp * Radius * 0.7;
	DrawLine(Tail.X, Tail.Y, Tip.X, Tip.Y, Color, Thickness);
	DrawLine(WingA.X, WingA.Y, Tip.X, Tip.Y, Color, Thickness);
	DrawLine(WingB.X, WingB.Y, Tip.X, Tip.Y, Color, Thickness);
}

// ---- sections ----

void AFTBattleHUD::DrawTurnOrder(const AFTBattleDemoActor& D)
{
	UFont* Font = GEngine->GetSmallFont();
	const ftcore::Battle* B = D.GetBattle()->GetBattle();
	const float S = UiScale;
	float X = 24.f * S;
	const float Y = 24.f * S;
	const float BoxW = 120.f * S;
	const float BoxH = 46.f * S;

	if (!IsOver(*B))
	{
		const int32 Actor = static_cast<int32>(B->actor());
		const bool bParty = B->combatant(B->actor()).team == ftcore::Team::Party;
		DrawRect(kPanel, X, Y, BoxW, BoxH);
		DrawFrame(X, Y, BoxW, BoxH, kGold, 3.f * S);
		DrawStr(kNow, kGold, X + 8.f * S, Y + 4.f * S, Font, 1.f);
		DrawStr(D.GetFighterName(Actor), bParty ? kHpParty : kHpEnemy, X + 8.f * S, Y + 20.f * S, Font, 1.2f);
		X += BoxW + 16.f * S;
	}
	DrawStr(kNext, kDim, X, Y + 4.f * S, Font, 1.f);
	X += 44.f * S;
	const std::span<const ftcore::CombatantId> Order = D.GetBattle()->PeekTurnOrder(6);
	for (const ftcore::CombatantId Id : Order)
	{
		const bool bParty = B->combatant(Id).team == ftcore::Team::Party;
		const float W = 100.f * S;
		DrawRect(kPanel, X, Y + 8.f * S, W, BoxH - 16.f * S);
		DrawRect(bParty ? kHpParty : kHpEnemy, X, Y + 8.f * S, 4.f * S, BoxH - 16.f * S);
		DrawStr(D.GetFighterName(static_cast<int32>(Id)), kWhite, X + 10.f * S, Y + 14.f * S, Font, 1.f);
		X += W + 6.f * S;
	}
}

void AFTBattleHUD::DrawPhase(const AFTBattleDemoActor& D)
{
	const ftcore::Battle* B = D.GetBattle()->GetBattle();
	const FString* Label = nullptr;
	switch (B->phase())
	{
	case ftcore::Phase::AwaitingCommand: Label = &kChoose; break;
	case ftcore::Phase::PartySkill: Label = &kAttack; break;
	case ftcore::Phase::EnemySkill: Label = &kDefend; break;
	case ftcore::Phase::Recovery: Label = &kWait; break;
	default: break;
	}
	if (!Label)
	{
		return;
	}
	UFont* Font = GEngine->GetMediumFont();
	const float CX = Canvas->ClipX * 0.5f;
	const float Y = 86.f * UiScale;
	const FString& Name = D.GetFighterName(static_cast<int32>(B->actor()));
	float NW = 0.f, NH = 0.f, LW = 0.f, LH = 0.f;
	GetTextSize(Name, NW, NH, Font, UiScale);
	GetTextSize(*Label, LW, LH, Font, UiScale);
	const float Gap = 12.f * UiScale;
	const float X0 = CX - (NW + Gap + LW) * 0.5f;
	DrawRect(kPanel, X0 - 14.f * UiScale, Y - 6.f * UiScale, NW + Gap + LW + 28.f * UiScale, NH + 12.f * UiScale);
	DrawStr(Name, kGold, X0, Y, Font);
	DrawStr(*Label, kWhite, X0 + NW + Gap, Y, Font);
}

void AFTBattleHUD::DrawPanel(const AFTBattleDemoActor& D, int32 Id, float X, float Y, bool bActive)
{
	const ftcore::Battle* B = D.GetBattle()->GetBattle();
	const ftcore::Combatant& C = B->combatant(static_cast<ftcore::CombatantId>(Id));
	const bool bParty = C.team == ftcore::Team::Party;
	const float S = UiScale;
	const float W = 300.f * S;
	const float H = bParty ? 84.f * S : 66.f * S;
	UFont* Font = GEngine->GetSmallFont();

	DrawRect(kPanel, X, Y, W, H);
	if (bActive)
	{
		DrawFrame(X, Y, W, H, kGold, 3.f * S);
	}
	const float Flash = D.GetFlash(Id);
	const FLinearColor NameColor = !C.alive() ? kDim : FLinearColor::LerpUsingHSV(kWhite, kRed, Flash);
	DrawStr(D.GetFighterName(Id), NameColor, X + 12.f * S, Y + 8.f * S, Font, 1.3f);
	if (!C.alive())
	{
		DrawStr(kDown, kRed, X + W - 70.f * S, Y + 8.f * S, Font, 1.3f);
	}

	const float BarX = X + 12.f * S;
	const float BarY = Y + 34.f * S;
	const float BarW = W - 24.f * S;
	const float BarH = 14.f * S;
	const float Frac = C.stats.maxHp > 0 ? FMath::Clamp(static_cast<float>(C.hp) / static_cast<float>(C.stats.maxHp), 0.f, 1.f) : 0.f;
	DrawRect(kHpBack, BarX, BarY, BarW, BarH);
	DrawRect(bParty ? kHpParty : kHpEnemy, BarX, BarY, BarW * Frac, BarH);
	if (Flash > 0.f)
	{
		DrawRect(WithAlpha(kWhite, Flash * 0.6f), BarX, BarY, BarW * Frac, BarH);
	}
	DrawStr(D.GetHpText(Id), kWhite, BarX + 4.f * S, BarY + BarH + 2.f * S, Font, 1.f);

	if (bParty)
	{
		// AP pips.
		const float Pip = 10.f * S;
		float PX = X + W - 12.f * S - static_cast<float>(C.maxAp) * (Pip + 4.f * S);
		for (int32 I = 0; I < C.maxAp; ++I)
		{
			DrawRect(I < C.ap ? kAp : kHpBack, PX, BarY + BarH + 8.f * S, Pip, Pip);
			PX += Pip + 4.f * S;
		}
	}
}

void AFTBattleHUD::DrawPanels(const AFTBattleDemoActor& D)
{
	const ftcore::Battle* B = D.GetBattle()->GetBattle();
	const float S = UiScale;
	const int32 Actor = IsOver(*B) ? -1 : static_cast<int32>(B->actor());
	const int32 Count = static_cast<int32>(B->combatants().size());
	float PartyY = Canvas->ClipY - 24.f * S;
	int32 EnemyIndex = 0;
	// Party bottom-left, stacked upward; enemies top-right, stacked downward.
	for (int32 Id = Count - 1; Id >= 0; --Id)
	{
		if (B->combatant(static_cast<ftcore::CombatantId>(Id)).team == ftcore::Team::Party)
		{
			PartyY -= 92.f * S;
			DrawPanel(D, Id, 24.f * S, PartyY, Id == Actor);
		}
	}
	for (int32 Id = 0; Id < Count; ++Id)
	{
		if (B->combatant(static_cast<ftcore::CombatantId>(Id)).team == ftcore::Team::Enemies)
		{
			DrawPanel(D, Id, Canvas->ClipX - 324.f * S, 24.f * S + static_cast<float>(EnemyIndex) * 74.f * S, Id == Actor);
			++EnemyIndex;
		}
	}
}

void AFTBattleHUD::DrawTargetMarker(const AFTBattleDemoActor& D)
{
	const ftcore::Battle* B = D.GetBattle()->GetBattle();
	if (B->phase() != ftcore::Phase::AwaitingCommand || D.IsWaitingForRestart())
	{
		return;
	}
	const int32 Target = D.GetSelectedTarget();
	if (Target < 0)
	{
		return;
	}
	const FVector P = Project(D.GetFigureTop(Target), true);
	if (P.Z <= 0.0)
	{
		return; // behind the camera
	}
	const float S = UiScale;
	const float Bob = 6.f * S * FMath::Sin(static_cast<float>(HudWall()) * 6.f);
	const float X = static_cast<float>(P.X);
	const float Y = static_cast<float>(P.Y) - 20.f * S + Bob;
	const float R = 16.f * S;
	DrawLine(X - R, Y - R, X, Y, kGold, 5.f * S);
	DrawLine(X + R, Y - R, X, Y, kGold, 5.f * S);
	DrawLine(X - R, Y - R - 10.f * S, X, Y - 10.f * S, kGold, 3.f * S);
	DrawLine(X + R, Y - R - 10.f * S, X, Y - 10.f * S, kGold, 3.f * S);
}

void AFTBattleHUD::DrawSkillMenu(const AFTBattleDemoActor& D)
{
	const ftcore::Battle* B = D.GetBattle()->GetBattle();
	if (B->phase() != ftcore::Phase::AwaitingCommand || D.IsWaitingForRestart())
	{
		return;
	}
	const int32 Actor = static_cast<int32>(B->actor());
	const ftcore::Combatant& C = B->combatant(B->actor());
	if (C.team != ftcore::Team::Party)
	{
		return;
	}
	UFont* Font = GEngine->GetMediumFont();
	UFont* Small = GEngine->GetSmallFont();
	const float S = UiScale;
	const int32 Count = static_cast<int32>(C.skills.size());
	const float RowH = 40.f * S;
	const float W = 420.f * S;
	const float H = RowH * static_cast<float>(Count) + 52.f * S;
	const float X = Canvas->ClipX * 0.5f - W * 0.5f;
	const float Y = Canvas->ClipY - H - 28.f * S;
	DrawRect(kPanel, X, Y, W, H);
	DrawFrame(X, Y, W, H, WithAlpha(kGold, 0.6f), 2.f * S);
	for (int32 I = 0; I < Count; ++I)
	{
		const ftcore::Skill& Skill = C.skills[static_cast<size_t>(I)];
		const bool bAffordable = C.ap >= Skill.apCost;
		const bool bSelected = I == D.GetSelectedSkill();
		const float RY = Y + 10.f * S + RowH * static_cast<float>(I);
		if (bSelected)
		{
			DrawRect(WithAlpha(kGold, 0.25f), X + 6.f * S, RY, W - 12.f * S, RowH - 4.f * S);
			DrawRect(kGold, X + 6.f * S, RY, 4.f * S, RowH - 4.f * S);
		}
		const FLinearColor Color = !bAffordable ? kDim : bSelected ? kGold : kWhite;
		DrawStr(D.GetSkillLabel(Actor, I), Color, X + 20.f * S, RY + 6.f * S, Font);
		const FString& Cost = D.GetSkillCost(Actor, I);
		float CW = 0.f, CH = 0.f;
		GetTextSize(Cost, CW, CH, Font, S);
		DrawStr(Cost, bAffordable ? kAp : kRed, X + W - 20.f * S - CW, RY + 6.f * S, Font);
	}
	DrawStrCentered(kMenuHint, kDim, Canvas->ClipX * 0.5f, Y + H - 30.f * S, Small, 1.f);
}

void AFTBattleHUD::DrawLane(const AFTBattleDemoActor& D)
{
	UFTBattleComponent* BC = D.GetBattle();
	const ftcore::Battle* B = BC->GetBattle();
	if (!BC->IsComboActive())
	{
		return;
	}
	const float S = UiScale;
	LaneWidth = 76.f * S;
	LaneX0 = Canvas->ClipX * 0.5f - LaneWidth * 2.f;
	HitY = Canvas->ClipY * 0.22f;
	const float Bottom = Canvas->ClipY * 0.86f;
	const double Now = BC->GetBattleTime();
	const bool bDefend = B->phase() == ftcore::Phase::EnemySkill;

	// Lane background, with a beat pulse on the hit line.
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.45f), LaneX0, HitY - 50.f * S, LaneWidth * 4.f, Bottom - HitY + 50.f * S);
	double Beat = 0.0;
	if (UFTConductorComponent* Conductor = D.GetConductor())
	{
		Beat = Conductor->GetBeat();
	}
	const float Pulse = static_cast<float>(1.0 - (Beat - FMath::FloorToDouble(Beat)));
	DrawRect(WithAlpha(bDefend ? kRed : kGold, 0.35f + 0.5f * Pulse * Pulse), LaneX0, HitY - 2.f * S, LaneWidth * 4.f, 4.f * S);

	const float R = LaneWidth * 0.36f;
	for (int32 Lane = 0; Lane < 4; ++Lane)
	{
		const float CX = LaneX0 + LaneWidth * (static_cast<float>(Lane) + 0.5f);
		// Receptor: grey outline, lit briefly after a press.
		const float Lit = static_cast<float>(FMath::Clamp(1.0 - D.GetLanePressAge(Lane) / 0.15, 0.0, 1.0));
		const FLinearColor Receptor = FLinearColor::LerpUsingHSV(FLinearColor(0.45f, 0.45f, 0.5f), kWhite, Lit);
		DrawArrow(Lane, CX, HitY, R * (1.f + 0.15f * Lit), Receptor, 3.f * S);
	}

	const ftcore::ComboSequence& Combo = B->combo();
	const float Span = Bottom - HitY;
	for (size_t I = Combo.nextIndex(); I < Combo.size(); ++I)
	{
		if (Combo.grade(I) != ftcore::Grade::Pending)
		{
			continue;
		}
		const ftcore::ComboNote& Note = Combo.note(I);
		const double Dt = Note.time - Now;
		if (Dt > Lookahead)
		{
			break; // notes are in time order
		}
		const float Y = HitY + static_cast<float>(Dt / Lookahead) * Span;
		const int32 Lane = static_cast<int32>(Note.arrow) & 3;
		const float CX = LaneX0 + LaneWidth * (static_cast<float>(Lane) + 0.5f);
		const FLinearColor Color = kLaneColors[Lane];
		DrawRect(WithAlpha(Color, 0.3f), CX - R, Y - R, R * 2.f, R * 2.f);
		DrawArrow(Lane, CX, Y, R, Color, 6.f * S);
	}

	UFont* Small = GEngine->GetSmallFont();
	DrawStrCentered(kLaneHint, kDim, Canvas->ClipX * 0.5f, Bottom + 8.f * S, Small, 1.f);
}

void AFTBattleHUD::DrawPopups(const AFTBattleDemoActor& D)
{
	UFont* Font = GEngine->GetMediumFont();
	UFont* Large = GEngine->GetLargeFont();
	const double Now = HudWall();
	const float S = UiScale;
	// Grade popups use the last lane geometry (they can outlive the combo by a moment).
	if (LaneWidth <= 0.f)
	{
		LaneWidth = 76.f * S;
		LaneX0 = Canvas->ClipX * 0.5f - LaneWidth * 2.f;
		HitY = Canvas->ClipY * 0.22f;
	}
	for (int32 I = 0; I < AFTBattleDemoActor::MaxPopups; ++I)
	{
		const AFTBattleDemoActor::FPopup& P = D.GetPopup(I);
		if (P.WallStart < 0.0)
		{
			continue;
		}
		const double Age = Now - P.WallStart;
		if (P.bNumber)
		{
			const float A = Fade(Age, 0.6, 0.5);
			if (A <= 0.f)
			{
				continue;
			}
			const FVector W = Project(D.GetFigureTop(P.Combatant), true);
			if (W.Z <= 0.0)
			{
				continue;
			}
			const float Rise = static_cast<float>(Age) * 60.f * S;
			const FLinearColor Color = P.bBlock ? kAp : kWhite;
			DrawStrCentered(P.Text, WithAlpha(FLinearColor(0.f, 0.f, 0.f), A), static_cast<float>(W.X) + 2.f * S,
				static_cast<float>(W.Y) - 40.f * S - Rise + 2.f * S, Large, 1.f);
			DrawStrCentered(P.Text, WithAlpha(Color, A), static_cast<float>(W.X), static_cast<float>(W.Y) - 40.f * S - Rise, Large, 1.f);
		}
		else
		{
			const float A = Fade(Age, 0.25, 0.35);
			if (A <= 0.f)
			{
				continue;
			}
			const FString* Label = &kMiss;
			FLinearColor Color = kRed;
			if (P.bWrong)
			{
				Label = &kWrong;
			}
			else
			{
				switch (P.Grade)
				{
				case EFTGrade::Perfect: Label = &kPerfect; Color = kGold; break;
				case EFTGrade::Great: Label = &kGreat; Color = FLinearColor(0.4f, 1.f, 0.45f); break;
				case EFTGrade::Good: Label = &kGood; Color = FLinearColor(0.35f, 0.85f, 1.f); break;
				default: break;
				}
			}
			const float CX = LaneX0 + LaneWidth * (static_cast<float>(P.Lane & 3) + 0.5f);
			const float Y = HitY - 44.f * S - static_cast<float>(Age) * 40.f * S;
			DrawStrCentered(*Label, WithAlpha(Color, A), CX, Y, Font, 1.f);
		}
	}
}

void AFTBattleHUD::DrawMessages(const AFTBattleDemoActor& D)
{
	const ftcore::Battle* B = D.GetBattle()->GetBattle();
	UFont* Large = GEngine->GetLargeFont();
	UFont* Font = GEngine->GetMediumFont();
	const float S = UiScale;
	const float CX = Canvas->ClipX * 0.5f;

	if (!D.GetBanner().IsEmpty())
	{
		const float A = Fade(D.GetBannerAge(), 1.4, 0.4);
		if (A > 0.f)
		{
			DrawStrCentered(D.GetBanner(), WithAlpha(kWhite, A), CX, 130.f * S, Font, 1.2f);
		}
	}
	if (!D.GetNotice().IsEmpty())
	{
		const float A = Fade(D.GetNoticeAge(), 1.0, 0.4);
		if (A > 0.f)
		{
			DrawStrCentered(D.GetNotice(), WithAlpha(kRed, A), CX, Canvas->ClipY * 0.55f, Font, 1.2f);
		}
	}
	if (IsOver(*B))
	{
		const bool bWon = B->phase() == ftcore::Phase::Victory;
		const float Y = Canvas->ClipY * 0.36f;
		DrawRect(kPanel, 0.f, Y - 20.f * S, Canvas->ClipX, 150.f * S);
		DrawStrCentered(bWon ? kVictory : kDefeat, bWon ? kGold : kRed, CX, Y, Large, 3.f);
		if (D.IsWaitingForRestart())
		{
			DrawStrCentered(kAgain, kWhite, CX, Y + 90.f * S, Font, 1.f);
		}
	}
}
