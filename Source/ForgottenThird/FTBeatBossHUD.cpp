#include "FTBeatBossHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "FTActionButtons.h"
#include "FTBeatBossGameMode.h"
#include "FTPauseOverlay.h"
#include "FTUiText.h"

namespace
{
const FLinearColor kBossWhite(1.f, 1.f, 1.f);
const FLinearColor kBossDim(0.6f, 0.6f, 0.66f);
const FLinearColor kBossShadow(0.f, 0.f, 0.f, 0.85f);
// The engine's fonts are small at 1080p; every label is drawn this much larger.
constexpr float kBossFontBoost = 1.35f;
const FLinearColor kBossPanel(0.02f, 0.02f, 0.05f, 0.86f);
const FLinearColor kBossBack(0.12f, 0.12f, 0.16f, 0.92f);
const FLinearColor kBossHpBack(0.18f, 0.05f, 0.05f, 0.92f);
const FLinearColor kBossHpFill(0.86f, 0.17f, 0.14f);
const FLinearColor kBossRed(1.f, 0.24f, 0.2f);
const FLinearColor kBossOrange(1.f, 0.55f, 0.12f);
const FLinearColor kBossGold(1.f, 0.82f, 0.25f);
const FLinearColor kBossCyan(0.35f, 0.85f, 1.f);
const FLinearColor kBossGreen(0.3f, 0.88f, 0.42f);
const FLinearColor kBossYellow(1.f, 0.93f, 0.3f);
const FLinearColor kBossGrey(0.45f, 0.45f, 0.5f);
const FLinearColor kBossCountBlue(0.25f, 0.45f, 0.95f);

// Static so drawing never builds strings.
const FTUiText kPerfectBoss(TEXT("common.perfect"));
const FTUiText kGoodBoss(TEXT("common.good"));
const FTUiText kMissBoss(TEXT("common.miss"));
const FTUiText kVictoryBoss(TEXT("battle.victory"));
const FTUiText kDefeatBoss(TEXT("battle.defeat"));
const FTUiText kLoadBoss(TEXT("battle.load"));
const FTUiText kLockedBoss(TEXT("beat.locked"));
const FTUiText kFinisherBoss(TEXT("beat.finisher"));
const FTUiText kQuitBoss(TEXT("beat.quit"));
const FTUiText kPhaseBoss(TEXT("beatboss.phase"));
const FTUiText kBarBoss(TEXT("beatboss.bar"));
const FTUiText kCountBoss(TEXT("beatboss.count"));
const FTUiText kFinisherBarBoss(TEXT("beatboss.finisher_bar"));
const FTUiText kSilenceBoss(TEXT("beatboss.silence"));
const FTUiText kStreakBoss(TEXT("beatboss.streak"));
const FTUiText kRestsBoss(TEXT("beatboss.rests"));
const FTUiText kLeadBoss(TEXT("beatboss.lead"));
const FTUiText kStaggerBoss(TEXT("beatboss.stagger"));
const FTUiText kReleaseBoss(TEXT("beatboss.release"));
const FTUiText kBrokenBoss(TEXT("beatboss.broken"));
const FTUiText kWhiffBoss(TEXT("beatboss.whiff"));
const FTUiText kRestOpenBoss(TEXT("beatboss.rest_open"));
const FTUiText kHealBoss(TEXT("beatboss.heal"));
const FTUiText kLoadGainBoss(TEXT("beatboss.load_gain"));
const FTUiText kStatsBoss(TEXT("beatboss.stats"));
const FTUiText kRetryBoss(TEXT("beatboss.retry"));
const FTUiText kAgainBoss(TEXT("beatboss.again"));
const FTUiText kAutoPlayBoss(TEXT("beatboss.autoplay"));
const FTUiText kIdleBoss(TEXT("beatboss.idle"));
const FTUiText kHintBoss(TEXT("beatboss.hint"));
const FTUiText kAnswerAttackBoss(TEXT("beatboss.answer.attack"));
const FTUiText kAnswerGuardBoss(TEXT("beatboss.answer.guard"));
const FTUiText kAnswerParryBoss(TEXT("beatboss.answer.parry"));
const FTUiText kAnswerDodgeBoss(TEXT("beatboss.answer.dodge"));
const FTUiText kAnswerJumpBoss(TEXT("beatboss.answer.jump"));
const FTUiText kAnswerHoldBoss(TEXT("beatboss.answer.hold"));
const FTUiText kAnswerPairedBoss(TEXT("beatboss.answer.paired"));
const FTUiText kAnswerReleaseBoss(TEXT("beatboss.answer.release"));
// ftcore::BeatBossAnswer order.
const FTUiText* const kAnswerBoss[] = {&kAnswerAttackBoss, &kAnswerGuardBoss, &kAnswerParryBoss, &kAnswerDodgeBoss, &kAnswerJumpBoss,
	&kAnswerHoldBoss, &kAnswerPairedBoss, &kAnswerReleaseBoss};
constexpr int32 kAnswerBossCount = static_cast<int32>(sizeof(kAnswerBoss) / sizeof(kAnswerBoss[0]));

constexpr int32 kBossNumbers = 1000;
constexpr int32 kBossMeterSide = 17;
// Seconds of the beat line to the right of the hit line, and the share of the line left of it.
constexpr double kBossLineAhead = 4.6;
constexpr float kBossHitShare = 0.2f;
constexpr float kBossPopupLife = 0.8f;
constexpr float kBossBannerLife = 1.4f;

FLinearColor BossAlpha(const FLinearColor& C, float A)
{
	return FLinearColor(C.R, C.G, C.B, C.A * A);
}

// 1 right after `Time`, falling to 0 over `Life` seconds; 0 before it and after.
float BossFade(double Now, double Time, float Life)
{
	const float Age = static_cast<float>(Now - Time);
	return (Age >= 0.f && Age < Life) ? 1.f - Age / Life : 0.f;
}

FLinearColor AnswerColor(ftcore::BeatBossAnswer A)
{
	if (A == ftcore::BeatBossAnswer::Release)
	{
		return kBossOrange;
	}
	if (A == ftcore::BeatBossAnswer::Hold)
	{
		return kBossGold;
	}
	return ftcore::isBossCounter(A) ? kBossRed : kBossCyan;
}

// The button an answer is played on when the prompt isn't open yet (the track's own choice shows once it is).
int32 AnswerButton(ftcore::BeatBossAnswer A)
{
	switch (A)
	{
	case ftcore::BeatBossAnswer::Guard:
	case ftcore::BeatBossAnswer::Parry:
		return static_cast<int32>(ftcore::ActionButton::Guard);
	case ftcore::BeatBossAnswer::Dodge:
		return static_cast<int32>(ftcore::ActionButton::Dodge);
	case ftcore::BeatBossAnswer::Jump:
		return static_cast<int32>(ftcore::ActionButton::Jump);
	default:
		return static_cast<int32>(ftcore::ActionButton::Attack);
	}
}

const ftcore::BeatBossPrompt* FindPrompt(const ftcore::BeatBoss& Fight, uint32 Id)
{
	if (Id == 0)
	{
		return nullptr;
	}
	for (size_t I = 0; I < Fight.promptCount(); ++I)
	{
		if (Fight.prompt(I).id == Id)
		{
			return &Fight.prompt(I);
		}
	}
	return nullptr;
}

const ftcore::BossBar& BarShapeOf(const ftcore::BarMap& Map, int32 Bar)
{
	if (Bar >= 1 && Bar <= Map.loopBars)
	{
		return Map.bars[static_cast<size_t>(Bar - 1)];
	}
	return Map.meter;
}
} // namespace

