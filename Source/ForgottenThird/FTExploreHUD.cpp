#include "FTExploreHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "EngineUtils.h"

#include "FTCutsceneDirector.h"
#include "FTCutsceneHUD.h"
#include "FTExploreDirector.h"
#include "FTPauseOverlay.h"
#include "FTUiText.h"

#include "ftcore/Explore.h"

namespace
{
const FLinearColor kGoldField(1.0f, 0.82f, 0.25f);
const FLinearColor kWhiteField(1.f, 1.f, 1.f);
const FLinearColor kDimField(0.6f, 0.6f, 0.65f);
const FLinearColor kDoneField(0.45f, 0.85f, 0.5f);
const FLinearColor kPanelField(0.02f, 0.02f, 0.05f, 0.6f);

// Static so drawing never builds strings.
const FTUiText kTitleField(TEXT("explore.objectives"));
const FString kTickField(TEXT("v"));
const FString kArrowField(TEXT(">"));
const FTUiText kHintField(TEXT("explore.hint"));
const FTUiText kCompleteField(TEXT("explore.complete"));
const FTUiText kWorldField(TEXT("explore.world"));
const FTUiText kOverworldField(TEXT("explore.overworld"));

float FieldFade(double Age, double Hold, double Out)
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

FLinearColor FieldWithAlpha(FLinearColor C, float A)
{
	C.A *= A;
	return C;
}
} // namespace

AFTExploreDirector* AFTExploreHUD::FindDirector()
{
	if (!CachedDirector.IsValid())
	{
		for (TActorIterator<AFTExploreDirector> It(GetWorld()); It; ++It)
		{
			CachedDirector = *It;
			break;
		}
	}
	return CachedDirector.Get();
}

void AFTExploreHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const AFTExploreDirector* Found = FindDirector();
	if (!Found)
	{
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	UiScale = FMath::Max(0.5f, Canvas->ClipY / 1080.f);
	// A cutscene playing in place has the screen: its letterbox, and none of the field's own lines.
	const AFTCutsceneDirector* Cutscene = Found->GetCutsceneDirector();
	if (Cutscene && Cutscene->IsPlayingInPlace())
	{
		FT::DrawCutsceneHUD(*this, Canvas, *Cutscene);
		FT::DrawPauseOverlay(*this, Canvas);
		return;
	}
	if (Found->IsStoryWalk())
	{
		DrawFieldObjective(*Found);
	}
	else if (Found->IsWorldMode() || Found->IsOverworld())
	{
		DrawWorld(*Found);
	}
	else
	{
		DrawObjectives(*Found);
	}
	DrawPrompt(*Found);
	DrawMessages(*Found);
	FT::DrawPauseOverlay(*this, Canvas);
}

void AFTExploreHUD::DrawStr(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale)
{
	DrawText(S, Color, X, Y, Font, Scale * UiScale);
}

void AFTExploreHUD::DrawStrCentered(const FString& S, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale)
{
	float W = 0.f;
	float H = 0.f;
	GetTextSize(S, W, H, Font, Scale * UiScale);
	DrawText(S, Color, X - W * 0.5f, Y, Font, Scale * UiScale);
}

void AFTExploreHUD::DrawObjectives(const AFTExploreDirector& D)
{
	const ftcore::Exploration& E = D.GetExploration();
	UFont* Font = GEngine->GetMediumFont();
	const float S = UiScale;
	const int32 Count = static_cast<int32>(E.objectiveCount());
	const int32 Current = E.currentObjective();
	const float X = 24.f * S;
	const float Y = 24.f * S;
	const float Line = 30.f * S;
	DrawRect(kPanelField, X - 10.f * S, Y - 8.f * S, 380.f * S, 40.f * S + Line * Count);
	DrawStr(kTitleField, kGoldField, X, Y, GEngine->GetSmallFont(), 1.1f);
	for (int32 I = 0; I < Count; ++I)
	{
		const float LY = Y + 26.f * S + Line * I;
		const bool bDone = E.objectiveDone(I);
		const bool bNow = I == Current;
		const FLinearColor Color = bDone ? kDoneField : (bNow ? kWhiteField : kDimField);
		if (bDone || bNow)
		{
			DrawStr(bDone ? kTickField : kArrowField, Color, X, LY, Font, 1.f);
		}
		DrawStr(D.GetObjectiveText(I), Color, X + 22.f * S, LY, Font, 1.f);
	}
}

