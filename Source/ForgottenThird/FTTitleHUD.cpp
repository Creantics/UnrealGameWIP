#include "FTTitleHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"

#include "FTGameMode.h"

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
const FString kHintTitle(TEXT("Up / Down and Enter, or 1 / 2    Pad: d-pad and A"));
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
	const AFTGameMode* Mode = World ? World->GetAuthGameMode<AFTGameMode>() : nullptr;
	if (!Canvas || !Mode || !Mode->IsTitleShown())
	{
		return;
	}
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

	DrawCentered(kHintTitle, kDimTitle, Canvas->ClipY - 48.f * S, GEngine->GetSmallFont(), 1.f * S);
}
