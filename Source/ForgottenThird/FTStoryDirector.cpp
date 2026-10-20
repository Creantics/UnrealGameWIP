#include "FTStoryDirector.h"

#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "ForgottenThird.h"
#include "FTPlayerController.h"
#include "FTStorySubsystem.h"
#include "FTText.h"

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
	bFast = FParse::Param(Cmd, TEXT("StoryAutoPlayFast"));
	bAutoPlay = bAutoPlay || bFast || FParse::Param(Cmd, TEXT("StoryAutoPlay"));
	bQuitOnFinish = bQuitOnFinish || FParse::Param(Cmd, TEXT("StoryQuitOnFinish"));

	const UGameInstance* GameInstance = GetGameInstance();
	Story = GameInstance ? GameInstance->GetSubsystem<UFTStorySubsystem>() : nullptr;
	if (!Story || !Story->IsLoaded())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: no story loaded; the dialogue box stays empty."));
		SetActorTickEnabled(false);
		return;
	}

	Presenter.setRevealRate(bFast ? 0.f : RevealRate);
	Presenter.setRunner(&Story->GetRunner());

	if (AFTPlayerController* PC = Cast<AFTPlayerController>(GetWorld()->GetFirstPlayerController()))
	{
		Controller = PC;
		PC->bRouteInputToStory = false;
		PC->OnAdvance.AddDynamic(this, &AFTStoryDirector::HandlePress);
		PC->OnConfirm.AddDynamic(this, &AFTStoryDirector::HandlePress);
		PC->OnChoose.AddDynamic(this, &AFTStoryDirector::HandleChoose);
	}

	StartTime = GetWorld()->GetTimeSeconds();
	SceneEpoch = MAX_uint32;
	Refresh();
	UE_LOG(LogForgottenThird, Log, TEXT("Story: dialogue on%s."), bAutoPlay ? (bFast ? TEXT(" (fast autoplay)") : TEXT(" (autoplay)")) : TEXT(""));
}

void AFTStoryDirector::EndPlay(const EEndPlayReason::Type Reason)
{
	if (AFTPlayerController* PC = Controller.Get())
	{
		PC->OnAdvance.RemoveDynamic(this, &AFTStoryDirector::HandlePress);
		PC->OnConfirm.RemoveDynamic(this, &AFTStoryDirector::HandlePress);
		PC->OnChoose.RemoveDynamic(this, &AFTStoryDirector::HandleChoose);
		PC->bRouteInputToStory = true;
	}
	Super::EndPlay(Reason);
}

void AFTStoryDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Story)
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
	if (Story && !bAutoPlay)
	{
		Apply(Presenter.press());
	}
}

void AFTStoryDirector::HandleChoose(int32 Option)
{
	if (Story && !bAutoPlay && Option >= 0)
	{
		Apply(Presenter.choose(static_cast<uint32>(Option)));
	}
}

bool AFTStoryDirector::Apply(const DialogueCommand& Command)
{
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
		if (bQuitOnFinish)
		{
			FPlatformMisc::RequestExit(false, TEXT("StoryDirector"));
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
	if (bQuitOnFinish)
	{
		FPlatformMisc::RequestExit(false, TEXT("StoryDirector"));
	}
}

double AFTStoryDirector::GetSceneAge() const
{
	return SceneStart < 0.0 ? -1.0 : GetWorld()->GetTimeSeconds() - SceneStart;
}

bool AFTStoryDirector::IsFinished() const
{
	return Presenter.state() == StoryRunner::State::Finished;
}
