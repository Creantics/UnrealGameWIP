#include "FTStorySubsystem.h"

#include "FTStorySaveGame.h"
#include "FTText.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ftcore/StoryImport.h"
#include "ftcore/StorySave.h"

#include <string>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFTStory, Log, All);

namespace
{
EFTStoryState ToState(ftcore::StoryRunner::State S)
{
	switch (S)
	{
	case ftcore::StoryRunner::State::Line: return EFTStoryState::Line;
	case ftcore::StoryRunner::State::Direction: return EFTStoryState::Direction;
	case ftcore::StoryRunner::State::Choice: return EFTStoryState::Choice;
	case ftcore::StoryRunner::State::Hold: return EFTStoryState::Hold;
	case ftcore::StoryRunner::State::Gameplay: return EFTStoryState::Gameplay;
	case ftcore::StoryRunner::State::Finished: return EFTStoryState::Finished;
	case ftcore::StoryRunner::State::Idle: break;
	}
	return EFTStoryState::Idle;
}

EFTCueAction ToCueAction(ftcore::CueAction A)
{
	switch (A)
	{
	case ftcore::CueAction::Stop: return EFTCueAction::Stop;
	case ftcore::CueAction::Stinger: return EFTCueAction::Stinger;
	case ftcore::CueAction::Play: break;
	}
	return EFTCueAction::Play;
}

bool LoadBytes(const FString& Path, TArray<uint8>& Out, FString& OutError)
{
	if (!FFileHelper::LoadFileToArray(Out, *Path))
	{
		OutError = FString::Printf(TEXT("can't read %s"), *Path);
		return false;
	}
	return true;
}

uint32 ToIndex(int32 I) { return I < 0 ? ftcore::kNone : static_cast<uint32>(I); }
} // namespace

UFTStorySubsystem::UFTStorySubsystem()
	: Database(MakeUnique<ftcore::StoryDatabase>())
	, Runner(MakeUnique<ftcore::StoryRunner>())
{
}

UFTStorySubsystem::~UFTStorySubsystem() = default;

void UFTStorySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Runner->setCueHandler(&UFTStorySubsystem::HandleCue, this);
	PendingCues.Reserve(16);
	bAutosave = !IsHeadlessRun();
	OnSceneChanged.AddDynamic(this, &UFTStorySubsystem::HandleSceneChanged);
}

void UFTStorySubsystem::Deinitialize()
{
	Runner->setCueHandler(nullptr, nullptr);
	Runner->setDatabase(nullptr);
	Seen.setDatabase(nullptr);
	PendingCues.Reset();
	Super::Deinitialize();
}

bool UFTStorySubsystem::LoadStory(const FString& Dir, FString& OutError, TArray<FString>& OutWarnings)
{
	OutError.Reset();
	OutWarnings.Reset();
	const FString Root = FPaths::IsRelative(Dir) ? FPaths::Combine(FPaths::ProjectDir(), Dir) : Dir;

	TArray<uint8> StoryBytes;
	if (!LoadBytes(FPaths::Combine(Root, TEXT("story.json")), StoryBytes, OutError))
	{
		return false;
	}
	const std::string_view StoryText = FT::BytesAsText(StoryBytes);

	std::vector<std::string> Ids;
	std::string Error;
	if (!ftcore::storySceneIds(StoryText, Ids, &Error))
	{
		OutError = FT::ToFString(Error);
		return false;
	}

	TArray<TArray<uint8>> SceneBytes;
	SceneBytes.SetNum(static_cast<int32>(Ids.size()));
	std::vector<std::string_view> SceneTexts;
	SceneTexts.reserve(Ids.size());
	for (size_t I = 0; I < Ids.size(); ++I)
	{
		const FString Path = FPaths::Combine(Root, TEXT("scenes"), FT::ToFString(Ids[I]) + TEXT(".json"));
		// A listed scene without a file is reported by the importer.
		if (IFileManager::Get().FileExists(*Path) && LoadBytes(Path, SceneBytes[static_cast<int32>(I)], OutError))
		{
			SceneTexts.push_back(FT::BytesAsText(SceneBytes[static_cast<int32>(I)]));
		}
	}

	TUniquePtr<ftcore::StoryDatabase> NewDb = MakeUnique<ftcore::StoryDatabase>();
	std::vector<std::string> Warnings;
	if (!ftcore::importStory(StoryText, SceneTexts, *NewDb, &Error, &Warnings))
	{
		OutError = FT::ToFString(Error);
		return false;
	}
	for (const std::string& W : Warnings)
	{
		OutWarnings.Add(FT::ToFString(W));
		UE_LOG(LogFTStory, Warning, TEXT("%s"), *OutWarnings.Last());
	}

	Runner->setDatabase(nullptr);
	Database = MoveTemp(NewDb);
	Runner->setDatabase(Database.Get());
	Seen.setDatabase(Database.Get()); // a new database: nothing seen yet, and the backlog's indices are stale
	Backlog.clear();
	PendingCues.Reset();
	SeenSceneEpoch = Runner->sceneEpoch();
	SeenBeatEpoch = Runner->beatEpoch();
	bLoaded = true;
	UE_LOG(LogFTStory, Log, TEXT("Loaded story '%s': %d scenes"), *FT::ToFString(Database->title()),
		static_cast<int32>(Database->sceneCount()));
	return true;
}

