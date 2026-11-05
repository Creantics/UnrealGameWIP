#include "FTArenaRunner.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "FTActionButtons.h"
#include "FTArenaFighter.h"
#include "FTGameInstance.h"
#include "FTPauseSubsystem.h"
#include "FTText.h"
#include "FTVfxComponent.h"
#include "ForgottenThird.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#if WITH_EDITORONLY_DATA
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#endif

#include "ftcore/Json.h"

#include <cmath>
#include <string>
#include <vector>

static_assert(UFTArenaRunner::MaxEnemies == 6, "EnemyFigures[6] in FTArenaRunner.h must follow ftcore::kMaxArenaEnemies");

namespace
{
// The stone_bearer's effects and where they are placed (FFTArenaBossVfx).
constexpr const TCHAR* kStoneBearerVfx = TEXT("Data/Battle/arena_boss/stone_bearer_vfx.json");
// The stone_bearer's carried piece after it splits: the slot that goes and the parts for the hands.
constexpr const TCHAR* kStoneBearerSplit = TEXT("Data/Battle/arena_boss/stone_bearer_split.json");
// After the outcome, the autopilot (without -ArenaQuitOnFinish) fights again this much later (s), or kArenaDefeatTail after
// the longest defeat clip of the bosses that died, when that is later: the clip plays out on the result screen.
constexpr double kArenaAutoAgain = 4.0;
constexpr double kArenaDefeatTail = 0.5;
// The autopilot shows a victory's banner, once its death clips have played, at least this long before it fights again (s).
constexpr double kArenaResultShown = 2.0;
// A press to fight again counts only once the result has been up this long (s), so a held attack doesn't skip it.
constexpr double kArenaAgainDelay = 0.6;
// How long a hit tints a body (s).
constexpr double kArenaHitTint = 0.16;
// The floor tell's flash after a move goes active (s).
constexpr double kArenaTellFlash = 0.3;
// A boss at least this tall (m) is looked up at: the camera's pitch rises toward its face, at this share of its height
// (hover included), to at most kArenaPitchMax (above that the boom swings into the ground).
constexpr float kArenaTallBoss = 4.f;
constexpr float kArenaFaceHeight = 0.7f;
// A skinned enemy's hit reactions and playing rates: the rate that lands a clip's impact at the end of its startup is held
// to these.
constexpr float kArenaMinClipRate = 0.25f;
constexpr float kArenaMaxClipRate = 4.f;
// Under this share of its hp a skinned enemy idles its second-phase way (when its table has one).
constexpr float kArenaSecondIdleHp = 0.6f;
// A skin with a walk clip walks above the first ground speed (m/s) and stops under the second; its speed is smoothed over
// kArenaWalkSmooth seconds, and the walk plays at that speed over its authored one, held to these rates.
constexpr float kArenaWalkStart = 0.35f;
constexpr float kArenaWalkStop = 0.1f;
constexpr float kArenaWalkSmooth = 0.15f;
constexpr float kArenaMinWalkRate = 0.5f;
constexpr float kArenaMaxWalkRate = 1.6f;
// The stone_bearer's walk (its slot only, when skinned): it plays at rate 1.0 and the body moves at the clip's authored
// speed along Core's wanted direction while it plays; Core's speed only starts and stops it. It starts only when Core's
// brain can begin no move for at least one whole cycle of the clip (ArenaFight::enemyPickIn: so never while it strafes
// out its attackDelay), while the wanted direction is within the cone about the facing (its cosine; outside it, it
// stands and turns), kStoneWalkStartPhase seconds into the clip (the walk's frame nearest the idle's first pose: frame
// 25 of 80 at 30 fps). When Core stops wanting it (under the stop speed, or out of the cone) it walks on to its next
// passing pose and stops there: kStoneWalkExits a cycle, whole parts of it from the start (frames 25 and 65), where the
// swinging foot passes the planted one and the pose is nearest the idle's (the curve report's join table: about 50 cm,
// at the hands; at a plant the feet are a stride apart, 70-80 cm off the idle's stance). A move, a reaction or a get-up
// stops it at once. Every stop logs its reason.
constexpr float kStoneWalkCone = 0.5f;
constexpr double kStoneWalkExits = 2.0;
// On Approach the cone is wider (90 degrees each side: cosine 0). Core's velocity there points at the lead while its
// facing turns at the kind's turn rate, so the narrow cone held most starts back.
constexpr float kStoneApproachCone = 0.f;
// The split's stagger is drawn by the runner this long from the split's frame (s), until Core's held Stagger plays it.
constexpr double kStoneSplitBridge = 0.1;
// The get-up's "stands it out" line only when this much of the down is left (s).
constexpr double kStoneStandOutLog = 0.05;
// Fight.autopilot never presses heavy. On the stone_bearer the pilot does, at most this often (s): in the heavy's reach,
// with the boss upright between its moves (no move of its own, not on its knees, not getting up).
constexpr double kStonePilotHeavyEvery = 4.0;
constexpr double kStoneWalkStartPhase = 0.833;
// The stone_bearer's flinches: a light blow starts no hit reaction within this long (s) of the last one's start, and a
// heavy blow restarts a playing one only after kStoneFlinchRestart.
constexpr double kStoneFlinchHold = 2.0;
constexpr double kStoneFlinchRestart = 0.4;
// The stone_bearer dies kneeling only while the kneel is on screen: down, going down, or its get-up at most this share in.
constexpr double kStoneKneelDeathUp = 0.4;
// The stepping turn (any enemy whose skin has turn_l and turn_r; the stone_bearer only with its host): standing between
// moves, Core never turns it on the spot. When the lead is kArenaTurnStart degrees or more off its facing, it doesn't walk
// and its idle has faded in, it steps round in cycles of kArenaTurnCycle seconds, each turning it kArenaTurnDeg on the
// clips' own curve (smootherstep over kArenaTurnPivot, then held while the pivot foot catches up), and at each cycle's end
// it goes on while the lead is still more than kArenaTurnResidual degrees off that way and the walk doesn't want to start.
// It starts, and steps on at a cycle's end, only when Core's brain can begin no move for at least kArenaTurnPivot
// (ArenaFight::enemyPickIn), so the step's yaw lands before a move takes the body. The figure's yaw follows the curve,
// scaled by the crossfade into the clip, so the pivot foot stays where it stood (turn_l pivots on foot_l, turn_r on
// foot_r). The stone_bearer's walk host holds its body still until its walk starts; any other kind's body goes where Core
// moves it, so Core keeps turning that one while it wants it moving (and no turn is under way), and the step Core wants it
// moving ends a turn under way. (The clips' table can't carry these: its rows take no unknown keys.)
constexpr float kArenaTurnDeg = 30.f;
constexpr double kArenaTurnPivot = 0.35;
constexpr double kArenaTurnCycle = 0.8;
constexpr float kArenaTurnStart = 20.f;
constexpr float kArenaTurnResidual = 15.f;
// -ArenaShowcase: the reel's lead-in on the idle's first frame (s, for the exposure to settle), the camera's bearing off
// the boss's front to its left (degrees) and its gap past the lead's camera arm (m: about where the lead stands off the
// boss's body), a lone tell's distance ahead of the boss (m) and a cut's crossfade (s: under a frame).
constexpr double kShowLeadIn = 4.0; // a new camera's auto-exposure takes about 3.2 s to settle at 30 fps
constexpr float kShowCamBearing = 30.f;
constexpr float kShowCamGap = 1.5f;
constexpr float kShowAhead = 3.f;
constexpr float kShowCutBlend = 0.001f;
// Camera: pitch limits, the pitch while locked, how fast it turns to a lock, stick and mouse rates.
constexpr float kArenaPitchMin = -50.f;
constexpr float kArenaPitchMax = 15.f;
constexpr float kArenaLockTurn = 7.f;
constexpr float kArenaStickTurn = 140.f; // degrees a second at full stick
constexpr float kArenaMouseTurn = 2.5f; // degrees per unit of mouse axis (as the engine's yaw scale)
// A right-stick flick switches the lock: past this, after returning under the rest.
constexpr float kArenaFlickOut = 0.7f;
constexpr float kArenaFlickRest = 0.3f;

// Body colours: the player's, and an enemy's by its kind's index in the table.
const FLinearColor kArenaPlayerColor(0.18f, 0.42f, 0.85f);
const FLinearColor kArenaKindColors[6] = {
	FLinearColor(0.55f, 0.12f, 0.1f),
	FLinearColor(0.45f, 0.2f, 0.6f),
	FLinearColor(0.25f, 0.38f, 0.3f),
	FLinearColor(0.6f, 0.42f, 0.12f),
	FLinearColor(0.2f, 0.45f, 0.55f),
	FLinearColor(0.4f, 0.4f, 0.42f),
};
const FLinearColor kArenaDeadColor(0.12f, 0.12f, 0.13f);
const FLinearColor kArenaOpenColor(1.f, 0.85f, 0.35f);
const FLinearColor kArenaHitColor(1.f, 0.95f, 0.9f);

float ArenaYaw(const ftcore::Vec2& Facing)
{
	return FMath::RadiansToDegrees(FMath::Atan2(Facing.y, Facing.x));
}

float ArenaLength(const ftcore::Vec2& V)
{
	return FMath::Sqrt(V.x * V.x + V.y * V.y);
}

ftcore::Vec2 ArenaLerp(const ftcore::Vec2& A, const ftcore::Vec2& B, float T)
{
	return ftcore::Vec2{A.x + (B.x - A.x) * T, A.y + (B.y - A.y) * T};
}

// How far through its part an attack is (0..1), from the time since it started.
float ArenaPartFraction(const ftcore::ArenaAttack& Attack, ftcore::ArenaPart Part, float Time)
{
	switch (Part)
	{
	case ftcore::ArenaPart::Startup:
		return Attack.startup > 0.f ? FMath::Clamp(Time / Attack.startup, 0.f, 1.f) : 1.f;
	case ftcore::ArenaPart::Active:
		return Attack.active > 0.f ? FMath::Clamp((Time - Attack.startup) / Attack.active, 0.f, 1.f) : 1.f;
	case ftcore::ArenaPart::Recovery:
		return Attack.recovery > 0.f ? FMath::Clamp((Time - Attack.startup - Attack.active) / Attack.recovery, 0.f, 1.f) : 1.f;
	default:
		return 0.f;
	}
}

// 1 at Time, falling to 0 over Life seconds.
float ArenaModePulse(double Now, double Time, double Life)
{
	const double Age = Now - Time;
	return (Age >= 0.0 && Age < Life) ? static_cast<float>(1.0 - Age / Life) : 0.f;
}

bool ArenaAnyJustPressed(const APlayerController& PC, const FKey& A, const FKey& B, const FKey& C = EKeys::Invalid,
	const FKey& D = EKeys::Invalid)
{
	return PC.WasInputKeyJustPressed(A) || PC.WasInputKeyJustPressed(B) || (C.IsValid() && PC.WasInputKeyJustPressed(C))
		|| (D.IsValid() && PC.WasInputKeyJustPressed(D));
}

const TCHAR* ArenaPilotName(bool bAutoPlay, bool bIdle)
{
	return bIdle ? TEXT("idle") : (bAutoPlay ? TEXT("autoplay") : TEXT("player"));
}

FVector ReadVector(const ftcore::JsonValue& V)
{
	return V.size() == 3 ? FVector(V[0].number(0.0), V[1].number(0.0), V[2].number(0.0)) : FVector::ZeroVector;
}

// The split's slot and parts from File; false (and logged) when it can't be read.
bool ReadCarriedSplit(const FString& File, FName& HideSlot, TArray<FFTCarriedPiece>& Pieces)
{
	TArray<uint8> Bytes;
	const FString Path = FPaths::ProjectDir() / File;
	ftcore::JsonDocument Doc;
	std::string Error;
	if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToArray(Bytes, *Path) || !Doc.parse(FT::BytesAsText(Bytes), &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: couldn't read %s (%s); the carried piece stays whole."), *File, *FT::ToFString(Error));
		return false;
	}
	const ftcore::JsonValue Root = Doc.root();
	HideSlot = FName(*FT::ToFString(Root["hideSlot"].string()));
	const ftcore::JsonValue List = Root["pieces"];
	for (std::uint32_t I = 0; I < List.size(); ++I)
	{
		const ftcore::JsonValue V = List[I];
		FFTCarriedPiece& P = Pieces.AddDefaulted_GetRef();
		P.Mesh = FT::ToFString(V["mesh"].string());
		P.Bone = FName(*FT::ToFString(V["bone"].string()));
		P.OffsetMeters = ReadVector(V["offset"]);
		const FVector Turn = ReadVector(V["turn"]); // pitch, yaw, roll (degrees)
		P.Turn = FRotator(Turn.X, Turn.Y, Turn.Z);
	}
	return true;
}

// The mesh as imported (its bind pose, before any clip), in metres at Scale: the body's top over its lowest point (every
// section but Slot's) and Slot's piece along its longest axis (the principal axis of its vertices). False when the
// mesh's source model isn't loaded (a cooked build) or it has no section drawn with Slot.
bool MeasureCarried(const USkeletalMesh& Mesh, FName Slot, double Scale, double& BodyTop, double& PieceLength)
{
#if WITH_EDITORONLY_DATA
	const FSkeletalMeshModel* Model = const_cast<USkeletalMesh&>(Mesh).GetImportedModel();
	if (!Model || Model->LODModels.Num() == 0)
	{
		return false;
	}
	const int32 SlotIndex = Mesh.GetMaterials().IndexOfByPredicate([Slot](const FSkeletalMaterial& M) { return M.MaterialSlotName == Slot; });
	const TArray<FSkelMeshSection>& Sections = Model->LODModels[0].Sections;
	double MinZ = TNumericLimits<double>::Max();
	double MaxZ = TNumericLimits<double>::Lowest();
	FVector Sum = FVector::ZeroVector;
	int64 Count = 0;
	for (const FSkelMeshSection& Section : Sections)
	{
		const bool bPiece = static_cast<int32>(Section.MaterialIndex) == SlotIndex;
		for (const FSoftSkinVertex& V : Section.SoftVertices)
		{
			const FVector P(V.Position);
			if (bPiece)
			{
				Sum += P;
				++Count;
			}
			else
			{
				MinZ = FMath::Min(MinZ, P.Z);
				MaxZ = FMath::Max(MaxZ, P.Z);
			}
		}
	}
	if (Count == 0 || MinZ > MaxZ)
	{
		return false;
	}
	const FVector Mean = Sum / static_cast<double>(Count);
	double Cov[3][3] = {};
	for (const FSkelMeshSection& Section : Sections)
	{
		if (static_cast<int32>(Section.MaterialIndex) != SlotIndex)
		{
			continue;
		}
		for (const FSoftSkinVertex& V : Section.SoftVertices)
		{
			const FVector D = FVector(V.Position) - Mean;
			for (int32 A = 0; A < 3; ++A)
			{
				for (int32 B = 0; B < 3; ++B)
				{
					Cov[A][B] += D[A] * D[B];
				}
			}
		}
	}
	// The principal axis by power iteration (the covariance is symmetric, its largest eigenvalue well apart for a slab).
	FVector Axis = FVector(1.0, 0.5, 0.25).GetSafeNormal();
	for (int32 I = 0; I < 64; ++I)
	{
		const FVector Next(Cov[0][0] * Axis.X + Cov[0][1] * Axis.Y + Cov[0][2] * Axis.Z, Cov[1][0] * Axis.X + Cov[1][1] * Axis.Y + Cov[1][2] * Axis.Z,
			Cov[2][0] * Axis.X + Cov[2][1] * Axis.Y + Cov[2][2] * Axis.Z);
		if (Next.IsNearlyZero())
		{
			break;
		}
		Axis = Next.GetSafeNormal();
	}
	double Lo = TNumericLimits<double>::Max();
	double Hi = TNumericLimits<double>::Lowest();
	for (const FSkelMeshSection& Section : Sections)
	{
		if (static_cast<int32>(Section.MaterialIndex) != SlotIndex)
		{
			continue;
		}
		for (const FSoftSkinVertex& V : Section.SoftVertices)
		{
			const double Along = FVector::DotProduct(FVector(V.Position) - Mean, Axis);
			Lo = FMath::Min(Lo, Along);
			Hi = FMath::Max(Hi, Along);
		}
	}
	BodyTop = (MaxZ - MinZ) * Scale / 100.0;
	PieceLength = (Hi - Lo) * Scale / 100.0;
	return true;
#else
	return false;
#endif
}
} // namespace


UFTArenaRunner::UFTArenaRunner()
{
	PrimaryComponentTick.bCanEverTick = false; // the owner ticks it, in its own order
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		ActiveSince[I] = -1e9;
		EnemyHitTime[I] = -1e9;
		SkinClip[I] = -2;
		SkinSince[I] = -1e9;
		SkinMove[I] = -1;
		SkinMoveTime[I] = 0.f;
		HitSince[I] = -1e9;
	}
}

const TCHAR* UFTArenaRunner::PilotName(bool bAutoPlayRun, bool bIdleRun)
{
	return ArenaPilotName(bAutoPlayRun, bIdleRun);
}

void UFTArenaRunner::Begin(const FFTArenaRunSetup& Setup)
{
	if (Setup.Table)
	{
		Table = *Setup.Table;
	}
	Def = Setup.Def;
	Fit = Setup.Fit;
	ZoneId = Setup.ZoneId;
	ZoneZ = Setup.ZoneZ;
	ZoneStyle = Setup.ZoneStyle;
	KitId = Setup.KitId;
	Seed = Setup.Seed;
	ZoneYaw = Setup.ZoneYaw;
	bAutoPlay = Setup.bAutoPlay;
	bIdle = Setup.bIdle;
	bHoldLoad = Setup.bHoldLoad;
	ParryHold = Setup.ParryHold;
	if (ParryHold > 0.f)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: no follow-up for %.1f s after a parry (-ArenaParryHold)."), ParryHold);
	}
	BossLoopName = Setup.BossLoop;
	BossLoopSlot = -1;
	BossLoopClip = -1;
	bBossLoopResolved = false;
	bBossSplitAtStart = Setup.bBossSplitAtStart;
	bQuitOnFinish = Setup.bQuitOnFinish;
	bShots = Setup.bShots;
	bFightAgain = Setup.bFightAgain;
	bReplacePawn = Setup.bReplacePawn;
	bShowcase = Setup.bShowcase;

	bBegun = true;
	BeginNow = Now;
	// The fitted zone's ring, and where the player starts in it.
	PrepareStoneBearer();
	PrepareKite();
	Fight.begin(Def, Fit.ring, Fit.playerStart, Fit.playerFacing, Seed);
	BeginStoneBearer();
	BeginKite();
	BeginKindCrits();
	SpawnFigures(Setup.Lead);
	PrepareBossCarried();
	if (bBossSplitAtStart)
	{
		SplitBossCarried(false);
	}
	SpawnKindCritEffects();
	PutBodiesAtStart();
	BeginBossLoop();
	PlaceFigures(1.f);
	if (KiteSlot >= 0)
	{
		KiteFx.Spawn(EnemyFigures[KiteSlot].Get());
	}
	if (bShowcase)
	{
		bShowcase = BeginShowcase();
	}
}

void UFTArenaRunner::TickFight(float DeltaSeconds, bool bHold)
{
	FrameDelta = DeltaSeconds;
	if (!bBegun)
	{
		return;
	}
	if (!bHold)
	{
		Now += DeltaSeconds;
	}
	if (bShowcase)
	{
		// The reel: no fight, no input, the camera fixed.
		TickShowcase(DeltaSeconds);
		return;
	}
	PollInput(DeltaSeconds);

	if (!Fight.over() && !bHold)
	{
		Accumulator = FMath::Min(Accumulator + static_cast<double>(DeltaSeconds), static_cast<double>(StepSeconds * MaxStepsPerFrame));
		while (Accumulator >= StepSeconds && !Fight.over())
		{
			StepFight();
			Accumulator -= StepSeconds;
		}
	}
	PlaceFigures(Fight.over() ? 1.f : static_cast<float>(Accumulator / StepSeconds));
	SteerCamera(DeltaSeconds);

	if (Fight.over())
	{
		if (!bResultLogged)
		{
			Finish();
		}
		else if (bFightAgain && !bShots && !bQuitOnFinish)
		{
			bool bAgain = bAutoPlay && GetResultAge() >= AutoAgainWait;
			const APlayerController* PC = GetPC();
			if (!bAgain && PC && !bAutoPlay && GetResultAge() >= kArenaAgainDelay && !UFTPauseSubsystem::IsBlocking(this))
			{
				bAgain = ArenaAnyJustPressed(*PC, EKeys::Enter, EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom);
			}
			if (bAgain)
			{
				Restart();
			}
		}
		return;
	}

	// A hang guard for unattended runs: a fight that outlasts the watchdog ends the run.
	if (bQuitOnFinish && (Fight.time() > WatchdogSeconds || Now - BeginNow > WatchdogSeconds * 2.0))
	{
		LogResult(); // a TIMEOUT line, as a warning
		bQuitOnFinish = false;
		FPlatformMisc::RequestExitWithStatus(false, 2, TEXT("Arena"));
	}
}

void UFTArenaRunner::End()
{
	EndStoneBearer();
	EndKite();
	EndKindCrits();
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		// Owned by the figures, which go next.
		KindCritTellFx[I] = nullptr;
		KindCritOpenFx[I] = nullptr;
		KindCritCritFx[I] = nullptr;
		KindCritHitFx[I] = nullptr;
	}
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		if (EnemyFigures[I])
		{
			EnemyFigures[I]->Destroy();
			EnemyFigures[I] = nullptr;
		}
		Skins[I] = FFTArenaSkin();
	}
	if (PlayerFigure)
	{
		PlayerFigure->Destroy();
		PlayerFigure = nullptr;
	}
	if (ShowCamera)
	{
		ShowCamera->Destroy();
		ShowCamera = nullptr;
	}
	Show.Reset();
	bFiguresSpawned = false;
	bBegun = false;
}

