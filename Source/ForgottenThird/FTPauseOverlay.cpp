#include "FTPauseOverlay.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"

#include "FTPauseSubsystem.h"
#include "FTSettingsOverlay.h"
#include "FTSettingsSubsystem.h"
#include "FTUiText.h"

#include "ftcore/PauseMenu.h"

namespace
{
const FLinearColor kBackdropPause(0.01f, 0.01f, 0.03f, 0.78f);
const FLinearColor kPanelPause(0.04f, 0.04f, 0.09f, 0.95f);
const FLinearColor kRowPause(1.f, 0.82f, 0.25f, 0.14f);
const FLinearColor kGoldPause(1.0f, 0.82f, 0.25f);
const FLinearColor kWhitePause(1.f, 1.f, 1.f);
const FLinearColor kGreyPause(0.35f, 0.35f, 0.38f);
const FLinearColor kDimPause(0.5f, 0.5f, 0.55f);

// Static so drawing never builds strings.
const FTUiText kTitlePause(TEXT("pause.title"));
const FTUiText kResumePause(TEXT("pause.resume"));
const FTUiText kSettingsPause(TEXT("pause.settings"));
const FTUiText kQuitTitlePause(TEXT("pause.quit_title"));
const FTUiText kQuitGamePause(TEXT("pause.quit_game"));
const FTUiText kAskTitlePause(TEXT("pause.ask_title"));
const FTUiText kAskGamePause(TEXT("pause.ask_game"));
const FTUiText kAskNotePause(TEXT("pause.ask_note"));
const FTUiText kYesPause(TEXT("pause.yes"));
const FTUiText kNoPause(TEXT("pause.no"));
const FTUiText kHintPause(TEXT("pause.hint"));
const FTUiText kAskHintPause(TEXT("pause.ask_hint"));

// The labels in ftcore::PauseItem order.
const FTUiText* const kItemLabelsPause[UFTPauseSubsystem::RowCount] = {&kResumePause, &kSettingsPause, &kQuitTitlePause, &kQuitGamePause};

void DrawCentered(AHUD& Hud, UCanvas& Canvas, const FString& Text, const FLinearColor& Color, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	Hud.GetTextSize(Text, W, H, Font, Scale);
	Hud.DrawText(Text, Color, Canvas.ClipX * 0.5f - W * 0.5f, Y, Font, Scale);
}

void DrawMenu(AHUD& Hud, UCanvas& Canvas, UFTPauseSubsystem& Pause, bool bFollowMouse)
{
	const float S = FMath::Max(Canvas.ClipY / 1080.f, 0.5f);
	UFont* Font = GEngine->GetMediumFont();
	UFont* Small = GEngine->GetSmallFont();
	const float Scale = 1.3f * S;
	const ftcore::PauseMenu& Menu = Pause.GetMenu();

	Hud.DrawRect(kBackdropPause, 0.f, 0.f, Canvas.ClipX, Canvas.ClipY);

	// The panel keeps its size when the prompt takes the place of the items, so it doesn't jump.
	const float RowH = 64.f * S;
	const float HeaderH = 108.f * S;
	const float FooterH = 72.f * S;
	const float BodyH = RowH * UFTPauseSubsystem::RowCount;
	const float PanelW = FMath::Min(Canvas.ClipX - 80.f * S, 640.f * S);
	const float PanelH = HeaderH + BodyH + FooterH;
	const float PanelX = Canvas.ClipX * 0.5f - PanelW * 0.5f;
	const float PanelY = FMath::Max(16.f * S, Canvas.ClipY * 0.5f - PanelH * 0.5f);
	Hud.DrawRect(kPanelPause, PanelX, PanelY, PanelW, PanelH);
	DrawCentered(Hud, Canvas, kTitlePause.Get(), kGoldPause, PanelY + 28.f * S, GEngine->GetLargeFont(), 1.5f * S);

	const float BodyY = PanelY + HeaderH;
	const float RowX0 = PanelX + 24.f * S;
	const float RowX1 = PanelX + PanelW - 24.f * S;
	float W = 0.f;
	float H = 0.f;

	if (!Menu.confirming())
	{
		for (int32 I = 0; I < UFTPauseSubsystem::RowCount; ++I)
		{
			const ftcore::PauseItem Item = static_cast<ftcore::PauseItem>(I);
			const float Y0 = BodyY + RowH * I;
			const float Y1 = Y0 + RowH - 6.f * S;
			Pause.SetRowRect(I, FBox2D(FVector2D(RowX0, Y0), FVector2D(RowX1, Y1)));
			const bool bSel = Menu.selected() == Item;
			const bool bOn = Menu.enabled(Item);
			if (bSel && bOn)
			{
				Hud.DrawRect(kRowPause, RowX0, Y0, RowX1 - RowX0, Y1 - Y0);
			}
			const FString& Label = kItemLabelsPause[I]->Get();
			Hud.GetTextSize(Label, W, H, Font, Scale);
			const FLinearColor& Color = !bOn ? kGreyPause : bSel ? kGoldPause : kWhitePause;
			Hud.DrawText(Label, Color, Canvas.ClipX * 0.5f - W * 0.5f, Y0 + (Y1 - Y0 - H) * 0.5f, Font, Scale);
		}
		DrawCentered(Hud, Canvas, kHintPause.Get(), kDimPause, PanelY + PanelH - FooterH * 0.65f, Small, S);
	}
	else
	{
		// The Yes / No prompt of the selected quit item: the question, a note, then the two buttons.
		const bool bGame = Menu.selected() == ftcore::PauseItem::QuitGame;
		DrawCentered(Hud, Canvas, bGame ? kAskGamePause.Get() : kAskTitlePause.Get(), kWhitePause, BodyY + 10.f * S, Font, Scale);
		DrawCentered(Hud, Canvas, kAskNotePause.Get(), kDimPause, BodyY + 10.f * S + RowH * 0.8f, Small, S);

		const float ButtonW = 190.f * S;
		const float Gap = 40.f * S;
		const float ButtonY0 = BodyY + RowH * 2.f;
		const float ButtonY1 = ButtonY0 + RowH - 6.f * S;
		const float Left = Canvas.ClipX * 0.5f - ButtonW - Gap * 0.5f;
		const FTUiText* const Labels[2] = {&kYesPause, &kNoPause};
		for (int32 I = 0; I < 2; ++I)
		{
			const float X0 = Left + (ButtonW + Gap) * I;
			Pause.SetChoiceRect(I, FBox2D(FVector2D(X0, ButtonY0), FVector2D(X0 + ButtonW, ButtonY1)));
			const bool bSel = Menu.confirmYes() == (I == 0);
			if (bSel)
			{
				Hud.DrawRect(kRowPause, X0, ButtonY0, ButtonW, ButtonY1 - ButtonY0);
			}
			const FString& Label = Labels[I]->Get();
			Hud.GetTextSize(Label, W, H, Font, Scale);
			Hud.DrawText(Label, bSel ? kGoldPause : kWhitePause, X0 + (ButtonW - W) * 0.5f, ButtonY0 + (ButtonY1 - ButtonY0 - H) * 0.5f, Font, Scale);
		}
		DrawCentered(Hud, Canvas, kAskHintPause.Get(), kDimPause, PanelY + PanelH - FooterH * 0.65f, Small, S);
	}

	// Hover follows the mouse when it moves (the settings overlay has its own while it is up).
	if (bFollowMouse)
	{
		const APlayerController* PC = Hud.GetOwningPlayerController();
		float MouseX = 0.f;
		float MouseY = 0.f;
		if (PC && PC->GetMousePosition(MouseX, MouseY))
		{
			Pause.UpdateHover(FVector2D(MouseX, MouseY));
		}
	}
}
} // namespace

void FT::DrawPauseOverlay(AHUD& Hud, UCanvas* Canvas)
{
	if (!Canvas)
	{
		return;
	}
	const UWorld* World = Hud.GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (!GameInstance)
	{
		return;
	}
	UFTPauseSubsystem* Pause = GameInstance->GetSubsystem<UFTPauseSubsystem>();
	UFTSettingsSubsystem* Settings = GameInstance->GetSubsystem<UFTSettingsSubsystem>();
	if (Pause && Pause->IsOpen())
	{
		DrawMenu(Hud, *Canvas, *Pause, !(Settings && Settings->IsOpen()));
	}
	if (Settings)
	{
		// On top of the menu: draws nothing unless it is open.
		FT::DrawSettingsOverlay(Hud, Canvas, *Settings);
	}
}
