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
#include "FTText.h"
#include "FTUiText.h"

#include "ftcore/Battle.h"

namespace
{
// Lane order Left, Down, Up, Right (screen directions of the arrowheads).
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
const FTUiText kEarly(TEXT("battle.early"));
const FTUiText kLate(TEXT("battle.late"));
const FTUiText kTooEarly(TEXT("battle.too_early"));
const FTUiText kFullCombo(TEXT("battle.full_combo"));
const FTUiText kDown(TEXT("battle.down"));
const FTUiText kStaggered(TEXT("battle.staggered"));
const FTUiText kOverloaded(TEXT("battle.overloaded"));
const FTUiText kLoad(TEXT("battle.load"));
const FTUiText kTorn(TEXT("battle.torn"));
const FTUiText kRead(TEXT("battle.read"));
const FString kCardKeys[ftcore::kReadCards] = {FString(TEXT("1  LS left")), FString(TEXT("2  LS up")), FString(TEXT("3  LS right"))};
const FString kDefenceKeys[5] = {FString(TEXT("Dodge  Left/Right  X/B")), FString(TEXT("Parry  Down  A")),
	FString(TEXT("Jump  Up  Y")), FString(TEXT("Guard  hold G  RB")), FString(TEXT("Intercept  hold I  LB"))};
const FString kTell(TEXT("!"));
// The count-in's numbers, built once (no allocation per frame); beats past the last show it.
const FString kCountNumbers[8] = {FString(TEXT("1")), FString(TEXT("2")), FString(TEXT("3")), FString(TEXT("4")),
	FString(TEXT("5")), FString(TEXT("6")), FString(TEXT("7")), FString(TEXT("8"))};
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
	DrawReadCards(D);
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
	// With a reader in the party the attacks are read as they come (Read cards): no intent is shown.
	const bool bHideIntent = B->reader().has_value();
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
			if (!Skill.IsEmpty() && !bHideIntent)
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
	// Party panels leave a row under the HP text for the Load gauge and Torn; enemy panels end below
	// the HP text.
	const float H = bParty ? 96.f * S : 70.f * S;
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
	else if (C.staggered)
	{
		DrawStr(kStaggered, kGold, X + W - 110.f * S, Y + 8.f * S, Font, 1.3f);
	}
	else if (C.load.overloaded())
	{
		DrawStr(kOverloaded, kOrange, X + W - 120.f * S, Y + 8.f * S, Font, 1.3f);
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
	if (C.stats.maxPoise > 0)
	{
		// Poise: a thin gold bar over the HP bar, empty while staggered.
		const float PoiseFrac = FMath::Clamp(static_cast<float>(C.poise) / static_cast<float>(C.stats.maxPoise), 0.f, 1.f);
		DrawRect(kHpBack, BarX, BarY - 6.f * S, BarW, 4.f * S);
		DrawRect(kGold, BarX, BarY - 6.f * S, BarW * PoiseFrac, 4.f * S);
	}

	const float LoadY = BarY + BarH + 28.f * S;
	const float LoadX = BarX + 46.f * S;
	const float LoadW = BarW * 0.5f;
	if (bParty && (C.can(ftcore::Verb::Guard) || C.can(ftcore::Verb::Intercept) || C.load.value > 0))
	{
		// Load: a cyan gauge under the HP text, orange when full (only Release and Take Half are open
		// then). A member without the verbs shows it only while it holds Load shared to it.
		const float LoadFrac = static_cast<float>(FMath::Clamp(C.load.value, 0, ftcore::kMaxLoad)) / static_cast<float>(ftcore::kMaxLoad);
		DrawStr(kLoad, kDim, BarX, LoadY - 3.f * S, Font, 0.9f);
		DrawRect(kHpBack, LoadX, LoadY, LoadW, 6.f * S);
		DrawRect(C.load.overloaded() ? kOrange : kCyan, LoadX, LoadY, LoadW * LoadFrac, 6.f * S);
	}
	if (bParty && C.alive() && C.tornTurns > 0)
	{
		// Torn: the tag right of the Load gauge, then one red pip per turn of it left.
		const float TornX = LoadX + LoadW + 16.f * S;
		DrawStr(kTorn, kRed, TornX, LoadY - 3.f * S, Font, 0.9f);
		const float Pip = 6.f * S;
		float PX = TornX + 44.f * S;
		for (int32 I = 0; I < C.tornTurns && I < 8; ++I)
		{
			DrawRect(kRed, PX, LoadY, Pip, Pip);
			PX += Pip + 3.f * S;
		}
	}

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
			PartyY -= 104.f * S;
			DrawPanel(D, Id, 24.f * S, PartyY, Id == Actor);
		}
	}
	// Up to five enemy panels (ftcore::kMaxLineupEnemies): 78 px apart, closer when the stack would
	// reach past the middle of the screen (never closer than a panel's height, 70 px, and a gap).
	int32 Enemies = 0;
	for (int32 Id = 0; Id < Count; ++Id)
	{
		Enemies += B->combatant(static_cast<ftcore::CombatantId>(Id)).team == ftcore::Team::Enemies ? 1 : 0;
	}
	const float EnemyStep = Enemies > 0 ? FMath::Clamp((Canvas->ClipY * 0.5f - 24.f * S) / static_cast<float>(Enemies), 74.f * S, 78.f * S) : 78.f * S;
	for (int32 Id = 0; Id < Count; ++Id)
	{
		if (B->combatant(static_cast<ftcore::CombatantId>(Id)).team == ftcore::Team::Enemies)
		{
			DrawPanel(D, Id, Canvas->ClipX - 324.f * S, 24.f * S + static_cast<float>(EnemyIndex) * EnemyStep, Id == Actor);
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

// ---- the arrow lane ----

namespace
{
FLinearColor Lc(const ftcore::LaneColor& C, float Alpha = 1.f)
{
	return FLinearColor(C.r, C.g, C.b, C.a * Alpha);
}

// The grade's colour in the lane's token set.
FLinearColor GradeColor(const ftcore::ArrowLaneColors& C, ftcore::Grade G)
{
	switch (G)
	{
	case ftcore::Grade::Perfect: return Lc(C.perfect);
	case ftcore::Grade::Great: return Lc(C.great);
	case ftcore::Grade::Good: return Lc(C.good);
	default: return Lc(C.miss);
	}
}

// The arrowhead in local axes: u along the arrow toward its tip (-R at the shaft's end, R at the tip), v across.
// A short shaft of half width 0.30 R, a head of half width 0.95 R at u = 0 narrowing to the tip.
constexpr float kShaftHalf = 0.30f;
constexpr float kHeadHalf = 0.95f;
} // namespace

// A solid arrowhead drawn as strips along its length (rectangles only, so no per-frame allocation).
void AFTBattleHUD::DrawArrowFill(int32 Lane, float X, float Y, float R, float Grow, const FLinearColor& Color)
{
	const FVector2D Dir = kLaneDirs[Lane & 3];
	const bool bHorizontal = Dir.X != 0.0;
	const float Sign = static_cast<float>(Dir.X + Dir.Y);
	const float Step = FMath::Max(2.f * UiScale, 1.5f);
	for (float U = -R - Grow; U < R + Grow; U += Step)
	{
		const float U1 = FMath::Min(U + Step, R + Grow);
		const float Mid = (U + U1) * 0.5f;
		float Half;
		if (Mid < 0.f)
		{
			Half = kShaftHalf * R + Grow;
		}
		else if (Mid <= R)
		{
			Half = kHeadHalf * R * (1.f - Mid / R) + Grow;
		}
		else
		{
			Half = FMath::Max(R + Grow - Mid, 0.f);
		}
		// Along the arrow from U to U1; across it +-Half.
		const float A = Sign * U;
		const float B = Sign * U1;
		const float Lo = FMath::Min(A, B);
		const float Len = FMath::Abs(B - A);
		if (bHorizontal)
		{
			DrawRect(Color, X + Lo, Y - Half, Len, Half * 2.f);
		}
		else
		{
			DrawRect(Color, X - Half, Y + Lo, Half * 2.f, Len);
		}
	}
}

// The same arrowhead, hollow.
void AFTBattleHUD::DrawArrowOutline(int32 Lane, float X, float Y, float R, const FLinearColor& Color, float Thickness)
{
	const FVector2D Dir = kLaneDirs[Lane & 3];
	const FVector2D Perp(-Dir.Y, Dir.X);
	const FVector2D C(X, Y);
	const auto At = [&](float U, float V) { return C + Dir * U + Perp * V; };
	const FVector2D P[7] = {At(R, 0.f), At(0.f, kHeadHalf * R), At(0.f, kShaftHalf * R), At(-R, kShaftHalf * R),
		At(-R, -kShaftHalf * R), At(0.f, -kShaftHalf * R), At(0.f, -kHeadHalf * R)};
	for (int32 I = 0; I < 7; ++I)
	{
		const FVector2D& A = P[I];
		const FVector2D& B = P[(I + 1) % 7];
		DrawLine(A.X, A.Y, B.X, B.Y, Color, Thickness);
	}
}

void AFTBattleHUD::DrawCross(float X, float Y, float R, const FLinearColor& Color, float Thickness)
{
	DrawLine(X - R, Y - R, X + R, Y + R, Color, Thickness);
	DrawLine(X - R, Y + R, X + R, Y - R, Color, Thickness);
}

void AFTBattleHUD::DrawStar(float X, float Y, float R, const FLinearColor& Color, float Thickness)
{
	DrawLine(X - R, Y, X + R, Y, Color, Thickness);
	DrawLine(X, Y - R, X, Y + R, Color, Thickness);
	const float D = R * 0.6f;
	DrawLine(X - D, Y - D, X + D, Y + D, Color, Thickness * 0.6f);
	DrawLine(X - D, Y + D, X + D, Y - D, Color, Thickness * 0.6f);
}

// The d-pad cross: a plus of four arms.
void AFTBattleHUD::DrawPadCross(float X, float Y, float Size, const FLinearColor& Color)
{
	const float Arm = Size * 0.34f;
	const float Len = Size * 0.5f;
	DrawRect(Color, X - Arm * 0.5f, Y - Len, Arm, Len * 2.f);
	DrawRect(Color, X - Len, Y - Arm * 0.5f, Len * 2.f, Arm);
}

// The receptor row alone, faint: where the lane will stand, shown while a lead chooses.
void AFTBattleHUD::DrawLanePreview(const AFTBattleDemoActor& D)
{
	const ftcore::ArrowLaneLayout& L = D.GetLaneLayout();
	const float S = UiScale;
	const float R = FMath::Min(Geo.glyphH, Geo.glyphW) * 0.5f;
	for (int32 Lane = 0; Lane < 4; ++Lane)
	{
		DrawArrowOutline(Lane, Geo.columnCentre(Lane), Geo.targetY, R, Lc(L.colors.targetIdle, L.times.previewAlpha), 2.5f * S);
	}
}

// Burst rings (and the Perfect's star) on the receptors, and the missed notes' ghosts.
void AFTBattleHUD::DrawLaneBursts(const AFTBattleDemoActor& D, float Vis)
{
	const ftcore::ArrowLaneLayout& L = D.GetLaneLayout();
	const ftcore::ArrowLaneTimes& T = L.times;
	const float S = UiScale;
	const double Now = HudWall();
	const float R = FMath::Min(Geo.glyphH, Geo.glyphW) * 0.5f;
	for (int32 Lane = 0; Lane < 4; ++Lane)
	{
		const AFTBattleDemoActor::FLaneJudge& J = D.GetLaneJudge(Lane);
		const double Age = Now - J.Wall;
		const float CX = Geo.columnCentre(Lane);
		if (J.Grade == ftcore::Grade::Miss)
		{
			if (Age < 0.0 || Age >= T.missFade)
			{
				continue;
			}
			// The note stops where it was judged, greys, sinks a little and fades; an X marks it.
			const float K = static_cast<float>(Age / T.missFade);
			const float Y = FMath::Max(Geo.noteY(J.NoteDt) + T.missSinkPx * S * K, Geo.plate.y + R + 4.f * S);
			const float A = (1.f - K) * Vis;
			DrawArrowFill(Lane, CX, Y, R, 3.f * S, WithAlpha(Lc(L.colors.arrowEdge), A));
			DrawArrowFill(Lane, CX, Y, R, 0.f, WithAlpha(FLinearColor(0.5f, 0.5f, 0.55f), A * 0.9f));
			DrawCross(CX, Y, R * 0.55f, WithAlpha(Lc(L.colors.miss), A * 1.4f), 4.f * S);
			continue;
		}
		const float Radius = ftcore::burstRadius(T, J.Grade, Age) * 0.01f * Geo.box.h * 0.5f; // the sizes are diameters
		if (Radius <= 0.f)
		{
			continue;
		}
		const float K = static_cast<float>(Age / ftcore::burstLength(T, J.Grade));
		const FLinearColor Color = WithAlpha(GradeColor(L.colors, J.Grade), (1.f - K * 0.8f) * Vis);
		const float Thick = (J.Grade == ftcore::Grade::Perfect ? 4.f : J.Grade == ftcore::Grade::Great ? 3.f : 2.f) * S;
		DrawRing(CX, Geo.targetY, Radius, Color, Thick);
		if (ftcore::burstHasStar(J.Grade, D.IsReduceFlashes()))
		{
			DrawStar(CX, Geo.targetY, Radius * 1.25f, Color, 3.f * S);
		}
	}
}

// The lane's word: the last grade (a jump pair's worse one), or TOO EARLY after a stray; under a non-Perfect
// grade the early or late tick, with a notch on the lane where the press landed.
void AFTBattleHUD::DrawLaneWord(const AFTBattleDemoActor& D, float Vis)
{
	const ftcore::ArrowLaneLayout& L = D.GetLaneLayout();
	const ftcore::ArrowLaneTimes& T = L.times;
	const float S = UiScale;
	UFont* Font = GEngine->GetLargeFont();
	UFont* TickFont = GEngine->GetMediumFont();
	const double Now = HudWall();
	const AFTBattleDemoActor::FLaneJudge& W = D.GetLaneWord();
	const double WordAge = Now - W.Wall;
	const double StrayAge = D.GetStrayAge();
	const float CX = Geo.plate.x + Geo.plate.w * 0.5f;
	const double Span = T.wordHold + T.wordOut;
	const float BaseY = Geo.box.y + L.wordStartY * 0.01f * Geo.box.h;
	const FLinearColor Black(0.f, 0.f, 0.f, 1.f);

	if (StrayAge >= 0.0 && StrayAge < WordAge && StrayAge < Span)
	{
		const float A = Fade(StrayAge, T.wordHold, T.wordOut) * Vis;
		const float Y = BaseY - T.wordRisePx * S * static_cast<float>(FMath::Clamp(StrayAge / Span, 0.0, 1.0));
		DrawStrCentered(kTooEarly, WithAlpha(Black, A), CX + 2.f * S, Y + 2.f * S, Font, 1.5f);
		DrawStrCentered(kTooEarly, WithAlpha(Lc(L.colors.warn), A), CX, Y, Font, 1.5f);
		return;
	}
	if (W.Grade == ftcore::Grade::Pending || WordAge < 0.0 || WordAge >= Span)
	{
		return;
	}
	const float A = Fade(WordAge, T.wordHold, T.wordOut) * Vis;
	const float Y = BaseY - T.wordRisePx * S * static_cast<float>(FMath::Clamp(WordAge / Span, 0.0, 1.0));
	const FString* Word = &kMiss.Get();
	float Size = 1.6f;
	switch (W.Grade)
	{
	case ftcore::Grade::Perfect: Word = &kPerfect.Get(); Size = 1.85f; break;
	case ftcore::Grade::Great: Word = &kGreat.Get(); break;
	case ftcore::Grade::Good: Word = &kGood.Get(); break;
	default:
		if (W.bWrong)
		{
			Word = &kWrong.Get();
		}
		break;
	}
	const FLinearColor Color = GradeColor(L.colors, W.Grade);
	DrawStrCentered(*Word, WithAlpha(Black, A), CX + 2.f * S, Y + 2.f * S, Font, Size);
	DrawStrCentered(*Word, WithAlpha(FLinearColor(Color.R, Color.G, Color.B, 1.f), A), CX, Y, Font, Size);

	// Early or late: a small word under it, and a notch where the press landed on the lane.
	const int32 Tick = ftcore::pressTick(W.Grade, !W.bExpired, W.Error);
	if (Tick != 0)
	{
		const FString& Label = Tick < 0 ? kEarly.Get() : kLate.Get();
		// Above the plate's top edge, over the word (the receptor row is under the word).
		const float LY = Geo.plate.y - 30.f * S;
		DrawStrCentered(Label, WithAlpha(Black, A), CX + 1.5f * S, LY + 1.5f * S, TickFont, 1.5f);
		DrawStrCentered(Label, WithAlpha(Lc(L.colors.arrowFill), A * 0.95f), CX, LY, TickFont, 1.5f);
		if (!W.bExpired)
		{
			const float NY = Geo.targetY + ftcore::tickOffsetPx(Geo, W.Error, GoodWindow);
			const float NX = Geo.columnCentre(W.Lane);
			const float Half = Geo.glyphW * 0.42f;
			DrawRect(WithAlpha(Black, A), NX - Half - 1.f * S, NY - 3.f * S, Half * 2.f + 2.f * S, 6.f * S);
			DrawRect(WithAlpha(Lc(L.colors.arrowFill), A), NX - Half, NY - 2.f * S, Half * 2.f, 4.f * S);
		}
	}
}

// The summary stamp in the lane once the last note is judged: the accuracy large, FULL COMBO, the best streak.
void AFTBattleHUD::DrawLaneStamp(const AFTBattleDemoActor& D, float Alpha)
{
	if (Alpha <= 0.f || !D.HasComboStamp())
	{
		return;
	}
	const ftcore::ArrowLaneLayout& L = D.GetLaneLayout();
	const float S = UiScale;
	UFont* Font = GEngine->GetMediumFont();
	UFont* Large = GEngine->GetLargeFont();
	const ftcore::PxRect Rect = ftcore::resolve(L.stamp, Geo.box);
	const float Scale = ftcore::stampScale(L.times, D.GetComboEndAge());
	const float CX = Rect.x + Rect.w * 0.5f;
	const FLinearColor Black(0.f, 0.f, 0.f, 1.f);
	const bool bFull = D.IsStampFull();
	DrawRect(Lc(L.colors.plate, Alpha), Rect.x - 6.f * S, Rect.y - 8.f * S, Rect.w + 12.f * S, (bFull ? 190.f : 150.f) * S);
	const FLinearColor Main = bFull ? Lc(L.colors.perfect) : Lc(L.colors.arrowFill);
	DrawStrCentered(D.GetStampAccuracyText(), WithAlpha(Black, Alpha), CX + 2.f * S, Rect.y + 2.f * S, Large, 3.2f * Scale);
	DrawStrCentered(D.GetStampAccuracyText(), WithAlpha(Main, Alpha), CX, Rect.y, Large, 3.2f * Scale);
	float LineY = Rect.y + 96.f * S;
	if (bFull)
	{
		float W = 0.f, H = 0.f;
		GetTextSize(kFullCombo, W, H, Font, UiScale * 1.7f);
		DrawStrCentered(kFullCombo, WithAlpha(Lc(L.colors.perfect), Alpha), CX, LineY, Font, 1.7f);
		DrawRect(WithAlpha(Lc(L.colors.perfect), Alpha), CX - W * 0.5f, LineY + H + 2.f * S, W, 3.f * S);
		LineY += 44.f * S;
	}
	DrawStrCentered(D.GetStampBestText(), WithAlpha(Lc(L.colors.targetIdle), Alpha), CX, LineY, Font, 1.5f);
}

void AFTBattleHUD::DrawLane(const AFTBattleDemoActor& D)
{
	UFTBattleComponent* BC = D.GetBattle();
	const ftcore::Battle* B = BC->GetBattle();
	const ftcore::ArrowLaneLayout& L = D.GetLaneLayout();
	const ftcore::ArrowLaneTimes& T = L.times;
	const ftcore::ArrowLaneColors& Col = L.colors;
	const bool bCombo = BC->IsComboActive();
	const double EndAge = D.GetComboEndAge();
	const bool bExiting = !bCombo && EndAge >= 0.0 && EndAge < T.exitEnd;
	const double Bpm = D.GetConductor() ? D.GetConductor()->Bpm : 0.0;
	Geo = ftcore::laneGeometry(L, Canvas->ClipX, Canvas->ClipY, ftcore::laneLookahead(T, Bpm));
	if (!bCombo && !bExiting)
	{
		// Choosing: the receptor row, faint, so its place is known before the combo starts.
		if (B->phase() == ftcore::Phase::AwaitingCommand && !D.IsWaitingForRestart() && !IsOver(*B)
			&& B->combatant(B->actor()).team == ftcore::Team::Party)
		{
			DrawLanePreview(D);
		}
		return;
	}
	const float S = UiScale;
	const double Now = BC->GetBattleTime();
	const ftcore::ComboSequence& Combo = B->combo();
	PerfectWindow = Combo.windows().perfect;
	GoodWindow = Combo.windows().good;
	const bool bQuick = bExiting && !D.HasComboStamp();
	const float Vis = ftcore::laneVisibility(T, D.GetComboAge(), bCombo ? -1.0 : EndAge, bQuick);
	if (Vis <= 0.f)
	{
		return;
	}
	const float Dim = bCombo ? 1.f : ftcore::plateDimming(T, EndAge);
	const float R = FMath::Min(Geo.glyphH, Geo.glyphW) * 0.5f;
	const float ColsX = Geo.columnX0;
	const float ColsW = Geo.pitch * 4.f;
	const float PlateBottom = Geo.plate.y + Geo.plate.h;
	const double StrayAge = D.GetStrayAge();
	const bool bLocked = StrayAge >= 0.0 && StrayAge < Combo.lockout();

	// The plate, a hairline round it, a faint tint and a hue bar per column.
	DrawRect(Lc(Col.plate, Vis * Dim), Geo.plate.x, Geo.plate.y, Geo.plate.w, Geo.plate.h);
	DrawFrame(Geo.plate.x, Geo.plate.y, Geo.plate.w, Geo.plate.h, FLinearColor(0.85f, 0.85f, 0.9f, 0.3f * Vis), 1.f * S);
	for (int32 Lane = 0; Lane < 4; ++Lane)
	{
		const float X = ColsX + Geo.pitch * static_cast<float>(Lane);
		DrawRect(Lc(Col.rim[Lane], 0.07f * Vis * Dim), X + 2.f * S, Geo.plate.y + 2.f * S, Geo.pitch - 4.f * S, Geo.plate.h - 4.f * S);
		DrawRect(Lc(Col.rim[Lane], 0.9f * Vis * Dim), X + 6.f * S, PlateBottom - 5.f * S, Geo.pitch - 12.f * S, 3.f * S);
	}
	if (D.IsStampFull() && D.HasComboStamp() && !bCombo && EndAge < T.exitEnd)
	{
		// A full combo: a gold edge on the plate, steady (it fades with the lane).
		DrawFrame(Geo.plate.x, Geo.plate.y, Geo.plate.w, Geo.plate.h, Lc(Col.perfect, Vis), 3.f * S);
	}

	// Beat lines, scrolling with the arrows (every fourth brighter): from the first chart note on the song's grid.
	if (Bpm > 0.0 && Combo.size() > 0 && Combo.note(0).beat != ftcore::kNoBeat && !bExiting)
	{
		const ftcore::ComboNote& Ref = Combo.note(0);
		const double Spb = 60.0 / Bpm;
		const int64 First = static_cast<int64>(FMath::CeilToDouble(Ref.beat + (Now - 0.05 - Ref.time) / Spb));
		for (int64 K = First;; ++K)
		{
			const double Dt = Ref.time + (static_cast<double>(K) - Ref.beat) * Spb - Now;
			if (Dt > Geo.lookahead)
			{
				break;
			}
			const float Y = Geo.noteY(Dt);
			if (Y > PlateBottom - 2.f * S || Y < Geo.plate.y + 2.f * S)
			{
				continue;
			}
			const bool bBar = (((K % 4) + 4) % 4) == 0;
			DrawRect(Lc(Col.targetIdle, (bBar ? 0.30f : 0.14f) * Vis * Geo.noteAlpha(T, Dt)), ColsX, Y - 1.f * S, ColsW, (bBar ? 2.f : 1.f) * S);
		}
	}

	// The target row: the Good and Perfect windows drawn on it at the arrows' speed, then a pulse on the beat.
	const float GoodPx = Geo.windowPx(GoodWindow);
	const float PerfectPx = Geo.windowPx(PerfectWindow);
	const FLinearColor Warn = Lc(Col.warn);
	DrawRect(bLocked ? WithAlpha(Warn, 0.16f * Vis) : Lc(Col.targetIdle, 0.08f * Vis * Dim), ColsX, Geo.targetY - GoodPx, ColsW, GoodPx * 2.f);
	DrawRect(bLocked ? WithAlpha(Warn, 0.32f * Vis) : Lc(Col.perfect, 0.17f * Vis * Dim), ColsX, Geo.targetY - PerfectPx, ColsW, PerfectPx * 2.f);
	double Beat = 0.0;
	if (UFTConductorComponent* Conductor = D.GetConductor())
	{
		Beat = Conductor->GetBeat();
	}
	const float Pulse = static_cast<float>(1.0 - (Beat - FMath::FloorToDouble(Beat)));
	const float PulseA = D.IsReduceFlashes() ? 0.6f : 0.25f + 0.45f * Pulse * Pulse; // under reduce flashes a steady 60%
	DrawRect(bLocked ? WithAlpha(Warn, Vis) : Lc(Col.targetIdle, PulseA * Vis * Dim), ColsX, Geo.targetY - 1.5f * S, ColsW, 3.f * S);
	if (bLocked)
	{
		// The lockout: a bar over the row that shrinks to nothing in the lockout's length.
		const float K = 1.f - static_cast<float>(StrayAge / Combo.lockout());
		DrawRect(WithAlpha(Warn, Vis), ColsX, Geo.targetY - R - 14.f * S, ColsW * K, 5.f * S);
	}

	// Which lanes this chart uses; the rest are dimmed.
	uint32 Used = 0;
	for (size_t I = 0; I < Combo.size(); ++I)
	{
		Used |= 1u << (static_cast<uint32>(Combo.note(I).arrow) & 3u);
	}
	const double RimAge = D.GetStreakStepAge();
	const double RimHold = D.IsReduceFlashes() ? 0.4 : T.rimHold;
	for (int32 Lane = 0; Lane < 4; ++Lane)
	{
		const float CX = Geo.columnCentre(Lane);
		const float Lit = static_cast<float>(FMath::Clamp(1.0 - D.GetLanePressAge(Lane) / T.receptorDecay, 0.0, 1.0));
		float A = ((Used >> Lane) & 1u) ? 1.f : T.unusedAlpha;
		if (bLocked)
		{
			A *= 0.35f;
		}
		const FLinearColor Idle = Lc(Col.targetIdle, A * Vis * Dim);
		const FLinearColor Color = FLinearColor::LerpUsingHSV(Idle, FLinearColor(1.f, 1.f, 1.f, Idle.A), Lit);
		DrawArrowOutline(Lane, CX, Geo.targetY, R * (1.f + 0.15f * Lit), Color, (3.f + 2.f * Lit) * S);
		if (RimAge >= 0.0 && RimAge < RimHold)
		{
			DrawArrowOutline(Lane, CX, Geo.targetY, R * 1.18f, Lc(Col.perfect, Vis), 3.f * S);
		}
	}

	// The notes still to come: a link bar between a jump pair's arrows first, then the arrows.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (size_t I = Combo.nextIndex(); I < Combo.size(); ++I)
		{
			if (Combo.grade(I) != ftcore::Grade::Pending)
			{
				continue;
			}
			const ftcore::ComboNote& Note = Combo.note(I);
			const double Dt = Note.time - Now;
			if (Dt > Geo.lookahead)
			{
				break; // notes are in time order
			}
			const float Y = Geo.noteY(Dt);
			const float A = Geo.noteAlpha(T, Dt) * Vis;
			const int32 Lane = static_cast<int32>(Note.arrow) & 3;
			const float CX = Geo.columnCentre(Lane);
			if (Pass == 0)
			{
				if (I + 1 < Combo.size() && Combo.grade(I + 1) == ftcore::Grade::Pending && Combo.note(I + 1).time == Note.time)
				{
					const float CX2 = Geo.columnCentre(static_cast<int32>(Combo.note(I + 1).arrow) & 3);
					DrawRect(Lc(Col.arrowFill, A * 0.85f), FMath::Min(CX, CX2), Y - 1.5f * S, FMath::Abs(CX2 - CX), 3.f * S);
				}
				continue;
			}
			DrawArrowFill(Lane, CX, Y, R, 3.f * S, Lc(Col.arrowEdge, A));
			DrawArrowFill(Lane, CX, Y, R, 0.f, Lc(Col.arrowFill, A));
		}
	}

	DrawLaneBursts(D, Vis);
	DrawLaneWord(D, Vis);

	// The streak badge above the plate's top right: from two in a row, popping every fourth; a Miss leaves the old
	// number grey for a moment.
	{
		const ftcore::PxRect Rect = ftcore::resolve(L.streak, Geo.box);
		UFont* Font = GEngine->GetLargeFont();
		const int32 Streak = D.GetStreak();
		const float CX = Rect.x + Rect.w * 0.5f;
		const float CY = Rect.y + Rect.h * 0.2f;
		const FLinearColor Black(0.f, 0.f, 0.f, 1.f);
		if (Streak >= T.streakFrom)
		{
			const float Pop = ftcore::streakPopScale(T, D.GetStreakStepAge());
			DrawStrCentered(D.GetStreakText(Streak), WithAlpha(Black, Vis), CX + 2.f * S, CY + 2.f * S, Font, 1.8f * Pop);
			DrawStrCentered(D.GetStreakText(Streak), Lc(Col.arrowFill, Vis), CX, CY, Font, 1.8f * Pop);
		}
		else if (Streak == 0 && D.GetBrokenStreak() >= T.streakFrom && D.GetBrokenStreakAge() < T.streakMissHold)
		{
			DrawStrCentered(D.GetStreakText(D.GetBrokenStreak()), FLinearColor(0.55f, 0.55f, 0.58f, Vis), CX, CY, Font, 1.8f);
		}
	}

	// The input caption under the plate: the d-pad cross and the keys (never the face buttons, which belong to Beat).
	{
		const ftcore::PxRect Rect = ftcore::resolve(L.caption, Geo.box);
		UFont* Small = GEngine->GetMediumFont();
		const FLinearColor Dimmed = Lc(Col.targetIdle, Vis * Dim);
		float TW = 0.f, TH = 0.f;
		GetTextSize(kLaneHint, TW, TH, Small, UiScale);
		const float Icon = Rect.h * 0.9f;
		const float Total = Icon + 8.f * S + TW;
		const float X0 = Rect.x + (Rect.w - Total) * 0.5f;
		DrawPadCross(X0 + Icon * 0.5f, Rect.y + Rect.h * 0.5f, Icon, Dimmed);
		DrawStr(kLaneHint, Dimmed, X0 + Icon + 8.f * S, Rect.y + (Rect.h - TH) * 0.5f, Small, 1.f);
	}

	if (!bCombo)
	{
		DrawLaneStamp(D, ftcore::stampAlpha(T, EndAge));
	}
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
		case ftcore::DefenceOutcome::PerfectGuard:
		case ftcore::DefenceOutcome::PerfectIntercept: Color = kGold; break;
		case ftcore::DefenceOutcome::Guarded:
		case ftcore::DefenceOutcome::Intercepted: Color = kCyan; break;
		default: break;
		}
		DrawRect(Color, PX, PY, Pip, Pip);
		PX += Pip + Gap;
	}

	// The keys, the last one pressed lit for a moment.
	const float Lit = static_cast<float>(FMath::Clamp(1.0 - D.GetDefencePressAge() / 0.2, 0.0, 1.0));
	const int32 Pressed = static_cast<int32>(D.GetLastDefenceInput());
	// The Guard key only for a defender with the verb, the Intercept key only when someone can take
	// the attack for the defender (never for a child); a hold's key stays lit while it is held.
	const bool bGuard = B->combatant(B->defender()).can(ftcore::Verb::Guard);
	const bool bIntercept = B->interceptor().has_value();
	int32 Shown[5] = {0, 1, 2, 0, 0};
	int32 Keys = 3;
	if (bGuard)
	{
		Shown[Keys++] = 3;
	}
	if (bIntercept)
	{
		Shown[Keys++] = 4;
	}
	const float KeyW = (Keys > 4 ? 200.f : 230.f) * S;
	float KX = Canvas->ClipX * 0.5f - KeyW * 0.5f * static_cast<float>(Keys);
	const float KY = PY + Pip + 14.f * S;
	for (int32 N = 0; N < Keys; ++N)
	{
		const int32 K = Shown[N];
		const bool bHeld = K == 3 ? T.guarding() && !T.intercepting() : K == 4 && T.intercepting();
		const bool bLit = (K == Pressed && Lit > 0.f) || bHeld;
		DrawRect(kPanel, KX + 4.f * S, KY, KeyW - 8.f * S, 30.f * S);
		if (bLit)
		{
			DrawFrame(KX + 4.f * S, KY, KeyW - 8.f * S, 30.f * S, WithAlpha(kGold, bHeld ? 1.f : Lit), 2.f * S);
		}
		DrawStrCentered(kDefenceKeys[K], bLit ? kGold : kWhite, KX + KeyW * 0.5f, KY + 7.f * S, Small, 1.f);
		KX += KeyW;
	}
}

