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

	// Imports the registry from Dir (relative: to the project folder). Called by Initialize with
	// Data/Story; call it again only before any fight session is bound (the registry is assigned in place).
	bool LoadRegistry(const FString& Dir);
	// True when the registry was imported (false: the empty registry).
	UFUNCTION(BlueprintPure, Category = "Story")
	bool HasRegistry() const { return bRegistryLoaded; }

	// ---- the scene ----

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

	// Writes the block. False (Out emptied) before Initialize.
	bool EncodeBlock(TArray<uint8>& Out);
	// Loads the block, or a new game's state when it is empty or Core refuses it. Returns Core's result.
	ftcore::StoryLoad DecodeBlock(const TArray<uint8>& In);

private:
	TUniquePtr<ftcore::FactRegistry> Registry;
	TUniquePtr<ftcore::StoryState> State;
	bool bRegistryLoaded = false;
};