bool UFTStorySubsystem::StartStory(const FString& SceneId)
{
	if (!bLoaded)
	{
		return false;
	}
	bReportedFinished = false;
	Backlog.clear(); // a new game (or a headless start): nothing shown yet
	bPaused = false;
	Party = ftcore::PartyProgress();
	const bool bOk = Runner->start(FT::ToUtf8(SceneId));
	Flush();
	return bOk;
}

int32 UFTStorySubsystem::GetPartyLevel() const
{
	return ftcore::levelForXp(Party.xp);
}

ftcore::AwardResult UFTStorySubsystem::AwardBattle(const ftcore::Reward& Reward)
{
	const ftcore::AwardResult Result = ftcore::award(Party, Reward);
	UE_LOG(LogFTStory, Display, TEXT("Party: +%d xp, +%d gold (%d xp, %d gold, level %d%s)."), Reward.xp, Reward.gold, Party.xp,
		Party.gold, Result.levelAfter, Result.levelAfter > Result.levelBefore ? TEXT(", LEVEL UP") : TEXT(""));
	return Result;
}

bool UFTStorySubsystem::Advance()
{
	const bool bOk = Runner->advance();
	Flush();
	return bOk;
}

bool UFTStorySubsystem::Choose(int32 Option)
{
	const bool bOk = Option >= 0 && Runner->choose(static_cast<uint32>(Option));
	Flush();
	return bOk;
}

bool UFTStorySubsystem::ResumeAfterGameplay()
{
	const bool bOk = Runner->resume();
	Flush();
	return bOk;
}

void UFTStorySubsystem::SkipHold()
{
	Runner->skipHold();
	Flush();
}

void UFTStorySubsystem::StopStory()
{
	Runner->stop();
	PendingCues.Reset();
	Backlog.clear();
	bPaused = false;
}

EFTStoryState UFTStorySubsystem::GetState() const { return ToState(Runner->state()); }
FString UFTStorySubsystem::GetSceneId() const { return FT::ToFString(Runner->position().scene); }
int32 UFTStorySubsystem::GetBeatIndex() const { return static_cast<int32>(Runner->beatIndex()); }
FString UFTStorySubsystem::GetText() const { return FT::ToFString(Runner->text()); }
FString UFTStorySubsystem::GetTextKey() const { return FT::ToFString(Runner->textKey()); }
FString UFTStorySubsystem::GetSpeaker() const { return FT::ToFString(Runner->speaker()); }
FString UFTStorySubsystem::GetDirection() const { return FT::ToFString(Runner->direction()); }
FString UFTStorySubsystem::GetLineId() const { return FT::ToFString(Runner->lineId()); }
int32 UFTStorySubsystem::GetOptionCount() const { return static_cast<int32>(Runner->optionCount()); }
FString UFTStorySubsystem::GetOptionText(int32 Option) const { return FT::ToFString(Runner->optionText(ToIndex(Option))); }
FString UFTStorySubsystem::GetOptionTextKey(int32 Option) const { return FT::ToFString(Runner->optionTextKey(ToIndex(Option))); }
bool UFTStorySubsystem::IsOptionSelectable(int32 Option) const { return Runner->optionSelectable(ToIndex(Option)); }
float UFTStorySubsystem::GetHoldRemaining() const { return Runner->holdRemaining(); }
FString UFTStorySubsystem::GetGameplayKind() const { return FT::ToFString(Runner->gameplayKindName()); }

int32 UFTStorySubsystem::GetDefaultOption() const
{
	const uint32 Option = Runner->defaultOption();
	return Option == ftcore::kNone ? -1 : static_cast<int32>(Option);
}

FString UFTStorySubsystem::GetGameplayParam(const FString& Key) const
{
	return FT::ToFString(Runner->param(FT::ToUtf8(Key)));
}

