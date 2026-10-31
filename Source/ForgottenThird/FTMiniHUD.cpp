#include "FTMiniHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "FTActionButtons.h"
#include "FTMiniGameMode.h"
#include "FTPauseOverlay.h"
#include "FTUiText.h"

namespace
{
const FLinearColor kGoldMini(1.0f, 0.82f, 0.25f);
const FLinearColor kWhiteMini(1.f, 1.f, 1.f);
const FLinearColor kDimMini(0.55f, 0.55f, 0.6f);
const FLinearColor kPanelMini(0.02f, 0.02f, 0.05f, 0.75f);
const FLinearColor kLaneMini(0.12f, 0.12f, 0.16f, 0.9f);
const FLinearColor kLaneOffMini(0.06f, 0.06f, 0.08f, 0.6f);
const FLinearColor kRedMini(1.0f, 0.3f, 0.3f);
const FLinearColor kCyanMini(0.35f, 0.85f, 1.f);
// A colour per lane: lane i is ftcore::ActionButton i (ftcore::miniButton).
const FLinearColor kNoteMini[4] = {FLinearColor(1.0f, 0.45f, 0.6f), FLinearColor(0.35f, 0.85f, 1.f), FLinearColor(0.5f, 1.f, 0.5f),
	FLinearColor(1.0f, 0.82f, 0.25f)};

// Static so drawing never builds strings.
const FTUiText kNoStepMini(TEXT("common.no_step"));
const FTUiText kHintMini(TEXT("common.buttons_hint"));
const FTUiText kTryMini(TEXT("mini.try"));
// By ftcore::Grade (Pending, Perfect, Great, Good, Miss).
const FString kGradeMini[5] = {FString(), FString(TEXT("PERFECT")), FString(TEXT("GREAT")), FString(TEXT("GOOD")), FString(TEXT("MISS"))};
// By ftcore::MiniOutcome (Pending, Cleared, Retry, Continue), matching ftcore::miniOutcomeName.
const FString kOutcomeMini[4] = {FString(), FString(TEXT("CLEARED")), FString(TEXT("AGAIN")), FString(TEXT("ONWARD"))};

constexpr double kLookAheadMini = 1.6; // seconds of round time from the top of a lane to the line
constexpr double kGradeLifeMini = 0.5; // seconds of round time a grade shows

FLinearColor WithAlphaMini(const FLinearColor& C, float A)
{
	return FLinearColor(C.R, C.G, C.B, A);
}
} // namespace

void AFTMiniHUD::DrawCentered(const FString& Text, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	GetTextSize(Text, W, H, Font, Scale * UiScale);
	DrawText(Text, Color, X - W * 0.5f, Y, Font, Scale * UiScale);
}

void AFTMiniHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const AFTMiniGameMode* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AFTMiniGameMode>() : nullptr;
	if (!Mode)
	{
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	UiScale = FMath::Max(0.5f, Canvas->ClipY / 1080.f);
	const float S = UiScale;
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;

	if (!Mode->HasStep())
	{
		const float W = 760.f * S;
		const float H = 300.f * S;
		DrawRect(kPanelMini, CX - W * 0.5f, CY - H * 0.5f, W, H);
		DrawCentered(kNoStepMini, kWhiteMini, CX, CY - 20.f * S, GEngine->GetLargeFont(), 1.4f);
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	const ftcore::MiniScreen& Screen = Mode->GetScreen();
	if (Screen.finished())
	{
		FT::DrawPauseOverlay(*this, Canvas);
		return; // the map is about to change
	}
	const ftcore::ComboSequence& Seq = Screen.sequence();
	const double Now = Screen.time();

	if (CachedAttempt != Screen.attempt())
	{
		CachedAttempt = Screen.attempt();
		TryText = FString::Format(*kTryMini.Get(), {Screen.triesUsed(), FMath::Max(Screen.def().tries, 1)});
	}
	const int32 Outcome = static_cast<int32>(Screen.outcome());
	if (CachedOutcome != Outcome)
	{
		CachedOutcome = Outcome;
		AccuracyText = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Screen.lastAccuracy() * 100.f));
	}

	// The board: four lanes, the judgement line near the bottom.
	const float LaneW = 110.f * S;
	const float Gap = 14.f * S;
	const float BoardW = 4.f * LaneW + 3.f * Gap;
	const float X0 = CX - BoardW * 0.5f;
	const float Top = 90.f * S;
	const float LineY = Canvas->ClipY - 190.f * S;
	const float Travel = LineY - Top;
	const float NoteH = 26.f * S;

	bool bUsed[4] = {};
	for (std::size_t I = 0; I < Seq.size(); ++I)
	{
		bUsed[static_cast<int32>(Seq.note(I).arrow) & 3] = true;
	}
	for (int32 L = 0; L < 4; ++L)
	{
		const float LX = X0 + L * (LaneW + Gap);
		DrawRect(bUsed[L] ? kLaneMini : kLaneOffMini, LX, Top, LaneW, LineY - Top + 40.f * S);
		DrawCentered(FTActionButtons::Label(L), bUsed[L] ? kDimMini : WithAlphaMini(kDimMini, 0.35f), LX + LaneW * 0.5f, LineY + 48.f * S,
			GEngine->GetSmallFont(), 1.2f);
	}

	// The line pulses on the beat.
	const double Beat = Screen.beat();
	const float Pulse = Beat >= 0.0 ? static_cast<float>(1.0 - (Beat - FMath::FloorToDouble(Beat))) : 0.f;
	DrawRect(WithAlphaMini(kWhiteMini, 0.35f + 0.55f * Pulse * Pulse), X0, LineY - 2.f * S, BoardW, (4.f + 4.f * Pulse) * S);

	if (Screen.playing())
	{
		for (std::size_t I = 0; I < Seq.size(); ++I)
		{
			const ftcore::ComboNote& N = Seq.note(I);
			const ftcore::Grade G = Seq.grade(I);
			const double Ahead = N.time - Now;
			if (Ahead > kLookAheadMini || (G != ftcore::Grade::Pending && G != ftcore::Grade::Miss))
			{
				continue; // not in view yet, or hit (a hit note vanishes)
			}
			const float Y = LineY - static_cast<float>(Ahead / kLookAheadMini) * Travel;
			if (Y > Canvas->ClipY)
			{
				continue;
			}
			const int32 L = static_cast<int32>(N.arrow) & 3;
			const float LX = X0 + L * (LaneW + Gap);
			const FLinearColor& C = G == ftcore::Grade::Miss ? kRedMini : kNoteMini[L];
			DrawRect(WithAlphaMini(C, G == ftcore::Grade::Miss ? 0.5f : 1.f), LX + 8.f * S, Y - NoteH * 0.5f, LaneW - 16.f * S, NoteH);
		}

		const ftcore::Grade Last = Screen.lastGrade();
		const double Age = Now - Screen.lastGradeTime();
		if (Last != ftcore::Grade::Pending && Age >= 0.0 && Age < kGradeLifeMini)
		{
			const float A = static_cast<float>(1.0 - Age / kGradeLifeMini);
			const FLinearColor& C = Last == ftcore::Grade::Miss ? kRedMini : (Last == ftcore::Grade::Perfect ? kGoldMini : kCyanMini);
			DrawCentered(kGradeMini[static_cast<int32>(Last) % 5], WithAlphaMini(C, A), CX, LineY - 140.f * S, GEngine->GetLargeFont(), 1.6f);
		}
	}
	else if (Screen.showingResult())
	{
		const float W = 560.f * S;
		const float H = 200.f * S;
		DrawRect(kPanelMini, CX - W * 0.5f, CY - H * 0.5f, W, H);
		const FLinearColor& C = Screen.outcome() == ftcore::MiniOutcome::Cleared ? kGoldMini : kWhiteMini;
		DrawCentered(kOutcomeMini[Outcome & 3], C, CX, CY - 70.f * S, GEngine->GetLargeFont(), 2.0f);
		DrawCentered(AccuracyText, kWhiteMini, CX, CY + 20.f * S, GEngine->GetMediumFont(), 1.5f);
	}

	DrawText(TryText, kDimMini, X0, Top - 50.f * S, GEngine->GetMediumFont(), 1.2f * S);
	DrawCentered(kHintMini, kDimMini, CX, Canvas->ClipY - 70.f * S, GEngine->GetSmallFont(), 1.2f);
	FT::DrawPauseOverlay(*this, Canvas);
}
