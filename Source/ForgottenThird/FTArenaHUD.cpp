#include "FTArenaHUD.h"

#include "FTAimReticle.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTArenaFighter.h"
#include "FTArenaGameMode.h"
#include "FTArenaHost.h"
#include "FTArenaRunner.h"
#include "ftcore/Settings.h"
#include "FTPauseOverlay.h"
#include "FTInputBindings.h"
#include "FTUiText.h"
#include "GameFramework/PlayerController.h"

namespace
{
const FLinearColor kArenaWhite(1.f, 1.f, 1.f);
const FLinearColor kArenaDim(0.6f, 0.62f, 0.68f);
const FLinearColor kArenaPanel(0.02f, 0.02f, 0.05f, 0.62f);
const FLinearColor kArenaBack(0.1f, 0.1f, 0.13f, 0.85f);
const FLinearColor kArenaHpBack(0.18f, 0.05f, 0.05f, 0.85f);
const FLinearColor kArenaGreen(0.3f, 0.85f, 0.4f);
const FLinearColor kArenaRed(1.f, 0.25f, 0.22f);
const FLinearColor kArenaStamina(0.95f, 0.85f, 0.3f);
const FLinearColor kArenaPoise(0.45f, 0.75f, 1.f);
const FLinearColor kArenaEnemyPoise(1.f, 0.78f, 0.25f);
const FLinearColor kArenaCyan(0.35f, 0.85f, 1.f);
const FLinearColor kArenaOrange(1.f, 0.55f, 0.15f);
const FLinearColor kArenaGold(1.f, 0.84f, 0.3f);
const FLinearColor kArenaLoad(1.f, 0.5f, 0.12f);
// The tell's colours: the answer it asks for.
const FLinearColor kArenaTellParry(1.f, 0.84f, 0.3f);
const FLinearColor kArenaTellBlock(1.f, 0.55f, 0.15f);
const FLinearColor kArenaTellDodge(1.f, 0.16f, 0.12f);

const FTUiText kHpArena(TEXT("arena.hp"));
const FTUiText kAllyArena(TEXT("arena.ally"));
const FTUiText kStaminaArena(TEXT("arena.stamina"));
const FTUiText kPoiseArena(TEXT("arena.poise"));
const FTUiText kHealsArena(TEXT("arena.heals"));
const FTUiText kTargetArena(TEXT("arena.target"));
const FTUiText kParryArena(TEXT("arena.parry"));
const FTUiText kBlockArena(TEXT("arena.block"));
const FTUiText kDodgeArena(TEXT("arena.dodge"));
const FTUiText kBlockedArena(TEXT("arena.blocked"));
const FTUiText kParriedArena(TEXT("arena.parried"));
const FTUiText kGuardBrokenArena(TEXT("arena.guard_broken"));
const FTUiText kEvadedArena(TEXT("arena.evaded"));
const FTUiText kRiposteArena(TEXT("arena.riposte"));
const FTUiText kNoStaminaArena(TEXT("arena.no_stamina"));
const FTUiText kAutoPlayArena(TEXT("arena.autoplay"));
const FTUiText kIdleArena(TEXT("arena.idle"));
const FTUiText kStatsArena(TEXT("arena.stats"));
const FTUiText kLoadArena(TEXT("arena.load"));
const FTUiText kLoadPlusArena(TEXT("arena.load_plus"));
const FTUiText kReleaseReadyArena(TEXT("arena.release_ready"));
const FTUiText kOverloadedArena(TEXT("arena.overloaded"));
const FTUiText kOverloadLockedArena(TEXT("arena.overload_locked"));
const FTUiText kReleaseArena(TEXT("arena.release"));
const FTUiText kCounterArena(TEXT("arena.counter"));
const FTUiText kReadArena(TEXT("arena.read"));
const FTUiText kMisreadArena(TEXT("arena.misread"));
const FTUiText kReadCounterArena(TEXT("arena.read_counter"));
const FTUiText kReadStacksArena(TEXT("arena.read_stacks"));
// The shapes a Read card names, in ftcore::ArenaTell's order.
const FTUiText kTellStrikeArena(TEXT("arena.tell.strike"));
const FTUiText kTellOverheadArena(TEXT("arena.tell.overhead"));
const FTUiText kTellBackhandArena(TEXT("arena.tell.backhand"));
const FTUiText kTellChargeArena(TEXT("arena.tell.charge"));
const FTUiText kTellThrustArena(TEXT("arena.tell.thrust"));
const FTUiText kTellSlashArena(TEXT("arena.tell.slash"));
const FTUiText kTellSweepArena(TEXT("arena.tell.sweep"));
const FTUiText kTellSlamArena(TEXT("arena.tell.slam"));
const FTUiText kTellShotArena(TEXT("arena.tell.shot"));
const FTUiText* const kArenaTellWords[] = {&kTellStrikeArena, &kTellOverheadArena, &kTellBackhandArena, &kTellChargeArena,
	&kTellThrustArena, &kTellSlashArena, &kTellSweepArena, &kTellSlamArena, &kTellShotArena};
static_assert(UE_ARRAY_COUNT(kArenaTellWords) == ftcore::kArenaTellCount, "a word for every ArenaTell");
const FTUiText kBossWeightArena(TEXT("arena.boss.weight"));
const FTUiText kBossInvulnerableArena(TEXT("arena.boss.invulnerable"));
const FTUiText kBossWeakPointArena(TEXT("arena.boss.weak_point"));
const FTUiText kBossCallArena(TEXT("arena.boss.call"));
const FTUiText kBossLeftArena(TEXT("arena.boss.left"));
const FTUiText kBossRightArena(TEXT("arena.boss.right"));
const FTUiText kBossPathArena(TEXT("arena.boss.path"));
const FTUiText kBossInterceptArena(TEXT("arena.boss.intercept"));
const FTUiText kBossGuardArena(TEXT("arena.boss.guard"));
const FTUiText kBossWrongSideArena(TEXT("arena.boss.wrong_side"));
const FTUiText kBossMissedArena(TEXT("arena.boss.missed"));
const FTUiText kBossBrokenArena(TEXT("arena.boss.broken"));
const FTUiText kBossShockRingArena(TEXT("arena.boss.shock_ring"));
const FTUiText kBossCriticalArena(TEXT("arena.boss.critical"));
const FTUiText kBossLiftedArena(TEXT("arena.boss.lifted"));
const FTUiText kBossReleaseOnCallArena(TEXT("arena.boss.release_on_call"));
const FTUiText kNowArena(TEXT("battle.now"));
const FTUiText kStaggeredArena(TEXT("battle.staggered"));
const FTUiText kDownArena(TEXT("battle.down"));
const FTUiText kVictoryArena(TEXT("battle.victory"));
const FTUiText kDefeatArena(TEXT("battle.defeat"));
const FTUiText kAgainArena(TEXT("battle.again"));

constexpr double kArenaPopupLife = 0.9;
constexpr double kArenaImpactLife = 0.3;  // the tell's flash once the move is active
constexpr double kArenaOnsetLife = 0.35;  // an off-frame windup marker's onset swell
constexpr double kArenaHitEdgeLife = 0.45; // the red edge when the player is hit
constexpr double kArenaParryEdgeLife = 0.4;
constexpr double kArenaResultFade = 0.3;
constexpr double kArenaReleaseBanner = 1.0;
constexpr double kArenaOverloadBanner = 1.4;
constexpr double kArenaBossWordLife = 1.2; // the boss panel's latest answer or rule, fading

// The boss panel's word for a note, and its colour; null for a note it doesn't show.
const FTUiText* ArenaBossWord(const FFTStoneBearerNote& Note, FLinearColor& OutColor)
{
	if (Note.Kind == FFTStoneBearerNote::EKind::ShockRingSent)
	{
		OutColor = kArenaOrange;
		return &kBossShockRingArena;
	}
	if (Note.Kind != FFTStoneBearerNote::EKind::Run)
	{
		return nullptr;
	}
	switch (Note.Event.type)
	{
	case ftcore::ArenaBossEventType::Answered:
		switch (Note.Event.outcome)
		{
		case ftcore::CalledOutcome::Intercepted:
			OutColor = kArenaGreen;
			return &kBossInterceptArena;
		case ftcore::CalledOutcome::Guarded:
			OutColor = kArenaCyan;
			return &kBossGuardArena;
		case ftcore::CalledOutcome::WrongZone:
			OutColor = kArenaRed;
			return &kBossWrongSideArena;
		case ftcore::CalledOutcome::Missed:
			OutColor = kArenaRed;
			return &kBossMissedArena;
		default:
			return nullptr;
		}
	case ftcore::ArenaBossEventType::Broke:
		OutColor = kArenaGold;
		return &kBossBrokenArena;
	case ftcore::ArenaBossEventType::Crit:
		OutColor = kArenaGold;
		return &kBossCriticalArena;
	case ftcore::ArenaBossEventType::Lifted:
		OutColor = kArenaGold;
		return &kBossLiftedArena;
	default:
		return nullptr;
	}
}

FLinearColor ArenaAlpha(const FLinearColor& C, float A)
{
	return FLinearColor(C.R, C.G, C.B, C.A * A);
}

float ArenaPulse(double Now, double Time, double Life)
{
	const double Age = Now - Time;
	return (Age >= 0.0 && Age < Life) ? static_cast<float>(1.0 - Age / Life) : 0.f;
}

// "value/max" (Second >= 0), "+value" (Plus), or the value alone; rebuilt only when the numbers change.
const FString& ArenaNumberOf(FArenaNumberText& Cache, int32 Value, int32 Second = -1, bool bPlus = false)
{
	if (Value != Cache.Value || Second != Cache.Second)
	{
		Cache.Value = Value;
		Cache.Second = Second;
		Cache.Text.Reset();
		if (bPlus)
		{
			Cache.Text += TEXT("+");
		}
		Cache.Text.AppendInt(Value);
		if (Second >= 0)
		{
			Cache.Text += TEXT("/");
			Cache.Text.AppendInt(Second);
		}
	}
	return Cache.Text;
}

// The colour and the word of the answer a move asks for.
void ArenaAnswer(const ftcore::ArenaEnemyMove& Move, FLinearColor& OutColor, const FString*& OutWord)
{
	if (Move.unblockable)
	{
		OutColor = kArenaTellDodge;
		OutWord = &kDodgeArena.Get();
	}
	else if (!Move.parryable)
	{
		OutColor = kArenaTellBlock;
		OutWord = &kBlockArena.Get();
	}
	else
	{
		OutColor = kArenaTellParry;
		OutWord = &kParryArena.Get();
	}
}
} // namespace

