#pragma once

// The story state for one game instance: one ftcore::StoryState (the facts a playthrough remembers) and
// the FactRegistry that maps it, both allocated once in Initialize so their addresses never change (the
// fight session keeps raw pointers to them; BindFightSession). All rules live in Core (StoryState.h);
// this reads the registry files, copies the save block and logs.
//
// * The registry comes from the story folder (UFTGameInstance::StoryDir, default Data/Story:
//   story_facts.csv + story_tuning.json, beside the story) at start.
//   With neither file it runs on an empty registry: every write is refused, nothing breaks. One file
//   without the other, or a refused import, also leaves it empty, with a warning.
// * The save block rides in UFTStorySaveGame beside the story JSON (StoryBlock, written by CaptureFrom,
//   read back by ApplyTo). A save from before it, or a block Core refuses, loads as a new game's state
//   (ftcore::storyLoadOrNew), with a warning for a refused one.
// * A new game resets the state (NewGame). A block autosaved at a scene start holds whatever scene the
//   state has open then: the host calls BeginScene when its scene starts, so the order of the two
//   decides it; both load back as saved.
// * The scene flow: with a scene map (story_scene_map.csv in the same folder: `scene,code`, a story-runner scene
//   id and the registry scene it plays; private, beside the registry) it follows UFTStorySubsystem. A new scene
//   commits the open one and begins its own (ftcore::storyEnterScene); a scene with no code, or the story's end,
//   only commits; consecutive runner scenes that share a code continue the one registry scene; a load
//   (SyncToRunnerScene) restarts the open scene when the runner is still in it. Without a map it calls nothing.
// * -StoryStateCheck: a headless check. It round-trips the block in memory, logs one
//   "StoryState check:" line and asks the engine to quit. It never writes a save slot.

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ftcore/StoryState.h"

#include "FTStoryStateSubsystem.generated.h"

class UFTFightSession;

UCLASS()
class FORGOTTENTHIRD_API UFTStoryStateSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Imports the registry from Dir (relative: to the project folder). Called by Initialize with the story folder
	// and by UFTGameInstance::ReloadStory, with the scene map beside it. The registry is assigned in place and the state reset to a new
	// game's (its slots and scenes belong to the registry); a bound fight session is unbound first (bind it again
	// after its next Begin).
	bool LoadRegistry(const FString& Dir);
	// True when the registry was imported (false: the empty registry).
	UFUNCTION(BlueprintPure, Category = "Story")
	bool HasRegistry() const { return bRegistryLoaded; }

	// ---- the scene ----

	// The scene flow (the scene map): the runner's scene SceneId began; empty: the story ended. Nothing without a map.
	void EnterRunnerScene(const FString& SceneId);
	// After a load: brings the state to the runner's current scene (restart when it is the open one). Nothing without a map.
	void SyncToRunnerScene(const FString& SceneId);
	// The number of runner scenes the map links (0: no map, the flow calls nothing).
	UFUNCTION(BlueprintPure, Category = "Story")
	int32 GetSceneMapSize() const { return SceneMap.Num(); }

	// Opens the scene with that code (the registry's scene list). False for an unknown code, a scene
	// already open, or one not after the last committed scene.
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool BeginScene(const FString& SceneCode);
	// Closes the open scene. False when none is open.
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool CommitScene();
	// Back to the open scene's start. False when none is open.
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool RestartScene();
	// A new game's state.
	UFUNCTION(BlueprintCallable, Category = "Story")
	void NewGame();

	// ---- native access (null before Initialize and after Deinitialize) ----

	ftcore::StoryState* GetState() { return State.Get(); }
	const ftcore::StoryState* GetState() const { return State.Get(); }
	const ftcore::FactRegistry* GetRegistry() const { return Registry.Get(); }
	// Binds the session's story writes to this state (FightSession::bindStory). Call after every Begin.
	void BindFightSession(UFTFightSession* Session);

	// ---- the save block (UFTStorySaveGame) ----

	// Writes the block into Out. False (Out kept as it was, with a warning) before Initialize or when Core refuses.
	bool EncodeBlock(TArray<uint8>& Out);
	// Loads the block, or a new game's state when it is empty or Core refuses it. Returns Core's result.
	ftcore::StoryLoad DecodeBlock(const TArray<uint8>& In);

private:
	void Unbind();
	void LoadSceneMap(const FString& Base);
	UFUNCTION()
	void HandleRunnerScene(const FString& SceneId);
	UFUNCTION()
	void HandleRunnerFinished();

	TUniquePtr<ftcore::FactRegistry> Registry;
	TUniquePtr<ftcore::StoryState> State;
	bool bRegistryLoaded = false;
	// The fight session last bound (weak: the runner owns it); unbound on a registry import and on Deinitialize.
	TWeakObjectPtr<UFTFightSession> BoundSession;
	// Runner scene id to registry scene ordinal (empty: no map). The runner scene last entered, so a scene that
	// shares its code with the one before continues it and only the same id again restarts.
	TMap<FString, uint8> SceneMap;
	FString LastRunnerScene;
};
