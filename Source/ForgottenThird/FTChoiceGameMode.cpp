#include "FTChoiceGameMode.h"

#include "Camera/CameraActor.h"
#include "EngineUtils.h"
#include "FTChoiceHUD.h"
#include "FTGameInstance.h"
#include "FTPauseSubsystem.h"
#include "FTPlayerController.h"
#include "FTStorySubsystem.h"
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
bool AlphaFastChoice()
{
	return FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
}

// Autoplay: the skip comes after the input grace, so the closing lines show for a moment; the rest
// is pressed through at the stub screen's autoplay delay.
constexpr double kAutoSkipAt = ftcore::StubScreen::kInputGrace;
constexpr double kAutoFinishAt = ftcore::StubScreen::kAutoPlayDelay;
// Autoplay moves at most (a skip plus the tag lines).
constexpr int32 kAutoMoves = 2 + static_cast<int32>(ftcore::DialogueLoopDef::kMaxTagLines);
} // namespace

AFTChoiceGameMode::AFTChoiceGameMode()
{
	HUDClass = AFTChoiceHUD::StaticClass();
	bHostsStory = false;
	PrimaryActorTick.bCanEverTick = true;
	Options.Reserve(MaxOptions);
}

void AFTChoiceGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as the stub, battle and exploration modes do: the
	// story must not start here.
	AGameModeBase::StartPlay();

	bFast = AlphaFastChoice();
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

		bHasLoop = LoadLoop(*Carry);
	}
	if (bHasLoop)
	{
		Loop.begin(&Def);
		Card.begin(false, false);
	}
	else
	{
		Card.begin(bHasStep, bAutoPlay);
	}
	CacheLoop();

	if (AFTPlayerController* PC = Cast<AFTPlayerController>(GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr))
	{
		Controller = PC;
		PC->OnAdvance.AddDynamic(this, &AFTChoiceGameMode::HandlePress);
		PC->OnConfirm.AddDynamic(this, &AFTChoiceGameMode::HandlePress);
		PC->OnChoose.AddDynamic(this, &AFTChoiceGameMode::HandleChoose);
		PC->OnCancel.AddDynamic(this, &AFTChoiceGameMode::HandleSkip);
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

void AFTChoiceGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
	if (AFTPlayerController* PC = Controller.Get())
	{
		PC->OnAdvance.RemoveDynamic(this, &AFTChoiceGameMode::HandlePress);
		PC->OnConfirm.RemoveDynamic(this, &AFTChoiceGameMode::HandlePress);
		PC->OnChoose.RemoveDynamic(this, &AFTChoiceGameMode::HandleChoose);
		PC->OnCancel.RemoveDynamic(this, &AFTChoiceGameMode::HandleSkip);
	}
	Super::EndPlay(Reason);
}

bool AFTChoiceGameMode::LoadLoop(const FFTStoryCarry& Carry)
{
	if (Carry.Beat < 0 || Carry.Step < 0 || Carry.Scene.IsEmpty())
	{
		return false;
	}
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const FString StoryDir = GI ? GI->StoryDir : FString(TEXT("Data/Story"));
	const FString Root = FPaths::IsRelative(StoryDir) ? FPaths::Combine(FPaths::ProjectDir(), StoryDir) : StoryDir;
	const std::string Name = ftcore::dialogueLoopFileName(FT::ToUtf8(Carry.Scene), static_cast<uint32>(Carry.Beat),
		static_cast<uint32>(Carry.Step));
	const FString File = FPaths::Combine(Root, TEXT("loops"), FT::ToFString(Name));

	if (!IFileManager::Get().FileExists(*File))
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Choice: no loop file for %s beat %d step %d; showing the step card."), *Carry.Scene,
			Carry.Beat, Carry.Step);
		return false;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Choice: couldn't read the loop file for %s beat %d step %d; showing the step card."),
			*Carry.Scene, Carry.Beat, Carry.Step);
		return false;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ftcore::importDialogueLoop(FT::BytesAsText(Bytes), Def, &Error, &Warnings))
	{
		// The loader's errors name paths and keys only, never line text.
		UE_LOG(LogForgottenThird, Warning, TEXT("Choice: loop file for %s beat %d step %d refused; showing the step card:\n%s"), *Carry.Scene,
			Carry.Beat, Carry.Step, *FT::ToFString(Error));
		return false;
	}
	if (!Def.matches(FT::ToUtf8(Carry.Scene), static_cast<uint32>(Carry.Beat), static_cast<uint32>(Carry.Step)))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Choice: loop file for %s beat %d step %d names another step; showing the step card."),
			*Carry.Scene, Carry.Beat, Carry.Step);
		Def = ftcore::DialogueLoopDef();
		return false;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Choice: %s"), *FT::ToFString(W));
	}
	UE_LOG(LogForgottenThird, Log, TEXT("Choice: loop %s beat %d step %d: %u rounds, %u tag lines, %u flags, %d warnings."), *Carry.Scene,
		Carry.Beat, Carry.Step, Def.roundCount, Def.tagCount, Def.flagCount, static_cast<int32>(Warnings.size()));
	return true;
}

