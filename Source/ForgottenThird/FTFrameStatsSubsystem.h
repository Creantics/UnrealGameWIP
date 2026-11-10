#pragma once

// Frame timing for a profile run (any map, any mode):
//
//   ... -game -FrameStats [-FrameStatsSkip=60] [-FrameStatsCensusAt=120]
//
// Every frame after the first -FrameStatsSkip frames (and under 2 s, so a load hitch doesn't count) keeps its frame,
// game-thread, render-thread and GPU time, draw calls and primitives drawn (ftcore::FrameStats, one buffer made at the
// start). At frame -FrameStatsCensusAt the world's actors, registered static mesh and instanced components, instances,
// and registered local lights (all, and visible) are counted once. When the world ends the medians, p95 and max are
// logged in one "FrameStats:" line.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "ftcore/FrameStats.h"

#include <memory>

#include "FTFrameStatsSubsystem.generated.h"

UCLASS()
class FORGOTTENTHIRD_API UFTFrameStatsSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UFTFrameStatsSubsystem();
	virtual ~UFTFrameStatsSubsystem() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return false; }

private:
	void Census() const;
	void Report();

	std::unique_ptr<ftcore::FrameStats> Stats;
	int32 Frames = 0;
	int32 Skip = 60;
	int32 CensusAt = 120;
	bool bReported = false;
};
