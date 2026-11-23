#pragma once

// Rain under cover: the top-down depth capture that FTRainCoverCollection.h's material term reads (ftcore/RainCover.h).
//
// On Solhaven's maps (game and PIE worlds) a transient ASceneCapture2D looks straight down from ftcore::RainCoverSpec
// aboveM over the camera: orthographic, sizeM square, SceneDepth into RT_RainCover. Only what can shelter is drawn:
// opaque static and instanced meshes (no characters, particles, translucency, decals, fog or landscape: the ground is
// open anyway). It captures on the first frame and again whenever the camera has moved recaptureM in plan from the
// capture's snapped centre, then writes MPC_RainCover (CoverFrame, CoverOn 1). With no capture CoverOn stays 0 (all
// open: rain everywhere, as before).
//
// Switches (FTLookSwitches): off unless -RainCover is given (FTRainCover::Enabled; the material and rain builds need it
// too, or nothing reads the capture); -NoRainCover wins over it. -RainCoverProbe logs the first capture's occluder
// heights at the centre and 4 m along +X and +Y, and the covered share of the square (a readback: proof runs only).
// Logs once: "RainCover: on, <size> m at <res>" or why it is off; on shutdown the capture count.
//
// Cost cells (dev builds only, all off by default): -RainCoverCost logs memory (RHI non-streaming textures, process
// physical) on the first tick and 600 ticks in, map on or off, and a cost summary on shutdown. -RainCoverEveryFrame
// recaptures on every tick (the per-capture cost, by difference against a run without it). -RainCoverReadback copies
// each capture back to the CPU asynchronously, as a lead reading the map at its plan point would, and times the copy.
// Both imply -RainCoverCost.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#if !UE_BUILD_SHIPPING
#include "RHIGPUReadback.h"
#include "Templates/UniquePtr.h"
#endif

#include "ftcore/RainCover.h"

#include "FTRainCoverSubsystem.generated.h"

class ASceneCapture2D;
class UTextureRenderTarget2D;

UCLASS()
class FORGOTTENTHIRD_API UFTRainCoverSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;
	// The cover map is drawn (-RainCover). The lead's head trace (AFTExploreCharacter::TraceLeadCover) does not need it:
	// it is a collision trace that takes only Spec (valid with the map off). With the map off the ground and the rain
	// fall everywhere while the lead still reads its roof.
	bool IsActive() const { return bActive; }
	const ftcore::RainCoverSpec& GetSpec() const { return Spec; }

private:
	void Probe(const FVector& Eye);

	UPROPERTY(Transient)
	TObjectPtr<ASceneCapture2D> Capture;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> Target;

	ftcore::RainCoverSpec Spec;
	ftcore::RainCoverFrame Frame;
	bool bActive = false;
	bool bHaveCapture = false;
	bool bProbe = false;
	int32 Captures = 0;
#if !UE_BUILD_SHIPPING
	void CostMemory(const TCHAR* When) const;
	void CostReadbackIssue();
	void CostReadbackPoll();
	void CostSummary();
	bool bCost = false;
	bool bEveryFrame = false;
	bool bReadback = false;
	int32 CostTicks = 0;
	double CallSum = 0.0; // game thread: CaptureScene() itself, s
	double CallMax = 0.0;
	TUniquePtr<FRHIGPUTextureReadback> Readback;
	TArray<float> CoverCpu; // resolution squared, sized once
	bool bReadbackInFlight = false;
	uint64 ReadbackIssued = 0; // GFrameCounter at the copy's issue
	int32 ReadbacksIssued = 0;
	int32 ReadbacksDone = 0;
	int32 ReadbacksSkipped = 0; // a capture while the last copy was still in flight
	int64 LatencySum = 0;       // frames from issue to ready
	int32 LatencyMax = 0;
	double CopySum = 0.0; // render thread: lock, copy rows, unlock, s (read after a flush)
	double CopyMax = 0.0;
#endif
};
