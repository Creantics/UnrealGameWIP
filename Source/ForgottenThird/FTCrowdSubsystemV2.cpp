#include "FTCrowdSubsystemV2.h"

#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "FTPartyFollower.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/HUD.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "WorldCollision.h"
#include "FTMeshFacing.h"
#include "ftcore/Interior.h"
#include "ForgottenThird.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include <string>
#include <vector>

namespace CrowdV2Local
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
// The first ground's headroom (Core's crowdPickGround): a body's height clear over a top, its line up starting this far
// above it (cm). A grounded member whose next ground is more than a step up looks again as at first (a slab over it).
constexpr double kGroundBody = 180.0;
constexpr double kGroundClearStart = 5.0;
constexpr double kGroundStep = 60.0;
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
// S-271 gates: two members at least this far apart and this far inside the frontage (m), every frame. The spec: "nobody
// nearer the frontage than wallInset" (0.8 m), so the wall gate passes at 0.8 m or more, unrounded, with no tolerance,
// in both its checks (the least wall clearance and the least distance to a massing box), compared in double metres.
constexpr float kGapPair = 0.6f;
constexpr double kGapWall = 0.8;
// The stills' cameras: this far back from the fight point and this high (cm); a member is this tall (cm) and the nearest
// in sight must stand at least this many px high in a 1600x900 still.
constexpr double kCamBack[] = {1350.0, 1200.0, 1500.0};
constexpr double kCamUp = 600.0;
constexpr double kPersonHeight = 175.0;
constexpr double kMinPersonPx = 60.0;
// S-332: camC, orthographic straight down: this high over the fight point, this wide across the 1600 px (cm).
constexpr double kTopUp = 15000.0;
constexpr double kTopWidth = 5000.0;
// S-332: nobody pops in or out within this far of a camera in its view (m); the stopped-knot gate (members within 3 m).
constexpr float kViewRange = 40.f;
constexpr uint32 kGroupGate = 3;
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
} // namespace CrowdV2Local

UFTCrowdSubsystemV2* UFTCrowdSubsystemV2::Get(const UWorld* World)
{
	return World ? World->GetSubsystem<UFTCrowdSubsystemV2>() : nullptr;
}

bool UFTCrowdSubsystemV2::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	FString Streets;
	// The rework runs only with -CrowdV2; without it UFTCrowdSubsystem runs the crowd as before.
	return World && World->WorldType == EWorldType::Game && FParse::Param(FCommandLine::Get(), TEXT("CrowdV2")) &&
		FParse::Value(FCommandLine::Get(), TEXT("CrowdStreets="), Streets) && !Streets.IsEmpty() && Super::ShouldCreateSubsystem(Outer);
}

bool UFTCrowdSubsystemV2::Fail(const TCHAR* Why, const FString& What)
{
	UE_LOG(LogForgottenThird, Error, TEXT("Crowd: %s %s"), Why, *What);
	FTRequestExit(1, TEXT("Crowd"));
	bRunning = false;
	return false;
}

void UFTCrowdSubsystemV2::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	const TCHAR* Cmd = FCommandLine::Get();
	FString StreetsPath, TuningPath;
	FParse::Value(Cmd, TEXT("CrowdStreets="), StreetsPath);
	if (!FParse::Value(Cmd, TEXT("CrowdTuning="), TuningPath))
	{
		TuningPath = FPaths::ProjectDir() / TEXT("Data/Crowd/crowd_v2.json");
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
	if (!ftcore::crowdv2::importCrowdTuning(TCHAR_TO_UTF8(*Text), Tuning, &Error))
	{
		Fail(TEXT("bad tuning:"), FString(UTF8_TO_TCHAR(Error.c_str())));
		return;
	}
	if (Side <= 0.0 || !FFileHelper::LoadFileToString(Text, *StreetsPath))
	{
		Fail(TEXT("couldn't read the streets (or -CrowdSide isn't above 0):"), StreetsPath);
		return;
	}
	std::vector<ftcore::crowdv2::CrowdStreet> Streets;
	const bool bStreetsRead = ftcore::crowdv2::importCrowdStreets(TCHAR_TO_UTF8(*Text), Side, Streets, &Error);
	// -Interiors=hero (the map built with the hero corner's rooms): the hero lane is a street and each room a walk-in door,
	// members going in through the doorway to a spot in the room, in sight, and back out (ftcore::heroCornerCrowd, the
	// default hero spec, in the -CrowdSide frame).
	std::vector<ftcore::crowdv2::CrowdDoor> HeroDoors;
	FString Interiors;
	if (bStreetsRead && FParse::Value(Cmd, TEXT("Interiors="), Interiors) && Interiors == TEXT("hero"))
	{
		const int HeroDoorCount = ftcore::heroCornerCrowd(ftcore::HeroCornerSpec{}, Side, Streets, HeroDoors);
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: -Interiors=hero: the hero lane added as a street, %d walk-in room doors."), HeroDoorCount);
	}
	if (!bStreetsRead || !Graph.build(Streets, Tuning.graph, &Error))
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
		for (ftcore::crowdv2::CrowdStreet& S : Streets)
		{
			for (ftcore::Vec2& Q : S.points)
			{
				Q.x += By.x;
				Q.y += By.y;
			}
		}
		// The hero room doors move with the streets they open onto.
		for (ftcore::crowdv2::CrowdDoor& D : HeroDoors)
		{
			for (ftcore::Vec2* Q : {&D.at, &D.inside, &D.through})
			{
				Q->x += By.x;
				Q->y += By.y;
			}
			for (ftcore::Vec2& Q : D.dwell)
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
	if (!HeroDoors.empty())
	{
		const std::size_t Attached = Sim.setDoors(HeroDoors);
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %d of %d hero room doors on a street."), static_cast<int32>(Attached), static_cast<int32>(HeroDoors.size()));
		for (const ftcore::crowdv2::CrowdDoor& D : HeroDoors)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: room door %s at (%.2f, %.2f) m, through (%.2f, %.2f), %d spots."), UTF8_TO_TCHAR(D.id.c_str()),
				D.at.x, D.at.y, D.through.x, D.through.y, static_cast<int32>(D.dwell.size()));
		}
	}
	Sim.setOcclusion(&UFTCrowdSubsystemV2::Occluded, this);
	TraceQuery = FCollisionQueryParams(SCENE_QUERY_STAT(FTCrowdTrace), false);
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %d streets, %u nodes, %u edges, %u components, %u spawnable; pool %u, drawn %u, target %u."),
		static_cast<int32>(Streets.size()), Graph.nodeCount(), Graph.edgeCount() / 2, Graph.componentCount(), Graph.spawnableCount(),
		Tuning.maxAgents, Tuning.maxDrawn, Tuning.targetAgents);
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: the rework runs (-CrowdV2), tuning %s."), *TuningPath);

	LoadArt();
	SlotActor.Init(INDEX_NONE, static_cast<int32>(Sim.capacity()));
	FootSeen.Init(false, static_cast<int32>(Sim.capacity()));
	FootSeenRender.Init(false, static_cast<int32>(Sim.capacity()));
	FootFramesBy.Init(0, static_cast<int32>(Sim.capacity()));
	FParse::Value(Cmd, TEXT("CrowdFootStill="), FootStillPath);
	FParse::Value(Cmd, TEXT("CrowdRoofStill="), RoofStillPath);
	FString ZoneText;
	if (FParse::Value(Cmd, TEXT("CrowdZoneZ="), ZoneText, false))
	{
		TArray<FString> Parts;
		ZoneText.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 4)
		{
			ZoneZBox = FBox2D(FVector2D(FMath::Min(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[2])), FMath::Min(FCString::Atod(*Parts[1]), FCString::Atod(*Parts[3]))),
				FVector2D(FMath::Max(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[2])), FMath::Max(FCString::Atod(*Parts[1]), FCString::Atod(*Parts[3]))));
			ZoneSeen.Init(false, static_cast<int32>(Sim.capacity()));
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Crowd: -CrowdZoneZ=%s isn't x0,y0,x1,y1 (engine cm); no zone log."), *ZoneText);
		}
	}

	double TestAt = 20.0;
	if (FParse::Param(Cmd, TEXT("CrowdTestFight")) || FParse::Value(Cmd, TEXT("CrowdTestFight="), TestAt))
	{
		TestFightAt = TestAt;
	}
	FParse::Value(Cmd, TEXT("CrowdQuitAfter="), QuitAfter);
	if (QuitAfter > 0.0)
	{
		BuildMassing(); // only the footprint check reads the boxes, and it runs with -CrowdQuitAfter only
	}
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
			// camC (S-332), straight down over the fight, at every moment camA takes.
			for (int32 Cam = 0; Cam < 3; ++Cam)
			{
				const int32 Frames = (T == 2 && Cam != 1) ? 4 : 1;
				for (int32 F = 0; F < Frames; ++F)
				{
					Shots.Add({Times[T] + F * 0.25, Cam, T, FString::Printf(TEXT("%s/crowd_%d_%s_cam%c%s.png"), *ShotDir, T, Labels[T], TEXT('A') + Cam,
						Frames > 1 ? *FString::Printf(TEXT("_f%d"), F) : TEXT(""))});
				}
			}
		}
		// In time order, so each moment's stills from the three cameras are taken together.
		Shots.StableSort([](const FCrowdShot& A, const FCrowdShot& B) { return A.At < B.At; });
	}
	bRunning = true;
}

