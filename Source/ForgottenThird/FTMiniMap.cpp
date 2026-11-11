#include "FTMiniMap.h"

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
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TextureResource.h"
#include "UnrealClient.h"

#include "FTExploreCharacter.h"
#include "FTExploreDirector.h"
#include "FTMiniMapSource.h"
#include "FTPartyFollower.h"
#include "FTPartyVan.h"
#include "ForgottenThird.h"

#include <cmath>
#include <string>
#include <utility>

namespace FTMiniMapDetail
{
constexpr int32 kDiscSegments = 72;
constexpr int32 kMipLevels = 7;   // 0.85 m/px down to about 55 m/px
constexpr float kMapPxPerM = 1.f; // asked for; the power-of-two fit makes it about 0.85 m/px on the strip
const FLinearColor kRim(0.02f, 0.025f, 0.03f, 0.85f);
const FLinearColor kPlayer(1.f, 1.f, 1.f, 1.f);
const FLinearColor kLead(0.35f, 0.85f, 0.8f, 1.f);
const FLinearColor kVanMark(1.f, 0.55f, 0.15f, 1.f);
const FLinearColor kDoor(0.95f, 0.8f, 0.45f, 0.9f);
const FLinearColor kLandmark(1.f, 0.82f, 0.25f, 1.f);
const FLinearColor kText(0.9f, 0.9f, 0.88f, 1.f);
const FLinearColor kDim(0.f, 0.f, 0.f, 0.7f);
const FString kNorth(TEXT("N"));
const FString kOverlayTitle(TEXT("MAP"));
const FString kOverlayHint(TEXT("M to close"));

// A transient BGRA8 texture of the image with a box-filtered mip chain.
UTexture2D* MakeMapTexture(UObject* Outer, const ftcore::MapImage& Img)
{
	const int32 W = static_cast<int32>(Img.w);
	const int32 H = static_cast<int32>(Img.h);
	UTexture2D* Tex = UTexture2D::CreateTransient(W, H, PF_B8G8R8A8, MakeUniqueObjectName(Outer, UTexture2D::StaticClass(), TEXT("T_MiniMap")));
	if (!Tex)
	{
		return nullptr;
	}
	Tex->SRGB = true;
	Tex->Filter = TF_Trilinear;
	Tex->AddressX = TA_Clamp;
	Tex->AddressY = TA_Clamp;
	Tex->NeverStream = true;
	FTexturePlatformData* Data = Tex->GetPlatformData();
	{
		void* Dst = Data->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Dst, Img.px.data(), Img.px.size() * sizeof(uint32));
		Data->Mips[0].BulkData.Unlock();
	}
	TArray<uint32> Prev(Img.px.data(), static_cast<int32>(Img.px.size()));
	TArray<uint32> Next;
	int32 PW = W, PH = H;
	for (int32 Level = 1; Level < kMipLevels && (PW > 1 || PH > 1); ++Level)
	{
		const int32 NW = FMath::Max(1, PW / 2), NH = FMath::Max(1, PH / 2);
		Next.SetNumUninitialized(NW * NH);
		for (int32 Y = 0; Y < NH; ++Y)
		{
			for (int32 X = 0; X < NW; ++X)
			{
				uint32 Sum[4] = {};
				for (int32 K = 0; K < 4; ++K)
				{
					const int32 SX = FMath::Min(PW - 1, 2 * X + (K & 1)), SY = FMath::Min(PH - 1, 2 * Y + (K >> 1));
					const uint32 P = Prev[SY * PW + SX];
					for (int32 C = 0; C < 4; ++C)
					{
						Sum[C] += (P >> (8 * C)) & 0xFFu;
					}
				}
				Next[Y * NW + X] = ((Sum[0] + 2) / 4) | (((Sum[1] + 2) / 4) << 8) | (((Sum[2] + 2) / 4) << 16) | (((Sum[3] + 2) / 4) << 24);
			}
		}
		FTexture2DMipMap* Mip = new FTexture2DMipMap(NW, NH, 1);
		Data->Mips.Add(Mip);
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		void* Dst = Mip->BulkData.Realloc(static_cast<int64>(NW) * NH * 4);
		FMemory::Memcpy(Dst, Next.GetData(), static_cast<SIZE_T>(NW) * NH * 4);
		Mip->BulkData.Unlock();
		Swap(Prev, Next);
		PW = NW;
		PH = NH;
	}
	Tex->UpdateResource();
	return Tex;
}

