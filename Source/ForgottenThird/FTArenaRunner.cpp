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
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"

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
// After the outcome, the autopilot (without -ArenaQuitOnFinish) fights again this much later (s).
constexpr double kArenaAutoAgain = 4.0;
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
	bBossSplitAtStart = Setup.bBossSplitAtStart;
	bQuitOnFinish = Setup.bQuitOnFinish;
	bShots = Setup.bShots;
	bFightAgain = Setup.bFightAgain;
	bReplacePawn = Setup.bReplacePawn;

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
	if (bBossSplitAtStart)
	{
		SplitBossCarried();
	}
	SpawnKindCritEffects();
	PutBodiesAtStart();
	PlaceFigures(1.f);
	if (KiteSlot >= 0)
	{
		KiteFx.Spawn(EnemyFigures[KiteSlot].Get());
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
			bool bAgain = bAutoPlay && GetResultAge() >= kArenaAutoAgain;
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
	if (Skin.Walk >= 0)
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
		}
		else if (!bKnees && bKneeling[I] && UpAt[I] < UpLength)
		{
			// The spell ended before its get-up ran out (a release from a hold): it still gets up, from where it got to.
			UpOwedUntil[I] = Now + (UpLength - UpAt[I]);
		}
		bKneeling[I] = bKnees;
	}
	bool bOwedUp = false;
	switch (Out.brain)
	{
	case ftcore::ArenaBrain::Dead:
		Clip = bKneeling[I] && Skin.DefeatKneel >= 0 ? Skin.DefeatKneel : (Skin.Defeat >= 0 ? Skin.Defeat : Skin.Idle);
		bHoldRoot = false; // it falls
		break;
	case ftcore::ArenaBrain::Staggered:
		Clip = Skin.Stagger >= 0 ? Skin.Stagger : Skin.Idle;
		break;
	case ftcore::ArenaBrain::Open:
	case ftcore::ArenaBrain::Down:
	{
		if (!bKnees)
		{
			// Its own clip where it has one: a parried enemy is open for openTime, and a riposte cuts it to the face-down clip.
			Clip = Skin.Open;
			break;
		}
		// On its knees: it goes down from the spell's start (an Open chained into a Down is one spell), holds there, and
		// gets up so that it stands as the state ends (never early in a hold until a release). The get-up wins a short state.
		Clip = Skin.FaceDown >= 0 ? Skin.FaceDown : (Skin.Stagger >= 0 ? Skin.Stagger : Skin.Idle);
		const double Spell = Now - KneelSince[I];
		const double Left = static_cast<double>(Out.stateLength) - (static_cast<double>(Out.stateTime) + static_cast<double>(Alpha) * StepSeconds);
		if (Skin.FaceDownOut >= 0 && FMath::IsFinite(Left) && Left < UpLength)
		{
			Clip = Skin.FaceDownOut;
			Time = UpLength - FMath::Max(Left, 0.0);
			bTimed = true;
			UpAt[I] = Time;
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
		const int32 MoveClip = Out.move < Kind.moveCount ? Skin.MoveClip[Out.move] : -1;
		if (MoveClip >= 0)
		{
			// The clip's impact lands at the end of the move's startup: Core's own clock for the move, at that rate.
			const ftcore::BeatClip& Row = (*Skin.Clips)[static_cast<size_t>(MoveClip)];
			const float Startup = Kind.moves[Out.move].attack.startup;
			const float Rate = (Row.impact > 0.0 && Startup > 0.f) ? FMath::Clamp(static_cast<float>(Row.impact) / Startup, kArenaMinClipRate, kArenaMaxClipRate) : 1.f;
			Clip = MoveClip;
			Time = (static_cast<double>(Out.stateTime) + static_cast<double>(Alpha) * StepSeconds) * Rate;
			bTimed = true;
			MoveRate = Rate;
		}
		break;
	}
	default:
	{
		// Between moves: the rest of a get-up still owed, else a hit reaction while it lasts, else the walk while it moves,
		// else the idle (the second one once it is hurt).
		const int32 Hit = bHitHeavy[I] ? Skin.HitHeavy : Skin.HitLight;
		if (Now < UpOwedUntil[I])
		{
			Clip = Skin.FaceDownOut;
			Time = UpLength - (UpOwedUntil[I] - Now);
			bTimed = true;
			bOwedUp = true;
		}
		else if (Hit >= 0 && Now - HitSince[I] < static_cast<double>(Skin.Sequences[Hit]->GetPlayLength()))
		{
			Clip = Hit;
			Time = Now - HitSince[I];
			bTimed = true;
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
	if (!bOwedUp)
	{
		UpOwedUntil[I] = -1e9; // anything else (a move, a stagger, a death) takes it from the get-up for good
	}
	if (Clip != SkinClip[I])
	{
		SkinClip[I] = Clip;
		SkinSince[I] = Now;
		if (MoveRate > 0.f)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: enemy %d plays %s for move %d at rate %.2f (%.2f s)."), I,
				*Skin.Sequences[Clip]->GetName(), static_cast<int32>(Out.move), MoveRate, Skin.Sequences[Clip]->GetPlayLength());
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
	// A loop wraps; anything else holds its last frame. The crossfade is the table's own when it states one; a clip the
	// table marks additive_in (a short hit reaction) lands at once on the pose the blow finds.
	const bool bLoop = Row.loop && !bTimed;
	const float At = bLoop && Length > 0.f ? FMath::Fmod(static_cast<float>(Time), Length) : FMath::Clamp(static_cast<float>(Time), 0.f, Length);
	Figure->PlayClip(Sequence, At, bLoop, FrameDelta, bHoldRoot, static_cast<float>(Skin.Clips->blend), Row.additiveIn);
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
		HitSince[I] = -1e9;
		WalkSpeed[I] = 0.f;
		bWalking[I] = false;
		WalkPhase[I] = 0.0;
		bKneeling[I] = false;
		KneelSince[I] = 0.0;
		UpAt[I] = 0.0;
		UpOwedUntil[I] = -1e9;
	}
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
		if (bHoldLoad && !Fight.overloaded())
		{
			In.release = false;
		}
		// The boss's own buttons, which the autopilot knows nothing of.
		StoneBearer.Assist(In, bIntercept, Bodies);
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
	}
	PrevBodies = Bodies;
	const ftcore::ArenaOutput& Out = Fight.tick(StepSeconds, In, Bodies);
	ftcore::arenaStep(Fight, Bodies, StepSeconds, Fit.zone);
	if (bBoss && StoneBearer.AfterTick(Out.events, Bodies))
	{
		// Onto its plinth: put there, not walked there.
		PrevBodies.enemies[StoneBearer.GetSlot()] = Bodies.enemies[StoneBearer.GetSlot()];
	}
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
	const bool bGlow = bBoss && StoneBearer.CritOpen();
	if (bGlow != BossVfx.IsGlowing())
	{
		BossVfx.SetGlow(bGlow, bGlow ? BossWeakPoint() : FVector::ZeroVector);
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
				BossHitVfx(E);
				EnemyHitTime[E.target] = Now;
				// A heavy blow, a riposte or a Release is a heavy reaction.
				if (Flinches(E.target))
				{
					HitSince[E.target] = Now;
					bHitHeavy[E.target] = E.move == ftcore::kArenaHeavyMove || E.move == ftcore::kArenaRiposteMove || E.move == ftcore::kArenaReleaseMove;
				}
			}
			break;
		case ftcore::ArenaEventType::Parried:
			ParryTime = Now;
			bPartnerDue = bAutoPlay;
			break;
		case ftcore::ArenaEventType::Staggered:
			// Logged for the stagger spacing checks (tuning poise from -nullrhi runs).
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: enemy %d staggered."), Now, static_cast<int32>(E.target));
			break;
		case ftcore::ArenaEventType::CounterStagger:
			if (E.target < MaxEnemies)
			{
				BossHitVfx(E);
				EnemyHitTime[E.target] = Now;
				if (Flinches(E.target))
				{
					HitSince[E.target] = Now;
					bHitHeavy[E.target] = true;
				}
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
				BossHitVfx(E);
				EnemyHitTime[E.target] = Now;
				if (Flinches(E.target))
				{
					HitSince[E.target] = Now;
					bHitHeavy[E.target] = false;
				}
			}
			break;
		case ftcore::ArenaEventType::AttackStarted:
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
	FFTStoneBearerStrike S;
	if (!StoneBearer.StrikeOf(Out.move, Bodies, T.CalledAhead, T.CalledSide, T.ReachShare, S))
	{
		return;
	}
	const bool bArea = S.Shape == FFTStoneBearerStrike::EShape::Area;
	const float ShapeMul = bArea ? T.AreaScale : 1.f;
	const double Lift = T.Lift * 100.0;
	const FVector At = ToWorld(S.To) + FVector(0.0, 0.0, Lift);
	if (To == Active)
	{
		BossVfx.Play(FFTArenaBossVfx::EEffect::Impact, At, ShapeMul, TEXT("blow lands"));
		return;
	}
	if (S.Shape != FFTStoneBearerStrike::EShape::Lane)
	{
		BossVfx.Play(FFTArenaBossVfx::EEffect::Tell, At, ShapeMul, bArea ? TEXT("tell all round") : TEXT("tell"));
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

void UFTArenaRunner::BossHitVfx(const ftcore::ArenaEvent& E)
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
	FVector At = ToWorld(ftcore::Vec2{Boss.x + Dir.x * Out, Boss.y + Dir.y * Out});
	At.Z = PlayerFigure ? PlayerFigure->GetChest().Z : At.Z + 100.0;
	BossVfx.Play(FFTArenaBossVfx::EEffect::Chips, At, 1.f, TEXT("hit"));
}

void UFTArenaRunner::SplitBossCarried()
{
	AFTArenaFighter* Figure = StoneBearerSlot >= 0 ? EnemyFigures[StoneBearerSlot].Get() : nullptr;
	if (!Figure || !Figure->IsSkinned() || Figure->IsSplit())
	{
		return;
	}
	FName HideSlot;
	TArray<FFTCarriedPiece> Pieces;
	if (ReadCarriedSplit(kStoneBearerSplit, HideSlot, Pieces) && Figure->SplitCarried(HideSlot, Pieces))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: boss %.2f: the carried piece split into %d parts on its bones."), Now, Pieces.Num());
	}
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
		// The boss on its plinth, or a refused pick being cut, is held Down but drawn on its feet.
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
		BossVfx.MoveGlow(BossWeakPoint());
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
	LogResult();
	if (bQuitOnFinish && !bShots)
	{
		FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(bWon ? 0 : 1), TEXT("Arena"));
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
		SplitBossCarried();
	}
	BeginKite();
	BeginKindCrits();
	bResultLogged = false;
	PutBodiesAtStart();
	BeginNow = Now;
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
				SplitBossCarried();
			}
			if (E.type == ftcore::ArenaBossEventType::Crit)
			{
				BossVfx.Play(FFTArenaBossVfx::EEffect::Crit, BossWeakPoint(), 1.f,
					E.value > 0 ? TEXT("weak point crit, first of the opening") : TEXT("weak point crit"));
			}
			if (E.type == ftcore::ArenaBossEventType::Answered)
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
			UE_LOG(LogForgottenThird, Verbose, TEXT("Arena: boss %.2f move %d refused and cut"), E.at, Move);
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
