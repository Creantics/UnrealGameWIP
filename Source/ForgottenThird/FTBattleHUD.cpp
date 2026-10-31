#include "FTBattleHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "EngineUtils.h"

#include "FTBattleComponent.h"
#include "FTBattleDemoActor.h"
#include "FTConductorComponent.h"
#include "FTPauseOverlay.h"
#include "FTPauseSubsystem.h"
#include "FTUiText.h"

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
const FLinearColor kOrange(1.0f, 0.55f, 0.15f);

// Static so drawing never builds strings.
const FTUiText kNow(TEXT("battle.now"));
const FTUiText kNext(TEXT("battle.next"));
const FTUiText kPerfect(TEXT("common.perfect"));
const FTUiText kGreat(TEXT("battle.great"));
const FTUiText kGood(TEXT("common.good"));
const FTUiText kMiss(TEXT("common.miss"));
const FTUiText kWrong(TEXT("battle.wrong"));
const FTUiText kVictory(TEXT("battle.victory"));
const FTUiText kDefeat(TEXT("battle.defeat"));
const FTUiText kAgain(TEXT("battle.again"));
const FTUiText kRetry(TEXT("battle.retry"));
const FTUiText kQuitToTitle(TEXT("battle.quit_to_title"));
const FTUiText kChoose(TEXT("battle.choose"));
const FTUiText kAttack(TEXT("battle.attack"));
const FTUiText kDefend(TEXT("battle.defend"));
const FTUiText kAimPhase(TEXT("battle.aim"));
const FTUiText kAimHint(TEXT("battle.aim_hint"));
const FString kWait(TEXT("..."));
const FTUiText kMenuHint(TEXT("battle.menu_hint"));
const FTUiText kLaneHint(TEXT("common.lanes_hint"));
const FTUiText kDown(TEXT("battle.down"));
const FString kDefenceKeys[3] = {FString(TEXT("Dodge  Left/Right  X/B")), FString(TEXT("Parry  Down  A")),
	FString(TEXT("Jump  Up  Y"))};
const FString kTell(TEXT("!"));
const FTUiText kJumpable(TEXT("battle.jump"));
const FLinearColor kCyan(0.35f, 0.85f, 1.f);

// The same pause-aware clock AFTBattleDemoActor stamps its popup times with (Wall() there): the popups
// are aged against those, so the two must agree.
double HudWall() { return UFTPauseSubsystem::WallSeconds(); }

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
		FT::DrawPauseOverlay(*this, Canvas);
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
	DrawDefence(D);
	DrawJudgement(D);
	DrawAim(D);
	DrawPopups(D);
	DrawMessages(D);
	FT::DrawPauseOverlay(*this, Canvas);
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

void AFTBattleHUD::DrawRing(float X, float Y, float Radius, const FLinearColor& Color, float Thickness)
{
	constexpr int32 Segments = 32;
	float PX = X + Radius;
	float PY = Y;
	for (int32 I = 1; I <= Segments; ++I)
	{
		const float A = 2.f * PI * static_cast<float>(I) / static_cast<float>(Segments);
		const float NX = X + Radius * FMath::Cos(A);
		const float NY = Y + Radius * FMath::Sin(A);
		DrawLine(PX, PY, NX, NY, Color, Thickness);
		PX = NX;
		PY = NY;
	}
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
	uint64 Named = 0; // enemies whose next skill is already shown (under their soonest turn)
	for (const ftcore::CombatantId Id : Order)
	{
		const bool bParty = B->combatant(Id).team == ftcore::Team::Party;
		const float W = 100.f * S;
		DrawRect(kPanel, X, Y + 8.f * S, W, BoxH - 16.f * S);
		DrawRect(bParty ? kHpParty : kHpEnemy, X, Y + 8.f * S, 4.f * S, BoxH - 16.f * S);
		DrawStr(D.GetFighterName(static_cast<int32>(Id)), kWhite, X + 10.f * S, Y + 14.f * S, Font, 1.f);
		const uint64 Bit = Id < 64 ? (uint64(1) << Id) : 0;
		if (!bParty && Bit && !(Named & Bit))
		{
			Named |= Bit;
			const FString& Skill = D.GetNextSkillName(static_cast<int32>(Id));
			if (!Skill.IsEmpty())
			{
				DrawStr(Skill, kOrange, X + 10.f * S, Y + BoxH - 4.f * S, Font, 0.9f);
			}
		}
		X += W + 6.f * S;
	}
}

