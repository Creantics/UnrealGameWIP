#include "FTNewUi.h"

#include "FTArenaFighter.h"
#include "FTArenaRunner.h"
#include "ForgottenThird.h"
#include "FTInputBindings.h"
#include "FTStoryDirector.h"
#include "FTText.h"
#include "FTUiText.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/Settings.h"
#include "ftcore/InputBindings.h"
#include "ftcore/UiStroke.h"
#include "ftcore/UiStyle.h"
#include "ftcore/UiViews.h"

#include "Algo/BinarySearch.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "Fonts/CompositeFont.h"
#include "Fonts/FontMeasure.h"
#include "HAL/FileManager.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
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
	TEXT("-FTNewUi on the command line sets it. Read when a map's HUD begins: a change applies from the next map."));

using ftcore::UiTextRole;
using ftcore::UiToken;

const FTUiText kTargetUi(TEXT("arena.target"));
const FTUiText kReleaseReadyUi(TEXT("arena.release_ready"));
const FTUiText kOverloadedUi(TEXT("arena.overloaded"));
const FTUiText kGuardBrokenUi(TEXT("arena.guard_broken"));
const FTUiText kDownUi(TEXT("battle.down"));
const FTUiText kWeightUi(TEXT("arena.boss.weight"));
const FTUiText kWeakPointUi(TEXT("arena.boss.weak_point"));
const FTUiText kInvulnerableUi(TEXT("arena.boss.invulnerable"));
const FTUiText kCallUi(TEXT("arena.boss.call"));
const FTUiText kZoneUi[3] = {FTUiText(TEXT("arena.boss.left")), FTUiText(TEXT("arena.boss.right")), FTUiText(TEXT("arena.boss.path"))};
const FTUiText kReleaseOnCallUi(TEXT("arena.boss.release_on_call"));
const FTUiText kInterceptUi(TEXT("arena.boss.intercept"));
const FTUiText kGuardNoteUi(TEXT("arena.boss.guard"));
const FTUiText kWrongSideUi(TEXT("arena.boss.wrong_side"));
const FTUiText kMissedUi(TEXT("arena.boss.missed"));
const FTUiText kBrokenNoteUi(TEXT("arena.boss.broken"));
const FTUiText kShockRingUi(TEXT("arena.boss.shock_ring"));
const FTUiText kCriticalUi(TEXT("arena.boss.critical"));
const FTUiText kLiftedUi(TEXT("arena.boss.lifted"));
constexpr double kBossNoteLife = 1.2; // the latest answer or rule on the boss line, fading
// The world's marks and the fight's events (the old canvas HUD's information, in the Carry hand).
const FTUiText kAnswerUi[3] = {FTUiText(TEXT("arena.parry")), FTUiText(TEXT("arena.block")), FTUiText(TEXT("arena.dodge"))};
const FTUiText kNowUi(TEXT("battle.now"));
const FTUiText kStaggeredUi(TEXT("battle.staggered"));
const FTUiText kRiposteUi(TEXT("arena.riposte"));
const FTUiText kTellUi[] = {FTUiText(TEXT("arena.tell.strike")), FTUiText(TEXT("arena.tell.overhead")), FTUiText(TEXT("arena.tell.backhand")),
	FTUiText(TEXT("arena.tell.charge")), FTUiText(TEXT("arena.tell.thrust")), FTUiText(TEXT("arena.tell.slash")), FTUiText(TEXT("arena.tell.sweep")),
	FTUiText(TEXT("arena.tell.slam")), FTUiText(TEXT("arena.tell.shot"))};
static_assert(UE_ARRAY_COUNT(kTellUi) == ftcore::kArenaTellCount, "a word for every ArenaTell");
// The popup words, by EPopWord.
enum EPopWord : int32 { PwBlocked, PwGuardBroken, PwParried, PwEvaded, PwStaggered, PwDown, PwCounter, PwReadCounter, PwMisread, PwNoStamina, PwCount };
const FTUiText kPopWordUi[PwCount] = {FTUiText(TEXT("arena.blocked")), FTUiText(TEXT("arena.guard_broken")), FTUiText(TEXT("arena.parried")),
	FTUiText(TEXT("arena.evaded")), FTUiText(TEXT("battle.staggered")), FTUiText(TEXT("battle.down")), FTUiText(TEXT("arena.counter")),
	FTUiText(TEXT("arena.read_counter")), FTUiText(TEXT("arena.misread")), FTUiText(TEXT("arena.no_stamina"))};
const FTUiText kReadUi(TEXT("arena.read"));
const FTUiText kLoadPlusUi(TEXT("arena.load_plus"));
const FTUiText kReleaseUi(TEXT("arena.release"));
const FTUiText kOverloadLockedUi(TEXT("arena.overload_locked"));
const FTUiText kVictoryUi(TEXT("battle.victory"));
const FTUiText kDefeatUi(TEXT("battle.defeat"));
const FTUiText kAgainUi(TEXT("battle.again"));
const FTUiText kStatsUi(TEXT("arena.stats"));
const FTUiText kAutoPlayUi(TEXT("arena.autoplay"));
const FTUiText kIdleUi(TEXT("arena.idle"));
constexpr double kPopupLife = 0.9;
constexpr double kImpactLife = 0.3;    // the tell's flash once the move is active
constexpr double kOnsetLife = 0.35;    // an off-frame windup marker's onset swell
constexpr double kHitEdgeLife = 0.45;  // the edge hairline when the lead is hit
constexpr double kParryEdgeLife = 0.4;
constexpr double kResultFade = 0.3;
constexpr double kReleaseBanner = 1.0;
constexpr double kOverloadBanner = 1.4;

// 1 at Time, falling to 0 over Life (0 before and after).
float Pulse(double Now, double Time, double Life)
{
	const double Age = Now - Time;
	return (Age >= 0.0 && Age < Life) ? static_cast<float>(1.0 - Age / Life) : 0.f;
}

// The colour of the answer a tell asks for (parry, block, dodge), with the style's cue shapes beside the word.
UiToken AnswerTok(uint8 Answer) { return Answer == 2 ? UiToken::Danger : Answer == 1 ? UiToken::Accent : UiToken::WeakPoint; }

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

// The UI's faces, read once from Content/UI/Fonts (OFL, each beside its licence): the serif and its small caps for
// display and labels, the sans for key caps and figures only. The small-caps files are baked by
// tools/bake_small_caps.py (Slate shapes Latin kerning-only, so the font's own substitution would never run). A missing
// file leaves that face on the engine's Roboto, logged once.
const FName FaceName(TEXT("Face")); // each composite holds one typeface entry, named

struct FUiFaces
{
	TSharedPtr<const FCompositeFont> Font[ftcore::kUiFontCount];
};

const FUiFaces& Faces()
{
	static FUiFaces Built = []
	{
		FUiFaces F;
		const FString Dir = FPaths::ProjectContentDir() / TEXT("UI/Fonts");
		const TCHAR* Files[ftcore::kUiFontCount] = {
			TEXT("SourceSans3/SourceSans3-Regular.ttf"),   // Sans
			TEXT("SourceSans3/SourceSans3-Semibold.ttf"),  // SansBold
			TEXT("Spectral/Spectral-Medium.ttf"),          // Serif
			nullptr,                                       // Hand: the placeholder italic
			TEXT("Spectral/Spectral-SemiBold.ttf"),        // SerifBold
			TEXT("Spectral/SpectralSCBaked-Medium.ttf"),   // SerifSc
			TEXT("Spectral/SpectralSCBaked-SemiBold.ttf"), // SerifScBold
		};
		int32 Loaded = 0;
		for (size_t I = 0; I < ftcore::kUiFontCount; ++I)
		{
			if (!Files[I])
			{
				continue;
			}
			const FString Path = Dir / Files[I];
			if (IFileManager::Get().FileSize(*Path) < 4096) // missing (-1), or an unfetched LFS pointer (tiny)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("UI: font %s missing; that face stays on Roboto."), *Path);
				continue;
			}
			F.Font[I] = MakeShared<FStandaloneCompositeFont>(FaceName, Path, EFontHinting::Default, EFontLoadingPolicy::LazyLoad);
			++Loaded;
		}
		UE_LOG(LogForgottenThird, Log, TEXT("UI: %d of 6 font faces loaded from %s."), Loaded, *Dir);
		return F;
	}();
	return Built;
}

