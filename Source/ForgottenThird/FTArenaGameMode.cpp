#include "FTArenaGameMode.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "FTActionButtons.h"
#include "FTArenaFighter.h"
#include "FTArenaHUD.h"
#include "FTPauseSubsystem.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include <cmath>
#include <string>
#include <vector>

static_assert(AFTArenaGameMode::MaxEnemies == 6, "EnemyFigures[6] in FTArenaGameMode.h must follow ftcore::kMaxArenaEnemies");
static_assert(AFTArenaFighter::TellSegments == 14, "Tell[14] in FTArenaFighter.h must follow TellSegments");

namespace
{
// The shortest tell the arena allows (s): an enemy's startup is its telegraph.
constexpr float kArenaMinTell = 0.4f;
// After the outcome, the autopilot (without -ArenaQuitOnFinish) fights again this much later (s).
constexpr double kArenaAutoAgain = 4.0;
// A press to fight again counts only once the result has been up this long (s), so a held attack doesn't skip it.
constexpr double kArenaAgainDelay = 0.6;
// How long a hit tints a body (s).
constexpr double kArenaHitTint = 0.16;
// The floor tell's flash after a move goes active (s).
constexpr double kArenaTellFlash = 0.3;
// Camera: pitch limits, the pitch while locked, how fast it turns to a lock, stick and mouse rates.
constexpr float kArenaPitchMin = -50.f;
constexpr float kArenaPitchMax = 10.f;
constexpr float kArenaLockPitch = -20.f;
constexpr float kArenaLockTurn = 7.f;
constexpr float kArenaStickTurn = 140.f; // degrees a second at full stick
constexpr float kArenaMouseTurn = 2.5f; // degrees per unit of mouse axis (as the engine's yaw scale)
// A right-stick flick switches the lock: past this, after returning under the rest.
constexpr float kArenaFlickOut = 0.7f;
constexpr float kArenaFlickRest = 0.3f;
// After a view is taken, the fight holds this long (wall seconds) so the frame it renders is that moment.
constexpr double kArenaShotHold = 1.0;
// A -ArenaShot run gives up this long after the first view (wall seconds).
constexpr double kArenaShotGiveUp = 200.0;

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
} // namespace

FVector AFTArenaGameMode::PlayerStartFor(float Radius)
{
	return FVector(-0.35 * static_cast<double>(Radius) * 100.0, 0.0, 0.0);
}

AFTArenaGameMode::AFTArenaGameMode()
{
	HUDClass = AFTArenaHUD::StaticClass();
	DefaultPawnClass = AFTArenaFighter::StaticClass();
	bHostsStory = false;
	PrimaryActorTick.bCanEverTick = true;
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		ActiveSince[I] = -1e9;
		EnemyHitTime[I] = -1e9;
	}
}

void AFTArenaGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as the other fight screens do: the story must not start here.
	AGameModeBase::StartPlay();

	const TCHAR* Cmd = FCommandLine::Get();
	FString Shot;
	if (FParse::Value(Cmd, TEXT("ArenaShot="), Shot) && !Shot.IsEmpty())
	{
		ShotPrefix = Shot;
		float Delay = ShotDelay;
		if (FParse::Value(Cmd, TEXT("ArenaShotDelay="), Delay))
		{
			ShotDelay = FMath::Clamp(Delay, 0.f, 60.f);
		}
	}
	bIdle = FParse::Param(Cmd, TEXT("ArenaIdle"));
	bAutoPlay = !bIdle && (FParse::Param(Cmd, TEXT("ArenaAutoPlay")) || !ShotPrefix.IsEmpty());
	bQuitOnFinish = FParse::Param(Cmd, TEXT("ArenaQuitOnFinish"));
	bHoldLoad = FParse::Param(Cmd, TEXT("ArenaHoldLoad"));
	int32 SeedValue = 1;
	if (FParse::Value(Cmd, TEXT("ArenaSeed="), SeedValue))
	{
		Seed = static_cast<uint32>(SeedValue);
	}

	BuildDef(LoadTable());

	// The enemies' tells: every startup at least kArenaMinTell (the data's loader doesn't hold it to that).
	float Shortest = 1e9f;
	for (int32 E = 0; E < static_cast<int32>(Def.enemyCount); ++E)
	{
		const ftcore::ArenaEnemyKind& Kind = Def.enemies[static_cast<size_t>(E)];
		for (int32 M = 0; M < static_cast<int32>(Kind.moveCount); ++M)
		{
			Shortest = FMath::Min(Shortest, Kind.moves[static_cast<size_t>(M)].attack.startup);
		}
	}
	if (Shortest < kArenaMinTell)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: an enemy move's tell is %.2f s, under the %.1f s floor."), Shortest, kArenaMinTell);
	}

	FString Kinds;
	for (int32 E = 0; E < static_cast<int32>(Def.enemyCount); ++E)
	{
		const uint8 Kind = Def.enemyKind[static_cast<size_t>(E)];
		Kinds += E > 0 ? TEXT(", ") : TEXT("");
		Kinds += Kind < Table.enemyIds.size() ? FT::ToFString(Table.enemyIds[Kind]) : FString(TEXT("built-in"));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: %d enemies (%s), ring %.1f m, %d attacking at once, seed %u, %s; shortest tell %.2f s."),
		static_cast<int32>(Def.enemyCount), *Kinds, Def.ringRadius, static_cast<int32>(Def.attackers), Seed,
		ArenaPilotName(bAutoPlay, bIdle), Shortest);
}

bool AFTArenaGameMode::LoadTable()
{
	const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/arena.json");
	if (!IFileManager::Get().FileExists(*File))
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: no Data/Battle/arena.json; using the built-in fight."));
		return false;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: couldn't read Data/Battle/arena.json; using the built-in fight."));
		return false;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ftcore::importArenaTable(FT::BytesAsText(Bytes), Table, &Error, &Warnings))
	{
		// The loader's errors name paths and keys only.
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: Data/Battle/arena.json refused; using the built-in fight:\n%s"), *FT::ToFString(Error));
		return false;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s"), *FT::ToFString(W));
	}
	return true;
}

void AFTArenaGameMode::BuildDef(bool bTable)
{
	Def = bTable ? Table.fallback : ftcore::defaultArenaDef();
	if (!bTable)
	{
		return;
	}

	FString Lineup;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaLineup="), Lineup, false) && !Lineup.IsEmpty())
	{
		TArray<FString> Ids;
		Lineup.ParseIntoArray(Ids, TEXT(","), true);
		int32 Count = 0;
		for (const FString& Id : Ids)
		{
			const std::string Want = FT::ToUtf8(Id.TrimStartAndEnd());
			int32 Found = -1;
			for (int32 K = 0; K < static_cast<int32>(Table.enemyIds.size()); ++K)
			{
				if (Table.enemyIds[static_cast<size_t>(K)] == Want)
				{
					Found = K;
					break;
				}
			}
			if (Found < 0)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaLineup: no enemy kind \"%s\" in arena.json; skipped."), *Id);
				continue;
			}
			if (Count >= MaxEnemies)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaLineup: more than %d enemies; the rest are left out."), MaxEnemies);
				break;
			}
			Def.enemies[static_cast<size_t>(Count)] = Table.enemies[static_cast<size_t>(Found)];
			Def.enemyKind[static_cast<size_t>(Count)] = static_cast<uint8>(Found);
			++Count;
		}
		if (Count > 0)
		{
			Def.enemyCount = static_cast<uint8>(Count);
			Def.attackers = 1;
		}
	}
	int32 Attackers = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaAttackers="), Attackers))
	{
		Def.attackers = static_cast<uint8>(FMath::Clamp(Attackers, 1, static_cast<int32>(Def.enemyCount)));
	}
	Def.ringRadius = FMath::Min(Def.ringRadius, MapRingRadius);
}

APlayerController* AFTArenaGameMode::GetPC() const
{
	return GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
}

