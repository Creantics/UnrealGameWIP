#pragma once

// Rain under cover: the cover map that FTRainCoverCollection.h's material term reads (ftcore/RainCover.h).
//
// Shipped (11-25, path 3b): the resident ring, FTRainCoverRing.cpp. The strip's 64 m tiles are baked once per content key
// into a cache of v2 tile files; in play RT_RainCover is a 3 x 3 atlas of G16 tiles round the lead's tile, fed from a
// 25-tile RAM pool read off the game thread, and CoverSlot0..8 say which tile each slot holds. Its switches are listed
// at the top of that file. With -RainCover the ring is what runs; nothing below draws in play.
//
// The moving capture below (dev only since the ring: -RainCoverLegacy, or any of the probes and cells that need it, on
// the old term the material builds no longer put in): a transient ASceneCapture2D looks straight down from ftcore::RainCoverSpec
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
// tick (Capture 1); with -RainCoverMainRenderer each half is a depth pass of that
// tick's main render, so the copy on tick 3 still follows both (render commands keep their order); under -RainCoverDiff
// the main renderer stays whole. Under -RainCoverDiff the drawn map runs both halves and the copy in one tick (the image
// does not depend on the ticks).
//
// Path 3b probe (11-25, dev only; nothing in play reads tiles yet, no material, MPC or indirection change):
// -RainCoverTileProbe=<file> captures fixed tiles (ftcore rainCoverTileAt, 64 m on a grid from the origin) once the
// shipped capture has run and 600 ticks have passed, with the shipped recapture paused. The file lists "t <tx> <ty>"
// tiles and "s <x> <y>" seam points (m; '#' comments). In one tick, from wherever the camera is (far from most tiles):
// per tile a find capture from 400 m with a 500 m far plane gives its highest occluder; the tile is then drawn from
// that plus 1 m with the shipped far plane (depthM), read back, packed to R16 over its own range (ftcore
// rainCoverR16Pack), written under Saved/RainCoverTiles and read back from there, each step timed. Two ticks on, the
// first five are drawn again (a stale readback would differ). Then, one at a time, a camera stands 2 m over the lowest
// surface at each tile's centre and each seam point (the view target, 20 ticks to settle): the tile is drawn again at
// its own top (far camera against camera on the tile, R32F both), and a square is drawn as the shipped capture would
// from that camera, which the stored R16 tiles are compared against texel for texel (heights within 2 mm, occluder
// present or absent, the band over the reference's top counted apart, texels below either far plane apart, tile edges
// counted). It logs a line per tile and per reference, then a summary, and asks the game to quit.
// -RainCoverUploadProbe: no scene capture at all; on the shipped 5 m trigger it uploads three 512 x 512 R32F tiles
// (3 MiB, the edge crossing of a 3 x 3 ring) from RAM into transient textures nothing reads, CSV stat RainCover/Upload 1.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "ftcore/RainCover.h"

#include "FTRainCoverSubsystem.generated.h"

class ASceneCapture2D;
class USceneCaptureComponent2D;
class UTexture2D;
class UTextureRenderTarget2D;
// Dev only (11-25 truth probe): the truth passes' and the streamed upload's state, in the .cpp.
struct FFTRainTruth;
struct FFTRainStream;
// The resident ring (FTRainCoverRing.cpp): its tiles, keys, cache, RAM pool and reads.
struct FFTRainRing;

// The capture settings the subsystem's captures share (straight down, orthographic, depth, the trimmed show flags).
void FTRainCoverCaptureSettings(USceneCaptureComponent2D& C, UTextureRenderTarget2D* Into, const ftcore::RainCoverSpec& S);

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
	// The ring (FTRainCoverRing.cpp): set up at begin play (false: the ring is off, logged); its tick; shut down.
	bool RingBegin(UWorld& World);
	void RingTick(UWorld& World, const FVector& Eye);
	void RingEnd();
