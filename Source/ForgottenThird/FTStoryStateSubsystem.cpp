#include "FTStoryStateSubsystem.h"

#include "FTFightSession.h"
#include "FTGameInstance.h"
#include "FTText.h"

#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "HAL/PlatformMisc.h"

#include <string>

DEFINE_LOG_CATEGORY_STATIC(LogFTStoryState, Log, All);

namespace
{
// The in-memory round trip -StoryStateCheck logs: the block encodes at its full size and loads back Ok.
bool RoundTrips(UFTStoryStateSubsystem& Sub, FString& OutWhy)
{
	const ftcore::StoryState* Live = Sub.GetState();
	if (!Live)
	{
		OutWhy = TEXT("no state");
		return false;
	}
	const ftcore::StoryState Before = *Live;
	TArray<uint8> First;
	if (!Sub.EncodeBlock(First))
	{
		OutWhy = TEXT("encode refused");
		return false;
	}
	const ftcore::StoryLoad Loaded = Sub.DecodeBlock(First);
	if (Loaded != ftcore::StoryLoad::Ok)
	{
		OutWhy = FString::Printf(TEXT("decode %hs"), ftcore::storyLoadName(Loaded));
		return false;
	}
	const ftcore::StoryState* After = Sub.GetState();
	if (After->facts != Before.facts || After->stamp != Before.stamp || After->wide != Before.wide ||
		After->openScene != Before.openScene || After->lastCommitted != Before.lastCommitted)
	{
		OutWhy = TEXT("decoded state differs");
		return false;
	}
	OutWhy = FString::Printf(TEXT("block %d bytes"), First.Num());
	return true;
}
} // namespace

void UFTStoryStateSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Registry = MakeUnique<ftcore::FactRegistry>();
	State = MakeUnique<ftcore::StoryState>();
	ftcore::storyReset(*State);
	// The story's own folder (UFTGameInstance::StoryDir), so the registry and the story never part.
	const UFTGameInstance* GI = Cast<UFTGameInstance>(GetGameInstance());
	LoadRegistry(GI ? GI->StoryDir : FString(TEXT("Data/Story")));

	if (FParse::Param(FCommandLine::Get(), TEXT("StoryStateCheck")))
	{
		FString Why;
		const bool bOk = RoundTrips(*this, Why);
		NewGame();
		UE_LOG(LogFTStoryState, Display, TEXT("StoryState check: %s (registry %s, %s)"), bOk ? TEXT("OK") : TEXT("FAILED"),
			   bRegistryLoaded ? TEXT("imported") : TEXT("empty"), *Why);
		FPlatformMisc::RequestExit(false, TEXT("StoryStateCheck"));
	}
}

void UFTStoryStateSubsystem::Deinitialize()
{
	State.Reset();
	Registry.Reset();
	bRegistryLoaded = false;
	Super::Deinitialize();
}

bool UFTStoryStateSubsystem::LoadRegistry(const FString& Dir)
{
	if (!Registry)
	{
		return false;
	}
	const FString Base = FPaths::IsRelative(Dir) ? FPaths::ProjectDir() / Dir : Dir;
	FString Csv;
	FString Json;
	const bool bCsv = FFileHelper::LoadFileToString(Csv, *(Base / TEXT("story_facts.csv")));
	const bool bJson = FFileHelper::LoadFileToString(Json, *(Base / TEXT("story_tuning.json")));
	*Registry = ftcore::FactRegistry{};
	bRegistryLoaded = false;
	if (!bCsv && !bJson)
	{
		UE_LOG(LogFTStoryState, Display, TEXT("StoryState: no registry in %s; running on an empty one."), *Dir);
		return false;
	}
	std::string Error;
	if (!bCsv || !bJson || !ftcore::importStoryRegistry(FT::ToUtf8(Csv), FT::ToUtf8(Json), *Registry, &Error))
	{
		*Registry = ftcore::FactRegistry{};
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: registry in %s refused (%s); running on an empty one."), *Dir,
			   !bCsv || !bJson ? TEXT("one of its two files is missing") : *FT::ToFString(Error));
		return false;
	}
	bRegistryLoaded = true;
	return true;
}

bool UFTStoryStateSubsystem::BeginScene(const FString& SceneCode)
{
	return State && ftcore::storyBeginScene(*State, *Registry, ftcore::storySceneOrdinal(*Registry, FT::ToUtf8(SceneCode)));
}

bool UFTStoryStateSubsystem::CommitScene()
{
	return State && ftcore::storyCommitScene(*State, *Registry);
}

bool UFTStoryStateSubsystem::RestartScene()
{
	return State && ftcore::storyRestartScene(*State, *Registry);
}

void UFTStoryStateSubsystem::NewGame()
{
	if (State)
	{
		ftcore::storyReset(*State);
	}
}

void UFTStoryStateSubsystem::BindFightSession(UFTFightSession* Session)
{
	if (Session)
	{
		Session->BindStory(State.Get(), Registry.Get());
	}
}

bool UFTStoryStateSubsystem::EncodeBlock(TArray<uint8>& Out)
{
	Out.Reset();
	if (!State)
	{
		return false;
	}
	Out.SetNumZeroed(ftcore::kStoryStateBlockSize);
	if (!ftcore::storyEncode(*State, *Registry, std::span<std::uint8_t, ftcore::kStoryStateBlockSize>(Out.GetData(), ftcore::kStoryStateBlockSize)))
	{
		UE_LOG(LogFTStoryState, Warning, TEXT("Story state not saved: the state refused to encode; that save loads as a new game's state"));
		Out.Reset();
		return false;
	}
	return true;
}

ftcore::StoryLoad UFTStoryStateSubsystem::DecodeBlock(const TArray<uint8>& In)
{
	if (!State)
	{
		return ftcore::StoryLoad::Empty;
	}
	const ftcore::StoryLoad Result =
		ftcore::storyLoadOrNew(std::span<const std::uint8_t>(In.GetData(), static_cast<std::size_t>(In.Num())), *Registry, *State);
	if (Result != ftcore::StoryLoad::Ok && Result != ftcore::StoryLoad::Empty)
	{
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: saved block refused (%hs); a new game's state instead."),
			   ftcore::storyLoadName(Result));
	}
	return Result;
}