AFTArenaHUD::AFTArenaHUD()
{
	StatsText.Reserve(160);
	HpText.Text.Reserve(16);
	TargetText.Text.Reserve(16);
	for (FArenaNumberText& P : PopupText)
	{
		P.Text.Reserve(16);
	}
}

void AFTArenaHUD::DrawStrCentered(const FString& Text, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	GetTextSize(Text, W, H, Font, Scale * UiScale);
	// A dark copy under it, so pale words read on the pale stone.
	const float Off = 2.f * UiScale;
	DrawText(Text, FLinearColor(0.f, 0.f, 0.f, Color.A * 0.75f), X - W * 0.5f + Off, Y + Off, Font, Scale * UiScale);
	DrawText(Text, Color, X - W * 0.5f, Y, Font, Scale * UiScale);
}

void AFTArenaHUD::DrawRing(float X, float Y, float Radius, const FLinearColor& Color, float Thickness)
{
	constexpr int32 Segments = 40;
	const float Turn = 6.2831853f / static_cast<float>(Segments);
	float PrevX = X + Radius;
	float PrevY = Y;
	for (int32 I = 1; I <= Segments; ++I)
	{
		const float A = Turn * static_cast<float>(I);
		const float NextX = X + Radius * FMath::Cos(A);
		const float NextY = Y + Radius * FMath::Sin(A);
		DrawLine(PrevX, PrevY, NextX, NextY, Color, Thickness);
		PrevX = NextX;
		PrevY = NextY;
	}
}

void AFTArenaHUD::DrawFrame(float X, float Y, float W, float H, const FLinearColor& Color, float Thickness)
{
	DrawRect(Color, X, Y, W, Thickness);
	DrawRect(Color, X, Y + H - Thickness, W, Thickness);
	DrawRect(Color, X, Y, Thickness, H);
	DrawRect(Color, X + W - Thickness, Y, Thickness, H);
}

void AFTArenaHUD::DrawBar(float X, float Y, float W, float H, float Fraction, const FLinearColor& Fill, const FLinearColor& Back)
{
	DrawRect(Back, X, Y, W, H);
	if (Fraction > 0.f)
	{
		DrawRect(Fill, X, Y, W * FMath::Clamp(Fraction, 0.f, 1.f), H);
	}
}

bool AFTArenaHUD::ToScreen(const FVector& World, FVector2D& Out)
{
	const FVector P = Project(World, true);
	if (P.Z <= 0.0)
	{
		return false;
	}
	Out = FVector2D(P.X, P.Y);
	return true;
}

bool AFTArenaHUD::OnScreen(const FVector2D& P, float Margin) const
{
	return P.X >= Margin && P.Y >= Margin && P.X <= Canvas->ClipX - Margin && P.Y <= Canvas->ClipY - Margin;
}

