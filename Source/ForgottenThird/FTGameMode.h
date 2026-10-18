#pragma once

// The default game mode: AFTPawn driven by AFTPlayerController. Starts the story when play starts
// if the game instance loaded one.

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
