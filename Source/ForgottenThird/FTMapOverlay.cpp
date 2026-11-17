#include "FTMapOverlay.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "TextureResource.h"
#include "UnrealClient.h"

#include "FTExploreCharacter.h"
#include "FTPartyFollower.h"
#include "FTPartyVan.h"
#include "ForgottenThird.h"

#include <algorithm>
#include <string>

namespace FTMapOverlayDetail
{
// The proposal's palette: ink on near black, one warm accent.
const FLinearColor kOvBack(0.004f, 0.003f, 0.002f, 0.9f);
const FLinearColor kOvBuilding(1.f, 0.94f, 0.85f, 0.07f);
const FLinearColor kOvEnterable(1.f, 0.94f, 0.85f, 0.15f);
const FLinearColor kOvWater(0.16f, 0.3f, 0.36f, 0.55f);
const FLinearColor kOvRoad[ftcore::kMapRoadClasses] = {
	{0.9f, 0.86f, 0.8f, 0.7f}, {0.9f, 0.86f, 0.8f, 0.42f}, {0.9f, 0.86f, 0.8f, 0.3f}, {0.9f, 0.86f, 0.8f, 0.24f}};
const float kOvRoadMinPx[ftcore::kMapRoadClasses] = {2.5f, 1.5f, 1.2f, 1.2f};
const FLinearColor kOvAccent(0.81f, 0.45f, 0.15f, 1.f); // (233, 179, 107) in linear
const FLinearColor kOvInk(0.9f, 0.86f, 0.79f, 1.f);
const FLinearColor kOvDimInk(0.64f, 0.6f, 0.52f, 1.f);
const FLinearColor kOvDistrict(0.9f, 0.86f, 0.79f, 0.55f);
const FLinearColor kOvHalo(0.004f, 0.003f, 0.002f, 0.85f);
const FLinearColor kOvPartner(0.35f, 0.85f, 0.8f, 1.f);
const FLinearColor kOvVan(1.f, 0.55f, 0.15f, 1.f);
const FString kOvHint(TEXT("M close    RS / mouse  cursor    LT RT / wheel  zoom"));
const TCHAR* kOvShotNames[ftcore::kMapZooms] = {TEXT("far"), TEXT("mid"), TEXT("near")};
constexpr float kOvCursorReachPx = 14.f; // the cursor picks a road this near (px)
constexpr float kOvEdgePx = 70.f;        // the view pans when the cursor is this near an edge

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
			NameSize.Init(FVector2D(-1.0, -1.0), NameText.Num());
			for (const ftcore::MapNames::District& D : Names.districts)
			{
				DistrictText.Add(D.name < NameText.Num() ? NameText[D.name].ToUpper() : FString());
			}
		}
		else
		{
			Names = ftcore::MapNames{};
			UE_LOG(LogForgottenThird, Error, TEXT("MapOverlay: the names %s: %s"), *Path, UTF8_TO_TCHAR(Why.c_str()));
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
	Marks = new FCanvasTriangleItem(None, White.GetResource());
	Geo->BlendMode = SE_BLEND_Translucent;
	Marks->BlendMode = SE_BLEND_Translucent;
	Geo->TriangleList.Reserve(static_cast<int32>((Model.buildings.size() + Water.size() + Segments * 2) * 2) + 64);
	Marks->TriangleList.Reserve(256);

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
	UE_LOG(LogForgottenThird, Display, TEXT("MAPOVERLAY READY option=C names=%d roads_named=%d/%d districts=%d unmatched=%d enterable=%d/%d water_rects=%d"),
		NameText.Num(), Named, static_cast<int32>(Model.roads.size()), static_cast<int32>(Names.districts.size()),
		static_cast<int32>(Names.unmatched), Lit, static_cast<int32>(Enterable.size()), static_cast<int32>(Water.size()));
}

FFTMapOverlay::~FFTMapOverlay()
{
	delete Geo;
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
			UE_LOG(LogForgottenThird, Display, TEXT("MAPOVERLAYSHOT %s m/px=%.2f centre=%.1f:%.1f file=%s"), kOvShotNames[Z], View.mPerPx,
				View.centre.x, View.centre.y, *File);
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
	Zoom = ftcore::MapZoom::Mid;
	View = ftcore::mapOverlayView(At, Zoom, ScreenW, ScreenH);
	View.centre = ftcore::mapOverlayClampCentre(Model, View);
	Cursor = FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, At));
	PlayerRoad = -1;
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
	// The world point under the cursor stays under it.
	const ftcore::Vec2 Under = ftcore::mapOverlayFromScreen(View, {static_cast<float>(Cursor.X), static_cast<float>(Cursor.Y)});
	Zoom = Z;
	const ftcore::MapOverlayView Next = ftcore::mapOverlayView(View.centre, Z, View.screenW, View.screenH);
	View.mPerPx = Next.mPerPx;
	View.centre = {Under.x - (static_cast<float>(Cursor.X) - View.screenW * 0.5f) * View.mPerPx,
		Under.y - (static_cast<float>(Cursor.Y) - View.screenH * 0.5f) * View.mPerPx};
	View.centre = ftcore::mapOverlayClampCentre(Model, View);
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

