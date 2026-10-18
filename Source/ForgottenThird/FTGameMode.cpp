#include "FTGameMode.h"

#include "Engine/GameInstance.h"
#include "FTPawn.h"
#include "FTPlayerController.h"
#include "FTStorySubsystem.h"

AFTGameMode::AFTGameMode()
{
	DefaultPawnClass = AFTPawn::StaticClass();
	PlayerControllerClass = AFTPlayerController::StaticClass();
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
	if (Story && Story->IsLoaded() && Story->GetState() == EFTStoryState::Idle)
	{
		Story->StartStory(StartScene);
	}
}