UTexture2D* MakeWhiteTexture(UObject* Outer)
{
	UTexture2D* Tex = UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8, MakeUniqueObjectName(Outer, UTexture2D::StaticClass(), TEXT("T_MiniMapWhite")));
	if (!Tex)
	{
		return nullptr;
	}
	FTexturePlatformData* Data = Tex->GetPlatformData();
	void* Dst = Data->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memset(Dst, 0xFF, 4 * 4 * 4);
	Data->Mips[0].BulkData.Unlock();
	Tex->UpdateResource();
	return Tex;
}

FVector2D V2(const ftcore::Vec2 V) { return FVector2D(V.x, V.y); }
ftcore::Vec2 Metres(const FVector& Cm) { return {static_cast<float>(Cm.X / 100.0), static_cast<float>(Cm.Y / 100.0)}; }

FCanvasUVTri Tri(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FLinearColor& Color)
{
	FCanvasUVTri T;
	T.V0_Pos = A;
	T.V1_Pos = B;
	T.V2_Pos = C;
	T.V0_UV = T.V1_UV = T.V2_UV = FVector2D(0.5, 0.5);
	T.V0_Color = T.V1_Color = T.V2_Color = Color;
	return T;
}
} // namespace FTMiniMapDetail

UFTMiniMap::UFTMiniMap() = default;

UFTMiniMap::~UFTMiniMap()
{
	delete DiscItem;
	delete RimItem;
	delete MarkItem;
}