void AFTArenaGameMode::Begin()
{
	bBegun = true;
	BeginNow = Now;

	// The ring: Def's radius around the map's centre (the player starts behind it), inside the map's wall.
	const FVector Start = PlayerStartFor(Def.ringRadius);
	const ftcore::Vec2 PlayerPos{static_cast<float>(Start.X / 100.0), static_cast<float>(Start.Y / 100.0)};
	ftcore::ArenaRing Bounds;
	Bounds.centre = ftcore::Vec2{0.f, 0.f};
	Bounds.radius = MapRingRadius;
	const ftcore::ArenaRing Ring = ftcore::arenaRingAt(PlayerPos, ftcore::Vec2{1.f, 0.f}, Def.ringRadius, Bounds);
	Fight.begin(Def, Ring, PlayerPos, ftcore::Vec2{1.f, 0.f}, Seed);

	SpawnFigures();
	PutBodiesAtStart();
	PlaceFigures(1.f);
}

void AFTArenaGameMode::SpawnFigures()
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

	// The player's figure is the pawn the controller has (DefaultPawnClass), else one made and possessed here.
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
				Old->Destroy();
			}
			PC->Possess(PlayerFigure);
		}
	}
	if (PlayerFigure)
	{
		PlayerFigure->Setup(true, Def.fighter.bodyRadius, kArenaPlayerColor);
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
		Enemy->Setup(false, Def.enemies[static_cast<size_t>(I)].bodyRadius, Color);
		EnemyFigures[I] = Enemy;
	}
}

void AFTArenaGameMode::PutBodiesAtStart()
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
	PlayerHitTime = -1e9;
	ParryTime = -1e9;
	ReleaseTime = -1e9;
	ReleaseSpent = 0;
	OverloadTime = -1e9;
	bPressLight = bPressHeavy = bPressDodge = bPressHeal = bPressLock = bPressRelease = bPressPair = bPartnerDue = false;
	PressSwitch = 0;
	CameraYaw = ArenaYaw(Fight.playerStartFacing());
	CameraPitch = -16.f;
}

void AFTArenaGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bBegun)
	{
		Begin();
	}

	const bool bHold = Shots(DeltaSeconds);
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
		else if (ShotPrefix.IsEmpty() && !bQuitOnFinish)
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
		UE_LOG(LogForgottenThird, Warning, TEXT("ARENA RESULT: Timeout after %.1f s (%s, seed %u): the fight never ended."),
			Fight.time(), ArenaPilotName(bAutoPlay, bIdle), Seed);
		bQuitOnFinish = false;
		FPlatformMisc::RequestExitWithStatus(false, 2, TEXT("Arena"));
	}
}

void AFTArenaGameMode::PollInput(float DeltaSeconds)
{
	APlayerController* PC = GetPC();
	if (!PC || bAutoPlay || bIdle || UFTPauseSubsystem::IsBlocking(this) || UFTPauseSubsystem::IsJustResumed(this))
	{
		bGuardHeld = false;
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
	bPressHeal |= ArenaAnyJustPressed(*PC, EKeys::H, EKeys::Gamepad_DPad_Up);
	bPressLock |= ArenaAnyJustPressed(*PC, EKeys::Tab, EKeys::MiddleMouseButton, EKeys::Gamepad_RightThumbstick);
	bPressRelease |= ArenaAnyJustPressed(*PC, EKeys::R, EKeys::Gamepad_LeftTrigger);
	bPressPair |= ArenaAnyJustPressed(*PC, EKeys::F, EKeys::Gamepad_DPad_Down);
	bGuardHeld = PC->IsInputKeyDown(Key(GuardB, 0)) || PC->IsInputKeyDown(Key(GuardB, 1)) || PC->IsInputKeyDown(EKeys::RightMouseButton)
		|| PC->IsInputKeyDown(EKeys::Gamepad_LeftShoulder);
	if (ArenaAnyJustPressed(*PC, EKeys::Q, EKeys::Gamepad_DPad_Left))
	{
		PressSwitch = -1;
	}
	if (ArenaAnyJustPressed(*PC, EKeys::E, EKeys::Gamepad_DPad_Right))
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

ftcore::ArenaInput AFTArenaGameMode::TakeInput()
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
	// Each press goes to one step.
	bPressLight = bPressHeavy = bPressDodge = bPressHeal = bPressLock = bPressRelease = bPressPair = false;
	PressSwitch = 0;
	return In;
}

void AFTArenaGameMode::StepFight()
{
	ftcore::ArenaInput In;
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
	}
	else if (!bIdle)
	{
		In = TakeInput();
	}
	PrevBodies = Bodies;
	const ftcore::ArenaOutput& Out = Fight.tick(StepSeconds, In, Bodies);
	ftcore::arenaStep(Fight, Bodies, StepSeconds);
	HandleEvents(Out.events);
	TrackParts(Out);
}

