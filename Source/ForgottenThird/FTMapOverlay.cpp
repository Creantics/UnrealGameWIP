#include "FTMapOverlay.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Fonts/FontMeasure.h"
#include "Fonts/SlateFontInfo.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Rendering/SlateRenderer.h"
#include "TextureResource.h"
#include "UnrealClient.h"

#include "FTExploreCharacter.h"
#include "FTNewUi.h"
#include "FTPartyFollower.h"
#include "FTPartyVan.h"
#include "ForgottenThird.h"
#include "ftcore/UiStyle.h"

#include <algorithm>
#include <string>

namespace FTMapOverlayDetail
{
// The picked look (2026-11-17): blue-black relief, thin warm gold, serif names. Linear colours.
const FLinearColor kOvLand(0.005f, 0.008f, 0.015f, 1.f);      // the ground: yards, parks, open land, and on past the area
const FLinearColor kOvSea(0.0015f, 0.0035f, 0.0075f, 1.f);    // the water and the sea past the area
const FLinearColor kOvBlock(0.011f, 0.017f, 0.034f, 1.f);     // a building's roof
const FLinearColor kOvEnterable(0.017f, 0.025f, 0.048f, 1.f); // a building with a door, a touch lighter
const FLinearColor kOvHatch(0.04f, 0.055f, 0.09f, 0.55f);
const FLinearColor kOvRoofLit(0.07f, 0.09f, 0.14f, 0.9f);
const FLinearColor kOvRoofShade(0.f, 0.f, 0.f, 0.55f);
const FLinearColor kOvRoadFill(0.007f, 0.011f, 0.021f, 1.f); // inside a cased road
const FLinearColor kOvGold(0.67f, 0.40f, 0.10f, 1.f);
const FLinearColor kOvGlow(0.67f, 0.40f, 0.10f, 0.07f);
const FLinearColor kOvLit(1.f, 0.66f, 0.22f, 1.f); // the cursor's road
const float kOvRoadAlpha[ftcore::kMapRoadClasses] = {0.95f, 0.8f, 0.6f, 0.42f};
const float kOvRoadLinePx[ftcore::kMapRoadClasses] = {1.7f, 1.3f, 1.f, 0.9f}; // a road too narrow to case: one line
constexpr float kOvCasePx = 5.f;  // a road this wide on screen (px at 1080p) or more is cased: two gold edges
constexpr float kOvEdgeLinePx = 1.2f;
constexpr float kOvGlowPx = 7.f;
constexpr float kOvRoadFadePx = 40.f; // a road that runs off the model fades out over this much of its end (px at 1080p)
const FLinearColor kOvLabel(0.80f, 0.62f, 0.34f, 1.f);
const FLinearColor kOvLabelDim(0.52f, 0.42f, 0.26f, 1.f);
const FLinearColor kOvCursorLabel(1.f, 0.78f, 0.42f, 1.f);
const FLinearColor kOvWaterLabel(0.36f, 0.45f, 0.56f, 1.f);
const FLinearColor kOvDistrictLabel(0.92f, 0.64f, 0.26f, 1.f);
const FLinearColor kOvStreetHere(0.82f, 0.76f, 0.62f, 1.f);
const FLinearColor kOvBoxFill(0.004f, 0.006f, 0.012f, 0.88f);
const FLinearColor kOvCaptionPlate(0.004f, 0.006f, 0.012f, 1.f); // the where-am-I line's plate: opaque, so no line shows through
const FLinearColor kOvHalo(0.001f, 0.0015f, 0.003f, 0.9f);
const FLinearColor kOvHaloSoft(0.001f, 0.0015f, 0.003f, 0.35f); // a label's halo: copies overlap, dark at the glyph, soft at 3 px
// The look's strokes (ftcore/MapLook.h), all gold: the coast at half a main road's line, the wave lines fading out to
// sea, the canals' edges, the parks' hatch at a tenth, the yards' dotted edges.
constexpr float kOvFinePx = 0.6f;
constexpr float kOvCoastAlpha = 0.9f;
const float kOvWaveAlpha[3] = {0.3f, 0.18f, 0.09f}; // 12, 24, 40 m out
constexpr float kOvCanalAlpha = 0.5f;
constexpr float kOvParkAlpha = 0.1f;
constexpr float kOvYardAlpha = 0.35f;
constexpr float kOvDotPx = 1.3f;
constexpr uint32 kOvEdgeRows = 4;        // the edge rows the look may give the sea (MapLook.cpp: kEdgeMarginRows)
constexpr float kOvSeaBandM = 20000.f;   // the sea past the north and south edges runs on this far
const FLinearColor kOvNeedleSouth(0.12f, 0.1f, 0.08f, 1.f);
const TCHAR* kOvShotNames[ftcore::kMapZooms] = {TEXT("far"), TEXT("mid"), TEXT("near")};
const TCHAR* kOvNorth = TEXT("N");
constexpr float kOvRefPx = 64.f;          // names are measured once at this size
constexpr float kOvCursorReachPx = 14.f;  // the cursor picks a road this near (px)
constexpr float kOvEdgePx = 70.f;         // the view pans when the cursor is this near an edge
constexpr float kOvSignPx = 7.f;          // a sign form's glyph height (px at 1080p)
constexpr int32 kOvSignStrokesMax = 192;  // per district name
constexpr float kOvShear = 0.2f;          // water names lean this much (x per y)
// The marks the cursor's name is kept clear of (px at 1080p, radius): the cursor's ring with its ticks (out to 16, a
// pixel spare), the player's arrow (its tip).
constexpr float kOvCursorMarkPx = 17.f;
constexpr float kOvArrowMarkPx = 12.f;

ftcore::Vec2 OvMetres(const FVector& Cm) { return {static_cast<float>(Cm.X / 100.0), static_cast<float>(Cm.Y / 100.0)}; }
FVector2D OvV2(const ftcore::Vec2 V) { return FVector2D(V.x, V.y); }

// The roads' ink at S (ftcore::MapRoadInk): the widths the road passes draw are the ones the labels are kept off. The lit
// line (the cursor's road, ftcore::mapRoadLit) is half the road's own width on screen, never under 2 px at 1080p.
ftcore::MapRoadInk OvInk(float S)
{
	ftcore::MapRoadInk Ink;
	Ink.casePx = kOvCasePx;
	for (int32 C = 0; C < ftcore::kMapRoadClasses; ++C)
	{
		Ink.linePx[C] = kOvRoadLinePx[C];
	}
	Ink.minPx = 0.8f;
	Ink.litShare = 0.5f;
	Ink.litMinPx = 2.f;
	Ink.scale = S;
	return Ink;
}

FCanvasUVTri OvTri(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FLinearColor& Color)
{
	FCanvasUVTri T;
	T.V0_Pos = A;
	T.V1_Pos = B;
	T.V2_Pos = C;
	T.V0_UV = T.V1_UV = T.V2_UV = FVector2D(0.5, 0.5);
	T.V0_Color = T.V1_Color = T.V2_Color = Color;
	return T;
}

// A label's face, size (px at 1080p) and colour from its rank and its name's kind.
struct FOvStyle
{
	uint8 Face = 0; // FFTMapOverlay::EFace
	float Px = 14.f;
	FLinearColor Color = kOvLabel;
	bool bItalic = false;
	bool bBox = false;
};
FOvStyle OvStyle(ftcore::MapLabelRank Rank, bool bWater)
{
	// Faces: 0 serif, 1 serif bold, 2 serif small caps (FFTMapOverlay::EFace).
	FOvStyle S;
	switch (Rank)
	{
	case ftcore::MapLabelRank::Cursor: S = {1, 17.f, kOvCursorLabel, false, false}; break;
	case ftcore::MapLabelRank::District: S = {2, 15.f, kOvDistrictLabel, false, true}; break;
	case ftcore::MapLabelRank::Water: S = {0, 16.f, kOvWaterLabel, true, false}; break;
	case ftcore::MapLabelRank::Main: S = {0, 16.f, kOvLabel, false, false}; break;
	case ftcore::MapLabelRank::Street: S = {0, 14.f, kOvLabel, false, false}; break;
	case ftcore::MapLabelRank::Place: S = {0, 13.f, kOvLabelDim, false, false}; break;
	case ftcore::MapLabelRank::Lane: S = {0, 12.5f, kOvLabelDim, false, false}; break;
	}
	S.bItalic = S.bItalic || bWater;
	return S;
}
// A district box's padding (px at 1080p).
constexpr float kOvBoxPadX = 9.f;
constexpr float kOvBoxPadY = 4.f;
constexpr float kOvSignGap = 4.f; // between the sign form and the box
} // namespace FTMapOverlayDetail

bool FFTMapOverlay::Wanted()
{
	return FParse::Param(FCommandLine::Get(), TEXT("MapOverlayC"));
}

