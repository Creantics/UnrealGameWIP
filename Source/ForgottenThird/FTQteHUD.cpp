#include "FTQteHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "FTQteGameMode.h"
#include "FTUiText.h"

namespace
{
const FLinearColor kGoldQte(1.0f, 0.82f, 0.25f);
const FLinearColor kWhiteQte(1.f, 1.f, 1.f);
const FLinearColor kDimQte(0.55f, 0.55f, 0.6f);
const FLinearColor kPanelQte(0.02f, 0.02f, 0.05f, 0.75f);
const FLinearColor kBackQte(0.12f, 0.12f, 0.16f, 0.9f);
const FLinearColor kRedQte(1.0f, 0.3f, 0.3f);
const FLinearColor kCyanQte(0.35f, 0.85f, 1.f);
// Co-op: the two leads' prompt colours.
const FLinearColor kSlotQte[2] = {FLinearColor(1.0f, 0.82f, 0.25f), FLinearColor(0.35f, 0.85f, 1.f)};

// Direction order Left, Down, Up, Right (ftcore::Arrow), in screen space.
const FVector2D kDirsQte[4] = {FVector2D(-1, 0), FVector2D(0, 1), FVector2D(0, -1), FVector2D(1, 0)};

// Static so drawing never builds strings.
const FTUiText kNoStepQte(TEXT("common.no_step"));
const FTUiText kHintQte(TEXT("common.lanes_hint"));
const FTUiText kHoldQte(TEXT("qte.hold"));
const FTUiText kMashQte(TEXT("qte.mash"));
const FString kKeyNamesQte[4] = {FString(TEXT("Left / A")), FString(TEXT("Down / S")), FString(TEXT("Up / W")), FString(TEXT("Right / D"))};
const FString kSlotTagQte[2] = {FString(TEXT("1")), FString(TEXT("2"))};
const FTUiText kPerfectQte(TEXT("common.perfect"));
const FTUiText kGoodQte(TEXT("common.good"));
const FTUiText kMissQte(TEXT("common.miss"));
// The result line by ftcore::QteOutcome (Perfect, Good, Fair, Poor).
const FString kResultQte[4] = {FString(TEXT("Perfect!")), FString(TEXT("Good")), FString(TEXT("Fair")), FString(TEXT("Poor"))};

constexpr float kPopupLifeQte = 0.8f; // game seconds
constexpr int32 kVisiblePromptsQte = 3;

FLinearColor WithAlphaQte(const FLinearColor& C, float A)
{
	return FLinearColor(C.R, C.G, C.B, A);
}
} // namespace

void AFTQteHUD::DrawCentered(const FString& Text, const FLinearColor& Color, float Y, UFont* Font, float Scale)
{
	DrawStrCentered(Text, Color, Canvas->ClipX * 0.5f, Y, Font, Scale);
}

void AFTQteHUD::DrawStrCentered(const FString& Text, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	GetTextSize(Text, W, H, Font, Scale * UiScale);
	DrawText(Text, Color, X - W * 0.5f, Y, Font, Scale * UiScale);
}

void AFTQteHUD::DrawRing(float X, float Y, float Radius, const FLinearColor& Color, float Thickness)
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