void UFTCrowdSubsystemV2::LoadArt()
{
	const TCHAR* Cmd = FCommandLine::Get();
	FString SkinPath = CrowdV2Local::kDefaultSkin, WalkPath = CrowdV2Local::kDefaultWalk, IdlePath = CrowdV2Local::kDefaultIdle, RunPath, CowerPath;
	FParse::Value(Cmd, TEXT("CrowdSkin="), SkinPath);
	FParse::Value(Cmd, TEXT("CrowdWalk="), WalkPath);
	FParse::Value(Cmd, TEXT("CrowdIdle="), IdlePath);
	FParse::Value(Cmd, TEXT("CrowdRun="), RunPath);
	FParse::Value(Cmd, TEXT("CrowdCower="), CowerPath);
	Skin = LoadObject<USkeletalMesh>(nullptr, *CrowdV2Local::CrowdObjectPath(SkinPath));
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
		UAnimSequence* A = LoadObject<UAnimSequence>(nullptr, *CrowdV2Local::CrowdObjectPath(Path));
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

bool UFTCrowdSubsystemV2::Occluded(void* Context, ftcore::Vec2 From, ftcore::Vec2 To)
{
	UFTCrowdSubsystemV2* Self = static_cast<UFTCrowdSubsystemV2*>(Context);
	UWorld* World = Self->GetWorld();
	if (!World || Self->OcclusionTraces >= CrowdV2Local::kOcclusionTracesPerFrame)
	{
		return false;
	}
	++Self->OcclusionTraces;
	const double Z = Self->PlayerZ + Self->EyeZ;
	FHitResult Hit;
	return World->LineTraceSingleByChannel(Hit, FVector(From.x * 100.0, From.y * 100.0, Z), FVector(To.x * 100.0, To.y * 100.0, Z),
		ECC_Visibility, Self->TraceQuery);
}

void UFTCrowdSubsystemV2::RaiseEvent(ftcore::crowdv2::CrowdEventKind Kind, const FVector& At, float RadiusScale, float IntensityScale)
{
	if (!bRunning)
	{
		return;
	}
	ftcore::crowdv2::CrowdEvent E;
	E.kind = Kind;
	E.at = CrowdV2Local::ToSim(At);
	E.radiusScale = RadiusScale;
	E.intensityScale = IntensityScale;
	Sim.pushEvent(E);
}

void UFTCrowdSubsystemV2::FeedArena(const ftcore::ArenaEvents& Events)
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
			if (RunTime - LastArenaEvent > CrowdV2Local::kFightQuiet)
			{
				RaiseEvent(ftcore::crowdv2::CrowdEventKind::FightStart, At);
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
			RaiseEvent(ftcore::crowdv2::CrowdEventKind::Strike, At);
			break;
		case ftcore::ArenaEventType::Blocked:
			RaiseEvent(ftcore::crowdv2::CrowdEventKind::Strike, At, 1.f, 0.6f);
			break;
		case ftcore::ArenaEventType::Downed:
			RaiseEvent(ftcore::crowdv2::CrowdEventKind::Death, At);
			break;
		case ftcore::ArenaEventType::Released:
			RaiseEvent(ftcore::crowdv2::CrowdEventKind::MagicBurst, At);
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
				const ftcore::Vec2 Q = CrowdV2Local::ToSim(At);
				for (uint32 I = 0; I < Sim.capacity(); ++I)
				{
					if (Sim.agent(I).state != ftcore::crowdv2::CrowdState::Free)
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

void UFTCrowdSubsystemV2::PickTestFight(const FVector& PlayerAt)
{
	if (bTestFightPicked)
	{
		return;
	}
	bTestFightPicked = true;
	// The street node within 40 m of a point 15 m east of the player with the most live members within 12 m (ties: the
	// one nearer that point), so the stills show people close to the fight; else the node nearest the player.
	const ftcore::Vec2 P = CrowdV2Local::ToSim(PlayerAt);
	const ftcore::Vec2 C{P.x + static_cast<float>(CrowdV2Local::kTestFightDistance), P.y};
	uint32 Best = UINT32_MAX;
	int32 BestN = -1;
	float BestD = 0.f;
	float CountR = 12.f; // m: the members counted round a node (the fallback counts the strike radius)
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
			const ftcore::crowdv2::CrowdAgentView& V = Sim.agent(i);
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
		// No street node within 40 m of that point: the node within 200 m of the player with the most live members
		// inside the strike radius (ties: the nearer), so somebody can hear the fight (was the player's nearest node).
		const float StrikeR = Tuning.events[static_cast<size_t>(ftcore::crowdv2::CrowdEventKind::Strike)].radius;
		CountR = StrikeR;
		for (uint32 n = 0; n < Graph.nodeCount(); ++n)
		{
			const ftcore::Vec2 Q = Graph.node(n);
			const float D = FMath::Sqrt((Q.x - P.x) * (Q.x - P.x) + (Q.y - P.y) * (Q.y - P.y));
			if (D > 200.f)
			{
				continue;
			}
			int32 Near = 0;
			for (uint32 i = 0; i < Sim.capacity(); ++i)
			{
				const ftcore::crowdv2::CrowdAgentView& V = Sim.agent(i);
				Near += V.live && (V.pos.x - Q.x) * (V.pos.x - Q.x) + (V.pos.y - Q.y) * (V.pos.y - Q.y) <= StrikeR * StrikeR ? 1 : 0;
			}
			if (Near > BestN || (Near == BestN && D < BestD))
			{
				Best = n;
				BestN = Near;
				BestD = D;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: test fight fallback: no street node within 40 m of the point %.0f m east of the player; node %d, %.1f m from the player, %d live members within the strike radius %.0f m."),
			CrowdV2Local::kTestFightDistance, Best == UINT32_MAX ? -1 : static_cast<int32>(Best), BestD, FMath::Max(BestN, 0), StrikeR);
	}
	const ftcore::Vec2 F = Best == UINT32_MAX ? C : Graph.node(Best);
	TestFightPoint = FVector(F.x * 100.0, F.y * 100.0, PlayerZ);
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: test fight point engine (%.0f, %.0f) cm, %.1f m from the player, %d live members within %.0f m at t=%.1f s."),
		TestFightPoint.X, TestFightPoint.Y, FVector::Dist2D(TestFightPoint, PlayerAt) / 100.0, FMath::Max(BestN, 0), CountR, RunTime);
}

void UFTCrowdSubsystemV2::ScriptFight(const FVector& PlayerAt)
{
	if (TestFightAt < 0.0 || RunTime < TestFightAt)
	{
		return;
	}
	if (!bTestFightStarted)
	{
		// Picked again at the start; the same spot keeps the ground height the cameras were aimed by (r5: the
		// player's height put the point 95 px off the centre fifth in both fight stills).
		const FVector Before = TestFightPoint;
		PickTestFight(PlayerAt);
		TestFightPoint.Z = FVector::Dist2D(Before, TestFightPoint) < 1.0 ? Before.Z : PlayerZ;
		AtTestFight = Sim.counts();
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: test fight starts at engine (%.0f, %.0f) cm, %.1f m from the player, at t=%.1f s; %u live, %u walking."),
			TestFightPoint.X, TestFightPoint.Y, FVector::Dist2D(TestFightPoint, PlayerAt) / 100.0, RunTime,
			Sim.liveCount(), Sim.countIn(ftcore::crowdv2::CrowdState::Walk));
		// Perception range at the start: who can feel the fight at all (within the close radius, within the fight
		// start's own radius) and the nearest member. Nobody in range means the reaction checks cannot pass here: a
		// fault of where the test fight stands, not of the crowd.
		{
			const ftcore::Vec2 F = CrowdV2Local::ToSim(TestFightPoint);
			const float EventR = Tuning.events[static_cast<size_t>(ftcore::crowdv2::CrowdEventKind::FightStart)].radius;
			uint32 InClose = 0, InEvent = 0;
			float Nearest = 1e9f;
			for (uint32 i = 0; i < Sim.capacity(); ++i)
			{
				const ftcore::crowdv2::CrowdAgentView& V = Sim.agent(i);
				if (!V.live)
				{
					continue;
				}
				const float D = FMath::Sqrt((V.pos.x - F.x) * (V.pos.x - F.x) + (V.pos.y - F.y) * (V.pos.y - F.y));
				InClose += D <= Tuning.reactRadius ? 1u : 0u;
				InEvent += D <= EventR ? 1u : 0u;
				Nearest = FMath::Min(Nearest, D);
			}
			FightInRange = InEvent;
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: test fight perception range: %u live members within the close radius %.0f m, %u within the fight start's radius %.0f m, nearest %.1f m%s."),
				InClose, Tuning.reactRadius, InEvent, EventR, Nearest >= 1e8f ? -1.f : Nearest,
				InEvent == 0 ? TEXT(": NOBODY IN RANGE (the test fight's spot, not a crowd fault)") : TEXT(""));
		}
		// The first second: everyone within the reaction radius now, checked one second later.
		FirstSecond.begin(Sim, CrowdV2Local::ToSim(TestFightPoint), Tuning.reactRadius);
		TestFightStartedAt = RunTime;
		RaiseEvent(ftcore::crowdv2::CrowdEventKind::FightStart, TestFightPoint);
		bTestFightStarted = true;
		TestStrikes = 0;
	}
	const double Into = RunTime - TestFightAt;
	while (TestStrikes * CrowdV2Local::kTestStrikeEvery <= Into && TestStrikes * CrowdV2Local::kTestStrikeEvery <= CrowdV2Local::kTestFightLength)
	{
		RaiseEvent(ftcore::crowdv2::CrowdEventKind::Strike, TestFightPoint);
		++TestStrikes;
	}
	if (!bTestDeathRaised && Into >= CrowdV2Local::kTestFightLength)
	{
		RaiseEvent(ftcore::crowdv2::CrowdEventKind::Death, TestFightPoint);
		bTestDeathRaised = true;
		LogCounts(TEXT("test fight over"));
	}
}

