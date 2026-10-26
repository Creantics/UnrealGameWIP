#include "FTExploreDirector.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "TimerManager.h"

#include "ForgottenThird.h"
#include "FTExploreCharacter.h"
#include "FTExploreSpot.h"
#include "FTFieldGuard.h"
#include "FTGameInstance.h"
#include "FTPartyFollower.h"
#include "FTStyleController.h"
#include "FTText.h"

#include "ftcore/BattleDemo.h"
#include "ftcore/ExploreDemo.h"
#include "ftcore/FieldGoals.h"
#include "ftcore/PathFollow.h"

#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr float kEncounterDelay = 1.2f;
// The autopilot gives up when it hasn't got 10 cm closer to its target for this long.
constexpr float kPilotStuckSeconds = 15.f;
// -WorldAutoPlay: a district counts as reached this close to its plaza's centre (metres); the
// tour's pilot targets get ids from here up, apart from the demo spots' and the field goal's.
constexpr float kTourReach = 4.f;
constexpr int32 kTourTargetBase = 300;

const TCHAR* const kObjectiveNames[ftcore::demo::kObjCount] = {
	TEXT("Talk to Duy"),
	TEXT("Find the light shards"),
	TEXT("Open the old gate"),
	TEXT("Win the ambush"),
};

const TCHAR* KindPrompt(ftcore::InteractKind Kind)
{
	switch (Kind)
	{
	case ftcore::InteractKind::Talk:
		return TEXT("E  Talk to Duy");
	case ftcore::InteractKind::Pickup:
		return TEXT("E  Pick up the light shard");
	case ftcore::InteractKind::Gate:
		return TEXT("E  Open the gate");
	default:
		return TEXT("");
	}
}

// Stream 1 adds UFTGameInstance::IsAlphaFast(); until then read the flag here.
bool IsAlphaFast()
{
	static const bool bFast = FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
	return bFast;
}

ftcore::Vec2 ToGround(const FVector& V)
{
	// Unreal centimetres to the demo's metres.
	return {static_cast<float>(V.X / 100.0), static_cast<float>(V.Y / 100.0)};
}
}

AFTExploreDirector::AFTExploreDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Exploration = ftcore::demo::makeExploration();
	Guards = ftcore::demo::makeGuards();
}

double AFTExploreDirector::Now() const
{
	return FPlatformTime::Seconds();
}

void AFTExploreDirector::BeginPlay()
{
	Super::BeginPlay();
	PilotPath.Reserve(64);
	bAutoPlay = bAutoPlay || FParse::Param(FCommandLine::Get(), TEXT("ExploreAutoPlay"));
	bQuitOnComplete = bQuitOnComplete || FParse::Param(FCommandLine::Get(), TEXT("ExploreQuitOnComplete"));

	Exploration = ftcore::demo::makeExploration();
	Guards = ftcore::demo::makeGuards();
	StartTime = Now();
	FindSpots();
	CacheText();
	RestoreFromCarry();
	BeginWorld();
	BeginStoryWalk();
	for (int32 I = 0; I < static_cast<int32>(Exploration.objectiveCount()); ++I)
	{
		UpdateObjectiveText(I);
	}
	if (bAutoPlay)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: autoplay on."));
	}
	// Back from the last fight: everything may be done already.
	CheckComplete();
}

void AFTExploreDirector::FindSpots()
{
	if (!WorldId.IsEmpty())
	{
		return; // a big world's map has no demo spots
	}
	const int32 Count = static_cast<int32>(Exploration.count());
	bool bHaveAll = Spots.Num() == Count;
	for (int32 I = 0; bHaveAll && I < Count; ++I)
	{
		bHaveAll = Spots[I] && Spots[I]->SpotId == I;
	}
	if (bHaveAll)
	{
		return;
	}
	Spots.Reset();
	Spots.SetNum(Count);
	for (TActorIterator<AFTExploreSpot> It(GetWorld()); It; ++It)
	{
		if (Spots.IsValidIndex(It->SpotId))
		{
			Spots[It->SpotId] = *It;
		}
	}
	for (int32 I = 0; I < Count; ++I)
	{
		if (!Spots[I])
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("ExploreDemo: no spot actor for spot %d."), I);
		}
	}
}

AFTExploreSpot* AFTExploreDirector::GetSpot(int32 Id) const
{
	return Spots.IsValidIndex(Id) ? Spots[Id].Get() : nullptr;
}