void FFTMapOverlay::AddTri(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FLinearColor& Color)
{
	Geo->TriangleList.Add(FTMapOverlayDetail::OvTri(A, B, C, Color));
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
	AddTri(P[0], P[1], P[2], Color);
	AddTri(P[0], P[2], P[3], Color);
}

void FFTMapOverlay::AddSegment(const ftcore::Vec2& A, const ftcore::Vec2& B, float WidthPx, const FLinearColor& Color)
{
	const FVector2D Pa = FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, A));
	const FVector2D Pb = FTMapOverlayDetail::OvV2(ftcore::mapOverlayToScreen(View, B));
	FVector2D D = Pb - Pa;
	const double Len = D.Size();
	if (Len < 1e-3)
	{
		return;
	}
	D /= Len;
	const FVector2D N(-D.Y * WidthPx * 0.5f, D.X * WidthPx * 0.5f);
	const FVector2D Cap = D * WidthPx * 0.5f; // square caps close the joints
	AddTri(Pa - Cap + N, Pb + Cap + N, Pb + Cap - N, Color);
	AddTri(Pa - Cap + N, Pb + Cap - N, Pa - Cap - N, Color);
}

FVector2D FFTMapOverlay::TextSize(AHUD& Hud, int32 Name, float S)
{
	if (!NameSize.IsValidIndex(Name))
	{
		return FVector2D::ZeroVector;
	}
	if (NameSize[Name].X < 0.0)
	{
		float W = 0.f, H = 0.f;
		Hud.GetTextSize(NameText[Name], W, H, GEngine->GetMediumFont(), 1.f);
		NameSize[Name] = FVector2D(W, H);
	}
	return NameSize[Name] * S;
}

