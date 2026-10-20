#include "FTGameMode.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "ForgottenThird.h"
#include "FTPawn.h"
#include "FTPlayerController.h"
#include "FTStoryDirector.h"
#include "FTStoryHUD.h"
#include "FTStorySubsystem.h"

AFTGameMode::AFTGameMode()
{
	DefaultPawnClass = AFTPawn::StaticClass();
	PlayerControllerClass = AFTPlayerController::StaticClass();
	HUDClass = AFTStoryHUD::StaticClass();
}

void AFTGameMode::StartPlay()
{
	Super::StartPlay();

	if (!bStartStoryOnPlay)
	{
		return;
	}
	const UGameInstance* GameInstance = GetGameInstance();
	UFTStorySubsystem* Story = GameInstance ? GameInstance->GetSubsystem<UFTStorySubsystem>() : nullptr;
	if (!Story || !Story->IsLoaded() || Story->GetState() != EFTStoryState::Idle)
	{
		return;
	}
	FString Scene = StartScene;
	FParse::Value(FCommandLine::Get(), TEXT("StoryScene="), Scene);
	if (!Story->StartStory(Scene))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: could not start at '%s'."), *Scene);
		return;
	}
	if (!TActorIterator<AFTStoryDirector>(GetWorld()))
	{
		GetWorld()->SpawnActor<AFTStoryDirector>();
	}
}
