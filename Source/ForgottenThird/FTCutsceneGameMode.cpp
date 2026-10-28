#include "FTCutsceneGameMode.h"

#include "EngineUtils.h"
#include "FTCutsceneDirector.h"
#include "FTCutsceneHUD.h"
#include "FTGameInstance.h"
#include "FTText.h"

#include "ftcore/StubScreen.h"

namespace
{
const FString kNoShotCutscene;
} // namespace

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
	const FFTStoryCarry* Carry = (GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone) ? &GI->StoryCarry : nullptr;
	bHasStep = Carry != nullptr;
	if (Carry)
	{
		// The title comes from Core, whatever the step's token is (CG: "Cutscene").
		const FString& Token = Carry->StepToken.IsEmpty() ? Carry->Kind : Carry->StepToken;
		TitleText = FT::ToFString(ftcore::stubTitle(FT::ToUtf8(Token)));
		StepText = FString::Printf(TEXT("Step %d of %d"), Carry->Step + 1, FMath::Max(Carry->StepCount, 1));
	}
	else
	{
		TitleText = FT::ToFString(ftcore::stubTitle("CG"));
		StepText = TEXT("No story step (preview of the default shots)");
	}

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

	const uint32 Count = Director->GetPlayer().shotCount();
	ShotTexts.Reset(static_cast<int32>(Count));
	for (uint32 I = 0; I < Count; ++I)
	{
		ShotTexts.Add(FString::Printf(TEXT("Shot %u of %u"), I + 1, Count));
	}
}

const FString& AFTCutsceneGameMode::GetShotText() const
{
	if (!Director || !Director->IsStarted())
	{
		return kNoShotCutscene;
	}
	const int32 Index = static_cast<int32>(Director->GetPlayer().currentIndex());
	return ShotTexts.IsValidIndex(Index) ? ShotTexts[Index] : kNoShotCutscene;
}
