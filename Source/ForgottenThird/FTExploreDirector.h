#pragma once

// The exploration demo's rules on the map: holds the ftcore::Exploration built from
// ftcore/ExploreDemo.h, finds the spot the player's prompt points at, uses it on Interact, and
// starts the fight when the player walks into the ambush or a field guard (ftcore::GuardSet)
// catches them. The fight is the battle demo map: the
// director stores the field's state in UFTGameInstance, opens BattleDemo, and the battle demo opens
// this map again after a victory; the director then restores the state and clears the ambush.
//
// bAutoPlay (or -ExploreAutoPlay) walks the player to each objective in turn with Core's pilot
// target and uses it; the battle demo also autoplays under that flag. With -ExploreQuitOnComplete
// the game exits once every objective is done (a headless end-to-end run).
//
// On a big world's map (WorldId set by FTBuildExploreWorld -World=) the director lays the world out
// again from Data/Field/worlds.json (ftcore::WorldLayout) and has no demo spots or guards. A story
// field step puts the player at the spawn of the scene's district and its goal (goals.json, in the
// district's frame) in that district's plaza, with the scene's style. -WorldAutoPlay walks the roads
// from district to district until it has reached them all (logging each), then exits with
// -ExploreQuitOnComplete. A map built from the world's own plan (FTBuildMassingWorld -World=) places
// its districts itself (DistrictCentres, DistrictSpawns): the layout's districts move there, and the
// tour walks between their spawns on the navmesh. With -WorldQuitOnDone a stuck leg is given up and the
// tour goes on, ending in TOUR ENDED and exit code 1 instead of TOUR DONE.
//
// Such a map may carry tour stops (FTBuildMassingWorld -Tour=, TourStopIds and the rest): the tour walks them after
// the district legs, in the table's order. A reach stop is walked to on the navmesh (a wider path search, a cap of
// ftcore::tourLegCapSeconds); an edge stop is walked to the navmesh's nearest point to its target, then pushed
// straight at it, and what stopped the lead is judged (ftcore::judgeEdge); a push stop is pushed straight at its
// target, which must hold the lead off. -WorldTourKinds=<list> picks the kinds, -WorldTourStops=only skips the
// district legs, and -WorldTourStills=<dir> saves a still at the end of each stop marked for one (not under
// -NullRHI). On a map with a sea a leg whose lead's feet go under it is wet. A given-up leg, a bare hidden wall, an
// unjudged edge, a push that got through or a wet leg ends the tour in TOUR ENDED. A map without stops tours as
// before.
//
// On the overworld (bOverworld, FTBuildExploreWorld -World=overworld) the director lays out the
// overworld (ftcore::OverworldLayout). A story field step whose carry has FromWorld set is a hop: the
// player starts at that world's gate and walks to the next world's; Use there opens that world's map
// with the carry, where the field step plays. Without a story step it is a free walk: Use at an open
// world's gate enters it. -OverworldAutoPlay walks to every gate in turn (logging each), then exits
// with -WorldQuitOnDone.
//
// Off the overworld the director is this map's field step host (FTStepHost.h), and it spawns an
// AFTCutsceneDirector as its cutscene host. A done field step first asks FTStep::ContinueHere: when the
// beat goes on with a field step on this map, the lead walks on from where it stands to the new marker
// (StartStepHere); when it goes on with a CG step, the cutscene plays around the lead. Otherwise the
// story map opens as before. A big world's map opened for a CG step (the step host's lookahead) stays
// quiet: no marker, tour or demo; the lead starts at the scene's district spawn and the cutscene host
// begins there on the first tick. It also spawns an AFTArenaHost, the arena step's host: a story fight
// that plays the arena system is fought around the lead, after a field step in place or on a map opened
// for it (then the lead starts at the district spawn, as for a CG). -ArenaHere starts a standalone
// arena fight in the lead's district (no story step).

#include "CoreMinimal.h"
#include "AI/Navigation/NavQueryFilter.h"
#include "GameFramework/Actor.h"

#include "ftcore/Explore.h"
#include "ftcore/Guard.h"
#include "ftcore/Overworld.h"
#include "ftcore/PartyTrail.h"
#include "ftcore/WorldChecks.h"
#include "ftcore/WorldLayout.h"

