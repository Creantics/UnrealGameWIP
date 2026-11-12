#include "FTStoryDirector.h"

#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "Misc/Parse.h"
#include "UnrealClient.h"

#include "ForgottenThird.h"
#include "FTAudioSubsystem.h"
#include "FTFaceComponent.h"
#include "FTGameInstance.h"
#include "FTPauseSubsystem.h"
#include "FTPlayerController.h"
#include "FTSettingsSubsystem.h"
#include "FTStepHost.h"
#include "FTStorySubsystem.h"
#include "FTText.h"

#include "ftcore/GameplayPlan.h"
#include "ftcore/StoryChain.h"
#include "ftcore/StoryFaces.h"

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

	FString Prefix;
	if (FParse::Value(Cmd, TEXT("StoryShot="), Prefix) && !Prefix.IsEmpty())
	{
		if (!ftcore::storyShotNameOk(FT::ToUtf8(Prefix)))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Story: -StoryShot takes a plain name (letters, digits, - and _, up to 64); no stills."));
		}
		else
		{
			int32 Max = ftcore::kStoryShotDefault;
			FString MaxText;
			if (FParse::Value(Cmd, TEXT("StoryShotMax="), MaxText))
			{
				if (MaxText.Len() <= 6 && MaxText.IsNumeric() && !MaxText.Contains(TEXT(".")))
				{
					Max = FCString::Atoi(*MaxText);
				}
				else
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("Story: -StoryShotMax takes a whole number 0..%d; %d stills."),
						ftcore::kStoryShotCap, ftcore::kStoryShotDefault);
				}
			}
			ShotPrefix = Prefix;
			ShotsLeft = ftcore::remainingStoryShots(Max, Story->StoryShotsTaken); // the count is per run, not per map
			if (ShotsLeft == 0)
			{
				ShotQuitIn = 0.5f; // nothing (left) to take: quit cleanly
			}
		}
	}
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
		if (!bAutoPlay)
		{
			// The reading aids exist for a person at the keys; autoplay never listens.
			PC->OnBacklog.AddDynamic(this, &AFTStoryDirector::HandleBacklogKey);
			PC->OnWheel.AddDynamic(this, &AFTStoryDirector::HandleWheel);
			PC->OnSkip.AddDynamic(this, &AFTStoryDirector::HandleSkip);
			PC->OnCancel.AddDynamic(this, &AFTStoryDirector::HandleCancel);
			PC->OnBack.AddDynamic(this, &AFTStoryDirector::HandleBack);
			PC->OnMenuStep.AddDynamic(this, &AFTStoryDirector::HandleMenuStep);
		}
	}
	if (!bAutoPlay)
	{
		SettingsSub = GameInstance->GetSubsystem<UFTSettingsSubsystem>();
		Skip.setInterval(SkipInterval);
		ApplySettings();
	}

	Audio = GameInstance->GetSubsystem<UFTAudioSubsystem>();
	if (UFTAudioSubsystem* A = Audio.Get())
	{
		LoadCast();
		A->OnVoiceLine.AddDynamic(this, &AFTStoryDirector::HandleVoiceLine);
		A->OnVoiceStopped.AddDynamic(this, &AFTStoryDirector::HandleVoiceStopped);
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
		PC->OnBacklog.RemoveDynamic(this, &AFTStoryDirector::HandleBacklogKey);
		PC->OnWheel.RemoveDynamic(this, &AFTStoryDirector::HandleWheel);
		PC->OnSkip.RemoveDynamic(this, &AFTStoryDirector::HandleSkip);
		PC->OnCancel.RemoveDynamic(this, &AFTStoryDirector::HandleCancel);
		PC->OnBack.RemoveDynamic(this, &AFTStoryDirector::HandleBack);
		PC->OnMenuStep.RemoveDynamic(this, &AFTStoryDirector::HandleMenuStep);
		PC->bRouteInputToStory = bPrevRouteInput;
	}
	if (UFTAudioSubsystem* A = Audio.Get())
	{
		A->OnVoiceLine.RemoveDynamic(this, &AFTStoryDirector::HandleVoiceLine);
		A->OnVoiceStopped.RemoveDynamic(this, &AFTStoryDirector::HandleVoiceStopped);
	}
	HandleVoiceStopped();
	if (bBacklogOpen)
	{
		CloseBacklog();
	}
	if (bStoryPaused && Story)
	{
		Story->SetPaused(false);
		bStoryPaused = false;
	}
	if (Story && Story->FaceTally.voiced > 0)
	{
		char Line[200];
		const std::size_t N = Story->FaceTally.format(Line, sizeof Line); // the run's totals so far
		UE_LOG(LogForgottenThird, Display, TEXT("%s"), *FT::ToFString(std::string_view(Line, N)));
	}
	Super::EndPlay(Reason);
}

void AFTStoryDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (ShotQuitIn >= 0.f) // -StoryShot: done (or nothing to take); runs with or without autoplay
	{
		ShotQuitIn -= DeltaSeconds;
		if (ShotQuitIn < 0.f)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Story: shots done; quitting."));
			FPlatformMisc::RequestExit(false, TEXT("StoryDirector"));
		}
		return;
	}
	if (!Story || bHandingOff)
	{
		return;
	}
	if (!bAutoPlay)
	{
		ApplySettings();
		const bool bBlocked = InputBlocked();
		if (bBlocked != bStoryPaused)
		{
			bStoryPaused = bBlocked;
			Story->SetPaused(bBlocked); // a hold waits while an overlay is up
		}
		if (bBlocked)
		{
			return; // the story stands still: no typing, no skipping
		}
	}
	Refresh(); // the subsystem's hold timer may have moved the runner
	Presenter.update(DeltaSeconds);
	if (bAutoPlay)
	{
		TickAutoPlay(DeltaSeconds);
	}
	else
	{
		TickReading(DeltaSeconds);
	}
}

void AFTStoryDirector::HandlePress()
{
	if (Story && !bAutoPlay && !bHandingOff && !InputBlocked())
	{
		Apply(Presenter.press());
	}
}

void AFTStoryDirector::HandleChoose(int32 Option)
{
	if (Story && !bAutoPlay && !bHandingOff && !InputBlocked() && Option >= 0)
	{
		Apply(Presenter.choose(static_cast<uint32>(Option)));
	}
}

// ---- reading aids ----

bool AFTStoryDirector::InputBlocked() const
{
	if (bBacklogOpen)
	{
		return true;
	}
	const UFTSettingsSubsystem* Settings = SettingsSub.Get();
	return (Settings && Settings->BlocksInput()) || UFTPauseSubsystem::IsBlocking(this);
}

bool AFTStoryDirector::CanOpenOverlay() const
{
	return Story && !bAutoPlay && !bHandingOff && !InputBlocked();
}

void AFTStoryDirector::HandleBacklogKey()
{
	// While the settings are open (or closed this frame) the key belongs to them.
	if (bBacklogOpen)
	{
		CloseBacklog();
	}
	else if (CanOpenOverlay() && !IsFinished())
	{
		OpenBacklog();
	}
}

void AFTStoryDirector::HandleWheel(int32 Direction)
{
	if (bBacklogOpen)
	{
		ScrollBacklog(Direction > 0 ? 1 : -1); // wheel up goes back in time
	}
	else if (Direction > 0 && CanOpenOverlay() && !IsFinished() && Story->GetBacklog().size() > 0)
	{
		OpenBacklog();
	}
}

void AFTStoryDirector::HandleMenuStep(int32 Step)
{
	if (bBacklogOpen)
	{
		ScrollBacklog(Step < 0 ? 1 : -1); // up arrow goes back
	}
}

void AFTStoryDirector::HandleSkip(bool bHeld)
{
	bSkipHeld = bHeld && Story && !bHandingOff && !InputBlocked();
}

void AFTStoryDirector::HandleCancel()
{
	// Backspace / pad B close the backlog. Esc and pad Start are the pause keys (HandleBack).
	if (bBacklogOpen)
	{
		CloseBacklog();
	}
}

void AFTStoryDirector::HandleBack()
{
	// Esc / pad Start close the backlog before they open the pause menu, and claim the press.
	if (bBacklogOpen)
	{
		CloseBacklog();
		if (AFTPlayerController* PC = Controller.Get())
		{
			PC->ClaimBack();
		}
	}
}

void AFTStoryDirector::OpenBacklog()
{
	AFTPlayerController* PC = Controller.Get();
	bBacklogOpen = true;
	BacklogScroll = 0;
	bSkipHeld = false;
	bBacklogBeganMenu = PC && !PC->IsMenuInputActive();
	if (PC)
	{
		PC->BeginMenuInput(); // up / down scroll, the cursor shows
	}
}