// Every glyph carries the hairline edge.
FSlateFontInfo FontFor(UiTextRole Role, float Height, float TextScale = 1.f)
{
	const ftcore::UiStyle& S = Table().Style;
	const ftcore::UiTextStyle& T = S.text[static_cast<size_t>(Role)];
	const float Px = ftcore::uiTextPx(S, Role, Height, TextScale);
	const TSharedPtr<const FCompositeFont>& Face = Faces().Font[static_cast<size_t>(T.font)];
	const bool bBold = T.font == ftcore::UiFont::SansBold || T.font == ftcore::UiFont::SerifBold || T.font == ftcore::UiFont::SerifScBold;
	const TCHAR* Fallback = bBold ? TEXT("Bold") : (T.font == ftcore::UiFont::Hand ? TEXT("Italic") : TEXT("Regular"));
	// Slate sizes are points at 96 dpi.
	FSlateFontInfo F = Face.IsValid() ? FSlateFontInfo(Face, Px * 0.75f, FaceName) : FCoreStyle::GetDefaultFontStyle(Fallback, Px * 0.75f);
	F.LetterSpacing = FMath::RoundToInt(T.tracking * 1000.f);
	// -FTUiEdge=<0..1> scales the glyph edge's strength (0 drops it): a capture switch for judging the edge, not a setting.
	static const float EdgeScale = []
	{
		float V = 1.f;
		FParse::Value(FCommandLine::Get(), TEXT("-FTUiEdge="), V);
		return FMath::Clamp(V, 0.f, 1.f);
	}();
	F.OutlineSettings.OutlineSize = EdgeScale <= 0.f ? 0 : FMath::Max(1, FMath::RoundToInt(ftcore::uiEdgePx(S, Height)));
	F.OutlineSettings.OutlineColor = Table().Edge;
	F.OutlineSettings.OutlineColor.A *= S.edgeText * EdgeScale; // lighter than the strokes' edge: words never read doubled
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
		// One brush on one layer, so Slate batches them into a single draw; 48 keeps each band under ~15 px on a wide pool
		// of shade (24 showed as rings over a bright lamp).
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
		// The hairline edge fades with the word: a faded word keeps its light face, never a dark hollow outline.
		FSlateFontInfo Faded = Font;
		Faded.OutlineSettings.OutlineColor.A *= C.A;
		const FSlateFontInfo& Use = C.A < 0.999f ? Faded : Font;
		if (End < 0)
		{
			FSlateDrawElement::MakeText(Out, Layer + 3, Geo, Str, Use, ESlateDrawEffect::None, C);
		}
		else if (End > Start)
		{
			FSlateDrawElement::MakeText(Out, Layer + 3, Geo, Str, Start, End, Use, ESlateDrawEffect::None, C);
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

// ---- the line mesh: every stroke, ring, mark and soft shade of a widget in one custom-verts element ------------------
//
// Core gives each line's points (UiStroke: place, half width, alpha); this turns them into triangles with a soft 1 px
// edge, a dark under-edge beneath (the glyph edge's twin, so a line holds on the sky), and draws the lot in one
// element: shades first, then every under-edge, then every line, so one line's dark edge never cuts another's core.
// The arrays are kept between frames (Reset, not freed): no allocation once warm. The engine copies them per element.
class FLineMesh
{
public:
	void Begin(const FGeometry& Geometry, float InS)
	{
		Transform = Geometry.GetAccumulatedRenderTransform();
		S = InS;
		for (FPart& P : Parts)
		{
			P.Verts.Reset();
			P.Idx.Reset();
		}
		if (!bHandle)
		{
			bHandle = true;
			Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")));
			const FSlateShaderResourceProxy* Proxy = Handle.GetResourceProxy();
			UV = Proxy ? FVector2f(Proxy->StartUV + Proxy->SizeUV * 0.5f) : FVector2f(0.5f, 0.5f);
		}
	}

	// A line from Core. Closed: the first and last points are the same place (a ring), joined without a seam.
	void Stroke(const ftcore::UiStroke& St, const FLinearColor& C, bool bEdge = true, bool bClosed = false)
	{
		if (St.count < 2 || C.A <= 0.f)
		{
			return;
		}
		if (bEdge)
		{
			Ribbon(Parts[Under], St, Table().Edge, 0.55f * C.A, 1.f * S, 1.6f, bClosed);
		}
		Ribbon(Parts[Core], St, C, 1.f, 0.f, 0.8f, bClosed);
	}

	// A filled convex shape (a dot, a lozenge, a small triangle), edged like the lines.
	void Fill(const FVector2f* Pts, int32 N, const FLinearColor& C, bool bEdge = true)
	{
		if (N < 3 || C.A <= 0.f)
		{
			return;
		}
		if (bEdge)
		{
			Convex(Parts[Under], Pts, N, Table().Edge, 0.5f * C.A, 1.f * S, 1.6f);
		}
		Convex(Parts[Core], Pts, N, C, 1.f, 0.f, 0.8f);
	}

	void Dot(float CX, float CY, float R, const FLinearColor& C, bool bEdge = true)
	{
		FVector2f Pts[16];
		for (int32 I = 0; I < 16; ++I)
		{
			const float A = I * (2.f * UE_PI / 16.f);
			Pts[I] = FVector2f(CX + R * FMath::Cos(A), CY + R * FMath::Sin(A));
		}
		Fill(Pts, 16, C, bEdge);
	}

	// Shade with no edge at all: an ellipse whose alpha falls smoothly from Peak in the middle to nothing at the rim.
	void Shade(float CX, float CY, float RX, float RY, float Peak)
	{
		if (Peak <= 0.f || RX <= 0.f || RY <= 0.f)
		{
			return;
		}
		constexpr int32 Rings = 12;
		constexpr int32 Segs = 48;
		FPart& P = Parts[ShadePart];
		const SlateIndex Base = static_cast<SlateIndex>(P.Verts.Num());
		const FLinearColor Back = Tok(UiToken::Backing, 1.f);
		P.Verts.Add(Vert(CX, CY, Back, Peak));
		for (int32 R = 1; R <= Rings; ++R)
		{
			const float T = static_cast<float>(R) / Rings;
			const float A = Peak * FMath::Square(1.f - T * T); // zero slope at the rim: no edge reads
			for (int32 K = 0; K < Segs; ++K)
			{
				const float Ang = K * (2.f * UE_PI / Segs);
				P.Verts.Add(Vert(CX + RX * T * FMath::Cos(Ang), CY + RY * T * FMath::Sin(Ang), Back, A));
			}
		}
		for (int32 K = 0; K < Segs; ++K)
		{
			const int32 K1 = (K + 1) % Segs;
			Tri(P, Base, Base + 1 + K, Base + 1 + K1);
			for (int32 R = 1; R < Rings; ++R)
			{
				const int32 I0 = Base + 1 + (R - 1) * Segs;
				const int32 I1 = I0 + Segs;
				Tri(P, I0 + K, I1 + K, I1 + K1);
				Tri(P, I0 + K, I1 + K1, I0 + K1);
			}
		}
	}

	void Flush(FSlateWindowElementList& Out, int32 Layer)
	{
		All.Reset();
		AllIdx.Reset();
		for (const FPart& P : Parts)
		{
			const SlateIndex Off = static_cast<SlateIndex>(All.Num());
			All.Append(P.Verts);
			for (const SlateIndex I : P.Idx)
			{
				AllIdx.Add(I + Off);
			}
		}
		if (AllIdx.Num() > 0)
		{
			FSlateDrawElement::MakeCustomVerts(Out, Layer, Handle, All, AllIdx, nullptr, 0, 0);
		}
	}

private:
	enum EPart { ShadePart, Under, Core, PartCount };
	struct FPart
	{
		TArray<FSlateVertex> Verts;
		TArray<SlateIndex> Idx;
	};

	FSlateVertex Vert(float X, float Y, const FLinearColor& C, float A) const
	{
		FLinearColor L = C;
		L.A = FMath::Clamp(A, 0.f, 1.f);
		return FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, FVector2f(X, Y), UV, L.ToFColor(true));
	}

	static void Tri(FPart& P, int32 A, int32 B, int32 C)
	{
		P.Idx.Add(static_cast<SlateIndex>(A));
		P.Idx.Add(static_cast<SlateIndex>(B));
		P.Idx.Add(static_cast<SlateIndex>(C));
	}

	// Four verts across each point (outer edge clear, two core, outer edge clear) and three quads between points.
	void Ribbon(FPart& P, const ftcore::UiStroke& St, const FLinearColor& C, float AlphaK, float Grow, float Feather, bool bClosed)
	{
		const int32 N = static_cast<int32>(St.count);
		const int32 Base = P.Verts.Num();
		for (int32 I = 0; I < N; ++I)
		{
			const ftcore::UiStrokePoint& Pt = St.pt[I];
			int32 Prev = I - 1;
			int32 Next = I + 1;
			if (bClosed)
			{
				Prev = I == 0 ? N - 2 : Prev;
				Next = I == N - 1 ? 1 : Next;
			}
			const ftcore::UiStrokePoint& A = St.pt[FMath::Clamp(Prev, 0, N - 1)];
			const ftcore::UiStrokePoint& B = St.pt[FMath::Clamp(Next, 0, N - 1)];
			FVector2f T(B.x - A.x, B.y - A.y);
			T = T.GetSafeNormal();
			const FVector2f Nrm(-T.Y, T.X);
			const float H = Pt.halfWidth + Grow;
			const float Inner = FMath::Max(0.f, H - Feather * 0.5f);
			const float Outer = H + Feather * 0.5f;
			const float Alpha = C.A * AlphaK * Pt.alpha * FMath::Min(1.f, 2.f * H); // a line under 1 px thins in alpha
			const FVector2f O(Pt.x, Pt.y);
			const FVector2f V0 = O - Nrm * Outer;
			const FVector2f V1 = O - Nrm * Inner;
			const FVector2f V2 = O + Nrm * Inner;
			const FVector2f V3 = O + Nrm * Outer;
			P.Verts.Add(Vert(V0.X, V0.Y, C, 0.f));
			P.Verts.Add(Vert(V1.X, V1.Y, C, Alpha));
			P.Verts.Add(Vert(V2.X, V2.Y, C, Alpha));
			P.Verts.Add(Vert(V3.X, V3.Y, C, 0.f));
		}
		for (int32 I = 0; I + 1 < N; ++I)
		{
			const int32 B0 = Base + I * 4;
			const int32 B1 = B0 + 4;
			for (int32 K = 0; K < 3; ++K)
			{
				Tri(P, B0 + K, B0 + K + 1, B1 + K + 1);
				Tri(P, B0 + K, B1 + K + 1, B1 + K);
			}
		}
	}

	void Convex(FPart& P, const FVector2f* Pts, int32 N, const FLinearColor& C, float AlphaK, float Grow, float Feather)
	{
		FVector2f Mid(0.f, 0.f);
		for (int32 I = 0; I < N; ++I)
		{
			Mid += Pts[I];
		}
		Mid /= static_cast<float>(N);
		const float A = C.A * AlphaK;
		const int32 Base = P.Verts.Num();
		P.Verts.Add(Vert(Mid.X, Mid.Y, C, A));
		for (int32 I = 0; I < N; ++I)
		{
			const FVector2f Out = (Pts[I] - Mid).GetSafeNormal();
			const FVector2f In = Pts[I] + Out * (Grow - Feather * 0.5f);
			const FVector2f Rim = Pts[I] + Out * (Grow + Feather * 0.5f);
			P.Verts.Add(Vert(In.X, In.Y, C, A));
			P.Verts.Add(Vert(Rim.X, Rim.Y, C, 0.f));
		}
		for (int32 I = 0; I < N; ++I)
		{
			const int32 J = (I + 1) % N;
			const int32 In0 = Base + 1 + I * 2;
			const int32 In1 = Base + 1 + J * 2;
			Tri(P, Base, In0, In1);
			Tri(P, In0, In0 + 1, In1 + 1);
			Tri(P, In0, In1 + 1, In1);
		}
	}

	FPart Parts[PartCount];
	TArray<FSlateVertex> All;
	TArray<SlateIndex> AllIdx;
	FSlateRenderTransform Transform;
	float S = 1.f;
	FSlateResourceHandle Handle;
	FVector2f UV = FVector2f(0.5f, 0.5f);
	bool bHandle = false;
};

// A hand-placed polyline (a chevron, a key ring's stadium) as a stroke of even weight.
void PolyStroke(ftcore::UiStroke& Out, const FVector2f* Pts, int32 N, float HalfWidth, float Alpha = 1.f)
{
	Out.count = 0;
	for (int32 I = 0; I < N && Out.count < ftcore::kUiStrokeMaxPoints; ++I)
	{
		ftcore::UiStrokePoint& P = Out.pt[Out.count++];
		P.x = Pts[I].X;
		P.y = Pts[I].Y;
		P.halfWidth = HalfWidth;
		P.alpha = Alpha;
	}
}

// A closed rounded box (a key cap) round (CX, CY): W wide, H high, corners of radius Corner; the first point repeats at
// the end.
void KeyCapStroke(ftcore::UiStroke& Out, float CX, float CY, float W, float H, float HalfWidth, float Corner)
{
	const float R = FMath::Clamp(Corner, 0.f, FMath::Min(W, H) * 0.5f);
	const float HX = W * 0.5f - R;
	const float HY = H * 0.5f - R;
	constexpr int32 Arc = 6;
	Out.count = 0;
	const auto Put = [&Out, HalfWidth](float X, float Y)
	{
		ftcore::UiStrokePoint& P = Out.pt[Out.count++];
		P.x = X;
		P.y = Y;
		P.halfWidth = HalfWidth;
		P.alpha = 1.f;
	};
	const float SX[4] = {1.f, 1.f, -1.f, -1.f}; // the corners clockwise from the top right
	const float SY[4] = {-1.f, 1.f, 1.f, -1.f};
	for (int32 K = 0; K < 4; ++K)
	{
		for (int32 I = 0; I <= Arc; ++I)
		{
			const float A = -0.5f * UE_PI + 0.5f * UE_PI * (K + static_cast<float>(I) / Arc);
			Put(CX + SX[K] * HX + R * FMath::Cos(A), CY + SY[K] * HY + R * FMath::Sin(A));
		}
	}
	Put(Out.pt[0].x, Out.pt[0].y);
}

// ---- the arena HUD ----------------------------------------------------------------------------------------------------

constexpr uint32 SectionBit(ftcore::ArenaHudSection Sec) { return 1u << static_cast<uint32>(Sec); }

// The boss line's word for a stone bearer note, and its colour; null for a note the line does not show.
const FTUiText* BossNoteWord(const FFTStoneBearerNote& Note, UiToken& OutTok)
{
	if (Note.Kind == FFTStoneBearerNote::EKind::ShockRingSent)
	{
		OutTok = UiToken::Danger;
		return &kShockRingUi;
	}
	if (Note.Kind != FFTStoneBearerNote::EKind::Run)
	{
		return nullptr;
	}
	switch (Note.Event.type)
	{
	case ftcore::ArenaBossEventType::Answered:
		switch (Note.Event.outcome)
		{
		case ftcore::CalledOutcome::Intercepted:
			OutTok = UiToken::Accent;
			return &kInterceptUi;
		case ftcore::CalledOutcome::Guarded:
			OutTok = UiToken::Ink;
			return &kGuardNoteUi;
		case ftcore::CalledOutcome::WrongZone:
			OutTok = UiToken::Danger;
			return &kWrongSideUi;
		case ftcore::CalledOutcome::Missed:
			OutTok = UiToken::Danger;
			return &kMissedUi;
		default:
			return nullptr;
		}
	case ftcore::ArenaBossEventType::Broke:
		OutTok = UiToken::Accent;
		return &kBrokenNoteUi;
	case ftcore::ArenaBossEventType::Crit:
		OutTok = UiToken::Accent;
		return &kCriticalUi;
	case ftcore::ArenaBossEventType::Lifted:
		OutTok = UiToken::Accent;
		return &kLiftedUi;
	default:
		return nullptr;
	}
}

// The boss section from whichever boss host runs this fight.
void FillBoss(const UFTArenaRunner& Runner, ftcore::ArenaHudBoss& Out)
{
	Out = ftcore::ArenaHudBoss{};
	const ftcore::ArenaFight& Fight = Runner.GetFight();
	size_t Slot = 0;
	bool bOpen = false;
	if (const FFTKiteHost* Kite = Runner.GetKite())
	{
		Slot = Kite->GetSlot();
		bOpen = Kite->IsOpen();
	}
	else if (const FFTStoneBearerHost* Bearer = Runner.GetStoneBearer())
	{
		Slot = Bearer->GetSlot();
		const ftcore::StoneBearer& Boss = Bearer->GetBoss();
		const ftcore::ArenaBossRun& Run = Boss.run();
		const ftcore::ArenaBossDef& Def = Bearer->GetDef().boss;
		bOpen = Run.crit().isOpen(Run.now());
		Out.invulnerable = Boss.invulnerable();
		const int32 WeightMax = FMath::Clamp(Bearer->GetDef().weight.cap, 0, 8);
		Out.weightMax = static_cast<std::uint8_t>(WeightMax);
		Out.weight = static_cast<std::uint8_t>(FMath::Clamp(static_cast<int32>(Boss.stacks()), 0, WeightMax));
		// The phases: their floors place the notches on the line.
		const int32 Phases = FMath::Clamp(static_cast<int32>(Def.phaseCount), 1, static_cast<int32>(ftcore::kArenaHudPhases));
		Out.phaseCount = static_cast<std::uint8_t>(Phases);
		Out.phase = static_cast<std::uint8_t>(FMath::Min(static_cast<int32>(Run.phase()), Phases - 1));
		for (int32 I = 0; I < Phases; ++I)
		{
			Out.phaseEnd[static_cast<size_t>(I)] = FMath::Clamp(Def.phases[static_cast<size_t>(I)].endAt, 0.f, 1.f);
		}
		Out.down = Boss.down();
		// A called blow: the zone it wants next, and whether the lead stands in it.
		const ftcore::CalledStrike& Strike = Run.strike();
		const std::uint8_t Live = Run.liveMove();
		if (!Out.invulnerable && !Out.down && Live < Def.moveCount && Def.moves[Live].intercept() && !Strike.complete())
		{
			const ftcore::CalledZone Want = Strike.nextZone();
			Out.call = Want == ftcore::CalledZone::Left ? ftcore::UiCalledZone::Left
				: Want == ftcore::CalledZone::Right   ? ftcore::UiCalledZone::Right
													  : ftcore::UiCalledZone::Path;
			Out.callIn = Bearer->ZoneNow(Runner.GetBodies()) == Want;
		}
		// The latest note the line has a word for (its fade is the widget's).
		for (int32 Serial = Bearer->GetNoteTotal() - 1; Serial >= Bearer->GetNoteTotal() - Bearer->GetNoteCount(); --Serial)
		{
			UiToken Unused = UiToken::Ink;
			if (BossNoteWord(Bearer->GetNoteBySerial(Serial), Unused))
			{
				Out.noteSerial = Serial;
				break;
			}
		}
	}
	else
	{
		return;
	}
	if (Slot >= Fight.enemyCount() || !Fight.enemyAlive(Slot))
	{
		Out = ftcore::ArenaHudBoss{};
		return;
	}
	Out.present = true;
	Out.slot = static_cast<std::uint8_t>(Slot);
	Out.nameKey = static_cast<std::uint16_t>(Slot);
	Out.hp = Fight.enemyHp(Slot);
	Out.hpMax = Fight.def().enemies[Slot].hp;
	Out.weakPoint = bOpen ? ftcore::UiWeakPoint::Open : ftcore::UiWeakPoint::Closed;
}

// The Carry layout's places in 1080p units: the lead's ring bottom left, the verbs bottom right, the boss at the top.
namespace Carry
{
constexpr float LeadX = 190.f;  // the ring's centre, in from the left
constexpr float LeadUp = 200.f; // and up from the bottom
constexpr float RingR = 54.f;
constexpr float StaminaR = 63.f;
constexpr float HpR = 70.f;
constexpr float LoadR = 84.f;
constexpr float A0 = -135.f; // the arcs' sweep: open at the bottom, where the numbers sit (0 = up, clockwise)
constexpr float A1 = 135.f;
constexpr float VerbIn = 108.f;  // the verbs' rule, in from the right
constexpr float VerbUp = 238.f;  // the first verb's baseline, up from the bottom
constexpr float VerbStep = 46.f;
constexpr float TopLine = 100.f; // the boss's line, down from the top
constexpr float BossLen = 680.f;
constexpr float TargetLen = 420.f;
constexpr float LowHp = 0.3f;
} // namespace Carry

// What was just lost: held a moment, then rubbed away toward the value (a hit rubs the line, it does not snap).
struct FLostTrace
{
	float Was = 0.f;
	float Hold = 0.f;

	void Step(float Now, float Dt)
	{
		if (Now >= Was)
		{
			Was = Now;
			Hold = 0.f;
			return;
		}
		Hold += Dt;
		if (Hold > 0.8f)
		{
			Was = FMath::Max(Now, Was - 0.35f * Dt);
		}
	}
};

// The verbs bottom right, in the controls table's order; each shows only when its device has a key for it.
struct FVerbSlot
{
	const char* Label; // the controls table's word key (Data/Loc/en/ui.json)
	FString Word;
	FString Keys;
	float WordW = 0.f;
	float KeysW = 0.f;
};

ftcore::UiStrokeStyle StrokeStyle(float Width, std::uint32_t Seed, float S, float Taper = 0.15f)
{
	ftcore::UiStrokeStyle St;
	St.width = Width * S;
	St.wobble = 0.8f * S;
	St.step = 4.f * S;
	St.taper = Taper;
	St.seed = Seed;
	if (Taper <= 0.f)
	{
		St.endWidth = 1.f;
		St.endAlpha = 1.f;
	}
	return St;
}

FVector2f Rotated(float X, float Y, float Deg) // screen rotation, clockwise for a positive angle
{
	const float R = FMath::DegreesToRadians(Deg);
	return FVector2f(X * FMath::Cos(R) - Y * FMath::Sin(R), X * FMath::Sin(R) + Y * FMath::Cos(R));
}

// The arena HUD's world marks, one per enemy (TickWorld fills them, PaintWorld draws them).
struct FFoeMark
{
	bool bShow = false;  // alive, with a figure
	bool bOn = false;    // its body on the screen
	bool bLine = false;  // its thin hp line drawn (the top line carries the target's and the boss's)
	FVector2f Over = FVector2f::ZeroVector;
	FVector2f Chest = FVector2f::ZeroVector;
	float Hp01 = 0.f;
	float Poise01 = 0.f;
	uint8 Word = 0;   // 1 staggered, 2 open to a riposte
	uint8 Tell = 0;   // 0 none, 1 winding up, 2 the hit
	uint8 Answer = 0; // 0 parry, 1 block, 2 dodge
	float Fill = 0.f;
	float Impact = 0.f;
	float Onset = 0.f;
	bool bEdge = false; // drawn at the screen's edge (off frame)
	FVector2f Dir = FVector2f(0.f, 1.f);
	bool bCards = false; // the Read cards are dealt on this windup
};

// The arena HUD's popups (TickEvents fills them, PaintEvents draws them).
struct FPopMark
{
	bool bShow = false;
	int32 Serial = -1; // the popup the number was built for
	int32 Amount = 0;
	float A = 0.f;
	UiToken Tok = UiToken::Ink;
	int32 Word = -1; // an EPopWord, or none
	FVector2f WordAt = FVector2f::ZeroVector;
	FString Num;     // the number, built once per popup
	float NumW = 0.f;
	bool bNumPop = false; // the number in the popup face (with a word in it), else the figures' face
	FVector2f NumAt = FVector2f::ZeroVector;
};

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
		if (H != BuiltHeight)
		{
			BuiltHeight = H;
			LabelFont = FontFor(UiTextRole::Label, H);
			ValueFont = FontFor(UiTextRole::Value, H);
			VerbFont = FontFor(UiTextRole::Verb, H);
			NameFont = FontFor(UiTextRole::Title, H);
			NameAsc = FontMeasure()->GetMaxCharacterHeight(NameFont) + FontMeasure()->GetBaseline(NameFont);
			LabelH = FontMeasure()->GetMaxCharacterHeight(LabelFont);
			ValueAsc = FontMeasure()->GetMaxCharacterHeight(ValueFont) + FontMeasure()->GetBaseline(ValueFont);
			LabelAsc = LabelH + FontMeasure()->GetBaseline(LabelFont);
			VerbAsc = FontMeasure()->GetMaxCharacterHeight(VerbFont) + FontMeasure()->GetBaseline(VerbFont);
			// Each role says whether it shows in capitals; the small caps take mixed case (a capital, then small caps). In the
			// small-caps face "capitals" is all small caps (lowercase in, one even height out), as the sheet's verbs are.
			const auto Word = [this](const FString& In, FString& Out, float& W, const FSlateFontInfo& F, bool bUpper = false, bool bScFace = false)
			{
				Out = !bUpper ? In : bScFace ? In.ToLower() : In.ToUpper();
				W = TextWidth(Out, F);
			};
			Word(kDownUi, DownWord, DownW, VerbFont);
			Word(kOverloadedUi, OverWord, OverW, VerbFont);
			Word(kGuardBrokenUi, BrokenWord, BrokenW, VerbFont);
			Word(kReleaseReadyUi, ReadyWord, ReadyW, VerbFont);
			Word(kTargetUi, TargetWord, TargetW, LabelFont);
			Word(kWeightUi, WeightWord, WeightW, LabelFont);
			Word(kWeakPointUi, WeakWord, WeakW, LabelFont);
			Word(kInvulnerableUi, InvulWord, InvulW, LabelFont);
			Word(kCallUi, CallWord, CallW, LabelFont);
			Word(kReleaseOnCallUi, RocWord, RocW, LabelFont);
			Word(kDownUi, BossDownWord, BossDownW, LabelFont);
			for (int32 I = 0; I < 3; ++I)
			{
				Word(kZoneUi[I], ZoneWord[I], ZoneW[I], LabelFont);
			}
			NameSlot = -2; // measured again in the new font
			NoteSeen = -2;
			// The world's marks and the events: the popup words in the popup role, the big words in the title at 1.8.
			PopFont = FontFor(UiTextRole::Popup, H);
			PopAsc = FontMeasure()->GetMaxCharacterHeight(PopFont) + FontMeasure()->GetBaseline(PopFont);
			BigFont = FontFor(UiTextRole::Title, H, 1.8f);
			BigAsc = FontMeasure()->GetMaxCharacterHeight(BigFont) + FontMeasure()->GetBaseline(BigFont);
			const auto RoleWord = [this](UiTextRole Role, const FString& In, FString& Out, float& W, const FSlateFontInfo& F)
			{
				const ftcore::UiTextStyle& St = Table().Style.text[static_cast<size_t>(Role)];
				const bool bSc = St.font == ftcore::UiFont::SerifSc || St.font == ftcore::UiFont::SerifScBold;
				Out = !St.upper ? In : bSc ? In.ToLower() : In.ToUpper();
				W = TextWidth(Out, F);
			};
			for (int32 I = 0; I < 3; ++I)
			{
				RoleWord(UiTextRole::Popup, kAnswerUi[I], AnswerWord[I], AnswerW[I], PopFont);
			}
			RoleWord(UiTextRole::Popup, kNowUi, NowWord, NowW, PopFont);
			for (int32 I = 0; I < PwCount; ++I)
			{
				RoleWord(UiTextRole::Popup, kPopWordUi[I], PopWord[I], PopWordW[I], PopFont);
			}
			for (int32 I = 0; I < static_cast<int32>(ftcore::kArenaTellCount); ++I)
			{
				Word(kTellUi[I], TellWord[I], TellW[I], LabelFont);
			}
			Word(kStaggeredUi, StaggerWord, StaggerW, LabelFont);
			Word(kRiposteUi, RiposteWord, RiposteW, LabelFont);
			Word(kOverloadLockedUi, LockedWord, LockedW, LabelFont);
			Word(kAgainUi, AgainWord, AgainW, LabelFont);
			Word(kAutoPlayUi, TagWord[0], TagW[0], LabelFont);
			Word(kIdleUi, TagWord[1], TagW[1], LabelFont);
			RoleWord(UiTextRole::Title, kReleaseUi, ReleaseWord, ReleaseW, BigFont);
			RoleWord(UiTextRole::Title, kOverloadedUi, OverBigWord, OverBigW, BigFont);
			RoleWord(UiTextRole::Title, kVictoryUi, VictoryWord, VictoryW, BigFont);
			RoleWord(UiTextRole::Title, kDefeatUi, DefeatWord, DefeatW, BigFont);
			bStatsBuilt = false;
			ReleaseShown = -1;
			for (FPopMark& M : Pops)
			{
				M.Serial = -1; // its number measured again
			}
			for (FVerbSlot& V : Verbs)
			{
				const ftcore::UiTextStyle& VS = Table().Style.text[static_cast<size_t>(UiTextRole::Verb)];
				Word(FTUiText::Lookup(*FT::ToFString(V.Label)), V.Word, V.WordW, VerbFont, VS.upper,
					VS.font == ftcore::UiFont::SerifSc || VS.font == ftcore::UiFont::SerifScBold);
			}
			bRebuild = true; // kept until a live frame rebuilds the strings that need the fight
		}
		if (!bLive)
		{
			return;
		}
		const bool bRebuildNow = bRebuild;
		bRebuild = false;
		AimA = Runner->GetAimAlpha();
		const ftcore::ArenaFight& Fight = Runner->GetFight();
		ftcore::ArenaHudFrame Frame = View.now;
		ftcore::fillArenaHudLead(Fight, Frame.lead);
		ftcore::fillArenaHudTarget(Fight, Frame.target);
		FillBoss(*Runner, Frame.boss);
		Frame.partner = ftcore::ArenaHudPartner{};
		ftcore::fillArenaHudPartner(Fight, Frame.partner); // the carrying runs' ally; zero (absent) otherwise
		Frame.third = ftcore::ArenaHudThird{};
		const uint32 Changed = ftcore::updateArenaHudView(View, Frame);
		const ftcore::ArenaHudLead& L = View.now.lead;
		if (bRebuildNow || (Changed & SectionBit(ftcore::ArenaHudSection::Lead)))
		{
			HpText = FString::Printf(TEXT("%d"), L.hp);
			HpTextW = TextWidth(HpText, ValueFont);
			LoadText = FString::Printf(TEXT("%d"), L.load);
		}
		TakeHalfAge = L.tookHalf ? 0.f : TakeHalfAge + DeltaTime;
		HpLost.Step(L.hpMax > 0 ? static_cast<float>(L.hp) / L.hpMax : 0.f, DeltaTime);
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		// The name over the line: the boss's, else the target's, from the loc table by its arena kind (none: the TARGET word).
		const int32 WantSlot = B.present ? static_cast<int32>(B.slot) : (T.present ? static_cast<int32>(T.enemy) : -1);
		if (WantSlot != NameSlot)
		{
			NameSlot = WantSlot;
			bNamed = false;
			NameText.Reset();
			if (WantSlot >= 0)
			{
				const FString Key = FString(TEXT("arena.name.")) + UTF8_TO_TCHAR(Runner->GetSlotKindId(WantSlot));
				const FString& Name = FTUiText::Lookup(*Key);
				if (Name != Key)
				{
					NameText = Name;
					NameW = TextWidth(NameText, NameFont);
					bNamed = true;
				}
			}
		}
		// The latest answer note's word, built when a new one comes.
		FightNow = Fight.time();
		if (B.noteSerial != NoteSeen)
		{
			NoteSeen = B.noteSerial;
			NoteWord.Reset();
			const FFTStoneBearerHost* Bearer = Runner->GetStoneBearer();
			if (Bearer && B.noteSerial >= 0)
			{
				const FFTStoneBearerNote& Note = Bearer->GetNoteBySerial(B.noteSerial);
				if (const FTUiText* W = BossNoteWord(Note, NoteTok))
				{
					NoteWord = W->Get();
					NoteW = TextWidth(NoteWord, LabelFont);
					NoteAt = Note.Event.at;
				}
			}
		}
		const int32 TopHp = B.present ? B.hp : T.hp;
		const int32 TopMax = B.present ? B.hpMax : T.hpMax;
		TopLost.Step(TopMax > 0 ? static_cast<float>(TopHp) / TopMax : 0.f, DeltaTime);

		// The verbs: their keys on the device in use (built when the device changes), which can be used now, which is lit.
		const APlayerController* PC = Runner->GetWorld() ? Runner->GetWorld()->GetFirstPlayerController() : nullptr;
		const ftcore::InputDevice Device = FTInputBindings::NoteDevice(PC);
		if (bRebuildNow || Device != VerbDevice)
		{
			VerbDevice = Device;
			const ftcore::InputHintEntry* Entries = nullptr;
			const size_t Count = ftcore::inputHintEntries(ftcore::InputHintScreen::Arena, Entries);
			for (FVerbSlot& V : Verbs)
			{
				V.Keys.Reset();
				for (size_t I = 0; I < Count; ++I)
				{
					if (FCStringAnsi::Strcmp(Entries[I].label, V.Label) == 0)
					{
						V.Keys = FT::ToFString(ftcore::inputHintKeys(FTInputBindings::Get(), Entries[I], Device)); // as bound: Ctrl, R
						break;
					}
				}
				V.KeysW = TextWidth(V.Keys, LabelFont);
			}
		}
		uint32 VerbState = static_cast<uint32>(Device) << 8;
		VerbState |= (!L.guardBroken && !L.down) ? 1u : 0u;
		VerbState |= L.releaseReady ? 2u : 0u;
		VerbState |= View.now.partner.present ? 4u : 0u;
		VerbState |= (L.heals > 0 && !L.down) ? 8u : 0u;
		const bool bVerbChanged = VerbState != LastVerbState;
		LastVerbState = VerbState;

		const ftcore::UiFadeTimes& Fade = Table().Style.fade;
		ftcore::uiFadeStep(LeadFade, (Changed & SectionBit(ftcore::ArenaHudSection::Lead)) != 0, DeltaTime, Fade);
		ftcore::uiFadeStep(TopFade, (Changed & (SectionBit(ftcore::ArenaHudSection::Target) | SectionBit(ftcore::ArenaHudSection::Boss))) != 0,
			DeltaTime, Fade);
		ftcore::uiFadeStep(VerbFade, bVerbChanged, DeltaTime, Fade);

		TickWorld(*Runner, Fight, PC, Geometry.GetLocalSize());
		TickEvents(*Runner, Fight, PC, Geometry.GetLocalSize());
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
		Mesh.Begin(Geometry, P.S);
		PaintWorld(P, Size);
		PaintLead(P, Size);
		PaintTop(P, Size);
		PaintVerbs(P, Size);
		PaintRounds(P, Size);
		PaintEvents(P, Size);
		Mesh.Flush(Out, Layer + 1); // every line, mark and shade in one element under the words
		return Layer + 3;
	}