double UFTArenaRunner::SampleGround(const UWorld* World, const ftcore::Vec2& P)
{
	if (!World)
	{
		return 0.0;
	}
	// From far above, down: what stops the line first is a roof, a crown or the plaza's paving, not the ground. Take the
	// paving's top (the figures stand on it) and for anything else skip the actor and look again, until the Landscape.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ArenaGround), false);
	const FVector Top(P.x * 100.0, P.y * 100.0, 60000.0);
	for (int32 Try = 0; Try < 8; ++Try)
	{
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, Top, Top - FVector(0.0, 0.0, 120000.0), ECC_Visibility, Params))
		{
			break;
		}
		const UPrimitiveComponent* Component = Hit.GetComponent();
		if (Component && Component->GetClass()->GetName().Contains(TEXT("Landscape")))
		{
			return Hit.ImpactPoint.Z;
		}
		if (Component && Component->GetName().Contains(TEXT("PlazaDiscs")))
		{
			return Hit.ImpactPoint.Z;
		}
		if (!Hit.GetActor())
		{
			break;
		}
		Params.AddIgnoredActor(Hit.GetActor());
	}
	UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no ground under the zone; the figures stand at 0."));
	return 0.0;
}

void UFTArenaRunner::PutPlayerAt(const ftcore::Vec2& P)
{
	Bodies.player = P;
	PrevBodies.player = P;
}

APlayerController* UFTArenaRunner::GetPC() const
{
	return GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
}

void UFTArenaRunner::SpawnFigures(const FFTLeadLook* Lead)
{
	if (bFiguresSpawned)
	{
		return;
	}
	bFiguresSpawned = true;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// The player's figure is the pawn the controller has (the demo's DefaultPawnClass), else one made and possessed here;
	// the old pawn is destroyed, or only let go when the setup keeps it (the explore character, put back after).
	APlayerController* PC = GetPC();
	PlayerFigure = PC ? Cast<AFTArenaFighter>(PC->GetPawn()) : nullptr;
	if (!PlayerFigure)
	{
		PlayerFigure = World->SpawnActor<AFTArenaFighter>(AFTArenaFighter::StaticClass(), FTransform::Identity, Params);
		if (PC && PlayerFigure)
		{
			if (APawn* Old = PC->GetPawn())
			{
				PC->UnPossess();
				if (bReplacePawn)
				{
					Old->Destroy();
				}
			}
			PC->Possess(PlayerFigure);
		}
	}
	if (PlayerFigure)
	{
		PlayerFigure->Setup(true, Def.fighter.bodyRadius, kArenaPlayerColor, 0.f, Lead);
		const ftcore::ZoneCamera& Rig = Fit.camera;
		PlayerFigure->SetCameraRig(Rig.arm, Rig.pivot, Rig.side, Rig.up, Rig.fov);
	}
	if (PC)
	{
		PC->SetViewTarget(PlayerFigure);
	}

	for (int32 I = 0; I < static_cast<int32>(Def.enemyCount) && I < MaxEnemies; ++I)
	{
		AFTArenaFighter* Enemy = World->SpawnActor<AFTArenaFighter>(AFTArenaFighter::StaticClass(), FTransform::Identity, Params);
		if (!Enemy)
		{
			continue;
		}
		const uint8 Kind = Def.enemyKind[static_cast<size_t>(I)];
		const FLinearColor& Color = kArenaKindColors[(Kind == ftcore::kArenaNoKind ? 0 : Kind) % 6];
		const ftcore::ArenaEnemyKind& EnemyKind = Def.enemies[static_cast<size_t>(I)];
		Enemy->Setup(false, EnemyKind.bodyRadius, Color, ftcore::arenaEnemyHeight(EnemyKind));
		EnemyFigures[I] = Enemy;
		LoadSkin(I);
	}
	// The stone_bearer's picks: only the moves its figure has a clip for (a clipless one would stand in the idle while
	// its tell plays).
	if (StoneBearerSlot >= 0 && StoneBearerSlot < MaxEnemies && Skins[StoneBearerSlot].IsLoaded())
	{
		const FFTArenaSkin& Skin = Skins[StoneBearerSlot];
		const ftcore::ArenaEnemyKind& Kind = Def.enemies[static_cast<size_t>(StoneBearerSlot)];
		uint32 Playable = 0xFFFFFFFFu;
		for (int32 M = 0; M < static_cast<int32>(Kind.moveCount) && M < 32; ++M)
		{
			if (Skin.MoveClip[M] < 0)
			{
				Playable &= ~(1u << M);
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: boss move %d has no clip: kept out of the picks."), M);
			}
		}
		StoneBearer.SetPlayableMoves(Playable);
		StoneBearer.RefreshMoveMask();
	}
}

void UFTArenaRunner::BeginBossLoop()
{
	if (BossLoopName.IsEmpty())
	{
		return;
	}
	if (!bBossLoopResolved)
	{
		// Once: the boss is the Kite-Mother's slot, else the stone_bearer's, else the first skinned enemy; a name its table
		// doesn't have (or whose asset didn't load) is refused, and the flag does nothing.
		bBossLoopResolved = true;
		BossLoopSlot = KiteSlot >= 0 ? KiteSlot : StoneBearerSlot;
		for (int32 I = 0; BossLoopSlot < 0 && I < MaxEnemies; ++I)
		{
			BossLoopSlot = Skins[I].IsLoaded() ? I : -1;
		}
		const FFTArenaSkin* Skin = BossLoopSlot >= 0 && Skins[BossLoopSlot].IsLoaded() ? &Skins[BossLoopSlot] : nullptr;
		const int32 Clip = Skin ? Skin->Clips->find(FT::ToUtf8(BossLoopName)) : -1;
		if (!Skin || Clip < 0 || !Skin->Sequences[Clip])
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaBossLoop=%s: %s; the flag does nothing."), *BossLoopName,
				Skin ? TEXT("the boss's clip table has no loaded clip of that name") : TEXT("there is no skinned boss in this fight"));
			BossLoopSlot = -1;
			return;
		}
		BossLoopClip = Clip;
	}
	if (BossLoopClip >= 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: boss loop %s on enemy %d from the fight's start (%.2f s) (-ArenaBossLoop)."), *BossLoopName,
			BossLoopSlot, Skins[BossLoopSlot].Sequences[BossLoopClip]->GetPlayLength());
	}
}

void UFTArenaRunner::LoadSkin(int32 I)
{
	AFTArenaFighter* Figure = EnemyFigures[I].Get();
	const ftcore::ArenaEnemyKind& Kind = Def.enemies[static_cast<size_t>(I)];
	if (!Figure || !Kind.look.skinned())
	{
		return;
	}
	FFTArenaSkin& Skin = Skins[I];
	Skin = FFTArenaSkin();
	// One warning for the kind, whatever fails to load: it then keeps its shapes (the assets live only on some machines).
	const FString TablePath = FT::ToFString(std::string(Kind.look.clips()));
	const auto Refuse = [&](const FString& Why)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: enemy %d keeps its shapes: %s (%s)."), I, *Why, *TablePath);
		Skin = FFTArenaSkin();
	};
	TArray<uint8> Bytes;
	const FString File = FPaths::ProjectDir() / TablePath;
	if (!IFileManager::Get().FileExists(*File) || !FFileHelper::LoadFileToArray(Bytes, *File))
	{
		Refuse(TEXT("couldn't read the clip table"));
		return;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	Skin.Clips = MakeShared<ftcore::BeatClips>();
	if (!ftcore::importBeatClips(FT::BytesAsText(Bytes), *Skin.Clips, &Error, &Warnings))
	{
		Refuse(FString::Printf(TEXT("the clip table was refused:\n%s"), *FT::ToFString(Error)));
		return;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s: %s"), *TablePath, *FT::ToFString(W));
	}
	const auto LoadAsset = [](const FString& Package) -> UObject*
	{
		return Package.IsEmpty() ? nullptr : StaticLoadObject(UObject::StaticClass(), nullptr, *(Package + TEXT(".") + FPackageName::GetShortName(Package)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	};
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(LoadAsset(FT::ToFString(std::string(Skin.Clips->mesh()))));
	if (!Mesh)
	{
		Refuse(FString::Printf(TEXT("no skeletal mesh at %s"), *FT::ToFString(std::string(Skin.Clips->mesh()))));
		return;
	}
	Skin.Sequences.SetNum(static_cast<int32>(Skin.Clips->count));
	int32 Missing = 0;
	FString FirstMissing;
	for (size_t C = 0; C < Skin.Clips->count; ++C)
	{
		const FString Package = FT::ToFString(std::string(Skin.Clips->asset(C)));
		UAnimSequence* Sequence = Cast<UAnimSequence>(LoadAsset(Package));
		Skin.Sequences[static_cast<int32>(C)] = Sequence;
		if (!Sequence && !Package.IsEmpty())
		{
			FirstMissing = Missing == 0 ? Package : FirstMissing;
			++Missing;
		}
	}
	// The clip of an id, as the table has it and the assets loaded; -1 for none.
	const auto Find = [&Skin](std::string_view Id)
	{
		const int32 C = Id.empty() ? -1 : Skin.Clips->find(Id);
		return (C >= 0 && Skin.Sequences[C]) ? C : -1;
	};
	Skin.Idle = Find("idle");
	Skin.IdleTwo = Find("idle_p2");
	Skin.Stagger = Find("stagger");
	Skin.FaceDown = Find("face_down");
	Skin.FaceDownIn = Skin.FaceDown >= 0 ? Find("face_down_in") : -1;
	Skin.FaceDownOut = Skin.FaceDown >= 0 ? Find("face_down_out") : -1;
	Skin.Open = Find("open");
	Skin.Defeat = Find("defeat");
	Skin.DefeatKneel = Find("defeat_kneel");
	Skin.HitLight = Find("hit_light");
	Skin.HitHeavy = Find("hit_heavy");
	// The walk needs its authored speed to be played at the body's.
	Skin.Walk = Find("walk");
	Skin.Walk = Skin.Walk >= 0 && (*Skin.Clips)[static_cast<size_t>(Skin.Walk)].speed > 0.0 ? Skin.Walk : -1;
	for (size_t M = 0; M < ftcore::kMaxArenaMoves; ++M)
	{
		Skin.MoveClip[M] = M < Kind.moveCount ? Find(Kind.moves[M].clip()) : -1;
		Skin.RecoilClip[M] = Skin.MoveClip[M] >= 0 ? Find(std::string(Kind.moves[M].clip()) + "_recoil") : -1;
		Skin.MoveClipL[M] = Skin.MoveClip[M] >= 0 ? Find(std::string(Kind.moves[M].clip()) + "_l") : -1;
	}
	// Both turns or neither: one alone would turn it one way only.
	Skin.TurnL = Find("turn_l");
	Skin.TurnR = Find("turn_r");
	if (Skin.TurnL < 0 || Skin.TurnR < 0)
	{
		Skin.TurnL = Skin.TurnR = -1;
	}
	if (Skin.Idle < 0)
	{
		Refuse(FString::Printf(TEXT("no idle clip loaded (%d of %d clips missing, the first %s)"), Missing, static_cast<int32>(Skin.Clips->count), *FirstMissing));
		return;
	}
	if (Missing > 0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: enemy %d: %d of %d clips are missing (the first %s); those moves play the idle."), I, Missing,
			static_cast<int32>(Skin.Clips->count), *FirstMissing);
	}
	Figure->SetupSkinned(Mesh, Kind.look.hover, Kind.look.scale, Kind.look.yawOffset, ftcore::arenaEnemyHeight(Kind));
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: enemy %d is skinned: %s, hover %.1f m, scale %.2f, %d clips."), I, *Mesh->GetName(), Kind.look.hover,
		Kind.look.scale, static_cast<int32>(Skin.Clips->count) - Missing);
}

void UFTArenaRunner::DriveSkin(int32 I, const ftcore::ArenaEnemyOut& Out, const ftcore::ArenaEnemyKind& Kind, float Alpha)
{
	AFTArenaFighter* Figure = EnemyFigures[I].Get();
	const FFTArenaSkin& Skin = Skins[I];
	if (!Figure || !Figure->IsSkinned() || !Skin.IsLoaded())
	{
		return;
	}
	int32 Clip = Skin.Idle;
	bool bTimed = false;  // the time is Core's, not the time since the clip began
	bool bPhased = false; // the time is the walk's own phase (it still loops)
	double Time = 0.0;
	bool bHoldRoot = true;
	float MoveRate = 0.f; // a move's clip's playing rate, else 0
	double MoveEntry = 0.0; // where a move's clip was entered at the move's start (s)
	bool bRunsOn = false;   // the move's clip runs on from the move before (no new start of it)
	const bool bStone = I == StoneBearerSlot;
	if (bStone && Skin.Walk >= 0 && bWalking[I])
	{
		// StoneBearerWalk starts and stops it; it plays at rate 1.0 from its start phase.
		WalkPhase[I] = kStoneWalkStartPhase + (Now - StoneWalkSince);
	}
	else if (Skin.Walk >= 0 && !bStone)
	{
		// The body's ground speed over the last step, smoothed. It walks between moves while that is over the start speed,
		// until it falls under the stop speed, at that speed over the clip's authored one.
		const ftcore::Vec2 From = PrevBodies.enemies[static_cast<size_t>(I)];
		const ftcore::Vec2 To = Bodies.enemies[static_cast<size_t>(I)];
		const float Ground = FMath::Sqrt(FMath::Square(To.x - From.x) + FMath::Square(To.y - From.y)) / StepSeconds;
		WalkSpeed[I] += (Ground - WalkSpeed[I]) * FMath::Min(1.f, FrameDelta / kArenaWalkSmooth);
		const bool bWas = bWalking[I];
		const bool bBetween = Out.brain == ftcore::ArenaBrain::Approach || Out.brain == ftcore::ArenaBrain::Strafe;
		bWalking[I] = bBetween && WalkSpeed[I] > (bWas ? kArenaWalkStop : kArenaWalkStart);
		if (bWalking[I])
		{
			const float Authored = static_cast<float>((*Skin.Clips)[static_cast<size_t>(Skin.Walk)].speed);
			const float Rate = FMath::Clamp(WalkSpeed[I] / Authored, kArenaMinWalkRate, kArenaMaxWalkRate);
			WalkPhase[I] = bWas ? WalkPhase[I] + static_cast<double>(FrameDelta * Rate) : 0.0;
			if (!bWas)
			{
				// Logged for the walk checks in -nullrhi runs.
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: enemy %d walks at %.2f m/s (rate %.2f)."), Now, I, WalkSpeed[I], Rate);
			}
		}
	}
	// Down, and Open without a clip of its own, are spent on its knees; one that dies there is still on them.
	const bool bKnees = Out.brain == ftcore::ArenaBrain::Down || (Out.brain == ftcore::ArenaBrain::Open && Skin.Open < 0);
	const double UpLength = Skin.FaceDownOut >= 0 ? static_cast<double>(Skin.Sequences[Skin.FaceDownOut]->GetPlayLength()) : 0.0;
	if (Out.brain != ftcore::ArenaBrain::Dead)
	{
		if (bKnees && !bKneeling[I])
		{
			// The go-down is its reaction: a blow just before it, or on its knees, makes no flinch.
			KneelSince[I] = Now;
			HitSince[I] = -1e9;
			UpAt[I] = 0.0;
			if (bStone)
			{
				StoneUpStart = -1e9;
				bStoneStood = false;
			}
		}
		else if (!bKnees && bKneeling[I] && UpAt[I] < UpLength)
		{
			// The spell ended before its get-up ran out (a release from a hold): it still gets up, from where it got to.
			UpOwedUntil[I] = Now + (UpLength - UpAt[I]);
		}
		if (bStone && !bKnees)
		{
			StoneUpStart = -1e9;
			bStoneStood = false;
		}
		bKneeling[I] = bKnees;
	}
	bool bOwedUp = false;
	// A parried move's recoil lasts while the spell does (Open, Staggered or Down, however they chain).
	if (Out.brain != ftcore::ArenaBrain::Open && Out.brain != ftcore::ArenaBrain::Staggered && Out.brain != ftcore::ArenaBrain::Down)
	{
		RecoilPending[I] = -1;
	}
	switch (Out.brain)
	{
	case ftcore::ArenaBrain::Dead:
		if (bStone)
		{
			// Chosen once, from what was on screen: the kneeling death only while the kneel shows (going down, down, or
			// its get-up under kStoneKneelDeathUp in); a finished or mostly finished get-up dies standing.
			if (StoneDeathClip < 0)
			{
				const int32 Last = SkinClip[I];
				const bool bUpEarly = Last == Skin.FaceDownOut && Last >= 0 && UpAt[I] < kStoneKneelDeathUp * UpLength;
				const bool bKneelShows = Last >= 0 && (Last == Skin.FaceDownIn || Last == Skin.FaceDown || bUpEarly);
				StoneDeathClip = bKneelShows && Skin.DefeatKneel >= 0 ? Skin.DefeatKneel : (Skin.Defeat >= 0 ? Skin.Defeat : Skin.Idle);
				const TCHAR* Reason = bKneelShows ? (bUpEarly ? TEXT("its get-up had only begun")
					: (Last == Skin.FaceDownIn ? TEXT("it was going down (the go-down plays out first)") : TEXT("it was on its knees")))
					: (Last >= 0 && Last == Skin.FaceDownOut ? TEXT("its get-up was mostly done") : TEXT("it was standing"));
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d dies with %s: %s (last %s, get-up %.2f of %.2f s)."), Fight.time(), I,
					*Skin.Sequences[StoneDeathClip]->GetName(), Reason, Last >= 0 && Skin.Sequences[Last] ? *Skin.Sequences[Last]->GetName() : TEXT("none"),
					UpAt[I], UpLength);
			}
			Clip = StoneDeathClip;
			// Dead while still going down: the fall plays out onto its knees and the kneeling death starts there (the poses
			// in the middle of the fall are far from both deaths' first frames).
			const double DownLength = Skin.FaceDownIn >= 0 ? static_cast<double>(Skin.Sequences[Skin.FaceDownIn]->GetPlayLength()) : 0.0;
			if (Clip == Skin.DefeatKneel && Skin.FaceDownIn >= 0 && SkinClip[I] == Skin.FaceDownIn && Now - KneelSince[I] < DownLength)
			{
				Clip = Skin.FaceDownIn;
				Time = Now - KneelSince[I];
				bTimed = true;
			}
		}
		else
		{
			Clip = bKneeling[I] && Skin.DefeatKneel >= 0 ? Skin.DefeatKneel : (Skin.Defeat >= 0 ? Skin.Defeat : Skin.Idle);
		}
		bHoldRoot = false; // it falls
		break;
	case ftcore::ArenaBrain::Staggered:
		if (RecoilPending[I] >= 0)
		{
			// The parry's recoil runs on into the stagger: the same clip from the parry, so no blend between them. One that
			// went face down first starts it over.
			if (bRecoilDown[I])
			{
				RecoilSince[I] = Now;
				bRecoilDown[I] = false;
			}
			Clip = RecoilPending[I];
			Time = Now - RecoilSince[I];
			bTimed = true;
			break;
		}
		Clip = Skin.Stagger >= 0 ? Skin.Stagger : Skin.Idle;
		break;
	case ftcore::ArenaBrain::Open:
	case ftcore::ArenaBrain::Down:
	{
		if (RecoilPending[I] >= 0 && !bRecoilDown[I])
		{
			// The recoil holds the parried pose's motion while the partner's press may still turn the Open into a stagger;
			// an Open that outlasts that goes face down.
			const double PairWait = static_cast<double>(Fight.def().fighter.pairWindow + StepSeconds);
			if (Out.brain == ftcore::ArenaBrain::Open && Now - RecoilSince[I] < PairWait)
			{
				Clip = RecoilPending[I];
				Time = Now - RecoilSince[I];
				bTimed = true;
				break;
			}
			bRecoilDown[I] = true;
		}
		if (!bKnees)
		{
			// Its own clip where it has one: a parried enemy is open for openTime, and a riposte cuts it to the face-down clip.
			Clip = Skin.Open;
			break;
		}
		// On its knees: it goes down from the spell's start (an Open chained into a Down is one spell), holds there, and
		// gets up so that it stands as the state ends (never early in a hold until a release). The get-up wins a short state.
		Clip = Skin.FaceDown >= 0 ? Skin.FaceDown : (Skin.Stagger >= 0 ? Skin.Stagger : Skin.Idle);
		const double Left = static_cast<double>(Out.stateLength) - (static_cast<double>(Out.stateTime) + static_cast<double>(Alpha) * StepSeconds);
		if (bStone && Skin.FaceDownOut >= 0 && (StoneUpStart > -1e8 || bStoneStood))
		{
			// The stone_bearer's get-up, once begun, never sinks back into the hold: it plays out in real time. If the state
			// runs on past it (a riposte turns an opening into a full down), it kneels again, or stands out the rest when
			// too little is left to go down and up again.
			const double Into = Now - StoneUpStart;
			if (!bStoneStood && Into < UpLength)
			{
				Clip = Skin.FaceDownOut;
				Time = Into;
				bTimed = true;
				UpAt[I] = Time;
				break;
			}
			const double InLength = Skin.FaceDownIn >= 0 ? static_cast<double>(Skin.Sequences[Skin.FaceDownIn]->GetPlayLength()) : 0.0;
			StoneUpStart = -1e9;
			if (bStoneStood || (FMath::IsFinite(Left) && Left < InLength + UpLength))
			{
				if (!bStoneStood && Left >= kStoneStandOutLog)
				{
					UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d is up with %.2f s of its down left: it stands it out."), Fight.time(), I, Left);
				}
				bStoneStood = true;
				Clip = Skin.Idle;
				UpAt[I] = UpLength;
				break;
			}
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d is up with %.2f s of its down left: it kneels again."), Fight.time(), I, Left);
			KneelSince[I] = Now;
			UpAt[I] = 0.0;
		}
		const double Spell = Now - KneelSince[I];
		if (Skin.FaceDownOut >= 0 && FMath::IsFinite(Left) && Left < UpLength)
		{
			Clip = Skin.FaceDownOut;
			Time = UpLength - FMath::Max(Left, 0.0);
			const double InLength = Skin.FaceDownIn >= 0 ? static_cast<double>(Skin.Sequences[Skin.FaceDownIn]->GetPlayLength()) : 0.0;
			if (bStone && SkinClip[I] == Skin.FaceDownIn && Skin.FaceDownIn >= 0 && InLength > 0.0 && Spell < InLength)
			{
				// The go-down cut at progress p: the get-up starts from the same pose, (1 - p) of the way in, and plays out.
				Time = (1.0 - Spell / InLength) * UpLength;
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d's go-down is cut at %.2f of it: its get-up starts %.2f s in."), Fight.time(), I,
					Spell / InLength, Time);
			}
			bTimed = true;
			UpAt[I] = Time;
			if (bStone)
			{
				StoneUpStart = Now - Time;
			}
		}
		else if (Skin.FaceDownIn >= 0 && Spell < static_cast<double>(Skin.Sequences[Skin.FaceDownIn]->GetPlayLength()))
		{
			Clip = Skin.FaceDownIn;
			Time = Spell;
			bTimed = true;
		}
		break;
	}
	case ftcore::ArenaBrain::Attack:
	{
		// A use the stone_bearer's rules refused is cancelled silently: its blows are vetoed and it is drawn in the idle.
		int32 MoveClip = Out.move < Kind.moveCount && !(bStone && StoneBearer.UseRefused()) ? Skin.MoveClip[Out.move] : -1;
		if (MoveClip >= 0 && bStone && bStoneMoveLeft && Skin.MoveClipL[Out.move] >= 0)
		{
			MoveClip = Skin.MoveClipL[Out.move]; // the run called this blow to the left: the mirrored clip strikes there
		}
		if (MoveClip >= 0)
		{
			// The clip plays at its own rate on Core's clock for the move, entered so that its impact lands as the move's
			// startup ends: impact - startup into it, or for the stone_bearer's moves that run on (a tile move's later
			// impacts), where it had reached as the move before ended. Only a startup longer than the clip's impact (none
			// in the shipped data) still stretches the clip from its start.
			const ftcore::BeatClip& Row = (*Skin.Clips)[static_cast<size_t>(MoveClip)];
			const float Startup = Kind.moves[Out.move].attack.startup;
			double Entry = 0.0;
			float Rate = 1.f;
			if (Row.impact > 0.0)
			{
				Entry = bStone ? StoneBearer.ClipEntry(Kind, Out.move, Row.impact) : Row.impact - static_cast<double>(Startup);
				bRunsOn = bStone && Out.move < StoneBearer.GetLayout().count && StoneBearer.GetLayout().runsOn[Out.move];
				if (Entry < -ftcore::kBeatClipEntrySlack && Startup > 0.f)
				{
					Rate = FMath::Clamp(static_cast<float>(Row.impact) / Startup, kArenaMinClipRate, kArenaMaxClipRate);
				}
				Entry = FMath::Max(Entry, 0.0);
			}
			Clip = MoveClip;
			Time = Entry + (static_cast<double>(Out.stateTime) + static_cast<double>(Alpha) * StepSeconds) * Rate;
			bTimed = true;
			MoveRate = Rate;
			MoveEntry = Entry;
		}
		break;
	}
	default:
	{
		// Between moves: the rest of a get-up still owed, else a hit reaction while it lasts, else the stepping turn while
		// it turns, else the walk while it moves, else the idle (the second one once it is hurt).
		const int32 Hit = bHitHeavy[I] ? Skin.HitHeavy : Skin.HitLight;
		if (Now < UpOwedUntil[I])
		{
			Clip = Skin.FaceDownOut;
			Time = UpLength - (UpOwedUntil[I] - Now);
			bTimed = true;
			bOwedUp = true;
			if (bStone)
			{
				UpAt[I] = Time;
			}
		}
		else if (Hit >= 0 && Now - HitSince[I] < static_cast<double>(Skin.Sequences[Hit]->GetPlayLength()))
		{
			Clip = Hit;
			Time = Now - HitSince[I];
			bTimed = true;
		}
		else if (SlotTurn[I].Sign != 0)
		{
			// Its cycles on the turn's own clock, the one TurnFacing yaws the figure by (looping, so no restart fade).
			Clip = SlotTurn[I].Sign > 0 ? Skin.TurnR : Skin.TurnL;
			Time = static_cast<double>(Fight.time()) + static_cast<double>(Alpha) * StepSeconds - SlotTurn[I].Start;
			bPhased = true;
		}
		else if (bWalking[I])
		{
			Clip = Skin.Walk;
			Time = WalkPhase[I];
			bPhased = true;
		}
		else if (Skin.IdleTwo >= 0 && Kind.hp > 0 && static_cast<float>(Fight.enemyHp(static_cast<size_t>(I))) < kArenaSecondIdleHp * static_cast<float>(Kind.hp))
		{
			Clip = Skin.IdleTwo;
		}
		break;
	}
	}
	if (bStone && Skin.Stagger >= 0 && Now < StoneSplitStaggerUntil && Out.brain != ftcore::ArenaBrain::Dead
		&& Out.brain != ftcore::ArenaBrain::Staggered && !bKnees)
	{
		// The split's frame: its stagger shows at once (Core's held Stagger plays the same clip from the next step on).
		Clip = Skin.Stagger;
		Time = Now - StoneSplitStaggerSince;
		bTimed = true;
		MoveRate = 0.f;
		bOwedUp = false;
	}
	// -ArenaBossLoop: the one clip, looped from the fight's start, whatever the fight does (a check of the clips, not a way to
	// play). Its time is its own phase; the row's blend still applies to the crossfade into it.
	const bool bBossLoop = I == BossLoopSlot && BossLoopClip >= 0;
	if (bBossLoop)
	{
		Clip = BossLoopClip;
		Time = Now - BeginNow;
		bTimed = false;
		bPhased = true;
		bHoldRoot = true;
		MoveRate = 0.f;
		bOwedUp = false;
	}
	if (!bOwedUp)
	{
		UpOwedUntil[I] = -1e9; // anything else (a move, a stagger, a death) takes it from the get-up for good
	}
	// A move's start: a new move, or the same one begun again (Core's time into it went back).
	const bool bMoveStart = MoveRate > 0.f && (static_cast<int32>(Out.move) != SkinMove[I] || Out.stateTime < SkinMoveTime[I]);
	SkinMove[I] = MoveRate > 0.f ? static_cast<int32>(Out.move) : -1;
	SkinMoveTime[I] = MoveRate > 0.f ? Out.stateTime : 0.f;
	if (bMoveStart && Clip == SkinClip[I] && bRunsOn)
	{
		// The same take goes on into the move (no "plays" line: a capture's cut is at the take's start).
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: enemy %d runs on in %s for move %d from %.2f s."), I, *Skin.Sequences[Clip]->GetName(),
			static_cast<int32>(Out.move), MoveEntry);
	}
	else if (Clip != SkinClip[I] || bMoveStart)
	{
		SkinClip[I] = Clip;
		SkinSince[I] = Now;
		if (MoveRate > 0.f)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: enemy %d plays %s for move %d at rate %.2f (%.2f s) from %.2f s."), I,
				*Skin.Sequences[Clip]->GetName(), static_cast<int32>(Out.move), MoveRate, Skin.Sequences[Clip]->GetPlayLength(), MoveEntry);
		}
		else
		{
			// Every other change too, at fight time, so a capture can be cut at each clip's start.
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: enemy %d plays %s at %.3f s (%.2f s)."), I, *Skin.Sequences[Clip]->GetName(),
				Fight.time(), Skin.Sequences[Clip]->GetPlayLength());
		}
	}
	const UAnimSequence* Sequence = Skin.Sequences[Clip];
	const ftcore::BeatClip& Row = (*Skin.Clips)[static_cast<size_t>(Clip)];
	const float Length = Sequence->GetPlayLength();
	if (!bTimed && !bPhased)
	{
		Time = Now - SkinSince[I];
	}
	if (bBossLoop && Length > 0.f)
	{
		// One line a wrap, at the frame whose pose is the clip's first again (the log's frame counter finds the seam).
		const int32 Wraps = static_cast<int32>(Time / Length);
		if (Wraps > BossLoopWraps)
		{
			BossLoopWraps = Wraps;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: boss loop %s wraps (%d) at %.3f s (%.2f s)."), *BossLoopName, Wraps, Fight.time(), Length);
		}
	}
	// A loop wraps; anything else holds its last frame. The crossfade into a clip is its row's own (blend_s) when it states
	// one, else the table's; a clip the table marks additive_in (a short hit reaction) lands at once on the pose the blow finds.
	const bool bLoop = (Row.loop || bBossLoop) && !bTimed;
	const float At = bLoop && Length > 0.f ? FMath::Fmod(static_cast<float>(Time), Length) : FMath::Clamp(static_cast<float>(Time), 0.f, Length);
	const double Blend = Row.blend > 0.0 ? Row.blend : Skin.Clips->blend;
	Figure->PlayClip(Sequence, At, bLoop, FrameDelta, bHoldRoot, static_cast<float>(Blend), Row.additiveIn);
}

