#pragma once

// Loads the story (UFTStorySubsystem::LoadStory) from StoryDir when the game instance starts. A
// missing or broken story is logged and the game runs on without one.

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"

#include "ftcore/Dialogue.h"
#include "ftcore/Explore.h"

#include "FTGameInstance.generated.h"

// What the exploration demo hands to its fight and gets back: set by AFTExploreDirector before it
// opens the battle map, read by AFTBattleDemoActor (where to return, whether it won) and by the
// director when the field opens again.
struct FFTExploreCarry
{
	bool bValid = false;
	FString ReturnMap;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	ftcore::ExploreState State;
	int32 Encounter = -1;
	bool bWon = false;
	double Seconds = 0.0; // time already spent in the field, for the completion log
};

// What the story hands to a gameplay screen and gets back: set by AFTStoryDirector before it opens
// the battle or field map for a gameplay beat. The screen sets bDone (and bWon) and opens ReturnMap;
// the director there resumes the story if the runner is still on the same beat.
struct FFTStoryCarry
{
	bool bValid = false;
	FString ReturnMap;
	FString Scene; // scene id and beat index of the gameplay beat
	int32 Beat = -1;
	FString Kind; // gameplay kind as written (CMB, ENG, PLAY)
	ftcore::GameplayScreen Screen = ftcore::GameplayScreen::Box;
	// The step of the beat's ftcore::GameplayPlan this screen plays (0-based) and the plan's step
	// count. The story resumes only after the last step; a single-screen beat is step 0 of 1.
	int32 Step = 0;
	int32 StepCount = 1;
	FString StepLabel; // the step's qualifiers ("duel", "loop, intercut"); empty when none
	bool bAutoPlay = false; // the screen plays itself (a headless hand-off run)
	bool bDone = false;
	bool bWon = false;
};

UCLASS(Config = Game)
class FORGOTTENTHIRD_API UFTGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	// story.json and scenes/<id>.json. Relative paths are relative to the project folder.
	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Story")
	FString StoryDir = TEXT("Data/Story");

	virtual void Init() override;

	// Loads StoryDir again (after editing the story files).
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool ReloadStory();

	FFTExploreCarry ExploreCarry;
	FFTStoryCarry StoryCarry;
};