void AFTExploreDirector::CacheText()
{
	const int32 Objectives = static_cast<int32>(Exploration.objectiveCount());
	ObjectiveText.SetNum(Objectives);
	const int32 Count = static_cast<int32>(Exploration.count());
	PromptText.SetNum(Count);
	LockedText.SetNum(Count);
	for (int32 Id = 0; Id < Count; ++Id)
	{
		const ftcore::Interactable& It = Exploration.item(Id);
		PromptText[Id] = KindPrompt(It.kind);
		// Locked: name the first objective it waits on.
		LockedText[Id].Reset();
		for (int32 O = 0; O < Objectives; ++O)
		{
			if ((It.needs >> O) & 1u)
			{
				LockedText[Id] = FString::Printf(TEXT("Locked: %s first"), kObjectiveNames[O]);
				break;
			}
		}
	}
}

void AFTExploreDirector::UpdateObjectiveText(int32 Objective)
{
	if (!ObjectiveText.IsValidIndex(Objective))
	{
		return;
	}
	const int32 Required = Exploration.required(Objective);
	if (Required > 1)
	{
		ObjectiveText[Objective] = FString::Printf(TEXT("%s  %d/%d"), kObjectiveNames[Objective], Exploration.progress(Objective), Required);
	}
	else
	{
		ObjectiveText[Objective] = kObjectiveNames[Objective];
	}
}

const FString& AFTExploreDirector::GetObjectiveText(int32 Objective) const
{
	static const FString Empty;
	return ObjectiveText.IsValidIndex(Objective) ? ObjectiveText[Objective] : Empty;
}

const FString& AFTExploreDirector::GetPrompt() const
{
	static const FString Empty;
	static const FString BackToStory(TEXT("E  Back to the story"));
	static const FString UseMarker(TEXT("E  Use the marker"));
	if (bStoryWalk)
	{
		if (bStoryReturning || !ftcore::demo::fieldStepUsable(bFieldReached, FieldElapsed))
		{
			return Empty;
		}
		return bFieldReached ? UseMarker : BackToStory;
	}
	if (Focus < 0 || !PromptText.IsValidIndex(Focus))
	{
		return Empty;
	}
	return Exploration.isAvailable(Focus) ? PromptText[Focus] : LockedText[Focus];
}

double AFTExploreDirector::GetNoticeAge() const
{
	return Notice.IsEmpty() ? 1e9 : Now() - NoticeTime;
}

double AFTExploreDirector::GetBannerAge() const
{
	return Banner.IsEmpty() ? 1e9 : Now() - BannerTime;
}

void AFTExploreDirector::SetNotice(const FString& Text)
{
	Notice = Text;
	NoticeTime = Now();
}

void AFTExploreDirector::SetBanner(const FString& Text)
{
	Banner = Text;
	BannerTime = Now();
}

AFTExploreCharacter* AFTExploreDirector::GetPlayerCharacter() const
{
	return Cast<AFTExploreCharacter>(UGameplayStatics::GetPlayerPawn(this, 0));
}

void AFTExploreDirector::RestoreFromCarry()
{
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI || !GI->ExploreCarry.bValid)
	{
		return;
	}
	FFTExploreCarry& Carry = GI->ExploreCarry;
	if (Carry.ReturnMap != UGameplayStatics::GetCurrentLevelName(this, true))
	{
		return;
	}
	Exploration.load(Carry.State);
	if (Carry.bWon && Carry.Encounter >= 0)
	{
		Exploration.clearEncounter(Carry.Encounter);
		SetBanner(TEXT("Ambush cleared"));
		UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: back from the fight, ambush cleared."));
	}
	Guards.load(Carry.Guards);
	if (Carry.Guard >= 0)
	{
		if (Carry.bWon)
		{
			Guards.defeat(Carry.Guard);
			SetBanner(TEXT("Guard defeated"));
			UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: back from the fight, guard %d defeated."), Carry.Guard);
		}
		else
		{
			Guards.reset(Carry.Guard);
		}
	}
	// Nobody pounces the moment the field opens again.
	Guards.holdOff(ftcore::demo::kGuardHoldOffSeconds);
	for (int32 Id = 0; Id < static_cast<int32>(Exploration.count()); ++Id)
	{
		if (Exploration.isUsed(Id))
		{
			if (AFTExploreSpot* Spot = GetSpot(Id))
			{
				Spot->MarkUsed(true);
			}
		}
	}
	StartTime = Now() - Carry.Seconds;
	bRestorePending = true;
	Carry.bValid = false;
	// The pawn may not exist yet; Tick moves it once it does.
	if (AFTExploreCharacter* Player = GetPlayerCharacter())
	{
		Player->SetActorLocationAndRotation(Carry.Location, Carry.Rotation, false, nullptr, ETeleportType::TeleportPhysics);
		bRestorePending = false;
	}
}