FFTMapOverlay::FFTMapOverlay(const ftcore::MiniMapModel& InModel, UTexture2D& White, double PlanSide) : Model(InModel)
{
	using namespace FTMapOverlayDetail;
	ftcore::mapEnterableBuildings(Model, Enterable);
	ftcore::mapWaterRuns(Model, Water);
	SeaSide = ftcore::mapSeaSide(Model);
	RoadBox.resize(Model.roads.size() * 4);
	RoadLen.assign(Model.roads.size(), 0.f);
	ftcore::mapRoadOffEnds(Model, RoadOff);
	size_t Segments = 0;
	for (size_t R = 0; R < Model.roads.size(); ++R)
	{
		const ftcore::MapRoad& Road = Model.roads[R];
		float X0 = 1e30f, Y0 = 1e30f, X1 = -1e30f, Y1 = -1e30f;
		for (uint32 K = 0; K < Road.count && Road.first + K < Model.points.size(); ++K)
		{
			const ftcore::Vec2 P = Model.points[Road.first + K];
			X0 = FMath::Min(X0, P.x);
			Y0 = FMath::Min(Y0, P.y);
			X1 = FMath::Max(X1, P.x);
			Y1 = FMath::Max(Y1, P.y);
			if (K > 0)
			{
				const ftcore::Vec2 Q = Model.points[Road.first + K - 1];
				RoadLen[R] += FMath::Sqrt((P.x - Q.x) * (P.x - Q.x) + (P.y - Q.y) * (P.y - Q.y));
			}
		}
		const float Half = Road.width * 0.5f;
		RoadBox[R * 4 + 0] = X0 - Half;
		RoadBox[R * 4 + 1] = Y0 - Half;
		RoadBox[R * 4 + 2] = X1 + Half;
		RoadBox[R * 4 + 3] = Y1 + Half;
		Segments += Road.count > 0 ? Road.count - 1 : 0;
	}

	// The names: the world lane's table (public display data), read once.
	FString Path;
	if (!FParse::Value(FCommandLine::Get(), TEXT("MapNames="), Path))
	{
		Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("Data/Map/solhaven_names.csv"));
	}
	FString Csv;
	if (FPaths::FileExists(Path) && FFileHelper::LoadFileToString(Csv, *Path))
	{
		const FTCHARToUTF8 Utf8(*Csv);
		std::string Why;
		if (ftcore::mapLoadNames(Model, std::string_view(Utf8.Get(), Utf8.Length()), Names, &Why, PlanSide))
		{
			for (const std::string& N : Names.names)
			{
				NameText.Add(UTF8_TO_TCHAR(N.c_str()));
			}
		}
		else
		{
			Names = ftcore::MapNames{};
			UE_LOG(LogForgottenThird, Error, TEXT("MapOverlay: the names %s: %s"), *Path, UTF8_TO_TCHAR(Why.c_str()));
		}
	}
	for (int32 F = 0; F < FaceCount; ++F)
	{
		NameRef[F].Init(FVector2f(-1.f, -1.f), NameText.Num());
	}
	Seen.assign(Names.names.size(), 0);
	// The look: the sea and its coast, the waves, the canals' edges, the blocks (ftcore/MapLook.h), built once.
	const double LookStart = FPlatformTime::Seconds();
	ftcore::mapBuildLook(Model, &Names, Look);
	const double LookMs = (FPlatformTime::Seconds() - LookStart) * 1000.0;
	// The sea along the north and south edges: each edge row's sea runs (the look may have given the dry margin rows the
	// sea), the outermost row's run on past the area, as ftcore::mapIsSea counts them.
	if (Look.w > 0 && Look.h > 0 && Look.cell > 0.f && Look.sea.size() >= static_cast<size_t>(Look.w) * Look.h)
	{
		for (uint32 Row = 0; Row < Look.h; ++Row)
		{
			if (Row >= kOvEdgeRows && Row + kOvEdgeRows < Look.h)
			{
				continue;
			}
			const size_t RowFirst = static_cast<size_t>(Row) * Look.w;
			uint32 X = 0;
			while (X < Look.w)
			{
				if (!Look.sea[RowFirst + X])
				{
					++X;
					continue;
				}
				uint32 End = X;
				while (End < Look.w && Look.sea[RowFirst + End])
				{
					++End;
				}
				const float Left = Look.x0 + static_cast<float>(X) * Look.cell;
				const float Right = Look.x0 + static_cast<float>(End) * Look.cell;
				float Top = Look.y0 + static_cast<float>(Row) * Look.cell;
				float Bottom = Top + Look.cell;
				if (Row == 0)
				{
					Top = FMath::Min(Top, Model.minY) - kOvSeaBandM;
				}
				if (Row + 1 == Look.h)
				{
					Bottom = FMath::Max(Bottom, Model.maxY) + kOvSeaBandM;
				}
				ftcore::MapRect Run;
				Run.cx = (Left + Right) * 0.5f;
				Run.cy = (Top + Bottom) * 0.5f;
				Run.hx = (Right - Left) * 0.5f;
				Run.hy = (Bottom - Top) * 0.5f;
				SeaEdge.push_back(Run);
				X = End;
			}
		}
	}
	size_t LookStrokes = 0;
	for (const std::vector<ftcore::MapStroke>& List : Look.strokes)
	{
		LookStrokes += List.size();
	}
	// The districts' sign forms, laid out once (in glyph-height units).
	SignFirst.Init(0, NameText.Num());
	SignCount.Init(0, NameText.Num());
	SignWidth.Init(0.f, NameText.Num());
	for (const ftcore::MapNames::District& D : Names.districts)
	{
		if (D.name >= Names.signForms.size() || Names.signForms[D.name].empty() || SignCount[D.name] > 0)
		{
			continue;
		}
		const size_t First = SignStrokes.size();
		SignStrokes.resize(First + kOvSignStrokesMax);
		float W = 0.f;
		const size_t N = ftcore::signScriptStrokes(Names.signForms[D.name], SignStrokes.data() + First, kOvSignStrokesMax, &W);
		SignStrokes.resize(First + N);
		SignFirst[D.name] = static_cast<int32>(First);
		SignCount[D.name] = static_cast<int32>(N);
		SignWidth[D.name] = W;
	}
	// The new UI's faces: Spectral, Spectral bold, Spectral small caps. Without them (no Slate): the engine's font.
	const ftcore::UiFont Want[FaceCount] = {ftcore::UiFont::Serif, ftcore::UiFont::SerifBold, ftcore::UiFont::SerifSc};
	int32 Missing = 0;
	for (int32 F = 0; F < FaceCount; ++F)
	{
		Fonts[F] = FSlateApplication::IsInitialized() ? FT::UiCanvasFont(static_cast<uint8>(Want[F])) : nullptr;
		if (!Fonts[F])
		{
			Fonts[F] = GEngine ? GEngine->GetMediumFont() : nullptr;
			++Missing;
		}
	}
	int32 Named = 0;
	for (const uint16 N : Names.roadName)
	{
		Named += N != ftcore::kMapNoName ? 1 : 0;
	}
	int32 Lit = 0;
	for (const uint8 E : Enterable)
	{
		Lit += E;
	}

	const TArray<FCanvasUVTri> None{OvTri(FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector, FLinearColor::Transparent)};
	Geo = new FCanvasTriangleItem(None, White.GetResource());
	Boxes = new FCanvasTriangleItem(None, White.GetResource());
	Marks = new FCanvasTriangleItem(None, White.GetResource());
	Geo->BlendMode = SE_BLEND_Translucent;
	Boxes->BlendMode = SE_BLEND_Translucent;
	Marks->BlendMode = SE_BLEND_Translucent;
	// Per building: its roof (2), up to 12 hatch lines and two edges (2 each); per road segment: glow, gold, fill, lit;
	// per stroke of the look and per sea run: 2.
	Geo->TriangleList.Reserve(static_cast<int32>(Model.buildings.size() * 30 + Water.size() * 2 + Segments * 8 + LookStrokes * 2 +
		SeaEdge.size() * 2) + 64);
	Boxes->TriangleList.Reserve(2048);
	Marks->TriangleList.Reserve(512);

	FParse::Value(FCommandLine::Get(), TEXT("MapOverlayShots="), ShotPrefix);
	FParse::Value(FCommandLine::Get(), TEXT("MapOverlayShotAt="), ShotAt);
	FString At;
	if (FParse::Value(FCommandLine::Get(), TEXT("MapOverlayCursor="), At))
	{
		FString X, Y;
		if (At.Split(TEXT(":"), &X, &Y))
		{
			ShotCursor = {FCString::Atof(*X), FCString::Atof(*Y)};
			bShotCursor = true;
		}
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("MAPOVERLAY READY option=C look=gold names=%d roads_named=%d/%d districts=%d places=%d unmatched=%d enterable=%d/%d water_rects=%d sea=%d sign_strokes=%d fonts_missing=%d plan_side=%.0f"),
		NameText.Num(), Named, static_cast<int32>(Model.roads.size()), static_cast<int32>(Names.districts.size()),
		static_cast<int32>(Names.places.size()), static_cast<int32>(Names.unmatched), Lit, static_cast<int32>(Enterable.size()),
		static_cast<int32>(Water.size()), SeaSide, static_cast<int32>(SignStrokes.size()), Missing, PlanSide);
	int32 OffEnds = 0;
	for (const uint8 E : RoadOff)
	{
		OffEnds += (E & ftcore::kMapRoadOffFirst ? 1 : 0) + (E & ftcore::kMapRoadOffLast ? 1 : 0);
	}
	auto StrokeCount = [this](ftcore::MapStrokeKind Kind) { return static_cast<int32>(Look.strokes[static_cast<size_t>(Kind)].size()); };
	UE_LOG(LogForgottenThird, Display,
		TEXT("MAPOVERLAY LOOK parks=%d yards=%d slivers=%d open=%d coast=%d waves=%d/%d/%d canal=%d hatch=%d dots=%d sea_spots=%d sea_edge=%d edge_coast=%d road_off=%d sea_name=%s ms=%.0f"),
		static_cast<int32>(Look.parks), static_cast<int32>(Look.yards), static_cast<int32>(Look.slivers), static_cast<int32>(Look.open),
		StrokeCount(ftcore::MapStrokeKind::Coast), StrokeCount(ftcore::MapStrokeKind::Wave12), StrokeCount(ftcore::MapStrokeKind::Wave24),
		StrokeCount(ftcore::MapStrokeKind::Wave40), StrokeCount(ftcore::MapStrokeKind::Canal), StrokeCount(ftcore::MapStrokeKind::ParkHatch),
		StrokeCount(ftcore::MapStrokeKind::YardDot), static_cast<int32>(Look.seaSpots.size()), static_cast<int32>(SeaEdge.size()),
		static_cast<int32>(Look.edgeCoast.size()), OffEnds,
		NameText.IsValidIndex(Names.seaName) ? *NameText[Names.seaName] : TEXT("none"), LookMs);
}