void UFTArenaRunner::PutBodiesAtStart()
{
	Bodies = ftcore::ArenaBodies();
	Bodies.player = Fight.playerStart();
	for (size_t I = 0; I < Fight.enemyCount(); ++I)
	{
		Bodies.enemies[I] = Fight.enemyStart(I);
	}
	PrevBodies = Bodies;
	Accumulator = 0.0;
	Fight.tick(0.f, ftcore::ArenaInput(), Bodies);
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		ActiveSince[I] = -1e9;
		EnemyHitTime[I] = -1e9;
		LastPart[I] = 0;
	}
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		SkinClip[I] = -2;
		SkinSince[I] = -1e9;
		SkinMove[I] = -1;
		SkinMoveTime[I] = 0.f;
		HitSince[I] = -1e9;
		WalkSpeed[I] = 0.f;
		bWalking[I] = false;
		WalkPhase[I] = 0.0;
		bKneeling[I] = false;
		KneelSince[I] = 0.0;
		UpAt[I] = 0.0;
		UpOwedUntil[I] = -1e9;
		RecoilPending[I] = -1;
		RecoilSince[I] = 0.0;
		bRecoilDown[I] = false;
	}
	BossLoopWraps = 0;
	StoneWalkSince = -1e9;
	StoneUpStart = -1e9;
	StonePilotHeavyAt = -1e9;
	bStoneStood = false;
	StoneDeathClip = -1;
	StoneSplitStaggerSince = -1e9;
	StoneSplitStaggerUntil = -1e9;
	StoneTellMove = 255;
	StoneWantApproach = StoneWantStrafe = StoneWantRuns = StoneWalksBegun = 0;
	StoneHeldCone = StoneHeldUp = StoneHeldHit = StoneHeldTurn = StoneHeldPick = 0;
	StoneWalkStopAt = -1.0;
	StoneWalkStopWhy = TEXT("");
	bStoneWanted = false;
	for (FFTArenaTurn& Turn : SlotTurn)
	{
		Turn = FFTArenaTurn();
	}
	bStoneMoveLeft = false;
	PlayerHitTime = -1e9;
	ParryTime = -1e9;
	ReleaseTime = -1e9;
	ReleaseSpent = 0;
	OverloadTime = -1e9;
	bPressLight = bPressHeavy = bPressDodge = bPressHeal = bPressLock = bPressRelease = bPressPair = bPartnerDue = false;
	PressSwitch = 0;
	PressRead = -1;
	CameraYaw = ArenaYaw(Fight.playerStartFacing());
	CameraPitch = -16.f;
}

void UFTArenaRunner::PollInput(float DeltaSeconds)
{
	APlayerController* PC = GetPC();
	if (!PC || bAutoPlay || bIdle || UFTPauseSubsystem::IsBlocking(this) || UFTPauseSubsystem::IsJustResumed(this))
	{
		bGuardHeld = false;
		bInterceptHeld = false;
		Stick = ftcore::Vec2();
		return;
	}
	using namespace FTActionButtons;
	const int32 AttackB = static_cast<int32>(ftcore::ActionButton::Attack);
	const int32 GuardB = static_cast<int32>(ftcore::ActionButton::Guard);
	const int32 DodgeB = static_cast<int32>(ftcore::ActionButton::Dodge);
	const int32 HeavyB = static_cast<int32>(ftcore::ActionButton::Jump);

	bPressLight |= ArenaAnyJustPressed(*PC, Key(AttackB, 0), Key(AttackB, 1), EKeys::LeftMouseButton, EKeys::Gamepad_RightShoulder);
	bPressHeavy |= ArenaAnyJustPressed(*PC, Key(HeavyB, 0), Key(HeavyB, 1), EKeys::Gamepad_RightTrigger);
	bPressDodge |= ArenaAnyJustPressed(*PC, Key(DodgeB, 0), Key(DodgeB, 1), EKeys::SpaceBar);
	// The partner's Read: 1 / 2 / 3, or on a pad the d-pad's left, up and right while the cards are up, picked or
	// not, until the move goes active (they switch the target and heal at any other time; the d-pad's down stays
	// the partner's pair). Core refuses a second pick, so after one those three do nothing until the cards go.
	const bool bCardsOpen = Fight.readCards().dealt();
	if (PC->WasInputKeyJustPressed(EKeys::One) || (bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Left)))
	{
		PressRead = 0;
	}
	else if (PC->WasInputKeyJustPressed(EKeys::Two) || (bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Up)))
	{
		PressRead = 1;
	}
	else if (PC->WasInputKeyJustPressed(EKeys::Three) || (bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Right)))
	{
		PressRead = 2;
	}
	bPressHeal |= PC->WasInputKeyJustPressed(EKeys::H) || (!bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Up));
	bPressLock |= ArenaAnyJustPressed(*PC, EKeys::Tab, EKeys::MiddleMouseButton, EKeys::Gamepad_RightThumbstick);
	bPressRelease |= ArenaAnyJustPressed(*PC, EKeys::R, EKeys::Gamepad_LeftTrigger);
	bPressPair |= ArenaAnyJustPressed(*PC, EKeys::F, EKeys::Gamepad_DPad_Down);
	bGuardHeld = PC->IsInputKeyDown(Key(GuardB, 0)) || PC->IsInputKeyDown(Key(GuardB, 1)) || PC->IsInputKeyDown(EKeys::RightMouseButton)
		|| PC->IsInputKeyDown(EKeys::Gamepad_LeftShoulder);
	// A called-blow boss's Intercept (its Guard is the guard above): U, the mouse's thumb button or the left stick's
	// click, held.
	bInterceptHeld = PC->IsInputKeyDown(EKeys::U) || PC->IsInputKeyDown(EKeys::ThumbMouseButton)
		|| PC->IsInputKeyDown(EKeys::Gamepad_LeftThumbstick);
	if (PC->WasInputKeyJustPressed(EKeys::Q) || (!bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Left)))
	{
		PressSwitch = -1;
	}
	if (PC->WasInputKeyJustPressed(EKeys::E) || (!bCardsOpen && PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Right)))
	{
		PressSwitch = 1;
	}

	// The left stick or WASD: forward and right, at most 1 long.
	float Forward = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY);
	float Right = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX);
	Forward += (PC->IsInputKeyDown(EKeys::W) ? 1.f : 0.f) - (PC->IsInputKeyDown(EKeys::S) ? 1.f : 0.f);
	Right += (PC->IsInputKeyDown(EKeys::D) ? 1.f : 0.f) - (PC->IsInputKeyDown(EKeys::A) ? 1.f : 0.f);
	const float Length = FMath::Sqrt(Forward * Forward + Right * Right);
	if (Length > 1.f)
	{
		Forward /= Length;
		Right /= Length;
	}
	else if (Length < 0.15f)
	{
		Forward = Right = 0.f;
	}
	const float Yaw = FMath::DegreesToRadians(CameraYaw);
	const float C = FMath::Cos(Yaw);
	const float S = FMath::Sin(Yaw);
	Stick = ftcore::Vec2{C * Forward - S * Right, S * Forward + C * Right};

	// The view: the mouse and the right stick turn the camera unless something is locked; locked, a flick of
	// the right stick switches the target.
	const float LookX = PC->GetInputAnalogKeyState(EKeys::Gamepad_RightX);
	const float LookY = PC->GetInputAnalogKeyState(EKeys::Gamepad_RightY);
	if (Fight.lock() != ftcore::kArenaNobody)
	{
		if (!bStickFlicked && FMath::Abs(LookX) > kArenaFlickOut)
		{
			bStickFlicked = true;
			PressSwitch = LookX > 0.f ? 1 : -1;
		}
		else if (bStickFlicked && FMath::Abs(LookX) < kArenaFlickRest)
		{
			bStickFlicked = false;
		}
	}
	else
	{
		float MouseX = 0.f;
		float MouseY = 0.f;
		PC->GetInputMouseDelta(MouseX, MouseY);
		CameraYaw += MouseX * kArenaMouseTurn + LookX * kArenaStickTurn * DeltaSeconds;
		CameraPitch = FMath::Clamp(CameraPitch + MouseY * kArenaMouseTurn + LookY * kArenaStickTurn * 0.6f * DeltaSeconds,
			kArenaPitchMin, kArenaPitchMax);
		bStickFlicked = false;
	}
}

ftcore::ArenaInput UFTArenaRunner::TakeInput()
{
	ftcore::ArenaInput In;
	In.move = Stick;
	const float Yaw = FMath::DegreesToRadians(CameraYaw);
	In.view = ftcore::Vec2{FMath::Cos(Yaw), FMath::Sin(Yaw)};
	In.light = bPressLight;
	In.heavy = bPressHeavy;
	In.dodge = bPressDodge;
	In.heal = bPressHeal;
	In.guard = bGuardHeld;
	In.lock = bPressLock;
	In.lockSwitch = PressSwitch;
	In.release = bPressRelease;
	In.pair = bPressPair;
	In.read = PressRead;
	// Each press goes to one step.
	bPressLight = bPressHeavy = bPressDodge = bPressHeal = bPressLock = bPressRelease = bPressPair = false;
	PressSwitch = 0;
	PressRead = -1;
	return In;
}

void UFTArenaRunner::StepFight()
{
	ftcore::ArenaInput In;
	bool bIntercept = false;
	if (bAutoPlay)
	{
		In = Fight.autopilot();
		// The partner's press, one step after the parry (well inside the pair window).
		In.pair = bPartnerDue;
		bPartnerDue = false;
		if (ParryHold > 0.f && Now - ParryTime < static_cast<double>(ParryHold))
		{
			// -ArenaParryHold: no follow-up for a while after a parry, the partner's press included, so the open boss's own
			// clips show (a check of the clips, not a way to play).
			In.light = In.heavy = In.release = In.pair = false;
		}
		if (bHoldLoad && !Fight.overloaded())
		{
			In.release = false;
		}
		// The pilot's heavy (stone_bearer only), pressed before Assist so its holds (the weak point's gate, the plinth)
		// can still take it back; the cooldown runs only from a press that stood.
		bool bPilotHeavy = false;
		float PilotGap = 0.f;
		if (StoneBearer.IsActive() && StoneBearerSlot >= 0 && StoneBearerSlot < MaxEnemies && !In.release && !In.dodge
			&& Fight.time() >= StonePilotHeavyAt + kStonePilotHeavyEvery)
		{
			const int32 I = StoneBearerSlot;
			const size_t S = static_cast<size_t>(I);
			const ftcore::ArenaEnemyOut& E = Fight.output().enemies[S];
			const bool bBetween = (E.brain == ftcore::ArenaBrain::Approach || E.brain == ftcore::ArenaBrain::Strafe)
				&& !bKneeling[I] && Now >= UpOwedUntil[I];
			const ftcore::Vec2 To{Bodies.enemies[S].x - Bodies.player.x, Bodies.enemies[S].y - Bodies.player.y};
			PilotGap = ArenaLength(To);
			const float Reach = Fight.def().fighter.heavy.range + Fight.def().enemies[S].bodyRadius;
			if (bBetween && Fight.enemyAlive(S) && PilotGap <= Reach)
			{
				In.heavy = true;
				In.light = false;
				bPilotHeavy = true;
			}
		}
		// The boss's own buttons, which the autopilot knows nothing of.
		StoneBearer.Assist(In, bIntercept, Bodies);
		if (bPilotHeavy && In.heavy)
		{
			StonePilotHeavyAt = Fight.time();
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: the pilot presses heavy on enemy %d (%.2f m)."), Fight.time(),
				StoneBearerSlot, PilotGap);
		}
	}
	else if (!bIdle)
	{
		In = TakeInput();
		bIntercept = bInterceptHeld;
	}
	const bool bBoss = StoneBearer.IsActive();
	if (bBoss)
	{
		StoneBearer.Buttons(bIntercept, In.guard, Bodies);
		StoneBearer.BeforeTick();
		LogStoneMoveMask();
	}
	TurnsBeforeTick(bBoss);
	PrevBodies = Bodies;
	const ftcore::ArenaOutput& Out = Fight.tick(StepSeconds, In, Bodies);
	ftcore::arenaStep(Fight, Bodies, StepSeconds, Fit.zone);
	if (bBoss && StoneBearer.AfterTick(Out.events, Bodies))
	{
		// Onto its plinth: put there, not walked there.
		PrevBodies.enemies[StoneBearer.GetSlot()] = Bodies.enemies[StoneBearer.GetSlot()];
	}
	if (bBoss)
	{
		LogStoneMoveMask(); // a blow inside the tick can change it
		StoneBearerWalk(Out);
	}
	StepTurns(Out, bBoss);
	if (Kite.IsActive())
	{
		Kite.AfterTick(Out.events);
		KiteFx.Feed(Out.events, Kite, Fight, ToWorld(Bodies.enemies[static_cast<size_t>(KiteSlot)]),
			PlayerFigure ? PlayerFigure->GetChest() : ToWorld(Bodies.player));
	}
	if (bKindCritHook)
	{
		FeedKindCrits(Out.events);
	}
	HandleEvents(Out.events);
	TrackParts(Out);
	if (bBoss)
	{
		LogStoneBearerNotes();
	}
	if (Kite.IsActive())
	{
		LogKiteNotes();
	}
	// The open weak point's glow follows the boss's state, not its notes, so a missed note can't leave it on.
	// Off on the boss's death and the fight's end (the last step runs this before the fight stops stepping).
	const bool bGlow = bBoss && StoneBearer.CritOpen() && !Fight.over() && Fight.enemyAlive(StoneBearer.GetSlot());
	if (bGlow != BossVfx.IsGlowing())
	{
		BossVfx.SetGlow(bGlow, bGlow ? BossFlashPoint() : FVector::ZeroVector);
	}
}

