#include "FTQteGameMode.h"

#include "Camera/CameraActor.h"
#include "EngineUtils.h"
#include "FTActionButtons.h"
#include "FTGameInstance.h"
#include "FTPauseSubsystem.h"
#include "FTQteHUD.h"
#include "FTSettingsSubsystem.h"
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

#include "ftcore/GameplayPlan.h"
#include "ftcore/StubScreen.h"

#include <string>
#include <vector>

namespace
{
bool AlphaFastQte()
{
	return FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
}

} // namespace

AFTQteGameMode::AFTQteGameMode()
{
	HUDClass = AFTQteHUD::StaticClass();
	bHostsStory = false;
	PrimaryActorTick.bCanEverTick = true;
}

void AFTQteGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as the stub, choice, battle and exploration modes do:
	// the story must not start here.
	AGameModeBase::StartPlay();

	bFast = AlphaFastQte();
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const FFTStoryCarry* Carry = (GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone) ? &GI->StoryCarry : nullptr;
	bHasStep = Carry != nullptr;

	ftcore::QteDef Def = ftcore::defaultQteDef();
	if (Carry)
	{
		const FString& Token = Carry->StepToken.IsEmpty() ? Carry->Kind : Carry->StepToken;
		bCoop = ftcore::gameplayTokenFromName(FT::ToUtf8(Token)) == ftcore::GameplayToken::CoopQte;
		if (LoadTable() && Carry->Beat >= 0 && Carry->Step >= 0)
		{
			Def = Table.find(FT::ToUtf8(Carry->Scene), static_cast<uint32>(Carry->Beat), static_cast<uint32>(Carry->Step));
		}
	}
	ftcore::QteAssist Assist;
	if (const UFTSettingsSubsystem* SettingsSub = GI ? GI->GetSubsystem<UFTSettingsSubsystem>() : nullptr)
	{
		Assist.autoComplete = SettingsSub->Get().qteAutoComplete;
		Assist.holdToggle = SettingsSub->Get().holdToggle;
	}
	Screen.begin(bHasStep, Def, bCoop, bHasStep && Carry->bAutoPlay, Assist);

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

bool AFTQteGameMode::LoadTable()
{
	const FString File = FPaths::ProjectDir() / TEXT("Data/Qte/qte.json");
	if (!IFileManager::Get().FileExists(*File))
	{
		UE_LOG(LogForgottenThird, Log, TEXT("QTE: no Data/Qte/qte.json; using the built-in sequence."));
		return false;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("QTE: couldn't read Data/Qte/qte.json; using the built-in sequence."));
		return false;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ftcore::importQteTable(FT::BytesAsText(Bytes), Table, &Error, &Warnings))
	{
		// The loader's errors name paths and keys only.
		UE_LOG(LogForgottenThird, Warning, TEXT("QTE: Data/Qte/qte.json refused; using the built-in sequence:\n%s"), *FT::ToFString(Error));
		return false;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("QTE: %s"), *FT::ToFString(W));
	}
	return true;
}

void AFTQteGameMode::PollKeys()
{
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	// The frame after the pause menu closed, a key still held (pad A that picked Resume is also "down" here)
	// is not a press of the screen's own: it counts once it is let go and pressed again.
	const bool bResumed = UFTPauseSubsystem::IsJustResumed(this);
	for (int32 D = 0; D < FTActionButtons::Count; ++D)
	{
		bool bPressed = false;
		bool bReleased = false;
		bool bNowDown = false;
		for (int32 K = 0; K < FTActionButtons::KeysPerButton; ++K)
		{
			const FKey& Key = FTActionButtons::Key(D, K);
			bPressed |= PC->WasInputKeyJustPressed(Key);
			bReleased |= PC->WasInputKeyJustReleased(Key);
			bNowDown |= PC->IsInputKeyDown(Key);
		}
		const ftcore::ActionButton Button = FTActionButtons::Button(D);
		if (!bResumed && (bPressed || (bNowDown && !bDown[D])))
		{
			Screen.press(Button);
		}
		if (bReleased || (!bNowDown && bDown[D]))
		{
			Screen.release(Button);
		}
		bDown[D] = bNowDown;
	}
}

void AFTQteGameMode::AddPopups(const ftcore::QteResults& Results)
{
	for (const ftcore::QteResult& R : Results)
	{
		FPopup& P = Popups[PopupTotal % MaxPopups];
		P.Prompt = R.prompt;
		P.Grade = R.grade;
		P.Time = Screen.elapsed();
		++PopupTotal;
	}
}

void AFTQteGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bHasStep || bReturning)
	{
		return;
	}
	const AWorldSettings* Settings = GetWorld() ? GetWorld()->GetWorldSettings() : nullptr;
	const double Dilation = Settings ? Settings->GetEffectiveTimeDilation() : 1.0;
	const double Step = static_cast<double>(DeltaSeconds) * ftcore::StubScreen::speedFactor(bFast, Dilation);

	// Presses first: the screen stamps them with the clock as it stood at the start of the frame.
	if (Screen.playing())
	{
		PollKeys();
	}
	Screen.tick(Step);
	AddPopups(Screen.takeFresh());
	if (Screen.finished())
	{
		Finish();
	}
}

void AFTQteGameMode::Finish()
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
	Carry.bWon = true; // there is no fail state: the grade is only shown
	const ftcore::QteSequence& Seq = Screen.sequence();
	// Ids and counts only; never text.
	UE_LOG(LogForgottenThird, Display, TEXT("QTE: done %s beat %d step %d prompts %d perfect %d good %d miss %d"), *Carry.Scene, Carry.Beat,
		Carry.Step, static_cast<int32>(Seq.size()), static_cast<int32>(Seq.perfects()), static_cast<int32>(Seq.goods()),
		static_cast<int32>(Seq.misses()));
	OpenReturnMap();
}

void AFTQteGameMode::OpenReturnMap()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->StoryCarry.bValid && !GI->StoryCarry.ReturnMap.IsEmpty())
	{
		FTStep::OpenMap(this, GI->StoryCarry.ReturnMap);
	}
}
