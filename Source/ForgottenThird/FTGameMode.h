#pragma once

// The default game mode: AFTPawn driven by AFTPlayerController, with the AFTStoryHUD dialogue box.
// Starts the story when play starts if the game instance loaded one (at -StoryScene=<id> when given)
// and spawns the AFTStoryDirector that plays it on screen. Back from a gameplay screen the story is
// already running: only the director is spawned, and it resumes the story.

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "FTGameMode.generated.h"

UCLASS(Config = Game)
class FORGOTTENTHIRD_API AFTGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AFTGameMode();

	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Story")
	bool bStartStoryOnPlay = true;
	// Empty: the story's start scene.
	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Story")
	FString StartScene;

	virtual void StartPlay() override;
};