bool UFTMiniMap::Init(const AFTExploreDirector& Director)
{
	using namespace FTMiniMapDetail;
	const double Start = FPlatformTime::Seconds();
	PlanSide = Director.PlanSide;
	bool bOk = false;
	const TCHAR* From = TEXT("blob");
	FTMiniMapSource::FOptions Options;
	Options.Side = PlanSide;
	if (FTMiniMapSource::ParseOptions(FCommandLine::Get(), TEXT("MiniMap"), Options))
	{
		From = TEXT("tables");
		FString Error, Skipped;
		bOk = FTMiniMapSource::Build(Options, Model, Error, &Skipped);
		if (!bOk)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("MiniMap: the tables %s: %s"), *FTMiniMapSource::Describe(Options), *Error);
		}
		else if (!Skipped.IsEmpty())
		{
			UE_LOG(LogForgottenThird, Display, TEXT("MiniMap: kinds left off the map: %s"), *Skipped);
		}
		PlanSide = Options.Side;
	}
	else if (Director.MiniMapBlob.Num() > 0)
	{
		std::string Why;
		bOk = ftcore::loadMiniMap(Director.MiniMapBlob.GetData(), static_cast<size_t>(Director.MiniMapBlob.Num()), Model, &Why);
		if (!bOk)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("MiniMap: the level's blob: %s"), UTF8_TO_TCHAR(Why.c_str()));
		}
	}
	else
	{
		UE_LOG(LogForgottenThird, Display, TEXT("MiniMap: this level has none (no baked blob)"));
	}
	if (!bOk)
	{
		return false;
	}
	ftcore::rasterMiniMap(Model, kMapPxPerM, Image, 4096, true);
	MapTexture = MakeMapTexture(this, Image);
	WhiteTexture = MakeWhiteTexture(this);
	std::vector<uint32_t>().swap(Image.px);
	if (!MapTexture || !WhiteTexture)
	{
		MapTexture = nullptr;
		return false;
	}
	// The item asserts on an empty list; each draw resets it.
	const TArray<FCanvasUVTri> None{Tri(FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector, FLinearColor::Transparent)};
	DiscItem = new FCanvasTriangleItem(None, MapTexture->GetResource());
	RimItem = new FCanvasTriangleItem(None, WhiteTexture->GetResource());
	MarkItem = new FCanvasTriangleItem(None, WhiteTexture->GetResource());
	DiscItem->BlendMode = SE_BLEND_Translucent;
	RimItem->BlendMode = SE_BLEND_Translucent;
	MarkItem->BlendMode = SE_BLEND_Translucent;
	DiscItem->TriangleList.Reserve(kDiscSegments);
	RimItem->TriangleList.Reserve(kDiscSegments);
	MarkItem->TriangleList.Reserve(512 + static_cast<int32>(Model.doors.size()) * 2);

	// Captures.
	if (FParse::Value(FCommandLine::Get(), TEXT("MiniMapShots="), ShotPrefix))
	{
		FString List;
		if (FParse::Value(FCommandLine::Get(), TEXT("MiniMapAt="), List, false))
		{
			TArray<FString> Points;
			List.ParseIntoArray(Points, TEXT(";"));
			for (const FString& P : Points)
			{
				TArray<FString> XY;
				P.ParseIntoArray(XY, TEXT(":"));
				if (XY.Num() >= 2)
				{
					ShotAt.Add(FVector2D(FCString::Atod(*XY[0]), FCString::Atod(*XY[1])));
				}
			}
		}
		if (FParse::Value(FCommandLine::Get(), TEXT("MiniMapShotTimes="), List, false))
		{
			TArray<FString> Times;
			List.ParseIntoArray(Times, TEXT(","));
			for (const FString& T : Times)
			{
				ShotTimes.Add(FCString::Atod(*T));
			}
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("MINIMAP READY from=%s roads=%d buildings=%d doors=%d water=%dx%d landmark=%d image=%ux%u m/px=%.3f ms=%.0f"),
		From, static_cast<int32>(Model.roads.size()), static_cast<int32>(Model.buildings.size()), static_cast<int32>(Model.doors.size()),
		static_cast<int32>(Model.waterW), static_cast<int32>(Model.waterH), Model.hasLandmark ? 1 : 0, Image.w, Image.h, Image.mPerPx,
		(FPlatformTime::Seconds() - Start) * 1000.0);
	return true;
}

void UFTMiniMap::FindActors(UWorld& World)
{
	if (!Van.IsValid())
	{
		for (TActorIterator<AFTPartyVan> It(&World); It; ++It)
		{
			Van = *It;
			break;
		}
	}
	if (!Followers[0].IsValid() || !Followers[1].IsValid())
	{
		int32 N = 0;
		for (TActorIterator<AFTPartyFollower> It(&World); It && N < 2; ++It)
		{
			Followers[N++] = *It;
		}
	}
}

void UFTMiniMap::AddTri(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FLinearColor& Color)
{
	MarkItem->TriangleList.Add(FTMiniMapDetail::Tri(A, B, C, Color));
}

void UFTMiniMap::AddQuad(const FVector2D& At, float Half, const FLinearColor& Color, float TurnRad)
{
	const FVector2D U(FMath::Cos(TurnRad) * Half, FMath::Sin(TurnRad) * Half);
	const FVector2D V(-U.Y, U.X);
	AddTri(At - U - V, At + U - V, At + U + V, Color);
	AddTri(At - U - V, At + U + V, At - U + V, Color);
}