void AFTBattleHUD::DrawPhase(const AFTBattleDemoActor& D)
{
	const ftcore::Battle* B = D.GetBattle()->GetBattle();
	const FString* Label = nullptr;
	switch (B->phase())
	{
	case ftcore::Phase::AwaitingCommand: Label = &kChoose.Get(); break;
	case ftcore::Phase::PartySkill: Label = &kAttack.Get(); break;
	case ftcore::Phase::EnemySkill: Label = &kDefend.Get(); break;
	case ftcore::Phase::PartyAim: Label = &kAimPhase.Get(); break;
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
	// The skills, then the Aim row.
	const float H = RowH * static_cast<float>(Count + 1) + 52.f * S;
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
	{
		const bool bAffordable = C.ap >= D.GetBattle()->Tuning.AimApCost;
		const bool bSelected = D.GetSelectedSkill() == Count;
		const float RY = Y + 10.f * S + RowH * static_cast<float>(Count);
		if (bSelected)
		{
			DrawRect(WithAlpha(kGold, 0.25f), X + 6.f * S, RY, W - 12.f * S, RowH - 4.f * S);
			DrawRect(kGold, X + 6.f * S, RY, 4.f * S, RowH - 4.f * S);
		}
		DrawStr(D.GetAimLabel(), !bAffordable ? kDim : bSelected ? kGold : kWhite, X + 20.f * S, RY + 6.f * S, Font);
		float CW = 0.f, CH = 0.f;
		GetTextSize(D.GetAimCost(), CW, CH, Font, S);
		DrawStr(D.GetAimCost(), bAffordable ? kAp : kRed, X + W - 20.f * S - CW, RY + 6.f * S, Font);
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

	// Lane background, with a beat pulse on the hit line.
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.45f), LaneX0, HitY - 50.f * S, LaneWidth * 4.f, Bottom - HitY + 50.f * S);
	double Beat = 0.0;
	if (UFTConductorComponent* Conductor = D.GetConductor())
	{
		Beat = Conductor->GetBeat();
	}
	const float Pulse = static_cast<float>(1.0 - (Beat - FMath::FloorToDouble(Beat)));
	DrawRect(WithAlpha(kGold, 0.35f + 0.5f * Pulse * Pulse), LaneX0, HitY - 2.f * S, LaneWidth * 4.f, 4.f * S);

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

void AFTBattleHUD::DrawDefence(const AFTBattleDemoActor& D)
{
	UFTBattleComponent* BC = D.GetBattle();
	const ftcore::Battle* B = BC->GetBattle();
	if (!BC->IsDefenceActive())
	{
		return;
	}
	const ftcore::DefenceTimeline& T = B->defence();
	const double Now = BC->GetBattleTime();
	const float S = UiScale;
	UFont* Font = GEngine->GetMediumFont();
	UFont* Small = GEngine->GetSmallFont();

	// Over the defender: the telegraph column fills toward the impact, the tell is a ring that
	// closes in on the figure and lands on it at the impact (the attacker flashes as it starts).
	const FVector P = Project(D.GetFigureTop(static_cast<int32>(B->defender())), true);
	if (P.Z > 0.0)
	{
		const float X = static_cast<float>(P.X);
		const float Y = static_cast<float>(P.Y) - 30.f * S;
		for (size_t I = T.nextIndex(); I < T.size(); ++I)
		{
			const ftcore::TimelineHit& Hit = T.hit(I);
			const double Left = T.impact(I) - Now;
			if (T.telegraphActive(I, Now))
			{
				const float Fill = static_cast<float>(FMath::Clamp(1.0 - Left / Hit.telegraph, 0.0, 1.0));
				const float ColH = 90.f * S;
				const float ColW = 10.f * S;
				const float CX = X + 46.f * S;
				DrawRect(kHpBack, CX, Y - ColH, ColW, ColH);
				DrawRect(WithAlpha(kRed, 0.5f + 0.5f * Fill), CX, Y - ColH * Fill, ColW, ColH * Fill);
			}
			if (T.tellActive(I, Now))
			{
				const float K = static_cast<float>(FMath::Clamp(Left / Hit.tell, 0.0, 1.0));
				const float Radius = (26.f + 80.f * K) * S;
				const FLinearColor Color = Hit.jumpable ? kGold : kRed;
				DrawRing(X, Y, 26.f * S, WithAlpha(kWhite, 0.35f), 2.f * S); // where the ring lands
				DrawRing(X, Y, Radius, WithAlpha(Color, 1.f - 0.6f * K), 4.f * S);
				DrawStrCentered(Hit.jumpable ? kJumpable : kTell, Color, X, Y - 70.f * S, Font, 1.4f);
			}
		}
	}

	// One pip per hit of the attack: grey pending, gold perfect, cyan dodged or jumped, red hit,
	// orange hit with a late press.
	const float Pip = 14.f * S;
	const float Gap = 6.f * S;
	const int32 Count = static_cast<int32>(T.size());
	float PX = Canvas->ClipX * 0.5f - (static_cast<float>(Count) * (Pip + Gap) - Gap) * 0.5f;
	const float PY = Canvas->ClipY * 0.74f;
	for (int32 I = 0; I < Count; ++I)
	{
		FLinearColor Color = kDim;
		switch (T.outcome(static_cast<size_t>(I)))
		{
		case ftcore::DefenceOutcome::Perfect: Color = kGold; break;
		case ftcore::DefenceOutcome::Dodged:
		case ftcore::DefenceOutcome::Jumped: Color = T.refunded(static_cast<size_t>(I)) ? kGold : kCyan; break;
		case ftcore::DefenceOutcome::Hit: Color = T.late(static_cast<size_t>(I)) ? kOrange : kRed; break;
		default: break;
		}
		DrawRect(Color, PX, PY, Pip, Pip);
		PX += Pip + Gap;
	}

	// The keys, the last one pressed lit for a moment.
	const float Lit = static_cast<float>(FMath::Clamp(1.0 - D.GetDefencePressAge() / 0.2, 0.0, 1.0));
	const int32 Pressed = static_cast<int32>(D.GetLastDefenceInput());
	const float KeyW = 230.f * S;
	float KX = Canvas->ClipX * 0.5f - KeyW * 1.5f;
	const float KY = PY + Pip + 14.f * S;
	for (int32 K = 0; K < 3; ++K)
	{
		const bool bLit = K == Pressed && Lit > 0.f;
		DrawRect(kPanel, KX + 4.f * S, KY, KeyW - 8.f * S, 30.f * S);
		if (bLit)
		{
			DrawFrame(KX + 4.f * S, KY, KeyW - 8.f * S, 30.f * S, WithAlpha(kGold, Lit), 2.f * S);
		}
		DrawStrCentered(kDefenceKeys[K], bLit ? kGold : kWhite, KX + KeyW * 0.5f, KY + 7.f * S, Small, 1.f);
		KX += KeyW;
	}
}

// The last enemy hit's judgement, on one line above the pips (it outlasts the attack a moment).
void AFTBattleHUD::DrawJudgement(const AFTBattleDemoActor& D)
{
	if (D.GetJudgement() == EFTJudgement::None)
	{
		return;
	}
	const float A = Fade(D.GetJudgementAge(), 0.5, 0.4);
	if (A <= 0.f)
	{
		return;
	}
	FLinearColor Color = kRed;
	switch (D.GetJudgement())
	{
	case EFTJudgement::Perfect: Color = kGold; break;
	case EFTJudgement::Defended: Color = kCyan; break;
	case EFTJudgement::Late: Color = kOrange; break;
	default: break;
	}
	const float S = UiScale;
	const float Pop = 1.f + 0.25f * static_cast<float>(FMath::Clamp(1.0 - D.GetJudgementAge() / 0.12, 0.0, 1.0));
	DrawStrCentered(D.GetJudgementText(), WithAlpha(Color, A), Canvas->ClipX * 0.5f, Canvas->ClipY * 0.74f - 48.f * S,
		GEngine->GetLargeFont(), Pop);
}

// Aim: the target's weak points (rings with their multipliers), the reticle where a shot now
// would land (gold over a weak point), the time left and the controls.
void AFTBattleHUD::DrawAim(const AFTBattleDemoActor& D)
{
	UFTBattleComponent* BC = D.GetBattle();
	const ftcore::Battle* B = BC->GetBattle();
	if (B->phase() != ftcore::Phase::PartyAim)
	{
		return;
	}
	const ftcore::AimShot& Aim = B->aimShot();
	const int32 Target = static_cast<int32>(B->aimTarget());
	const double Now = BC->GetBattleTime();
	const float S = UiScale;
	UFont* Font = GEngine->GetMediumFont();
	UFont* Small = GEngine->GetSmallFont();

	// Pixels per figure height at the target, for the ring sizes.
	const FVector Feet = Project(D.GetFigurePoint(Target, 0.f, 0.f), true);
	const FVector Top = Project(D.GetFigurePoint(Target, 0.f, 1.f), true);
	if (Feet.Z <= 0.0 || Top.Z <= 0.0)
	{
		return;
	}
	const float FigurePx = FMath::Max(static_cast<float>(FVector::Dist2D(Feet, Top)), 1.f);

	const ftcore::ShotResult Shot = Aim.judge(Now);
	const float Pulse = 0.5f + 0.5f * FMath::Sin(static_cast<float>(HudWall()) * 7.f);
	const ftcore::WeakPoints& Points = Aim.weakPoints();
	for (size_t I = 0; I < Points.size(); ++I)
	{
		const ftcore::WeakPoint& P = Points[I];
		const FVector At = Project(D.GetFigurePoint(Target, P.x, P.y), true);
		if (At.Z <= 0.0)
		{
			continue;
		}
		const bool bOn = Shot.kind == ftcore::ShotKind::Weak && Shot.weakPoint == I;
		const float R = P.radius * FigurePx;
		const FLinearColor Color = bOn ? kGold : WithAlpha(kOrange, 0.55f + 0.45f * Pulse);
		DrawRing(static_cast<float>(At.X), static_cast<float>(At.Y), R, Color, (bOn ? 4.f : 2.5f) * S);
		DrawRing(static_cast<float>(At.X), static_cast<float>(At.Y), R * 0.3f, Color, 2.f * S);
		DrawStrCentered(D.GetWeakPointLabel(static_cast<int32>(I)), Color, static_cast<float>(At.X),
			static_cast<float>(At.Y) - R - 26.f * S, Font, 1.f);
	}

	// The reticle: a ring with a cross, at the aim point (the reticle plus its sway).
	const ftcore::AimPoint AimPt = Aim.aimPoint(Now);
	const FVector Ret = Project(D.GetFigurePoint(Target, AimPt.x, AimPt.y), true);
	if (Ret.Z > 0.0)
	{
		const float X = static_cast<float>(Ret.X);
		const float Y = static_cast<float>(Ret.Y);
		const FLinearColor Color = Shot.kind == ftcore::ShotKind::Weak ? kGold : Shot.kind == ftcore::ShotKind::Body ? kWhite : kDim;
		const float Rad = 18.f * S;
		DrawRing(X, Y, Rad, Color, 2.5f * S);
		DrawLine(X - Rad * 1.6f, Y, X - Rad * 0.5f, Y, Color, 2.f * S);
		DrawLine(X + Rad * 0.5f, Y, X + Rad * 1.6f, Y, Color, 2.f * S);
		DrawLine(X, Y - Rad * 1.6f, X, Y - Rad * 0.5f, Color, 2.f * S);
		DrawLine(X, Y + Rad * 0.5f, X, Y + Rad * 1.6f, Color, 2.f * S);
		DrawRect(Color, X - 2.f * S, Y - 2.f * S, 4.f * S, 4.f * S);
	}

	// The time left before the shot fires on its own.
	const double Limit = Aim.tuning().timeLimit;
	const float Frac = Limit > 0.0 ? static_cast<float>(FMath::Clamp((Aim.deadline() - Now) / Limit, 0.0, 1.0)) : 0.f;
	const float BarW = 360.f * S;
	const float BarH = 10.f * S;
	const float BarX = Canvas->ClipX * 0.5f - BarW * 0.5f;
	const float BarY = Canvas->ClipY * 0.80f;
	DrawRect(kHpBack, BarX, BarY, BarW, BarH);
	DrawRect(Frac < 0.3f ? kRed : kGold, BarX, BarY, BarW * Frac, BarH);
	DrawStrCentered(kAimHint, kDim, Canvas->ClipX * 0.5f, BarY + BarH + 10.f * S, Small, 1.f);
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
			FLinearColor Color = kWhite;
			if (P.bDefence)
			{
				Color = P.Grade == EFTGrade::Perfect ? kGold : P.Grade == EFTGrade::Miss ? kRed : kCyan;
			}
			// A weak-point hit is gold and bigger; HP healed is green.
			const float Size = P.bWeak ? 1.35f : 1.f;
			if (P.bWeak)
			{
				Color = kGold;
			}
			else if (P.bHeal)
			{
				Color = kHpParty;
			}
			DrawStrCentered(P.Text, WithAlpha(FLinearColor(0.f, 0.f, 0.f), A), static_cast<float>(W.X) + 2.f * S,
				static_cast<float>(W.Y) - 40.f * S - Rise + 2.f * S, Large, Size);
			DrawStrCentered(P.Text, WithAlpha(Color, A), static_cast<float>(W.X), static_cast<float>(W.Y) - 40.f * S - Rise, Large, Size);
		}
		else
		{
			const float A = Fade(Age, 0.25, 0.35);
			if (A <= 0.f)
			{
				continue;
			}
			const FString* Label = &kMiss.Get();
			FLinearColor Color = kRed;
			if (P.bWrong)
			{
				Label = &kWrong.Get();
			}
			else
			{
				switch (P.Grade)
				{
				case EFTGrade::Perfect: Label = &kPerfect.Get(); Color = kGold; break;
				case EFTGrade::Great: Label = &kGreat.Get(); Color = FLinearColor(0.4f, 1.f, 0.45f); break;
				case EFTGrade::Good: Label = &kGood.Get(); Color = FLinearColor(0.35f, 0.85f, 1.f); break;
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
	if (!D.GetPhaseBanner().IsEmpty() && !IsOver(*B))
	{
		const float A = Fade(D.GetPhaseBannerAge(), 1.8, 0.5);
		if (A > 0.f)
		{
			const float Y = Canvas->ClipY * 0.30f;
			DrawStrCentered(D.GetPhaseBanner(), WithAlpha(FLinearColor(0.f, 0.f, 0.f), A), CX + 2.f * S, Y + 2.f * S, Large, 1.6f);
			DrawStrCentered(D.GetPhaseBanner(), WithAlpha(kOrange, A), CX, Y, Large, 1.6f);
		}
	}
	if (D.GetCountBeat() > 0 && B->phase() == ftcore::Phase::EnemySkill)
	{
		// The count to the enemy's first hit: the beats left, each held a moment, then fading.
		const float A = Fade(D.GetCountAge(), 0.2, 0.25);
		if (A > 0.f)
		{
			const FString Count = FString::FromInt(D.GetCountBeat());
			const float Pop = 1.f + 0.4f * static_cast<float>(FMath::Clamp(1.0 - D.GetCountAge() / 0.1, 0.0, 1.0));
			const float Y = Canvas->ClipY * 0.42f;
			DrawStrCentered(Count, WithAlpha(FLinearColor(0.f, 0.f, 0.f), A), CX + 2.f * S, Y + 2.f * S, Large, 2.f * Pop);
			DrawStrCentered(Count, WithAlpha(kCyan, A), CX, Y, Large, 2.f * Pop);
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
		// The title, the results (one line each), then what a press does: continue (a won story or
		// field fight), Retry and Quit to title (a lost story fight) or fight again.
		const bool bWon = B->phase() == ftcore::Phase::Victory;
		const int32 Lines = D.GetResultLineCount();
		const bool bQuit = D.CanQuitToTitle();
		const float LineH = 28.f * S;
		const float PromptH = 34.f * S;
		const float ResultsY = 86.f * S;
		const float PromptsY = ResultsY + static_cast<float>(Lines) * LineH + (Lines > 0 ? 16.f * S : 4.f * S);
		const float Height = PromptsY + (bQuit ? 2.f : 1.f) * PromptH + 30.f * S;
		const float Y = FMath::Max(Canvas->ClipY * 0.5f - Height * 0.5f, 20.f * S);
		DrawRect(kPanel, 0.f, Y - 20.f * S, Canvas->ClipX, Height);
		DrawStrCentered(bWon ? kVictory : kDefeat, bWon ? kGold : kRed, CX, Y, Large, 3.f);
		for (int32 I = 0; I < Lines; ++I)
		{
			// After the stat lines, a story win's reward.
			const bool bReward = bWon && I >= AFTBattleDemoActor::StatResultLines;
			DrawStrCentered(D.GetResultLine(I), bReward ? kGold : kWhite, CX, Y + ResultsY + static_cast<float>(I) * LineH, Font, 0.9f);
		}
		if (D.IsWaitingForRestart())
		{
			const float PY = Y + PromptsY;
			if (!D.GetContinuePrompt().IsEmpty())
			{
				DrawStrCentered(D.GetContinuePrompt(), kDim, CX, PY, Font, 1.f);
			}
			else if (bQuit)
			{
				DrawStrCentered(kRetry, kWhite, CX, PY, Font, 1.f);
				DrawStrCentered(kQuitToTitle, kWhite, CX, PY + PromptH, Font, 1.f);
			}
			else
			{
				DrawStrCentered(kAgain, kWhite, CX, PY, Font, 1.f);
			}
		}
	}
}