void AFTExploreDirector::BeginStoryWalk()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI || !GI->StoryCarry.bValid || GI->StoryCarry.bDone || GI->StoryCarry.Screen != ftcore::GameplayScreen::Field)
	{
		return;
	}
	bStoryWalk = true;
	bAutoPlay = bAutoPlay || GI->StoryCarry.bAutoPlay;
	StoryWalkStart = Now();
	for (const TObjectPtr<AFTExploreSpot>& Spot : Spots)
	{
		if (Spot)
		{
			Spot->SetActorHiddenInGame(true);
			Spot->SetActorEnableCollision(false);
		}
	}
	FieldElapsed = 0.f;
	bFieldReached = false;
	const FFTStoryCarry& Carry = GI->StoryCarry;
	const FTCHARToUTF8 SceneUtf8(*Carry.Scene);
	const size_t GoalIndex = ftcore::demo::fieldGoalIndex(std::string_view(SceneUtf8.Get(), static_cast<size_t>(SceneUtf8.Length())), Carry.Beat, Carry.Step);
	FieldGoal = ftcore::demo::kFieldGoals[GoalIndex];
	bFieldHasVia = false;
	bFieldViaDone = false;
	bool bFromTable = false;
	{
		// Data/Field/goals.json: this step's own marker (and waypoint); the hashed default when the
		// file or the entry is missing.
		ftcore::FieldGoalTable Table;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Field/goals.json");
		TArray<uint8> Bytes;
		if (IFileManager::Get().FileExists(*File) && FFileHelper::LoadFileToArray(Bytes, *File))
		{
			std::string Error;
			if (ftcore::importFieldGoalTable(FT::BytesAsText(Bytes), Table, &Error))
			{
				const ftcore::FieldGoalEntry* Entry = Table.findEntry(std::string_view(SceneUtf8.Get(), static_cast<size_t>(SceneUtf8.Length())),
					static_cast<uint32>(FMath::Max(Carry.Beat, 0)), static_cast<uint32>(FMath::Max(Carry.Step, 0)));
				if (Entry)
				{
					FieldGoal = Entry->goal;
					FieldVia = Entry->via;
					bFieldHasVia = Entry->hasVia;
					bFromTable = true;
				}
			}
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("ExploreDemo: Data/Field/goals.json refused; using the default markers: %s"), *FT::ToFString(Error).TrimEnd());
			}
		}
	}
	if (Layout)
	{
		// A big world: the goal and waypoint are in the district's frame; the player starts at its spawn.
		const int32 Count = static_cast<int32>(Layout->districtCount());
		const bool bPlaced = Carry.World == WorldId && Carry.District >= 0 && Carry.District < Count;
		WorldDistrict = bPlaced ? Carry.District : 0;
		if (!bPlaced)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("World %s: the field step's scene %s isn't placed here; using district 0."), *WorldId, *Carry.Scene);
		}
		const size_t D = static_cast<size_t>(WorldDistrict);
		FieldGoal = Layout->toWorld(D, FieldGoal);
		FieldVia = Layout->toWorld(D, FieldVia);
		const ftcore::Vec2 Spawn = Layout->district(D).origin;
		WorldSpawn = FVector(Spawn.x * 100.0, Spawn.y * 100.0, 0.0);
		WorldSpawnRotation = FRotator(0.f, FMath::RadiansToDegrees(Layout->spawnYaw(D)), 0.f);
		bWorldSpawnPending = true;
		if (!Carry.Style.IsEmpty())
		{
			for (TActorIterator<AFTStyleController> It(GetWorld()); It; ++It)
			{
				// Before its BeginPlay the controller applies StyleSpec itself; after it, switch now.
				It->StyleSpec = Carry.Style;
				if (It->HasActorBegunPlay())
				{
					It->SetStyleSpec(Carry.Style);
				}
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("World %s: field step in district %d (style %s)."), *WorldId, WorldDistrict,
			Carry.Style.IsEmpty() ? TEXT("the map's") : *Carry.Style);
	}
	SpawnFieldGoal();
	FieldObjective = Carry.StepLabel.IsEmpty()
		? FString::Printf(TEXT("Reach the marker  %s"), *Carry.Kind)
		: FString::Printf(TEXT("Reach the marker  %s  %s"), *Carry.Kind, *Carry.StepLabel);
	SetBanner(TEXT("Reach the marker"));
	if (bAutoPlay && IsAlphaFast())
	{
		UGameplayStatics::SetGlobalTimeDilation(this, 8.f);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: field step for %s at %s beat %d step %d, marker %d%s."), *Carry.Kind, *Carry.Scene, Carry.Beat, Carry.Step, static_cast<int32>(GoalIndex), bFromTable ? TEXT(" (table)") : TEXT(""));
}