#include "FTNavIslands.h"
#include "FTStepHost.h"
#include "FTWaterSubsystem.h"

#include "FTExploreDirector.generated.h"

class AFTArenaHost;
class AFTCutsceneDirector;
class AFTExploreCharacter;
class AFTExploreSpot;
class AFTFieldGuard;
class AFTPartyFollower;
struct FFTStoryCarry;

UCLASS()
class FORGOTTENTHIRD_API AFTExploreDirector : public AActor
{
	GENERATED_BODY()

public:
	AFTExploreDirector();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Explore")
	bool bAutoPlay = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Explore")
	bool bQuitOnComplete = false;
	// The map the ambush opens.
	UPROPERTY(EditAnywhere, Category = "Explore")
	FName BattleMap = TEXT("BattleDemo");

	// Indexed by SpotId; filled by the map builder (and found by SpotId if empty).
	UPROPERTY(EditAnywhere, Category = "Explore")
	TArray<TObjectPtr<AFTExploreSpot>> Spots;
	// The big world this map is (an id in Data/Field/worlds.json); empty on the Explore map.
	UPROPERTY(EditAnywhere, Category = "Explore")
	FString WorldId;
	// This map is the overworld that joins the big worlds (Data/Field/worlds.json's "overworld").
	UPROPERTY(EditAnywhere, Category = "Explore")
	bool bOverworld = false;
	// Each district's floor (cm) on a big world's ground, by district index: filled by the map builder; empty on a
	// flat map, where everything stands at 0.
	UPROPERTY(EditAnywhere, Category = "Explore")
	TArray<float> DistrictFloors;
	// A map built from its world's own plan (FTBuildMassingWorld -World=) places its districts itself: each district's
	// plaza centre and spawn (cm; a spawn's Z is the floor the lead lands on there), by district index, filled by the
	// builder. Empty on a generated world's map. When set, BeginWorld moves the layout's districts there
	// (ftcore::WorldLayout::pinDistricts) and the tour walks between them on the navmesh, since the generated roads and
	// buildings aren't on such a map.
	UPROPERTY(EditAnywhere, Category = "Explore")
	TArray<FVector2D> DistrictCentres;
	UPROPERTY(EditAnywhere, Category = "Explore")
	TArray<FVector> DistrictSpawns;
	// The stops a -WorldAutoPlay tour walks after the districts, in order (FTBuildMassingWorld -Tour=, the tour table of
	// ftcore/WorldChecks.h), filled by the builder; empty on any other map, which tours as before. One entry each per
	// stop: its id; its kind (ftcore::TourStopKind: reach, edge or push); its target (cm; Z is the floor to stand on for a
	// reach stop, the ground at the edge point for an edge stop); whether it has a start, and the start (cm, Z its floor),
	// where the lead is set down before the leg; its edge's letter ('W', 'S', 'N' or 'E', 0 off an edge stop); and
	// whether its leg ends in a still (-WorldTourStills=).
	UPROPERTY(EditAnywhere, Category = "Explore|Tour")
	TArray<FString> TourStopIds;
	UPROPERTY(EditAnywhere, Category = "Explore|Tour")
	TArray<uint8> TourStopKinds;
	UPROPERTY(EditAnywhere, Category = "Explore|Tour")
	TArray<FVector> TourStopTargets;
	UPROPERTY(EditAnywhere, Category = "Explore|Tour")
	TArray<bool> TourStopHasStart;
	UPROPERTY(EditAnywhere, Category = "Explore|Tour")
	TArray<FVector> TourStopStarts;
	UPROPERTY(EditAnywhere, Category = "Explore|Tour")
	TArray<uint8> TourStopEdges;
	UPROPERTY(EditAnywhere, Category = "Explore|Tour")
	TArray<bool> TourStopStills;
	// The sea's level (cm) on a map with a sea (FTBuildMassingWorld -Sea=): a tour leg whose lead's feet go under it is
	// wet. Filled by the builder.
	UPROPERTY(EditAnywhere, Category = "Explore|Tour")
	bool bHasSeaLevel = false;
	UPROPERTY(EditAnywhere, Category = "Explore|Tour")
	float SeaLevel = 0.f;
	// The map's water (FTWaterSubsystem.h): its canals (the road net's canal rows) and its sea, in the engine's frame,
	// metres. Filled by the builder.
	UPROPERTY()
	TArray<FFTWaterBodyRow> WaterBodies;
	// The plan's square (m, FTBuildMassingWorld -Side=), for the tour's lines in the plan frame; 0 when not known, and
	// the lines give engine metres.
	UPROPERTY(EditAnywhere, Category = "Explore|Tour")
	float PlanSide = 0.f;
	// The HUD minimap's model (ftcore::saveMiniMap), baked by FTBuildMiniMap; empty: no minimap on this level.
	UPROPERTY()
	TArray<uint8> MiniMapBlob;

