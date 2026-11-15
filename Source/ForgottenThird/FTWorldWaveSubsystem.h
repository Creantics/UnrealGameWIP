#pragma once

// The world wave's host: one ftcore world wave (Core/include/ftcore/WorldWave.h) stepped each frame, and the things it
// cuts in and out.
//
//   ft.WorldWave <x> <y> <z> [reverse]   a wave from that world point (cm) starting now; ft.WorldWave off ends it
//   ft.WorldWaveDress <x> <y> <z> <r>     the proof dressing round that world point (cm), within r cm
//   -WorldWaveAt=<x,y,z,t>                a wave from that world point (cm) starting at t seconds on the wave clock
//   -WorldWaveReverse                     ... run in reverse (the sphere holds the first world)
//   -WorldWaveDress=<x,y,z,r>             the dressing on the first frame, before any -WorldWaveAt wave starts
//
// The wave clock is game time, unless a capture holds it (SetClock): the eye shot's fixed-step walk holds it at -1 until
// its first frame and at (frame - 1) / -WorldEyeWalkFps from then on, so -WorldWaveAt=x,y,z,0 with
// -WorldEyeWalkTo=<6 m on> -WorldEyeWalkSpeed=2 -WorldEyeWalkFps=30 saves the wave's first 3 s as 90 frames. On game
// time, start a wave a few seconds in: the exposure is held from the wave's start, where the eye has adapted to by then.
//
// Each frame, once every actor and the player's camera have ticked (FWorldDelegates::OnWorldPostActorTick), while a wave
// is set it
//   - writes the wave's parameters to MPC_Look (FTWorldWave.h has the names and units), the origin and the streak's tip
//     (StreakHeightCm over it) projected through the first player's view;
//   - keeps an unbound post-process volume on with PP_WorldWave as its blendable (the second world's grade, ground fog
//     and the front's refraction, per pixel), and from the wave's start holds the auto exposure where it stands (speed
//     0.001 up and down), so the change reads as the world's, not the eye's;
//   - traces a 12 x 7 grid of view rays for the share of the view the second world has (ftcore::worldWaveViewShare),
//     follows it at a limited rate (stepWaveCover) and blends the look actors' sun, sky light, fog and clouds from the
//     first world's look to the second's (the definition's toWorld in looks.json, the Unwritten by default) by it, never ahead of the ground; the post-process's gains
//     (waveGains) keep the unreached side in its own light. StopWave puts the first world's look back;
//   - the surfaces behind the front take the second world's state in their own materials (MF_WorldWaveState: near grey,
//     a stop down, damp, grimed and leak-stained, their light dead but for the wave definition's kept lights, ftcore::WorldWaveParams::keep);
//   - logs each cue (no sound yet) and raises it on OnCue;
//   - steps every tracked thing (ftcore::updateWorldWaveThing): its visibility, its collision (collisionDelay after the
//     front passes it) and a held arrival's own front (custom primitive data FTWorldWave::kLocalSlot). An arrival that
//     would come in round the player or a lead (AFTExploreCharacter) waits, not drawn and not solid, until its footprint
//     (its collision's box, else its bounds) is clear, then cuts in. Nothing is pushed.
// No heap allocation in the frame's step: the dressing lists the tracked things, the wave's start the pawns and fogs
// (into arrays reserved once). The post-actor-tick delegate is bound only while there is work: from a boot switch or a
// wave's start until the wave settles (the radius at its end, the camera's ramp done, every tracked thing at rest) or
// ends. A map with no wave never binds it. A settled wave keeps its look until StopWave or the next wave. A wave whose
// parameters Core refuses (ftcore::beginWorldWave: a non-finite origin or start) does not start and logs the reason.
//
// The proof dressing (Dress). Within the radius of the point: every street tree (a /Game/Props/TREE- mesh, not the
// grate), TQ-04 lamp post and parked kei car (SM_KeiCar) is tagged OldOnly. One in a cell's instanced mesh is split off
// into a one-instance instanced mesh of its own on the same holder (its transform, per-instance data, materials,
// collision, culling, shadow and tags), as the tag is per primitive (custom primitive data FTWorldWave::kTagSlot); a
// fallback actor's component is tagged in place. The lamps' night lenses (the night lights' instanced mesh tagged
// FTScene::NightLampHeadTag) within the radius go with them, cut by visibility alone (their material has no cut). A slot
// whose master is a prop master (FTWorldWave::PropMasters) moves to
// a dynamic instance of the master's twin with the cut (M_FTCharacter_WaveCut, M_FTCharacterTex_WaveCut). The ground,
// the buildings and the clock tower are not touched: untagged is Both (never cut, they take the second world's look
// from the post-process alone). Then it spawns the second world's own things, tagged NewOnly, on the ground in a ring 8 to
// 25 m out: bare trees (TREE-broadleaf-a, TREE-park-a, leaves and billboard at Bare 1) and standing boards (the kit's
// SIGN_VERT_L and SIGN_VERT_M, on twin instances), each set down on a ground trace and turned to face the point. The log
// lists each pick.

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"