void AFTQteHUD::DrawGlyph(int32 Direction, float X, float Y, float Radius, const FLinearColor& Color, float Thickness)
{
	const FVector2D Dir = kDirsQte[Direction & 3];
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

void AFTQteHUD::DrawPrompt(const AFTQteGameMode& Mode, int32 Index, int32 Slot, float X, float Y)
{
	const ftcore::QteScreen& Screen = Mode.GetScreen();
	const ftcore::QteSequence& Seq = Screen.sequence();
	const ftcore::QtePrompt& P = Seq.prompt(static_cast<size_t>(Index));
	const float S = UiScale;
	const bool bCurrent = Slot == 0;
	const float Size = bCurrent ? 1.f : 0.6f;
	const float R = 62.f * S * Size;
	const FLinearColor Base = Mode.IsCoop() ? kSlotQte[P.actor & 1] : kGoldQte;
	const FLinearColor Shown = bCurrent ? Base : WithAlphaQte(Base, 0.55f);

	// The target ring (a second, inner one marks a narrow window), and the ring that closes in on it.
	DrawRing(X, Y, R, Shown, 4.f * S);
	if (P.kind != ftcore::QteKind::Mash && P.window == ftcore::QteWindow::Narrow)
	{
		DrawRing(X, Y, R * 0.62f, WithAlphaQte(kWhiteQte, bCurrent ? 1.f : 0.55f), 3.f * S);
	}
	const float Tell = static_cast<float>(Seq.tellProgress(static_cast<size_t>(Index), Screen.time()));
	if (Tell >= 0.f)
	{
		DrawRing(X, Y, R + 110.f * S * Size * Tell, WithAlphaQte(kWhiteQte, 1.f - 0.6f * Tell), 4.f * S);
	}
	DrawGlyph(static_cast<int32>(P.input), X, Y, R * 0.55f, Shown, 5.f * S * Size);

	float LabelY = Y + R + 14.f * S;
	DrawStrCentered(kKeyNamesQte[static_cast<int32>(P.input) & 3], bCurrent ? kWhiteQte : kDimQte, X, LabelY, GEngine->GetSmallFont(), 1.2f);
	if (Mode.IsCoop())
	{
		DrawStrCentered(kSlotTagQte[P.actor & 1], Base, X, Y - R - 34.f * S, GEngine->GetMediumFont(), 1.3f);
	}
	LabelY += 26.f * S;
	if (P.kind == ftcore::QteKind::Hold)
	{
		DrawStrCentered(kHoldQte, Shown, X, LabelY, GEngine->GetSmallFont(), 1.3f);
		const float BarW = 200.f * S * Size;
		const float BarH = 12.f * S;
		const float BarX = X - BarW * 0.5f;
		const float BarY = LabelY + 24.f * S;
		DrawRect(kBackQte, BarX, BarY, BarW, BarH);
		if (bCurrent && Seq.holding())
		{
			DrawRect(Base, BarX, BarY, BarW * static_cast<float>(Seq.holdProgress()), BarH);
		}
	}
	else if (P.kind == ftcore::QteKind::Mash)
	{
		DrawStrCentered(kMashQte, Shown, X, LabelY, GEngine->GetSmallFont(), 1.3f);
		const int32 Count = P.mashCount;
		const float Box = 14.f * S * Size;
		const float Gap = 4.f * S * Size;
		float BoxX = X - (static_cast<float>(Count) * (Box + Gap) - Gap) * 0.5f;
		const float BoxY = LabelY + 24.f * S;
		const int32 Done = bCurrent ? Seq.mashPresses() : 0;
		for (int32 I = 0; I < Count; ++I)
		{
			DrawRect(I < Done ? Base : kBackQte, BoxX, BoxY, Box, Box);
			BoxX += Box + Gap;
		}
	}
}

void AFTQteHUD::DrawPips(const AFTQteGameMode& Mode, float Y)
{
	const ftcore::QteSequence& Seq = Mode.GetScreen().sequence();
	const float S = UiScale;
	const float Pip = 14.f * S;
	const float Gap = 6.f * S;
	const int32 Count = static_cast<int32>(Seq.size());
	float X = Canvas->ClipX * 0.5f - (static_cast<float>(Count) * (Pip + Gap) - Gap) * 0.5f;
	for (int32 I = 0; I < Count; ++I)
	{
		FLinearColor Color = kDimQte;
		switch (Seq.grade(static_cast<size_t>(I)))
		{
		case ftcore::QteGrade::Perfect: Color = kGoldQte; break;
		case ftcore::QteGrade::Good: Color = kCyanQte; break;
		case ftcore::QteGrade::Miss: Color = kRedQte; break;
		default: break;
		}
		if (static_cast<size_t>(I) == Seq.current())
		{
			DrawRect(kWhiteQte, X - 2.f * S, Y - 2.f * S, Pip + 4.f * S, Pip + 4.f * S);
		}
		DrawRect(Color, X, Y, Pip, Pip);
		X += Pip + Gap;
	}
}

void AFTQteHUD::DrawPopups(const AFTQteGameMode& Mode, float X, float Y)
{
	const double Now = Mode.GetScreen().elapsed();
	for (int32 I = 0; I < Mode.GetPopupCount(); ++I)
	{
		const AFTQteGameMode::FPopup& Popup = Mode.GetPopup(I);
		const float Age = static_cast<float>(Now - Popup.Time);
		if (Age < 0.f || Age >= kPopupLifeQte)
		{
			continue;
		}
		const float K = Age / kPopupLifeQte;
		const FString* Text = &kMissQte.Get();
		FLinearColor Color = kRedQte;
		if (Popup.Grade == ftcore::QteGrade::Perfect)
		{
			Text = &kPerfectQte.Get();
			Color = kGoldQte;
		}
		else if (Popup.Grade == ftcore::QteGrade::Good)
		{
			Text = &kGoodQte.Get();
			Color = kCyanQte;
		}
		DrawStrCentered(*Text, WithAlphaQte(Color, 1.f - K), X, Y - 60.f * UiScale * K, GEngine->GetLargeFont(), 1.4f);
	}
}

void AFTQteHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const AFTQteGameMode* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AFTQteGameMode>() : nullptr;
	if (!Mode)
	{
		return;
	}
	UiScale = FMath::Max(0.5f, Canvas->ClipY / 1080.f);
	const float S = UiScale;
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	const float W = 1000.f * S;
	const float H = 460.f * S;
	DrawRect(kPanelQte, CX - W * 0.5f, CY - H * 0.5f, W, H);

	if (!Mode->HasStep())
	{
		DrawCentered(kNoStepQte, kWhiteQte, CY - 20.f * S, GEngine->GetLargeFont(), 1.4f);
		return;
	}
	const ftcore::QteScreen& Screen = Mode->GetScreen();
	const ftcore::QteSequence& Seq = Screen.sequence();
	DrawPips(*Mode, CY - H * 0.5f + 28.f * S);

	if (Screen.showingResult() || Screen.finished())
	{
		DrawCentered(kResultQte[static_cast<int32>(Seq.outcome()) & 3], kGoldQte, CY - 40.f * S, GEngine->GetLargeFont(), 2.2f);
	}
	else
	{
		// The current prompt in the middle (or left of centre when more follow), the next two to its right.
		const int32 First = static_cast<int32>(Seq.current());
		const int32 Total = static_cast<int32>(Seq.size());
		for (int32 Slot = 0; Slot < kVisiblePromptsQte && First + Slot < Total; ++Slot)
		{
			const float X = CX - 150.f * S + static_cast<float>(Slot) * 250.f * S + (Slot > 0 ? 30.f * S : 0.f);
			DrawPrompt(*Mode, First + Slot, Slot, X, CY - 30.f * S);
		}
		DrawPopups(*Mode, CX - 150.f * S, CY - 150.f * S);
	}
	DrawCentered(kHintQte, kDimQte, CY + H * 0.5f - 40.f * S, GEngine->GetSmallFont(), 1.3f);
}