FString UFTStorySubsystem::GetFlag(const FString& Id) const
{
	if (!bLoaded || Database->findFlag(FT::ToUtf8(Id)) == ftcore::kNone)
	{
		return FString();
	}
	return FT::ToFString(Database->formatFlagValue(Runner->flag(FT::ToUtf8(Id))));
}

bool UFTStorySubsystem::SetFlag(const FString& Id, const FString& Value)
{
	ftcore::FlagValue V;
	return bLoaded && Database->parseFlagValue(FT::ToUtf8(Value), V) && Runner->setFlag(FT::ToUtf8(Id), V);
}

bool UFTStorySubsystem::CaptureSave(FFTStorySave& OutSave) const
{
	OutSave = FFTStorySave();
	ftcore::StorySave Save;
	if (!ftcore::captureStory(*Runner, Save))
	{
		return false;
	}
	OutSave.Scene = FT::ToFString(Save.scene);
	OutSave.Beat = static_cast<int32>(Save.beat);
	for (const ftcore::SavedStoryFlag& F : Save.flags)
	{
		FFTStoryFlagSave& Out = OutSave.Flags.AddDefaulted_GetRef();
		Out.Id = FT::ToFString(F.id);
		Out.Kind = F.kind == ftcore::FlagValue::Kind::Int      ? EFTFlagKind::Int
				 : F.kind == ftcore::FlagValue::Kind::String ? EFTFlagKind::String
															 : EFTFlagKind::Bool;
		Out.Number = F.number;
		Out.Text = FT::ToFString(F.text);
	}
	return true;
}

bool UFTStorySubsystem::ApplySave(const FFTStorySave& InSave, TArray<FString>& OutSkippedFlags)
{
	OutSkippedFlags.Reset();
	if (!bLoaded)
	{
		return false;
	}
	ftcore::StorySave Save;
	Save.scene = FT::ToUtf8(InSave.Scene);
	Save.beat = InSave.Beat < 0 ? 0u : static_cast<uint32>(InSave.Beat);
	for (const FFTStoryFlagSave& F : InSave.Flags)
	{
		ftcore::SavedStoryFlag Out;
		Out.id = FT::ToUtf8(F.Id);
		Out.kind = F.Kind == EFTFlagKind::Int      ? ftcore::FlagValue::Kind::Int
				 : F.Kind == EFTFlagKind::String ? ftcore::FlagValue::Kind::String
												 : ftcore::FlagValue::Kind::Bool;
		Out.number = F.Number;
		Out.text = FT::ToUtf8(F.Text);
		Save.flags.push_back(std::move(Out));
	}
	ftcore::StoryLoadReport Report;
	bReportedFinished = false;
	Backlog.clear(); // a load: the backlog starts over
	bPaused = false;
	const bool bOk = ftcore::applyStory(Save, *Runner, &Report);
	TGuardValue<bool> NoAutosave(bApplyingSave, true); // the flush below reports the loaded scene
	for (const std::string& Id : Report.skippedFlags)
	{
		OutSkippedFlags.Add(FT::ToFString(Id));
	}
	Flush();
	return bOk;
}

bool UFTStorySubsystem::IsHeadlessRun()
{
	const TCHAR* Cmd = FCommandLine::Get();
	FString Scene;
	return FParse::Param(Cmd, TEXT("StoryAutoPlay")) || FParse::Param(Cmd, TEXT("StoryAutoPlayFast")) ||
		FParse::Param(Cmd, TEXT("StoryAutoPlayHandoff")) || FParse::Param(Cmd, TEXT("AlphaAutoPlay")) ||
		FParse::Value(Cmd, TEXT("StoryScene="), Scene);
}

bool UFTStorySubsystem::Autosave()
{
	UFTStorySaveGame* Slot = Cast<UFTStorySaveGame>(UGameplayStatics::CreateSaveGameObject(UFTStorySaveGame::StaticClass()));
	if (!Slot || !Slot->CaptureFrom(this))
	{
		return false;
	}
	if (!UGameplayStatics::SaveGameToSlot(Slot, GetSlotName(), 0))
	{
		UE_LOG(LogFTStory, Warning, TEXT("Save: writing the slot failed."));
		return false;
	}
	UE_LOG(LogFTStory, Log, TEXT("Save: slot written at scene %s."), *GetSceneId());
	return true;
}

void UFTStorySubsystem::HandleSceneChanged(const FString& SceneId)
{
	if (bAutosave && !bApplyingSave && bLoaded)
	{
		Autosave();
	}
}

