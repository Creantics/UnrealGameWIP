#include "FTCrowdSubsystem.h"

#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/HUD.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "WorldCollision.h"
#include "FTMeshFacing.h"
#include "ForgottenThird.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include <string>
#include <vector>

namespace
{
const TCHAR* const kDefaultSkin = TEXT("/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_human");
const TCHAR* const kDefaultWalk = TEXT("/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_humanwalk");
const TCHAR* const kDefaultIdle = TEXT("/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_humanidle");

// The walk clip's own pace (m/s) and the run clip's: the play rate is the agent's speed over it.
constexpr float kWalkClipSpeed = 1.3f;
constexpr float kRunClipSpeed = 4.2f;
// Without a run clip the walk plays this much faster, at most.
constexpr float kRunOnWalkMax = 2.2f;
// Ground lines per frame (round robin over the pool), and how far each looks up and down (cm).
constexpr int32 kGroundTracesPerFrame = 8;
constexpr double kGroundUp = 300.0;
constexpr double kGroundDown = 600.0;
constexpr double kGroundFirstSpan = 20000.0;
// Occlusion lines per frame at most (the sim also has its own budget per step).
constexpr int32 kOcclusionTracesPerFrame = 32;
// A quiet spell this long (s) between arena events makes the next attack a fight start again.
constexpr double kFightQuiet = 10.0;
// Run checks: one member may run, stop and run again a few times in a long fight, not every step.
constexpr uint32 kMaxFleeEntries = 3;
constexpr uint32 kMaxPlansOne = 12;
// The scripted fight: how far from the player, how long, how often a strike lands.
constexpr double kTestFightDistance = 15.0;
constexpr double kTestFightLength = 10.0;
// A second after the fight starts (S-267): 1.5 m along its route, facing its motion, over 0.3 m farther from the fight.
constexpr float kFirstSecondPath = 1.5f;   // m
constexpr float kFirstSecondRadial = 0.3f; // m
constexpr double kTestStrikeEvery = 0.5;
// S-271 gates: two members at least this far apart and this far inside the frontage (m), every frame.
constexpr float kGapPair = 0.6f;
constexpr float kGapWall = 0.8f;
// The stills' cameras: this far back from the fight point and this high (cm); a member is this tall (cm) and the nearest
// in sight must stand at least this many px high in a 1600x900 still.
constexpr double kCamBack[] = {1350.0, 1200.0, 1500.0};
constexpr double kCamUp = 600.0;
constexpr double kPersonHeight = 175.0;
constexpr double kMinPersonPx = 60.0;
// Massing boxes the footprint check uses: at least this tall and wide, at most this long (cm); the grid's cell.
constexpr double kMassMinHeight = 250.0;
constexpr double kMassMinSide = 200.0;
constexpr double kMassMaxSide = 30000.0;
constexpr double kMassCell = 1000.0;
// Where the ground under the streets is logged (engine cm), unless -CrowdGroundAt=X,Y says elsewhere.
constexpr double kGroundAtX = 0.0, kGroundAtY = 31050.0;

FString CrowdObjectPath(const FString& Path)
{
	return Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPaths::GetBaseFilename(Path);
}

ftcore::Vec2 ToSim(const FVector& P)
{
	return ftcore::Vec2{static_cast<float>(P.X / 100.0), static_cast<float>(P.Y / 100.0)};
}
} // namespace

UFTCrowdSubsystem* UFTCrowdSubsystem::Get(const UWorld* World)
{
	return World ? World->GetSubsystem<UFTCrowdSubsystem>() : nullptr;
}

bool UFTCrowdSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	FString Streets;
	return World && World->WorldType == EWorldType::Game && FParse::Value(FCommandLine::Get(), TEXT("CrowdStreets="), Streets) &&
		!Streets.IsEmpty() && Super::ShouldCreateSubsystem(Outer);
}

bool UFTCrowdSubsystem::Fail(const TCHAR* Why, const FString& What)
{
	UE_LOG(LogForgottenThird, Error, TEXT("Crowd: %s %s"), Why, *What);
	FTRequestExit(1, TEXT("Crowd"));
	bRunning = false;
	return false;
}

void UFTCrowdSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	const TCHAR* Cmd = FCommandLine::Get();
	FString StreetsPath, TuningPath;
	FParse::Value(Cmd, TEXT("CrowdStreets="), StreetsPath);
	if (!FParse::Value(Cmd, TEXT("CrowdTuning="), TuningPath))
	{
		TuningPath = FPaths::ProjectDir() / TEXT("Data/Crowd/crowd.json");
	}
	double Side = 1000.0;
	FParse::Value(Cmd, TEXT("CrowdSide="), Side);
	uint32 Seed = 1;
	FParse::Value(Cmd, TEXT("CrowdSeed="), Seed);

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *TuningPath))
	{
		Fail(TEXT("couldn't read the tuning"), TuningPath);
		return;
	}
	std::string Error;
	if (!ftcore::importCrowdTuning(TCHAR_TO_UTF8(*Text), Tuning, &Error))
	{
		Fail(TEXT("bad tuning:"), FString(UTF8_TO_TCHAR(Error.c_str())));
		return;
	}
	if (Side <= 0.0 || !FFileHelper::LoadFileToString(Text, *StreetsPath))
	{
		Fail(TEXT("couldn't read the streets (or -CrowdSide isn't above 0):"), StreetsPath);
		return;
	}
	std::vector<ftcore::CrowdStreet> Streets;
	if (!ftcore::importCrowdStreets(TCHAR_TO_UTF8(*Text), Side, Streets, &Error) || !Graph.build(Streets, Tuning.graph, &Error))
	{
		Fail(TEXT("bad streets:"), FString(UTF8_TO_TCHAR(Error.c_str())));
		return;
	}
	// -CrowdShift=auto (test aid for maps with no city, such as the arena): move the streets so the street node nearest
	// the world origin sits on it. -CrowdShift=X,Y moves them by that many metres.
	FString Shift;
	if (FParse::Value(Cmd, TEXT("CrowdShift="), Shift) && !Shift.IsEmpty())
	{
		ftcore::Vec2 By{0.f, 0.f};
		FString SX, SY;
		if (Shift == TEXT("auto"))
		{
			const uint32 N = Graph.nearest(ftcore::Vec2{0.f, 0.f}, 1.0e5f);
			if (N != UINT32_MAX)
			{
				By = ftcore::Vec2{-Graph.node(N).x, -Graph.node(N).y};
			}
		}
		else if (Shift.Split(TEXT(","), &SX, &SY))
		{
			By = ftcore::Vec2{FCString::Atof(*SX), FCString::Atof(*SY)};
		}
		for (ftcore::CrowdStreet& S : Streets)
		{
			for (ftcore::Vec2& Q : S.points)
			{
				Q.x += By.x;
				Q.y += By.y;
			}
		}
		if (!Graph.build(Streets, Tuning.graph, &Error))
		{
			Fail(TEXT("bad streets after the shift:"), FString(UTF8_TO_TCHAR(Error.c_str())));
			return;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: streets shifted by (%.1f, %.1f) m."), By.x, By.y);
	}
	if (!Sim.init(Graph, Tuning, Seed))
	{
		Fail(TEXT("no spawnable street in"), StreetsPath);
		return;
	}
	Sim.setOcclusion(&UFTCrowdSubsystem::Occluded, this);
	TraceQuery = FCollisionQueryParams(SCENE_QUERY_STAT(FTCrowdTrace), false);
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %d streets, %u nodes, %u edges, %u components, %u spawnable; pool %u, drawn %u, target %u."),
		static_cast<int32>(Streets.size()), Graph.nodeCount(), Graph.edgeCount() / 2, Graph.componentCount(), Graph.spawnableCount(),
		Tuning.maxAgents, Tuning.maxDrawn, Tuning.targetAgents);

	LoadArt();
	SlotActor.Init(INDEX_NONE, static_cast<int32>(Sim.capacity()));
	FootSeen.Init(false, static_cast<int32>(Sim.capacity()));
	BuildMassing();

	double TestAt = 20.0;
	if (FParse::Param(Cmd, TEXT("CrowdTestFight")) || FParse::Value(Cmd, TEXT("CrowdTestFight="), TestAt))
	{
		TestFightAt = TestAt;
	}
	FParse::Value(Cmd, TEXT("CrowdQuitAfter="), QuitAfter);
	FString MoveTo, MX, MY;
	if (FParse::Value(Cmd, TEXT("CrowdPlayerAt="), MoveTo) && MoveTo.Split(TEXT(","), &MX, &MY))
	{
		bPlayerMove = true;
		PlayerMoveTo = FVector2D(FCString::Atod(*MX) * 100.0, FCString::Atod(*MY) * 100.0);
	}
	if (FParse::Value(Cmd, TEXT("CrowdShots="), ShotDir) && !ShotDir.IsEmpty() && TestFightAt >= 0.0)
	{
		// Calm, 1 s into the fight, mid-flee (a short run of frames from the low camera), 30 s after.
		const double Times[] = {-2.0, 1.0, 4.0, 30.0};
		const TCHAR* Labels[] = {TEXT("calm"), TEXT("fight1s"), TEXT("flee"), TEXT("after30s")};
		for (int32 T = 0; T < 4; ++T)
		{
			for (int32 Cam = 0; Cam < 2; ++Cam)
			{
				const int32 Frames = (T == 2 && Cam == 0) ? 4 : 1;
				for (int32 F = 0; F < Frames; ++F)
				{
					Shots.Add({Times[T] + F * 0.25, Cam, T, FString::Printf(TEXT("%s/crowd_%d_%s_cam%c%s.png"), *ShotDir, T, Labels[T], TEXT('A') + Cam,
						Frames > 1 ? *FString::Printf(TEXT("_f%d"), F) : TEXT(""))});
				}
			}
		}
	}
	bRunning = true;
}