void AFTBeatBossHUD::BuildTexts(const AFTBeatBossGameMode& Mode)
{
	bBuilt = true;
	Numbers.Reserve(kBossNumbers);
	Damage.Reserve(kBossNumbers);
	for (int32 I = 0; I < kBossNumbers; ++I)
	{
		Numbers.Add(FString::FromInt(I));
		Damage.Add(FString::Printf(TEXT("-%d"), I));
	}
	for (int32 I = 0; I < 10; ++I)
	{
		Multipliers.Add(FString::Printf(TEXT("x%d"), I));
	}
	const FString& Gain = kLoadGainBoss.Get();
	for (int32 I = 0; I <= ftcore::kMaxLoad; ++I)
	{
		LoadGains.Add(FString::Format(*Gain, FStringFormatOrderedArguments{FStringFormatArg(I)}));
	}
	const ftcore::BeatBossDef& Def = Mode.GetTrack().def;
	const int32 Phases = FMath::Max(1, static_cast<int32>(Def.phaseCount));
	for (int32 P = 0; P < Phases; ++P)
	{
		PhaseLabels.Add(FString::Format(*kPhaseBoss.Get(), FStringFormatOrderedArguments{FStringFormatArg(P + 1), FStringFormatArg(Phases)}));
		RetryLabels.Add(FString::Format(*kRetryBoss.Get(), FStringFormatOrderedArguments{FStringFormatArg(P + 1)}));
	}
	Meters.Reserve(kBossMeterSide * kBossMeterSide);
	for (int32 C = 0; C < kBossMeterSide; ++C)
	{
		for (int32 U = 0; U < kBossMeterSide; ++U)
		{
			Meters.Add(FString::Printf(TEXT("%d/%d"), C, U));
		}
	}
	StatsText.Reserve(256);
}

const FString& AFTBeatBossHUD::Number(int32 Value) const
{
	return Numbers[FMath::Clamp(Value, 0, kBossNumbers - 1)];
}

void AFTBeatBossHUD::Text(const FString& Str, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale, int32 Align)
{
	const float Sc = Scale * FontScale;
	if (Align != 0)
	{
		float W = 0.f;
		float H = 0.f;
		GetTextSize(Str, W, H, Font, Sc);
		X -= Align == 1 ? W * 0.5f : W;
	}
	const float Off = FMath::Max(1.f, 2.f * UiScale);
	DrawText(Str, FLinearColor(kBossShadow.R, kBossShadow.G, kBossShadow.B, kBossShadow.A * Color.A), X + Off, Y + Off, Font, Sc);
	DrawText(Str, Color, X, Y, Font, Sc);
}

void AFTBeatBossHUD::Ring(float X, float Y, float Radius, const FLinearColor& Color, float Thickness, int32 Segments)
{
	const float Turn = 6.2831853f / static_cast<float>(Segments);
	float PrevX = X + Radius;
	float PrevY = Y;
	for (int32 I = 1; I <= Segments; ++I)
	{
		const float A = Turn * static_cast<float>(I);
		const float NextX = X + Radius * FMath::Cos(A);
		const float NextY = Y + Radius * FMath::Sin(A);
		AHUD::DrawLine(PrevX, PrevY, NextX, NextY, Color, Thickness);
		PrevX = NextX;
		PrevY = NextY;
	}
}

void AFTBeatBossHUD::Disc(float X, float Y, float Radius, const FLinearColor& Color)
{
	// Horizontal strips: a filled circle out of rectangles.
	const int32 Strips = FMath::Clamp(static_cast<int32>(Radius), 4, 24);
	const float Step = 2.f * Radius / static_cast<float>(Strips);
	for (int32 I = 0; I < Strips; ++I)
	{
		const float Mid = -Radius + Step * (static_cast<float>(I) + 0.5f);
		const float Half = FMath::Sqrt(FMath::Max(0.f, Radius * Radius - Mid * Mid));
		DrawRect(Color, X - Half, Y - Radius + Step * static_cast<float>(I), Half * 2.f, Step + 0.5f);
	}
}

