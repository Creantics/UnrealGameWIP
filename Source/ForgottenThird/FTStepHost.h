#pragma once

// Where each step of a gameplay beat plays, and the steps that play in place.
//
// The story director lowers a gameplay beat into steps (ftcore::GameplayPlan, kept in
// UFTGameInstance::Chain). FTStep::Prepare fills UFTGameInstance::StoryCarry for the chain's current
// step and picks the map that plays it:
//   - each screen's own map (FTStep::ScreenMap: BattleDemo, Explore, Stub, Choice, Qte, Cutscene,
//     Banter, Mini);
//   - for a field step in a scene placed in one of the big worlds (Data/Field/worlds.json), that
//     world's map when it is built, or the overworld first when the last field step was in another
//     world;
//   - for a fight, the combat system its encounter names (ftcore::CombatKind,
//     EncounterTable::combatFor on the scene, beat and step; Turns when untagged) when a screen for
//     it is built, else Turns, the rhythm battle, on BattleDemo. A beat fight opens the Beat map;
//     an arena fight plays on its world's map like a field step. StoryCarry.Combat says which system
//     the step plays. A system with no screen yet logs one warning per run, never a counted one;
//   - for a CG step in a scene placed in a big world, that world's map when it is built and no
//     overworld hop would come first: the cutscene plays around the lead at the district's spawn, so
//     the field steps beside it play in place. Otherwise the Cutscene stage, and the hop waits for the
//     next field step (LastWorld is left as it was).
//
// Steps in place: a screen whose step is done calls FTStep::ContinueHere before it opens
// StoryCarry.ReturnMap. When the beat's next step can play on this map, the done step is counted,
// the carry is filled for the next step and the host for it starts it on the next tick, with no map
// load. A host is an actor on this map that registered with UFTStepHostSubsystem: the field walk and
// an arena fight play in place on their own world's map only, a cutscene on any map. Otherwise
// ContinueHere returns false and the screen goes back to the story map as before; the director there
// counts the step and opens the next one. Either way FTStep::FinishCarried counts each step once.
//
// Logs carry scene ids, maps and counts only, never script text.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ftcore/CombatKind.h"
#include "ftcore/Dialogue.h"

#include "FTStepHost.generated.h"

class UFTGameInstance;
struct FFTStoryCarry;

namespace ftcore
{
class StoryRunner;
}

// Who can play a step in place (None: the step always opens its own map).
enum class EFTStepHost : uint8
{
	None,
	Field, // the field walk, on the step's world map
	Cutscene, // a cutscene staged around the lead, on any map
	Arena, // an arena fight, on the step's world map
};

namespace FTStep
{
inline constexpr int32 kHostKinds = 4;

// The combat systems that have a screen. The others play the rhythm battle (ftcore::playableCombat).
// Arena joins once its host is built; the Beat fights play the beat fight until the boss mode lands.
inline constexpr uint32 kPlayableCombat = ftcore::combatBit(ftcore::CombatKind::Rhythm) | ftcore::combatBit(ftcore::CombatKind::Beat);

// The map a screen opens when it doesn't play in place ("BattleDemo", "Explore", ...), and the beat
// fight's map ("Beat").
FORGOTTENTHIRD_API const TCHAR* ScreenMap(ftcore::GameplayScreen Routed);
inline const TCHAR* BeatMap() { return TEXT("Beat"); }

// Opens a map by its long name under /Game/Maps (ftcore::mapPackage), never by the short name the data,
// the screens and GetCurrentLevelName use, which the engine would resolve to the first package of that
// name anywhere under /Game, a stray local copy included. Every map the game opens goes through here.
FORGOTTENTHIRD_API void OpenMap(const UObject* WorldContext, const FString& Map, const FString& Options = FString());

// The host that plays a routed screen in place: Field, Cutscene, or Arena for a battle that plays the
// arena system.
FORGOTTENTHIRD_API EFTStepHost HostFor(ftcore::GameplayScreen Routed, ftcore::CombatKind Combat);
// "field", "cutscene", "arena" ("none"), for logs.
FORGOTTENTHIRD_API const TCHAR* HostName(EFTStepHost Host);

// Fills GI.StoryCarry for GI.Chain's current step (the chain must be active) of the runner's gameplay
// beat, sets GI.LastWorld for a step in a big world, and returns the map that plays the step.
// ReturnMap: the story map the screens go back to.
FORGOTTENTHIRD_API FString Prepare(UFTGameInstance& GI, const ftcore::StoryRunner& Runner, const FString& ReturnMap, bool bAutoPlay);

// What became of a step that came back.
enum class EFinish : uint8
{
	Next, // counted, and GI.Chain is on the beat's next step
	Resume, // counted, and that was the beat's last step: the story resumes
	Failed, // not done, or the story moved since the hand-off: logged and counted as a warning, chain reset
};

// Counts the carried step (GI.Alpha) and moves GI.Chain on. The one place a step is counted.
FORGOTTENTHIRD_API EFinish FinishCarried(UFTGameInstance& GI, const ftcore::StoryRunner& Runner, const FFTStoryCarry& Carry);

// For a screen whose step is done (StoryCarry.bDone), before it opens StoryCarry.ReturnMap: true when
// the beat's next step starts in place on this map (StoryCarry now holds that step); false when the
// screen should open ReturnMap as before.
FORGOTTENTHIRD_API bool ContinueHere(const UObject* WorldContext);
} // namespace FTStep

DECLARE_DELEGATE(FFTStartStepHere);

// The hosts on one map that can play a step in place. A host registers in BeginPlay with what starts
// its step (StoryCarry is already filled for it when that runs) and unregisters in EndPlay:
//
//   Hosts->Register(EFTStepHost::Field, FFTStartStepHere::CreateUObject(this, &AMyDirector::StartStepHere));
//   Hosts->Unregister(EFTStepHost::Field, this);
//
// One host per kind; a later Register replaces the earlier one.
UCLASS()
class FORGOTTENTHIRD_API UFTStepHostSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void Register(EFTStepHost Kind, FFTStartStepHere Start);
	// Only when Owner is the object the host's delegate is bound to.
	void Unregister(EFTStepHost Kind, const UObject* Owner);
	bool Has(EFTStepHost Kind) const;
	// Starts Kind's host on the next tick. If it is gone by then, opens FallbackMap, so the step still
	// plays. False (nothing scheduled) when there is no such host.
	bool StartHere(EFTStepHost Kind, const FString& FallbackMap);

private:
	void StartPending();

	FFTStartStepHere Hosts[FTStep::kHostKinds];
	EFTStepHost PendingKind = EFTStepHost::None;
	FString PendingMap;
};
