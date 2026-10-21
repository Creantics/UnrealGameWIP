#pragma once

// The exploration demo's rules on the map: holds the ftcore::Exploration built from
// ftcore/ExploreDemo.h, finds the spot the player's prompt points at, uses it on Interact, and
// starts the fight when the player walks into the ambush. The fight is the battle demo map: the
// director stores the field's state in UFTGameInstance, opens BattleDemo, and the battle demo opens
// this map again after a victory; the director then restores the state and clears the ambush.
//
// bAutoPlay (or -ExploreAutoPlay) walks the player to each objective in turn with Core's pilot
// target and uses it; the battle demo also autoplays under that flag. With -ExploreQuitOnComplete
// the game exits once every objective is done (a headless end-to-end run).

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ftcore/Explore.h"

#include "FTExploreDirector.generated.h"

class AFTExploreCharacter;
class AFTExploreSpot;

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
	void OpenBattle();
	void TickAutoPlay(AFTExploreCharacter& Player, float DeltaSeconds);
	void TickStoryWalk(AFTExploreCharacter& Player, float DeltaSeconds);
	// Walks the pawn toward Goal (metres) and runs the stuck detection for TargetId.
	void PilotWalk(AFTExploreCharacter& Player, ftcore::Vec2 Pos, ftcore::Vec2 Goal, int32 TargetId, float DeltaSeconds);
	// The navmesh point to steer at on the way to Goal, or null to walk straight at it.
	const ftcore::Vec2* PilotWaypoint(const AFTExploreCharacter& Player, ftcore::Vec2 Pos, ftcore::Vec2 Goal, int32 TargetId);
	void SpawnFieldGoal();
	void SetNotice(const FString& Text);
	void SetBanner(const FString& Text);
	void CheckComplete();
	void BeginStoryWalk();
	void ReturnToStory();
	void OpenStoryMap();
	double Now() const;

	ftcore::Exploration Exploration;
	int32 Focus = -1;
	int32 PendingEncounter = -1;
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
	bool bFieldReached = false;
	float FieldElapsed = 0.f;
	FString FieldObjective;

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
