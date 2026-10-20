#include "FTExploreDirector.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"

#include "ForgottenThird.h"
#include "FTExploreCharacter.h"
#include "FTExploreSpot.h"
#include "FTGameInstance.h"

#include "ftcore/ExploreDemo.h"

namespace
{
constexpr float kEncounterDelay = 1.2f;
// The autopilot gives up when it hasn't got 10 cm closer to its target for this long.
constexpr float kPilotStuckSeconds = 15.f;

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
}

double AFTExploreDirector::Now() const
{
	return FPlatformTime::Seconds();
}

void AFTExploreDirector::BeginPlay()
{
	Super::BeginPlay();
	bAutoPlay = bAutoPlay || FParse::Param(FCommandLine::Get(), TEXT("ExploreAutoPlay"));
	bQuitOnComplete = bQuitOnComplete || FParse::Param(FCommandLine::Get(), TEXT("ExploreQuitOnComplete"));

	Exploration = ftcore::demo::makeExploration();
	StartTime = Now();
	FindSpots();
	CacheText();
	RestoreFromCarry();
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

void AFTExploreDirector::Interact()
{
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
	if (bBattleOpening || PendingEncounter >= 0)
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
	const ftcore::Vec2 Goal = Exploration.item(Target).pos;
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
	const FVector2D Dir = To.GetSafeNormal();
	Player.AddMovementInput(FVector(Dir.X, Dir.Y, 0.0), 1.f);
}