void AFTArenaHUD::DrawPlayerPanel(const UFTArenaRunner& Mode)
{
	const float S = UiScale;
	const ftcore::ArenaFight& Fight = Mode.GetFight();
	const ftcore::ArenaFighter& F = Fight.def().fighter;
	const float X = 40.f * S;
	const float Y = Canvas->ClipY - 230.f * S;
	const float W = 340.f * S;
	UFont* Small = GEngine->GetSmallFont();
	DrawRect(kArenaPanel, X - 14.f * S, Y - 54.f * S, W + 150.f * S, 226.f * S);
	const float Top = Y - 40.f * S; // hp and stamina sit above the Load row

	// hp, with the number beside it; the bar goes red below a third.
	const float Hp = F.hp > 0 ? static_cast<float>(Fight.hp()) / static_cast<float>(F.hp) : 0.f;
	DrawText(kHpArena, kArenaDim, X, Top, Small, 1.1f * S);
	DrawBar(X, Top + 20.f * S, W, 22.f * S, Hp, Hp < 0.34f ? kArenaRed : kArenaGreen, kArenaHpBack);
	DrawText(ArenaNumberOf(HpText, Fight.hp(), F.hp), kArenaWhite, X + W + 12.f * S, Top + 20.f * S, GEngine->GetMediumFont(), 1.f * S);

	// stamina: dim while empty
	const float Stamina = F.stamina > 0.f ? Fight.stamina() / F.stamina : 0.f;
	DrawText(kStaminaArena, kArenaDim, X, Top + 50.f * S, Small, 1.1f * S);
	DrawBar(X, Top + 70.f * S, W, 12.f * S, Stamina, Stamina < 0.05f ? kArenaDim : kArenaStamina, kArenaBack);

	// The ally (carrying runs only): a thin bar under the hp, labelled ALLY with its hp after it, dark once it is down.
	if (Fight.allyMaxHp() > 0)
	{
		const float Ally = static_cast<float>(Fight.allyHp()) / static_cast<float>(Fight.allyMaxHp());
		const FLinearColor AllyColor = Fight.allyAlive() ? kArenaGreen : kArenaDim;
		DrawBar(X, Top + 44.f * S, W * 0.6f, 5.f * S, Ally, AllyColor, kArenaBack);
		DrawText(kAllyArena, kArenaDim, X + W * 0.6f + 10.f * S, Top + 40.f * S, Small, 0.9f * S);
		DrawText(ArenaNumberOf(AllyText, Fight.allyHp(), Fight.allyMaxHp()), AllyColor, X + W * 0.6f + 50.f * S, Top + 40.f * S, Small,
			0.9f * S);
	}

	// Load, beside stamina and never in its place: a mark at a third of the cap (the Release opens above it), RELEASE
	// READY once past it, OVERLOADED (pulsing) at the cap with the verbs it locks. Under a cap below 100 (the named
	// places, carrying runs only) a notch stands at the cap and the bar past it is dimmed.
	const int32 Load = Fight.load();
	const int32 Cap = Fight.loadCap();
	const int32 ReleaseAt = Fight.releaseOpenLoad();
	const float LoadShare = static_cast<float>(Load) / static_cast<float>(ftcore::kMaxLoad);
	const bool bOver = Fight.overloaded();
	const float Pulse = bOver ? 0.65f + 0.35f * FMath::Sin(static_cast<float>(Mode.GetNow()) * 10.f) : 1.f;
	const FLinearColor LoadColor = bOver ? FLinearColor(1.f, 0.2f * Pulse, 0.1f, 1.f) : kArenaLoad;
	DrawText(kLoadArena, kArenaDim, X, Y + 50.f * S, Small, 1.1f * S);
	DrawText(ArenaNumberOf(LoadText, Load), LoadColor, X + 60.f * S, Y + 50.f * S, Small, 1.1f * S);
	if (bOver)
	{
		DrawText(kOverloadedArena, LoadColor, X + 110.f * S, Y + 50.f * S, Small, 1.1f * S);
		DrawText(kOverloadLockedArena, kArenaWhite, X + 215.f * S, Y + 50.f * S, Small, 1.1f * S);
	}
	else if (Load > ReleaseAt)
	{
		DrawText(kReleaseReadyArena, kArenaLoad, X + 110.f * S, Y + 50.f * S, Small, 1.1f * S);
	}
	DrawBar(X, Y + 70.f * S, W, 12.f * S, LoadShare, LoadColor, kArenaBack);
	if (Cap < ftcore::kMaxLoad)
	{
		const float CapX = X + W * static_cast<float>(Cap) / static_cast<float>(ftcore::kMaxLoad);
		DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.6f), CapX, Y + 70.f * S, X + W - CapX, 12.f * S);
		DrawRect(kArenaLoad, CapX - 1.5f * S, Y + 64.f * S, 3.f * S, 24.f * S);
	}
	const float Mark = X + W * static_cast<float>(ReleaseAt) / static_cast<float>(ftcore::kMaxLoad);
	DrawRect(kArenaWhite, Mark - 1.f * S, Y + 66.f * S, 2.f * S, 20.f * S);
	// The named chairs: a pip each at the bar's end, coloured by what the chair gives.
	const ftcore::ArenaChairs& Chairs = Fight.chairs();
	for (int32 I = 0; I < static_cast<int32>(Chairs.count); ++I)
	{
		const ftcore::ChairKind Kind = Chairs.named[static_cast<size_t>(I)].kind;
		const FLinearColor Pip = Kind == ftcore::ChairKind::LoadGain ? kArenaLoad
			: Kind == ftcore::ChairKind::HealCharges ? kArenaGreen : kArenaWhite;
		DrawRect(Pip, X + W + 12.f * S + static_cast<float>(I) * 14.f * S, Y + 70.f * S, 10.f * S, 10.f * S);
	}

	const float Poise = F.poise > 0.f ? Fight.poise() / F.poise : 0.f;
	DrawText(kPoiseArena, kArenaDim, X, Y + 90.f * S, Small, 1.1f * S);
	DrawBar(X, Y + 110.f * S, W, 7.f * S, Poise, kArenaPoise, kArenaBack);

	// the heal charges: a pip each, filled while unused
	DrawText(kHealsArena, kArenaDim, X, Y + 126.f * S, Small, 1.1f * S);
	const int32 Left = static_cast<int32>(Fight.healCharges());
	for (int32 I = 0; I < static_cast<int32>(Fight.healChargesMax()); ++I)
	{
		const float PX = X + 90.f * S + static_cast<float>(I) * 28.f * S;
		const float PY = Y + 128.f * S;
		if (I < Left)
		{
			DrawRect(kArenaGreen, PX, PY, 20.f * S, 20.f * S);
		}
		else
		{
			DrawFrame(PX, PY, 20.f * S, 20.f * S, kArenaDim, 2.f * S);
		}
	}

	// the Read stacks: a pip each, lit while held
	const int32 Stacks = Fight.readStacks();
	DrawText(kReadStacksArena, kArenaDim, X + 250.f * S, Y + 126.f * S, Small, 1.1f * S);
	for (int32 I = 0; I < ftcore::kMaxReadStacks; ++I)
	{
		const float PX = X + 320.f * S + static_cast<float>(I) * 22.f * S;
		const float PY = Y + 130.f * S;
		if (I < Stacks)
		{
			DrawRect(kArenaCyan, PX, PY, 16.f * S, 16.f * S);
		}
		else
		{
			DrawFrame(PX, PY, 16.f * S, 16.f * S, kArenaDim, 2.f * S);
		}
	}
}

void AFTArenaHUD::DrawEnemies(const UFTArenaRunner& Mode)
{
	const float S = UiScale;
	const ftcore::ArenaFight& Fight = Mode.GetFight();
	for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()); ++I)
	{
		const AFTArenaFighter* Figure = Mode.GetEnemyFigure(I);
		if (!Figure || !Fight.enemyAlive(static_cast<size_t>(I)))
		{
			continue;
		}
		const ftcore::ArenaEnemyKind& Kind = Fight.def().enemies[static_cast<size_t>(I)];
		FVector2D Over;
		FVector2D Chest;
		const bool bOver = ToScreen(Figure->GetAnchor(), Over);
		const bool bChest = ToScreen(Figure->GetChest(), Chest);
		const bool bOnScreen = bOver && bChest && OnScreen(Chest, 0.f);
		if (bOnScreen)
		{
			const float W = 130.f * S;
			const float Hp = Kind.hp > 0 ? static_cast<float>(Fight.enemyHp(static_cast<size_t>(I))) / static_cast<float>(Kind.hp) : 0.f;
			const float Poise = Kind.poise > 0.f ? Fight.enemyPoise(static_cast<size_t>(I)) / Kind.poise : 0.f;
			DrawBar(Over.X - W * 0.5f, Over.Y, W, 10.f * S, Hp, kArenaRed, kArenaHpBack);
			DrawBar(Over.X - W * 0.5f, Over.Y + 12.f * S, W, 5.f * S, Poise, kArenaEnemyPoise, kArenaBack);
			const ftcore::ArenaBrain Brain = Fight.output().enemies[static_cast<size_t>(I)].brain;
			if (Brain == ftcore::ArenaBrain::Staggered)
			{
				DrawStrCentered(kStaggeredArena, kArenaOrange, Over.X, Over.Y + 20.f * S, GEngine->GetSmallFont(), 1.2f);
			}
			else if (Brain == ftcore::ArenaBrain::Open)
			{
				DrawStrCentered(kRiposteArena, kArenaGold, Over.X, Over.Y + 20.f * S, GEngine->GetSmallFont(), 1.3f);
			}
		}
		DrawTell(Mode, I, Over, Chest, bOnScreen);
	}
}