void UFTCrowdSubsystemV2::Shoot(APlayerController* PC, const FVector& PlayerAt)
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
			for (const double Back : CrowdV2Local::kCamBack)
			{
				const double R = FMath::DegreesToRadians(static_cast<double>(Az));
				FEye E;
				E.At = Fight + FVector(FMath::Cos(R) * Back, FMath::Sin(R) * Back, CrowdV2Local::kCamUp);
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
		const FVector Fallback[] = {Fight + FVector(-1350.0, 0.0, CrowdV2Local::kCamUp), Fight + FVector(0.0, 1350.0, CrowdV2Local::kCamUp)};
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
		{
			// camC: orthographic, straight down over the fight point; the still's right is engine +Y, its top engine +X.
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ACameraActor* Top = World->SpawnActor<ACameraActor>(Fight + FVector(0.0, 0.0, CrowdV2Local::kTopUp), FRotator(-90.0, 0.0, 0.0), Spawn);
			if (!Top)
			{
				Shots.Reset();
				return;
			}
			UCameraComponent* C = Top->GetCameraComponent();
			C->SetProjectionMode(ECameraProjectionMode::Orthographic);
			C->SetOrthoWidth(static_cast<float>(CrowdV2Local::kTopWidth));
			C->SetConstraintAspectRatio(false);
			ShotCams.Add(Top);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: shots: cameras at %s and %s, aimed at %s (1 m over the fight point); camC orthographic %.0f m across, straight down, still right = engine +Y, still up = engine +X, %.1f px per m; %d stills to %s."),
			*Eyes[0].ToString(), *Eyes[1].ToString(), *Aim.ToString(), CrowdV2Local::kTopWidth / 100.0, 1600.0 / (CrowdV2Local::kTopWidth / 100.0), Shots.Num(), *ShotDir);
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
		if (S.Cam == 2)
		{
			LogTopPairs(S.Name);
			++ShotIndex;
			ShotTicks = -1;
			return;
		}
		LogScreenPairs(S.Cam, S.Name);
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
			const bool bTall = V.NearestPx >= CrowdV2Local::kMinPersonPx;
			bCamsOk = bCamsOk && bCentre && bSeen && bTall;
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %s: fight point at pixel (%.0f, %.0f) of 1600x900 (centre fifth %s), %s; nearest member in sight %.0f px tall (%s, gate %.0f)."),
				*FPaths::GetCleanFilename(S.Name), Px, Py, bCentre ? TEXT("ok") : TEXT("FAILED"), bSeen ? TEXT("in sight") : TEXT("OCCLUDED"), V.NearestPx,
				bTall ? TEXT("ok") : TEXT("FAILED"), CrowdV2Local::kMinPersonPx);
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
			const bool bBack = CalmCount[S.Cam] > 0 && VC.Calm * 2 >= CalmCount[S.Cam] && VC.MinGap >= CrowdV2Local::kGapPair;
			bRefillOk = bRefillOk && bBack;
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: refill cam%c: %d walking or standing about at +30 s of %d calm, least gap %.2f m: %s."), TEXT('A') + S.Cam,
				VC.Calm, CalmCount[S.Cam], VC.MinGap >= 1e8f ? -1.f : VC.MinGap, bBack ? TEXT("ok") : TEXT("FAILED"));
			int32 Still = 0, Elsewhere = 0, Gone = 0;
			for (const uint32 Id : CalmInView[S.Cam])
			{
				const ftcore::crowdv2::CrowdAgentView& V = Sim.agent(Id & 0xffffu);
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

void UFTCrowdSubsystemV2::LogCounts(const TCHAR* Label) const
{
	const ftcore::crowdv2::CrowdCounts& C = Sim.counts();
	UE_LOG(LogForgottenThird, Display,
		TEXT("Crowd: %s t=%.1f s: live %u (walk %u idle %u notice %u back %u flee %u cower %u recover %u), drawn actors %d; spawned %llu despawned %llu perceived %llu noticed %llu backed %llu fled %llu cowered %llu resumed %llu spread %llu plans %llu fails %llu dropped %llu occlusion %llu."),
		Label, RunTime, Sim.liveCount(), Sim.countIn(ftcore::crowdv2::CrowdState::Walk), Sim.countIn(ftcore::crowdv2::CrowdState::Idle),
		Sim.countIn(ftcore::crowdv2::CrowdState::Notice), Sim.countIn(ftcore::crowdv2::CrowdState::BackAway), Sim.countIn(ftcore::crowdv2::CrowdState::Flee),
		Sim.countIn(ftcore::crowdv2::CrowdState::Cower), Sim.countIn(ftcore::crowdv2::CrowdState::Recover), [this]
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
	UE_LOG(LogForgottenThird, Display,
		TEXT("Crowd: %s churn: despawned %llu (born past the despawn distance %llu, walked out %llu); spawn tries missed: no street near the ring point %llu, no free slot %llu, spot outside the ring %llu, crowded %llu, too many near (spawnNearMax %u) %llu, in view %llu."),
		Label, C.despawned, C.despawnNewborn, C.despawnWalkedOut, C.missNoStreet, C.missSlot, C.missSpotRing, C.missCrowded, Tuning.spawnNearMax, C.missDense,
		C.missSpotView);
}

void UFTCrowdSubsystemV2::Tick(float DeltaTime)
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
		Sim.setDrawFocus(true, CrowdV2Local::ToSim(TestFightPoint));
		if (!bPartyMoved && !ShotDir.IsEmpty())
		{
			MovePartyToFight(World->GetFirstPlayerController());
			if (const APawn* Pawn = World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr)
			{
				PlayerAt = Pawn->GetActorLocation();
				PlayerZ = PlayerAt.Z - Pawn->GetSimpleCollisionHalfHeight();
			}
		}
	}
	ScriptFight(PlayerAt);
	FeedViews(World->GetFirstPlayerController());
	if (!bGroundLogged && RunTime >= 1.0)
	{
		bGroundLogged = true;
		LogGroundAt();
	}

	OcclusionTraces = 0;
	Sim.tick(DeltaTime, CrowdV2Local::ToSim(PlayerAt), CrowdV2Local::ToSim(PlayerVel));
	if (QuitAfter > 0.0)
	{
		const float Pair = Sim.minPairDistance(2.f);
		if (Pair < RunMinPair && Pair < 2.f)
		{
			NoteMinPair(Pair);
		}
		RunMinPair = FMath::Min(RunMinPair, Pair);
		SlowRunFrames += Sim.runPoseSlow();
		// Every 8th frame once the streets have filled (the first members stand still on their first step).
		if (RunTime >= 2.0 && ++GroupTick % 8 == 0)
		{
			const uint32 Group = Sim.largestStoppedGroup(3.f);
			NoteReactingLeftOut(Group);
			if (Group > CrowdV2Local::kGroupGate && Group > WorstGroup)
			{
				LogStoppedGroup(Group);
			}
			WorstGroup = FMath::Max(WorstGroup, Group);
			const uint32 Jam = Sim.largestJam(ftcore::crowdv2::kCrowdJamRadius, ftcore::crowdv2::kCrowdJamSeconds);
			if (Jam > WorstJam)
			{
				if (Jam > ftcore::crowdv2::kCrowdJamGate)
				{
					UE_LOG(LogForgottenThird, Display, TEXT("Crowd: walking jam of %u at t=%.2f s (walkers slow for %.0f s or more within %.0f m of one; gate %u)."),
						Jam, RunTime, ftcore::crowdv2::kCrowdJamSeconds, ftcore::crowdv2::kCrowdJamRadius, ftcore::crowdv2::kCrowdJamGate);
				}
				WorstJam = Jam;
				WorstJamAt = RunTime;
			}
		}
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
		const ftcore::crowdv2::CrowdReactionResult R = FirstSecond.evaluate(Sim, CrowdV2Local::kFirstSecondPath, CrowdV2Local::kFirstSecondRadial);
		bFirstSecondOk = R.ok();
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: first second (%.2f s, radius %.0f m): inside %u, away %u, cornered %u, turned %u, toward %u, worst path %.2f m, worst gain %.2f m: %s."),
			RunTime - TestFightStartedAt, Tuning.reactRadius, R.inside, R.away, R.cornered, R.turned, R.toward, R.worstPath, R.worstGain, bFirstSecondOk ? TEXT("ok") : TEXT("FAILED"));
	}
	Draw();
	Shoot(World->GetFirstPlayerController(), PlayerAt);
	FootStill(World->GetFirstPlayerController());

	if (RunTime >= NextLog)
	{
		LogCounts(TEXT("counts"));
		NextLog += 10.0;
	}
	if (QuitAfter > 0.0 && RunTime >= QuitAfter)
	{
		LogCounts(TEXT("final"));
		const ftcore::crowdv2::CrowdCounts& C = Sim.counts();
		// Shared checks: nobody flip-flops between running and stopping, and the street is calm again at the end.
		const uint32 Live = Sim.liveCount();
		const uint32 Upset = Live - Sim.countIn(ftcore::crowdv2::CrowdState::Walk) - Sim.countIn(ftcore::crowdv2::CrowdState::Idle);
		const bool bCalm = Upset * 20 <= Live;
		const bool bSteady = C.maxFleeEntries <= CrowdV2Local::kMaxFleeEntries && C.maxPlans <= CrowdV2Local::kMaxPlansOne;
		// With bandDensity on the streets fill only to its cap, the walking band in reach: that is the target
		// here then. Off (0, the default), bandCap() is UINT32_MAX and the tuning's target stands as before.
		const uint32 FullTarget = FMath::Min(Tuning.targetAgents, Sim.bandCap());
		const bool bFull = Live + Live / 10 >= FullTarget;
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: %u live of %u target (%s); %u still upset (%s); %llu fleers, most flee entries by one %u, most plans by one %u (%s)."),
			Live, FullTarget, bFull ? TEXT("full") : TEXT("SHORT"), Upset, bCalm ? TEXT("calm") : TEXT("NOT CALM"), C.fleers,
			C.maxFleeEntries, C.maxPlans, bSteady ? TEXT("steady") : TEXT("FLIP-FLOP"));
		const bool bUnstuck = C.stuck == 0;
		const bool bOutside = FootFrames == 0;
		const bool bClearOfMass = MinMassClear / 100.0 >= CrowdV2Local::kGapWall;
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: members stuck (no progress for 2 s walking or backing off, 1 s running) %llu (%s); drawn members inside a massing box or on a roof %llu member-frames, %d members (%s); least distance from a drawn member to a massing box %.6f m unrounded (%s, passes at %.1f m or more; %d boxes)."),
			C.stuck, bUnstuck ? TEXT("ok") : TEXT("FAILED"), FootFrames, FootMembers, bOutside ? TEXT("ok") : TEXT("FAILED"),
			MinMassClear >= 1e8 ? -1.0 : MinMassClear / 100.0, bClearOfMass ? TEXT("ok") : TEXT("FAILED"), CrowdV2Local::kGapWall, MassBoxes.Num());
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: by render bounds (the box the check used before; reference, not gated): %llu member-frames, %d members inside one, least distance %.6f m; of those members, re-tested against the mesh's own collision shape: %d overlapped by a 25 cm body, %d within the 0.8 m gate, of %d re-tested."),
			FootFramesRender, FootMembersRender, MinMassClearRender >= 1e8 ? -1.0 : MinMassClearRender / 100.0, RetestInBody, RetestInGate, RetestLogged);
		if (MinMassBox != INDEX_NONE)
		{
			const FMassBox& M = MassBoxes[MinMassBox];
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: the least distance to a massing box came from member %d and box %d, %s: centre (%.0f, %.0f) half (%.0f, %.0f), z %.0f to %.0f cm."),
				MinMassMember, MinMassBox, *MassBoxName(MinMassBox), M.C.X, M.C.Y, M.Half.X, M.Half.Y, M.ZMin, M.ZMax);
		}
		if (FootMembers > 0)
		{
			FString PerMember;
			for (int32 Member = 0; Member < FootFramesBy.Num(); ++Member)
			{
				if (FootFramesBy[Member] > 0)
				{
					PerMember += FString::Printf(TEXT("%s%d: %d"), PerMember.IsEmpty() ? TEXT("") : TEXT(", "), Member, FootFramesBy[Member]);
				}
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: footprint member-frames by member: %s."), *PerMember);
		}
		if (ZoneZBox.bIsValid)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: zone z: %d members, %llu member-frames inside x %.0f..%.0f, y %.0f..%.0f cm; feet z %.1f to %.1f cm."), ZoneMembers,
				ZoneFrames, ZoneZBox.Min.X, ZoneZBox.Max.X, ZoneZBox.Min.Y, ZoneZBox.Max.Y, ZoneMembers ? ZoneZMin : 0.0, ZoneMembers ? ZoneZMax : 0.0);
		}
		const bool bPairs = RunMinPair >= CrowdV2Local::kGapPair;
		const bool bWalls = static_cast<double>(RunMinWall) >= CrowdV2Local::kGapWall;
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: least distance between two members %.2f m (%s, gate %.1f m), least wall clearance %.6f m unrounded (%s, passes at %.1f m or more), every frame, every state."),
			RunMinPair, bPairs ? TEXT("ok") : TEXT("FAILED"), CrowdV2Local::kGapPair, RunMinWall, bWalls ? TEXT("ok") : TEXT("FAILED"), CrowdV2Local::kGapWall);
		if (MinPairTime >= 0.0)
		{
			const ftcore::Vec2 Fight = CrowdV2Local::ToSim(TestFightPoint);
			FString Who;
			for (int32 k = 0; k < 2; ++k)
			{
				const std::string_view N = ftcore::crowdv2::crowdStateName(static_cast<ftcore::crowdv2::CrowdState>(MinPairState[k]));
				Who += FString::Printf(TEXT(" [%u %s at (%.2f, %.2f) m, %.2f m/s, wall %.4f m%s]"), MinPairSlot[k], *FString(static_cast<int32>(N.size()), N.data()),
					MinPairPos[k].x, MinPairPos[k].y, MinPairSpeed[k], MinPairWall[k],
					bTestFightStarted ? *FString::Printf(TEXT(", %.1f m from the fight"), FMath::Sqrt(FMath::Square(MinPairPos[k].x - Fight.x) + FMath::Square(MinPairPos[k].y - Fight.y))) : TEXT(""));
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: the least distance between two members came at t=%.2f s%s:%s."), MinPairTime,
				TestFightAt >= 0.0 ? *FString::Printf(TEXT(" (%.2f s from the fight's start)"), MinPairTime - TestFightAt) : TEXT(""), *Who);
		}
		// S-332: no run pose below walking speed, no knot of more than three standing within 3 m, nobody popping in or out
		// in a camera's view within 40 m.
		const bool bNoSlowRun = SlowRunFrames == 0;
		const bool bNoKnot = WorstGroup <= CrowdV2Local::kGroupGate;
		const bool bNoPops = C.popsInView == 0;
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: run pose below walking speed %llu member-frames (%s, gate 0); largest group standing within 3 m %u (%s, gate %u); spawned, despawned, drawn or undrawn in a camera's view within %.0f m %llu (%s, gate 0)."),
			SlowRunFrames, bNoSlowRun ? TEXT("ok") : TEXT("FAILED"), WorstGroup, bNoKnot ? TEXT("ok") : TEXT("FAILED"), CrowdV2Local::kGroupGate, CrowdV2Local::kViewRange,
			static_cast<unsigned long long>(C.popsInView), bNoPops ? TEXT("ok") : TEXT("FAILED"));
		const bool bStills = Shots.Num() == 0 || ShotIndex >= Shots.Num();
		if (Shots.Num() > 0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: checks: stills %d of %d (%s); cameras on the fight %s; the crowd back in view at +30 s %s."), ShotIndex, Shots.Num(),
				bStills ? TEXT("ok") : TEXT("FAILED"), bCamsOk ? TEXT("ok") : TEXT("FAILED"), bRefillOk ? TEXT("ok") : TEXT("FAILED"));
		}
		bool bOk = bCalm && bSteady && bFull && bUnstuck && bOutside && bClearOfMass && bPairs && bWalls && bStills && bCamsOk && bRefillOk &&
			bNoSlowRun && bNoKnot && bNoPops;
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
				bReacted ? TEXT("ok") : FightInRange == 0 ? TEXT("FAILED (nobody was within the fight start's radius when it started: the test fight's spot, not a crowd fault)")
					: TEXT("FAILED (nobody perceived, fled, resumed or spread panic)"));
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
		// The walking-jam check (walkers held under kCrowdJamSpeed x their own pace, the close-reaction stop left out) is
		// reported on its own line, after the verdict and not in it, until the narrow walking bands are widened. With it,
		// the walking band in reach (what bandDensity caps by) and the streets' target now (capped only when it is on).
		const bool bNoJam = WorstJam <= ftcore::crowdv2::kCrowdJamGate;
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: jam (reported, not in the verdict): largest walking jam (walkers under %.0f%% of their pace for %.0f s or more, within %.0f m of one) %u at t=%.2f s (%s, gate %u); walking band in the spawn ring %.1f m2 on %.1f m of spawnable street; streets' target %u (bandDensity %.3f, 0 = off)."),
			100.f * ftcore::crowdv2::kCrowdJamSpeed, ftcore::crowdv2::kCrowdJamSeconds, ftcore::crowdv2::kCrowdJamRadius, WorstJam, WorstJamAt,
			bNoJam ? TEXT("within the gate") : TEXT("over the gate"), ftcore::crowdv2::kCrowdJamGate, Sim.reachBandArea(), Sim.reachLength(),
			Sim.streetTarget(), Tuning.bandDensity);
		bRunning = false;
		FTRequestExit(bOk ? 0 : 1, TEXT("Crowd"));
	}
}