private:
	// One arc of a track round the lead's ring (from01..to01 of the A0..A1 sweep).
	void Arc(float CX, float CY, float R, float From01, float To01, const ftcore::UiStrokeStyle& St, const FLinearColor& C, bool bEdge = true) const
	{
		ftcore::uiStrokeArc(Line, CX, CY, R, Carry::A0, Carry::A1, St, From01, To01);
		Mesh.Stroke(Line, C, bEdge);
	}

	// A whole ring, joined without a seam.
	void Ring(float CX, float CY, float R, float Width, std::uint32_t Seed, float S, const FLinearColor& C) const
	{
		ftcore::uiStrokeArc(Line, CX, CY, R, 0.f, 360.f, StrokeStyle(Width, Seed, S, 0.f));
		if (Line.count > 1)
		{
			Line.pt[Line.count - 1].x = Line.pt[0].x;
			Line.pt[Line.count - 1].y = Line.pt[0].y;
		}
		Mesh.Stroke(Line, C, true, true);
	}

	void HLine(float X0, float X1, float Y, float From01, float To01, const ftcore::UiStrokeStyle& St, const FLinearColor& C, bool bEdge = true) const
	{
		ftcore::uiStrokeLine(Line, X0, Y, X1, Y, St, From01, To01);
		Mesh.Stroke(Line, C, bEdge);
	}

	void Poly(const FVector2f* Pts, int32 N, float HalfWidth, const FLinearColor& C, bool bClosed = false) const
	{
		PolyStroke(Line, Pts, N, HalfWidth);
		Mesh.Stroke(Line, C, true, bClosed);
	}

	// A small V pointing down, turned clockwise by Deg (on an arc at angle Deg it points at the centre; -90 points right).
	void Chevron(float X, float Y, float Size, float Deg, float S, const FLinearColor& C) const
	{
		const FVector2f Pts[3] = {FVector2f(X, Y) + Rotated(-Size * 0.55f, -Size * 0.45f, Deg), FVector2f(X, Y) + Rotated(0.f, Size * 0.2f, Deg),
			FVector2f(X, Y) + Rotated(Size * 0.55f, -Size * 0.45f, Deg)};
		Poly(Pts, 3, 0.8f * S, C);
	}

	void Danger(float CX, float CY, float Size, const FLinearColor& C) const
	{
		const FVector2f Pts[3] = {FVector2f(CX, CY - Size * 0.55f), FVector2f(CX + Size * 0.55f, CY + Size * 0.45f), FVector2f(CX - Size * 0.55f, CY + Size * 0.45f)};
		Mesh.Fill(Pts, 3, C);
	}

	void Lozenge(float CX, float CY, float R, bool bFilled, float S, const FLinearColor& C) const
	{
		const FVector2f Pts[5] = {FVector2f(CX, CY - R), FVector2f(CX + R, CY), FVector2f(CX, CY + R), FVector2f(CX - R, CY), FVector2f(CX, CY - R)};
		if (bFilled)
		{
			Mesh.Fill(Pts, 4, C);
		}
		else
		{
			Poly(Pts, 5, 0.6f * S, C, true);
		}
	}

	// The lead, bottom left: the ring as a table, HP as the hand-drawn line round it (what was just lost rubbed out to a
	// faint trace), stamina a hairline inside, Load the accent line outside with the Release mark at its place; the
	// numbers and heals in the open bottom; one state word at the ring's shoulder.
	void PaintLead(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::ArenaHudLead& L = View.now.lead;
		const ftcore::UiStyle& St = Table().Style;
		const float S = P.S;
		const float A = LeadFade.alpha;
		const float TA = TextAlpha(A);
		const float CX = Carry::LeadX * S;
		const float CY = Size.Y - Carry::LeadUp * S;
		Mesh.Shade(CX + 10.f * S, CY - 40.f * S, 230.f * S, 230.f * S, St.backingSoftHud * FMath::Max(A, 0.6f));

		Ring(CX, CY, Carry::RingR * S, 2.1f, 11, S, Tok(UiToken::Ink, 0.9f * A));

		const float Hp = L.hpMax > 0 ? FMath::Clamp(static_cast<float>(L.hp) / L.hpMax, 0.f, 1.f) : 0.f;
		const bool bLow = Hp <= Carry::LowHp;
		const FLinearColor HpC = bLow ? Tok(UiToken::Danger, A) : Tok(UiToken::Ink, A);
		Arc(CX, CY, Carry::HpR * S, 0.f, Hp, StrokeStyle(5.6f, 21, S), HpC);
		if (HpLost.Was > Hp + 0.004f)
		{
			ftcore::UiStrokeStyle Rub = StrokeStyle(1.6f, 21, S, 0.f);
			Rub.broken = true;
			Arc(CX, CY, Carry::HpR * S, Hp, HpLost.Was, Rub, Tok(UiToken::Ink, 0.22f * A), false);
		}
		Arc(CX, CY, Carry::StaminaR * S, 0.f, FMath::Clamp(L.stamina01, 0.f, 1.f), StrokeStyle(1.6f, 23, S), Tok(UiToken::InkDim, 0.8f * A), false);

		if (L.loadMax > 0)
		{
			const float Load = FMath::Clamp(static_cast<float>(L.load) / L.loadMax, 0.f, 1.f);
			// The track runs to the cap (the named chairs' limit); past it the arc is not drawn, and a short cross-stroke
			// closes it.
			const float Cap = L.loadCap > 0 ? FMath::Clamp(static_cast<float>(L.loadCap) / L.loadMax, 0.f, 1.f) : 1.f;
			Arc(CX, CY, Carry::LoadR * S, 0.f, Cap, StrokeStyle(1.f, 31, S, 0.f), Tok(UiToken::Ink, 0.28f * A), false);
			if (Cap < 0.999f)
			{
				const float CapDeg = Carry::A0 + (Carry::A1 - Carry::A0) * Cap;
				float X0 = 0.f;
				float Y0 = 0.f;
				float X1 = 0.f;
				float Y1 = 0.f;
				ftcore::uiArcPoint(CX, CY, (Carry::LoadR - 6.f) * S, CapDeg, X0, Y0);
				ftcore::uiArcPoint(CX, CY, (Carry::LoadR + 6.f) * S, CapDeg, X1, Y1);
				const FVector2f Pts[2] = {FVector2f(X0, Y0), FVector2f(X1, Y1)};
				Poly(Pts, 2, 0.9f * S, Tok(UiToken::Ink, 0.8f * A));
			}
			Arc(CX, CY, Carry::LoadR * S, 0.f, Load, StrokeStyle(4.2f, 33, S), L.overloaded ? Tok(UiToken::Danger, A) : Tok(UiToken::Accent, A));
			const int32 ReleaseAt = L.releaseAt > 0 ? L.releaseAt : ftcore::kArenaReleaseOpenLoad;
			const float Mark = FMath::Clamp(static_cast<float>(ReleaseAt) / L.loadMax, 0.f, 1.f);
			const float Deg = Carry::A0 + (Carry::A1 - Carry::A0) * Mark;
			float MX = 0.f;
			float MY = 0.f;
			ftcore::uiArcPoint(CX, CY, (Carry::LoadR + 11.f) * S, Deg, MX, MY);
			Chevron(MX, MY, 10.f * S, Deg, S, Tok(UiToken::Ink, A));
			P.Text(CX + 30.f * S, CY + 84.f * S - ValueAsc, LoadText, ValueFont, L.overloaded ? Tok(UiToken::Danger, TA) : Tok(UiToken::Accent, TA));
		}

		// The chairs at the table: one small filled mark per named chair, in a row inside the ring's crown.
		for (int32 I = 0; I < L.chairCount; ++I)
		{
			const float X = CX + (static_cast<float>(I) - 0.5f * (L.chairCount - 1)) * 16.f * S;
			Mesh.Dot(X, CY - 26.f * S, 4.6f * S, Tok(UiToken::Ink, A));
		}

		// The numbers in the open bottom: HP to the left of the middle (a danger mark beside it when low), Load to the right.
		P.Text(CX - 30.f * S - HpTextW, CY + 84.f * S - ValueAsc, HpText, ValueFont, bLow ? Tok(UiToken::Danger, TA) : Tok(UiToken::Ink, TA));
		if (bLow)
		{
			Danger(CX - 30.f * S - HpTextW - 14.f * S, CY + 84.f * S - ValueAsc * 0.45f, 13.f * S, Tok(UiToken::Danger, A));
		}
		// Heals: lozenges between the numbers, filled while held. Read: upright marks under them, full height while held.
		for (int32 I = 0; I < L.healsMax; ++I)
		{
			const float X = CX + (static_cast<float>(I) - 0.5f * (L.healsMax - 1)) * 13.f * S;
			Lozenge(X, CY + 76.f * S, 4.5f * S, I < L.heals, S, Tok(UiToken::Ink, (I < L.heals ? 1.f : 0.6f) * A));
		}
		for (int32 I = 0; I < L.readMax; ++I)
		{
			const float X = CX + (static_cast<float>(I) - 0.5f * (L.readMax - 1)) * 9.f * S;
			const bool bHeld = I < L.read;
			const float H = (bHeld ? 12.f : 5.f) * S;
			const FVector2f Pts[2] = {FVector2f(X, CY + 104.f * S - H), FVector2f(X, CY + 104.f * S)};
			Poly(Pts, 2, 1.4f * S, bHeld ? Tok(UiToken::Accent, A) : Tok(UiToken::InkDim, 0.6f * A));
		}

		// One state at a time at the ring's shoulder, the most urgent first, each with its shape.
		const float WX = CX + 102.f * S;
		const float WY = CY - 40.f * S; // the word's baseline
		const FString* Word = nullptr;
		bool bDanger = true;
		if (L.down)
		{
			Word = &DownWord;
		}
		else if (L.overloaded)
		{
			Word = &OverWord;
		}
		else if (L.guardBroken)
		{
			Word = &BrokenWord;
		}
		else if (L.releaseReady)
		{
			Word = &ReadyWord;
			bDanger = false;
		}
		if (Word)
		{
			const FLinearColor C = bDanger ? Tok(UiToken::Danger, A) : Tok(UiToken::Accent, A);
			if (bDanger)
			{
				Danger(WX + 8.f * S, WY - VerbAsc * 0.38f, 14.f * S, C);
			}
			else
			{
				Chevron(WX + 8.f * S, WY - VerbAsc * 0.38f, 12.f * S, -90.f, S, C);
			}
			P.Text(WX + 22.f * S, WY - VerbAsc, *Word, VerbFont, C);
		}

		// The party places stacked above the lead: the partner, then the third place, always empty (the only dashed ring).
		const ftcore::ArenaHudPartner& Pa = View.now.partner;
		if (Pa.present)
		{
			const float PX = CX - 6.f * S;
			const float PY = CY - 140.f * S;
			Ring(PX, PY, 20.f * S, 1.4f, 61, S, Tok(UiToken::Ink, A));
			const float F = Pa.hpMax > 0 ? FMath::Clamp(static_cast<float>(Pa.hp) / Pa.hpMax, 0.f, 1.f) : 0.f;
			Arc(PX, PY, 27.f * S, 0.f, F, StrokeStyle(2.2f, 62, S), Pa.down ? Tok(UiToken::Danger, A) : Tok(UiToken::Ink, A));
		}
		if (View.now.third.present)
		{
			const float TX = CX - 6.f * S;
			const float TY = CY - 208.f * S;
			for (float Deg = 0.f; Deg < 360.f; Deg += 30.f)
			{
				ftcore::uiStrokeArc(Line, TX, TY, 20.f * S, Deg, Deg + 14.f, StrokeStyle(1.3f, 71, S, 0.f));
				Mesh.Stroke(Line, Tok(UiToken::Ink, 0.75f * A));
			}
		}
	}

	// The top: the boss as one fine line with its HP drawn over it and phase notches; a plain target as a shorter line
	// under its word. A second target while a boss fights: a short line below.
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
		const float A = TopFade.alpha;
		const float TA = TextAlpha(A);
		const float CX = Size.X * 0.5f;
		const float Y = Carry::TopLine * S;
		const float Len = (B.present ? Carry::BossLen : Carry::TargetLen) * S;
		const float X0 = CX - Len * 0.5f;
		const float Hp = (B.present ? B.hpMax : T.hpMax) > 0
			? FMath::Clamp(static_cast<float>(B.present ? B.hp : T.hp) / (B.present ? B.hpMax : T.hpMax), 0.f, 1.f)
			: 0.f;
		Mesh.Shade(CX, Y - 8.f * S, Len * 0.5f + 180.f * S, 150.f * S, St.backingSoftHud * FMath::Max(A, 0.6f));

		// The track: a hairline past both ends, fading out over its last 80 px.
		ftcore::UiStrokeStyle Hair = StrokeStyle(1.f, 99, S, 80.f / (Carry::BossLen + 80.f));
		Hair.endAlpha = 0.f;
		Hair.endWidth = 0.6f;
		Hair.wobble = 0.3f * S;
		HLine(X0 - 40.f * S, X0 + Len + 40.f * S, Y, 0.f, 1.f, Hair, Tok(UiToken::Ink, 0.5f * A));
		ftcore::UiStrokeStyle HpSt = StrokeStyle(B.present ? 3.6f : 3.f, 101, S, 0.04f);
		HLine(X0, X0 + Len, Y, 0.f, Hp, HpSt, Tok(UiToken::Ink, A));
		if (TopLost.Was > Hp + 0.004f)
		{
			ftcore::UiStrokeStyle Rub = StrokeStyle(1.6f, 101, S, 0.f);
			Rub.broken = true;
			HLine(X0, X0 + Len, Y, Hp, TopLost.Was, Rub, Tok(UiToken::Ink, 0.22f * A), false);
		}
		// The name over the line in the title small caps, the boss's rank as short strokes after it; else the TARGET word.
		if (bNamed)
		{
			const float Base = Y - 22.f * S;
			P.Text(CX - NameW * 0.5f, Base - NameAsc, NameText, NameFont, Tok(UiToken::Ink, TA));
			for (int32 I = 0; B.present && I < B.rank; ++I)
			{
				const float X = CX + NameW * 0.5f + 18.f * S + I * 8.f * S;
				const FVector2f Pts[2] = {FVector2f(X, Base - 20.f * S), FVector2f(X, Base - 2.f * S)};
				Poly(Pts, 2, 0.9f * S, Tok(UiToken::Ink, A));
			}
		}
		else if (!B.present)
		{
			P.Text(CX - TargetW * 0.5f, Y - 26.f * S - LabelAsc, TargetWord, LabelFont, Tok(UiToken::InkDim, TA));
		}
		// A notch at each phase's floor; the phases already passed step back.
		for (int32 I = 0; B.present && I + 1 < B.phaseCount; ++I)
		{
			const float X = X0 + Len * B.phaseEnd[static_cast<size_t>(I)];
			const FVector2f Pts[2] = {FVector2f(X, Y - 9.f * S), FVector2f(X, Y + 9.f * S)};
			Poly(Pts, 2, 0.8f * S, Tok(I < B.phase ? UiToken::InkDim : UiToken::Ink, (I < B.phase ? 0.5f : 1.f) * A));
		}
		// The target's poise under the line (the boss's own when the target is the boss).
		if (T.present && (!B.present || T.enemy == B.slot))
		{
			HLine(X0, X0 + Len, Y + 12.f * S, 0.f, FMath::Clamp(T.poise01, 0.f, 1.f), StrokeStyle(1.4f, 103, S), Tok(UiToken::InkDim, A), false);
		}
		float LY = Y + 28.f * S; // the next row's top
		if (B.present && B.weightMax > 0)
		{
			P.Text(X0, LY, WeightWord, LabelFont, Tok(UiToken::InkDim, TA));
			for (int32 I = 0; I < B.weightMax; ++I)
			{
				const float X = X0 + WeightW + 14.f * S + I * 9.f * S;
				const bool bHeld = I < B.weight;
				const float H = (bHeld ? 14.f : 5.f) * S;
				const FVector2f Pts[2] = {FVector2f(X, LY + LabelAsc - H), FVector2f(X, LY + LabelAsc)};
				Poly(Pts, 2, 1.4f * S, bHeld ? Tok(UiToken::Accent, A) : Tok(UiToken::InkDim, 0.6f * A));
			}
		}
		const float WY = Y + 54.f * S - LabelAsc * 0.5f; // the cue row, centred under the line
		if (B.present && B.weakPoint == ftcore::UiWeakPoint::Open)
		{
			const FLinearColor C = Tok(UiToken::WeakPoint, 0.85f + 0.15f * FMath::Sin(Time * 6.f));
			Lozenge(CX - WeakW * 0.5f - 14.f * S, WY + LabelAsc * 0.55f, 6.f * S, true, S, C);
			P.Text(CX - WeakW * 0.5f, WY, WeakWord, LabelFont, C);
		}
		else if (B.present && B.invulnerable)
		{
			// Shut: a square, the word, and how to open it (the Release on the call).
			const float Total = InvulW + 20.f * S + RocW;
			const float TX = CX - Total * 0.5f;
			const float BX = TX - 14.f * S;
			const float BY = WY + LabelAsc * 0.55f;
			const float R = 5.f * S;
			const FVector2f Pts[5] = {FVector2f(BX - R, BY - R), FVector2f(BX + R, BY - R), FVector2f(BX + R, BY + R), FVector2f(BX - R, BY + R),
				FVector2f(BX - R, BY - R)};
			Poly(Pts, 5, 0.7f * S, Tok(UiToken::Ink, A), true);
			P.Text(TX, WY, InvulWord, LabelFont, Tok(UiToken::Ink, TA));
			P.Text(TX + InvulW + 20.f * S, WY, RocWord, LabelFont, Tok(UiToken::InkDim, TA));
		}
		else if (B.present && B.down)
		{
			Lozenge(CX - BossDownW * 0.5f - 14.f * S, WY + LabelAsc * 0.55f, 6.f * S, false, S, Tok(UiToken::Ink, A));
			P.Text(CX - BossDownW * 0.5f, WY, BossDownWord, LabelFont, Tok(UiToken::Ink, TA));
		}
		else if (B.present && B.call != ftcore::UiCalledZone::None)
		{
			// The called zone: CALL and the zone; standing in it, a filled ring in the ally colour, else a danger triangle.
			const int32 Zi = FMath::Clamp(static_cast<int32>(B.call) - 1, 0, 2);
			const float Total = CallW + 12.f * S + ZoneW[Zi];
			const float TX = CX - Total * 0.5f;
			const float MY = WY + LabelAsc * 0.55f;
			const FLinearColor ZoneC = Tok(B.callIn ? UiToken::Ally : UiToken::Danger, TA);
			if (B.callIn)
			{
				P.Circle(TX - 15.f * S, MY, 11.f * S, ZoneC);
			}
			else
			{
				Danger(TX - 15.f * S, MY, 13.f * S, ZoneC);
			}
			P.Text(TX, WY, CallWord, LabelFont, Tok(UiToken::InkDim, TA));
			P.Text(TX + CallW + 12.f * S, WY, ZoneWord[Zi], LabelFont, ZoneC);
		}
		// The latest answer or rule at the line's right end, fading.
		const float NoteA = NoteWord.IsEmpty() ? 0.f : FMath::Clamp(1.f - static_cast<float>((FightNow - NoteAt) / kBossNoteLife), 0.f, 1.f);
		if (B.present && NoteA > 0.f)
		{
			P.Text(X0 + Len - NoteW, WY, NoteWord, LabelFont, Tok(NoteTok, NoteA));
		}
		if (B.present && T.present && T.enemy != B.slot)
		{
			const float OY = Y + 96.f * S;
			const float OLen = 300.f * S;
			const float OX = CX - OLen * 0.5f;
			const float OHp = T.hpMax > 0 ? FMath::Clamp(static_cast<float>(T.hp) / T.hpMax, 0.f, 1.f) : 0.f;
			P.Text(OX - TargetW - 16.f * S, OY - LabelAsc * 0.6f, TargetWord, LabelFont, Tok(UiToken::InkDim, TA));
			HLine(OX, OX + OLen, OY, 0.f, 1.f, StrokeStyle(1.f, 105, S, 0.f), Tok(UiToken::Ink, 0.3f * A), false);
			HLine(OX, OX + OLen, OY, 0.f, OHp, StrokeStyle(2.4f, 106, S, 0.04f), Tok(UiToken::Ink, A));
			HLine(OX, OX + OLen, OY + 9.f * S, 0.f, FMath::Clamp(T.poise01, 0.f, 1.f), StrokeStyle(1.2f, 107, S), Tok(UiToken::InkDim, A), false);
		}
	}

	// The verbs bottom right: an unboxed list on a hairline rule, each with its key in a thin ring; the one the fight
	// asks for now lit in the accent with a mark at the rule. A verb that cannot be used now steps back.
	void PaintVerbs(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::ArenaHudLead& L = View.now.lead;
		const ftcore::UiStyle& St = Table().Style;
		const float S = P.S;
		const float A = VerbFade.alpha;
		const float RX = Size.X - Carry::VerbIn * S;
		const float Y0 = Size.Y - Carry::VerbUp * S;
		Mesh.Shade(RX - 110.f * S, Y0 + 60.f * S, 250.f * S, 170.f * S, St.backingSoftHud * FMath::Max(A, 0.6f));
		ftcore::UiStrokeStyle Rule = StrokeStyle(1.f, 121, S, 0.2f);
		Rule.endAlpha = 0.f;
		ftcore::uiStrokeLine(Line, RX, Y0 - 50.f * S, RX, Y0 + (3.f * Carry::VerbStep + 34.f) * S, Rule);
		Mesh.Stroke(Line, Tok(UiToken::Ink, 0.75f * A));
		Lozenge(RX, Y0 - 44.f * S, 3.5f * S, true, S, Tok(UiToken::Ink, 0.8f * A));

		const bool Avail[4] = {!L.guardBroken && !L.down, L.releaseReady, View.now.partner.present, L.heals > 0 && !L.down};
		const int32 Hot = L.releaseReady ? 1 : -1;
		for (int32 I = 0; I < 4; ++I)
		{
			const FVerbSlot& V = Verbs[I];
			if (V.Keys.IsEmpty())
			{
				continue;
			}
			const float Y = Y0 + I * Carry::VerbStep * S; // the word's baseline
			const float VA = FMath::Max(A, 0.6f) * (Avail[I] ? 1.f : 0.5f); // a reference list: its marks step back, never out
			const FLinearColor C = I == Hot ? Tok(UiToken::Accent, VA) : Tok(UiToken::Ink, VA);
			// The words keep full strength at rest (they must read on bright sky); an unavailable verb is the dim ink.
			const float WA = TextAlpha(A);
			const FLinearColor WC = I == Hot ? Tok(UiToken::Accent, WA) : Avail[I] ? Tok(UiToken::Ink, WA) : Tok(UiToken::InkDim, WA);
			const float KX = RX - 44.f * S;
			const float KY = Y - 7.f * S;
			const float KW = V.Keys.Len() <= 1 ? 30.f * S : FMath::Max(30.f * S, V.KeysW + 26.f * S);
			KeyCapStroke(Line, KX, KY, KW, 30.f * S, 0.75f * S, 6.f * S);
			Mesh.Stroke(Line, C, true, true);
			if (I == 0 && TakeHalfAge < 0.6f)
			{
				// Took half: a fine accent ring opens out from the Guard key and fades.
				const float T = TakeHalfAge / 0.6f;
				KeyCapStroke(Line, KX, KY, KW + 16.f * T * S, (30.f + 16.f * T) * S, 0.8f * S, (6.f + 8.f * T) * S);
				Mesh.Stroke(Line, Tok(UiToken::Accent, (1.f - T) * FMath::Max(A, 0.6f)), true, true);
			}
			// Centred on the cap height (the serif's capitals are 0.62 of its ascent), not on the line box.
			P.Text(KX - V.KeysW * 0.5f, KY - LabelAsc * 0.69f, V.Keys, LabelFont, WC);
			P.Text(KX - KW * 0.5f - 18.f * S - V.WordW, Y - VerbAsc, V.Word, VerbFont, WC);
			if (I == Hot)
			{
				// Points out past the rule, as on the sheet.
				const FVector2f Pts[3] = {FVector2f(RX + 11.f * S, Y - 16.f * S), FVector2f(RX + 19.f * S, Y - 8.f * S), FVector2f(RX + 11.f * S, Y)};
				Poly(Pts, 3, 0.9f * S, Tok(UiToken::Accent, A));
			}
		}
	}

	// Text at the rest alpha keeps full strength (a faded word on bright sky reads as a hollow outline); below the rest
	// alpha (the HUD going away) it fades with the rest.
	static float TextAlpha(float A)
	{
		const float Rest = Table().Style.fade.restAlpha;
		return Rest > 0.01f ? FMath::Min(1.f, A / Rest) : A;
	}

	// A world point on this widget (false: behind the camera or no view).
	static bool Project(const APlayerController* PC, const FVector& World, const FVector2f& Size, FVector2f& Out)
	{
		int32 VX = 0;
		int32 VY = 0;
		if (!PC)
		{
			return false;
		}
		PC->GetViewportSize(VX, VY);
		FVector2D Px;
		if (VX <= 0 || VY <= 0 || !PC->ProjectWorldLocationToScreen(World, Px, true))
		{
			return false;
		}
		Out = FVector2f(static_cast<float>(Px.X) * Size.X / VX, static_cast<float>(Px.Y) * Size.Y / VY);
		return true;
	}

	// The world's marks this frame: each enemy's thin hp line and state word, its tell (on the body, or at the screen's edge
	// when off frame), the Read cards and the lock. Fixed arrays, no allocation.
	void TickWorld(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, const APlayerController* PC, const FVector2f& Size)
	{
		const auto OnScreen = [&Size](const FVector2f& Q) { return Q.X >= 0.f && Q.Y >= 0.f && Q.X <= Size.X && Q.Y <= Size.Y; };
		const double Now = Runner.GetNow();
		const ftcore::Settings* Settings = Runner.GetSettings();
		const bool bCalm = Settings && Settings->reduceFlashes;
		const ftcore::ReadCards& Cards = Fight.readCards();
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		bCardsLoose = false;
		bCardsShown = false;
		for (int32 I = 0; I < UFTArenaRunner::MaxEnemies; ++I)
		{
			FFoeMark& M = Foes[I];
			M = FFoeMark{};
			const AFTArenaFighter* Figure = Runner.GetEnemyFigure(I);
			if (I >= static_cast<int32>(Fight.enemyCount()) || !Figure || !Fight.enemyAlive(static_cast<size_t>(I)))
			{
				continue;
			}
			const size_t Ix = static_cast<size_t>(I);
			M.bShow = true;
			const bool bOver = Project(PC, Figure->GetAnchor(), Size, M.Over);
			const bool bChest = Project(PC, Figure->GetChest(), Size, M.Chest);
			M.bOn = bOver && bChest && OnScreen(M.Chest);
			// The top line carries the target's and the boss's hp: over those only the words and the tell.
			M.bLine = !(B.present && B.slot == I) && !(T.present && T.enemy == I);
			const ftcore::ArenaEnemyKind& Kind = Fight.def().enemies[Ix];
			M.Hp01 = Kind.hp > 0 ? FMath::Clamp(static_cast<float>(Fight.enemyHp(Ix)) / Kind.hp, 0.f, 1.f) : 0.f;
			M.Poise01 = Kind.poise > 0.f ? FMath::Clamp(Fight.enemyPoise(Ix) / Kind.poise, 0.f, 1.f) : 0.f;
			const ftcore::ArenaEnemyOut& E = Fight.output().enemies[Ix];
			M.Word = E.brain == ftcore::ArenaBrain::Staggered ? 1 : (E.brain == ftcore::ArenaBrain::Open ? 2 : 0);
			const ftcore::ArenaEnemyMove* Move = Runner.GetEnemyMove(I);
			if (!Move)
			{
				continue;
			}
			M.Answer = Move->unblockable ? 2 : (!Move->parryable ? 1 : 0);
			M.Impact = Pulse(Now, Runner.GetEnemyActiveSince(I), kImpactLife);
			const bool bStartup = E.part == ftcore::ArenaPart::Startup;
			if (!bStartup && M.Impact <= 0.f)
			{
				continue;
			}
			M.Tell = bStartup ? 1 : 2;
			M.Fill = bStartup ? FMath::Clamp(E.stateTime / FMath::Max(Move->attack.startup, 0.01f), 0.f, 1.f) : 1.f;
			M.bCards = bStartup && Fight.readFoe() == static_cast<uint8>(I) && Cards.dealt();
			bCardsShown |= M.bCards;
			// Out of the camera's view (the runner's off-frame test, the one the stinger answers to), or off the screen.
			const bool bOffFrame = bStartup && Runner.IsOffFrame(I);
			if (!M.bOn || bOffFrame)
			{
				M.bEdge = true;
				bCardsLoose |= M.bCards;
				M.Onset = (bOffFrame && !bCalm) ? Pulse(Now, Runner.GetOffFrameSince(I), kOnsetLife) : 0.f;
				if (PC && PC->PlayerCameraManager)
				{
					const FVector Local = PC->PlayerCameraManager->GetCameraRotation().UnrotateVector(
						Figure->GetChest() - PC->PlayerCameraManager->GetCameraLocation());
					M.Dir = FVector2f(static_cast<float>(Local.Y), static_cast<float>(-Local.Z));
				}
				if (M.Dir.SizeSquared() < 1e-6f)
				{
					M.Dir = FVector2f(0.f, 1.f);
				}
				M.Dir.Normalize();
			}
		}
		// The Read cards: the shapes dealt and how the pick went (0 live, 1 right, 2 wrong, 3 passed over).
		CardCount = 0;
		if (bCardsShown)
		{
			const bool bPicked = Cards.picked();
			CardCount = FMath::Min(static_cast<int32>(Cards.size()), static_cast<int32>(ftcore::kReadCards));
			for (int32 I = 0; I < CardCount; ++I)
			{
				const ftcore::ReadCard& Card = Cards[static_cast<size_t>(I)];
				uint8 Tell = 0;
				if (Card.actor < Fight.enemyCount())
				{
					const ftcore::ArenaEnemyKind& Kind = Fight.def().enemies[Card.actor];
					if (Card.action < Kind.moveCount)
					{
						Tell = static_cast<uint8>(Kind.moves[Card.action].tell);
					}
				}
				CardTell[I] = Tell < ftcore::kArenaTellCount ? Tell : 0;
				const bool bThis = bPicked && Cards.pickedIndex() == static_cast<size_t>(I);
				CardState[I] = !bPicked ? 0 : (!bThis ? 3 : (Cards.correct() == static_cast<size_t>(I) ? 1 : 2));
			}
		}
		// The lock: on the locked body while it is on the screen.
		bLockOn = false;
		const uint8 Lock = Fight.lock();
		if (Lock != ftcore::kArenaNobody)
		{
			const AFTArenaFighter* Figure = Runner.GetEnemyFigure(static_cast<int32>(Lock));
			bLockOn = Figure && Project(PC, Figure->GetChest(), Size, LockAt) && OnScreen(LockAt);
		}
	}

	// The fight's events this frame: the popups over bodies (their numbers built once each), the edge hairlines of a hit
	// or a parry, the Release and overload words, the result and the autoplay tag.
	void TickEvents(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, const APlayerController* PC, const FVector2f& Size)
	{
		const double Now = Runner.GetNow();
		const float S = FMath::Max(0.25f, Size.Y / 1080.f);
		for (FPopMark& M : Pops)
		{
			M.bShow = false;
		}
		const int32 Count = Runner.GetPopupCount();
		const int32 First = Runner.GetPopupTotal() - Count;
		for (int32 I = 0; I < Count; ++I)
		{
			const UFTArenaRunner::FPopup& Pp = Runner.GetPopup(I);
			const int32 Serial = First + I;
			FPopMark& M = Pops[Serial % UFTArenaRunner::MaxPopups];
			const float Life = Pulse(Now, Pp.Time, kPopupLife);
			FVector2f At;
			if (Life <= 0.f || !Project(PC, Pp.At + FVector(0.0, 0.0, 210.0), Size, At))
			{
				continue;
			}
			const bool bNew = M.Serial != Serial || M.Amount != Pp.Amount;
			M.Serial = Serial;
			M.Amount = Pp.Amount;
			M.bShow = true;
			M.A = FMath::Min(1.f, Life * 2.f);
			const float Y = At.Y - 40.f * S - (1.f - Life) * 70.f * S;
			const bool bOnLead = Pp.Target == ftcore::kArenaPlayer;
			// Numbers stand beside the body (right of an enemy, left of the lead), clear of its line and its tell.
			M.NumAt = FVector2f(At.X + (bOnLead ? -110.f : 110.f) * S, Y + 40.f * S);
			M.WordAt = FVector2f(At.X, Y - 30.f * S);
			M.Word = -1;
			M.bNumPop = false;
			int32 Number = MIN_int32; // the number to build (none: MIN_int32)
			const TCHAR* Before = nullptr;
			const FString* After = nullptr;
			switch (Pp.Type)
			{
			case ftcore::ArenaEventType::Hit:
			{
				const bool bRiposte = Pp.Source == ftcore::kArenaPlayer && Pp.Move == ftcore::kArenaRiposteMove;
				M.Tok = bOnLead ? UiToken::Danger : (bRiposte ? UiToken::Accent : UiToken::Ink);
				Number = Pp.Amount;
				break;
			}
			case ftcore::ArenaEventType::Healed:
				M.Tok = UiToken::Ally;
				Number = Pp.Amount;
				Before = TEXT("+");
				break;
			case ftcore::ArenaEventType::Blocked:
				M.Tok = UiToken::Ink;
				M.Word = PwBlocked;
				break;
			case ftcore::ArenaEventType::GuardBroken:
				M.Tok = UiToken::Danger;
				M.Word = PwGuardBroken;
				M.WordAt.Y = Y - 60.f * S;
				break;
			case ftcore::ArenaEventType::Parried:
				M.Tok = UiToken::WeakPoint;
				M.Word = PwParried;
				break;
			case ftcore::ArenaEventType::Evaded:
				M.Tok = UiToken::Ink;
				M.Word = PwEvaded;
				break;
			case ftcore::ArenaEventType::Staggered:
				M.Tok = UiToken::Accent;
				M.Word = PwStaggered;
				M.WordAt.Y = Y - 60.f * S;
				break;
			case ftcore::ArenaEventType::Downed:
				M.Tok = UiToken::Ink;
				M.Word = PwDown;
				M.WordAt.Y = Y - 60.f * S;
				break;
			case ftcore::ArenaEventType::LoadStored:
				// "+N LOAD" beside the lead, in the accent the Load line is drawn in.
				M.Tok = UiToken::Accent;
				Number = Pp.Amount;
				Before = TEXT("+");
				After = &kLoadPlusUi.Get();
				M.bNumPop = true;
				M.NumAt = FVector2f(At.X - 150.f * S, Y - 10.f * S);
				break;
			case ftcore::ArenaEventType::CounterStagger:
				M.Tok = UiToken::WeakPoint;
				M.Word = PwCounter;
				M.WordAt.Y = Y - 90.f * S;
				Number = Pp.Amount;
				break;
			case ftcore::ArenaEventType::Read:
				// READ xN beside the attacker.
				M.Tok = UiToken::Ally;
				Number = Pp.Amount;
				M.bNumPop = true;
				M.NumAt = FVector2f(At.X - 190.f * S, Y + 10.f * S);
				break;
			case ftcore::ArenaEventType::Misread:
				M.Tok = UiToken::Danger;
				M.Word = PwMisread;
				M.WordAt = FVector2f(At.X - 190.f * S, Y + 10.f * S);
				break;
			case ftcore::ArenaEventType::ReadCounter:
				M.Tok = UiToken::Ally;
				M.Word = PwReadCounter;
				M.WordAt.Y = Y - 90.f * S;
				Number = Pp.Amount;
				break;
			case ftcore::ArenaEventType::StaminaEmpty:
				M.Tok = UiToken::Danger;
				M.Word = PwNoStamina;
				M.WordAt.Y = Y - 60.f * S;
				break;
			default:
				M.bShow = false;
				break;
			}
			if (bNew)
			{
				M.Num.Reset();
				if (Number != MIN_int32)
				{
					if (Pp.Type == ftcore::ArenaEventType::Read)
					{
						M.Num += kReadUi.Get();
						M.Num += TEXT(" x");
					}
					if (Before)
					{
						M.Num += Before;
					}
					M.Num.AppendInt(Number);
					if (After)
					{
						M.Num += TEXT(" ");
						M.Num += *After;
					}
				}
				M.NumW = TextWidth(M.Num, M.bNumPop ? PopFont : ValueFont);
			}
		}
		HitA = Pulse(Now, Runner.GetPlayerHitTime(), kHitEdgeLife);
		ParryA = Pulse(Now, Runner.GetParryTime(), kParryEdgeLife);
		ReleaseA = Pulse(Now, Runner.GetReleaseTime(), kReleaseBanner);
		if (ReleaseA > 0.f && Runner.GetReleaseSpent() != ReleaseShown)
		{
			ReleaseShown = Runner.GetReleaseSpent();
			ReleaseNum.Reset();
			ReleaseNum.AppendInt(ReleaseShown);
			ReleaseNumW = TextWidth(ReleaseNum, ValueFont);
		}
		OverA = Fight.overloaded() ? Pulse(Now, Runner.GetOverloadTime(), kOverloadBanner) : 0.f;
		bResult = Fight.over();
		if (!bResult)
		{
			bStatsBuilt = false; // the next result gets its own counts
		}
		else
		{
			if (!bStatsBuilt)
			{
				const ftcore::ArenaStats& St = Fight.stats();
				StatsText = FString::Format(*kStatsUi.Get(), {FString::Printf(TEXT("%.1f"), Fight.time()), St.hits, St.parries, St.ripostes,
					St.evades, St.blocks, St.hitsTaken});
				StatsW = TextWidth(StatsText, LabelFont);
				bStatsBuilt = true;
			}
			bWon = Fight.phase() == ftcore::ArenaPhase::Victory;
			ResultA = FMath::Clamp(static_cast<float>(Runner.GetResultAge() / kResultFade), 0.f, 1.f);
			bAgain = !Runner.IsAutoPlay() && Runner.CanFightAgain();
		}
		TagMode = Runner.IsIdle() ? 2 : (Runner.IsAutoPlay() ? 1 : 0);
	}

	// The world's marks, under everything else.
	void PaintWorld(const FPainter& P, const FVector2f& Size) const
	{
		const float S = P.S;
		for (const FFoeMark& M : Foes)
		{
			if (!M.bShow)
			{
				continue;
			}
			if (M.bOn)
			{
				// A thin hp line over a body the top line doesn't carry, the state word under it.
				const float W = 90.f * S;
				if (M.bLine)
				{
					HLine(M.Over.X - W * 0.5f, M.Over.X + W * 0.5f, M.Over.Y, 0.f, 1.f, StrokeStyle(0.6f, 71, S, 0.f), Tok(UiToken::Track), false);
					HLine(M.Over.X - W * 0.5f, M.Over.X + W * 0.5f, M.Over.Y, 0.f, M.Hp01, StrokeStyle(1.1f, 73, S, 0.f), Tok(UiToken::Ink, 0.9f));
					// Its poise, a finer line under the hp in the second accent.
					HLine(M.Over.X - W * 0.5f, M.Over.X + W * 0.5f, M.Over.Y + 5.f * S, 0.f, M.Poise01, StrokeStyle(0.7f, 75, S, 0.f), Tok(UiToken::Accent2, 0.85f));
				}
				if (M.Word != 0)
				{
					const FString& Wd = M.Word == 1 ? StaggerWord : RiposteWord;
					const float WW = M.Word == 1 ? StaggerW : RiposteW;
					P.Text(M.Over.X - WW * 0.5f, M.Over.Y + 12.f * S, Wd, LabelFont, Tok(M.Word == 1 ? UiToken::Accent : UiToken::WeakPoint));
				}
			}
			if (M.Tell == 0)
			{
				continue;
			}
			const UiToken AT = AnswerTok(M.Answer);
			if (M.bEdge)
			{
				PaintEdge(P, Size, M, AT);
				continue;
			}
			// The windup line over the body: it fills through the startup and the hit lands as it reaches the end mark.
			const float W = 170.f * S;
			const float X0 = M.Over.X - W * 0.5f;
			const float Y = M.Over.Y - 30.f * S;
			if (M.Tell == 1)
			{
				HLine(X0, X0 + W, Y, 0.f, 1.f, StrokeStyle(0.7f, 81, S, 0.f), Tok(UiToken::Track), false);
				HLine(X0, X0 + W, Y, 0.f, M.Fill, StrokeStyle(1.8f, 83, S, 0.f), Tok(AT));
				const FVector2f End[2] = {FVector2f(X0 + W, Y - 8.f * S), FVector2f(X0 + W, Y + 8.f * S)};
				Poly(End, 2, 0.9f * S, Tok(UiToken::Ink));
				float Base = Y - 14.f * S;
				if (M.bCards)
				{
					PaintCards(P, M.Over.X, Y - 14.f * S);
					Base -= 62.f * S;
				}
				PaintAnswer(P, M.Over.X, Base, M.Answer, Tok(AT));
				// A ring closes in on the body, meeting it at the hit.
				Ring(M.Chest.X, M.Chest.Y, 38.f * S, 0.6f, 91, S, Tok(AT, 0.45f));
				Ring(M.Chest.X, M.Chest.Y, FMath::Lerp(150.f, 38.f, M.Fill) * S, 1.3f, 93, S, Tok(AT, 0.35f + 0.65f * M.Fill));
			}
			else
			{
				// The hit: the line and NOW in the ink, a ring spreading from the body.
				const float I = M.Impact;
				HLine(X0, X0 + W, Y, 0.f, 1.f, StrokeStyle(1.8f, 83, S, 0.f), Tok(UiToken::Ink, I));
				P.Text(M.Over.X - NowW * 0.5f, Y - 14.f * S - PopAsc, NowWord, PopFont, Tok(UiToken::Ink, I));
				const float Spread = 1.f - I;
				Ring(M.Chest.X, M.Chest.Y, (38.f + 90.f * Spread) * S, 0.6f + 2.4f * I, 95, S, Tok(UiToken::Ink, I));
				Ring(M.Chest.X, M.Chest.Y, (30.f + 50.f * Spread) * S, 1.3f, 97, S, Tok(AT, I));
			}
		}
		if (bCardsLoose)
		{
			PaintCards(P, Size.X * 0.5f, Size.Y - 130.f * S); // the cards still want a pick: bottom centre
		}
		if (bLockOn)
		{
			// The lock: four arcs round the body and a point on it, breathing a little.
			const float R = (58.f + 5.f * FMath::Sin(Time * 5.f)) * S;
			for (int32 K = 0; K < 4; ++K)
			{
				const float Mid = 45.f + 90.f * K;
				ftcore::uiStrokeArc(Line, LockAt.X, LockAt.Y, R, Mid - 20.f, Mid + 20.f, StrokeStyle(1.3f, 111 + K, S, 0.f));
				Mesh.Stroke(Line, Tok(UiToken::Ink, 0.9f));
			}
			Mesh.Dot(LockAt.X, LockAt.Y, 2.6f * S, Tok(UiToken::Ink, 0.9f));
		}
	}

	// The answer a tell asks for, centred on X with its baseline at Base; dodge and parry carry the style's cue shape too.
	void PaintAnswer(const FPainter& P, float X, float Base, uint8 Answer, const FLinearColor& C) const
	{
		const float S = P.S;
		const float W = AnswerW[Answer];
		P.Text(X - W * 0.5f, Base - PopAsc, AnswerWord[Answer], PopFont, C);
		const float MY = Base - PopAsc * 0.35f;
		if (Answer == 2)
		{
			Danger(X - W * 0.5f - 16.f * S, MY, 14.f * S, C);
		}
		else if (Answer == 0)
		{
			Lozenge(X - W * 0.5f - 16.f * S, MY, 6.5f * S, true, S, C);
		}
	}

	// An off-frame tell: a filled head at a frame 70 px inside the screen's edge, toward the enemy, a ring round it (an onset
	// ring spreading as it starts), and toward the centre the answer over the windup's filling line. At the hit, the ink.
	void PaintEdge(const FPainter& P, const FVector2f& Size, const FFoeMark& M, UiToken AT) const
	{
		const float S = P.S;
		const float CX = Size.X * 0.5f;
		const float CY = Size.Y * 0.5f;
		const float HalfW = CX - 70.f * S;
		const float HalfH = CY - 70.f * S;
		const float T = FMath::Min(M.Dir.X != 0.f ? HalfW / FMath::Abs(M.Dir.X) : 1e9f, M.Dir.Y != 0.f ? HalfH / FMath::Abs(M.Dir.Y) : 1e9f);
		const FVector2f Tip(CX + M.Dir.X * T, CY + M.Dir.Y * T);
		const bool bWind = M.Tell == 1;
		const FLinearColor C = bWind ? Tok(AT) : Tok(UiToken::Ink, M.Impact);
		const float Len = 30.f * S * (1.f + 0.35f * M.Onset);
		const FVector2f Back = Tip - M.Dir * Len;
		const FVector2f Side(-M.Dir.Y * Len * 0.5f, M.Dir.X * Len * 0.5f);
		const FVector2f Head[3] = {Tip, Back + Side, Back - Side};
		Mesh.Fill(Head, 3, C);
		const FVector2f RingAt = Tip - M.Dir * (Len * 0.45f);
		Ring(RingAt.X, RingAt.Y, Len * 1.15f, 0.7f, 101, S, FLinearColor(C.R, C.G, C.B, C.A * 0.5f));
		if (M.Onset > 0.f)
		{
			Ring(RingAt.X, RingAt.Y, Len * (1.15f + 1.4f * (1.f - M.Onset)), 0.6f + 1.8f * M.Onset, 103, S, Tok(AT, M.Onset));
		}
		if (!bWind)
		{
			return;
		}
		const FVector2f In = RingAt - FVector2f(M.Dir.X * (Len * 1.15f + 80.f * S), M.Dir.Y * (Len * 1.15f + 50.f * S));
		PaintAnswer(P, In.X, In.Y - 6.f * S, M.Answer, C);
		const float BW = 96.f * S;
		HLine(In.X - BW * 0.5f, In.X + BW * 0.5f, In.Y + 6.f * S, 0.f, 1.f, StrokeStyle(0.6f, 105, S, 0.f), Tok(UiToken::Track), false);
		HLine(In.X - BW * 0.5f, In.X + BW * 0.5f, In.Y + 6.f * S, 0.f, M.Fill, StrokeStyle(1.5f, 107, S, 0.f), C);
	}

	// The Read cards over Bottom, centred on X: a hairline frame each, its key and the shape it names; the pick in the ally
	// colour when right, the danger colour when wrong, the others stepping back.
	void PaintCards(const FPainter& P, float X, float Bottom) const
	{
		static const FString Keys[ftcore::kReadCards] = {TEXT("1"), TEXT("2"), TEXT("3")};
		if (CardCount <= 0)
		{
			return;
		}
		const float S = P.S;
		const float CW = 116.f * S;
		const float CH = 52.f * S;
		const float Gap = 10.f * S;
		const float Total = CardCount * CW + (CardCount - 1) * Gap;
		const float Left = X - Total * 0.5f;
		const float Top = Bottom - CH;
		P.Soft(X, Top + CH * 0.5f, Total + 120.f * S, CH + 60.f * S, Table().Style.backingSoftHud);
		for (int32 I = 0; I < CardCount; ++I)
		{
			const uint8 St = CardState[I];
			const UiToken EdgeTok = St == 1 ? UiToken::Ally : (St == 2 ? UiToken::Danger : (St == 3 ? UiToken::InkDim : UiToken::Ink));
			const float EA = St == 3 ? 0.5f : 1.f;
			const float X0 = Left + I * (CW + Gap);
			const FVector2f Pts[4] = {FVector2f(X0, Top), FVector2f(X0 + CW, Top), FVector2f(X0 + CW, Top + CH), FVector2f(X0, Top + CH)};
			Poly(Pts, 4, ((St == 1 || St == 2) ? 1.2f : 0.6f) * S, Tok(EdgeTok, EA), true);
			P.Text(X0 + 7.f * S, Top + 3.f * S, Keys[I], LabelFont, Tok(EdgeTok, EA));
			const int32 Ti = CardTell[I];
			P.Text(X0 + CW * 0.5f - TellW[Ti] * 0.5f, Top + CH - 6.f * S - LabelAsc, TellWord[Ti], LabelFont, Tok(St == 3 ? UiToken::InkDim : UiToken::Ink, EA));
		}
	}

	// The events, over everything else.
	void PaintEvents(const FPainter& P, const FVector2f& Size) const
	{
		const float S = P.S;
		const float CX = Size.X * 0.5f;
		// A hit: a danger hairline round the screen's edge; a parry: the weak point's gold along the top and bottom.
		if (HitA > 0.f)
		{
			const FLinearColor C = Tok(UiToken::Danger, 0.85f * HitA);
			const float T = 3.f * S;
			P.Box(0.f, 0.f, Size.X, T, C);
			P.Box(0.f, Size.Y - T, Size.X, T, C);
			P.Box(0.f, T, T, Size.Y - 2.f * T, C);
			P.Box(Size.X - T, T, T, Size.Y - 2.f * T, C);
		}
		if (ParryA > 0.f)
		{
			const FLinearColor C = Tok(UiToken::WeakPoint, 0.8f * ParryA);
			const float T = 2.f * S;
			P.Box(0.f, 0.f, Size.X, T, C);
			P.Box(0.f, Size.Y - T, Size.X, T, C);
		}
		for (const FPopMark& M : Pops)
		{
			if (!M.bShow)
			{
				continue;
			}
			if (M.Word >= 0)
			{
				P.Text(M.WordAt.X - PopWordW[M.Word] * 0.5f, M.WordAt.Y - PopAsc, PopWord[M.Word], PopFont, Tok(M.Tok, M.A));
			}
			if (!M.Num.IsEmpty())
			{
				P.Text(M.NumAt.X - M.NumW * 0.5f, M.NumAt.Y - (M.bNumPop ? PopAsc : ValueAsc), M.Num, M.bNumPop ? PopFont : ValueFont, Tok(M.Tok, M.A));
			}
		}
		// Release: the word large in the accent, a hairline opening under it, the Load it spent; overload in the danger colour.
		if (ReleaseA > 0.f)
		{
			const float A = FMath::Min(1.f, ReleaseA * 2.f);
			const float Base = Size.Y * 0.22f;
			P.Text(CX - ReleaseW * 0.5f, Base - BigAsc, ReleaseWord, BigFont, Tok(UiToken::Accent, A));
			const float L = (120.f + 80.f * (1.f - ReleaseA)) * S;
			HLine(CX - L, CX + L, Base + 14.f * S, 0.f, 1.f, StrokeStyle(1.2f, 121, S), Tok(UiToken::Accent, A));
			P.Text(CX - ReleaseNumW * 0.5f, Base + 24.f * S, ReleaseNum, ValueFont, Tok(UiToken::Ink, A));
		}
		else if (OverA > 0.f)
		{
			const float A = FMath::Min(1.f, OverA * 2.f);
			const float Base = Size.Y * 0.22f;
			P.Text(CX - OverBigW * 0.5f, Base - BigAsc, OverBigWord, BigFont, Tok(UiToken::Danger, A));
			HLine(CX - 160.f * S, CX + 160.f * S, Base + 14.f * S, 0.f, 1.f, StrokeStyle(1.2f, 123, S), Tok(UiToken::Danger, A));
			P.Text(CX - LockedW * 0.5f, Base + 24.f * S, LockedWord, LabelFont, Tok(UiToken::InkDim, A));
		}
		// The result: the word, a hairline, the counts and how to fight again, on a soft shade (no band).
		if (bResult && ResultA > 0.f)
		{
			const float A = ResultA;
			const float Base = Size.Y * 0.42f;
			const float WordW = bWon ? VictoryW : DefeatW;
			P.Soft(CX, Base + 30.f * S, FMath::Max(WordW, StatsW) + 360.f * S, 260.f * S, Table().Style.backingSoftDialogue, A);
			P.Text(CX - WordW * 0.5f, Base - BigAsc, bWon ? VictoryWord : DefeatWord, BigFont, Tok(bWon ? UiToken::Accent : UiToken::Danger, A));
			HLine(CX - 220.f * S, CX + 220.f * S, Base + 16.f * S, 0.f, 1.f, StrokeStyle(1.1f, 125, S), Tok(UiToken::Ink, 0.8f * A));
			P.Text(CX - StatsW * 0.5f, Base + 34.f * S, StatsText, LabelFont, Tok(UiToken::Ink, A));
			if (bAgain)
			{
				P.Text(CX - AgainW * 0.5f, Base + 34.f * S + LabelAsc + 18.f * S, AgainWord, LabelFont, Tok(UiToken::InkDim, A));
			}
		}
		if (TagMode > 0)
		{
			P.Text(40.f * S, 30.f * S, TagWord[TagMode - 1], LabelFont, Tok(UiToken::InkDim));
		}
	}

	// The cylinder, by the crosshair (a guns kit only): one mark per chamber on a short arc to the reticle's right, top
	// down, filled while loaded and a faint point once spent; a reload draws the accent along the same arc as it fills.
	// Full while aiming; otherwise held at the rest alpha so the count is never lost.
	void PaintRounds(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::ArenaHudLead& L = View.now.lead;
		if (L.roundsMax == 0)
		{
			return;
		}
		const float S = P.S;
		const float A = FMath::Max(AimA, FMath::Min(LeadFade.alpha, Table().Style.fade.restAlpha));
		if (A <= 0.01f)
		{
			return;
		}
		const float CX = Size.X * 0.5f;
		const float CY = Size.Y * 0.5f;
		const float R = 30.f * S;
		constexpr float Step = 15.f; // degrees between chambers
		const float A0 = 90.f - Step * 0.5f * (L.roundsMax - 1);
		const float A1 = 90.f + Step * 0.5f * (L.roundsMax - 1);
		for (int32 I = 0; I < L.roundsMax; ++I)
		{
			float X;
			float Y;
			ftcore::uiArcPoint(CX, CY, R, A0 + Step * I, X, Y);
			if (I < L.rounds)
			{
				Mesh.Dot(X, Y, 2.6f * S, Tok(UiToken::Ink, A));
			}
			else
			{
				Mesh.Dot(X, Y, 1.4f * S, Tok(UiToken::InkDim, 0.5f * A), false);
			}
		}
		if (L.reload01 > 0.f)
		{
			ftcore::uiStrokeArc(Line, CX, CY, R + 8.f * S, A0 - Step * 0.5f, A1 + Step * 0.5f, StrokeStyle(1.2f, 61, S, 0.f), 0.f, FMath::Clamp(L.reload01, 0.f, 1.f));
			Mesh.Stroke(Line, Tok(UiToken::Accent, A));
		}
	}

	TFunction<const UFTArenaRunner*()> GetRunner;
	ftcore::ArenaHudView View;
	ftcore::UiFade LeadFade;
	ftcore::UiFade TopFade;
	ftcore::UiFade VerbFade;
	FLostTrace HpLost;
	float AimA = 0.f; // the aim stance's fade (the runner's), for the cylinder by the crosshair
	FLostTrace TopLost;
	FVerbSlot Verbs[4] = {{"input.guard"}, {"input.release"}, {"input.count"}, {"input.heal"}};
	ftcore::InputDevice VerbDevice = ftcore::InputDevice::Keys;
	uint32 LastVerbState = MAX_uint32;
	bool bRebuild = true;
	float TakeHalfAge = 10.f; // s since the last take-half (its pulse on the Guard key)
	mutable FLineMesh Mesh;
	mutable ftcore::UiStroke Line; // the stroke being drawn (Core fills it, the mesh takes it)
	bool bLive = false;
	float Time = 0.f;
	float BuiltHeight = -1.f;
	FSlateFontInfo LabelFont;
	FSlateFontInfo ValueFont;
	FSlateFontInfo VerbFont;
	float LabelH = 0.f;
	float LabelAsc = 0.f;
	float ValueAsc = 0.f;
	float VerbAsc = 0.f;
	FString HpText;
	float HpTextW = 0.f;
	FString LoadText;
	FString DownWord, OverWord, BrokenWord, ReadyWord, TargetWord, WeightWord, WeakWord, InvulWord;
	float DownW = 0.f, OverW = 0.f, BrokenW = 0.f, ReadyW = 0.f, TargetW = 0.f, WeightW = 0.f, WeakW = 0.f, InvulW = 0.f;
	// The top line's name, its called zone and answer words.
	FSlateFontInfo NameFont;
	float NameAsc = 0.f;
	FString NameText;
	float NameW = 0.f;
	int32 NameSlot = -2; // the slot the name was built for (-1: none, -2: not built)
	bool bNamed = false;
	FString CallWord, RocWord, BossDownWord;
	float CallW = 0.f, RocW = 0.f, BossDownW = 0.f;
	FString ZoneWord[3];
	float ZoneW[3] = {};
	FString NoteWord;
	float NoteW = 0.f;
	UiToken NoteTok = UiToken::Ink;
	double NoteAt = 0.0;
	int32 NoteSeen = -2;
	double FightNow = 0.0;

	FFoeMark Foes[UFTArenaRunner::MaxEnemies];
	bool bLockOn = false;
	FVector2f LockAt = FVector2f::ZeroVector;
	bool bCardsShown = false;
	bool bCardsLoose = false;
	int32 CardCount = 0;
	uint8 CardTell[ftcore::kReadCards] = {};
	uint8 CardState[ftcore::kReadCards] = {};
	FSlateFontInfo PopFont;
	float PopAsc = 0.f;
	FSlateFontInfo BigFont;
	float BigAsc = 0.f;
	FString AnswerWord[3];
	float AnswerW[3] = {};
	FString NowWord, StaggerWord, RiposteWord;
	float NowW = 0.f, StaggerW = 0.f, RiposteW = 0.f;
	FString TellWord[ftcore::kArenaTellCount];
	float TellW[ftcore::kArenaTellCount] = {};
	FPopMark Pops[UFTArenaRunner::MaxPopups];
	FString PopWord[PwCount];
	float PopWordW[PwCount] = {};
	float HitA = 0.f, ParryA = 0.f, ReleaseA = 0.f, OverA = 0.f;
	FString ReleaseWord, OverBigWord, VictoryWord, DefeatWord, LockedWord, AgainWord;
	float ReleaseW = 0.f, OverBigW = 0.f, VictoryW = 0.f, DefeatW = 0.f, LockedW = 0.f, AgainW = 0.f;
	FString ReleaseNum;
	float ReleaseNumW = 0.f;
	int32 ReleaseShown = -1;
	bool bResult = false;
	bool bWon = false;
	bool bAgain = false;
	bool bStatsBuilt = false;
	float ResultA = 0.f;
	FString StatsText;
	float StatsW = 0.f;
	int32 TagMode = 0; // 1 autoplay, 2 idle
	FString TagWord[2];
	float TagW[2] = {};
};