void AFTExploreDirector::SpawnFieldGoal()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AFTExploreSpot> It(World); It; ++It)
	{
		if (It->SpotId == ftcore::demo::kFieldGoalSpotId)
		{
			GoalSpot = *It;
		}
	}
	const FTransform At(FVector(FieldGoal.x * 100.0, FieldGoal.y * 100.0, 0.0));
	if (GoalSpot)
	{
		GoalSpot->SetActorTransform(At);
	}
	else
	{
		GoalSpot = World->SpawnActorDeferred<AFTExploreSpot>(AFTExploreSpot::StaticClass(), At, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (GoalSpot)
		{
			GoalSpot->SpotId = ftcore::demo::kFieldGoalSpotId;
			GoalSpot->FinishSpawning(At);
		}
	}
	if (GoalSpot)
	{
		GoalSpot->SetActorHiddenInGame(false);
	}
}

void AFTExploreDirector::SpawnFollowers(AFTExploreCharacter& Player)
{
	bFollowersSpawned = true;
	UWorld* World = GetWorld();
	const size_t Count = FMath::Min(ftcore::demo::kPartyNames.size() - 1u, ftcore::PartyTrail::kMaxFollowers);
	const FVector2D Facing = Player.GetFacing2D();
	Trail.reset(ToGround(Player.GetActorLocation()), {static_cast<float>(Facing.X), static_cast<float>(Facing.Y)},
		static_cast<float>(Count) * ftcore::PartyTrail::kFollowerSpacing);
	Followers.Reserve(static_cast<int32>(Count));
	for (size_t I = 0; World && I < Count; ++I)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AFTPartyFollower* Follower = World->SpawnActor<AFTPartyFollower>(AFTPartyFollower::StaticClass(), Player.GetActorTransform(), Params);
		if (!Follower)
		{
			continue;
		}
		const FString Name = UTF8_TO_TCHAR(ftcore::demo::kPartyNames[I + 1u]);
		if (!Follower->Setup(Name, &Player))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("ExploreDemo: no mesh for follower %s."), *Name);
		}
		Followers.Add(Follower);
	}
	PlaceFollowers(Player, 0.f);
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: %d following the lead."), Followers.Num());
}

void AFTExploreDirector::PlaceFollowers(AFTExploreCharacter& Player, float DeltaSeconds)
{
	Trail.push(ToGround(Player.GetActorLocation()));
	const float FeetZ = static_cast<float>(Player.GetActorLocation().Z - Player.GetSimpleCollisionHalfHeight());
	for (int32 I = 0; I < Followers.Num(); ++I)
	{
		if (AFTPartyFollower* Follower = Followers[I])
		{
			const ftcore::Vec2 At = Trail.pointBehind(static_cast<float>(I + 1) * ftcore::PartyTrail::kFollowerSpacing);
			Follower->Place(FVector2D(At.x * 100.0, At.y * 100.0), FeetZ, DeltaSeconds);
		}
	}
}

