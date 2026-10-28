#include "FTStubHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "FTPauseOverlay.h"
#include "FTStubGameMode.h"
#include "FTUiText.h"

namespace
{
const FLinearColor kGoldStub(1.0f, 0.82f, 0.25f);
const FLinearColor kWhiteStub(1.f, 1.f, 1.f);
const FLinearColor kDimStub(0.6f, 0.6f, 0.65f);
const FLinearColor kPanelStub(0.02f, 0.02f, 0.05f, 0.75f);

// Static so drawing never builds strings.
const FTUiText kNoStepStub(TEXT("common.no_step"));
const FTUiText kContinueStub(TEXT("common.continue_pad"));
} // namespace

void AFTStubHUD::DrawCentered(const FString& Text, const FLinearColor& Color, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	GetTextSize(Text, W, H, Font, Scale * UiScale);
	DrawText(Text, Color, Canvas->ClipX * 0.5f - W * 0.5f, Y, Font, Scale * UiScale);
}

void AFTStubHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const AFTStubGameMode* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AFTStubGameMode>() : nullptr;
	if (!Mode)
	{
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	UiScale = FMath::Max(0.5f, Canvas->ClipY / 1080.f);
	const float S = UiScale;
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	const float W = 760.f * S;
	const float H = 300.f * S;
	DrawRect(kPanelStub, CX - W * 0.5f, CY - H * 0.5f, W, H);

	if (!Mode->HasStep())
	{
		DrawCentered(kNoStepStub, kWhiteStub, CY - 20.f * S, GEngine->GetLargeFont(), 1.4f);
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	float Y = CY - H * 0.5f + 40.f * S;
	DrawCentered(Mode->GetTitleText(), kGoldStub, Y, GEngine->GetLargeFont(), 2.2f);
	Y += 110.f * S;
	if (!Mode->GetLabelText().IsEmpty())
	{
		DrawCentered(Mode->GetLabelText(), kWhiteStub, Y, GEngine->GetMediumFont(), 1.5f);
	}
	Y += 50.f * S;
	DrawCentered(Mode->GetStepText(), kDimStub, Y, GEngine->GetMediumFont(), 1.2f);
	DrawCentered(kContinueStub, kDimStub, CY + H * 0.5f - 50.f * S, GEngine->GetSmallFont(), 1.3f);
	FT::DrawPauseOverlay(*this, Canvas);
}