void UFTCrowdSubsystem::LoadArt()
{
	const TCHAR* Cmd = FCommandLine::Get();
	FString SkinPath = kDefaultSkin, WalkPath = kDefaultWalk, IdlePath = kDefaultIdle, RunPath, CowerPath;
	FParse::Value(Cmd, TEXT("CrowdSkin="), SkinPath);
	FParse::Value(Cmd, TEXT("CrowdWalk="), WalkPath);
	FParse::Value(Cmd, TEXT("CrowdIdle="), IdlePath);
	FParse::Value(Cmd, TEXT("CrowdRun="), RunPath);
	FParse::Value(Cmd, TEXT("CrowdCower="), CowerPath);
	Skin = LoadObject<USkeletalMesh>(nullptr, *CrowdObjectPath(SkinPath));
	if (!Skin)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Crowd: couldn't load the skin %s; the crowd runs undrawn."), *SkinPath);
		return;
	}
	auto Clip = [this](const FString& Path, const TCHAR* Role) -> UAnimSequence*
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		UAnimSequence* A = LoadObject<UAnimSequence>(nullptr, *CrowdObjectPath(Path));
		if (!A || A->GetSkeleton() != Skin->GetSkeleton())
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Crowd: the %s clip %s %s; it falls back."), Role, *Path,
				A ? TEXT("is on another skeleton") : TEXT("doesn't load"));
			return nullptr;
		}
		return A;
	};
	WalkClip = Clip(WalkPath, TEXT("walk"));
	RunClip = Clip(RunPath, TEXT("run"));
	IdleClip = Clip(IdlePath, TEXT("idle"));
	CowerClip = Clip(CowerPath, TEXT("cower"));
	MeshYaw = FTMeshFacing::FacingYaw(*Skin);

	UWorld* World = GetWorld();
	const int32 Count = static_cast<int32>(Tuning.maxDrawn);
	Pool.Reserve(Count);
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	for (int32 i = 0; i < Count; ++i)
	{
		ASkeletalMeshActor* Actor = World->SpawnActor<ASkeletalMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
		if (!Actor)
		{
			break;
		}
		Actor->SetActorEnableCollision(false);
		Actor->SetActorHiddenInGame(true);
		USkeletalMeshComponent* C = Actor->GetSkeletalMeshComponent();
		C->SetMobility(EComponentMobility::Movable);
		C->SetSkeletalMesh(Skin);
		C->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		C->SetCastShadow(true);
		Pool.Add(Actor);
		TraceQuery.AddIgnoredActor(Actor);
	}
	PoolAgent.Init(0, Pool.Num());
	PoolAnim.Init(0xff, Pool.Num());
	PoolRate.Init(0.f, Pool.Num());
	PoolGround.Init(0.0, Pool.Num());
	PoolGrounded.Init(false, Pool.Num());
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: skin %s, %d actors; clips walk %s, run %s, idle %s, cower %s."), *Skin->GetPathName(), Pool.Num(),
		WalkClip ? *WalkClip->GetName() : TEXT("none"), RunClip ? *RunClip->GetName() : TEXT("walk, faster"),
		IdleClip ? *IdleClip->GetName() : TEXT("reference pose"), CowerClip ? *CowerClip->GetName() : TEXT("idle"));
}

bool UFTCrowdSubsystem::Occluded(void* Context, ftcore::Vec2 From, ftcore::Vec2 To)
{
	UFTCrowdSubsystem* Self = static_cast<UFTCrowdSubsystem*>(Context);
	UWorld* World = Self->GetWorld();
	if (!World || Self->OcclusionTraces >= kOcclusionTracesPerFrame)
	{
		return false;
	}
	++Self->OcclusionTraces;
	const double Z = Self->PlayerZ + Self->EyeZ;
	FHitResult Hit;
	return World->LineTraceSingleByChannel(Hit, FVector(From.x * 100.0, From.y * 100.0, Z), FVector(To.x * 100.0, To.y * 100.0, Z),
		ECC_Visibility, Self->TraceQuery);
}

void UFTCrowdSubsystem::RaiseEvent(ftcore::CrowdEventKind Kind, const FVector& At, float RadiusScale, float IntensityScale)
{
	if (!bRunning)
	{
		return;
	}
	ftcore::CrowdEvent E;
	E.kind = Kind;
	E.at = ToSim(At);
	E.radiusScale = RadiusScale;
	E.intensityScale = IntensityScale;
	Sim.pushEvent(E);
}