void AFTExploreDirector::SpawnGuards(AFTExploreCharacter& Player)
{
	bGuardsSpawned = true;
	UWorld* World = GetWorld();
	const int32 Count = static_cast<int32>(Guards.count());
	GuardActors.SetNum(Count);
	int32 OnPatrol = 0;
	for (int32 I = 0; World && I < Count; ++I)
	{
		if (!Guards.active(I))
		{
			continue;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AFTFieldGuard* Guard = World->SpawnActor<AFTFieldGuard>(AFTFieldGuard::StaticClass(), FTransform::Identity, Params);
		if (Guard)
		{
			Guard->Setup(&Player);
			++OnPatrol;
		}
		GuardActors[I] = Guard;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: %d guards on patrol."), OnPatrol);
}

int32 AFTExploreDirector::TickGuards(AFTExploreCharacter& Player, float DeltaSeconds)
{
	if (!bGuardsSpawned)
	{
		SpawnGuards(Player);
	}
	const ftcore::Vec2 Pos = ToGround(Player.GetActorLocation());
	// Core does the cone and the distances; a line trace answers for the walls, only for the guards
	// close enough to need one.
	uint32 Sight = 0;
	for (int32 I = 0; I < GuardActors.Num(); ++I)
	{
		const AFTFieldGuard* Guard = GuardActors[I];
		if (Guard && Guards.inSenseRange(I, Pos) && Guard->CanSee(Player))
		{
			Sight |= 1u << I;
		}
	}
	ftcore::GuardEvents Events;
	const int32 Caught = Guards.step(Pos, Sight, DeltaSeconds, &Events);
	const float FeetZ = static_cast<float>(Player.GetActorLocation().Z - Player.GetSimpleCollisionHalfHeight());
	for (int32 I = 0; I < GuardActors.Num(); ++I)
	{
		AFTFieldGuard* Guard = GuardActors[I];
		if (!Guard)
		{
			continue;
		}
		if (!Guards.active(I))
		{
			Guard->SetActorHiddenInGame(true);
			continue;
		}
		const ftcore::Vec2 At = Guards.position(I);
		const ftcore::Vec2 Facing = Guards.facing(I);
		Guard->Place(FVector2D(At.x * 100.0, At.y * 100.0), FVector2D(Facing.x, Facing.y), Guards.state(I), FeetZ);
		const uint8 Bits = Events.bits[static_cast<size_t>(I)];
		if (Bits & ftcore::kGuardNoticed)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: guard %d noticed you."), I);
		}
		if (Bits & ftcore::kGuardChaseStarted)
		{
			SetNotice(TEXT("A guard has spotted you"));
			UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: guard %d gives chase."), I);
		}
		if (Bits & ftcore::kGuardLost)
		{
			SetNotice(TEXT("The guard lost you"));
			UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: guard %d lost you."), I);
		}
	}
	return Caught;
}

void AFTExploreDirector::TickStoryWalk(AFTExploreCharacter& Player, float DeltaSeconds)
{
	FieldElapsed += DeltaSeconds;
	if (!bFollowersSpawned)
	{
		SpawnFollowers(Player);
	}
	else
	{
		PlaceFollowers(Player, DeltaSeconds);
	}
	const ftcore::Vec2 Pos = ToGround(Player.GetActorLocation());
	const FVector2D Facing = Player.GetFacing2D();
	const bool bReached = ftcore::demo::fieldGoalInReach(Pos, {static_cast<float>(Facing.X), static_cast<float>(Facing.Y)}, FieldGoal);
	if (bReached != bFieldReached)
	{
		bFieldReached = bReached;
		if (GoalSpot)
		{
			GoalSpot->SetFocused(bReached);
		}
	}
	if (!bAutoPlay || bStoryReturning)
	{
		return;
	}
	if (bFieldReached)
	{
		Interact();
		return;
	}
	const ftcore::Vec2 Aim = ftcore::demo::fieldPilotTarget(Pos, FieldGoal, FieldVia, bFieldHasVia, bFieldViaDone);
	// The waypoint and the marker count as separate targets for the stuck detection.
	PilotWalk(Player, Pos, Aim, bFieldHasVia && !bFieldViaDone ? ftcore::demo::kFieldGoalSpotId + 1 : ftcore::demo::kFieldGoalSpotId, DeltaSeconds);
}

void AFTExploreDirector::ReturnToStory()
{
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (bStoryReturning || !GI || !GI->StoryCarry.bValid)
	{
		return;
	}
	bStoryReturning = true;
	GI->StoryCarry.bDone = true;
	GI->StoryCarry.bWon = true;
	SetBanner(TEXT("Back to the story"));
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: field step done at %s beat %d step %d after %.1f s."), *GI->StoryCarry.Scene, GI->StoryCarry.Beat, GI->StoryCarry.Step, FieldElapsed);
	FTimerHandle Timer;
	GetWorldTimerManager().SetTimer(Timer, this, &AFTExploreDirector::OpenStoryMap, 0.8f, false);
}

void AFTExploreDirector::OpenStoryMap()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->StoryCarry.bValid)
	{
		UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
		UGameplayStatics::OpenLevel(this, FName(*GI->StoryCarry.ReturnMap));
	}
}