FFTMapOverlay::~FFTMapOverlay()
{
	delete Geo;
	delete Boxes;
	delete Marks;
}

void FFTMapOverlay::TickShots(APlayerController& PC, double Now, bool& bOverlay)
{
	using namespace FTMapOverlayDetail;
	if (ShotPrefix.IsEmpty() || Now < ShotAt || Now < StageStart)
	{
		return;
	}
	const int32 Zooms = ftcore::kMapZooms;
	if (ShotStage == 0)
	{
		bOverlay = true;
		StageStart = Now + 1.5;
	}
	else if (ShotStage <= 2 * Zooms)
	{
		const int32 Z = (ShotStage - 1) / 2;
		if ((ShotStage - 1) % 2 == 0)
		{
			// Each shot is framed on the player (or the asked point), not wherever the last zoom left it; the cursor is
			// on it.
			Zoom = static_cast<ftcore::MapZoom>(Z);
			FrameOn(Focus);
			StageStart = Now + 1.0;
		}
		else
		{
			const FString File = ShotPrefix + TEXT("_") + kOvShotNames[Z] + TEXT(".png");
			FScreenshotRequest::RequestScreenshot(File, true, false);
			UE_LOG(LogForgottenThird, Display, TEXT("MAPOVERLAYSHOT %s m/px=%.2f centre=%.1f:%.1f picks=%d candidates=%d labels=%d tris=%d file=%s"),
				kOvShotNames[Z], View.mPerPx, View.centre.x, View.centre.y, LastPicks, LastCandidates, static_cast<int32>(Placed.count),
				Geo->TriangleList.Num(), *File);
			// The land's share of the frame as drawn (truncated, so 0.75 means at least 0.75) and the where-am-I line. While
			// the view is still the framed one, the share is the framing's own (the value its test passed), not a recount.
			const bool bAsFramed = View.centre.x == Framed.x && View.centre.y == Framed.y;
			const float Share = bAsFramed ? FramedLand : ftcore::mapLandShare(Model, Look, View);
			const float Land = FMath::FloorToFloat(Share * 100.f) / 100.f;
			const FString District = NameText.IsValidIndex(CaptionDistrict) ? NameText[CaptionDistrict].ToUpper() : FString();
			const FString Street = NameText.IsValidIndex(CaptionStreet) && CaptionStreet != CaptionDistrict ? NameText[CaptionStreet].ToUpper() : FString();
			UE_LOG(LogForgottenThird, Display, TEXT("MAPOVERLAY SHOT %s centre=%.1f:%.1f mpp=%.3f land=%.2f caption='%s \u00B7 %s' labels=%d"),
				kOvShotNames[Z], View.centre.x, View.centre.y, View.mPerPx, Land, *District, *Street, static_cast<int32>(Placed.count));
			// The text at its size: labels whose halo runs off the screen (0: none clipped), and the where-am-I line's right
			// and bottom ends (its rule) against the screen's.
			int32 Clipped = 0;
			for (size_t I = 0; I < Placed.count; ++I)
			{
				ftcore::Vec2 K[4];
				ftcore::mapLabelCorners(Placed.labels[I], ftcore::kMapLabelHaloPx * LabelTS, K);
				bool bOut = false;
				for (const ftcore::Vec2& Q : K)
				{
					bOut = bOut || Q.x < 0.f || Q.y < 0.f || Q.x > View.screenW || Q.y > View.screenH;
				}
				Clipped += bOut ? 1 : 0;
			}
			UE_LOG(LogForgottenThird, Display,
				TEXT("MAPOVERLAY TEXT %s ts=%.2f clipped=%d caption_right=%.0f caption_bottom=%.0f caption_text_right=%.0f caption_plate_right=%.0f caption_fit=%.2f caption_in_plate=%d text_draws=%d screen=%.0fx%.0f"),
				kOvShotNames[Z], LabelTS, Clipped, CaptionRight, CaptionBottom, CaptionTextRight, CaptionPlateRight, CaptionFit,
				CaptionTextRight + ftcore::kMapCaptionPad * LabelTS <= CaptionPlateRight + 0.5f ? 1 : 0, LastTextDraws, View.screenW, View.screenH);
			// Place and water names (the sea's too) whose text box meets any road drawn at the zoom, the cursor's at its lit
			// width (0: none), and the names the road and water rules left out, with why.
			int32 OnRoad = 0, PointNames = 0;
			const ftcore::MapRoadInk Ink = OvInk(LabelS);
			const uint16 LitName = ftcore::mapRoadName(Names, LabelPicked);
			for (size_t I = 0; I < Placed.count; ++I)
			{
				const ftcore::MapLabel& L = Placed.labels[I];
				const auto Rank = static_cast<ftcore::MapLabelRank>(L.priority);
				if (Rank != ftcore::MapLabelRank::Place && Rank != ftcore::MapLabelRank::Water)
				{
					continue;
				}
				++PointNames;
				OnRoad += ftcore::mapLabelOnAnyRoad(Model, Names, View, Zoom, Ink, L, 0.f, LabelPicked, LitName) >= 0 ? 1 : 0;
			}
			FString DropList;
			static const TCHAR* const kWhy[] = {TEXT("dry"), TEXT("no_seat"), TEXT("on_road")};
			for (int32 I = 0; I < DropCount; ++I)
			{
				DropList += FString::Printf(TEXT("%s%s:%s"), I ? TEXT(", ") : TEXT(""),
					NameText.IsValidIndex(Drops[I].Name) ? *NameText[Drops[I].Name] : TEXT("?"), kWhy[static_cast<int32>(Drops[I].Why)]);
			}
			UE_LOG(LogForgottenThird, Display, TEXT("MAPOVERLAY NAMES %s point_names=%d names_on_road=%d dropped=%d [%s]"),
				kOvShotNames[Z], PointNames, OnRoad, DropTotal, *DropList);
			// F4: the cursor's name, its halo included, under none of the marks drawn over it (on_marks=0), and whether it moved
			// for them or was left out.
			int32 CursorNames = 0, OnMarks = 0;
			for (size_t I = 0; I < Placed.count; ++I)
			{
				const ftcore::MapLabel& L = Placed.labels[I];
				if (L.priority != static_cast<uint8>(ftcore::MapLabelRank::Cursor))
				{
					continue;
				}
				++CursorNames;
				for (int32 K = 0; K < LabelDiscCount; ++K)
				{
					OnMarks += ftcore::mapLabelMeetsDisc(L, ftcore::kMapLabelHaloPx * LabelTS, LabelDiscs[K]) ? 1 : 0;
				}
			}
			UE_LOG(LogForgottenThird, Display, TEXT("MAPOVERLAY CURSOR %s name=%d on_marks=%d moved=%d dropped=%d marks=%d"),
				kOvShotNames[Z], CursorNames, OnMarks, bCursorNameMoved ? 1 : 0, bCursorNameDropped ? 1 : 0, LabelDiscCount);
			if (!bFramedReached)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("MAPOVERLAY FRAME %s unreached: no allowed shift brings the land to 0.75; the best share %.3f"),
					kOvShotNames[Z], FramedLand);
			}
			StageStart = Now + 0.8;
		}
	}
	else if (ShotStage == 2 * Zooms + 1)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("MAPOVERLAYSHOTS DONE"));
		PC.ConsoleCommand(TEXT("quit"));
	}
	++ShotStage;
}

void FFTMapOverlay::OnOpen(AHUD& Hud, float ScreenW, float ScreenH)
{
	ftcore::Vec2 At{(Model.minX + Model.maxX) * 0.5f, (Model.minY + Model.maxY) * 0.5f};
	if (const APawn* Pawn = Hud.GetOwningPawn())
	{
		At = FTMapOverlayDetail::OvMetres(Pawn->GetActorLocation());
	}
	if (bShotCursor)
	{
		At = ShotCursor;
	}
	// It opens on the player at the near zoom.
	Focus = At;
	Zoom = ftcore::MapZoom::Near;
	View.screenW = ScreenW;
	View.screenH = ScreenH;
	FrameOn(At);
	LabelPicked = -2; // place the labels again
	// The party, found when the map opens (not each frame).
	Van = nullptr;
	Followers[0] = Followers[1] = nullptr;
	if (UWorld* World = Hud.GetWorld())
	{
		for (TActorIterator<AFTPartyVan> It(World); It; ++It)
		{
			Van = *It;
			break;
		}
		int32 N = 0;
		for (TActorIterator<AFTPartyFollower> It(World); It && N < 2; ++It)
		{
			Followers[N++] = *It;
		}
	}
}

void FFTMapOverlay::SetZoom(ftcore::MapZoom Z)
{
	// The new zoom is framed on the world point under the cursor, as the map opens on the player: the land at least
	// three quarters of the frame, the point in its middle (the far zoom fits the land); the cursor stays on the point.
	const ftcore::Vec2 Under = ftcore::mapOverlayFromScreen(View, {static_cast<float>(Cursor.X), static_cast<float>(Cursor.Y)});
	Zoom = Z;
	FrameOn(Under);
}

void FFTMapOverlay::FrameOn(const ftcore::Vec2& At)
{
	const ftcore::MapFrame Frame = ftcore::mapOverlayFrame(Model, Look, At, Zoom, View.screenW, View.screenH);
	View = Frame.view;
	Framed = View.centre;
	FramedLand = Frame.land;
	bFramedReached = Frame.reached;
	Cursor = FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, At));
}

