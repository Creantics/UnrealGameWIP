#include "FTStoryHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "EngineUtils.h"

#include "FTStoryDirector.h"

#include "ftcore/StoryRunner.h"

using ftcore::StoryRunner;

namespace
{
const FLinearColor kGoldBox(1.0f, 0.82f, 0.25f);
const FLinearColor kWhiteBox(1.f, 1.f, 1.f);
const FLinearColor kDirectionBox(0.72f, 0.8f, 0.92f);
const FLinearColor kDimBox(0.5f, 0.5f, 0.55f);
const FLinearColor kPanelBox(0.02f, 0.02f, 0.05f, 0.72f);

// Static so drawing never builds strings.
const FString kMoreBox(TEXT(">"));
const FString kContinueBox(TEXT("Space / Enter: continue"));
const FString kHintBox(TEXT("Space / Enter / pad A: continue    1-4 / d-pad: choose"));
const FString kEndBox(TEXT("The End"));

constexpr float kTextScaleBox = 1.15f;
constexpr double kTitleHoldBox = 2.5;
constexpr double kTitleFadeBox = 0.8;

bool BoxIsLowSurrogate(TCHAR C)
{
	return C >= 0xDC00 && C <= 0xDFFF;
}

// Codepoints in S[From, To).
int32 BoxCountChars(const FString& S, int32 From, int32 To)
{
	int32 N = 0;
	for (int32 I = From; I < To; ++I)
	{
		N += BoxIsLowSurrogate(S[I]) ? 0 : 1;
	}
	return N;
}

// Code units holding the first Chars codepoints of S.
int32 BoxUnitsFor(const FString& S, int32 Chars)
{
	int32 I = 0;
	int32 Seen = 0;
	while (I < S.Len())
	{
		if (!BoxIsLowSurrogate(S[I]))
		{
			if (Seen == Chars)
			{
				break;
			}
			++Seen;
		}
		++I;
	}
	return I;
}

float BoxFade(double Age, double Hold, double Out)
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

FLinearColor BoxWithAlpha(FLinearColor C, float A)
{
	C.A *= A;
	return C;
}
} // namespace

AFTStoryDirector* AFTStoryHUD::FindDirector()
{
	if (!CachedDirector.IsValid())
	{
		for (TActorIterator<AFTStoryDirector> It(GetWorld()); It; ++It)
		{
			CachedDirector = *It;
			break;
		}
	}
	return CachedDirector.Get();
}

void AFTStoryHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const AFTStoryDirector* Found = FindDirector();
	if (!Found)
	{
		return;
	}
	UiScale = FMath::Max(0.5f, Canvas->ClipY / 1080.f);
	DrawBox(*Found);
	DrawTitle(*Found);
	DrawBanner(*Found);
}

void AFTStoryHUD::DrawStr(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale)
{
	DrawText(S, Color, X, Y, Font, Scale * UiScale);
}

void AFTStoryHUD::DrawStrCentered(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	GetTextSize(S, W, H, Font, Scale * UiScale);
	DrawText(S, Color, X - W * 0.5f, Y, Font, Scale * UiScale);
}

void AFTStoryHUD::Wrap(const FString& Src, UFont* Font, float Scale, float MaxWidth)
{
	Lines.Reset();
	LineStart.Reset();
	LineChars.Reset();
	const int32 N = Src.Len();
	int32 I = 0;
	while (I < N)
	{
		const int32 From = I;
		int32 End = N;
		int32 Next = N;
		int32 LastBreak = -1;
		bool bNewline = false;
		for (int32 J = From; J <= N; ++J)
		{
			const bool bAtEnd = J == N;
			const TCHAR C = bAtEnd ? TCHAR(' ') : Src[J];
			if (C == TCHAR('\n'))
			{
				// A newline ends the line, unless the words before it already overflow.
				bNewline = true;
			}
			if (C != TCHAR(' ') && !bNewline)
			{
				continue;
			}
			float W = 0.f;
			float H = 0.f;
			GetTextSize(Src.Mid(From, J - From), W, H, Font, Scale);
			if (W > MaxWidth && LastBreak > From)
			{
				End = LastBreak;
				Next = LastBreak + 1;
				break;
			}
			if (bNewline || bAtEnd)
			{
				End = J;
				Next = bAtEnd ? N : J + 1;
				break;
			}
			LastBreak = J;
		}
		Lines.Add(Src.Mid(From, End - From));
		LineStart.Add(BoxCountChars(Src, 0, From));
		LineChars.Add(BoxCountChars(Src, From, End));
		I = Next;
	}
}