void AFTBeatBossHUD::Spikes(float X, float Y, float Radius, const FLinearColor& Color, float Thickness)
{
	constexpr int32 Count = 8;
	const float Turn = 6.2831853f / static_cast<float>(Count);
	for (int32 I = 0; I < Count; ++I)
	{
		const float A = Turn * (static_cast<float>(I) + 0.5f);
		const float C = FMath::Cos(A);
		const float S = FMath::Sin(A);
		AHUD::DrawLine(X + Radius * 1.05f * C, Y + Radius * 1.05f * S, X + Radius * 1.45f * C, Y + Radius * 1.45f * S, Color, Thickness);
	}
}

void AFTBeatBossHUD::Frame(float X, float Y, float W, float H, const FLinearColor& Color, float Thickness)
{
	DrawRect(Color, X, Y, W, Thickness);
	DrawRect(Color, X, Y + H - Thickness, W, Thickness);
	DrawRect(Color, X, Y, Thickness, H);
	DrawRect(Color, X + W - Thickness, Y, Thickness, H);
}

void AFTBeatBossHUD::Bar(float X, float Y, float W, float H, float Fraction, const FLinearColor& Fill, const FLinearColor& Back)
{
	DrawRect(Back, X, Y, W, H);
	if (Fraction > 0.f)
	{
		DrawRect(Fill, X, Y, W * FMath::Clamp(Fraction, 0.f, 1.f), H);
	}
}

void AFTBeatBossHUD::Diamond(float X, float Y, float Radius, const FLinearColor& Color, bool bFilled)
{
	if (bFilled)
	{
		for (float D = 0.f; D < Radius; D += 1.f)
		{
			const float Half = Radius - D;
			DrawRect(Color, X - Half, Y - D - 1.f, Half * 2.f, 1.f);
			DrawRect(Color, X - Half, Y + D, Half * 2.f, 1.f);
		}
		return;
	}
	const float T = FMath::Max(1.5f, 2.f * UiScale);
	AHUD::DrawLine(X, Y - Radius, X + Radius, Y, Color, T);
	AHUD::DrawLine(X + Radius, Y, X, Y + Radius, Color, T);
	AHUD::DrawLine(X, Y + Radius, X - Radius, Y, Color, T);
	AHUD::DrawLine(X - Radius, Y, X, Y - Radius, Color, T);
}

void AFTBeatBossHUD::DrawBoss(const AFTBeatBossGameMode& Mode)
{
	const float S = UiScale;
	const ftcore::BeatBoss& Fight = Mode.GetFight();
	const double Now = Mode.GetNow();
	const ftcore::BeatBossDef& Def = Mode.GetTrack().def;

	// The boss's hp across the top, the phase's floor marked on it.
	const float BW = Canvas->ClipX * 0.46f;
	const float BX = (Canvas->ClipX - BW) * 0.5f;
	const float BY = 58.f * S;
	const float BH = 22.f * S;
	const float Max = static_cast<float>(FMath::Max(1, Fight.bossMaxHp()));
	const float Struck = BossFade(Now, Mode.GetPulse(AFTBeatBossGameMode::EPulse::BossHit), 0.18f);
	DrawRect(kBossPanel, BX - 6.f * S, BY - 6.f * S, BW + 12.f * S, BH + 12.f * S);
	Bar(BX, BY, BW, BH, static_cast<float>(Fight.bossHp()) / Max, FMath::Lerp(kBossHpFill, kBossWhite, Struck), kBossHpBack);
	if (Fight.floorHp() > 0)
	{
		const float FX = BX + BW * static_cast<float>(Fight.floorHp()) / Max;
		DrawRect(BossAlpha(kBossWhite, 0.9f), FX - 1.5f * S, BY - 6.f * S, 3.f * S, BH + 12.f * S);
	}
	const int32 Phase = FMath::Clamp(Fight.phase(), 0, PhaseLabels.Num() - 1);
	const float Pop = BossFade(Now, Mode.GetPulse(AFTBeatBossGameMode::EPulse::Phase), 0.8f);
	Text(PhaseLabels[Phase], FMath::Lerp(kBossWhite, kBossGold, Pop), BX, BY - 32.f * S, GEngine->GetMediumFont(), 1.15f);
	Text(Number(Fight.bossHp()), kBossWhite, BX + BW, BY - 32.f * S, GEngine->GetMediumFont(), 1.15f, 2);

	// Under it: the bar playing and its meter (a broken meter in orange), or the finisher.
	const float LY = BY + BH + 12.f * S;
	UFont* Small = GEngine->GetMediumFont();
	if (Fight.inFinisher())
	{
		Text(kFinisherBarBoss, kBossGold, BX, LY, Small, 1.0f);
	}
	else if (Fight.bar() > 0 && Def.phaseCount > 0)
	{
		const ftcore::BarMap& Map = Def.phases[static_cast<size_t>(FMath::Clamp(Fight.phase(), 0, static_cast<int32>(Def.phaseCount) - 1))];
		const ftcore::BossBar& Shape = BarShapeOf(Map, Fight.bar());
		float X = BX;
		float W = 0.f;
		float H = 0.f;
		Text(kBarBoss, kBossDim, X, LY, Small, 1.0f);
		GetTextSize(kBarBoss, W, H, Small, FontScale);
		X += W + 8.f * S;
		Text(Number(Fight.bar()), kBossWhite, X, LY, Small, 1.0f);
		GetTextSize(Number(Fight.bar()), W, H, Small, FontScale);
		X += W + 14.f * S;
		const bool bBroken = Shape.eighths != Map.meter.eighths;
		Text(Meters[FMath::Clamp<int32>(Shape.meterCount, 0, kBossMeterSide - 1) * kBossMeterSide + FMath::Clamp<int32>(Shape.meterUnit, 0, kBossMeterSide - 1)],
			bBroken ? kBossOrange : kBossWhite, X, LY, Small, 1.0f);
	}

	// Over the boss: the next counter's answer, and a ring closing on it as its impact comes.
	if (Mode.GetStage() != AFTBeatBossGameMode::EStage::Playing)
	{
		return;
	}
	const FVector Anchor = Project(Mode.GetBossAnchor());
	if (Anchor.Z <= 0.0)
	{
		return;
	}
	const float AX = static_cast<float>(Anchor.X);
	const float AY = static_cast<float>(Anchor.Y);
	const ftcore::BeatBossPrompt* Next = nullptr;
	double NextApproach = 2.0;
	for (size_t I = 0; I < Fight.promptCount(); ++I)
	{
		const ftcore::BeatBossPrompt& P = Fight.prompt(I);
		const double A = Fight.approach(P);
		if (ftcore::isBossCounter(P.answer) && A >= 0.0 && A < NextApproach)
		{
			Next = &P;
			NextApproach = A;
		}
	}
	if (Next)
	{
		const bool bLocked = Now < Mode.GetLockoutUntil();
		const FLinearColor C = bLocked ? kBossGrey : kBossRed;
		const float R = (30.f + 90.f * static_cast<float>(NextApproach)) * S;
		Ring(AX, AY, R, BossAlpha(C, 0.9f), 4.f * S, 40);
		Ring(AX, AY, 30.f * S, BossAlpha(kBossWhite, 0.8f), 2.f * S);
		const int32 Answer = static_cast<int32>(Next->answer);
		if (Answer < kAnswerBossCount)
		{
			Text(*kAnswerBoss[Answer], C, AX, AY - R - 34.f * S, GEngine->GetLargeFont(), 1.0f, 1);
		}
		FTActionButtons::DrawGlyph(*this, static_cast<int32>(Next->button), AX, AY, 20.f * S, C, 2.5f * S);
	}
	// The boss's damage, rising over it.
	const double HitAge = Now - Mode.GetPulse(AFTBeatBossGameMode::EPulse::BossHit);
	if (HitAge >= 0.0 && HitAge < 0.7)
	{
		const float A = 1.f - static_cast<float>(HitAge / 0.7);
		Text(Damage[FMath::Clamp(Mode.GetPulseAmount(AFTBeatBossGameMode::EPulse::BossHit), 0, kBossNumbers - 1)], BossAlpha(kBossWhite, A),
			AX + 110.f * S, AY - 20.f * S - static_cast<float>(HitAge) * 70.f * S, GEngine->GetLargeFont(), 1.1f, 1);
	}
}