void FFTMapOverlay::ReadInput(APlayerController& PC, float S, float Dt)
{
	using namespace FTMapOverlayDetail;
	if (PC.WasInputKeyJustPressed(EKeys::MouseScrollUp) || PC.WasInputKeyJustPressed(EKeys::Gamepad_RightTrigger))
	{
		SetZoom(ftcore::mapZoomIn(Zoom));
	}
	if (PC.WasInputKeyJustPressed(EKeys::MouseScrollDown) || PC.WasInputKeyJustPressed(EKeys::Gamepad_LeftTrigger))
	{
		SetZoom(ftcore::mapZoomOut(Zoom));
	}
	float MX = 0.f, MY = 0.f;
	PC.GetInputMouseDelta(MX, MY);
	FVector2D Move(MX * 10.0 * S, -MY * 10.0 * S);
	const float RX = PC.GetInputAnalogKeyState(EKeys::Gamepad_RightX), RY = PC.GetInputAnalogKeyState(EKeys::Gamepad_RightY);
	const float Speed = 900.f * S * Dt;
	if (FMath::Abs(RX) > 0.15f || FMath::Abs(RY) > 0.15f)
	{
		Move += FVector2D(RX, -RY) * Speed;
	}
	Move.X += ((PC.IsInputKeyDown(EKeys::Right) ? 1.f : 0.f) - (PC.IsInputKeyDown(EKeys::Left) ? 1.f : 0.f)) * Speed;
	Move.Y += ((PC.IsInputKeyDown(EKeys::Down) ? 1.f : 0.f) - (PC.IsInputKeyDown(EKeys::Up) ? 1.f : 0.f)) * Speed;
	Cursor += Move;
	// At an edge the view pans under the cursor instead (held inside the map's area, widened to take in the framed centre).
	const float Edge = kOvEdgePx * S;
	const FVector2D Lo(Edge, Edge), Hi(View.screenW - Edge, View.screenH - Edge);
	const FVector2D Over(Cursor.X < Lo.X ? Cursor.X - Lo.X : (Cursor.X > Hi.X ? Cursor.X - Hi.X : 0.0),
		Cursor.Y < Lo.Y ? Cursor.Y - Lo.Y : (Cursor.Y > Hi.Y ? Cursor.Y - Hi.Y : 0.0));
	if (!Over.IsNearlyZero())
	{
		const ftcore::Vec2 Was = View.centre;
		View.centre = {Was.x + static_cast<float>(Over.X) * View.mPerPx, Was.y + static_cast<float>(Over.Y) * View.mPerPx};
		View.centre = ftcore::mapOverlayHold(Model, View, Framed);
		Cursor -= FVector2D((View.centre.x - Was.x) / View.mPerPx, (View.centre.y - Was.y) / View.mPerPx);
	}
	Cursor.X = FMath::Clamp(Cursor.X, 0.0, static_cast<double>(View.screenW));
	Cursor.Y = FMath::Clamp(Cursor.Y, 0.0, static_cast<double>(View.screenH));
}

void FFTMapOverlay::AddTri(FCanvasTriangleItem& To, const FVector2D& A, const FVector2D& B, const FVector2D& C, const FLinearColor& Color)
{
	To.TriangleList.Add(FTMapOverlayDetail::OvTri(A, B, C, Color));
}

void FFTMapOverlay::AddScreenRect(FCanvasTriangleItem& To, float X0, float Y0, float X1, float Y1, const FLinearColor& Color)
{
	if (X1 <= X0 || Y1 <= Y0)
	{
		return;
	}
	AddTri(To, FVector2D(X0, Y0), FVector2D(X1, Y0), FVector2D(X1, Y1), Color);
	AddTri(To, FVector2D(X0, Y0), FVector2D(X1, Y1), FVector2D(X0, Y1), Color);
}

void FFTMapOverlay::AddRect(const ftcore::MapRect& R, const FLinearColor& Color)
{
	const float C = FMath::Cos(R.rot), Sn = FMath::Sin(R.rot);
	const float Ox[4] = {-R.hx, R.hx, R.hx, -R.hx}, Oy[4] = {-R.hy, -R.hy, R.hy, R.hy};
	FVector2D P[4];
	for (int32 I = 0; I < 4; ++I)
	{
		P[I] = FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, {R.cx + Ox[I] * C - Oy[I] * Sn, R.cy + Ox[I] * Sn + Oy[I] * C}));
	}
	AddTri(*Geo, P[0], P[1], P[2], Color);
	AddTri(*Geo, P[0], P[2], P[3], Color);
}

void FFTMapOverlay::AddRoof(const ftcore::MapRect& R, float StepM, float WidthPx, const FLinearColor& Hatch, const FLinearColor& Lit,
	const FLinearColor& Shade)
{
	const float C = FMath::Cos(R.rot), Sn = FMath::Sin(R.rot);
	auto Local = [&](float U, float V) {
		return FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, {R.cx + U * C - V * Sn, R.cy + U * Sn + V * C}));
	};
	// The hatch: lines along the long side, inset a little, at most 12.
	if (StepM > 0.f)
	{
		const bool bAlongX = R.hx >= R.hy;
		const float Long = (bAlongX ? R.hx : R.hy) * 0.9f, Short = bAlongX ? R.hy : R.hx;
		const int32 N = FMath::Min(static_cast<int32>(2.f * Short / StepM), 13);
		for (int32 I = 1; I < N; ++I)
		{
			const float T = -Short + 2.f * Short * static_cast<float>(I) / static_cast<float>(N);
			const FVector2D A = bAlongX ? Local(-Long, T) : Local(T, -Long);
			const FVector2D B = bAlongX ? Local(Long, T) : Local(T, Long);
			AddSegment(*Geo, A, B, WidthPx, Hatch, false);
		}
	}
	// The edges: the one facing north on screen lit, the one facing south dark (light from the top of the screen).
	const float Ox[4] = {-R.hx, R.hx, R.hx, -R.hx}, Oy[4] = {-R.hy, -R.hy, R.hy, R.hy};
	FVector2D P[4];
	for (int32 I = 0; I < 4; ++I)
	{
		P[I] = Local(Ox[I], Oy[I]);
	}
	const FVector2D Centre = (P[0] + P[2]) * 0.5;
	int32 North = 0, South = 0;
	double MinY = 1e30, MaxY = -1e30;
	for (int32 I = 0; I < 4; ++I)
	{
		const double Y = (P[I] + P[(I + 1) % 4]).Y * 0.5 - Centre.Y;
		if (Y < MinY)
		{
			MinY = Y;
			North = I;
		}
		if (Y > MaxY)
		{
			MaxY = Y;
			South = I;
		}
	}
	AddSegment(*Geo, P[North], P[(North + 1) % 4], WidthPx, Lit, false);
	AddSegment(*Geo, P[South], P[(South + 1) % 4], WidthPx, Shade, false);
}

void FFTMapOverlay::AddSegment(FCanvasTriangleItem& To, const FVector2D& Pa, const FVector2D& Pb, float WidthPx, const FLinearColor& Color, bool bCaps)
{
	AddShadedSegment(To, Pa, Pb, WidthPx, Color, Color, bCaps, bCaps);
}

void FFTMapOverlay::AddShadedSegment(FCanvasTriangleItem& To, const FVector2D& Pa, const FVector2D& Pb, float WidthPx, const FLinearColor& ColorA,
	const FLinearColor& ColorB, bool bCapA, bool bCapB)
{
	FVector2D D = Pb - Pa;
	const double Len = D.Size();
	if (Len < 1e-3)
	{
		return;
	}
	D /= Len;
	const FVector2D N(-D.Y * WidthPx * 0.5f, D.X * WidthPx * 0.5f);
	// Square caps close a road's joints.
	const FVector2D A = bCapA ? Pa - D * WidthPx * 0.5f : Pa;
	const FVector2D B = bCapB ? Pb + D * WidthPx * 0.5f : Pb;
	FCanvasUVTri T0 = FTMapOverlayDetail::OvTri(A + N, B + N, B - N, ColorB);
	T0.V0_Color = ColorA;
	FCanvasUVTri T1 = FTMapOverlayDetail::OvTri(A + N, B - N, A - N, ColorA);
	T1.V1_Color = ColorB;
	To.TriangleList.Add(T0);
	To.TriangleList.Add(T1);
}

void FFTMapOverlay::AddRoadSegment(const ftcore::Vec2& A, const ftcore::Vec2& B, float WidthPx, const FLinearColor& Color, bool bCaps)
{
	AddSegment(*Geo, FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, A)), FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, B)),
		WidthPx, Color, bCaps);
}

void FFTMapOverlay::AddFadedRoadSegment(size_t R, uint32 K, float S0, float FadeM, float WidthPx, const FLinearColor& Color, bool bCaps)
{
	const ftcore::MapRoad& Road = Model.roads[R];
	const ftcore::Vec2 A = Model.points[Road.first + K];
	const ftcore::Vec2 B = Model.points[Road.first + K + 1];
	const uint8 Off = R < RoadOff.size() ? RoadOff[R] : 0;
	const float Len = R < RoadLen.size() ? RoadLen[R] : 0.f;
	if (Off == 0 || !(FadeM > 0.f))
	{
		AddRoadSegment(A, B, WidthPx, Color, bCaps);
		return;
	}
	const float SegLen = FMath::Sqrt((B.x - A.x) * (B.x - A.x) + (B.y - A.y) * (B.y - A.y));
	if (SegLen < 1e-4f)
	{
		return;
	}
	// The pieces: cut where the fade begins at each off end, inside this segment.
	float Cut[4] = {0.f, 0.f, 0.f, SegLen};
	int32 Cuts = 1;
	const float C0 = (Off & ftcore::kMapRoadOffFirst) ? FadeM - S0 : -1.f;
	const float C1 = (Off & ftcore::kMapRoadOffLast) ? Len - FadeM - S0 : -1.f;
	for (const float C : {FMath::Min(C0, C1), FMath::Max(C0, C1)})
	{
		if (C > Cut[Cuts - 1] && C < SegLen)
		{
			Cut[Cuts++] = C;
		}
	}
	Cut[Cuts] = SegLen;
	for (int32 I = 0; I < Cuts; ++I)
	{
		const float T0 = Cut[I], T1 = Cut[I + 1];
		const float F0 = ftcore::mapRoadFade(S0 + T0, Len, Off, FadeM), F1 = ftcore::mapRoadFade(S0 + T1, Len, Off, FadeM);
		if (F0 <= 0.f && F1 <= 0.f)
		{
			continue;
		}
		const ftcore::Vec2 P0{A.x + (B.x - A.x) * (T0 / SegLen), A.y + (B.y - A.y) * (T0 / SegLen)};
		const ftcore::Vec2 P1{A.x + (B.x - A.x) * (T1 / SegLen), A.y + (B.y - A.y) * (T1 / SegLen)};
		FLinearColor Ca = Color, Cb = Color;
		Ca.A *= F0;
		Cb.A *= F1;
		// Caps only at the segment's own ends, so the cut pieces do not overlap.
		AddShadedSegment(*Geo, FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, P0)),
			FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, P1)), WidthPx, Ca, Cb, bCaps && I == 0, bCaps && I + 1 == Cuts);
	}
}