void AFTExploreHUD::DrawFieldObjective(const AFTExploreDirector& D)
{
	UFont* Font = GEngine->GetMediumFont();
	const float S = UiScale;
	const float X = 24.f * S;
	const float Y = 24.f * S;
	DrawRect(kPanelField, X - 10.f * S, Y - 8.f * S, 420.f * S, 70.f * S);
	DrawStr(kTitleField, kGoldField, X, Y, GEngine->GetSmallFont(), 1.1f);
	DrawStr(kArrowField, kWhiteField, X, Y + 26.f * S, Font, 1.f);
	DrawStr(D.GetFieldObjective(), kWhiteField, X + 22.f * S, Y + 26.f * S, Font, 1.f);
}

void AFTExploreHUD::DrawWorld(const AFTExploreDirector& D)
{
	const float S = UiScale;
	const float X = 24.f * S;
	const float Y = 24.f * S;
	DrawRect(kPanelField, X - 10.f * S, Y - 8.f * S, 360.f * S, 70.f * S);
	DrawStr(D.IsOverworld() ? kOverworldField : kWorldField, kGoldField, X, Y, GEngine->GetSmallFont(), 1.1f);
	DrawStr(D.GetWorldLine(), kWhiteField, X, Y + 26.f * S, GEngine->GetMediumFont(), 1.f);
}

void AFTExploreHUD::DrawPrompt(const AFTExploreDirector& D)
{
	const FString& Prompt = D.GetPrompt();
	UFont* Font = GEngine->GetMediumFont();
	const float S = UiScale;
	const float CX = Canvas->ClipX * 0.5f;
	if (!Prompt.IsEmpty())
	{
		float W = 0.f;
		float H = 0.f;
		GetTextSize(Prompt, W, H, Font, 1.2f * S);
		const float Y = Canvas->ClipY * 0.68f;
		DrawRect(kPanelField, CX - W * 0.5f - 16.f * S, Y - 8.f * S, W + 32.f * S, H + 16.f * S);
		DrawStrCentered(Prompt, kWhiteField, CX, Y, Font, 1.2f);
	}
	DrawStrCentered(kHintField, kDimField, CX, Canvas->ClipY - 36.f * S, GEngine->GetSmallFont(), 1.f);
}

void AFTExploreHUD::DrawMessages(const AFTExploreDirector& D)
{
	UFont* Large = GEngine->GetLargeFont();
	UFont* Font = GEngine->GetMediumFont();
	const float S = UiScale;
	const float CX = Canvas->ClipX * 0.5f;

	if (!D.GetNotice().IsEmpty())
	{
		const float A = FieldFade(D.GetNoticeAge(), 1.8, 0.5);
		if (A > 0.f)
		{
			DrawStrCentered(D.GetNotice(), FieldWithAlpha(kGoldField, A), CX, Canvas->ClipY * 0.6f, Font, 1.3f);
		}
	}
	if (D.IsComplete())
	{
		const float Y = Canvas->ClipY * 0.36f;
		DrawRect(kPanelField, 0.f, Y - 20.f * S, Canvas->ClipX, 120.f * S);
		DrawStrCentered(kCompleteField, kGoldField, CX, Y, Large, 2.5f);
		return;
	}
	if (!D.GetBanner().IsEmpty())
	{
		const float A = FieldFade(D.GetBannerAge(), 1.4, 0.5);
		if (A > 0.f)
		{
			const float Y = Canvas->ClipY * 0.36f;
			DrawRect(FieldWithAlpha(kPanelField, A), 0.f, Y - 20.f * S, Canvas->ClipX, 110.f * S);
			DrawStrCentered(D.GetBanner(), FieldWithAlpha(kWhiteField, A), CX, Y, Large, 2.2f);
		}
	}
}
