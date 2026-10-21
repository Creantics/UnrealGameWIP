#include "FTGameMode.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "ForgottenThird.h"
#include "FTGameInstance.h"
#include "FTPawn.h"
#include "FTPlayerController.h"
#include "FTSettingsSubsystem.h"
#include "FTStoryDirector.h"
#include "FTStoryHUD.h"
#include "FTStorySubsystem.h"
#include "FTTitleHUD.h"

AFTGameMode::AFTGameMode()
{
	DefaultPawnClass = AFTPawn::StaticClass();
	PlayerControllerClass = AFTPlayerController::StaticClass();
	HUDClass = AFTStoryHUD::StaticClass();
}

UFTStorySubsystem* AFTGameMode::GetStory() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UFTStorySubsystem>() : nullptr;
}

AFTPlayerController* AFTGameMode::GetPlayer() const
{
	return Cast<AFTPlayerController>(GetWorld()->GetFirstPlayerController());
}

void AFTGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	// Decided here, before the player controller exists, because the HUD class is read when it spawns.
	bTitle = false;
	bHeadless = UFTStorySubsystem::IsHeadlessRun();
	const UFTStorySubsystem* Story = GetStory();
	if (!bHostsStory || !bStartStoryOnPlay || bHeadless || !Story || !Story->IsLoaded())
	{
		return;
	}
	const UFTGameInstance* GI = Cast<UFTGameInstance>(GetGameInstance());
	const bool bHandoffPending = GI && GI->StoryCarry.bValid;
	const bool bFresh = Story->GetState() == EFTStoryState::Idle && !bHandoffPending;
	if (UGameplayStatics::HasOption(Options, TEXT("Title")) || bFresh)
	{
		bTitle = true;
		HUDClass = AFTTitleHUD::StaticClass();
	}
}

void AFTGameMode::StartPlay()
{
	Super::StartPlay();

	if (!bStartStoryOnPlay)
	{
		return;
	}
	UFTStorySubsystem* Story = GetStory();
	if (!Story || !Story->IsLoaded())
	{
		return;
	}
	if (bTitle)
	{
		EnterTitle(*Story);
		return;
	}

	// Not idle: the story is already running (back from a gameplay screen); just show it again.
	if (Story->GetState() == EFTStoryState::Idle)
	{
		FString Scene = StartScene;
		FParse::Value(FCommandLine::Get(), TEXT("StoryScene="), Scene);
		if (FParse::Param(FCommandLine::Get(), TEXT("AlphaAutoPlay")))
		{
			UE_LOG(LogForgottenThird, Log, TEXT("Title: New Game (autoplay)."));
		}
		if (!Story->StartStory(Scene))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Story: could not start at '%s'."), *Scene);
			return;
		}
	}
	SpawnDirector();
	WatchForEnd();
}

void AFTGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UFTStorySubsystem* Story = GetStory())
	{
		Story->OnFinished.RemoveDynamic(this, &AFTGameMode::HandleStoryFinished);
	}
	Super::EndPlay(Reason);
}

// ---- title ----

void AFTGameMode::EnterTitle(UFTStorySubsystem& Story)
{
	// Back to the title from a screen or the end card: nothing of the old run stays.
	if (UFTGameInstance* GI = Cast<UFTGameInstance>(GetGameInstance()))
	{
		GI->StoryCarry = FFTStoryCarry();
	}
	if (Story.GetState() != EFTStoryState::Idle)
	{
		Story.StopStory();
	}

	SaveStatus = Story.CheckSlot();
	Menu = ftcore::TitleMenu();
	Menu.setContinueEnabled(SaveStatus == ftcore::SaveRefusal::None);
	UE_LOG(LogForgottenThird, Log, TEXT("Title: shown, continue %s (%hs)."),
		Menu.continueEnabled() ? TEXT("on") : TEXT("off"), ftcore::saveRefusalName(SaveStatus));

	if (AFTPlayerController* PC = GetPlayer())
	{
		PC->bRouteInputToStory = false;
		PC->OnMenuStep.AddUniqueDynamic(this, &AFTGameMode::HandleMenuStep);
		PC->OnAdvance.AddUniqueDynamic(this, &AFTGameMode::HandleMenuPress);
		PC->OnConfirm.AddUniqueDynamic(this, &AFTGameMode::HandleMenuPress);
		PC->OnChoose.AddUniqueDynamic(this, &AFTGameMode::HandleMenuChoose);
		PC->BeginMenuInput();
	}
}

bool AFTGameMode::SettingsBlocked() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UFTSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
	return Settings && Settings->BlocksInput();
}