void UFTArenaRunner::HandleEvents(const ftcore::ArenaEvents& Events)
{
	for (const ftcore::ArenaEvent& E : Events)
	{
		switch (E.type)
		{
		case ftcore::ArenaEventType::Hit:
			if (E.target == ftcore::kArenaPlayer)
			{
				PlayerHitTime = Now;
			}
			else if (E.target < MaxEnemies)
			{
				// A heavy blow, a riposte or a Release is a heavy reaction (and its chips are the heavy ones).
				const bool bHeavy = E.move == ftcore::kArenaHeavyMove || E.move == ftcore::kArenaRiposteMove || E.move == ftcore::kArenaReleaseMove;
				BossHitVfx(E, bHeavy);
				EnemyHitTime[E.target] = Now;
				const double HitWas = HitSince[E.target];
				const bool bFlinches = Flinches(E.target);
				StartFlinch(E.target, bHeavy);
				if (bHeavy && static_cast<int32>(E.target) == StoneBearerSlot)
				{
					// Logged for the heavy reaction checks: whether the heavy reaction is what shows, else what holds it.
					const ftcore::ArenaBrain Brain = Fight.output().enemies[E.target].brain;
					const TCHAR* Shows = !bFlinches ? TEXT("no: on its knees or getting up")
						: HitSince[E.target] == HitWas ? TEXT("no: a reaction is playing")
						: Brain == ftcore::ArenaBrain::Attack ? TEXT("no: its move's clip plays")
						: Brain == ftcore::ArenaBrain::Staggered ? TEXT("no: it staggers")
						: Brain == ftcore::ArenaBrain::Dead ? TEXT("no: it dies")
						: (Brain == ftcore::ArenaBrain::Open || Brain == ftcore::ArenaBrain::Down) ? TEXT("no: it goes down")
						: TEXT("yes");
					UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d takes a heavy blow (move %d, %d hp, brain %d): heavy reaction %s."),
						Fight.time(), static_cast<int32>(E.target), static_cast<int32>(E.move), E.amount, static_cast<int32>(Brain), Shows);
				}
			}
			break;
		case ftcore::ArenaEventType::Parried:
			ParryTime = Now;
			bPartnerDue = bAutoPlay;
			// The parried move's recoil, if its skin has one (E.source is the enemy, E.move the move it was in).
			if (E.source < MaxEnemies)
			{
				const FFTArenaSkin& Parried = Skins[E.source];
				RecoilPending[E.source] = Parried.IsLoaded() && E.move < ftcore::kMaxArenaMoves ? Parried.RecoilClip[E.move] : -1;
				RecoilSince[E.source] = Now;
				bRecoilDown[E.source] = false;
			}
			break;
		case ftcore::ArenaEventType::Staggered:
			// Logged for the stagger spacing checks (tuning poise from -nullrhi runs).
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: enemy %d staggered."), Now, static_cast<int32>(E.target));
			break;
		case ftcore::ArenaEventType::CounterStagger:
			if (E.target < MaxEnemies)
			{
				BossHitVfx(E, true);
				EnemyHitTime[E.target] = Now;
				StartFlinch(E.target, true);
			}
			break;
		case ftcore::ArenaEventType::Released:
			ReleaseTime = Now;
			ReleaseSpent = E.amount;
			continue; // a banner, not a popup
		case ftcore::ArenaEventType::Overloaded:
			OverloadTime = Now;
			continue; // a banner, not a popup
		case ftcore::ArenaEventType::ReadCounter:
			if (E.target < MaxEnemies)
			{
				BossHitVfx(E, false);
				EnemyHitTime[E.target] = Now;
				StartFlinch(E.target, false);
			}
			break;
		case ftcore::ArenaEventType::AttackStarted:
			if (StoneBearerSlot >= 0 && E.source == static_cast<uint8>(StoneBearerSlot))
			{
				// Each start (a chained impact's too), and whether the figure draws it with a clip of its own. Its left-hand
				// clip is chosen here, once, for the whole move: the run's call for its blow is made as it starts.
				const FFTArenaSkin& Skin = Skins[StoneBearerSlot];
				const bool bClip = Skin.IsLoaded() && E.move < ftcore::kMaxArenaMoves && Skin.MoveClip[E.move] >= 0;
				bStoneMoveLeft = bClip && Skin.MoveClipL[E.move] >= 0 && StoneBearer.CalledSideOf(E.move) == ftcore::CalledZone::Left;
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: boss move %d starts (clip %s)%s."), Fight.time(), static_cast<int32>(E.move),
					bClip ? TEXT("yes") : TEXT("no"), bStoneMoveLeft ? TEXT(", its left-hand clip") : TEXT(""));
			}
			continue; // no popup
		case ftcore::ArenaEventType::CardsDealt: // the cards themselves are the display
		case ftcore::ArenaEventType::LockChanged:
		case ftcore::ArenaEventType::Victory:
		case ftcore::ArenaEventType::Defeat:
			continue; // no popup
		default:
			break;
		}
		FPopup& P = Popups[PopupTotal % MaxPopups];
		P.Type = E.type;
		P.Source = E.source;
		P.Target = E.target;
		P.Move = E.move;
		P.Amount = E.amount;
		P.At = ToWorld(E.at);
		P.Time = Now;
		++PopupTotal;
	}
}

void UFTArenaRunner::TrackParts(const ftcore::ArenaOutput& Out)
{
	for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()) && I < MaxEnemies; ++I)
	{
		const ftcore::ArenaEnemyOut& E = Out.enemies[static_cast<size_t>(I)];
		const uint8 Part = E.brain == ftcore::ArenaBrain::Attack ? static_cast<uint8>(E.part) : 0;
		if (Part != LastPart[I])
		{
			if (Part == static_cast<uint8>(ftcore::ArenaPart::Active))
			{
				ActiveSince[I] = Now;
			}
			else if (Part == static_cast<uint8>(ftcore::ArenaPart::Startup))
			{
				ActiveSince[I] = -1e9;
			}
			if (I == StoneBearerSlot)
			{
				BossPartChanged(E, LastPart[I], Part);
			}
			LastPart[I] = Part;
		}
	}
}

void UFTArenaRunner::BossPartChanged(const ftcore::ArenaEnemyOut& Out, uint8 From, uint8 To)
{
	constexpr uint8 Startup = static_cast<uint8>(ftcore::ArenaPart::Startup);
	constexpr uint8 Active = static_cast<uint8>(ftcore::ArenaPart::Active);
	if (!BossVfx.IsSpawned())
	{
		return;
	}
	if (From == Startup)
	{
		BossVfx.StopTells();
	}
	// A refused pick is cut and its blows vetoed: nothing of it is drawn.
	if ((To != Startup && To != Active) || StoneBearer.UseRefused())
	{
		return;
	}
	const FFTArenaBossVfx::FTuning& T = BossVfx.GetTuning();
	const double Lift = T.Lift * 100.0;
	AFTArenaFighter* Figure = EnemyFigures[StoneBearerSlot].Get();
	if (To == Active && StoneTellMove == Out.move)
	{
		// One strike point, literally: the dust rises where the tell was drawn (its point kept from the startup's
		// start; Core holds the facing meanwhile, ArenaFight::setEnemyFacingHeld). Logged: how far the striking part is,
		// on the ground, from the tell, and how the figure and Core's facing moved since the tell.
		// A blow in the air (its contact's "air": nothing under it to strike) bursts at its contact's height over the tell.
		const FVector Tell = ToWorld(StoneTellAt);
		const FVector Burst = Tell + FVector(0.0, 0.0, bStoneTellAir ? static_cast<double>(StoneTellUp) * 100.0 : Lift);
		BossVfx.Play(FFTArenaBossVfx::EEffect::Impact, Burst, StoneTellShapeMul, bStoneTellAir ? TEXT("blow lands in the air") : TEXT("blow lands"));
		FVector Part;
		const char* Socket = BossContactSocket(Out.move, bStoneMoveLeft);
		if (Socket && Figure && Figure->GetSkinPoint(FName(Socket), Part))
		{
			if (bStoneTellAir)
			{
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: boss move %d tell-to-impact %.2f m (%s, in the air: it is %.2f m up, the dust %.2f m up and %.2f m from it)."),
					Fight.time(), static_cast<int32>(Out.move), FVector::Dist2D(Part, Tell) / 100.0, ANSI_TO_TCHAR(Socket), (Part.Z - Tell.Z) / 100.0,
					static_cast<double>(StoneTellUp), FVector::Dist(Part, Burst) / 100.0);
			}
			else
			{
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: boss move %d tell-to-impact %.2f m (%s, dust 0.00 m from the tell)."), Fight.time(),
					static_cast<int32>(Out.move), FVector::Dist2D(Part, Tell) / 100.0, ANSI_TO_TCHAR(Socket));
			}
		}
		if (Figure)
		{
			const float FacingWas = FMath::RadiansToDegrees(FMath::Atan2(StoneTellFacing.y, StoneTellFacing.x));
			const float FacingNow = FMath::RadiansToDegrees(FMath::Atan2(Out.facing.y, Out.facing.x));
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: boss move %d from tell to blow: figure yaw %.1f -> %.1f deg at (%.2f, %.2f) -> (%.2f, %.2f) m (moved %.2f m); Core facing %.1f -> %.1f deg."),
				Fight.time(), static_cast<int32>(Out.move), StoneTellFigureYaw, Figure->GetActorRotation().Yaw,
				StoneTellFigurePos.X / 100.0, StoneTellFigurePos.Y / 100.0, Figure->GetActorLocation().X / 100.0, Figure->GetActorLocation().Y / 100.0,
				FVector::Dist2D(StoneTellFigurePos, Figure->GetActorLocation()) / 100.0, FacingWas, FacingNow);
		}
		StoneTellMove = 255;
		return;
	}
	FFTStoneBearerStrike S;
	if (!StoneBearer.StrikeOf(Out.move, Bodies, T.CalledAhead, T.CalledSide, T.ReachShare, S))
	{
		return;
	}
	// Every mark lands inside the ring (a lane's end can reach past it).
	S.To = Fit.zone.clamp(S.To, 0.f);
	const bool bArea = S.Shape == FFTStoneBearerStrike::EShape::Area;
	const float ShapeMul = bArea ? T.AreaScale : 1.f;
	const FVector At = ToWorld(S.To) + FVector(0.0, 0.0, Lift);
	if (To == Active)
	{
		// A blow whose startup drew no tell: its dust where it lands now (in the air: at its contact's height).
		BossVfx.Play(FFTArenaBossVfx::EEffect::Impact, S.bAir ? ToWorld(S.To) + FVector(0.0, 0.0, static_cast<double>(S.Up) * 100.0) : At, ShapeMul,
			S.bAir ? TEXT("blow lands in the air") : TEXT("blow lands"));
		StoneTellMove = 255;
		return;
	}
	StoneTellAt = S.To;
	StoneTellMove = Out.move;
	StoneTellShapeMul = ShapeMul;
	StoneTellFacing = Out.facing;
	StoneTellFigurePos = Figure ? Figure->GetActorLocation() : FVector::ZeroVector;
	StoneTellFigureYaw = Figure ? Figure->GetActorRotation().Yaw : 0.f;
	bStoneTellAir = S.bAir;
	StoneTellUp = S.Up;
	if (S.Shape != FFTStoneBearerStrike::EShape::Lane)
	{
		// All round: the area tell, its ring drawn at the area's size (its ShapeScale 1).
		if (bArea)
		{
			BossVfx.Play(FFTArenaBossVfx::EEffect::TellArea, At, 1.f, TEXT("tell all round"));
		}
		else
		{
			BossVfx.Play(FFTArenaBossVfx::EEffect::Tell, At, ShapeMul, TEXT("tell"));
		}
		return;
	}
	// A lane: its marks spaced from the boss's edge to the lane's end (one log line for them all).
	const ftcore::Vec2 Span{S.To.x - S.From.x, S.To.y - S.From.y};
	const float Length = ArenaLength(Span);
	const float Edge = FMath::Min(Fight.def().enemies[static_cast<size_t>(StoneBearerSlot)].bodyRadius, Length);
	for (int32 K = 0; K < T.LaneMarks; ++K)
	{
		const float Along = T.LaneMarks > 1 ? FMath::Lerp(Edge, Length, static_cast<float>(K) / static_cast<float>(T.LaneMarks - 1)) : Length;
		const float Share = Length > 1e-4f ? Along / Length : 0.f;
		const FVector Mark = ToWorld(ftcore::Vec2{S.From.x + Span.x * Share, S.From.y + Span.y * Share}) + FVector(0.0, 0.0, Lift);
		BossVfx.Play(FFTArenaBossVfx::EEffect::Tell, Mark, ShapeMul, K == 0 ? TEXT("tell along a lane") : nullptr);
	}
}

void UFTArenaRunner::BossHitVfx(const ftcore::ArenaEvent& E, bool bHeavy)
{
	if (StoneBearerSlot < 0 || E.target != static_cast<uint8>(StoneBearerSlot) || E.amount <= 0 || !BossVfx.IsSpawned())
	{
		return;
	}
	// On the boss's surface toward the lead, at the lead's chest height.
	const size_t Slot = static_cast<size_t>(StoneBearerSlot);
	const ftcore::Vec2& Boss = Bodies.enemies[Slot];
	ftcore::Vec2 Dir{Bodies.player.x - Boss.x, Bodies.player.y - Boss.y};
	const float Length = ArenaLength(Dir);
	if (Length > 1e-4f)
	{
		Dir = ftcore::Vec2{Dir.x / Length, Dir.y / Length};
	}
	else
	{
		Dir = Fight.output().enemies[Slot].facing;
	}
	const float Out = Fight.def().enemies[Slot].bodyRadius * BossVfx.GetTuning().Surface;
	const FFTArenaBossVfx::EEffect Chips = bHeavy ? FFTArenaBossVfx::EEffect::ChipsHeavy : FFTArenaBossVfx::EEffect::Chips;
	// On its knees the body is the clip's, not the capsule's (no root motion yet): out from the pelvis, at its height.
	FVector Pelvis;
	const AFTArenaFighter* Figure = EnemyFigures[StoneBearerSlot].Get();
	if (StoneKneelShows() && Figure && Figure->GetSkinPoint(FName(TEXT("pelvis")), Pelvis))
	{
		BossVfx.Play(Chips, Pelvis + FVector(Dir.x * Out * 100.0, Dir.y * Out * 100.0, 0.0), 1.f, bHeavy ? TEXT("heavy hit on its knees") : TEXT("hit on its knees"));
		return;
	}
	FVector At = ToWorld(ftcore::Vec2{Boss.x + Dir.x * Out, Boss.y + Dir.y * Out});
	At.Z = PlayerFigure ? PlayerFigure->GetChest().Z : At.Z + 100.0;
	BossVfx.Play(Chips, At, 1.f, bHeavy ? TEXT("heavy hit") : TEXT("hit"));
}

void UFTArenaRunner::PrepareBossCarried()
{
	AFTArenaFighter* Figure = StoneBearerSlot >= 0 ? EnemyFigures[StoneBearerSlot].Get() : nullptr;
	if (!Figure || !Figure->IsSkinned())
	{
		return;
	}
	FName HideSlot;
	TArray<FFTCarriedPiece> Pieces;
	if (ReadCarriedSplit(kStoneBearerSplit, HideSlot, Pieces) && Figure->PrepareCarried(Pieces))
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: the carried piece's %d parts wait hidden on their bones."), Pieces.Num());
	}
}

void UFTArenaRunner::SplitBossCarried(bool bStagger)
{
	AFTArenaFighter* Figure = StoneBearerSlot >= 0 ? EnemyFigures[StoneBearerSlot].Get() : nullptr;
	if (!Figure || !Figure->IsSkinned() || Figure->IsSplit())
	{
		return;
	}
	FName HideSlot;
	TArray<FFTCarriedPiece> Pieces;
	if (!ReadCarriedSplit(kStoneBearerSplit, HideSlot, Pieces) || !Figure->SplitCarried(HideSlot, Pieces))
	{
		return;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: boss %.2f: the carried piece split into %d parts on its bones."), Now, Pieces.Num());
	if (!bStagger)
	{
		return;
	}
	// On the split's own frame: the impact at the piece's bone, and the stagger (Core holds one from the next step; a
	// poise break is one already). The host says why there is none when it holds none.
	FVector Slab;
	if (BossVfx.IsSpawned() && Figure->GetSkinPoint(FName(TEXT("slab")), Slab))
	{
		BossVfx.Play(FFTArenaBossVfx::EEffect::Impact, Slab, 1.f, TEXT("split"));
	}
	const ftcore::ArenaBrain Brain = Fight.output().enemies[StoneBearer.GetSlot()].brain;
	const bool bStaggers = StoneBearer.BreakStaggered() || Brain == ftcore::ArenaBrain::Staggered;
	if (bStaggers)
	{
		StoneSplitStaggerSince = Now;
		StoneSplitStaggerUntil = Now + kStoneSplitBridge;
	}
	static const TCHAR* const Why[] = {TEXT("a poise break"), TEXT("held"), TEXT("already staggered"), TEXT("on its knees"),
		TEXT("poise frozen or on the plinth"), TEXT("hold refused")};
	const int32 Note = static_cast<int32>(StoneBearer.GetBreakStagger());
	UE_LOG(LogForgottenThird, Log, TEXT("Arena: boss %.2f: the split staggers it: %s (%s)."), Now, bStaggers ? TEXT("yes") : TEXT("no"),
		Note >= 0 && Note < static_cast<int32>(UE_ARRAY_COUNT(Why)) ? Why[Note] : TEXT("?"));
}

bool UFTArenaRunner::StoneKneelShows() const
{
	if (StoneBearerSlot < 0 || StoneBearerSlot >= MaxEnemies)
	{
		return false;
	}
	const FFTArenaSkin& Skin = Skins[StoneBearerSlot];
	const int32 Clip = SkinClip[StoneBearerSlot];
	return Skin.IsLoaded() && Clip >= 0 && (Clip == Skin.FaceDownIn || Clip == Skin.FaceDown || Clip == Skin.FaceDownOut);
}

FVector UFTArenaRunner::BossFlashPoint() const
{
	const FVector Weak = BossWeakPoint();
	const FFTArenaBossVfx::FTuning& T = BossVfx.GetTuning();
	FVector Toward = FVector::ZeroVector;
	const APlayerController* PC = GetPC();
	if (PC && PC->PlayerCameraManager)
	{
		Toward = (PC->PlayerCameraManager->GetCameraLocation() - Weak).GetSafeNormal();
	}
	return Weak + FVector(0.0, 0.0, T.FlashUp * 100.0) + Toward * (T.FlashToCamera * 100.0);
}

FVector UFTArenaRunner::BossWeakPoint() const
{
	if (StoneBearerSlot < 0 || StoneBearerSlot >= MaxEnemies)
	{
		return FVector::ZeroVector;
	}
	const size_t Slot = static_cast<size_t>(StoneBearerSlot);
	const AFTArenaFighter* Figure = EnemyFigures[StoneBearerSlot].Get();
	if (!Figure)
	{
		return ToWorld(Bodies.enemies[Slot]);
	}
	static const FName Socket(FFTStoneBearerHost::WeakPointSocket);
	FVector At;
	if (Figure->GetSkinPoint(Socket, At))
	{
		return At;
	}
	if (!bWeakPointWarned)
	{
		bWeakPointWarned = true;
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena VFX: the stone_bearer figure has no %s bone; its weak-point effects use the boss file's point."),
			UTF8_TO_TCHAR(FFTStoneBearerHost::WeakPointSocket));
	}
	// The boss file's first point: its height up the figure, out on its gate's side (degrees in the boss's frame).
	const ftcore::ArenaWeakPoints& W = StoneBearer.GetDef().boss.weakPoints;
	const float Up = W.points.count > 0 ? W.points.items[0].y : 0.5f;
	const float Rad = FMath::DegreesToRadians(W.points.count > 0 ? W.gates[0].facing : 180.f);
	const ftcore::Vec2 F = Fight.output().enemies[Slot].facing;
	const ftcore::Vec2 Right{-F.y, F.x};
	const float Out = Fight.def().enemies[Slot].bodyRadius * 100.f * BossVfx.GetTuning().WeakDepth;
	const float C = FMath::Cos(Rad);
	const float Sn = FMath::Sin(Rad);
	return Figure->GetActorLocation() + FVector((F.x * C + Right.x * Sn) * Out, (F.y * C + Right.y * Sn) * Out, Up * Figure->GetHeight());
}

const ftcore::ArenaEnemyMove* UFTArenaRunner::GetEnemyMove(int32 Index) const
{
	if (!bBegun || Index < 0 || Index >= static_cast<int32>(Fight.enemyCount()))
	{
		return nullptr;
	}
	const ftcore::ArenaEnemyOut& E = Fight.output().enemies[static_cast<size_t>(Index)];
	if (E.brain != ftcore::ArenaBrain::Attack)
	{
		return nullptr;
	}
	const ftcore::ArenaEnemyKind& Kind = Fight.def().enemies[static_cast<size_t>(Index)];
	return E.move < Kind.moveCount ? &Kind.moves[E.move] : nullptr;
}