#include "ftcore/SceneLook.h"
#include "ftcore/WorldWave.h"

#include "FTWorldWaveSubsystem.generated.h"

class APawn;
class APostProcessVolume;
class UExponentialHeightFogComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UMaterialParameterCollection;
class UPrimitiveComponent;
class UStaticMesh;

// A wave cue as it fires (Prelude, FrontPassedListener: the front has passed the listener, Settled), with the seconds since
// the wave's start. Raised on the game thread in the post-actor tick, after the cue's log line.
DECLARE_MULTICAST_DELEGATE_TwoParams(FFTWorldWaveCueEvent, ftcore::WorldWaveCue /*Cue*/, double /*SecondsSinceStart*/);

UCLASS()
class FORGOTTENTHIRD_API UFTWorldWaveSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& InCollection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	// A wave from Origin (world cm) starting at StartTime on the wave clock. A wave already set is replaced; the tracked
	// things start it from the state the side beyond its front has.
	void StartWave(const FVector& Origin, double StartTime, bool bReverse);
	// Ends the wave: the collection cleared, the volume off, the fog and the exposure back, every tracked thing as the
	// first world has it.
	void StopWave();
	bool HasWave() const { return bWave; }
	// Which world a point (world cm) is in now: the side the wave leaves (before its front), the band, or the side it
	// brings, from Core's classifyWorldWave on the state the shaders' constants come from (worldWaveShaderConstants).
	// With no wave, the side beyond the front (Old, band 0). For gameplay: enemies, sound, the lead changing with the front.
	ftcore::WaveClass WhichWorldAt(const FVector& WorldCm) const;
	// The same by the SHADER rule (ftcore::classifyWorldWaveShader on the constants written to MPC_Look this frame): the
	// tongues change pixels up to a band's width ahead of the outer edge, so a point there is Band here while WhichWorldAt
	// still says Old. With no wave, Old, band 0. For the HUD and effects that must agree with what is drawn.
	ftcore::WaveClass WhichWorldAtShader(const FVector& WorldCm) const;
	bool IsInShaderBand(const FVector& WorldCm) const { return WhichWorldAtShader(WorldCm).side == ftcore::WaveSide::Band; }

	// The wave set now (last StartWave; read with HasWave): its origin (world cm), its direction, and the ids of the two
	// worlds it changes between (looks.json ids: the map's look at the start, empty for the shared look, and the wave
	// definition's toWorld). Forward brings ToWorld over FromWorld; Reverse runs the same pair back.
	const FVector& GetOriginCm() const { return OriginCm; }
	ftcore::WaveDirection GetDirection() const { return Params.direction; }
	bool IsReverse() const { return Params.direction == ftcore::WaveDirection::Reverse; }
	const FString& GetFromWorld() const { return FromWorld; }
	const FString& GetToWorld() const { return ToWorld; }

	// Each cue as it fires (see FFTWorldWaveCueEvent). Bind with AddUObject / AddRaw and remove on teardown.
	FFTWorldWaveCueEvent OnCue;

	// The proof dressing round Origin (world cm) within Radius (cm); returns how many things it tracks now.
	int32 Dress(const FVector& Origin, double Radius);

	// Logs where the front meets a lead's body against where it meets the ground under its feet: the materials' own field
	// (FTWaveSecond's ragged edge, tongues and jitter, ported to the CPU, with the radius, band and age the collection gets
	// this frame) along the line from the origin through the body's axis, at the ground's, feet's, chest's and head's
	// heights; each crossing against the ground's, in cm, and how far each lies from the body's axis. Nothing without a
	// wave or a skeletal body.
	void LogBodyBoundary(const AActor& Lead) const;

	// Holds the wave clock at Seconds (a fixed-step capture); ReleaseClock goes back to game time.
	void SetClock(double Seconds);
	void ReleaseClock();
	double GetClock() const;

