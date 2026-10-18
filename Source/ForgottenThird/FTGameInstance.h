#pragma once

// Loads the story (UFTStorySubsystem::LoadStory) from StoryDir when the game instance starts. A
// missing or broken story is logged and the game runs on without one.

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"

#include "FTGameInstance.generated.h"

UCLASS(Config = Game)
class FORGOTTENTHIRD_API UFTGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	// story.json and scenes/<id>.json. Relative paths are relative to the project folder.
	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Story")
	FString StoryDir = TEXT("Data/story");

	virtual void Init() override;

	// Loads StoryDir again (after editing the story files).
	UFUNCTION(BlueprintCallable, Category = "Story")
	bool ReloadStory();
};