void UFTCrowdSubsystemV2::PlayFor(int32 Slot, ftcore::crowdv2::CrowdAnim Anim, float Speed)
{
	USkeletalMeshComponent* C = Pool[Slot]->GetSkeletalMeshComponent();
	UAnimSequence* Clip = nullptr;
	float Rate = 1.f;
	switch (Anim)
	{
	case ftcore::crowdv2::CrowdAnim::Walk:
		Clip = WalkClip;
		Rate = Speed / CrowdV2Local::kWalkClipSpeed;
		break;
	case ftcore::crowdv2::CrowdAnim::Run:
		Clip = RunClip ? RunClip.Get() : WalkClip.Get();
		Rate = RunClip ? Speed / CrowdV2Local::kRunClipSpeed : FMath::Min(Speed / CrowdV2Local::kWalkClipSpeed, CrowdV2Local::kRunOnWalkMax);
		break;
	case ftcore::crowdv2::CrowdAnim::BackStep:
		// The walk played backwards until there is a back-step clip.
		Clip = WalkClip;
		Rate = -FMath::Max(Speed, 0.3f) / CrowdV2Local::kWalkClipSpeed;
		break;
	case ftcore::crowdv2::CrowdAnim::Cower:
		Clip = CowerClip ? CowerClip.Get() : IdleClip.Get();
		break;
	case ftcore::crowdv2::CrowdAnim::Idle:
	case ftcore::crowdv2::CrowdAnim::Look:
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

void UFTCrowdSubsystemV2::Draw()
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
		const ftcore::crowdv2::CrowdAgentView& V = Sim.agent(Slot);
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
		const ftcore::crowdv2::CrowdAgentView& V = Sim.agent(i);
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
		double Z = PlayerZ;
		if (FirstGround(static_cast<int32>(i), V.pos.x * 100.0, V.pos.y * 100.0, PlayerZ, Z))
		{
			PoolGround[FreeCursor] = Z;
			PoolGrounded[FreeCursor] = true;
		}
		Pool[FreeCursor]->SetActorHiddenInGame(!PoolGrounded[FreeCursor]);
	}
	// Ground: a few lines a frame, round robin; an actor that has never found the ground looks over a long span first.
	for (int32 n = 0; n < CrowdV2Local::kGroundTracesPerFrame && n < Pool.Num(); ++n)
	{
		const int32 k = GroundCursor;
		GroundCursor = (GroundCursor + 1) % Pool.Num();
		if (PoolAgent[k] == 0)
		{
			continue;
		}
		const ftcore::crowdv2::CrowdAgentView& V = Sim.agent((PoolAgent[k] - 1) & 0xffffu);
		const double X = V.pos.x * 100.0, Y = V.pos.y * 100.0;
		if (!PoolGrounded[k])
		{
			double Z = PoolGround[k];
			if (FirstGround(static_cast<int32>((PoolAgent[k] - 1) & 0xffffu), X, Y, PoolGround[k], Z))
			{
				PoolGround[k] = Z;
				PoolGrounded[k] = true;
				Pool[k]->SetActorHiddenInGame(false);
			}
			continue;
		}
		const FVector At(X, Y, PoolGround[k]);
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, At + FVector(0.0, 0.0, CrowdV2Local::kGroundUp), At - FVector(0.0, 0.0, CrowdV2Local::kGroundDown), ECC_Visibility, TraceQuery))
		{
			// More than a step up (or a line that starts inside a slab, hit where it starts): a slab over the member,
			// whose top this line met from above; look again as at first, not up onto it.
			double Z = Hit.ImpactPoint.Z;
			if (Hit.bStartPenetrating || Z - PoolGround[k] > CrowdV2Local::kGroundStep)
			{
				Z = PoolGround[k];
				FirstGround(static_cast<int32>((PoolAgent[k] - 1) & 0xffffu), X, Y, PoolGround[k], Z);
			}
			PoolGround[k] = Z;
		}
	}
	const float Alpha = FMath::Clamp(Sim.stepAlpha(), 0.f, 1.f);
	for (int32 k = 0; k < Pool.Num(); ++k)
	{
		if (PoolAgent[k] == 0 || !PoolGrounded[k])
		{
			continue;
		}
		const ftcore::crowdv2::CrowdAgentView& V = Sim.agent((PoolAgent[k] - 1) & 0xffffu);
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

bool UFTCrowdSubsystemV2::FirstGround(int32 Slot, double X, double Y, double CentreZ, double& OutZ)
{
	// Every surface top under (X, Y) on one long line down, highest first (the same line again with each top's component
	// ignored), then a short line up from just above each top: a body's height, with the plain query, so a slab over it
	// or a start inside a solid reads as not clear. Core picks the ground (crowdPickGround): the street under a corridor
	// or balcony slab, not the slab's top. TraceQuery ignores actors only; the components added here stay within the
	// list's inline 8 and are cleared before the lines up, so no heap.
	UWorld* World = GetWorld();
	ftcore::crowdv2::CrowdGroundHit Hits[ftcore::crowdv2::kCrowdGroundHitsMax];
	const UPrimitiveComponent* By[ftcore::crowdv2::kCrowdGroundHitsMax] = {};
	size_t N = 0;
	const FVector From(X, Y, CentreZ + CrowdV2Local::kGroundFirstSpan);
	const FVector To(X, Y, CentreZ - CrowdV2Local::kGroundFirstSpan);
	while (N < ftcore::crowdv2::kCrowdGroundHitsMax)
	{
		FHitResult H;
		if (!World->LineTraceSingleByChannel(H, From, To, ECC_Visibility, TraceQuery))
		{
			break;
		}
		const UPrimitiveComponent* C = H.GetComponent();
		By[N] = C;
		Hits[N++].z = H.ImpactPoint.Z;
		if (!C || N == ftcore::crowdv2::kCrowdGroundHitsMax)
		{
			break;
		}
		TraceQuery.AddIgnoredComponent(C);
	}
	TraceQuery.ClearIgnoredComponents();
	if (N == 0)
	{
		return false;
	}
	for (size_t i = 0; i < N; ++i)
	{
		FHitResult Up;
		const FVector Start(X, Y, Hits[i].z + CrowdV2Local::kGroundClearStart);
		Hits[i].clearUp = !World->LineTraceSingleByChannel(Up, Start, FVector(X, Y, Hits[i].z + CrowdV2Local::kGroundBody), ECC_Visibility, TraceQuery);
	}
	const size_t Pick = ftcore::crowdv2::crowdPickGround(Hits, N, CrowdV2Local::kGroundBody);
	OutZ = Hits[Pick].z;
	// Log only, capped: a ground that is not the first top (what was skipped). A skipped top that is a deck, quay or
	// bridge a street runs on is where this pick is wrong.
	if (Pick != 0 && ++GroundPicksUnder <= 24)
	{
		const AActor* Owner = By[0] ? By[0]->GetOwner() : nullptr;
		const AActor* PickOwner = By[Pick] ? By[Pick]->GetOwner() : nullptr;
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: member %d ground under a top (%d so far): skipped the top of %s (%s) at z %.1f cm, %.2f m over the pick; picked z %.1f cm on %s (%s), %d of %d tops."),
			Slot, GroundPicksUnder, Owner ? *Owner->GetActorNameOrLabel() : TEXT("no actor"), By[0] ? *By[0]->GetName() : TEXT("no component"), Hits[0].z,
			(Hits[0].z - OutZ) / 100.0, OutZ, PickOwner ? *PickOwner->GetActorNameOrLabel() : TEXT("no actor"), By[Pick] ? *By[Pick]->GetName() : TEXT("no component"),
			static_cast<int32>(Pick) + 1, static_cast<int32>(N));
	}
	return true;
}

