#include "FTNewUi.h"

#include "FTArenaRunner.h"
#include "FTInputBindings.h"
#include "FTStoryDirector.h"
#include "FTText.h"
#include "FTUiText.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/InputBindings.h"
#include "ftcore/UiStroke.h"
#include "ftcore/UiStyle.h"
#include "ftcore/UiViews.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Engine/World.h"
#include "Fonts/FontMeasure.h"
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
		constexpr int32 Bands = 24; // one brush on one layer, so Slate batches them into a single draw
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

// A closed stadium (a key's ring) round (CX, CY): W wide, H high, round ends; the first point repeats at the end.
void StadiumStroke(ftcore::UiStroke& Out, float CX, float CY, float W, float H, float HalfWidth)
{
	const float R = H * 0.5f;
	const float Half = FMath::Max(0.f, W * 0.5f - R);
	constexpr int32 Arc = 14;
	Out.count = 0;
	const auto Put = [&Out, HalfWidth](float X, float Y)
	{
		ftcore::UiStrokePoint& P = Out.pt[Out.count++];
		P.x = X;
		P.y = Y;
		P.halfWidth = HalfWidth;
		P.alpha = 1.f;
	};
	for (int32 I = 0; I <= Arc; ++I) // the right end, top to bottom
	{
		const float A = -0.5f * UE_PI + UE_PI * I / Arc;
		Put(CX + Half + R * FMath::Cos(A), CY + R * FMath::Sin(A));
	}
	for (int32 I = 0; I <= Arc; ++I) // the left end, bottom to top
	{
		const float A = 0.5f * UE_PI + UE_PI * I / Arc;
		Put(CX - Half + R * FMath::Cos(A), CY + R * FMath::Sin(A));
	}
	Put(Out.pt[0].x, Out.pt[0].y);
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
			LabelH = FontMeasure()->GetMaxCharacterHeight(LabelFont);
			ValueAsc = FontMeasure()->GetMaxCharacterHeight(ValueFont) + FontMeasure()->GetBaseline(ValueFont);
			LabelAsc = LabelH + FontMeasure()->GetBaseline(LabelFont);
			VerbAsc = FontMeasure()->GetMaxCharacterHeight(VerbFont) + FontMeasure()->GetBaseline(VerbFont);
			const auto Word = [this](const FString& In, FString& Out, float& W, const FSlateFontInfo& F)
			{
				Out = In.ToUpper();
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
			for (FVerbSlot& V : Verbs)
			{
				Word(FTUiText::Lookup(*FT::ToFString(V.Label)), V.Word, V.WordW, VerbFont);
			}
			bRebuild = true; // kept until a live frame rebuilds the strings that need the fight
		}
		if (!bLive)
		{
			return;
		}
		const bool bRebuildNow = bRebuild;
		bRebuild = false;
		const ftcore::ArenaFight& Fight = Runner->GetFight();
		ftcore::ArenaHudFrame Frame = View.now;
		ftcore::fillArenaHudLead(Fight, Frame.lead);
		ftcore::fillArenaHudTarget(Fight, Frame.target);
		FillBoss(*Runner, Frame.boss);
		Frame.partner = ftcore::ArenaHudPartner{}; // the arena runs one fighter; the session's partner arrives with the party
		Frame.third = ftcore::ArenaHudThird{};
		const uint32 Changed = ftcore::updateArenaHudView(View, Frame);
		const ftcore::ArenaHudLead& L = View.now.lead;
		if (bRebuildNow || (Changed & SectionBit(ftcore::ArenaHudSection::Lead)))
		{
			HpText = FString::Printf(TEXT("%d"), L.hp);
			HpTextW = TextWidth(HpText, ValueFont);
			LoadText = FString::Printf(TEXT("%d"), L.load);
		}
		HpLost.Step(L.hpMax > 0 ? static_cast<float>(L.hp) / L.hpMax : 0.f, DeltaTime);
		const ftcore::ArenaHudBoss& B = View.now.boss;
		const ftcore::ArenaHudTarget& T = View.now.target;
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
						V.Keys = FT::ToFString(ftcore::inputHintKeys(FTInputBindings::Get(), Entries[I], Device)).ToUpper();
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
		PaintLead(P, Size);
		PaintTop(P, Size);
		PaintVerbs(P, Size);
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
		const float CX = Carry::LeadX * S;
		const float CY = Size.Y - Carry::LeadUp * S;
		Mesh.Shade(CX + 10.f * S, CY - 40.f * S, 230.f * S, 230.f * S, St.backingSoftHud * FMath::Max(A, 0.6f));

		Ring(CX, CY, Carry::RingR * S, 1.5f, 11, S, Tok(UiToken::Ink, 0.9f * A));

		const float Hp = L.hpMax > 0 ? FMath::Clamp(static_cast<float>(L.hp) / L.hpMax, 0.f, 1.f) : 0.f;
		const bool bLow = Hp <= Carry::LowHp;
		const FLinearColor HpC = bLow ? Tok(UiToken::Danger, A) : Tok(UiToken::Ink, A);
		Arc(CX, CY, Carry::HpR * S, 0.f, Hp, StrokeStyle(4.2f, 21, S), HpC);
		if (HpLost.Was > Hp + 0.004f)
		{
			ftcore::UiStrokeStyle Rub = StrokeStyle(1.6f, 21, S, 0.f);
			Rub.broken = true;
			Arc(CX, CY, Carry::HpR * S, Hp, HpLost.Was, Rub, Tok(UiToken::Ink, 0.22f * A), false);
		}
		Arc(CX, CY, Carry::StaminaR * S, 0.f, FMath::Clamp(L.stamina01, 0.f, 1.f), StrokeStyle(1.2f, 23, S), Tok(UiToken::InkDim, 0.7f * A), false);

		if (L.loadMax > 0)
		{
			const float Load = FMath::Clamp(static_cast<float>(L.load) / L.loadMax, 0.f, 1.f);
			Arc(CX, CY, Carry::LoadR * S, 0.f, 1.f, StrokeStyle(1.f, 31, S, 0.f), Tok(UiToken::Ink, 0.28f * A), false);
			Arc(CX, CY, Carry::LoadR * S, 0.f, Load, StrokeStyle(3.f, 33, S), L.overloaded ? Tok(UiToken::Danger, A) : Tok(UiToken::Accent, A));
			const float Mark = FMath::Clamp(static_cast<float>(ftcore::kArenaReleaseOpenLoad) / L.loadMax, 0.f, 1.f);
			const float Deg = Carry::A0 + (Carry::A1 - Carry::A0) * Mark;
			float MX = 0.f;
			float MY = 0.f;
			ftcore::uiArcPoint(CX, CY, (Carry::LoadR + 11.f) * S, Deg, MX, MY);
			Chevron(MX, MY, 10.f * S, Deg, S, Tok(UiToken::Ink, A));
			P.Text(CX + 30.f * S, CY + 84.f * S - ValueAsc, LoadText, ValueFont, L.overloaded ? Tok(UiToken::Danger, A) : Tok(UiToken::Accent, A));
		}

		// The numbers in the open bottom: HP to the left of the middle (a danger mark beside it when low), Load to the right.
		P.Text(CX - 30.f * S - HpTextW, CY + 84.f * S - ValueAsc, HpText, ValueFont, HpC);
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
		if (!B.present)
		{
			P.Text(CX - TargetW * 0.5f, Y - 26.f * S - LabelAsc, TargetWord, LabelFont, Tok(UiToken::InkDim, A));
		}
		for (int32 I = 1; B.present && I < B.phaseCount; ++I)
		{
			const float X = X0 + Len * I / B.phaseCount;
			const FVector2f Pts[2] = {FVector2f(X, Y - 8.f * S), FVector2f(X, Y + 8.f * S)};
			Poly(Pts, 2, 0.7f * S, Tok(UiToken::Ink, A));
		}
		// The target's poise under the line (the boss's own when the target is the boss).
		if (T.present && (!B.present || T.enemy == B.nameKey))
		{
			HLine(X0, X0 + Len, Y + 12.f * S, 0.f, FMath::Clamp(T.poise01, 0.f, 1.f), StrokeStyle(1.4f, 103, S), Tok(UiToken::InkDim, A), false);
		}
		float LY = Y + 28.f * S; // the next row's top
		if (B.present && B.weightMax > 0)
		{
			P.Text(X0, LY, WeightWord, LabelFont, Tok(UiToken::InkDim, A));
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
			const float BX = CX - InvulW * 0.5f - 14.f * S;
			const float BY = WY + LabelAsc * 0.55f;
			const float R = 5.f * S;
			const FVector2f Pts[5] = {FVector2f(BX - R, BY - R), FVector2f(BX + R, BY - R), FVector2f(BX + R, BY + R), FVector2f(BX - R, BY + R),
				FVector2f(BX - R, BY - R)};
			Poly(Pts, 5, 0.7f * S, Tok(UiToken::Ink, A), true);
			P.Text(CX - InvulW * 0.5f, WY, InvulWord, LabelFont, Tok(UiToken::Ink, A));
		}
		if (B.present && T.present && T.enemy != B.nameKey)
		{
			const float OY = Y + 96.f * S;
			const float OLen = 300.f * S;
			const float OX = CX - OLen * 0.5f;
			const float OHp = T.hpMax > 0 ? FMath::Clamp(static_cast<float>(T.hp) / T.hpMax, 0.f, 1.f) : 0.f;
			P.Text(OX - TargetW - 16.f * S, OY - LabelAsc * 0.6f, TargetWord, LabelFont, Tok(UiToken::InkDim, A));
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
			const float VA = FMath::Max(A, 0.6f) * (Avail[I] ? 1.f : 0.5f); // a reference list: it steps back, never out
			const FLinearColor C = I == Hot ? Tok(UiToken::Accent, VA) : Tok(UiToken::Ink, VA);
			const float KX = RX - 44.f * S;
			const float KY = Y - 7.f * S;
			const float KW = V.Keys.Len() <= 1 ? 30.f * S : FMath::Max(30.f * S, V.KeysW + 18.f * S);
			StadiumStroke(Line, KX, KY, KW, 30.f * S, 0.65f * S);
			Mesh.Stroke(Line, C, true, true);
			P.Text(KX - V.KeysW * 0.5f, KY - LabelAsc * 0.55f, V.Keys, LabelFont, C);
			P.Text(KX - KW * 0.5f - 18.f * S - V.WordW, Y - VerbAsc, V.Word, VerbFont, C);
			if (I == Hot)
			{
				const FVector2f Pts[3] = {FVector2f(RX + 19.f * S, Y - 15.f * S), FVector2f(RX + 10.f * S, Y - 7.f * S), FVector2f(RX + 19.f * S, Y + 1.f * S)};
				Poly(Pts, 3, 0.9f * S, Tok(UiToken::Accent, A));
			}
		}
	}

	TFunction<const UFTArenaRunner*()> GetRunner;
	ftcore::ArenaHudView View;
	ftcore::UiFade LeadFade;
	ftcore::UiFade TopFade;
	ftcore::UiFade VerbFade;
	FLostTrace HpLost;
	FLostTrace TopLost;
	FVerbSlot Verbs[4] = {{"input.guard"}, {"input.release"}, {"input.count"}, {"input.heal"}};
	ftcore::InputDevice VerbDevice = ftcore::InputDevice::Keys;
	uint32 LastVerbState = MAX_uint32;
	bool bRebuild = true;
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