void AFTChoiceGameMode::CacheLoop()
{
	if (Loop.epoch() == CachedEpoch)
	{
		return;
	}
	CachedEpoch = Loop.epoch();
	++Serial;
	Speaker = FT::ToFString(Loop.speaker());
	Text = FT::ToFString(Loop.text());
	Options.Reset();
	const uint32 Count = FMath::Min(Loop.optionCount(), static_cast<uint32>(MaxOptions));
	for (uint32 I = 0; I < Count; ++I)
	{
		Options.Add(FString::Printf(TEXT("%u  %s"), I + 1, *FT::ToFString(Loop.optionText(I))));
	}
	for (int32 I = 0; I < MaxOptions; ++I)
	{
		Greyed[I] = Loop.optionGreyed(static_cast<uint32>(I));
	}
}

bool AFTChoiceGameMode::InputReady() const
{
	// Not while the pause menu is up (or closed this frame): its presses are not the screen's.
	return bHasStep && !bReturning && Elapsed >= ftcore::StubScreen::kInputGrace && !UFTPauseSubsystem::IsBlocking(this);
}

void AFTChoiceGameMode::HandlePress()
{
	if (!InputReady())
	{
		return;
	}
	if (bHasLoop)
	{
		Loop.press();
	}
	else
	{
		Card.press();
	}
}

void AFTChoiceGameMode::HandleChoose(int32 Option)
{
	if (InputReady() && bHasLoop && Option >= 0)
	{
		Loop.choose(static_cast<uint32>(Option));
	}
}

void AFTChoiceGameMode::HandleSkip()
{
	if (InputReady() && bHasLoop)
	{
		Loop.skip();
	}
}

void AFTChoiceGameMode::Tick(float DeltaSeconds)
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

	if (!bHasLoop)
	{
		Card.tick(Step);
		if (Card.finished())
		{
			Finish();
		}
		return;
	}

	if (bAutoPlay)
	{
		// The skip path: to the closing lines, then through them.
		if (!bAutoSkipped && Elapsed >= kAutoSkipAt)
		{
			bAutoSkipped = true;
			Loop.apply(Loop.autoplay());
		}
		if (Elapsed >= kAutoFinishAt)
		{
			for (int32 I = 0; I < kAutoMoves && !Loop.finished(); ++I)
			{
				if (!Loop.apply(Loop.autoplay()))
				{
					break;
				}
			}
		}
	}
	CacheLoop();
	if (Loop.finished())
	{
		Finish();
	}
}

void AFTChoiceGameMode::ApplyFlags(const FFTStoryCarry& Carry)
{
	const uint32 Count = Loop.flagCount();
	if (Count == 0)
	{
		return;
	}
	UFTStorySubsystem* Story = GetGameInstance() ? GetGameInstance()->GetSubsystem<UFTStorySubsystem>() : nullptr;
	uint32 Set = 0;
	for (uint32 I = 0; I < Count; ++I)
	{
		const FString Id = FT::ToFString(Loop.flagId(I));
		if (Story && Story->SetFlag(Id, FT::ToFString(Loop.flagValue(I))))
		{
			++Set;
		}
		else
		{
			// The id only: the story doesn't know the flag (or that value for it), or there is no story.
			UE_LOG(LogForgottenThird, Warning, TEXT("Choice: flag %s not set at %s beat %d step %d."), *Id, *Carry.Scene, Carry.Beat, Carry.Step);
		}
	}
	UE_LOG(LogForgottenThird, Log, TEXT("Choice: set %u of %u flags at %s beat %d step %d."), Set, Count, *Carry.Scene, Carry.Beat, Carry.Step);
}

void AFTChoiceGameMode::Finish()
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
	if (bHasLoop)
	{
		ApplyFlags(Carry);
	}
	Carry.bDone = true;
	Carry.bWon = true;
	// Ids and counts only; never line text.
	UE_LOG(LogForgottenThird, Display, TEXT("Choice: done %s beat %d step %d picks %u skipped %d"), *Carry.Scene, Carry.Beat, Carry.Step,
		bHasLoop ? Loop.picks() : 0u, (bHasLoop && Loop.skipped()) ? 1 : 0);
	OpenReturnMap();
}

void AFTChoiceGameMode::OpenReturnMap()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->StoryCarry.bValid && !GI->StoryCarry.ReturnMap.IsEmpty())
	{
		UGameplayStatics::OpenLevel(this, FName(*GI->StoryCarry.ReturnMap));
	}
}