#if !UE_BUILD_SHIPPING
	void DiffStart(UWorld& World);
	void DiffTick();
	bool DiffRead(UTextureRenderTarget2D* From, TArray<FLinearColor>& Out) const;
	void DiffCompare(const TCHAR* Label, const TArray<FLinearColor>& Ref, const TArray<FLinearColor>& Var, const FString& SaveAs) const;
	// Path 3b probe: set up, the far loop, the steps after it; the camera; one draw; a reference against the tiles; uploads.
	bool TileSetup(UWorld& World);
	void TileFarLoop();
	void TileTick(UWorld& World);
	bool TilePlaceCamera(UWorld& World, float X, float Y);
	bool TileDraw(const ftcore::RainCoverFrame& At, float FarM, TArray<float>& DepthM, double& EnqueueMs, double& ReadMs);
	void TileCompareRef(const TCHAR* Label, int32 Own, int32 Item, float X, float Y);
	void UploadTick(const FVector& Eye);
	// 11-25 truth probe (ruling 7047 item 5): the switch passes against a full-detail truth, the whole strip's loop and key,
	// the streamed upload.
	void TruthSetUp(UWorld& World);
	void TruthTick(UWorld& World);
	void TruthSwitches(UWorld& World, bool bLod0, bool bNoCull, bool bNoHlod);
	void StripTick(UWorld& World);
	void StreamTick(const FVector& Eye);
	// The ring's fidelity witness (FTRainCoverRing.cpp, -RainCoverWitness): one square a tick after its camera settles.
	void RingWitnessTick(UWorld& World);
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

	// Dev only (path 3b probe, -RainCoverTileProbe / -RainCoverUploadProbe; null otherwise): the tile capture, its target
	// and the camera it stands on the tiles; the upload cell's transient tiles.
	UPROPERTY(Transient)
	TObjectPtr<ASceneCapture2D> TileCapture;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> TileTarget;
	UPROPERTY(Transient)
	TObjectPtr<AActor> TileCamera;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> UploadTex;

	// The ring's bake capture and its target (R32F, one tile; the witness's 60 m draw uses them too: the same size).
	UPROPERTY(Transient)
	TObjectPtr<ASceneCapture2D> RingCapture;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> RingTarget;
	// Route B (dev): the staging texture each entering tile is written into before its copy into the atlas.
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> RingStaging;
	TSharedPtr<FFTRainRing> Ring;

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

	// -RainCoverTileProbe (3b): the tiles, their tops, far captures (depth m), R16 codes and ranges; the seam points.
	bool bTiles = false;
	int32 TileState = 0;
	int32 TileTicks = 0;
	int32 TileAt = 0;
	int32 TileWait = 0;
	FString TileDir;
	TArray<ftcore::RainCoverTile> TileList;
	TArray<FVector2f> SeamList;
	TMap<FIntPoint, int32> TileIndex;
	TArray<float> TileTop;
	TArray<float> TileFloor; // the lowest Z a tile's capture reaches (its top minus its far plane)
	TArray<float> TileOn;    // the last on-tile draw (camera on the tile), depth m
	TArray<TArray<float>> TileFar;
	TArray<TArray<uint16>> TileCode;
	TArray<ftcore::RainCoverR16Range> TileRange;
	TArray<float> TileScratch;
	TArray<FLinearColor> TilePixels;
	FVector TileCam = FVector::ZeroVector;
	int32 TileRefs = 0, TileRefFails = 0, TileHeld = 0;

	// -RainCoverUploadProbe: the source in RAM (made once) and the trigger's own square.
	bool bUpload = false;
	bool bUploadAny = false;
	int32 Uploads = 0;
	ftcore::RainCoverFrame UploadFrame;
	TArray<float> UploadSrc;

	// -RainCoverTileStrip: the whole strip's far loop only (5b, 5c), no per-tile arrays kept. The truth passes (5a) and the
	// streamed upload (5d, -RainCoverUploadTiles=<n>) keep their state in the .cpp.
	bool bTileStrip = false;
	TSharedPtr<FFTRainTruth> Truth;
	TSharedPtr<FFTRainStream> Stream;
#endif
};