FVector2D FFTMapOverlay::Measure(FStringView Text, EFace Face, float Px) const
{
	if (!FSlateApplication::IsInitialized() || !Fonts[Face])
	{
		return FVector2D(Text.Len() * Px * 0.5f, Px * 1.2f);
	}
	const FSlateFontInfo Info(Fonts[Face], FMath::Max(1.f, Px * 0.75f));
	return FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, Info, 1.f);
}

FVector2D FFTMapOverlay::NameSize(int32 Name, EFace Face, float Px)
{
	using namespace FTMapOverlayDetail;
	if (!NameRef[Face].IsValidIndex(Name))
	{
		return FVector2D::ZeroVector;
	}
	FVector2f& Ref = NameRef[Face][Name];
	if (Ref.X < 0.f)
	{
		const FVector2D M = Measure(NameText[Name], Face, kOvRefPx);
		Ref = FVector2f(static_cast<float>(M.X), static_cast<float>(M.Y));
	}
	return FVector2D(Ref.X, Ref.Y) * (Px / kOvRefPx);
}

void FFTMapOverlay::DrawText(UCanvas& Canvas, FStringView Text, EFace Face, float Px, const FVector2D& TopLeft, const FLinearColor& Color,
	float HaloPx) const
{
	if (!Fonts[Face] || Text.IsEmpty())
	{
		return;
	}
	const FSlateFontInfo Info(Fonts[Face], FMath::Max(1.f, Px * 0.75f));
	if (HaloPx > 0.f)
	{
		// The halo: the text in a faint dark, offset all round. Each copy is drawn outlined, so the engine adds its four
		// diagonal neighbours 1 px off within the same item: four items along the axes (every other ring turned 45 degrees)
		// give twenty overlapping copies, dark at the glyphs and soft at the edge, out to HaloPx. A ring per 3 px of halo
		// (a larger text size). The directions are a fixed table; drawn in the text's own frame, so a turned or leaning name
		// has it too.
		static constexpr float H = 0.70710678f;
		static const FVector2D kDir[8] = {{1.0, 0.0}, {0.0, 1.0}, {-1.0, 0.0}, {0.0, -1.0}, {H, H}, {-H, H}, {-H, -H}, {H, -H}};
		const int32 Rings = FMath::Clamp(FMath::RoundToInt(HaloPx / 3.f), 1, 3);
		for (int32 Ring = 1; Ring <= Rings; ++Ring)
		{
			const float Reach = FMath::Max(HaloPx * static_cast<float>(Ring) / static_cast<float>(Rings) - 1.f, 1.f);
			const int32 First = (Ring & 1) ? 0 : 4;
			for (int32 K = 0; K < 4; ++K)
			{
				FCanvasTextStringViewItem Copy(TopLeft + kDir[First + K] * Reach, Text, Info, FTMapOverlayDetail::kOvHaloSoft);
				Copy.bOutlined = true;
				Copy.OutlineColor = FTMapOverlayDetail::kOvHaloSoft;
				Copy.BlendMode = SE_BLEND_Translucent;
				Canvas.DrawItem(Copy);
				++TextDraws;
			}
		}
	}
	FCanvasTextStringViewItem Item(TopLeft, Text, Info, Color);
	Item.bOutlined = true; // a dark rim keeps the name clear of the gold lines
	Item.OutlineColor = FTMapOverlayDetail::kOvHalo;
	Item.BlendMode = SE_BLEND_Translucent;
	Canvas.DrawItem(Item);
	++TextDraws;
}

void FFTMapOverlay::BuildLabels(int32 Picked, uint16 PickedHere, float S, float TS, const ftcore::MapDisc* Discs, int32 NumDiscs,
	const ftcore::MapLabel& Kept)
{
	using namespace FTMapOverlayDetail;
	Placed.count = 0;
	LabelDiscCount = FMath::Clamp(NumDiscs, 0, static_cast<int32>(UE_ARRAY_COUNT(LabelDiscs)));
	for (int32 K = 0; K < LabelDiscCount; ++K)
	{
		LabelDiscs[K] = Discs[K];
	}
	bCursorNameMoved = bCursorNameDropped = false;
	if (Names.names.empty())
	{
		return;
	}
	// The sea's name, once, level, over open sea (none when the table names no sea or no box of it lies wholly on sea).
	ftcore::Vec2 SeaAt{};
	bool bSeaName = false;
	if (Names.seaName != ftcore::kMapNoName)
	{
		const FOvStyle SeaStyle = OvStyle(ftcore::MapLabelRank::Water, true);
		const FVector2D SeaSize = NameSize(Names.seaName, static_cast<EFace>(SeaStyle.Face), SeaStyle.Px * TS);
		bSeaName = ftcore::mapSeaLabelAt(Model, Look, View, static_cast<float>(SeaSize.X), static_cast<float>(SeaSize.Y), SeaAt);
	}
	// The cursor's name is the one at the cursor on its road (a zone's part of a shared run names itself).
	const size_t NPicks = ftcore::mapLabelPicks(Model, Names, View, Zoom, Picked, Picks, kMaxPicks, Seen.data(), 24.f * S, PickedHere,
		bSeaName ? &SeaAt : nullptr);
	const uint16 PickedName = ftcore::mapRoadName(Names, Picked);
	const float Halo = ftcore::kMapLabelHaloPx * TS;
	const ftcore::MapRoadInk Ink = OvInk(S);
	DropCount = DropTotal = 0;
	auto Drop = [this](uint16 Name, EDrop Why) {
		if (DropCount < kMaxDrops)
		{
			Drops[DropCount++] = {Name, Why};
		}
		++DropTotal;
	};
	size_t Count = 0;
	for (size_t I = 0; I < NPicks && Count < ftcore::kMaxMapLabels; ++I)
	{
		const ftcore::MapLabelPick& P = Picks[I];
		const bool bWater = P.name < Names.nameKind.size() && Names.nameKind[P.name] == ftcore::MapNameKind::Water;
		const FOvStyle St = OvStyle(P.rank, bWater);
		const FVector2D Text = NameSize(P.name, static_cast<EFace>(St.Face), St.Px * TS);
		float W = static_cast<float>(Text.X), H = static_cast<float>(Text.Y);
		if (St.bBox)
		{
			const float Sign = SignCount.IsValidIndex(P.name) && SignCount[P.name] > 0 ? SignWidth[P.name] * kOvSignPx * TS : 0.f;
			W = FMath::Max(W + 2.f * kOvBoxPadX * TS, Sign);
			H += 2.f * kOvBoxPadY * TS + (Sign > 0.f ? (kOvSignPx + kOvSignGap) * TS : 0.f);
		}
		ftcore::MapLabel L;
		float Run = 0.f; // the stretch's length on screen (px)
		if (P.along)
		{
			if (!ftcore::mapLabelAlong(Model, Names, P.name, View, W, H, L, 24.f * S, 8.f * S, &Run))
			{
				continue;
			}
			if (!L.fits && P.rank != ftcore::MapLabelRank::Cursor)
			{
				continue; // a name longer than its stretch on screen is left out, never squeezed
			}
		}
		else
		{
			L.at = ftcore::mapOverlayToScreen(View, P.at);
			L.w = W;
			L.h = H;
			if (L.at.x < 0.f || L.at.y < 0.f || L.at.x > View.screenW || L.at.y > View.screenH)
			{
				continue;
			}
		}
		// A water name (but the sea's, already over open sea) sits on its own water, its lean included, clear of every road
		// drawn at the zoom: never over land or a road. One with no seat is left out.
		const bool bSeatWater = bWater && !P.along && P.name != Names.seaName;
		if (bSeatWater)
		{
			const ftcore::MapWaterSeat Seat = ftcore::mapWaterLabelAt(Model, Names, Look, View, Zoom, Ink, P.at, W + kOvShear * H, H, Halo, L,
				Picked, PickedName);
			if (Seat != ftcore::MapWaterSeat::Seated)
			{
				Drop(P.name, Seat == ftcore::MapWaterSeat::Dry ? EDrop::Dry : EDrop::NoSeat);
				continue;
			}
		}
		// Every name stays whole on the screen, its halo too: one at an edge moves in; one too big for the screen is left out.
		if (!ftcore::mapLabelKeepInside(L, View.screenW, View.screenH, Halo))
		{
			continue;
		}
		if (P.rank == ftcore::MapLabelRank::Cursor)
		{
			// The cursor's name never lies under the cursor's ring or the player's arrow, its halo included: it slides along
			// its line (up to half its stretch, at least its own width), else steps off the line beside it.
			const ftcore::Vec2 Home = L.at;
			if (!ftcore::mapLabelClearOfDiscs(L, LabelDiscs, static_cast<size_t>(LabelDiscCount), Halo, FMath::Max(Run * 0.5f, W),
					2.f * H + 32.f * S, View.screenW, View.screenH))
			{
				bCursorNameDropped = true;
				continue;
			}
			bCursorNameMoved = L.at.x != Home.x || L.at.y != Home.y;
		}
		if (!P.along && P.rank == ftcore::MapLabelRank::Place)
		{
			// A place's name meets no road drawn at the zoom (the cursor's road at its lit width): one on a road moves the least
			// way off it; one with no clear spot near is left out.
			if (!ftcore::mapLabelOffRoads(Model, Names, View, Zoom, Ink, L, Halo, Picked, PickedName))
			{
				Drop(P.name, EDrop::OnRoad);
				continue;
			}
		}
		else if (!bSeatWater && P.rank != ftcore::MapLabelRank::Cursor && Picked >= 0 &&
			ftcore::mapLabelOnAnyRoad(Model, Names, View, Zoom, Ink, L, Halo, Picked, PickedName, true) >= 0)
		{
			// No other name lies on the cursor's lit road, which is drawn wider than a halo hides.
			continue;
		}
		L.name = P.name;
		L.priority = static_cast<std::uint8_t>(P.rank);
		Candidates[Count++] = L;
	}
	// No two labels overlap, each one's halo included, and none covers the where-am-I line's kept box.
	ftcore::mapPlaceLabels(Candidates, Count, Placed, 6.f * TS, &Kept, 1, Halo);
	LastPicks = static_cast<int32>(NPicks);
	LastCandidates = static_cast<int32>(Count);
}