void UFTArenaRunner::PlaceFigures(float Alpha)
{
	if (!bBegun)
	{
		return;
	}
	const ftcore::ArenaOutput& Out = Fight.output();
	const ftcore::ArenaDef& D = Fight.def();
	Alpha = FMath::Clamp(Alpha, 0.f, 1.f);

	if (PlayerFigure)
	{
		const ftcore::ArenaPlayerOut& P = Out.player;
		FFTArenaPose Pose;
		Pose.Feet = ToWorld(ArenaLerp(PrevBodies.player, Bodies.player, Alpha));
		Pose.Yaw = ArenaYaw(P.facing);
		Pose.Speed = ArenaLength(P.velocity);
		Pose.Tint = PlayerFigure->GetBaseColor();
		const ftcore::ArenaAttack* Attack = nullptr;
		switch (P.action)
		{
		case ftcore::ArenaAction::Light:
			Attack = &D.fighter.light[FMath::Min<size_t>(P.move, ftcore::kMaxLightChain - 1)];
			Pose.Swing = (P.move + 1 >= D.fighter.lightCount) ? EFTArenaSwing::Overhead : ((P.move % 2) == 0 ? EFTArenaSwing::Across : EFTArenaSwing::Backhand);
			break;
		case ftcore::ArenaAction::Heavy:
			Attack = &D.fighter.heavy;
			Pose.Swing = EFTArenaSwing::Overhead;
			break;
		case ftcore::ArenaAction::Riposte:
			Attack = &D.fighter.riposte;
			Pose.Swing = EFTArenaSwing::Thrust;
			break;
		case ftcore::ArenaAction::Release:
			Attack = &D.fighter.release;
			Pose.Swing = EFTArenaSwing::Overhead;
			Pose.Tint = FMath::Lerp(Pose.Tint, FLinearColor(1.f, 0.5f, 0.12f), 0.6f);
			break;
		case ftcore::ArenaAction::Dodge:
		{
			Pose.bDodging = true;
			const ftcore::Vec2& F = P.facing;
			const ftcore::Vec2& Dir = P.dodgeDir;
			Pose.DodgeLocal = FVector2D(Dir.x * F.x + Dir.y * F.y, Dir.y * F.x - Dir.x * F.y);
			Pose.DodgeFraction = D.fighter.dodgeTime > 0.f ? P.actionTime / D.fighter.dodgeTime : 1.f;
			break;
		}
		case ftcore::ArenaAction::Guard:
			Pose.bGuard = true;
			Pose.bParrying = P.parrying;
			break;
		case ftcore::ArenaAction::Heal:
			Pose.bHealing = true;
			Pose.Tint = FMath::Lerp(Pose.Tint, FLinearColor(0.3f, 0.95f, 0.45f), 0.5f);
			break;
		case ftcore::ArenaAction::Hitstun:
			Pose.LeanBack = 14.f;
			break;
		case ftcore::ArenaAction::GuardBreak:
			Pose.LeanBack = 22.f;
			Pose.Wobble = 6.f * FMath::Sin(static_cast<float>(Now) * 16.f);
			break;
		case ftcore::ArenaAction::Downed:
			Pose.bLying = true;
			break;
		default:
			break;
		}
		if (Attack)
		{
			Pose.bAttacking = true;
			Pose.Part = static_cast<uint8>(P.part);
			Pose.PartFraction = ArenaPartFraction(*Attack, P.part, P.actionTime);
		}
		if (P.invulnerable)
		{
			Pose.Tint = FMath::Lerp(Pose.Tint, FLinearColor(0.85f, 0.95f, 1.f), 0.45f);
		}
		Pose.Tint = FMath::Lerp(Pose.Tint, FLinearColor(1.f, 0.2f, 0.15f), ArenaModePulse(Now, PlayerHitTime, kArenaHitTint));
		PlayerFigure->ApplyPose(Pose);
	}

	for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()) && I < MaxEnemies; ++I)
	{
		AFTArenaFighter* Figure = EnemyFigures[I].Get();
		if (!Figure)
		{
			continue;
		}
		// The boss on its plinth is held Down but drawn on its feet.
		const bool bBossStands = I == StoneBearerSlot && StoneBearer.DrawnStanding();
		ftcore::ArenaEnemyOut E = Out.enemies[static_cast<size_t>(I)];
		if (bBossStands && E.brain == ftcore::ArenaBrain::Down)
		{
			E.brain = ftcore::ArenaBrain::Strafe;
		}
		const ftcore::ArenaEnemyKind& Kind = D.enemies[static_cast<size_t>(I)];
		FFTArenaPose Pose;
		Pose.Feet = ToWorld(ArenaLerp(PrevBodies.enemies[static_cast<size_t>(I)], Bodies.enemies[static_cast<size_t>(I)], Alpha));
		if (I == StoneBearerSlot && StoneBearer.OnPlinth())
		{
			Pose.Feet.Z += FFTStoneBearerHost::PlinthHeight * 100.0;
		}
		Pose.Yaw = ArenaYaw(E.facing);
		if (SlotTurn[I].Sign != 0)
		{
			// Mid-turn, the turn's own yaw at the frame's time, in step with its clip (Core's facing is the last step's).
			Pose.Yaw = ArenaYaw(TurnFacing(I, static_cast<double>(Fight.time()) + static_cast<double>(Alpha) * StepSeconds - SlotTurn[I].Start));
		}
		Pose.Speed = ArenaLength(E.velocity);
		Pose.Tint = Figure->GetBaseColor();
		switch (E.brain)
		{
		case ftcore::ArenaBrain::Attack:
			if (E.move < Kind.moveCount)
			{
				const ftcore::ArenaEnemyMove& Move = Kind.moves[E.move];
				Pose.bAttacking = true;
				Pose.Part = static_cast<uint8>(E.part);
				Pose.PartFraction = ArenaPartFraction(Move.attack, E.part, E.stateTime);
				Pose.Swing = (!Move.parryable || Move.unblockable) ? EFTArenaSwing::Overhead
					: ((E.move % 2) == 0 ? EFTArenaSwing::Across : EFTArenaSwing::Backhand);
				const float Flash = ArenaModePulse(Now, ActiveSince[I], kArenaTellFlash);
				if (E.part == ftcore::ArenaPart::Startup || Flash > 0.f)
				{
					Pose.TellReach = (Move.attack.range + D.fighter.bodyRadius) * 100.f;
					Pose.TellHalfAngle = FMath::Acos(FMath::Clamp(Move.attack.arcCos, -1.f, 1.f));
					Pose.TellWidth = Move.attack.width > 0.f ? (Move.attack.width + 2.f * D.fighter.bodyRadius) * 100.f : 0.f;
					Pose.TellFraction = E.part == ftcore::ArenaPart::Startup ? Pose.PartFraction : 1.f;
					Pose.TellFlash = E.part == ftcore::ArenaPart::Startup ? 0.f : Flash;
					Pose.bTellUnblockable = Move.unblockable;
				}
			}
			break;
		case ftcore::ArenaBrain::Staggered:
			Pose.LeanBack = 12.f;
			Pose.Wobble = 9.f * FMath::Sin(static_cast<float>(Now) * 18.f);
			break;
		case ftcore::ArenaBrain::Open:
			Pose.LeanBack = 20.f;
			// Open to a riposte: it glows.
			Pose.Tint = FMath::Lerp(Pose.Tint, kArenaOpenColor, 0.35f + 0.25f * FMath::Sin(static_cast<float>(Now) * 12.f));
			break;
		case ftcore::ArenaBrain::Down:
			Pose.bLying = true;
			break;
		case ftcore::ArenaBrain::Dead:
			Pose.bLying = true;
			Pose.Tint = kArenaDeadColor;
			break;
		default:
			break;
		}
		if (E.brain != ftcore::ArenaBrain::Dead)
		{
			Pose.Tint = FMath::Lerp(Pose.Tint, kArenaHitColor, ArenaModePulse(Now, EnemyHitTime[I], kArenaHitTint));
		}
		Figure->ApplyPose(Pose);
		DriveSkin(I, E, Kind, Alpha);
	}
	if (BossVfx.IsGlowing())
	{
		BossVfx.MoveGlow(BossFlashPoint());
	}
}

void UFTArenaRunner::SteerCamera(float DeltaSeconds)
{
	APlayerController* PC = GetPC();
	if (!PC || !bBegun)
	{
		return;
	}
	// Locked: turn to look from the player at the target. The autopilot and an idle run with nothing locked
	// look at the nearest living enemy, so the fight stays in frame.
	int32 Target = Fight.lock() != ftcore::kArenaNobody ? static_cast<int32>(Fight.lock()) : -1;
	if (Target < 0 && (bAutoPlay || bIdle))
	{
		float Best = 1e9f;
		for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()); ++I)
		{
			if (Fight.enemyAlive(static_cast<size_t>(I)))
			{
				const ftcore::Vec2& E = Bodies.enemies[static_cast<size_t>(I)];
				const float Dist = ArenaLength(ftcore::Vec2{E.x - Bodies.player.x, E.y - Bodies.player.y});
				if (Dist < Best)
				{
					Best = Dist;
					Target = I;
				}
			}
		}
	}
	if (Target >= 0 && Target < static_cast<int32>(Fight.enemyCount()))
	{
		const ftcore::Vec2& E = Bodies.enemies[static_cast<size_t>(Target)];
		const ftcore::Vec2 To{E.x - Bodies.player.x, E.y - Bodies.player.y};
		if (ArenaLength(To) > 0.3f)
		{
			const float Want = ArenaYaw(To);
			const float Delta = FMath::FindDeltaAngleDegrees(CameraYaw, Want);
			CameraYaw += Delta * FMath::Min(1.f, kArenaLockTurn * DeltaSeconds);
		}
		// A tall boss (a hanging one, hover included) is looked up at: the pitch that sends the view through its face from
		// where the arm puts the camera (the pivot and shoulder height over the feet, the boom swung back along the pitch),
		// no higher than the free camera may look.
		float WantPitch = Fit.camera.lockPitch;
		const ftcore::ArenaEnemyKind& Foe = Fight.def().enemies[static_cast<size_t>(Target)];
		const float Hover = Foe.look.skinned() ? Foe.look.hover : 0.f;
		const float Tall = ftcore::arenaEnemyHeight(Foe) * (Foe.look.skinned() ? Foe.look.scale : 1.f) + Hover;
		if (Tall >= kArenaTallBoss)
		{
			const ftcore::ZoneCamera& Rig = Fit.camera;
			const float Away = ArenaLength(To);
			float Pitch = Rig.lockPitch;
			for (int32 Pass = 0; Pass < 3; ++Pass)
			{
				const float Rad = FMath::DegreesToRadians(Pitch);
				const float CameraHeight = Rig.pivot + Rig.up - Rig.arm * FMath::Sin(Rad);
				const float Along = Rig.arm * FMath::Cos(Rad) + Away;
				Pitch = FMath::RadiansToDegrees(FMath::Atan2(Tall * kArenaFaceHeight - CameraHeight, Along));
			}
			WantPitch = FMath::Clamp(Pitch, Rig.lockPitch, kArenaPitchMax);
		}
		CameraPitch = FMath::FInterpTo(CameraPitch, WantPitch, DeltaSeconds, 4.f);
	}
	CameraYaw = FMath::UnwindDegrees(CameraYaw);
	PC->SetControlRotation(FRotator(CameraPitch, CameraYaw, 0.0));
}

void UFTArenaRunner::Finish()
{
	bResultLogged = true;
	ResultTime = Now;
	const bool bWon = Fight.phase() == ftcore::ArenaPhase::Victory;
	LogStoneWalkCounts();
	LogTurnCounts();
	LogResult();
	if (bQuitOnFinish && !bShots)
	{
		FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(bWon ? 0 : 1), TEXT("Arena"));
	}
	// The autopilot's wait before it fights again: long enough for the bosses' defeat clips (all dead on a victory) to play out.
	// The banner waits for what is left of the death clips on screen now (the death started this step, or a fall still
	// plays out onto the knees first).
	float LongestDefeat = 0.f;
	ResultWait = 0.0;
	if (bWon)
	{
		for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()) && I < MaxEnemies; ++I)
		{
			const FFTArenaSkin& Skin = Skins[I];
			if (!Skin.IsLoaded())
			{
				continue;
			}
			for (const int32 Clip : {Skin.Defeat, Skin.DefeatKneel})
			{
				if (Clip >= 0)
				{
					LongestDefeat = FMath::Max(LongestDefeat, Skin.Sequences[Clip]->GetPlayLength());
				}
			}
			const int32 Shown = SkinClip[I];
			double Left = 0.0;
			if (Shown >= 0 && (Shown == Skin.Defeat || Shown == Skin.DefeatKneel))
			{
				Left = static_cast<double>(Skin.Sequences[Shown]->GetPlayLength()) - (Now - SkinSince[I]);
			}
			else if (Shown >= 0 && Shown == Skin.FaceDownIn && Skin.DefeatKneel >= 0 && !Fight.enemyAlive(static_cast<size_t>(I)))
			{
				Left = static_cast<double>(Skin.Sequences[Shown]->GetPlayLength()) - (Now - KneelSince[I])
					+ static_cast<double>(Skin.Sequences[Skin.DefeatKneel]->GetPlayLength());
			}
			ResultWait = FMath::Max(ResultWait, Left);
		}
	}
	AutoAgainWait = FMath::Max(FMath::Max(kArenaAutoAgain, static_cast<double>(LongestDefeat) + kArenaDefeatTail), ResultWait + kArenaResultShown);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: the result shows %.2f s after it (the death clips on screen play out first)."), ResultWait);
	if (bAutoPlay && bFightAgain && !bShots && !bQuitOnFinish)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: autoplay fights again %.2f s after the result (longest defeat clip %.2f s)."), AutoAgainWait,
			LongestDefeat);
	}
}

void UFTArenaRunner::LogResult() const
{
	// Ids and counts only, never text (Core's arenaResultLine). The scene, beat and step are the story step's
	// when one carried the fight here, else "standalone".
	const UFTGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance<UFTGameInstance>() : nullptr;
	const bool bCarried = GI && GI->StoryCarry.bValid && GI->StoryCarry.Combat == ftcore::CombatKind::Arena;
	const std::string Scene = bCarried ? FT::ToUtf8(GI->StoryCarry.Scene) : std::string("standalone");
	const std::string Kit = FT::ToUtf8(KitId);
	const std::string Pilot = FT::ToUtf8(ArenaPilotName(bAutoPlay, bIdle));
	ftcore::ArenaLogIds Ids;
	Ids.scene = Scene;
	Ids.beat = bCarried ? static_cast<uint32>(FMath::Max(0, GI->StoryCarry.Beat)) : 0u;
	Ids.step = bCarried ? static_cast<uint32>(FMath::Max(0, GI->StoryCarry.Step)) : 0u;
	Ids.zone = ZoneId;
	Ids.kit = Kit;
	Ids.pilot = Pilot;
	Ids.seed = Seed;
	char Line[ftcore::kArenaResultLineMax];
	ftcore::arenaResultLine(Line, sizeof Line, Fight, Table, Ids);
	if (Fight.over())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s"), UTF8_TO_TCHAR(Line));
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s"), UTF8_TO_TCHAR(Line));
	}
}

void UFTArenaRunner::Restart()
{
	Fight.restart();
	BeginStoneBearer();
	// The boss is whole again (its carried piece too) unless the run asks for it split.
	if (AFTArenaFighter* Boss = StoneBearerSlot >= 0 ? EnemyFigures[StoneBearerSlot].Get() : nullptr)
	{
		Boss->MendCarried();
	}
	if (bBossSplitAtStart)
	{
		SplitBossCarried(false);
	}
	BeginKite();
	BeginKindCrits();
	bResultLogged = false;
	PutBodiesAtStart();
	BeginNow = Now;
	BeginBossLoop();
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: fight again."));
}

void UFTArenaRunner::PrepareStoneBearer()
{
	StoneBearerSlot = -1;
	for (size_t I = 0; I < static_cast<size_t>(Def.enemyCount); ++I)
	{
		const size_t KindIndex = Def.enemyKind[I];
		if (KindIndex < Table.enemyIds.size() && Table.enemyIds[KindIndex] == FFTStoneBearerHost::KindId)
		{
			StoneBearerSlot = static_cast<int32>(I);
			break;
		}
	}
	if (StoneBearerSlot < 0)
	{
		return;
	}
	if (!StoneBearer.IsLoaded())
	{
		TArray<uint8> Bytes;
		const FString File = FPaths::ProjectDir() / UTF8_TO_TCHAR(FFTStoneBearerHost::DataPath);
		std::string LoadError;
		std::vector<std::string> Warnings;
		if (!FFileHelper::LoadFileToArray(Bytes, *File))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: couldn't read %s; stone_bearer fights as its arena.json kind."), UTF8_TO_TCHAR(FFTStoneBearerHost::DataPath));
		}
		else if (!StoneBearer.Load(FT::BytesAsText(Bytes), &LoadError, &Warnings))
		{
			// The loader's errors name paths and keys only.
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s refused; stone_bearer fights as its arena.json kind:\n%s"), UTF8_TO_TCHAR(FFTStoneBearerHost::DataPath),
				*FT::ToFString(LoadError));
		}
		for (const std::string& W : Warnings)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: stone_bearer: %s"), *FT::ToFString(W));
		}
	}
	std::string Error;
	if (!StoneBearer.IsLoaded() || !StoneBearer.BuildKind(Def.enemies[static_cast<size_t>(StoneBearerSlot)], Def.fighter, &Error))
	{
		if (!Error.empty())
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s"), *FT::ToFString(Error));
		}
		StoneBearerSlot = -1;
	}
}

void UFTArenaRunner::BeginStoneBearer()
{
	if (StoneBearerSlot < 0)
	{
		StoneBearer.End();
		BossVfx.Destroy();
		return;
	}
	StoneBearer.Begin(Fight, static_cast<size_t>(StoneBearerSlot), Fit.zone.centre, ZoneYaw);
	StoneBearerLogged = StoneBearer.GetNoteTotal();
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: stone_bearer in slot %d: %d arena moves from %d boss moves."), StoneBearerSlot,
		static_cast<int32>(Fight.def().enemies[static_cast<size_t>(StoneBearerSlot)].moveCount), static_cast<int32>(StoneBearer.GetDef().boss.moveCount));
	// Each boss move's strike point: its clip's contact, or the file's reach when the contact is inside the body.
	const float BossRadius = Fight.def().enemies[static_cast<size_t>(StoneBearerSlot)].bodyRadius;
	const ftcore::ArenaBossDef& BossDef = StoneBearer.GetDef().boss;
	for (std::uint8_t M = 0; M < BossDef.moveCount; ++M)
	{
		const ftcore::ArenaBossMove& Move = BossDef.moves[M];
		if (!Move.hasContact)
		{
			continue;
		}
		const bool bUsed = FFTStoneBearerHost::UsableContact(Move, BossRadius);
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: stone_bearer boss move %d: contact %s at %.2f m from the root (%.2f ahead, %.2f right): %s."),
			static_cast<int32>(M), ANSI_TO_TCHAR(Move.contactSocketChars.data()), FFTStoneBearerHost::ContactPlanar(Move), Move.contactAhead,
			Move.contactSide, bUsed ? TEXT("its strike point and reach") : TEXT("inside the body or a lane or tile move, the file's reach stays"));
	}

	// Its effects: the pool spawned once, everything stopped at each (re)start.
	BossVfx.StopAll();
	UWorld* World = GetWorld();
	if (World && !BossVfx.IsSpawned())
	{
		BossVfx.Spawn(World, kStoneBearerVfx);
	}

	// Its plinth at the zone's centre and the guest's keep-out disc: markers, nothing to bump into (the plinth is the
	// host's: arenas.json's cover blocks).
	if (!World || PlinthActor)
	{
		return;
	}
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Cylinder || !Base)
	{
		return;
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const auto Place = [&](const ftcore::Vec2& At, float Radius, float Height, const FLinearColor& Color) -> AStaticMeshActor*
	{
		AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(ToWorld(At) + FVector(0.0, 0.0, Height * 50.0), FRotator::ZeroRotator, Spawn);
		if (!Prop)
		{
			return nullptr;
		}
		UStaticMeshComponent* Component = Prop->GetStaticMeshComponent();
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Cylinder);
		UMaterialInstanceDynamic* Paint = UMaterialInstanceDynamic::Create(Base, World);
		Paint->SetVectorParameterValue(TEXT("Color"), Color);
		Component->SetMaterial(0, Paint);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
		Prop->SetActorScale3D(FVector(Radius * 2.0, Radius * 2.0, Height)); // the cylinder is 1 m across and 1 m tall
		return Prop;
	};
	const float BodyRadius = Fight.def().enemies[static_cast<size_t>(StoneBearerSlot)].bodyRadius;
	PlinthActor = Place(StoneBearer.GetPlinthSpot(), BodyRadius + 0.4f, FFTStoneBearerHost::PlinthHeight, FLinearColor(0.3f, 0.29f, 0.27f));
	const ftcore::ArenaKeepOut& KeepOut = StoneBearer.GetKeepOut();
	if (!KeepOut.none())
	{
		KeepOutActor = Place(KeepOut.centre, KeepOut.radius, 0.03f, FLinearColor(0.75f, 0.62f, 0.25f));
	}
}

void UFTArenaRunner::EndStoneBearer()
{
	StoneBearer.End();
	StoneBearerSlot = -1;
	BossVfx.Destroy();
	if (PlinthActor)
	{
		PlinthActor->Destroy();
		PlinthActor = nullptr;
	}
	if (KeepOutActor)
	{
		KeepOutActor->Destroy();
		KeepOutActor = nullptr;
	}
}

void UFTArenaRunner::LogStoneMoveMask()
{
	if (StoneBearerSlot < 0 || static_cast<size_t>(StoneBearerSlot) >= Fight.enemyCount())
	{
		return;
	}
	const uint32 Mask = Fight.enemyMoveMask(static_cast<size_t>(StoneBearerSlot));
	if (Mask != StoneMoveMask)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: boss move mask %08x (was %08x)."), Fight.time(), Mask, StoneMoveMask);
		StoneMoveMask = Mask;
	}
}

