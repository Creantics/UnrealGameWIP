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
const FLinearColor kOvHills(0.0025f, 0.0038f, 0.0068f, 1.f);  // past the map's area, inland
const FLinearColor kOvLand(0.005f, 0.008f, 0.015f, 1.f);      // the ground: yards, parks, open land
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
const FLinearColor kOvLabel(0.80f, 0.62f, 0.34f, 1.f);
const FLinearColor kOvLabelDim(0.52f, 0.42f, 0.26f, 1.f);
const FLinearColor kOvCursorLabel(1.f, 0.78f, 0.42f, 1.f);
const FLinearColor kOvWaterLabel(0.36f, 0.45f, 0.56f, 1.f);
const FLinearColor kOvDistrictLabel(0.92f, 0.64f, 0.26f, 1.f);
const FLinearColor kOvStreetHere(0.82f, 0.76f, 0.62f, 1.f);
const FLinearColor kOvBoxFill(0.004f, 0.006f, 0.012f, 0.88f);
const FLinearColor kOvHalo(0.001f, 0.0015f, 0.003f, 0.9f);
const FLinearColor kOvNeedleSouth(0.12f, 0.1f, 0.08f, 1.f);
const TCHAR* kOvShotNames[ftcore::kMapZooms] = {TEXT("far"), TEXT("mid"), TEXT("near")};
const TCHAR* kOvNorth = TEXT("N");
constexpr float kOvRefPx = 64.f;          // names are measured once at this size
constexpr float kOvCursorReachPx = 14.f;  // the cursor picks a road this near (px)
constexpr float kOvEdgePx = 70.f;         // the view pans when the cursor is this near an edge
constexpr float kOvSignPx = 7.f;          // a sign form's glyph height (px at 1080p)
constexpr int32 kOvSignStrokesMax = 192;  // per district name
constexpr float kOvShear = 0.2f;          // water names lean this much (x per y)

ftcore::Vec2 OvMetres(const FVector& Cm) { return {static_cast<float>(Cm.X / 100.0), static_cast<float>(Cm.Y / 100.0)}; }
FVector2D OvV2(const ftcore::Vec2 V) { return FVector2D(V.x, V.y); }

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