void AFTBeatBossHUD::DrawBeatLine(const AFTBeatBossGameMode& Mode)
{
	const float S = UiScale;
	const ftcore::BeatBoss& Fight = Mode.GetFight();
	const ftcore::BeatLine& Line = Mode.GetLine();
	const ftcore::BeatBossDef& Def = Mode.GetTrack().def;
	const double Now = Mode.GetNow();

	const float X0 = 40.f * S;
	const float X1 = Canvas->ClipX - 40.f * S;
	const float LY = Canvas->ClipY * 0.79f;
	const float Top = LY - 82.f * S;
	const float Bottom = LY + 64.f * S;
	const float HitX = X0 + (X1 - X0) * kBossHitShare;
	const float Pps = static_cast<float>((X1 - HitX) / kBossLineAhead);
	const float Shade = Line.frozen ? 0.4f : 1.f;
	const auto ScreenX = [&](double At) { return HitX + static_cast<float>(At - Line.playhead) * Pps; };

	DrawRect(kBossPanel, X0 - 10.f * S, Top, X1 - X0 + 20.f * S, Bottom - Top);

	// The bars: a tint for a count, a finisher and a broken meter, and a label at each bar's start.
	double Start = 0.0;
	UFont* Small = GEngine->GetMediumFont();
	for (int32 K = 0; K < 2; ++K)
	{
		const ftcore::BeatLineBar& B = Line.bars[static_cast<size_t>(K)];
		const float BX0 = FMath::Max(X0, ScreenX(Start));
		const float BX1 = FMath::Min(X1, ScreenX(Start + B.seconds));
		if (BX1 > BX0)
		{
			FLinearColor Tint(0.f, 0.f, 0.f, 0.f);
			if (B.count)
			{
				Tint = BossAlpha(kBossCountBlue, 0.22f);
			}
			else if (B.finisher)
			{
				Tint = BossAlpha(kBossGold, 0.2f);
			}
			else if (B.shortBar || B.longBar)
			{
				Tint = BossAlpha(kBossOrange, 0.14f);
			}
			if (Tint.A > 0.f)
			{
				DrawRect(BossAlpha(Tint, Shade), BX0, LY - 46.f * S, BX1 - BX0, 92.f * S);
			}
		}
		const float LX = ScreenX(Start) + 8.f * S;
		if (LX > X0 - 2.f && LX < X1 - 60.f * S)
		{
			const float TY = Top + 6.f * S;
			if (B.count)
			{
				Text(kCountBoss, BossAlpha(kBossCountBlue, Shade), LX, TY, Small, 0.95f);
			}
			else if (B.finisher)
			{
				Text(kFinisherBarBoss, BossAlpha(kBossGold, Shade), LX, TY, Small, 0.95f);
			}
			else
			{
				const ftcore::BarMap& Map = Def.phases[static_cast<size_t>(FMath::Clamp(Fight.phase(), 0, FMath::Max(0, static_cast<int32>(Def.phaseCount) - 1)))];
				const ftcore::BossBar& Shape = BarShapeOf(Map, B.number);
				float W = 0.f;
				float H = 0.f;
				Text(Number(B.number), BossAlpha(kBossWhite, Shade), LX, TY, Small, 0.95f);
				GetTextSize(Number(B.number), W, H, Small, 0.95f * FontScale);
				Text(Meters[FMath::Clamp<int32>(Shape.meterCount, 0, kBossMeterSide - 1) * kBossMeterSide + FMath::Clamp<int32>(Shape.meterUnit, 0, kBossMeterSide - 1)],
					BossAlpha((B.shortBar || B.longBar) ? kBossOrange : kBossDim, Shade), LX + W + 10.f * S, TY, Small, 0.95f);
			}
		}
		Start += B.seconds;
	}

	// The staff line.
	DrawRect(BossAlpha(kBossDim, 0.6f * Shade), X0, LY - 1.f * S, X1 - X0, 2.f * S);

	// Beats and bar lines; a count's numerals over its beats.
	for (size_t I = 0; I < Line.tickCount; ++I)
	{
		const ftcore::BeatLineTick& T = Line.ticks[I];
		const float X = ScreenX(T.at);
		if (X < X0 || X > X1)
		{
			continue;
		}
		switch (T.kind)
		{
		case ftcore::BeatTickKind::BarLine:
		case ftcore::BeatTickKind::End:
			DrawRect(BossAlpha(kBossWhite, 0.9f * Shade), X - 1.5f * S, LY - 46.f * S, 3.f * S, 92.f * S);
			break;
		case ftcore::BeatTickKind::Beat:
			DrawRect(BossAlpha(kBossWhite, 0.6f * Shade), X - 1.f * S, LY - 18.f * S, 2.f * S, 36.f * S);
			break;
		case ftcore::BeatTickKind::Empty:
		{
			// An empty beat: nothing on it, a small cross where the beat would be.
			const float R = 6.f * S;
			AHUD::DrawLine(X - R, LY - R, X + R, LY + R, BossAlpha(kBossGrey, Shade), 2.f * S);
			AHUD::DrawLine(X - R, LY + R, X + R, LY - R, BossAlpha(kBossGrey, Shade), 2.f * S);
			break;
		}
		default:
			break;
		}
		if (T.numeral > 0)
		{
			Text(Number(T.numeral), BossAlpha(kBossGold, Shade), X, LY - 44.f * S, GEngine->GetLargeFont(), 1.0f, 1);
		}
	}

	// Rests: diamonds under the line, green while open.
	for (size_t I = 0; I < Line.restCount; ++I)
	{
		const ftcore::BeatLineRest& R = Line.rests[I];
		const float X = ScreenX(R.at);
		if (X < X0 || X > X1)
		{
			continue;
		}
		Diamond(X, LY + 50.f * S, 8.f * S, BossAlpha(R.open ? kBossGreen : kBossGrey, Shade), R.open);
	}

	// The hit line, flashing with each judgement.
	const float Judged = BossFade(Now, Mode.GetPulse(AFTBeatBossGameMode::EPulse::Judged), 0.3f);
	const int32 Grade = Mode.GetPulseAmount(AFTBeatBossGameMode::EPulse::Judged);
	const FLinearColor Flash = Grade == static_cast<int32>(ftcore::QteGrade::Perfect) ? kBossGold
		: Grade == static_cast<int32>(ftcore::QteGrade::Good)                          ? kBossGreen
																						: kBossRed;
	if (Judged > 0.f)
	{
		DrawRect(BossAlpha(Flash, 0.45f * Judged), HitX - 18.f * S, LY - 60.f * S, 36.f * S, 120.f * S);
	}
	const bool bLocked = Now < Mode.GetLockoutUntil();
	DrawRect(BossAlpha(bLocked ? kBossOrange : kBossWhite, Shade), HitX - 2.f * S, LY - 60.f * S, 4.f * S, 120.f * S);
	if (bLocked)
	{
		Text(kLockedBoss, kBossOrange, HitX, Bottom + 4.f * S, Small, 1.0f, 1);
	}

	// The markers: holds' tails first, then the moves from the farthest in, so the next one is on top.
	for (size_t I = 0; I < Line.markerCount; ++I)
	{
		const ftcore::BeatLineMarker& M = Line.markers[I];
		if (!ftcore::isBossHold(M.answer))
		{
			continue;
		}
		const float XA = FMath::Max(X0, ScreenX(M.at));
		const float XB = FMath::Min(X1, ScreenX(static_cast<double>(M.endX) * Line.seconds));
		if (XB <= XA)
		{
			continue;
		}
		const ftcore::BeatBossPrompt* P = FindPrompt(Fight, M.prompt);
		const bool bHeld = P && P->pressA >= 0.0;
		const bool bTold = P && Fight.approach(*P) >= 0.0;
		DrawRect(BossAlpha(AnswerColor(M.answer), (bHeld ? 0.9f : bTold ? 0.55f : 0.25f) * Shade), XA, LY - 6.f * S, XB - XA, 12.f * S);
	}
	for (int32 I = static_cast<int32>(Line.markerCount) - 1; I >= 0; --I)
	{
		const ftcore::BeatLineMarker& M = Line.markers[static_cast<size_t>(I)];
		const float X = ScreenX(M.at);
		if (X < X0 - 30.f * S || X > X1 + 30.f * S)
		{
			continue;
		}
		const ftcore::BeatBossPrompt* P = FindPrompt(Fight, M.prompt);
		const bool bTold = P && Fight.approach(*P) >= 0.0;
		const bool bCounter = ftcore::isBossCounter(M.answer);
		const bool bGrey = bCounter && bLocked;
		const float Size = M.stem < Def.stemCount ? Def.stemSize[M.stem] : 0.6f;
		const float R = (12.f + 12.f * FMath::Clamp(Size, 0.f, 1.f)) * S;
		const float A = (bTold ? 1.f : 0.38f) * Shade;
		const FLinearColor C = BossAlpha(bGrey ? kBossGrey : AnswerColor(M.answer), A);

		if (bCounter)
		{
			Disc(X, LY, R, BossAlpha(C, 0.85f));
			Spikes(X, LY, R, C, 3.f * S);
		}
		else
		{
			Disc(X, LY, R * 0.55f, BossAlpha(C, 0.7f));
			Ring(X, LY, R, C, 4.f * S, 24);
		}
		if (bGrey)
		{
			AHUD::DrawLine(X - R, LY - R, X + R, LY + R, BossAlpha(kBossWhite, A), 3.f * S);
			AHUD::DrawLine(X - R, LY + R, X + R, LY - R, BossAlpha(kBossWhite, A), 3.f * S);
		}
		// A counter's or the Release's answer over it, once it is told.
		const int32 Answer = static_cast<int32>(M.answer);
		if (bTold && Answer < kAnswerBossCount && (bCounter || M.answer == ftcore::BeatBossAnswer::Release || M.answer == ftcore::BeatBossAnswer::Hold))
		{
			Text(*kAnswerBoss[Answer], C, X, LY - R - 30.f * S, Small, 0.95f, 1);
		}
		// The button under it (both for a paired answer).
		const float GY = LY + R + 18.f * S;
		const float GR = 16.f * S;
		const int32 First = P ? static_cast<int32>(P->button) : AnswerButton(M.answer);
		if (M.answer == ftcore::BeatBossAnswer::Paired)
		{
			const int32 Second = P ? static_cast<int32>(P->second) : static_cast<int32>(ftcore::ActionButton::Dodge);
			FTActionButtons::DrawGlyph(*this, First, X - GR * 1.15f, GY, GR, C, 2.f * S);
			FTActionButtons::DrawGlyph(*this, Second, X + GR * 1.15f, GY, GR, C, 2.f * S);
		}
		else
		{
			FTActionButtons::DrawGlyph(*this, First, X, GY, GR, C, 2.f * S);
		}
	}

	// A scripted silence: the line stands still under a banner and a bar draining to the count.
	if (Line.frozen && Mode.GetSilenceSeconds() > 0.0)
	{
		const double Left = 1.0 - (Now - Mode.GetPulse(AFTBeatBossGameMode::EPulse::Silence)) / Mode.GetSilenceSeconds();
		const float CX = (X0 + X1) * 0.5f;
		Text(kSilenceBoss, kBossWhite, CX, LY - 30.f * S, GEngine->GetLargeFont(), 1.4f, 1);
		Bar(CX - 160.f * S, LY + 22.f * S, 320.f * S, 8.f * S, static_cast<float>(FMath::Clamp(Left, 0.0, 1.0)), kBossWhite, kBossBack);
	}
}