void UFTArenaRunner::StoneBearerWalk(const ftcore::ArenaOutput& Out)
{
	if (StoneBearerSlot < 0 || StoneBearerSlot >= MaxEnemies || static_cast<size_t>(StoneBearerSlot) >= Fight.enemyCount())
	{
		return;
	}
	const int32 I = StoneBearerSlot;
	const size_t Slot = static_cast<size_t>(I);
	const FFTArenaSkin& Skin = Skins[I];
	const AFTArenaFighter* Figure = EnemyFigures[I].Get();
	if (!Figure || !Figure->IsSkinned() || !Skin.IsLoaded() || Skin.Walk < 0)
	{
		return; // the shapes' figure keeps Core's motion
	}
	const ftcore::ArenaEnemyOut& E = Out.enemies[Slot];
	const bool bBetween = E.brain == ftcore::ArenaBrain::Approach || E.brain == ftcore::ArenaBrain::Strafe;
	const bool bOwedUp = Now < UpOwedUntil[I];
	const int32 Hit = bHitHeavy[I] ? Skin.HitHeavy : Skin.HitLight;
	const bool bHitPlays = Hit >= 0 && Now - HitSince[I] < static_cast<double>(Skin.Sequences[Hit]->GetPlayLength());
	// Core's wanted ground speed and direction (its velocity this step).
	const float Wanted = ArenaLength(E.velocity);
	const ftcore::Vec2 Dir = Wanted > 1e-4f ? ftcore::Vec2{E.velocity.x / Wanted, E.velocity.y / Wanted} : E.facing;
	const float Cone = E.brain == ftcore::ArenaBrain::Approach ? kStoneApproachCone : kStoneWalkCone;
	const bool bAhead = Wanted > 1e-4f && Dir.x * E.facing.x + Dir.y * E.facing.y >= Cone;
	const bool bWas = bWalking[I];
	const double Walked = Now - StoneWalkSince;
	// One whole cycle of the clip: a walk starts only when Core can begin no move for that long. It stops at a passing pose
	// (every Cycle / kStoneWalkExits from its start) unless a move, a reaction or a get-up cuts it.
	const double Cycle = (*Skin.Clips)[static_cast<size_t>(Skin.Walk)].length;
	const double Pass = Cycle / kStoneWalkExits;
	const float PickIn = Fight.enemyPickIn(Slot);
	const bool bPickWaits = static_cast<double>(PickIn) >= Cycle;
	bool bWalk = bWas;
	const TCHAR* Why = TEXT("");
	if (!bBetween || bOwedUp || bHitPlays)
	{
		bWalk = false;
		Why = E.brain == ftcore::ArenaBrain::Attack ? TEXT("a move starts")
			: (E.brain == ftcore::ArenaBrain::Dead ? TEXT("it dies")
			: (!bBetween ? TEXT("a reaction or a hold") : (bOwedUp ? TEXT("a get-up is owed") : TEXT("a hit reaction"))));
	}
	else if (!bWas)
	{
		bWalk = Wanted > kArenaWalkStart && bAhead && SlotTurn[I].Sign == 0 && bPickWaits; // a turn under way ends at its cycle's end first
	}
	else
	{
		const bool bWantsOn = Wanted >= kArenaWalkStop && bAhead;
		if (!bWantsOn && StoneWalkStopAt < 0.0)
		{
			// Core stopped wanting it: it walks on to its next passing pose (at least one).
			StoneWalkStopAt = FMath::Max(1.0, FMath::CeilToDouble((Walked - 1e-6) / Pass)) * Pass;
			StoneWalkStopWhy = Wanted < kArenaWalkStop ? TEXT("at its passing pose (Core wanted it under the stop speed)")
				: TEXT("at its passing pose (Core's way left its cone)");
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d's walk will stop at its next passing pose, %.2f s in (%.2f s now)."), Fight.time(), I,
				StoneWalkStopAt, Walked);
		}
		else if (bWantsOn && StoneWalkStopAt >= 0.0)
		{
			StoneWalkStopAt = -1.0; // Core wants it again before the passing pose: it goes on
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d's walk goes on (Core wants it again, %.2f s in)."), Fight.time(), I, Walked);
		}
		if (StoneWalkStopAt >= 0.0 && Walked + 0.5 * FMath::Max(static_cast<double>(StepSeconds), static_cast<double>(FrameDelta)) >= StoneWalkStopAt)
		{
			bWalk = false;
			Why = StoneWalkStopWhy;
		}
	}
	// The counts for the fight's walk line: each step between moves that wanted the walk, and what held a start back.
	const bool bWants = bBetween && Wanted > kArenaWalkStart;
	if (bWants)
	{
		StoneWantApproach += E.brain == ftcore::ArenaBrain::Approach ? 1 : 0;
		StoneWantStrafe += E.brain == ftcore::ArenaBrain::Strafe ? 1 : 0;
		StoneWantRuns += bStoneWanted ? 0 : 1;
		if (!bWas && !bWalk)
		{
			StoneHeldUp += bOwedUp ? 1 : 0;
			StoneHeldHit += !bOwedUp && bHitPlays ? 1 : 0;
			StoneHeldCone += !bOwedUp && !bHitPlays && !bAhead ? 1 : 0;
			StoneHeldTurn += !bOwedUp && !bHitPlays && bAhead && SlotTurn[I].Sign != 0 ? 1 : 0;
			StoneHeldPick += !bOwedUp && !bHitPlays && bAhead && SlotTurn[I].Sign == 0 && !bPickWaits ? 1 : 0;
		}
	}
	bStoneWanted = bWants;
	const float Speed = static_cast<float>((*Skin.Clips)[static_cast<size_t>(Skin.Walk)].speed);
	if (bWalk && !bWas)
	{
		StoneWalkSince = Now;
		StoneWalkStopAt = -1.0;
		++StoneWalksBegun;
		// Logged for the walk checks in -nullrhi runs.
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d walks at %.2f m/s (rate 1.00, wanted %.2f m/s; %s %s)."), Fight.time(), I, Speed, Wanted,
			E.brain == ftcore::ArenaBrain::Approach ? TEXT("Approach") : TEXT("Strafe"),
			PickIn >= ftcore::kArenaPickNever ? TEXT("no move due") : TEXT("a move due later"));
	}
	else if (!bWalk && bWas)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d stops walking after %.2f s (rate 1.00): %s."), Fight.time(), I, Walked, Why);
		StoneWalkStopAt = -1.0;
	}
	bWalking[I] = bWalk;
	if (!bBetween)
	{
		return; // a move's lunge, a reaction or a hold moves it as Core says
	}
	// Between moves (an owed get-up too) the body moves only as the walk does: from where it stood, at the clip's speed
	// along the wanted direction (the facing past the cone), held in the ring and off the lead. Core reads it back.
	ftcore::Vec2 P = PrevBodies.enemies[Slot];
	if (bWalk)
	{
		const ftcore::Vec2 Along = bAhead ? Dir : E.facing;
		P.x += Along.x * Speed * StepSeconds;
		P.y += Along.y * Speed * StepSeconds;
	}
	const float Radius = Fight.def().enemies[Slot].bodyRadius;
	P = Fit.zone.constrain(P, Radius);
	const ftcore::Vec2 Off{P.x - Bodies.player.x, P.y - Bodies.player.y};
	const float Gap = ArenaLength(Off);
	const float Least = Radius + Fight.def().fighter.bodyRadius;
	if (Gap < Least && Gap > 1e-4f)
	{
		P = ftcore::Vec2{Bodies.player.x + Off.x * (Least / Gap), Bodies.player.y + Off.y * (Least / Gap)};
	}
	Bodies.enemies[Slot] = P;
}

void UFTArenaRunner::LogStoneWalkCounts() const
{
	if (StoneBearerSlot < 0 || StoneBearerSlot >= MaxEnemies || !Skins[StoneBearerSlot].IsLoaded() || Skins[StoneBearerSlot].Walk < 0)
	{
		return;
	}
	UE_LOG(LogForgottenThird, Log, TEXT("Arena: stone_bearer walk: wanted over %.2f m/s on %d steps (Approach %d, Strafe %d) in %d runs; %d walks begun; starts held by the cone %d, a get-up %d, a hit %d, a move due within a cycle %d steps."),
		kArenaWalkStart, StoneWantApproach + StoneWantStrafe, StoneWantApproach, StoneWantStrafe, StoneWantRuns, StoneWalksBegun,
		StoneHeldCone, StoneHeldUp, StoneHeldHit, StoneHeldPick);
	if (Skins[StoneBearerSlot].TurnL >= 0)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: stone_bearer turns: %d begun, %d steps, %d cancelled; starts held by a move due within a step %d steps; walk starts held by a turn %d steps."),
			SlotTurn[StoneBearerSlot].Begun, SlotTurn[StoneBearerSlot].Steps, SlotTurn[StoneBearerSlot].Cancelled,
			SlotTurn[StoneBearerSlot].HeldPick, StoneHeldTurn);
	}
}

bool UFTArenaRunner::CanTurn(int32 I, bool bHost) const
{
	if (I < 0 || I >= MaxEnemies || static_cast<size_t>(I) >= Fight.enemyCount() || (I == StoneBearerSlot && !bHost))
	{
		return false;
	}
	const FFTArenaSkin& Skin = Skins[I];
	const AFTArenaFighter* Figure = EnemyFigures[I].Get();
	return Figure && Figure->IsSkinned() && Skin.IsLoaded() && Skin.TurnL >= 0 && Skin.TurnR >= 0;
}

ftcore::Vec2 UFTArenaRunner::TurnFacing(int32 I, double T) const
{
	// Its whole cycles, then the smootherstep of the one under way (held once it has settled), never past the cycles it is
	// committed to; scaled while its clip fades in by the crossfade's weight (which the anim instance raises linearly).
	const FFTArenaTurn& Turn = SlotTurn[I];
	const double At = FMath::Max(T, 0.0);
	const double K = FMath::Floor(At / kArenaTurnCycle);
	const double U = FMath::Min((At - K * kArenaTurnCycle) / kArenaTurnPivot, 1.0);
	const double Eased = U * U * U * (10.0 - 15.0 * U + 6.0 * U * U);
	const double Weight = Turn.Blend > 0.0 ? FMath::Min(1.0, (At + static_cast<double>(FrameDelta)) / Turn.Blend) : 1.0;
	const double Angle = FMath::DegreesToRadians(static_cast<double>(kArenaTurnDeg)) * FMath::Min(K + Eased, static_cast<double>(Turn.Cycles))
		* Weight * static_cast<double>(Turn.Sign);
	const double C = FMath::Cos(Angle);
	const double S = FMath::Sin(Angle);
	return ftcore::Vec2{static_cast<float>(Turn.From.x * C - Turn.From.y * S), static_cast<float>(Turn.From.x * S + Turn.From.y * C)};
}

void UFTArenaRunner::TurnsBeforeTick(bool bHost)
{
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		if (!CanTurn(I, bHost))
		{
			continue;
		}
		const size_t Slot = static_cast<size_t>(I);
		const FFTArenaTurn& Turn = SlotTurn[I];
		// Standing, only the turn turns it; walking, Core turns it as it goes. Any kind but the stone_bearer (whose walk host
		// holds its body still until it walks) is also Core's to turn while Core wanted it moving on the last step and no
		// turn is under way: Core moves that body whatever its facing, so a held facing would let it slide sideways.
		const bool bHeld = I == StoneBearerSlot ? !bWalking[I] : (Turn.Sign != 0 || (!bWalking[I] && !Turn.bMoves));
		Fight.setEnemyTurnHeld(Slot, bHeld);
		if (Turn.Sign != 0)
		{
			Fight.setEnemyFacing(Slot, TurnFacing(I, static_cast<double>(Fight.time()) + StepSeconds - Turn.Start));
		}
	}
}

void UFTArenaRunner::StepTurns(const ftcore::ArenaOutput& Out, bool bHost)
{
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		if (CanTurn(I, bHost))
		{
			StepTurn(I, Out);
		}
	}
}

void UFTArenaRunner::LogTurnCounts() const
{
	for (int32 I = 0; I < MaxEnemies && static_cast<size_t>(I) < Fight.enemyCount(); ++I)
	{
		if (I == StoneBearerSlot || !Skins[I].IsLoaded() || Skins[I].TurnL < 0)
		{
			continue;
		}
		const FFTArenaTurn& Turn = SlotTurn[I];
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: enemy %d (%s) turns: %d begun, %d steps, %d cancelled; starts held by a move due within a step %d steps."),
			I, UTF8_TO_TCHAR(SlotKindId(I)), Turn.Begun, Turn.Steps, Turn.Cancelled, Turn.HeldPick);
	}
}

void UFTArenaRunner::StepTurn(int32 I, const ftcore::ArenaOutput& Out)
{
	const size_t Slot = static_cast<size_t>(I);
	const bool bStone = I == StoneBearerSlot;
	FFTArenaTurn& Turn = SlotTurn[I];
	const FFTArenaSkin& Skin = Skins[I];
	const AFTArenaFighter* Figure = EnemyFigures[I].Get();
	const ftcore::ArenaEnemyOut& E = Out.enemies[Slot];
	const int32 Hit = bHitHeavy[I] ? Skin.HitHeavy : Skin.HitLight;
	const bool bHitPlays = Hit >= 0 && Now - HitSince[I] < static_cast<double>(Skin.Sequences[Hit]->GetPlayLength());
	const bool bBetween = E.brain == ftcore::ArenaBrain::Approach || E.brain == ftcore::ArenaBrain::Strafe;
	const bool bOwedUp = Now < UpOwedUntil[I];
	// Core's wanted ground speed this step. Any kind but the stone_bearer (whose walk host holds its body still until it
	// walks): Core wanting it moving ends a turn under way and starts none.
	const float Wanted = ArenaLength(E.velocity);
	const bool bCoreMoves = !bStone && Wanted > kArenaWalkStart;
	Turn.bMoves = bCoreMoves;
	const bool bFree = bBetween && !bWalking[I] && !bHitPlays && !bOwedUp && !(bStone && StoneBearer.OnPlinth()) && Fight.enemyAlive(Slot)
		&& !bCoreMoves;
	// How far the lead is off its facing (degrees): + to its right (the yaw rising), - to its left.
	const ftcore::Vec2 To{Bodies.player.x - Bodies.enemies[Slot].x, Bodies.player.y - Bodies.enemies[Slot].y};
	const float Off = FMath::RadiansToDegrees(FMath::Atan2(E.facing.x * To.y - E.facing.y * To.x, E.facing.x * To.x + E.facing.y * To.y));
	// A step starts only when Core can begin no move before its yaw lands.
	const float PickIn = Fight.enemyPickIn(Slot);
	const bool bStepFits = static_cast<double>(PickIn) >= kArenaTurnPivot;
	if (Turn.Sign != 0)
	{
		const TCHAR* Way = Turn.Sign > 0 ? TEXT("right") : TEXT("left");
		const TCHAR* PivotName = Turn.Sign > 0 ? TEXT("foot_r") : TEXT("foot_l"); // turn_r pivots on its right foot
		const double T = static_cast<double>(Fight.time()) - Turn.Start;
		FVector Foot;
		if (!Turn.bChecked && bFree && T >= kArenaTurnPivot)
		{
			// The sign check, logged for the -nullrhi runs: the pivot foot as drawn once the first cycle's yaw has settled,
			// against where it stood as the turn began (it is planted until the catch-up step, 0.43 s in), both from the
			// actor's origin so a shove of the whole body doesn't read as a slide. A turn whose yaw runs against its clip
			// moves it 0.2-0.4 m.
			Turn.bChecked = true;
			if (Turn.bFoot && Figure && Figure->GetSkinPoint(FName(PivotName), Foot))
			{
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d's turn (%s) has settled its first step: its pivot %s moved %.1f cm."), Fight.time(), I,
					Way, PivotName, FVector::Dist2D(Foot - Figure->GetActorLocation(), Turn.Foot));
			}
		}
		if (!bFree)
		{
			// A move, a reaction or a hold takes it mid-turn: the facing stays where the turn had it. Logged with how far the
			// pivot foot is from where it stood as the cycle began, from the actor's origin (the crossfade out of a cycle
			// under way slides it; a shove of the whole body doesn't).
			const float Turned = FMath::RadiansToDegrees(FMath::Atan2(Turn.From.x * E.facing.y - Turn.From.y * E.facing.x,
				Turn.From.x * E.facing.x + Turn.From.y * E.facing.y));
			const TCHAR* Why = E.brain == ftcore::ArenaBrain::Attack ? TEXT("a move starts")
				: (E.brain == ftcore::ArenaBrain::Staggered ? TEXT("a stagger")
				: (!bBetween ? TEXT("it leaves its steps") : (bHitPlays ? TEXT("a hit reaction") : (bWalking[I] ? TEXT("a walk")
				: (bCoreMoves ? TEXT("Core moves it") : TEXT("a hold"))))));
			const double Moved = Turn.bFoot && Figure && Figure->GetSkinPoint(FName(PivotName), Foot) ? FVector::Dist2D(Foot - Figure->GetActorLocation(), Turn.Foot) / 100.0 : -1.0;
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d's turn (%s) is cancelled at %.1f deg, %.2f s into step %d (%s): its pivot %s is %.2f m from where it stood."),
				Fight.time(), I, Way, FMath::Abs(Turned), T - (Turn.Cycles - 1) * kArenaTurnCycle, Turn.Cycles, Why, PivotName, Moved);
			++Turn.Cancelled;
			Turn.Sign = 0;
			return;
		}
		if (T + 1e-6 < Turn.Cycles * kArenaTurnCycle)
		{
			return;
		}
		// A cycle's end: its facing is exactly its cycles round (TurnFacing holds it there). Another step while the lead
		// is still that way past the residual and the walk doesn't want to start; else it ends, and the walk may start next.
		// (Any other kind is free only while Core wants it still: only the stone_bearer's walk can want to start here.)
		const float Cone = E.brain == ftcore::ArenaBrain::Approach ? kStoneApproachCone : kStoneWalkCone;
		const bool bWalkWants = bStone && Wanted > kArenaWalkStart && E.velocity.x * E.facing.x + E.velocity.y * E.facing.y >= Cone * Wanted
			&& Skin.Walk >= 0 && static_cast<double>(PickIn) >= (*Skin.Clips)[static_cast<size_t>(Skin.Walk)].length;
		if (static_cast<float>(Turn.Sign) * Off > kArenaTurnResidual && !bWalkWants && bStepFits)
		{
			++Turn.Cycles;
			++Turn.Steps;
			Turn.bFoot = Figure && Figure->GetSkinPoint(FName(PivotName), Turn.Foot);
			if (Turn.bFoot)
			{
				Turn.Foot -= Figure->GetActorLocation();
			}
			return;
		}
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d's turn (%s) ends after %d steps (%.0f deg): the lead is %.1f deg off%s."), Fight.time(), I, Way,
			Turn.Cycles, Turn.Cycles * kArenaTurnDeg, Off,
			bWalkWants ? TEXT(", and it walks") : (!bStepFits && static_cast<float>(Turn.Sign) * Off > kArenaTurnResidual ? TEXT(", and a move is due") : TEXT("")));
		Turn.Sign = 0;
		return;
	}
	if (!bFree || FMath::Abs(Off) < kArenaTurnStart)
	{
		return;
	}
	if (!bStepFits)
	{
		++Turn.HeldPick; // a move begins before the step's yaw would land: the move's own tracking turns it
		return;
	}
	// Only from a settled idle (its crossfade done), so the pivot foot starts from the stance the turn's cycle starts from.
	const int32 Shown = SkinClip[I];
	if (Shown < 0 || (Shown != Skin.Idle && Shown != Skin.IdleTwo))
	{
		return;
	}
	const ftcore::BeatClip& ShownRow = (*Skin.Clips)[static_cast<size_t>(Shown)];
	if (Now - SkinSince[I] < (ShownRow.blend > 0.0 ? ShownRow.blend : Skin.Clips->blend))
	{
		return;
	}
	Turn.Sign = Off > 0.f ? 1 : -1;
	Turn.Start = static_cast<double>(Fight.time());
	Turn.From = E.facing;
	Turn.Cycles = 1;
	const ftcore::BeatClip& Row = (*Skin.Clips)[static_cast<size_t>(Turn.Sign > 0 ? Skin.TurnR : Skin.TurnL)];
	Turn.Blend = Row.blend > 0.0 ? Row.blend : Skin.Clips->blend;
	Turn.bFoot = Figure && Figure->GetSkinPoint(FName(Turn.Sign > 0 ? TEXT("foot_r") : TEXT("foot_l")), Turn.Foot);
	if (Turn.bFoot)
	{
		Turn.Foot -= Figure->GetActorLocation();
	}
	Turn.bChecked = false;
	++Turn.Begun;
	++Turn.Steps;
	UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d turns %s: the lead is %.1f deg off (about %d steps; a move due in %s)."), Fight.time(), I,
		Turn.Sign > 0 ? TEXT("right") : TEXT("left"), Off, FMath::Max(1, FMath::RoundToInt(FMath::Abs(Off) / kArenaTurnDeg)),
		PickIn >= ftcore::kArenaPickNever ? TEXT("no bound") : *FString::Printf(TEXT("%.2f s"), PickIn));
}

void UFTArenaRunner::StartFlinch(int32 I, bool bHeavy)
{
	if (I < 0 || I >= MaxEnemies || !Flinches(I))
	{
		return;
	}
	if (I == StoneBearerSlot)
	{
		// The statue shrugs off light blows for kStoneFlinchHold seconds after a reaction starts, playing or not; a heavy
		// blow restarts a playing reaction once it has held kStoneFlinchRestart seconds.
		const FFTArenaSkin& Skin = Skins[I];
		const int32 Playing = bHitHeavy[I] ? Skin.HitHeavy : Skin.HitLight;
		const double Since = Now - HitSince[I];
		const bool bPlays = Skin.IsLoaded() && Playing >= 0 && Since < static_cast<double>(Skin.Sequences[Playing]->GetPlayLength());
		if (bHeavy ? (bPlays && Since < kStoneFlinchRestart) : Since < kStoneFlinchHold)
		{
			return;
		}
	}
	HitSince[I] = Now;
	bHitHeavy[I] = bHeavy;
}

void UFTArenaRunner::LogStoneBearerNotes()
{
	const int32 Total = StoneBearer.GetNoteTotal();
	if (Total - StoneBearerLogged > static_cast<int32>(FFTStoneBearerHost::MaxNotes))
	{
		StoneBearerLogged = Total - static_cast<int32>(FFTStoneBearerHost::MaxNotes);
	}
	for (; StoneBearerLogged < Total; ++StoneBearerLogged)
	{
		const FFTStoneBearerNote& N = StoneBearer.GetNoteBySerial(StoneBearerLogged);
		const ftcore::ArenaBossEvent& E = N.Event;
		const int32 Move = static_cast<int32>(E.move);
		switch (N.Kind)
		{
		case FFTStoneBearerNote::EKind::Run:
			if (E.type == ftcore::ArenaBossEventType::Broke)
			{
				SplitBossCarried(true);
			}
			if (E.type == ftcore::ArenaBossEventType::Crit)
			{
				BossVfx.Play(FFTArenaBossVfx::EEffect::Crit, BossFlashPoint(), 1.f,
					E.value > 0 ? TEXT("weak point crit, first of the opening") : TEXT("weak point crit"));
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: boss %.2f: crit%s."), E.at, E.value > 0 ? TEXT(", the first of the opening") : TEXT(""));
			}
			else if (E.type == ftcore::ArenaBossEventType::Answered)
			{
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: boss %.2f move %d impact %d: %s"), E.at, Move, static_cast<int32>(E.impact),
					UTF8_TO_TCHAR(ftcore::calledOutcomeName(E.outcome)));
			}
			else
			{
				UE_LOG(LogForgottenThird, Verbose, TEXT("Arena: boss %.2f event %d move %d impact %d value %d"), E.at, static_cast<int32>(E.type), Move,
					static_cast<int32>(E.impact), E.value);
			}
			break;
		case FFTStoneBearerNote::EKind::Refused:
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: boss %.2f move %d refused: cancelled, no hold"), E.at, Move);
			break;
		case FFTStoneBearerNote::EKind::ShockRingSent:
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: boss %.2f move %d: shock ring (stub, no ring blow)"), E.at, Move);
			break;
		case FFTStoneBearerNote::EKind::TileRain:
			UE_LOG(LogForgottenThird, Verbose, TEXT("Arena: boss %.2f move %d: tile rain (stub, one all-round blow)"), E.at, Move);
			break;
		case FFTStoneBearerNote::EKind::LoadGiven:
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: boss %.2f move %d: %d Load given"), E.at, Move, E.value);
			break;
		case FFTStoneBearerNote::EKind::ReleaseMissed:
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: boss %.2f: a Release on the plinth missed (%s)"), E.at,
				N.Release == ftcore::PedestalRelease::LowLoad ? TEXT("too little Load") : TEXT("off the call"));
			break;
		default:
			break;
		}
	}
}

