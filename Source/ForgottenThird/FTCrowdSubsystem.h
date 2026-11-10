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
//       [-CrowdSeed=<n>] [-CrowdTestFight[=<seconds>]] [-CrowdQuitAfter=<seconds>] [-CrowdPlayerAt=<x>,<y>]
//   -CrowdPlayerAt moves the player there (metres, on the ground) on the first tick, so a test runs where the map's
//   start isn't (the playable core).
//
// The streets table is the massing one (id, kind, width_m, pts, pavement_m; plan frame, a square -CrowdSide metres across)
// and the sim's frame is the engine's in metres, the same as the arena's (FTArenaRunner::ToWorld is P * 100). Without
// -CrowdStreets nothing is created. The skin defaults to the test human and its walk and idle clips; a skin that doesn't
// load leaves the sim running undrawn (the log still counts). -CrowdTestFight scripts a fight 15 m from the player at
// <seconds> (default 20): a fight start, then a strike every half second for 10 s and a death at the end; with or
// without it, the counts are logged every 10 s ("Crowd: t=...") and -CrowdQuitAfter ends the game with the final line
// ("Crowd: final ...", exit 0, or 1 when the scripted fight made nobody flee or nobody resume). The test fight stands on
// the street node within 40 m of a point 15 m east of the player with the most civilians round it. Run checks folded
// into the verdict: the first second (everyone within the reaction radius turned away and 1.5 m farther at +1 s, none
// closer), no running member stuck, and no drawn member inside a building or on a roof, any frame.

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
	void ScriptFight(const FVector& PlayerAt);
	void PickTestFight(const FVector& PlayerAt);
	void CheckFootprint(int32 Actor, const FVector& Feet);
	struct FViewCount
	{
		int32 N = 0;          // drawn members in frame and in sight (a line from the eye to the chest)
		int32 Calm = 0;       // of them walking or standing about
		float MinGap = 1e9f;  // m, least distance between two of them
		double NearestPx = 0; // the nearest one's height in the still, px of 900
	};
	FViewCount CountInView(int32 Cam, TArray<uint32>* Ids) const;
	bool Clear(const FVector& Eye, const FVector& To) const;
	void BuildMassing();
	void LogGroundAt();
	void Shoot(APlayerController* PC, const FVector& PlayerAt);
	void LogCounts(const TCHAR* Label) const;
	// S-332: the cameras the crowd must not pop in front of (the player's view and the stills' cameras), every frame.
	void FeedViews(APlayerController* PC);
	// S-332: the party stands at the test fight point for the stills.
	void MovePartyToFight(APlayerController* PC);
	// S-332: true distances behind the stills: members overlapping on screen (camA/camB), and pairs seen from above (camC).
	void LogScreenPairs(int32 Cam, const FString& Name) const;
	void LogTopPairs(const FString& Name) const;
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
	// -CrowdShots=<dir>: two fixed cameras round the test fight, one orthographic straight down over it (S-332), and the
	// stills they take.
	UPROPERTY()
	TArray<TObjectPtr<ACameraActor>> ShotCams;
	struct FCrowdShot
	{
		double At = 0.0; // s from the test fight's start
		int32 Cam = 0;
		int32 Phase = 0; // 0 calm, 1 fight + 1 s, 2 flee, 3 + 30 s
		FString Name;
	};
	TArray<FCrowdShot> Shots;
	FString ShotDir;
	int32 ShotIndex = 0;
	int32 ShotTicks = -1; // ticks since the view moved to the shot's camera; -1 = idle
	TArray<uint32> CalmInView[2]; // agent ids each camera saw in its calm still
	int32 CalmCount[2] = {-1, -1};  // calm members each camera saw in its calm still
	bool bCamsOk = true;            // both cameras framed the fight as asked, and the crowd came back into view
	bool bRefillOk = true;
	float RunMinPair = 1e9f;        // m, every frame
	float RunMinWall = 1e9f;        // m, every frame
	// S-332 run checks: member-frames in the run pose below walking speed, the largest knot standing within 3 m.
	uint64 SlowRunFrames = 0;
	uint32 WorstGroup = 0;
	uint32 GroupTick = 0;
	bool bPartyMoved = false;
	struct FMassBox
	{
		FVector2D C, Ax, Half; // centre, unit x axis, half sizes (cm)
		double ZMin = 0.0, ZMax = 0.0;
	};
	TArray<FMassBox> MassBoxes;
	TMap<FIntPoint, TArray<int32>> MassGrid; // 10 m cells: boxes reaching within a metre of the cell
	double MinMassClear = 1e9;              // cm, least distance from a drawn member's feet to a massing box
	bool bGroundLogged = false;

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
	bool bPlayerMove = false;
	FVector2D PlayerMoveTo = FVector2D::ZeroVector;
	double NextLog = 10.0;
	FVector TestFightPoint = FVector::ZeroVector;
	int32 TestStrikes = 0;
	bool bTestFightStarted = false;
	bool bTestFightPicked = false;
	double TestFightStartedAt = -1.0;
	// The first second: who stood within the reaction radius when the fight started, checked 1 s later.
	ftcore::CrowdReactionProbe FirstSecond;
	bool bFirstSecondDone = false;
	bool bFirstSecondOk = true;
	// Footprint check (runs with -CrowdQuitAfter): drawn members inside a building or on a roof.
	uint64 FootFrames = 0;   // member-frames
	int32 FootMembers = 0;   // distinct members
	TBitArray<> FootSeen;    // per sim slot
	int32 FootLogged = 0;
	bool bTestDeathRaised = false;
	ftcore::CrowdCounts AtTestFight;
	ftcore::CrowdCounts AtArena;
	int32 ArenaFed = 0;   // fight events passed on from the arena
	int32 ArenaSeen = 0;  // arena events of any kind
	bool bRunning = false;
};