void AFTArenaHUD::DrawTell(const UFTArenaRunner& Mode, int32 Enemy, const FVector2D& Over, const FVector2D& Chest, bool bOnScreen)
{
	const ftcore::ArenaEnemyMove* Move = Mode.GetEnemyMove(Enemy);
	if (!Move)
	{
		return;
	}
	const float S = UiScale;
	const double Now = Mode.GetNow();
	const ftcore::ArenaEnemyOut& E = Mode.GetFight().output().enemies[static_cast<size_t>(Enemy)];
	FLinearColor Color;
	const FString* Word = nullptr;
	ArenaAnswer(*Move, Color, Word);
	const float Impact = ArenaPulse(Now, Mode.GetEnemyActiveSince(Enemy), kArenaImpactLife);
	const bool bStartup = E.part == ftcore::ArenaPart::Startup;
	if (!bStartup && Impact <= 0.f)
	{
		return;
	}
	const float Fill = bStartup ? FMath::Clamp(E.stateTime / FMath::Max(Move->attack.startup, 0.01f), 0.f, 1.f) : 1.f;

	const ftcore::ArenaFight& Fight = Mode.GetFight();
	const bool bCards = bStartup && Fight.readFoe() == static_cast<uint8>(Enemy) && Fight.readCards().dealt();
	// Out of the camera's view (the runner's off-frame test, the one the stinger answers to), or off the screen.
	const bool bOffFrame = bStartup && Mode.IsOffFrame(Enemy);
	if (!bOnScreen || bOffFrame)
	{
		if (bCards)
		{
			// The cards still want a pick: bottom centre, over the key hint.
			DrawReadCards(Mode, Canvas->ClipX * 0.5f, Canvas->ClipY - 70.f * S);
		}
		// Off the screen: an arrow at the edge, toward the enemy, in the tell's colour (white at impact). Through a
		// windup it also swells at the onset (a steady ring with reduce flashes on), fills toward the hit and names the
		// answer: the marker is the tell, the stinger only doubles it.
		const AFTArenaFighter* Figure = Mode.GetEnemyFigure(Enemy);
		const APlayerController* PC = GetOwningPlayerController();
		if (Figure && PC && PC->PlayerCameraManager)
		{
			const FVector Local = PC->PlayerCameraManager->GetCameraRotation().UnrotateVector(
				Figure->GetChest() - PC->PlayerCameraManager->GetCameraLocation());
			if (bStartup)
			{
				const ftcore::Settings* Settings = Mode.GetSettings();
				const bool bCalm = Settings && Settings->reduceFlashes;
				const float Onset = bOffFrame ? ArenaPulse(Now, Mode.GetOffFrameSince(Enemy), kArenaOnsetLife) : 0.f;
				DrawEdgeArrow(FVector2D(Local.Y, -Local.Z), Color, bCalm ? 0.f : Onset, Fill, Word);
			}
			else
			{
				DrawEdgeArrow(FVector2D(Local.Y, -Local.Z), kArenaWhite);
			}
		}
		return;
	}

	// The bar over the enemy: it fills through the startup and the hit lands as it reaches the end.
	const float W = 170.f * S;
	const float H = 14.f * S;
	const float BX = Over.X - W * 0.5f;
	const float BY = Over.Y - 30.f * S;
	if (bStartup)
	{
		DrawBar(BX, BY, W, H, Fill, Color, kArenaBack);
		DrawRect(kArenaWhite, BX + W - 3.f * S, BY - 4.f * S, 3.f * S, H + 8.f * S);
		DrawFrame(BX, BY, W, H, ArenaAlpha(Color, 0.9f), 2.f * S);
		// The Read cards sit on the bar, the answer above them.
		float WordY = BY - 44.f * S;
		if (bCards)
		{
			DrawReadCards(Mode, Over.X, BY - 8.f * S);
			WordY -= 62.f * S;
		}
		DrawStrCentered(*Word, Color, Over.X, WordY, GEngine->GetLargeFont(), 1.1f);
		// A ring closes in on the body, meeting it at the hit.
		const float Inner = 38.f * S;
		DrawRing(Chest.X, Chest.Y, Inner, ArenaAlpha(Color, 0.55f), 2.f * S);
		DrawRing(Chest.X, Chest.Y, FMath::Lerp(150.f * S, Inner, Fill), ArenaAlpha(Color, 0.35f + 0.65f * Fill), 4.f * S);
	}
	else
	{
		// Impact: everything flashes white, a burst spreads from the body.
		DrawBar(BX, BY, W, H, 1.f, ArenaAlpha(kArenaWhite, 0.4f + 0.6f * Impact), kArenaBack);
		DrawStrCentered(kNowArena, ArenaAlpha(kArenaWhite, Impact), Over.X, BY - 40.f * S, GEngine->GetLargeFont(), 1.4f);
		const float Spread = 1.f - Impact;
		DrawRing(Chest.X, Chest.Y, (38.f + 90.f * Spread) * S, ArenaAlpha(kArenaWhite, Impact), (8.f * Impact + 2.f) * S);
		DrawRing(Chest.X, Chest.Y, (30.f + 50.f * Spread) * S, ArenaAlpha(Color, Impact), 4.f * S);
	}
}

void AFTArenaHUD::DrawReadCards(const UFTArenaRunner& Mode, float X, float Bottom)
{
	static const FString KeyLabels[ftcore::kReadCards] = {TEXT("1"), TEXT("2"), TEXT("3")};
	const float S = UiScale;
	const ftcore::ArenaFight& Fight = Mode.GetFight();
	const ftcore::ReadCards& Cards = Fight.readCards();
	const int32 Count = FMath::Min(static_cast<int32>(Cards.size()), static_cast<int32>(ftcore::kReadCards));
	const bool bPicked = Cards.picked();
	const float CW = 104.f * S;
	const float CH = 50.f * S;
	const float Gap = 8.f * S;
	const float Left = X - (static_cast<float>(Count) * CW + static_cast<float>(Count - 1) * Gap) * 0.5f;
	const float Top = Bottom - CH;
	for (int32 I = 0; I < Count; ++I)
	{
		const ftcore::ReadCard& Card = Cards[static_cast<size_t>(I)];
		uint8 Tell = 0;
		if (Card.actor < Fight.enemyCount())
		{
			const ftcore::ArenaEnemyKind& Kind = Fight.def().enemies[Card.actor];
			if (Card.action < Kind.moveCount)
			{
				Tell = static_cast<uint8>(Kind.moves[Card.action].tell);
			}
		}
		const bool bThis = bPicked && Cards.pickedIndex() == static_cast<size_t>(I);
		const bool bRight = bThis && Cards.correct() == static_cast<size_t>(I);
		const FLinearColor Edge = bThis ? (bRight ? kArenaGreen : kArenaRed) : (bPicked ? kArenaDim : kArenaCyan);
		const FLinearColor Ink = bPicked && !bThis ? kArenaDim : kArenaWhite;
		const float CX = Left + static_cast<float>(I) * (CW + Gap);
		DrawRect(bThis ? ArenaAlpha(Edge, 0.35f) : kArenaPanel, CX, Top, CW, CH);
		DrawFrame(CX, Top, CW, CH, Edge, (bThis ? 3.f : 2.f) * S);
		DrawText(KeyLabels[I], Edge, CX + 6.f * S, Top + 3.f * S, GEngine->GetSmallFont(), 1.1f * S);
		DrawStrCentered(*kArenaTellWords[Tell < ftcore::kArenaTellCount ? Tell : 0], Ink, CX + CW * 0.5f, Top + 18.f * S,
			GEngine->GetMediumFont(), 0.95f);
	}
}