void UFTMiniMap::AddArrow(const FVector2D& At, const FVector2D& Dir, float Size, const FLinearColor& Color)
{
	const FVector2D Side(-Dir.Y, Dir.X);
	const FVector2D Tip = At + Dir * Size;
	const FVector2D Notch = At - Dir * (Size * 0.3f);
	const FVector2D Left = At - Dir * (Size * 0.7f) + Side * (Size * 0.7f);
	const FVector2D Right = At - Dir * (Size * 0.7f) - Side * (Size * 0.7f);
	AddTri(Tip, Left, Notch, Color);
	AddTri(Tip, Notch, Right, Color);
}

void UFTMiniMap::AddDisc(TArray<FCanvasUVTri>& Tris, const FVector2D& Centre, float Radius, const FLinearColor& Color)
{
	const int32 N = Radius > 20.f ? FTMiniMapDetail::kDiscSegments : 12;
	for (int32 I = 0; I < N; ++I)
	{
		const float A0 = 2.f * PI * I / N, A1 = 2.f * PI * (I + 1) / N;
		Tris.Add(FTMiniMapDetail::Tri(Centre, Centre + FVector2D(FMath::Cos(A0), FMath::Sin(A0)) * Radius,
			Centre + FVector2D(FMath::Cos(A1), FMath::Sin(A1)) * Radius, Color));
	}
}

void UFTMiniMap::Draw(AHUD& Hud, UCanvas& Canvas, float S)
{
	UWorld* World = Hud.GetWorld();
	APlayerController* PC = Hud.GetOwningPlayerController();
	if (!IsReady() || !World || !PC)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	if (Now >= NextScan && (!Van.IsValid() || !Followers[0].IsValid() || !Followers[1].IsValid()))
	{
		FindActors(*World);
		NextScan = Now + 2.0;
	}
	if (!ShotPrefix.IsEmpty())
	{
		TickShots(*PC, Now);
	}
	if (bOverlay)
	{
		DrawOverlay(Hud, Canvas, S);
		return;
	}
	const float Radius = 125.f * S;
	DrawCorner(Hud, Canvas, S, FVector2D(Canvas.ClipX - Radius - 28.f * S, Radius + 28.f * S), Radius);
}

