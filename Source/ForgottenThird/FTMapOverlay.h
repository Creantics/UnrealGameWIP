// The map overlay, option C (ftcore/MapOverlay.h), behind -MapOverlayC: north up over the paused play, three zoom levels
// (LT/RT, the wheel), a cursor (the right stick, the mouse, the arrows; the view pans when it reaches an edge). The look
// is the orchestrator's pick (2026-11-17): a blue-black ground, building blocks as dark fills with a faint roof hatch and
// a lit north edge (until the top-down relief bake exists), near-black water, roads cased in thin gold with a soft glow,
// serif names along the roads, the districts' names in small caps in gold-ruled boxes with their sign forms drawn small
// above, water named in italics, the objective as a hollow diamond, the party as gold marks, a small N needle and the
// where-am-I line bottom left over a gold rule. It opens on the player at the near zoom. Every zoom is framed by Core
// (ftcore/MapLook.h, mapOverlayFrame): the model's own land at least three quarters of the frame where a shift can reach
// it (the ground past the model counts as none), the player kept in its middle; the far zoom fits the land. Past the area
// the ground runs on, and the sea on its side and past its edge rows, with the coast drawn wherever that sea meets ground
// (edgeCoast); roads that run off the model fade out at its edge. The look built at load (mapBuildLook): the coast in
// gold, wave lines off it, the canals' edges, parks hatched faintly, yards edged in dots, the sea named once over open
// sea. Which names show at each zoom is Core's (mapLabelPicks); each unboxed one has a 3 px halo and none covers the
// where-am-I line's kept box (mapCaptionLayout: the plate follows the measured text). Under -UiHudTextScale the names,
// their boxes and halos,
// the N and the where-am-I line (and its kept box) grow with the text size setting (FT::HudTextScale). Names come from the
// world lane's names table (-MapNames=<csv>, else Data/Map/solhaven_names.csv; none: no names drawn). Fonts are the new
// UI's (FT::UiCanvasFont). Without the switch UFTMiniMap draws its old whole-map overlay.
//
// Captures: -MapOverlayShots=<path prefix> [-MapOverlayShotAt=<s>, default 8] [-MapOverlayCursor=x:y, engine m]: the
// overlay opens at that time and a shot is taken at each zoom (_far, _mid, _near), each framed on the player (or the
// cursor's point), then the game quits. Per shot: MAPOVERLAY SHOT <far|mid|near> centre=<x>:<y> mpp=<m per px>
// land=<share, two places, truncated> caption='<DISTRICT> (middle dot) <STREET>' labels=<n>; then MAPOVERLAY TEXT <zoom> ts=<text
// scale> clipped=<labels whose halo leaves the screen> caption_right=<px> caption_bottom=<px> caption_text_right=<px>
// caption_plate_right=<px> caption_fit=<text scale> caption_in_plate=<1: the text ends a pad inside the plate>
// text_draws=<text items drawn in the last frame> glyph_passes=<the glyph passes they made> screen=<w>x<h>. Then NAMES,
// DRAWN (name:WxH px), UNPICKED (why out_of_tier / road_rule / cap / other), TIERS <zoom> lowest=<tier> out_of_tier=<n>
// above_lowest_undrawn=<n> [name:tier:why] lowest_undrawn=<n> unbuilt=<n> [names] cap_cut=<n> cap_cut_above_lowest=<n>
// drawn_out_of_tier=<n> bank_seats=<n> district_moves=<n> (the tier table: ftcore::mapTierShown), and CURSOR (why none / unnamed / drawn /
// district / a drop reason).
#pragma once

#include "CoreMinimal.h"