void UFTCrowdSubsystem::FeedArena(const ftcore::ArenaEvents& Events)
{
	if (!bRunning)
	{
		return;
	}
	for (const ftcore::ArenaEvent& E : Events)
	{
		const FVector At(E.at.x * 100.0, E.at.y * 100.0, 0.0);
		bool bFightEvent = true;
		switch (E.type)
		{
		case ftcore::ArenaEventType::AttackStarted:
			if (RunTime - LastArenaEvent > kFightQuiet)
			{
				RaiseEvent(ftcore::CrowdEventKind::FightStart, At);
			}
			else
			{
				bFightEvent = false; // the fight is already on: not passed on, not counted
				LastArenaEvent = RunTime;
			}
			break;
		case ftcore::ArenaEventType::Hit:
		case ftcore::ArenaEventType::CounterStagger:
		case ftcore::ArenaEventType::ReadCounter:
			RaiseEvent(ftcore::CrowdEventKind::Strike, At);
			break;
		case ftcore::ArenaEventType::Blocked:
			RaiseEvent(ftcore::CrowdEventKind::Strike, At, 1.f, 0.6f);
			break;
		case ftcore::ArenaEventType::Downed:
			RaiseEvent(ftcore::CrowdEventKind::Death, At);
			break;
		case ftcore::ArenaEventType::Released:
			RaiseEvent(ftcore::CrowdEventKind::MagicBurst, At);
			break;
		default:
			bFightEvent = false;
			break;
		}
		++ArenaSeen;
		if (bFightEvent)
		{
			if (ArenaFed == 0)
			{
				AtArena = Sim.counts();
				float Nearest = 1e9f;
				const ftcore::Vec2 Q = ToSim(At);
				for (uint32 I = 0; I < Sim.capacity(); ++I)
				{
					if (Sim.agent(I).state != ftcore::CrowdState::Free)
					{
						Nearest = FMath::Min(Nearest, FMath::Sqrt(FMath::Square(Sim.agent(I).pos.x - Q.x) + FMath::Square(Sim.agent(I).pos.y - Q.y)));
					}
				}
				UE_LOG(LogForgottenThird, Display, TEXT("Crowd: first arena fight event (type %d) at engine (%.0f, %.0f) cm, t=%.1f s; nearest member %.1f m, %u live."),
					static_cast<int32>(E.type), At.X, At.Y, RunTime, Nearest, Sim.liveCount());
			}
			++ArenaFed;
			LastArenaEvent = RunTime;
		}
	}
}

void UFTCrowdSubsystem::PickTestFight(const FVector& PlayerAt)
{
	if (bTestFightPicked)
	{
		return;
	}
	bTestFightPicked = true;
	// The street node within 40 m of a point 15 m east of the player with the most live members within 12 m (ties: the
	// one nearer that point), so the stills show people close to the fight; else the node nearest the player.
	const ftcore::Vec2 P = ToSim(PlayerAt);
	const ftcore::Vec2 C{P.x + static_cast<float>(kTestFightDistance), P.y};
	uint32 Best = UINT32_MAX;
	int32 BestN = -1;
	float BestD = 0.f;
	for (uint32 n = 0; n < Graph.nodeCount(); ++n)
	{
		const ftcore::Vec2 Q = Graph.node(n);
		const float D = FMath::Sqrt((Q.x - C.x) * (Q.x - C.x) + (Q.y - C.y) * (Q.y - C.y));
		if (D > 40.f)
		{
			continue;
		}
		int32 Near = 0;
		for (uint32 i = 0; i < Sim.capacity(); ++i)
		{
			const ftcore::CrowdAgentView& V = Sim.agent(i);
			Near += V.live && (V.pos.x - Q.x) * (V.pos.x - Q.x) + (V.pos.y - Q.y) * (V.pos.y - Q.y) <= 144.f ? 1 : 0;
		}
		if (Near > BestN || (Near == BestN && D < BestD))
		{
			Best = n;
			BestN = Near;
			BestD = D;
		}
	}
	if (Best == UINT32_MAX)
	{
		Best = Graph.nearest(P, 200.f);
	}
	const ftcore::Vec2 F = Best == UINT32_MAX ? C : Graph.node(Best);
	TestFightPoint = FVector(F.x * 100.0, F.y * 100.0, PlayerZ);
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: test fight point engine (%.0f, %.0f) cm, %.1f m from the player, %d live members within 12 m at t=%.1f s."),
		TestFightPoint.X, TestFightPoint.Y, FVector::Dist2D(TestFightPoint, PlayerAt) / 100.0, FMath::Max(BestN, 0), RunTime);
}

void UFTCrowdSubsystem::ScriptFight(const FVector& PlayerAt)
{
	if (TestFightAt < 0.0 || RunTime < TestFightAt)
	{
		return;
	}
	if (!bTestFightStarted)
	{
		PickTestFight(PlayerAt);
		TestFightPoint.Z = PlayerZ;
		AtTestFight = Sim.counts();
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: test fight starts at engine (%.0f, %.0f) cm, %.1f m from the player, at t=%.1f s; %u live, %u walking."),
			TestFightPoint.X, TestFightPoint.Y, FVector::Dist2D(TestFightPoint, PlayerAt) / 100.0, RunTime,
			Sim.liveCount(), Sim.countIn(ftcore::CrowdState::Walk));
		// The first second: everyone within the reaction radius now, checked one second later.
		FirstSecond.begin(Sim, ToSim(TestFightPoint), Tuning.reactRadius);
		TestFightStartedAt = RunTime;
		RaiseEvent(ftcore::CrowdEventKind::FightStart, TestFightPoint);
		bTestFightStarted = true;
		TestStrikes = 0;
	}
	const double Into = RunTime - TestFightAt;
	while (TestStrikes * kTestStrikeEvery <= Into && TestStrikes * kTestStrikeEvery <= kTestFightLength)
	{
		RaiseEvent(ftcore::CrowdEventKind::Strike, TestFightPoint);
		++TestStrikes;
	}
	if (!bTestDeathRaised && Into >= kTestFightLength)
	{
		RaiseEvent(ftcore::CrowdEventKind::Death, TestFightPoint);
		bTestDeathRaised = true;
		LogCounts(TEXT("test fight over"));
	}
}