void AFTStoryDirector::CloseBacklog()
{
	bBacklogOpen = false;
	AFTPlayerController* PC = Controller.Get();
	if (bBacklogBeganMenu && PC)
	{
		PC->EndMenuInput();
	}
	bBacklogBeganMenu = false;
}

void AFTStoryDirector::ScrollBacklog(int32 Delta)
{
	const int32 Last = FMath::Max(0, static_cast<int32>(Story->GetBacklog().size()) - 1);
	BacklogScroll = FMath::Clamp(BacklogScroll + Delta, 0, Last);
}

void AFTStoryDirector::ApplySettings()
{
	const UFTSettingsSubsystem* Settings = SettingsSub.Get();
	if (bAutoPlay || !Settings || AppliedSettings == Settings->GetRevision())
	{
		return;
	}
	AppliedSettings = Settings->GetRevision();
	const ftcore::Settings& S = Settings->Get();
	Presenter.setRevealRate(S.textCharsPerSecond());
	Skip.setMode(S.skipMode);
	AutoAdvance.setEnabled(S.autoAdvance);
	AutoAdvance.setDelay(S.autoDelay);
	SubtitleScale = S.subtitleScale();
}

bool AFTStoryDirector::IsSkipKeyDown() const
{
	const AFTPlayerController* PC = Controller.Get();
	return PC && (PC->IsInputKeyDown(EKeys::LeftControl) || PC->IsInputKeyDown(EKeys::RightControl) || PC->IsInputKeyDown(EKeys::S)
		|| PC->IsInputKeyDown(EKeys::Gamepad_RightTrigger));
}

void AFTStoryDirector::TickReading(float DeltaSeconds)
{
	NoteShown();
	if (Presenter.state() == StoryRunner::State::Finished)
	{
		Skip.setHeld(false);
		return;
	}
	// A release can be lost (the menu context eats S while an overlay is up): trust the key state.
	if (bSkipHeld && !IsSkipKeyDown())
	{
		bSkipHeld = false;
	}
	Skip.setHeld(bSkipHeld);
	DialogueCommand Command = Skip.update(DeltaSeconds, Presenter, Story->GetSeen());
	if (Command.none() && !Skip.skipping())
	{
		Command = AutoAdvance.update(DeltaSeconds, Presenter, IsVoiceBusy());
	}
	if (!Command.none())
	{
		Apply(Command);
	}
}

void AFTStoryDirector::NoteShown()
{
	Story->GetBacklog().noteShown(Story->GetRunner(), Presenter);
}

const ftcore::DialogueBacklog* AFTStoryDirector::GetBacklog() const
{
	return Story ? &Story->GetBacklog() : nullptr;
}

const ftcore::StoryDatabase* AFTStoryDirector::GetStoryDatabase() const
{
	return Story ? &Story->GetDatabase() : nullptr;
}

bool AFTStoryDirector::Apply(const DialogueCommand& Input)
{
	DialogueCommand Command = Input;
	if (!Command.none())
	{
		Story->GetSeen().mark(Story->GetRunner()); // the player is leaving this beat: it counts as seen
	}
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
		Story->GetBacklog().noteChoice(Story->GetRunner(), Command.option); // before the move: it reads the choice
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
		Story->NoteMove();
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
		Story->NoteScene();
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
	NoteShown(); // a text shown whole (instant reveal) goes in the backlog at once
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
	BeatAge = 0.f;
	bShotThisBeat = false;
	++BeatSerial;
}