void AFTArenaGameMode::HandleEvents(const ftcore::ArenaEvents& Events)
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
				EnemyHitTime[E.target] = Now;
			}
			break;
		case ftcore::ArenaEventType::Parried:
			ParryTime = Now;
			bPartnerDue = bAutoPlay;
			break;
		case ftcore::ArenaEventType::CounterStagger:
			if (E.target < MaxEnemies)
			{
				EnemyHitTime[E.target] = Now;
			}
			break;
		case ftcore::ArenaEventType::Released:
			ReleaseTime = Now;
			ReleaseSpent = E.amount;
			continue; // a banner, not a popup
		case ftcore::ArenaEventType::Overloaded:
			OverloadTime = Now;
			continue; // a banner, not a popup
		case ftcore::ArenaEventType::AttackStarted:
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

void AFTArenaGameMode::TrackParts(const ftcore::ArenaOutput& Out)
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
			LastPart[I] = Part;
		}
	}
}

const ftcore::ArenaEnemyMove* AFTArenaGameMode::GetEnemyMove(int32 Index) const
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

void AFTArenaGameMode::PlaceFigures(float Alpha)
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
		const ftcore::ArenaEnemyOut& E = Out.enemies[static_cast<size_t>(I)];
		const ftcore::ArenaEnemyKind& Kind = D.enemies[static_cast<size_t>(I)];
		FFTArenaPose Pose;
		Pose.Feet = ToWorld(ArenaLerp(PrevBodies.enemies[static_cast<size_t>(I)], Bodies.enemies[static_cast<size_t>(I)], Alpha));
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
	}
}

void AFTArenaGameMode::SteerCamera(float DeltaSeconds)
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
		CameraPitch = FMath::FInterpTo(CameraPitch, kArenaLockPitch, DeltaSeconds, 4.f);
	}
	CameraYaw = FMath::UnwindDegrees(CameraYaw);
	PC->SetControlRotation(FRotator(CameraPitch, CameraYaw, 0.0));
}

void AFTArenaGameMode::Finish()
{
	bResultLogged = true;
	ResultTime = Now;
	const bool bWon = Fight.phase() == ftcore::ArenaPhase::Victory;
	const ftcore::ArenaStats& S = Fight.stats();
	UE_LOG(LogForgottenThird, Display,
		TEXT("ARENA RESULT: %s in %.1f s (%s, seed %u, %d enemies, hp %d/%d, %d heals left; %d hits, %d taken, %d blocks, %d parries, %d ripostes, %d evades, %d staggers, %d guard breaks, %d heals; %d load stored, %d releases, %d overloads, %d counter staggers; dealt %d, took %d)."),
		bWon ? TEXT("Victory") : TEXT("Defeat"), Fight.time(), ArenaPilotName(bAutoPlay, bIdle), Seed,
		static_cast<int32>(Fight.enemyCount()), Fight.hp(), Fight.def().fighter.hp, static_cast<int32>(Fight.healCharges()),
		S.hits, S.hitsTaken, S.blocks, S.parries, S.ripostes, S.evades, S.staggers, S.guardBreaks, S.heals, S.loadStored, S.releases, S.overloads, S.counterStaggers,
		S.damageDealt, S.damageTaken);
	if (bQuitOnFinish && ShotPrefix.IsEmpty())
	{
		FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(bWon ? 0 : 1), TEXT("Arena"));
	}
}