void AFTBeatBossHUD::DrawLeadPanel(const AFTBeatBossGameMode& Mode)
{
	const float S = UiScale;
	const ftcore::BeatBoss& Fight = Mode.GetFight();
	const double Now = Mode.GetNow();
	UFont* Medium = GEngine->GetMediumFont();
	const float Y = Canvas->ClipY * 0.79f + 76.f * S;
	float X = 40.f * S;

	// The lead's hp.
	const float HpFrac = static_cast<float>(Fight.hp()) / static_cast<float>(FMath::Max(1, Fight.maxHp()));
	const float Hit = BossFade(Now, Mode.GetPulse(AFTBeatBossGameMode::EPulse::LeadHit), 0.4f);
	Text(kLeadBoss, kBossDim, X, Y, Medium, 1.0f);
	X += 44.f * S;
	Bar(X, Y + 4.f * S, 260.f * S, 16.f * S, HpFrac, FMath::Lerp(HpFrac < 0.3f ? kBossRed : kBossGreen, kBossWhite, Hit), kBossBack);
	Text(Number(Fight.hp()), kBossWhite, X + 270.f * S, Y, Medium, 1.0f);
	X += 330.f * S;

	// The Load, stored by perfect guards and spent by the Release.
	const float Stored = BossFade(Now, Mode.GetPulse(AFTBeatBossGameMode::EPulse::LoadStored), 0.5f);
	Text(kLoadBoss, kBossDim, X, Y, Medium, 1.0f);
	X += 60.f * S;
	Bar(X, Y + 4.f * S, 200.f * S, 16.f * S, static_cast<float>(Fight.load()) / static_cast<float>(ftcore::kMaxLoad),
		FMath::Lerp(kBossOrange, kBossWhite, Stored), kBossBack);
	Text(Number(Fight.load()), kBossOrange, X + 210.f * S, Y, Medium, 1.0f);

	// The streak and the multiplier, and the open rests (a stagger at staggerOpens).
	const float RX = Canvas->ClipX - 40.f * S;
	const float Mul = BossFade(Now, Mode.GetPulse(AFTBeatBossGameMode::EPulse::Multiplier), 0.5f);
	Text(Multipliers[FMath::Clamp(Fight.multiplier(), 0, 9)], FMath::Lerp(kBossWhite, kBossGold, FMath::Max(Mul, Fight.multiplier() > 1 ? 0.7f : 0.f)), RX,
		Y - 10.f * S - 8.f * Mul * S, GEngine->GetLargeFont(), 1.2f + 0.5f * Mul, 2);
	Text(Number(Fight.streak()), kBossWhite, RX - 90.f * S, Y, Medium, 1.1f, 2);
	Text(kStreakBoss, kBossDim, RX - 140.f * S, Y, Medium, 1.0f, 2);
	const int32 Opens = Mode.GetTrack().def.staggerOpens;
	if (Opens > 0)
	{
		float PX = RX - 300.f * S;
		const float Lit = BossFade(Now, Mode.GetPulse(AFTBeatBossGameMode::EPulse::RestOpened), 0.5f);
		for (int32 I = Opens - 1; I >= 0; --I)
		{
			Diamond(PX, Y + 12.f * S, (8.f + (I < Fight.openRests() ? 2.f * Lit : 0.f)) * S, I < Fight.openRests() ? kBossGreen : kBossGrey,
				I < Fight.openRests());
			PX -= 24.f * S;
		}
		Text(kRestsBoss, kBossDim, PX + 4.f * S, Y, Medium, 1.0f, 2);
	}
}

