#include "FTBanterHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "FTBanterGameMode.h"
#include "FTPauseOverlay.h"
#include "FTUiText.h"

namespace
{
const FLinearColor kGoldBanter(1.0f, 0.82f, 0.25f);
const FLinearColor kTealBanter(0.45f, 0.85f, 0.9f);
const FLinearColor kWhiteBanter(1.f, 1.f, 1.f);
const FLinearColor kDimBanter(0.6f, 0.6f, 0.65f);
const FLinearColor kPanelBanter(0.02f, 0.02f, 0.05f, 0.75f);
const FLinearColor kBarBanter(1.f, 1.f, 1.f, 0.35f);

// Static so drawing never builds strings. The card's match AFTStubHUD's.
const FTUiText kNoStepBanter(TEXT("common.no_step"));
const FTUiText kContinueBanter(TEXT("common.continue_pad"));
const FTUiText kPromptBanter(TEXT("banter.prompt"));
} // namespace

void AFTBanterHUD::DrawCentered(const FString& Text, const FLinearColor& Color, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	GetTextSize(Text, W, H, Font, Scale * UiScale);
	DrawText(Text, Color, Canvas->ClipX * 0.5f - W * 0.5f, Y, Font, Scale * UiScale);
}

void AFTBanterHUD::Wrap(const FString& Src, UFont* Font, float Scale, float MaxWidth)
{
	Lines.Reset();
	FString Current;
	const int32 N = Src.Len();
	int32 I = 0;
	while (I < N)
	{
		int32 J = I;
		while (J < N && Src[J] != TCHAR(' ') && Src[J] != TCHAR('\n'))
		{
			++J;
		}
		const FString Word = Src.Mid(I, J - I);
		const FString Candidate = Current.IsEmpty() ? Word : Current + TEXT(" ") + Word;
		float W = 0.f;
		float H = 0.f;
		GetTextSize(Candidate, W, H, Font, Scale);
		if (W > MaxWidth && !Current.IsEmpty())
		{
			Lines.Add(Current);
			Current = Word;
		}
		else
		{
			Current = Candidate;
		}
		if (J < N && Src[J] == TCHAR('\n'))
		{
			Lines.Add(Current);
			Current.Reset();
		}
		I = J + 1;
	}
	if (!Current.IsEmpty())
	{
		Lines.Add(Current);
	}
}

void AFTBanterHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const AFTBanterGameMode* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AFTBanterGameMode>() : nullptr;
	if (!Mode)
	{
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	UiScale = FMath::Max(0.5f, Canvas->ClipY / 1080.f);
	if (Mode->HasBanter())
	{
		DrawBanter(*Mode);
	}
	else
	{
		DrawCard(*Mode);
	}
	FT::DrawPauseOverlay(*this, Canvas);
}

// The stub card, as AFTStubHUD draws it.
void AFTBanterHUD::DrawCard(const AFTBanterGameMode& Mode)
{
	const float S = UiScale;
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	const float W = 760.f * S;
	const float H = 300.f * S;
	DrawRect(kPanelBanter, CX - W * 0.5f, CY - H * 0.5f, W, H);

	if (!Mode.HasStep())
	{
		DrawCentered(kNoStepBanter, kWhiteBanter, CY - 20.f * S, GEngine->GetLargeFont(), 1.4f);
		return;
	}
	float Y = CY - H * 0.5f + 40.f * S;
	DrawCentered(Mode.GetTitleText(), kGoldBanter, Y, GEngine->GetLargeFont(), 2.2f);
	Y += 110.f * S;
	if (!Mode.GetLabelText().IsEmpty())
	{
		DrawCentered(Mode.GetLabelText(), kWhiteBanter, Y, GEngine->GetMediumFont(), 1.5f);
	}
	Y += 50.f * S;
	DrawCentered(Mode.GetStepText(), kDimBanter, Y, GEngine->GetMediumFont(), 1.2f);
	DrawCentered(kContinueBanter, kDimBanter, CY + H * 0.5f - 50.f * S, GEngine->GetSmallFont(), 1.3f);
}

void AFTBanterHUD::DrawBanter(const AFTBanterGameMode& Mode)
{
	const ftcore::BanterSequence& Banter = Mode.GetBanter();
	if (!Banter.active())
	{
		return; // finished: the map is about to change
	}
	const float S = UiScale;
	UFont* Font = GEngine->GetMediumFont();
	const float TextScale = 1.4f * S;
	const float LineH = 34.f * S;
	const float Margin = 40.f * S;
	const float W = FMath::Min(Canvas->ClipX - 2.f * Margin, 1000.f * S);
	const bool bRight = Banter.side() == ftcore::BanterSide::Right;
	const float X = bRight ? Canvas->ClipX - Margin - W : Margin;
	const float Pad = 28.f * S;
	const float TextW = W - 2.f * Pad;

	if (WrappedSerial != Mode.GetSerial() || WrappedWidth != TextW)
	{
		Wrap(Mode.GetText(), Font, TextScale, TextW);
		WrappedSerial = Mode.GetSerial();
		WrappedWidth = TextW;
	}
	if (CountSerial != Mode.GetSerial())
	{
		CountText = FString::Printf(TEXT("%u / %u"), Banter.line() + 1, Banter.count());
		CountSerial = Mode.GetSerial();
	}

	const float H = Pad * 2.f + 40.f * S + Lines.Num() * LineH + 36.f * S;
	const float Y0 = Canvas->ClipY - Margin - H;
	DrawRect(kPanelBanter, X, Y0, W, H);

	// The hold bar along the box's top edge: full when the line appears, empty when the next comes.
	const double Hold = Banter.lineHold();
	const float Left = Hold > 0.0 ? static_cast<float>(FMath::Clamp(1.0 - Banter.lineAge() / Hold, 0.0, 1.0)) : 0.f;
	DrawRect(kBarBanter, X, Y0, W * Left, 4.f * S);

	float Y = Y0 + Pad;
	const FString& Speaker = Mode.GetSpeaker();
	if (!Speaker.IsEmpty())
	{
		float SW = 0.f;
		float SH = 0.f;
		GetTextSize(Speaker, SW, SH, Font, 1.3f * S);
		DrawText(Speaker, bRight ? kTealBanter : kGoldBanter, bRight ? X + W - Pad - SW : X + Pad, Y, Font, 1.3f * S);
	}
	Y += 40.f * S;
	for (const FString& L : Lines)
	{
		DrawText(L, kWhiteBanter, X + Pad, Y, Font, TextScale);
		Y += LineH;
	}
	UFont* Small = GEngine->GetSmallFont();
	float PW = 0.f;
	float PH = 0.f;
	GetTextSize(kPromptBanter, PW, PH, Small, 1.2f * S);
	DrawText(kPromptBanter, kDimBanter, X + W - Pad - PW, Y0 + H - Pad - PH, Small, 1.2f * S);
	DrawText(CountText, kDimBanter, X + Pad, Y0 + H - Pad - PH, Small, 1.2f * S);
}
