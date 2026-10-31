#include "FTBeatHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "FTActionButtons.h"
#include "Engine/World.h"
#include "FTBeatGameMode.h"
#include "FTPauseOverlay.h"
#include "FTUiText.h"

namespace
{
const FLinearColor kBeatGold(1.0f, 0.82f, 0.25f);
const FLinearColor kBeatWhite(1.f, 1.f, 1.f);
const FLinearColor kBeatDim(0.55f, 0.55f, 0.6f);
const FLinearColor kBeatPanel(0.02f, 0.02f, 0.05f, 0.7f);
const FLinearColor kBeatBack(0.12f, 0.12f, 0.16f, 0.9f);
const FLinearColor kBeatHpBack(0.15f, 0.05f, 0.05f, 0.9f);
const FLinearColor kBeatRed(1.0f, 0.3f, 0.3f);
const FLinearColor kBeatOrange(1.0f, 0.55f, 0.15f);
const FLinearColor kBeatCyan(0.35f, 0.85f, 1.f);
const FLinearColor kBeatGreen(0.3f, 0.85f, 0.4f);

// Static so drawing never builds strings.
const FTUiText kNoStepBeat(TEXT("common.no_step"));
const FTUiText kHintBeat(TEXT("common.buttons_hint"));
const FTUiText kPerfectBeat(TEXT("common.perfect"));
const FTUiText kGoodBeat(TEXT("common.good"));
const FTUiText kMissBeat(TEXT("common.miss"));
const FTUiText kHoldBeat(TEXT("qte.hold"));
const FTUiText kVictoryBeat(TEXT("battle.victory"));
const FTUiText kDefeatBeat(TEXT("battle.defeat"));
const FTUiText kDownBeat(TEXT("battle.down"));
const FTUiText kLateBeat(TEXT("beat.late"));
const FTUiText kLockedBeat(TEXT("beat.locked"));
const FTUiText kFinisherBeat(TEXT("beat.finisher"));
const FTUiText kChainBeat(TEXT("beat.chain"));
const FTUiText kCounterBeat(TEXT("beat.counter"));
const FTUiText kStatsBeat(TEXT("beat.stats"));
const FTUiText kRetryBeat(TEXT("beat.retry"));
const FTUiText kQuitBeat(TEXT("beat.quit"));
const FString kBeatIndexText[4] = {FString(TEXT("1")), FString(TEXT("2")), FString(TEXT("3")), FString(TEXT("4"))};

constexpr float kBeatPopupLife = 0.8f;       // seconds on the fight's clock
constexpr float kBeatHitFlashLife = 0.45f;   // the lead was hit
constexpr float kBeatLockFlashLife = 0.35f;  // counters were locked out
constexpr float kBeatFinisherLife = 1.4f;    // the cue when a finisher starts
constexpr float kBeatMultiplierPulse = 0.5f; // the multiplier just changed
constexpr float kBeatResultFade = 0.25f;     // the result fades in
constexpr int32 kBeatMarkersPerEnemy = 4;
constexpr int32 kBeatPromptCap = static_cast<int32>(ftcore::kMaxBeatPrompts);

FLinearColor BeatWithAlpha(const FLinearColor& C, float A)
{
	return FLinearColor(C.R, C.G, C.B, C.A * A);
}

// The text of a number, with Prefix in front ("x" for a multiplier): rebuilt only when the number changes,
// so drawing a frame builds nothing. Each cache is used with one prefix.
const FString& BeatNumberOf(FBeatNumberText& Cache, int32 Value, const TCHAR* Prefix = TEXT(""))
{
	if (Value != Cache.Value)
	{
		Cache.Value = Value;
		Cache.Text = FString(Prefix) + FString::FromInt(Value);
	}
	return Cache.Text;
}

// 1 right after `Time`, falling to 0 over `Life` seconds; 0 before it and after.
float BeatPulse(double Now, double Time, float Life)
{
	const float Age = static_cast<float>(Now - Time);
	return (Age >= 0.f && Age < Life) ? 1.f - Age / Life : 0.f;
}
} // namespace

