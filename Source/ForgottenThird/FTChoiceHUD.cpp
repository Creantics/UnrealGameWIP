#include "FTChoiceHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "FTChoiceGameMode.h"
#include "FTPauseOverlay.h"
#include "FTUiText.h"

namespace
{
const FLinearColor kGoldChoice(1.0f, 0.82f, 0.25f);
const FLinearColor kWhiteChoice(1.f, 1.f, 1.f);
const FLinearColor kDimChoice(0.6f, 0.6f, 0.65f);
const FLinearColor kGreyedChoice(0.35f, 0.35f, 0.38f);
const FLinearColor kPanelChoice(0.02f, 0.02f, 0.05f, 0.75f);

// Static so drawing never builds strings. The card's match AFTStubHUD's.
const FTUiText kNoStepChoice(TEXT("common.no_step"));
const FTUiText kContinueChoice(TEXT("common.continue_pad"));
const FTUiText kPromptLine(TEXT("choice.prompt_line"));
const FTUiText kPromptPick(TEXT("choice.prompt_pick"));
const FTUiText kPromptTag(TEXT("common.continue"));
} // namespace

void AFTChoiceHUD::DrawCentered(const FString& Text, const FLinearColor& Color, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	GetTextSize(Text, W, H, Font, Scale * UiScale);
	DrawText(Text, Color, Canvas->ClipX * 0.5f - W * 0.5f, Y, Font, Scale * UiScale);
}

void AFTChoiceHUD::Wrap(const FString& Src, UFont* Font, float Scale, float MaxWidth)
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

void AFTChoiceHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const AFTChoiceGameMode* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AFTChoiceGameMode>() : nullptr;
	if (!Mode)
	{
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	UiScale = FMath::Max(0.5f, Canvas->ClipY / 1080.f);
	if (Mode->HasLoop())
	{
		DrawLoop(*Mode);
	}
	else
	{
		DrawCard(*Mode);
	}
	FT::DrawPauseOverlay(*this, Canvas);
}

// The stub card, as AFTStubHUD draws it.
void AFTChoiceHUD::DrawCard(const AFTChoiceGameMode& Mode)
{
	const float S = UiScale;
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	const float W = 760.f * S;
	const float H = 300.f * S;
	DrawRect(kPanelChoice, CX - W * 0.5f, CY - H * 0.5f, W, H);

	if (!Mode.HasStep())
	{
		DrawCentered(kNoStepChoice, kWhiteChoice, CY - 20.f * S, GEngine->GetLargeFont(), 1.4f);
		return;
	}
	float Y = CY - H * 0.5f + 40.f * S;
	DrawCentered(Mode.GetTitleText(), kGoldChoice, Y, GEngine->GetLargeFont(), 2.2f);
	Y += 110.f * S;
	if (!Mode.GetLabelText().IsEmpty())
	{
		DrawCentered(Mode.GetLabelText(), kWhiteChoice, Y, GEngine->GetMediumFont(), 1.5f);
	}
	Y += 50.f * S;
	DrawCentered(Mode.GetStepText(), kDimChoice, Y, GEngine->GetMediumFont(), 1.2f);
	DrawCentered(kContinueChoice, kDimChoice, CY + H * 0.5f - 50.f * S, GEngine->GetSmallFont(), 1.3f);
}

void AFTChoiceHUD::DrawLoop(const AFTChoiceGameMode& Mode)
{
	const ftcore::DialogueLoop& Loop = Mode.GetLoop();
	if (!Loop.active())
	{
		return; // finished: the map is about to change
	}
	const float S = UiScale;
	UFont* Font = GEngine->GetMediumFont();
	const float TextScale = 1.4f * S;
	const float LineH = 34.f * S;
	const float OptionH = 36.f * S;
	const float Margin = 40.f * S;
	const float W = FMath::Min(Canvas->ClipX - 2.f * Margin, 1400.f * S);
	const float X = (Canvas->ClipX - W) * 0.5f;
	const float Pad = 28.f * S;
	const float TextW = W - 2.f * Pad;

	if (WrappedSerial != Mode.GetSerial() || WrappedWidth != TextW)
	{
		Wrap(Mode.GetText(), Font, TextScale, TextW);
		WrappedSerial = Mode.GetSerial();
		WrappedWidth = TextW;
	}

	const TArray<FString>& Options = Mode.GetOptions();
	const bool bPick = Loop.optionsVisible();
	const int32 OptionCount = bPick ? Options.Num() : 0;
	const float H = Pad * 2.f + 40.f * S + Lines.Num() * LineH + (OptionCount > 0 ? 16.f * S + OptionCount * OptionH : 0.f) + 36.f * S;
	const float Y0 = Canvas->ClipY - Margin - H;
	DrawRect(kPanelChoice, X, Y0, W, H);

	float Y = Y0 + Pad;
	if (!Mode.GetSpeaker().IsEmpty())
	{
		DrawText(Mode.GetSpeaker(), kGoldChoice, X + Pad, Y, Font, 1.3f * S);
	}
	Y += 40.f * S;
	for (const FString& L : Lines)
	{
		DrawText(L, kWhiteChoice, X + Pad, Y, Font, TextScale);
		Y += LineH;
	}
	if (OptionCount > 0)
	{
		Y += 16.f * S;
		for (int32 I = 0; I < OptionCount; ++I)
		{
			DrawText(Options[I], Mode.IsOptionGreyed(I) ? kGreyedChoice : kWhiteChoice, X + Pad + 20.f * S, Y, Font, TextScale);
			Y += OptionH;
		}
	}
	const FString& Prompt = bPick ? kPromptPick : (Loop.state() == ftcore::DialogueLoopState::Tag ? kPromptTag : kPromptLine);
	float PW = 0.f;
	float PH = 0.f;
	GetTextSize(Prompt, PW, PH, GEngine->GetSmallFont(), 1.2f * S);
	DrawText(Prompt, kDimChoice, X + W - Pad - PW, Y0 + H - Pad - PH, GEngine->GetSmallFont(), 1.2f * S);
}
