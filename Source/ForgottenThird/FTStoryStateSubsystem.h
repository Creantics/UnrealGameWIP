#pragma once

// The story state for one game instance: one ftcore::StoryState (the facts a playthrough remembers) and
// the FactRegistry that maps it, both allocated once in Initialize so their addresses never change (the
// fight session keeps raw pointers to them; BindFightSession). All rules live in Core (StoryState.h);
// this reads the registry files, copies the save block and logs.
//
// * The registry comes from the story folder (UFTGameInstance::StoryDir, default Data/Story:
//   story_facts.csv + story_tuning.json, beside the story) at start.
//   With neither file it runs on an empty registry: every write is refused, nothing breaks. One file
//   without the other, or a refused import, also leaves it empty at start, with a warning; a later import
//   (a reload) that fails keeps the registry and state it had.
// * The save block rides in UFTStorySaveGame beside the story JSON (StoryBlock, written by CaptureFrom,
//   read back by ApplyTo). A save from before it, or a block Core refuses, loads as a new game's state
//   (ftcore::storyLoadOrNew), with a warning for a refused one.
// * A new game resets the state (NewGame). With the bridge bound, the state enters a runner scene inside the
//   runner, before the story subsystem reports the scene change, so the autosave on a scene change holds the
//   new scene open; runner and block are taken at one moment and load back together.
// * The scene flow and the facts: an ftcore::StoryBridge ties the runner (UFTStorySubsystem) to the state. It
//   follows the runner's scenes (a new scene commits the open one and begins its own; a restart restarts it; the
//   story's end commits), shows the facts as the runner's `sf.<fact id>` flags and forwards the story's writes
//   to them (StoryBridge.h). The runner's scene ids are the registry's scene codes, unless a scene map
//   (story_scene_map.csv in the same folder: `scene,code`; private, beside the registry) links them. It is bound
//   again after every story load and registry import; a load calls SyncAfterLoad once the block is decoded.
// * -StoryStateCheck: a headless check. It round-trips the block in memory, logs one
//   "StoryState check:" line and asks the engine to quit. It never writes a save slot.

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ftcore/StoryBridge.h"
#include "ftcore/StoryState.h"

#include <vector>

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
	// and by UFTGameInstance::ReloadStory, with the scene map beside it. The files are imported into a new registry
	// first; only when that succeeds is it assigned in place and the state reset to a new game's (its slots and
	// scenes belong to the registry), with a bound fight session unbound first (bind it again after its next Begin).
	// A missing or refused registry keeps the current registry, state and scene map, with a warning.
	bool LoadRegistry(const FString& Dir);
	// True when the registry was imported (false: the empty registry).
	UFUNCTION(BlueprintPure, Category = "Story")
	bool HasRegistry() const { return bRegistryLoaded; }

	// ---- the scene ----

	// Binds the bridge to the story runner and this state (UFTStorySubsystem::LoadStory and LoadRegistry call it).
	// Unbound, with a warning, when the story has a fact flag the registry lacks or fact flags but no mapped scene;
	// unbound when either is not loaded.
	void RebindBridge();
	// After a load: the runner restored and the block decoded (StoryBridge::sync: the runner stays where it was
	// restored, the state follows it, a later restart goes back to the load point).
	void SyncAfterLoad();
	UFUNCTION(BlueprintPure, Category = "Story")
	bool IsBridgeBound() const;
	// The runner scenes that play a registry scene (identity, or the map's links).
	UFUNCTION(BlueprintPure, Category = "Story")
	int32 GetMappedSceneCount() const;
	// The links in the scene map (0: no map, identity).
	UFUNCTION(BlueprintPure, Category = "Story")
	int32 GetSceneMapSize() const { return static_cast<int32>(SceneLinks.size()); }

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

	// Writes the block into Out and keeps a copy as the last good block. When Core refuses, it returns false with a
	// warning naming the last good block's scene and writes that block instead, unless there is none or the story
	// has moved since it (its scene position, or any fact, stamp or wide value: a later beat of the same scene that
	// wrote something counts; a beat that wrote nothing gives the same block): then Out is untouched and *bOutSkip is set (the caller skips the save, so the slot
	// keeps what it has). False with Out untouched before Initialize.
	bool EncodeBlock(TArray<uint8>& Out, bool* bOutSkip = nullptr);
	// Loads the block, or a new game's state when it is empty or Core refuses it. Returns Core's result. A loaded
	// block becomes the last good block; a new game's state clears it.
	ftcore::StoryLoad DecodeBlock(const TArray<uint8>& In);
	// The last block encoded or loaded for this state (empty after a new game or a registry import).
	const TArray<uint8>& GetLastGoodBlock() const { return LastGoodBlock; }

private:
	void Unbind();
	void LoadSceneMap(const FString& Base);
	static void HandleBridgeReport(void* User, const ftcore::StoryBridgeReport& Report);

	TUniquePtr<ftcore::FactRegistry> Registry;
	TUniquePtr<ftcore::StoryState> State;
	bool bRegistryLoaded = false;
	// The fight session last bound (weak: the runner owns it); unbound on a registry import and on Deinitialize.
	TWeakObjectPtr<UFTFightSession> BoundSession;
	// The scene map's links (empty: no map, identity) and the bridge (allocated once in Initialize).
	std::vector<ftcore::StorySceneLink> SceneLinks;
	TUniquePtr<ftcore::StoryBridge> Bridge;
	// The last block that encoded or loaded (EncodeBlock writes it when Core refuses).
	TArray<uint8> LastGoodBlock;
	// The state as LastGoodBlock was taken (EncodeBlock writes that block only while nothing moved since).
	TUniquePtr<ftcore::StoryState> GoodState;
	void KeepGood(const TArray<uint8>& Block);
	bool MovedSinceGood() const;
};