void AFTGameMode::OpenSettings()
{
	const UGameInstance* GameInstance = GetGameInstance();
	UFTSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
	AFTPlayerController* PC = GetPlayer();
	if (Settings && PC)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Title: Settings."));
		Settings->Open(PC);
	}
}

void AFTGameMode::HandleMenuStep(int32 Step)
{
	if (bTitle && !SettingsBlocked())
	{
		Menu.press(Step < 0 ? ftcore::TitleKey::Up : ftcore::TitleKey::Down);
	}
}

void AFTGameMode::HandleMenuPress()
{
	if (bTitle && !SettingsBlocked())
	{
		ApplyTitleAction(Menu.press(ftcore::TitleKey::Confirm));
	}
}

void AFTGameMode::HandleMenuChoose(int32 Option)
{
	if (bTitle && !SettingsBlocked() && Option >= 0 && Option < ftcore::TitleMenu::kItemCount)
	{
		ApplyTitleAction(Menu.press(Option == 0 ? ftcore::TitleKey::Pick1 : Option == 1 ? ftcore::TitleKey::Pick2 : ftcore::TitleKey::Pick3));
	}
}

void AFTGameMode::ApplyTitleAction(ftcore::TitleAction Action)
{
	UFTStorySubsystem* Story = GetStory();
	if (!Story || !bTitle)
	{
		return;
	}
	if (Action == ftcore::TitleAction::NewGame)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Title: New Game."));
		Story->AdoptSeenFromSlot(); // a replay keeps what the player has read (before the first autosave overwrites the slot)
		if (!Story->StartStory(StartScene))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Story: could not start at '%s'."), *StartScene);
			return;
		}
		EnterStory();
	}
	else if (Action == ftcore::TitleAction::Settings)
	{
		OpenSettings();
	}
	else if (Action == ftcore::TitleAction::Continue)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Title: Continue."));
		SaveStatus = Story->LoadSlot();
		if (SaveStatus != ftcore::SaveRefusal::None)
		{
			Menu.setContinueEnabled(false); // refused: grey it out and stay on the title
			return;
		}
		EnterStory();
	}
}

void AFTGameMode::EnterStory()
{
	bTitle = false;
	if (AFTPlayerController* PC = GetPlayer())
	{
		PC->OnMenuStep.RemoveDynamic(this, &AFTGameMode::HandleMenuStep);
		PC->OnAdvance.RemoveDynamic(this, &AFTGameMode::HandleMenuPress);
		PC->OnConfirm.RemoveDynamic(this, &AFTGameMode::HandleMenuPress);
		PC->OnChoose.RemoveDynamic(this, &AFTGameMode::HandleMenuChoose);
		PC->EndMenuInput();
		PC->ClientSetHUD(AFTStoryHUD::StaticClass()); // replaces the title HUD
	}
	SpawnDirector();
	WatchForEnd();
}

void AFTGameMode::SpawnDirector()
{
	if (!TActorIterator<AFTStoryDirector>(GetWorld()))
	{
		GetWorld()->SpawnActor<AFTStoryDirector>();
	}
}

// ---- end card ----

void AFTGameMode::WatchForEnd()
{
	// Headless runs keep the director's own ending (-StoryQuitOnFinish quits).
	if (bWatchingEnd || bHeadless || FParse::Param(FCommandLine::Get(), TEXT("StoryQuitOnFinish")))
	{
		return;
	}
	UFTStorySubsystem* Story = GetStory();
	AFTPlayerController* PC = GetPlayer();
	if (!Story || !PC)
	{
		return;
	}
	bWatchingEnd = true;
	Story->OnFinished.AddUniqueDynamic(this, &AFTGameMode::HandleStoryFinished);
	PC->OnAdvance.AddUniqueDynamic(this, &AFTGameMode::HandleEndPress);
	PC->OnConfirm.AddUniqueDynamic(this, &AFTGameMode::HandleEndPress);
}

void AFTGameMode::HandleStoryFinished()
{
	bEndCard = true;
	EndCardTime = GetWorld()->GetTimeSeconds();
	UE_LOG(LogForgottenThird, Log, TEXT("Title: end card up."));
}

void AFTGameMode::HandleEndPress()
{
	if (bEndCard && !bReturning && GetWorld()->GetTimeSeconds() - EndCardTime >= EndCardDelay)
	{
		ReturnToTitle();
	}
}

void AFTGameMode::ReturnToTitle()
{
	bReturning = true;
	UE_LOG(LogForgottenThird, Log, TEXT("Title: back to the title."));
	UGameplayStatics::OpenLevel(this, FName(*TitleMap), true, TEXT("Title"));
}