// The Read cards, under the enemy banner while the attack is up: each names an action, with its key and
// stick flick. Once picked, the right card is framed gold and a wrong pick red.
void AFTBattleHUD::DrawReadCards(const AFTBattleDemoActor& D)
{
	UFTBattleComponent* BC = D.GetBattle();
	const ftcore::Battle* B = BC->GetBattle();
	if (!BC->IsDefenceActive() || !B->readCards().dealt())
	{
		return;
	}
	const ftcore::ReadCards& Cards = B->readCards();
	const float S = UiScale;
	UFont* Font = GEngine->GetMediumFont();
	UFont* Small = GEngine->GetSmallFont();
	const float CardW = 210.f * S;
	const float CardH = 54.f * S;
	const float Gap = 14.f * S;
	// size_t throughout (ReadCards' own index type), never past the key labels.
	const size_t Count = FMath::Min(Cards.size(), ftcore::kReadCards);
	float X = Canvas->ClipX * 0.5f - (static_cast<float>(Count) * (CardW + Gap) - Gap) * 0.5f;
	const float Y = 165.f * S;
	DrawStr(kRead, kCyan, X - 70.f * S, Y + 16.f * S, Font, 1.f);
	for (size_t I = 0; I < Count; ++I)
	{
		const ftcore::ReadCard& Card = Cards[I];
		// The names cached at the battle's start (no allocation per frame).
		const FString& Name = D.GetSkillName(static_cast<int32>(Card.actor), static_cast<int32>(Card.action));
		const bool bRight = Cards.picked() && I == Cards.correct();
		const bool bWrong = Cards.picked() && !bRight && I == Cards.pickedIndex();
		DrawRect(kPanel, X, Y, CardW, CardH);
		DrawFrame(X, Y, CardW, CardH, bRight ? kGold : bWrong ? kRed : kDim, (bRight || bWrong ? 3.f : 1.f) * S);
		DrawStrCentered(Name, bRight ? kGold : kWhite, X + CardW * 0.5f, Y + 6.f * S, Font, 1.f);
		DrawStrCentered(kCardKeys[I], kDim, X + CardW * 0.5f, Y + 32.f * S, Small, 1.f);
		X += CardW + Gap;
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
	UFont* Large = GEngine->GetLargeFont();
	const double Now = HudWall();
	const float S = UiScale;
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
			const FString& Count = kCountNumbers[FMath::Clamp(D.GetCountBeat(), 1, 8) - 1];
			const float Pop = 1.f + 0.4f * static_cast<float>(FMath::Clamp(1.0 - D.GetCountAge() / 0.1, 0.0, 1.0));
			const float Y = Canvas->ClipY * 0.42f;
			// On a dark square, so it reads over the bright floor as well as the sky.
			const float Box = 76.f * S;
			DrawRect(WithAlpha(kPanel, A * kPanel.A), CX - Box * 0.5f, Y - 10.f * S, Box, Box);
			DrawStrCentered(Count, WithAlpha(FLinearColor(0.f, 0.f, 0.f), A), CX + 2.f * S, Y + 2.f * S, Large, 3.f * Pop);
			DrawStrCentered(Count, WithAlpha(kCyan, A), CX, Y, Large, 3.f * Pop);
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
