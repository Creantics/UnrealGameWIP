#pragma once

// Solhaven's civilians (ftcore::CrowdSim, Core/include/ftcore/Crowd.h): the sim walks them on a graph built from the
// world's streets table and this subsystem only feeds it and draws it. It ticks the sim with the player's position,
// raises world events (the arena fight's strikes, downs and Releases come in through FeedArena; anything else calls
// RaiseEvent), and stands a pool of placeholder skinned meshes on the nearest agents, each playing the clip of its
// agent's motion (walk, run, back-step, cower, idle; a missing clip falls back as the comments on the clips say).
//
//   UnrealEditor-Cmd ForgottenThird.uproject /Game/Maps/<map> -game ... -CrowdStreets=<streets csv> [-CrowdSide=1000]
//       [-CrowdTuning=<json>]  (default Data/Crowd/crowd.json)
//       [-CrowdSkin=<mesh>] [-CrowdWalk=<clip>] [-CrowdRun=<clip>] [-CrowdIdle=<clip>] [-CrowdCower=<clip>]
//       [-CrowdSeed=<n>] [-CrowdTestFight[=<seconds>]] [-CrowdQuitAfter=<seconds>]
//
// The streets table is the massing one (id, kind, width_m, pts, pavement_m; plan frame, a square -CrowdSide metres across)
// and the sim's frame is the engine's in metres, the same as the arena's (FTArenaRunner::ToWorld is P * 100). Without
// -CrowdStreets nothing is created. The skin defaults to the test human and its walk and idle clips; a skin that doesn't
// load leaves the sim running undrawn (the log still counts). -CrowdTestFight scripts a fight 15 m from the player at
// <seconds> (default 20): a fight start, then a strike every half second for 10 s and a death at the end; with or
// without it, the counts are logged every 10 s ("Crowd: t=...") and -CrowdQuitAfter ends the game with the final line
// ("Crowd: final ...", exit 0, or 1 when the scripted fight made nobody flee or nobody resume).

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/Crowd.h"

#include "FTCrowdSubsystem.generated.h"

class ACameraActor;
class ASkeletalMeshActor;
class UAnimSequence;
class USkeletalMesh;

UCLASS()
class FORGOTTENTHIRD_API UFTCrowdSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// The world's crowd, or null when the map has none.
	static UFTCrowdSubsystem* Get(const UWorld* World);

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bRunning; }

	// A world event at an engine point (centimetres; z is ignored). Scales multiply the event rule's radius and intensity.
	void RaiseEvent(ftcore::CrowdEventKind Kind, const FVector& At, float RadiusScale = 1.f, float IntensityScale = 1.f);
	// One arena tick's events (positions in world metres, as ArenaEvent.at): the first attack after a quiet spell is a
	// fight start, a hit or a block a strike, a down a death, a Release a magic burst.
	void FeedArena(const ftcore::ArenaEvents& Events);

	const ftcore::CrowdSim& GetSim() const { return Sim; }

private:
	bool Fail(const TCHAR* Why, const FString& What);
	void LoadArt();
	void Draw();
	void PlayFor(int32 Slot, ftcore::CrowdAnim Anim, float Speed);
	void ScriptFight();
	void Shoot(APlayerController* PC, const FVector& PlayerAt);
	void LogCounts(const TCHAR* Label) const;
	static bool Occluded(void* Context, ftcore::Vec2 From, ftcore::Vec2 To);

	ftcore::CrowdTuning Tuning;
	ftcore::CrowdGraph Graph;
	ftcore::CrowdSim Sim;

	UPROPERTY()
	TObjectPtr<USkeletalMesh> Skin;
	UPROPERTY()
	TObjectPtr<UAnimSequence> WalkClip;
	UPROPERTY()
	TObjectPtr<UAnimSequence> RunClip;
	UPROPERTY()
	TObjectPtr<UAnimSequence> IdleClip;
	UPROPERTY()
	TObjectPtr<UAnimSequence> CowerClip;
	UPROPERTY()
	TArray<TObjectPtr<ASkeletalMeshActor>> Pool;
	// -CrowdShots=<dir>: two fixed cameras round the test fight and the stills they take.
	UPROPERTY()
	TArray<TObjectPtr<ACameraActor>> ShotCams;
	struct FCrowdShot
	{
		double At = 0.0; // s from the test fight's start
		int32 Cam = 0;
		FString Name;
	};
	TArray<FCrowdShot> Shots;
	FString ShotDir;
	int32 ShotIndex = 0;
	int32 ShotTicks = -1; // ticks since the view moved to the shot's camera; -1 = idle

	// Per pool actor: the agent id it stands for (0 = free), the clip it plays and at what rate, its ground height (cm).
	TArray<uint32> PoolAgent;
	TArray<uint8> PoolAnim;
	TArray<float> PoolRate;
	TArray<double> PoolGround;
	TArray<bool> PoolGrounded;
	// Per sim slot: the pool actor drawing it, or INDEX_NONE.
	TArray<int32> SlotActor;

	FCollisionQueryParams TraceQuery;
	double MeshYaw = 0.0;
	double EyeZ = 160.0;     // cm: the height the occlusion line runs at, from the player's feet
	double PlayerZ = 0.0;    // cm
	int32 GroundCursor = 0;
	int32 OcclusionTraces = 0;
	double RunTime = 0.0;
	double LastArenaEvent = -1e9;
	double TestFightAt = -1.0;
	double QuitAfter = -1.0;
	double NextLog = 10.0;
	FVector TestFightPoint = FVector::ZeroVector;
	int32 TestStrikes = 0;
	bool bTestFightStarted = false;
	bool bTestDeathRaised = false;
	ftcore::CrowdCounts AtTestFight;
	ftcore::CrowdCounts AtArena;
	int32 ArenaFed = 0;   // fight events passed on from the arena
	int32 ArenaSeen = 0;  // arena events of any kind
	bool bRunning = false;
};
