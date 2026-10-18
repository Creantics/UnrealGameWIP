#pragma once

// The story for one game instance: a StoryDatabase loaded from the story JSON (story.json plus
// scenes/<id>.json) and the StoryRunner that walks it. All story rules live in Core (ftcore); this
// only loads files, converts text to FString and turns the runner's state changes into events.
//
// Cue events are queued while the runner moves and broadcast after it, so a handler may call
// Advance / Choose itself. OnBeatChanged / OnSceneChanged fire once per new beat / scene.

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"

#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"

#include "FTStorySubsystem.generated.h"

UENUM(BlueprintType)
enum class EFTStoryState : uint8
{
	Idle,
	Line,
	Direction,
	Choice,
	Hold,
	Gameplay,
	Finished
};

UENUM(BlueprintType)
enum class EFTCueAction : uint8
{
	Play,
	Stop,
	Stinger
};

UENUM(BlueprintType)
enum class EFTFlagKind : uint8
{
	Bool,
	Int,
	String
};

USTRUCT(BlueprintType)
struct FFTStoryFlagSave
{
	GENERATED_BODY()

	UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Story")
	FString Id;
	UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Story")
	EFTFlagKind Kind = EFTFlagKind::Bool;
	// Bool: 0 / 1. Int: the value.
	UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Story")
	int64 Number = 0;
	// String: the value's text.
	UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Story")
	FString Text;
};

// Story progress for a SaveGame: the cursor and every flag that differs from its default.
USTRUCT(BlueprintType)
struct FFTStorySave
{
	GENERATED_BODY()

	UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Story")
	FString Scene;
	UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Story")
	int32 Beat = 0;
	UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Story")
	TArray<FFTStoryFlagSave> Flags;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFTStoryCueEvent, const FString&, CueId, const FString&, Kind, EFTCueAction, Action);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTStoryStateEvent, EFTStoryState, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFTStorySceneEvent, const FString&, SceneId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFTStoryFinishedEvent);

UCLASS()
class FORGOTTENTHIRD_API UFTStorySubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	UFTStorySubsystem();
	virtual ~UFTStorySubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---- loading ----

	// Loads story.json and every scenes/<id>.json it lists from Dir (relative paths are relative to
	// the project folder). On failure the previous story stays loaded and OutError says why.
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool LoadStory(const FString& Dir, FString& OutError, TArray<FString>& OutWarnings);
	UFUNCTION(BlueprintPure, Category = "Story")
	bool IsLoaded() const { return bLoaded; }

	// ---- moving ----

	// Resets the flags and starts at SceneId (empty: the story's start scene).
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool StartStory(const FString& SceneId);
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool Advance();
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool Choose(int32 Option);
	// Gameplay beat done: continue the story.
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool ResumeAfterGameplay();
	UFUNCTION(BlueprintCallable, Category = "Story")
	void SkipHold();
	UFUNCTION(BlueprintCallable, Category = "Story")
	void StopStory();

	// ---- current beat ----

	UFUNCTION(BlueprintPure, Category = "Story")
	EFTStoryState GetState() const;
	UFUNCTION(BlueprintPure, Category = "Story")
	FString GetSceneId() const;
	UFUNCTION(BlueprintPure, Category = "Story")
	int32 GetBeatIndex() const;
	UFUNCTION(BlueprintPure, Category = "Story")
	FString GetText() const;
	// Localisation key of GetText() (empty when none).
	UFUNCTION(BlueprintPure, Category = "Story")
	FString GetTextKey() const;
	UFUNCTION(BlueprintPure, Category = "Story")
	FString GetSpeaker() const;
	// Delivery tag of a line.
	UFUNCTION(BlueprintPure, Category = "Story")
	FString GetDirection() const;
	// VO line id (matches the voice file name).
	UFUNCTION(BlueprintPure, Category = "Story")
	FString GetLineId() const;
	UFUNCTION(BlueprintPure, Category = "Story")
	int32 GetOptionCount() const;
	UFUNCTION(BlueprintPure, Category = "Story")
	FString GetOptionText(int32 Option) const;
	UFUNCTION(BlueprintPure, Category = "Story")
	FString GetOptionTextKey(int32 Option) const;
	UFUNCTION(BlueprintPure, Category = "Story")
	bool IsOptionSelectable(int32 Option) const;
	// The option autoplay takes (-1 when not on a choice).
	UFUNCTION(BlueprintPure, Category = "Story")
	int32 GetDefaultOption() const;
	UFUNCTION(BlueprintPure, Category = "Story")
	float GetHoldRemaining() const;
	// Gameplay kind as written in the script (CMB, ENG, PLAY, MINI, ...).
	UFUNCTION(BlueprintPure, Category = "Story")
	FString GetGameplayKind() const;
	UFUNCTION(BlueprintPure, Category = "Story")
	FString GetGameplayParam(const FString& Key) const;

	// ---- flags ----

	// "true" / "false", an integer, or a string value; empty when the flag is unknown.
	UFUNCTION(BlueprintPure, Category = "Story")
	FString GetFlag(const FString& Id) const;
	// Value as GetFlag writes it. False when the flag or the value is unknown.
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool SetFlag(const FString& Id, const FString& Value);

	// ---- saves ----

	// False when the story isn't on a scene.
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool CaptureSave(FFTStorySave& OutSave) const;
	// False when the saved scene no longer exists (the story is then stopped). OutSkippedFlags
	// lists saved flags the script no longer has.
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool ApplySave(const FFTStorySave& Save, TArray<FString>& OutSkippedFlags);

	// ---- events ----

	UPROPERTY(BlueprintAssignable, Category = "Story")
	FFTStoryCueEvent OnCue;
	// The cursor rests on a new beat (also when a choice is shown again).
	UPROPERTY(BlueprintAssignable, Category = "Story")
	FFTStoryStateEvent OnBeatChanged;
	UPROPERTY(BlueprintAssignable, Category = "Story")
	FFTStorySceneEvent OnSceneChanged;
	UPROPERTY(BlueprintAssignable, Category = "Story")
	FFTStoryFinishedEvent OnFinished;

	// Native access for C++ gameplay code.
	const ftcore::StoryRunner& GetRunner() const { return *Runner; }
	const ftcore::StoryDatabase& GetDatabase() const { return *Database; }

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override;

private:
	// Views into the database, which outlives the queue.
	struct FQueuedCue
	{
		std::string_view Id;
		std::string_view Kind;
		ftcore::CueAction Action = ftcore::CueAction::Play;
	};

	static void HandleCue(void* User, const ftcore::CueEvent& Event);
	// Broadcasts queued cues and beat / scene changes. Called after every move.
	void Flush();

	TUniquePtr<ftcore::StoryDatabase> Database;
	TUniquePtr<ftcore::StoryRunner> Runner;
	TArray<FQueuedCue> PendingCues;
	uint32 SeenSceneEpoch = 0;
	uint32 SeenBeatEpoch = 0;
	bool bLoaded = false;
	bool bFlushing = false;
	bool bReportedFinished = false;
};
