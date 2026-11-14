#include "FTCutsceneGameMode.h"

#include "EngineUtils.h"
#include "FTCutsceneDirector.h"
#include "FTCutsceneHUD.h"
#include "FTGameInstance.h"

AFTCutsceneGameMode::AFTCutsceneGameMode()
{
	HUDClass = AFTCutsceneHUD::StaticClass();
	bHostsStory = false;
}

void AFTCutsceneGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as the stub, battle and exploration modes do: the story
	// must not start here. This also begins play for the map's actors (the director among them).
	AGameModeBase::StartPlay();

	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	bHasStep = GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AFTCutsceneDirector> It(World); It; ++It)
	{
		Director = *It;
		break;
	}
	if (!Director)
	{
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Director = World->SpawnActor<AFTCutsceneDirector>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	}
	if (!Director)
	{
		return;
	}
	// On the stage, and not this map's step host: a CG step after a CG step loads the map again.
	Director->HostSteps(false, false);
	Director->StartStepHere();
}
