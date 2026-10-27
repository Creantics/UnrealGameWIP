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
// -ExploreQuitOnComplete.
//
// On the overworld (bOverworld, FTBuildExploreWorld -World=overworld) the director lays out the
// overworld (ftcore::OverworldLayout). A story field step whose carry has FromWorld set is a hop: the
// player starts at that world's gate and walks to the next world's; Use there opens that world's map
// with the carry, where the field step plays. Without a story step it is a free walk: Use at an open
// world's gate enters it. -OverworldAutoPlay walks to every gate in turn (logging each), then exits
// with -WorldQuitOnDone.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ftcore/Explore.h"
#include "ftcore/Guard.h"
#include "ftcore/Overworld.h"
#include "ftcore/PartyTrail.h"
#include "ftcore/WorldLayout.h"

#include "FTExploreDirector.generated.h"

class AFTExploreCharacter;
class AFTExploreSpot;
class AFTFieldGuard;
class AFTPartyFollower;

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

	virtual void Tick(float DeltaSeconds) override;

	// Use the spot the prompt points at, if any.
	void Interact();

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
	// without the navmesh when !bUseNav.
	void PilotWalk(AFTExploreCharacter& Player, ftcore::Vec2 Pos, ftcore::Vec2 Goal, int32 TargetId, float DeltaSeconds,
		bool bUseNav = true);
	// The navmesh point to steer at on the way to Goal, or null to walk straight at it.
	const ftcore::Vec2* PilotWaypoint(const AFTExploreCharacter& Player, ftcore::Vec2 Pos, ftcore::Vec2 Goal, int32 TargetId);
	void SpawnFieldGoal();
	void SetNotice(const FString& Text);
	void SetBanner(const FString& Text);
	void CheckComplete();
	void BeginStoryWalk();
	void BeginWorld();
	void TickWorld(AFTExploreCharacter& Player, float DeltaSeconds);
	void UpdateWorldLine(ftcore::Vec2 Pos);
	void BeginOverworld();
	void TickOverworld(AFTExploreCharacter& Player, float DeltaSeconds);
	void UpdateOverworldLine(ftcore::Vec2 Pos);
	FString WorldName(int32 World) const;
	// Opens a world's map from its gate: with the story's carry on a hop, as a free walk otherwise.
	void EnterWorld(int32 World);
	void ReturnToStory();
	void OpenStoryMap();
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

	// The party following the lead on a story field step (ftcore::kPartyNames after the first).
	void SpawnFollowers(AFTExploreCharacter& Player);
	void PlaceFollowers(AFTExploreCharacter& Player, float DeltaSeconds);
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
	bool bWorldTour = false;
	bool bWorldTourDone = false;
	uint8 Tour[2 * ftcore::WorldLayout::kMaxDistricts] = {};
	int32 TourCount = 0;
	int32 TourLeg = 0;
	double TourStart = 0.0;
	double TourGameStart = 0.0;
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
	float PilotStuck = 0.f;
	// Autopilot: its navmesh path to the target in ground metres, refreshed about once a second.
	// Empty (walk straight at the target) when the map has no navmesh or no path was found.
	TArray<ftcore::Vec2> PilotPath;
	int32 PilotPathTarget = -1;
	int32 PilotPathNext = -1;
	double PilotPathRefreshAt = 0.0;
	bool bPilotPathLogged = false;
};