void UFTStorySubsystem::AdoptSeenFromSlot()
{
	if (!bLoaded || Seen.count() > 0 || !UGameplayStatics::DoesSaveGameExist(GetSlotName(), 0))
	{
		return;
	}
	const UFTStorySaveGame* Slot = Cast<UFTStorySaveGame>(UGameplayStatics::LoadGameFromSlot(GetSlotName(), 0));
	ftcore::StorySave Save;
	if (Slot && ftcore::decodeStorySave(FT::ToUtf8(Slot->SaveJson), Save))
	{
		const uint32 Count = ftcore::applySeen(Save, Seen);
		UE_LOG(LogFTStory, Log, TEXT("Save: kept %u seen beats from the slot for the new game."), Count);
	}
}

ftcore::SaveRefusal UFTStorySubsystem::CheckSlot() const
{
	if (!bLoaded)
	{
		return ftcore::SaveRefusal::NoStory;
	}
	if (!UGameplayStatics::DoesSaveGameExist(GetSlotName(), 0))
	{
		return ftcore::SaveRefusal::Empty;
	}
	const UFTStorySaveGame* Slot = Cast<UFTStorySaveGame>(UGameplayStatics::LoadGameFromSlot(GetSlotName(), 0));
	if (!Slot)
	{
		return ftcore::SaveRefusal::Invalid; // unreadable, or not our save class
	}
	return ftcore::checkStorySave(FT::ToUtf8(Slot->SaveJson), Database.Get());
}

ftcore::SaveRefusal UFTStorySubsystem::LoadSlot()
{
	const ftcore::SaveRefusal Check = CheckSlot();
	if (Check != ftcore::SaveRefusal::None)
	{
		UE_LOG(LogFTStory, Warning, TEXT("Save: load refused (%hs)."), ftcore::saveRefusalName(Check));
		return Check;
	}
	const UFTStorySaveGame* Slot = Cast<UFTStorySaveGame>(UGameplayStatics::LoadGameFromSlot(GetSlotName(), 0));
	FString Error;
	TArray<FString> Skipped;
	if (!Slot || !Slot->ApplyTo(this, Error, Skipped))
	{
		UE_LOG(LogFTStory, Warning, TEXT("Save: load refused (%hs)."), ftcore::saveRefusalName(ftcore::SaveRefusal::SceneMissing));
		return ftcore::SaveRefusal::SceneMissing;
	}
	UE_LOG(LogFTStory, Log, TEXT("Save: loaded the slot at scene %s, %d flags skipped."), *GetSceneId(), Skipped.Num());
	return ftcore::SaveRefusal::None;
}

void UFTStorySubsystem::HandleCue(void* User, const ftcore::CueEvent& Event)
{
	UFTStorySubsystem* Self = static_cast<UFTStorySubsystem*>(User);
	Self->PendingCues.Add({Event.id, Event.kind, Event.action});
}

void UFTStorySubsystem::Flush()
{
	if (bFlushing)
	{
		return; // a handler moved the story: the outer Flush picks the changes up
	}
	TGuardValue<bool> Guard(bFlushing, true);
	for (;;)
	{
		if (PendingCues.Num() > 0)
		{
			// Handlers may queue more cues; take this batch by index.
			for (int32 I = 0; I < PendingCues.Num(); ++I)
			{
				const FQueuedCue Cue = PendingCues[I];
				OnCue.Broadcast(FT::ToFString(Cue.Id), FT::ToFString(Cue.Kind), ToCueAction(Cue.Action));
			}
			PendingCues.Reset();
			continue;
		}
		if (Runner->sceneEpoch() != SeenSceneEpoch)
		{
			SeenSceneEpoch = Runner->sceneEpoch();
			OnSceneChanged.Broadcast(GetSceneId());
			continue;
		}
		if (Runner->beatEpoch() != SeenBeatEpoch)
		{
			SeenBeatEpoch = Runner->beatEpoch();
			OnBeatChanged.Broadcast(GetState());
			continue;
		}
		if (Runner->finished() && !bReportedFinished)
		{
			bReportedFinished = true;
			if (Runner->stalled())
			{
				UE_LOG(LogFTStory, Error, TEXT("Story stalled in a loop of empty scenes"));
			}
			OnFinished.Broadcast();
			continue;
		}
		break;
	}
}

void UFTStorySubsystem::Tick(float DeltaTime)
{
	if (!bLoaded || bPaused || Runner->state() != ftcore::StoryRunner::State::Hold)
	{
		return;
	}
	Runner->update(DeltaTime);
	Flush();
}

TStatId UFTStorySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTStorySubsystem, STATGROUP_Tickables);
}

ETickableTickType UFTStorySubsystem::GetTickableTickType() const
{
	return IsTemplate() ? ETickableTickType::Never : ETickableTickType::Always;
}
