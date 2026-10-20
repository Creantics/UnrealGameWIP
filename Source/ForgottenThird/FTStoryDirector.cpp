#include "FTStoryDirector.h"

#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "ForgottenThird.h"
#include "FTGameInstance.h"
#include "FTPlayerController.h"
#include "FTStorySubsystem.h"
#include "FTText.h"

#include "ftcore/GameplayPlan.h"
#include "ftcore/StoryChain.h"

using ftcore::DialogueCommand;
using ftcore::StoryRunner;

namespace
{
// Fast autoplay: moves per frame at most.
constexpr int32 kFastMovesPerTick = 200;
// Autoplay gives up after this long with nothing it can do.
constexpr float kStuckSeconds = 10.f;
} // namespace

AFTStoryDirector::AFTStoryDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	Options.Reserve(MaxOptions);
}

void AFTStoryDirector::BeginPlay()
{
	Super::BeginPlay();
	const TCHAR* Cmd = FCommandLine::Get();
	bAlpha = FParse::Param(Cmd, TEXT("AlphaAutoPlay"));
	bFast = FParse::Param(Cmd, TEXT("StoryAutoPlayFast")) || bAlpha;
	bAutoPlay = bAutoPlay || bFast || FParse::Param(Cmd, TEXT("StoryAutoPlay"));
	bQuitOnFinish = bQuitOnFinish || FParse::Param(Cmd, TEXT("StoryQuitOnFinish"));
	bHandoffAuto = FParse::Param(Cmd, TEXT("StoryAutoPlayHandoff")) || bAlpha;
	bAutoPlay = bAutoPlay || bHandoffAuto;
	bQuitOnReturn = FParse::Param(Cmd, TEXT("StoryQuitOnReturn"));

	const UGameInstance* GameInstance = GetGameInstance();
	Story = GameInstance ? GameInstance->GetSubsystem<UFTStorySubsystem>() : nullptr;
	if (!Story || !Story->IsLoaded())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: no story loaded; the dialogue box stays empty."));
		SetActorTickEnabled(false);
		return;
	}

	StartTime = GetWorld()->GetTimeSeconds();
	SceneEpoch = MAX_uint32;
	ResumeFromCarry();

	Presenter.setRevealRate(bFast ? 0.f : RevealRate);
	Presenter.setHandoff(!bFast || bHandoffAuto);
	Presenter.setRunner(&Story->GetRunner());
	CacheBeat(); // setRunner already read the first beat, so Refresh() sees no change

	if (AFTPlayerController* PC = Cast<AFTPlayerController>(GetWorld()->GetFirstPlayerController()))
	{
		Controller = PC;
		bPrevRouteInput = PC->bRouteInputToStory;
		PC->bRouteInputToStory = false;
		PC->OnAdvance.AddDynamic(this, &AFTStoryDirector::HandlePress);
		PC->OnConfirm.AddDynamic(this, &AFTStoryDirector::HandlePress);
		PC->OnChoose.AddDynamic(this, &AFTStoryDirector::HandleChoose);
	}

	Refresh();
	UE_LOG(LogForgottenThird, Log, TEXT("Story: dialogue on%s%s."), bAutoPlay ? (bFast ? TEXT(" (fast autoplay)") : TEXT(" (autoplay)")) : TEXT(""),
		bAlpha ? TEXT(" (alpha)") : TEXT(""));
}

void AFTStoryDirector::EndPlay(const EEndPlayReason::Type Reason)
{
	if (AFTPlayerController* PC = Controller.Get())
	{
		PC->OnAdvance.RemoveDynamic(this, &AFTStoryDirector::HandlePress);
		PC->OnConfirm.RemoveDynamic(this, &AFTStoryDirector::HandlePress);
		PC->OnChoose.RemoveDynamic(this, &AFTStoryDirector::HandleChoose);
		PC->bRouteInputToStory = bPrevRouteInput;
	}
	Super::EndPlay(Reason);
}

void AFTStoryDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Story || bHandingOff)
	{
		return;
	}
	Refresh(); // the subsystem's hold timer may have moved the runner
	Presenter.update(DeltaSeconds);
	if (bAutoPlay)
	{
		TickAutoPlay(DeltaSeconds);
	}
}

void AFTStoryDirector::HandlePress()
{
	if (Story && !bAutoPlay && !bHandingOff)
	{
		Apply(Presenter.press());
	}
}