	virtual void Tick(float DeltaSeconds) override;

	// Use the spot the prompt points at, if any.
	void Interact();

	// From the pawn (AFTExploreCharacter): it switched to another lead, so the followers are spawned
	// again in a line behind it; or it hid or showed them (hidden ones keep walking the trail).
	void OnLeadChanged(const FString& Lead);
	void SetPartyHidden(bool bHide);
	// For a scene played in place: held, the followers stand where they are and nothing here moves them, so the
	// scene can (AFTPartyFollower::Place walks one to a mark); none hides for being in the camera's way, only while
	// the party is hidden. Followers not spawned yet still spawn behind the lead, then stand. Let go, the trail starts
	// over behind the lead as it stands, and each follower takes it up from where it stands, walking back to its
	// place without a jump (ftcore::PartySlot::resetAt).
	void SetPartyHeld(bool bHold);
	bool IsPartyHeld() const { return bPartyHeld; }
	const TArray<TObjectPtr<AFTPartyFollower>>& GetFollowers() const { return Followers; }
	// This map's in-place cutscene host (null before BeginPlay, or when it could not spawn).
	const AFTCutsceneDirector* GetCutsceneDirector() const;
	// The big world's layout (null off a big world: on the Explore map and the overworld), and a district's spawn:
	// the spot (cm on the ground plane; the caller picks the height) and yaw (degrees). False for no such district.
	const ftcore::WorldLayout* GetWorldLayout() const { return Layout.Get(); }
	bool GetDistrictSpawn(int32 District, FVector2D& Spot, float& Yaw) const;

