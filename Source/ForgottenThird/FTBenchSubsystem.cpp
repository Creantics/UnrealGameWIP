#include "FTBenchSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "ForgottenThird.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "RHI.h"
#include "RenderCore.h"
#include "UnrealClient.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"

#include "ftcore/MassingPlan.h"

namespace
{
// The sample buffer holds the capture's seconds at this frame rate, made once before the capture starts; a faster run
// drops the frames past the end and says so in the log and the summary.
constexpr double kMaxFps = 500.0;
// Longer captures are refused (the buffer would pass about 40 MB).
constexpr double kMaxSeconds = 1800.0;
// Frames over 33 ms written out by name (the counts cover all of them).
constexpr std::size_t kListedHitches = 64;
// How long the CSV writer may take after the capture ends, in wall-clock seconds, before the run quits anyway.
constexpr double kFlushLimit = 60.0;

// The settings that decide what the look costs, logged at the start of every run.
const TCHAR* const kSettings[] = {
	TEXT("sg.ResolutionQuality"), TEXT("sg.ViewDistanceQuality"), TEXT("sg.AntiAliasingQuality"), TEXT("sg.ShadowQuality"),
	TEXT("sg.GlobalIlluminationQuality"), TEXT("sg.ReflectionQuality"), TEXT("sg.PostProcessQuality"),
	TEXT("sg.TextureQuality"), TEXT("sg.EffectsQuality"), TEXT("sg.FoliageQuality"), TEXT("sg.ShadingQuality"),
	TEXT("r.ScreenPercentage"), TEXT("r.ScreenPercentage.Default"), TEXT("r.ScreenPercentage.Default.Desktop.Mode"),
	TEXT("r.AntiAliasingMethod"), TEXT("r.TSR.History.ScreenPercentage"), TEXT("r.DynamicGlobalIlluminationMethod"),
	TEXT("r.ReflectionMethod"), TEXT("r.Lumen.HardwareRayTracing"), TEXT("r.Lumen.HardwareRayTracing.LightingMode"),
	TEXT("r.RayTracing"), TEXT("r.RayTracing.Shadows"), TEXT("r.Nanite"), TEXT("r.Shadow.Virtual.Enable"),
	TEXT("r.MegaLights.EnableForProject"), TEXT("r.MegaLights.Allow"), TEXT("r.HairStrands.Enable"),
	TEXT("r.Substrate"), TEXT("r.Streaming.PoolSize"), TEXT("r.Streaming.LimitPoolSizeToVRAM"), TEXT("r.VSync"),
	TEXT("t.MaxFPS"), TEXT("r.DynamicRes.OperationMode"), TEXT("r.PSOPrecaching"), TEXT("r.ShaderPipelineCache.Enabled"),
	TEXT("r.D3D12.PSO.DiskCache"), TEXT("gc.TimeBetweenPurgingPendingKillObjects"), TEXT("gc.AllowIncrementalGather"),
	TEXT("gc.IncrementalReachabilityTimeLimit"), TEXT("r.GPUCsvStatsEnabled"),
};

double SinceStart() { return FPlatformTime::Seconds() - GStartTime; }
} // namespace

UFTBenchSubsystem::UFTBenchSubsystem() = default;
UFTBenchSubsystem::~UFTBenchSubsystem() = default;

bool UFTBenchSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if CSV_PROFILER
	const UWorld* World = Cast<UWorld>(Outer);
	FString Want;
	return World && World->IsGameWorld() && FParse::Value(FCommandLine::Get(), TEXT("Bench="), Want) && !Want.IsEmpty() &&
		Super::ShouldCreateSubsystem(Outer);
#else
	(void)Outer;
	return false;
#endif
}

void UFTBenchSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	BeginPlayAt = SinceStart();
	const TCHAR* Cmd = FCommandLine::Get();
	FParse::Value(Cmd, TEXT("Bench="), Name);
	if (!FParse::Value(Cmd, TEXT("BenchOut="), OutDir) || OutDir.IsEmpty())
	{
		OutDir = FPaths::ProfilingDir() / TEXT("Bench");
	}
	FParse::Value(Cmd, TEXT("BenchWarmup="), Warmup);
	FParse::Value(Cmd, TEXT("BenchWarmFrames="), WarmGate.minSmoothFrames);
	FParse::Value(Cmd, TEXT("BenchWarmMax="), WarmGate.maxSeconds);
	FParse::Value(Cmd, TEXT("BenchSeconds="), Seconds);
	FParse::Value(Cmd, TEXT("BenchSide="), MassingSide);
	FParse::Value(Cmd, TEXT("BenchLeadAhead="), LeadAhead);
	bQuit = !FParse::Param(Cmd, TEXT("BenchNoQuit"));
	if (FParse::Value(Cmd, TEXT("BenchFixedStep="), FixedHz) && FixedHz > 0.0)
	{
		FApp::SetUseFixedTimeStep(true);
		FApp::SetFixedDeltaTime(1.0 / FixedHz);
	}
	FString PathFile;
	if (FParse::Value(Cmd, TEXT("BenchPath="), PathFile) && !PathFile.IsEmpty())
	{
		if (FPaths::IsRelative(PathFile))
		{
			PathFile = FPaths::ProjectDir() / TEXT("Data/Bench") / PathFile;
		}
		FString Text;
		int Bad = -1;
		const bool bRead = FFileHelper::LoadFileToString(Text, *PathFile);
		const FTCHARToUTF8 Utf8(*Text);
		if (!bRead || !Path.parse(std::string_view(Utf8.Get(), static_cast<std::size_t>(Utf8.Length())), &Bad))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("Bench: can't use the path %s (line %d)."), *PathFile, Bad);
			Phase = EPhase::Done;
			if (bQuit)
			{
				FTRequestExit(1, TEXT("Bench"));
			}
			return;
		}
		bHasPath = true;
		Seconds = Path.duration();
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Camera = InWorld.SpawnActor<ACameraActor>(Spawn);
		if (Camera)
		{
			Camera->GetCameraComponent()->SetFieldOfView(70.f);
			Camera->GetCameraComponent()->bConstrainAspectRatio = false;
		}
		PlaceCamera(0.0);
	}
	FString Teleport;
	if (FParse::Value(Cmd, TEXT("BenchTeleport="), Teleport, false) && !Teleport.IsEmpty())
	{
		TArray<FString> Parts;
		Teleport.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 2 && bHasPath)
		{
			TeleportAt = FCString::Atod(*Parts[0]);
			TeleportPathTo = FCString::Atod(*Parts[1]);
		}
		else if (Parts.Num() == 5)
		{
			TeleportAt = FCString::Atod(*Parts[0]);
			const double X = FCString::Atod(*Parts[1]);
			const double Y = FCString::Atod(*Parts[2]);
			const double Z = FCString::Atod(*Parts[3]);
			if (MassingSide > 0.0)
			{
				const ftcore::MassingVec E = ftcore::massingEnginePoint(MassingSide, X, Y, Z);
				TeleportTo = FVector(E.x, E.y, E.z) * 100.0;
			}
			else
			{
				TeleportTo = FVector(X, Y, Z) * 100.0;
			}
			TeleportYaw = FCString::Atod(*Parts[4]);
			bTeleportPawn = true;
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Bench: -BenchTeleport=%s isn't s,pathSeconds (with -BenchPath) or s,x,y,z,yaw; no teleport."),
				*Teleport);
		}
	}
	FString ExecList;
	if (FParse::Value(Cmd, TEXT("BenchExec="), ExecList, false) && !ExecList.IsEmpty())
	{
		TArray<FString> Items;
		ExecList.ParseIntoArray(Items, TEXT("|"));
		for (const FString& Item : Items)
		{
			FString At;
			FString Line;
			const bool bSplit = Item.Split(TEXT(":"), &At, &Line);
			At.TrimStartAndEndInline();
			Line.TrimStartAndEndInline();
			if (bSplit && At.IsNumeric() && !Line.IsEmpty())
			{
				Execs.Emplace(FCString::Atod(*At), Line);
			}
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Bench: -BenchExec item '%s' isn't seconds:command; skipped."), *Item);
			}
		}
		Execs.Sort([](const TPair<double, FString>& A, const TPair<double, FString>& B) { return A.Key < B.Key; });
	}
	WarmGate.minSeconds = Warmup;
	if (!(Seconds > 0.0 && Seconds <= kMaxSeconds && Warmup >= 0.0))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("Bench %s: capture of %.1f s after a %.1f s warm-up refused (0 to %.0f s)."), *Name, Seconds,
			Warmup, kMaxSeconds);
		Phase = EPhase::Done;
		if (bQuit)
		{
			FTRequestExit(1, TEXT("Bench"));
		}
		return;
	}
	Stats = std::make_unique<ftcore::FrameStats>(static_cast<std::size_t>(FMath::CeilToDouble(Seconds * kMaxFps)) + 1);
	LastRealTime = -1.0;
	UE_LOG(LogForgottenThird, Display, TEXT("Bench %s: %s, warm-up %.1f s, capture %.1f s, %s; world begun %.2f s after start.\n%s"),
		*Name, bHasPath ? *FString::Printf(TEXT("rail path of %d keys"), static_cast<int32>(Path.size())) : TEXT("game camera"),
		Warmup, Seconds, FixedHz > 0.0 ? *FString::Printf(TEXT("fixed step %.0f Hz"), FixedHz) : TEXT("real time"), BeginPlayAt,
		*SettingsReport());
}

