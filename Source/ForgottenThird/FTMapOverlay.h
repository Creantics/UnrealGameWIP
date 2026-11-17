// The map overlay, option C (ftcore/MapOverlay.h), behind -MapOverlayC: north up over the paused play, three zoom levels
// (LT/RT, the wheel), a cursor (the right stick, the mouse, the arrows; the view pans when it reaches an edge). Building
// blocks as faint fills, the enterable ones a shade brighter; water; roads by the zoom's classes; the road under the
// cursor lit along its whole length and named once along it; district names; the lead as the arrow, the partners as dots;
// the player's district and street bottom left. Names come from the world lane's names table (-MapNames=<csv>, else
// Data/Map/solhaven_names.csv; none: no names drawn). Without the switch UFTMiniMap draws its old whole-map overlay.
//
// Captures: -MapOverlayShots=<path prefix> [-MapOverlayShotAt=<s>, default 8] [-MapOverlayCursor=x:y, engine m]: the
// overlay opens at that time and a shot is taken at each zoom (_far, _mid, _near), then the game quits.
#pragma once

#include "CoreMinimal.h"

#include "ftcore/MapOverlay.h"

#include <vector>

class AFTPartyFollower;
class AFTPartyVan;
class AHUD;
class APlayerController;
class FCanvasTriangleItem;
class UCanvas;
class UTexture2D;

class FFTMapOverlay
{
public:
	static bool Wanted(); // -MapOverlayC

	// The model is UFTMiniMap's (it outlives this); White is its plain white texture.
	FFTMapOverlay(const ftcore::MiniMapModel& InModel, UTexture2D& White);
	~FFTMapOverlay();
	FFTMapOverlay(const FFTMapOverlay&) = delete;
	FFTMapOverlay& operator=(const FFTMapOverlay&) = delete;

	// The capture run's clock: may open the overlay (bOverlay) and quits when done.
	void TickShots(APlayerController& PC, double Now, bool& bOverlay);
	void Draw(AHUD& Hud, UCanvas& Canvas, float S);

private:
	void OnOpen(AHUD& Hud, float ScreenW, float ScreenH);
	void ReadInput(APlayerController& PC, float S, float Dt);
	void SetZoom(ftcore::MapZoom Z);
	void AddRect(const ftcore::MapRect& R, const FLinearColor& Color);
	void AddSegment(const ftcore::Vec2& A, const ftcore::Vec2& B, float WidthPx, const FLinearColor& Color);
	void AddTri(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FLinearColor& Color);
	FVector2D TextSize(AHUD& Hud, int32 Name, float S);
	void DrawLabel(AHUD& Hud, UCanvas& Canvas, const ftcore::MapLabel& L, const FString& Text, const FLinearColor& Color, float S);

	const ftcore::MiniMapModel& Model;
	std::vector<std::uint8_t> Enterable;
	std::vector<ftcore::MapRect> Water;
	std::vector<float> RoadBox; // per road: x0, y0, x1, y1 (m)
	ftcore::MapNames Names;
	TArray<FString> NameText;     // per name
	TArray<FVector2D> NameSize;   // per name at scale 1, measured on first use (x < 0: not yet)
	TArray<FString> DistrictText; // per district, upper case

	FCanvasTriangleItem* Geo = nullptr;
	FCanvasTriangleItem* Marks = nullptr;
	ftcore::MapLabel Candidates[ftcore::kMaxMapLabels];
	ftcore::MapLabelSet Placed;

	ftcore::MapZoom Zoom = ftcore::MapZoom::Mid;
	ftcore::MapOverlayView View;
	FVector2D Cursor = FVector2D::ZeroVector; // screen px
	uint64 LastFrame = 0;
	int32 PlayerRoad = -1;
	TWeakObjectPtr<AFTPartyVan> Van;
	TWeakObjectPtr<AFTPartyFollower> Followers[2];

	// Captures.
	FString ShotPrefix;
	double ShotAt = 8.0;
	bool bShotCursor = false;
	ftcore::Vec2 ShotCursor{};
	int32 ShotStage = 0;
	double StageStart = 0.0;
};