	// ---- for the HUD (no allocation) ----
	const ftcore::Exploration& GetExploration() const { return Exploration; }
	int32 GetFocus() const { return Focus; }
	// The prompt for the focused spot ("E  Pick up"), or empty.
	const FString& GetPrompt() const;
	const FString& GetObjectiveText(int32 Objective) const;
	const FString& GetNotice() const { return Notice; }
	double GetNoticeAge() const;
	const FString& GetBanner() const { return Banner; }
	double GetBannerAge() const;
	bool IsComplete() const { return bComplete; }
	// Opened by the story for a field step: the demo spots are hidden and one goal marker is shown;
	// reaching it and pressing Use ends the step (after kFieldFallbackSeconds Use works anywhere).
	bool IsStoryWalk() const { return bStoryWalk; }
	// "Reach the marker  ENG  <step label>": notation only. Empty outside a story field step.
	const FString& GetFieldObjective() const { return FieldObjective; }
	// On a big world's map: its layout is in play. GetWorldLine is "solhaven  district 3 of 16".
	bool IsWorldMode() const { return Layout.IsValid(); }
	const FString& GetWorldLine() const { return WorldLine; }
	// On the overworld: its layout is in play, and GetWorldLine is "to lattice" or "lattice  E  Enter".
	bool IsOverworld() const { return Overworld.IsValid(); }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	void CacheText();
	void UpdateObjectiveText(int32 Objective);
	void FindSpots();
	AFTExploreSpot* GetSpot(int32 Id) const;
	AFTExploreCharacter* GetPlayerCharacter() const;
	void RestoreFromCarry();
	void StartEncounter(int32 Id);
	void StartGuardFight(int32 Id);
	void OpenBattle();
	void TickAutoPlay(AFTExploreCharacter& Player, float DeltaSeconds);
	// While FaceTarget is set: turns the standing lead on the spot to face it and uses it once it is
	// in the focus cone; gives up when the lead moves off or out of reach. True while it holds the tick.
	bool TickFace(AFTExploreCharacter& Player, ftcore::Vec2 Pos, float DeltaSeconds);
	void TickStoryWalk(AFTExploreCharacter& Player, float DeltaSeconds);
	// Walks the pawn toward Goal (metres) and runs the stuck detection for TargetId; straight at it
	// without the navmesh when !bUseNav. GoalFloor (cm), when set, is the floor at Goal for the navmesh query;
	// otherwise that of the district Goal stands in. With bPathProgress (a pinned world tour, which caps its legs),
	// getting closer along a full navmesh path counts as progress too.
	void PilotWalk(AFTExploreCharacter& Player, ftcore::Vec2 Pos, ftcore::Vec2 Goal, int32 TargetId, float DeltaSeconds,
		bool bUseNav = true, TOptional<double> GoalFloor = {}, bool bPathProgress = false);
	// The path length (m) left from Pos along the pilot's full navmesh path to TargetId, or -1 when it has none.
	float PilotPathLeftFrom(ftcore::Vec2 Pos, int32 TargetId) const;
	// The STUCK line, with why it fired and the pilot's path.
	void LogPilotStuck(float Dist, int32 TargetId, const FString& Why) const;
	// The navmesh point to steer at on the way to Goal, or null to walk straight at it.
	const ftcore::Vec2* PilotWaypoint(const AFTExploreCharacter& Player, ftcore::Vec2 Pos, ftcore::Vec2 Goal, int32 TargetId,
		TOptional<double> GoalFloor);
	void SpawnFieldGoal();
	void SetNotice(const FString& Text);
	void SetBanner(const FString& Text);
	void CheckComplete();
	void BeginStoryWalk();
	// A big world's district for the carried step (district 0, with a warning, when the scene isn't
	// placed here), its spawn for the first tick and the scene's style. What: "field step", "CG step".
	void PlaceInDistrict(const FFTStoryCarry& Carry, const TCHAR* What);
	// Moves the lead to the district's spawn on the next tick, set down on the spawn's floor when the map has one. False
	// for no such district.
	bool QueueDistrictSpawn(int32 District);
	void BeginWorld();
	void TickWorld(AFTExploreCharacter& Player, float DeltaSeconds);
	// The tour's stops. BeginTourStops reads the switches and picks the stops the tour walks (TourStopOrder);
	// TickTourStop plays a stop leg's phases (set down, settle, walk, push, still) and EndStopLeg ends it; NextTourLeg
	// moves the tour on and EndWorldTour gives its end lines. Game is the tour's game time (s).
	void BeginTourStops();
	void TickTourStop(AFTExploreCharacter& Player, ftcore::Vec2 Pos, double Game, float DeltaSeconds);
	void BeginStopPush(ftcore::Vec2 Pos, ftcore::Vec2 Target, double Feet, double Game);
	void JudgeEdgeStop(const AFTExploreCharacter& Player, int32 Stop);
	void EndStopLeg(int32 Stop, double Game);
	void NextTourLeg(double Game);
	void EndWorldTour(double Game);
	// The given-up leg's located line: where the lead stopped and what the pilot's last navmesh query answered.
	void LogLegStopped(const AFTExploreCharacter& Player, ftcore::Vec2 Pos) const;
	void CountTourMiss();
	// Sets the lead down at At (cm, Z its floor) facing Yaw (degrees) on the next tick, as QueueDistrictSpawn does.
	void QueueStopSetDown(const FVector& At, float Yaw);
	// The next leg is a stop that sets the lead down at its own start.
	bool NextLegSetsDown() const;
	int32 TourLegNumber() const { return TourLeg - TourFirstLeg + 1; }
	// A ground point (engine metres) in the plan frame when the plan's square is known (TourFrame names the frame).
	void TourPlanXY(ftcore::Vec2 Engine, float& X, float& Y) const;
	const TCHAR* TourFrame() const;
	// Gathers TourIslands for the edge walks' aims, once per tour; true when this call gathered them (a scan of the whole
	// navmesh, so the caller waits a tick and the hitch's long frame lands on the settle).
	bool GatherTourIslands();
	void UpdateWorldLine(ftcore::Vec2 Pos);
	void BeginOverworld();
	void TickOverworld(AFTExploreCharacter& Player, float DeltaSeconds);
	void UpdateOverworldLine(ftcore::Vec2 Pos);
	FString WorldName(int32 World) const;
	// Opens a world's map from its gate: with the story's carry on a hop, as a free walk otherwise.
	void EnterWorld(int32 World);
	void ReturnToStory();
	void OpenStoryMap();
	// The step host's start for the beat's next field step on this map: a new marker, walked to from
	// where the lead stands.
	void StartStepHere();
	double Now() const;

