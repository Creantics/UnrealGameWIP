#include "FTGameMode.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Crc.h"
#include "Misc/Parse.h"
#include "TimerManager.h"

#include "ForgottenThird.h"
#include "FTGameInstance.h"
#include "FTNpcSubsystem.h"
#include "FTPauseSubsystem.h"
#include "FTPawn.h"
#include "FTPlayerController.h"
#include "FTSettingsSubsystem.h"
#include "FTStepHost.h"
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
		PC->EndPauseInput(); // the title has no pause: Start confirms here
		PC->OnMenuStep.AddUniqueDynamic(this, &AFTGameMode::HandleMenuStep);
		PC->OnAdvance.AddUniqueDynamic(this, &AFTGameMode::HandleMenuPress);
		PC->OnConfirm.AddUniqueDynamic(this, &AFTGameMode::HandleMenuPress);
		PC->OnChoose.AddUniqueDynamic(this, &AFTGameMode::HandleMenuChoose);
		PC->BeginMenuInput();
	}

#if !UE_BUILD_SHIPPING
	// Test runs: -SaveRunTest=new|continue picks New Game or Continue by itself 3 s after the title first shows, and the
	// game quits 5 s later, logging what NPC memory holds, so a save/load run needs no input (the autosave and the load
	// log their NPC block). -SaveRunThen=new|autosave|slice acts 1 s after the pick: a New Game over the running game
	// (StartStory, as the title's New Game does), an autosave, or the NPC slice's scripted day and then an autosave
	// (-NpcSlice places its three NPCs), so a run can put a New Game or a save inside a held NPC block's window.
	static bool bSaveRunArmed = false;
	FString SaveRunPick;
	if (!bSaveRunArmed && FParse::Value(FCommandLine::Get(), TEXT("SaveRunTest="), SaveRunPick))
	{
		bSaveRunArmed = true;
		if (SaveRunPick != TEXT("new") && SaveRunPick != TEXT("continue"))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("SAVERUN -SaveRunTest=%s: want new or continue."), *SaveRunPick);
			return;
		}
		FString SaveRunThen;
		FParse::Value(FCommandLine::Get(), TEXT("SaveRunThen="), SaveRunThen);
		if (!SaveRunThen.IsEmpty() && SaveRunThen != TEXT("new") && SaveRunThen != TEXT("autosave") && SaveRunThen != TEXT("slice"))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("SAVERUN -SaveRunThen=%s: want new, autosave or slice."), *SaveRunThen);
			return;
		}
		const int32 Option = SaveRunPick == TEXT("continue") ? 1 : 0; // the title's items: New Game, Continue, ...
		FTimerHandle PickTimer;
		GetWorldTimerManager().SetTimer(PickTimer, FTimerDelegate::CreateWeakLambda(this, [this, Option, SaveRunThen]()
		{
			UE_LOG(LogForgottenThird, Log, TEXT("SAVERUN pick %s."), Option == 1 ? TEXT("Continue") : TEXT("New Game"));
			HandleMenuChoose(Option);
			if (SaveRunThen.IsEmpty())
			{
				return;
			}
			FTimerHandle ThenTimer;
			GetWorldTimerManager().SetTimer(ThenTimer, FTimerDelegate::CreateWeakLambda(this, [this, SaveRunThen]()
			{
				UFTStorySubsystem* Story = GetStory();
				if (!Story)
				{
					return;
				}
				if (bTitle)
				{
					// The pick didn't leave the title (no save to continue, say): the run proves nothing.
					UE_LOG(LogForgottenThird, Warning, TEXT("SAVERUN then %s skipped: still on the title."), *SaveRunThen);
					return;
				}
				UE_LOG(LogForgottenThird, Log, TEXT("SAVERUN then %s."), *SaveRunThen);
				if (SaveRunThen == TEXT("new"))
				{
					if (!Story->StartStory(StartScene))
					{
						UE_LOG(LogForgottenThird, Warning, TEXT("SAVERUN then new: could not start at '%s'."), *StartScene);
					}
					return;
				}
				if (SaveRunThen == TEXT("slice"))
				{
					UFTNpcWorldSubsystem* Slice = GetWorld() ? GetWorld()->GetSubsystem<UFTNpcWorldSubsystem>() : nullptr;
					if (!Slice)
					{
						UE_LOG(LogForgottenThird, Warning, TEXT("SAVERUN then slice: no NPC slice here (-NpcSlice)."));
						return;
					}
					Slice->RunSliceTest();
				}
				Story->Autosave();
			}), 1.f, false);
		}), 3.f, false);
		if (UGameInstance* GI = GetGameInstance())
		{
			FTimerHandle QuitTimer;
			GI->GetTimerManager().SetTimer(QuitTimer, FTimerDelegate::CreateWeakLambda(GI, [GI]()
			{
				const UFTNpcSubsystem* Npcs = GI->GetSubsystem<UFTNpcSubsystem>();
				const bool bReady = Npcs && Npcs->IsReady();
				const FString Captured = bReady ? Npcs->CaptureSaveJson() : FString();
				UE_LOG(LogForgottenThird, Log, TEXT("SAVERUN quit: NPC memory %s, captures %d chars (crc %08x)."), bReady ? TEXT("ready") : TEXT("off"),
					Captured.Len(), FCrc::StrCrc32(*Captured));
				FTRequestExit(0, TEXT("SaveRunTest"));
			}), 8.f, false);
		}
	}
#endif
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
	// The number keys 1-4, in item order: New Game, Continue, Settings, Quit.
	static const ftcore::TitleKey Picks[ftcore::TitleMenu::kItemCount] = {
		ftcore::TitleKey::Pick1, ftcore::TitleKey::Pick2, ftcore::TitleKey::Pick3, ftcore::TitleKey::Pick4};
	if (bTitle && !SettingsBlocked() && Option >= 0 && Option < ftcore::TitleMenu::kItemCount)
	{
		ApplyTitleAction(Menu.press(Picks[Option]));
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
	else if (Action == ftcore::TitleAction::Quit)
	{
		// No confirm step: nothing is open on the title, and the story autosaves at every scene start.
		UE_LOG(LogForgottenThird, Log, TEXT("Title: Quit."));
		UKismetSystemLibrary::QuitGame(this, GetPlayer(), EQuitPreference::Quit, false);
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
		PC->BeginPauseInput(); // from here on Esc and Start pause (not in a headless run)
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
	// The pause menu owns the keys while it is up (and for the rest of the frame it closed in).
	if (bEndCard && !bReturning && GetWorld()->GetTimeSeconds() - EndCardTime >= EndCardDelay && !UFTPauseSubsystem::IsBlocking(this))
	{
		ReturnToTitle();
	}
}

void AFTGameMode::ReturnToTitle()
{
	bReturning = true;
	UE_LOG(LogForgottenThird, Log, TEXT("Title: back to the title."));
	FTStep::OpenMap(this, TitleMap, TEXT("Title"));
}
