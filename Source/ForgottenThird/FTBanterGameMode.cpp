#include "FTBanterGameMode.h"

#include "Camera/CameraActor.h"
#include "EngineUtils.h"
#include "FTBanterHUD.h"
#include "FTGameInstance.h"
#include "FTPlayerController.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include <string>
#include <vector>

namespace
{
bool AlphaFastBanter()
{
	return FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
}

// Autoplay: the first line shows for the input grace, then the rest is skipped.
constexpr double kAutoSkipAtBanter = ftcore::StubScreen::kInputGrace;
} // namespace

AFTBanterGameMode::AFTBanterGameMode()
{
	HUDClass = AFTBanterHUD::StaticClass();
	PrimaryActorTick.bCanEverTick = true;
}

void AFTBanterGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as the stub, choice, battle and exploration modes do:
	// the story must not start here.
	AGameModeBase::StartPlay();

	bFast = AlphaFastBanter();
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const FFTStoryCarry* Carry = (GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone) ? &GI->StoryCarry : nullptr;
	bHasStep = Carry != nullptr;
	bAutoPlay = bHasStep && Carry->bAutoPlay;

	if (Carry)
	{
		// The stub card's text, for the fallback: built exactly as AFTStubGameMode builds it.
		const FString& Token = Carry->StepToken.IsEmpty() ? Carry->Kind : Carry->StepToken;
		TitleText = FT::ToFString(ftcore::stubTitle(FT::ToUtf8(Token)));
		LabelText = Carry->StepLabel;
		StepText = FString::Printf(TEXT("Step %d of %d"), Carry->Step + 1, FMath::Max(Carry->StepCount, 1));

		bHasBanter = LoadBanter(*Carry);
	}
	if (bHasBanter)
	{
		Banter.begin(&Def);
		Card.begin(false, false);
	}
	else
	{
		Card.begin(bHasStep, bAutoPlay);
	}
	CacheLine();

	if (AFTPlayerController* PC = Cast<AFTPlayerController>(GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr))
	{
		Controller = PC;
		PC->OnAdvance.AddDynamic(this, &AFTBanterGameMode::HandlePress);
		PC->OnConfirm.AddDynamic(this, &AFTBanterGameMode::HandlePress);
		PC->OnCancel.AddDynamic(this, &AFTBanterGameMode::HandleSkip);
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

void AFTBanterGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
	if (AFTPlayerController* PC = Controller.Get())
	{
		PC->OnAdvance.RemoveDynamic(this, &AFTBanterGameMode::HandlePress);
		PC->OnConfirm.RemoveDynamic(this, &AFTBanterGameMode::HandlePress);
		PC->OnCancel.RemoveDynamic(this, &AFTBanterGameMode::HandleSkip);
	}
	Super::EndPlay(Reason);
}

bool AFTBanterGameMode::LoadBanter(const FFTStoryCarry& Carry)
{
	if (Carry.Beat < 0 || Carry.Step < 0 || Carry.Scene.IsEmpty())
	{
		return false;
	}
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const FString StoryDir = GI ? GI->StoryDir : FString(TEXT("Data/Story"));
	const FString Root = FPaths::IsRelative(StoryDir) ? FPaths::Combine(FPaths::ProjectDir(), StoryDir) : StoryDir;
	const std::string Name =
		ftcore::banterFileName(FT::ToUtf8(Carry.Scene), static_cast<uint32>(Carry.Beat), static_cast<uint32>(Carry.Step));
	const FString File = FPaths::Combine(Root, TEXT("banter"), FT::ToFString(Name));

	if (!IFileManager::Get().FileExists(*File))
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Banter: no banter file for %s beat %d step %d; showing the step card."), *Carry.Scene,
			Carry.Beat, Carry.Step);
		return false;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Banter: couldn't read the banter file for %s beat %d step %d; showing the step card."),
			*Carry.Scene, Carry.Beat, Carry.Step);
		return false;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ftcore::importBanter(FT::BytesAsText(Bytes), Def, &Error, &Warnings))
	{
		// The loader's errors name paths and keys only, never line text.
		UE_LOG(LogForgottenThird, Warning, TEXT("Banter: banter file for %s beat %d step %d refused; showing the step card:\n%s"),
			*Carry.Scene, Carry.Beat, Carry.Step, *FT::ToFString(Error));
		return false;
	}
	if (!Def.matches(FT::ToUtf8(Carry.Scene), static_cast<uint32>(Carry.Beat), static_cast<uint32>(Carry.Step)))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Banter: banter file for %s beat %d step %d names another step; showing the step card."),
			*Carry.Scene, Carry.Beat, Carry.Step);
		Def = ftcore::BanterDef();
		return false;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Banter: %s"), *FT::ToFString(W));
	}
	UE_LOG(LogForgottenThird, Log, TEXT("Banter: lines %u speakers %u at %s beat %d step %d, %d warnings."), Def.lineCount,
		Def.speakerCount, *Carry.Scene, Carry.Beat, Carry.Step, static_cast<int32>(Warnings.size()));
	return true;
}

void AFTBanterGameMode::CacheLine()
{
	if (Banter.epoch() == CachedEpoch)
	{
		return;
	}
	CachedEpoch = Banter.epoch();
	++Serial;
	Speaker = FT::ToFString(Banter.speaker());
	Text = FT::ToFString(Banter.text());
}

bool AFTBanterGameMode::InputReady() const
{
	return bHasStep && !bReturning && Elapsed >= ftcore::StubScreen::kInputGrace;
}

void AFTBanterGameMode::HandlePress()
{
	if (!InputReady())
	{
		return;
	}
	if (bHasBanter)
	{
		Banter.press();
	}
	else
	{
		Card.press();
	}
}

void AFTBanterGameMode::HandleSkip()
{
	if (InputReady() && bHasBanter)
	{
		Banter.skip();
	}
}

void AFTBanterGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bHasStep || bReturning)
	{
		return;
	}
	const AWorldSettings* Settings = GetWorld() ? GetWorld()->GetWorldSettings() : nullptr;
	const double Dilation = Settings ? Settings->GetEffectiveTimeDilation() : 1.0;
	const double Step = static_cast<double>(DeltaSeconds) * ftcore::StubScreen::speedFactor(bFast, Dilation);
	Elapsed += Step;

	if (!bHasBanter)
	{
		Card.tick(Step);
		if (Card.finished())
		{
			Finish();
		}
		return;
	}

	Banter.tick(Step);
	if (bAutoPlay && Elapsed >= kAutoSkipAtBanter)
	{
		Banter.skip();
	}
	CacheLine();
	if (Banter.finished())
	{
		Finish();
	}
}

void AFTBanterGameMode::Finish()
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
	// Ids and counts only; never line text.
	UE_LOG(LogForgottenThird, Display, TEXT("Banter: done %s beat %d step %d lines %u skipped %d"), *Carry.Scene, Carry.Beat, Carry.Step,
		bHasBanter ? Banter.linesShown() : 0u, (bHasBanter && Banter.skipped()) ? 1 : 0);
	OpenReturnMap();
}

void AFTBanterGameMode::OpenReturnMap()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->StoryCarry.bValid && !GI->StoryCarry.ReturnMap.IsEmpty())
	{
		UGameplayStatics::OpenLevel(this, FName(*GI->StoryCarry.ReturnMap));
	}
}
