#include "FTStepHost.h"

#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"
#include "TimerManager.h"

#include "ForgottenThird.h"
#include "FTGameInstance.h"
#include "FTStorySubsystem.h"
#include "FTText.h"

#include "ftcore/GameplayPlan.h"
#include "ftcore/StoryChain.h"
#include "ftcore/StoryRunner.h"
#include "ftcore/WorldLayout.h"

using ftcore::CombatKind;
using ftcore::GameplayScreen;
using ftcore::StoryRunner;

namespace
{
FString MapPackage(const FString& Map)
{
	return FT::ToFString(ftcore::mapPackage(FT::ToUtf8(Map)));
}

bool MapExists(const FString& Map)
{
	return !Map.IsEmpty() && FPackageName::DoesPackageExist(MapPackage(Map));
}

// Where a step plays, worked out before anything is changed or logged.
struct FStepRoute
{
	FString Map;
	GameplayScreen Screen = GameplayScreen::Stub;
	CombatKind Combat = CombatKind::Rhythm;
	// A step in one of the big worlds: the world's id, the district, the scene's style, and the world
	// the overworld hop starts from (empty: no hop).
	FString World;
	int32 District = -1;
	FString Style;
	FString FromWorld;
	// The scene's world map when it isn't built, and that world's id.
	FString MissingMap;
	FString MissingWorld;
	// The system a fight's encounter names, and why it can't play here when it can't (Combat is
	// Rhythm then).
	CombatKind Wanted = CombatKind::Rhythm;
	const TCHAR* FallbackWhy = nullptr;
};

enum class EPlaced : uint8
{
	NoWorld, // the scene isn't placed in a big world
	NotBuilt, // it is, but the world's map isn't built
	Placed,
};

// Puts a step on its scene's world map, or on the overworld first when the last field step was in
// another world.
EPlaced PlaceInWorld(const UFTGameInstance& GI, std::string_view Scene, FStepRoute& Route)
{
	const ftcore::WorldTable& Worlds = GI.GetWorlds();
	const ftcore::WorldScene* Placed = Worlds.findScene(Scene);
	if (!Placed)
	{
		return EPlaced::NoWorld;
	}
	const ftcore::WorldSpec& World = Worlds.worlds[Placed->world];
	const FString WorldMap = FT::ToFString(World.map);
	if (!MapExists(WorldMap))
	{
		Route.MissingMap = WorldMap;
		Route.MissingWorld = FT::ToFString(World.id);
		return EPlaced::NotBuilt;
	}
	Route.Map = WorldMap;
	Route.World = FT::ToFString(World.id);
	Route.District = static_cast<int32>(Placed->district);
	Route.Style = FT::ToFString(Placed->style.empty() ? World.style : Placed->style);
	const FString OverMap = FT::ToFString(Worlds.overworld.map);
	if (Worlds.overworld.present && !GI.LastWorld.IsEmpty() && GI.LastWorld != Route.World && MapExists(OverMap))
	{
		Route.Map = OverMap;
		Route.FromWorld = GI.LastWorld;
	}
	return EPlaced::Placed;
}

FStepRoute RouteStep(const UFTGameInstance& GI, std::string_view Scene, uint32 Beat, uint32 StepIndex, const ftcore::GameplayStep& Step)
{
	FStepRoute Route;
	Route.Screen = ftcore::screenRoutesTo(Step.screen);
	Route.Map = FTStep::ScreenMap(Route.Screen);
	if (Route.Screen == GameplayScreen::Field)
	{
		PlaceInWorld(GI, Scene, Route);
	}
	else if (Route.Screen == GameplayScreen::Battle)
	{
		const CombatKind Want = GI.GetEncounters().combatFor(Scene, Beat, StepIndex);
		Route.Wanted = Want;
		const CombatKind Play = ftcore::playableCombat(Want, FTStep::kPlayableCombat);
		if (Play != Want)
		{
			Route.FallbackWhy = TEXT("it has no screen yet");
		}
		else if (Play == CombatKind::Beat)
		{
			if (MapExists(FTStep::BeatMap()))
			{
				Route.Map = FTStep::BeatMap();
				Route.Combat = CombatKind::Beat;
			}
			else
			{
				Route.FallbackWhy = TEXT("the Beat map isn't built (FTBuildBeatWorld)");
			}
		}
		else if (Play == CombatKind::Arena)
		{
			// An arena fight plays in the scene's district, like a field step.
			FStepRoute Placed = Route;
			if (PlaceInWorld(GI, Scene, Placed) == EPlaced::Placed)
			{
				Route = Placed;
				Route.Combat = CombatKind::Arena;
			}
			else
			{
				Route.FallbackWhy = TEXT("the scene's world map isn't built");
			}
		}
	}
	else if (Route.Screen == GameplayScreen::Cutscene)
	{
		// Lookahead: a CG in a scene placed in a built world plays on that world's map, around the lead at
		// the district's spawn, so the field steps beside it play in place. One that would need the
		// overworld hop first plays on the stage, and the hop waits for the next field step (the route
		// keeps no world, so LastWorld stays as it was).
		FStepRoute Placed = Route;
		if (PlaceInWorld(GI, Scene, Placed) == EPlaced::Placed && Placed.FromWorld.IsEmpty())
		{
			Route = Placed;
		}
	}
	return Route;
}

// The runner is still on the carried beat, and the chain on the carried step.
bool OnCarriedBeat(const UFTGameInstance& GI, const StoryRunner& Runner, const FFTStoryCarry& Carry)
{
	const ftcore::StoryPosition Pos = Runner.position();
	return Runner.state() == StoryRunner::State::Gameplay && FT::ToFString(Pos.scene) == Carry.Scene && static_cast<int32>(Pos.beat) == Carry.Beat
		&& (!GI.Chain.active() || static_cast<int32>(GI.Chain.step()) == Carry.Step);
}
} // namespace

