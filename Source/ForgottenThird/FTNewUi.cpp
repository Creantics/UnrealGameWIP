#include "FTNewUi.h"

#include "FTArenaFighter.h"
#include "FTArenaRunner.h"
#include "ForgottenThird.h"
#include "FTInputBindings.h"
#include "FTStoryDirector.h"
#include "FTText.h"
#include "FTUiText.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/UiBudget.h"
#include "ftcore/UiEventMarks.h"
#include "ftcore/Settings.h"
#include "ftcore/InputBindings.h"
#include "ftcore/UiStroke.h"
#include "ftcore/UiStyle.h"
#include "ftcore/UiViews.h"

#include "Algo/BinarySearch.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
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
#include "UnrealClient.h"
#include "Widgets/SLeafWidget.h"

#include <string>

namespace
{
TAutoConsoleVariable<int32> CVarNewUi(TEXT("ft.NewUi"), 0,
	TEXT("1: the new game UI (Slate, drawn from Core view-models) in place of the Canvas HUD's parts it replaces. ")
	TEXT("-FTNewUi on the command line sets it. Read when a map's HUD begins: a change applies from the next map."));

using ftcore::UiTextRole;
using ftcore::UiToken;

// The words a fight keeps: the call word under the boss name (a called zone, or the answer a boss's move needs).
const FTUiText kZoneUi[3] = {FTUiText(TEXT("arena.boss.left")), FTUiText(TEXT("arena.boss.right")), FTUiText(TEXT("arena.boss.path"))};
constexpr double kBossNoteLife = 1.2; // a wrong-side answer's side mark, fading
const FTUiText kAnswerUi[3] = {FTUiText(TEXT("arena.parry")), FTUiText(TEXT("arena.block")), FTUiText(TEXT("arena.dodge"))};
const FTUiText kVictoryUi(TEXT("battle.victory"));
const FTUiText kDefeatUi(TEXT("battle.defeat"));
const FTUiText kAgainUi(TEXT("battle.again"));
// The result's counts: the time, then each count with its word by number (one / other).
const FTUiText kStatTimeUi(TEXT("arena.stat.time"));
const FTUiText kStatHitsOne(TEXT("arena.stat.hits.one"));
const FTUiText kStatHitsOther(TEXT("arena.stat.hits.other"));
const FTUiText kStatParriesOne(TEXT("arena.stat.parries.one"));
const FTUiText kStatParriesOther(TEXT("arena.stat.parries.other"));
const FTUiText kStatRipostesOne(TEXT("arena.stat.ripostes.one"));
const FTUiText kStatRipostesOther(TEXT("arena.stat.ripostes.other"));
const FTUiText kStatEvadesOne(TEXT("arena.stat.evades.one"));
const FTUiText kStatEvadesOther(TEXT("arena.stat.evades.other"));
const FTUiText kStatBlocksOne(TEXT("arena.stat.blocks.one"));
const FTUiText kStatBlocksOther(TEXT("arena.stat.blocks.other"));
const FTUiText kStatHitsTakenOne(TEXT("arena.stat.hits_taken.one"));
const FTUiText kStatHitsTakenOther(TEXT("arena.stat.hits_taken.other"));
const FTUiText* const kStatUi[6][2] = {{&kStatHitsOne, &kStatHitsOther}, {&kStatParriesOne, &kStatParriesOther}, {&kStatRipostesOne, &kStatRipostesOther}, {&kStatEvadesOne, &kStatEvadesOther}, {&kStatBlocksOne, &kStatBlocksOther}, {&kStatHitsTakenOne, &kStatHitsTakenOther}};
const FTUiText kAutoPlayUi(TEXT("arena.autoplay"));
const FTUiText kIdleUi(TEXT("arena.idle"));
const FTUiText kStoryHintUi(TEXT("story.hint"));
constexpr double kImpactLife = 0.3;    // the tell's flash once the move is active
constexpr double kOnsetLife = 0.35;    // an off-frame windup marker's onset swell
constexpr double kHitEdgeLife = 0.35;  // the edge flash when the lead is hit (under 0.4 s)
constexpr double kParryEdgeLife = 0.4;
constexpr double kResultFade = 0.3;
constexpr float kWallBand = 3.5f; // m from the wall where its cue starts

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
	FLinearColor Word[ftcore::kUiTokenCount]; // a coloured word's colour: lifted to the style's luminance floor
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
			const ftcore::UiColor C = ftcore::uiColor(T.Style, ftcore::UiWorld::Solhaven, ftcore::UiPalette::Default, static_cast<UiToken>(I));
			T.Token[I] = LinOf(C);
			T.Word[I] = LinOf(ftcore::uiLumaFloor(C, T.Style.wordLumaFloor));
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

// A word in a token's colour: the colour lifted to the luminance floor so a dark hue still reads over its dark edge.
FLinearColor WordTok(UiToken T, float Alpha = 1.f)
{
	FLinearColor C = Table().Word[static_cast<size_t>(T)];
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
			// Bands spaced by the inverse of a smoothstep over the outer part: an even middle, a soft fall, no cone.
			const float Y = (static_cast<float>(I) + 0.5f) / static_cast<float>(Bands);
			const float T = 0.55f + 0.45f * (0.5f - FMath::Sin(FMath::Asin(1.f - 2.f * Y) / 3.f));
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

	// A vignette with no edge at all: an ellipse centred on a screen corner or edge (most of it off the screen) whose alpha
	// falls from Peak at its centre to nothing at the rim, with zero slope at both ends, so no boundary or patch reads.
	void Vignette(float CX, float CY, float RX, float RY, float Peak, const FLinearColor* Colour = nullptr)
	{
		if (Peak <= 0.f || RX <= 0.f || RY <= 0.f)
		{
			return;
		}
		constexpr int32 Rings = 24;
		constexpr int32 Segs = 48;
		constexpr float Flat = 0.f; // no even middle: the fall starts at the centre
		FPart& P = Parts[ShadePart];
		const SlateIndex Base = static_cast<SlateIndex>(P.Verts.Num());
		const FLinearColor Back = Colour ? *Colour : Tok(UiToken::Backing, 1.f);
		P.Verts.Add(Vert(CX, CY, Back, Peak));
		for (int32 R = 1; R <= Rings; ++R)
		{
			const float T = static_cast<float>(R) / Rings;
			const float U = FMath::Clamp((T - Flat) / (1.f - Flat), 0.f, 1.f);
			const float A = Peak * FMath::Square(1.f - U * U); // flat, then zero slope at the rim: no edge or blob reads
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

	// A gradient strip with no edge: alpha Peak along one side (bPeakAtEnd: the bottom or right side), even for the first
	// 1 - Ramp of the way, then falling to nothing at the other side with zero slope at both ends of the fall.
	void Band(float X0, float Y0, float X1, float Y1, bool bAlongY, bool bPeakAtEnd, float Ramp, float Peak, const FLinearColor& C)
	{
		if (Peak <= 0.f || X1 <= X0 || Y1 <= Y0)
		{
			return;
		}
		constexpr int32 Steps = 32;
		FPart& P = Parts[ShadePart];
		const int32 Base = P.Verts.Num();
		const float R = FMath::Clamp(Ramp, 0.05f, 1.f);
		for (int32 I = 0; I <= Steps; ++I)
		{
			const float T = static_cast<float>(I) / Steps;
			const float D = bPeakAtEnd ? 1.f - T : T; // the share of the way from the peak side
			const float U = FMath::Clamp((D - (1.f - R)) / R, 0.f, 1.f);
			const float A = Peak * (1.f - U * U * (3.f - 2.f * U));
			if (bAlongY)
			{
				const float Y = FMath::Lerp(Y0, Y1, T);
				P.Verts.Add(Vert(X0, Y, C, A));
				P.Verts.Add(Vert(X1, Y, C, A));
			}
			else
			{
				const float X = FMath::Lerp(X0, X1, T);
				P.Verts.Add(Vert(X, Y0, C, A));
				P.Verts.Add(Vert(X, Y1, C, A));
			}
		}
		for (int32 I = 0; I < Steps; ++I)
		{
			const int32 I0 = Base + 2 * I;
			Tri(P, I0, I0 + 1, I0 + 3);
			Tri(P, I0, I0 + 3, I0 + 2);
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

// Whether a stone bearer note wakes the boss line (answers, breaks, crits, lifts, the shock ring). The line shows it as
// motion only; no note draws a word.
bool BossNoteShown(const FFTStoneBearerNote& Note)
{
	if (Note.Kind == FFTStoneBearerNote::EKind::ShockRingSent)
	{
		return true;
	}
	if (Note.Kind != FFTStoneBearerNote::EKind::Run)
	{
		return false;
	}
	switch (Note.Event.type)
	{
	case ftcore::ArenaBossEventType::Answered:
		switch (Note.Event.outcome)
		{
		case ftcore::CalledOutcome::Intercepted:
		case ftcore::CalledOutcome::Guarded:
		case ftcore::CalledOutcome::WrongZone:
		case ftcore::CalledOutcome::Missed: return true;
		default: return false;
		}
	case ftcore::ArenaBossEventType::Broke:
	case ftcore::ArenaBossEventType::Crit:
	case ftcore::ArenaBossEventType::Lifted: return true;
	default: return false;
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
		// The latest note that wakes the line.
		for (int32 Serial = Bearer->GetNoteTotal() - 1; Serial >= Bearer->GetNoteTotal() - Bearer->GetNoteCount(); --Serial)
		{
			if (BossNoteShown(Bearer->GetNoteBySerial(Serial)))
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
constexpr float VerbUp = 252.f;  // the first verb's baseline, up from the bottom
constexpr float VerbStep = 52.f;
constexpr float TopLine = 100.f; // the boss's line, down from the top
constexpr float TopLen = 600.f;  // the top line, boss or target: one length
constexpr float LowHp = 0.3f;
constexpr float LeadScale = 1.2f; // the lead's block (ring, marks, numbers) against the 1080p unit
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
			LeadFont = FontFor(UiTextRole::Value, H, Carry::LeadScale);
			LeadAsc = FontMeasure()->GetMaxCharacterHeight(LeadFont) + FontMeasure()->GetBaseline(LeadFont);
			VerbFont = FontFor(UiTextRole::Verb, H);
			NameFont = FontFor(UiTextRole::Title, H);
			NameFont.LetterSpacing = 220; // the name in spaced small caps
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
			NameSlot = -2; // measured again in the new font
			NoteSeen = -2;
			// The call word under the boss name: one large word, near-white on its full dark edge. The result's words in the title
			// at 1.8.
			CallFont = FontFor(UiTextRole::Popup, H, 1.35f);
			CallFont.OutlineSettings.OutlineColor = Table().Edge;
			CallAsc = FontMeasure()->GetMaxCharacterHeight(CallFont) + FontMeasure()->GetBaseline(CallFont);
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
				RoleWord(UiTextRole::Popup, kAnswerUi[I], AnswerWord[I], AnswerW[I], CallFont);
				RoleWord(UiTextRole::Popup, kZoneUi[I], ZoneWord[I], ZoneW[I], CallFont);
			}
			Word(kAgainUi, AgainWord, AgainW, LabelFont);
			Word(kAutoPlayUi, TagWord[0], TagW[0], LabelFont);
			Word(kIdleUi, TagWord[1], TagW[1], LabelFont);
			RoleWord(UiTextRole::Title, kVictoryUi, VictoryWord, VictoryW, BigFont);
			RoleWord(UiTextRole::Title, kDefeatUi, DefeatWord, DefeatW, BigFont);
			bStatsBuilt = false;
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
		Frame.lead.lead = Runner->GetLeadSeat();
		ftcore::fillArenaHudLead(Fight, Frame.lead);
		ftcore::fillArenaHudTarget(Fight, Frame.target);
		FillBoss(*Runner, Frame.boss);
		Frame.partner = ftcore::ArenaHudPartner{};
		Frame.partner.lead = Runner->GetAllySeat();
		ftcore::fillArenaHudPartner(Fight, Frame.partner); // the carrying runs' ally; zero (absent) otherwise
		Frame.third = ftcore::ArenaHudThird{};
		const uint32 Changed = ftcore::updateArenaHudView(View, Frame);
		const ftcore::ArenaHudLead& L = View.now.lead;
		if (bRebuildNow || (Changed & SectionBit(ftcore::ArenaHudSection::Lead)))
		{
			HpText = FString::Printf(TEXT("%d"), L.hp);
			HpTextW = TextWidth(HpText, LeadFont);
			LoadText = FString::Printf(TEXT("%d"), L.load);
		}
		TakeHalfAge = L.tookHalf ? 0.f : TakeHalfAge + DeltaTime;
		HpLost.Step(L.hpMax > 0 ? static_cast<float>(L.hp) / L.hpMax : 0.f, DeltaTime);
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		// The name over the line: the boss's, else the target's, from the loc table by its arena kind (none: no name, the line
		// alone). Lowercase in the small-caps face: one even small-cap height.
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
					NameText = Name.ToLower();
					NameW = TextWidth(NameText, NameFont);
					bNamed = true;
				}
			}
		}
		// A wrong-side answer: a side mark at the reticle on the side that was called (no word).
		FightNow = Fight.time();
		if (B.noteSerial != NoteSeen)
		{
			NoteSeen = B.noteSerial;
			const FFTStoneBearerHost* Bearer = Runner->GetStoneBearer();
			if (Bearer && B.noteSerial >= 0)
			{
				const FFTStoneBearerNote& Note = Bearer->GetNoteBySerial(B.noteSerial);
				if (Note.Kind == FFTStoneBearerNote::EKind::Run && Note.Event.type == ftcore::ArenaBossEventType::Answered
					&& Note.Event.outcome == ftcore::CalledOutcome::WrongZone)
				{
					WrongAt = Note.Event.at;
					WrongCall = LastCall;
				}
			}
		}
		if (B.present && B.call != ftcore::UiCalledZone::None)
		{
			LastCall = B.call;
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
		TickEvents(*Runner, Fight, PC, Geometry.GetLocalSize(), DeltaTime);
		TickBudget(Fight, Changed, DeltaTime);
	}

	// The element budget (Core): the data asks for marks, the budget grants them in priority order, and what is not granted
	// is not drawn. At rest only the lead's ring and the party's places; never more than the cap in action.
	void TickBudget(const ftcore::ArenaFight& Fight, uint32 Changed, float DeltaTime)
	{
		using ftcore::UiMarkKind;
		const ftcore::ArenaHudFrame& Now = View.now;
		const ftcore::ArenaHudBoss& B = Now.boss;
		ftcore::UiBudgetInput In;
		In.dt = DeltaTime;
		In.changed = Changed;
		In.verbsAvailable = ftcore::uiVerbsAvailable(Now);
		In.prompted = Now.lead.releaseReady ? static_cast<int8>(ftcore::UiVerb::Release) : static_cast<int8>(-1);
		In.resultOpen = bResult;
		In.want = ftcore::uiMarkWants(Now, In.verbsAvailable);
		uint8 Tells = 0;
		uint8 FoeLines = 0;
		bool bBossActs = false;
		for (int32 I = 0; I < UFTArenaRunner::MaxEnemies; ++I)
		{
			const FFoeMark& M = Foes[I];
			if (!M.bShow)
			{
				continue;
			}
			Tells += M.Tell != 0 ? 1 : 0;
			FoeLines += (M.bOn && M.bLine) ? 1 : 0;
			bBossActs |= B.present && B.slot == I && M.Tell != 0;
		}
		const auto Want = [&In](UiMarkKind K, uint8 N) { In.want[static_cast<size_t>(K)] = N; };
		Want(UiMarkKind::Tell, Tells);
		Want(UiMarkKind::Event, static_cast<uint8>(ftcore::uiMarkLiveCount(Marks)));
		Want(UiMarkKind::WeakPoint, bWeakOn ? 1 : 0);
		Want(UiMarkKind::Lock, bLockOn ? 1 : 0);
		Want(UiMarkKind::Foe, FoeLines);
		Want(UiMarkKind::Wall, WallA > 0.01f ? 1 : 0);
		In.activity = bMarkNew || Tells > 0 || HitA > 0.f || ParryA > 0.f || AimA > 0.01f;
		// The boss line returns when the boss acts (a windup or its hit), is hit, opens its weak point, or is locked on anew.
		const uint8 LockNow = Fight.lock();
		// Any change in the boss's line (hp, phase, a call, a note, down, invulnerable, the weak point) counts, and a live call
		// keeps it up.
		In.bossEvent = B.present
			&& (!bBossSeen || bBossActs || !(B == BossSeen) || B.call != ftcore::UiCalledZone::None || B.weakPoint == ftcore::UiWeakPoint::Open
				|| (LockNow == B.slot && LockSeen != LockNow));
		bBossSeen = B.present;
		BossSeen = B;
		LockSeen = LockNow;

		const ftcore::UiBudgetOut& G = ftcore::uiBudgetStep(Budget, In, Table().Style.budget);
		// Event marks past the grant: the oldest are not drawn.
		MarkMask = 0;
		{
			uint32 Over = ftcore::uiMarkLiveCount(Marks);
			const uint32 Allowed = G.allowed(UiMarkKind::Event);
			while (Over > Allowed)
			{
				int32 Oldest = -1;
				for (int32 I = 0; I < static_cast<int32>(ftcore::kUiLiveMarkMax); ++I)
				{
					const ftcore::UiLiveMark& M = Marks.m[static_cast<size_t>(I)];
					if (M.live && !(MarkMask & (1u << I)) && (Oldest < 0 || M.serial < Marks.m[static_cast<size_t>(Oldest)].serial))
					{
						Oldest = I;
					}
				}
				if (Oldest < 0)
				{
					break;
				}
				MarkMask |= static_cast<uint16>(1u << Oldest);
				--Over;
			}
		}
		uint8 TellsLeft = G.allowed(UiMarkKind::Tell);
		uint8 LinesLeft = G.allowed(UiMarkKind::Foe);
		for (FFoeMark& M : Foes)
		{
			if (!M.bShow)
			{
				continue;
			}
			if (M.Tell != 0)
			{
				if (TellsLeft > 0)
				{
					--TellsLeft;
				}
				else
				{
					M.Tell = 0;
				}
			}
			if (M.bOn && M.bLine)
			{
				if (LinesLeft > 0)
				{
					--LinesLeft;
				}
				else
				{
					M.bLine = false;
				}
			}
		}
		bWeakOn &= G.allowed(UiMarkKind::WeakPoint) > 0;
		bLockOn &= G.allowed(UiMarkKind::Lock) > 0;
		if (G.allowed(UiMarkKind::Wall) == 0)
		{
			WallA = 0.f;
		}
		// The lead's details (numbers, Load, stamina, heals, read) go at rest, leaving the ring and its mark.
		RestA = G.actionAlpha;
		if (Budget.maxMarks > MarksLogged)
		{
			MarksLogged = Budget.maxMarks;
			UE_LOG(LogForgottenThird, Log, TEXT("Arena UI: marks max %d (cap %d)."), MarksLogged, Table().Style.budget.marks);
		}
		if (G.rest != bRestLogged)
		{
			bRestLogged = G.rest;
			UE_LOG(LogForgottenThird, Log, TEXT("Arena UI: %s at %.1f s (marks %d)."), G.rest ? TEXT("rest") : TEXT("action"), Fight.time(), G.marks);
		}
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
		PaintReticle(P, Size);
		PaintLead(P, Size);
		PaintTop(P, Size);
		PaintSecondLine(P, Size);
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
		if (To01 - From01 < 0.002f || C.A <= 0.f)
		{
			return; // nothing to draw: an empty arc must not leave a stray stroke
		}
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
		if (To01 - From01 < 0.002f || C.A <= 0.f)
		{
			return;
		}
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

	// Each live event mark of a kind the budget grants, with its pose now.
	template <typename FnT>
	void EachMark(ftcore::UiEventMark Kind, FnT&& Fn) const
	{
		for (int32 I = 0; I < static_cast<int32>(ftcore::kUiLiveMarkMax); ++I)
		{
			const ftcore::UiLiveMark& M = Marks.m[static_cast<size_t>(I)];
			if (M.live && M.mark == Kind && !(MarkMask & (1u << I)))
			{
				Fn(M, ftcore::uiEventMarkPose(Kind, M.age));
			}
		}
	}

	// A lead's own mark (style data, by party seat): a gauntlet's knuckle line, a cylinder's dot ring or a broken ring, in
	// the ring's pencil line, all three one weight and about 24 units across. K scales it (1 in the lead's ring); Width is
	// the line at 1080p.
	void LeadMark(float X, float Y, float K, int32 Seat, float Width, float S, const FLinearColor& C) const
	{
		const ftcore::UiLeadMark M = Table().Style.leadMark[static_cast<size_t>(FMath::Clamp(Seat, 0, 2))];
		const float U = K * S;
		const ftcore::UiStrokeStyle Pen = StrokeStyle(Width * K, 141u + static_cast<uint32>(Seat), S, 0.f);
		if (M == ftcore::UiLeadMark::Knuckles)
		{
			// Four knuckles on one bar (no second line: it read as a mouth).
			for (int32 I = 0; I < 4; ++I)
			{
				ftcore::uiStrokeArc(Line, X + (static_cast<float>(I) - 1.5f) * 6.f * U, Y - 1.f * U, 3.f * U, -90.f, 90.f, Pen);
				Mesh.Stroke(Line, C);
			}
			ftcore::uiStrokeLine(Line, X - 12.f * U, Y, X + 12.f * U, Y, Pen);
			Mesh.Stroke(Line, C);
		}
		else if (M == ftcore::UiLeadMark::Cylinder)
		{
			ftcore::uiStrokeArc(Line, X, Y, 12.f * U, 0.f, 360.f, Pen);
			Mesh.Stroke(Line, C, true, true);
			for (int32 I = 0; I < 6; ++I)
			{
				float DX = 0.f;
				float DY = 0.f;
				ftcore::uiArcPoint(X, Y, 7.f * U, I * 60.f, DX, DY);
				Mesh.Dot(DX, DY, 1.8f * U, C);
			}
		}
		else if (M == ftcore::UiLeadMark::Mask)
		{
			// A broken ring: one gap at the top and a crack running in from the gap's right end (no eyes, no face).
			ftcore::uiStrokeArc(Line, X, Y, 12.f * U, 25.f, 335.f, Pen);
			Mesh.Stroke(Line, C);
			float GX = 0.f;
			float GY = 0.f;
			ftcore::uiArcPoint(X, Y, 12.f * U, 25.f, GX, GY);
			const FVector2f Crack[4] = {FVector2f(GX, GY), FVector2f(GX - 3.5f * U, GY + 3.f * U), FVector2f(GX - 2.f * U, GY + 6.5f * U),
				FVector2f(GX - 6.f * U, GY + 9.f * U)};
			Poly(Crack, 4, 0.5f * Width * U, C);
		}
	}

	// The lead, bottom left: the ring as a table, HP as the hand-drawn line round it (what was just lost rubbed out to a
	// faint trace), stamina a hairline inside, Load the accent line outside with a tick at the Release place; the
	// numbers and heals in the open bottom. No state words: the verbs say what can be done.
	void PaintLead(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::ArenaHudLead& L = View.now.lead;
		const ftcore::UiStyle& St = Table().Style;
		if (Budget.out.allowed(ftcore::UiMarkKind::Lead) == 0)
		{
			return; // the result card stands alone
		}
		const float S = P.S * Carry::LeadScale; // the whole block at the sheet's share of the frame
		const float A = LeadFade.alpha;
		const float TA = TextAlpha(A);
		const float DA = A * RestA;   // the details: gone at rest, the ring faded to its mark
		const float DTA = TA * RestA;
		const float CX = Carry::LeadX * S;
		const float CY = Size.Y - Carry::LeadUp * S;
		// In action the lines take the sheet's action weight; they thin back as the ring fades to rest.
		const float Act = FMath::Clamp((A - St.fade.restAlpha) / FMath::Max(1.f - St.fade.restAlpha, 0.01f), 0.f, 1.f);
		const auto W = [Act](float Rest, float Action) { return FMath::Lerp(Rest, Action, Act); };
		// No shade behind the block: the glyph edge carries the words, and one wide, weak darkening of the whole corner (a
		// lens vignette with no boundary) steadies them on a bright sky.
		Mesh.Vignette(0.f, Size.Y, Size.X * 0.46f, Size.Y * 0.72f, St.backingSoftHud * FMath::Max(A, 0.6f));

		Ring(CX, CY, Carry::RingR * S, W(2.1f, 3.4f), 11, S, Tok(UiToken::Ink, 0.9f * A));
		LeadMark(CX, CY + 8.f * S, 1.5f, L.lead, W(1.5f, 2.1f), S, Tok(UiToken::Ink, 0.9f * A));

		const float Hp = L.hpMax > 0 ? FMath::Clamp(static_cast<float>(L.hp) / L.hpMax, 0.f, 1.f) : 0.f;
		const bool bLow = Hp <= Carry::LowHp;
		const FLinearColor HpC = bLow ? Tok(UiToken::Danger, A) : Tok(UiToken::Ink, A);
		Arc(CX, CY, Carry::HpR * S, 0.f, Hp, StrokeStyle(W(5.6f, 7.6f), 21, S), HpC);
		if (HpLost.Was > Hp + 0.004f)
		{
			ftcore::UiStrokeStyle Rub = StrokeStyle(1.6f, 21, S, 0.f);
			Rub.broken = true;
			Arc(CX, CY, Carry::HpR * S, Hp, HpLost.Was, Rub, Tok(UiToken::Ink, 0.22f * A), false);
		}
		Arc(CX, CY, Carry::StaminaR * S, 0.f, FMath::Clamp(L.stamina01, 0.f, 1.f), StrokeStyle(W(1.6f, 2.4f), 23, S), Tok(UiToken::InkDim, 0.8f * DA), false);

		if (L.loadMax > 0)
		{
			const float Load = FMath::Clamp(static_cast<float>(L.load) / L.loadMax, 0.f, 1.f);
			// The track runs to the cap (the named chairs' limit); past it the arc is not drawn, and a short cross-stroke
			// closes it.
			const float Cap = L.loadCap > 0 ? FMath::Clamp(static_cast<float>(L.loadCap) / L.loadMax, 0.f, 1.f) : 1.f;
			Arc(CX, CY, Carry::LoadR * S, 0.f, Cap, StrokeStyle(W(1.f, 1.8f), 31, S, 0.f), Tok(UiToken::Ink, W(0.28f, 0.4f) * DA), false);
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
				Poly(Pts, 2, 0.9f * S, Tok(UiToken::Ink, 0.8f * DA));
			}
			Arc(CX, CY, Carry::LoadR * S, 0.f, Load, StrokeStyle(W(4.2f, 5.6f), 33, S), L.overloaded ? Tok(UiToken::Danger, DA) : Tok(UiToken::Accent, DA));
			const int32 ReleaseAt = L.releaseAt > 0 ? L.releaseAt : ftcore::kArenaReleaseOpenLoad;
			const float Mark = FMath::Clamp(static_cast<float>(ReleaseAt) / L.loadMax, 0.f, 1.f);
			const float Deg = Carry::A0 + (Carry::A1 - Carry::A0) * Mark;
			float MX = 0.f;
			float MY = 0.f;
			float MX1 = 0.f;
			float MY1 = 0.f;
			ftcore::uiArcPoint(CX, CY, (Carry::LoadR + 4.f) * S, Deg, MX, MY);
			ftcore::uiArcPoint(CX, CY, (Carry::LoadR + 10.f) * S, Deg, MX1, MY1);
			const FVector2f Tick[2] = {FVector2f(MX, MY), FVector2f(MX1, MY1)};
			Poly(Tick, 2, 0.7f * S, Tok(UiToken::InkDim, 0.7f * DA)); // the Release place: a scale tick; the verb alone lights
			// Load stored: the arc's leading tip flares.
			const float TipDeg = Carry::A0 + (Carry::A1 - Carry::A0) * Load;
			float TX = 0.f;
			float TY = 0.f;
			ftcore::uiArcPoint(CX, CY, Carry::LoadR * S, TipDeg, TX, TY);
			EachMark(ftcore::UiEventMark::LoadTip, [&](const ftcore::UiLiveMark&, const ftcore::UiMarkPose& Po)
			{
				Mesh.Dot(TX, TY, 3.2f * Po.tip * S, Tok(UiToken::Accent, Po.alpha * A));
				Mesh.Dot(TX, TY, 1.4f * Po.tip * S, Tok(UiToken::Ink, Po.alpha * A));
			});
			P.Text(CX + 30.f * S, CY + 84.f * S - LeadAsc, LoadText, LeadFont, L.overloaded ? Tok(UiToken::Danger, DTA) : Tok(UiToken::Accent, DTA));
		}

		// The chairs: small seats set against the inside of the table's ring at its crown, one filled seat per named chair,
		// a clear gap between them so they count at a glance. Only named chairs reach the arena: an open or emptied place has
		// no field here, so none is drawn.
		for (int32 I = 0; I < L.chairCount; ++I)
		{
			const float Deg = (static_cast<float>(I) - 0.5f * (L.chairCount - 1)) * (L.chairCount > 4 ? 19.f : 23.f);
			float SX = 0.f;
			float SY = 0.f;
			ftcore::uiArcPoint(CX, CY, (Carry::RingR - 10.f) * S, Deg, SX, SY);
			const FVector2f C0(SX, SY);
			const FVector2f Seat[4] = {C0 + Rotated(-6.f * S, -3.f * S, Deg), C0 + Rotated(6.f * S, -3.f * S, Deg), C0 + Rotated(5.f * S, 3.f * S, Deg),
				C0 + Rotated(-5.f * S, 3.f * S, Deg)};
			Mesh.Fill(Seat, 4, Tok(UiToken::Ink, A));
		}

		// The numbers in the open bottom: HP to the left of the middle (a danger mark beside it when low), Load to the right.
		P.Text(CX - 30.f * S - HpTextW, CY + 84.f * S - LeadAsc, HpText, LeadFont, bLow ? Tok(UiToken::Danger, DTA) : Tok(UiToken::Ink, DTA));
		if (bLow)
		{
			Danger(CX - 30.f * S - HpTextW - 14.f * S, CY + 84.f * S - LeadAsc * 0.45f, 13.f * S, Tok(UiToken::Danger, DA));
		}
		// Heals: lozenges between the numbers, filled while held.
		for (int32 I = 0; I < L.healsMax; ++I)
		{
			const float X = CX + (static_cast<float>(I) - 0.5f * (L.healsMax - 1)) * 13.f * S;
			Lozenge(X, CY + 76.f * S, 4.5f * S, I < L.heals, S, Tok(UiToken::Ink, (I < L.heals ? 1.f : 0.6f) * DA));
		}
		// A block: a short flat tick across the stamina (guard) arc at its end, struck long and settling.
		{
			const float GDeg = Carry::A0 + (Carry::A1 - Carry::A0) * FMath::Clamp(L.stamina01, 0.f, 1.f);
			EachMark(ftcore::UiEventMark::GuardTick, [&](const ftcore::UiLiveMark&, const ftcore::UiMarkPose& Po)
			{
				float X0 = 0.f;
				float Y0 = 0.f;
				float X1 = 0.f;
				float Y1 = 0.f;
				ftcore::uiArcPoint(CX, CY, (Carry::RingR + 2.f) * S, GDeg, X0, Y0);
				ftcore::uiArcPoint(CX, CY, (Carry::RingR + 2.f + 11.f * Po.tip) * S, GDeg, X1, Y1);
				const FVector2f Pts[2] = {FVector2f(X0, Y0), FVector2f(X1, Y1)};
				Poly(Pts, 2, 1.3f * S, Tok(UiToken::Ink, Po.alpha * A));
			});
		}

		// The party's places above the lead, only where the fight has a party member (no data, no place): in action the
		// partner's ring with its mark and its HP round it, joined by a fine thread; at rest a dot.
		const ftcore::ArenaHudPartner& Pa = View.now.partner;
		if (Pa.present && Budget.out.allowed(ftcore::UiMarkKind::Party) > 0)
		{
			const float PX = CX - 6.f * S;
			const float PY = CY - 140.f * S;
			const FLinearColor DotC = Pa.down ? Tok(UiToken::Danger, A) : Tok(UiToken::Ink, A);
			Mesh.Dot(PX, PY, 5.f * S, FLinearColor(DotC.R, DotC.G, DotC.B, DotC.A * (1.f - RestA)));
			if (DA > 0.01f)
			{
				const FVector2f Thread0[2] = {FVector2f(CX - 1.f * S, CY - (Carry::LoadR + 8.f) * S), FVector2f(PX, PY + 31.f * S)};
				Poly(Thread0, 2, 0.5f * S, Tok(UiToken::Ink, 0.55f * DA));
				Ring(PX, PY, 20.f * S, W(1.4f, 2.f), 61, S, Tok(UiToken::Ink, DA));
				LeadMark(PX, PY + 1.f * S, 0.85f, Pa.lead, W(1.6f, 2.2f), S, Tok(UiToken::Ink, 0.9f * DA));
				const float F = Pa.hpMax > 0 ? FMath::Clamp(static_cast<float>(Pa.hp) / Pa.hpMax, 0.f, 1.f) : 0.f;
				Arc(PX, PY, 27.f * S, 0.f, F, StrokeStyle(W(2.2f, 3.f), 62, S), Pa.down ? Tok(UiToken::Danger, DA) : Tok(UiToken::Ink, DA));
			}
			// The partner's kept Load: a thin accent arc outside its HP (none without a cap), its tip flaring when Load is kept.
			if (PartnerLoad01 >= 0.f)
			{
				Arc(PX, PY, 33.f * S, 0.f, 1.f, StrokeStyle(0.8f, 63, S, 0.f), Tok(UiToken::Ink, 0.25f * DA), false);
				Arc(PX, PY, 33.f * S, 0.f, PartnerLoad01, StrokeStyle(W(1.6f, 2.2f), 64, S), Tok(UiToken::Accent, FMath::Max(DA, 0.6f * A)), false);
				const float Flare = PartnerTipAge >= 0.f && PartnerTipAge < 0.6f ? 1.f - PartnerTipAge / 0.6f : 0.f;
				if (Flare > 0.f)
				{
					float TX = 0.f;
					float TY = 0.f;
					ftcore::uiArcPoint(PX, PY, 33.f * S, Carry::A0 + (Carry::A1 - Carry::A0) * PartnerLoad01, TX, TY);
					Mesh.Dot(TX, TY, (1.5f + 1.5f * Flare) * S, Tok(UiToken::Accent, Flare * A));
					Mesh.Dot(TX, TY, 1.2f * S, Tok(UiToken::Ink, Flare * A));
				}
			}
		}
	}

	// The top: the boss as one fine line with its HP drawn over it and phase notches; a plain target as a shorter line
	// under its word. A second target while a boss fights: a short line below.
	void PaintTop(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		using ftcore::UiMarkKind;
		// The boss line comes and goes with the boss (the budget's boss fade); a plain target's line goes at rest.
		const bool bBossLine = B.present && Budget.out.allowed(UiMarkKind::Boss) > 0;
		const bool bTargetLine = !B.present && T.present && Budget.out.allowed(UiMarkKind::Target) > 0;
		if (!bBossLine && !bTargetLine)
		{
			return;
		}
		const ftcore::UiStyle& St = Table().Style;
		const float S = P.S;
		const float A = B.present ? Budget.out.bossAlpha : TopFade.alpha * RestA;
		const float TA = B.present ? A : TextAlpha(TopFade.alpha) * RestA;
		const float CX = Size.X * 0.5f;
		const float Y = Carry::TopLine * S;
		const float Len = Carry::TopLen * S;
		const float X0 = CX - Len * 0.5f;
		const float Hp = (B.present ? B.hpMax : T.hpMax) > 0
			? FMath::Clamp(static_cast<float>(B.present ? B.hp : T.hp) / (B.present ? B.hpMax : T.hpMax), 0.f, 1.f)
			: 0.f;
		Mesh.Vignette(CX, 0.f, Size.X * 0.42f, Size.Y * 0.3f, St.backingSoftHud * A);

		// The track: a hairline only where the line is not (from its end to the full length), so it never shows as a second
		// line under the stroke.
		ftcore::UiStrokeStyle Hair = StrokeStyle(1.f, 99, S, 0.f);
		Hair.wobble = 0.3f * S;
		const float LA = A;
		HLine(X0, X0 + Len, Y, Hp, 1.f, Hair, Tok(UiToken::Ink, 0.8f * LA));
		ftcore::UiStrokeStyle HpSt = StrokeStyle(B.present ? 7.f : 3.8f, 101, S, 0.1f);
		if (B.present && Hp > 0.002f)
		{
			// One soft dark blur centred on the drawn line (no edge, one piece: it cannot split from the stroke); it holds the
			// line on a bright sky.
			const float Drawn = Len * Hp;
			Mesh.Vignette(X0 + Drawn * 0.5f, Y, FMath::Max(Drawn * 0.62f, 24.f * S), 15.f * S, 0.42f * LA);
		}
		// A stagger: the line breaks open at the phase notch nearest below its end (or at its middle), the right piece knocked
		// aside a little.
		float BreakAt = -1.f;
		if (B.present && BreakK > 0.01f && Hp > 0.02f)
		{
			BreakAt = Hp * 0.5f;
			for (int32 I = 0; I + 1 < B.phaseCount; ++I)
			{
				const float E = B.phaseEnd[static_cast<size_t>(I)];
				if (E < Hp - 0.01f && E > 0.01f)
				{
					BreakAt = E;
					break;
				}
			}
		}
		if (BreakAt > 0.f)
		{
			const float Gap = 0.018f * BreakK;
			HLine(X0, X0 + Len, Y, 0.f, FMath::Max(BreakAt - Gap, 0.f), HpSt, Tok(UiToken::Ink, LA));
			HLine(X0, X0 + Len, Y + 2.f * S * BreakK, BreakAt + Gap, Hp, HpSt, Tok(UiToken::Ink, LA));
		}
		else
		{
			HLine(X0, X0 + Len, Y, 0.f, Hp, HpSt, Tok(UiToken::Ink, LA));
		}
		if (B.present)
		{
			// The line starts from a small drawn oval (the dialogue's portrait oval in small), open where the line leaves it.
			ftcore::uiStrokeArc(Line, X0 - 12.f * S, Y, 9.f * S, 120.f, 420.f, StrokeStyle(1.6f, 109, S, 0.2f));
			for (uint32 I = 0; I < Line.count; ++I)
			{
				Line.pt[I].y = Y + (Line.pt[I].y - Y) * 0.7f; // flatten the circle to an oval
			}
			Mesh.Stroke(Line, Tok(UiToken::Ink, A));
		}
		if (TopLost.Was > Hp + 0.004f)
		{
			ftcore::UiStrokeStyle Rub = StrokeStyle(1.6f, 101, S, 0.f);
			Rub.broken = true;
			HLine(X0, X0 + Len, Y, Hp, TopLost.Was, Rub, Tok(UiToken::Ink, 0.4f * LA), false);
		}
		// The name over the line in spaced small caps. An unnamed target shows no name: the line alone.
		if (bNamed)
		{
			const float Base = Y - 22.f * S;
			P.Text(CX - NameW * 0.5f, Base - NameAsc, NameText, NameFont, Tok(UiToken::Ink, TA));
		}
		// A notch across the line at each phase's floor, over a dark under-stroke so it holds on a bright sky; the phases
		// already passed step back.
		for (int32 I = 0; B.present && I + 1 < B.phaseCount; ++I)
		{
			const float X = X0 + Len * B.phaseEnd[static_cast<size_t>(I)];
			const FVector2f Pts[2] = {FVector2f(X, Y - 16.f * S), FVector2f(X, Y + 16.f * S)};
			Poly(Pts, 2, 3.2f * S, Tok(UiToken::Backing, 0.35f * LA));
			Poly(Pts, 2, 1.8f * S, Tok(I < B.phase ? UiToken::InkDim : UiToken::Ink, (I < B.phase ? 0.55f : 1.f) * LA));
		}
		// The target's poise under the line (the boss's own when the target is the boss).
		if (T.present && (!B.present || T.enemy == B.slot))
		{
			HLine(X0, X0 + Len, Y + 12.f * S, 0.f, FMath::Clamp(T.poise01, 0.f, 1.f), StrokeStyle(1.4f, 103, S), Tok(UiToken::InkDim, A), false);
		}
		if (B.present && B.weightMax > 0)
		{
			// The weight as notches alone (no word), rising from the line at its left end.
			for (int32 I = 0; I < B.weightMax; ++I)
			{
				const float X = X0 + 4.f * S + I * 9.f * S;
				const bool bHeld = I < B.weight;
				const float H = (bHeld ? 12.f : 5.f) * S;
				const FVector2f Pts[2] = {FVector2f(X, Y - 5.f * S - H), FVector2f(X, Y - 5.f * S)};
				Poly(Pts, 2, 1.4f * S, bHeld ? Tok(UiToken::Accent, A) : Tok(UiToken::InkDim, 0.6f * A));
			}
		}
		// The boss's state as one mark past the line's right end (the call word keeps the row under the line).
		const float MX = X0 + Len + 16.f * S;
		const float MY = Y;
		if (B.present && B.weakPoint == ftcore::UiWeakPoint::Open)
		{
			const float Br = (0.85f + 0.15f * FMath::Sin(Time * 6.f)) * A;
			Lozenge(MX, MY, 6.f * S, true, S, Tok(UiToken::WeakPoint, Br));
		}
		else if (B.present && B.invulnerable)
		{
			// Shut: a square mark.
			const float R = 5.f * S;
			const FVector2f Pts[5] = {FVector2f(MX - R, MY - R), FVector2f(MX + R, MY - R), FVector2f(MX + R, MY + R), FVector2f(MX - R, MY + R),
				FVector2f(MX - R, MY - R)};
			Poly(Pts, 5, 0.7f * S, Tok(UiToken::Ink, A), true);
		}
		else if (B.present && B.down)
		{
			Lozenge(MX, MY, 6.f * S, false, S, Tok(UiToken::Ink, A));
		}
		PaintCall(P, Size, Y + 54.f * S, TA);
	}

	// The call word: one large near-white word in one fixed place under the boss name, when the boss calls a move that needs
	// the player's answer (a called zone, or the answer its move asks for), with its mark beside it.
	void PaintCall(const FPainter& P, const FVector2f& Size, float Base, float TA) const
	{
		if (CallKind < 0 || TA <= 0.01f)
		{
			return;
		}
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const float S = P.S;
		const float CX = Size.X * 0.5f;
		const bool bZone = CallKind < 3;
		const int32 I = bZone ? CallKind : CallKind - 3;
		const FString& Word = bZone ? ZoneWord[I] : AnswerWord[I];
		const float WW = bZone ? ZoneW[I] : AnswerW[I];
		const float Top = Base - 14.f * S;
		const float MidY = Top + CallAsc * 0.55f;
		Mesh.Vignette(CX, MidY, WW * 0.75f + 30.f * S, CallAsc * 0.9f, 0.4f * TA);
		const float TX = CX - WW * 0.5f;
		const float MX = TX - 20.f * S;
		if (bZone)
		{
			if (B.callIn)
			{
				P.Circle(MX, MidY, 13.f * S, Tok(UiToken::Ally, TA));
			}
			else
			{
				Danger(MX, MidY, 15.f * S, Tok(UiToken::Danger, TA));
			}
		}
		else if (I == 0)
		{
			Lozenge(MX, MidY, 7.f * S, true, S, Tok(UiToken::WeakPoint, TA));
		}
		else if (I == 2)
		{
			Danger(MX, MidY, 15.f * S, Tok(UiToken::Danger, TA));
		}
		P.Text(TX, Top, Word, CallFont, Tok(UiToken::Ink, TA));
	}

	// The second hp line under the boss's: the target, when it is not the boss. Its own grant and fade (the boss line may be
	// gone at rest while an add is still targeted).
	void PaintSecondLine(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		if (!(B.present && T.present && T.enemy != B.slot) || Budget.out.allowed(ftcore::UiMarkKind::Target) == 0)
		{
			return;
		}
		const float S = P.S;
		const float A = TopFade.alpha * RestA;
		const float CX = Size.X * 0.5f;
		const float Y = Carry::TopLine * S;
		const float OY = Y + 96.f * S;
		const float OLen = 300.f * S;
		const float OX = CX - OLen * 0.5f;
		const float OHp = T.hpMax > 0 ? FMath::Clamp(static_cast<float>(T.hp) / T.hpMax, 0.f, 1.f) : 0.f;
		HLine(OX, OX + OLen, OY, OHp, 1.f, StrokeStyle(1.f, 105, S, 0.f), Tok(UiToken::Ink, 0.3f * A), false);
		HLine(OX, OX + OLen, OY, 0.f, OHp, StrokeStyle(2.4f, 106, S, 0.04f), Tok(UiToken::Ink, A));
		HLine(OX, OX + OLen, OY + 9.f * S, 0.f, FMath::Clamp(T.poise01, 0.f, 1.f), StrokeStyle(1.2f, 107, S), Tok(UiToken::InkDim, A), false);
	}

	// The verbs bottom right: only the verbs that can be used right now, stacked without gaps on a hairline rule; the one the
	// fight prompts (lit in the accent, a mark at the rule) is the only one with its key in a ring. The list goes at rest.
	void PaintVerbs(const FPainter& P, const FVector2f& Size) const
	{
		const ftcore::UiBudgetOut& G = Budget.out;
		const float A = G.verbAlpha;
		if (G.verbsShown == 0 || A <= 0.01f)
		{
			return;
		}
		const ftcore::UiStyle& St = Table().Style;
		const float S = P.S;
		const float RX = Size.X - Carry::VerbIn * S;
		const float Y0 = Size.Y - Carry::VerbUp * S;
		int32 Rows = 0;
		for (int32 I = 0; I < 4; ++I)
		{
			Rows += ((G.verbsShown >> I) & 1u) && !Verbs[I].Keys.IsEmpty() ? 1 : 0;
		}
		if (Rows == 0)
		{
			return;
		}
		// Stacked from the bottom row up, so the list's foot stays put as verbs come and go.
		const float YTop = Y0 + (3 - (Rows - 1)) * Carry::VerbStep * S;
		Mesh.Vignette(Size.X, Size.Y, Size.X * 0.46f, Size.Y * 0.72f, St.backingSoftHud * A);
		ftcore::UiStrokeStyle Rule = StrokeStyle(1.f, 121, S, 0.2f);
		Rule.endAlpha = 0.f;
		ftcore::uiStrokeLine(Line, RX, YTop - 50.f * S, RX, Y0 + (3.f * Carry::VerbStep + 34.f) * S, Rule);
		Mesh.Stroke(Line, Tok(UiToken::Ink, 0.75f * A));
		const float WA = TextAlpha(A);
		int32 Row = 0;
		for (int32 I = 0; I < 4; ++I)
		{
			const FVerbSlot& V = Verbs[I];
			if (!((G.verbsShown >> I) & 1u) || V.Keys.IsEmpty())
			{
				continue;
			}
			const float Y = YTop + Row * Carry::VerbStep * S; // the word's baseline
			++Row;
			const bool bHot = G.keyRing == I;
			const FLinearColor WC = bHot ? WordTok(UiToken::Accent, WA) : Tok(UiToken::Ink, WA);
			const float KX = RX - 50.f * S;
			const float KY = Y - 9.f * S;
			const float KW = V.Keys.Len() <= 1 ? 38.f * S : FMath::Max(38.f * S, V.KeysW + 28.f * S);
			if (bHot)
			{
				KeyCapStroke(Line, KX, KY, KW, 38.f * S, 0.75f * S, 7.f * S);
				Mesh.Stroke(Line, Tok(UiToken::Accent, A), true, true);
				// Points out past the rule, as on the sheet.
				const FVector2f Pts[3] = {FVector2f(RX + 11.f * S, Y - 16.f * S), FVector2f(RX + 19.f * S, Y - 8.f * S), FVector2f(RX + 11.f * S, Y)};
				Poly(Pts, 3, 0.9f * S, Tok(UiToken::Accent, A));
			}
			if (I == 0 && TakeHalfAge < 0.6f)
			{
				// Took half: a fine accent ring opens out from the Guard key and fades.
				const float T = TakeHalfAge / 0.6f;
				KeyCapStroke(Line, KX, KY, KW + 16.f * T * S, (38.f + 16.f * T) * S, 0.8f * S, (7.f + 8.f * T) * S);
				Mesh.Stroke(Line, Tok(UiToken::Accent, (1.f - T) * A), true, true);
			}
			// Centred on the cap height (the serif's capitals are 0.62 of its ascent), not on the line box.
			P.Text(KX - V.KeysW * 0.5f, KY - LabelAsc * 0.69f, V.Keys, LabelFont, bHot ? WC : Tok(UiToken::InkDim, WA));
			P.Text(KX - KW * 0.5f - 18.f * S - V.WordW, Y - VerbAsc, V.Word, VerbFont, WC);
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
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
		// The wall: how near (within the old cue's 3.5 m band) and which way on screen, down the zone's distance field.
		WallA = 0.f;
		if (PC && PC->PlayerCameraManager)
		{
			const ftcore::ArenaZone& Zone = Runner.GetZone();
			const ftcore::Vec2 Me = Runner.GetPlayerPosition();
			const float Near = Zone.wallDistance(Me);
			if (Near < kWallBand)
			{
				constexpr float E = 0.25f;
				const float GX = Zone.wallDistance(ftcore::Vec2{Me.x + E, Me.y}) - Zone.wallDistance(ftcore::Vec2{Me.x - E, Me.y});
				const float GY = Zone.wallDistance(ftcore::Vec2{Me.x, Me.y + E}) - Zone.wallDistance(ftcore::Vec2{Me.x, Me.y - E});
				const FVector Dir(-GX, -GY, 0.0); // towards less room: the wall
				if (!Dir.IsNearlyZero())
				{
					const FVector Local = FRotator(0.0, PC->PlayerCameraManager->GetCameraRotation().Yaw, 0.0).UnrotateVector(Dir);
					WallDeg = static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(Local.Y, Local.X))); // 0 ahead (up), clockwise
					WallA = FMath::Clamp(1.f - Near / kWallBand, 0.f, 1.f);
				}
			}
		}
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
			// Out of the camera's view (the runner's off-frame test, the one the stinger answers to), or off the screen.
			const bool bOffFrame = bStartup && Runner.IsOffFrame(I);
			if (!M.bOn || bOffFrame)
			{
				M.bEdge = true;
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
		// The weak point while it is open: on the boss's body itself.
		bWeakOn = false;
		if (B.present && B.weakPoint == ftcore::UiWeakPoint::Open)
		{
			FVector Weak;
			bWeakOn = Runner.GetBossWeakPoint(static_cast<int32>(B.slot), Weak) && Project(PC, Weak, Size, WeakAt) && OnScreen(WeakAt);
		}
		// The lock: on the locked body while it is on the screen.
		bLockOn = false;
		const uint8 Lock = Fight.lock();
		if (Lock != ftcore::kArenaNobody)
		{
			const AFTArenaFighter* Figure = Runner.GetEnemyFigure(static_cast<int32>(Lock));
			bLockOn = Figure && Project(PC, Figure->GetChest(), Size, LockAt) && OnScreen(LockAt);
		}
		LockSlot = Lock;
		bLockSeen = bLockOn; // where the reticle sits (the budget may still not grant the lock's own ring)
		// The standing stagger: the locked body staggered or open, or the boss's weak point open (the boss locked, or no lock).
		bHold = (Lock < UFTArenaRunner::MaxEnemies && Foes[Lock].bShow && Foes[Lock].Word != 0)
			|| (B.present && B.weakPoint == ftcore::UiWeakPoint::Open && (Lock == B.slot || Lock == ftcore::kArenaNobody));
		// The call word: a called zone, else the answer the boss's winding-up move needs.
		CallKind = -1;
		if (B.present && !B.invulnerable && !B.down)
		{
			if (B.call != ftcore::UiCalledZone::None)
			{
				CallKind = FMath::Clamp(static_cast<int32>(B.call) - 1, 0, 2);
			}
			else if (B.slot < UFTArenaRunner::MaxEnemies && Foes[B.slot].bShow && Foes[B.slot].Tell == 1)
			{
				CallKind = 3 + FMath::Min<int32>(Foes[B.slot].Answer, 2);
			}
		}
	}

	// The fight's events this frame: each new event's one mark (Core's table: no event draws a word), the edge hairlines
	// of a hit or a parry, the partner's kept Load, the result and the autoplay tag.
	void TickEvents(const UFTArenaRunner& Runner, const ftcore::ArenaFight& Fight, const APlayerController* PC, const FVector2f& Size,
		float DeltaTime)
	{
		(void)PC;
		(void)Size;
		const double Now = Runner.GetNow();
		const int32 Count = Runner.GetPopupCount();
		const int32 First = Runner.GetPopupTotal() - Count;
		ftcore::uiMarkTick(Marks, DeltaTime);
		bMarkNew = false;
		if (Fight.over())
		{
			ftcore::uiMarkClear(Marks);
			MarkPushed = Runner.GetPopupTotal();
		}
		else
		{
			// A shared blow's struck lead, from who took the shares of it this frame.
			bool bToPlayer = false;
			bool bToAlly = false;
			for (int32 Serial = FMath::Max(MarkPushed, First); Serial < First + Count; ++Serial)
			{
				const UFTArenaRunner::FPopup& Pp = Runner.GetPopup(Serial - First);
				if (Pp.Type == ftcore::ArenaEventType::TableShare)
				{
					bToPlayer |= Pp.Target == ftcore::kArenaPlayer;
					bToAlly |= Pp.Target == ftcore::kArenaAlly;
				}
			}
			const uint8 Struck = ftcore::uiShareStruck(Fight.tableSeats(), bToPlayer, bToAlly);
			for (int32 Serial = FMath::Max(MarkPushed, First); Serial < First + Count; ++Serial)
			{
				const UFTArenaRunner::FPopup& Pp = Runner.GetPopup(Serial - First);
				uint8 From = Pp.Source;
				uint8 To = Pp.Target;
				switch (Pp.Type)
				{
				case ftcore::ArenaEventType::TableShare:
					From = Struck;
					if (From == ftcore::kArenaNobody || From == To)
					{
						continue; // no struck body to draw from
					}
					break;
				case ftcore::ArenaEventType::TookHalf:
					From = ftcore::kArenaAlly;
					To = ftcore::kArenaPlayer;
					break;
				case ftcore::ArenaEventType::Staggered:
				case ftcore::ArenaEventType::CounterStagger:
					if (To >= UFTArenaRunner::MaxEnemies)
					{
						continue; // the lines and the reticle are the enemies'
					}
					break;
				default:
					break;
				}
				bMarkNew |= ftcore::uiMarkPush(Marks, Pp.Type, From, To, Pp.Amount);
			}
			MarkPushed = First + Count;
			TickStrip();
		}
		// The stagger's break on the boss line: a live break on the boss, else held open while it stands staggered or open.
		BreakK = 0.f;
		const ftcore::ArenaHudBoss& B = View.now.boss;
		if (B.present)
		{
			for (const ftcore::UiLiveMark& M : Marks.m)
			{
				if (M.live && M.mark == ftcore::UiEventMark::LineBreak && M.to == B.slot)
				{
					const ftcore::UiMarkPose Po = ftcore::uiEventMarkPose(M.mark, M.age);
					BreakK = FMath::Max(BreakK, Po.tip * Po.alpha);
				}
			}
			if (B.weakPoint == ftcore::UiWeakPoint::Open || (B.slot < UFTArenaRunner::MaxEnemies && Foes[B.slot].bShow && Foes[B.slot].Word != 0))
			{
				BreakK = 1.f;
			}
		}
		// The partner's kept Load (none without a cap).
		const int32 PCap = Runner.GetPartnerLoadCap();
		PartnerLoad01 = PCap > 0 ? FMath::Clamp(static_cast<float>(Runner.GetPartnerLoad()) / PCap, 0.f, 1.f) : -1.f;
		PartnerTipAge = static_cast<float>(Now - Runner.GetPartnerLoadTime());
		HitA = Pulse(Now, Runner.GetPlayerHitTime(), kHitEdgeLife);
		ParryA = Pulse(Now, Runner.GetParryTime(), kParryEdgeLife);
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
				StatsText = FString::Format(*kStatTimeUi.Get(), {FString::Printf(TEXT("%.1f"), Fight.time())});
				const int32 Counts[6] = {static_cast<int32>(St.hits), static_cast<int32>(St.parries), static_cast<int32>(St.ripostes),
					static_cast<int32>(St.evades), static_cast<int32>(St.blocks), static_cast<int32>(St.hitsTaken)};
				for (int32 I = 0; I < 6; ++I)
				{
					StatsText += TEXT("    ");
					StatsText += FString::Format(*kStatUi[I][Counts[I] == 1 ? 0 : 1]->Get(), {Counts[I]}); // the word by the count
				}
				StatsW = TextWidth(StatsText, LabelFont);
				bStatsBuilt = true;
			}
			bWon = Fight.phase() == ftcore::ArenaPhase::Victory;
			ResultA = FMath::Clamp(static_cast<float>(Runner.GetResultAge() / kResultFade), 0.f, 1.f);
			bAgain = !Runner.IsAutoPlay() && Runner.CanFightAgain();
		}
		static const bool bShotRun = FParse::Param(FCommandLine::Get(), TEXT("ArenaShotUi")); // review stills carry no debug tag
		TagMode = bShotRun ? 0 : (Runner.IsIdle() ? 2 : (Runner.IsAutoPlay() ? 1 : 0));
	}

	// -FTNewUiStrip=<path prefix>: the marks' motion strips for review. The first live mark of each kind is held at fixed
	// ages, one still a step (<prefix>_<mark>_<age in ms>.png), while the fight runs on under it; then it ages on as usual.
	void TickStrip()
	{
		static FString Prefix;
		static const bool bStrip = FParse::Value(FCommandLine::Get(), TEXT("FTNewUiStrip="), Prefix);
		if (!bStrip)
		{
			return;
		}
		static constexpr float kAges[] = {0.f, // fractions of the mark's life
			0.04f, 0.08f, 0.12f, 0.2f, 0.3f, 0.45f, 0.6f, 0.8f, 0.95f};
		static const TCHAR* const kNames[ftcore::kUiEventMarkCount] = {
			TEXT("none"), TEXT("ringsnap"), TEXT("guardtick"), TEXT("ringopen"), TEXT("linebreak"), TEXT("sharethread"), TEXT("loadtip"),
			TEXT("readtick")};
		if (StripSlot < 0)
		{
			for (int32 I = 0; I < static_cast<int32>(ftcore::kUiLiveMarkMax); ++I)
			{
				const ftcore::UiLiveMark& M = Marks.m[I];
				if (M.live && !(StripDone & (1u << static_cast<uint32>(M.mark))))
				{
					StripSlot = I;
					StripKind = M.mark;
					StripStep = 0;
					StripWait = 0;
					break;
				}
			}
			if (StripSlot < 0)
			{
				return;
			}
		}
		ftcore::UiLiveMark& M = Marks.m[StripSlot];
		const uint32 Bit = 1u << static_cast<uint32>(StripKind);
		if (M.mark != StripKind)
		{
			StripDone |= Bit; // its slot was taken: the strip ends where it got to
			StripSlot = -1;
			return;
		}
		if (StripStep > 0)
		{
			M.live = true;
			M.age = StripAge;
		}
		if (FScreenshotRequest::IsScreenshotRequested() || ++StripWait < 3)
		{
			return;
		}
		const float Life = ftcore::uiEventMarkSpec(StripKind).life;
		if (StripStep >= static_cast<int32>(UE_ARRAY_COUNT(kAges)))
		{
			StripDone |= Bit;
			StripSlot = -1;
			return;
		}
		StripAge = kAges[StripStep] * Life;
		M.live = true;
		M.age = StripAge;
		const FString File = FString::Printf(TEXT("%s_%s_%04d.png"), *Prefix, kNames[static_cast<int32>(StripKind)],
			FMath::RoundToInt(StripAge * 1000.f));
		FScreenshotRequest::RequestScreenshot(File, true, false);
		UE_LOG(LogForgottenThird, Display, TEXT("FTNewUi strip: %s (from %d to %d)."), *File, M.from, M.to);
		++StripStep;
		StripWait = 0;
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
				// A thin hp line over a body the top line doesn't carry (its track only where the hp is not).
				const float W = 90.f * S;
				if (M.bLine)
				{
					HLine(M.Over.X - W * 0.5f, M.Over.X + W * 0.5f, M.Over.Y, M.Hp01, 1.f, StrokeStyle(0.6f, 71, S, 0.f), Tok(UiToken::Track), false);
					HLine(M.Over.X - W * 0.5f, M.Over.X + W * 0.5f, M.Over.Y, 0.f, M.Hp01, StrokeStyle(1.1f, 73, S, 0.f), Tok(UiToken::Ink, 0.9f));
					// Its poise, a finer line under the hp in the second accent.
					HLine(M.Over.X - W * 0.5f, M.Over.X + W * 0.5f, M.Over.Y + 5.f * S, 0.f, M.Poise01, StrokeStyle(0.7f, 75, S, 0.f), Tok(UiToken::Accent2, 0.85f));
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
			// Kept below the top line's block.
			const float Y = FMath::Max(M.Over.Y - 30.f * S, (Carry::TopLine + 80.f) * S);
			if (M.Tell == 1)
			{
				HLine(X0, X0 + W, Y, 0.f, 1.f, StrokeStyle(0.7f, 81, S, 0.f), Tok(UiToken::Track), false);
				HLine(X0, X0 + W, Y, 0.f, M.Fill, StrokeStyle(1.8f, 83, S, 0.f), Tok(AT));
				const FVector2f End[2] = {FVector2f(X0 + W, Y - 8.f * S), FVector2f(X0 + W, Y + 8.f * S)};
				Poly(End, 2, 0.9f * S, Tok(UiToken::Ink));
				AnswerMark(X0 - 14.f * S, Y, M.Answer, S, Tok(AT));
				// A ring closes in on the body, meeting it at the hit.
				Ring(M.Chest.X, M.Chest.Y, 38.f * S, 0.6f, 91, S, Tok(AT, 0.45f));
				Ring(M.Chest.X, M.Chest.Y, FMath::Lerp(150.f, 38.f, M.Fill) * S, 1.3f, 93, S, Tok(AT, 0.35f + 0.65f * M.Fill));
			}
			else
			{
				// The hit: the line in the ink, a ring spreading from the body.
				const float I = M.Impact;
				HLine(X0, X0 + W, Y, 0.f, 1.f, StrokeStyle(1.8f, 83, S, 0.f), Tok(UiToken::Ink, I));
				const float Spread = 1.f - I;
				Ring(M.Chest.X, M.Chest.Y, (38.f + 90.f * Spread) * S, 0.6f + 2.4f * I, 95, S, Tok(UiToken::Ink, I));
				Ring(M.Chest.X, M.Chest.Y, (30.f + 50.f * Spread) * S, 1.3f, 97, S, Tok(AT, I));
			}
		}
		if (bWeakOn)
		{
			// The weak point open on the body: a filled diamond and a ring round it, breathing, in the weak point's colour.
			const float Br = 0.5f + 0.5f * FMath::Sin(Time * 6.f);
			const FLinearColor C = Tok(UiToken::WeakPoint, 0.8f + 0.2f * Br);
			Lozenge(WeakAt.X, WeakAt.Y, 7.f * S, true, S, C);
			Ring(WeakAt.X, WeakAt.Y, (24.f + 4.f * Br) * S, 1.6f, 141, S, C);
		}
	}

	// The answer a tell asks for, as its shape alone (no word): a diamond to parry, a triangle to dodge, none to block.
	void AnswerMark(float X, float Y, uint8 Answer, float S, const FLinearColor& C) const
	{
		if (Answer == 2)
		{
			Danger(X, Y, 14.f * S, C);
		}
		else if (Answer == 0)
		{
			Lozenge(X, Y, 6.5f * S, true, S, C);
		}
	}

	// A ring of four arcs in a mark's pose round (X, Y) at rest radius R: each arc spans Span of its quarter, the ring trembling
	// by the pose's jitter; shut (one whole ring) at full span.
	void ReticleRing(float X, float Y, float R, const ftcore::UiMarkPose& Po, float S, float Width, const FLinearColor& C) const
	{
		if (C.A <= 0.005f)
		{
			return;
		}
		if (Po.span >= 0.995f)
		{
			Ring(X, Y, R * Po.radius, Width, 113, S, C);
			return;
		}
		for (int32 K = 0; K < 4; ++K)
		{
			const float Mid = 45.f + 90.f * K;
			const float Half = Po.span * 45.f;
			const float RK = R * Po.radius * (1.f + Po.jitter * 0.06f * FMath::Sin(Time * 53.f + K * 2.1f));
			ftcore::uiStrokeArc(Line, X, Y, RK, Mid - Half, Mid + Half, StrokeStyle(Width, 111 + K, S, 0.f));
			Mesh.Stroke(Line, C);
		}
	}

	// The weak point open: four short cracks pointing in from the ring's four gaps (the stagger's mark, unlike the rest ring).
	void StaggerCracks(float X, float Y, float R, float S, float A) const
	{
		if (A <= 0.005f)
		{
			return;
		}
		for (int32 K = 0; K < 4; ++K)
		{
			const float Deg = 90.f * K;
			float X0 = 0.f;
			float Y0 = 0.f;
			float X1 = 0.f;
			float Y1 = 0.f;
			ftcore::uiArcPoint(X, Y, R * 0.96f, Deg, X0, Y0);
			ftcore::uiArcPoint(X, Y, R * 0.7f, Deg, X1, Y1);
			const FVector2f Pts[2] = {FVector2f(X0, Y0), FVector2f(X1, Y1)};
			Poly(Pts, 2, 2.f * S, Tok(UiToken::WeakPoint, A));
		}
	}

	// The reticle: the lock's ring round the locked body (the screen's centre without a lock), where a parry snaps it shut
	// and rings once, an evade opens it and lets go, a stagger springs it open trembling (held while the weak point is open),
	// and the Read stacks stand as small ticks in its top gap.
	void PaintReticle(const FPainter& P, const FVector2f& Size) const
	{
		const float S = P.S;
		const FVector2f At = bLockSeen ? LockAt : FVector2f(Size.X * 0.5f, Size.Y * 0.5f);
		const float R0 = (58.f + (bLockSeen ? 5.f * FMath::Sin(Time * 5.f) : 0.f)) * S;
		const uint8 Lock = LockSlot;
		float Base = 1.f;
		float SnapA = 0.f;
		const auto Mine = [Lock](const ftcore::UiLiveMark& M) { return Lock == ftcore::kArenaNobody || M.to == Lock || M.from == Lock; };
		EachMark(ftcore::UiEventMark::RingSnap, [&](const ftcore::UiLiveMark&, const ftcore::UiMarkPose& Po)
		{
			ReticleRing(At.X, At.Y, R0, Po, S, 1.8f, Tok(UiToken::Ink, Po.alpha));
			if (Po.rippleAlpha > 0.f)
			{
				Ring(At.X, At.Y, R0 * Po.ripple, 1.4f, 117, S, Tok(UiToken::WeakPoint, Po.rippleAlpha));
			}
			Base = FMath::Min(Base, Po.base);
			SnapA = FMath::Max(SnapA, Po.alpha);
		});
		EachMark(ftcore::UiEventMark::RingOpen, [&](const ftcore::UiLiveMark&, const ftcore::UiMarkPose& Po)
		{
			ReticleRing(At.X, At.Y, R0, Po, S, 1.8f, Tok(UiToken::Ink, Po.alpha));
			Base = FMath::Min(Base, Po.base);
		});
		// A stagger: its live mark springs the ring open; the standing hold keeps it there (the mark's pose, settled, while
		// both are on).
		bool bBroke = false;
		EachMark(ftcore::UiEventMark::LineBreak, [&](const ftcore::UiLiveMark& M, const ftcore::UiMarkPose& Po)
		{
			if (bBroke || !Mine(M))
			{
				return;
			}
			bBroke = true;
			const float A = bHold ? 1.f : Po.alpha;
			ReticleRing(At.X, At.Y, R0, Po, S, 2.4f, Tok(UiToken::Accent, A * (1.f - SnapA)));
			StaggerCracks(At.X, At.Y, R0 * Po.radius, S, A * (1.f - SnapA));
			Base = FMath::Min(Base, bHold ? 0.f : Po.base + (1.f - Po.alpha));
		});
		if (bHold && !bBroke)
		{
			ReticleRing(At.X, At.Y, R0, ftcore::uiStaggerHoldPose(), S, 2.4f, Tok(UiToken::Accent, 1.f - SnapA));
			StaggerCracks(At.X, At.Y, R0 * ftcore::uiStaggerHoldPose().radius, S, 1.f - SnapA);
			Base = 0.f;
		}
		if (bLockOn && Base > 0.01f)
		{
			ftcore::UiMarkPose Rest;
			Rest.span = ftcore::kUiLockSpan;
			ReticleRing(At.X, At.Y, R0, Rest, S, 1.3f, Tok(UiToken::Ink, 0.9f * Base));
			Mesh.Dot(At.X, At.Y, 2.6f * S, Tok(UiToken::Ink, 0.9f * Base));
		}
		// The Read stacks: small ticks out from the ring in its top gap, the newest growing in.
		const int32 N = View.now.lead.read;
		if (N > 0 && RestA > 0.01f)
		{
			float Grow = 1.f;
			EachMark(ftcore::UiEventMark::ReadTick, [&](const ftcore::UiLiveMark&, const ftcore::UiMarkPose& Po) { Grow = FMath::Min(Grow, Po.tip); });
			for (int32 I = 0; I < N; ++I)
			{
				const float Deg = (static_cast<float>(I) - 0.5f * (N - 1)) * 8.f;
				const float Len = 9.f * S * (I == N - 1 ? Grow : 1.f);
				float X0 = 0.f;
				float Y0 = 0.f;
				float X1 = 0.f;
				float Y1 = 0.f;
				ftcore::uiArcPoint(At.X, At.Y, R0 + 4.f * S, Deg, X0, Y0);
				ftcore::uiArcPoint(At.X, At.Y, R0 + 4.f * S + Len, Deg, X1, Y1);
				const FVector2f Pts[2] = {FVector2f(X0, Y0), FVector2f(X1, Y1)};
				Poly(Pts, 2, 1.1f * S, Tok(UiToken::Accent, RestA));
			}
		}
	}

	// An off-frame tell: a filled head at a frame 70 px inside the screen's edge, toward the enemy, a ring round it (an onset
	// ring spreading as it starts), and toward the centre the answer's shape over the windup's filling line. At the hit, the ink.
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
		AnswerMark(In.X, In.Y - 12.f * S, M.Answer, S, C);
		const float BW = 96.f * S;
		HLine(In.X - BW * 0.5f, In.X + BW * 0.5f, In.Y + 6.f * S, 0.f, 1.f, StrokeStyle(0.6f, 105, S, 0.f), Tok(UiToken::Track), false);
		HLine(In.X - BW * 0.5f, In.X + BW * 0.5f, In.Y + 6.f * S, 0.f, M.Fill, StrokeStyle(1.5f, 107, S, 0.f), C);
	}

	// The events, over everything else.
	void PaintEvents(const FPainter& P, const FVector2f& Size) const
	{
		const float S = P.S;
		const float CX = Size.X * 0.5f;
		// A hit: a brief flash of the danger colour in from the screen's edges (under 0.4 s), each edge a soft lens falloff with
		// no inner edge or line anywhere. A parry has no frame: the reticle snapping shut says it.
		if (HitA > 0.f)
		{
			const FLinearColor C = Tok(UiToken::Danger, 1.f);
			const float Pk = 0.3f * HitA * HitA;
			const float D = 120.f * S;
			Mesh.Vignette(CX, 0.f, Size.X * 0.7f, D, Pk, &C);
			Mesh.Vignette(CX, Size.Y, Size.X * 0.7f, D, Pk, &C);
			Mesh.Vignette(0.f, Size.Y * 0.5f, D, Size.Y * 0.7f, Pk, &C);
			Mesh.Vignette(Size.X, Size.Y * 0.5f, D, Size.Y * 0.7f, Pk, &C);
		}
		// The wrong side: a danger mark beside the reticle on the side that was called, fading.
		const float WrongA = WrongAt < 0.0 ? 0.f : FMath::Clamp(1.f - static_cast<float>((FightNow - WrongAt) / kBossNoteLife), 0.f, 1.f);
		if (WrongA > 0.f && !bResult)
		{
			const float CY = Size.Y * 0.5f;
			const float Side = WrongCall == ftcore::UiCalledZone::Left ? -1.f : (WrongCall == ftcore::UiCalledZone::Right ? 1.f : 0.f);
			const FLinearColor C = Tok(UiToken::Danger, WrongA);
			if (Side != 0.f)
			{
				const float BX = CX + Side * 72.f * S;
				const FVector2f Pts[3] = {FVector2f(BX + Side * 16.f * S, CY), FVector2f(BX, CY - 11.f * S), FVector2f(BX, CY + 11.f * S)};
				Mesh.Fill(Pts, 3, C);
			}
			else
			{
				const float BY = CY + 72.f * S;
				const FVector2f Pts[3] = {FVector2f(CX, BY + 16.f * S), FVector2f(CX - 11.f * S, BY), FVector2f(CX + 11.f * S, BY)};
				Mesh.Fill(Pts, 3, C);
			}
		}
		// The wall near: a hairline arc in the accent along the screen's edge, on the wall's side.
		if (WallA > 0.01f)
		{
			const float MidY = Size.Y * 0.5f;
			const float RY = MidY - 34.f * S;
			const float RX = CX - 34.f * S;
			ftcore::uiStrokeArc(Line, CX, MidY, RY, WallDeg - 28.f, WallDeg + 28.f, StrokeStyle(1.8f, 131, S, 0.3f));
			for (uint32 I = 0; I < Line.count; ++I)
			{
				Line.pt[I].x = CX + (Line.pt[I].x - CX) * (RX / RY);
			}
			Mesh.Stroke(Line, Tok(UiToken::Accent, 0.9f * WallA));
		}
		// A shared blow: a thin thread drawn from the struck body's place to the body that took the share, pulsing once there.
		const float LS = S * Carry::LeadScale;
		const auto Place = [&](uint8 Who, FVector2f& Out) -> bool
		{
			if (Who == ftcore::kArenaPlayer)
			{
				Out = FVector2f(Carry::LeadX * LS, Size.Y - Carry::LeadUp * LS);
				return true;
			}
			if (Who == ftcore::kArenaAlly)
			{
				Out = FVector2f((Carry::LeadX - 6.f) * LS, Size.Y - (Carry::LeadUp + 140.f) * LS);
				return View.now.partner.present;
			}
			if (Who < UFTArenaRunner::MaxEnemies && Foes[Who].bShow && Foes[Who].bOn)
			{
				Out = Foes[Who].Chest;
				return true;
			}
			return false;
		};
		EachMark(ftcore::UiEventMark::ShareThread, [&](const ftcore::UiLiveMark& M, const ftcore::UiMarkPose& Po)
		{
			FVector2f From;
			FVector2f To;
			if (bResult || !Place(M.from, From) || !Place(M.to, To))
			{
				return;
			}
			const FVector2f Pts[2] = {From, From + (To - From) * Po.tip};
			Poly(Pts, 2, 0.6f * S, Tok(UiToken::Ink, 0.85f * Po.alpha));
			if (Po.rippleAlpha > 0.f)
			{
				Ring(To.X, To.Y, (10.f + 16.f * (1.f - Po.rippleAlpha)) * S, 1.4f, 159, S, Tok(UiToken::Danger, Po.rippleAlpha));
			}
		});
		// The result: the word, a hairline, the counts and how to fight again, over one weak dim of the whole screen.
		if (bResult && ResultA > 0.f)
		{
			const float A = ResultA;
			const float Base = Size.Y * 0.42f;
			const float WordW = bWon ? VictoryW : DefeatW;
			P.Box(0.f, 0.f, Size.X, Size.Y, Tok(UiToken::Backing, Table().Style.backingSoftHud * A), 0); // one even dim, no shape
			P.Text(CX - WordW * 0.5f, Base - BigAsc, bWon ? VictoryWord : DefeatWord, BigFont, WordTok(bWon ? UiToken::Accent : UiToken::Danger, A));
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
		if (L.roundsMax == 0 || Budget.out.allowed(ftcore::UiMarkKind::Rounds) == 0)
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
		const float R = 46.f * S;
		constexpr float Step = 19.f; // degrees between chambers
		const float A0 = 90.f - Step * 0.5f * (L.roundsMax - 1);
		const float A1 = 90.f + Step * 0.5f * (L.roundsMax - 1);
		for (int32 I = 0; I < L.roundsMax; ++I)
		{
			float X;
			float Y;
			ftcore::uiArcPoint(CX, CY, R, A0 + Step * I, X, Y);
			if (I < L.rounds)
			{
				Mesh.Dot(X, Y, 7.f * S, Tok(UiToken::Ink, A));
			}
			else
			{
				ftcore::uiStrokeArc(Line, X, Y, 6.f * S, 0.f, 360.f, StrokeStyle(1.4f, 63u + static_cast<uint32>(I), S, 0.f));
				Mesh.Stroke(Line, Tok(UiToken::InkDim, 0.7f * A), true, true);
			}
		}
		if (L.reload01 > 0.f)
		{
			ftcore::uiStrokeArc(Line, CX, CY, R + 13.f * S, A0 - Step * 0.5f, A1 + Step * 0.5f, StrokeStyle(1.2f, 61, S, 0.f), 0.f, FMath::Clamp(L.reload01, 0.f, 1.f));
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
	FSlateFontInfo LeadFont; // the lead's numbers, at the block's scale
	float LeadAsc = 0.f;
	float VerbAsc = 0.f;
	FString HpText;
	float HpTextW = 0.f;
	FString LoadText;
	// The top line's name, and the call word under it (a called zone, or the answer the boss's move needs).
	FSlateFontInfo NameFont;
	float NameAsc = 0.f;
	FString NameText;
	float NameW = 0.f;
	int32 NameSlot = -2; // the slot the name was built for (-1: none, -2: not built)
	bool bNamed = false;
	FSlateFontInfo CallFont;
	float CallAsc = 0.f;
	FString ZoneWord[3];
	float ZoneW[3] = {};
	FString AnswerWord[3];
	float AnswerW[3] = {};
	int32 CallKind = -1; // -1 none, 0..2 a called zone, 3 + the answer a boss's windup needs
	int32 NoteSeen = -2;
	double FightNow = 0.0;

	FFoeMark Foes[UFTArenaRunner::MaxEnemies];
	bool bLockOn = false;
	FVector2f LockAt = FVector2f::ZeroVector;
	bool bWeakOn = false;
	FVector2f WeakAt = FVector2f::ZeroVector;
	bool bLockSeen = false; // the lock projected on screen (the reticle's place), granted or not
	uint8 LockSlot = ftcore::kArenaNobody;
	bool bHold = false;     // a standing stagger: the reticle held open
	float BreakK = 0.f;     // the boss line's break (0 whole, 1 broken open)
	float PartnerLoad01 = -1.f; // the partner's kept Load against its cap (-1: none)
	float PartnerTipAge = 1e9f;
	FSlateFontInfo BigFont;
	float BigAsc = 0.f;
	// The element budget and the event marks (Core's table: one mark per event, never a word).
	ftcore::UiBudgetState Budget;
	ftcore::UiLiveMarks Marks;
	int32 StripSlot = -1; // -FTNewUiStrip: the mark held for its strip, its step, and the kinds done
	ftcore::UiEventMark StripKind = ftcore::UiEventMark::None;
	int32 StripStep = 0;
	int32 StripWait = 0;
	float StripAge = 0.f;
	uint32 StripDone = 1u; // the None kind has no strip
	int32 MarkPushed = 0; // runner events already turned into marks
	bool bMarkNew = false;
	uint16 MarkMask = 0;  // live marks the budget does not grant this frame
	float RestA = 1.f; // the lead's details: 1 in action, 0 at rest
	uint8 MarksLogged = 0;
	bool bRestLogged = false;
	bool bBossSeen = false;
	ftcore::ArenaHudBoss BossSeen;
	uint8 LockSeen = ftcore::kArenaNobody;
	ftcore::UiCalledZone LastCall = ftcore::UiCalledZone::None;
	ftcore::UiCalledZone WrongCall = ftcore::UiCalledZone::None;
	double WrongAt = -1.0;
	float HitA = 0.f, ParryA = 0.f;
	float WallA = 0.f;   // the wall's nearness (0 out of the band)
	float WallDeg = 0.f; // its side on screen: 0 up, clockwise
	FString VictoryWord, DefeatWord, AgainWord;
	float VictoryW = 0.f, DefeatW = 0.f, AgainW = 0.f;
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
		const bool bWasShown = Shown.IsValid();
		Shown = nullptr;
		if (!D)
		{
			return;
		}
		// The key hint: shown when the dialogue opens and on any input, gone after the style's quiet time.
		const double LastInput = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetLastUserInteractionTime() : 0.0;
		HintQuiet = (!bWasShown || LastInput != HintInputSeen) ? 0.f : HintQuiet + DeltaTime;
		HintInputSeen = LastInput;
		HintA = FMath::Clamp(1.f - (HintQuiet - Table().Style.budget.hint) / 0.6f, 0.f, 1.f);
		if ((HintA > 0.f) != bHintUp)
		{
			bHintUp = HintA > 0.f;
			UE_LOG(LogForgottenThird, Display, TEXT("UI: key hint %s after %.1f s quiet."), bHintUp ? TEXT("up") : TEXT("gone"), HintQuiet);
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
		// The dim behind the options, measured once per choice where they will stand, before they are first drawn there
		// (the viewport still holds the last frame, without them).
		if (OptionRows == 0)
		{
			DimFor = MAX_uint32;
			OptionDim = 0.f;
		}
		else if (DimFor != OptionsFor)
		{
			DimFor = OptionsFor;
			OptionDim = MeasureOptionDim(Size);
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
		Mesh.Begin(Geometry, S);
		// No oval: a full-width gradient up from the screen's foot (a letterbox's falloff), even under the block and fading
		// out above it with no line where it ends.
		const float FadeTop = FMath::Max(0.f, Top - 170.f * S);
		Mesh.Band(0.f, FadeTop, Size.X, Size.Y, true, true, (Top + 30.f * S - FadeTop) / FMath::Max(Size.Y - FadeTop, 1.f),
			Table().Style.backingSoftDialogue, Tok(UiToken::Backing, 1.f));
		// The keys, once, under the block at the prompt size (never under the 18 px floor); gone after a few quiet seconds,
		// back on any input.
		if (HintA > 0.f)
		{
			P.Text(CX - HintW * 0.5f, Size.Y - 50.f * S, HintText, HintFont, Tok(UiToken::InkDim, HintA));
		}

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
		if (OptionRows > 0)
		{
			const float RowH = LineH * 1.05f;
			const float OptBlockH = RowH * OptionRows;
			const float MarkGap = 22.f * S; // the mark's centre to the text
			float MarkX;
			float OptTop;
			bool bRight;
			FVector2f H;
			OptionPlace(Size, S, MarkX, OptTop, bRight, H);
			if (OptionDim > 0.f)
			{
				// The measured dim (Tick), only over a bright picture: a wide falloff with no edge, its middle sized so the
				// block's ends still get the measured strength (the falloff gives 0.56 of its peak half way out).
				const float HalfW = (MarkGap + OptionW) * 0.5f + 16.f * S;
				const float HalfH = OptBlockH * 0.5f + 10.f * S;
				Mesh.Vignette(MarkX + HalfW - 16.f * S, OptTop + OptBlockH * 0.5f, HalfW * 2.f, HalfH * 2.2f,
					FMath::Min(OptionDim / 0.56f, 0.85f));
			}
			if (bAnchor)
			{
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
	// Where the options stand this frame: their marks' x and their top; anchored, the face and the side they hang on
	// (beside the person, to their right when it fits, else their left, never down into the line), else under the line.
	void OptionPlace(const FVector2f& Size, float S, float& MarkX, float& OptTop, bool& bRight, FVector2f& H) const
	{
		const int32 BelowRows = bAnchor ? 0 : OptionRows;
		const float BlockH = LineH * (Lines.Num() + BelowRows + (BelowRows > 0 ? 0.4f : 0.f));
		const float Top = Size.Y - 96.f * S - BlockH;
		const float OptBlockH = LineH * 1.05f * OptionRows;
		const float MarkGap = 22.f * S;
		bRight = true;
		H = FVector2f::ZeroVector;
		if (bAnchor)
		{
			H = FVector2f(AnchorUV.X * Size.X, AnchorUV.Y * Size.Y);
			const float Gap = 150.f * S;
			bRight = H.X + Gap + MarkGap + OptionW < Size.X - 80.f * S;
			MarkX = bRight ? H.X + Gap : H.X - Gap - MarkGap - OptionW;
			OptTop = FMath::Clamp(H.Y - 30.f * S - OptBlockH * 0.5f, 60.f * S, Top - OptBlockH - 40.f * S);
		}
		else
		{
			MarkX = Size.X * 0.5f - MaxW * 0.35f - MarkGap;
			OptTop = Top + LineH * (Lines.Num() + 0.4f);
		}
	}

	// The picture where the options will stand, read back once per choice (never per frame): the dim the option ink
	// needs over its brightest tenth (Core uiOptionDim), 0 when nothing could be read.
	float MeasureOptionDim(const FVector2f& Size) const
	{
		FViewport* VP = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
		const FIntPoint VS = VP ? VP->GetSizeXY() : FIntPoint::ZeroValue;
		if (VS.X <= 0 || VS.Y <= 0 || Size.X <= 0.f || Size.Y <= 0.f)
		{
			return 0.f;
		}
		const float S = FMath::Max(0.25f, Size.Y / 1080.f);
		float MarkX;
		float OptTop;
		bool bRight;
		FVector2f H;
		OptionPlace(Size, S, MarkX, OptTop, bRight, H);
		const float KX = VS.X / Size.X;
		const float KY = VS.Y / Size.Y;
		const FIntRect Rc(FMath::Clamp(FMath::FloorToInt((MarkX - 20.f * S) * KX), 0, VS.X - 1),
			FMath::Clamp(FMath::FloorToInt((OptTop - 8.f * S) * KY), 0, VS.Y - 1),
			FMath::Clamp(FMath::CeilToInt((MarkX + 42.f * S + OptionW) * KX), 1, VS.X),
			FMath::Clamp(FMath::CeilToInt((OptTop + LineH * 1.05f * OptionRows) * KY), 1, VS.Y));
		TArray<FColor> Px;
		if (Rc.Width() <= 0 || Rc.Height() <= 0 || !VP->ReadPixels(Px, FReadSurfaceDataFlags(), Rc) || Px.Num() == 0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("UI: options dim not measured (no picture read)."));
			return 0.f;
		}
		ftcore::UiLumaHistogram Hist;
		for (const FColor& C : Px)
		{
			ftcore::uiLumaAdd(Hist, ftcore::UiColor{C.R / 255.f, C.G / 255.f, C.B / 255.f, 1.f});
		}
		const ftcore::UiColor Pic = ftcore::uiLumaBrightest(Hist, 0.1f);
		const ftcore::UiStyle& St = Table().Style;
		const ftcore::UiColor Ink = ftcore::uiColor(St, ftcore::UiWorld::Solhaven, ftcore::UiPalette::Default, UiToken::Ink);
		const ftcore::UiColor Back = ftcore::uiColor(St, ftcore::UiWorld::Solhaven, ftcore::UiPalette::Default, UiToken::Backing);
		const float Dim = ftcore::uiOptionDim(St, Ink, Back, Pic);
		UE_LOG(LogForgottenThird, Display, TEXT("UI: options dim %.2f over the picture's brightest tenth (%.2f, %.2f, %.2f), %dx%d px read."), Dim,
			Pic.r, Pic.g, Pic.b, Rc.Width(), Rc.Height());
		return Dim;
	}

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
		HintFont = FontFor(UiTextRole::Prompt, Size.Y, Scale);
		HintText = kStoryHintUi;
		HintW = TextWidth(HintText, HintFont);
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
	FSlateFontInfo HintFont;
	FString HintText;
	float HintW = 0.f;
	float HintQuiet = 0.f;     // s since the dialogue opened or the last input
	double HintInputSeen = 0.0;
	float HintA = 1.f;
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
	float OptionDim = 0.f;     // the measured dim behind the options (0 over a dark picture)
	uint32 DimFor = MAX_uint32; // the options it was measured for
	bool bHintUp = false;
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