void UFTMiniMap::DrawCorner(AHUD& Hud, UCanvas& Canvas, float S, const FVector2D& Centre, float Radius)
{
	using namespace FTMiniMapDetail;
	APlayerController* PC = Hud.GetOwningPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn && !bHaveView)
	{
		return;
	}
	// With no pawn (a respawn or a possession change) the map stays where it last was, without the player's arrow.
	const AFTExploreCharacter* Lead = Cast<AFTExploreCharacter>(Pawn);
	AFTPartyVan* TheVan = Van.Get();
	const bool bInVan = Pawn ? TheVan && TheVan->IsAboard(Lead) : bLastInVan;
	const ftcore::MapMode Mode = bInVan ? ftcore::MapMode::InVan : ftcore::MapMode::OnFoot;
	ftcore::MapView View = LastView;
	float Facing = 0.f;
	if (Pawn)
	{
		const AActor* Who = bInVan ? static_cast<const AActor*>(TheVan) : Pawn;
		View.centre = Metres(Who->GetActorLocation());
		View.radius = ftcore::mapZoomRadius(Mode, Model);
		View.yawDeg = static_cast<float>(bInVan ? TheVan->GetActorRotation().Yaw : PC->GetControlRotation().Yaw);
		View.rotate = ftcore::mapRotates(Mode);
		Facing = static_cast<float>(Who->GetActorRotation().Yaw);
		LastView = View;
		bLastInVan = bInVan;
		bHaveView = true;
	}

	// The rim, then the map: a fan whose rim UVs sample the turned map, so the circle is exact and the turn free.
	RimItem->TriangleList.Reset();
	AddDisc(RimItem->TriangleList, Centre, Radius + 3.f * S, kRim);
	Canvas.DrawItem(*RimItem);
	DiscItem->TriangleList.Reset();
	const FVector2D CentreUv = V2(ftcore::mapImageUv(Image, View.centre));
	FVector2D PrevPos(Centre), PrevUv(CentreUv);
	for (int32 I = 0; I <= kDiscSegments; ++I)
	{
		const float A = 2.f * PI * I / kDiscSegments;
		const ftcore::Vec2 Dir{FMath::Cos(A), FMath::Sin(A)};
		const FVector2D Pos = Centre + V2(Dir) * Radius;
		const FVector2D Uv = V2(ftcore::mapImageUv(Image, ftcore::mapFromView(View, Dir)));
		if (I > 0)
		{
			FCanvasUVTri T = Tri(Centre, PrevPos, Pos, FLinearColor(1.f, 1.f, 1.f, 0.94f));
			T.V0_UV = CentreUv;
			T.V1_UV = PrevUv;
			T.V2_UV = Uv;
			DiscItem->TriangleList.Add(T);
		}
		PrevPos = Pos;
		PrevUv = Uv;
	}
	Canvas.DrawItem(*DiscItem);

	// Markers.
	MarkItem->TriangleList.Reset();
	auto Screen = [&](const ftcore::Vec2 V) { return Centre + V2(V) * Radius; };
	const ftcore::MapLayers Layers = ftcore::mapLayers(Mode);
	if (Layers.doors)
	{
		for (const ftcore::Vec2& Door : Model.doors)
		{
			const ftcore::Vec2 V = ftcore::mapToView(View, Door);
			if (V.x * V.x + V.y * V.y < 0.9f * 0.9f)
			{
				AddQuad(Screen(V), 2.2f * S, kDoor);
			}
		}
	}
	if (Model.hasLandmark)
	{
		AddQuad(Screen(ftcore::mapPin(View, Model.landmark, 0.9f).at), 5.5f * S, kLandmark, PI * 0.25f);
	}
	for (const TWeakObjectPtr<AFTPartyFollower>& F : Followers)
	{
		if (const AFTPartyFollower* Follower = F.Get())
		{
			AddDisc(MarkItem->TriangleList, Screen(ftcore::mapPin(View, Metres(Follower->GetActorLocation()), 0.9f).at), 4.5f * S, kLead);
		}
	}
	ftcore::MapPin VanPin;
	if (TheVan && !bInVan)
	{
		VanPin = ftcore::mapPin(View, Metres(TheVan->GetActorLocation()), 0.86f);
		const ftcore::Vec2 D = ftcore::mapHeadingDir(View, static_cast<float>(TheVan->GetActorRotation().Yaw));
		AddQuad(Screen(VanPin.at), 5.f * S, kVanMark, FMath::Atan2(D.y, D.x));
	}
	if (Pawn)
	{
		AddArrow(Centre, V2(ftcore::mapHeadingDir(View, Facing)), 9.f * S, bInVan ? kVanMark : kPlayer);
	}
	if (MarkItem->TriangleList.Num() > 0) // the item asserts on an empty list
	{
		Canvas.DrawItem(*MarkItem);
	}

	UFont* Small = GEngine->GetSmallFont();
	float W = 0.f, H = 0.f;
	Hud.GetTextSize(kNorth, W, H, Small, S);
	const FVector2D North = Centre + V2(ftcore::mapNorth(View)) * (Radius - 10.f * S);
	Hud.DrawText(kNorth, kLandmark, North.X - W * 0.5f, North.Y - H * 0.5f, Small, S);
	if (VanPin.pinned)
	{
		const int32 Metres10 = FMath::RoundToInt(VanPin.distance / 10.f) * 10;
		if (Metres10 != VanMetresShown)
		{
			VanMetresShown = Metres10;
			VanMetres = FString::Printf(TEXT("%d m"), Metres10);
		}
		Hud.GetTextSize(VanMetres, W, H, Small, S * 0.9f);
		const FVector2D P = Screen({VanPin.at.x * 0.78f, VanPin.at.y * 0.78f});
		Hud.DrawText(VanMetres, kVanMark, P.X - W * 0.5f, P.Y - H * 0.5f, Small, S * 0.9f);
	}
	// The street under the player: its id and class (no names on the map).
	Road = ftcore::mapRoadAtSticky(Model, View.centre, Road);
	if (Road != RoadShown)
	{
		RoadShown = Road;
		RoadLine = Road < 0 ? FString()
							: FString::Printf(TEXT("%s  -  %s"), UTF8_TO_TCHAR(Model.roads[Road].id.c_str()),
								  UTF8_TO_TCHAR(ftcore::mapRoadClassName(Model.roads[Road].cls)));
	}
	if (!RoadLine.IsEmpty())
	{
		Hud.GetTextSize(RoadLine, W, H, Small, S);
		Hud.DrawRect(kRim, Centre.X - W * 0.5f - 8.f * S, Centre.Y + Radius + 8.f * S, W + 16.f * S, H + 6.f * S);
		Hud.DrawText(RoadLine, kText, Centre.X - W * 0.5f, Centre.Y + Radius + 11.f * S, Small, S);
	}
}

