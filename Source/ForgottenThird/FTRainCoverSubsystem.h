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
// The capture draws depth only (SetCoverShowFlags): on top of the set above, hair, every lighting, reflection, AO,
// shadow and GI feature, the temporal and post passes, and text, widget and media planes are off. None of them changes
// which opaque surface is nearest the capture. Each capture sets the CSV stat RainCover/Capture on its frame (the marker
// the frame-cost cells read; 1, or 1/2/3 over a split recapture, below) and times each draw as RainCover/CaptureScene.
//
// Dev switches (not in shipping builds, all off by default), one per lever, so their costs can be read in one build:
// -RainCoverUntrimmed draws with the set before the trim; -RainCoverPersist keeps the capture's view state between
// captures (bAlwaysPersistRenderingState); -RainCoverMainRenderer draws it as a depth pass of the main renderer
// (bRenderInMainRenderer). -RainCoverDiff compares the map as drawn (with the switches given) against a second capture
// with the set before the trim, at nine squares round the first capture and 5 m on from each, first after the move and
// settled, with the reference against itself as the noise floor; it logs differing texels by kind (on a character, on
// water, other) and saves each settled pair as raw R32F under Saved/RainCoverDiff.
// A tenth square sits on the strip's pent eaves. Lighting is off in the trim; -RainCoverShowFlags=Lighting=1 puts it back.
// Path-1 draw cuts on the shipped capture only: -RainCoverMaxDist=<m>, -RainCoverLOD=<f>, -RainCoverHideSmall=<cm>.
//
// Split (path 2, 11-25; dropped, dev switch -RainCoverSplit only): it did not lower the peak, since the capture's cost is
// fixed per scene render, so each half cost about what the whole did. A recapture is spread over three ticks. Tick 1 draws the
// square's +X half (image rows 0 to res/2), tick 2 its -X half, each into its own res x res/2 target; tick 3 copies both
// halves into RT_RainCover and only then moves CoverFrame, so the map and its frame change together. The capture actor
// stays at the square's centre; each half uses an off-centre orthographic matrix built like the engine's own (same width,
// near 0, far = the capture's MaxViewDistanceOverride), so every texel lands where the whole capture puts it. Half a
// square doubles the projection's larger scale term, which the LOD code reads, so LODDistanceFactor is doubled to keep
// the same LODs. RainCover/Capture is 1, 2 and 3 on those ticks. Without -RainCoverSplit the whole square is drawn in one
// tick (Capture 1); -RainCoverMainRenderer always is, since that capture lands later than the copy. Under
// -RainCoverDiff the drawn map runs both halves and the copy in one tick (the image does not depend on the ticks).

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "ftcore/RainCover.h"

#include "FTRainCoverSubsystem.generated.h"

class ASceneCapture2D;
class USceneCaptureComponent2D;
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
	virtual bool IsTickable() const override { return bActive; }
	// The cover map is drawn (-RainCover). The lead's head trace (AFTExploreCharacter::TraceLeadCover) does not need it:
	// it is a collision trace that takes only Spec (valid with the map off). With the map off the ground and the rain
	// fall everywhere while the lead still reads its roof.
	bool IsActive() const { return bActive; }
	const ftcore::RainCoverSpec& GetSpec() const { return Spec; }

private:
	void Probe(const FVector& Eye);
	void Shoot(ASceneCapture2D& With);
	// Split: set up the half targets and matrices (false: stay whole); draw one half (0 = +X, 1 = -X); copy both into Target.
	bool SplitSetup(USceneCaptureComponent2D& C);
	void ShootHalf(int32 Half);
	void Join();
	// The drawn map in one tick: both halves and the copy when split, else the whole square.
	void ShootNow();
	// Frame is the map's square from here on: CoverFrame and CoverOn written, the capture counted.
	void Publish(UWorld& World);
#if !UE_BUILD_SHIPPING
	void DiffStart(UWorld& World);
	void DiffTick();
	bool DiffRead(UTextureRenderTarget2D* From, TArray<FLinearColor>& Out) const;
	void DiffCompare(const TCHAR* Label, const TArray<FLinearColor>& Ref, const TArray<FLinearColor>& Var, const FString& SaveAs) const;
#endif

	UPROPERTY(Transient)
	TObjectPtr<ASceneCapture2D> Capture;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> Target;
	// -RainCoverDiff only (dev): the reference capture, drawn with the set before the trim, into its own target.
	UPROPERTY(Transient)
	TObjectPtr<ASceneCapture2D> RefCapture;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> RefTarget;

	// Split only: the two halves (+X, -X), res x res/2 in Target's format.
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> HalfPlus;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> HalfMinus;

	ftcore::RainCoverSpec Spec;
	ftcore::RainCoverFrame Frame;
	// Split: the square being drawn (latched on its first tick, published on the copy tick) and the step reached (0 idle,
	// 1 = +X half drawn, 2 = -X half drawn).
	ftcore::RainCoverFrame Pending;
	FMatrix HalfProjection[2];
	int32 SplitStep = 0;
	bool bSplit = false;
	bool bActive = false;
	bool bHaveCapture = false;
	bool bProbe = false;
	int32 Captures = 0;
#if !UE_BUILD_SHIPPING
	// -RainCoverDiff: the squares are visited in turn, a few steps each (DiffTick), the shipped recapture paused.
	bool bDiff = false;
	int32 DiffTicks = 0;
	int32 DiffSquare = 0;
	int32 DiffStep = 0;
	int32 DiffWait = 0;
	int32 DiffSettle = 0;
	ftcore::RainCoverFrame DiffOrigin;
	ftcore::RainCoverFrame DiffAt;
	TArray<FLinearColor> DiffRef;
	TArray<FLinearColor> DiffVar;
	TArray<FLinearColor> DiffRefAgain;
#endif
};