void AFTStoryDirector::HandleChoose(int32 Option)
{
	if (Story && !bAutoPlay && !bHandingOff && Option >= 0)
	{
		Apply(Presenter.choose(static_cast<uint32>(Option)));
	}
}

bool AFTStoryDirector::Apply(const DialogueCommand& Input)
{
	DialogueCommand Command = Input;
	// A gameplay beat plays as its plan's steps, whatever screen its kind alone would open (or none).
	if ((Command.kind == DialogueCommand::Kind::Handoff || Command.kind == DialogueCommand::Kind::Resume) && Presenter.handoff()
		&& Story->GetRunner().state() == StoryRunner::State::Gameplay)
	{
		if (BeginChain())
		{
			return false; // the runner waits on the beat until the screens come back
		}
		Command = {DialogueCommand::Kind::Resume};
	}
	bool bOk = false;
	switch (Command.kind)
	{
	case DialogueCommand::Kind::Advance:
		bOk = Story->Advance();
		break;
	case DialogueCommand::Kind::Choose:
		bOk = Story->Choose(static_cast<int32>(Command.option));
		break;
	case DialogueCommand::Kind::Resume:
		bOk = Story->ResumeAfterGameplay();
		break;
	case DialogueCommand::Kind::SkipHold:
		Story->SkipHold();
		bOk = true;
		break;
	case DialogueCommand::Kind::Handoff: // BeginChain took every hand-off above
	case DialogueCommand::Kind::None:
		return false;
	}
	if (bOk)
	{
		++Moves;
	}
	Refresh();
	return bOk;
}