void AFTExploreDirector::Interact()
{
	if (bStoryWalk)
	{
		if (ftcore::demo::fieldStepUsable(bFieldReached, FieldElapsed))
		{
			ReturnToStory();
		}
		return;
	}
	if (Focus < 0 || bComplete)
	{
		return;
	}
	const int32 Id = Focus;
	const ftcore::InteractKind Kind = Exploration.item(Id).kind;
	const ftcore::InteractResult Result = Exploration.interact(Id);
	if (Result == ftcore::InteractResult::Locked)
	{
		SetNotice(LockedText[Id]);
		return;
	}
	if (Result != ftcore::InteractResult::Used)
	{
		return;
	}
	if (AFTExploreSpot* Spot = GetSpot(Id))
	{
		Spot->MarkUsed(false);
	}
	const int32 Objective = Exploration.item(Id).objective;
	UpdateObjectiveText(Objective);
	switch (Kind)
	{
	case ftcore::InteractKind::Talk:
		SetNotice(TEXT("Talked to Duy"));
		break;
	case ftcore::InteractKind::Pickup:
		SetNotice(FString::Printf(TEXT("Light shard  %d/%d"), Exploration.progress(Objective), Exploration.required(Objective)));
		break;
	case ftcore::InteractKind::Gate:
		SetNotice(TEXT("The gate opens"));
		break;
	default:
		break;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: used spot %d after %.1f s."), Id, Now() - StartTime);
	Focus = -1;
	CheckComplete();
}

void AFTExploreDirector::StartEncounter(int32 Id)
{
	PendingEncounter = Id;
	SetBanner(TEXT("Ambush!"));
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: ambush at spot %d after %.1f s."), Id, Now() - StartTime);
	FTimerHandle Timer;
	GetWorldTimerManager().SetTimer(Timer, this, &AFTExploreDirector::OpenBattle, kEncounterDelay, false);
}

void AFTExploreDirector::StartGuardFight(int32 Id)
{
	PendingGuard = Id;
	SetBanner(TEXT("Caught!"));
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: guard %d caught you after %.1f s."), Id, Now() - StartTime);
	FTimerHandle Timer;
	GetWorldTimerManager().SetTimer(Timer, this, &AFTExploreDirector::OpenBattle, kEncounterDelay, false);
}

void AFTExploreDirector::OpenBattle()
{
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("ExploreDemo: the game instance isn't UFTGameInstance, so the fight can't return here."));
		return;
	}
	FFTExploreCarry& Carry = GI->ExploreCarry;
	Carry.bValid = true;
	Carry.ReturnMap = UGameplayStatics::GetCurrentLevelName(this, true);
	Carry.State = Exploration.save();
	Carry.Encounter = PendingEncounter;
	Carry.Guard = PendingGuard;
	Carry.Guards = Guards.save();
	Carry.bWon = false;
	Carry.Seconds = Now() - StartTime;
	if (const AFTExploreCharacter* Player = GetPlayerCharacter())
	{
		Carry.Location = Player->GetActorLocation();
		Carry.Rotation = Player->GetActorRotation();
	}
	bBattleOpening = true;
	UGameplayStatics::OpenLevel(this, BattleMap);
}

void AFTExploreDirector::CheckComplete()
{
	if (bComplete || !Exploration.allDone())
	{
		return;
	}
	bComplete = true;
	SetBanner(TEXT("Demo complete"));
	UE_LOG(LogForgottenThird, Display, TEXT("ExploreDemo: COMPLETE after %.1f s."), Now() - StartTime);
	if (bQuitOnComplete)
	{
		FPlatformMisc::RequestExit(false, TEXT("ExploreDemo"));
	}
}

void AFTExploreDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	AFTExploreCharacter* Player = GetPlayerCharacter();
	if (!Player)
	{
		return;
	}
	if (bRestorePending)
	{
		if (const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
		{
			Player->SetActorLocationAndRotation(GI->ExploreCarry.Location, GI->ExploreCarry.Rotation, false, nullptr, ETeleportType::TeleportPhysics);
		}
		bRestorePending = false;
		CheckComplete();
	}
	if (bWorldSpawnPending)
	{
		bWorldSpawnPending = false;
		const FVector At(WorldSpawn.X, WorldSpawn.Y, Player->GetActorLocation().Z);
		Player->SetActorLocationAndRotation(At, WorldSpawnRotation, false, nullptr, ETeleportType::TeleportPhysics);
		if (AController* Controller = Player->GetController())
		{
			Controller->SetControlRotation(WorldSpawnRotation);
		}
	}
	if (bStoryWalk)
	{
		TickStoryWalk(*Player, DeltaSeconds);
		return;
	}
	if (Layout)
	{
		// A big world has no demo spots or guards: a free walk, or the -WorldAutoPlay tour.
		TickWorld(*Player, DeltaSeconds);
		return;
	}
	if (bBattleOpening || PendingEncounter >= 0 || PendingGuard >= 0)
	{
		return;
	}

	const ftcore::Vec2 Pos = ToGround(Player->GetActorLocation());
	const FVector2D Facing = Player->GetFacing2D();
	const int32 NewFocus = bComplete ? -1 : Exploration.focus(Pos, {static_cast<float>(Facing.X), static_cast<float>(Facing.Y)});
	if (NewFocus != Focus)
	{
		if (AFTExploreSpot* Old = GetSpot(Focus))
		{
			Old->SetFocused(false);
		}
		if (AFTExploreSpot* New = GetSpot(NewFocus))
		{
			New->SetFocused(true);
		}
		Focus = NewFocus;
	}

	const int32 Caught = TickGuards(*Player, DeltaSeconds);
	if (Caught >= 0)
	{
		StartGuardFight(Caught);
		return;
	}
	const int32 Encounter = Exploration.encounterAt(Pos);
	if (Encounter >= 0)
	{
		StartEncounter(Encounter);
		return;
	}
	if (bAutoPlay && !bComplete)
	{
		TickAutoPlay(*Player, DeltaSeconds);
	}
}

