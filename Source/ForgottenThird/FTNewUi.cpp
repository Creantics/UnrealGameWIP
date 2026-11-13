#include "FTNewUi.h"

#include "FTArenaRunner.h"
#include "FTStoryDirector.h"
#include "FTText.h"
#include "FTUiText.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/UiStyle.h"
#include "ftcore/UiViews.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"

#include <string>

namespace
{
TAutoConsoleVariable<int32> CVarNewUi(TEXT("ft.NewUi"), 0,
	TEXT("1: the new game UI (Slate, drawn from Core view-models) in place of the Canvas HUD's parts it replaces. ")
	TEXT("-FTNewUi on the command line sets it."));

using ftcore::UiTextRole;
using ftcore::UiToken;

const FTUiText kHpUi(TEXT("arena.hp"));
const FTUiText kLoadUi(TEXT("arena.load"));
const FTUiText kHealsUi(TEXT("arena.heals"));
const FTUiText kReadUi(TEXT("arena.read_stacks"));
const FTUiText kTargetUi(TEXT("arena.target"));
const FTUiText kReleaseReadyUi(TEXT("arena.release_ready"));
const FTUiText kOverloadedUi(TEXT("arena.overloaded"));
const FTUiText kGuardBrokenUi(TEXT("arena.guard_broken"));
const FTUiText kDownUi(TEXT("battle.down"));
const FTUiText kWeightUi(TEXT("arena.boss.weight"));
const FTUiText kWeakPointUi(TEXT("arena.boss.weak_point"));
const FTUiText kInvulnerableUi(TEXT("arena.boss.invulnerable"));

// ---- the style table ------------------------------------------------------------------------------------------

FLinearColor LinOf(const ftcore::UiColor& C, float Alpha = 1.f)
{
	const auto To8 = [](float V) { return static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(V, 0.f, 1.f) * 255.f)); };
	FLinearColor L(FColor(To8(C.r), To8(C.g), To8(C.b), 255)); // sRGB to linear
	L.A = FMath::Clamp(C.a * Alpha, 0.f, 1.f);
	return L;
}

// Data/UI/style.json once (the built-in style when it is missing or refused), and its colours for the world shown.
struct FUiTable
{
	ftcore::UiStyle Style;
	FLinearColor Token[ftcore::kUiTokenCount];
	FLinearColor Edge;
};

const FUiTable& Table()
{
	static const FUiTable Loaded = []()
	{
		FUiTable T;
		T.Style = ftcore::defaultUiStyle();
		FString Text;
		const FString File = FPaths::ProjectDir() / TEXT("Data/UI/style.json");
		if (FFileHelper::LoadFileToString(Text, *File))
		{
			std::string Error;
			if (!ftcore::importUiStyle(FT::ToUtf8(Text), T.Style, &Error))
			{
				UE_LOG(LogTemp, Warning, TEXT("FTNewUi: %s: %s (the built-in style is used)"), *File, *FT::ToFString(Error));
			}
		}
		// The world's own colours arrive with the world flag on the screens; Solhaven until then.
		for (size_t I = 0; I < ftcore::kUiTokenCount; ++I)
		{
			T.Token[I] = LinOf(ftcore::uiColor(T.Style, ftcore::UiWorld::Solhaven, ftcore::UiPalette::Default, static_cast<UiToken>(I)));
		}
		T.Edge = LinOf(T.Style.edge);
		return T;
	}();
	return Loaded;
}

FLinearColor Tok(UiToken T, float Alpha = 1.f)
{
	FLinearColor C = Table().Token[static_cast<size_t>(T)];
	C.A *= Alpha;
	return C;
}

// The interim font (the engine's Roboto); Hand is a marked placeholder italic. Every glyph carries the hairline edge.
FSlateFontInfo FontFor(UiTextRole Role, float Height, float TextScale = 1.f)
{
	const ftcore::UiStyle& S = Table().Style;
	const ftcore::UiTextStyle& T = S.text[static_cast<size_t>(Role)];
	const float Px = ftcore::uiTextPx(S, Role, Height, TextScale);
	const TCHAR* Face = T.font == ftcore::UiFont::SansBold ? TEXT("Bold") : (T.font == ftcore::UiFont::Hand ? TEXT("Italic") : TEXT("Regular"));
	FSlateFontInfo F = FCoreStyle::GetDefaultFontStyle(Face, Px * 0.75f); // Slate sizes are points at 96 dpi
	F.LetterSpacing = FMath::RoundToInt(T.tracking * 1000.f);
	F.OutlineSettings.OutlineSize = FMath::Max(1, FMath::RoundToInt(ftcore::uiEdgePx(S, Height)));
	F.OutlineSettings.OutlineColor = Table().Edge;
	return F;
}

