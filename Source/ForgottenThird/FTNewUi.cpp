#include "FTNewUi.h"

#include "FTArenaRunner.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"

namespace
{
TAutoConsoleVariable<int32> CVarNewUi(TEXT("ft.NewUi"), 0,
	TEXT("1: the new game UI (Slate, drawn from Core view-models) in place of the Canvas HUD's parts it replaces. ")
	TEXT("-FTNewUi on the command line sets it."));

// The arena HUD's Slate layer. Spike stage: the lead's hp as a hairline bar on a backing, bottom left, at 1080p
// sizes scaled by the viewport height, so a windowless capture shows whether Slate lands in the frame.
class SFTArenaHud : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFTArenaHud) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&, TFunction<const UFTArenaRunner*()> InGetRunner)
	{
		GetRunner = MoveTemp(InGetRunner);
		SetVisibility(EVisibility::HitTestInvisible); // never takes a click from the world
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1920.0, 1080.0); }

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&,
		FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle&, bool) const override
	{
		const FVector2f Size = Geometry.GetLocalSize();
		const float S = FMath::Max(0.5f, Size.Y / 1080.f);
		const FSlateBrush* White = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
		const UFTArenaRunner* Runner = GetRunner ? GetRunner() : nullptr;
		float Hp = 1.f;
		if (Runner && Runner->IsBegun())
		{
			const ftcore::ArenaFight& Fight = Runner->GetFight();
			const int MaxHp = Fight.def().fighter.hp;
			Hp = MaxHp > 0 ? FMath::Clamp(static_cast<float>(Fight.hp()) / static_cast<float>(MaxHp), 0.f, 1.f) : 0.f;
		}
		const FVector2f Origin(48.f * S, Size.Y - 220.f * S);
		const FVector2f PanelSize(420.f * S, 72.f * S);
		// The backing (default on in the plan's accessibility rules): a dark plate behind the block.
		FSlateDrawElement::MakeBox(Out, Layer, Geometry.ToPaintGeometry(PanelSize, FSlateLayoutTransform(Origin)), White,
			ESlateDrawEffect::None, FLinearColor(0.f, 0.f, 0.f, 0.55f));
		// The hp track and fill: 4 px lines at 1080p.
		const FVector2f BarOrigin = Origin + FVector2f(16.f * S, 46.f * S);
		const float BarW = 388.f * S;
		const float BarH = 4.f * S;
		FSlateDrawElement::MakeBox(Out, Layer + 1, Geometry.ToPaintGeometry(FVector2f(BarW, BarH), FSlateLayoutTransform(BarOrigin)),
			White, ESlateDrawEffect::None, FLinearColor(1.f, 1.f, 1.f, 0.3f));
		FSlateDrawElement::MakeBox(Out, Layer + 2, Geometry.ToPaintGeometry(FVector2f(BarW * Hp, BarH), FSlateLayoutTransform(BarOrigin)),
			White, ESlateDrawEffect::None, FLinearColor(0.91f, 0.70f, 0.42f, 1.f));
		// The engine's Roboto (the interim font): the spike's tag, so the capture shows text from Slate too.
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), FMath::RoundToInt(20.f * S));
		FSlateDrawElement::MakeText(Out, Layer + 2,
			Geometry.ToPaintGeometry(FVector2f(BarW, 28.f * S), FSlateLayoutTransform(Origin + FVector2f(16.f * S, 10.f * S))),
			SpikeTag, Font, ESlateDrawEffect::None, FLinearColor(0.95f, 0.93f, 0.9f, 1.f));
		return Layer + 2;
	}

private:
	TFunction<const UFTArenaRunner*()> GetRunner;
	// Not shipped text: the spike's tag (replaced by the view-model's strings in the slice).
	const FString SpikeTag = TEXT("SLATE");
};
}

namespace FT
{
bool NewUiEnabled()
{
	static const bool bFlag = FParse::Param(FCommandLine::Get(), TEXT("FTNewUi"));
	return bFlag || CVarNewUi.GetValueOnGameThread() != 0;
}

TSharedRef<SWidget> MakeArenaHudWidget(TFunction<const UFTArenaRunner*()> GetRunner)
{
	return SNew(SFTArenaHud, MoveTemp(GetRunner));
}
}
