#include "FTStubGameMode.h"

#include "FTStubHUD.h"

AFTStubGameMode::AFTStubGameMode()
{
	HUDClass = AFTStubHUD::StaticClass();
}

void AFTStubGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as the battle and exploration modes do: no story here.
	AGameModeBase::StartPlay();
}