void UFTCrowdSubsystemV2::CheckFootprint(int32 Actor, const FVector& Feet)
{
	// Inside a building's massing box (its bounds, not its collision), or stood on a roof: more ground under where it
	// stands, over half a metre down. Also the least distance to a box, round the feet's height.
	UWorld* World = GetWorld();
	FHitResult Below;
	const bool bRoof = PoolGrounded[Actor] && World->LineTraceSingleByChannel(Below, Feet - FVector(0.0, 0.0, 50.0),
		Feet - FVector(0.0, 0.0, 5000.0), ECC_Visibility, TraceQuery);
	const int32 Slot = static_cast<int32>((PoolAgent[Actor] - 1) & 0xffffu);
	if (ZoneZBox.bIsValid && ZoneZBox.IsInside(FVector2D(Feet.X, Feet.Y)))
	{
		++ZoneFrames;
		ZoneZMin = FMath::Min(ZoneZMin, Feet.Z);
		ZoneZMax = FMath::Max(ZoneZMax, Feet.Z);
		if (!ZoneSeen[Slot])
		{
			ZoneSeen[Slot] = true;
			if (++ZoneMembers <= 40)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Crowd: zone z: member %d first in at engine (%.0f, %.0f) cm, feet z %.1f cm, t=%.1f s."), Slot, Feet.X, Feet.Y,
					Feet.Z, RunTime);
			}
		}
	}
	int32 InBox = INDEX_NONE, InRender = INDEX_NONE;
	double InDepth = 0.0, InRenderDepth = 0.0; // the deepest box it is in (cm, below 0)
	const FIntPoint Cell(FMath::FloorToInt32(Feet.X / CrowdV2Local::kMassCell), FMath::FloorToInt32(Feet.Y / CrowdV2Local::kMassCell));
	auto BoxOut = [&Feet](const FVector2D& C, const FVector2D& Ax, const FVector2D& Half) {
		ftcore::crowdv2::CrowdMassBox B;
		B.cx = C.X;
		B.cy = C.Y;
		B.hx = Half.X;
		B.hy = Half.Y;
		return ftcore::crowdv2::crowdBoxClearance(B, Ax.X, Ax.Y, Feet.X, Feet.Y);
	};
	if (const TArray<int32>* Near = MassGrid.Find(Cell))
	{
		for (const int32 b : *Near)
		{
			const FMassBox& M = MassBoxes[b];
			if (Feet.Z > M.RZMin - 50.0 && Feet.Z < M.RZMax - 30.0)
			{
				const double R = BoxOut(M.RC, M.Ax, M.RHalf);
				MinMassClearRender = FMath::Min(MinMassClearRender, R);
				if (R < 0.0 && (InRender == INDEX_NONE || R < InRenderDepth))
				{
					InRender = b;
					InRenderDepth = R;
				}
			}
			if (Feet.Z <= M.ZMin - 50.0 || Feet.Z >= M.ZMax - 30.0)
			{
				continue;
			}
			const double Out = BoxOut(M.C, M.Ax, M.Half);
			if (Out < MinMassClear)
			{
				MinMassClear = Out;
				MinMassBox = b;
				MinMassMember = Slot;
			}
			if (Out < 0.0 && (InBox == INDEX_NONE || Out < InDepth))
			{
				InBox = b;
				InDepth = Out;
			}
		}
	}
	if (InRender != INDEX_NONE)
	{
		RetestFootprint(Slot, Feet, InRender, -InRenderDepth);
	}
	if (InBox == INDEX_NONE && !bRoof)
	{
		return;
	}
	++FootFrames;
	++FootFramesBy[Slot];
	if (!FootSeen[Slot])
	{
		FootSeen[Slot] = true;
		++FootMembers;
		// One line per member, on its first bad frame: what it stands in or on (the frames per member come at the end).
		if (FootLogged < 32)
		{
			++FootLogged;
			const ftcore::crowdv2::CrowdAgentView& V = Sim.agent(static_cast<uint32>(Slot));
			FString What;
			if (InBox != INDEX_NONE)
			{
				const FMassBox& M = MassBoxes[InBox];
				What = FString::Printf(TEXT(" inside massing box %d, %s: centre (%.0f, %.0f) half (%.0f, %.0f), z %.0f to %.0f cm, %.2f m in"), InBox,
					*MassBoxName(InBox), M.C.X, M.C.Y, M.Half.X, M.Half.Y, M.ZMin, M.ZMax, -InDepth / 100.0);
			}
			if (bRoof)
			{
				const AActor* Hit = Below.GetActor();
				What += FString::Printf(TEXT("%s on a roof: ground %.2f m further down, %s (%s)"), What.IsEmpty() ? TEXT("") : TEXT(";"),
					(Feet.Z - Below.ImpactPoint.Z) / 100.0, Hit ? *Hit->GetActorNameOrLabel() : TEXT("no actor"),
					Below.GetComponent() ? *Below.GetComponent()->GetName() : TEXT("no component"));
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: member %d (%s) at engine (%.0f, %.0f, %.0f) cm, t=%.1f s:%s."), Slot,
				*FString(static_cast<int32>(ftcore::crowdv2::crowdStateName(V.state).size()), ftcore::crowdv2::crowdStateName(V.state).data()),
				Feet.X, Feet.Y, Feet.Z, RunTime, *What);
			if (bRoof)
			{
				LogSurfacesUnder(Slot, Feet, Below);
				if (!RoofStillPath.IsEmpty() && !bRoofStillDone)
				{
					// From its side: across its heading.
					const double Yaw = FMath::DegreesToRadians(static_cast<double>(V.yaw) + 90.0);
					AimFootStill(Slot, Feet, FVector2D(FMath::Cos(Yaw), FMath::Sin(Yaw)), RoofStillPath, true);
				}
			}
		}
	}
}

