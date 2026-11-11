// The HUD minimap (ftcore/MiniMap.h): a round map in the top-right corner of a world map, turning with the player's
// heading (on foot about 120 m across the radius, in the party van 250 m), and with M a north-up overlay of the whole
// map. The map is drawn once into a texture when the level starts; each frame only a triangle fan sampling it and the
// live markers are drawn: the player's arrow, the other two leads, the party van (pinned to the rim with its distance
// when off the map), doors on foot, the landmark, a north tick, and the street under the player as an id and class.
//
// The model comes from the level's AFTExploreDirector::MiniMapBlob (baked by the world build, FTBuildMassingWorld, from
// the tables it built the map from; or re-baked by FTBuildMiniMap), or, for a dev run, from the layout tables named file
// by file: -MiniMapRoads= -MiniMapStreets= [-MiniMapBoxes= -MiniMapStandInTowers= -MiniMapRoadDoors= -MiniMapHeight=
// -MiniMapSea=<x> -MiniMapSide=<m> -MiniMapArea=x0,y0,x1,y1] (FTMiniMapSource.h).
// Captures: -MiniMapShots=<path prefix> with -MiniMapAt=x:y;x:y (plan frame: a shot standing at each, then the overlay,
// then quit) or -MiniMapShotTimes=<s>,<s> (shots at those seconds of play).
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ftcore/MiniMap.h"

#include "FTMiniMap.generated.h"

class AFTExploreDirector;
class AFTPartyFollower;
class AFTPartyVan;
class AHUD;
class APlayerController;
class FCanvasTriangleItem;
class UCanvas;
class UTexture2D;
struct FCanvasUVTri;

UCLASS(Transient)
class FORGOTTENTHIRD_API UFTMiniMap : public UObject
{
	GENERATED_BODY()

public:
	UFTMiniMap();
	virtual ~UFTMiniMap() override;

	// Loads the model and draws the map texture; false (and nothing drawn later) when the level has no minimap.
	bool Init(const AFTExploreDirector& Director);
	bool IsReady() const { return MapTexture != nullptr; }
	void ToggleOverlay() { bOverlay = !bOverlay; }
	bool IsOverlay() const { return bOverlay; }
	void Draw(AHUD& Hud, UCanvas& Canvas, float UiScale);

private:
	void FindActors(UWorld& World);
	void DrawCorner(AHUD& Hud, UCanvas& Canvas, float S, const FVector2D& Centre, float Radius);
	void DrawOverlay(AHUD& Hud, UCanvas& Canvas, float S);
	void TickShots(APlayerController& PC, double Now);
	void AddTri(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FLinearColor& Color);
	void AddQuad(const FVector2D& At, float Half, const FLinearColor& Color, float TurnRad = 0.f);
	void AddArrow(const FVector2D& At, const FVector2D& Dir, float Size, const FLinearColor& Color);
	void AddDisc(TArray<FCanvasUVTri>& Tris, const FVector2D& Centre, float Radius, const FLinearColor& Color);

	UPROPERTY()
	TObjectPtr<UTexture2D> MapTexture;
	UPROPERTY()
	TObjectPtr<UTexture2D> WhiteTexture;

	ftcore::MiniMapModel Model;
	ftcore::MapImage Image; // its geometry only (the pixels are freed once uploaded)
	double PlanSide = 0.0;
	bool bOverlay = false;

	// Built once, refilled each frame (their triangle arrays keep their capacity).
	FCanvasTriangleItem* DiscItem = nullptr;
	FCanvasTriangleItem* RimItem = nullptr;
	FCanvasTriangleItem* MarkItem = nullptr;

	TWeakObjectPtr<AFTPartyVan> Van;
	TWeakObjectPtr<AFTPartyFollower> Followers[2];
	double NextScan = 0.0;

	// The corner view last drawn with a pawn, kept while there is none.
	ftcore::MapView LastView;
	bool bHaveView = false;
	bool bLastInVan = false;

	int32 Road = -1;
	int32 RoadShown = -2;
	FString RoadLine;
	int32 VanMetresShown = -1;
	FString VanMetres;

	// Captures.
	FString ShotPrefix;
	TArray<FVector2D> ShotAt;  // plan frame
	TArray<double> ShotTimes;
	int32 ShotStage = 0;
	double StageStart = -1.0;
};