void UFTCrowdSubsystem::Shoot(APlayerController* PC, const FVector& PlayerAt)
{
	if (!PC || ShotIndex >= Shots.Num() || TestFightAt < 0.0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (ShotCams.Num() == 0)
	{
		// Aimed at the test fight's point, picked just before the calm still so the members near it are the ones there
		// at the fight's start; placed before the fight so every still matches.
		if (RunTime < TestFightAt - 2.5)
		{
			return;
		}
		PickTestFight(PlayerAt);
		// The street under the fight point (it may sit well above or below the player's).
		double FightZ = PlayerZ;
		{
			const FVector P(TestFightPoint.X, TestFightPoint.Y, PlayerZ);
			FHitResult H;
			if (World->LineTraceSingleByChannel(H, P + FVector(0.0, 0.0, 5000.0), P - FVector(0.0, 0.0, 5000.0), ECC_Visibility, TraceQuery))
			{
				FightZ = H.ImpactPoint.Z;
			}
		}
		const FVector Fight(TestFightPoint.X, TestFightPoint.Y, FightZ);
		TestFightPoint.Z = FightZ;
		const FVector Aim = Fight + FVector(0.0, 0.0, 100.0);
		// A red disc on the street at the fight point, so a reader of the stills knows where it is.
		if (UStaticMesh* Disc = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")))
		{
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (AStaticMeshActor* Mark = World->SpawnActor<AStaticMeshActor>(Fight + FVector(0.0, 0.0, 2.0), FRotator::ZeroRotator, Spawn))
			{
				Mark->SetActorEnableCollision(false);
				UStaticMeshComponent* M = Mark->GetStaticMeshComponent();
				M->SetMobility(EComponentMobility::Movable);
				M->SetStaticMesh(Disc);
				M->SetWorldScale3D(FVector(1.2, 1.2, 0.02));
				if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
				{
					UMaterialInstanceDynamic* Red = UMaterialInstanceDynamic::Create(Base, Mark);
					Red->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.8f, 0.02f, 0.02f));
					M->SetMaterial(0, Red);
				}
				TraceQuery.AddIgnoredActor(Mark);
			}
		}
		const double TanX = FMath::Tan(FMath::DegreesToRadians(35.0)), TanY = TanX * 9.0 / 16.0;
		auto InFrame = [&](const FVector& Eye, const FVector& To) {
			const FVector L = (Aim - Eye).Rotation().Quaternion().UnrotateVector(To - Eye);
			return L.X > 0.0 && FMath::Abs(L.Y) < L.X * TanX * 0.95 && FMath::Abs(L.Z) < L.X * TanY * 0.95;
		};
		// Candidate eyes round the fight point: every 15 degrees, 12-15 m back, 6 m up, the fight point in plain sight.
		// Score: drawn members within 20 m of it in frame and in sight.
		struct FEye
		{
			FVector At;
			double Az = 0.0;
			int32 Seen = -1;
		};
		TArray<FEye> Cands;
		for (int32 Az = 0; Az < 360; Az += 15)
		{
			for (const double Back : kCamBack)
			{
				const double R = FMath::DegreesToRadians(static_cast<double>(Az));
				FEye E;
				E.At = Fight + FVector(FMath::Cos(R) * Back, FMath::Sin(R) * Back, kCamUp);
				E.Az = Az;
				TArray<FOverlapResult> In;
				if (World->OverlapMultiByObjectType(In, E.At, FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic),
						FCollisionShape::MakeSphere(30.f), TraceQuery) ||
					!Clear(E.At, Aim) || !Clear(E.At, Fight + FVector(0.0, 0.0, 20.0)))
				{
					continue;
				}
				E.Seen = 0;
				for (int32 k = 0; k < Pool.Num(); ++k)
				{
					if (PoolAgent[k] == 0 || !PoolGrounded[k])
					{
						continue;
					}
					const FVector Chest = Pool[k]->GetActorLocation() + FVector(0.0, 0.0, 120.0);
					E.Seen += FVector::Dist2D(Chest, Fight) <= 2000.0 && InFrame(E.At, Chest) && Clear(E.At, Chest);
				}
				Cands.Add(E);
				break; // the nearest clear distance for this azimuth
			}
		}
		int32 A = INDEX_NONE, B = INDEX_NONE;
		for (int32 c = 0; c < Cands.Num(); ++c)
		{
			if (A == INDEX_NONE || Cands[c].Seen > Cands[A].Seen)
			{
				A = c;
			}
		}
		for (int32 c = 0; A != INDEX_NONE && c < Cands.Num(); ++c)
		{
			const double Apart = FMath::Abs(FMath::FindDeltaAngleDegrees(Cands[c].Az, Cands[A].Az));
			if (Apart >= 90.0 && (B == INDEX_NONE || Cands[c].Seen > Cands[B].Seen))
			{
				B = c;
			}
		}
		if (A == INDEX_NONE || B == INDEX_NONE)
		{
			bCamsOk = false;
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: cameras: FAILED, %d clear eyes 12-15 m back and 6 m up round the fight point (two 90 degrees apart needed); fallback views."), Cands.Num());
		}
		const FVector Fallback[] = {Fight + FVector(-1350.0, 0.0, kCamUp), Fight + FVector(0.0, 1350.0, kCamUp)};
		const FVector Eyes[] = {A != INDEX_NONE ? Cands[A].At : Fallback[0], B != INDEX_NONE ? Cands[B].At : Fallback[1]};
		for (int32 c = 0; c < 2; ++c)
		{
			const int32 Pick = c == 0 ? A : B;
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: cam%c at %s, %.1f m from the fight point, azimuth %.0f: %d drawn members within 20 m in frame and in sight."),
				c == 0 ? TEXT('A') : TEXT('B'), *Eyes[c].ToString(), FVector::Dist(Eyes[c], Aim) / 100.0, Pick != INDEX_NONE ? Cands[Pick].Az : -1.0,
				Pick != INDEX_NONE ? Cands[Pick].Seen : -1);
		}
		for (const FVector& From : Eyes)
		{
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ACameraActor* Cam = World->SpawnActor<ACameraActor>(From, (Aim - From).Rotation(), Spawn);
			if (!Cam)
			{
				Shots.Reset();
				return;
			}
			Cam->GetCameraComponent()->SetFieldOfView(70.f);
			Cam->GetCameraComponent()->SetConstraintAspectRatio(false);
			ShotCams.Add(Cam);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: shots: cameras at %s and %s, aimed at %s (1 m over the fight point); %d stills to %s."), *Eyes[0].ToString(),
			*Eyes[1].ToString(), *Aim.ToString(), Shots.Num(), *ShotDir);
	}
	if (AHUD* Hud = PC->GetHUD())
	{
		Hud->bShowHUD = false;
	}
	const FCrowdShot& S = Shots[ShotIndex];
	if (ShotTicks < 0)
	{
		if (RunTime < TestFightAt + S.At - 0.1)
		{
			return;
		}
		PC->SetViewTarget(ShotCams[S.Cam]);
		ShotTicks = 0;
		return;
	}
	// The view moves first; the still is taken a couple of frames later so it shows the new camera.
	if (PC->GetViewTarget() != ShotCams[S.Cam])
	{
		PC->SetViewTarget(ShotCams[S.Cam]);
	}
	if (++ShotTicks == 2)
	{
		if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
		{
			Viewport->Exec(World, *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *S.Name), *GLog);
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: shot %s at %.2f s from the fight's start."), *S.Name, RunTime - TestFightAt);
		}
		// The framing: the fight point in the centre fifth and in sight, the nearest member in sight tall enough to read.
		if (S.Phase <= 1)
		{
			const ACameraActor* Cam = ShotCams[S.Cam];
			const FVector Eye = Cam->GetActorLocation();
			const FVector L = Cam->GetActorQuat().UnrotateVector(TestFightPoint - Eye);
			const double TanX = FMath::Tan(FMath::DegreesToRadians(35.0)), TanY = TanX * 9.0 / 16.0;
			const double Px = L.X > 0.0 ? 800.0 * (1.0 + L.Y / (L.X * TanX)) : -1.0;
			const double Py = L.X > 0.0 ? 450.0 * (1.0 - L.Z / (L.X * TanY)) : -1.0;
			const bool bCentre = FMath::Abs(Px - 800.0) <= 160.0 && FMath::Abs(Py - 450.0) <= 90.0;
			const bool bSeen = Clear(Eye, TestFightPoint + FVector(0.0, 0.0, 20.0));
			const FViewCount V = CountInView(S.Cam, nullptr);
			const bool bTall = V.NearestPx >= kMinPersonPx;
			bCamsOk = bCamsOk && bCentre && bSeen && bTall;
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %s: fight point at pixel (%.0f, %.0f) of 1600x900 (centre fifth %s), %s; nearest member in sight %.0f px tall (%s, gate %.0f)."),
				*FPaths::GetCleanFilename(S.Name), Px, Py, bCentre ? TEXT("ok") : TEXT("FAILED"), bSeen ? TEXT("in sight") : TEXT("OCCLUDED"), V.NearestPx,
				bTall ? TEXT("ok") : TEXT("FAILED"), kMinPersonPx);
		}
		// Members in view at the calm still and 30 s later: the same ones, gone elsewhere, or gone?
		if (S.Phase == 0)
		{
			const FViewCount V = CountInView(S.Cam, &CalmInView[S.Cam]);
			CalmCount[S.Cam] = V.Calm;
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: in view cam%c calm: %d in sight, %d walking or standing about, least gap %.2f m."), TEXT('A') + S.Cam,
				V.N, V.Calm, V.MinGap >= 1e8f ? -1.f : V.MinGap);
		}
		else if (S.Phase == 3)
		{
			TArray<uint32> Now;
			const FViewCount VC = CountInView(S.Cam, &Now);
			const int32 N = VC.N;
			// Back at +30 s: at least half the calm count walking or standing about, none inside another.
			const bool bBack = CalmCount[S.Cam] > 0 && VC.Calm * 2 >= CalmCount[S.Cam] && VC.MinGap >= kGapPair;
			bRefillOk = bRefillOk && bBack;
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: refill cam%c: %d walking or standing about at +30 s of %d calm, least gap %.2f m: %s."), TEXT('A') + S.Cam,
				VC.Calm, CalmCount[S.Cam], VC.MinGap >= 1e8f ? -1.f : VC.MinGap, bBack ? TEXT("ok") : TEXT("FAILED"));
			int32 Still = 0, Elsewhere = 0, Gone = 0;
			for (const uint32 Id : CalmInView[S.Cam])
			{
				const ftcore::CrowdAgentView& V = Sim.agent(Id & 0xffffu);
				if (!V.live || V.id != Id)
				{
					++Gone;
				}
				else if (Now.Contains(Id))
				{
					++Still;
				}
				else
				{
					++Elsewhere;
				}
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: in view cam%c +30 s: %d (calm %d); of the calm ones %d still in view, %d live elsewhere, %d despawned; %d new in view."),
				TEXT('A') + S.Cam, N, CalmInView[S.Cam].Num(), Still, Elsewhere, Gone, N - Still);
		}
		++ShotIndex;
		ShotTicks = -1;
	}
}

