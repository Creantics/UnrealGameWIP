#pragma once

// Garbage collection at hidden moments (ftcore::GcPacing). Config/DefaultEngine.ini turns on incremental reachability
// (the mark phase sliced at 2 ms a frame) and raises the engine's periodic pass to 300 s, a ceiling; the game asks for
// a full collection where nobody sees the hitch: the pause menu opening, the unstick fade, and (later) fast travel
// and loads. UFTGcPacingSubsystem::HiddenMoment does it once 30 s have passed since the last collection of any kind.
// -GcNoHidden turns the hidden-moment collections off (the engine's pass alone); the fallback for the experimental
// incremental mode is -dpcvars=gc.AllowIncrementalReachability=0,gc.AllowIncrementalGather=0 or the two ini lines.
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ftcore/GcPacing.h"

#include "FTGcPacingSubsystem.generated.h"

UCLASS()
class FORGOTTENTHIRD_API UFTGcPacingSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// A moment the player can't see a hitch in (Why: for the log). Collects when the pacing allows it.
	static void HiddenMoment(const UObject* WorldContext, const TCHAR* Why);

private:
	void OnPostGarbageCollect();

	ftcore::GcPacing Pacing;
	FDelegateHandle PostGcHandle;
	bool bEnabled = true;
};