void FFTMapOverlay::DrawLabel(AHUD& Hud, UCanvas& Canvas, const ftcore::MapLabel& L, const FString& Text, const FLinearColor& Color, float S)
{
	using namespace FTMapOverlayDetail;
	FCanvas* C = Canvas.Canvas;
	const FVector At(L.at.x, L.at.y, 0.0);
	const bool bTurn = FMath::Abs(L.angleDeg) > 0.5f && C;
	if (bTurn)
	{
		C->PushRelativeTransform(FTranslationMatrix(-At) * FRotationMatrix(FRotator(0.0, L.angleDeg, 0.0)) * FTranslationMatrix(At));
	}
	const float X = L.at.x - L.w * 0.5f, Y = L.at.y - L.h * 0.5f;
	UFont* Font = GEngine->GetMediumFont();
	const float H = 1.5f * S;
	for (const FVector2D Off : {FVector2D(-H, 0.0), FVector2D(H, 0.0), FVector2D(0.0, -H), FVector2D(0.0, H)})
	{
		Hud.DrawText(Text, kOvHalo, X + Off.X, Y + Off.Y, Font, S);
	}
	Hud.DrawText(Text, Color, X, Y, Font, S);
	if (bTurn)
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

	Hud.DrawRect(kOvBack, 0.f, 0.f, SW, SH);
	Geo->TriangleList.Reset();
	for (const ftcore::MapRect& R : Water)
	{
		if (RectInView(R))
		{
			AddRect(R, kOvWater);
		}
	}
	for (size_t B = 0; B < Model.buildings.size(); ++B)
	{
		if (RectInView(Model.buildings[B]))
		{
			AddRect(Model.buildings[B], B < Enterable.size() && Enterable[B] ? kOvEnterable : kOvBuilding);
		}
	}
	// Roads, the lower classes first so main roads lie on top; then the cursor's road lit along its whole length.
	auto IsLit = [&](size_t R) {
		return Picked >= 0 && (static_cast<int32>(R) == Picked || (PickedName != ftcore::kMapNoName && Names.roadName[R] == PickedName));
	};
	for (int32 Pass = ftcore::kMapRoadClasses - 1; Pass >= -1; --Pass)
	{
		for (size_t R = 0; R < Model.roads.size(); ++R)
		{
			const ftcore::MapRoad& Road = Model.roads[R];
			const int32 Cls = static_cast<int32>(Road.cls);
			const bool bLit = IsLit(R);
			if ((Pass >= 0 && (Cls != Pass || bLit)) || (Pass < 0 && !bLit))
			{
				continue;
			}
			if ((!bLit && !ftcore::mapRoadShown(Road.cls, Zoom)) || !InView(RoadBox[R * 4], RoadBox[R * 4 + 1], RoadBox[R * 4 + 2], RoadBox[R * 4 + 3]))
			{
				continue;
			}
			const float Px = FMath::Max(Road.width / View.mPerPx, kOvRoadMinPx[Cls] * S) * (bLit ? 0.6f : 1.f);
			const FLinearColor Color = bLit ? kOvAccent : kOvRoad[Cls];
			for (uint32 K = 0; K + 1 < Road.count && Road.first + K + 1 < Model.points.size(); ++K)
			{
				AddSegment(Model.points[Road.first + K], Model.points[Road.first + K + 1], FMath::Max(Px, 2.f * S), Color);
			}
		}
	}
	if (Geo->TriangleList.Num() > 0) // the item asserts on an empty list
	{
		Canvas.DrawItem(*Geo);
	}

	// The party and the cursor.
	Marks->TriangleList.Reset();
	auto Disc = [&](const FVector2D& C, float R, const FLinearColor& Color) {
		constexpr int32 N = 14;
		for (int32 I = 0; I < N; ++I)
		{
			const float A0 = 2.f * PI * I / N, A1 = 2.f * PI * (I + 1) / N;
			Marks->TriangleList.Add(OvTri(C, C + FVector2D(FMath::Cos(A0), FMath::Sin(A0)) * R, C + FVector2D(FMath::Cos(A1), FMath::Sin(A1)) * R, Color));
		}
	};
	auto Screen = [&](const FVector& Cm) { return OvV2(ftcore::mapOverlayToScreen(View, OvMetres(Cm))); };
	auto Dir = [](double Yaw) { return FVector2D(FMath::Cos(FMath::DegreesToRadians(Yaw)), FMath::Sin(FMath::DegreesToRadians(Yaw))); };
	if (Model.hasLandmark)
	{
		const FVector2D L = OvV2(ftcore::mapOverlayToScreen(View, Model.landmark));
		const float R = 6.f * S;
		Marks->TriangleList.Add(OvTri(L + FVector2D(0, -R), L + FVector2D(R, 0), L + FVector2D(0, R), kOvInk));
		Marks->TriangleList.Add(OvTri(L + FVector2D(0, -R), L + FVector2D(0, R), L + FVector2D(-R, 0), kOvInk));
	}
	for (const TWeakObjectPtr<AFTPartyFollower>& F : Followers)
	{
		if (const AFTPartyFollower* Follower = F.Get())
		{
			Disc(Screen(Follower->GetActorLocation()), 4.5f * S, kOvPartner);
		}
	}
	const APawn* Pawn = Hud.GetOwningPawn();
	const AFTPartyVan* TheVan = Van.Get();
	const bool bInVan = TheVan && TheVan->IsAboard(Cast<AFTExploreCharacter>(Pawn));
	if (TheVan && !bInVan)
	{
		Disc(Screen(TheVan->GetActorLocation()), 5.f * S, kOvVan);
	}
	if (const AActor* Who = bInVan ? static_cast<const AActor*>(TheVan) : Pawn)
	{
		const FVector2D At = Screen(Who->GetActorLocation()), D = Dir(Who->GetActorRotation().Yaw), N(-D.Y, D.X);
		const float Sz = 11.f * S;
		Marks->TriangleList.Add(OvTri(At + D * Sz, At - D * Sz * 0.6f + N * Sz * 0.65f, At - D * Sz * 0.25f, kOvHalo));
		Marks->TriangleList.Add(OvTri(At + D * Sz, At - D * Sz * 0.25f, At - D * Sz * 0.6f - N * Sz * 0.65f, kOvHalo));
		Marks->TriangleList.Add(OvTri(At + D * Sz * 0.8f, At - D * Sz * 0.45f + N * Sz * 0.5f, At - D * Sz * 0.15f, kOvInk));
		Marks->TriangleList.Add(OvTri(At + D * Sz * 0.8f, At - D * Sz * 0.15f, At - D * Sz * 0.45f - N * Sz * 0.5f, kOvInk));
	}
	// The cursor: a ring with four ticks.
	{
		const float R = 9.f * S, T = 1.6f * S;
		constexpr int32 N = 20;
		for (int32 I = 0; I < N; ++I)
		{
			const float A0 = 2.f * PI * I / N, A1 = 2.f * PI * (I + 1) / N;
			const FVector2D U0(FMath::Cos(A0), FMath::Sin(A0)), U1(FMath::Cos(A1), FMath::Sin(A1));
			Marks->TriangleList.Add(OvTri(Cursor + U0 * (R - T), Cursor + U0 * R, Cursor + U1 * R, kOvInk));
			Marks->TriangleList.Add(OvTri(Cursor + U0 * (R - T), Cursor + U1 * R, Cursor + U1 * (R - T), kOvInk));
		}
		for (const FVector2D U : {FVector2D(1, 0), FVector2D(-1, 0), FVector2D(0, 1), FVector2D(0, -1)})
		{
			const FVector2D A = Cursor + U * (R + 2.f * S), B = Cursor + U * (R + 7.f * S), Nn(-U.Y * T * 0.5f, U.X * T * 0.5f);
			Marks->TriangleList.Add(OvTri(A + Nn, B + Nn, B - Nn, kOvInk));
			Marks->TriangleList.Add(OvTri(A + Nn, B - Nn, A - Nn, kOvInk));
		}
	}
	if (Marks->TriangleList.Num() > 0)
	{
		Canvas.DrawItem(*Marks);
	}

	// Labels: the cursor's road first, then the district names (far and mid); never overlapping, always upright.
	size_t Count = 0;
	if (PickedName != ftcore::kMapNoName)
	{
		const FVector2D Size = TextSize(Hud, PickedName, S);
		ftcore::MapLabel L;
		if (ftcore::mapLabelAlong(Model, Names, PickedName, View, static_cast<float>(Size.X), static_cast<float>(Size.Y), L, 24.f * S))
		{
			L.priority = 0;
			Candidates[Count++] = L;
		}
	}
	if (Zoom != ftcore::MapZoom::Near)
	{
		for (size_t D = 0; D < Names.districts.size() && Count < ftcore::kMaxMapLabels; ++D)
		{
			const ftcore::MapNames::District& Dist = Names.districts[D];
			if (!Dist.hasAnchor)
			{
				continue;
			}
			float W = 0.f, H = 0.f;
			Hud.GetTextSize(DistrictText[D], W, H, GEngine->GetMediumFont(), S);
			ftcore::MapLabel L;
			L.at = ftcore::mapOverlayToScreen(View, Dist.anchor);
			L.w = W;
			L.h = H;
			L.name = static_cast<uint16>(D); // a district index here, not a name index
			L.priority = 1;
			if (L.at.x > 0.f && L.at.y > 0.f && L.at.x < SW && L.at.y < SH)
			{
				Candidates[Count++] = L;
			}
		}
	}
	ftcore::mapPlaceLabels(Candidates, Count, Placed, 6.f * S);
	for (size_t I = 0; I < Placed.count; ++I)
	{
		const ftcore::MapLabel& L = Placed.labels[I];
		if (L.priority == 0)
		{
			DrawLabel(Hud, Canvas, L, NameText[L.name], kOvInk, S);
		}
		else if (DistrictText.IsValidIndex(L.name))
		{
			DrawLabel(Hud, Canvas, L, DistrictText[L.name], kOvDistrict, S);
		}
	}

	// Where the player is, bottom left: the district, then the street.
	if (Pawn)
	{
		PlayerRoad = ftcore::mapRoadAtSticky(Model, OvMetres(Pawn->GetActorLocation()), PlayerRoad);
		const uint16 District = ftcore::mapRoadDistrict(Names, PlayerRoad);
		const uint16 Street = ftcore::mapRoadName(Names, PlayerRoad);
		const float X = 64.f * S;
		if (DistrictText.IsValidIndex(District))
		{
			Hud.DrawText(DistrictText[District], kOvDimInk, X, SH - 118.f * S, GEngine->GetSmallFont(), S * 1.3f);
		}
		if (NameText.IsValidIndex(Street))
		{
			Hud.DrawText(NameText[Street], kOvInk, X, SH - 92.f * S, GEngine->GetMediumFont(), S * 1.25f);
		}
	}
	float TW = 0.f, TH = 0.f;
	Hud.GetTextSize(kOvHint, TW, TH, GEngine->GetSmallFont(), S);
	Hud.DrawText(kOvHint, kOvDimInk, SW - 64.f * S - TW, SH - 52.f * S, GEngine->GetSmallFont(), S);
}