void AFTBeatBossHUD::DrawCount(const AFTBeatBossGameMode& Mode)
{
	const ftcore::BeatBoss& Fight = Mode.GetFight();
	if (Mode.GetStage() != AFTBeatBossGameMode::EStage::Playing || !Fight.counting())
	{
		return;
	}
	const double Now = Mode.GetNow();
	const float Age = static_cast<float>(Now - Mode.GetPulse(AFTBeatBossGameMode::EPulse::Count));
	const int32 Numeral = Mode.GetPulseAmount(AFTBeatBossGameMode::EPulse::Count);
	if (Age < 0.f || Age > 0.9f || Numeral <= 0)
	{
		return;
	}
	// Full strength for most of the beat, popping big on it and settling.
	const float A = FMath::Clamp((0.9f - Age) / 0.3f, 0.f, 1.f);
	const float Pop = FMath::Clamp(1.f - Age / 0.15f, 0.f, 1.f);
	Text(Number(Numeral), BossAlpha(kBossGold, A), Canvas->ClipX * 0.5f, Canvas->ClipY * 0.32f - 60.f * UiScale, GEngine->GetLargeFont(), 6.f + 2.f * Pop, 1);
}

void AFTBeatBossHUD::DrawPopups(const AFTBeatBossGameMode& Mode)
{
	const float S = UiScale;
	const double Now = Mode.GetNow();
	const float X0 = 40.f * S;
	const float X1 = Canvas->ClipX - 40.f * S;
	const float HitX = X0 + (X1 - X0) * kBossHitShare;
	const float BaseY = Canvas->ClipY * 0.79f - 128.f * S;
	const ftcore::BeatBossDef& Def = Mode.GetTrack().def;
	int32 Shown = 0;
	for (int32 I = Mode.GetPopupCount() - 1; I >= 0 && Shown < 3; --I)
	{
		const AFTBeatBossGameMode::FPopup& P = Mode.GetPopup(I);
		const float Age = static_cast<float>(Now - P.Time);
		if (Age < 0.f || Age > kBossPopupLife)
		{
			continue;
		}
		// Solid for the first half of its life, then fading.
		const float A = FMath::Min(1.f, 2.f * (1.f - Age / kBossPopupLife));
		const FString* Str = nullptr;
		FLinearColor C = kBossWhite;
		float Scale = 1.3f;
		switch (P.Type)
		{
		case ftcore::BeatBossEventType::Judged:
			if (P.Grade == ftcore::QteGrade::Perfect)
			{
				Str = &kPerfectBoss.Get();
				C = kBossGold;
				Scale = 1.6f;
			}
			else if (P.Grade == ftcore::QteGrade::Good)
			{
				Str = &kGoodBoss.Get();
				C = kBossGreen;
			}
			else
			{
				Str = &kMissBoss.Get();
				C = kBossRed;
				Scale = 1.5f;
			}
			break;
		case ftcore::BeatBossEventType::Whiff:
			Str = &kWhiffBoss.Get();
			C = kBossOrange;
			break;
		case ftcore::BeatBossEventType::LockedOut:
			Str = &kLockedBoss.Get();
			C = kBossGrey;
			break;
		case ftcore::BeatBossEventType::LoadStored:
			Str = &LoadGains[FMath::Clamp(P.Amount, 0, LoadGains.Num() - 1)];
			C = kBossOrange;
			break;
		case ftcore::BeatBossEventType::Reward:
			if (P.Amount >= 0 && P.Amount < Def.rewardCount)
			{
				const ftcore::BossReward& R = Def.rewards[static_cast<size_t>(P.Amount)];
				if (R.effect == ftcore::BossRewardEffect::Open)
				{
					Str = &kRestOpenBoss.Get();
				}
				else if (R.effect == ftcore::BossRewardEffect::Heal)
				{
					Str = &kHealBoss.Get();
				}
				else if (R.effect == ftcore::BossRewardEffect::Load)
				{
					Str = &LoadGains[FMath::Clamp(R.amount, 0, LoadGains.Num() - 1)];
				}
				C = kBossGreen;
			}
			break;
		default:
			break;
		}
		if (!Str)
		{
			continue;
		}
		// On a plate, so it reads over a bright floor.
		const float PY = BaseY - 40.f * S * static_cast<float>(Shown) - Age * 40.f * S;
		float W = 0.f;
		float H = 0.f;
		GetTextSize(*Str, W, H, GEngine->GetLargeFont(), Scale * FontScale);
		DrawRect(BossAlpha(kBossPanel, 0.8f * A), HitX - W * 0.5f - 10.f * S, PY - 3.f * S, W + 20.f * S, H + 6.f * S);
		Text(*Str, BossAlpha(C, A), HitX, PY, GEngine->GetLargeFont(), Scale, 1);
		++Shown;
	}
}