// ---- dialogue ---------------------------------------------------------------------------------------------------------

bool IsLowSurrogateUnit(TCHAR C) { return C >= 0xDC00 && C <= 0xDFFF; }

#if !UE_BUILD_SHIPPING
// -UiChoiceShot placeholders (non-shipping stills only; the script has no choice beats yet). The last cannot be taken.
const TCHAR* const kFakeOptions[3] = {TEXT("1   First option"), TEXT("2   Second option"), TEXT("3   A locked option")};
#endif

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
		// Line arrival: each codepoint's time out, so the words come up by alpha behind the reveal.
		if (ArrivalSerial != D->GetBeatSerial())
		{
			ArrivalSerial = D->GetBeatSerial();
			ArriveAt.Init(TNumericLimits<float>::Max(), CpToUnit.Num()); // once per beat
			SeenRevealed = 0;
		}
		const int32 Revealed = FMath::Min(static_cast<int32>(View.revealedChars), ArriveAt.Num());
		for (int32 I = SeenRevealed; I < Revealed; ++I)
		{
			ArriveAt[I] = Time;
		}
		SeenRevealed = FMath::Max(SeenRevealed, Revealed);

		// The options: the beat's own, or (non-shipping -UiChoiceShot, for stills while the script has no choices)
		// three placeholders under any finished line.
		OptionRows = 0;
		bFakeOptions = false;
		if (View.kind == ftcore::DialogueViewKind::Choice && D->GetPresenter().optionsVisible())
		{
			OptionRows = FMath::Min(static_cast<int32>(View.optionCount), D->GetOptions().Num());
		}
