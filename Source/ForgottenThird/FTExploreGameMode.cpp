#include "FTExploreGameMode.h"

#include "FTExploreCharacter.h"
#include "FTExploreHUD.h"

AFTExploreGameMode::AFTExploreGameMode()
{
	DefaultPawnClass = AFTExploreCharacter::StaticClass();
	HUDClass = AFTExploreHUD::StaticClass();
}

void AFTExploreGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as AFTBattleGameMode does: no story here.
	AGameModeBase::StartPlay();
}
