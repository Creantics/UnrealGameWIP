#include "FTPauseSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/UObjectGlobals.h"

#include "ForgottenThird.h"
#include "FTAudioSubsystem.h"
#include "FTGameInstance.h"
#include "FTGameMode.h"
#include "FTPlayerController.h"
#include "FTSettingsSubsystem.h"
#include "FTStorySubsystem.h"

namespace
{
// The pause-aware wall clock's books, process-wide so they outlive the game instance and every map:
// the seconds spent paused so far, and when the pause now running began.
double GPausedTotal = 0.0;
double GPausedSince = 0.0;
bool GClockPaused = false;

void BeginClockPause()
{
	if (!GClockPaused)
	{
		GClockPaused = true;
		GPausedSince = FPlatformTime::Seconds();
	}
}

void EndClockPause()
{
	if (GClockPaused)
	{
		GPausedTotal += FPlatformTime::Seconds() - GPausedSince;
		GClockPaused = false;
	}
}
} // namespace

double UFTPauseSubsystem::WallSeconds()
{
	// Paused: the clock reads what it read when the pause began.
	return (GClockPaused ? GPausedSince : FPlatformTime::Seconds()) - GPausedTotal;
}

bool UFTPauseSubsystem::IsHeadlessRun()
{
	static const bool bHeadless = []
	{
		const TCHAR* Cmd = FCommandLine::Get();
		return UFTStorySubsystem::IsHeadlessRun() || FParse::Param(Cmd, TEXT("ExploreAutoPlay")) || FParse::Param(Cmd, TEXT("BattleAutoPlay"))
			|| FParse::Param(Cmd, TEXT("WorldAutoPlay")) || FParse::Param(Cmd, TEXT("OverworldAutoPlay"));
	}();
	return bHeadless;
}

const UFTPauseSubsystem* UFTPauseSubsystem::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UFTPauseSubsystem>() : nullptr;
}

bool UFTPauseSubsystem::IsBlocking(const UObject* WorldContext)
{
	const UFTPauseSubsystem* Pause = Get(WorldContext);
	return Pause && Pause->BlocksInput();
}

bool UFTPauseSubsystem::IsJustResumed(const UObject* WorldContext)
{
	const UFTPauseSubsystem* Pause = Get(WorldContext);
	return Pause && Pause->JustResumed();
}

void UFTPauseSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// The menu reaches all three while it is open, so they start before it and stop after it.
	Collection.InitializeDependency<UFTSettingsSubsystem>();
	Collection.InitializeDependency<UFTStorySubsystem>();
	Collection.InitializeDependency<UFTAudioSubsystem>();
	RowRects.Init(FBox2D(ForceInit), RowCount);
	ChoiceRects.Init(FBox2D(ForceInit), 2);
	EndClockPause(); // a clock left stopped by an earlier game instance (a play-in-editor session) starts again
	FCoreUObjectDelegates::PreLoadMap.AddUObject(this, &UFTPauseSubsystem::HandlePreLoadMap);
}

void UFTPauseSubsystem::Deinitialize()
{
	FCoreUObjectDelegates::PreLoadMap.RemoveAll(this);
	if (bOpen)
	{
		Teardown();
	}
	Super::Deinitialize();
}

void UFTPauseSubsystem::HandlePreLoadMap(const FString& /*MapName*/)
{
	// A new map is loading: whatever the menu froze is undone, and the next screen starts with every item on.
	if (bOpen)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Pause: closed by a map change."));
		Teardown();
	}
	Menu = ftcore::PauseMenu();
}