private:
	// One tagged thing the wave cuts.
	struct FTracked
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		ftcore::WorldWaveThing Thing;
		FVector FootCentre = FVector::ZeroVector; // its footprint for the pawn test: a box's centre and half size, cm
		FVector FootExtent = FVector::ZeroVector;
		TEnumAsByte<ECollisionEnabled::Type> Collision = ECollisionEnabled::QueryAndPhysics; // when solid
		bool bDrawn = true;
		bool bSolid = true;
		bool bHeld = false;
		float Local = 0.f; // the local front last written (cm), 0 for none
	};
	struct FFog
	{
		TWeakObjectPtr<UExponentialHeightFogComponent> Component;
		float Density = 0.f; // the map's own
		float Written = 0.f;
	};

	void PostActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds);
	// Binds or unbinds PostActorTick (each at most once).
	void BindTick(bool bOn);
	void Step(UWorld& World);
	// The volume with the wave's post-process material and the exposure hold, spawned on the first wave.
	void EnsureVolume(UWorld& World);
	void SetExposureHold(bool bHold);
	void GatherPawns(UWorld& World);
	// The audit's characters: every actor with a skeletal mesh, not attached to another, within the wave's whole reach
	// (maxRadius plus the widest band) of its origin, and every lead. Gathered at StartWave over the world once; actors
	// spawned while a state is live are taken at the next re-audit.
	bool IsAuditRoot(const AActor& Actor) const;
	void AddAuditRoot(AActor& Actor);
	// One audit of a character (the root and every actor attached to it, at any depth: its body, each piece, shoes,
	// glasses, grooms, held weapons). A slot is wave-aware when its master's FTWaveAware default is 1 (FTWorldWave::
	// WaveAware, the same in the editor and a cooked build). Every primitive with a slot renders custom depth: stencil
	// FTWorldWave::kAwareStencil when every slot is wave-aware (the post leaves it), FTWorldWave::kCatchStencil when any
	// is not (PP_WorldWave gives its pixels the character state; r.CustomDepth must be 3, the project's setting). A primitive's own
	// settings are recorded the first time they are changed, and RestoreWorn puts them back. A primitive that already
	// renders custom depth with another stencil is left (its other slots counted unconverted, logged once). A master not
	// wave-aware is logged once a run; a primitive that mixes both kinds takes 223 (its wave-aware slots then take the
	// state twice: logged once, as the stencil is per primitive). The summary line is logged when anything changed.
	struct FAuditCount
	{
		int32 Actors = 0;
		int32 Slots = 0;
		int32 Other = 0;       // slots not wave-aware
		int32 Catch = 0;       // primitives at 223
		int32 Aware = 0;       // primitives at 222
		int32 Unconverted = 0; // slots on a primitive with someone else's stencil
		int32 Changed = 0;     // primitives whose custom depth or stencil this audit set
	};
	FAuditCount AuditWorn(const AActor& Root);
	// Every audit root again (from StartWave, and every AuditPeriod seconds on a world timer while a wave is set, its
	// settled state included; never per frame): a weapon drawn, a jacket state shown or a material swapped is corrected.
	void ReAudit();
	void OnActorSpawned(AActor* Actor);
	// Puts every primitive the audit changed back as it was; stops the re-audit.
	void RestoreWorn();
	// r.CustomDepth at 3 (the stencil, the project's setting), checked only: false when it is lower (the setting
	// overridden, the console say), warned once; the audit then tags nothing, as the catch-all could not work.
	bool EnsureStencil();
	// The FTWaveAware marker of a slot's master (-1 none), read once per master for the life of the wave state.
	float AwareMarker(const UMaterialInterface& Material);
	struct FWornDepth
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		bool bRender = false; // its own custom depth and stencil before the audit set them
		int32 Stencil = 0;
		uint32 Pass = 0;      // the last re-audit that saw it on a root (one not seen is put back)
	};
	static constexpr float AuditPeriod = 0.5f;
	TArray<TWeakObjectPtr<AActor>> AuditRoots;
	TArray<TWeakObjectPtr<AActor>> Spawned; // since the last re-audit
	TMap<FObjectKey, FWornDepth> WornDepth;
	TMap<FObjectKey, float> AwareMarkers;
	uint32 AuditPass = 0;
	bool bStencilRefused = false;  // r.CustomDepth was found below 3 (warned once)
	FTimerHandle AuditTimer;
	FDelegateHandle SpawnHandle;
	// The audit's scratch, reserved once (no allocation in a re-audit once the arrays have grown).
	TArray<const AActor*> AuditActors;
	TArray<UMaterialInterface*> AuditUsed;
	TSet<FObjectKey> AuditLogged; // the masters and primitives already logged
	bool PawnInFootprint(const FTracked& Item, const APawn* PlayerPawn) const;
	// A thing as one side of the front has it (drawn and solid on its own world's side), its wave state reset.
	void ResetThing(FTracked& Item, bool bOldSide);
	// Tracks a primitive with its tag (the custom primitive data set here), its pivot, radius and footprint from Mesh at
	// WorldTransform.
	void Track(UPrimitiveComponent& Component, ftcore::WorldTag Tag, const UStaticMesh& Mesh, const FTransform& WorldTransform);
	// The material a dressed prop's slot takes: a twin instance when its master is a prop master, else Material itself.
	UMaterialInterface* CutMaterialFor(UMaterialInterface* Material);
	void ClearCollection(UWorld& World);
	// Loads MPC_Look on the first wave (once; a map with no wave never loads it).
	void EnsureCollection();
	bool bCollectionTried = false;

	UPROPERTY()
	TObjectPtr<UMaterialParameterCollection> MpcLook;
	UPROPERTY()
	TObjectPtr<APostProcessVolume> Volume;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Instances; // the dressing's twin and bare instances
	UPROPERTY()
	TMap<TObjectPtr<UMaterialInterface>, TObjectPtr<UMaterialInterface>> CutFor; // a dressed slot's material to its cut one

	ftcore::WorldWaveParams Params;
	ftcore::WorldWaveState State;
	ftcore::WorldWaveFrame Frame;
	FVector OriginCm = FVector::ZeroVector;
	FString FromWorld; // the map's look id when the wave started (empty: the shared look)
	FString ToWorld;   // Params.toWorld, for the getters
	// The ground under the origin (one trace at StartWave): the prelude streak's foot.
	FVector StreakFootCm = FVector::ZeroVector;
	bool bWave = false;
	bool bStepping = false;   // a set wave still moving; false once it has settled
	double SettledAt = -1.0;  // wave-clock time the radius reached its end, -1 before
	float CollectionAge = 0.f; // the age (s) the last Step gave the collection's WorldWaveTime; LogBodyBoundary's field uses it
	bool bExposureHeld = false;
	bool bHoldClock = false;
	double HeldClock = 0.0;

	// The switches' dressing and wave, run on the first frame once every actor has begun play (OnWorldBeginPlay comes
	// before the actors' BeginPlay).
	bool bBootPending = false;
	bool bBootDress = false;
	bool bBootWave = false;
	bool bBootReverse = false;
	FVector BootDressAt = FVector::ZeroVector;
	double BootDressRadius = 0.0;
	FVector BootWaveAt = FVector::ZeroVector;
	double BootWaveTime = 0.0;

	TArray<FTracked> Tracked;
	TArray<TWeakObjectPtr<APawn>> Pawns;
	TArray<FFog> Fogs;

	// The second world's sky, light and air, crossing over in step with how much of the view the front has covered (a
	// grid of view rays traced each step), never ahead of it: the first world's look while the front is far, the second's
	// once it fills the view; the post-process makes the grade's change per pixel.
	void StepLook(UWorld& World, const FVector& Eye, const FMatrix& ViewProjection, bool bView, double Time, const APawn* PlayerPawn);
	// Puts the look actors back to the first world's look.
	void RestoreLook(UWorld& World);
	bool bLookBlend = false;
	ftcore::SceneLook LookFrom;     // the first world's, as the look subsystem has it
	ftcore::SceneLook LookStart;    // ftcore::waveSkyStart(LookFrom, LookTo)
	ftcore::SceneLook LookTo;       // the second world's
	ftcore::SceneLook LookOnActors; // what the look actors carry now
	/** The map's own sun cloud-shadow flag from before the wave (0/1; -1 not taken), given back by RestoreLook. */
	int32 SunCloudShadowsMap = -1;
	float Cover = 0.f;
	float CoverShare = 0.f;
	/** The cloud layer's own share (ftcore::stepWaveCloud): it changes only once the view is all one world. */
	float CloudT = 0.f;
	/** The post-process's own overcast over the second world's sky (WorldWaveGain.w): 1 until the cloud layer has changed,
	 *  then down to 0 at kSkyCoverRate, so the reshaping layer is never seen. */
	float SkyCover = 1.f;
	double LastLookTime = -1.0;
	/** LastLookTime is set (the held clock of a still's settle stands at -1, so -1 cannot mean unset). */
	bool bLookTimed = false;
	/** The wave clock at the last look line in the log (one every half second of the wave). */
	double LastLookLog = -1.0;
	double LastLookReal = -1.0;
	/** The map's sky light captures the sky in real time (found at the wave's start): its fill is lifted by the sun lost. */
	bool bSkyRealTime = false;
	// The proof's overrides of the wave's definition (Params.front, Params.keep), parsed and applied only outside Shipping:
	// -WorldWaveFront=A|B|Band, -WorldWaveKeep0/1=x,y,z,r (cm). -1 / false: the definition's own.
	int32 DevFront = -1;
	bool bDevKeep[ftcore::kWaveKeepLights] = {false, false};
	ftcore::WaveKeepLight DevKeep[ftcore::kWaveKeepLights];

	FName VectorParams[7];
	FName ScalarParams[7];
	FDelegateHandle PostTickHandle;
};