	ftcore::Exploration Exploration;
	int32 Focus = -1;
	int32 FaceTarget = -1; // a spot in reach the lead is turning on the spot to face, to use it once faced
	float FaceTime = 0.f;
	int32 PendingEncounter = -1;
	int32 PendingGuard = -1;
	bool bRestorePending = false;
	bool bComplete = false;
	bool bBattleOpening = false;
	bool bStoryWalk = false;
	bool bStoryReturning = false;
	double StoryWalkStart = 0.0;
	// Story field step: the goal marker (metres), its actor, whether the pawn can use it, game
	// seconds spent, and the HUD line.
	ftcore::Vec2 FieldGoal;
	// Optional waypoint the autopilot passes before the marker (Data/Field/goals.json).
	ftcore::Vec2 FieldVia;
	bool bFieldHasVia = false;
	bool bFieldViaDone = false;
	UPROPERTY(Transient)
	TObjectPtr<AFTExploreSpot> GoalSpot;
	// Plays the beat's CG steps on this map, around the lead (off the overworld).
	UPROPERTY(Transient)
	TObjectPtr<AFTCutsceneDirector> CutsceneDirector;
	// Plays the beat's arena fights on this map, around the lead (off the overworld).
	UPROPERTY(Transient)
	TObjectPtr<AFTArenaHost> ArenaHost;
	// The map was opened for a CG step or an arena fight: nothing here runs until a field step starts in
	// place. PendingHost: the host that starts once the lead stands at the spawn (-ArenaHere: the arena's,
	// without a step).
	bool bStepCarry = false;
	EFTStepHost PendingHost = EFTStepHost::None;

	// The party following the lead on a field step, a big world and the overworld, and on the demo
	// walk when the pawn has a party in config: the pawn's PartyLooks after the lead it wears, else
	// ftcore::kPartyNames after the first.
	void TickFollowers(AFTExploreCharacter& Player, float DeltaSeconds);
	void SpawnFollowers(AFTExploreCharacter& Player);
	void PlaceFollowers(AFTExploreCharacter& Player, float DeltaSeconds);
	void ClearFollowers();
	UPROPERTY(Transient)
	TArray<TObjectPtr<AFTPartyFollower>> Followers;
	ftcore::PartyTrail Trail;
	TArray<ftcore::PartySlot> Slots; // each follower's place: behind, to one side, a beat late
	uint32 LeadTurns = 0;       // the lead's turns begun, last seen
	TArray<uint32> TurnCursors; // each follower's place in the trail's turns
	TArray<uint32> JumpCursors; // and in its jumps
	bool bLeadInAir = false;
	// The lead's last turn on the spot: the way it turned to, and how long ago (each follower turns that
	// way a beat after the one before it).
	float LeadFacedYaw = 0.f;
	float LeadFacedAgo = -1.f;
	bool bFollowersSpawned = false;
	bool bPartyHeld = false; // SetPartyHeld
	bool bWatchFollower = false; // -LocoWatchFollower: the camera is behind the first follower, so it stays in view
	// -LocoLog: once a second, the nearest the first follower came to the lead (cm), so overlap shows as a number.
	bool bLogGap = false;
	float GapLogTimer = 0.f;
	float NearestGap = TNumericLimits<float>::Max();