void AFTBeatHUD::DrawStrCentered(const FString& Text, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	GetTextSize(Text, W, H, Font, Scale * UiScale);
	DrawText(Text, Color, X - W * 0.5f, Y, Font, Scale * UiScale);
}

void AFTBeatHUD::DrawRing(float X, float Y, float Radius, const FLinearColor& Color, float Thickness)
{
	constexpr int32 Segments = 36;
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

void AFTBeatHUD::DrawSpikes(float X, float Y, float Radius, const FLinearColor& Color, float Thickness)
{
	constexpr int32 Spikes = 8;
	const float Turn = 6.2831853f / static_cast<float>(Spikes);
	for (int32 I = 0; I < Spikes; ++I)
	{
		const float A = Turn * (static_cast<float>(I) + 0.5f);
		const float Cosine = FMath::Cos(A);
		const float Sine = FMath::Sin(A);
		DrawLine(X + Radius * 1.1f * Cosine, Y + Radius * 1.1f * Sine, X + Radius * 1.45f * Cosine, Y + Radius * 1.45f * Sine, Color, Thickness);
	}
}

void AFTBeatHUD::DrawFrame(float X, float Y, float W, float H, const FLinearColor& Color, float Thickness)
{
	DrawRect(Color, X, Y, W, Thickness);
	DrawRect(Color, X, Y + H - Thickness, W, Thickness);
	DrawRect(Color, X, Y, Thickness, H);
	DrawRect(Color, X + W - Thickness, Y, Thickness, H);
}

void AFTBeatHUD::DrawBar(float X, float Y, float W, float H, float Fraction, const FLinearColor& Fill, const FLinearColor& Back)
{
	DrawRect(Back, X, Y, W, H);
	if (Fraction > 0.f)
	{
		DrawRect(Fill, X, Y, W * FMath::Clamp(Fraction, 0.f, 1.f), H);
	}
}

void AFTBeatHUD::DrawGlyph(int32 Button, float X, float Y, float Radius, const FLinearColor& Color, float Thickness)
{
	FTActionButtons::DrawGlyph(*this, Button, X, Y, Radius, Color, Thickness);
}

FVector2D AFTBeatHUD::EnemyPoint(const AFTBeatGameMode& Mode, int32 Enemy, int32 Count)
{
	FVector World = FVector::ZeroVector;
	if (Mode.GetEnemyAnchor(Enemy, World))
	{
		const FVector P = Project(World, true);
		if (P.Z > 0.0)
		{
			return FVector2D(P.X, P.Y);
		}
	}
	// No stand-in (or it is behind the camera): a place on the screen, spread across it.
	const float Spread = (static_cast<float>(Enemy) - 0.5f * static_cast<float>(Count - 1)) * 300.f * UiScale;
	return FVector2D(Canvas->ClipX * 0.5f + Spread, Canvas->ClipY * 0.42f);
}

void AFTBeatHUD::DrawMarker(float X, float Y, float Scale, const ftcore::BeatPrompt& Prompt, float Approach, bool bLocked, bool bFirst, float HoldFill)
{
	const float S = UiScale * Scale;
	const float R = 34.f * S;
	const bool bCounter = Prompt.kind == ftcore::BeatKind::Counter;
	const bool bHold = Prompt.kind == ftcore::BeatKind::Hold;
	FLinearColor Base = kBeatCyan;
	if (bCounter)
	{
		Base = bLocked ? kBeatDim : kBeatRed;
	}
	else if (bHold || Prompt.finisher)
	{
		Base = kBeatGold;
	}
	// Only the first prompt is the one to answer; the others wait dimmer behind it.
	const float Dim = bFirst ? 1.f : 0.6f;
	const FLinearColor Shown = BeatWithAlpha(Base, Dim);

	// The target ring: a counter's is spiked, a finisher's has a second ring inside.
	DrawRing(X, Y, R, Shown, 4.f * S);
	if (bCounter)
	{
		DrawSpikes(X, Y, R, Shown, 3.f * S);
	}
	if (Prompt.finisher)
	{
		DrawRing(X, Y, R * 0.62f, BeatWithAlpha(kBeatWhite, Dim), 3.f * S);
	}
	// The ring that closes in on it; once it has landed the target pulses while the prompt waits for its answer.
	if (Approach > 0.f)
	{
		DrawRing(X, Y, R + 72.f * S * Approach, BeatWithAlpha(bCounter ? Base : kBeatWhite, Dim * (1.f - 0.6f * Approach)), 4.f * S);
	}
	else
	{
		DrawRing(X, Y, R * 1.14f, BeatWithAlpha(Base, 0.55f * Dim), 6.f * S);
	}
	DrawGlyph(static_cast<int32>(Prompt.input), X, Y, R * 0.55f, BeatWithAlpha(kBeatWhite, Dim), 5.f * S);
	if (bCounter && bLocked)
	{
		// Locked out: crossed through.
		const FLinearColor Cross = BeatWithAlpha(kBeatRed, Dim);
		DrawLine(X - R * 0.8f, Y - R * 0.8f, X + R * 0.8f, Y + R * 0.8f, Cross, 4.f * S);
		DrawLine(X - R * 0.8f, Y + R * 0.8f, X + R * 0.8f, Y - R * 0.8f, Cross, 4.f * S);
	}

	float LabelY = Y + R + 10.f * S;
	if (bHold)
	{
		// The bar is as long as the hold, and fills while it is held.
		const float BarW = static_cast<float>(Prompt.length) * 18.f * S;
		const float BarH = 9.f * S;
		DrawBar(X - BarW * 0.5f, LabelY, BarW, BarH, HoldFill, Shown, kBeatBack);
		LabelY += BarH + 4.f * S;
		if (bFirst)
		{
			DrawStrCentered(kHoldBeat, Shown, X, LabelY, GEngine->GetSmallFont(), 1.1f);
		}
	}
	else if (bCounter && bFirst)
	{
		DrawStrCentered(kCounterBeat, Shown, X, LabelY, GEngine->GetSmallFont(), 1.1f);
	}
}

void AFTBeatHUD::DrawPrompts(const AFTBeatGameMode& Mode)
{
	const ftcore::BeatScreen& Screen = Mode.GetScreen();
	const ftcore::BeatFight& Fight = Screen.fight();
	const double Now = Screen.now();
	const int32 Total = FMath::Min(static_cast<int32>(Fight.promptCount()), kBeatPromptCap);
	const int32 Enemies = static_cast<int32>(Fight.enemyCount());
	const bool bLocked = Fight.counterLockedOut(Now);

	// Each enemy's showing prompts in time order: the first is the one to answer.
	int32 RankOf[kBeatPromptCap] = {};
	int32 Shown[AFTBeatGameMode::MaxStandIns] = {};
	for (int32 I = 0; I < Total; ++I)
	{
		const ftcore::BeatPrompt& P = Fight.prompt(static_cast<size_t>(I));
		const int32 E = FMath::Clamp(static_cast<int32>(P.enemy), 0, AFTBeatGameMode::MaxStandIns - 1);
		RankOf[I] = Fight.approach(P, Now) < 0.0 ? -1 : Shown[E]++;
	}
	// The later prompts first, so the next one is drawn on top.
	for (int32 I = Total - 1; I >= 0; --I)
	{
		if (RankOf[I] < 0 || RankOf[I] >= kBeatMarkersPerEnemy)
		{
			continue;
		}
		const ftcore::BeatPrompt& P = Fight.prompt(static_cast<size_t>(I));
		const FVector2D At = EnemyPoint(Mode, static_cast<int32>(P.enemy), Enemies);
		const bool bFirst = RankOf[I] == 0;
		const float X = static_cast<float>(At.X) + static_cast<float>(RankOf[I]) * 84.f * UiScale;
		const float Y = static_cast<float>(At.Y) - 40.f * UiScale;
		const float Fill = (P.kind == ftcore::BeatKind::Hold && P.grade != ftcore::QteGrade::Pending) ? static_cast<float>(Fight.holdProgress()) : 0.f;
		DrawMarker(X, Y, bFirst ? 1.f : 0.72f, P, static_cast<float>(Fight.approach(P, Now)), bLocked, bFirst, Fill);
	}
}

void AFTBeatHUD::DrawEnemyPanels(const AFTBeatGameMode& Mode)
{
	const ftcore::BeatFight& Fight = Mode.GetScreen().fight();
	const int32 Count = FMath::Min(static_cast<int32>(Fight.enemyCount()), AFTBeatGameMode::MaxStandIns);
	const int32 Turn = Fight.turnEnemy();
	const float S = UiScale;
	for (int32 I = 0; I < Count; ++I)
	{
		const size_t E = static_cast<size_t>(I);
		const float X = 24.f * S;
		const float Y = 24.f * S + static_cast<float>(I) * 66.f * S;
		const float W = 300.f * S;
		const float H = 56.f * S;
		const bool bAlive = Fight.enemyAlive(E);
		DrawRect(kBeatPanel, X, Y, W, H);
		if (I == Turn)
		{
			DrawFrame(X, Y, W, H, kBeatGold, 3.f * S);
		}
		DrawText(kBeatIndexText[I], bAlive ? kBeatWhite : kBeatDim, X + 16.f * S, Y + 10.f * S, GEngine->GetLargeFont(), 1.2f * S);
		const int32 Max = Fight.enemyMaxHp(E);
		const float Fraction = Max > 0 ? static_cast<float>(Fight.enemyHp(E)) / static_cast<float>(Max) : 0.f;
		DrawBar(X + 56.f * S, Y + 32.f * S, W - 72.f * S, 14.f * S, Fraction, kBeatRed, kBeatHpBack);
		if (!bAlive)
		{
			DrawText(kDownBeat, kBeatRed, X + 56.f * S, Y + 8.f * S, GEngine->GetSmallFont(), 1.2f * S);
		}
	}
}

void AFTBeatHUD::DrawLeadPanel(const AFTBeatGameMode& Mode)
{
	const ftcore::BeatScreen& Screen = Mode.GetScreen();
	const ftcore::BeatFight& Fight = Screen.fight();
	const float S = UiScale;
	const float W = 320.f * S;
	const float H = 64.f * S;
	const float X = 24.f * S;
	const float Y = Canvas->ClipY - 130.f * S;
	DrawRect(kBeatPanel, X, Y, W, H);
	const float Fraction = Fight.maxHp() > 0 ? static_cast<float>(Fight.hp()) / static_cast<float>(Fight.maxHp()) : 0.f;
	DrawBar(X + 14.f * S, Y + 24.f * S, W - 100.f * S, 18.f * S, Fraction, kBeatGreen, kBeatHpBack);
	DrawText(BeatNumberOf(LeadHpText, Fight.hp()), kBeatWhite, X + W - 76.f * S, Y + 16.f * S, GEngine->GetMediumFont(), 1.3f * S);
	const float Hit = BeatPulse(Screen.now(), Mode.GetLeadHitTime(), kBeatHitFlashLife);
	if (Hit > 0.f)
	{
		DrawRect(BeatWithAlpha(kBeatRed, 0.6f * Hit), X, Y, W, H);
	}
	if (Screen.now() < Mode.GetLockoutUntil() && Screen.now() >= Mode.GetLockoutSince())
	{
		DrawStrCentered(kLockedBeat, kBeatOrange, X + W * 0.5f, Y - 30.f * S, GEngine->GetMediumFont(), 1.1f);
	}
}

void AFTBeatHUD::DrawChain(const AFTBeatGameMode& Mode)
{
	const ftcore::BeatScreen& Screen = Mode.GetScreen();
	const ftcore::BeatFight& Fight = Screen.fight();
	const float S = UiScale;
	const float W = 250.f * S;
	const float H = 64.f * S;
	const float X = Canvas->ClipX - 24.f * S - W;
	const float Y = Canvas->ClipY - 130.f * S;
	DrawRect(kBeatPanel, X, Y, W, H);
	DrawText(kChainBeat, kBeatDim, X + 14.f * S, Y + 8.f * S, GEngine->GetSmallFont(), 1.2f * S);
	DrawText(BeatNumberOf(ChainText, Fight.chain()), kBeatWhite, X + 14.f * S, Y + 26.f * S, GEngine->GetMediumFont(), 1.3f * S);
	// The multiplier swells when it changes.
	const int32 Multiplier = Fight.multiplier();
	const float Pulse = BeatPulse(Screen.now(), Mode.GetMultiplierTime(), kBeatMultiplierPulse);
	DrawStrCentered(BeatNumberOf(MultiplierText, Multiplier, TEXT("x")), Multiplier > 1 ? kBeatGold : kBeatWhite, X + W - 70.f * S, Y + 8.f * S,
		GEngine->GetLargeFont(), 1.2f + 0.6f * Pulse);
}

void AFTBeatHUD::DrawLeadIn(const AFTBeatGameMode& Mode)
{
	const ftcore::BeatScreen& Screen = Mode.GetScreen();
	const double Beats = Screen.leadInBeats();
	if (Beats <= 0.0)
	{
		return;
	}
	// The beats left, counted down: each number swells as its beat comes in.
	const int32 Count = FMath::CeilToInt32(Beats);
	const float Swell = static_cast<float>(FMath::Frac(Beats));
	DrawStrCentered(BeatNumberOf(LeadInText, Count), kBeatGold, Canvas->ClipX * 0.5f, Canvas->ClipY * 0.5f - 230.f * UiScale, GEngine->GetLargeFont(), 2.4f + 0.8f * Swell);
}

void AFTBeatHUD::DrawPopups(const AFTBeatGameMode& Mode)
{
	const ftcore::BeatScreen& Screen = Mode.GetScreen();
	const double Now = Screen.now();
	const int32 Enemies = static_cast<int32>(Screen.fight().enemyCount());
	for (int32 I = 0; I < Mode.GetPopupCount(); ++I)
	{
		const AFTBeatGameMode::FPopup& Popup = Mode.GetPopup(I);
		const float Age = static_cast<float>(Now - Popup.Time);
		if (Age < 0.f || Age >= kBeatPopupLife)
		{
			continue;
		}
		const float K = Age / kBeatPopupLife;
		const FString* Text = nullptr;
		FLinearColor Color = kBeatWhite;
		switch (Popup.Type)
		{
		case ftcore::BeatEventType::Judged:
			if (Popup.Grade == ftcore::QteGrade::Perfect)
			{
				Text = &kPerfectBeat.Get();
				Color = kBeatGold;
			}
			else if (Popup.Grade == ftcore::QteGrade::Good)
			{
				Text = &kGoodBeat.Get();
				Color = kBeatCyan;
			}
			else if (Popup.Grade == ftcore::QteGrade::Miss)
			{
				Text = &kMissBeat.Get();
				Color = kBeatRed;
			}
			break;
		case ftcore::BeatEventType::Late:
			Text = &kLateBeat.Get();
			Color = kBeatOrange;
			break;
		case ftcore::BeatEventType::LockedOut:
			Text = &kLockedBeat.Get();
			Color = kBeatOrange;
			break;
		default:
			break;
		}
		if (!Text)
		{
			continue;
		}
		const FVector2D At = EnemyPoint(Mode, Popup.Enemy, Enemies);
		DrawStrCentered(*Text, BeatWithAlpha(Color, 1.f - K), static_cast<float>(At.X), static_cast<float>(At.Y) - (120.f + 60.f * K) * UiScale,
			GEngine->GetLargeFont(), 1.2f);
	}
}

void AFTBeatHUD::DrawFlashes(const AFTBeatGameMode& Mode)
{
	const ftcore::BeatScreen& Screen = Mode.GetScreen();
	const double Now = Screen.now();
	const float S = UiScale;
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	// The screen's edges flash: red when the lead is hit, orange when counters are locked out.
	const auto Edges = [this, S](const FLinearColor& Color, float Alpha)
	{
		const float T = 18.f * S;
		const FLinearColor C = BeatWithAlpha(Color, Alpha);
		DrawRect(C, 0.f, 0.f, Canvas->ClipX, T);
		DrawRect(C, 0.f, Canvas->ClipY - T, Canvas->ClipX, T);
		DrawRect(C, 0.f, 0.f, T, Canvas->ClipY);
		DrawRect(C, Canvas->ClipX - T, 0.f, T, Canvas->ClipY);
	};
	const float Hit = BeatPulse(Now, Mode.GetLeadHitTime(), kBeatHitFlashLife);
	if (Hit > 0.f)
	{
		Edges(kBeatRed, 0.55f * Hit);
	}
	const float Lock = BeatPulse(Now, Mode.GetLockoutSince(), kBeatLockFlashLife);
	if (Lock > 0.f)
	{
		Edges(kBeatOrange, 0.45f * Lock);
	}
	// A finisher is coming: its cue rises and fades.
	const float Age = static_cast<float>(Now - Mode.GetFinisherTime());
	if (Age >= 0.f && Age < kBeatFinisherLife)
	{
		const float K = Age / kBeatFinisherLife;
		DrawStrCentered(kFinisherBeat, BeatWithAlpha(kBeatGold, 1.f - K), CX, CY - 130.f * S - 30.f * S * K, GEngine->GetLargeFont(), 2.0f);
	}
}

void AFTBeatHUD::DrawResult(const AFTBeatGameMode& Mode)
{
	const ftcore::BeatScreen& Screen = Mode.GetScreen();
	const bool bWon = Screen.fight().phase() == ftcore::BeatPhase::Victory;
	const float S = UiScale;
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	const float Fade = FMath::Clamp(static_cast<float>(Screen.resultAge()) / kBeatResultFade, 0.f, 1.f);
	const bool bAsking = Screen.askingRetry();
	const float H = (bAsking ? 330.f : 250.f) * S;
	const float Top = CY - H * 0.5f;

	if (!bStatsBuilt)
	{
		// Once per result: the counts (they carry through retries).
		const ftcore::BeatStats& Stats = Screen.stats();
		StatsText = FString::Format(*kStatsBeat.Get(), {Stats.perfects, Stats.goods, Stats.misses, Stats.maxChain});
		bStatsBuilt = true;
	}
	DrawRect(BeatWithAlpha(kBeatPanel, Fade), 0.f, Top, Canvas->ClipX, H);
	DrawStrCentered(bWon ? kVictoryBeat : kDefeatBeat, BeatWithAlpha(bWon ? kBeatGold : kBeatRed, Fade), CX, Top + 28.f * S, GEngine->GetLargeFont(), 3.f);
	DrawStrCentered(StatsText, BeatWithAlpha(kBeatWhite, Fade), CX, Top + 140.f * S, GEngine->GetMediumFont(), 1.1f);
	if (bAsking)
	{
		DrawStrCentered(kRetryBeat, BeatWithAlpha(kBeatWhite, Fade), CX, Top + 200.f * S, GEngine->GetMediumFont(), 1.0f);
		DrawStrCentered(kQuitBeat, BeatWithAlpha(kBeatDim, Fade), CX, Top + 244.f * S, GEngine->GetMediumFont(), 1.0f);
	}
}

void AFTBeatHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const AFTBeatGameMode* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AFTBeatGameMode>() : nullptr;
	if (!Mode || !Mode->IsBegun())
	{
		// Nothing to show yet: the fight begins on the mode's first tick.
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	UiScale = FMath::Max(0.5f, Canvas->ClipY / 1080.f);
	const float S = UiScale;
	const ftcore::BeatScreen& Screen = Mode->GetScreen();

	if (Screen.phase() == ftcore::BeatScreenPhase::NoStep)
	{
		DrawStrCentered(kNoStepBeat, kBeatWhite, Canvas->ClipX * 0.5f, Canvas->ClipY * 0.5f - 20.f * S, GEngine->GetLargeFont(), 1.4f);
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	if (Screen.playing())
	{
		bStatsBuilt = false; // the next result gets its own counts
	}

	DrawEnemyPanels(*Mode);
	DrawLeadPanel(*Mode);
	DrawChain(*Mode);
	if (Screen.playing())
	{
		DrawLeadIn(*Mode);
		DrawPrompts(*Mode);
	}
	DrawPopups(*Mode);
	DrawFlashes(*Mode);
	if (!Screen.playing())
	{
		DrawResult(*Mode);
	}
	DrawStrCentered(kHintBeat, kBeatDim, Canvas->ClipX * 0.5f, Canvas->ClipY - 50.f * S, GEngine->GetSmallFont(), 1.3f);
	FT::DrawPauseOverlay(*this, Canvas);
}