void UFTBenchSubsystem::Deinitialize()
{
	if (Phase == EPhase::Capture)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Bench %s: the world ended during the capture; the summary covers the frames so far."), *Name);
		WriteSummary();
	}
	Super::Deinitialize();
}

TStatId UFTBenchSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTBenchSubsystem, STATGROUP_Tickables);
}

void UFTBenchSubsystem::PlaceCamera(double PathSeconds)
{
	if (!bHasPath || !Camera)
	{
		return;
	}
	const ftcore::BenchPose P = Path.sample(PathSeconds);
	FVector Eye(P.eye[0], P.eye[1], P.eye[2]);
	FVector At(P.at[0], P.at[1], P.at[2]);
	if (MassingSide > 0.0)
	{
		const ftcore::MassingVec E = ftcore::massingEnginePoint(MassingSide, Eye.X, Eye.Y, Eye.Z);
		const ftcore::MassingVec A = ftcore::massingEnginePoint(MassingSide, At.X, At.Y, At.Z);
		Eye = FVector(E.x, E.y, E.z) * 100.0;
		At = FVector(A.x, A.y, A.z) * 100.0;
	}
	Camera->SetActorLocationAndRotation(Eye, (At - Eye).Rotation());
	// -BenchLeadAhead: the possessed lead stands that many metres ahead of the eye along the look, on a floor 1.6 m under
	// the eye, facing the camera, so a groom or outfit's cost is measured on the same path with and without a lead.
	const UWorld* World = GetWorld();
	const APlayerController* Pc = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Lead = Pc ? Pc->GetPawn() : nullptr;
	if (LeadAhead > 0.0 && Lead)
	{
		const FVector Flat = FVector(At.X - Eye.X, At.Y - Eye.Y, 0.0).GetSafeNormal();
		const FVector Feet = Eye + Flat * (LeadAhead * 100.0) - FVector(0.0, 0.0, 160.0);
		Lead->SetActorLocationAndRotation(Feet + FVector(0.0, 0.0, Lead->GetSimpleCollisionHalfHeight()), (-Flat).Rotation(),
			false, nullptr, ETeleportType::TeleportPhysics);
	}
}

FString UFTBenchSubsystem::SettingsReport() const
{
	FString Out;
	for (const TCHAR* SettingName : kSettings)
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(SettingName);
		Out += FString::Printf(TEXT("  %s=%s\n"), SettingName, Var ? *Var->GetString() : TEXT("(none)"));
	}
	FIntPoint Size(0, 0);
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		Size = GEngine->GameViewport->Viewport->GetSizeXY();
	}
	Out += FString::Printf(TEXT("  viewport=%dx%d rhi=%s gpu=%s\n"), Size.X, Size.Y, GDynamicRHI ? GDynamicRHI->GetName() : TEXT("none"),
		*GRHIAdapterName);
	return Out;
}

void UFTBenchSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
#if CSV_PROFILER
	// Warm-up, capture span and the rail camera all run on the wall clock, from the world's first tick (so a fixed game
	// step or a time dilation changes neither).
	const double Now = FPlatformTime::Seconds();
	const double RealSeconds = LastRealTime < 0.0 ? 0.0 : Now - LastRealTime;
	const double RealMs = RealSeconds * 1000.0;
	LastRealTime = Now;
	++Frames;
	if (FirstFrameAt < 0.0)
	{
		FirstFrameAt = SinceStart();
	}
	UWorld* World = GetWorld();
	APlayerController* Pc = World ? World->GetFirstPlayerController() : nullptr;
	if (FirstPawnAt < 0.0 && Pc && Pc->GetPawn())
	{
		FirstPawnAt = SinceStart();
	}
	if (Camera && Pc && !bViewSet)
	{
		Pc->SetViewTarget(Camera);
		bViewSet = true;
	}
	PhaseClock += RealSeconds;
	switch (Phase)
	{
	case EPhase::Warmup:
		PlaceCamera(0.0);
		if (RealMs > 0.0 && WarmGate.step(RealMs))
		{
			Phase = EPhase::Capture;
			PhaseClock = 0.0;
			bSkipNext = true; // the next tick's time includes this one's capture start
			IFileManager::Get().MakeDirectory(*OutDir, true);
			FCsvProfiler::Get()->BeginCapture(-1, OutDir, Name + TEXT(".csv"));
			FCsvProfiler::SetMetadata(TEXT("Bench"), *Name);
			Marks = FString::Printf(TEXT("warm-up %.1f s, %d frames under %.0f ms%s"), WarmGate.seconds(), WarmGate.smoothFrames(),
				WarmGate.smoothMs, WarmGate.timedOut() ? TEXT(", TIMED OUT (the scene never settled)") : TEXT(""));
			UE_LOG(LogForgottenThird, Display, TEXT("Bench %s: capture begun at frame %d, %.2f s after start; %s."), *Name, Frames, SinceStart(),
				*Marks);
			Marks += TEXT("\n");
		}
		break;
	case EPhase::Capture:
	{
		if (TeleportAt >= 0.0 && PhaseClock >= TeleportAt)
		{
			TeleportAt = -1.0;
			APawn* Pawn = Pc ? Pc->GetPawn() : nullptr;
			if (bTeleportPawn && Pawn)
			{
				const FRotator Face(0.0, TeleportYaw, 0.0);
				Pawn->SetActorLocationAndRotation(TeleportTo + FVector(0.0, 0.0, Pawn->GetSimpleCollisionHalfHeight()), Face, false, nullptr,
					ETeleportType::TeleportPhysics);
				Pc->SetControlRotation(Face);
			}
			else if (!bTeleportPawn)
			{
				PathOffset = TeleportPathTo - PhaseClock;
			}
			const FString Mark = FString::Printf(TEXT("teleport at capture frame %d (%.1f s)%s"), static_cast<int32>(Stats->size()), PhaseClock,
				bTeleportPawn && !Pawn ? TEXT(": no pawn, skipped") : TEXT(""));
			UE_LOG(LogForgottenThird, Display, TEXT("Bench %s: %s."), *Name, *Mark);
			Marks += Mark + TEXT("\n");
		}
		const bool bCensusDue = bCensusPending; // the census's second half runs on the frame after its first
		while (NextExec < Execs.Num() && PhaseClock >= Execs[NextExec].Key)
		{
			const FString Mark = FString::Printf(TEXT("exec '%s' at capture frame %d (%.1f s)"), *Execs[NextExec].Value,
				static_cast<int32>(Stats->size()), PhaseClock);
			UE_LOG(LogForgottenThird, Display, TEXT("Bench %s: %s."), *Name, *Mark);
			Marks += Mark + TEXT("\n");
			if (Execs[NextExec].Value.Equals(TEXT("ftcensus"), ESearchCase::IgnoreCase))
			{
				CensusStart(*World);
			}
			else if (GEngine)
			{
				GEngine->Exec(World, *Execs[NextExec].Value);
			}
			++NextExec;
		}
		if (bCensusDue)
		{
			CensusMoved();
		}
		PlaceCamera(PhaseClock + PathOffset);
		if (bSkipNext)
		{
			bSkipNext = false;
			PhaseClock = 0.0;
			break;
		}
		ftcore::FrameSample S;
		S.frameMs = static_cast<float>(RealMs);
		S.gameMs = static_cast<float>(FPlatformTime::ToMilliseconds(GGameThreadTime));
		S.renderMs = static_cast<float>(FPlatformTime::ToMilliseconds(GRenderThreadTime));
		S.gpuMs = static_cast<float>(FPlatformTime::ToMilliseconds64(RHIGetGPUFrameCycles(0)));
		S.drawCalls = static_cast<float>(GNumDrawCallsRHI[0]);
		S.primitives = static_cast<float>(GNumPrimitivesDrawnRHI[0]);
		if (!Stats->add(S) && Dropped++ == 0)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Bench %s: sample buffer full (%.0f fps over %.0f s); later frames are not kept."), *Name,
				kMaxFps, Seconds);
		}
		if (PhaseClock + PathOffset >= Seconds)
		{
			FinishCapture();
		}
		break;
	}
	case EPhase::Flush:
		if ((CsvFile.IsValid() && CsvFile.IsReady() && !FCsvProfiler::Get()->IsWritingFile()) || PhaseClock > kFlushLimit)
		{
			Phase = EPhase::Done;
			UE_LOG(LogForgottenThird, Display, TEXT("Bench %s: CSV %s."), *Name,
				CsvFile.IsValid() && CsvFile.IsReady() ? *CsvFile.Get() : TEXT("not confirmed written (flush limit reached)"));
			if (bQuit)
			{
				FTRequestExit(0, TEXT("Bench"));
			}
		}
		break;
	case EPhase::Done:
		break;
	}