	// The field guards (demo only; a story field step has none). Returns the guard that caught the
	// player this frame, or -1.
	void SpawnGuards(AFTExploreCharacter& Player);
	int32 TickGuards(AFTExploreCharacter& Player, float DeltaSeconds);
	ftcore::GuardSet Guards;
	UPROPERTY(Transient)
	TArray<TObjectPtr<AFTFieldGuard>> GuardActors;
	bool bGuardsSpawned = false;
	bool bFieldReached = false;
	float FieldElapsed = 0.f;
	FString FieldObjective;

	// The big world (null on the Explore map), the district of the story step, the spawn the player is
	// moved to on the first tick, and the -WorldAutoPlay tour (district indices over the roads).
	TUniquePtr<ftcore::WorldLayout> Layout;
	int32 WorldDistrict = -1;
	bool bWorldSpawnPending = false;
	FVector WorldSpawn = FVector::ZeroVector;
	FRotator WorldSpawnRotation = FRotator::ZeroRotator;
	bool bWorldSpawnOnFloor = false; // WorldSpawn.Z is the district's floor, so the lead is set down on it
	bool bDistrictsPinned = false;   // the layout's districts stand at DistrictCentres / DistrictSpawns
	double FloorLogAt = -1.0;        // game time to log where the lead's feet stand against the floor; -1 when done
	bool bWorldTour = false;
	bool bWorldTourDone = false;
	uint8 Tour[2 * ftcore::WorldLayout::kMaxDistricts] = {};
	int32 TourCount = 0;
	int32 TourLeg = 0;
	bool bTourQuitOnDone = false;                                 // -WorldQuitOnDone (or -ExploreQuitOnComplete), read when the tour starts
	int32 TourMisses = 0;                                         // legs given up stuck (quitting runs only)
	TArray<int32> TourMissLegs;                                   // their leg numbers, from 1 (reserved when the tour starts)
	double TourStart = 0.0;
	double TourGameStart = 0.0;
	double TourTraceAt = 0.0; // tour game time of the next per-second trace line
	int32 TourLegFor = -1;     // the leg TourLegStartAt is for
	double TourLegStartAt = 0.0; // tour game time the leg started, for kTourLegCapSeconds
	bool bTourLegCapped = false; // the cap fired on this leg (logged once)
	// The tour's legs: [0, TourCount) walk the districts (Tour), [TourCount, TourEndLeg) the stops picked, by index into
	// TourStopIds (TourStopOrder). The tour starts at TourFirstLeg (TourCount with -WorldTourStops=only) and numbers its
	// legs from there, out of TourLegTotal.
	TArray<int32> TourStopOrder;
	int32 TourFirstLeg = 0;
	int32 TourEndLeg = 0;
	int32 TourLegTotal = 0;
	int32 TourStopsPicked[ftcore::kTourStopKinds] = {};
	int32 TourStillsPicked = 0;
	// The stop leg in play: its phase (the .cpp's ETourStopPhase) and when it began (tour game time); the point the walk
	// aims at (engine metres; for an edge stop the navmesh's nearest to its target on the lead's own island) and its
	// floor (cm); the walk's cap (s), its straight line at the start (m) and whether a full path has re-set the cap
	// (ftcore::tourLegBudget); an edge leg whose walk gave up short, or found no navmesh near the target; the push's
	// direction (held once the lead is at the target), its closest approach to the target (m) and the feet's start and
	// lowest (cm); a stop whose given-up leg sets the lead down on its target for the next leg (-1 for none), and the
	// yaw it faces there.
	uint8 TourStopPhase = 0;
	double TourPhaseAt = 0.0;
	ftcore::Vec2 TourStopAim;
	double TourStopAimFloor = 0.0;
	double TourStopCap = 0.0;
	double TourStopStraight = 0.0;
	bool bTourStopFullPath = false;
	bool bTourStopWalkShort = false;
	bool bTourStopNoNav = false;
	FVector2D TourPushDir = FVector2D(1.0, 0.0);
	bool bTourPushLocked = false;
	float TourPushClosest = 0.f;
	double TourPushStartFeet = 0.0;
	double TourPushLowFeet = 0.0;
	int32 TourSetDownStop = -1;
	float TourSetDownYaw = 0.f;
	// The navmesh's islands (FTGatherNavIslands, corners in engine metres, the polys under the sea left out), gathered
	// once per tour at its first edge stop, for the edge walk's aim; null before that or when the map has no Recast
	// navmesh (tried once).
	TUniquePtr<FFTNavIslands> TourIslands;
	bool bTourIslandsTried = false;
	// The stops' counts: run by kind, reach stops reached, edge legs that met a cause, push legs held and got through;
	// each edge's tally (W, S, N, E), its bare hidden walls and unjudged legs; the wet legs and the last leg found wet.
	int32 TourStopsRun[ftcore::kTourStopKinds] = {};
	int32 TourStopsReached = 0;
	int32 TourEdgesMet = 0;
	int32 TourBareWalls = 0;
	int32 TourUnjudged = 0;
	int32 TourPushHeld = 0;
	int32 TourGotThrough = 0;
	ftcore::EdgeTally TourEdges[4];
	int32 TourWetLegs = 0;
	int32 TourWetLeg = -1;
	// -WorldTourStills=<dir> (full path; empty without it), the still being waited for, the file's stamp before it was
	// asked, and the wait (real seconds).
	FString TourStillsDir;
	FString TourStillFile;
	FDateTime TourStillOldStamp;
	double TourStillUntil = 0.0;
	double TourStillCheckAt = 0.0;
	int32 WorldLineDistrict = -2;
	FString WorldLine;