void UFTCrowdSubsystem::LogCounts(const TCHAR* Label) const
{
	const ftcore::CrowdCounts& C = Sim.counts();
	UE_LOG(LogForgottenThird, Display,
		TEXT("Crowd: %s t=%.1f s: live %u (walk %u idle %u notice %u back %u flee %u cower %u recover %u), drawn actors %d; spawned %llu despawned %llu perceived %llu noticed %llu backed %llu fled %llu cowered %llu resumed %llu spread %llu plans %llu fails %llu dropped %llu occlusion %llu."),
		Label, RunTime, Sim.liveCount(), Sim.countIn(ftcore::CrowdState::Walk), Sim.countIn(ftcore::CrowdState::Idle),
		Sim.countIn(ftcore::CrowdState::Notice), Sim.countIn(ftcore::CrowdState::BackAway), Sim.countIn(ftcore::CrowdState::Flee),
		Sim.countIn(ftcore::CrowdState::Cower), Sim.countIn(ftcore::CrowdState::Recover), [this]
		{
			int32 N = 0;
			for (const uint32 A : PoolAgent)
			{
				N += A != 0 ? 1 : 0;
			}
			return N;
		}(),
		C.spawned, C.despawned, C.perceived, C.noticed, C.backedAway, C.fled, C.cowered, C.resumed, C.spread, C.plans, C.planFails,
		C.eventsDropped, C.occlusionQueries);
}

void UFTCrowdSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!bRunning || !World)
	{
		return;
	}
	RunTime += DeltaTime;
	FVector PlayerAt = FVector::ZeroVector, PlayerVel = FVector::ZeroVector;
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (bPlayerMove && PC->GetPawn())
		{
			// Once: onto the ground at -CrowdPlayerAt, before the sim sees the player.
			bPlayerMove = false;
			APawn* Mover = PC->GetPawn();
			const FVector Top(PlayerMoveTo.X, PlayerMoveTo.Y, 100000.0);
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(CrowdPlayerAt), false, Mover);
			const bool bHit = World->LineTraceSingleByChannel(Hit, Top, Top - FVector(0.0, 0.0, 200000.0), ECC_Visibility, Q);
			const double Z = (bHit ? Hit.ImpactPoint.Z : 0.0) + Mover->GetSimpleCollisionHalfHeight() + 5.0;
			Mover->SetActorLocation(FVector(PlayerMoveTo.X, PlayerMoveTo.Y, Z), false, nullptr, ETeleportType::TeleportPhysics);
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: player moved to engine (%.0f, %.0f, %.0f) cm (%s)."), PlayerMoveTo.X, PlayerMoveTo.Y, Z,
				bHit ? TEXT("on the ground") : TEXT("no ground found, FAILED"));
		}
		if (const APawn* Pawn = PC->GetPawn())
		{
			PlayerAt = Pawn->GetActorLocation();
			PlayerVel = Pawn->GetVelocity();
			PlayerZ = PlayerAt.Z - Pawn->GetSimpleCollisionHalfHeight();
		}
		else if (const AActor* View = PC->GetViewTarget())
		{
			PlayerAt = View->GetActorLocation();
			PlayerZ = PlayerAt.Z;
		}
	}
	if (TestFightAt >= 0.0 && RunTime >= TestFightAt - 8.0)
	{
		// The drawn members are the ones round the fight (they are what the stills show), not round the player.
		PickTestFight(PlayerAt);
		Sim.setDrawFocus(true, ToSim(TestFightPoint));
	}
	ScriptFight(PlayerAt);
	if (!bGroundLogged && RunTime >= 1.0)
	{
		bGroundLogged = true;
		LogGroundAt();
	}

	OcclusionTraces = 0;
	Sim.tick(DeltaTime, ToSim(PlayerAt), ToSim(PlayerVel));
	if (QuitAfter > 0.0)
	{
		RunMinPair = FMath::Min(RunMinPair, Sim.minPairDistance(2.f));
		for (uint32 i = 0; i < Sim.capacity(); ++i)
		{
			if (Sim.agent(i).live)
			{
				RunMinWall = FMath::Min(RunMinWall, Sim.wallClearance(i));
			}
		}
	}
	if (bTestFightStarted && !bFirstSecondDone && RunTime >= TestFightStartedAt + 1.0)
	{
		bFirstSecondDone = true;
		const ftcore::CrowdReactionResult R = FirstSecond.evaluate(Sim, kFirstSecondPath, kFirstSecondRadial);
		bFirstSecondOk = R.ok();
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: first second (%.2f s, radius %.0f m): inside %u, away %u, cornered %u, turned %u, toward %u, worst path %.2f m, worst gain %.2f m: %s."),
			RunTime - TestFightStartedAt, Tuning.reactRadius, R.inside, R.away, R.cornered, R.turned, R.toward, R.worstPath, R.worstGain, bFirstSecondOk ? TEXT("ok") : TEXT("FAILED"));
	}
	Draw();
	Shoot(World->GetFirstPlayerController(), PlayerAt);

	if (RunTime >= NextLog)
	{
		LogCounts(TEXT("counts"));
		NextLog += 10.0;
	}
	if (QuitAfter > 0.0 && RunTime >= QuitAfter)
	{
		LogCounts(TEXT("final"));
		const ftcore::CrowdCounts& C = Sim.counts();
		// Shared checks: nobody flip-flops between running and stopping, and the street is calm again at the end.
		const uint32 Live = Sim.liveCount();
		const uint32 Upset = Live - Sim.countIn(ftcore::CrowdState::Walk) - Sim.countIn(ftcore::CrowdState::Idle);
		const bool bCalm = Upset * 20 <= Live;
		const bool bSteady = C.maxFleeEntries <= kMaxFleeEntries && C.maxPlans <= kMaxPlansOne;
		const bool bFull = Live + Live / 10 >= Tuning.targetAgents;
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: %u live of %u target (%s); %u still upset (%s); %llu fleers, most flee entries by one %u, most plans by one %u (%s)."),
			Live, Tuning.targetAgents, bFull ? TEXT("full") : TEXT("SHORT"), Upset, bCalm ? TEXT("calm") : TEXT("NOT CALM"), C.fleers,
			C.maxFleeEntries, C.maxPlans, bSteady ? TEXT("steady") : TEXT("FLIP-FLOP"));
		const bool bUnstuck = C.stuck == 0;
		const bool bOutside = FootFrames == 0;
		const bool bClearOfMass = MinMassClear >= kGapWall * 100.0;
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: members stuck (no progress for 2 s walking or backing off, 1 s running) %llu (%s); drawn members inside a massing box or on a roof %llu member-frames, %d members (%s); least distance from a drawn member to a massing box %.2f m (%s, gate %.1f m; %d boxes)."),
			C.stuck, bUnstuck ? TEXT("ok") : TEXT("FAILED"), FootFrames, FootMembers, bOutside ? TEXT("ok") : TEXT("FAILED"),
			MinMassClear >= 1e8 ? -1.0 : MinMassClear / 100.0, bClearOfMass ? TEXT("ok") : TEXT("FAILED"), kGapWall, MassBoxes.Num());
		const bool bPairs = RunMinPair >= kGapPair;
		const bool bWalls = RunMinWall >= kGapWall - 0.01f;
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: least distance between two members %.2f m (%s, gate %.1f m), least wall clearance %.2f m (%s, gate %.1f m), every frame, every state."),
			RunMinPair, bPairs ? TEXT("ok") : TEXT("FAILED"), kGapPair, RunMinWall, bWalls ? TEXT("ok") : TEXT("FAILED"), kGapWall);
		const bool bStills = Shots.Num() == 0 || ShotIndex >= Shots.Num();
		if (Shots.Num() > 0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: stills %d of %d (%s); cameras on the fight %s; the crowd back in view at +30 s %s."), ShotIndex, Shots.Num(),
				bStills ? TEXT("ok") : TEXT("FAILED"), bCamsOk ? TEXT("ok") : TEXT("FAILED"), bRefillOk ? TEXT("ok") : TEXT("FAILED"));
		}
		bool bOk = bCalm && bSteady && bFull && bUnstuck && bOutside && bClearOfMass && bPairs && bWalls && bStills && bCamsOk && bRefillOk;
		const bool bStimulus = bTestFightStarted || ArenaFed > 0;
		if (!bStimulus)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: no stimulus this run (no -CrowdTestFight, no arena fight event): FAILED."));
			bOk = false;
		}
		if (bTestFightStarted)
		{
			const uint64 Perceived = C.perceived - AtTestFight.perceived;
			const uint64 Fled = C.fled - AtTestFight.fled;
			const uint64 Resumed = C.resumed - AtTestFight.resumed;
			const uint64 Spread = C.spread - AtTestFight.spread;
			const bool bReacted = Perceived > 0 && Fled > 0 && Resumed > 0 && Spread > 0;
			bOk = bOk && bReacted && bFirstSecondDone && bFirstSecondOk;
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: since the test fight: perceived %llu, fled %llu, cowered %llu, resumed %llu, spread %llu: %s."),
				Perceived, Fled, C.cowered - AtTestFight.cowered, Resumed, Spread,
				bReacted ? TEXT("ok") : TEXT("FAILED (nobody perceived, fled, resumed or spread panic)"));
		}
		if (ArenaSeen > 0)
		{
			const uint64 Perceived = C.perceived - AtArena.perceived;
			const bool bReacted = ArenaFed > 0 && Perceived > 0 && C.resumed > AtArena.resumed;
			bOk = bOk && bReacted;
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: from the arena: %d events, %d fed as fight events; since the first: perceived %llu, noticed %llu, backed %llu, fled %llu, cowered %llu, resumed %llu: %s."),
				ArenaSeen, ArenaFed, Perceived, C.noticed - AtArena.noticed, C.backedAway - AtArena.backedAway, C.fled - AtArena.fled,
				C.cowered - AtArena.cowered, C.resumed - AtArena.resumed, bReacted ? TEXT("ok") : TEXT("FAILED (no fight event fed, or nobody perceived one or resumed)"));
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: verdict %s."), bOk ? TEXT("ok") : TEXT("FAILED"));
		bRunning = false;
		FTRequestExit(bOk ? 0 : 1, TEXT("Crowd"));
	}
}

void UFTCrowdSubsystem::PlayFor(int32 Slot, ftcore::CrowdAnim Anim, float Speed)
{
	USkeletalMeshComponent* C = Pool[Slot]->GetSkeletalMeshComponent();
	UAnimSequence* Clip = nullptr;
	float Rate = 1.f;
	switch (Anim)
	{
	case ftcore::CrowdAnim::Walk:
		Clip = WalkClip;
		Rate = Speed / kWalkClipSpeed;
		break;
	case ftcore::CrowdAnim::Run:
		Clip = RunClip ? RunClip.Get() : WalkClip.Get();
		Rate = RunClip ? Speed / kRunClipSpeed : FMath::Min(Speed / kWalkClipSpeed, kRunOnWalkMax);
		break;
	case ftcore::CrowdAnim::BackStep:
		// The walk played backwards until there is a back-step clip.
		Clip = WalkClip;
		Rate = -FMath::Max(Speed, 0.3f) / kWalkClipSpeed;
		break;
	case ftcore::CrowdAnim::Cower:
		Clip = CowerClip ? CowerClip.Get() : IdleClip.Get();
		break;
	case ftcore::CrowdAnim::Idle:
	case ftcore::CrowdAnim::Look:
	default:
		Clip = IdleClip;
		break;
	}
	Rate = FMath::Clamp(Rate, -3.f, 3.f);
	const uint8 AnimByte = static_cast<uint8>(Anim);
	if (PoolAnim[Slot] != AnimByte)
	{
		PoolAnim[Slot] = AnimByte;
		PoolRate[Slot] = Rate;
		if (Clip)
		{
			C->PlayAnimation(Clip, true);
			C->SetPlayRate(Rate);
		}
		else
		{
			C->Stop();
		}
	}
	else if (Clip && FMath::Abs(PoolRate[Slot] - Rate) > 0.05f)
	{
		PoolRate[Slot] = Rate;
		C->SetPlayRate(Rate);
	}
}