FFTMapOverlay::FFTMapOverlay(const ftcore::MiniMapModel& InModel, UTexture2D& White) : Model(InModel)
{
	using namespace FTMapOverlayDetail;
	ftcore::mapEnterableBuildings(Model, Enterable);
	ftcore::mapWaterRuns(Model, Water);
	SeaSide = ftcore::mapSeaSide(Model);
	RoadBox.resize(Model.roads.size() * 4);
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
		if (ftcore::mapLoadNames(Model, std::string_view(Utf8.Get(), Utf8.Length()), Names, &Why))
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
	// Per building: its roof (2), up to 12 hatch lines and two edges (2 each); per road segment: glow, gold, fill, lit.
	Geo->TriangleList.Reserve(static_cast<int32>(Model.buildings.size() * 30 + Water.size() * 2 + Segments * 8) + 64);
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
		TEXT("MAPOVERLAY READY option=C look=gold names=%d roads_named=%d/%d districts=%d places=%d unmatched=%d enterable=%d/%d water_rects=%d sea=%d sign_strokes=%d fonts_missing=%d"),
		NameText.Num(), Named, static_cast<int32>(Model.roads.size()), static_cast<int32>(Names.districts.size()),
		static_cast<int32>(Names.places.size()), static_cast<int32>(Names.unmatched), Lit, static_cast<int32>(Enterable.size()),
		static_cast<int32>(Water.size()), SeaSide, static_cast<int32>(SignStrokes.size()), Missing);
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
			SetZoom(static_cast<ftcore::MapZoom>(Z));
			StageStart = Now + 1.0;
		}
		else
		{
			const FString File = ShotPrefix + TEXT("_") + kOvShotNames[Z] + TEXT(".png");
			FScreenshotRequest::RequestScreenshot(File, true, false);
			UE_LOG(LogForgottenThird, Display, TEXT("MAPOVERLAYSHOT %s m/px=%.2f centre=%.1f:%.1f labels=%d file=%s"), kOvShotNames[Z],
				View.mPerPx, View.centre.x, View.centre.y, static_cast<int32>(Placed.count), *File);
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
	Zoom = ftcore::MapZoom::Near;
	View = ftcore::mapOverlayViewIn(Model, At, Zoom, ScreenW, ScreenH);
	View.centre = ftcore::mapOverlayClampCentre(Model, View);
	Cursor = FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, At));
	PlayerRoad = -1;
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
	// The world point under the cursor stays under it (the far zoom then centres the whole area).
	const ftcore::Vec2 Under = ftcore::mapOverlayFromScreen(View, {static_cast<float>(Cursor.X), static_cast<float>(Cursor.Y)});
	Zoom = Z;
	const ftcore::MapOverlayView Next = ftcore::mapOverlayViewIn(Model, View.centre, Z, View.screenW, View.screenH);
	View.mPerPx = Next.mPerPx;
	View.centre = {Under.x - (static_cast<float>(Cursor.X) - View.screenW * 0.5f) * View.mPerPx,
		Under.y - (static_cast<float>(Cursor.Y) - View.screenH * 0.5f) * View.mPerPx};
	View.centre = ftcore::mapOverlayClampCentre(Model, View);
	Cursor = FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, Under));
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
	// At an edge the view pans under the cursor instead (held inside the map's area).
	const float Edge = kOvEdgePx * S;
	const FVector2D Lo(Edge, Edge), Hi(View.screenW - Edge, View.screenH - Edge);
	const FVector2D Over(Cursor.X < Lo.X ? Cursor.X - Lo.X : (Cursor.X > Hi.X ? Cursor.X - Hi.X : 0.0),
		Cursor.Y < Lo.Y ? Cursor.Y - Lo.Y : (Cursor.Y > Hi.Y ? Cursor.Y - Hi.Y : 0.0));
	if (!Over.IsNearlyZero())
	{
		const ftcore::Vec2 Was = View.centre;
		View.centre = {Was.x + static_cast<float>(Over.X) * View.mPerPx, Was.y + static_cast<float>(Over.Y) * View.mPerPx};
		View.centre = ftcore::mapOverlayClampCentre(Model, View);
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
	FVector2D D = Pb - Pa;
	const double Len = D.Size();
	if (Len < 1e-3)
	{
		return;
	}
	D /= Len;
	const FVector2D N(-D.Y * WidthPx * 0.5f, D.X * WidthPx * 0.5f);
	const FVector2D Cap = bCaps ? D * WidthPx * 0.5f : FVector2D::ZeroVector; // square caps close a road's joints
	AddTri(To, Pa - Cap + N, Pb + Cap + N, Pb + Cap - N, Color);
	AddTri(To, Pa - Cap + N, Pb + Cap - N, Pa - Cap - N, Color);
}

void FFTMapOverlay::AddRoadSegment(const ftcore::Vec2& A, const ftcore::Vec2& B, float WidthPx, const FLinearColor& Color, bool bCaps)
{
	AddSegment(*Geo, FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, A)), FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, B)),
		WidthPx, Color, bCaps);
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

void FFTMapOverlay::DrawText(UCanvas& Canvas, FStringView Text, EFace Face, float Px, const FVector2D& TopLeft, const FLinearColor& Color) const
{
	if (!Fonts[Face] || Text.IsEmpty())
	{
		return;
	}
	FCanvasTextStringViewItem Item(TopLeft, Text, FSlateFontInfo(Fonts[Face], FMath::Max(1.f, Px * 0.75f)), Color);
	Item.bOutlined = true; // a dark rim keeps the name clear of the gold lines
	Item.OutlineColor = FTMapOverlayDetail::kOvHalo;
	Item.BlendMode = SE_BLEND_Translucent;
	Canvas.DrawItem(Item);
}