void AFTStoryHUD::DrawBox(const AFTStoryDirector& D)
{
	const ftcore::DialoguePresenter& P = D.GetPresenter();
	const StoryRunner::State State = P.state();
	UFont* Font = GEngine->GetMediumFont();
	UFont* Small = GEngine->GetSmallFont();
	const float S = UiScale;
	const float CX = Canvas->ClipX * 0.5f;

	if (State == StoryRunner::State::Finished)
	{
		const float Y = Canvas->ClipY * 0.4f;
		DrawRect(kPanelBox, 0.f, Y - 20.f * S, Canvas->ClipX, 120.f * S);
		DrawStrCentered(kEndBox, kGoldBox, CX, Y, GEngine->GetLargeFont(), 2.5f);
		return;
	}
	DrawStrCentered(kHintBox, kDimBox, CX, Canvas->ClipY - 32.f * S, Small, 1.f);
	if (State == StoryRunner::State::Idle || State == StoryRunner::State::Hold || D.GetText().IsEmpty())
	{
		return; // a hold shows nothing new
	}

	const float BoxW = FMath::Min(Canvas->ClipX - 80.f * S, 1400.f * S);
	const float Pad = 24.f * S;
	const float TextW = BoxW - 2.f * Pad;
	const float Scale = kTextScaleBox * S;
	if (WrappedSerial != D.GetBeatSerial() || WrappedWidth != TextW)
	{
		Wrap(D.GetText(), Font, Scale, TextW);
		WrappedSerial = D.GetBeatSerial();
		WrappedWidth = TextW;
	}

	float CharW = 0.f;
	float LineH = 0.f;
	GetTextSize(kMoreBox, CharW, LineH, Font, Scale);
	LineH *= 1.25f;
	const bool bOptions = P.optionsVisible();
	const int32 OptionCount = bOptions ? D.GetOptions().Num() : 0;
	const bool bSpeaker = !D.GetSpeaker().IsEmpty();
	const float BoxH = Pad * 2.f + LineH * (Lines.Num() + OptionCount) + (OptionCount > 0 ? LineH * 0.4f : 0.f);
	const float BoxX = CX - BoxW * 0.5f;
	const float BoxY = Canvas->ClipY - 64.f * S - BoxH;
	DrawRect(kPanelBox, BoxX, BoxY, BoxW, BoxH);

	if (bSpeaker)
	{
		float W = 0.f;
		float H = 0.f;
		GetTextSize(D.GetSpeaker(), W, H, Font, Scale);
		DrawRect(kPanelBox, BoxX, BoxY - H - 14.f * S, W + 2.f * Pad, H + 14.f * S);
		DrawText(D.GetSpeaker(), kGoldBox, BoxX + Pad, BoxY - H - 7.f * S, Font, Scale);
	}

	const FLinearColor TextColor = State == StoryRunner::State::Direction || State == StoryRunner::State::Gameplay ? kDirectionBox : kWhiteBox;
	const int32 Revealed = static_cast<int32>(P.revealedChars());
	float Y = BoxY + Pad;
	for (int32 I = 0; I < Lines.Num(); ++I, Y += LineH)
	{
		if (Revealed >= LineStart[I] + LineChars[I])
		{
			DrawText(Lines[I], TextColor, BoxX + Pad, Y, Font, Scale);
			continue;
		}
		if (Revealed > LineStart[I])
		{
			Partial.Reset();
			Partial.AppendChars(*Lines[I], BoxUnitsFor(Lines[I], Revealed - LineStart[I]));
			DrawText(Partial, TextColor, BoxX + Pad, Y, Font, Scale);
		}
		break;
	}

	if (bOptions)
	{
		Y = BoxY + Pad + LineH * (Lines.Num() + 0.4f);
		for (int32 I = 0; I < OptionCount; ++I, Y += LineH)
		{
			DrawText(D.GetOptions()[I], D.IsOptionSelectable(I) ? kGoldBox : kDimBox, BoxX + Pad + 16.f * S, Y, Font, Scale);
		}
	}
	else if (P.revealed())
	{
		// Pulsing continue marker; a gameplay beat waiting here says what to press.
		const float A = 0.55f + 0.45f * FMath::Sin(static_cast<float>(P.beatTime()) * 5.f);
		if (State == StoryRunner::State::Gameplay)
		{
			float W = 0.f;
			float H = 0.f;
			GetTextSize(kContinueBox, W, H, Small, S);
			DrawText(kContinueBox, BoxWithAlpha(kGoldBox, A), BoxX + BoxW - Pad - W, BoxY + BoxH - Pad * 0.5f - H, Small, S);
		}
		else
		{
			DrawText(kMoreBox, BoxWithAlpha(kGoldBox, A), BoxX + BoxW - Pad - CharW, BoxY + BoxH - Pad - LineH * 0.8f, Font, Scale);
		}
	}
}

void AFTStoryHUD::DrawBanner(const AFTStoryDirector& D)
{
	if (D.GetBanner().IsEmpty())
	{
		return;
	}
	const float A = static_cast<float>(FMath::Clamp(D.GetBannerAge() / 0.3, 0.0, 1.0));
	const float S = UiScale;
	const float Y = Canvas->ClipY * 0.42f;
	DrawRect(BoxWithAlpha(kPanelBox, A), 0.f, Y - 20.f * S, Canvas->ClipX, 110.f * S);
	DrawStrCentered(D.GetBanner(), BoxWithAlpha(kGoldBox, A), Canvas->ClipX * 0.5f, Y, GEngine->GetLargeFont(), 2.2f);
}

void AFTStoryHUD::DrawTitle(const AFTStoryDirector& D)
{
	if (D.GetSceneTitle().IsEmpty() || D.IsFinished())
	{
		return;
	}
	const float A = BoxFade(D.GetSceneAge(), kTitleHoldBox, kTitleFadeBox);
	if (A <= 0.f)
	{
		return;
	}
	const float S = UiScale;
	const float Y = Canvas->ClipY * 0.16f;
	DrawRect(BoxWithAlpha(kPanelBox, A), 0.f, Y - 16.f * S, Canvas->ClipX, 90.f * S);
	DrawStrCentered(D.GetSceneTitle(), BoxWithAlpha(kWhiteBox, A), Canvas->ClipX * 0.5f, Y, GEngine->GetLargeFont(), 1.8f);
}
