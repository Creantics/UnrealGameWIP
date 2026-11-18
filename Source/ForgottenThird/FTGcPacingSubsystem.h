#pragma once

// Garbage collection at hidden moments (ftcore::GcPacing): a full collection where nobody sees the hitch, i.e. the pause
// menu opening, the unstick fade, and (later) fast travel and loads, once 30 s have passed since the last collection of
// any kind. OFF by default (11-18 ruling): only -GcHidden turns it on; without the flag HiddenMoment does nothing and
// the engine's GC settings are main's. The trial's engine settings (incremental reachability, purge ceiling) are passed
// as -dpcvars by the bench, never from the ini. Default-on waits for a trial under 20 ms and a 10-minute soak with no
// memory growth.
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
};