void FTStep::OpenMap(const UObject* WorldContext, const FString& Map, const FString& Options)
{
	UGameplayStatics::OpenLevel(WorldContext, FName(*MapPackage(Map)), true, Options);
}

const TCHAR* FTStep::ScreenMap(GameplayScreen Routed)
{
	switch (Routed)
	{
	case GameplayScreen::Battle: return TEXT("BattleDemo");
	case GameplayScreen::Field: return TEXT("Explore");
	case GameplayScreen::Choice: return TEXT("Choice");
	case GameplayScreen::Qte: return TEXT("Qte");
	case GameplayScreen::Cutscene: return TEXT("Cutscene");
	case GameplayScreen::Banter: return TEXT("Banter");
	case GameplayScreen::Mini: return TEXT("Mini");
	case GameplayScreen::Box:
	case GameplayScreen::Stub: break;
	}
	return TEXT("Stub");
}

EFTStepHost FTStep::HostFor(GameplayScreen Routed, CombatKind Combat)
{
	switch (Routed)
	{
	case GameplayScreen::Field: return EFTStepHost::Field;
	case GameplayScreen::Cutscene: return EFTStepHost::Cutscene;
	case GameplayScreen::Battle: return Combat == CombatKind::Arena ? EFTStepHost::Arena : EFTStepHost::None;
	default: return EFTStepHost::None;
	}
}

const TCHAR* FTStep::HostName(EFTStepHost Host)
{
	switch (Host)
	{
	case EFTStepHost::Field: return TEXT("field");
	case EFTStepHost::Cutscene: return TEXT("cutscene");
	case EFTStepHost::Arena: return TEXT("arena");
	case EFTStepHost::None: break;
	}
	return TEXT("none");
}

