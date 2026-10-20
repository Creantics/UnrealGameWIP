#include "FTTitleHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"

#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"

#include "FTGameMode.h"
#include "FTSettingsOverlay.h"
#include "FTSettingsSubsystem.h"

#include "ftcore/StorySave.h"
#include "ftcore/TitleMenu.h"

namespace
{
const FLinearColor kBackdropTitle(0.01f, 0.01f, 0.03f, 0.88f);
const FLinearColor kGoldTitle(1.0f, 0.82f, 0.25f);
const FLinearColor kWhiteTitle(1.f, 1.f, 1.f);
const FLinearColor kGreyTitle(0.35f, 0.35f, 0.38f);
const FLinearColor kDimTitle(0.5f, 0.5f, 0.55f);

// Static so drawing never builds strings.
const FString kNameTitle(TEXT("Forgotten Third"));
const FString kNewGameTitle(TEXT("New Game"));
const FString kContinueTitle(TEXT("Continue"));
const FString kNewGameSelTitle(TEXT("> New Game"));
const FString kContinueSelTitle(TEXT("> Continue"));
const FString kSettingsTitle(TEXT("Settings"));
const FString kSettingsSelTitle(TEXT("> Settings"));
const FString kHintTitle(TEXT("Up / Down and Enter, mouse, or 1 / 2 / 3    Pad: d-pad and A"));
const FString kNoSaveTitle(TEXT("No saved game"));
const FString kBadSaveTitle(TEXT("The saved game can't be loaded"));
} // namespace

void AFTTitleHUD::DrawCentered(const FString& S, const FLinearColor& Color, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	GetTextSize(S, W, H, Font, Scale);
	DrawText(S, Color, Canvas->ClipX * 0.5f - W * 0.5f, Y, Font, Scale);
}

void AFTTitleHUD::DrawHUD()
{
	Super::DrawHUD();
	const UWorld* World = GetWorld();
	AFTGameMode* Mode = World ? World->GetAuthGameMode<AFTGameMode>() : nullptr;
	if (!Canvas || !Mode || !Mode->IsTitleShown())
	{
		return;
	}
	if (!CachedSettings.IsValid())
	{
		const UGameInstance* GameInstance = World->GetGameInstance();
		CachedSettings = GameInstance ? GameInstance->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
	}
	UFTSettingsSubsystem* Settings = CachedSettings.Get();
	const bool bSettingsOpen = Settings && Settings->IsOpen();
	const ftcore::TitleMenu& Menu = Mode->GetTitleMenu();
	const float S = FMath::Max(Canvas->ClipY / 1080.f, 0.5f);

	DrawRect(kBackdropTitle, 0.f, 0.f, Canvas->ClipX, Canvas->ClipY);
	DrawCentered(kNameTitle, kGoldTitle, Canvas->ClipY * 0.22f, GEngine->GetLargeFont(), 2.5f * S);

	UFont* Font = GEngine->GetMediumFont();
	float Y = Canvas->ClipY * 0.5f;
	const bool bNewSel = Menu.selected() == ftcore::TitleItem::NewGame;
	DrawCentered(bNewSel ? kNewGameSelTitle : kNewGameTitle, bNewSel ? kGoldTitle : kWhiteTitle, Y, Font, 1.6f * S);

	Y += 70.f * S;
	const bool bContinueOn = Menu.enabled(ftcore::TitleItem::Continue);
	const bool bContSel = Menu.selected() == ftcore::TitleItem::Continue;
	DrawCentered(bContSel ? kContinueSelTitle : kContinueTitle, !bContinueOn ? kGreyTitle : bContSel ? kGoldTitle : kWhiteTitle, Y,
		Font, 1.6f * S);
	if (!bContinueOn)
	{
		const bool bNone = Mode->GetSaveStatus() == ftcore::SaveRefusal::Empty;
		DrawCentered(bNone ? kNoSaveTitle : kBadSaveTitle, kDimTitle, Y + 48.f * S, GEngine->GetSmallFont(), 1.f * S);
	}

	const float SettingsY = Y + (bContinueOn ? 70.f : 110.f) * S; // room for the reason under a greyed Continue
	const bool bSetSel = Menu.selected() == ftcore::TitleItem::Settings;
	DrawCentered(bSetSel ? kSettingsSelTitle : kSettingsTitle, bSetSel ? kGoldTitle : kWhiteTitle, SettingsY, Font, 1.6f * S);

	DrawCentered(kHintTitle, kDimTitle, Canvas->ClipY - 48.f * S, GEngine->GetSmallFont(), 1.f * S);

	// The mouse selects the item under it once it moves; the overlay has its own hover while it is up.
	const APlayerController* PC = GetOwningPlayerController();
	float MouseX = 0.f;
	float MouseY = 0.f;
	if (!bSettingsOpen && PC && PC->GetMousePosition(MouseX, MouseY) && FVector2D(MouseX, MouseY) != LastMouse)
	{
		LastMouse = FVector2D(MouseX, MouseY);
		const float Left = Canvas->ClipX * 0.3f;
		const float Right = Canvas->ClipX * 0.7f;
		const float Rows[3] = {Canvas->ClipY * 0.5f, Y, SettingsY};
		const ftcore::TitleItem Items[3] = {ftcore::TitleItem::NewGame, ftcore::TitleItem::Continue, ftcore::TitleItem::Settings};
		for (int32 I = 0; I < 3; ++I)
		{
			if (MouseX >= Left && MouseX <= Right && MouseY >= Rows[I] - 8.f * S && MouseY <= Rows[I] + 56.f * S)
			{
				Mode->HoverTitleItem(Items[I]);
			}
		}
	}
	if (Settings)
	{
		FT::DrawSettingsOverlay(*this, *Settings);
	}
}
