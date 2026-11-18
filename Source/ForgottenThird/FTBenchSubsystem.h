#pragma once

// The performance benchmark run (dev builds only; the CSV profiler is compiled out of Shipping):
//
//   <map> -game -RenderOffScreen -ResX=2560 -ResY=1440 -Bench=<name> -BenchOut=<dir>
//         [-BenchPath=<file> [-BenchSide=<m>] [-BenchLeadAhead=<m>]] [-BenchSeconds=<s>] [-BenchWarmup=<s>] [-BenchFixedStep=<hz>] [-BenchNoQuit]
//         [-BenchWarmFrames=<n>] [-BenchWarmMax=<s>] [-BenchTeleport=<s>,<path s> | <s>,<x>,<y>,<z>,<yaw>] [-BenchExec="<s>:<cmd>|..."]
//
// With -BenchPath (a ftcore::BenchPath text file, engine cm, or plan metres as -WorldEye takes them with -BenchSide; relative paths are under the project's Data/Bench) a rail
// camera holds the first key for -BenchWarmup seconds (default 5) and then runs the path; without it the run keeps the
// game's own camera (the arena runner, a world-wave boot) for -BenchSeconds (default 60) after the warm-up. The CSV
// profiler is started and stopped here, so the file <dir>/<name>.csv is complete before the game quits. Each frame's
// real time (FPlatformTime, not the fixed step), game, render, RHI-side GPU time, draw calls and primitives go into
// one ftcore::FrameStats buffer made before the capture, sized for the capture's seconds at 500 fps (captures over
// 1800 s are refused; frames past the end are counted as dropped). The first capture frame is not kept: it carries the
// capture's own start. At the end one "Bench:" line (median, p95, p99, max, counts over 33 and 50 ms) and the frames
// over 33 ms are logged and written to <dir>/<name>_summary.txt, with the live values of the settings that decide the
// look's cost and the seconds from process start to the world's first frame and to the first frame with a possessed
// pawn. Warm-up, capture span and the rail camera run on the wall clock from the world's first tick. -BenchFixedStep
// runs the game clock at a fixed rate for A/B cost comparisons; real time (the default) is the one for hitch numbers.
// The warm-up ends once its seconds have passed AND -BenchWarmFrames (default 60) frames under 100 ms have run, or at
// -BenchWarmMax seconds (default 120, logged as timed out): a cold start's compile frames no longer end it at frame 2.
// -BenchTeleport at capture second s jumps the rail to path second <path s> (with -BenchPath), or sets the possessed
// pawn down at x,y,z (m; plan metres with -BenchSide) facing yaw: a scripted streaming jump inside the capture.
// -BenchExec runs console commands at capture seconds (ProfileGPU, stat dumpave ...). The summary names the capture
// frame of the teleport and of each command, so their own hitches can be told apart.
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "Async/Future.h"

#include "ftcore/BenchPath.h"
#include "ftcore/BenchWarmup.h"
#include "ftcore/FrameStats.h"

#include <memory>

#include "FTBenchSubsystem.generated.h"

class ACameraActor;

UCLASS()
class FORGOTTENTHIRD_API UFTBenchSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UFTBenchSubsystem();
	virtual ~UFTBenchSubsystem() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return false; }

private:
	enum class EPhase : uint8 { Warmup, Capture, Flush, Done };

	void PlaceCamera(double PathSeconds);
	FString SettingsReport() const;
	void FinishCapture();
	void WriteSummary();

	std::unique_ptr<ftcore::FrameStats> Stats;
	ftcore::BenchPath Path;
	bool bHasPath = false;
	UPROPERTY()
	TObjectPtr<ACameraActor> Camera;

	FString Name;
	FString OutDir;
	double Warmup = 5.0;
	double Seconds = 60.0;
	double FixedHz = 0.0;
	double LeadAhead = 0.0;   // above 0: the lead stands this many metres ahead of the rail camera
	double MassingSide = 0.0; // above 0: the path's keys are plan metres (the -WorldEye frame) on a square this wide
	bool bQuit = true;

	EPhase Phase = EPhase::Warmup;
	double PhaseClock = 0.0;     // wall-clock seconds in the current phase
	double LastRealTime = -1.0;  // FPlatformTime::Seconds() at the previous tick; below 0 before the first
	int32 Dropped = 0;           // capture frames past the buffer's end
	bool bSkipNext = false;      // the first capture tick is not kept (it carries the capture's start)
	double BeginPlayAt = -1.0;   // seconds from process start
	double FirstFrameAt = -1.0;
	double FirstPawnAt = -1.0;
	int32 Frames = 0;
	bool bViewSet = false;
	TSharedFuture<FString> CsvFile;

	ftcore::BenchWarmup WarmGate;
	double PathOffset = 0.0;       // added to the capture clock for the rail (a -BenchTeleport rail jump)
	double TeleportAt = -1.0;       // capture second of the teleport; below 0: none
	double TeleportPathTo = -1.0;   // rail form: the path second to jump to
	FVector TeleportTo = FVector::ZeroVector; // pawn form (cm)
	double TeleportYaw = 0.0;
	bool bTeleportPawn = false;
	TArray<TPair<double, FString>> Execs; // capture second, console command; sorted by second
	int32 NextExec = 0;
	FString Marks;                  // warm-up, teleport and command frames, for the summary

	// "ftcensus" in -BenchExec: logs the ray-tracing dynamic geometry (skinned, groom and WPO meshes visible in ray
	// tracing) and, a frame later, the primitives whose transform changed in that frame, both by owner class.
	void CensusStart(UWorld& World);
	void CensusMoved();
	TArray<TPair<TWeakObjectPtr<USceneComponent>, FTransform>> CensusSnap;
	bool bCensusPending = false;
};
