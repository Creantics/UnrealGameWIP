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
	Collection.InitializeDependency<UFTStorySubsystem>(); // the runner outlives the bridge (deinitialized after this)
	Registry = MakeUnique<ftcore::FactRegistry>();
	State = MakeUnique<ftcore::StoryState>();
	GoodState = MakeUnique<ftcore::StoryState>(); // large: off the stack
	Bridge = MakeUnique<ftcore::StoryBridge>();
	Bridge->setReportHandler(&UFTStoryStateSubsystem::HandleBridgeReport, this);
	ftcore::storyReset(*State);
	// The story's own folder (UFTGameInstance::StoryDir), so the registry and the story never part.
	const UFTGameInstance* GI = Cast<UFTGameInstance>(GetGameInstance());
	LoadRegistry(GI ? GI->StoryDir : FString(TEXT("Data/Story"))); // binds the bridge when the story is already loaded

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
	Bridge.Reset(); // gives the runner's handlers back before the state goes
	SceneLinks.clear();
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
	const TCHAR* Keeps = bRegistryLoaded ? TEXT("the current registry and state are kept") : TEXT("running on an empty one");
	if (!bCsv && !bJson)
	{
		UE_LOG(LogFTStoryState, Display, TEXT("StoryState: no registry in %s; %s."), *Dir, Keeps);
		return false;
	}
	// Imported aside first: a refused import changes nothing.
	TUniquePtr<ftcore::FactRegistry> Fresh = MakeUnique<ftcore::FactRegistry>(); // large: off the stack
	std::string Error;
	if (!bCsv || !bJson || !ftcore::importStoryRegistry(FT::ToUtf8(Csv), FT::ToUtf8(Json), *Fresh, &Error))
	{
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: registry in %s refused (%s); %s."), *Dir,
			   !bCsv || !bJson ? TEXT("one of its two files is missing") : *FT::ToFString(Error), Keeps);
		return false;
	}
	if (Bridge && Bridge->awaitingSync())
	{
		// A load restored the runner and has not called SyncAfterLoad: its block belongs to the old registry and is
		// dropped with the state below; the unbind ends the load's pause and the sync at the end opens the
		// restored scene in a new game's state. Import a registry before a load, not during one.
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: registry imported while a load awaited SyncAfterLoad; the loaded state is dropped and the story goes on in a new game's state."));
	}
	Unbind(); // the registry changes under it
	if (Bridge)
	{
		Bridge->unbind();
	}
	*Registry = MoveTemp(*Fresh); // in place: the state and the fight session keep its address
	bRegistryLoaded = true;
	SceneLinks.clear(); // its ordinals belong to the old registry
	if (State)
	{
		ftcore::storyReset(*State); // the state's slots and scenes belong to the registry too: a new game's state
	}
	ForgetGood();
	LoadSceneMap(Base);
	RebindBridge();
	if (Bridge && Bridge->bound())
	{
		Bridge->sync(); // a story under way: its registry scene opens in the new game's state (idle: nothing)
	}
	return true;
}

void UFTStoryStateSubsystem::LoadSceneMap(const FString& Base)
{
	FString Csv;
	if (!FFileHelper::LoadFileToString(Csv, *(Base / TEXT("story_scene_map.csv"))))
	{
		UE_LOG(LogFTStoryState, Display, TEXT("StoryState: no scene map; runner scene ids are registry codes."));
		return;
	}
	std::string Error;
	if (!ftcore::importStorySceneMap(FT::ToUtf8(Csv), *Registry, SceneLinks, &Error))
	{
		SceneLinks.clear();
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: scene map refused (%s); runner scene ids are registry codes."),
			   *FT::ToFString(Error));
		return;
	}
	UE_LOG(LogFTStoryState, Log, TEXT("StoryState: scene map links %d runner scenes."), static_cast<int32>(SceneLinks.size()));
}

void UFTStoryStateSubsystem::RebindBridge()
{
	if (!Bridge)
	{
		return;
	}
	Bridge->unbind();
	UFTStorySubsystem* Story = GetGameInstance()->GetSubsystem<UFTStorySubsystem>();
	if (!bRegistryLoaded || !Story || !Story->IsLoaded())
	{
		return;
	}
	std::string Error;
	if (!Bridge->bind(Story->GetRunnerForBridge(), *State, *Registry, SceneLinks, &Error))
	{
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: the story and the registry do not match; facts not followed: %s"),
			   *FT::ToFString(Error));
		return;
	}
	UE_LOG(LogFTStoryState, Log, TEXT("StoryState: bridge bound (%d fact flags, %d runner scenes play registry scenes)."),
		   static_cast<int32>(Bridge->boundFlags()), static_cast<int32>(Bridge->mappedScenes()));
}

void UFTStoryStateSubsystem::SyncAfterLoad()
{
	if (Bridge)
	{
		Bridge->sync();
	}
}

bool UFTStoryStateSubsystem::IsBridgeBound() const
{
	return Bridge && Bridge->bound();
}

int32 UFTStoryStateSubsystem::GetMappedSceneCount() const
{
	return Bridge ? static_cast<int32>(Bridge->mappedScenes()) : 0;
}