void AFTStoryDirector::Refresh()
{
	const bool bNewBeat = Presenter.sync();
	const StoryRunner& Runner = Story->GetRunner();
	if (Runner.sceneEpoch() != SceneEpoch && Runner.scene())
	{
		SceneEpoch = Runner.sceneEpoch();
		++Scenes;
		if (UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
		{
			++GI->Alpha.scenes;
		}
		const ftcore::StoryDatabase& Db = Story->GetDatabase();
		SceneTitle = FT::ToFString(Db.view(Runner.scene()->title));
		SceneStart = GetWorld()->GetTimeSeconds();
		UE_LOG(LogForgottenThird, Log, TEXT("Story: scene %s"), *FT::ToFString(Db.view(Runner.scene()->id)));
		++BeatSerial;
	}
	if (bNewBeat)
	{
		CacheBeat();
	}
	if (Presenter.state() == StoryRunner::State::Finished)
	{
		ReportFinished();
	}
}

void AFTStoryDirector::CacheBeat()
{
	const StoryRunner& Runner = Story->GetRunner();
	Text = FT::ToFString(Runner.text());
	Speaker = FT::ToFString(Runner.speaker());
	Options.Reset();
	const int32 Count = static_cast<int32>(Runner.optionCount());
	HiddenOptions = FMath::Max(0, Count - MaxOptions);
	for (int32 I = 0; I < Count && I < MaxOptions; ++I)
	{
		Options.Add(FString::Printf(TEXT("%d   "), I + 1) + FT::ToFString(Runner.optionText(static_cast<uint32>(I))));
		Selectable[I] = Runner.optionSelectable(static_cast<uint32>(I));
	}
	if (HiddenOptions > 0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: a choice has %d options; only %d can be picked."), Count, MaxOptions);
	}
	WaitTime = 0.f;
	++BeatSerial;
}

void AFTStoryDirector::TickAutoPlay(float DeltaSeconds)
{
	if (Presenter.state() == StoryRunner::State::Finished)
	{
		return;
	}
	if (Presenter.revealed())
	{
		WaitTime += DeltaSeconds; // CacheBeat resets it
	}
	if (bFast)
	{
		for (int32 I = 0; I < kFastMovesPerTick; ++I)
		{
			const DialogueCommand Command = Presenter.autoplay();
			if (Command.none() || !Apply(Command) || Presenter.state() == StoryRunner::State::Finished)
			{
				break;
			}
		}
	}
	else
	{
		const DialogueCommand Command = Presenter.autoplay();
		const float Pause = Command.kind == DialogueCommand::Kind::SkipHold ? 0.f : AutoPlayPause;
		if (!Command.none() && WaitTime >= Pause)
		{
			Apply(Command);
		}
	}
	if (WaitTime > kStuckSeconds && !bReported)
	{
		const StoryRunner& Runner = Story->GetRunner();
		UE_LOG(LogForgottenThird, Error, TEXT("Story: STUCK at %s beat %u (state %d)."),
			*FT::ToFString(Runner.position().scene), Runner.beatIndex(), static_cast<int32>(Runner.state()));
		bReported = true;
		if (UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
		{
			++GI->Alpha.stuck;
		}
		if (bQuitOnFinish)
		{
			Quit(bAlpha ? 1 : 0);
		}
	}
}

void AFTStoryDirector::ReportFinished()
{
	if (bReported)
	{
		return;
	}
	bReported = true;
	const double Seconds = GetWorld()->GetTimeSeconds() - StartTime;
	UE_LOG(LogForgottenThird, Log, TEXT("Story: FINISHED after %d moves, %d scenes, %.1f s.%s"), Moves, Scenes, Seconds,
		Story->GetRunner().stalled() ? TEXT(" (stalled)") : TEXT(""));
	int32 ExitCode = 0;
	if (bAlpha)
	{
		if (const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
		{
			char Line[160];
			GI->Alpha.formatFinished(Line, sizeof Line);
			UE_LOG(LogForgottenThird, Log, TEXT("%s"), UTF8_TO_TCHAR(Line));
			ExitCode = GI->Alpha.exitCode();
		}
		else
		{
			ExitCode = 1;
		}
	}
	if (bQuitOnFinish)
	{
		Quit(ExitCode);
	}
}

void AFTStoryDirector::Quit(int32 ExitCode)
{
	if (ExitCode == 0)
	{
		FPlatformMisc::RequestExit(false, TEXT("StoryDirector"));
	}
	else
	{
		FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(ExitCode), TEXT("StoryDirector"));
	}
}

void AFTStoryDirector::ResumeFromCarry()
{
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI || !GI->StoryCarry.bValid)
	{
		return;
	}
	const FFTStoryCarry Carry = GI->StoryCarry;
	GI->StoryCarry = FFTStoryCarry();
	const StoryRunner& Runner = Story->GetRunner();
	const ftcore::StoryPosition Pos = Runner.position();
	// No title card or scene line for the scene we came back to; a new scene still gets both.
	SceneEpoch = Runner.sceneEpoch();
	ftcore::StoryChain& Chain = GI->Chain;
	if (!Carry.bDone)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: back from the %s screen before it was done (step %d/%d at %s beat %d); the beat waits in the box."),
			*FT::ToFString(ftcore::gameplayScreenName(Carry.Screen)), Carry.Step + 1, Carry.StepCount, *Carry.Scene, Carry.Beat);
		++GI->Alpha.warnings;
		GI->FailedScene = Carry.Scene;
		GI->FailedBeat = Carry.Beat;
		Chain.reset();
		return;
	}
	GI->Alpha.addScreen(Carry.Screen);
	if (Runner.state() != StoryRunner::State::Gameplay || FT::ToFString(Pos.scene) != Carry.Scene || static_cast<int32>(Pos.beat) != Carry.Beat
		|| (Chain.active() && static_cast<int32>(Chain.step()) != Carry.Step))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: the story moved since the hand-off at %s beat %d; not resuming."), *Carry.Scene, Carry.Beat);
		++GI->Alpha.warnings;
		Chain.reset();
		return;
	}
	if (Chain.active() && Chain.finishStep(true) == ftcore::StoryChain::Next::Open)
	{
		StartStep();
		return;
	}
	const uint32 Epoch = Runner.sceneEpoch();
	UE_LOG(LogForgottenThird, Log, TEXT("Story: resumed after %s at %s beat %d."), *Carry.Kind, *Carry.Scene, Carry.Beat);
	if (Story->ResumeAfterGameplay())
	{
		++Moves;
	}
	SceneEpoch = Epoch;
	if (bQuitOnReturn)
	{
		Quit(0);
	}
}

bool AFTStoryDirector::BeginChain()
{
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: no game instance to hand off with; skipping the beat."));
		return false;
	}
	const StoryRunner& Runner = Story->GetRunner();
	const FString Scene = FT::ToFString(Runner.position().scene);
	const int32 Beat = static_cast<int32>(Runner.position().beat);
	if (bAlpha && GI->FailedBeat == Beat && GI->FailedScene == Scene)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: skipping %s beat %d, a screen gave up on it."), *Scene, Beat);
		GI->FailedScene.Reset();
		GI->FailedBeat = -1;
		return false;
	}
	ftcore::GameplayPlan Plan;
	if (!Plan.lower(Runner))
	{
		return false;
	}
	if (Plan.warnings() > 0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: the plan at %s beat %d has %u unknown and %u dropped steps."), *Scene, Beat,
			Plan.unknownTokens(), Plan.droppedSteps());
		GI->Alpha.warnings += Plan.warnings();
	}
	GI->Chain.begin(Plan);
	StartStep();
	return true;
}