TSharedRef<FSlateFontMeasure> FontMeasure()
{
	return FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
}

// ---- painting helpers (boxes only: no per-frame arrays) ------------------------------------------------------------

struct FPainter
{
	const FGeometry& Geometry;
	FSlateWindowElementList& Out;
	int32 Layer;
	float S; // 1080p units to local

	const FSlateBrush* White() const { return FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")); }

	void Box(float X, float Y, float W, float H, const FLinearColor& C, int32 L = 1) const
	{
		if (W <= 0.f || H <= 0.f || C.A <= 0.f)
		{
			return;
		}
		FSlateDrawElement::MakeBox(Out, Layer + L, Geometry.ToPaintGeometry(FVector2f(W, H), FSlateLayoutTransform(FVector2f(X, Y))), White(),
			ESlateDrawEffect::None, C);
	}

	void Hollow(float X, float Y, float W, float H, float T, const FLinearColor& C) const
	{
		Box(X, Y, W, T, C);
		Box(X, Y + H - T, W, T, C);
		Box(X, Y + T, T, H - 2.f * T, C);
		Box(X + W - T, Y + T, T, H - 2.f * T, C);
	}

	// Shade with no edge: stacked boxes growing outward, so the middle reaches Peak and the rim fades to nothing.
	void Soft(float CX, float CY, float W, float H, float Peak, float Alpha = 1.f) const
	{
		// Many thin, round-ended (anti-aliased) bands so no step or corner reads: the backing must never show as an object.
		static const FSlateRoundedBoxBrush Round(FLinearColor::White, 256.f);
		constexpr int32 Bands = 48;
		const float P = FMath::Clamp(Peak * Alpha, 0.f, 0.99f);
		const float Each = 1.f - FMath::Pow(1.f - P, 1.f / static_cast<float>(Bands));
		if (Each <= 0.f || W <= 0.f || H <= 0.f)
		{
			return;
		}
		const FLinearColor C = Tok(UiToken::Backing, 0.f) + FLinearColor(0.f, 0.f, 0.f, Each);
		for (int32 I = 0; I < Bands; ++I)
		{
			const float T = 0.3f + 0.7f * static_cast<float>(I + 1) / static_cast<float>(Bands);
			const float BW = W * T;
			const float BH = H * T;
			FSlateDrawElement::MakeBox(Out, Layer, Geometry.ToPaintGeometry(FVector2f(BW, BH), FSlateLayoutTransform(FVector2f(CX - BW * 0.5f, CY - BH * 0.5f))),
				&Round, ESlateDrawEffect::None, C);
		}
	}

	void Diamond(float CX, float CY, float Size, const FLinearColor& C) const
	{
		const float Side = Size * 0.7071f;
		FSlateDrawElement::MakeRotatedBox(Out, Layer + 2,
			Geometry.ToPaintGeometry(FVector2f(Side, Side), FSlateLayoutTransform(FVector2f(CX - Side * 0.5f, CY - Side * 0.5f))), White(),
			ESlateDrawEffect::None, UE_PI * 0.25f, TOptional<FVector2f>(), FSlateDrawElement::RelativeToElement, C);
	}

	// Pointing up, from five bars.
	void Triangle(float CX, float CY, float Size, const FLinearColor& C) const
	{
		constexpr int32 Rows = 5;
		const float RowH = Size / Rows;
		for (int32 I = 0; I < Rows; ++I)
		{
			const float W = Size * static_cast<float>(I + 1) / Rows;
			Box(CX - W * 0.5f, CY - Size * 0.5f + RowH * I, W, RowH + 0.5f, C, 2);
		}
	}

	void Circle(float CX, float CY, float D, const FLinearColor& C) const
	{
		static const FSlateRoundedBoxBrush Round(FLinearColor::White, 256.f);
		FSlateDrawElement::MakeBox(Out, Layer + 2, Geometry.ToPaintGeometry(FVector2f(D, D), FSlateLayoutTransform(FVector2f(CX - D * 0.5f, CY - D * 0.5f))),
			&Round, ESlateDrawEffect::None, C);
	}

	// A chevron pointing right: two rotated bars.
	void Chevron(float CX, float CY, float Size, const FLinearColor& C) const
	{
		const float Len = Size * 0.7f;
		const float T = FMath::Max(2.f, Size * 0.18f);
		for (int32 Sign = -1; Sign <= 1; Sign += 2)
		{
			const float Y = CY + Sign * Size * 0.22f - T * 0.5f;
			FSlateDrawElement::MakeRotatedBox(Out, Layer + 2,
				Geometry.ToPaintGeometry(FVector2f(Len, T), FSlateLayoutTransform(FVector2f(CX - Len * 0.5f, Y))), White(), ESlateDrawEffect::None,
				-Sign * UE_PI * 0.25f, TOptional<FVector2f>(), FSlateDrawElement::RelativeToElement, C);
		}
	}

	void Text(float X, float Y, const FString& Str, const FSlateFontInfo& Font, const FLinearColor& C, int32 Start = 0, int32 End = -1) const
	{
		if (Str.IsEmpty() || C.A <= 0.f)
		{
			return;
		}
		const FPaintGeometry Geo = Geometry.ToPaintGeometry(FVector2f(4096.f, 512.f), FSlateLayoutTransform(FVector2f(X, Y)));
		if (End < 0)
		{
			FSlateDrawElement::MakeText(Out, Layer + 3, Geo, Str, Font, ESlateDrawEffect::None, C);
		}
		else if (End > Start)
		{
			FSlateDrawElement::MakeText(Out, Layer + 3, Geo, Str, Start, End, Font, ESlateDrawEffect::None, C);
		}
	}

	// A meter in the style's painter. Ring and Stroke are drawn as Line until their painters land with the pick.
	void Meter(float X, float Y, float W, float Thick, float Fraction, const FLinearColor& Fill, float Alpha) const
	{
		const float F = FMath::Clamp(Fraction, 0.f, 1.f);
		const FLinearColor Track = Tok(UiToken::Track, Alpha);
		if (Table().Style.meter == ftcore::UiMeterShape::Teeth)
		{
			const float Pitch = 9.f * S;
			const int32 Teeth = FMath::Max(4, FMath::FloorToInt(W / Pitch));
			const float ToothW = FMath::Max(1.f, Pitch * 0.4f);
			const int32 Lit = FMath::RoundToInt(F * Teeth);
			for (int32 I = 0; I < Teeth; ++I)
			{
				Box(X + I * (W / Teeth), Y - Thick * 2.f, ToothW, Thick * 4.f, I < Lit ? Fill : Track);
			}
			return;
		}
		Box(X, Y, W, Thick, Track);
		Box(X, Y, W * F, Thick, Fill, 2);
	}
};

float TextWidth(const FString& Str, const FSlateFontInfo& Font)
{
	return Str.IsEmpty() ? 0.f : static_cast<float>(FontMeasure()->Measure(Str, Font).X);
}

// ---- the arena HUD ----------------------------------------------------------------------------------------------------

constexpr uint32 SectionBit(ftcore::ArenaHudSection Sec) { return 1u << static_cast<uint32>(Sec); }

// The boss section from whichever boss host runs this fight.
void FillBoss(const UFTArenaRunner& Runner, ftcore::ArenaHudBoss& Out)
{
	Out = ftcore::ArenaHudBoss{};
	const ftcore::ArenaFight& Fight = Runner.GetFight();
	size_t Slot = 0;
	bool bOpen = false;
	bool bInvulnerable = false;
	int32 Weight = 0;
	int32 WeightMax = 0;
	if (const FFTKiteHost* Kite = Runner.GetKite())
	{
		Slot = Kite->GetSlot();
		bOpen = Kite->IsOpen();
	}
	else if (const FFTStoneBearerHost* Bearer = Runner.GetStoneBearer())
	{
		Slot = Bearer->GetSlot();
		const ftcore::StoneBearer& Boss = Bearer->GetBoss();
		bOpen = Boss.run().crit().isOpen(Boss.run().now());
		bInvulnerable = Boss.invulnerable();
		WeightMax = FMath::Clamp(Bearer->GetDef().weight.cap, 0, 8);
		Weight = FMath::Clamp(static_cast<int32>(Boss.stacks()), 0, WeightMax);
	}
	else
	{
		return;
	}
	if (Slot >= Fight.enemyCount() || !Fight.enemyAlive(Slot))
	{
		return;
	}
	Out.present = true;
	Out.nameKey = static_cast<std::uint16_t>(Slot);
	Out.hp = Fight.enemyHp(Slot);
	Out.hpMax = Fight.def().enemies[Slot].hp;
	Out.weight = static_cast<std::uint8_t>(Weight);
	Out.weightMax = static_cast<std::uint8_t>(WeightMax);
	Out.weakPoint = bOpen ? ftcore::UiWeakPoint::Open : ftcore::UiWeakPoint::Closed;
	Out.invulnerable = bInvulnerable;
}

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