bool UFTPauseSubsystem::Open(UWorld* World)
{
	if (bOpen || !World || IsHeadlessRun())
	{
		return false;
	}
	UGameInstance* GameInstance = GetGameInstance();
	AFTPlayerController* PC = Cast<AFTPlayerController>(UGameplayStatics::GetPlayerController(World, 0));
	if (!GameInstance || !PC)
	{
		return false;
	}
	// A carry on autoplay is a headless hand-off that started without the command-line flags.
	if (const UFTGameInstance* GI = Cast<UFTGameInstance>(GameInstance))
	{
		if (GI->StoryCarry.bValid && GI->StoryCarry.bAutoPlay)
		{
			return false;
		}
	}
	if (!UGameplayStatics::SetGamePaused(World, true))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Pause: the game could not be paused."));
		return false;
	}

	bOpen = true;
	OpenFrame = GFrameCounter;
	Menu.open();
	LastMouse = FVector2D(-1.0, -1.0);
	PausedWorld = World;
	BeginClockPause();

	// The story's hold timer ticks outside the world's pause: stop it too (a story that runs, not one idle on a screen).
	UFTStorySubsystem* Story = GameInstance->GetSubsystem<UFTStorySubsystem>();
	if (Story && Story->IsLoaded() && Story->GetState() != EFTStoryState::Idle && !Story->IsPaused())
	{
		Story->SetPaused(true);
		bPausedStory = true;
	}
	if (UFTAudioSubsystem* Audio = GameInstance->GetSubsystem<UFTAudioSubsystem>())
	{
		Audio->SetGamePaused(true);
	}

	Controller = PC;
	bBeganMenu = !PC->IsMenuInputActive();
	PC->BeginMenuInput();
	PC->BeginPauseMenuInput();
	PC->OnMenuStep.AddUniqueDynamic(this, &UFTPauseSubsystem::HandleStep);
	PC->OnMenuAdjust.AddUniqueDynamic(this, &UFTPauseSubsystem::HandleAdjust);
	PC->OnAdvance.AddUniqueDynamic(this, &UFTPauseSubsystem::HandlePress);
	PC->OnConfirm.AddUniqueDynamic(this, &UFTPauseSubsystem::HandlePress);
	PC->OnCancel.AddUniqueDynamic(this, &UFTPauseSubsystem::HandleCancel);
	UE_LOG(LogForgottenThird, Log, TEXT("Pause: opened."));
	return true;
}

void UFTPauseSubsystem::Close()
{
	if (!bOpen)
	{
		return;
	}
	Teardown();
	UE_LOG(LogForgottenThird, Log, TEXT("Pause: resumed."));
}

void UFTPauseSubsystem::Teardown()
{
	bOpen = false;
	CloseFrame = GFrameCounter;
	const UGameInstance* GameInstance = GetGameInstance();

	// A settings overlay opened from here goes with it (it would be left drawing over nothing).
	UFTSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
	if (Settings && Settings->IsOpen())
	{
		Settings->Close();
	}

	if (AFTPlayerController* PC = Controller.Get())
	{
		PC->OnMenuStep.RemoveDynamic(this, &UFTPauseSubsystem::HandleStep);
		PC->OnMenuAdjust.RemoveDynamic(this, &UFTPauseSubsystem::HandleAdjust);
		PC->OnAdvance.RemoveDynamic(this, &UFTPauseSubsystem::HandlePress);
		PC->OnConfirm.RemoveDynamic(this, &UFTPauseSubsystem::HandlePress);
		PC->OnCancel.RemoveDynamic(this, &UFTPauseSubsystem::HandleCancel);
		PC->EndPauseMenuInput();
		if (bBeganMenu)
		{
			PC->EndMenuInput();
		}
	}
	Controller.Reset();
	bBeganMenu = false;

	if (UWorld* World = PausedWorld.Get())
	{
		UGameplayStatics::SetGamePaused(World, false);
	}
	PausedWorld.Reset();

	if (bPausedStory)
	{
		if (UFTStorySubsystem* Story = GameInstance ? GameInstance->GetSubsystem<UFTStorySubsystem>() : nullptr)
		{
			Story->SetPaused(false);
		}
		bPausedStory = false;
	}
	if (UFTAudioSubsystem* Audio = GameInstance ? GameInstance->GetSubsystem<UFTAudioSubsystem>() : nullptr)
	{
		Audio->SetGamePaused(false);
	}
	EndClockPause();
}

void UFTPauseSubsystem::PressBack()
{
	if (bOpen)
	{
		Apply(Menu.press(ftcore::PauseKey::Back));
	}
}

void UFTPauseSubsystem::Apply(ftcore::PauseAction Action)
{
	switch (Action)
	{
	case ftcore::PauseAction::Resume:
		Close();
		break;
	case ftcore::PauseAction::OpenSettings:
		OpenSettings();
		break;
	case ftcore::PauseAction::QuitToTitle:
		QuitToTitle();
		break;
	case ftcore::PauseAction::QuitGame:
		QuitGame();
		break;
	case ftcore::PauseAction::None:
	case ftcore::PauseAction::Moved:
	case ftcore::PauseAction::Prompt:
		break;
	}
}

void UFTPauseSubsystem::OpenSettings()
{
	const UGameInstance* GameInstance = GetGameInstance();
	UFTSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
	AFTPlayerController* PC = Controller.Get();
	if (Settings && PC)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Pause: settings."));
		Settings->Open(PC);
	}
}