#else
	(void)DeltaTime;
#endif
}

void UFTBenchSubsystem::FinishCapture()
{
#if CSV_PROFILER
	CsvFile = FCsvProfiler::Get()->EndCapture();
#endif
	WriteSummary();
	Phase = EPhase::Flush;
	PhaseClock = 0.0;
}

void UFTBenchSubsystem::WriteSummary()
{
	if (!Stats)
	{
		return;
	}
	const ftcore::FrameSummary Sum = Stats->summary();
	const FString Line = FString::Printf(
		TEXT("Bench %s: %d frames; frame %.1f ms median, p95 %.1f, p99 %.1f, max %.1f; over 33 ms %d, over 50 ms %d; ")
		TEXT("game %.1f (p99 %.1f); render %.1f (p99 %.1f); GPU %.1f (p99 %.1f); draw calls %.0f (p99 %.0f); primitives %.0f; ")
		TEXT("world begun %.2f s, first frame %.2f s, first pawn %.2f s after start; %d frames dropped (buffer full)."),
		*Name, static_cast<int32>(Sum.count), Sum.frame.median, Sum.frame.p95, Sum.frame.p99, Sum.frame.max,
		static_cast<int32>(Sum.over33), static_cast<int32>(Sum.over50), Sum.game.median, Sum.game.p99, Sum.render.median,
		Sum.render.p99, Sum.gpu.median, Sum.gpu.p99, Sum.drawCalls.median, Sum.drawCalls.p99, Sum.primitives.median,
		BeginPlayAt, FirstFrameAt, FirstPawnAt, Dropped);
	UE_LOG(LogForgottenThird, Display, TEXT("%s"), *Line);
	FString Text = Line + TEXT("\n\n") + Marks + TEXT("\nframes over 33 ms (capture frame, frame / game / render / GPU ms):\n");
	ftcore::FrameHitch Hitches[kListedHitches];
	const std::size_t N = Stats->hitches(ftcore::kHitchMs, Hitches, kListedHitches);
	for (std::size_t i = 0; i < N && i < kListedHitches; ++i)
	{
		const ftcore::FrameSample& S = Hitches[i].sample;
		Text += FString::Printf(TEXT("  %d: %.1f / %.1f / %.1f / %.1f\n"), static_cast<int32>(Hitches[i].index), S.frameMs, S.gameMs,
			S.renderMs, S.gpuMs);
	}
	if (N > kListedHitches)
	{
		Text += FString::Printf(TEXT("  ... %d more\n"), static_cast<int32>(N - kListedHitches));
	}
	Text += TEXT("\nsettings:\n") + SettingsReport();
	IFileManager::Get().MakeDirectory(*OutDir, true);
	FFileHelper::SaveStringToFile(Text, *(OutDir / Name + TEXT("_summary.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

namespace
{
FString CensusOwner(const UActorComponent& Component)
{
	const AActor* Owner = Component.GetOwner();
	return FString::Printf(TEXT("%s/%s"), Owner ? *Owner->GetClass()->GetName() : TEXT("(none)"), *Component.GetClass()->GetName());
}

void CensusLog(const TCHAR* What, TMap<FString, int32>& Counts)
{
	Counts.ValueSort([](int32 A, int32 B) { return A > B; });
	int32 Total = 0;
	for (const TPair<FString, int32>& Row : Counts)
	{
		Total += Row.Value;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Bench census: %s: %d in all."), What, Total);
	int32 Shown = 0;
	for (const TPair<FString, int32>& Row : Counts)
	{
		if (++Shown > 25)
		{
			break;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Bench census:   %5d  %s"), Row.Value, *Row.Key);
	}
}
} // namespace

void UFTBenchSubsystem::CensusStart(UWorld& World)
{
	// Ray-tracing dynamic geometry: skinned meshes (r.RayTracing.Geometry.SkeletalMeshes), grooms and static meshes that
	// evaluate WPO in ray tracing; near = within 30 m of the view.
	FVector Eye = FVector::ZeroVector;
	if (const APlayerController* PC = World.GetFirstPlayerController())
	{
		FRotator Unused;
		PC->GetPlayerViewPoint(Eye, Unused);
	}
	TMap<FString, int32> Dynamic;
	TMap<FString, int32> Near;
	CensusSnap.Reset();
	for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
	{
		UPrimitiveComponent* Prim = *It;
		if (!Prim || Prim->GetWorld() != &World || !Prim->IsRegistered() || !Prim->ShouldRender())
		{
			continue;
		}
		CensusSnap.Emplace(Prim, Prim->GetComponentTransform());
		if (!Prim->bVisibleInRayTracing)
		{
			continue;
		}
		const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Prim);
		const bool bDynamic = Prim->IsA<USkinnedMeshComponent>() || Prim->GetClass()->GetName().Contains(TEXT("Groom"))
			|| (Static && Static->bEvaluateWorldPositionOffsetInRayTracing);
		if (!bDynamic)
		{
			continue;
		}
		const FString Key = CensusOwner(*Prim);
		Dynamic.FindOrAdd(Key) += 1;
		if (FVector::Dist(Prim->Bounds.Origin, Eye) < 3000.0)
		{
			Near.FindOrAdd(Key) += 1;
		}
	}
	CensusLog(TEXT("ray-tracing dynamic geometry (skinned, groom, WPO in RT)"), Dynamic);
	CensusLog(TEXT("of those within 30 m of the view"), Near);
	bCensusPending = true;
}

void UFTBenchSubsystem::CensusMoved()
{
	bCensusPending = false;
	TMap<FString, int32> Moved;
	for (const TPair<TWeakObjectPtr<USceneComponent>, FTransform>& Row : CensusSnap)
	{
		const USceneComponent* Component = Row.Key.Get();
		if (Component && !Component->GetComponentTransform().Equals(Row.Value, 0.01))
		{
			Moved.FindOrAdd(CensusOwner(*Component)) += 1;
		}
	}
	CensusLog(TEXT("rendered primitives moved in one frame"), Moved);
	CensusSnap.Empty();
}