	virtual void Tick(const FGeometry& Geometry, const double, const float DeltaTime) override
	{
		Time += DeltaTime;
		const UFTArenaRunner* Runner = GetRunner ? GetRunner() : nullptr;
		bLive = Runner && Runner->IsBegun();
		const float H = Geometry.GetLocalSize().Y;
		bool bRebuild = false;
		if (H != BuiltHeight)
		{
			BuiltHeight = H;
			LabelFont = FontFor(UiTextRole::Label, H);
			ValueFont = FontFor(UiTextRole::Value, H);
			LabelH = FontMeasure()->GetMaxCharacterHeight(LabelFont);
			ValueH = FontMeasure()->GetMaxCharacterHeight(ValueFont);
			bRebuild = true;
		}
		if (!bLive)
		{
			return;
		}
		const ftcore::ArenaFight& Fight = Runner->GetFight();
		ftcore::ArenaHudFrame Frame = View.now;
		ftcore::fillArenaHudLead(Fight, Frame.lead);
		ftcore::fillArenaHudTarget(Fight, Frame.target);
		FillBoss(*Runner, Frame.boss);
		Frame.partner = ftcore::ArenaHudPartner{}; // the arena runs one fighter; the session's partner arrives with the party
		Frame.third = ftcore::ArenaHudThird{};
		const uint32 Changed = ftcore::updateArenaHudView(View, Frame);
		// Strings only on a change.
		if (bRebuild || (Changed & SectionBit(ftcore::ArenaHudSection::Lead)))
		{
			HpText = FString::Printf(TEXT("%d"), View.now.lead.hp);
			HpTextW = TextWidth(HpText, ValueFont);
		}
		if (bRebuild || (Changed & (SectionBit(ftcore::ArenaHudSection::Target) | SectionBit(ftcore::ArenaHudSection::Boss))))
		{
			const bool bBoss = View.now.boss.present;
			TargetText = FString::Printf(TEXT("%d"), bBoss ? View.now.boss.hp : View.now.target.hp);
			TargetTextW = TextWidth(TargetText, ValueFont);
			if (bBoss && View.now.target.present && View.now.target.enemy != View.now.boss.nameKey)
			{
				OtherText = FString::Printf(TEXT("%d"), View.now.target.hp);
				OtherTextW = TextWidth(OtherText, ValueFont);
			}
			WordW = TextWidth(kWeakPointUi, LabelFont);
		}
		ftcore::uiFadeStep(LeadFade, (Changed & SectionBit(ftcore::ArenaHudSection::Lead)) != 0, DeltaTime, Table().Style.fade);
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer,
		const FWidgetStyle&, bool) const override
	{
		if (!bLive)
		{
			return Layer;
		}
		const FVector2f Size = Geometry.GetLocalSize();
		const FPainter P{Geometry, Out, Layer, FMath::Max(0.25f, Size.Y / 1080.f)};
		PaintLead(P, Size);
		PaintTop(P, Size);
		return Layer + 3;
	}

private:
	void PaintLead(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::ArenaHudLead& L = View.now.lead;
		const ftcore::UiStyle& St = Table().Style;
		const float S = P.S;
		const float A = LeadFade.alpha;
		const float Lw = ftcore::uiLinePx(St, Size.Y);
		const float X = 64.f * S;
		const float W = 420.f * S;
		const float Top = Size.Y - 250.f * S;
		P.Soft(X + W * 0.5f, Top + 92.f * S, W + 320.f * S, 300.f * S, St.backingSoftHud, FMath::Max(A, 0.6f));

		// HP: label, number (a triangle beside it when low), the line.
		const float HpFrac = L.hpMax > 0 ? static_cast<float>(L.hp) / static_cast<float>(L.hpMax) : 0.f;
		const bool bLow = HpFrac <= 0.3f;
		P.Text(X, Top + (ValueH - LabelH), kHpUi, LabelFont, Tok(UiToken::InkDim, A));
		P.Text(X + W - HpTextW, Top, HpText, ValueFont, Tok(UiToken::Ink, A));
		if (bLow)
		{
			P.Triangle(X + W - HpTextW - 18.f * S, Top + ValueH * 0.55f, 14.f * S, Tok(UiToken::Danger, A));
		}
		float Y = Top + ValueH + 8.f * S;
		P.Meter(X, Y, W, Lw * 1.5f, HpFrac, bLow ? Tok(UiToken::Danger, A) : Tok(UiToken::Accent, A), A);
		// Stamina: a thinner line under it.
		Y += 12.f * S;
		P.Meter(X, Y, W, FMath::Max(1.f, Lw * 0.67f), L.stamina01, Tok(UiToken::Ink, 0.85f * A), A);

		// Load: ticks with the release mark taller; the state as a word with its own shape.
		Y += 26.f * S;
		if (L.loadMax > 0)
		{
			P.Text(X, Y - LabelH * 0.5f, kLoadUi, LabelFont, Tok(UiToken::InkDim, A));
			const float TX = X + 90.f * S;
			const float TW = W - 90.f * S;
			constexpr int32 Ticks = 20;
			const int32 Lit = FMath::RoundToInt(static_cast<float>(L.load) / static_cast<float>(L.loadMax) * Ticks);
			const int32 Mark = FMath::RoundToInt(static_cast<float>(ftcore::kArenaReleaseOpenLoad) / static_cast<float>(L.loadMax) * Ticks);
			const FLinearColor On = L.overloaded ? Tok(UiToken::Danger, A) : Tok(UiToken::Accent, A);
			for (int32 I = 0; I < Ticks; ++I)
			{
				const float H = (I == Mark ? 18.f : 10.f) * S;
				P.Box(TX + I * (TW / Ticks), Y - H * 0.5f, FMath::Max(1.f, Lw), H, I < Lit ? On : Tok(UiToken::Track, A), 1);
			}
		}
		// One state word at a time, the most urgent first.
		Y += 22.f * S;
		const float Mid = Y + LabelH * 0.5f;
		if (L.down)
		{
			P.Triangle(X + 7.f * S, Mid, 14.f * S, Tok(UiToken::Danger, A));
			P.Text(X + 24.f * S, Y, kDownUi, LabelFont, Tok(UiToken::Danger, A));
		}
		else if (L.overloaded)
		{
			P.Triangle(X + 7.f * S, Mid, 14.f * S, Tok(UiToken::Danger, A));
			P.Text(X + 24.f * S, Y, kOverloadedUi, LabelFont, Tok(UiToken::Danger, A));
		}
		else if (L.guardBroken)
		{
			P.Triangle(X + 7.f * S, Mid, 14.f * S, Tok(UiToken::Danger, A));
			P.Text(X + 24.f * S, Y, kGuardBrokenUi, LabelFont, Tok(UiToken::Danger, A));
		}
		else if (L.releaseReady)
		{
			P.Chevron(X + 7.f * S, Mid, 16.f * S, Tok(UiToken::Accent, A));
			P.Text(X + 24.f * S, Y, kReleaseReadyUi, LabelFont, Tok(UiToken::Accent, A));
		}

		// Heals and Read: pips, filled while held.
		Y += LabelH + 10.f * S;
		const float Pip = 12.f * S;
		P.Text(X, Y, kHealsUi, LabelFont, Tok(UiToken::InkDim, A));
		float PX = X + 90.f * S;
		for (int32 I = 0; I < L.healsMax; ++I, PX += Pip + 6.f * S)
		{
			if (I < L.heals)
			{
				P.Box(PX, Y + (LabelH - Pip) * 0.5f, Pip, Pip, Tok(UiToken::Ink, A), 1);
			}
			else
			{
				P.Hollow(PX, Y + (LabelH - Pip) * 0.5f, Pip, Pip, FMath::Max(1.f, Lw * 0.5f), Tok(UiToken::InkDim, A));
			}
		}
		if (L.readMax > 0)
		{
			PX = X + 250.f * S;
			P.Text(PX, Y, kReadUi, LabelFont, Tok(UiToken::InkDim, A));
			PX += 70.f * S;
			for (int32 I = 0; I < L.readMax; ++I, PX += Pip * 0.7f + 5.f * S)
			{
				if (I < L.read)
				{
					P.Box(PX, Y + (LabelH - Pip) * 0.5f, Pip * 0.7f, Pip, Tok(UiToken::Accent, A), 1);
				}
				else
				{
					P.Hollow(PX, Y + (LabelH - Pip) * 0.5f, Pip * 0.7f, Pip, FMath::Max(1.f, Lw * 0.5f), Tok(UiToken::InkDim, A));
				}
			}
		}

		// The partner (when the party has one) above the lead; the third place as its reserved slot, unlabelled.
		const ftcore::ArenaHudPartner& Pa = View.now.partner;
		if (Pa.present)
		{
			const float PY = Top - 40.f * S;
			P.Circle(X + 7.f * S, PY, 12.f * S, Tok(UiToken::Ally));
			P.Meter(X + 24.f * S, PY - Lw * 0.5f, 240.f * S, Lw, Pa.hpMax > 0 ? static_cast<float>(Pa.hp) / Pa.hpMax : 0.f, Tok(UiToken::Ally), 1.f);
		}
		if (View.now.third.present)
		{
			const float D = 22.f * S;
			P.Circle(X + W + 30.f * S, Top + ValueH * 0.5f, D, Tok(UiToken::InkDim, 0.5f));
			P.Circle(X + W + 30.f * S, Top + ValueH * 0.5f, D - 4.f * S, Tok(UiToken::Backing, 0.9f));
		}
	}