void AFTStoryDirector::TickAutoPlay(float DeltaSeconds)
{
	if (Presenter.state() == StoryRunner::State::Finished)
	{
		return;
	}
	BeatAge += DeltaSeconds;
	const bool bLine = Story->GetRunner().state() == StoryRunner::State::Line;
	if (!bFast && ShotsLeft > 0 && bLine && !bShotThisBeat && Presenter.revealed() && BeatAge >= 1.f)
	{
		bShotThisBeat = true;
		const FString File = StoryShotFile(Story->GetLineId());
		if (!File.IsEmpty())
		{
			FScreenshotRequest::RequestScreenshot(File, true, false); // the viewport's frame with the HUD
			UE_LOG(LogForgottenThird, Display, TEXT("Story: shot %s."), *FPaths::GetCleanFilename(File));
			++Story->StoryShotsTaken;
			Story->StoryShotFiles.Add(File);
			if (--ShotsLeft == 0)
			{
				ShotQuitIn = 2.f; // the last shot is written before the exit
			}
		}
	}
	if (!bFast && IsVoiceBusy())
	{
		WaitTime = 0.f; // the pause starts once the line has been spoken
	}
	else if (Presenter.revealed())
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
			if (bLine)
			{
				const FString LineId = Story->GetLineId();
				UE_LOG(LogForgottenThird, Log, TEXT("Story: line %s held %.2f s (voice %.2f s)."), *LineId, BeatAge,
					LineId == VoiceLineId ? VoiceSeconds : 0.f);
			}
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
	UE_LOG(LogForgottenThird, Log, TEXT("Story: FINISHED after %d moves, %d scenes, %.1f s.%s"), Story->GetRunMoves(), Story->GetRunScenes(),
		Story->GetRunSeconds(),
		Story->GetRunner().stalled() ? TEXT(" (stalled)") : TEXT(""));
	int32 ExitCode = 0;
	if (bAlpha)
	{
		if (const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
		{
			char Line[160];
			GI->Alpha.formatFinished(Line, sizeof Line);
			UE_LOG(LogForgottenThird, Log, TEXT("%s"), UTF8_TO_TCHAR(Line));
			GI->Alpha.formatFights(Line, sizeof Line);
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
	// No title card or scene line for the scene we came back to; a new scene still gets both.
	SceneEpoch = Runner.sceneEpoch();
	const FTStep::EFinish Finish = FTStep::FinishCarried(*GI, Runner, Carry);
	switch (Finish)
	{
	case FTStep::EFinish::Failed:
		return;
	case FTStep::EFinish::Next:
	case FTStep::EFinish::Retry: // the chain is still on the lost fight: it opens again
		StartStep();
		return;
	case FTStep::EFinish::Resume:
	case FTStep::EFinish::ResumeWon:
	case FTStep::EFinish::ResumeLost:
		break;
	}
	const uint32 Epoch = Runner.sceneEpoch();
	UE_LOG(LogForgottenThird, Log, TEXT("Story: resumed after %s at %s beat %d%s."), *Carry.Kind, *Carry.Scene, Carry.Beat,
		Finish == FTStep::EFinish::ResumeLost ? TEXT(", lost") : Finish == FTStep::EFinish::ResumeWon ? TEXT(", won") : TEXT(""));
	bool bResumed = false;
	if (Finish == FTStep::EFinish::Resume)
	{
		bResumed = Story->ResumeAfterGameplay();
	}
	else if (!(bResumed = Story->ResumeAfterGameplayWithValue(Finish == FTStep::EFinish::ResumeWon ? 1 : 0)))
	{
		// The beat's `sets` flag could not take the value (the load refuses such a beat, so this is a bug). Never a
		// loop on the fight: the story goes on past the beat with its flag untouched (no win written for a loss).
		UE_LOG(LogForgottenThird, Error, TEXT("Story: the result of %s beat %d did not fit its flag; going on past the beat with the flag unset."),
			*Carry.Scene, Carry.Beat);
		++GI->Alpha.warnings;
		bResumed = Story->ResumeAfterGameplay();
	}
	if (bResumed)
	{
		Story->NoteMove();
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
	// A beat a screen or the autopilot gave up on is skipped in a headless run (a counted warning), so the
	// run can't loop on it; a player never reaches this (a player's lost fight retries without a cap).
	if ((bAlpha || bAutoPlay) && GI->FailedBeat == Beat && GI->FailedScene == Scene)
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
	GI->StepRetries = 0;
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
	const FString Map = FTStep::Prepare(*GI, Story->GetRunner(), UGameplayStatics::GetCurrentLevelName(this, true), bAutoPlay);
	FTStep::OpenMap(this, Map);
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

void AFTStoryDirector::LoadCast()
{
	const FString Path = FPaths::ProjectDir() / TEXT("Data/Voice/speakers.json");
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
	{
		return; // no cast yet: voiced lines play without faces
	}
	ftcore::ModelTable Models;
	TArray<uint8> ModelBytes;
	const bool bModels = FFileHelper::LoadFileToArray(ModelBytes, *(FPaths::ProjectDir() / TEXT("Data/Assets/models.json")))
		&& ftcore::importModelManifest(FT::BytesAsText(ModelBytes), Models);
	std::string Error;
	if (!ftcore::importSpeakerCast(FT::BytesAsText(Bytes), Speakers, bModels ? &Models : nullptr, &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: Data/Voice/speakers.json is bad; voiced lines play without faces: %s"),
			*FT::ToFString(Error).TrimEnd().Replace(TEXT("\n"), TEXT("; ")));
		return;
	}
	UE_LOG(LogForgottenThird, Log, TEXT("Story: %d speakers cast."), static_cast<int32>(Speakers.entries.size()));
}

bool AFTStoryDirector::IsVoiceBusy() const
{
	const UFTAudioSubsystem* A = Audio.Get();
	return A && A->IsVoicePlaying();
}

void AFTStoryDirector::HandleVoiceLine(const FString& LineId, float Seconds)
{
	HandleVoiceStopped();
	VoiceLineId = LineId;
	VoiceSeconds = Seconds;
	if (!Story)
	{
		return;
	}
	const std::string_view Model = Speakers.modelFor(Story->GetRunner().speaker());
	const FString ModelId = FT::ToFString(Model);
	// The faces of the speaker's model in this map, else its stand-ins (ftcore::pickFaceRoute).
	TArray<UFTFaceComponent*, TInlineAllocator<4>> Own;
	TArray<UFTFaceComponent*, TInlineAllocator<4>> StandIns;
	const UWorld* World = GetWorld();
	if (!Model.empty())
	{
		for (TObjectIterator<UFTFaceComponent> It; It; ++It)
		{
			UFTFaceComponent* Face = *It;
			if (Face->GetWorld() != World)
			{
				continue;
			}
			if (Face->ModelId == ModelId)
			{
				Own.Add(Face);
			}
			else if (Face->bStandIn)
			{
				StandIns.Add(Face);
			}
		}
	}
	const ftcore::FaceRoute Route = ftcore::pickFaceRoute(!Model.empty(), static_cast<uint32>(Own.Num()), static_cast<uint32>(StandIns.Num()));
	const bool bFace = Route == ftcore::FaceRoute::Own || Route == ftcore::FaceRoute::StandIn;
	// FTValidateAssets reports lines without a track.
	const FString Track = bFace ? UFTAudioSubsystem::VoiceTrackFile(LineId) : FString();
	bool bPlayed = false;
	if (!Track.IsEmpty())
	{
		for (UFTFaceComponent* Face : Route == ftcore::FaceRoute::Own ? Own : StandIns)
		{
			if (Face->PlayTrack(Track))
			{
				SpeakingFaces.Add(Face);
				bPlayed = bPlayed || Face->GetBoundShapes() > 0;
			}
		}
	}
	Story->FaceTally.add(Route, !Track.IsEmpty(), bPlayed);
	if (Route == ftcore::FaceRoute::NoFace && !bLoggedNoFace)
	{
		bLoggedNoFace = true;
		UE_LOG(LogForgottenThird, Display, TEXT("Story: no face here for model %s (line %s); voiced lines play without a face."), *ModelId, *LineId);
	}
}

FString AFTStoryDirector::StoryShotFile(const FString& LineId) const
{
	if (!ftcore::storyShotNameOk(FT::ToUtf8(LineId)))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: the line id is not a plain name; no still."));
		return FString();
	}
	// Always under the game's screenshot folder; an existing still is never overwritten.
	const FString Base = FPaths::ScreenShotDir() / FString::Printf(TEXT("%s_%s"), *ShotPrefix, *LineId);
	for (int32 N = 1; N <= 99; ++N)
	{
		const FString File = N == 1 ? Base + TEXT(".png") : FString::Printf(TEXT("%s_%d.png"), *Base, N);
		// A still requested this run may not be on disk yet (the capture writes later).
		if (!Story->StoryShotFiles.Contains(File) && !IFileManager::Get().FileExists(*File))
		{
			return File;
		}
	}
	UE_LOG(LogForgottenThird, Warning, TEXT("Story: 99 stills of line %s are saved already; no still."), *LineId);
	return FString();
}

void AFTStoryDirector::HandleVoiceStopped()
{
	for (const TWeakObjectPtr<UFTFaceComponent>& Face : SpeakingFaces)
	{
		if (UFTFaceComponent* F = Face.Get())
		{
			F->StopTrack();
		}
	}
	SpeakingFaces.Reset();
}
