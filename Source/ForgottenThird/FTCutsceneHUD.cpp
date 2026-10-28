#include "FTCutsceneHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "FTCutsceneDirector.h"
#include "FTCutsceneGameMode.h"
#include "FTPauseOverlay.h"
#include "FTUiText.h"

namespace
{
const FLinearColor kBarCutscene(0.f, 0.f, 0.f, 1.f);
const FLinearColor kGoldCutscene(1.0f, 0.82f, 0.25f);
const FLinearColor kDimCutscene(0.6f, 0.6f, 0.65f);

// Static so drawing never builds strings.
const FTUiText kSkipCutscene(TEXT("cutscene.skip"));
constexpr float kLetterboxCutscene = 2.39f;
constexpr float kMinBarCutscene = 0.08f;
} // namespace

void FT::DrawCutsceneHUD(AHUD& Hud, UCanvas* Canvas, const AFTCutsceneDirector& Director)
{
	if (!Canvas || !Director.IsStarted())
	{
		return;
	}
	const ftcore::CutscenePlayer& Player = Director.GetPlayer();
	const bool bInPlace = Director.IsInPlace();
	// In place the bars come in and go with the camera's blend from and back to the gameplay view.
	const float Presence = bInPlace ? FMath::Clamp(Player.presence(), 0.f, 1.f) : 1.f;
	if (Presence <= 0.f)
	{
		return;
	}
	const float W = Canvas->ClipX;
	const float H = Canvas->ClipY;
	const float S = FMath::Max(0.5f, H / 1080.f);
	const float Bar = FMath::Max((H - W / kLetterboxCutscene) * 0.5f, H * kMinBarCutscene) * Presence;
	Hud.DrawRect(kBarCutscene, 0.f, 0.f, W, Bar);
	Hud.DrawRect(kBarCutscene, 0.f, H - Bar, W, Bar);

	UFont* Large = GEngine->GetLargeFont();
	UFont* Small = GEngine->GetSmallFont();
	const float Margin = 40.f * S;
	float HintW = 0.f;
	float HintH = 0.f;
	Hud.GetTextSize(kSkipCutscene, HintW, HintH, Small, 1.2f * S);
	const float BottomY = H - Bar * 0.5f - HintH * 0.5f;
	if (!bInPlace)
	{
		// Top bar: what is playing.
		float TitleW = 0.f;
		float TitleH = 0.f;
		Hud.GetTextSize(Director.GetTitleText(), TitleW, TitleH, Large, 1.3f * S);
		const float TitleY = FMath::Max(0.f, Bar * 0.5f - TitleH * 0.5f);
		Hud.DrawText(Director.GetTitleText(), kGoldCutscene, Margin, TitleY, Large, 1.3f * S);
		Hud.DrawText(Director.GetStepText(), kDimCutscene, Margin + TitleW + 24.f * S, TitleY + TitleH * 0.25f, Small, 1.2f * S);
		// Bottom bar: the shot.
		const FString& Shot = Director.GetShotText();
		if (!Shot.IsEmpty())
		{
			Hud.DrawText(Shot, kDimCutscene, Margin, BottomY, Small, 1.2f * S);
		}
	}
	if (Player.playing() || Player.blendingIn())
	{
		FLinearColor Hint = kDimCutscene;
		Hint.A *= Presence;
		Hud.DrawText(kSkipCutscene, Hint, W - Margin - HintW, BottomY, Small, 1.2f * S);
	}
}

void AFTCutsceneHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const AFTCutsceneGameMode* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AFTCutsceneGameMode>() : nullptr;
	if (const AFTCutsceneDirector* Director = Mode ? Mode->GetDirector() : nullptr)
	{
		FT::DrawCutsceneHUD(*this, Canvas, *Director);
	}
	FT::DrawPauseOverlay(*this, Canvas);
}
