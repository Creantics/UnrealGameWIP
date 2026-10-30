#include "FTMiniGameMode.h"

#include "Camera/CameraActor.h"
#include "EngineUtils.h"
#include "FTGameInstance.h"
#include "FTMiniHUD.h"
#include "FTPauseSubsystem.h"
#include "FTStepHost.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ftcore/StubScreen.h"

#include <string>
#include <vector>

namespace
{
bool AlphaFastMini()
{
	return FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
}

// The four directions in ftcore::Arrow order (Left, Down, Up, Right), each with the keys
// AFTPlayerController maps for the battle lanes: arrow keys, WASD, the pad d-pad and the pad's face
// buttons (X, A, Y, B).
constexpr int32 kDirectionsMini = 4;
constexpr int32 kKeysPerDirectionMini = 4;

const FKey& MiniKey(int32 Direction, int32 Which)
{
	static const FKey Keys[kDirectionsMini][kKeysPerDirectionMini] = {
		{EKeys::Left, EKeys::A, EKeys::Gamepad_DPad_Left, EKeys::Gamepad_FaceButton_Left},
		{EKeys::Down, EKeys::S, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_FaceButton_Bottom},
		{EKeys::Up, EKeys::W, EKeys::Gamepad_DPad_Up, EKeys::Gamepad_FaceButton_Top},
		{EKeys::Right, EKeys::D, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_FaceButton_Right},
	};
	return Keys[Direction][Which];
}
} // namespace

AFTMiniGameMode::AFTMiniGameMode()
{
	HUDClass = AFTMiniHUD::StaticClass();
	bHostsStory = false;
	PrimaryActorTick.bCanEverTick = true;
}

void AFTMiniGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as the stub, choice, battle and exploration modes do:
	// the story must not start here.
	AGameModeBase::StartPlay();

	bFast = AlphaFastMini();
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const FFTStoryCarry* Carry = (GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone) ? &GI->StoryCarry : nullptr;
	bHasStep = Carry != nullptr;

	ftcore::MiniDef Def = ftcore::defaultMiniDef();
	if (Carry && LoadTable())
	{
		Def = (Carry->Beat >= 0 && Carry->Step >= 0)
			? Table.find(FT::ToUtf8(Carry->Scene), static_cast<uint32>(Carry->Beat), static_cast<uint32>(Carry->Step))
			: Table.fallback;
	}
	Screen.begin(bHasStep, Def, bHasStep && Carry->bAutoPlay);
	if (Carry)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Mini: round at %s beat %d step %d: steps %d lanes %d bpm %.0f tries %d."), *Carry->Scene,
			Carry->Beat, Carry->Step, Def.steps, Def.lanes, Def.bpm, Def.tries);
	}

	// Look through the map's camera rather than the pawn.
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		for (TActorIterator<ACameraActor> It(GetWorld()); It; ++It)
		{
			PC->SetViewTarget(*It);
			break;
		}
	}
}

bool AFTMiniGameMode::LoadTable()
{
	const FString File = FPaths::ProjectDir() / TEXT("Data/Mini/mini.json");
	if (!IFileManager::Get().FileExists(*File))
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Mini: no Data/Mini/mini.json; using the built-in round."));
		return false;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Mini: couldn't read Data/Mini/mini.json; using the built-in round."));
		return false;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ftcore::importMiniTable(FT::BytesAsText(Bytes), Table, &Error, &Warnings))
	{
		// The loader's errors name paths and keys only.
		UE_LOG(LogForgottenThird, Warning, TEXT("Mini: Data/Mini/mini.json refused; using the built-in round:\n%s"), *FT::ToFString(Error));
		return false;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Mini: %s"), *FT::ToFString(W));
	}
	return true;
}

void AFTMiniGameMode::PollKeys()
{
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	// The frame after the pause menu closed, a key still held (pad A that picked Resume is also "down" here)
	// is not a press of the screen's own: it counts once it is let go and pressed again.
	const bool bResumed = UFTPauseSubsystem::IsJustResumed(this);
	for (int32 D = 0; D < kDirectionsMini; ++D)
	{
		bool bPressed = false;
		bool bNowDown = false;
		for (int32 K = 0; K < kKeysPerDirectionMini; ++K)
		{
			const FKey& Key = MiniKey(D, K);
			bPressed |= PC->WasInputKeyJustPressed(Key);
			bNowDown |= PC->IsInputKeyDown(Key);
		}
		if (!bResumed && (bPressed || (bNowDown && !bDown[D])))
		{
			Screen.press(static_cast<ftcore::Arrow>(D));
		}
		bDown[D] = bNowDown;
	}
}

void AFTMiniGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bHasStep || bReturning)
	{
		return;
	}
	const AWorldSettings* Settings = GetWorld() ? GetWorld()->GetWorldSettings() : nullptr;
	const double Dilation = Settings ? Settings->GetEffectiveTimeDilation() : 1.0;
	const double Step = static_cast<double>(DeltaSeconds) * ftcore::StubScreen::speedFactor(bFast, Dilation);

	// Presses first: the screen stamps them with the round's clock as it stood at the start of the frame.
	if (Screen.playing())
	{
		PollKeys();
	}
	Screen.tick(Step);
	if (Screen.finished())
	{
		Finish();
	}
}

void AFTMiniGameMode::Finish()
{
	if (bReturning)
	{
		return;
	}
	bReturning = true;
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI || !GI->StoryCarry.bValid)
	{
		return;
	}
	FFTStoryCarry& Carry = GI->StoryCarry;
	Carry.bDone = true;
	Carry.bWon = true; // there is no fail state: the outcome is only shown
	// Ids and counts only.
	UE_LOG(LogForgottenThird, Display, TEXT("Mini: done %s beat %d step %d tries %d accuracy %d cleared %d"), *Carry.Scene, Carry.Beat,
		Carry.Step, Screen.triesUsed(), FMath::RoundToInt(Screen.bestAccuracy() * 100.f), Screen.cleared() ? 1 : 0);
	OpenReturnMap();
}

void AFTMiniGameMode::OpenReturnMap()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->StoryCarry.bValid && !GI->StoryCarry.ReturnMap.IsEmpty())
	{
		FTStep::OpenMap(this, GI->StoryCarry.ReturnMap);
	}
}