FString FTStep::Prepare(UFTGameInstance& GI, const StoryRunner& Runner, const FString& ReturnMap, bool bAutoPlay)
{
	const ftcore::StoryChain& Chain = GI.Chain;
	const ftcore::GameplayStep& Step = Chain.current();
	const ftcore::StoryPosition Pos = Runner.position();
	const FStepRoute Route = RouteStep(GI, Pos.scene, Pos.beat, Chain.step(), Step);

	FFTStoryCarry& Carry = GI.StoryCarry;
	Carry = FFTStoryCarry();
	Carry.bValid = true;
	Carry.ReturnMap = ReturnMap;
	Carry.Scene = FT::ToFString(Pos.scene);
	Carry.Beat = static_cast<int32>(Pos.beat);
	Carry.Kind = FT::ToFString(Runner.gameplayKindName());
	Carry.Screen = Route.Screen;
	Carry.Combat = Route.Combat;
	Carry.WantedCombat = Route.Wanted;
	Carry.Step = static_cast<int32>(Chain.step());
	Carry.StepCount = static_cast<int32>(Chain.count());
	Carry.StepLabel = FT::ToFString(Step.label());
	Carry.StepToken = FT::ToFString(Step.tokenName());
	Carry.World = Route.World;
	Carry.District = Route.District;
	Carry.Style = Route.Style;
	Carry.FromWorld = Route.FromWorld;
	Carry.bAutoPlay = bAutoPlay;

	if (!Route.World.IsEmpty())
	{
		GI.LastWorld = Route.World;
	}
	if (!Route.FromWorld.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Story: overworld hop from %s to %s for %s."), *Carry.FromWorld, *Carry.World, *Carry.Scene);
	}
	if (Route.Screen == GameplayScreen::Field && !Route.MissingMap.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: %s isn't built (FTBuildExploreWorld -World=%s); the field step uses %s."), *Route.MissingMap,
			*Route.MissingWorld, *Route.Map);
	}
	if (Route.FallbackWhy)
	{
		// Once a run per system, and never counted: every fight stays playable on the rhythm battle.
		const uint8 Bit = static_cast<uint8>(ftcore::combatBit(Route.Wanted));
		if ((GI.CombatFallbackWarned & Bit) == 0)
		{
			GI.CombatFallbackWarned |= Bit;
			const FString Wanted = FT::ToFString(ftcore::combatKindName(Route.Wanted));
			if (ftcore::playableCombat(Route.Wanted, kPlayableCombat) != Route.Wanted)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Story: %s fights play the rhythm battle for now (%s)."), *Wanted, Route.FallbackWhy);
			}
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Story: %s fights play the rhythm battle for now (%s)."), *Wanted, Route.FallbackWhy);
			}
		}
	}
	if (Route.Combat != CombatKind::Rhythm)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Story: the fight at %s beat %d step %d plays the %s system on %s."), *Carry.Scene, Carry.Beat,
			Carry.Step, *FT::ToFString(ftcore::combatKindName(Route.Combat)), *Route.Map);
	}
	if (Route.Screen == GameplayScreen::Cutscene && !Route.World.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Story: the CG at %s beat %d step %d plays on %s, in district %d."), *Carry.Scene, Carry.Beat, Carry.Step,
			*Route.Map, Route.District);
	}
	return Route.Map;
}

FTStep::EFinish FTStep::FinishCarried(UFTGameInstance& GI, const StoryRunner& Runner, const FFTStoryCarry& Carry)
{
	ftcore::StoryChain& Chain = GI.Chain;
	if (!Carry.bDone)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: back from the %s screen before it was done (step %d/%d at %s beat %d); the beat waits in the box."),
			*FT::ToFString(ftcore::gameplayScreenName(Carry.Screen)), Carry.Step + 1, Carry.StepCount, *Carry.Scene, Carry.Beat);
		++GI.Alpha.warnings;
		GI.FailedScene = Carry.Scene;
		GI.FailedBeat = Carry.Beat;
		Chain.reset();
		return EFinish::Failed;
	}
	GI.Alpha.addScreen(Carry.Screen);
	if (ftcore::screenRoutesTo(Carry.Screen) == GameplayScreen::Battle)
	{
		GI.Alpha.addFight(Carry.WantedCombat, Carry.Combat);
	}
	if (!OnCarriedBeat(GI, Runner, Carry))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: the story moved since the hand-off at %s beat %d; not resuming."), *Carry.Scene, Carry.Beat);
		++GI.Alpha.warnings;
		Chain.reset();
		return EFinish::Failed;
	}
	if (Chain.active() && Chain.finishStep(true) == ftcore::StoryChain::Next::Open)
	{
		return EFinish::Next;
	}
	return EFinish::Resume;
}