void AFTArenaHUD::DrawEdgeArrow(const FVector2D& Toward, const FLinearColor& Color, float Pulse, float Fill, const FString* Word)
{
	const float S = UiScale;
	FVector2D Dir = Toward;
	if (Dir.SizeSquared() < 1e-6)
	{
		Dir = FVector2D(0.0, 1.0);
	}
	Dir.Normalize();
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	// Where the ray from the centre meets a frame 70 px inside the screen's edge.
	const float HalfW = CX - 70.f * S;
	const float HalfH = CY - 70.f * S;
	const float T = FMath::Min(Dir.X != 0.0 ? HalfW / FMath::Abs(static_cast<float>(Dir.X)) : 1e9f,
		Dir.Y != 0.0 ? HalfH / FMath::Abs(static_cast<float>(Dir.Y)) : 1e9f);
	const float TipX = CX + static_cast<float>(Dir.X) * T;
	const float TipY = CY + static_cast<float>(Dir.Y) * T;
	Pulse = FMath::Clamp(Pulse, 0.f, 1.f);
	const float Len = 44.f * S * (1.f + 0.35f * Pulse);
	const float BackX = TipX - static_cast<float>(Dir.X) * Len;
	const float BackY = TipY - static_cast<float>(Dir.Y) * Len;
	const float SideX = -static_cast<float>(Dir.Y) * Len * 0.55f;
	const float SideY = static_cast<float>(Dir.X) * Len * 0.55f;
	DrawLine(TipX, TipY, BackX + SideX, BackY + SideY, Color, 6.f * S);
	DrawLine(TipX, TipY, BackX - SideX, BackY - SideY, Color, 6.f * S);
	DrawLine(BackX + SideX, BackY + SideY, BackX - SideX, BackY - SideY, Color, 4.f * S);
	const float RingX = TipX - static_cast<float>(Dir.X) * Len * 0.45f;
	const float RingY = TipY - static_cast<float>(Dir.Y) * Len * 0.45f;
	DrawRing(RingX, RingY, Len * 0.9f, ArenaAlpha(Color, 0.5f), 3.f * S);
	if (Pulse > 0.f)
	{
		// The onset: a ring spreading out from the marker as it fades.
		DrawRing(RingX, RingY, Len * (0.9f + 1.4f * (1.f - Pulse)), ArenaAlpha(Color, Pulse), 6.f * S * Pulse + 2.f * S);
	}
	if (Fill < 0.f && !Word)
	{
		return;
	}
	// Beside the marker, toward the screen's centre: the answer, and under it the windup filling toward the hit.
	const float InX = RingX - static_cast<float>(Dir.X) * (Len * 0.9f + 64.f * S);
	const float InY = RingY - static_cast<float>(Dir.Y) * (Len * 0.9f + 40.f * S);
	if (Word)
	{
		DrawStrCentered(*Word, Color, InX, InY - 26.f * S, GEngine->GetLargeFont(), 0.9f);
	}
	if (Fill >= 0.f)
	{
		const float BW = 96.f * S;
		const float BH = 9.f * S;
		DrawBar(InX - BW * 0.5f, InY + 4.f * S, BW, BH, FMath::Clamp(Fill, 0.f, 1.f), Color, kArenaBack);
		DrawRect(kArenaWhite, InX + BW * 0.5f - 2.f * S, InY + 1.f * S, 2.f * S, BH + 6.f * S);
	}
}

void AFTArenaHUD::DrawTargetBar(const UFTArenaRunner& Mode)
{
	const ftcore::ArenaFight& Fight = Mode.GetFight();
	int32 Target = Fight.lock() != ftcore::kArenaNobody ? static_cast<int32>(Fight.lock()) : -1;
	if (Target < 0)
	{
		// The nearest living enemy.
		const AFTArenaFighter* Player = Mode.GetPlayerFigure();
		double Best = 1e18;
		for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()); ++I)
		{
			const AFTArenaFighter* Figure = Mode.GetEnemyFigure(I);
			if (Figure && Player && Fight.enemyAlive(static_cast<size_t>(I)))
			{
				const double D = FVector::DistSquared(Figure->GetActorLocation(), Player->GetActorLocation());
				if (D < Best)
				{
					Best = D;
					Target = I;
				}
			}
		}
	}
	if (Target < 0 || Target >= static_cast<int32>(Fight.enemyCount()))
	{
		return;
	}
	const float S = UiScale;
	const ftcore::ArenaEnemyKind& Kind = Fight.def().enemies[static_cast<size_t>(Target)];
	const int32 Hp = Fight.enemyHp(static_cast<size_t>(Target));
	const float W = 640.f * S;
	const float X = Canvas->ClipX * 0.5f - W * 0.5f;
	const float Y = 46.f * S;
	DrawRect(kArenaPanel, X - 12.f * S, Y - 32.f * S, W + 24.f * S, 76.f * S);
	DrawText(kTargetArena, kArenaDim, X, Y - 26.f * S, GEngine->GetSmallFont(), 1.1f * S);
	const FString& Number = ArenaNumberOf(TargetText, Hp, Kind.hp);
	float TW = 0.f;
	float TH = 0.f;
	GetTextSize(Number, TW, TH, GEngine->GetSmallFont(), 1.1f * S);
	DrawText(Number, kArenaWhite, X + W - TW, Y - 26.f * S, GEngine->GetSmallFont(), 1.1f * S);
	DrawBar(X, Y, W, 18.f * S, Kind.hp > 0 ? static_cast<float>(Hp) / static_cast<float>(Kind.hp) : 0.f, kArenaRed, kArenaHpBack);
	DrawBar(X, Y + 22.f * S, W, 8.f * S, Kind.poise > 0.f ? Fight.enemyPoise(static_cast<size_t>(Target)) / Kind.poise : 0.f,
		kArenaEnemyPoise, kArenaBack);
}

void AFTArenaHUD::DrawBossPanel(const UFTArenaRunner& Mode)
{
	const FFTStoneBearerHost* Host = Mode.GetStoneBearer();
	const ftcore::ArenaFight& Fight = Mode.GetFight();
	if (!Host || !Fight.enemyAlive(Host->GetSlot()))
	{
		return;
	}
	const ftcore::StoneBearer& Boss = Host->GetBoss();
	const float S = UiScale;
	UFont* Small = GEngine->GetSmallFont();
	const float W = 640.f * S;
	const float X = Canvas->ClipX * 0.5f - W * 0.5f;
	const float Y = 100.f * S;
	const double T = Fight.time();
	DrawRect(kArenaPanel, X - 12.f * S, Y - 6.f * S, W + 24.f * S, 34.f * S);

	// The stacks, as pips.
	DrawText(kBossWeightArena, kArenaDim, X, Y, Small, 1.1f * S);
	const int32 Cap = FMath::Clamp(Host->GetDef().weight.cap, 0, 8);
	for (int32 I = 0; I < Cap; ++I)
	{
		const float PX = X + 86.f * S + static_cast<float>(I) * 22.f * S;
		if (I < Boss.stacks())
		{
			DrawRect(kArenaLoad, PX, Y + 2.f * S, 16.f * S, 16.f * S);
		}
		else
		{
			DrawFrame(PX, Y + 2.f * S, 16.f * S, 16.f * S, kArenaDim, 2.f * S);
		}
	}

	// What it is doing: on its plinth, open, down, or calling a blow (green when the lead stands in the called zone).
	const float Mid = Canvas->ClipX * 0.5f + 40.f * S;
	const ftcore::ArenaBossRun& Run = Boss.run();
	const ftcore::CalledStrike& Strike = Run.strike();
	const std::uint8_t Live = Run.liveMove();
	if (Boss.invulnerable())
	{
		DrawStrCentered(kBossInvulnerableArena, kArenaCyan, Mid - 60.f * S, Y, Small, 1.2f);
		DrawStrCentered(kBossReleaseOnCallArena, kArenaWhite, Mid + 90.f * S, Y, Small, 1.1f);
	}
	else if (Run.crit().isOpen(Run.now()))
	{
		DrawStrCentered(kBossWeakPointArena, ArenaAlpha(kArenaGold, 0.7f + 0.3f * FMath::Sin(static_cast<float>(Mode.GetNow()) * 10.f)), Mid, Y, Small, 1.2f);
	}
	else if (Boss.down())
	{
		DrawStrCentered(kDownArena, kArenaWhite, Mid, Y, Small, 1.2f);
	}
	else if (Live < Host->GetDef().boss.moveCount && Host->GetDef().boss.moves[Live].intercept() && !Strike.complete())
	{
		const ftcore::CalledZone Want = Strike.nextZone();
		const bool bIn = Host->ZoneNow(Mode.GetBodies()) == Want;
		const FTUiText& Zone = Want == ftcore::CalledZone::Left ? kBossLeftArena : (Want == ftcore::CalledZone::Right ? kBossRightArena : kBossPathArena);
		DrawStrCentered(kBossCallArena, kArenaDim, Mid - 50.f * S, Y, Small, 1.2f);
		DrawStrCentered(Zone, bIn ? kArenaGreen : kArenaRed, Mid + 30.f * S, Y, Small, 1.2f);
	}

	// The latest answer or rule, fading.
	for (int32 Serial = Host->GetNoteTotal() - 1; Serial >= Host->GetNoteTotal() - Host->GetNoteCount(); --Serial)
	{
		const FFTStoneBearerNote& Note = Host->GetNoteBySerial(Serial);
		const float Fade = ArenaPulse(T, Note.Event.at, kArenaBossWordLife);
		if (Fade <= 0.f)
		{
			break;
		}
		FLinearColor Color = kArenaWhite;
		if (const FTUiText* Word = ArenaBossWord(Note, Color))
		{
			DrawStrCentered(*Word, ArenaAlpha(Color, Fade), X + W - 80.f * S, Y, Small, 1.2f);
			break;
		}
	}
}