void FFTMapOverlay::DrawLabel(UCanvas& Canvas, const ftcore::MapLabel& L, float TS)
{
	using namespace FTMapOverlayDetail;
	if (!NameText.IsValidIndex(L.name))
	{
		return;
	}
	const bool bWater = L.name < Names.nameKind.size() && Names.nameKind[L.name] == ftcore::MapNameKind::Water;
	const FOvStyle St = OvStyle(static_cast<ftcore::MapLabelRank>(L.priority), bWater);
	const EFace Face = static_cast<EFace>(St.Face);
	const float Px = St.Px * TS;
	const FVector2D Text = NameSize(L.name, Face, Px);
	FVector2D TopLeft(L.at.x - Text.X * 0.5, L.at.y - Text.Y * 0.5);
	if (St.bBox)
	{
		// The text sits in the box at the label's foot; the sign form is above it.
		TopLeft.Y = L.at.y + L.h * 0.5f - kOvBoxPadY * TS - Text.Y;
	}
	FCanvas* C = Canvas.Canvas;
	const bool bTurn = FMath::Abs(L.angleDeg) > 0.5f;
	const bool bLean = St.bItalic;
	if (C && (bTurn || bLean))
	{
		const FVector At(L.at.x, L.at.y, 0.0);
		FMatrix Shear = FMatrix::Identity;
		if (bLean)
		{
			Shear.M[1][0] = -kOvShear; // the top of the text leans forward (y is down)
		}
		C->PushRelativeTransform(FTranslationMatrix(-At) * Shear * FRotationMatrix(FRotator(0.0, L.angleDeg, 0.0)) * FTranslationMatrix(At));
	}
	// A boxed name sits on its box's dark fill: no halo.
	DrawText(Canvas, NameText[L.name], Face, Px, TopLeft, St.Color, St.bBox ? 0.f : ftcore::kMapLabelHaloPx * TS);
	if (C && (bTurn || bLean))
	{
		C->PopTransform();
	}
}