	// The overworld (null elsewhere): the story's hop (the route and the gate it has reached), which
	// worlds are open on a free walk (bit per world), and the -OverworldAutoPlay tour (node indices).
	TUniquePtr<ftcore::OverworldLayout> Overworld;
	ftcore::OverworldRoute Hop;
	bool bHop = false;
	int32 HopPoint = 0;
	bool bEntering = false;
	uint32 OpenWorlds = 0;
	int32 OverLineNode = -2;
	uint8 OverTour[2 * ftcore::OverworldLayout::kMaxNodes] = {};
	int32 OverTourCount = 0;

	FString Notice;
	double NoticeTime = -100.0;
	FString Banner;
	double BannerTime = -100.0;
	double StartTime = 0.0;

	// Built once: per-objective lines ("Find the light shards  1/3") and per-kind prompts.
	TArray<FString> ObjectiveText;
	TArray<FString> PromptText;
	TArray<FString> LockedText;

	// Autopilot: stuck detection.
	int32 PilotTarget = -1;
	float PilotBestDist = 0.f;
	float PilotStuck = 0.f;    // s without a new best
	float PilotStall = 0.f;    // s under kPilotStallSpeed; negative once that has fired, until the lead walks at speed
	float PilotBestPath = -1.f; // with bPathProgress: the shortest path left (m) on a full path to PilotTarget, -1 for none yet
	float PilotPathLeft = -1.f; // with bPathProgress: the path left now (m), -1 when the pilot has no full path
	bool bPilotGaveUp = false; // the stuck line is logged for PilotTarget; cleared on a new target or any progress
	// Autopilot: its navmesh path to the target in ground metres, refreshed about once a second.
	// Empty (walk straight at the target) when the map has no navmesh or no path was found.
	TArray<ftcore::Vec2> PilotPath;
	int32 PilotPathTarget = -1;
	int32 PilotPathNext = -1;
	double PilotPathRefreshAt = 0.0;
	bool bPilotPathLogged = false;
	// The last refresh's answer, for a world tour's give-up line: 0 never asked, 1 no path, 2 a partial path, 3 a full one.
	uint8 PilotPathState = 0;
	bool bPilotPathLimit = false; // the last refresh's search ran out of nodes
	FVector PilotPathGoal = FVector::ZeroVector; // the goal as asked (engine cm), its z the query's
	FVector PilotPathEnd = FVector::ZeroVector; // that path's last point (engine cm), when it has one
	// A tour stop's walk asks its path with a search budget of kTourSearchNodes (the engine's default is 2048; Detour's
	// node index is 16 bits, so 65535 is the most it can take): the filter, a copy of the navmesh's own, is made on the
	// first such query.
	bool bPilotWideSearch = false;
	FSharedConstNavQueryFilter PilotWideFilter;
};