	// One bar at the top centre: the boss when a boss fights, else the target; a second, smaller line for another target.
	void PaintTop(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		if (!B.present && !T.present)
		{
			return;
		}
		const ftcore::UiStyle& St = Table().Style;
		const float S = P.S;
		const float Lw = ftcore::uiLinePx(St, Size.Y);
		const float W = 640.f * S;
		const float X = Size.X * 0.5f - W * 0.5f;
		const float Y = 40.f * S;
		P.Soft(Size.X * 0.5f, Y + 40.f * S, W + 420.f * S, 190.f * S, St.backingSoftHud);
		const int32 Hp = B.present ? B.hp : T.hp;
		const int32 HpMax = B.present ? B.hpMax : T.hpMax;
		P.Text(X, Y + (ValueH - LabelH), kTargetUi, LabelFont, Tok(UiToken::InkDim));
		P.Text(X + W - TargetTextW, Y, TargetText, ValueFont, Tok(UiToken::Ink));
		float LY = Y + ValueH + 8.f * S;
		P.Meter(X, LY, W, Lw * 1.5f, HpMax > 0 ? static_cast<float>(Hp) / HpMax : 0.f, Tok(UiToken::Ink), 1.f);
		if (B.present)
		{
			for (int32 I = 1; I < B.phaseCount; ++I) // phase marks
			{
				P.Box(X + W * I / B.phaseCount, LY - 5.f * S, FMath::Max(1.f, Lw * 0.67f), 10.f * S + Lw * 1.5f, Tok(UiToken::Ink));
			}
		}
		// The target's poise (the boss's own when the target is the boss).
		if (T.present && (!B.present || T.enemy == B.nameKey))
		{
			P.Meter(X, LY + 10.f * S, W, FMath::Max(1.f, Lw * 0.67f), T.poise01, Tok(UiToken::InkDim), 1.f);
		}
		LY += 26.f * S;
		if (B.present && B.weightMax > 0)
		{
			P.Text(X, LY, kWeightUi, LabelFont, Tok(UiToken::InkDim));
			const float Pip = 12.f * S;
			for (int32 I = 0; I < B.weightMax; ++I)
			{
				const float PX = X + 100.f * S + I * (Pip + 6.f * S);
				if (I < B.weight)
				{
					P.Box(PX, LY + (LabelH - Pip) * 0.5f, Pip, Pip, Tok(UiToken::Accent), 1);
				}
				else
				{
					P.Hollow(PX, LY + (LabelH - Pip) * 0.5f, Pip, Pip, FMath::Max(1.f, Lw * 0.5f), Tok(UiToken::InkDim));
				}
			}
		}
		if (B.present && B.weakPoint == ftcore::UiWeakPoint::Open)
		{
			const float Pulse = 0.75f + 0.25f * FMath::Sin(Time * 8.f);
			const float CX = Size.X * 0.5f;
			P.Diamond(CX - WordW * 0.5f - 16.f * S, LY + LabelH * 0.5f, 16.f * S, Tok(UiToken::WeakPoint, Pulse));
			P.Text(CX - WordW * 0.5f, LY, kWeakPointUi, LabelFont, Tok(UiToken::WeakPoint, Pulse));
		}
		else if (B.present && B.invulnerable)
		{
			const float CX = Size.X * 0.5f;
			P.Hollow(CX - 80.f * S, LY + (LabelH - 12.f * S) * 0.5f, 12.f * S, 12.f * S, FMath::Max(1.f, Lw * 0.5f), Tok(UiToken::Ink));
			P.Text(CX - 60.f * S, LY, kInvulnerableUi, LabelFont, Tok(UiToken::Ink));
		}
		// Another enemy targeted while the boss fights: a short line under the boss bar.
		if (B.present && T.present && T.enemy != B.nameKey)
		{
			const float OY = LY + LabelH + 14.f * S;
			const float OW = W * 0.5f;
			const float OX = Size.X * 0.5f - OW * 0.5f;
			P.Text(OX, OY, kTargetUi, LabelFont, Tok(UiToken::InkDim));
			P.Text(OX + OW - OtherTextW, OY, OtherText, ValueFont, Tok(UiToken::Ink));
			P.Meter(OX, OY + ValueH + 4.f * S, OW, Lw, T.hpMax > 0 ? static_cast<float>(T.hp) / T.hpMax : 0.f, Tok(UiToken::Ink), 1.f);
			P.Meter(OX, OY + ValueH + 12.f * S, OW, FMath::Max(1.f, Lw * 0.67f), T.poise01, Tok(UiToken::InkDim), 1.f);
		}
	}

