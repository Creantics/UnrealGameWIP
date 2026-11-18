// The map overlay, option C (ftcore/MapOverlay.h), behind -MapOverlayC: north up over the paused play, three zoom levels
// (LT/RT, the wheel), a cursor (the right stick, the mouse, the arrows; the view pans when it reaches an edge). The look
// is the orchestrator's pick (2026-11-17): a blue-black ground, building blocks as dark fills with a faint roof hatch and
// a lit north edge (until the top-down relief bake exists), near-black water, roads cased in thin gold with a soft glow,
// serif names along the roads, the districts' names in small caps in gold-ruled boxes with their sign forms drawn small
// above, water named in italics, the objective as a hollow diamond, the party as gold marks, a small N needle and the
// where-am-I line bottom left over a gold rule. It opens on the player at the near zoom; the far zoom shows the whole
// area, letterboxed by hills and the sea. Which names show at each zoom is Core's (mapLabelPicks). Names come from the
// world lane's names table (-MapNames=<csv>, else Data/Map/solhaven_names.csv; none: no names drawn). Fonts are the new
// UI's (FT::UiCanvasFont). Without the switch UFTMiniMap draws its old whole-map overlay.
//
// Captures: -MapOverlayShots=<path prefix> [-MapOverlayShotAt=<s>, default 8] [-MapOverlayCursor=x:y, engine m]: the
// overlay opens at that time and a shot is taken at each zoom (_far, _mid, _near), then the game quits.
#pragma once

#include "CoreMinimal.h"

#include "ftcore/MapOverlay.h"
#include "ftcore/SignScript.h"

#include <vector>

class AFTPartyFollower;
class AFTPartyVan;
class AHUD;
class APlayerController;
class FCanvasTriangleItem;
class UCanvas;
class UFont;
class UTexture2D;

class FFTMapOverlay
{
public:
	static bool Wanted(); // -MapOverlayC

	// The model is UFTMiniMap's (it outlives this); White is its plain white texture. PlanSide (m) turns the names
	// table's plan-metre points to engine metres (0: read them as engine metres).
	FFTMapOverlay(const ftcore::MiniMapModel& InModel, UTexture2D& White, double PlanSide);
	~FFTMapOverlay();
	FFTMapOverlay(const FFTMapOverlay&) = delete;
	FFTMapOverlay& operator=(const FFTMapOverlay&) = delete;

	// The capture run's clock: may open the overlay (bOverlay) and quits when done.
	void TickShots(APlayerController& PC, double Now, bool& bOverlay);
	void Draw(AHUD& Hud, UCanvas& Canvas, float S);

private:
	enum EFace : uint8 { Serif, SerifBold, SerifSc, FaceCount };

	void OnOpen(AHUD& Hud, float ScreenW, float ScreenH);
	void ReadInput(APlayerController& PC, float S, float Dt);
	void SetZoom(ftcore::MapZoom Z);
	void AddTri(FCanvasTriangleItem& To, const FVector2D& A, const FVector2D& B, const FVector2D& C, const FLinearColor& Color);
	void AddScreenRect(FCanvasTriangleItem& To, float X0, float Y0, float X1, float Y1, const FLinearColor& Color);
	void AddRect(const ftcore::MapRect& R, const FLinearColor& Color);
	// A block's roof: a faint hatch along its long side (StepM apart; none when StepM <= 0), its north edge lit, its south
	// edge dark.
	void AddRoof(const ftcore::MapRect& R, float StepM, float WidthPx, const FLinearColor& Hatch, const FLinearColor& Lit,
		const FLinearColor& Shade);
	void AddSegment(FCanvasTriangleItem& To, const FVector2D& A, const FVector2D& B, float WidthPx, const FLinearColor& Color, bool bCaps);
	void AddRoadSegment(const ftcore::Vec2& A, const ftcore::Vec2& B, float WidthPx, const FLinearColor& Color, bool bCaps);
	// A name's size at Px (screen px) in a face: measured once per name and face at a reference size, then scaled.
	FVector2D NameSize(int32 Name, EFace Face, float Px);
	FVector2D Measure(FStringView Text, EFace Face, float Px) const;
	void DrawText(UCanvas& Canvas, FStringView Text, EFace Face, float Px, const FVector2D& TopLeft, const FLinearColor& Color) const;
	void BuildLabels(int32 Picked, float S);
	void DrawLabel(UCanvas& Canvas, const ftcore::MapLabel& L, float S);

	const ftcore::MiniMapModel& Model;
	std::vector<std::uint8_t> Enterable;
	std::vector<ftcore::MapRect> Water;
	std::vector<float> RoadBox; // per road: x0, y0, x1, y1 (m)
	int32 SeaSide = 0;          // ftcore::mapSeaSide
	ftcore::MapNames Names;
	TArray<FString> NameText;               // per name
	TArray<FVector2f> NameRef[FaceCount];   // per face, per name: its size at the reference size (x < 0: not measured yet)
	FVector2f NeedleRef{-1.f, -1.f};        // the needle's "N" at the reference size
	std::vector<std::uint8_t> Seen; // mapLabelPicks' scratch, one byte per name
	// The sign forms of the districts' names, laid out once: per name, its strokes' first index and count, and width.
	std::vector<ftcore::SignStroke> SignStrokes;
	TArray<int32> SignFirst, SignCount;
	TArray<float> SignWidth;
	UFont* Fonts[FaceCount] = {};

	FCanvasTriangleItem* Geo = nullptr;   // the map: ground, water, blocks, roads
	FCanvasTriangleItem* Boxes = nullptr; // the labels' boxes and sign forms
	FCanvasTriangleItem* Marks = nullptr; // the objective, the party, the cursor, the needle, the rule
	// More picks than labels: names that do not fit their stretch drop out before the placer, so the lanes still get a turn.
	static constexpr size_t kMaxPicks = 256;
	ftcore::MapLabelPick Picks[kMaxPicks];
	int32 LastPicks = 0, LastCandidates = 0; // for the capture log
	ftcore::MapLabel Candidates[ftcore::kMaxMapLabels];
	ftcore::MapLabelSet Placed;
	// The labels are placed again only when what they depend on moves.
	ftcore::MapOverlayView LabelView;
	ftcore::MapZoom LabelZoom = ftcore::MapZoom::Near;
	int32 LabelPicked = -2;

	ftcore::MapZoom Zoom = ftcore::MapZoom::Near;
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