void UFTPauseSubsystem::QuitToTitle()
{
	UWorld* World = PausedWorld.Get();
	UE_LOG(LogForgottenThird, Log, TEXT("Pause: quit to title."));
	Teardown(); // unpaused first, as the map change would find a paused world

	// The title is on the story's map: the carry's return map when a screen was handed the story (what the
	// battle's own Quit to title opens), else the game mode's TitleMap, else the demo map.
	FString Map = TEXT("Demo");
	if (const AFTGameMode* Mode = World ? World->GetAuthGameMode<AFTGameMode>() : nullptr)
	{
		Map = Mode->TitleMap;
	}
	if (UFTGameInstance* GI = Cast<UFTGameInstance>(GetGameInstance()))
	{
		GI->ExploreCarry = FFTExploreCarry(); // a field fight left half way is over, so the next run starts clean
		if (GI->StoryCarry.bValid && !GI->StoryCarry.ReturnMap.IsEmpty())
		{
			Map = GI->StoryCarry.ReturnMap;
		}
	}
	UGameplayStatics::OpenLevel(World, FName(*Map), true, TEXT("Title"));
}

void UFTPauseSubsystem::QuitGame()
{
	UWorld* World = PausedWorld.Get();
	APlayerController* PC = Controller.Get();
	UE_LOG(LogForgottenThird, Log, TEXT("Pause: quit game."));
	// The menu stays up until the process ends: unpausing first would show a frame of the game running.
	UKismetSystemLibrary::QuitGame(World, PC, EQuitPreference::Quit, false);
}

bool UFTPauseSubsystem::SettingsBlocked() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UFTSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
	return Settings && Settings->BlocksInput();
}

void UFTPauseSubsystem::HandleStep(int32 Step)
{
	if (bOpen && !SettingsBlocked())
	{
		Apply(Menu.press(Step < 0 ? ftcore::PauseKey::Up : ftcore::PauseKey::Down));
	}
}

void UFTPauseSubsystem::HandleAdjust(int32 Direction)
{
	// Left / Right only mean something to the quit prompt, where either one flips Yes and No.
	if (bOpen && !SettingsBlocked() && Menu.confirming())
	{
		Apply(Menu.press(Direction < 0 ? ftcore::PauseKey::Up : ftcore::PauseKey::Down));
	}
}

void UFTPauseSubsystem::HandlePress()
{
	// The press that opened the menu is still being broadcast: ignore it. So is one the settings overlay owns.
	if (!bOpen || OpenFrame == GFrameCounter || SettingsBlocked())
	{
		return;
	}
	const AFTPlayerController* PC = Controller.Get();
	float MouseX = 0.f;
	float MouseY = 0.f;
	if (PC && PC->WasInputKeyJustPressed(EKeys::LeftMouseButton))
	{
		// A click: pick what is under the cursor, nothing when it is outside every row (or on a greyed one).
		if (!PC->GetMousePosition(MouseX, MouseY))
		{
			return;
		}
		const FVector2D Mouse(MouseX, MouseY);
		if (Menu.confirming())
		{
			const int32 Choice = HitChoice(Mouse);
			if (Choice < 0)
			{
				return;
			}
			Menu.hoverChoice(Choice == 0);
		}
		else
		{
			const int32 Row = HitRow(Mouse);
			if (Row < 0)
			{
				return;
			}
			const ftcore::PauseItem Item = static_cast<ftcore::PauseItem>(Row);
			Menu.hover(Item);
			if (Menu.selected() != Item)
			{
				return;
			}
		}
	}
	Apply(Menu.press(ftcore::PauseKey::Confirm));
}

void UFTPauseSubsystem::HandleCancel()
{
	// Backspace and pad B. Esc and pad Start arrive through AFTPlayerController, which calls PressBack.
	if (bOpen && OpenFrame != GFrameCounter && !SettingsBlocked())
	{
		PressBack();
	}
}

int32 UFTPauseSubsystem::HitRow(const FVector2D& Point) const
{
	for (int32 I = 0; I < RowRects.Num(); ++I)
	{
		if (RowRects[I].IsInside(Point))
		{
			return I;
		}
	}
	return -1;
}

int32 UFTPauseSubsystem::HitChoice(const FVector2D& Point) const
{
	for (int32 I = 0; I < ChoiceRects.Num(); ++I)
	{
		if (ChoiceRects[I].IsInside(Point))
		{
			return I;
		}
	}
	return -1;
}

void UFTPauseSubsystem::UpdateHover(const FVector2D& Mouse)
{
	if (Mouse == LastMouse)
	{
		return;
	}
	LastMouse = Mouse;
	if (Menu.confirming())
	{
		const int32 Choice = HitChoice(Mouse);
		if (Choice >= 0)
		{
			Menu.hoverChoice(Choice == 0);
		}
		return;
	}
	const int32 Row = HitRow(Mouse);
	if (Row >= 0)
	{
		Menu.hover(static_cast<ftcore::PauseItem>(Row));
	}
}