	TFunction<const UFTArenaRunner*()> GetRunner;
	ftcore::ArenaHudView View;
	ftcore::UiFade LeadFade;
	bool bLive = false;
	float Time = 0.f;
	float BuiltHeight = -1.f;
	FSlateFontInfo LabelFont;
	FSlateFontInfo ValueFont;
	float LabelH = 0.f;
	float ValueH = 0.f;
	FString HpText;
	float HpTextW = 0.f;
	FString TargetText;
	float TargetTextW = 0.f;
	FString OtherText;
	float OtherTextW = 0.f;
	float WordW = 0.f;
};

// ---- dialogue ---------------------------------------------------------------------------------------------------------

bool IsLowSurrogateUnit(TCHAR C) { return C >= 0xDC00 && C <= 0xDFFF; }

class SFTDialogue : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFTDialogue) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&, TFunction<const AFTStoryDirector*()> InGetDirector)
	{
		GetDirector = MoveTemp(InGetDirector);
		SetVisibility(EVisibility::HitTestInvisible);
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1920.0, 1080.0); }

	virtual void Tick(const FGeometry& Geometry, const double, const float DeltaTime) override
	{
		Time += DeltaTime;
		const AFTStoryDirector* D = GetDirector ? GetDirector() : nullptr;
		Shown = nullptr;
		if (!D)
		{
			return;
		}
		ftcore::DialogueViewFlags Flags;
		Flags.autoOn = D->IsAutoAdvanceOn();
		Flags.skipOn = D->IsSkipping();
		const ftcore::DialogueBacklog* Log = D->GetBacklog();
		Flags.logAvailable = Log && Log->size() > 0;
		ftcore::updateDialogueView(View, D->GetPresenter(), Flags);
		if (View.kind == ftcore::DialogueViewKind::Hidden || D->GetText().IsEmpty())
		{
			return;
		}
		Shown = D;
		const FVector2f Size = Geometry.GetLocalSize();
		const float Scale = D->GetSubtitleScale();
		if (WrappedSerial != D->GetBeatSerial() || WrappedBeat != View.beatRevision || WrappedSize != Size || WrappedScale != Scale)
		{
			Rewrap(*D, Size, Scale);
		}
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer,
		const FWidgetStyle&, bool) const override
	{
		const AFTStoryDirector* D = Shown;
		if (!D || Lines.Num() == 0)
		{
			return Layer;
		}
		const FVector2f Size = Geometry.GetLocalSize();
		const FPainter P{Geometry, Out, Layer, FMath::Max(0.25f, Size.Y / 1080.f)};
		const float S = P.S;
		const FString& Text = D->GetText();
		const bool bOptions = View.kind == ftcore::DialogueViewKind::Choice && D->GetPresenter().optionsVisible();
		const int32 OptionRows = bOptions ? FMath::Min(static_cast<int32>(View.optionCount), D->GetOptions().Num()) : 0;
		const float BlockH = LineH * (Lines.Num() + OptionRows + (OptionRows > 0 ? 0.4f : 0.f));
		const float Bottom = Size.Y - 96.f * S;
		const float Top = Bottom - BlockH;
		const float CX = Size.X * 0.5f;
		P.Soft(CX, Top + BlockH * 0.5f, MaxW + 360.f * S, BlockH + 200.f * S, Table().Style.backingSoftDialogue);

		const FLinearColor Body = View.kind == ftcore::DialogueViewKind::Direction ? Tok(UiToken::InkDim) : Tok(UiToken::Ink);
		const int32 RevealedUnits = CpToUnit.Num() > 0 ? CpToUnit[FMath::Min(static_cast<int32>(View.revealedChars), CpToUnit.Num() - 1)] : 0;
		float LastRight = CX;
		for (int32 I = 0; I < Lines.Num(); ++I)
		{
			const float Lead = I == 0 ? SpeakerW : 0.f;
			float X = CX - (LineW[I] + Lead) * 0.5f;
			const float Y = Top + LineH * I;
			if (I == 0 && !SpeakerUpper.IsEmpty())
			{
				P.Text(X, Y + SpeakerDy, SpeakerUpper, SpeakerFont, Tok(UiToken::Accent));
			}
			X += Lead;
			const int32 End = FMath::Min(Lines[I].Y, RevealedUnits);
			P.Text(X, Y, Text, TextFont, Body, Lines[I].X, End);
			LastRight = X + LineW[I];
		}
		if (OptionRows > 0)
		{
			const float OX = CX - MaxW * 0.35f;
			float Y = Top + LineH * (Lines.Num() + 0.4f);
			const float Mark = 10.f * S;
			for (int32 I = 0; I < OptionRows; ++I, Y += LineH)
			{
				const bool bPick = View.options[static_cast<size_t>(I)].selectable;
				const float MY = Y + (OptionH - Mark) * 0.5f;
				if (bPick)
				{
					P.Box(OX - 24.f * S, MY, Mark, Mark, Tok(UiToken::Accent), 1);
				}
				else
				{
					P.Hollow(OX - 24.f * S, MY, Mark, Mark, FMath::Max(1.f, S), Tok(UiToken::InkDim));
				}
				P.Text(OX, Y, D->GetOptions()[I], OptionFont, bPick ? Tok(UiToken::Ink) : Tok(UiToken::InkDim));
			}
		}
		else if (View.continuePrompt)
		{
			const float A = 0.55f + 0.45f * FMath::Sin(Time * 5.f);
			P.Chevron(LastRight + 22.f * S, Top + LineH * (Lines.Num() - 1) + LineH * 0.45f, 18.f * S, Tok(UiToken::Accent, A));
		}
		return Layer + 3;
	}