const char* UFTArenaRunner::SlotKindId(int32 I) const
{
	const size_t KindIndex = Def.enemyKind[static_cast<size_t>(I)];
	return KindIndex < Table.enemyIds.size() ? Table.enemyIds[KindIndex].c_str() : "";
}

void UFTArenaRunner::BeginKindCrits()
{
	StopKindCritEffects();
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		bKindCritSlot[I] = false;
		bKindCritWasOpen[I] = false;
		KindCritCritsSeen[I] = 0;
	}
	const bool bHadHook = bKindCritHook;
	bKindCritHook = false;

	if (!bKindCritTableRead)
	{
		bKindCritTableRead = true;
		static constexpr const TCHAR* DataPath = TEXT("Data/Battle/arena_crit.json");
		TArray<uint8> Bytes;
		std::string LoadError;
		std::vector<std::string> Warnings;
		if (!FFileHelper::LoadFileToArray(Bytes, *(FPaths::ProjectDir() / DataPath)))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: couldn't read %s; no kind has weak points."), DataPath);
		}
		else if (!ftcore::importArenaKindCrits(FT::BytesAsText(Bytes), KindCritTable, &LoadError, &Warnings))
		{
			// The loader's errors name paths and keys only.
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s refused; no kind has weak points:\n%s"), DataPath, *FT::ToFString(LoadError));
		}
		for (const std::string& W : Warnings)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: arena_crit: %s"), *FT::ToFString(W));
		}
	}

	// One blow hook per fight, and the stone_bearer's host owns it while it fights here (BeginStoneBearer runs first), so
	// a line-up of a stone_bearer and a crit kind isn't supported yet: the crit kind fights without its weak points.
	bool bAny = false;
	for (int32 I = 0; I < static_cast<int32>(Def.enemyCount) && I < MaxEnemies; ++I)
	{
		const ftcore::ArenaKindCritDef* Entry = KindCritTable.find(SlotKindId(I));
		if (!Entry)
		{
			continue;
		}
		if (StoneBearerSlot >= 0 || KiteSlot >= 0)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s in slot %d fights without its weak points beside the stone_bearer or the kite_boss."), UTF8_TO_TCHAR(SlotKindId(I)), I);
			continue;
		}
		ftcore::ArenaKindCrit& Crit = KindCrits[static_cast<size_t>(I)];
		Crit.reset(*Entry);
		if (!Crit.opensOn(Fight.def().enemies[static_cast<size_t>(I)]))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s in slot %d has no move playing \"%s\"; its weak points never open."), UTF8_TO_TCHAR(SlotKindId(I)), I,
				UTF8_TO_TCHAR(Entry->opensAfterChars.data()));
		}
		bKindCritSlot[I] = true;
		bAny = true;
	}
	if (bAny)
	{
		Fight.setBlowHook(&UFTArenaRunner::RuleKindCritBlow, this);
		bKindCritHook = true;
	}
	else if (bHadHook && StoneBearerSlot < 0 && KiteSlot < 0)
	{
		// The last fight's hook, not the stone_bearer's: nothing rules on this one.
		Fight.setBlowHook(nullptr, nullptr);
	}
}

void UFTArenaRunner::EndKindCrits()
{
	StopKindCritEffects();
	if (bKindCritHook)
	{
		Fight.setBlowHook(nullptr, nullptr);
	}
	bKindCritHook = false;
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		bKindCritSlot[I] = false;
		bKindCritWasOpen[I] = false;
	}
}

void UFTArenaRunner::FeedKindCrits(const ftcore::ArenaEvents& Events)
{
	const double T = static_cast<double>(Fight.time());
	for (const ftcore::ArenaEvent& E : Events)
	{
		if (E.type == ftcore::ArenaEventType::AttackStarted && E.source < ftcore::kMaxArenaEnemies && bKindCritSlot[E.source])
		{
			if (KindCrits[E.source].onAttackStarted(E.move, T, Fight.def().enemies[E.source]) && KindCritTellFx[E.source])
			{
				KindCritTellFx[E.source]->Play();
				bKindCritTelling[E.source] = true;
			}
		}
		else if (E.type == ftcore::ArenaEventType::Hit && E.source == ftcore::kArenaPlayer && E.target < ftcore::kMaxArenaEnemies &&
				 bKindCritSlot[E.target])
		{
			// The blow hook counted a crit for this blow, or it landed plain: the burst at the seam, or ink where it struck.
			const int32 Crits = KindCrits[E.target].crits();
			const bool bCrit = Crits != KindCritCritsSeen[E.target];
			KindCritCritsSeen[E.target] = Crits;
			if (bCrit && KindCritCritFx[E.target])
			{
				KindCritCritFx[E.target]->Play();
			}
			else if (!bCrit && KindCritHitFx[E.target])
			{
				const FVector At = ToWorld(E.at);
				const AFTArenaFighter* Figure = EnemyFigures[E.target].Get();
				KindCritHitFx[E.target]->SetWorldLocation(FVector(At.X, At.Y, Figure ? Figure->GetChest().Z : At.Z));
				KindCritHitFx[E.target]->Play();
			}
		}
	}
	// The window opens at the end of the move's active part, later than its AttackStarted: logged when it does.
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		if (!bKindCritSlot[I])
		{
			continue;
		}
		const ftcore::ArenaKindCrit& Crit = KindCrits[static_cast<size_t>(I)];
		const bool bOpen = Crit.isOpen(T);
		if (bOpen && !bKindCritWasOpen[I])
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %s seam open %.1f s"), UTF8_TO_TCHAR(SlotKindId(I)), Crit.opens());
			if (bKindCritTelling[I] && KindCritTellFx[I])
			{
				KindCritTellFx[I]->Stop();
			}
			bKindCritTelling[I] = false;
			if (KindCritOpenFx[I])
			{
				KindCritOpenFx[I]->Play();
			}
		}
		else if (!bOpen && bKindCritWasOpen[I] && KindCritOpenFx[I])
		{
			KindCritOpenFx[I]->Stop();
		}
		bKindCritWasOpen[I] = bOpen;
	}
}

ftcore::ArenaBlowRuling UFTArenaRunner::RuleKindCritBlow(void* User, const ftcore::ArenaBlow& Blow)
{
	UFTArenaRunner* Runner = static_cast<UFTArenaRunner*>(User);
	if (!Runner || Blow.target >= ftcore::kMaxArenaEnemies || !Runner->bKindCritSlot[Blow.target])
	{
		return ftcore::ArenaBlowRuling();
	}
	ftcore::ArenaKindCrit& Crit = Runner->KindCrits[Blow.target];
	const int Before = Crit.crits();
	// ArenaBlow carries only the source's facing: the slot's is the last tick's output. Its position is Blow.at, the body
	// as reported for this tick (ArenaEnemyOut has none), as the stone_bearer's host reads it.
	const ftcore::ArenaBlowRuling Ruling = Crit.rule(Blow, Blow.target, Blow.at, Runner->Fight.output().enemies[Blow.target].facing);
	if (Crit.crits() != Before)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %s crit x%.1f on %s, %d hp"), UTF8_TO_TCHAR(Runner->SlotKindId(Blow.target)), Crit.lastMultiplier(),
			UTF8_TO_TCHAR(Crit.points().points[Crit.lastPoint()].idChars.data()), Crit.lastDamage());
	}
	return Ruling;
}

void UFTArenaRunner::SpawnKindCritEffects()
{
	// The OFF half of an OFF/ON capture: the fight runs the same, no kind's tell, open, crit or hit effect plays.
	if (FParse::Param(FCommandLine::Get(), TEXT("ArenaNoKindFx")))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: kind crit fx off (-ArenaNoKindFx)."));
		return;
	}
	for (int32 I = 0; I < static_cast<int32>(Def.enemyCount) && I < MaxEnemies; ++I)
	{
		AFTArenaFighter* Figure = EnemyFigures[I].Get();
		const ftcore::ArenaKindCritDef* Entry = bKindCritSlot[I] ? KindCritTable.find(SlotKindId(I)) : nullptr;
		if (!Figure || !Entry || KindCritTellFx[I])
		{
			continue;
		}
		const ftcore::ArenaKindCritEffects& Fx = Entry->effects;
		KindCritTellFx[I] = MakeKindCritEffect(Figure, Fx.tell(), Fx.socket());
		KindCritOpenFx[I] = MakeKindCritEffect(Figure, Fx.open(), Fx.socket());
		KindCritCritFx[I] = MakeKindCritEffect(Figure, Fx.crit(), Fx.socket());
		// Where the blow lands, not the seam: put there on each hit.
		KindCritHitFx[I] = MakeKindCritEffect(Figure, Fx.hit(), std::string_view());
		if (KindCritHitFx[I])
		{
			KindCritHitFx[I]->SetUsingAbsoluteLocation(true);
		}
	}
}

UFTVfxComponent* UFTArenaRunner::MakeKindCritEffect(AFTArenaFighter* Figure, std::string_view Effect, std::string_view Socket)
{
	if (Effect.empty())
	{
		return nullptr;
	}
	UFTVfxComponent* Fx = NewObject<UFTVfxComponent>(Figure);
	Fx->Effect = FName(*FT::ToFString(std::string(Effect)));
	Fx->bPlayOnBeginPlay = false;
	USkeletalMeshComponent* Skin = Figure->GetSkin();
	const FName SocketName = Socket.empty() ? NAME_None : FName(*FT::ToFString(std::string(Socket)));
	if (Skin)
	{
		if (!SocketName.IsNone() && !Skin->DoesSocketExist(SocketName))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: effect %s: the mesh has no bone or socket %s; it plays at the mesh's origin."),
				*Fx->Effect.ToString(), *SocketName.ToString());
		}
		Fx->SetupAttachment(Skin, SocketName);
	}
	else
	{
		Fx->SetupAttachment(Figure->GetRootComponent());
	}
	Fx->RegisterComponent();
	return Fx;
}

void UFTArenaRunner::StopKindCritEffects()
{
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		if (KindCritTellFx[I])
		{
			KindCritTellFx[I]->Stop();
		}
		if (KindCritOpenFx[I])
		{
			KindCritOpenFx[I]->Stop();
		}
		bKindCritTelling[I] = false;
	}
}

// -ArenaShowcase: a plain blow's ink this share of the body radius out from the crit kind's centre, toward the camera. The
// fight plays it at the body's centre; the reel puts it on the camera's side so the body doesn't hide it.
constexpr float kShowKindHitSurface = 0.5f;

void UFTArenaRunner::ShowcaseKindCritCue(const ftcore::ShowCueAt& Cue, AFTArenaFighter& Figure)
{
	if (ShowSlot < 0 || ShowSlot >= MaxEnemies || !bKindCritSlot[ShowSlot])
	{
		return;
	}
	const int32 I = ShowSlot;
	const size_t Slot = static_cast<size_t>(ShowSlot);
	switch (Cue.cue)
	{
	case ftcore::ShowCue::Tell:
	{
		// As in the fight, only the move whose clip opens the window tells (opensAfter); the lone tell plays it too.
		bool bOpening = Cue.place == ftcore::ShowPlace::Ahead;
		if (!bOpening && Cue.attack >= 0 && static_cast<size_t>(Cue.attack) < ShowSpecUsed.attackCount)
		{
			const ftcore::ArenaEnemyKind& EnemyKind = Fight.def().enemies[Slot];
			const size_t Move = static_cast<size_t>(ShowSpecUsed.attacks[static_cast<size_t>(Cue.attack)].move);
			bOpening = Move < EnemyKind.moveCount && EnemyKind.moves[Move].clip() == KindCrits[Slot].opensAfter();
		}
		if (bOpening && KindCritTellFx[I])
		{
			KindCritTellFx[I]->Play();
			bKindCritTelling[I] = true;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: %s tell on (%s)."), UTF8_TO_TCHAR(SlotKindId(I)),
				Cue.place == ftcore::ShowPlace::Ahead ? TEXT("alone") : TEXT("its opening move"));
		}
		return;
	}
	case ftcore::ShowCue::TellStop:
		if (bKindCritTelling[I] && KindCritTellFx[I])
		{
			KindCritTellFx[I]->Stop();
			UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: %s tell off."), UTF8_TO_TCHAR(SlotKindId(I)));
		}
		bKindCritTelling[I] = false;
		return;
	case ftcore::ShowCue::Impact:
		return;
	case ftcore::ShowCue::Chips:
	case ftcore::ShowCue::ChipsHeavy:
	{
		// The fight plays the kind's hit effect on every blow that isn't a crit, light or heavy, so the heavy hit's cue plays it too.
		if (!KindCritHitFx[I])
		{
			return;
		}
		const FVector Feet = Figure.GetActorLocation();
		FVector Toward = ShowCamera ? ShowCamera->GetActorLocation() - Feet : Figure.GetActorForwardVector();
		Toward.Z = 0.0;
		Toward = Toward.GetSafeNormal();
		const double Out = static_cast<double>(Fight.def().enemies[Slot].bodyRadius * kShowKindHitSurface) * 100.0;
		FVector At = Feet + Toward * Out;
		At.Z = Figure.GetChest().Z;
		KindCritHitFx[I]->SetWorldLocation(At);
		KindCritHitFx[I]->Play();
		UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: %s ink %.2f m out at %.2f m up."), UTF8_TO_TCHAR(SlotKindId(I)), Out / 100.0, (At.Z - Feet.Z) / 100.0);
		return;
	}
	case ftcore::ShowCue::GlowOn:
		// The window opens: the tell ends, as in the fight.
		if (bKindCritTelling[I] && KindCritTellFx[I])
		{
			KindCritTellFx[I]->Stop();
		}
		bKindCritTelling[I] = false;
		if (KindCritOpenFx[I])
		{
			KindCritOpenFx[I]->Play();
			UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: %s seam open."), UTF8_TO_TCHAR(SlotKindId(I)));
		}
		return;
	case ftcore::ShowCue::GlowOff:
		if (KindCritOpenFx[I])
		{
			KindCritOpenFx[I]->Stop();
			UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: %s seam shut."), UTF8_TO_TCHAR(SlotKindId(I)));
		}
		return;
	case ftcore::ShowCue::Crit:
		if (KindCritCritFx[I])
		{
			KindCritCritFx[I]->Play();
			UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: %s seam crit."), UTF8_TO_TCHAR(SlotKindId(I)));
		}
		return;
	}
}

void UFTArenaRunner::PrepareKite()
{
	KiteSlot = -1;
	for (size_t I = 0; I < static_cast<size_t>(Def.enemyCount); ++I)
	{
		const size_t KindIndex = Def.enemyKind[I];
		if (KindIndex < Table.enemyIds.size() && Table.enemyIds[KindIndex] == FFTKiteHost::KindId)
		{
			KiteSlot = static_cast<int32>(I);
			break;
		}
	}
	if (KiteSlot < 0)
	{
		return;
	}
	if (StoneBearerSlot >= 0)
	{
		// One blow hook per fight, and the stone_bearer's host has it.
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite_boss shares the fight with the stone_bearer, whose host holds the blow hook; the kite has no weak point."));
		KiteSlot = -1;
		return;
	}
	if (!Kite.IsLoaded())
	{
		TArray<uint8> Bytes;
		const FString File = FPaths::ProjectDir() / UTF8_TO_TCHAR(FFTKiteHost::DataPath);
		std::string LoadError;
		std::vector<std::string> Warnings;
		if (!FFileHelper::LoadFileToArray(Bytes, *File))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: couldn't read %s; kite_boss fights without a weak point."), UTF8_TO_TCHAR(FFTKiteHost::DataPath));
		}
		else if (!Kite.Load(FT::BytesAsText(Bytes), &LoadError, &Warnings))
		{
			// The loader's errors name paths and keys only.
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s refused; kite_boss fights without a weak point:\n%s"), UTF8_TO_TCHAR(FFTKiteHost::DataPath),
				*FT::ToFString(LoadError));
		}
		for (const std::string& W : Warnings)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite: %s"), *FT::ToFString(W));
		}
	}
	if (!Kite.IsLoaded())
	{
		KiteSlot = -1;
	}
}

void UFTArenaRunner::BeginKite()
{
	KiteFx.Stop();
	if (KiteSlot < 0)
	{
		Kite.End();
		return;
	}
	Kite.Begin(Fight, static_cast<size_t>(KiteSlot));
	KiteLogged = Kite.GetNoteTotal();
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: kite in slot %d: %d weak point(s), socket %s."), KiteSlot,
		static_cast<int32>(Kite.GetWeakPoints().points.size()), UTF8_TO_TCHAR(FFTKiteHost::WeakPointSocket));
}

void UFTArenaRunner::EndKite()
{
	KiteFx.Reset();
	Kite.End();
	KiteSlot = -1;
}

void UFTArenaRunner::LogKiteNotes()
{
	const int32 Total = Kite.GetNoteTotal();
	if (Total - KiteLogged > static_cast<int32>(FFTKiteHost::MaxNotes))
	{
		KiteLogged = Total - static_cast<int32>(FFTKiteHost::MaxNotes);
	}
	for (; KiteLogged < Total; ++KiteLogged)
	{
		const FFTKiteNote& N = Kite.GetNoteBySerial(KiteLogged);
		switch (N.Kind)
		{
		case FFTKiteNote::EKind::Telegraph:
			UE_LOG(LogForgottenThird, Verbose, TEXT("Arena: kite %.2f move %d telegraph"), N.At, static_cast<int32>(N.Move));
			break;
		case FFTKiteNote::EKind::CritOpened:
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: kite %.2f: crit opened until %.2f"), N.At, N.Until);
			break;
		case FFTKiteNote::EKind::CritLanded:
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: kite %.2f: crit landed on point %d, x%.1f%s"), N.At, static_cast<int32>(N.Point), N.Multiplier,
				N.bFirst ? TEXT(", first") : TEXT(""));
			break;
		case FFTKiteNote::EKind::CritClosed:
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: kite %.2f: crit closed"), N.At);
			break;
		case FFTKiteNote::EKind::HitTaken:
			UE_LOG(LogForgottenThird, Verbose, TEXT("Arena: kite %.2f hit taken%s"), N.At, N.bCrit ? TEXT(", crit") : TEXT(""));
			break;
		default:
			break;
		}
	}
}

const char* UFTArenaRunner::BossContactSocket(uint8 Move, bool bLeft) const
{
	const uint8 BossMove = StoneBearer.BossMoveOf(Move);
	const ftcore::StoneBearerDef& D = StoneBearer.GetDef();
	const char* Socket = BossMove < D.boss.moveCount && D.boss.moves[BossMove].hasContact ? D.boss.moves[BossMove].contactSocketChars.data() : nullptr;
	// The contact's socket is a label, not always a bone: the carried piece's striking edge rides the piece's bone.
	if (Socket && FCStringAnsi::Strcmp(Socket, "slab_edge") == 0)
	{
		return "slab";
	}
	if (Socket && bLeft && FCStringAnsi::Strcmp(Socket, "hand_r") == 0)
	{
		return "hand_l"; // its left-hand clip strikes with the other hand
	}
	return Socket;
}