void FFTMapOverlay::BuildLabels(int32 Picked, float S)
{
	using namespace FTMapOverlayDetail;
	Placed.count = 0;
	if (Names.names.empty())
	{
		return;
	}
	const size_t NPicks = ftcore::mapLabelPicks(Model, Names, View, Zoom, Picked, Picks, ftcore::kMaxMapLabels, Seen.data(), 24.f * S);
	size_t Count = 0;
	for (size_t I = 0; I < NPicks; ++I)
	{
		const ftcore::MapLabelPick& P = Picks[I];
		const bool bWater = P.name < Names.nameKind.size() && Names.nameKind[P.name] == ftcore::MapNameKind::Water;
		const FOvStyle St = OvStyle(P.rank, bWater);
		const FVector2D Text = NameSize(P.name, static_cast<EFace>(St.Face), St.Px * S);
		float W = static_cast<float>(Text.X), H = static_cast<float>(Text.Y);
		if (St.bBox)
		{
			const float Sign = SignCount.IsValidIndex(P.name) && SignCount[P.name] > 0 ? SignWidth[P.name] * kOvSignPx * S : 0.f;
			W = FMath::Max(W + 2.f * kOvBoxPadX * S, Sign);
			H += 2.f * kOvBoxPadY * S + (Sign > 0.f ? (kOvSignPx + kOvSignGap) * S : 0.f);
		}
		ftcore::MapLabel L;
		if (P.along)
		{
			if (!ftcore::mapLabelAlong(Model, Names, P.name, View, W, H, L, 24.f * S, 8.f * S))
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
		L.name = P.name;
		L.priority = static_cast<std::uint8_t>(P.rank);
		Candidates[Count++] = L;
	}
	ftcore::mapPlaceLabels(Candidates, Count, Placed, 6.f * S);
}

void FFTMapOverlay::DrawLabel(UCanvas& Canvas, const ftcore::MapLabel& L, float S)
{
	using namespace FTMapOverlayDetail;
	if (!NameText.IsValidIndex(L.name))
	{
		return;
	}
	const bool bWater = L.name < Names.nameKind.size() && Names.nameKind[L.name] == ftcore::MapNameKind::Water;
	const FOvStyle St = OvStyle(static_cast<ftcore::MapLabelRank>(L.priority), bWater);
	const EFace Face = static_cast<EFace>(St.Face);
	const float Px = St.Px * S;
	const FVector2D Text = NameSize(L.name, Face, Px);
	FVector2D TopLeft(L.at.x - Text.X * 0.5, L.at.y - Text.Y * 0.5);
	if (St.bBox)
	{
		// The text sits in the box at the label's foot; the sign form is above it.
		TopLeft.Y = L.at.y + L.h * 0.5f - kOvBoxPadY * S - Text.Y;
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
	DrawText(Canvas, NameText[L.name], Face, Px, TopLeft, St.Color);
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
	if (GFrameCounter > LastFrame + 1 || View.screenW != SW || View.screenH != SH)
	{
		OnOpen(Hud, SW, SH);
	}
	LastFrame = GFrameCounter;
	if (PC)
	{
		ReadInput(*PC, S, FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.1f));
	}
	const ftcore::Vec2 CursorWorld = ftcore::mapOverlayFromScreen(View, {static_cast<float>(Cursor.X), static_cast<float>(Cursor.Y)});
	const int32 Picked = ftcore::mapRoadNear(Model, CursorWorld, kOvCursorReachPx * S * View.mPerPx, Zoom);
	const uint16 PickedName = ftcore::mapRoadName(Names, Picked);

	// The view's area in metres, a little grown, for culling.
	const ftcore::Vec2 Lo = ftcore::mapOverlayFromScreen(View, {-20.f, -20.f});
	const ftcore::Vec2 Hi = ftcore::mapOverlayFromScreen(View, {SW + 20.f, SH + 20.f});
	auto InView = [&](float X0, float Y0, float X1, float Y1) { return X1 >= Lo.x && X0 <= Hi.x && Y1 >= Lo.y && Y0 <= Hi.y; };
	auto RectInView = [&](const ftcore::MapRect& R) {
		const float E = FMath::Max(R.hx, R.hy) * 1.415f;
		return InView(R.cx - E, R.cy - E, R.cx + E, R.cy + E);
	};

	Geo->TriangleList.Reset();
	// The letterbox: hills past the map's area, the sea past it on the sea's side, the ground inside it.
	{
		const ftcore::Vec2 A = ftcore::mapOverlayToScreen(View, {Model.minX, Model.minY});
		const ftcore::Vec2 B = ftcore::mapOverlayToScreen(View, {Model.maxX, Model.maxY});
		AddScreenRect(*Geo, 0.f, 0.f, SW, SH, kOvHills);
		if (SeaSide > 0)
		{
			AddScreenRect(*Geo, FMath::Max(B.x, 0.f), 0.f, SW, SH, kOvSea);
		}
		else if (SeaSide < 0)
		{
			AddScreenRect(*Geo, 0.f, 0.f, FMath::Min(A.x, SW), SH, kOvSea);
		}
		AddScreenRect(*Geo, FMath::Max(A.x, 0.f), FMath::Max(A.y, 0.f), FMath::Min(B.x, SW), FMath::Min(B.y, SH), kOvLand);
	}
	for (const ftcore::MapRect& R : Water)
	{
		if (RectInView(R))
		{
			AddRect(R, kOvSea);
		}
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
	// Roads: a soft glow under the main roads and streets, then every road in gold (cased when wide enough: the gold
	// full width first, the dark inside after, so the junctions join), then the cursor's road lit along its whole length.
	auto IsLit = [&](size_t R) {
		return Picked >= 0 && (static_cast<int32>(R) == Picked || (PickedName != ftcore::kMapNoName && Names.roadName[R] == PickedName));
	};
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
				if ((Pass == LitPass && !bLit) || (Pass == Glow && Cls > static_cast<int32>(ftcore::MapRoadClass::Street)))
				{
					continue;
				}
				if ((!bLit && !ftcore::mapRoadShown(Road.cls, Zoom)) || !InView(RoadBox[R * 4], RoadBox[R * 4 + 1], RoadBox[R * 4 + 2], RoadBox[R * 4 + 3]))
				{
					continue;
				}
				const float Px = Road.width / View.mPerPx;
				const bool bCased = Px >= kOvCasePx * S;
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
					Width = bCased ? Px : FMath::Max(kOvRoadLinePx[Cls] * S, 0.8f);
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
					Width = FMath::Max(Px * 0.5f, 2.f * S);
					Color = kOvLit;
					break;
				}
				for (uint32 K = 0; K + 1 < Road.count && Road.first + K + 1 < Model.points.size(); ++K)
				{
					AddRoadSegment(Model.points[Road.first + K], Model.points[Road.first + K + 1], Width, Color, bCaps);
				}
			}
		}
	}
	Canvas.DrawItem(*Geo); // never empty: the letterbox

	// The labels, placed again only when the view, the zoom or the cursor's road changes.
	if (LabelPicked != Picked || LabelZoom != Zoom || LabelView.centre.x != View.centre.x || LabelView.centre.y != View.centre.y ||
		LabelView.mPerPx != View.mPerPx || LabelView.screenW != View.screenW || LabelView.screenH != View.screenH)
	{
		BuildLabels(Picked, S);
		LabelPicked = Picked;
		LabelZoom = Zoom;
		LabelView = View;
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
		const FVector2D Text = NameSize(L.name, SerifSc, 15.f * S);
		const float BoxW = static_cast<float>(Text.X) + 2.f * kOvBoxPadX * S;
		const float BoxH = static_cast<float>(Text.Y) + 2.f * kOvBoxPadY * S;
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
			const float G = kOvSignPx * S;
			const FVector2D Origin(L.at.x - SignWidth[L.name] * G * 0.5f, L.at.y - L.h * 0.5f);
			for (int32 K = 0; K < SignCount[L.name]; ++K)
			{
				const ftcore::SignStroke& Sk = SignStrokes[SignFirst[L.name] + K];
				AddSegment(*Boxes, Origin + FVector2D(Sk.x0, Sk.y0) * G, Origin + FVector2D(Sk.x1, Sk.y1) * G, FMath::Max(1.1f * S, 1.f), Rule, true);
			}
		}
	}
	if (Boxes->TriangleList.Num() > 0) // the item asserts on an empty list
	{
		Canvas.DrawItem(*Boxes);
	}
	for (size_t I = 0; I < Placed.count; ++I)
	{
		DrawLabel(Canvas, Placed.labels[I], S);
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
	const APawn* Pawn = Hud.GetOwningPawn();
	const AFTPartyVan* TheVan = Van.Get();
	const bool bInVan = TheVan && TheVan->IsAboard(Cast<AFTExploreCharacter>(Pawn));
	if (TheVan && !bInVan)
	{
		const FVector2D At = Screen(TheVan->GetActorLocation());
		AddScreenRect(*Marks, At.X - 6.f * S, At.Y - 6.f * S, At.X + 6.f * S, At.Y + 6.f * S, kOvHalo);
		AddScreenRect(*Marks, At.X - 4.f * S, At.Y - 4.f * S, At.X + 4.f * S, At.Y + 4.f * S, kOvLabelDim);
	}
	if (const AActor* Who = bInVan ? static_cast<const AActor*>(TheVan) : Pawn)
	{
		const FVector2D At = Screen(Who->GetActorLocation()), D = Dir(Who->GetActorRotation().Yaw), N(-D.Y, D.X);
		const float Sz = 12.f * S;
		AddTri(*Marks, At + D * Sz, At - D * Sz * 0.6f + N * Sz * 0.65f, At - D * Sz * 0.25f, kOvHalo);
		AddTri(*Marks, At + D * Sz, At - D * Sz * 0.25f, At - D * Sz * 0.6f - N * Sz * 0.65f, kOvHalo);
		AddTri(*Marks, At + D * Sz * 0.8f, At - D * Sz * 0.45f + N * Sz * 0.5f, At - D * Sz * 0.15f, kOvLit);
		AddTri(*Marks, At + D * Sz * 0.8f, At - D * Sz * 0.15f, At - D * Sz * 0.45f - N * Sz * 0.5f, kOvLit);
	}
	// The cursor: a thin ring with four ticks.
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
			AddSegment(*Marks, Cursor + U * (R + 2.f * S), Cursor + U * (R + 7.f * S), T, Ink, false);
		}
	}
	// The north needle, top right: a slim diamond, its north half gold.
	const FVector2D Needle(SW - 72.f * S, 96.f * S);
	{
		const float L = 22.f * S, Wd = 5.f * S;
		AddTri(*Marks, Needle + FVector2D(0, -L), Needle + FVector2D(Wd, 0), Needle + FVector2D(-Wd, 0), kOvGold);
		AddTri(*Marks, Needle + FVector2D(0, L), Needle + FVector2D(-Wd, 0), Needle + FVector2D(Wd, 0), kOvNeedleSouth);
	}
	// Where the player is, bottom left: the district in small caps, a small diamond, the street, over a thin gold rule.
	uint16 HereDistrict = ftcore::kMapNoName, HereStreet = ftcore::kMapNoName;
	if (Pawn)
	{
		PlayerRoad = ftcore::mapRoadAtSticky(Model, OvMetres(Pawn->GetActorLocation()), PlayerRoad);
		const uint16 D = ftcore::mapRoadDistrict(Names, PlayerRoad);
		HereDistrict = D < Names.districts.size() ? Names.districts[D].name : ftcore::kMapNoName;
		HereStreet = ftcore::mapRoadName(Names, PlayerRoad);
	}
	const float HerePx = 18.f * S, HereX = 64.f * S, HereY = SH - 96.f * S;
	const FVector2D DistrictSize = NameText.IsValidIndex(HereDistrict) ? NameSize(HereDistrict, SerifSc, HerePx) : FVector2D::ZeroVector;
	const FVector2D StreetSize = NameText.IsValidIndex(HereStreet) && HereStreet != HereDistrict ? NameSize(HereStreet, SerifSc, HerePx) : FVector2D::ZeroVector;
	const float Gap = 22.f * S;
	const float LineW = static_cast<float>(DistrictSize.X + (StreetSize.X > 0.0 ? Gap + StreetSize.X : 0.0));
	if (LineW > 0.f)
	{
		const float LineH = static_cast<float>(FMath::Max(DistrictSize.Y, StreetSize.Y));
		const float RuleY = HereY + LineH + 5.f * S;
		AddScreenRect(*Marks, HereX, RuleY, HereX + LineW + 28.f * S, RuleY + FMath::Max(1.f * S, 1.f), kOvGold);
		if (DistrictSize.X > 0.0 && StreetSize.X > 0.0)
		{
			const FVector2D Dot(HereX + DistrictSize.X + Gap * 0.5f, HereY + LineH * 0.55f);
			const float R = 3.f * S;
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
	const float NPx = 14.f * S;
	const FVector2D NSize(NeedleRef.X * NPx / kOvRefPx, NeedleRef.Y * NPx / kOvRefPx);
	DrawText(Canvas, kOvNorth, SerifSc, NPx, FVector2D(Needle.X - NSize.X * 0.5, Needle.Y - 22.f * S - 4.f * S - NSize.Y), kOvGold);
	if (DistrictSize.X > 0.0)
	{
		DrawText(Canvas, NameText[HereDistrict], SerifSc, HerePx, FVector2D(HereX, HereY), kOvDistrictLabel);
	}
	if (StreetSize.X > 0.0)
	{
		DrawText(Canvas, NameText[HereStreet], SerifSc, HerePx, FVector2D(HereX + (DistrictSize.X > 0.0 ? DistrictSize.X + Gap : 0.0), HereY),
			kOvStreetHere);
	}
}