private:
	// Once per beat (or on a size change): fonts, the speaker's capitals, the codepoint-to-unit map and the line breaks.
	void Rewrap(const AFTStoryDirector& D, const FVector2f& Size, float Scale)
	{
		WrappedSerial = D.GetBeatSerial();
		WrappedBeat = View.beatRevision;
		WrappedSize = Size;
		WrappedScale = Scale;
		const float S = FMath::Max(0.25f, Size.Y / 1080.f);
		TextFont = FontFor(UiTextRole::Subtitle, Size.Y, Scale);
		SpeakerFont = FontFor(UiTextRole::Speaker, Size.Y, Scale);
		OptionFont = FontFor(UiTextRole::Subtitle, Size.Y, Scale);
		const TSharedRef<FSlateFontMeasure> M = FontMeasure();
		LineH = M->GetMaxCharacterHeight(TextFont) * 1.15f;
		OptionH = M->GetMaxCharacterHeight(OptionFont);
		SpeakerUpper = View.kind == ftcore::DialogueViewKind::Line ? D.GetSpeaker().ToUpper() : FString();
		SpeakerW = SpeakerUpper.IsEmpty() ? 0.f : TextWidth(SpeakerUpper, SpeakerFont) + 18.f * S;
		SpeakerDy = (M->GetMaxCharacterHeight(TextFont) - M->GetMaxCharacterHeight(SpeakerFont)) * 0.62f;
		MaxW = FMath::Min(Size.X - 160.f * S, 1500.f * S);

		const FString& T = D.GetText();
		CpToUnit.Reset();
		for (int32 I = 0; I < T.Len(); ++I)
		{
			if (!IsLowSurrogateUnit(T[I]))
			{
				CpToUnit.Add(I);
			}
		}
		CpToUnit.Add(T.Len());

		// Greedy by spaces; a word wider than a line stands alone.
		Lines.Reset();
		LineW.Reset();
		const auto Width = [&](int32 From, int32 To) // [From, To)
		{ return To > From ? static_cast<float>(M->Measure(FStringView(T), From, To - 1, TextFont).X) : 0.f; };
		int32 Start = 0;
		int32 Break = -1;
		float Avail = MaxW - SpeakerW;
		for (int32 I = 0; I <= T.Len(); ++I)
		{
			const bool bEnd = I == T.Len();
			if (!bEnd && T[I] != TEXT(' ') && T[I] != TEXT('\n'))
			{
				continue;
			}
			if (Width(Start, I) > Avail && Break > Start)
			{
				Lines.Add(FIntPoint(Start, Break));
				Start = Break + 1;
				Avail = MaxW;
				I = Break; // re-run from the new line's start
				Break = -1;
				continue;
			}
			if (bEnd)
			{
				Lines.Add(FIntPoint(Start, T.Len()));
				break;
			}
			if (T[I] == TEXT('\n'))
			{
				Lines.Add(FIntPoint(Start, I));
				Start = I + 1;
				Avail = MaxW;
				Break = -1;
				continue;
			}
			Break = I;
		}
		for (const FIntPoint& L : Lines)
		{
			LineW.Add(Width(L.X, L.Y));
		}
	}

	TFunction<const AFTStoryDirector*()> GetDirector;
	const AFTStoryDirector* Shown = nullptr; // this frame's director when there is text to show
	ftcore::DialogueView View;
	float Time = 0.f;
	uint32 WrappedSerial = MAX_uint32;
	uint32 WrappedBeat = MAX_uint32;
	FVector2f WrappedSize = FVector2f::ZeroVector;
	float WrappedScale = -1.f;
	FSlateFontInfo TextFont;
	FSlateFontInfo SpeakerFont;
	FSlateFontInfo OptionFont;
	FString SpeakerUpper;
	float SpeakerW = 0.f;
	float SpeakerDy = 0.f;
	float LineH = 0.f;
	float OptionH = 0.f;
	float MaxW = 0.f;
	TArray<int32> CpToUnit;
	TArray<FIntPoint> Lines; // [start, end) in UTF-16 units
	TArray<float> LineW;
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

TSharedRef<SWidget> MakeDialogueWidget(TFunction<const AFTStoryDirector*()> GetDirector)
{
	return SNew(SFTDialogue, MoveTemp(GetDirector));
}
}