bool UFTArenaRunner::BeginShowcase()
{
	// The boss: the stone_bearer, else the kite_boss, else the line-up's first skinned enemy.
	ShowSlot = -1;
	if (StoneBearerSlot >= 0 && StoneBearer.IsActive())
	{
		ShowSlot = StoneBearerSlot;
	}
	else if (KiteSlot >= 0)
	{
		ShowSlot = KiteSlot;
	}
	else
	{
		for (int32 I = 0; I < MaxEnemies; ++I)
		{
			if (EnemyFigures[I] && Skins[I].IsLoaded())
			{
				ShowSlot = I;
				break;
			}
		}
	}
	AFTArenaFighter* Figure = ShowSlot >= 0 ? EnemyFigures[ShowSlot].Get() : nullptr;
	if (!Figure || !Skins[ShowSlot].IsLoaded())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena showcase: there is no skinned enemy in this fight; -ArenaShowcase does nothing."));
		ShowSlot = -1;
		return false;
	}
	const bool bStone = ShowSlot == StoneBearerSlot;
	const FFTArenaSkin& Skin = Skins[ShowSlot];
	const ftcore::BeatClips& Clips = *Skin.Clips;
	const auto Role = [&Skin](int32 Clip) -> int16
	{
		return Clip >= 0 && Skin.Sequences.IsValidIndex(Clip) && Skin.Sequences[Clip] ? static_cast<int16>(Clip) : ftcore::kNoClip;
	};
	ftcore::ShowSpec Spec;
	Spec.idle = Role(Skin.Idle);
	Spec.idleTwo = Role(Skin.IdleTwo);
	Spec.walk = Role(Skin.Walk);
	Spec.turnL = Role(Skin.TurnL);
	Spec.turnR = Role(Skin.TurnR);
	if (Spec.turnL == ftcore::kNoClip || Spec.turnR == ftcore::kNoClip)
	{
		Spec.turnL = ftcore::kNoClip;
		Spec.turnR = ftcore::kNoClip;
	}
	Spec.stagger = Role(Skin.Stagger);
	Spec.downIn = Role(Skin.FaceDownIn);
	Spec.down = Role(Skin.FaceDown);
	Spec.downOut = Role(Skin.FaceDownOut);
	Spec.defeat = Role(Skin.Defeat);
	Spec.defeatKneel = Role(Skin.DefeatKneel);
	Spec.hitLight = Role(Skin.HitLight);
	Spec.hitHeavy = Role(Skin.HitHeavy);
	Spec.walkFrom = bStone ? kStoneWalkStartPhase : 0.0;
	Spec.turnDeg = kArenaTurnDeg;
	Spec.turnPivot = kArenaTurnPivot;
	// The weak point's facing (its first gate's, from the boss's data): the glow and crit takes turn it to the camera. A kind
	// without weak points keeps the spec's default (behind).
	const ftcore::ArenaWeakPoints* Weak = bStone ? &StoneBearer.GetDef().boss.weakPoints
		: (ShowSlot == KiteSlot && GetKite()) ? &GetKite()->GetWeakPoints()
		: GetKindCrit(ShowSlot) ? &GetKindCrit(ShowSlot)->points() : nullptr;
	if (Weak && !Weak->points.empty())
	{
		Spec.weakFacing = Weak->gates[0].facing;
	}
	UE_LOG(LogForgottenThird, Log, TEXT("Arena showcase: the weak point faces %.0f deg in the boss's frame (%s); the glow and crit takes turn it to the camera."),
		Spec.weakFacing, Weak && !Weak->points.empty() ? TEXT("its data") : TEXT("no weak point, the default"));

	// Every move's clip once (a variant plays its base move's clip, and a tile move's later impacts run on from its
	// first), then its left-hand twin: entered where the fight enters it at rate 1.0, left as the move ends.
	const size_t Slot = static_cast<size_t>(ShowSlot);
	const ftcore::ArenaEnemyKind& Kind = Fight.def().enemies[Slot];
	const auto AddAttack = [&](size_t Move, int32 Clip, bool bLeft)
	{
		const int16 C = Role(Clip);
		if (C == ftcore::kNoClip || Spec.attackCount >= ftcore::kMaxShowAttacks)
		{
			return;
		}
		for (size_t K = 0; K < Spec.attackCount; ++K)
		{
			if (Spec.attacks[K].clip == C)
			{
				return;
			}
		}
		const ftcore::ArenaAttack& A = Kind.moves[Move].attack;
		const double Impact = Clips[static_cast<size_t>(C)].impact;
		double Entry = 0.0;
		if (Impact > 0.0)
		{
			Entry = bStone ? StoneBearer.ClipEntry(Kind, Move, Impact) : Impact - static_cast<double>(A.startup);
		}
		if (Entry < -ftcore::kBeatClipEntrySlack)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena showcase: move %d's clip %s is entered at %.2f s; the fight plays it faster, the reel at rate 1.0 from 0."),
				static_cast<int32>(Move), *Skin.Sequences[C]->GetName(), Entry);
		}
		Entry = FMath::Max(Entry, 0.0);
		ftcore::ShowAttack& S = Spec.attacks[Spec.attackCount++];
		S.clip = C;
		S.move = static_cast<int16>(Move);
		S.entry = Entry;
		S.until = Entry + static_cast<double>(A.startup + A.active + A.recovery);
		S.left = bLeft;
	};
	if (bStone)
	{
		const ftcore::StoneBearerLayout& Layout = StoneBearer.GetLayout();
		for (size_t M = 0; M < Kind.moveCount && M < Layout.count && M < ftcore::kMaxArenaMoves; ++M)
		{
			if (Layout.impactOf[M] == 0)
			{
				AddAttack(M, Skin.MoveClip[M], false);
				AddAttack(M, Skin.MoveClipL[M], true);
			}
		}
	}
	else
	{
		for (size_t M = 0; M < Kind.moveCount && M < ftcore::kMaxArenaMoves; ++M)
		{
			AddAttack(M, Skin.MoveClip[M], false);
		}
	}

	Show = MakeShared<ftcore::ClipShow>();
	std::string Error;
	if (!ftcore::buildClipShow(Clips, Spec, *Show, &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena showcase: the reel can't be built (%s); -ArenaShowcase does nothing."), UTF8_TO_TCHAR(Error.c_str()));
		Show.Reset();
		return false;
	}
	ShowSpecUsed = Spec;
	// A clip whose asset runs a frame longer or shorter than its row is played by the row's length: say so.
	for (size_t I = 0; I < Show->segmentCount; ++I)
	{
		const ftcore::ShowSegment& Seg = Show->segments[I];
		const float Asset = Seg.clip >= 0 ? Skin.Sequences[Seg.clip]->GetPlayLength() : 0.f;
		if (Seg.clip >= 0 && !Seg.goesOn && FMath::Abs(static_cast<double>(Asset) - Seg.clipLength) > 1.0 / 30.0)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena showcase: %s is %.3f s long but its row says %.3f s."), *Skin.Sequences[Seg.clip]->GetName(),
				Asset, Seg.clipLength);
		}
	}

	// Only the boss: the lead, the other enemies, the plinth and the keep-out disc hidden, the HUD off.
	if (PlayerFigure)
	{
		PlayerFigure->SetActorHiddenInGame(true);
	}
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		if (I != ShowSlot && EnemyFigures[I])
		{
			EnemyFigures[I]->SetActorHiddenInGame(true);
		}
	}
	if (PlinthActor)
	{
		PlinthActor->SetActorHiddenInGame(true);
	}
	if (KeepOutActor)
	{
		KeepOutActor->SetActorHiddenInGame(true);
	}
	BossVfx.StopAll();
	// Hidden between parts it still poses, so the frame it shows again isn't a stale pose.
	if (USkeletalMeshComponent* FigureSkin = Figure->GetSkin())
	{
		FigureSkin->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	}

	// The boss on the ground at its start, facing as it began; the camera where the lead's would be, off its front-left.
	ShowOrigin = ToWorld(Bodies.enemies[Slot]);
	ShowBaseYaw = static_cast<float>(Figure->GetActorRotation().Yaw);
	const FRotator Base(0.0, ShowBaseYaw, 0.0);
	const FVector Front = Base.Vector();
	const FVector Right = FRotationMatrix(Base).GetUnitAxis(EAxis::Y);
	const double Bearing = FMath::DegreesToRadians(static_cast<double>(kShowCamBearing));
	const FVector Dir = Front * FMath::Cos(Bearing) - Right * FMath::Sin(Bearing);
	const double Distance = static_cast<double>(Fit.camera.arm + Kind.bodyRadius + kShowCamGap) * 100.0;
	const FVector Target = ShowOrigin + FVector(0.0, 0.0, Figure->GetHeight() * 0.5);
	const FVector From = ShowOrigin + Dir * Distance + FVector(0.0, 0.0, static_cast<double>(Fit.camera.pivot + Fit.camera.up) * 100.0);
	if (UWorld* World = GetWorld())
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ShowCamera = World->SpawnActor<ACameraActor>(From, (Target - From).Rotation(), Params);
	}
	APlayerController* PC = GetPC();
	if (ShowCamera)
	{
		ShowCamera->GetCameraComponent()->SetFieldOfView(Fit.camera.fov);
		ShowCamera->GetCameraComponent()->bConstrainAspectRatio = false;
		if (PC)
		{
			PC->SetViewTarget(ShowCamera);
		}
	}
	ShowTime = -kShowLeadIn;
	ShowCueTime = -1.0;
	ShowSegment = -1;
	bShowHidden = false;
	bShowUnhideDue = false;
	ShowTellAttack = -1;
	bShowTellAir = false;
	bShowDone = false;
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena showcase: enemy %d (%s), %d parts, %d cues, %.2f s (after a %.1f s lead-in), %d attacks; the boss at (%.2f, %.2f) m yaw %.1f, %.2f m tall; the camera %.2f m out at %.0f deg to its left, %.2f m up, FOV %.0f%s."),
		ShowSlot, UTF8_TO_TCHAR(SlotKindId(ShowSlot)), static_cast<int32>(Show->segmentCount), static_cast<int32>(Show->cueCount), Show->length, kShowLeadIn, static_cast<int32>(Spec.attackCount),
		ShowOrigin.X / 100.0, ShowOrigin.Y / 100.0, ShowBaseYaw, Figure->GetHeight() / 100.0, Distance / 100.0, kShowCamBearing,
		static_cast<double>(Fit.camera.pivot + Fit.camera.up), Fit.camera.fov,
		bStone ? TEXT("") : (Spec.turnL == ftcore::kNoClip ? TEXT("; no stepping turns (no turn clips), and no effects until this boss's own are wired into ShowcaseCue")
			: TEXT("; no effects until this boss's own are wired into ShowcaseCue")));
	// The stone_bearer's scale, measured on the mesh as imported (the scale check's evidence: its body's top and its slab).
	if (bStone)
	{
		FName HideSlot;
		TArray<FFTCarriedPiece> Pieces;
		double BodyTop = 0.0;
		double SlabLength = 0.0;
		const USkeletalMeshComponent* FigureSkin = Figure->GetSkin();
		const USkeletalMesh* Mesh = FigureSkin ? FigureSkin->GetSkeletalMeshAsset() : nullptr;
		if (Mesh && ReadCarriedSplit(kStoneBearerSplit, HideSlot, Pieces)
			&& MeasureCarried(*Mesh, HideSlot, FigureSkin->GetComponentScale().Z, BodyTop, SlabLength))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: scale: %s's body top %.3f m over its feet, its slab (%s) %.3f m long (the bind pose as imported, at scale %.3f)."),
				*Mesh->GetName(), BodyTop, *HideSlot.ToString(), SlabLength, FigureSkin->GetComponentScale().Z);
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena showcase: scale: the body's top and the slab's length can't be measured (no source model, or no %s section)."),
				*HideSlot.ToString());
		}
	}
	for (size_t K = 0; K < Spec.attackCount; ++K)
	{
		const ftcore::ShowAttack& A = Spec.attacks[K];
		UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: attack %d is move %d, %s from %.3f s to %.3f s%s."), static_cast<int32>(K),
			static_cast<int32>(A.move), *Skin.Sequences[A.clip]->GetName(), A.entry, A.until, A.left ? TEXT(", to the left") : TEXT(""));
	}
	return true;
}

void UFTArenaRunner::TickShowcase(float DeltaSeconds)
{
	AFTArenaFighter* Figure = ShowSlot >= 0 ? EnemyFigures[ShowSlot].Get() : nullptr;
	if (!Show || !Figure || bShowDone)
	{
		return;
	}
	if (APlayerController* PC = GetPC())
	{
		if (AHUD* Hud = PC->GetHUD())
		{
			Hud->bShowHUD = false;
		}
		if (ShowCamera && PC->GetViewTarget() != ShowCamera)
		{
			PC->SetViewTarget(ShowCamera);
		}
	}
	const bool bLive = ShowTime >= 0.0;
	const double T = FMath::Max(ShowTime, 0.0);
	const ftcore::ShowPose Pose = ftcore::clipShowPose(*Show, T);
	if (Pose.segment < 0)
	{
		bShowDone = true;
		BossVfx.StopAll();
		UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: done at reel %.3f s, frame %llu."), T, static_cast<unsigned long long>(GFrameCounter));
		FPlatformMisc::RequestExitWithStatus(false, 0, TEXT("Arena"));
		return;
	}
	const FFTArenaSkin& Skin = Skins[ShowSlot];
	const ftcore::ShowSegment& Seg = Show->segments[static_cast<size_t>(Pose.segment)];
	const bool bNew = bLive && Pose.segment != ShowSegment;
	if (bNew)
	{
		ShowSegment = Pose.segment;
		const FString Label = UTF8_TO_TCHAR(std::string(Seg.label()).c_str());
		UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: part %d %s (%s) at reel %.3f s for %.2f s, frame %llu."), Pose.segment, *Label,
			Seg.clip >= 0 ? *Skin.Sequences[Seg.clip]->GetName() : TEXT("hidden"), Seg.start, Seg.length,
			static_cast<unsigned long long>(GFrameCounter));
		if (Seg.clip >= 0 && !Seg.goesOn)
		{
			// The fight's own lines, so the loops cutter reads the reel as it reads a fight.
			if (Seg.attack >= 0)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: enemy %d plays %s for move %d at rate %.2f (%.2f s) from %.2f s."), ShowSlot,
					*Skin.Sequences[Seg.clip]->GetName(), static_cast<int32>(ShowSpecUsed.attacks[static_cast<size_t>(Seg.attack)].move), 1.0,
					Seg.clipLength, Seg.clipFrom);
			}
			else
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: enemy %d plays %s at %.3f s (%.2f s)."), ShowSlot, *Skin.Sequences[Seg.clip]->GetName(),
					T, Seg.clipLength);
			}
		}
	}
	// Shown again a frame late: posed (and its bones refreshed) the frame its part starts while still hidden, drawn from
	// the next with its motion vectors cleared, so its first drawn frame is neither the last part's pose nor a smear from it.
	const bool bHide = Pose.clip < 0;
	if (bShowUnhideDue && !bHide)
	{
		bShowUnhideDue = false;
		Figure->SetActorHiddenInGame(false);
		if (USkeletalMeshComponent* FigureSkin = Figure->GetSkin())
		{
			FigureSkin->ClearMotionVector();
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: shown again at reel %.3f s, frame %llu (posed the frame before)."), T,
			static_cast<unsigned long long>(GFrameCounter));
	}
	if (bHide != bShowHidden)
	{
		bShowHidden = bHide;
		bShowUnhideDue = !bHide;
		if (bHide)
		{
			Figure->SetActorHiddenInGame(true);
		}
	}
	// Placed in the reel's frame, turned with the boss's start.
	const FRotator Base(0.0, ShowBaseYaw, 0.0);
	const FVector Front = Base.Vector();
	const FVector Right = FRotationMatrix(Base).GetUnitAxis(EAxis::Y);
	FFTArenaPose Place;
	Place.Feet = ShowOrigin + Front * (static_cast<double>(Pose.pos.x) * 100.0) + Right * (static_cast<double>(Pose.pos.y) * 100.0);
	Place.Yaw = ShowBaseYaw + Pose.yaw;
	Figure->ApplyPose(Place);
	if (!bHide)
	{
		const UAnimSequence* Sequence = Skin.Sequences[Pose.clip];
		const ftcore::BeatClip& Row = (*Skin.Clips)[static_cast<size_t>(Pose.clip)];
		const double Blend = Seg.cut ? static_cast<double>(kShowCutBlend) : (Row.blend > 0.0 ? Row.blend : Skin.Clips->blend);
		const float Length = Sequence->GetPlayLength();
		const float At = FMath::Clamp(static_cast<float>(Pose.clipTime), 0.f, Length);
		Figure->PlayClip(Sequence, At, Pose.loop, FrameDelta, true, static_cast<float>(Blend), Row.additiveIn);
	}
	if (bLive)
	{
		std::size_t First = 0;
		std::size_t Last = 0;
		ftcore::clipShowCues(*Show, ShowCueTime, T, First, Last);
		for (std::size_t I = First; I < Last; ++I)
		{
			ShowcaseCue(Show->cues[I], *Figure);
		}
		ShowCueTime = T;
	}
	if (BossVfx.IsGlowing())
	{
		BossVfx.MoveGlow(BossFlashPoint());
	}
	ShowTime += static_cast<double>(DeltaSeconds);
}

void UFTArenaRunner::ShowcaseCue(const ftcore::ShowCueAt& Cue, AFTArenaFighter& Figure)
{
	UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: cue %s at reel %.3f s, frame %llu."), UTF8_TO_TCHAR(ftcore::showCueKey(Cue.cue)), Cue.at,
		static_cast<unsigned long long>(GFrameCounter));
	// The Dealer's effects: its kind-crit components (ShowcaseKindCritCue). A crit kind never fights beside the stone_bearer
	// (BeginKindCrits), so the stone block below never plays for it.
	if (ShowSlot >= 0 && ShowSlot < MaxEnemies && bKindCritSlot[ShowSlot])
	{
		ShowcaseKindCritCue(Cue, Figure);
		return;
	}
	// The effects: the stone_bearer's pool. Another boss's effects go here, beside it, keyed on its slot.
	if (ShowSlot != StoneBearerSlot || !BossVfx.IsSpawned())
	{
		return;
	}
	const FFTArenaBossVfx::FTuning& Tune = BossVfx.GetTuning();
	const double Lift = Tune.Lift * 100.0;
	const FVector Feet = Figure.GetActorLocation();
	const float YawRad = FMath::DegreesToRadians(static_cast<float>(Figure.GetActorRotation().Yaw));
	const ftcore::Vec2 Pos{static_cast<float>(Feet.X / 100.0), static_cast<float>(Feet.Y / 100.0)};
	const ftcore::Vec2 Facing{FMath::Cos(YawRad), FMath::Sin(YawRad)};
	const size_t Slot = static_cast<size_t>(ShowSlot);
	switch (Cue.cue)
	{
	case ftcore::ShowCue::Tell:
	{
		if (Cue.place == ftcore::ShowPlace::Ahead)
		{
			ShowTellAt = ftcore::Vec2{Pos.x + Facing.x * kShowAhead, Pos.y + Facing.y * kShowAhead};
			ShowTellShapeMul = 1.f;
			ShowTellAttack = -1;
			bShowTellAir = false;
			BossVfx.Play(FFTArenaBossVfx::EEffect::Tell, ToWorld(ShowTellAt) + FVector(0.0, 0.0, Lift), 1.f, TEXT("tell alone"));
			return;
		}
		if (Cue.attack < 0 || static_cast<size_t>(Cue.attack) >= ShowSpecUsed.attackCount)
		{
			return;
		}
		const ftcore::ShowAttack& A = ShowSpecUsed.attacks[static_cast<size_t>(Cue.attack)];
		FFTStoneBearerStrike S;
		if (!StoneBearer.StrikeAt(static_cast<uint8>(A.move), Pos, Facing, A.left ? ftcore::CalledZone::Left : ftcore::CalledZone::Right,
				Tune.CalledAhead, Tune.CalledSide, Tune.ReachShare, false, S))
		{
			return;
		}
		S.To = Fit.zone.clamp(S.To, 0.f);
		const bool bArea = S.Shape == FFTStoneBearerStrike::EShape::Area;
		ShowTellAt = S.To;
		ShowTellShapeMul = bArea ? Tune.AreaScale : 1.f;
		ShowTellAttack = Cue.attack;
		bShowTellAir = S.bAir;
		ShowTellUp = S.Up;
		if (S.Shape != FFTStoneBearerStrike::EShape::Lane)
		{
			if (bArea)
			{
				BossVfx.Play(FFTArenaBossVfx::EEffect::TellArea, ToWorld(S.To) + FVector(0.0, 0.0, Lift), 1.f, TEXT("tell all round"));
			}
			else
			{
				BossVfx.Play(FFTArenaBossVfx::EEffect::Tell, ToWorld(S.To) + FVector(0.0, 0.0, Lift), ShowTellShapeMul, TEXT("tell"));
			}
			return;
		}
		// A lane: its marks spaced from the boss's edge to the lane's end, as the fight draws them.
		const ftcore::Vec2 Span{S.To.x - S.From.x, S.To.y - S.From.y};
		const float Length = ArenaLength(Span);
		const float Edge = FMath::Min(Fight.def().enemies[Slot].bodyRadius, Length);
		for (int32 K = 0; K < Tune.LaneMarks; ++K)
		{
			const float Along = Tune.LaneMarks > 1 ? FMath::Lerp(Edge, Length, static_cast<float>(K) / static_cast<float>(Tune.LaneMarks - 1)) : Length;
			const float Share = Length > 1e-4f ? Along / Length : 0.f;
			const FVector Mark = ToWorld(ftcore::Vec2{S.From.x + Span.x * Share, S.From.y + Span.y * Share}) + FVector(0.0, 0.0, Lift);
			BossVfx.Play(FFTArenaBossVfx::EEffect::Tell, Mark, ShowTellShapeMul, K == 0 ? TEXT("tell along a lane") : nullptr);
		}
		return;
	}
	case ftcore::ShowCue::TellStop:
		BossVfx.StopTells();
		return;
	case ftcore::ShowCue::Impact:
	{
		// The dust where the tell was drawn (a blow in the air: at its contact's height over it), and how far the striking
		// part is from it.
		const FVector Tell = ToWorld(ShowTellAt);
		const bool bAir = bShowTellAir && Cue.place != ftcore::ShowPlace::Ahead;
		const FVector Burst = Tell + FVector(0.0, 0.0, bAir ? static_cast<double>(ShowTellUp) * 100.0 : Lift);
		BossVfx.Play(FFTArenaBossVfx::EEffect::Impact, Burst, ShowTellShapeMul,
			Cue.place == ftcore::ShowPlace::Ahead ? TEXT("dust alone") : (bAir ? TEXT("blow lands in the air") : TEXT("blow lands")));
		if (ShowTellAttack >= 0 && static_cast<size_t>(ShowTellAttack) < ShowSpecUsed.attackCount)
		{
			const ftcore::ShowAttack& A = ShowSpecUsed.attacks[static_cast<size_t>(ShowTellAttack)];
			const char* Socket = BossContactSocket(static_cast<uint8>(A.move), A.left);
			FVector Part;
			if (Socket && Figure.GetSkinPoint(FName(Socket), Part))
			{
				if (bAir)
				{
					UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: attack %d's %s lands %.2f m from its tell on the ground (in the air: it is %.2f m up, the dust %.2f m up and %.2f m from it)."),
						static_cast<int32>(ShowTellAttack), UTF8_TO_TCHAR(Socket), FVector::Dist2D(Part, Tell) / 100.0, (Part.Z - Tell.Z) / 100.0,
						static_cast<double>(ShowTellUp), FVector::Dist(Part, Burst) / 100.0);
				}
				else
				{
					UE_LOG(LogForgottenThird, Display, TEXT("Arena showcase: attack %d's %s lands %.2f m from its tell."), static_cast<int32>(ShowTellAttack),
						UTF8_TO_TCHAR(Socket), FVector::Dist2D(Part, Tell) / 100.0);
				}
			}
		}
		ShowTellAttack = -1;
		bShowTellAir = false;
		return;
	}
	case ftcore::ShowCue::Chips:
	case ftcore::ShowCue::ChipsHeavy:
	{
		// On the body's surface toward the camera, at a lead's chest height (a heavy blow's: the heavy chips).
		const bool bHeavy = Cue.cue == ftcore::ShowCue::ChipsHeavy;
		FVector Toward = ShowCamera ? ShowCamera->GetActorLocation() - Feet : Figure.GetActorForwardVector();
		Toward.Z = 0.0;
		Toward = Toward.GetSafeNormal();
		const double Out = static_cast<double>(Fight.def().enemies[Slot].bodyRadius * Tune.Surface) * 100.0;
		FVector At = Feet + Toward * Out;
		At.Z = PlayerFigure ? PlayerFigure->GetChest().Z : Feet.Z + 120.0;
		BossVfx.Play(bHeavy ? FFTArenaBossVfx::EEffect::ChipsHeavy : FFTArenaBossVfx::EEffect::Chips, At, 1.f, bHeavy ? TEXT("heavy hit") : TEXT("hit"));
		return;
	}
	case ftcore::ShowCue::GlowOn:
		BossVfx.SetGlow(true, BossFlashPoint());
		return;
	case ftcore::ShowCue::GlowOff:
		BossVfx.SetGlow(false, FVector::ZeroVector);
		return;
	case ftcore::ShowCue::Crit:
		BossVfx.Play(FFTArenaBossVfx::EEffect::Crit, BossFlashPoint(), 1.f, TEXT("weak point crit"));
		return;
	}
}