void AFTArenaHUD::DrawLock(const UFTArenaRunner& Mode)
{
	const ftcore::ArenaFight& Fight = Mode.GetFight();
	const uint8 Lock = Fight.lock();
	if (Lock == ftcore::kArenaNobody)
	{
		return;
	}
	const AFTArenaFighter* Figure = Mode.GetEnemyFigure(static_cast<int32>(Lock));
	FVector2D C;
	if (!Figure || !ToScreen(Figure->GetChest(), C) || !OnScreen(C, 0.f))
	{
		return;
	}
	// Four corner brackets around the body and a dot on it, breathing a little.
	const float S = UiScale;
	const float R = (58.f + 5.f * FMath::Sin(static_cast<float>(Mode.GetNow()) * 5.f)) * S;
	const float L = 20.f * S;
	const float T = 3.f * S;
	const FLinearColor Color = ArenaAlpha(kArenaWhite, 0.9f);
	for (int32 SX = -1; SX <= 1; SX += 2)
	{
		for (int32 SY = -1; SY <= 1; SY += 2)
		{
			const float X = C.X + static_cast<float>(SX) * R;
			const float Y = C.Y + static_cast<float>(SY) * R * 1.3f;
			DrawLine(X, Y, X - static_cast<float>(SX) * L, Y, Color, T);
			DrawLine(X, Y, X, Y - static_cast<float>(SY) * L, Color, T);
		}
	}
	DrawRect(Color, C.X - 4.f * S, C.Y - 4.f * S, 8.f * S, 8.f * S);
}

void AFTArenaHUD::DrawPopups(const UFTArenaRunner& Mode)
{
	const float S = UiScale;
	const double Now = Mode.GetNow();
	const int32 Count = Mode.GetPopupCount();
	const int32 First = Mode.GetPopupTotal() - Count;
	for (int32 I = 0; I < Count; ++I)
	{
		const UFTArenaRunner::FPopup& P = Mode.GetPopup(I);
		const float Life = ArenaPulse(Now, P.Time, kArenaPopupLife);
		if (Life <= 0.f)
		{
			continue;
		}
		FVector2D At;
		if (!ToScreen(P.At + FVector(0.0, 0.0, 210.0), At))
		{
			continue;
		}
		const int32 Serial = First + I;
		FArenaNumberText& Cache = PopupText[Serial % UFTArenaRunner::MaxPopups];
		const float Rise = (1.f - Life) * 70.f * S;
		const float Y = At.Y - 40.f * S - Rise;
		// Numbers stand beside the body (right of an enemy, left of the player), clear of its bars and its tell.
		const float Side = (P.Target == ftcore::kArenaPlayer ? -110.f : 110.f) * S;
		const FLinearColor Fade(1.f, 1.f, 1.f, FMath::Min(1.f, Life * 2.f));
		const bool bOnPlayer = P.Target == ftcore::kArenaPlayer;
		switch (P.Type)
		{
		case ftcore::ArenaEventType::Hit:
		{
			// Keyed by the popup's serial as well as its amount, so a slot's text follows its own popup.
			if (Cache.Second != Serial || Cache.Value != P.Amount)
			{
				Cache.Value = P.Amount;
				Cache.Second = Serial;
				Cache.Text.Reset();
				Cache.Text.AppendInt(P.Amount);
			}
			const bool bRiposte = P.Source == ftcore::kArenaPlayer && P.Move == ftcore::kArenaRiposteMove;
			const FLinearColor C = bOnPlayer ? kArenaRed : (bRiposte ? kArenaGold : kArenaWhite);
			DrawStrCentered(Cache.Text, C * Fade, At.X + Side, Y + 40.f * S, GEngine->GetLargeFont(), bRiposte ? 2.1f : 1.6f);
			break;
		}
		case ftcore::ArenaEventType::Healed:
			if (Cache.Second != Serial || Cache.Value != P.Amount)
			{
				Cache.Value = P.Amount;
				Cache.Second = Serial;
				Cache.Text.Reset();
				Cache.Text += TEXT("+");
				Cache.Text.AppendInt(P.Amount);
			}
			DrawStrCentered(Cache.Text, kArenaGreen * Fade, At.X + Side, Y + 40.f * S, GEngine->GetLargeFont(), 1.6f);
			break;
		case ftcore::ArenaEventType::Blocked:
			DrawStrCentered(kBlockedArena, kArenaCyan * Fade, At.X, Y - 30.f * S, GEngine->GetMediumFont(), 1.1f);
			break;
		case ftcore::ArenaEventType::GuardBroken:
			DrawStrCentered(kGuardBrokenArena, kArenaRed * Fade, At.X, Y - 60.f * S, GEngine->GetMediumFont(), 1.3f);
			break;
		case ftcore::ArenaEventType::Parried:
			DrawStrCentered(kParriedArena, kArenaGold * Fade, At.X, Y - 30.f * S, GEngine->GetLargeFont(), 1.2f);
			break;
		case ftcore::ArenaEventType::Evaded:
			DrawStrCentered(kEvadedArena, kArenaCyan * Fade, At.X, Y - 30.f * S, GEngine->GetMediumFont(), 1.1f);
			break;
		case ftcore::ArenaEventType::Staggered:
			DrawStrCentered(kStaggeredArena, kArenaOrange * Fade, At.X, Y - 60.f * S, GEngine->GetMediumFont(), 1.3f);
			break;
		case ftcore::ArenaEventType::Downed:
			DrawStrCentered(kDownArena, kArenaWhite * Fade, At.X, Y - 60.f * S, GEngine->GetMediumFont(), 1.3f);
			break;
		case ftcore::ArenaEventType::LoadStored:
			// "+N LOAD" beside the lead, in the gauge's colour.
			if (Cache.Second != Serial || Cache.Value != P.Amount)
			{
				Cache.Value = P.Amount;
				Cache.Second = Serial;
				Cache.Text.Reset();
				Cache.Text += TEXT("+");
				Cache.Text.AppendInt(P.Amount);
				Cache.Text += TEXT(" ");
				Cache.Text += kLoadPlusArena.Get();
			}
			DrawStrCentered(Cache.Text, kArenaLoad * Fade, At.X - 150.f * S, Y - 10.f * S, GEngine->GetMediumFont(), 1.3f);
			break;
		case ftcore::ArenaEventType::CounterStagger:
			// The paired counter: COUNTER over the enemy, its damage beside it.
			if (Cache.Second != Serial || Cache.Value != P.Amount)
			{
				Cache.Value = P.Amount;
				Cache.Second = Serial;
				Cache.Text.Reset();
				Cache.Text.AppendInt(P.Amount);
			}
			DrawStrCentered(kCounterArena, kArenaGold * Fade, At.X, Y - 90.f * S, GEngine->GetLargeFont(), 1.5f);
			DrawStrCentered(Cache.Text, kArenaGold * Fade, At.X + Side, Y + 40.f * S, GEngine->GetLargeFont(), 2.1f);
			break;
		case ftcore::ArenaEventType::Read:
			// READ beside the attacker, with the stacks it makes.
			if (Cache.Second != Serial || Cache.Value != P.Amount)
			{
				Cache.Value = P.Amount;
				Cache.Second = Serial;
				Cache.Text.Reset();
				Cache.Text += kReadArena.Get();
				Cache.Text += TEXT(" x");
				Cache.Text.AppendInt(P.Amount);
			}
			DrawStrCentered(Cache.Text, kArenaCyan * Fade, At.X - 190.f * S, Y + 10.f * S, GEngine->GetMediumFont(), 1.4f);
			break;
		case ftcore::ArenaEventType::Misread:
			DrawStrCentered(kMisreadArena, kArenaRed * Fade, At.X - 190.f * S, Y + 10.f * S, GEngine->GetMediumFont(), 1.4f);
			break;
		case ftcore::ArenaEventType::ReadCounter:
			// The Read counter: READ COUNTER over the enemy, its damage beside it.
			if (Cache.Second != Serial || Cache.Value != P.Amount)
			{
				Cache.Value = P.Amount;
				Cache.Second = Serial;
				Cache.Text.Reset();
				Cache.Text.AppendInt(P.Amount);
			}
			DrawStrCentered(kReadCounterArena, kArenaCyan * Fade, At.X, Y - 90.f * S, GEngine->GetLargeFont(), 1.3f);
			DrawStrCentered(Cache.Text, kArenaCyan * Fade, At.X + Side, Y + 40.f * S, GEngine->GetLargeFont(), 1.9f);
			break;
		case ftcore::ArenaEventType::StaminaEmpty:
			DrawStrCentered(kNoStaminaArena, kArenaStamina * Fade, At.X, Y - 60.f * S, GEngine->GetSmallFont(), 1.3f);
			break;
		default:
			break;
		}
	}
}

