#include "FTCutsceneHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "FTCutsceneDirector.h"
#include "FTCutsceneGameMode.h"
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

void AFTCutsceneHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const AFTCutsceneGameMode* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AFTCutsceneGameMode>() : nullptr;
	if (!Mode)
	{
		return;
	}
	const float W = Canvas->ClipX;
	const float H = Canvas->ClipY;
	const float S = FMath::Max(0.5f, H / 1080.f);
	const float Bar = FMath::Max((H - W / kLetterboxCutscene) * 0.5f, H * kMinBarCutscene);
	DrawRect(kBarCutscene, 0.f, 0.f, W, Bar);
	DrawRect(kBarCutscene, 0.f, H - Bar, W, Bar);

	UFont* Large = GEngine->GetLargeFont();
	UFont* Small = GEngine->GetSmallFont();
	const float Margin = 40.f * S;

	// Top bar: what is playing.
	float TitleW = 0.f;
	float TitleH = 0.f;
	GetTextSize(Mode->GetTitleText(), TitleW, TitleH, Large, 1.3f * S);
	const float TitleY = FMath::Max(0.f, Bar * 0.5f - TitleH * 0.5f);
	DrawText(Mode->GetTitleText(), kGoldCutscene, Margin, TitleY, Large, 1.3f * S);
	DrawText(Mode->GetStepText(), kDimCutscene, Margin + TitleW + 24.f * S, TitleY + TitleH * 0.25f, Small, 1.2f * S);

	// Bottom bar: the shot and the skip hint.
	const FString& Shot = Mode->GetShotText();
	float HintW = 0.f;
	float HintH = 0.f;
	GetTextSize(kSkipCutscene, HintW, HintH, Small, 1.2f * S);
	const float BottomY = H - Bar * 0.5f - HintH * 0.5f;
	if (!Shot.IsEmpty())
	{
		DrawText(Shot, kDimCutscene, Margin, BottomY, Small, 1.2f * S);
	}
	const AFTCutsceneDirector* Director = Mode->GetDirector();
	if (Director && Director->GetPlayer().playing())
	{
		DrawText(kSkipCutscene, kDimCutscene, W - Margin - HintW, BottomY, Small, 1.2f * S);
	}
}