void UFTMiniMap::DrawOverlay(AHUD& Hud, UCanvas& Canvas, float S)
{
	using namespace FTMiniMapDetail;
	APlayerController* PC = Hud.GetOwningPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	const float SpanX = Model.maxX - Model.minX, SpanY = Model.maxY - Model.minY;
	const float Pad = 40.f * S;
	float H = Canvas.ClipY - 2.f * Pad;
	float W = H * SpanX / SpanY;
	if (W > Canvas.ClipX - 2.f * Pad)
	{
		W = Canvas.ClipX - 2.f * Pad;
		H = W * SpanY / SpanX;
	}
	const FVector2D Origin((Canvas.ClipX - W) * 0.5f, (Canvas.ClipY - H) * 0.5f);
	Hud.DrawRect(kDim, 0.f, 0.f, Canvas.ClipX, Canvas.ClipY);
	Hud.DrawRect(kRim, Origin.X - 4.f * S, Origin.Y - 4.f * S, W + 8.f * S, H + 8.f * S);
	FCanvasTileItem Tile(Origin, MapTexture->GetResource(), FVector2D(W, H), FVector2D(0.0, 0.0),
		FVector2D(SpanX / (Image.w * Image.mPerPx), SpanY / (Image.h * Image.mPerPx)), FLinearColor::White);
	Tile.BlendMode = SE_BLEND_Opaque;
	Canvas.DrawItem(Tile);

	// Markers off the map's area stay on its edge, so the player and the van are always on the overlay.
	auto Screen = [&](const ftcore::Vec2 P) {
		const ftcore::Vec2 Uv = ftcore::mapAreaUv(Model, P);
		return Origin + FVector2D(Uv.x * W, Uv.y * H);
	};
	auto Heading = [](double Yaw) { return FVector2D(FMath::Cos(FMath::DegreesToRadians(Yaw)), FMath::Sin(FMath::DegreesToRadians(Yaw))); };
	MarkItem->TriangleList.Reset();
	if (Model.hasLandmark)
	{
		AddQuad(Screen(Model.landmark), 6.f * S, kLandmark, PI * 0.25f);
	}
	for (const TWeakObjectPtr<AFTPartyFollower>& F : Followers)
	{
		if (const AFTPartyFollower* Follower = F.Get())
		{
			AddDisc(MarkItem->TriangleList, Screen(Metres(Follower->GetActorLocation())), 4.5f * S, kLead);
		}
	}
	const AFTPartyVan* TheVan = Van.Get();
	const bool bInVan = TheVan && TheVan->IsAboard(Cast<AFTExploreCharacter>(Pawn));
	if (TheVan && !bInVan)
	{
		const FVector2D D = Heading(TheVan->GetActorRotation().Yaw);
		AddQuad(Screen(Metres(TheVan->GetActorLocation())), 5.f * S, kVanMark, FMath::Atan2(D.Y, D.X));
	}
	if (Pawn)
	{
		const AActor* Who = bInVan ? static_cast<const AActor*>(TheVan) : Pawn;
		AddArrow(Screen(Metres(Who->GetActorLocation())), Heading(Who->GetActorRotation().Yaw), 10.f * S, bInVan ? kVanMark : kPlayer);
	}
	if (MarkItem->TriangleList.Num() > 0) // the item asserts on an empty list
	{
		Canvas.DrawItem(*MarkItem);
	}
	UFont* Small = GEngine->GetSmallFont();
	Hud.DrawText(kOverlayTitle, kLandmark, Origin.X + 10.f * S, Origin.Y + 8.f * S, GEngine->GetMediumFont(), S);
	Hud.DrawText(kOverlayHint, kText, Origin.X + 10.f * S, Origin.Y + 34.f * S, Small, S);
	float TW = 0.f, TH = 0.f;
	Hud.GetTextSize(kNorth, TW, TH, Small, S);
	Hud.DrawText(kNorth, kLandmark, Origin.X + W * 0.5f - TW * 0.5f, Origin.Y + 6.f * S, Small, S);
}

