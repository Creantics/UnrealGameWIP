#pragma once

// Loads the story (UFTStorySubsystem::LoadStory) from StoryDir when the game instance starts. A
// missing or broken story is logged and the game runs on without one.

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"

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
};