void AFTBeatBossHUD::DrawBanners(const AFTBeatBossGameMode& Mode)
{
	const float S = UiScale;
	const double Now = Mode.GetNow();

	// The lead was hit: the screen's edges flash red.
	const float Hit = BossFade(Now, Mode.GetPulse(AFTBeatBossGameMode::EPulse::LeadHit), 0.45f);
	if (Hit > 0.f)
	{
		const FLinearColor C = BossAlpha(kBossRed, 0.55f * Hit);
		const float T = 26.f * S;
		Frame(0.f, 0.f, Canvas->ClipX, Canvas->ClipY, C, T);
	}

	// The newest of the banners.
	struct FBanner
	{
		AFTBeatBossGameMode::EPulse Pulse;
		const FString* Str;
		FLinearColor Color;
	};
	const FBanner Banners[] = {
		{AFTBeatBossGameMode::EPulse::Finisher, &kFinisherBoss.Get(), kBossGold},
		{AFTBeatBossGameMode::EPulse::FinisherBroken, &kBrokenBoss.Get(), kBossRed},
		{AFTBeatBossGameMode::EPulse::Stagger, &kStaggerBoss.Get(), kBossYellow},
		{AFTBeatBossGameMode::EPulse::Released, &kReleaseBoss.Get(), kBossOrange},
		{AFTBeatBossGameMode::EPulse::Phase, nullptr, kBossWhite},
	};
	const FBanner* Newest = nullptr;
	double NewestTime = -1e9;
	for (const FBanner& B : Banners)
	{
		const double T = Mode.GetPulse(B.Pulse);
		if (T > NewestTime && Now - T >= 0.0 && Now - T < kBossBannerLife)
		{
			Newest = &B;
			NewestTime = T;
		}
	}
	if (!Newest || Mode.GetFight().counting())
	{
		return;
	}
	const float A = BossFade(Now, NewestTime, kBossBannerLife);
	const float Pop = 1.f + 0.25f * FMath::Max(0.f, (A - 0.8f) / 0.2f);
	const FString& Str = Newest->Str ? *Newest->Str
									 : PhaseLabels[FMath::Clamp(Mode.GetPulseAmount(AFTBeatBossGameMode::EPulse::Phase), 0, PhaseLabels.Num() - 1)];
	// Left of the boss, over the open sky, on a plate: clear of the counter's ring above the boss.
	const float BX = Canvas->ClipX * 0.24f;
	const float BY = Canvas->ClipY * 0.34f;
	const float BA = FMath::Min(1.f, A * 2.f);
	float W = 0.f;
	float H = 0.f;
	GetTextSize(Str, W, H, GEngine->GetLargeFont(), 2.2f * Pop * FontScale);
	DrawRect(BossAlpha(kBossPanel, 0.8f * BA), BX - W * 0.5f - 16.f * S, BY - 6.f * S, W + 32.f * S, H + 12.f * S);
	Text(Str, BossAlpha(Newest->Color, BA), BX, BY, GEngine->GetLargeFont(), 2.2f * Pop, 1);
}