void UFTStoryStateSubsystem::HandleBridgeReport(void* User, const ftcore::StoryBridgeReport& Report)
{
	(void)User;
	if (Report.kind == ftcore::StoryBridgeReport::Kind::ClearIgnored)
	{
		// By design (docs/STORY_STATE.md): facts are never cleared; a note, not a fault.
		UE_LOG(LogFTStoryState, Log, TEXT("StoryState: fact slot %u kept (facts are never cleared) at runner scene %u."),
			   static_cast<uint32>(Report.fact), Report.scene);
	}
	else if (Report.kind == ftcore::StoryBridgeReport::Kind::SyncMissing)
	{
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: the story moved after a restore with no SyncAfterLoad; facts are not followed until it is called."));
	}
	else if (Report.kind == ftcore::StoryBridgeReport::Kind::SceneRefused)
	{
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: runner scene %u could not open registry scene %u."), Report.scene,
			   static_cast<uint32>(Report.ordinal));
	}
	else
	{
		UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: fact slot %u refused value %lld (result %u) at runner scene %u."),
			   static_cast<uint32>(Report.fact), static_cast<long long>(Report.value), static_cast<uint32>(Report.result),
			   Report.scene);
	}
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
	ForgetGood();
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

namespace
{
FString RegistrySceneName(const ftcore::FactRegistry& Reg, uint8 Ordinal)
{
	if (Ordinal == ftcore::kNoScene || Ordinal >= Reg.sceneCount)
	{
		return TEXT("none");
	}
	return FT::ToFString(std::string_view(Reg.sceneCodes[Ordinal].data()));
}
} // namespace

void UFTStoryStateSubsystem::KeepGood(const TArray<uint8>& Block)
{
	LastGoodBlock = Block;
	if (GoodState && State)
	{
		*GoodState = *State;
	}
}

void UFTStoryStateSubsystem::ForgetGood()
{
	LastGoodBlock.Reset();
	if (ftcore::storyClearSaveSkip(SaveSkip))
	{
		OnSaveSkipChanged.Broadcast(false);
	}
}

void UFTStoryStateSubsystem::NoteSave(bool bSkipped)
{
	const uint8 At = State ? ftcore::storyScenePosition(*State) : ftcore::kNoScene;
	if (ftcore::storyNoteSave(SaveSkip, bSkipped, At))
	{
		if (SaveSkip.skipping)
		{
			UE_LOG(LogFTStoryState, Warning, TEXT("StoryState: saves are being skipped from scene %s on."), *GetSavesSkippedSince());
		}
		else
		{
			UE_LOG(LogFTStoryState, Log, TEXT("StoryState: saves are written again."));
		}
		OnSaveSkipChanged.Broadcast(SaveSkip.skipping);
	}
}

FString UFTStoryStateSubsystem::GetSavesSkippedSince() const
{
	if (!SaveSkip.skipping || !Registry)
	{
		return FString();
	}
	return RegistrySceneName(*Registry, SaveSkip.sinceScene);
}

bool UFTStoryStateSubsystem::EncodeBlock(TArray<uint8>& Out, bool* bOutSkip)
{
	if (bOutSkip)
	{
		*bOutSkip = false;
	}
	if (!State)
	{
		return false;
	}
	TArray<uint8> Block;
	Block.SetNumZeroed(ftcore::kStoryStateBlockSize);
	if (!ftcore::storyEncode(*State, *Registry, std::span<std::uint8_t, ftcore::kStoryStateBlockSize>(Block.GetData(), ftcore::kStoryStateBlockSize)))
	{
		const ftcore::StorySaveFallback Fallback = ftcore::storySaveFallback(
			*State, LastGoodBlock.Num() > 0 ? GoodState.Get() : nullptr,
			std::span<const std::uint8_t>(LastGoodBlock.GetData(), static_cast<std::size_t>(LastGoodBlock.Num())));
		const FString NowScene = RegistrySceneName(*Registry, ftcore::storyScenePosition(*State));
		if (Fallback == ftcore::StorySaveFallback::WriteLastGood)
		{
			UE_LOG(LogFTStoryState, Warning, TEXT("Story state not saved: the state refused to encode; the last good block (scene %s; nothing it saves has moved) is written."),
				   *NowScene);
			Out = LastGoodBlock;
			NoteSave(false);
			return false;
		}
		// Never an empty block over real progress, nor a block that would put other values under this position:
		// the save is skipped and the slot keeps what it has.
		UE_LOG(LogFTStoryState, Warning, TEXT("Story state not saved: the state refused to encode at scene %s and %s; the save is skipped."),
			   *NowScene,
			   Fallback == ftcore::StorySaveFallback::SkipNoGoodBlock ? TEXT("there is no last good block")
																	  : TEXT("the story has moved since the last good block"));
		if (bOutSkip)
		{
			*bOutSkip = true;
		}
		NoteSave(true);
		return false;
	}
	KeepGood(Block);
	Out = MoveTemp(Block);
	NoteSave(false);
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
	if (Result == ftcore::StoryLoad::Ok)
	{
		KeepGood(In);
		if (ftcore::storyClearSaveSkip(SaveSkip)) // a loaded block: the old run is over
		{
			OnSaveSkipChanged.Broadcast(false);
		}
	}
	else
	{
		ForgetGood(); // a new game's state
	}
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