void AFTArenaHUD::DrawFlashes(const UFTArenaRunner& Mode)
{
	const float S = UiScale;
	const double Now = Mode.GetNow();
	const float Hit = ArenaPulse(Now, Mode.GetPlayerHitTime(), kArenaHitEdgeLife);
	const float Parry = ArenaPulse(Now, Mode.GetParryTime(), kArenaParryEdgeLife);
	const float W = Canvas->ClipX;
	const float H = Canvas->ClipY;
	const float T = 34.f * S;
	if (Hit > 0.f)
	{
		const FLinearColor C(0.9f, 0.05f, 0.05f, 0.5f * Hit);
		DrawRect(C, 0.f, 0.f, W, T);
		DrawRect(C, 0.f, H - T, W, T);
		DrawRect(C, 0.f, T, T, H - 2.f * T);
		DrawRect(C, W - T, T, T, H - 2.f * T);
	}
	if (Parry > 0.f)
	{
		const FLinearColor C(1.f, 0.84f, 0.3f, 0.45f * Parry);
		DrawRect(C, 0.f, 0.f, W, T * 0.6f);
		DrawRect(C, 0.f, H - T * 0.6f, W, T * 0.6f);
		DrawRect(FLinearColor(1.f, 0.95f, 0.8f, 0.12f * Parry), 0.f, 0.f, W, H);
	}
}

void AFTArenaHUD::DrawBanners(const UFTArenaRunner& Mode)
{
	const float S = UiScale;
	const double Now = Mode.GetNow();
	const float W = Canvas->ClipX;
	const float H = Canvas->ClipY;
	const float Release = ArenaPulse(Now, Mode.GetReleaseTime(), kArenaReleaseBanner);
	if (Release > 0.f)
	{
		// An orange edge and RELEASE with the Load it spent, growing a little as it fades.
		const float T = 30.f * S;
		const FLinearColor Edge(1.f, 0.5f, 0.12f, 0.5f * Release);
		DrawRect(Edge, 0.f, 0.f, W, T);
		DrawRect(Edge, 0.f, H - T, W, T);
		const float Grow = 1.f + 0.2f * (1.f - Release);
		const float A = FMath::Min(1.f, Release * 2.f);
		const FLinearColor C = ArenaAlpha(kArenaLoad, A);
		DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.4f * A), 0.f, H * 0.2f - 12.f * S, W, 150.f * S);
		DrawStrCentered(kReleaseArena, C, W * 0.5f, H * 0.2f, GEngine->GetLargeFont(), 3.f * Grow);
		DrawStrCentered(ArenaNumberOf(ReleaseText, Mode.GetReleaseSpent()), ArenaAlpha(kArenaWhite, A), W * 0.5f, H * 0.2f + 84.f * S,
			GEngine->GetLargeFont(), 2.f);
	}
	const float Over = ArenaPulse(Now, Mode.GetOverloadTime(), kArenaOverloadBanner);
	if (Over > 0.f && Mode.GetFight().overloaded())
	{
		const FLinearColor C(1.f, 0.2f, 0.1f, FMath::Min(1.f, Over * 2.f));
		DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.4f * C.A), 0.f, H * 0.2f - 12.f * S, W, 110.f * S);
		DrawStrCentered(kOverloadedArena, C, W * 0.5f, H * 0.2f, GEngine->GetLargeFont(), 3.f);
		DrawStrCentered(kOverloadLockedArena, ArenaAlpha(kArenaWhite, C.A), W * 0.5f, H * 0.2f + 60.f * S, GEngine->GetMediumFont(), 1.2f);
	}
}

void AFTArenaHUD::DrawResult(const UFTArenaRunner& Mode)
{
	const float S = UiScale;
	const ftcore::ArenaFight& Fight = Mode.GetFight();
	const bool bWon = Fight.phase() == ftcore::ArenaPhase::Victory;
	if (!bStatsBuilt)
	{
		const ftcore::ArenaStats& St = Fight.stats();
		StatsText = FString::Format(*kStatsArena.Get(), {FString::Printf(TEXT("%.1f"), Fight.time()), St.hits, St.parries, St.ripostes,
			St.evades, St.blocks, St.hitsTaken});
		bStatsBuilt = true;
	}
	const float Fade = FMath::Clamp(static_cast<float>(Mode.GetResultAge() / kArenaResultFade), 0.f, 1.f);
	const float CX = Canvas->ClipX * 0.5f;
	const float Top = Canvas->ClipY * 0.36f;
	DrawRect(ArenaAlpha(kArenaPanel, Fade), 0.f, Top, Canvas->ClipX, 250.f * S);
	DrawStrCentered(bWon ? kVictoryArena : kDefeatArena, ArenaAlpha(bWon ? kArenaGold : kArenaRed, Fade), CX, Top + 28.f * S,
		GEngine->GetLargeFont(), 3.f);
	DrawStrCentered(StatsText, ArenaAlpha(kArenaWhite, Fade), CX, Top + 140.f * S, GEngine->GetMediumFont(), 1.1f);
	if (!Mode.IsAutoPlay() && Mode.CanFightAgain())
	{
		DrawStrCentered(kAgainArena, ArenaAlpha(kArenaDim, Fade), CX, Top + 196.f * S, GEngine->GetMediumFont(), 1.f);
	}
}