void UFTCrowdSubsystem::Draw()
{
	if (Pool.Num() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	const uint32 Cap = Sim.capacity();
	// Free the actors whose agent is no longer drawn (or was replaced in its slot).
	for (int32 k = 0; k < Pool.Num(); ++k)
	{
		const uint32 Id = PoolAgent[k];
		if (Id == 0)
		{
			continue;
		}
		const uint32 Slot = (Id - 1) & 0xffffu;
		const ftcore::CrowdAgentView& V = Sim.agent(Slot);
		if (!V.live || !V.drawn || V.id + 1 != Id)
		{
			PoolAgent[k] = 0;
			SlotActor[static_cast<int32>(Slot)] = INDEX_NONE;
			Pool[k]->SetActorHiddenInGame(true);
			Pool[k]->GetSkeletalMeshComponent()->Stop();
			PoolAnim[k] = 0xff;
		}
	}
	// Give each newly drawn agent a free actor.
	int32 FreeCursor = 0;
	for (uint32 i = 0; i < Cap; ++i)
	{
		const ftcore::CrowdAgentView& V = Sim.agent(i);
		if (!V.live || !V.drawn || SlotActor[static_cast<int32>(i)] != INDEX_NONE)
		{
			continue;
		}
		while (FreeCursor < Pool.Num() && PoolAgent[FreeCursor] != 0)
		{
			++FreeCursor;
		}
		if (FreeCursor >= Pool.Num())
		{
			break;
		}
		PoolAgent[FreeCursor] = V.id + 1;
		SlotActor[static_cast<int32>(i)] = FreeCursor;
		PoolGrounded[FreeCursor] = false;
		PoolGround[FreeCursor] = PlayerZ;
		// Grounded now, shown only once it is (never a frame at the player's height, inside a hill or in mid-air).
		const FVector At(V.pos.x * 100.0, V.pos.y * 100.0, PlayerZ);
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, At + FVector(0.0, 0.0, kGroundFirstSpan), At - FVector(0.0, 0.0, kGroundFirstSpan), ECC_Visibility, TraceQuery))
		{
			PoolGround[FreeCursor] = Hit.ImpactPoint.Z;
			PoolGrounded[FreeCursor] = true;
		}
		Pool[FreeCursor]->SetActorHiddenInGame(!PoolGrounded[FreeCursor]);
	}
	// Ground: a few lines a frame, round robin; an actor that has never found the ground looks over a long span first.
	for (int32 n = 0; n < kGroundTracesPerFrame && n < Pool.Num(); ++n)
	{
		const int32 k = GroundCursor;
		GroundCursor = (GroundCursor + 1) % Pool.Num();
		if (PoolAgent[k] == 0)
		{
			continue;
		}
		const ftcore::CrowdAgentView& V = Sim.agent((PoolAgent[k] - 1) & 0xffffu);
		const double Up = PoolGrounded[k] ? kGroundUp : kGroundFirstSpan;
		const double Down = PoolGrounded[k] ? kGroundDown : kGroundFirstSpan;
		const FVector At(V.pos.x * 100.0, V.pos.y * 100.0, PoolGround[k]);
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, At + FVector(0.0, 0.0, Up), At - FVector(0.0, 0.0, Down), ECC_Visibility, TraceQuery))
		{
			PoolGround[k] = Hit.ImpactPoint.Z;
			if (!PoolGrounded[k])
			{
				PoolGrounded[k] = true;
				Pool[k]->SetActorHiddenInGame(false);
			}
		}
	}
	const float Alpha = FMath::Clamp(Sim.stepAlpha(), 0.f, 1.f);
	for (int32 k = 0; k < Pool.Num(); ++k)
	{
		if (PoolAgent[k] == 0 || !PoolGrounded[k])
		{
			continue;
		}
		const ftcore::CrowdAgentView& V = Sim.agent((PoolAgent[k] - 1) & 0xffffu);
		const double X = FMath::Lerp(V.prevPos.x, V.pos.x, Alpha) * 100.0;
		const double Y = FMath::Lerp(V.prevPos.y, V.pos.y, Alpha) * 100.0;
		Pool[k]->SetActorLocationAndRotation(FVector(X, Y, PoolGround[k]), FRotator(0.0, V.yaw + MeshYaw, 0.0));
		PlayFor(k, V.anim, V.speed);
		if (QuitAfter > 0.0)
		{
			CheckFootprint(k, FVector(X, Y, PoolGround[k]));
		}
	}
}

void UFTCrowdSubsystem::CheckFootprint(int32 Actor, const FVector& Feet)
{
	// Inside a building's massing box (its bounds, not its collision), or stood on a roof: more ground under where it
	// stands, over half a metre down. Also the least distance to a box, round the feet's height.
	UWorld* World = GetWorld();
	FHitResult Below;
	const bool bRoof = PoolGrounded[Actor] && World->LineTraceSingleByChannel(Below, Feet - FVector(0.0, 0.0, 50.0),
		Feet - FVector(0.0, 0.0, 5000.0), ECC_Visibility, TraceQuery);
	int32 InBox = INDEX_NONE;
	const FIntPoint Cell(FMath::FloorToInt32(Feet.X / kMassCell), FMath::FloorToInt32(Feet.Y / kMassCell));
	if (const TArray<int32>* Near = MassGrid.Find(Cell))
	{
		for (const int32 b : *Near)
		{
			const FMassBox& M = MassBoxes[b];
			if (Feet.Z <= M.ZMin - 50.0 || Feet.Z >= M.ZMax - 30.0)
			{
				continue;
			}
			const FVector2D D(Feet.X - M.C.X, Feet.Y - M.C.Y);
			const double Lx = FMath::Abs(D.X * M.Ax.X + D.Y * M.Ax.Y) - M.Half.X;
			const double Ly = FMath::Abs(-D.X * M.Ax.Y + D.Y * M.Ax.X) - M.Half.Y;
			const double Out = Lx > 0.0 || Ly > 0.0 ? FMath::Sqrt(FMath::Square(FMath::Max(Lx, 0.0)) + FMath::Square(FMath::Max(Ly, 0.0))) : FMath::Max(Lx, Ly);
			MinMassClear = FMath::Min(MinMassClear, Out);
			if (Out < 0.0 && InBox == INDEX_NONE)
			{
				InBox = b;
			}
		}
	}
	if (InBox == INDEX_NONE && !bRoof)
	{
		return;
	}
	++FootFrames;
	const int32 Slot = static_cast<int32>((PoolAgent[Actor] - 1) & 0xffffu);
	if (!FootSeen[Slot])
	{
		FootSeen[Slot] = true;
		++FootMembers;
	}
	if (FootLogged < 8)
	{
		++FootLogged;
		const ftcore::CrowdAgentView& V = Sim.agent(static_cast<uint32>(Slot));
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: member %d (%s) %s at engine (%.0f, %.0f, %.0f) cm, t=%.1f s%s."), Slot, *FString(static_cast<int32>(ftcore::crowdStateName(V.state).size()), ftcore::crowdStateName(V.state).data()),
			InBox != INDEX_NONE ? TEXT("inside a massing box") : TEXT("on a roof"), Feet.X, Feet.Y, Feet.Z, RunTime,
			InBox != INDEX_NONE ? *FString::Printf(TEXT(", box centre (%.0f, %.0f) half (%.0f, %.0f)"), MassBoxes[InBox].C.X, MassBoxes[InBox].C.Y,
				MassBoxes[InBox].Half.X, MassBoxes[InBox].Half.Y) : TEXT(""));
	}
}