void UFTMiniMap::TickShots(APlayerController& PC, double Now)
{
	auto Shot = [&](const FString& Name) {
		const FString File = ShotPrefix + TEXT("_") + Name + TEXT(".png");
		FScreenshotRequest::RequestScreenshot(File, true, false);
		UE_LOG(LogForgottenThird, Display, TEXT("MINIMAPSHOT %s road=%s overlay=%d file=%s"), *Name, *RoadLine, bOverlay ? 1 : 0, *File);
	};
	if (ShotAt.Num() == 0)
	{
		if (ShotStage < ShotTimes.Num() && Now >= ShotTimes[ShotStage])
		{
			Shot(FString::Printf(TEXT("t%d"), FMath::RoundToInt(ShotTimes[ShotStage])));
			++ShotStage;
		}
		return;
	}
	if (StageStart < 0.0)
	{
		StageStart = Now + 4.0; // let the level settle
	}
	if (Now < StageStart)
	{
		return;
	}
	const int32 N = ShotAt.Num();
	if (ShotStage < 2 * N)
	{
		const int32 I = ShotStage / 2;
		if (ShotStage % 2 == 0)
		{
			if (APawn* Pawn = PC.GetPawn())
			{
				const double Half = PlanSide * 0.5;
				const FVector Top((ShotAt[I].X - Half) * 100.0, (Half - ShotAt[I].Y) * 100.0, 60000.0);
				FHitResult Hit;
				FCollisionQueryParams Params(TEXT("MiniMapShot"), false, Pawn);
				const FVector Ground = Pawn->GetWorld()->LineTraceSingleByChannel(Hit, Top, Top - FVector(0.0, 0.0, 90000.0), ECC_Visibility, Params)
										   ? Hit.ImpactPoint
										   : FVector(Top.X, Top.Y, Pawn->GetActorLocation().Z);
				Pawn->SetActorLocation(Ground + FVector(0.0, 0.0, 120.0), false, nullptr, ETeleportType::TeleportPhysics);
			}
			StageStart = Now + 3.0;
		}
		else
		{
			Shot(FString::Printf(TEXT("at%d"), I));
			StageStart = Now + 0.6;
		}
		++ShotStage;
	}
	else if (ShotStage == 2 * N)
	{
		bOverlay = true;
		StageStart = Now + 1.5;
		++ShotStage;
	}
	else if (ShotStage == 2 * N + 1)
	{
		Shot(TEXT("overlay"));
		StageStart = Now + 2.0;
		++ShotStage;
	}
	else if (ShotStage == 2 * N + 2)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("MINIMAPSHOTS DONE"));
		++ShotStage;
		PC.ConsoleCommand(TEXT("quit"));
	}
}