FString UFTCrowdSubsystemV2::MassBoxName(int32 Box) const
{
	// The actor, component and mesh behind a massing box (and the instance, for an instanced mesh): log only.
	const FMassBox& M = MassBoxes[Box];
	const UStaticMeshComponent* C = M.Comp.Get();
	if (!C)
	{
		return TEXT("its mesh is gone");
	}
	const AActor* OwnerActor = C->GetOwner();
	const UStaticMesh* Mesh = C->GetStaticMesh();
	return FString::Printf(TEXT("actor %s, component %s, mesh %s%s"), OwnerActor ? *OwnerActor->GetActorNameOrLabel() : TEXT("none"), *C->GetName(),
		Mesh ? *Mesh->GetPathName() : TEXT("none"), M.Instance != INDEX_NONE ? *FString::Printf(TEXT(", instance %d"), M.Instance) : TEXT(""));
}

void UFTCrowdSubsystemV2::RetestFootprint(int32 Slot, const FVector& Feet, int32 Box, double DepthCm)
{
	// Inside a box's render bounds (the box the check used before): re-tested once per member against the mesh's own
	// collision shape (its body, or its instance's): a 25 cm body round the member, and the 0.8 m gate round it.
	++FootFramesRender;
	if (FootSeenRender[Slot])
	{
		return;
	}
	FootSeenRender[Slot] = true;
	++FootMembersRender;
	const FMassBox& M = MassBoxes[Box];
	const UStaticMeshComponent* C = M.Comp.Get();
	const FBodyInstance* Body = nullptr;
	if (const UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C))
	{
		const TArray<FBodyInstance*>& Bodies = I->GetInstanceBodies();
		Body = Bodies.IsValidIndex(M.Instance) ? Bodies[M.Instance] : nullptr;
	}
	else if (C)
	{
		Body = C->GetBodyInstance();
	}
	const FVector Mid = Feet + FVector(0.0, 0.0, 90.0);
	const bool bBody = Body && Body->OverlapTest(Mid, FQuat::Identity, FCollisionShape::MakeCapsule(25.f, 90.f));
	const bool bGate = Body && Body->OverlapTest(Mid, FQuat::Identity, FCollisionShape::MakeCapsule(static_cast<float>(CrowdV2Local::kGapWall * 100.0), 90.f));
	++RetestLogged;
	RetestInBody += bBody ? 1 : 0;
	RetestInGate += bGate ? 1 : 0;
	if (RetestLogged <= 16)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: re-test: member %d at engine (%.0f, %.0f, %.0f) cm, t=%.1f s, %.2f m inside the render bounds of box %d, %s (render centre (%.0f, %.0f) half (%.0f, %.0f); checked box %s centre (%.0f, %.0f) half (%.0f, %.0f) z %.0f to %.0f cm): against its collision shape %s: a 25 cm body overlaps it: %s; within the 0.8 m gate of it: %s."),
			Slot, Feet.X, Feet.Y, Feet.Z, RunTime, DepthCm / 100.0, Box, *MassBoxName(Box), M.RC.X, M.RC.Y, M.RHalf.X, M.RHalf.Y,
			M.bColl ? TEXT("(simple collision's bounds)") : TEXT("(render bounds: no simple collision)"), M.C.X, M.C.Y, M.Half.X, M.Half.Y, M.ZMin, M.ZMax,
			Body ? TEXT("(its body)") : TEXT("(NO BODY: not tested)"), bBody ? TEXT("YES") : TEXT("no"), bGate ? TEXT("YES") : TEXT("no"));
	}
	// One still of the first, from 6 m out on the member's side of the box, 2.5 m up (before the fight's stills).
	if (!FootStillPath.IsEmpty() && !bFootStillDone)
	{
		AimFootStill(Slot, Feet, FVector2D(Feet.X - M.RC.X, Feet.Y - M.RC.Y), FootStillPath, false);
	}
}

void UFTCrowdSubsystemV2::AimFootStill(int32 Slot, const FVector& Feet, FVector2D Out, const FString& Path, bool bRoof)
{
	// A still from 6 m out along Out, 2.5 m up, aimed at the member's chest; one at a time, before the fight's stills.
	if (FootStillTicks >= 0 || ShotTicks >= 0 || (TestFightAt >= 0.0 && RunTime >= TestFightAt - 4.0))
	{
		return;
	}
	if (!Out.Normalize())
	{
		Out = FVector2D(1.0, 0.0);
	}
	const FVector Eye = Feet + FVector(Out.X * 600.0, Out.Y * 600.0, 250.0);
	const FVector Aim = Feet + FVector(0.0, 0.0, 100.0);
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	FootStillCam = GetWorld()->SpawnActor<ACameraActor>(Eye, (Aim - Eye).Rotation(), Spawn);
	if (FootStillCam)
	{
		FootStillCam->GetCameraComponent()->SetFieldOfView(70.f);
		FootStillCam->GetCameraComponent()->SetConstraintAspectRatio(false);
		FootStillTicks = 0;
		FootStillShot = Path;
		bStillIsRoof = bRoof;
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %s still of member %d from %s, aimed at %s."), bRoof ? TEXT("roof") : TEXT("re-test"), Slot, *Eye.ToString(),
			*Aim.ToString());
	}
}

void UFTCrowdSubsystemV2::LogSurfacesUnder(int32 Slot, const FVector& Feet, const FHitResult& Roof) const
{
	// Log only, a member's first frame on a roof. The roof line starts half a metre under the feet, so a slab thicker
	// than that is hit where the line starts (0.50 m down, started inside it). Then every surface under the member, top
	// down, from where its first grounding line starts: it stands on the first.
	const AActor* RoofBy = Roof.GetActor();
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: member %d roof line: hit at z %.1f cm, %.2f m under the feet, started inside it %s (penetration %.1f cm), normal z %.2f, %s (%s)."),
		Slot, Roof.ImpactPoint.Z, (Feet.Z - Roof.ImpactPoint.Z) / 100.0, Roof.bStartPenetrating ? TEXT("YES") : TEXT("no"), Roof.PenetrationDepth,
		Roof.ImpactNormal.Z, RoofBy ? *RoofBy->GetActorNameOrLabel() : TEXT("no actor"), Roof.GetComponent() ? *Roof.GetComponent()->GetName() : TEXT("no component"));
	UWorld* World = GetWorld();
	FCollisionQueryParams Q(TraceQuery);
	const FVector From(Feet.X, Feet.Y, Feet.Z + CrowdV2Local::kGroundFirstSpan);
	const FVector To(Feet.X, Feet.Y, Feet.Z - 5000.0);
	for (int32 n = 0; n < 10; ++n)
	{
		FHitResult H;
		if (!World->LineTraceSingleByChannel(H, From, To, ECC_Visibility, Q))
		{
			break;
		}
		const AActor* By = H.GetActor();
		const UPrimitiveComponent* C = H.GetComponent();
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: member %d surface %d under it: z %.1f cm (%+.2f m from the feet), normal z %.2f, %s (%s, %s)%s."), Slot, n,
			H.ImpactPoint.Z, (H.ImpactPoint.Z - Feet.Z) / 100.0, H.ImpactNormal.Z, By ? *By->GetActorNameOrLabel() : TEXT("no actor"),
			C ? *C->GetName() : TEXT("no component"), By ? *By->GetClass()->GetName() : TEXT("?"), H.bStartPenetrating ? TEXT(", started inside it") : TEXT(""));
		if (C)
		{
			Q.AddIgnoredComponent(C);
		}
		else if (By)
		{
			Q.AddIgnoredActor(By);
		}
		else
		{
			break;
		}
	}
}

void UFTCrowdSubsystemV2::FootStill(APlayerController* PC)
{
	if (!PC || FootStillTicks < 0 || !FootStillCam)
	{
		return;
	}
	if (PC->GetViewTarget() != FootStillCam)
	{
		PC->SetViewTarget(FootStillCam);
	}
	++FootStillTicks;
	if (FootStillTicks == 3)
	{
		if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
		{
			Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *FootStillShot), *GLog);
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %s still %s at t=%.1f s."), bStillIsRoof ? TEXT("roof") : TEXT("re-test"), *FootStillShot, RunTime);
		}
	}
	else if (FootStillTicks >= 6)
	{
		PC->SetViewTarget(PC->GetPawn());
		FootStillTicks = -1;
		(bStillIsRoof ? bRoofStillDone : bFootStillDone) = true;
	}
}