bool FTStep::ContinueHere(const UObject* WorldContext)
{
	UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	UFTGameInstance* GI = World ? World->GetGameInstance<UFTGameInstance>() : nullptr;
	UFTStepHostSubsystem* Hosts = World ? World->GetSubsystem<UFTStepHostSubsystem>() : nullptr;
	UFTStorySubsystem* Story = GI ? GI->GetSubsystem<UFTStorySubsystem>() : nullptr;
	if (!Hosts || !Story || !Story->IsLoaded() || !GI->StoryCarry.bValid || !GI->StoryCarry.bDone || !GI->Chain.active())
	{
		return false;
	}
	const StoryRunner& Runner = Story->GetRunner();
	if (!OnCarriedBeat(*GI, Runner, GI->StoryCarry))
	{
		return false; // the director on the story map reports it
	}
	// Peek at the next step on a copy: the chain moves on only if the step plays here.
	ftcore::StoryChain Peek = GI->Chain;
	if (Peek.finishStep(true) != ftcore::StoryChain::Next::Open)
	{
		return false; // the beat's last step: back to the story
	}
	const ftcore::StoryPosition Pos = Runner.position();
	const FStepRoute Route = RouteStep(*GI, Pos.scene, Pos.beat, Peek.step(), Peek.current());
	const EFTStepHost Host = HostFor(Route.Screen, Route.Combat);
	const FString Here = UGameplayStatics::GetCurrentLevelName(WorldContext, true);
	if (Host == EFTStepHost::None || !Hosts->Has(Host) || (Host != EFTStepHost::Cutscene && Route.Map != Here))
	{
		return false;
	}
	const FFTStoryCarry Done = GI->StoryCarry;
	if (FinishCarried(*GI, Runner, Done) != EFinish::Next)
	{
		// Can't happen after the checks above; leave the carry to the story map's director.
		GI->StoryCarry = Done;
		return false;
	}
	const FString Map = Prepare(*GI, Runner, Done.ReturnMap, Done.bAutoPlay);
	UE_LOG(LogForgottenThird, Log, TEXT("Story: step %u/%u %s at %s beat %u, in place on %s."), GI->Chain.step() + 1, GI->Chain.count(),
		*FT::ToFString(ftcore::gameplayScreenName(Route.Screen)), *Done.Scene, Runner.beatIndex(), *Here);
	Hosts->StartHere(Host, Host == EFTStepHost::Cutscene ? Map : Here);
	return true;
}

void UFTStepHostSubsystem::Register(EFTStepHost Kind, FFTStartStepHere Start)
{
	const int32 Index = static_cast<int32>(Kind);
	if (Kind == EFTStepHost::None || Index >= FTStep::kHostKinds)
	{
		return;
	}
	Hosts[Index] = MoveTemp(Start);
}

void UFTStepHostSubsystem::Unregister(EFTStepHost Kind, const UObject* Owner)
{
	const int32 Index = static_cast<int32>(Kind);
	if (Kind == EFTStepHost::None || Index >= FTStep::kHostKinds || !Owner)
	{
		return;
	}
	if (Hosts[Index].IsBoundToObject(Owner))
	{
		Hosts[Index].Unbind();
	}
}

bool UFTStepHostSubsystem::Has(EFTStepHost Kind) const
{
	const int32 Index = static_cast<int32>(Kind);
	return Kind != EFTStepHost::None && Index < FTStep::kHostKinds && Hosts[Index].IsBound();
}

bool UFTStepHostSubsystem::StartHere(EFTStepHost Kind, const FString& FallbackMap)
{
	UWorld* World = GetWorld();
	if (!World || !Has(Kind))
	{
		return false;
	}
	PendingKind = Kind;
	PendingMap = FallbackMap;
	World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &UFTStepHostSubsystem::StartPending));
	return true;
}

void UFTStepHostSubsystem::StartPending()
{
	const EFTStepHost Kind = PendingKind;
	PendingKind = EFTStepHost::None;
	if (Kind == EFTStepHost::None || Hosts[static_cast<int32>(Kind)].ExecuteIfBound())
	{
		return;
	}
	UE_LOG(LogForgottenThird, Warning, TEXT("Story: the %s host left before its step began; opening %s."), FTStep::HostName(Kind), *PendingMap);
	FTStep::OpenMap(this, PendingMap);
}
