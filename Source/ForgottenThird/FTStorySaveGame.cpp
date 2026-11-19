#include "FTStorySaveGame.h"

#include "FTStoryStateSubsystem.h"
#include "FTNpcSubsystem.h"
#include "FTStorySubsystem.h"
#include "ForgottenThird.h"
#include "FTText.h"

#include "ftcore/NpcMemory.h"
#include "ftcore/Seen.h"
#include "ftcore/StorySave.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FTWorldClockSubsystem.h"

namespace
{
// The running world clock of the story's world, or null (no world, or the clock is off there).
UFTWorldClockSubsystem* ClockOf(const UFTStorySubsystem* Story)
{
	UWorld* World = Story ? Story->GetWorld() : nullptr;
	UFTWorldClockSubsystem* Clock = World ? World->GetSubsystem<UFTWorldClockSubsystem>() : nullptr;
	return Clock && Clock->IsRunning() ? Clock : nullptr;
}
// The story state beside the story (same game instance), or null.
UFTStoryStateSubsystem* StateOf(const UFTStorySubsystem* Story)
{
	UGameInstance* GI = Story ? Story->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UFTStoryStateSubsystem>() : nullptr;
}
} // namespace

bool UFTStorySaveGame::CaptureFrom(const UFTStorySubsystem* Story)
{
	FFTStorySave Captured;
	if (!Story || !Story->CaptureSave(Captured))
	{
		return false;
	}
	ftcore::StorySave Save;
	Save.scene = FT::ToUtf8(Captured.Scene);
	Save.beat = Captured.Beat < 0 ? 0u : static_cast<uint32>(Captured.Beat);
	for (const FFTStoryFlagSave& F : Captured.Flags)
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
	ftcore::captureSeen(Story->GetSeen(), Save); // the read-state rides along (format 2)
	Save.party = Story->GetParty();
	if (const UFTWorldClockSubsystem* Clock = ClockOf(Story))
	{
		Save.clock = Clock->GetClock();
		if (Clock->IsFixedByCommandLine())
		{
			Save.clock.paused = false; // the capture switch's pause is not part of the game
		}
		Save.hasClock = true;
	}
	// The block first: when the state cannot give one that matches this position, nothing is captured.
	UFTStoryStateSubsystem* State = StateOf(Story);
	TArray<uint8> Block;
	bool bSkip = false;
	if (State)
	{
		State->EncodeBlock(Block, &bSkip);
		if (bSkip)
		{
			return false;
		}
	}
	SaveJson = FT::ToFString(ftcore::encodeStorySave(Save));
	const UGameInstance* GI = Story->GetGameInstance();
	const UFTNpcSubsystem* Npcs = GI ? GI->GetSubsystem<UFTNpcSubsystem>() : nullptr;
	// While the NPC tables aren't loaded, the block this object already holds stays (ftcore::npcSaveBlockToWrite);
	// a fresh object holds the slot's when its caller copied it in (UFTStorySubsystem::Autosave), else none.
	const bool bNpcReady = Npcs && Npcs->IsReady();
	const std::string NpcCaptured = bNpcReady ? FT::ToUtf8(Npcs->CaptureSaveJson()) : std::string();
	const std::string Kept = bNpcReady ? std::string() : FT::ToUtf8(NpcSaveJson);
	NpcSaveJson = FT::ToFString(ftcore::npcSaveBlockToWrite(bNpcReady, NpcCaptured, Kept));
	if (Block.Num() > 0)
	{
		StoryBlock = MoveTemp(Block);
	}
	return true;
}

bool UFTStorySaveGame::ApplyTo(UFTStorySubsystem* Story, FString& OutError, TArray<FString>& OutSkippedFlags) const
{
	OutError.Reset();
	OutSkippedFlags.Reset();
	if (!Story)
	{
		OutError = TEXT("no story subsystem");
		return false;
	}
	ftcore::StorySave Save;
	std::string Error;
	if (!ftcore::decodeStorySave(FT::ToUtf8(SaveJson), Save, &Error))
	{
		OutError = FT::ToFString(Error);
		return false;
	}
	FFTStorySave Loaded;
	Loaded.Scene = FT::ToFString(Save.scene);
	Loaded.Beat = static_cast<int32>(FMath::Min<uint32>(Save.beat, MAX_int32));
	for (const ftcore::SavedStoryFlag& F : Save.flags)
	{
		FFTStoryFlagSave& Out = Loaded.Flags.AddDefaulted_GetRef();
		Out.Id = FT::ToFString(F.id);
		Out.Kind = F.kind == ftcore::FlagValue::Kind::Int      ? EFTFlagKind::Int
				 : F.kind == ftcore::FlagValue::Kind::String ? EFTFlagKind::String
															 : EFTFlagKind::Bool;
		Out.Number = F.number;
		Out.Text = FT::ToFString(F.text);
	}
	if (!Story->ApplySave(Loaded, OutSkippedFlags))
	{
		OutError = TEXT("the saved scene no longer exists, or the story is not loaded");
		return false;
	}
	ftcore::applySeen(Save, Story->GetSeen()); // an old (format 1) save has none: nothing seen
	Story->SetParty(Save.party);               // a save from before progression: xp and gold 0
	if (UFTWorldClockSubsystem* Clock = ClockOf(Story); Clock && Save.hasClock)
	{
		ftcore::WorldClock SavedClock = Save.clock;
		if (Clock->IsFixedByCommandLine())
		{
			SavedClock.paused = true; // a capture run's fixed clock stays fixed
		}
		Clock->SetClock(SavedClock); // a save from before the clock: the world keeps its own time
	}
	if (UFTStoryStateSubsystem* State = StateOf(Story))
	{
		State->DecodeBlock(StoryBlock); // a save from before the block: a new game's state
		State->SyncAfterLoad(); // the runner stays where it was restored; the state follows it (StoryBridge::sync)
	}
	const UGameInstance* GI = Story->GetGameInstance();
	UFTNpcSubsystem* Npcs = GI ? GI->GetSubsystem<UFTNpcSubsystem>() : nullptr;
	FString NpcError;
	if (Npcs && ftcore::npcSaveBlockAction(Npcs->IsReady(), NpcSaveJson.IsEmpty()) == ftcore::NpcSaveBlockAction::Hold)
	{
		// The tables load later (UFTNpcSubsystem::Load applies it then); a save made meanwhile writes this block.
		Npcs->HoldSaveJson(NpcSaveJson);
		UE_LOG(LogForgottenThird, Log, TEXT("NPC memory block held until the NPC tables load: %d chars."), NpcSaveJson.Len());
	}
	else if (Npcs && NpcSaveJson.IsEmpty())
	{
		Npcs->ResetMemory(); // a save from before NPC memory: nobody remembers this session's deeds
	}
	else if (Npcs && !Npcs->ApplySaveJson(NpcSaveJson, NpcError))
	{
		// The story loads anyway; a damaged NPC block leaves everyone remembering nothing rather than this session.
		Npcs->ResetMemory();
		UE_LOG(LogForgottenThird, Warning, TEXT("NPC memory not loaded from the save: %s"), *NpcError);
	}
	else if (Npcs)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("NPC memory loaded from the save: %d chars."), NpcSaveJson.Len());
	}
	return true;
}