#if !UE_BUILD_SHIPPING
		static const bool bChoiceShot = FParse::Param(FCommandLine::Get(), TEXT("UiChoiceShot"));
		if (bChoiceShot && View.kind == ftcore::DialogueViewKind::Line && View.complete)
		{
			OptionRows = UE_ARRAY_COUNT(kFakeOptions);
			bFakeOptions = true;
		}
#endif
		if (OptionRows > 0 && OptionsFor != (ArrivalSerial ^ (bFakeOptions ? 0x80000000u : 0u)))
		{
			OptionsFor = ArrivalSerial ^ (bFakeOptions ? 0x80000000u : 0u);
			OptionW = 0.f;
			for (int32 I = 0; I < OptionRows; ++I)
			{
				OptionW = FMath::Max(OptionW, TextWidth(OptionText(*D, I), OptionFont));
			}
		}
		// Where the options hang: beside the person answered, projected each frame.
		bAnchor = false;
		FVector Head;
		const UWorld* World = D->GetWorld();
		const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		if (OptionRows > 0 && PC && D->GetAnsweredHead(Head))
		{
			int32 VX = 0;
			int32 VY = 0;
			PC->GetViewportSize(VX, VY);
			FVector2D Px;
			if (VX > 0 && VY > 0 && PC->ProjectWorldLocationToScreen(Head, Px, true))
			{
				AnchorUV = FVector2f(static_cast<float>(Px.X / VX), static_cast<float>(Px.Y / VY));
				bAnchor = AnchorUV.X > 0.05f && AnchorUV.X < 0.95f && AnchorUV.Y > 0.08f && AnchorUV.Y < 0.75f;
			}
		}
		if (OptionRows > 0 && AnchorLoggedFor != OptionsFor)
		{
			AnchorLoggedFor = OptionsFor;
			UE_LOG(LogForgottenThird, Log, TEXT("UI: choice anchor %s (%.2f, %.2f)."), bAnchor ? TEXT("beside the face") : TEXT("none, under the line"),
				AnchorUV.X, AnchorUV.Y);
		}
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer,
		const FWidgetStyle&, bool) const override
	{
		// The director is re-checked here (weak pointer): it may be gone between Tick and Paint. View's string_views are
		// not read in Paint; the text comes from the director's own FString.
		const AFTStoryDirector* D = Shown.Get();
		if (!D || Lines.Num() == 0)
		{
			return Layer;
		}
		const FVector2f Size = Geometry.GetLocalSize();
		const FPainter P{Geometry, Out, Layer, FMath::Max(0.25f, Size.Y / 1080.f)};
		const float S = P.S;
		const FString& Text = D->GetText();
		// Anchored options stand beside the person; otherwise they sit under the line in the same block.
		const int32 BelowRows = bAnchor ? 0 : OptionRows;
		const float BlockH = LineH * (Lines.Num() + BelowRows + (BelowRows > 0 ? 0.4f : 0.f));
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
			PaintArriving(P, X, Y, Text, Lines[I].X, End, Body);
			LastRight = X + LineW[I];
		}
		Mesh.Begin(Geometry, S);
		if (OptionRows > 0)
		{
			const float RowH = LineH * 1.05f;
			const float OptBlockH = RowH * OptionRows;
			const float MarkGap = 22.f * S; // the mark's centre to the text
			float MarkX;
			float OptTop;
			if (bAnchor)
			{
				// Beside the person: to their right when it fits, else to their left; never down into the line.
				const FVector2f H(AnchorUV.X * Size.X, AnchorUV.Y * Size.Y);
				const float Gap = 150.f * S;
				const bool bRight = H.X + Gap + MarkGap + OptionW < Size.X - 80.f * S;
				MarkX = bRight ? H.X + Gap : H.X - Gap - MarkGap - OptionW;
				OptTop = FMath::Clamp(H.Y - 30.f * S - OptBlockH * 0.5f, 60.f * S, Top - OptBlockH - 40.f * S);
				// Off the line's block the options often sit on a lamp pool or a bright wall: a deeper, tighter shade than
				// the line's (still a falloff, never a box).
				P.Soft(MarkX + (MarkGap + OptionW) * 0.5f, OptTop + OptBlockH * 0.5f, OptionW + 200.f * S, OptBlockH + 110.f * S,
					FMath::Min(0.62f, Table().Style.backingSoftDialogue * 1.45f));
				// The leader: a fine line from beside the face to the middle row's mark, eased flat at both ends.
				const float MidY = OptTop + RowH * (static_cast<float>(OptionRows - 1) * 0.5f) + OptionH * 0.5f;
				const FVector2f P0(H.X + (bRight ? 40.f : -40.f) * S, H.Y);
				const FVector2f P1(bRight ? MarkX - 12.f * S : MarkX + MarkGap + OptionW + 12.f * S, MidY);
				constexpr int32 N = 17;
				FVector2f Pts[N];
				for (int32 I = 0; I < N; ++I)
				{
					const float T = static_cast<float>(I) / (N - 1);
					const float E = T * T * (3.f - 2.f * T);
					Pts[I] = FVector2f(FMath::Lerp(P0.X, P1.X, T), FMath::Lerp(P0.Y, P1.Y, E));
				}
				PolyStroke(Line, Pts, N, 0.6f * S, 0.85f);
				Mesh.Stroke(Line, Tok(UiToken::Ink));
				Mesh.Dot(P0.X, P0.Y, 2.4f * S, Tok(UiToken::Ink));
			}
			else
			{
				MarkX = CX - MaxW * 0.35f - MarkGap;
				OptTop = Top + LineH * (Lines.Num() + 0.4f);
			}
			for (int32 I = 0; I < OptionRows; ++I)
			{
				const float Y = OptTop + RowH * I;
				const bool bPick = OptionPick(*D, I);
				const float MY = Y + OptionH * 0.5f;
				// A small ring: open for a choice that can be taken, a short dash for one that cannot.
				if (bPick)
				{
					ftcore::uiStrokeArc(Line, MarkX, MY, 4.5f * S, 0.f, 360.f, StrokeStyle(1.3f, 40u + static_cast<uint32>(I), S, 0.f));
					if (Line.count > 1)
					{
						Line.pt[Line.count - 1].x = Line.pt[0].x; // closed without a seam
						Line.pt[Line.count - 1].y = Line.pt[0].y;
					}
					Mesh.Stroke(Line, Tok(UiToken::Ink), true, true);
				}
				else
				{
					ftcore::uiStrokeLine(Line, MarkX - 4.5f * S, MY, MarkX + 4.5f * S, MY, StrokeStyle(1.3f, 50u + static_cast<uint32>(I), S, 0.f));
					Mesh.Stroke(Line, Tok(UiToken::InkDim));
				}
				P.Text(MarkX + MarkGap, Y, OptionText(*D, I), OptionFont, bPick ? Tok(UiToken::Ink) : Tok(UiToken::InkDim));
			}
		}
		else if (View.continuePrompt)
		{
			const float A = 0.55f + 0.45f * FMath::Sin(Time * 5.f);
			P.Chevron(LastRight + 22.f * S, Top + LineH * (Lines.Num() - 1) + LineH * 0.45f, 18.f * S, Tok(UiToken::Accent, A));
		}
		Mesh.Flush(Out, Layer + 1);
		return Layer + 3;
	}

