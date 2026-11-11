#pragma once

// The host side of the shared combat inputs (docs/SHARED_COMBAT_INPUTS.md section 7): a UObject that holds one
// ftcore::FightSession, the count, the gift slot, the places, the table, the held refusal, the weak-point lock and an
// optional boss script on one fight clock. A host makes one per fight, calls Begin, then Tick every frame with the
// fight's delta (0 while its hit-stop holds, so the shared inputs see fight time frozen), and turns its input actions
// into Press calls. Core judges; this wrapper only forwards and logs.
//
// Lifetimes: the owner keeps this object in a UPROPERTY (or a TWeakObjectPtr it checks), and reads Get() and a
// Tick's result within the frame, never keeping them. The story binding lasts one fight: Begin drops it, so the
// host binds again after each Begin (a story state can't outlive a fight it was bound for). Allocation: the
// constructor makes the session once; Begin, Tick and Press allocate nothing (a refused Begin logs).

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ftcore/FightSession.h"

#include "Templates/UniquePtr.h"

#include "FTFightSession.generated.h"

// ftcore::SharedAction, for Blueprint-free C++ hosts and the binding table's adapter. Append only, in step with Core.
UENUM()
enum class EFTSharedAction : uint8
{
	Count,
	Gift,
	RefuseDown,
	RefuseUp,
	SetTable,
	LeaveOpen,
};

UCLASS()
class FORGOTTENTHIRD_API UFTFightSession : public UObject
{
	GENERATED_BODY()

public:
	UFTFightSession();

	// Starts a fight at StartSeconds on the fight clock; false (with a log line naming each piece) when a tuning was
	// refused and that piece kept its defaults.
	bool Begin(const ftcore::FightSessionDef& Def, double StartSeconds);
	// Moves the fight clock and every piece. FightDeltaSeconds is fight time: 0 during hit-stop.
	const ftcore::FightTick& Tick(float FightDeltaSeconds);
	// One shared-input press by Who (an ftcore::FighterId), aimed at Target where the action has one (a gift's lead).
	ftcore::SharedReply Press(EFTSharedAction Action, uint8 Who, uint8 Target = ftcore::kNoFighter);
	// The story state gift presses at a lead are written to, given or refused (both or neither), until the next Begin.
	void BindStory(ftcore::StoryState* Story, const ftcore::FactRegistry* Registry);

	ftcore::FightSession& Get() { return *Session; }
	const ftcore::FightSession& Get() const { return *Session; }

private:
	TUniquePtr<ftcore::FightSession> Session;
};