void AFTArenaGameMode::Restart()
{
	Fight.restart();
	bResultLogged = false;
	PutBodiesAtStart();
	BeginNow = Now;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: fight again."));
}

bool AFTArenaGameMode::Shots(float DeltaSeconds)
{
	if (ShotPrefix.IsEmpty())
	{
		return false;
	}
	ShotWall += DeltaSeconds;
	if (ShotWall < ShotHoldUntil)
	{
		return true;
	}
	switch (ShotsTaken)
	{
	case 0:
		// The ring before anyone moves.
		if (ShotWall < ShotDelay)
		{
			return true;
		}
		TakeShot(TEXT("1_ring"));
		return true;
	case 1:
		// An enemy winding up, its tell most of the way full, with the lock on it.
		for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()); ++I)
		{
			const ftcore::ArenaEnemyOut& E = Fight.output().enemies[static_cast<size_t>(I)];
			const ftcore::ArenaEnemyMove* Move = GetEnemyMove(I);
			if (Move && E.part == ftcore::ArenaPart::Startup && Fight.lock() != ftcore::kArenaNobody
				&& E.stateTime >= Move->attack.startup * 0.65f)
			{
				TakeShot(TEXT("2_tell"));
				return true;
			}
		}
		break;
	case 2:
		// The moment a move lands: the flash at impact (or a parry).
		for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()); ++I)
		{
			if (Now - ActiveSince[I] < 0.05 || Now - ParryTime < 0.05)
			{
				TakeShot(TEXT("3_impact"));
				return true;
			}
		}
		break;
	case 3:
		// The Load gauge past the Release's threshold, a moment after the parry that filled it (its +LOAD and the
		// partner's COUNTER still up); skipped if the fight ends first.
		// Under -ArenaHoldLoad, the first frame overloaded (the autopilot releases at once when in reach).
		if (bHoldLoad ? Fight.overloaded()
					  : (Fight.load() > ftcore::kArenaReleaseOpenLoad + 10 && Now - ParryTime >= 0.15 && Now - ParryTime < 0.5))
		{
			TakeShot(TEXT("4_load"));
			return true;
		}
		if (bResultLogged)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: the fight ended before the load view; no load or release view."));
			ShotsTaken += 2;
		}
		break;
	case 4:
		// Just after a Release began: its banner and its pose (skipped if the fight ends first).
		if (Now - ReleaseTime >= 0.12 && Now - ReleaseTime < 0.4)
		{
			TakeShot(TEXT("5_release"));
			return true;
		}
		if (bResultLogged)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: no Release in this fight; no release view."));
			++ShotsTaken;
		}
		break;
	case 5:
		if (bResultLogged && GetResultAge() >= 0.8)
		{
			TakeShot(TEXT("6_result"));
			return true;
		}
		break;
	default:
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: views taken; quitting."));
		ShotPrefix.Reset();
		FPlatformMisc::RequestExitWithStatus(false, 0, TEXT("Arena"));
		return true;
	}
	if (ShotWall > ShotDelay + kArenaShotGiveUp)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: only %d of 6 views came; quitting."), ShotsTaken);
		ShotPrefix.Reset();
		FPlatformMisc::RequestExitWithStatus(false, 2, TEXT("Arena"));
		return true;
	}
	return false;
}

void AFTArenaGameMode::TakeShot(const TCHAR* Name)
{
	const FString File = FString::Printf(TEXT("%s_%s.png"), *ShotPrefix, Name);
	// The game viewport handles HighResShot; the engine's own Exec doesn't.
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *File), *GLog);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: view %s."), *File);
	++ShotsTaken;
	ShotHoldUntil = ShotWall + kArenaShotHold;
}
