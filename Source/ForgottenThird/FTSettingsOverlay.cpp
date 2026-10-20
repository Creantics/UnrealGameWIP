#include "FTSettingsOverlay.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"

#include "FTSettingsSubsystem.h"

namespace
{
const FLinearColor kBackdropSet(0.01f, 0.01f, 0.03f, 0.9f);
const FLinearColor kPanelSet(0.04f, 0.04f, 0.09f, 0.95f);
const FLinearColor kRowSet(1.f, 0.82f, 0.25f, 0.14f);
const FLinearColor kGoldSet(1.0f, 0.82f, 0.25f);
const FLinearColor kWhiteSet(1.f, 1.f, 1.f);
const FLinearColor kDimSet(0.5f, 0.5f, 0.55f);

// Static so drawing never builds strings.
const FString kTitleSet(TEXT("Settings"));
const FString kHintSet(TEXT("Up / Down: row    Left / Right or click: change    Enter: cycle    Esc: back (saved)"));
const FString kLeftSet(TEXT("<"));
const FString kRightSet(TEXT(">"));
} // namespace

void FT::DrawSettingsOverlay(AHUD& Hud, UFTSettingsSubsystem& Settings)
{
	UCanvas* Canvas = Hud.Canvas;
	if (!Canvas || !Settings.IsOpen())
	{
		return;
	}
	const float S = FMath::Max(Canvas->ClipY / 1080.f, 0.5f);
	UFont* Font = GEngine->GetMediumFont();
	UFont* Small = GEngine->GetSmallFont();
	const float Scale = 1.15f * S;

	Hud.DrawRect(kBackdropSet, 0.f, 0.f, Canvas->ClipX, Canvas->ClipY);

	const int32 N = UFTSettingsSubsystem::RowCount;
	const float RowH = 50.f * S;
	const float HeaderH = 96.f * S;
	const float FooterH = 64.f * S;
	const float PanelW = FMath::Min(Canvas->ClipX - 80.f * S, 1000.f * S);
	const float PanelH = HeaderH + RowH * N + FooterH;
	const float PanelX = Canvas->ClipX * 0.5f - PanelW * 0.5f;
	const float PanelY = FMath::Max(16.f * S, Canvas->ClipY * 0.5f - PanelH * 0.5f);
	Hud.DrawRect(kPanelSet, PanelX, PanelY, PanelW, PanelH);

	float W = 0.f;
	float H = 0.f;
	Hud.GetTextSize(kTitleSet, W, H, GEngine->GetLargeFont(), 1.5f * S);
	Hud.DrawText(kTitleSet, kGoldSet, Canvas->ClipX * 0.5f - W * 0.5f, PanelY + 28.f * S, GEngine->GetLargeFont(), 1.5f * S);

	const float RowX0 = PanelX + 24.f * S;
	const float RowX1 = PanelX + PanelW - 24.f * S;
	const float ValueLeft = RowX1 - 340.f * S;
	const float ValueRight = RowX1 - 24.f * S;
	const float ValueMid = ValueLeft + (ValueRight - ValueLeft) * 0.5f;
	Settings.SetValueColumn(ValueLeft, ValueMid);

	const int32 Selected = Settings.GetSelectedRow();
	for (int32 I = 0; I < N; ++I)
	{
		const float Y0 = PanelY + HeaderH + RowH * I;
		const float Y1 = Y0 + RowH - 4.f * S;
		Settings.SetRowRect(I, FBox2D(FVector2D(RowX0, Y0), FVector2D(RowX1, Y1)));
		const bool bSel = I == Selected;
		if (bSel)
		{
			Hud.DrawRect(kRowSet, RowX0, Y0, RowX1 - RowX0, Y1 - Y0);
		}
		const FString& Label = Settings.GetRowLabel(I);
		Hud.GetTextSize(Label, W, H, Font, Scale);
		const float TextY = Y0 + (Y1 - Y0 - H) * 0.5f;
		const FLinearColor& Color = bSel ? kGoldSet : kWhiteSet;
		if (!UFTSettingsSubsystem::IsRowAdjustable(I))
		{
			Hud.DrawText(Label, Color, Canvas->ClipX * 0.5f - W * 0.5f, TextY, Font, Scale);
			continue;
		}
		Hud.DrawText(Label, Color, RowX0 + 16.f * S, TextY, Font, Scale);
		const FString& Value = Settings.GetRowValue(I);
		Hud.GetTextSize(Value, W, H, Font, Scale);
		Hud.DrawText(Value, bSel ? kGoldSet : kDimSet, ValueMid - W * 0.5f, TextY, Font, Scale);
		if (bSel)
		{
			Hud.DrawText(kLeftSet, kGoldSet, ValueLeft, TextY, Font, Scale);
			Hud.GetTextSize(kRightSet, W, H, Font, Scale);
			Hud.DrawText(kRightSet, kGoldSet, ValueRight - W, TextY, Font, Scale);
		}
	}

	Hud.GetTextSize(kHintSet, W, H, Small, S);
	Hud.DrawText(kHintSet, kDimSet, Canvas->ClipX * 0.5f - W * 0.5f, PanelY + PanelH - FooterH * 0.65f, Small, S);

	// Hover follows the mouse when it moves.
	const APlayerController* PC = Hud.GetOwningPlayerController();
	float MouseX = 0.f;
	float MouseY = 0.f;
	if (PC && PC->GetMousePosition(MouseX, MouseY))
	{
		Settings.UpdateHover(FVector2D(MouseX, MouseY));
	}
}