void AFTExploreDirector::TickAutoPlay(AFTExploreCharacter& Player, float DeltaSeconds)
{
	const ftcore::Vec2 Pos = ToGround(Player.GetActorLocation());
	const int32 Target = Exploration.pilotTarget(Pos);
	if (Target < 0)
	{
		return;
	}
	if (Target == Focus && Exploration.item(Target).kind != ftcore::InteractKind::Encounter)
	{
		Interact();
		return;
	}
	PilotWalk(Player, Pos, Exploration.item(Target).pos, Target, DeltaSeconds);
}

void AFTExploreDirector::PilotWalk(AFTExploreCharacter& Player, ftcore::Vec2 Pos, ftcore::Vec2 Goal, int32 Target, float DeltaSeconds,
	bool bUseNav)
{
	const FVector2D To(Goal.x - Pos.x, Goal.y - Pos.y);
	const float Dist = static_cast<float>(To.Size());
	if (Target != PilotTarget || Dist < PilotBestDist - 0.1f)
	{
		PilotTarget = Target;
		PilotBestDist = Dist;
		PilotStuck = 0.f;
	}
	else
	{
		PilotStuck += DeltaSeconds;
		if (PilotStuck > kPilotStuckSeconds)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("ExploreDemo: STUCK %.1f m from spot %d."), Dist, Target);
			PilotStuck = -1e9f; // log once
			if (bQuitOnComplete)
			{
				FPlatformMisc::RequestExit(false, TEXT("ExploreDemo"));
			}
		}
	}
	FVector2D Dir = To.GetSafeNormal();
	if (const ftcore::Vec2* Waypoint = bUseNav ? PilotWaypoint(Player, Pos, Goal, Target) : nullptr)
	{
		Dir = FVector2D(Waypoint->x - Pos.x, Waypoint->y - Pos.y).GetSafeNormal();
	}
	Player.AddMovementInput(FVector(Dir.X, Dir.Y, 0.0), 1.f);
}

const ftcore::Vec2* AFTExploreDirector::PilotWaypoint(const AFTExploreCharacter& Player, ftcore::Vec2 Pos, ftcore::Vec2 Goal, int32 Target)
{
	UWorld* World = GetWorld();
	UNavigationSystemV1* Nav = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
	if (!Nav)
	{
		return nullptr;
	}
	// The path is found about once a second (and on a new target), not every frame.
	const double Now = World->GetTimeSeconds();
	if (Target != PilotPathTarget || Now >= PilotPathRefreshAt)
	{
		PilotPathTarget = Target;
		PilotPathRefreshAt = Now + 1.0;
		PilotPath.Reset();
		PilotPathNext = 1;
		const FVector From = Player.GetActorLocation();
		const FVector To(Goal.x * 100.0, Goal.y * 100.0, From.Z);
		const UNavigationPath* Path = Nav->FindPathToLocationSynchronously(World, From, To);
		if (Path && Path->IsValid())
		{
			for (const FVector& Point : Path->PathPoints)
			{
				PilotPath.Add(ToGround(Point));
			}
			if (!bPilotPathLogged)
			{
				bPilotPathLogged = true;
				UE_LOG(LogForgottenThird, Display, TEXT("Explore: pilot follows a navmesh path (%d points)."), PilotPath.Num());
			}
		}
	}
	if (PilotPath.Num() < 2)
	{
		return nullptr;
	}
	PilotPathNext = ftcore::advanceWaypoint(PilotPath.GetData(), PilotPath.Num(), PilotPathNext, Pos, 0.6f);
	return PilotPathNext >= 0 ? &PilotPath[PilotPathNext] : nullptr;
}