void UFTCrowdSubsystemV2::BuildMassing()
{
	// The buildings' massing boxes: every static mesh (and instance) tall and wide enough to be one by its render bounds;
	// the box checked is ftcore's crowdMassingBox (its simple collision's bounds when it has any, else its render bounds).
	UWorld* World = GetWorld();
	int32 Colls = 0;
	auto Add = [this, &Colls](const FBox& Local, const FBox* Coll, const FTransform& X, const UStaticMeshComponent* Comp, int32 Instance)
	{
		const FVector Scale = X.GetScale3D().GetAbs();
		auto ToWorld = [&X, &Scale](const FBox& B) {
			const FVector C = X.TransformPosition(B.GetCenter());
			const FVector H = B.GetExtent() * Scale;
			ftcore::crowdv2::CrowdMassBox Out;
			Out.cx = C.X;
			Out.cy = C.Y;
			Out.cz = C.Z;
			Out.hx = H.X;
			Out.hy = H.Y;
			Out.hz = H.Z;
			return Out;
		};
		const ftcore::crowdv2::CrowdMassBox Render = ToWorld(Local);
		const ftcore::crowdv2::CrowdMassBox Collision = Coll ? ToWorld(*Coll) : ftcore::crowdv2::CrowdMassBox{};
		ftcore::crowdv2::CrowdMassSize Size;
		Size.minHeight = CrowdV2Local::kMassMinHeight;
		Size.minSide = CrowdV2Local::kMassMinSide;
		Size.maxSide = CrowdV2Local::kMassMaxSide;
		ftcore::crowdv2::CrowdMassBox Box;
		if (!ftcore::crowdv2::crowdMassingBox(Render, Coll ? &Collision : nullptr, Size, Box))
		{
			return;
		}
		FVector2D Ax(X.GetUnitAxis(EAxis::X).X, X.GetUnitAxis(EAxis::X).Y);
		if (!Ax.Normalize())
		{
			Ax = FVector2D(1.0, 0.0);
		}
		FMassBox M;
		M.C = FVector2D(Box.cx, Box.cy);
		M.Ax = Ax;
		M.Half = FVector2D(Box.hx, Box.hy);
		M.ZMin = Box.cz - Box.hz;
		M.ZMax = Box.cz + Box.hz;
		M.RC = FVector2D(Render.cx, Render.cy);
		M.RHalf = FVector2D(Render.hx, Render.hy);
		M.RZMin = Render.cz - Render.hz;
		M.RZMax = Render.cz + Render.hz;
		M.bColl = Coll != nullptr;
		Colls += M.bColl ? 1 : 0;
		M.Comp = Comp;
		M.Instance = Instance;
		const int32 b = MassBoxes.Add(M);
		// Grid cells by the larger of the two boxes (both are looked up).
		const FVector C(Render.cx, Render.cy, Render.cz);
		const FVector2D Half(FMath::Max(Render.hx, Box.hx + FMath::Abs(Box.cx - Render.cx) + FMath::Abs(Box.cy - Render.cy)),
			FMath::Max(Render.hy, Box.hy + FMath::Abs(Box.cx - Render.cx) + FMath::Abs(Box.cy - Render.cy)));
		const double R = FMath::Abs(Ax.X) * Half.X + FMath::Abs(Ax.Y) * Half.Y + 100.0;
		const double Ry = FMath::Abs(Ax.Y) * Half.X + FMath::Abs(Ax.X) * Half.Y + 100.0;
		for (int32 cx = FMath::FloorToInt32((C.X - R) / CrowdV2Local::kMassCell); cx <= FMath::FloorToInt32((C.X + R) / CrowdV2Local::kMassCell); ++cx)
		{
			for (int32 cy = FMath::FloorToInt32((C.Y - Ry) / CrowdV2Local::kMassCell); cy <= FMath::FloorToInt32((C.Y + Ry) / CrowdV2Local::kMassCell); ++cy)
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
			// Simple collision, when the mesh has any and queries use it (not complex as simple).
			FBox CollLocal(ForceInit);
			const UBodySetup* Body = Mesh->GetBodySetup();
			if (Body && Body->AggGeom.GetElementCount() > 0 && Body->GetCollisionTraceFlag() != CTF_UseComplexAsSimple)
			{
				CollLocal = Body->AggGeom.CalcAABB(FTransform::Identity);
			}
			const FBox* Coll = CollLocal.IsValid ? &CollLocal : nullptr;
			if (const UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C))
			{
				for (int32 n = 0; n < I->GetInstanceCount(); ++n)
				{
					FTransform X;
					if (I->GetInstanceTransform(n, X, true))
					{
						Add(Local, Coll, X, C, n);
					}
				}
			}
			else
			{
				Add(Local, Coll, C->GetComponentTransform(), C, INDEX_NONE);
			}
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %d massing boxes for the footprint check (at least %.1f m tall and %.1f m wide by render bounds; %d checked by their simple collision's bounds, the rest by render bounds), %d grid cells."), MassBoxes.Num(),
		CrowdV2Local::kMassMinHeight / 100.0, CrowdV2Local::kMassMinSide / 100.0, Colls, MassGrid.Num());
}

// A knot over the gate: who stands in it (state, speed, metres from the fight), so a failed run says why.
void UFTCrowdSubsystemV2::LogStoppedGroup(uint32 Size) const
{
	// Core's rule (largestStoppedGroup): the one-step close-reaction stop is not standing.
	auto Stopped = [](const ftcore::crowdv2::CrowdAgentView& V) { return V.live && !V.indoors && !V.reacting && V.speed <= 0.05f; };
	const ftcore::Vec2 Fight = CrowdV2Local::ToSim(TestFightPoint);
	for (uint32 i = 0; i < Sim.capacity(); ++i)
	{
		const ftcore::crowdv2::CrowdAgentView& A = Sim.agent(i);
		if (!Stopped(A))
		{
			continue;
		}
		TArray<uint32> Members{i};
		for (uint32 j = 0; j < Sim.capacity(); ++j)
		{
			const ftcore::crowdv2::CrowdAgentView& B = Sim.agent(j);
			if (j != i && Stopped(B) && FMath::Square(A.pos.x - B.pos.x) + FMath::Square(A.pos.y - B.pos.y) < 9.f)
			{
				Members.Add(j);
			}
		}
		if (static_cast<uint32>(Members.Num()) < Size)
		{
			continue;
		}
		FString Who;
		for (const uint32 m : Members)
		{
			const ftcore::crowdv2::CrowdAgentView& V = Sim.agent(m);
			const std::string_view N = ftcore::crowdv2::crowdStateName(V.state);
			Who += FString::Printf(TEXT(" [%u %s%s fear %.2f, %.1f m from the fight]"), m, *FString(static_cast<int32>(N.size()), N.data()),
				V.cornered ? TEXT(" cornered") : TEXT(""), V.fear, FMath::Sqrt(FMath::Square(V.pos.x - Fight.x) + FMath::Square(V.pos.y - Fight.y)));
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: knot of %u standing at t=%.1f s:%s."), Size, RunTime, *Who);
		return;
	}
}

// A knot check that left members out for the one-step close-reaction stop: how many, and the figure the old rule (those
// counted as standing) gives beside the check's, so a quiet run shows it sampled that step (capped at 8 lines).
void UFTCrowdSubsystemV2::NoteReactingLeftOut(uint32 Group)
{
	if (ReactingLines >= 8)
	{
		return;
	}
	auto OldStopped = [](const ftcore::crowdv2::CrowdAgentView& V) { return V.live && !V.indoors && V.speed <= 0.05f; };
	uint32 Reacting = 0;
	for (uint32 i = 0; i < Sim.capacity(); ++i)
	{
		const ftcore::crowdv2::CrowdAgentView& V = Sim.agent(i);
		Reacting += V.live && !V.indoors && V.reacting ? 1u : 0u;
	}
	if (Reacting == 0)
	{
		return;
	}
	uint32 Old = 0;
	for (uint32 i = 0; i < Sim.capacity(); ++i)
	{
		const ftcore::crowdv2::CrowdAgentView& A = Sim.agent(i);
		if (!OldStopped(A))
		{
			continue;
		}
		uint32 N = 1;
		for (uint32 j = 0; j < Sim.capacity(); ++j)
		{
			const ftcore::crowdv2::CrowdAgentView& B = Sim.agent(j);
			N += j != i && OldStopped(B) && FMath::Square(A.pos.x - B.pos.x) + FMath::Square(A.pos.y - B.pos.y) < 9.f ? 1u : 0u;
		}
		Old = FMath::Max(Old, N);
	}
	++ReactingLines;
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: knot check at t=%.1f s left out %u members in the one-step close-reaction stop: largest standing within 3 m %u (the old rule, counting them: %u)."),
		RunTime, Reacting, Group, Old);
}

void UFTCrowdSubsystemV2::NoteMinPair(float Pair)
{
	// Which two: of the live members off their doors, the pair whose distance is nearest the sim's least (the sim skips
	// members at doors, so the plain closest pair could be another). A brute pass, only when the least drops.
	float BestErr = 0.01f;
	uint32 A = UINT32_MAX, B = UINT32_MAX;
	for (uint32 i = 0; i < Sim.capacity(); ++i)
	{
		const ftcore::crowdv2::CrowdAgentView& U = Sim.agent(i);
		if (!U.live || U.indoors)
		{
			continue;
		}
		for (uint32 j = i + 1; j < Sim.capacity(); ++j)
		{
			const ftcore::crowdv2::CrowdAgentView& V = Sim.agent(j);
			if (!V.live || V.indoors)
			{
				continue;
			}
			const float D2 = FMath::Square(U.pos.x - V.pos.x) + FMath::Square(U.pos.y - V.pos.y);
			if (D2 > FMath::Square(Pair + BestErr))
			{
				continue;
			}
			const float Err = FMath::Abs(FMath::Sqrt(D2) - Pair);
			if (Err < BestErr)
			{
				BestErr = Err;
				A = i;
				B = j;
			}
		}
	}
	if (A == UINT32_MAX)
	{
		return;
	}
	const uint32 Slots[2] = {A, B};
	for (int32 k = 0; k < 2; ++k)
	{
		const ftcore::crowdv2::CrowdAgentView& V = Sim.agent(Slots[k]);
		MinPairSlot[k] = Slots[k];
		MinPairState[k] = static_cast<uint8>(V.state);
		MinPairPos[k] = V.pos;
		MinPairSpeed[k] = V.speed;
		MinPairWall[k] = Sim.wallClearance(Slots[k]);
	}
	MinPairTime = RunTime;
}

void UFTCrowdSubsystemV2::LogGroundAt()
{
	// Every surface under one point, top down: the terrain against the street there. Members stand on the first one.
	double X = CrowdV2Local::kGroundAtX, Y = CrowdV2Local::kGroundAtY;
	FString At;
	// bShouldStopOnSeparator false: the default stops at the comma and drops Y (as -WorldStartAt reads its list).
	if (FParse::Value(FCommandLine::Get(), TEXT("CrowdGroundAt="), At, false))
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

bool UFTCrowdSubsystemV2::Clear(const FVector& Eye, const FVector& To) const
{
	// Nothing static between the eye and the point (a hit within half a metre of it is the thing itself).
	FHitResult H;
	return !GetWorld()->LineTraceSingleByChannel(H, Eye, To, ECC_Visibility, TraceQuery) || FVector::Dist(H.ImpactPoint, To) < 50.0;
}

UFTCrowdSubsystemV2::FViewCount UFTCrowdSubsystemV2::CountInView(int32 Cam, TArray<uint32>* Ids) const
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
		const ftcore::crowdv2::CrowdAgentView& V = Sim.agent((PoolAgent[k] - 1) & 0xffffu);
		const FVector Feet = Pool[k]->GetActorLocation();
		const FVector Chest = Feet + FVector(0.0, 0.0, 120.0);
		const FVector L = Q.UnrotateVector(Chest - Eye);
		if (L.X <= 0.0 || FMath::Abs(L.Y) > L.X * TanX || FMath::Abs(L.Z) > L.X * TanY || !Clear(Eye, Chest))
		{
			continue;
		}
		++R.N;
		R.Calm += V.state == ftcore::crowdv2::CrowdState::Walk || V.state == ftcore::crowdv2::CrowdState::Idle ? 1 : 0;
		R.NearestPx = FMath::Max(R.NearestPx, CrowdV2Local::kPersonHeight * Focal / L.X);
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

void UFTCrowdSubsystemV2::FeedViews(APlayerController* PC)
{
	ftcore::crowdv2::CrowdViewCone Cones[ftcore::crowdv2::CrowdSim::kMaxViews];
	uint32 N = 0;
	auto Add = [&](const FVector& Eye, double Yaw, double HalfFov, double Range) {
		if (N < ftcore::crowdv2::CrowdSim::kMaxViews)
		{
			Cones[N++] = ftcore::crowdv2::CrowdViewCone{CrowdV2Local::ToSim(Eye), static_cast<float>(Yaw), static_cast<float>(HalfFov), static_cast<float>(Range)};
		}
	};
	if (PC && PC->PlayerCameraManager)
	{
		// The player's view (during the stills, the still's camera; the stills' cameras are added below as well).
		Add(PC->PlayerCameraManager->GetCameraLocation(), PC->PlayerCameraManager->GetCameraRotation().Yaw, PC->PlayerCameraManager->GetFOVAngle() * 0.5,
			CrowdV2Local::kViewRange);
	}
	for (int32 c = 0; c < ShotCams.Num() && c < 2; ++c)
	{
		if (ShotCams[c])
		{
			Add(ShotCams[c]->GetActorLocation(), ShotCams[c]->GetActorRotation().Yaw, 35.0, CrowdV2Local::kViewRange);
		}
	}
	if (ShotCams.Num() > 2 && ShotCams[2])
	{
		// camC sees all round its frame from straight above: half its diagonal (m), and a margin.
		const double Half = 0.5 * FMath::Sqrt(1.0 + (9.0 / 16.0) * (9.0 / 16.0)) * CrowdV2Local::kTopWidth / 100.0 + 5.0;
		Add(ShotCams[2]->GetActorLocation(), 0.0, 180.0, Half);
	}
	Sim.setViews(Cones, N);
}

void UFTCrowdSubsystemV2::MovePartyToFight(APlayerController* PC)
{
	bPartyMoved = true;
	UWorld* World = GetWorld();
	APawn* Lead = PC ? PC->GetPawn() : nullptr;
	if (!World || !Lead)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Crowd: party to the fight point: no player pawn, FAILED."));
		return;
	}
	auto Ground = [&](const FVector2D& At, const AActor* Self) {
		const FVector Top(At.X, At.Y, TestFightPoint.Z + 3000.0);
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(CrowdParty), false, Self);
		return World->LineTraceSingleByChannel(Hit, Top, Top - FVector(0.0, 0.0, 6000.0), ECC_Visibility, Q) ? Hit.ImpactPoint.Z : TestFightPoint.Z;
	};
	// The lead on the fight point, the others a step behind on either side, all facing east (towards the fight's
	// usual camera side is not known yet; the stills are taken from round it).
	const FVector2D F(TestFightPoint.X, TestFightPoint.Y);
	Lead->SetActorLocationAndRotation(FVector(F.X, F.Y, Ground(F, Lead) + Lead->GetSimpleCollisionHalfHeight() + 5.0), FRotator::ZeroRotator, false, nullptr,
		ETeleportType::TeleportPhysics);
	TraceQuery.AddIgnoredActor(Lead);
	const FVector2D Offsets[] = {{-120.0, 110.0}, {-120.0, -110.0}, {-240.0, 0.0}};
	int32 Moved = 0;
	for (TActorIterator<AFTPartyFollower> It(World); It; ++It)
	{
		const FVector2D At = F + Offsets[FMath::Min(Moved, 2)];
		It->SetActorLocationAndRotation(FVector(At.X, At.Y, Ground(At, *It) + It->GetSimpleCollisionHalfHeight() + 2.0), FRotator::ZeroRotator, false, nullptr,
			ETeleportType::TeleportPhysics);
		TraceQuery.AddIgnoredActor(*It);
		++Moved;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: party to the fight point engine (%.0f, %.0f) cm: the lead and %d others, at t=%.1f s."), F.X, F.Y, Moved, RunTime);
}

void UFTCrowdSubsystemV2::LogScreenPairs(int32 Cam, const FString& Name) const
{
	// Every two drawn members in sight whose figures overlap in the still (half a metre wide, feet to head), with the
	// true distance between them on the ground.
	const ACameraActor* C = ShotCams[Cam];
	const FVector Eye = C->GetActorLocation();
	const FQuat Q = C->GetActorQuat();
	const double TanX = FMath::Tan(FMath::DegreesToRadians(35.0)), TanY = TanX * 9.0 / 16.0;
	struct FFig
	{
		FVector2D Feet;
		double X0, X1, Y0, Y1;
		uint8 State;
	};
	TArray<FFig, TInlineAllocator<64>> Figs;
	for (int32 k = 0; k < Pool.Num(); ++k)
	{
		if (PoolAgent[k] == 0 || !PoolGrounded[k])
		{
			continue;
		}
		const FVector Feet = Pool[k]->GetActorLocation();
		const FVector Lf = Q.UnrotateVector(Feet - Eye), Lh = Q.UnrotateVector(Feet + FVector(0.0, 0.0, CrowdV2Local::kPersonHeight) - Eye);
		if (Lf.X <= 0.0 || Lh.X <= 0.0 || !Clear(Eye, Feet + FVector(0.0, 0.0, 120.0)))
		{
			continue;
		}
		const double Px = 800.0 * (1.0 + Lf.Y / (Lf.X * TanX));
		const double HalfW = 800.0 * 25.0 / (Lf.X * TanX);
		const double Yf = 450.0 * (1.0 - Lf.Z / (Lf.X * TanY)), Yh = 450.0 * (1.0 - Lh.Z / (Lh.X * TanY));
		if (Px + HalfW < 0.0 || Px - HalfW > 1600.0 || Yf < 0.0 || Yh > 900.0)
		{
			continue;
		}
		const ftcore::crowdv2::CrowdAgentView& V = Sim.agent((PoolAgent[k] - 1) & 0xffffu);
		Figs.Add({FVector2D(Feet.X, Feet.Y), Px - HalfW, Px + HalfW, Yh, Yf, static_cast<uint8>(V.state)});
	}
	int32 Overlaps = 0;
	float Least = 1e9f;
	for (int32 i = 0; i < Figs.Num(); ++i)
	{
		for (int32 j = i + 1; j < Figs.Num(); ++j)
		{
			const FFig& A = Figs[i];
			const FFig& B = Figs[j];
			if (A.X1 < B.X0 || B.X1 < A.X0 || A.Y1 < B.Y0 || B.Y1 < A.Y0)
			{
				continue;
			}
			++Overlaps;
			const float D = static_cast<float>(FVector2D::Distance(A.Feet, B.Feet) / 100.0);
			Least = FMath::Min(Least, D);
			UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %s: on screen together at (%.0f-%.0f, %.0f-%.0f) px: true distance %.2f m (%s, %s)."),
				*FPaths::GetCleanFilename(Name), FMath::Min(A.X0, B.X0), FMath::Max(A.X1, B.X1), FMath::Min(A.Y0, B.Y0), FMath::Max(A.Y1, B.Y1), D,
				*FString(UTF8_TO_TCHAR(std::string(ftcore::crowdv2::crowdStateName(static_cast<ftcore::crowdv2::CrowdState>(A.State))).c_str())),
				*FString(UTF8_TO_TCHAR(std::string(ftcore::crowdv2::crowdStateName(static_cast<ftcore::crowdv2::CrowdState>(B.State))).c_str())));
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %s: %d members in frame and in sight, %d pairs overlapping on screen, least true distance among them %.2f m."),
		*FPaths::GetCleanFilename(Name), Figs.Num(), Overlaps, Least >= 1e8f ? -1.f : Least);
}

void UFTCrowdSubsystemV2::LogTopPairs(const FString& Name) const
{
	// From straight above: the true spacing of the drawn members in camC's frame, and every pair closer than 1 m.
	const double PxPerCm = 1600.0 / CrowdV2Local::kTopWidth;
	TArray<FVector2D, TInlineAllocator<64>> In;
	for (int32 k = 0; k < Pool.Num(); ++k)
	{
		if (PoolAgent[k] == 0 || !PoolGrounded[k])
		{
			continue;
		}
		const FVector P = Pool[k]->GetActorLocation();
		const double Px = 800.0 + (P.Y - TestFightPoint.Y) * PxPerCm, Py = 450.0 - (P.X - TestFightPoint.X) * PxPerCm;
		if (Px >= 0.0 && Px <= 1600.0 && Py >= 0.0 && Py <= 900.0)
		{
			In.Add(FVector2D(P.X, P.Y));
		}
	}
	float Least = 1e9f;
	int32 Close = 0;
	for (int32 i = 0; i < In.Num(); ++i)
	{
		for (int32 j = i + 1; j < In.Num(); ++j)
		{
			const float D = static_cast<float>(FVector2D::Distance(In[i], In[j]) / 100.0);
			Least = FMath::Min(Least, D);
			if (D < 1.f)
			{
				++Close;
				UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %s: pair at (%.0f, %.0f) and (%.0f, %.0f) px: %.2f m apart."), *FPaths::GetCleanFilename(Name),
					800.0 + (In[i].Y - TestFightPoint.Y) * PxPerCm, 450.0 - (In[i].X - TestFightPoint.X) * PxPerCm, 800.0 + (In[j].Y - TestFightPoint.Y) * PxPerCm,
					450.0 - (In[j].X - TestFightPoint.X) * PxPerCm, D);
			}
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Crowd: %s: %d drawn members in the frame from above, least distance between two %.2f m, %d pairs under 1 m (gate %.1f m)."),
		*FPaths::GetCleanFilename(Name), In.Num(), Least >= 1e8f ? -1.f : Least, Close, CrowdV2Local::kGapPair);
}

TStatId UFTCrowdSubsystemV2::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTCrowdSubsystemV2, STATGROUP_Tickables);
}