void AFTStoryDirector::StartStep()
{
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI || !GI->Chain.active())
	{
		return;
	}
	const ftcore::StoryChain& Chain = GI->Chain;
	const ftcore::GameplayStep& Step = Chain.current();
	bHandingOff = true;
	const FString ScreenNameText = FT::ToFString(ftcore::gameplayScreenName(ftcore::screenRoutesTo(Step.screen)));
	const TCHAR* ScreenName = *ScreenNameText;
	switch (ftcore::screenRoutesTo(Step.screen))
	{
	case ftcore::GameplayScreen::Battle:
		Banner = TEXT("Battle!");
		break;
	case ftcore::GameplayScreen::Field:
		Banner = TEXT("Into the field");
		break;
	default:
		Banner = FT::ToFString(Step.tokenName()); // the token as notation ("CHC")
		break;
	}
	BannerStart = GetWorld()->GetTimeSeconds();
	const StoryRunner& Runner = Story->GetRunner();
	UE_LOG(LogForgottenThird, Log, TEXT("Story: step %u/%u %s at %s beat %u."), Chain.step() + 1, Chain.count(), ScreenName,
		*FT::ToFString(Runner.position().scene), Runner.beatIndex());
	float Delay = FMath::Max(0.01f, HandoffDelay);
	if (GI->IsAlphaFast())
	{
		Delay = FMath::Min(Delay, 0.1f);
	}
	FTimerHandle Timer;
	GetWorldTimerManager().SetTimer(Timer, this, &AFTStoryDirector::OpenStep, Delay, false);
}

void AFTStoryDirector::OpenStep()
{
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI || !GI->Chain.active())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: no step to open; skipping the beat."));
		bHandingOff = false;
		Banner.Reset();
		Apply({DialogueCommand::Kind::Resume});
		return;
	}
	const StoryRunner& Runner = Story->GetRunner();
	const ftcore::GameplayStep& Step = GI->Chain.current();
	FFTStoryCarry& Carry = GI->StoryCarry;
	Carry = FFTStoryCarry();
	Carry.bValid = true;
	Carry.ReturnMap = UGameplayStatics::GetCurrentLevelName(this, true);
	Carry.Scene = FT::ToFString(Runner.position().scene);
	Carry.Beat = static_cast<int32>(Runner.position().beat);
	Carry.Kind = FT::ToFString(Runner.gameplayKindName());
	const ftcore::GameplayScreen Routed = ftcore::screenRoutesTo(Step.screen);
	Carry.Screen = Routed;
	Carry.Step = static_cast<int32>(GI->Chain.step());
	Carry.StepCount = static_cast<int32>(GI->Chain.count());
	Carry.StepLabel = FT::ToFString(Step.label());
	Carry.StepToken = FT::ToFString(Step.tokenName());
	Carry.bAutoPlay = bAutoPlay;
	UGameplayStatics::OpenLevel(this, FName(*MapFor(Routed)));
}

const FString& AFTStoryDirector::MapFor(ftcore::GameplayScreen Routed) const
{
	switch (Routed)
	{
	case ftcore::GameplayScreen::Battle: return BattleMap;
	case ftcore::GameplayScreen::Field: return FieldMap;
	case ftcore::GameplayScreen::Choice: return ChoiceMap;
	case ftcore::GameplayScreen::Qte: return QteMap;
	case ftcore::GameplayScreen::Cutscene: return CutsceneMap;
	case ftcore::GameplayScreen::Box:
	case ftcore::GameplayScreen::Stub: break;
	}
	return StubMap;
}

double AFTStoryDirector::GetBannerAge() const
{
	return BannerStart < 0.0 ? -1.0 : GetWorld()->GetTimeSeconds() - BannerStart;
}

double AFTStoryDirector::GetSceneAge() const
{
	return SceneStart < 0.0 ? -1.0 : GetWorld()->GetTimeSeconds() - SceneStart;
}

bool AFTStoryDirector::IsFinished() const
{
	return Presenter.state() == StoryRunner::State::Finished;
}