void UFTCrowdSubsystem::BuildMassing()
{
	// The buildings' massing boxes: every static mesh (and instance) tall and wide enough to be one, by its bounds.
	UWorld* World = GetWorld();
	auto Add = [this](const FBox& Local, const FTransform& X)
	{
		const FVector Scale = X.GetScale3D().GetAbs();
		const FVector Half = Local.GetExtent() * Scale;
		if (Half.Z * 2.0 < kMassMinHeight || FMath::Min(Half.X, Half.Y) * 2.0 < kMassMinSide || FMath::Max(Half.X, Half.Y) * 2.0 > kMassMaxSide)
		{
			return;
		}
		const FVector C = X.TransformPosition(Local.GetCenter());
		FVector2D Ax(X.GetUnitAxis(EAxis::X).X, X.GetUnitAxis(EAxis::X).Y);
		if (!Ax.Normalize())
		{
			Ax = FVector2D(1.0, 0.0);
		}
		FMassBox M;
		M.C = FVector2D(C.X, C.Y);
		M.Ax = Ax;
		M.Half = FVector2D(Half.X, Half.Y);
		M.ZMin = C.Z - Half.Z;
		M.ZMax = C.Z + Half.Z;
		const int32 b = MassBoxes.Add(M);
		const double R = FMath::Abs(Ax.X) * Half.X + FMath::Abs(Ax.Y) * Half.Y + 100.0;
		const double Ry = FMath::Abs(Ax.Y) * Half.X + FMath::Abs(Ax.X) * Half.Y + 100.0;
		for (int32 cx = FMath::FloorToInt32((C.X - R) / kMassCell); cx <= FMath::FloorToInt32((C.X + R) / kMassCell); ++cx)
		{
			for (int32 cy = FMath::FloorToInt32((C.Y - Ry) / kMassCell); cy <= FMath::FloorToInt32((C.Y + Ry) / kMassCell); ++cy)
			{
				MassGrid.FindOrAdd(FIntPoint(cx, cy)).Add(b);
			}
		}
	};
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!Actor || Actor->GetClass()->GetName().Contains(TEXT("Landscape")) || Pool.Contains(Cast<ASkeletalMeshActor>(Actor)))
		{
			continue;
		}
		TInlineComponentArray<UStaticMeshComponent*> Comps(Actor);
		for (const UStaticMeshComponent* C : Comps)
		{
			const UStaticMesh* Mesh = C ? C->GetStaticMesh() : nullptr;
			if (!Mesh || C->GetClass()->GetName().Contains(TEXT("Foliage")) || !C->IsCollisionEnabled())
			{
				continue;
			}
			const FBox Local = Mesh->GetBoundingBox();
			if (const UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C))
			{
				for (int32 n = 0; n < I->GetInstanceCount(); ++n)
				{
					FTransform X;
					if (I->GetInstanceTransform(n, X, true))
					{
						Add(Local, X);
					}
				}
			}
			else
			{
				Add(Local, C->GetComponentTransform());
			}
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %d massing boxes for the footprint check (at least %.1f m tall and %.1f m wide), %d grid cells."), MassBoxes.Num(),
		kMassMinHeight / 100.0, kMassMinSide / 100.0, MassGrid.Num());
}

void UFTCrowdSubsystem::LogGroundAt()
{
	// Every surface under one point, top down: the terrain against the street there. Members stand on the first one.
	double X = kGroundAtX, Y = kGroundAtY;
	FString At;
	if (FParse::Value(FCommandLine::Get(), TEXT("CrowdGroundAt="), At))
	{
		FString L, R;
		if (At.Split(TEXT(","), &L, &R))
		{
			X = FCString::Atod(*L);
			Y = FCString::Atod(*R);
		}
	}
	UWorld* World = GetWorld();
	FCollisionQueryParams Q(TraceQuery);
	FVector From(X, Y, PlayerZ + 20000.0);
	const FVector To(X, Y, PlayerZ - 20000.0);
	double Terrain = -1e9, Street = -1e9, First = -1e9;
	for (int32 n = 0; n < 8; ++n)
	{
		FHitResult H;
		if (!World->LineTraceSingleByChannel(H, From, To, ECC_Visibility, Q))
		{
			break;
		}
		const AActor* By = H.GetActor();
		const bool bLand = By && By->GetClass()->GetName().Contains(TEXT("Landscape"));
		if (n == 0)
		{
			First = H.ImpactPoint.Z;
		}
		if (bLand && Terrain < -1e8)
		{
			Terrain = H.ImpactPoint.Z;
		}
		else if (!bLand && Street < -1e8)
		{
			Street = H.ImpactPoint.Z;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: ground at (%.0f, %.0f) cm: hit %d at z %.1f cm, %s (%s)."), X, Y, n, H.ImpactPoint.Z,
			By ? *By->GetName() : TEXT("?"), By ? *By->GetClass()->GetName() : TEXT("?"));
		if (By)
		{
			Q.AddIgnoredActor(By);
		}
		From = H.ImpactPoint + FVector(0.0, 0.0, 1.0);
	}
	const bool bBoth = Terrain > -1e8 && Street > -1e8;
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: ground at (%.0f, %.0f) cm: terrain z %.1f, street z %.1f, terrain over street %.1f cm (%s); members stand on the first surface, z %.1f."),
		X, Y, Terrain, Street, bBoth ? Terrain - Street : 0.0, !bBoth ? TEXT("one surface only") : Terrain - Street > 10.0 ? TEXT("OVER 10 cm: world-build to fix the ground") : TEXT("ok"), First);
}

bool UFTCrowdSubsystem::Clear(const FVector& Eye, const FVector& To) const
{
	// Nothing static between the eye and the point (a hit within half a metre of it is the thing itself).
	FHitResult H;
	return !GetWorld()->LineTraceSingleByChannel(H, Eye, To, ECC_Visibility, TraceQuery) || FVector::Dist(H.ImpactPoint, To) < 50.0;
}

UFTCrowdSubsystem::FViewCount UFTCrowdSubsystem::CountInView(int32 Cam, TArray<uint32>* Ids) const
{
	// Drawn members on the ground whose chest is inside the camera's frustum (70 degrees across, 16:9) and in sight.
	const ACameraActor* C = ShotCams[Cam];
	const FVector Eye = C->GetActorLocation();
	const FQuat Q = C->GetActorQuat();
	const double TanX = FMath::Tan(FMath::DegreesToRadians(35.0)), TanY = TanX * 9.0 / 16.0;
	const double Focal = 800.0 / TanX;
	FViewCount R;
	TArray<FVector2D, TInlineAllocator<64>> At;
	for (int32 k = 0; k < Pool.Num(); ++k)
	{
		if (PoolAgent[k] == 0 || !PoolGrounded[k])
		{
			continue;
		}
		const ftcore::CrowdAgentView& V = Sim.agent((PoolAgent[k] - 1) & 0xffffu);
		const FVector Feet = Pool[k]->GetActorLocation();
		const FVector Chest = Feet + FVector(0.0, 0.0, 120.0);
		const FVector L = Q.UnrotateVector(Chest - Eye);
		if (L.X <= 0.0 || FMath::Abs(L.Y) > L.X * TanX || FMath::Abs(L.Z) > L.X * TanY || !Clear(Eye, Chest))
		{
			continue;
		}
		++R.N;
		R.Calm += V.state == ftcore::CrowdState::Walk || V.state == ftcore::CrowdState::Idle ? 1 : 0;
		R.NearestPx = FMath::Max(R.NearestPx, kPersonHeight * Focal / L.X);
		for (const FVector2D& O : At)
		{
			R.MinGap = FMath::Min(R.MinGap, static_cast<float>(FVector2D::Distance(O, FVector2D(Feet.X, Feet.Y)) / 100.0));
		}
		At.Add(FVector2D(Feet.X, Feet.Y));
		if (Ids)
		{
			Ids->Add(V.id);
		}
	}
	return R;
}

TStatId UFTCrowdSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTCrowdSubsystem, STATGROUP_Tickables);
}
