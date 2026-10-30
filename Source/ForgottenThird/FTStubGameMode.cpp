#include "FTStubGameMode.h"

#include "Camera/CameraActor.h"
#include "EngineUtils.h"
#include "FTGameInstance.h"
#include "FTStepHost.h"
#include "FTStubHUD.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
bool AlphaFast()
{
	return FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
}
} // namespace

AFTStubGameMode::AFTStubGameMode()
{
	HUDClass = AFTStubHUD::StaticClass();
	bHostsStory = false;
	PrimaryActorTick.bCanEverTick = true;
}

void AFTStubGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as the battle and exploration modes do: the story
	// must not start here.
	AGameModeBase::StartPlay();

	bFast = AlphaFast();
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const FFTStoryCarry* Carry = (GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone) ? &GI->StoryCarry : nullptr;
	bHasStep = Carry != nullptr;
	Screen.begin(bHasStep, bHasStep && Carry->bAutoPlay);

	if (Carry)
	{
		// Whatever the step's token is, the title comes from Core.
		const FString& Token = Carry->StepToken.IsEmpty() ? Carry->Kind : Carry->StepToken;
		TitleText = FT::ToFString(ftcore::stubTitle(FT::ToUtf8(Token)));
		LabelText = Carry->StepLabel;
		StepText = FString::Printf(TEXT("Step %d of %d"), Carry->Step + 1, FMath::Max(Carry->StepCount, 1));
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

void AFTStubGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Screen.waiting())
	{
		return;
	}
	const AWorldSettings* Settings = GetWorld() ? GetWorld()->GetWorldSettings() : nullptr;
	const double Dilation = Settings ? Settings->GetEffectiveTimeDilation() : 1.0;
	Screen.tick(static_cast<double>(DeltaSeconds) * ftcore::StubScreen::speedFactor(bFast, Dilation));

	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC && !Screen.finished()
		&& (PC->WasInputKeyJustPressed(EKeys::SpaceBar) || PC->WasInputKeyJustPressed(EKeys::Enter)
			|| PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Bottom)))
	{
		Screen.press();
	}
	if (Screen.finished())
	{
		Finish();
	}
}

void AFTStubGameMode::Finish()
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
	Carry.bWon = true;
	// Token name, ids and indices only; never the step's label.
	const FString& Token = Carry.StepToken.IsEmpty() ? Carry.Kind : Carry.StepToken;
	UE_LOG(LogForgottenThird, Display, TEXT("Stub: %s step %d/%d at %s beat %d done."), *Token, Carry.Step + 1,
		FMath::Max(Carry.StepCount, 1), *Carry.Scene, Carry.Beat);
	OpenReturnMap();
}

void AFTStubGameMode::OpenReturnMap()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->StoryCarry.bValid && !GI->StoryCarry.ReturnMap.IsEmpty())
	{
		FTStep::OpenMap(this, GI->StoryCarry.ReturnMap);
	}
}