#include "ftcore/MapLook.h"
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
	// Frames the view at the zoom on a world point (mapOverlayFrame) and puts the cursor on it.
	void FrameOn(const ftcore::Vec2& At);
	void AddTri(FCanvasTriangleItem& To, const FVector2D& A, const FVector2D& B, const FVector2D& C, const FLinearColor& Color);
	void AddScreenRect(FCanvasTriangleItem& To, float X0, float Y0, float X1, float Y1, const FLinearColor& Color);
	void AddRect(const ftcore::MapRect& R, const FLinearColor& Color);
	// A block's roof: a faint hatch along its long side (StepM apart; none when StepM <= 0), its north edge lit, its south
	// edge dark.
	void AddRoof(const ftcore::MapRect& R, float StepM, float WidthPx, const FLinearColor& Hatch, const FLinearColor& Lit,
		const FLinearColor& Shade);
	void AddSegment(FCanvasTriangleItem& To, const FVector2D& A, const FVector2D& B, float WidthPx, const FLinearColor& Color, bool bCaps);
	// A segment shaded from ColorA at A to ColorB at B, each end capped or not on its own.
	void AddShadedSegment(FCanvasTriangleItem& To, const FVector2D& A, const FVector2D& B, float WidthPx, const FLinearColor& ColorA,
		const FLinearColor& ColorB, bool bCapA, bool bCapB);
	void AddRoadSegment(const ftcore::Vec2& A, const ftcore::Vec2& B, float WidthPx, const FLinearColor& Color, bool bCaps);
	// Road R's segment K (S0: how far along the road it starts, m), faded out toward an end that runs off the model
	// (ftcore::mapRoadFade over FadeM): split where the fade starts, so the rest is drawn whole.
	void AddFadedRoadSegment(size_t R, uint32 K, float S0, float FadeM, float WidthPx, const FLinearColor& Color, bool bCaps);
	// A name's size at Px (screen px) in a face: measured once per name and face at a reference size, then scaled.
	FVector2D NameSize(int32 Name, EFace Face, float Px);
	FVector2D Measure(FStringView Text, EFace Face, float Px) const;
	// HaloPx > 0: a soft dark halo that far round the glyphs, under the text's own dark rim.
	void DrawText(UCanvas& Canvas, FStringView Text, EFace Face, float Px, const FVector2D& TopLeft, const FLinearColor& Color,
		float HaloPx = 0.f) const;
	// TS: the text's scale (S times the player's text size); S: the map's own lengths. Discs: the marks drawn over the names
	// (the cursor's ring, the player's arrow), which the cursor's own name is kept clear of.
	// Kept: the where-am-I line's kept box (ftcore::mapCaptionLayout), which no label enters. CursorAt: the cursor's world
	// point (where no road's name stands at it, the cursor's name is the district there, set level at it).
	void BuildLabels(int32 Picked, uint16 PickedHere, float S, float TS, const ftcore::MapDisc* Discs, int32 NumDiscs,
		const ftcore::MapLabel& Kept, const ftcore::Vec2& CursorAt);
	void DrawLabel(UCanvas& Canvas, const ftcore::MapLabel& L, float TS);

	const ftcore::MiniMapModel& Model;
	std::vector<std::uint8_t> Enterable;
	std::vector<ftcore::MapRect> Water;
	std::vector<float> RoadBox; // per road: x0, y0, x1, y1 (m)
	std::vector<float> RoadLen; // per road: its length (m)
	std::vector<std::uint8_t> RoadOff; // per road: its ends that run off the model (ftcore::mapRoadOffEnds)
	int32 SeaSide = 0;          // ftcore::mapSeaSide
	ftcore::MapNames Names;
	ftcore::MapLook Look; // built once at load from the model and the names
	// The sea along the north and south edges: the edge rows' sea runs, the outer ones run on past the area.
	std::vector<ftcore::MapRect> SeaEdge;
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
	// Every picked name in the frame that was left out at the last placing, with why (for the capture log; the first
	// kMaxDrops by name, every one by reason). A pick in the frame is an along pick, or a point pick whose anchor is on screen;
	// the sea's name, when the table has one, is in the frame whenever sea is on screen. TooLong: longer than its stretch
	// in the frame (the frame cut the road); TooLongWhole: longer than the whole road, or a water name than its water.
	// Unbuilt: a point the world does not hold yet (the names table's built column). OffDistrict: no seat inside its
	// district. OffFrame: a road's name whose road only grazes the frame (no stretch of it inside). None: no reason, only
	// as a first reason (a drop's own seat was not tried or not needed).
	enum class EDrop : uint8 { Dry, NoSeat, OnRoad, OnCoast, OnWater, NoStretch, TooLong, TooBig, OnMarks, OnLit, Crowded, Caption, Full,
		Unbuilt, TooLongWhole, OffDistrict, OffFrame, None };
	static constexpr int32 kDropKinds = 17; // every reason but None
	struct FDrop
	{
		uint16 Name = 0;
		EDrop Why = EDrop::Dry;
		ftcore::MapTier Tier = ftcore::MapTier::District; // the tier its pick held (ftcore::mapRankTier)
		EDrop First = EDrop::None; // why it had no seat of its own, when it then fell back to a point and failed there too
	};
	// Each pick drops at most once, the sea's name once more: the list is never cut (the second seat's names drop last, so
	// a cut list would lose exactly the must-draw names; turn 14 reached 126).
	static constexpr int32 kMaxDrops = static_cast<int32>(kMaxPicks) + 1;
	FDrop Drops[kMaxDrops];
	int32 DropCount = 0, DropTotal = 0;
	int32 DropKinds[kDropKinds] = {};
	// The tier table's counters at the last placing (every drop, not only the first kMaxDrops): drops above the zoom's
	// lowest tier (unbuilt apart), at the lowest, unbuilt; label cap cuts, and those above the lowest (must be 0).
	int32 DropsAboveLowest = 0, DropsLowest = 0, DropsUnbuilt = 0, CapCuts = 0, CapCutsAboveLowest = 0;
	int32 BankSeats = 0;     // water names seated beside their water (ftcore::mapBankLabelAt)
	int32 DistrictMoves = 0; // districts' names seated off their point (the place seat's rings)
	// The must-draw names (ftcore::mapTierMustDraw) left out at the last placing, unbuilt and off-frame apart: all, then
	// crowded by an earlier name, under the caption, and any other reason (a defect).
	int32 MustUndrawn = 0, MustCrowded = 0, MustCaption = 0, MustOther = 0;
	// How each candidate was seated: along its road, on its water (or its bank), the sea's, at a point after falling back,
	// a place's, a district's, the cursor's (its road's or district's).
	enum class ESeat : uint8 { Along, Water, Sea, Point, Place, District, Cursor };
	// A point seat's note for the capture log: the name, the gap from its point to its box's nearest edge (box heights), and
	// (second seats only) the box's centre on the screen, px, so a close still can be cut round it.
	struct FSeatNote
	{
		uint16 Name = 0;
		float GapH = 0.f;
		float X = 0.f, Y = 0.f;
	};
	static constexpr int32 kMaxSeatNotes = 16;
	// The point seats at the last placing: names seated at a point after falling back, standing seats, places seated on any
	// dry land (mapPlaceLabelOrAnyLand), districts seated outside their own, seats more than 3 heights from their point
	// (the first kMaxSeatNotes of each listed), the furthest gap, and the build's time.
	int32 PointSeats = 0, StandingSeats = 0, PlaceAnyLand = 0, DistrictOutside = 0, FarSeats = 0;
	float MaxGapH = 0.f;
	double LastBuildMs = 0.0;
	FSeatNote FarNotes[kMaxSeatNotes], AnyLandNotes[kMaxSeatNotes], OutsideNotes[kMaxSeatNotes];
	// The second seat (turn 15): must-draw names crowded at their seat or left out by the placer, seated again after it
	// against only the labels drawn. Names it seated (with the gap, box heights), and of them the road names the placer
	// left out along their road that it seated at a point (counted apart, for ruling); at most kMaxSecondSeats wait.
	static constexpr int32 kMaxSecondSeats = 64;
	int32 SecondSeats = 0, AlongAsPoint = 0;
	FSeatNote SecondNotes[kMaxSeatNotes], AlongPointNotes[kMaxSeatNotes];
	int32 InFramePicks = 0;                       // picks in the frame at the last placing
	uint16 CursorPickName = ftcore::kMapNoName;   // the cursor's pick at the last placing (none: no road, or no name)
	bool bCursorDistrict = false;                 // and whether it is the district at the cursor (no road's name there)
	FVector2D LabelCursor = FVector2D(-1.0, -1.0); // the cursor (px) the labels were placed at, while that pick follows it
	// The placer's candidates, then the names a second seat seated that never were one (their kinds for the log's checks);
	// LastSeatKinds counts both.
	ftcore::MapLabel Candidates[ftcore::kMaxMapLabels + kMaxSecondSeats];
	ESeat CandKind[ftcore::kMaxMapLabels + kMaxSecondSeats] = {};
	int32 LastSeatKinds = 0;
	ftcore::MapLabelSet Placed;
	// The labels are placed again only when what they depend on moves.
	ftcore::MapOverlayView LabelView;
	ftcore::MapZoom LabelZoom = ftcore::MapZoom::Near;
	int32 LabelPicked = -2;
	uint16 LabelPickedName = ftcore::kMapNoName;
	float LabelTS = -1.f; // the text scale the labels were placed at
	float LabelS = 1.f;   // and the map's own scale (the roads' ink)
	// The marks the cursor's name was kept clear of at the last placing, and whether it had to move (or was left out): then
	// a mark that moves places the labels again, so the name comes back as near its stretch's middle as it can.
	ftcore::MapDisc LabelDiscs[2];
	int32 LabelDiscCount = 0;
	bool bCursorNameMoved = false, bCursorNameDropped = false;

	ftcore::MapZoom Zoom = ftcore::MapZoom::Near;
	ftcore::MapOverlayView View;
	FVector2D Cursor = FVector2D::ZeroVector; // screen px
	uint64 LastFrame = 0;
	ftcore::Vec2 Focus{};  // the player (or the capture's cursor point) when the map opened
	ftcore::Vec2 Framed{}; // the last framed centre: a pan may always come back to it (mapOverlayHold)
	float FramedLand = 1.f;
	bool bFramedReached = true;
	// The where-am-I line's names, as last drawn (for the capture log).
	uint16 CaptionDistrict = ftcore::kMapNoName;
	uint16 CaptionStreet = ftcore::kMapNoName;
	float CaptionRight = 0.f, CaptionBottom = 0.f; // its rule's right and bottom ends, px (0: no line drawn yet)
	float CaptionFit = 1.f, CaptionTextRight = 0.f, CaptionPlateRight = 0.f; // its text's scale, the text's and plate's right ends
	ftcore::MapLabel LabelKept;         // the caption's kept box the labels were placed against
	mutable int32 TextDraws = 0;        // text items drawn this frame (each halo copy one)
	mutable int32 GlyphPasses = 0;      // and the glyph passes they make (an outlined item is five)
	int32 LastTextDraws = 0, LastGlyphPasses = 0; // the same in the last whole frame
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