void AFTBeatBossHUD::DrawResult(const AFTBeatBossGameMode& Mode)
{
	const float S = UiScale;
	const ftcore::BeatBoss& Fight = Mode.GetFight();
	const bool bWon = Fight.state() == ftcore::BeatBossState::Victory;
	if (StatsFor < 0)
	{
		// Built once a result appears, not each frame.
		const ftcore::BeatBossStats& St = Fight.stats();
		StatsText = FString::Format(*kStatsBoss.Get(), FStringFormatOrderedArguments{FStringFormatArg(St.perfects), FStringFormatArg(St.goods),
															FStringFormatArg(St.misses), FStringFormatArg(St.maxStreak), FStringFormatArg(St.bars),
															FStringFormatArg(St.retries)});
		StatsFor = 1;
	}
	const float A = FMath::Clamp(static_cast<float>(Mode.GetResultAge() / 0.25), 0.f, 1.f);
	const float CY = Canvas->ClipY * 0.4f;
	DrawRect(BossAlpha(kBossPanel, A), 0.f, CY - 70.f * S, Canvas->ClipX, 210.f * S);
	Text(bWon ? kVictoryBoss.Get() : kDefeatBoss.Get(), BossAlpha(bWon ? kBossGold : kBossRed, A), Canvas->ClipX * 0.5f, CY - 56.f * S,
		GEngine->GetLargeFont(), 2.4f, 1);
	Text(StatsText, BossAlpha(kBossWhite, A), Canvas->ClipX * 0.5f, CY + 30.f * S, GEngine->GetMediumFont(), 1.1f, 1);
	// The retry prompt shows on an autoplay run too, which confirms it on its own a second later; the
	// play-again prompt only when keys are read.
	const bool bKeys = !Mode.IsAutoPlay() && !Mode.IsIdle();
	switch (Mode.GetStage())
	{
	case AFTBeatBossGameMode::EStage::RetryPrompt:
	{
		const int32 Phase = FMath::Clamp(Fight.checkpointPhase(), 0, RetryLabels.Num() - 1);
		Text(RetryLabels[Phase], BossAlpha(kBossGold, A), Canvas->ClipX * 0.5f, CY + 70.f * S, GEngine->GetMediumFont(), 1.1f, 1);
		Text(kQuitBoss, BossAlpha(kBossDim, A), Canvas->ClipX * 0.5f, CY + 100.f * S, GEngine->GetMediumFont(), 1.0f, 1);
		break;
	}
	case AFTBeatBossGameMode::EStage::Done:
		if (!bKeys)
		{
			break;
		}
		Text(kAgainBoss, BossAlpha(kBossDim, A), Canvas->ClipX * 0.5f, CY + 70.f * S, GEngine->GetMediumFont(), 1.1f, 1);
		break;
	case AFTBeatBossGameMode::EStage::Result:
		if (bWon && bKeys)
		{
			Text(kAgainBoss, BossAlpha(kBossDim, A), Canvas->ClipX * 0.5f, CY + 70.f * S, GEngine->GetMediumFont(), 1.1f, 1);
		}
		break;
	default:
		break;
	}
}

void AFTBeatBossHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const AFTBeatBossGameMode* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AFTBeatBossGameMode>() : nullptr;
	if (!Mode || !Mode->IsBegun())
	{
		// Nothing to show yet: the fight begins once the clock runs.
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	if (!bBuilt)
	{
		BuildTexts(*Mode);
	}
	UiScale = FMath::Max(0.5f, Canvas->ClipY / 1080.f);
	FontScale = UiScale * kBossFontBoost;
	const float S = UiScale;
	const bool bPlaying = Mode->GetStage() == AFTBeatBossGameMode::EStage::Playing;
	if (bPlaying)
	{
		StatsFor = -1; // the next result gets its own counts
	}

	DrawBoss(*Mode);
	if (bPlaying)
	{
		DrawBeatLine(*Mode);
	}
	DrawLeadPanel(*Mode);
	DrawCount(*Mode);
	if (bPlaying)
	{
		DrawPopups(*Mode);
	}
	if (bPlaying)
	{
		DrawBanners(*Mode);
	}
	else
	{
		DrawResult(*Mode);
	}
	if (Mode->IsAutoPlay())
	{
		Text(kAutoPlayBoss, kBossGold, 24.f * S, 18.f * S, GEngine->GetMediumFont(), 1.0f);
	}
	else if (Mode->IsIdle())
	{
		Text(kIdleBoss, kBossDim, 24.f * S, 18.f * S, GEngine->GetMediumFont(), 1.0f);
	}
	Text(kHintBoss, kBossDim, Canvas->ClipX * 0.5f, Canvas->ClipY - 30.f * S, GEngine->GetSmallFont(), 1.2f, 1);
	FT::DrawPauseOverlay(*this, Canvas);
}