void AFTExploreDirector::BeginWorld()
{
	if (WorldId.IsEmpty())
	{
		return;
	}
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI)
	{
		return;
	}
	const ftcore::WorldTable& Worlds = GI->GetWorlds();
	const FTCHARToUTF8 IdUtf8(*WorldId);
	const int32 Index = Worlds.findWorld(std::string_view(IdUtf8.Get(), static_cast<size_t>(IdUtf8.Length())));
	if (Index < 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("World %s: not in Data/Field/worlds.json; the map plays as a plain field."), *WorldId);
		return;
	}
	Layout = MakeUnique<ftcore::WorldLayout>();
	std::string Error;
	if (!ftcore::generateWorldLayout(Worlds.worlds[static_cast<size_t>(Index)], *Layout, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("World %s: %s"), *WorldId, *FT::ToFString(Error).TrimEnd());
		Layout.Reset();
		return;
	}
	WorldDistrict = 0;
	UE_LOG(LogForgottenThird, Display, TEXT("World %s: %d districts, %d roads, %d buildings, %d props over %.0f m."), *WorldId,
		static_cast<int32>(Layout->districtCount()), static_cast<int32>(Layout->roadCount()), static_cast<int32>(Layout->blockCount()),
		static_cast<int32>(Layout->propCount()), Layout->side());
	const FFTStoryCarry& Carry = GI->StoryCarry;
	const bool bStoryStep = Carry.bValid && !Carry.bDone && Carry.Screen == ftcore::GameplayScreen::Field;
	bWorldTour = !bStoryStep && FParse::Param(FCommandLine::Get(), TEXT("WorldAutoPlay"));
	if (!bWorldTour)
	{
		return;
	}
	TourCount = static_cast<int32>(Layout->tour(0, Tour, UE_ARRAY_COUNT(Tour)));
	TourLeg = 0;
	TourStart = Now();
	TourGameStart = GetWorld()->GetTimeSeconds();
	if (IsAlphaFast())
	{
		UGameplayStatics::SetGlobalTimeDilation(this, 8.f);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("World %s: tour of %d legs over the roads."), *WorldId, TourCount);
}

void AFTExploreDirector::UpdateWorldLine(ftcore::Vec2 Pos)
{
	const int32 At = Layout->districtAt(Pos);
	if (At == WorldLineDistrict)
	{
		return;
	}
	WorldLineDistrict = At;
	WorldLine = At >= 0
		? FString::Printf(TEXT("%s  district %d of %d"), *WorldId, At + 1, static_cast<int32>(Layout->districtCount()))
		: FString::Printf(TEXT("%s  open ground"), *WorldId);
}

void AFTExploreDirector::TickWorld(AFTExploreCharacter& Player, float DeltaSeconds)
{
	if (!bFollowersSpawned)
	{
		SpawnFollowers(Player);
	}
	else
	{
		PlaceFollowers(Player, DeltaSeconds);
	}
	const ftcore::Vec2 Pos = ToGround(Player.GetActorLocation());
	UpdateWorldLine(Pos);
	if (!bWorldTour || bWorldTourDone || TourLeg >= TourCount)
	{
		return;
	}
	const int32 District = Tour[TourLeg];
	const ftcore::Vec2 Target = Layout->district(static_cast<size_t>(District)).centre;
	if (FVector2D(Target.x - Pos.x, Target.y - Pos.y).Size() > kTourReach)
	{
		// Roads run straight between plaza centres and nothing stands on them, so no navmesh is needed.
		PilotWalk(Player, Pos, Target, kTourTargetBase + TourLeg, DeltaSeconds, false);
		return;
	}
	const double Game = GetWorld()->GetTimeSeconds() - TourGameStart;
	UE_LOG(LogForgottenThird, Display, TEXT("World %s: reached district %d (leg %d/%d) at %.1f s game time."), *WorldId, District,
		TourLeg + 1, TourCount, Game);
	if (++TourLeg < TourCount)
	{
		return;
	}
	bWorldTourDone = true;
	UE_LOG(LogForgottenThird, Display, TEXT("World %s: TOUR DONE, all %d districts in %d legs, %.1f s game time (%.1f s real)."), *WorldId,
		static_cast<int32>(Layout->districtCount()), TourCount, Game, Now() - TourStart);
	UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
	if (bQuitOnComplete || FParse::Param(FCommandLine::Get(), TEXT("WorldQuitOnDone")))
	{
		FPlatformMisc::RequestExit(false, TEXT("WorldTour"));
	}
}
