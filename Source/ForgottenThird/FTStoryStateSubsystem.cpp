#include "FTStoryStateSubsystem.h"

#include "FTFightSession.h"
#include "FTGameInstance.h"
#include "FTStorySubsystem.h"
#include "FTText.h"

#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "HAL/PlatformMisc.h"

#include <string>
#include <vector>

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
	UFTStorySubsystem* Story = Collection.InitializeDependency<UFTStorySubsystem>();
	Registry = MakeUnique<ftcore::FactRegistry>();
	State = MakeUnique<ftcore::StoryState>();
	ftcore::storyReset(*State);
	// The story's own folder (UFTGameInstance::StoryDir), so the registry and the story never part.
	const UFTGameInstance* GI = Cast<UFTGameInstance>(GetGameInstance());
	LoadRegistry(GI ? GI->StoryDir : FString(TEXT("Data/Story")));
	if (Story)
	{
		Story->OnSceneChanged.AddDynamic(this, &UFTStoryStateSubsystem::HandleRunnerScene);
		Story->OnFinished.AddDynamic(this, &UFTStoryStateSubsystem::HandleRunnerFinished);
	}

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
	Unbind();
	if (UFTStorySubsystem* Story = GetGameInstance()->GetSubsystem<UFTStorySubsystem>())
	{
		Story->OnSceneChanged.RemoveAll(this);
		Story->OnFinished.RemoveAll(this);
	}
	SceneMap.Reset();
	LastRunnerScene.Reset();
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
	Unbind(); // the registry changes under it
	*Registry = ftcore::FactRegistry{};
	bRegistryLoaded = false;
	SceneMap.Reset(); // its ordinals belong to the registry
	LastRunnerScene.Reset();
	if (State)
	{
		ftcore::storyReset(*State); // the state's slots and scenes belong to the registry too: a new game's state
	}
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
	LoadSceneMap(Base);
	return true;
}

void UFTStoryStateSubsystem::LoadSceneMap(const FString& Base)
{
	FString Csv;
	if (!FFileHelper::LoadFileToString(Csv, *(Base / TEXT("story_scene_map.csv"))))
	{
		UE_LOG(LogFTStoryState, Display, TEXT("StoryState: no scene map; the scene flow is not followed."));
		return;
	}
	std::vector<ftcore::StorySceneLink> Links;
	std::string Error;
	if (!ftcore::importStorySceneMap(FT::ToUtf8(Csv), *Registry, Links, &Error))
	{
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: scene map refused (%s); the scene flow is not followed."),
			   *FT::ToFString(Error));
		return;
	}
	for (const ftcore::StorySceneLink& Link : Links)
	{
		SceneMap.Add(FT::ToFString(Link.scene), Link.ordinal);
	}
	UE_LOG(LogFTStoryState, Log, TEXT("StoryState: scene map links %d runner scenes."), SceneMap.Num());
}

namespace
{
void LogStep(const ftcore::StorySceneStep& Step, const FString& SceneId, const TCHAR* What)
{
	if (Step.refused)
	{
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: %s at runner scene '%s' refused (committed %d, began %d, restarted %d)."),
			   What, *SceneId, Step.committed ? 1 : 0, Step.began ? 1 : 0, Step.restarted ? 1 : 0);
	}
}
} // namespace

void UFTStoryStateSubsystem::EnterRunnerScene(const FString& SceneId)
{
	if (!State || SceneMap.Num() == 0)
	{
		return;
	}
	const uint8* Found = SceneId.IsEmpty() ? nullptr : SceneMap.Find(SceneId);
	const uint8 Scene = Found ? *Found : ftcore::kNoScene;
	const bool bSameRunnerScene = !SceneId.IsEmpty() && SceneId == LastRunnerScene;
	LastRunnerScene = SceneId;
	if (Scene != ftcore::kNoScene && Scene == State->openScene && !bSameRunnerScene)
	{
		return; // the next runner scene of the same registry scene: it goes on
	}
	LogStep(ftcore::storyEnterScene(*State, *Registry, Scene), SceneId, TEXT("scene entry"));
}

void UFTStoryStateSubsystem::SyncToRunnerScene(const FString& SceneId)
{
	if (!State || SceneMap.Num() == 0)
	{
		return;
	}
	const uint8* Found = SceneId.IsEmpty() ? nullptr : SceneMap.Find(SceneId);
	LastRunnerScene = SceneId;
	LogStep(ftcore::storyEnterScene(*State, *Registry, Found ? *Found : ftcore::kNoScene), SceneId, TEXT("load"));
}

void UFTStoryStateSubsystem::HandleRunnerScene(const FString& SceneId)
{
	const UFTStorySubsystem* Story = GetGameInstance()->GetSubsystem<UFTStorySubsystem>();
	if (Story && Story->IsApplyingSave())
	{
		return; // a load: the block decodes next and SyncToRunnerScene follows it
	}
	EnterRunnerScene(SceneId);
}

void UFTStoryStateSubsystem::HandleRunnerFinished()
{
	const UFTStorySubsystem* Story = GetGameInstance()->GetSubsystem<UFTStorySubsystem>();
	if (Story && Story->IsApplyingSave())
	{
		return; // as HandleRunnerScene
	}
	EnterRunnerScene(FString());
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
	LastRunnerScene.Reset();
}

void UFTStoryStateSubsystem::BindFightSession(UFTFightSession* Session)
{
	if (Session)
	{
		if (BoundSession.Get() != Session)
		{
			Unbind(); // one session holds the state at a time
		}
		Session->BindStory(State.Get(), Registry.Get());
		BoundSession = Session;
	}
}

void UFTStoryStateSubsystem::Unbind()
{
	if (UFTFightSession* Session = BoundSession.Get())
	{
		Session->BindStory(nullptr, nullptr);
	}
	BoundSession.Reset();
}

bool UFTStoryStateSubsystem::EncodeBlock(TArray<uint8>& Out)
{
	if (!State)
	{
		return false;
	}
	TArray<uint8> Block;
	Block.SetNumZeroed(ftcore::kStoryStateBlockSize);
	if (!ftcore::storyEncode(*State, *Registry, std::span<std::uint8_t, ftcore::kStoryStateBlockSize>(Block.GetData(), ftcore::kStoryStateBlockSize)))
	{
		UE_LOG(LogFTStoryState, Warning, TEXT("Story state not saved: the state refused to encode; the previous block is kept"));
		return false;
	}
	Out = MoveTemp(Block);
	return true;
}

ftcore::StoryLoad UFTStoryStateSubsystem::DecodeBlock(const TArray<uint8>& In)
{
	if (!State)
	{
		return ftcore::StoryLoad::Empty;
	}
	ftcore::StoryStateLoadReport Report;
	const ftcore::StoryLoad Result = ftcore::storyLoadOrNew(
		std::span<const std::uint8_t>(In.GetData(), static_cast<std::size_t>(In.Num())), *Registry, *State, &Report);
	if (Result != ftcore::StoryLoad::Ok && Result != ftcore::StoryLoad::Empty)
	{
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: saved block refused (%hs); a new game's state instead."),
			   ftcore::storyLoadName(Result));
	}
	else if (Result == ftcore::StoryLoad::Ok && Report.registryMismatch)
	{
		UE_LOG(LogFTStoryState, Warning,
			   TEXT("StoryState: the save was written with a different registry (%d stale facts, %d unknown slots kept, %d derived)."),
			   static_cast<int32>(Report.staleFacts), static_cast<int32>(Report.unknownSlotsPreserved), static_cast<int32>(Report.derived));
	}
	return Result;
}