void AFTArenaHUD::DrawEdgeCue(const UFTArenaRunner& Mode)
{
	constexpr float Band = 3.f;   // the cue is full at the wall and gone this far from it
	constexpr float Reach = 8.f;  // how much of the wall along from the player is drawn
	constexpr float Step = 0.8f;  // metres between the cue's marks along the wall
	const ftcore::ArenaZone& Zone = Mode.GetZone();
	const ftcore::Vec2 Player = Mode.GetPlayerPosition();
	const float Wall = Zone.wallDistance(Player);
	if (Wall > Band + 0.5f)
	{
		return;
	}
	const float Strength = FMath::Clamp(1.f - Wall / (Band + 0.5f), 0.f, 1.f);
	const double Now = Mode.GetNow();
	const double Z0 = Mode.GetZoneZ();
	const int32 Style = Mode.GetZoneStyle();
	const float S = UiScale;
	int32 Index = 0;
	ftcore::Vec2 Last{};
	bool bHaveLast = false;
	// One mark at wall point P: its fade by distance from the player, and the style's drawing.
	const auto Mark = [&](const ftcore::Vec2& P)
	{
		const float Away = FMath::Sqrt((P.x - Player.x) * (P.x - Player.x) + (P.y - Player.y) * (P.y - Player.y));
		const float Fade = Strength * FMath::Clamp(1.f - Away / Reach, 0.f, 1.f);
		const int32 I = Index++;
		if (Fade <= 0.01f)
		{
			bHaveLast = false;
			return;
		}
		const auto At = [&](const ftcore::Vec2& Q, double Height) { return FVector(Q.x * 100.0, Q.y * 100.0, Z0 + Height * 100.0); };
		FVector2D A, B;
		switch (Style)
		{
		case 1: // Solhaven: rain streaks falling through sodium and violet haze
		{
			const double Phase = FMath::Frac(Now * 1.6 + static_cast<double>((I * 37) % 100) * 0.01);
			const double Top = 3.2 - Phase * 3.2;
			const FLinearColor Colour = (I % 3 == 0) ? FLinearColor(0.75f, 0.55f, 1.f, 0.9f * Fade) : FLinearColor(1.f, 0.66f, 0.3f, 0.85f * Fade);
			if (ToScreen(At(P, Top + 0.9), A) && ToScreen(At(ftcore::Vec2{P.x + 0.05f, P.y + 0.05f}, Top), B))
			{
				DrawLine(A.X, A.Y, B.X, B.Y, Colour, 2.6f * S);
			}
			break;
		}
		case 2: // the Lattice: a faint blue ruled grid, pulsing
		{
			const float Pulse = 0.55f + 0.45f * FMath::Sin(static_cast<float>(Now) * 3.f + static_cast<float>(I) * 0.35f);
			const FLinearColor Colour(0.3f, 0.7f, 1.f, 0.75f * Fade * Pulse);
			if ((I % 2 == 0) && ToScreen(At(P, 0.0), A) && ToScreen(At(P, 3.0), B))
			{
				DrawLine(A.X, A.Y, B.X, B.Y, Colour, 1.4f * S);
			}
			if (bHaveLast)
			{
				for (double Height : {0.6, 1.8, 3.0})
				{
					if (ToScreen(At(Last, Height), A) && ToScreen(At(P, Height), B))
					{
						DrawLine(A.X, A.Y, B.X, B.Y, Colour, 1.2f * S);
					}
				}
			}
			break;
		}
		case 3: // the Unwritten: paper-edge fog, pale bars drifting up from the ground
		default: // the plain zone: dust, in sand
		{
			const float Drift = 0.6f + 0.4f * FMath::Sin(static_cast<float>(Now) * 0.8f + static_cast<float>(I));
			const FLinearColor Colour = Style == 3 ? FLinearColor(0.92f, 0.9f, 0.82f, 0.16f * Fade * Drift) : FLinearColor(0.85f, 0.78f, 0.6f, 0.16f * Fade * Drift);
			if (ToScreen(At(P, 0.0), A) && ToScreen(At(P, 1.2 + 1.4 * Drift), B))
			{
				DrawLine(A.X, A.Y, B.X, B.Y, Colour, 16.f * S);
			}
			break;
		}
		}
		Last = P;
		bHaveLast = true;
	};
	// The wall's points about every Step metres, from the zone's own shape.
	if (Zone.shape == ftcore::ZoneShape::Circle)
	{
		const int32 Count = FMath::Max(16, FMath::CeilToInt(2.f * UE_PI * Zone.radius / Step));
		for (int32 N = 0; N < Count; ++N)
		{
			const float Angle = 2.f * UE_PI * static_cast<float>(N) / static_cast<float>(Count);
			Mark(ftcore::Vec2{Zone.centre.x + Zone.radius * FMath::Cos(Angle), Zone.centre.y + Zone.radius * FMath::Sin(Angle)});
		}
	}
	else
	{
		for (int32 V = 0; V < static_cast<int32>(Zone.vertCount); ++V)
		{
			const ftcore::Vec2 From = Zone.verts[static_cast<size_t>(V)], To = Zone.verts[static_cast<size_t>((V + 1) % Zone.vertCount)];
			const float Length = FMath::Sqrt((To.x - From.x) * (To.x - From.x) + (To.y - From.y) * (To.y - From.y));
			const int32 Count = FMath::Max(1, FMath::CeilToInt(Length / Step));
			for (int32 N = 0; N < Count; ++N)
			{
				const float T = static_cast<float>(N) / static_cast<float>(Count);
				Mark(ftcore::Vec2{From.x + (To.x - From.x) * T, From.y + (To.y - From.y) * T});
			}
		}
	}
}

void AFTArenaHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	// The arena demo's fight, else the one this map's arena host runs (an arena step in a world).
	const AFTArenaGameMode* Arena = GetWorld() ? GetWorld()->GetAuthGameMode<AFTArenaGameMode>() : nullptr;
	const UFTArenaRunner* Mode = Arena ? Arena->GetRunner() : nullptr;
	for (TActorIterator<AFTArenaHost> It(GetWorld()); !Mode && It; ++It)
	{
		Mode = It->GetRunner();
	}
	if (!Mode || !Mode->IsBegun())
	{
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	UiScale = FMath::Max(0.5f, Canvas->ClipY / 1080.f);
	const float S = UiScale;
	const ftcore::ArenaFight& Fight = Mode->GetFight();
	if (!Fight.over())
	{
		bStatsBuilt = false; // the next result gets its own counts
	}

	DrawEdgeCue(*Mode);
	DrawFlashes(*Mode);
	DrawEnemies(*Mode);
	DrawLock(*Mode);
	DrawTargetBar(*Mode);
	DrawBossPanel(*Mode);
	DrawPlayerPanel(*Mode);
	DrawPopups(*Mode);
	DrawBanners(*Mode);
	if (Mode->IsAutoPlay() || Mode->IsIdle())
	{
		DrawText(Mode->IsIdle() ? kIdleArena : kAutoPlayArena, kArenaDim, 40.f * S, 30.f * S, GEngine->GetSmallFont(), 1.2f * S);
	}
	if (Fight.over())
	{
		DrawResult(*Mode);
	}
	// The aim's centre reticle (right mouse held), faded in with the stance.
	FT::DrawAimReticle(*this, Canvas, Mode->GetAimAlpha(), Mode->IsReticleOutOfReach(), Mode->IsReticleBlocked());
	DrawRect(kArenaPanel, 0.f, Canvas->ClipY - 46.f * S, Canvas->ClipX, 46.f * S);
	DrawStrCentered(FTInputBindings::HintLine(ftcore::InputHintScreen::Arena, FTInputBindings::NoteDevice(GetOwningPlayerController())), kArenaWhite, Canvas->ClipX * 0.5f, Canvas->ClipY - 36.f * S, GEngine->GetSmallFont(), 1.2f);
	FT::DrawPauseOverlay(*this, Canvas);
}