private:
	const FString& OptionText(const AFTStoryDirector& D, int32 I) const
	{
#if !UE_BUILD_SHIPPING
		if (bFakeOptions)
		{
			static const FString Fake[UE_ARRAY_COUNT(kFakeOptions)] = {kFakeOptions[0], kFakeOptions[1], kFakeOptions[2]};
			return Fake[I];
		}
#endif
		return D.GetOptions()[I];
	}

	bool OptionPick(const AFTStoryDirector& D, int32 I) const
	{
		return bFakeOptions ? I < OptionRows - 1 : D.IsOptionSelectable(I);
	}

	// One line's revealed text [From, To), in runs of equal arrival alpha (a few steps): the words just out come up by
	// alpha behind the reveal; once every codepoint is out a while, one run, as before.
	void PaintArriving(const FPainter& P, float X, float Y, const FString& T, int32 From, int32 To, const FLinearColor& Body) const
	{
		if (To <= From)
		{
			return;
		}
		constexpr float Fade = 0.4f; // s from out to full
		constexpr int32 Steps = 4;
		const TSharedRef<FSlateFontMeasure> M = FontMeasure();
		const auto Draw = [&](int32 U0, int32 U1, int32 Level)
		{
			const float Off = U0 > From ? static_cast<float>(M->Measure(FStringView(T), From, U0, TextFont).X) : 0.f;
			FLinearColor C = Body;
			C.A *= static_cast<float>(Level) / Steps;
			P.Text(X + Off, Y, T, TextFont, C, U0, U1);
		};
		int32 Cp = Algo::LowerBound(CpToUnit, From);
		int32 RunStart = From;
		int32 RunLevel = -1;
		for (; Cp < CpToUnit.Num() && CpToUnit[Cp] < To; ++Cp)
		{
			const float Age = Cp < ArriveAt.Num() ? Time - ArriveAt[Cp] : Fade;
			const int32 Level = FMath::Clamp(FMath::CeilToInt(FMath::Clamp(Age / Fade, 0.f, 1.f) * Steps), 1, Steps);
			if (Level != RunLevel)
			{
				if (RunLevel >= 0 && CpToUnit[Cp] > RunStart)
				{
					Draw(RunStart, CpToUnit[Cp], RunLevel);
				}
				RunStart = CpToUnit[Cp];
				RunLevel = Level;
			}
		}
		if (RunLevel >= 0)
		{
			Draw(RunStart, To, RunLevel);
		}
	}

	// Once per beat (or on a size change): fonts, the speaker's capitals, the codepoint-to-unit map and the line breaks.
	void Rewrap(const AFTStoryDirector& D, const FVector2f& Size, float Scale)
	{
		OptionsFor = MAX_uint32; // the options are measured again in the new font
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
		const bool bSpeakerUpper = Table().Style.text[static_cast<size_t>(UiTextRole::Speaker)].upper;
		SpeakerUpper = View.kind != ftcore::DialogueViewKind::Line ? FString() : bSpeakerUpper ? D.GetSpeaker().ToUpper() : D.GetSpeaker();
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
	TWeakObjectPtr<const AFTStoryDirector> Shown; // this frame's director when there is text to show
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
	// Line arrival.
	uint32 ArrivalSerial = MAX_uint32;
	TArray<float> ArriveAt; // by codepoint: the widget time it came out (sized once per beat)
	int32 SeenRevealed = 0;
	// Options.
	int32 OptionRows = 0;
	bool bFakeOptions = false;
	uint32 OptionsFor = MAX_uint32;
	uint32 AnchorLoggedFor = MAX_uint32;
	float OptionW = 0.f;
	bool bAnchor = false;
	FVector2f AnchorUV = FVector2f::ZeroVector; // the answered person's head, as a fraction of the view
	mutable FLineMesh Mesh;
	mutable ftcore::UiStroke Line;
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
