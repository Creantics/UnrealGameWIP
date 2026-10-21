#include "FTBattleGameMode.h"

#include "FTBattleHUD.h"

AFTBattleGameMode::AFTBattleGameMode()
{
	HUDClass = AFTBattleHUD::StaticClass();
	bHostsStory = false;
}

void AFTBattleGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose: the story must not start here, and
	// bStartStoryOnPlay is a config value that can't be relied on.
	AGameModeBase::StartPlay();
}