void FFTMapOverlay::Draw(AHUD& Hud, UCanvas& Canvas, float S)
{
	using namespace FTMapOverlayDetail;
	APlayerController* PC = Hud.GetOwningPlayerController();
	const float SW = Canvas.ClipX, SH = Canvas.ClipY;
	// The names and the where-am-I line at the player's text size (FT::HudTextScale: 1 without -UiHudTextScale); the map's
	// own lines and marks stay at S.
	const float TS = S * FT::HudTextScale();
	if (GFrameCounter > LastFrame + 1 || View.screenW != SW || View.screenH != SH)
	{
		OnOpen(Hud, SW, SH);
	}
	LastFrame = GFrameCounter;
	TextDraws = 0;
	if (PC)
	{
		ReadInput(*PC, S, FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.1f));
	}
	const ftcore::Vec2 CursorWorld = ftcore::mapOverlayFromScreen(View, {static_cast<float>(Cursor.X), static_cast<float>(Cursor.Y)});
	const int32 Picked = ftcore::mapRoadNear(Model, CursorWorld, kOvCursorReachPx * S * View.mPerPx, Zoom);
	const uint16 PickedName = ftcore::mapRoadName(Names, Picked);
	const uint16 PickedHere = Picked >= 0 ? ftcore::mapNameAt(Model, Names, Picked, CursorWorld).name : ftcore::kMapNoName;

	// The view's area in metres, a little grown, for culling.
	const ftcore::Vec2 Lo = ftcore::mapOverlayFromScreen(View, {-20.f, -20.f});
	const ftcore::Vec2 Hi = ftcore::mapOverlayFromScreen(View, {SW + 20.f, SH + 20.f});
	auto InView = [&](float X0, float Y0, float X1, float Y1) { return X1 >= Lo.x && X0 <= Hi.x && Y1 >= Lo.y && Y0 <= Hi.y; };
	auto RectInView = [&](const ftcore::MapRect& R) {
		const float E = FMath::Max(R.hx, R.hy) * 1.415f;
		return InView(R.cx - E, R.cy - E, R.cx + E, R.cy + E);
	};

	Geo->TriangleList.Reset();
	// The ground runs on past the map's area (drawn, though the framing does not count it as land: nothing is mapped
	// there); the sea past the area on the sea's side, at full height, and past the north and south edges over their sea
	// (ftcore::mapIsSea).
	{
		const ftcore::Vec2 A = ftcore::mapOverlayToScreen(View, {Model.minX, Model.minY});
		const ftcore::Vec2 B = ftcore::mapOverlayToScreen(View, {Model.maxX, Model.maxY});
		AddScreenRect(*Geo, 0.f, 0.f, SW, SH, kOvLand);
		if (SeaSide > 0)
		{
			AddScreenRect(*Geo, FMath::Max(B.x, 0.f), 0.f, SW, SH, kOvSea);
		}
		else if (SeaSide < 0)
		{
			AddScreenRect(*Geo, 0.f, 0.f, FMath::Min(A.x, SW), SH, kOvSea);
		}
	}
	for (const ftcore::MapRect& Run : SeaEdge)
	{
		if (RectInView(Run))
		{
			AddRect(Run, kOvSea);
		}
	}
	for (const ftcore::MapRect& R : Water)
	{
		if (RectInView(R))
		{
			AddRect(R, kOvSea);
		}
	}
	// The look, built at load and culled by tile (nothing allocated): the parks' hatch and the yards' dots on the ground,
	// the wave lines and the coast on the sea, the canals' edges. A stroke with no length is a dot.
	auto Strokes = [&](ftcore::MapStrokeKind Kind, float LinePx, const FLinearColor& Ink) {
		ftcore::mapEachStroke(Look, Kind, Lo, Hi, [&](const ftcore::MapStroke& Sk) {
			if (!ftcore::mapStrokeShown(Sk, Zoom) ||
				!InView(FMath::Min(Sk.a.x, Sk.b.x), FMath::Min(Sk.a.y, Sk.b.y), FMath::Max(Sk.a.x, Sk.b.x), FMath::Max(Sk.a.y, Sk.b.y)))
			{
				return;
			}
			const FVector2D Pa = OvV2(ftcore::mapOverlayToScreen(View, Sk.a));
			if (Sk.a.x == Sk.b.x && Sk.a.y == Sk.b.y)
			{
				const float Half = LinePx * 0.5f, Px0 = static_cast<float>(Pa.X), Py0 = static_cast<float>(Pa.Y);
				AddScreenRect(*Geo, Px0 - Half, Py0 - Half, Px0 + Half, Py0 + Half, Ink);
				return;
			}
			AddSegment(*Geo, Pa, OvV2(ftcore::mapOverlayToScreen(View, Sk.b)), LinePx, Ink, false);
		});
	};
	auto GoldAt = [](float Alpha) { return FLinearColor(kOvGold.R, kOvGold.G, kOvGold.B, Alpha); };
	const float FinePx = FMath::Max(kOvFinePx * S, kOvFinePx);
	Strokes(ftcore::MapStrokeKind::ParkHatch, FinePx, GoldAt(kOvParkAlpha));
	Strokes(ftcore::MapStrokeKind::YardDot, FMath::Max(kOvDotPx * S, 1.f), GoldAt(kOvYardAlpha));
	const ftcore::MapStrokeKind Waves[3] = {ftcore::MapStrokeKind::Wave12, ftcore::MapStrokeKind::Wave24, ftcore::MapStrokeKind::Wave40};
	for (int32 K = 0; K < 3; ++K)
	{
		Strokes(Waves[K], FinePx, GoldAt(kOvWaveAlpha[K]));
	}
	Strokes(ftcore::MapStrokeKind::Canal, FinePx, GoldAt(kOvCanalAlpha));
	const float CoastPx = FMath::Max(kOvRoadLinePx[0] * 0.5f * S, kOvFinePx);
	Strokes(ftcore::MapStrokeKind::Coast, CoastPx, GoldAt(kOvCoastAlpha));
	// The coast past the model's edges, wherever the sea drawn there meets ground: level or upright lines, clipped to the
	// view (they run on for kilometres).
	for (const ftcore::MapStroke& Sk : Look.edgeCoast)
	{
		const float X0 = FMath::Max(FMath::Min(Sk.a.x, Sk.b.x), Lo.x), X1 = FMath::Min(FMath::Max(Sk.a.x, Sk.b.x), Hi.x);
		const float Y0 = FMath::Max(FMath::Min(Sk.a.y, Sk.b.y), Lo.y), Y1 = FMath::Min(FMath::Max(Sk.a.y, Sk.b.y), Hi.y);
		if (X1 < X0 || Y1 < Y0)
		{
			continue;
		}
		AddSegment(*Geo, OvV2(ftcore::mapOverlayToScreen(View, {X0, Y0})), OvV2(ftcore::mapOverlayToScreen(View, {X1, Y1})), CoastPx,
			GoldAt(kOvCoastAlpha), false);
	}
	// The blocks: dark roofs; from mid, a faint hatch and lit edges (until the top-down relief bake replaces them).
	const bool bRoofs = Zoom != ftcore::MapZoom::Far;
	const float HatchM = Zoom == ftcore::MapZoom::Near ? 5.f * S * View.mPerPx : 0.f;
	for (size_t B = 0; B < Model.buildings.size(); ++B)
	{
		const ftcore::MapRect& R = Model.buildings[B];
		if (!RectInView(R))
		{
			continue;
		}
		AddRect(R, B < Enterable.size() && Enterable[B] ? kOvEnterable : kOvBlock);
		if (bRoofs)
		{
			AddRoof(R, HatchM, FMath::Max(0.8f * S, 0.8f), kOvHatch, kOvRoofLit, kOvRoofShade);
		}
	}
	// Roads: a soft glow under the roads that carry names from mid (main roads and streets), then every road in gold (cased when wide enough: the gold
	// full width first, the dark inside after, so the junctions join), then the cursor's road lit along its whole length.
	auto IsLit = [&](size_t R) { return ftcore::mapRoadLit(Names, R, Picked, PickedName); };
	const ftcore::MapRoadInk RoadInk = OvInk(S);
	// Roads that run off the model fade out over their last kOvRoadFadePx, so none stops dead at the model's edge.
	const float FadeM = kOvRoadFadePx * S * View.mPerPx;
	enum EPass { Glow, Gold, Fill, LitPass };
	for (int32 Pass = Glow; Pass <= LitPass; ++Pass)
	{
		// Lower classes first so main roads lie on top.
		for (int32 Cls = ftcore::kMapRoadClasses - 1; Cls >= 0; --Cls)
		{
			for (size_t R = 0; R < Model.roads.size(); ++R)
			{
				const ftcore::MapRoad& Road = Model.roads[R];
				if (static_cast<int32>(Road.cls) != Cls)
				{
					continue;
				}
				const bool bLit = IsLit(R);
				if ((Pass == LitPass && !bLit) || (Pass == Glow && !ftcore::mapRoadLabelled(Road.cls, ftcore::MapZoom::Mid)))
				{
					continue;
				}
				if ((!bLit && !ftcore::mapRoadShown(Road.cls, Zoom)) || !InView(RoadBox[R * 4], RoadBox[R * 4 + 1], RoadBox[R * 4 + 2], RoadBox[R * 4 + 3]))
				{
					continue;
				}
				const float Px = Road.width / View.mPerPx;
				const bool bCased = ftcore::mapRoadCased(Road, View, RoadInk);
				float Width = 0.f;
				FLinearColor Color = kOvGold;
				bool bCaps = true;
				switch (Pass)
				{
				case Glow:
					Width = (bCased ? Px : kOvRoadLinePx[Cls] * S) + kOvGlowPx * S;
					Color = kOvGlow;
					bCaps = false;
					break;
				case Gold:
					Width = ftcore::mapRoadDrawnPx(Road, View, RoadInk);
					Color.A = kOvRoadAlpha[Cls];
					break;
				case Fill:
					if (!bCased)
					{
						continue;
					}
					Width = Px - 2.f * kOvEdgeLinePx * S;
					Color = kOvRoadFill;
					break;
				default:
					Width = ftcore::mapRoadLitPx(Road, View, RoadInk);
					Color = kOvLit;
					break;
				}
				float S0 = 0.f;
				for (uint32 K = 0; K + 1 < Road.count && Road.first + K + 1 < Model.points.size(); ++K)
				{
					AddFadedRoadSegment(R, K, S0, FadeM, Width, Color, bCaps);
					const ftcore::Vec2 A = Model.points[Road.first + K], B = Model.points[Road.first + K + 1];
					S0 += FMath::Sqrt((B.x - A.x) * (B.x - A.x) + (B.y - A.y) * (B.y - A.y));
				}
			}
		}
	}
	Canvas.DrawItem(*Geo); // never empty: the letterbox

	// The marks drawn over the names: the cursor's ring with its ticks, and the player's arrow (the van's mark when aboard).
	const APawn* Pawn = Hud.GetOwningPawn();
	const AFTPartyVan* TheVan = Van.Get();
	const bool bInVan = TheVan && TheVan->IsAboard(Cast<AFTExploreCharacter>(Pawn));
	const AActor* Who = bInVan ? static_cast<const AActor*>(TheVan) : Pawn;
	ftcore::MapDisc Discs[2];
	int32 NumDiscs = 0;
	Discs[NumDiscs++] = {{static_cast<float>(Cursor.X), static_cast<float>(Cursor.Y)}, kOvCursorMarkPx * S};
	if (Who)
	{
		Discs[NumDiscs++] = {ftcore::mapOverlayToScreen(View, OvMetres(Who->GetActorLocation())), kOvArrowMarkPx * S};
	}
	// Whether the cursor's name must be placed again for the marks: it lies under one (the cursor moved along its road), or
	// it was moved or left out for them and they have moved since.
	bool bMarksMoved = NumDiscs != LabelDiscCount;
	for (int32 K = 0; K < NumDiscs && !bMarksMoved; ++K)
	{
		bMarksMoved = FMath::Abs(Discs[K].at.x - LabelDiscs[K].at.x) >= 2.f || FMath::Abs(Discs[K].at.y - LabelDiscs[K].at.y) >= 2.f;
	}
	bool bNameUnderMark = false;
	for (size_t I = 0; I < Placed.count && !bNameUnderMark; ++I)
	{
		const ftcore::MapLabel& L = Placed.labels[I];
		for (int32 K = 0; L.priority == static_cast<uint8>(ftcore::MapLabelRank::Cursor) && K < NumDiscs && !bNameUnderMark; ++K)
		{
			bNameUnderMark = ftcore::mapLabelMeetsDisc(L, ftcore::kMapLabelHaloPx * LabelTS, Discs[K]);
		}
	}
	// Where the player is, bottom left: the district in small caps, a small diamond, the street, over a thin gold rule, on a
	// dark plate laid out from the measured text (ftcore::mapCaptionLayout: a long name grows the plate and the box no label
	// enters; one too long for the screen is scaled to fit). Read each frame from where the player stands
	// (ftcore::mapHereAt: the name of the road's part there, the district from that name's row).
	uint16 HereDistrict = ftcore::kMapNoName, HereStreet = ftcore::kMapNoName;
	if (Pawn)
	{
		const ftcore::MapHere Here = ftcore::mapHereAt(Model, Names, OvMetres(Pawn->GetActorLocation()));
		HereDistrict = Here.district < Names.districts.size() ? Names.districts[Here.district].name : ftcore::kMapNoName;
		HereStreet = Here.name;
	}
	CaptionDistrict = HereDistrict;
	CaptionStreet = HereStreet;
	const float HerePx = 18.f * TS;
	const FVector2D DistrictSize = NameText.IsValidIndex(HereDistrict) ? NameSize(HereDistrict, SerifSc, HerePx) : FVector2D::ZeroVector;
	const FVector2D StreetSize = NameText.IsValidIndex(HereStreet) && HereStreet != HereDistrict ? NameSize(HereStreet, SerifSc, HerePx) : FVector2D::ZeroVector;
	const ftcore::MapCaption Cap = ftcore::mapCaptionLayout(View.screenW, View.screenH, TS, static_cast<float>(DistrictSize.X),
		static_cast<float>(StreetSize.X), static_cast<float>(FMath::Max(DistrictSize.Y, StreetSize.Y)), FMath::Max(1.f * S, 1.f));
	CaptionRight = Cap.shown ? Cap.ruleRight : 0.f;
	CaptionBottom = Cap.shown ? Cap.ruleBottom : 0.f;
	CaptionFit = Cap.fit;
	CaptionTextRight = Cap.shown ? FMath::Max(Cap.text.x + Cap.districtW, Cap.streetX + Cap.streetW) : 0.f;
	CaptionPlateRight = Cap.shown ? Cap.plate.at.x + Cap.plate.w * 0.5f : 0.f;

	// The labels, placed again only when the view, the zoom, the cursor's road, the caption's kept box or (for the cursor's
	// name) the marks change.
	if (LabelPicked != Picked || LabelPickedName != PickedHere || LabelZoom != Zoom || LabelView.centre.x != View.centre.x ||
		LabelView.centre.y != View.centre.y || LabelView.mPerPx != View.mPerPx || LabelView.screenW != View.screenW ||
		LabelView.screenH != View.screenH || LabelTS != TS || LabelS != S || bNameUnderMark ||
		LabelKept.w != Cap.kept.w || LabelKept.h != Cap.kept.h || (bMarksMoved && (bCursorNameMoved || bCursorNameDropped)))
	{
		BuildLabels(Picked, PickedHere, S, TS, Discs, NumDiscs, Cap.kept);
		LabelKept = Cap.kept;
		LabelS = S;
		LabelPicked = Picked;
		LabelPickedName = PickedHere;
		LabelZoom = Zoom;
		LabelView = View;
		LabelTS = TS;
	}

	// The districts' boxes: a dark fill, a thin gold rule, the sign form small above.
	Boxes->TriangleList.Reset();
	for (size_t I = 0; I < Placed.count; ++I)
	{
		const ftcore::MapLabel& L = Placed.labels[I];
		if (static_cast<ftcore::MapLabelRank>(L.priority) != ftcore::MapLabelRank::District)
		{
			continue;
		}
		const bool bSign = SignCount.IsValidIndex(L.name) && SignCount[L.name] > 0;
		const FVector2D Text = NameSize(L.name, SerifSc, 15.f * TS);
		const float BoxW = static_cast<float>(Text.X) + 2.f * kOvBoxPadX * TS;
		const float BoxH = static_cast<float>(Text.Y) + 2.f * kOvBoxPadY * TS;
		const float X0 = L.at.x - BoxW * 0.5f, X1 = L.at.x + BoxW * 0.5f, Y1 = L.at.y + L.h * 0.5f, Y0 = Y1 - BoxH;
		AddScreenRect(*Boxes, X0, Y0, X1, Y1, kOvBoxFill);
		const float T = FMath::Max(1.f * S, 1.f);
		const FLinearColor Rule(kOvGold.R, kOvGold.G, kOvGold.B, 0.85f);
		AddScreenRect(*Boxes, X0, Y0, X1, Y0 + T, Rule);
		AddScreenRect(*Boxes, X0, Y1 - T, X1, Y1, Rule);
		AddScreenRect(*Boxes, X0, Y0 + T, X0 + T, Y1 - T, Rule);
		AddScreenRect(*Boxes, X1 - T, Y0 + T, X1, Y1 - T, Rule);
		if (bSign)
		{
			const float G = kOvSignPx * TS;
			const FVector2D Origin(L.at.x - SignWidth[L.name] * G * 0.5f, L.at.y - L.h * 0.5f);
			for (int32 K = 0; K < SignCount[L.name]; ++K)
			{
				const ftcore::SignStroke& Sk = SignStrokes[SignFirst[L.name] + K];
				AddSegment(*Boxes, Origin + FVector2D(Sk.x0, Sk.y0) * G, Origin + FVector2D(Sk.x1, Sk.y1) * G, FMath::Max(1.1f * S, 1.f), Rule, true);
			}
		}
	}
	if (Cap.shown)
	{
		// The where-am-I line's plate: its text and rule with the pad round them, however long the names.
		const ftcore::MapLabel& P = Cap.plate;
		AddScreenRect(*Boxes, P.at.x - P.w * 0.5f, P.at.y - P.h * 0.5f, P.at.x + P.w * 0.5f, P.at.y + P.h * 0.5f, kOvCaptionPlate);
	}
	if (Boxes->TriangleList.Num() > 0) // the item asserts on an empty list
	{
		Canvas.DrawItem(*Boxes);
	}
	for (size_t I = 0; I < Placed.count; ++I)
	{
		DrawLabel(Canvas, Placed.labels[I], TS);
	}

	// The marks: the objective, the party, the player, the cursor, the north needle, the where-am-I rule.
	Marks->TriangleList.Reset();
	auto Disc = [&](const FVector2D& C, float R, const FLinearColor& Color) {
		constexpr int32 N = 14;
		for (int32 I = 0; I < N; ++I)
		{
			const float A0 = 2.f * PI * I / N, A1 = 2.f * PI * (I + 1) / N;
			AddTri(*Marks, C, C + FVector2D(FMath::Cos(A0), FMath::Sin(A0)) * R, C + FVector2D(FMath::Cos(A1), FMath::Sin(A1)) * R, Color);
		}
	};
	auto Screen = [&](const FVector& Cm) { return OvV2(ftcore::mapOverlayToScreen(View, OvMetres(Cm))); };
	auto Dir = [](double Yaw) { return FVector2D(FMath::Cos(FMath::DegreesToRadians(Yaw)), FMath::Sin(FMath::DegreesToRadians(Yaw))); };
	if (Model.hasLandmark)
	{
		// The objective: a hollow gold diamond on a dark rim.
		const FVector2D L = OvV2(ftcore::mapOverlayToScreen(View, Model.landmark));
		const float R = 10.f * S;
		const FVector2D P[4] = {L + FVector2D(0, -R), L + FVector2D(R, 0), L + FVector2D(0, R), L + FVector2D(-R, 0)};
		for (int32 I = 0; I < 4; ++I)
		{
			AddSegment(*Marks, P[I], P[(I + 1) % 4], 4.5f * S, kOvHalo, true);
		}
		for (int32 I = 0; I < 4; ++I)
		{
			AddSegment(*Marks, P[I], P[(I + 1) % 4], 2.f * S, kOvLit, true);
		}
		Disc(L, 2.f * S, kOvLit);
	}
	for (const TWeakObjectPtr<AFTPartyFollower>& F : Followers)
	{
		if (const AFTPartyFollower* Follower = F.Get())
		{
			const FVector2D At = Screen(Follower->GetActorLocation());
			Disc(At, 6.f * S, kOvHalo);
			Disc(At, 4.f * S, kOvGold);
		}
	}
	if (TheVan && !bInVan)
	{
		const FVector2D At = Screen(TheVan->GetActorLocation());
		AddScreenRect(*Marks, At.X - 6.f * S, At.Y - 6.f * S, At.X + 6.f * S, At.Y + 6.f * S, kOvHalo);
		AddScreenRect(*Marks, At.X - 4.f * S, At.Y - 4.f * S, At.X + 4.f * S, At.Y + 4.f * S, kOvLabelDim);
	}
	if (Who)
	{
		const FVector2D At = Screen(Who->GetActorLocation()), D = Dir(Who->GetActorRotation().Yaw), N(-D.Y, D.X);
		const float Sz = kOvArrowMarkPx * S; // its tip; the back corners are nearer
		AddTri(*Marks, At + D * Sz, At - D * Sz * 0.6f + N * Sz * 0.65f, At - D * Sz * 0.25f, kOvHalo);
		AddTri(*Marks, At + D * Sz, At - D * Sz * 0.25f, At - D * Sz * 0.6f - N * Sz * 0.65f, kOvHalo);
		AddTri(*Marks, At + D * Sz * 0.8f, At - D * Sz * 0.45f + N * Sz * 0.5f, At - D * Sz * 0.15f, kOvLit);
		AddTri(*Marks, At + D * Sz * 0.8f, At - D * Sz * 0.15f, At - D * Sz * 0.45f - N * Sz * 0.5f, kOvLit);
	}
	// The cursor: a thin ring with four ticks out to kOvCursorMarkPx.
	{
		const float R = 9.f * S, T = 1.4f * S;
		const FLinearColor Ink(kOvCursorLabel.R, kOvCursorLabel.G, kOvCursorLabel.B, 0.9f);
		constexpr int32 N = 20;
		for (int32 I = 0; I < N; ++I)
		{
			const float A0 = 2.f * PI * I / N, A1 = 2.f * PI * (I + 1) / N;
			const FVector2D U0(FMath::Cos(A0), FMath::Sin(A0)), U1(FMath::Cos(A1), FMath::Sin(A1));
			AddTri(*Marks, Cursor + U0 * (R - T), Cursor + U0 * R, Cursor + U1 * R, Ink);
			AddTri(*Marks, Cursor + U0 * (R - T), Cursor + U1 * R, Cursor + U1 * (R - T), Ink);
		}
		for (const FVector2D U : {FVector2D(1, 0), FVector2D(-1, 0), FVector2D(0, 1), FVector2D(0, -1)})
		{
			AddSegment(*Marks, Cursor + U * (R + 2.f * S), Cursor + U * (kOvCursorMarkPx - 1.f) * S, T, Ink, false);
		}
	}
	// The north needle, top right: a slim diamond, its north half gold.
	const FVector2D Needle(SW - 72.f * S, 96.f * S);
	{
		const float L = 22.f * S, Wd = 5.f * S;
		AddTri(*Marks, Needle + FVector2D(0, -L), Needle + FVector2D(Wd, 0), Needle + FVector2D(-Wd, 0), kOvGold);
		AddTri(*Marks, Needle + FVector2D(0, L), Needle + FVector2D(-Wd, 0), Needle + FVector2D(Wd, 0), kOvNeedleSouth);
	}
	// The where-am-I rule (its plate is under the labels, with the districts' boxes).
	if (Cap.shown)
	{
		AddScreenRect(*Marks, Cap.text.x, Cap.ruleY, Cap.ruleRight, Cap.ruleBottom, kOvGold);
		if (DistrictSize.X > 0.0 && StreetSize.X > 0.0)
		{
			const FVector2D Dot(Cap.dot.x, Cap.dot.y);
			const float R = 3.f * TS * Cap.fit;
			AddTri(*Marks, Dot + FVector2D(0, -R), Dot + FVector2D(R, 0), Dot + FVector2D(0, R), kOvGold);
			AddTri(*Marks, Dot + FVector2D(0, -R), Dot + FVector2D(0, R), Dot + FVector2D(-R, 0), kOvGold);
		}
	}
	Canvas.DrawItem(*Marks); // never empty: the cursor

	// The texts that go over the marks.
	if (NeedleRef.X < 0.f)
	{
		const FVector2D M = Measure(kOvNorth, SerifSc, kOvRefPx);
		NeedleRef = FVector2f(static_cast<float>(M.X), static_cast<float>(M.Y));
	}
	const float NPx = 14.f * TS;
	const FVector2D NSize(NeedleRef.X * NPx / kOvRefPx, NeedleRef.Y * NPx / kOvRefPx);
	DrawText(Canvas, kOvNorth, SerifSc, NPx, FVector2D(Needle.X - NSize.X * 0.5, Needle.Y - 22.f * S - 4.f * S - NSize.Y), kOvGold);
	// On its opaque plate the line needs no halo.
	if (DistrictSize.X > 0.0)
	{
		DrawText(Canvas, NameText[HereDistrict], SerifSc, HerePx * Cap.fit, FVector2D(Cap.text.x, Cap.text.y), kOvDistrictLabel);
	}
	if (StreetSize.X > 0.0)
	{
		DrawText(Canvas, NameText[HereStreet], SerifSc, HerePx * Cap.fit, FVector2D(Cap.streetX, Cap.text.y), kOvStreetHere);
	}
	LastTextDraws = TextDraws;
}
