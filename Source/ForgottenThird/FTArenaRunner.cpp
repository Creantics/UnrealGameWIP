#include "FTArenaRunner.h"

#include "FTCrowdSubsystem.h"

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
#include "FTAudioSubsystem.h"
#include "FTArenaJoltModifier.h"
#include "FTBeatBossMetronome.h"
#include "FTSettingsSubsystem.h"
#include "Components/AudioComponent.h"
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "FTGameInstance.h"
#include "FTInputBindings.h"
#include "FTPauseSubsystem.h"
#include "FTStoryStateSubsystem.h"
#include "FTText.h"
#include "FTVfxComponent.h"
#include "ForgottenThird.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"

#include "ftcore/Brief.h"
#include "ftcore/UiStyle.h"
#include "ftcore/BriefKit.h"
#include "ftcore/LicenceRating.h"
#include "ftcore/Chairs.h"
#include "ftcore/CountLiftData.h"
#include "ftcore/FieldStrike.h"
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
// A press to fight again counts only once the result has been up this long (s), so a held attack doesn't skip it.
constexpr double kArenaAgainDelay = 0.6;
// The boss break's tell on the body (Core's enemyBreakTell): a warm white glow added to its materials' Emissive, pulsing
// at this rate (Hz) from 0 up to this much and back, from the tick the tell rises; off the tick the break fires.
constexpr double kBreakTellPulseHz = 1.0;
constexpr float kBreakTellGlow = 3.f;
// How long a hit tints a body (s).
constexpr double kArenaHitTint = 0.16;
// The floor tell's flash after a move goes active (s).
constexpr double kArenaTellFlash = 0.3;
// A boss at least this tall (m) is looked up at: the camera's pitch rises toward its face, at this share of its height
// (hover included), by ftcore::arenaLookUpPitch (the boom held at its limit, the lens tilting the rest).
constexpr float kArenaTallBoss = ftcore::kArenaLookTallBoss;
constexpr float kArenaFaceHeight = ftcore::kArenaLookFaceShare;
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
// speed along Core's wanted direction while it plays; Core's speed only starts and stops it. Once begun it lasts at least
// kStoneWalkHold seconds (unless the brain leaves Approach/Strafe or a reaction starts); it starts only while the wanted
// direction is within the cone about the facing (its cosine; outside it, it stands and turns), and it starts
// kStoneWalkStartPhase seconds into the clip (the walk's frame nearest the idle's first pose: frame 25 of 80 at 30 fps).
constexpr double kStoneWalkHold = 1.0;
constexpr float kStoneWalkCone = 0.5f;
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
// The stone_bearer's stepping turn (its slot only, skinned, with turn_l and turn_r): standing between moves, Core never turns
// it on the spot. When the lead is kStoneTurnStart degrees or more off its facing, it doesn't walk and its idle has faded in,
// it steps round in cycles of kStoneTurnCycle seconds, each turning it kStoneTurnDeg on the clips' own curve (smootherstep
// over kStoneTurnPivot, then held while the pivot foot catches up), and at each cycle's end it goes on while the lead is
// still more than kStoneTurnResidual degrees off that way and the walk doesn't want to start. The figure's yaw follows the
// curve, scaled by the crossfade into the clip, so the pivot foot stays where it stood. (The clips' table can't carry these:
// its rows take no unknown keys.)
constexpr float kStoneTurnDeg = 30.f;
constexpr double kStoneTurnPivot = 0.35;
constexpr double kStoneTurnCycle = 0.8;
constexpr float kStoneTurnStart = 20.f;
constexpr float kStoneTurnResidual = 15.f;
// Camera: the pitch limits are ftcore::kArenaLook's (near straight up; the boom stops at its boomMax and the lens
// tilts the rest); how fast it turns to a lock, stick and mouse rates.
constexpr float kArenaLockTurn = 7.f; // (the stick and mouse rates: ftcore::kArenaStickTurn and kArenaMouseTurn)
// A right-stick flick switches the lock: past this, after returning under the rest.
constexpr float kArenaFlickOut = 0.7f;
constexpr float kArenaFlickRest = 0.3f;
// Locked on, a sideways mouse move past this in one frame switches the target; under the rest it re-arms
// (provisional, to tune on a run).
constexpr float kArenaMouseFlickOut = 8.f;
constexpr float kArenaMouseFlickRest = 2.f;

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
	Session = CreateDefaultSubobject<UFTFightSession>(TEXT("FightSession"));
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

bool UFTArenaRunner::JumpBound()
{
#if UE_BUILD_SHIPPING
	return false;
#else
	static const bool bOn = FParse::Param(FCommandLine::Get(), TEXT("ArenaJump"));
	return bOn;
#endif
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
	bNoLock = Setup.bNoLock;
	if (bNoLock && bAutoPlay)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: the autopilot never locks (-ArenaNoLock): its attacks take soft targets."));
	}
	FTInputBindings::LogFocusGate(TEXT("Arena"));
	if (!FTInputBindings::UnboundLine().IsEmpty())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s; choose a key for each in Saved/InputBindings.json."), *FTInputBindings::UnboundLine());
	}
	bAutoShoot = Setup.bAutoShoot && bAutoPlay;
	Fight.setAutopilotShoots(bAutoShoot);
	if (bAutoShoot)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: the autopilot aims and shoots on the way in with a guns kit (-ArenaAutoShoot)."));
	}
	// -ArenaCameraShake=<0..1>: the jolt's scale for this run, over the player's setting (headless runs read no settings).
	ShakeOverride = -1.f;
	float Shake = 1.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaCameraShake="), Shake))
	{
		ShakeOverride = FMath::IsFinite(Shake) ? FMath::Clamp(Shake, 0.f, 1.f) : 0.f;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: camera shake %.2f for this run (-ArenaCameraShake)."), ShakeOverride);
	}
	GlideStick = -1;
#if !UE_BUILD_SHIPPING
	// A measuring probe: never in a shipping build (it would steer the autopilot's stick).
	FString GlideWord;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaGlide="), GlideWord))
	{
		GlideStick = GlideWord == TEXT("toward") ? 0 : GlideWord == TEXT("across") ? 1 : GlideWord == TEXT("away") ? 2
			: GlideWord == TEXT("pulse") ? 4 : GlideWord == TEXT("orbitguard") ? 5 : GlideWord == TEXT("rollstrike") ? 6 : GlideWord == TEXT("backstep") ? 7
			: GlideWord == TEXT("sidestep") ? 8 : GlideWord == TEXT("jump") ? 9 : GlideWord == TEXT("jumpmove") ? 10
			: GlideWord == TEXT("jumpstrike") ? 11 : 3;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: glide probe on, the stick %s (-ArenaGlide)."),
			GlideStick == 0 ? TEXT("held toward the target") : GlideStick == 1 ? TEXT("held across") : GlideStick == 2 ? TEXT("held away")
			: GlideStick == 4 ? TEXT("pulsed: toward, let go, away, let go") : GlideStick == 5 ? TEXT("held across with the guard up")
			: GlideStick == 6 ? TEXT("the autopilot's, a light attack on the first free frame after each roll")
			: GlideStick == 7 ? TEXT("the autopilot's, let go for each roll (a backstep under the lock)")
			: GlideStick == 8 ? TEXT("the autopilot's, across the lock for each roll, right then left (a side dash)")
			: GlideStick == 9 ? TEXT("let go: standing jumps only")
			: GlideStick == 10 ? TEXT("held across: moving jumps only")
			: GlideStick == 11 ? TEXT("let go: standing jumps, each with an air light") : TEXT("the autopilot's"));
	}
#endif
	Glide.Begin(GlideStick >= 0);
#if !UE_BUILD_SHIPPING
	AimCheckPhase = 0;
	AimCheckSlot = -1;
	AimCheckPresses = 0;
	AimCheckLine.Reset();
	bAimCheckPass = false;
	bAimCheckNa = false;
	bAimCheckOnBoss = false;
	bAimCheckPitchOk = false;
	bAimCheckFire = false;
	bAssistCheck = false;
	AssistStep = AssistSub = 0;
	AssistMarkAt = -1e9;
	AssistSweep = 1.f;
	AssistKeep = 1.f;
	bAssistAimOff = bAssistLockPress = false;
	AssistReturns = 0;
	AssistFail.Reset();
	AssistEnters = AssistExits = 0;
	bCoverCheck = FParse::Param(FCommandLine::Get(), TEXT("ArenaCoverCheck"));
	bFrameCheck = FParse::Param(FCommandLine::Get(), TEXT("ArenaFrameCheck"));
	bNoFrame = FParse::Param(FCommandLine::Get(), TEXT("ArenaNoFrame"));
	FrameFrames = FrameSettled = FrameInThird = 0;
	FrameLeadWorst = FrameFoeWorst = FrameTurnMost = FrameRateMost = FrameLensMost = FrameArmMost = FramePrev = 0.f;
	for (int32 I = 0; I < 4; ++I)
	{
		FrameBandN[I] = 0;
		FrameBandFoe[I] = 0.f;
		FrameBandGap[I] = 9.f;
	}
	AssistTime[0] = AssistTime[1] = AssistTurned[0] = AssistTurned[1] = 0.f;
	if (FParse::Param(FCommandLine::Get(), TEXT("ArenaAimCheck")))
	{
		const bool bGunKit = Def.fighter.weapon == ftcore::ArenaWeapon::Guns;
		AimCheckPhase = bAutoPlay && bGunKit ? 1 : 0;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: aim check %s (-ArenaAimCheck)."),
			AimCheckPhase ? TEXT("on: the camera aims at the tallest enemy's top and fires one aimed shot")
						  : TEXT("off: it needs -ArenaAutoPlay and a guns kit"));
		bAssistCheck = AimCheckPhase != 0 && FParse::Param(FCommandLine::Get(), TEXT("ArenaAssistCheck"));
		if (bAssistCheck)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: aim assist check on (-ArenaAssistCheck): after the shot the stick sweeps the view off the body and back, then a lock, the aim let go and the target's death each must bring the look scale back to 1."));
		}
	}
#endif
	Jolt = ftcore::ArenaJolt();
	JoltAsks = 0;
	JoltApplied = 0.f;
	OffFrameWindups = 0;
	Stingers = 0;
	LastStinger = -1e9;
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		bOffFrame[I] = false;
		bWindupTold[I] = false;
		OffFrameSince[I] = -1e9;
	}
	bSharedCount = Setup.bSharedCount && bAutoPlay;
	if (bSharedCount)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: the lead calls the count whenever it can and a scripted partner joins a held blow's window (-ArenaSharedCount)."));
	}
#if !UE_BUILD_SHIPPING
	bCountLift = FParse::Param(FCommandLine::Get(), TEXT("ArenaCountLift"));
	PartnerLoad = -1;
	if (bCountLift && FParse::Value(FCommandLine::Get(), TEXT("ArenaPartnerLoad="), PartnerLoad))
	{
		PartnerLoad = FMath::Clamp(PartnerLoad, 0, 100);
	}
#else
	bCountLift = false;
#endif
	if (bCountLift)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: the count stores Load and lifts; a Release marks its target; a locked kite seam opens on a shared blow (-ArenaCountLift, partner Load %d)."), PartnerLoad);
	}
#if !UE_BUILD_SHIPPING
	bLone = FParse::Param(FCommandLine::Get(), TEXT("ArenaLone"));
	LoneGiftAt = -1.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaLoneGiftAt="), LoneGiftAt) && !(LoneGiftAt >= 0.f))
	{
		LoneGiftAt = -1.f;
	}
	LoneSlot = static_cast<uint8>(ftcore::SlotState::Companion);
	{
		FString Slot;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaThirdSlot="), Slot))
		{
			if (Slot.Equals(TEXT("Vacant"), ESearchCase::IgnoreCase)) LoneSlot = static_cast<uint8>(ftcore::SlotState::Vacant);
			else if (Slot.Equals(TEXT("Hostile"), ESearchCase::IgnoreCase)) LoneSlot = static_cast<uint8>(ftcore::SlotState::Hostile);
		}
	}
#else
	bLone = false;
	LoneSlot = 0;
#endif
	LoneTuning = ftcore::LoneTuning();
	if (LoneWanted())
	{
		TArray<uint8> Bytes;
		std::string Error;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/lone_carry.json");
		if (!(FFileHelper::LoadFileToArray(Bytes, *File) && ftcore::importLoneCarry(FT::BytesAsText(Bytes), LoneTuning, &Error)))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Battle/lone_carry.json; the Lone Carry keeps its defaults.\n%s"),
				*FT::ToFString(Error));
			LoneTuning = ftcore::LoneTuning();
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: the lead fights Lone (%s; cap %d, gain x%.2f, Release x%.2f, taken x%.2f)."),
			bLone ? TEXT("-ArenaLone") : TEXT("forced by the third slot"), LoneTuning.cap, LoneTuning.gain, LoneTuning.release, LoneTuning.taken);
	}
	HallMode = 0;
#if !UE_BUILD_SHIPPING
	{
		FString Mode;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaHallucination="), Mode))
		{
			HallMode = Mode.Equals(TEXT("accept"), ESearchCase::IgnoreCase) ? 1
				: Mode.Equals(TEXT("refuse"), ESearchCase::IgnoreCase) ? 2
				: Mode.Equals(TEXT("expire"), ESearchCase::IgnoreCase) ? 3
				: Mode.Equals(TEXT("refuseheld"), ESearchCase::IgnoreCase) ? 4 : 0;
		}
		FParse::Value(FCommandLine::Get(), TEXT("ArenaHallucinationAt="), HallAt);
	}
#endif
	if (HallMode != 0)
	{
		const FString Dir = FPaths::ProjectDir() / TEXT("Data/Unwritten/");
		TArray<uint8> Bytes;
		std::string Error;
		if (!(FFileHelper::LoadFileToArray(Bytes, *(Dir + TEXT("hallucination.json")))
			&& ftcore::importHallucinationTuning(FT::BytesAsText(Bytes), HallTuning, &Error)))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Unwritten/hallucination.json; defaults kept.\n%s"), *FT::ToFString(Error));
			HallTuning = ftcore::HallucinationTuning();
		}
		Bytes.Reset();
		if (!(FFileHelper::LoadFileToArray(Bytes, *(Dir + TEXT("tragedies.json")))
			&& ftcore::importTragedies(FT::BytesAsText(Bytes), HallTable, &Error)))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Unwritten/tragedies.json; no offer can start.\n%s"), *FT::ToFString(Error));
		}
		BeginHallucination();
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: hallucination probe (-ArenaHallucination, mode %d, from %.1f s; %d keys; hold %.1f s, expire %.0f s, gap %.0f s)."),
			HallMode, HallAt, static_cast<int32>(HallTable.keys.size()), HallTuning.holdSeconds, HallTuning.expireSeconds, HallTuning.gapSeconds);
	}
	// Before begin, and only a stance the session will accept: the chairs' heal charges are left out from the start.
	Fight.setLone(LoneAccepted(SessionLeadOf(Setup.Lead)), LoneTuning);

	bBegun = true;
	bDrawSnap = FParse::Param(FCommandLine::Get(), TEXT("ArenaDrawSnap"));
	bWasDodging = false;
	BeginNow = Now;
	// The fitted zone's ring, and where the player starts in it.
	BeginBrief();
	BeginCarry(Setup.Ally);
	PrepareStoneBearer();
	PrepareKite();
	Fight.begin(Def, Fit.ring, Fit.playerStart, Fit.playerFacing, Seed);
	SessionLead = SessionLeadOf(Setup.Lead);
	BeginSession();
	BeginStoneBearer();
	BeginKite();
	BeginKindCrits();
	InstallSessionHook();
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
	// A view holding the fight's clock holds the lead's own clips too (its locomotion runs on the frame's time), so the
	// views after it show the pose at their fight time.
	if (USkeletalMeshComponent* LeadSkin = PlayerFigure ? PlayerFigure->GetLeadSkin() : nullptr)
	{
		LeadSkin->GlobalAnimRateScale = bHold ? 0.f : 1.f;
	}
	// The pose drawn last frame at last frame's placement, before anything moves.
	Glide.Sample(PlayerFigure ? PlayerFigure->GetLeadSkin() : nullptr, DeltaSeconds);
	PollInput(DeltaSeconds);
	TickAim(DeltaSeconds);

	const float HeldBefore = Fight.heldTime();
	for (float& Paused : PausedThisFrame)
	{
		Paused = 0.f;
	}
	if (!Fight.over() && !bHold)
	{
		Accumulator = FMath::Min(Accumulator + static_cast<double>(DeltaSeconds), static_cast<double>(StepSeconds * MaxStepsPerFrame));
		while (Accumulator >= StepSeconds && !Fight.over())
		{
			StepFight();
			Accumulator -= StepSeconds;
		}
	}
	HeldDelta = FMath::Max(0.f, Fight.heldTime() - HeldBefore);
	PlaceFigures(Fight.over() ? 1.f : static_cast<float>(Accumulator / StepSeconds));
	// The legs' speed as the figure hands it to the locomotion (none while dodging or down).
	{
		const ftcore::ArenaAction Act = Fight.output().player.action;
		Glide.Drawn(Fight.output().player, Now, Act == ftcore::ArenaAction::Dodge || Act == ftcore::ArenaAction::Downed ? 0.f : PlayerStepSpeed);
	}
	SteerCamera(DeltaSeconds);
	ApplyAimCamera();
	TickImpact(bHold ? 0.f : DeltaSeconds);

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
	EndTable();
	EndImpact();
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
	LeadWeapons.Reset(); // its components are the player's figure's
	if (AllyFigure)
	{
		AllyFigure->Destroy();
		AllyFigure = nullptr;
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
	PlayerStepSpeed = 0.f;
	PlayerStepAt = -1;
	LeadWantSpeed = -1.f;
	LeadStopDistance = -1.f;
	LeadLegYaw = 0.f;
	LeadUpperShare = 0.f;
	if (PlayerFigure)
	{
		PlayerFigure->ResetFeet();
	}
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
		// Locked dodges across the target are side dashes (drawn side-on, leaping on the dash clips) when the body
		// has both side clips; without them they stay rolls.
		Fight.setSideDashes(PlayerFigure->CanSideDash());
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: side dashes %s"), Fight.sideDashes() ? TEXT("on") : TEXT("off"));
		// The rig from Core's pose (ftcore::arenaCameraPose: the zone's, blended by the aim stance), as every frame sets it.
		const ftcore::ArenaCameraPose Pose = ftcore::arenaCameraPose(Fit.camera, ArenaAim, ftcore::ArenaFrameYaw{CameraYaw, FrameTurn}, CameraPitch);
		PlayerFigure->SetCameraRig(Pose.arm, Pose.pivot, Pose.side, Pose.up, Pose.fov);
		// The kit's weapon on the lead's body: its strike clips, props and effects.
		if (Lead)
		{
			LeadWeapons.Begin(PlayerFigure, *Lead, Def.fighter);
			PlayerFigure->SetJumpTiming(Def.fighter.jumpStartup, Def.fighter.jumpAir, Def.fighter.jumpLandLock);
		}
	}
	if (PC)
	{
		PC->SetViewTarget(PlayerFigure);
	}
	SpawnAlly(World);

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
	// The figures are placed in the fight's tick (the game mode's or the host's): their meshes pose after it, so a frame
	// shows each body's pose for where it stands that frame. Ticking first, a mesh posed for the last frame's placement and
	// was then moved and turned under it: a frame late, which slid the planted feet at every start, stop and turn, and
	// swung the old pose round with a sharp turn (the riposte's finisher snaps the facing at contact).
	// The figure keeps the tie for meshes it makes later (AFTArenaFighter::PoseAfter); pieces follow the body's pose
	// and grooms its mesh, each ticking after the mesh it follows, so they come along.
	if (AActor* Ticker = Cast<AActor>(GetOuter()))
	{
		if (PlayerFigure)
		{
			PlayerFigure->PoseAfter(Ticker);
		}
		if (AllyFigure)
		{
			AllyFigure->PoseAfter(Ticker);
		}
		for (int32 I = 0; I < MaxEnemies; ++I)
		{
			if (EnemyFigures[I])
			{
				EnemyFigures[I]->PoseAfter(Ticker);
			}
		}
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
	Figure->SetupSkinned(Mesh, Kind.look.hover, Kind.look.scale, Kind.look.yawOffset, ftcore::arenaEnemyHeight(Kind),
		Kind.boss && Kind.breakMove >= 0); // only a boss that breaks out gets the break tell's pulse
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
	const bool bTell = Kind.boss && Out.brain != ftcore::ArenaBrain::Dead && Fight.enemyBreakTell(static_cast<size_t>(I));
	if (bTell && !bTellShown[I])
	{
		TellSince[I] = Now;
	}
	bTellShown[I] = bTell;
	Figure->SetBodyPulse(bTell ? kBreakTellGlow * 0.5f * (1.f - static_cast<float>(FMath::Cos(UE_DOUBLE_TWO_PI * kBreakTellPulseHz * (Now - TellSince[I])))) : 0.f);
	// A hit pause holds the figure: HoldPausedEnemies moved its stamps on by the paused part of each step; the walk's
	// phase and the crossfade wait leave out what of this frame was paused.
	const float ClipDelta = FMath::Max(0.f, FrameDelta - FMath::Min(FrameDelta, PausedThisFrame[I]));
	int32 Clip = Skin.Idle;
	bool bTimed = false;  // the time is Core's, not the time since the clip began
	bool bPhased = false; // the time is the walk's own phase (it still loops)
	double Time = 0.0;
	bool bHoldRoot = true;
	float MoveRate = 0.f; // a move's clip's playing rate, else 0
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
			WalkPhase[I] = bWas ? WalkPhase[I] + static_cast<double>(ClipDelta * Rate) : 0.0;
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
			// The clip's impact lands at the end of the move's startup: Core's own clock for the move, at that rate.
			const ftcore::BeatClip& Row = (*Skin.Clips)[static_cast<size_t>(MoveClip)];
			// An unseen start (ArenaEnemyOut::unseen) has the longer windup: the impact still lands at its end.
			const float Startup = Kind.moves[Out.move].attack.startup + (Out.unseen ? ftcore::kArenaUnseenWindup : 0.f);
			const float Rate = (Row.impact > 0.0 && Startup > 0.f) ? FMath::Clamp(static_cast<float>(Row.impact) / Startup, kArenaMinClipRate, kArenaMaxClipRate) : 1.f;
			Clip = MoveClip;
			Time = (static_cast<double>(Out.stateTime) + static_cast<double>(Alpha) * StepSeconds) * Rate;
			bTimed = true;
			MoveRate = Rate;
		}
		break;
	}
	case ftcore::ArenaBrain::StepBack:
		// The break-out's step back: the walk clip played backward at the step's own speed, the figure still facing the
		// lead, so the blow it dodges is seen to miss. No walk clip: the idle.
		if (Skin.Walk >= 0 && !bStone)
		{
			const float Authored = static_cast<float>((*Skin.Clips)[static_cast<size_t>(Skin.Walk)].speed);
			const float Rate = Authored > 0.f ? FMath::Clamp(StepBackSpeed[I] / Authored, kArenaMinWalkRate, kArenaMaxWalkRate) : 1.f;
			const double Length = static_cast<double>(Skin.Sequences[Skin.Walk]->GetPlayLength());
			const double Phase = (static_cast<double>(Out.stateTime) + static_cast<double>(Alpha) * StepSeconds) * Rate;
			Clip = Skin.Walk;
			Time = Length > 0.0 ? Length - FMath::Fmod(Phase, Length) : 0.0;
			bPhased = true;
		}
		break;
	case ftcore::ArenaBrain::Flinch: // the light hit reaction StartFlinch began on the blow, else the idle (placeholder)
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
		else if (bStone && StoneTurnSign != 0)
		{
			// Its cycles on the turn's own clock, the one StoneTurnFacing yaws the figure by (looping, so no restart fade).
			Clip = StoneTurnSign > 0 ? Skin.TurnR : Skin.TurnL;
			Time = static_cast<double>(Fight.time()) + static_cast<double>(Alpha) * StepSeconds - StoneTurnStart;
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
	Figure->PlayClip(Sequence, At, bLoop, ClipDelta, bHoldRoot, static_cast<float>(Blend), Row.additiveIn);
}

void UFTArenaRunner::PutBodiesAtStart()
{
	Bodies = ftcore::ArenaBodies();
	Bodies.player = Fight.playerStart();
	if (bAllyOn)
	{
		Bodies.ally = AllyHome(Bodies.player, Fight.playerStartFacing());
	}
	for (size_t I = 0; I < Fight.enemyCount(); ++I)
	{
		Bodies.enemies[I] = Fight.enemyStart(I);
	}
	PrevBodies = Bodies;
	PlayerStepSpeed = 0.f;
	PlayerStepAt = -1;
	LeadWantSpeed = -1.f;
	LeadStopDistance = -1.f;
	LeadUpperShare = 0.f;
	LeadLegYaw = 0.f;
	Accumulator = 0.0;
	if (PlayerFigure)
	{
		PlayerFigure->ResetFeet();
	}
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		if (EnemyFigures[I])
		{
			EnemyFigures[I]->ResetFeet();
		}
	}
	Fight.tick(0.f, ftcore::ArenaInput(), Bodies);
	PrevHeldTime = static_cast<double>(Fight.heldTime());
	HeldDelta = 0.f;
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
	StoneHeldCone = StoneHeldUp = StoneHeldHit = StoneHeldTurn = 0;
	bStoneWanted = false;
	StoneTurnSign = 0;
	StoneTurnStart = -1e9;
	StoneTurnCycles = 0;
	bStoneTurnFoot = bStoneTurnChecked = false;
	StoneTurnsBegun = StoneTurnSteps = StoneTurnsCancelled = 0;
	bStoneMoveLeft = false;
	PlayerHitTime = -1e9;
	ParryTime = -1e9;
	ReleaseTime = -1e9;
	ReleaseSpent = 0;
	OverloadTime = -1e9;
	bPressLight = bPressHeavy = bPressDodge = bPressJump = bPressHeal = bPressLock = bPressRelease = bPressPair = bPartnerDue = false;
	GiftPick = ftcore::Gift::None;
	PressSwitch = 0;
	PressRead = -1;
	CameraYaw = ArenaYaw(Fight.playerStartFacing());
	CameraPitch = -16.f;
	FrameTurn = 0.f;
}

void UFTArenaRunner::PollInput(float DeltaSeconds)
{
	APlayerController* PC = GetPC();
	// Paused or just resumed: every press state clears, so a key let go meanwhile fires nothing later. The autopilot
	// reads no keys. Out of focus only the held attack clears (below); the toggled latches stay.
	if (!PC || bAutoPlay || bIdle || UFTPauseSubsystem::IsBlocking(this) || UFTPauseSubsystem::IsJustResumed(this))
	{
		bGuardHeld = false;
		GuardInput.reset();
		bInterceptHeld = false;
		InterceptInput.reset();
		bAimHeld = false;
		AimInput.reset();
		AttackHold.reset();
		Stick = ftcore::Vec2();
		return;
	}
	// Every key here comes from the binding table (FTInputBindings, ftcore/InputBindings.h): one set for the field
	// and the arena, remappable in Saved/InputBindings.json.
	using ftcore::InputAction;
	const UWorld* World = GetWorld();
	// Light: a tap is the light, held ftcore::AimRules::heavyHold the heavy. Aim held aims; a gun kit aiming fires
	// one round per press, at the press, and never a heavy.
	bAimHeld = AimInput.tick(FTInputBindings::IsDown(*PC, InputAction::Aim), FTInputBindings::Toggles(World, InputAction::Aim));
	const ftcore::PressHold::Out Press = AttackHold.tickIf(!FTInputBindings::LostFocus(), DeltaSeconds, FTInputBindings::IsDown(*PC, InputAction::Light), ArenaAim.rules().heavyHold,
		Fight.output().player.action == ftcore::ArenaAction::Light || Fight.output().player.action == ftcore::ArenaAction::Heavy,
		bAimHeld && Def.fighter.weapon == ftcore::ArenaWeapon::Guns);
	bPressLight |= Press.light;
	bPressHeavy |= Press.heavy;
	bPressDodge |= FTInputBindings::WasJustPressed(*PC, InputAction::Roll);
	// The jump: its key does nothing in a fight unless the jump is bound (-ArenaJump, until the user has tried it).
	if (JumpBound())
	{
		bPressJump |= FTInputBindings::WasJustPressed(*PC, InputAction::Jump);
	}
	// The partner's Read: the three pick keys (1 / 2 / 3, the d-pad's left, up and right) while the cards are up,
	// picked or not, until the move goes active. Core refuses a second pick. While the cards are up those keys are
	// the picks only, so the d-pad's up is not also the heal.
	const bool bCardsOpen = Fight.readCards().dealt();
	if (bCardsOpen)
	{
		const InputAction Picks[] = {InputAction::ReadPick1, InputAction::ReadPick2, InputAction::ReadPick3};
		for (int32 Pick = 0; Pick < 3; ++Pick)
		{
			if (FTInputBindings::WasJustPressed(*PC, Picks[Pick]))
			{
				PressRead = Pick;
				break;
			}
		}
	}
	// Heal on Gift's key and the partner's pair on Count's, until those actions have their own logic.
	const auto Shadowed = [bCardsOpen](InputAction Action, ftcore::InputDevice Device)
	{
		if (!bCardsOpen)
		{
			return false;
		}
		const FKey& Mine = FTInputBindings::Key(Action, Device);
		for (const InputAction Pick : {InputAction::ReadPick1, InputAction::ReadPick2, InputAction::ReadPick3})
		{
			if (FTInputBindings::Key(Pick, Device) == Mine)
			{
				return true;
			}
		}
		return false;
	};
	const auto Pressed = [PC, &Shadowed](InputAction Action)
	{
		for (const ftcore::InputDevice Device : {ftcore::InputDevice::Keys, ftcore::InputDevice::Pad})
		{
			const FKey& K = FTInputBindings::Key(Action, Device);
			if (K.IsValid() && !Shadowed(Action, Device) && PC->WasInputKeyJustPressed(K))
			{
				return true;
			}
		}
		return false;
	};
	bPressLock |= Pressed(InputAction::LockOn);
	bPressRelease |= Pressed(InputAction::Release);
	if (bTable)
	{
		bReleaseDown = false;
		for (const ftcore::InputDevice Device : {ftcore::InputDevice::Keys, ftcore::InputDevice::Pad})
		{
			const FKey& K = FTInputBindings::Key(InputAction::Release, Device);
			bReleaseDown |= K.IsValid() && !Shadowed(InputAction::Release, Device) && PC->IsInputKeyDown(K);
		}
	}
	bPressPair |= Pressed(InputAction::Count);
	// Guard: held, or pressed once to toggle (a refusal is a held Guard, so the toggle saves holding it).
	bGuardHeld = GuardInput.tick(FTInputBindings::IsDown(*PC, InputAction::Guard), FTInputBindings::Toggles(World, InputAction::Guard));
	// Three presses, each its own key with no modifier: the lead's own heal (Gift's row), and the third slot's Gift
	// Heal and Gift Guard (no pad button yet: those two are keyboard only until the pad's layout is ruled).
	bPressHeal |= Pressed(InputAction::Gift);
	if (Pressed(InputAction::GiftHeal))
	{
		GiftPick = ftcore::Gift::Heal;
	}
	else if (Pressed(InputAction::GiftGuard))
	{
		GiftPick = ftcore::Gift::Guard;
	}
	// Take half: its key, and on a pad with none bound, Interact pressed while the pad's Guard button is down (route
	// "guard_interact"; the button itself, so a toggled guard leaves Interact as it is).
	bPressTakeHalf |= Pressed(InputAction::TakeHalf);
	{
		const FKey& PadTake = FTInputBindings::Key(InputAction::TakeHalf, ftcore::InputDevice::Pad);
		const FKey& PadGuard = FTInputBindings::Key(InputAction::Guard, ftcore::InputDevice::Pad);
		const FKey& PadInteract = FTInputBindings::Key(InputAction::Interact, ftcore::InputDevice::Pad);
		if (!PadTake.IsValid() && PadGuard.IsValid() && PadInteract.IsValid() && PC->IsInputKeyDown(PadGuard)
			&& PC->WasInputKeyJustPressed(PadInteract))
		{
			bPressTakeHalf = true;
		}
	}
	// A called-blow boss's Intercept (its Guard is the guard above), held or toggled.
	bInterceptHeld = InterceptInput.tick(FTInputBindings::IsDown(*PC, InputAction::Intercept),
		FTInputBindings::Toggles(World, InputAction::Intercept));
	// Target switch: its keys when bound (none by default), else a flick of the mouse or the right stick while locked
	// (below).
	if (Pressed(InputAction::TargetPrev))
	{
		PressSwitch = -1;
	}
	if (Pressed(InputAction::TargetNext))
	{
		PressSwitch = 1;
	}

	// The left stick or the move keys: forward and right, at most 1 long.
	float Forward = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY);
	float Right = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX);
	const auto Down = [PC](InputAction Action) { return FTInputBindings::IsDown(*PC, Action) ? 1.f : 0.f; };
	Forward += Down(InputAction::MoveForward) - Down(InputAction::MoveBack);
	Right += Down(InputAction::MoveRight) - Down(InputAction::MoveLeft);
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
	// Along the view as drawn (the lock's framing turn included), so the lead goes where the stick points on the screen.
	Stick = ftcore::arenaStickToWorld(ViewYaw(), Forward, Right);

	// The view: the mouse and the right stick turn the camera unless something is locked; locked, a flick of
	// the right stick switches the target.
	const float LookX = PC->GetInputAnalogKeyState(EKeys::Gamepad_RightX);
	const float LookY = PC->GetInputAnalogKeyState(EKeys::Gamepad_RightY);
	if (Fight.lock() != ftcore::kArenaNobody)
	{
		// The mouse: a sharp sideways move switches, and it re-arms once the mouse slows.
		float MouseX = 0.f;
		float MouseY = 0.f;
		PC->GetInputMouseDelta(MouseX, MouseY);
		if (!bMouseFlicked && FMath::Abs(MouseX) > kArenaMouseFlickOut)
		{
			bMouseFlicked = true;
			PressSwitch = MouseX > 0.f ? 1 : -1;
		}
		else if (bMouseFlicked && FMath::Abs(MouseX) < kArenaMouseFlickRest)
		{
			bMouseFlicked = false;
		}
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
		// Aim assist: never under -ArenaAutoPlay (its -ArenaAssistCheck probe turns it on there); off, both shares are 1.
		const ftcore::Settings* Prefs = GetSettings();
		TurnLook(MouseX, MouseY, LookX, LookY, DeltaSeconds, !bAutoPlay && (!Prefs || Prefs->aimAssist));
		bStickFlicked = false;
		bMouseFlicked = false;
	}
}

ftcore::ArenaInput UFTArenaRunner::TakeInput()
{
	ftcore::ArenaInput In;
	In.move = Stick;
	const float Yaw = FMath::DegreesToRadians(ViewYaw());
	In.view = ftcore::Vec2{FMath::Cos(Yaw), FMath::Sin(Yaw)};
	In.cameraFacing = In.view; // a regular enemy's windup starts only where the camera or the lead faces
	In.light = bPressLight;
	In.heavy = bPressHeavy;
	In.dodge = bPressDodge;
	In.jump = bPressJump;
	In.heal = bPressHeal;
	In.guard = bGuardHeld;
	In.lock = bPressLock;
	In.lockSwitch = PressSwitch;
	In.release = bPressRelease;
	In.pair = bPressPair;
	In.takeHalf = bPressTakeHalf;
	In.read = PressRead;
	// Aimed: the lead faces the camera's way and strafes (ArenaFight's aim input).
	In.aim = ArenaAim.aiming();
	// The shot's way: at the locked enemy when locked, else from the lead's body to the reticle's point (the camera's
	// yaw alone runs the lane parallel to the reticle's ray, off it by the shoulder offset).
	In.aimDir = In.view;
	{
		const std::uint8_t Locked = Fight.lock();
		if (Locked != ftcore::kArenaNobody && Locked < Fight.enemyCount())
		{
			const ftcore::Vec2 To = Bodies.enemies[Locked];
			const ftcore::Vec2 D{To.x - Bodies.player.x, To.y - Bodies.player.y};
			if (D.x * D.x + D.y * D.y > 0.01f)
			{
				const float L = FMath::Sqrt(D.x * D.x + D.y * D.y);
				In.aimDir = ftcore::Vec2{D.x / L, D.y / L};
			}
			// The round climbs to the locked body's middle (the sim's 3D shot test; see ArenaInput::aimPitched).
			In.aimPitched = BodyPitch(static_cast<int32>(Locked), In.aimPitch);
		}
		else if (ArenaAim.alpha() > 0.f)
		{
			In.aimDir = ReticleAimDir(In.view);
			In.aimPitched = ReticlePitch(In.aimPitch);
			In.aimBody = ReticleAimBody(); // the round prefers it by the crosshair readout's own rule
		}
	}
	In.aimTurn = ArenaAim.rules().turnRate;
	In.aimSpeed = ArenaAim.speedScale();
	// Attacking on the move: every attack under way (as the last step left it) walks and turns by its rules, none
	// roots: a light by the light's numbers, a heavy, a riposte and a Release by the heavy's.
	ftcore::attackMoveInput(AttackMoveTable, Fight, In);
	// Each press goes to one step.
	bPressLight = bPressHeavy = bPressDodge = bPressJump = bPressHeal = bPressLock = bPressRelease = bPressPair = bPressTakeHalf = false;
	PressSwitch = 0;
	PressRead = -1;
	return In;
}

// Before a step: an enemy in a hit pause stands still for the part of the step the pause covers (Core's stopLeft after
// the last step), so the screen-clock stamps its clips are timed from, and the stone bearer's walk and turn, move on by
// that much. Stamps the step's events set come after, so they are never moved; a step not taken (a hold) moves nothing.
void UFTArenaRunner::HoldPausedEnemies()
{
	const ftcore::ArenaOutput& Last = Fight.output();
	for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()) && I < MaxEnemies; ++I)
	{
		const ftcore::ArenaEnemyOut& E = Last.enemies[static_cast<size_t>(I)];
		const float Paused = E.brain == ftcore::ArenaBrain::Dead ? 0.f : FMath::Min(StepSeconds, E.stopLeft);
		if (!(Paused > 0.f))
		{
			continue;
		}
		PausedThisFrame[I] += Paused;
		SkinSince[I] += Paused;
		HitSince[I] += Paused;
		RecoilSince[I] += Paused;
		KneelSince[I] += Paused;
		UpOwedUntil[I] += Paused;
		if (I == StoneBearerSlot)
		{
			StoneWalkSince += Paused;
			StoneTurnStart += Paused;
		}
	}
}

// The fight session's lead slot for the look the player's figure wears: Devin fills kLeadA and Duy kLeadB (the
// session's two lead slots); any other look, or none (the test human), stands in as kLeadA.
uint8 UFTArenaRunner::SessionLeadOf(const FFTLeadLook* Look)
{
	return Look && Look->Name.Equals(TEXT("Duy"), ESearchCase::IgnoreCase) ? ftcore::kLeadB : ftcore::kLeadA;
}

void UFTArenaRunner::BeginSession()
{
	SessionHeld = Fight.heldTime();
	bSessionGuard = false;
	bSessionPartnerDue = false;
	SessionHeldBlows = 0;
	SessionJoins = 0;
	SessionBonuses = 0;
	SessionBonusDamage = 0;
	CountLoadStored = CountLoadStores = Lifts = LiftSpent = Marks = KiteUnlocks = MaxJoinLoad = PartnerLoadStored = 0;
	CappedBlows = 0;
	LoneEndedAt = -1.0;
	LoneReleases = LoneGiftPresses = LoneGiftsRefused = LoneBreaches = 0;
	bLoneGiftDone = false;
	LoneGift = {};
	PartnerLoadTime = -1e9;
	LiftTime = LastJoinAt = KiteLockUntil = -1e9;
	LiftMultiplier = 1.f;
	if (Session)
	{
		ftcore::FightSessionDef SessionDef;
		if (bCountLift)
		{
			SessionDef.lift = true;
			// The Lift's tuning and the blow cap from Data/Battle/count_lift.json (the defaults, 6x, if it is refused).
			{
				ftcore::CountLiftData LiftData;
				TArray<uint8> Bytes;
				std::string Error;
				const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/count_lift.json");
				if (!(FFileHelper::LoadFileToArray(Bytes, *File) && ftcore::importCountLift(FT::BytesAsText(Bytes), LiftData, &Error)))
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Battle/count_lift.json; the Lift keeps its defaults.\n%s"),
						*FT::ToFString(Error));
					LiftData = ftcore::CountLiftData();
				}
				SessionDef.liftTuning = LiftData.lift;
				SessionDef.blowCap = LiftData.blowCap;
			}
			// The partner lead's own gauge, kept by the session (a probe's fixed -ArenaPartnerLoad replaces it). Its cap
			// is the chairs' formula with the partner's own Named chairs: one for now (60).
			if (PartnerLoad < 0)
			{
				SessionDef.keptLead = SessionLead == ftcore::kLeadA ? ftcore::kLeadB : ftcore::kLeadA;
				SessionDef.keptNamed = 1;
			}
			// The kite's seam behind its lock: the session holds the lock (a shared blow, a take half, opens it).
			if (KiteSlot >= 0 && Kite.IsLoaded() && Kite.GetWeakPoints().locked)
			{
				SessionDef.locked = true;
				SessionDef.lock = Kite.GetWeakPoints().lock;
			}
		}
		if (LoneWanted())
		{
			SessionDef.loneLead = SessionLead;
			SessionDef.loneTuning = LoneTuning;
		}
		// The gift slot's tuning from Data/Battle/gifts.json (its defaults if the file is refused).
		{
			TArray<uint8> Bytes;
			std::string Error;
			const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/gifts.json");
			if (!(FFileHelper::LoadFileToArray(Bytes, *File) && ftcore::importGiftTuning(FT::BytesAsText(Bytes), SessionDef.gift, &Error)))
			{
				static bool bGiftWarned = false; // once a process, not every fight start
				if (!bGiftWarned)
				{
					bGiftWarned = true;
					UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Battle/gifts.json; the gift slot keeps its defaults.\n%s"),
						*FT::ToFString(Error));
				}
				SessionDef.gift = ftcore::GiftTuning{};
			}
		}
		// The lone pool's tuning from Data/Battle/lone_pool.json (its defaults if the file is refused), for the roll.
		{
			TArray<uint8> Bytes;
			std::string Error;
			const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/lone_pool.json");
			if (!(FFileHelper::LoadFileToArray(Bytes, *File) && ftcore::importLonePool(FT::BytesAsText(Bytes), LonePool, &Error)))
			{
				static bool bPoolWarned = false; // once a process
				if (!bPoolWarned)
				{
					bPoolWarned = true;
					UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Battle/lone_pool.json; the lone pool keeps its defaults.\n%s"),
						*FT::ToFString(Error));
				}
				LonePool = ftcore::LonePoolTuning{};
			}
		}
		LoneBargainAt = -1.0;
		Session->Begin(SessionDef, static_cast<double>(SessionHeld));
		if (LoneSlot != static_cast<uint8>(ftcore::SlotState::Companion))
		{
			Session->Get().giftSlot().setState(static_cast<ftcore::SlotState>(LoneSlot)); // begin set it Companion
		}
		// The game instance's story state takes the session's story writes (bound again after every Begin), but only
		// for a fight the story handed off (ftcore::fightTakesStory): a standalone, demo or practice fight stays unbound.
		const UWorld* World = GetWorld();
		UFTGameInstance* Instance = World ? World->GetGameInstance<UFTGameInstance>() : nullptr;
		ftcore::FightHandOff HandOff;
		if (Instance)
		{
			HandOff.fromStory = Instance->StoryCarry.bValid;
			HandOff.settled = Instance->StoryCarry.bDone;
			HandOff.kind = Instance->StoryCarry.Combat;
		}
		const bool bStory = ftcore::fightTakesStory(HandOff, ftcore::CombatKind::Arena);
		UFTStoryStateSubsystem* Story = Instance ? Instance->GetSubsystem<UFTStoryStateSubsystem>() : nullptr;
		if (Story && bStory)
		{
			Story->BindFightSession(Session, !bRestarting);
		}
		else
		{
			Session->UnbindStory();
		}
		bStoryFight = bStory && Story;
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: fight session story %s"),
			Session->IsStoryBound() ? TEXT("bound") : (bStoryFight ? TEXT("REFUSED (plays unbound)") : TEXT("unbound")));
	}
	if (LoneWanted())
	{
		// No session, or a refused stance: not Lone (LoneAccepted kept the heal charges). The single-blow cap keeps a
		// Lone Release's x1.3 under it.
		Fight.setLone(Session && Session->Get().lone(), LoneTuning, Session ? Session->Get().blowCap() : 0.f);
		bLoneAtStart = Fight.lone();
		if (!bLoneAtStart)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: the Lone Carry was refused (no session, or a refused stance); the lead fights as usual."));
		}
	}
}

void UFTArenaRunner::PressPendingLoneGift()
{
	if (!Session)
	{
		return;
	}
	++LoneGiftPresses;
	// The slot refuses a Heal at a lead at full health (NotHurt); it reads the lead's health as of this press.
	Session->Get().setLeadHealth(SessionLead, static_cast<float>(Fight.hp()) / static_cast<float>(FMath::Max(Fight.def().fighter.hp, 1)));
	if (bProbeAwaiting)
	{
		ProbeLeadHealth = static_cast<float>(Fight.hp()) / static_cast<float>(FMath::Max(Fight.def().fighter.hp, 1));
	}
	const ftcore::GiftResult Result = Session->Press(EFTSharedAction::Gift, ftcore::kThirdSlot, SessionLead).gift;
	if (bProbeAwaiting)
	{
		bProbeAwaiting = false;
		LogProbeCheck(Result);
	}
	if (bLoneGiftApply)
	{
		ApplyGift(Result);
	}
	bLoneGiftApply = false;
	const bool bGiven = Result.given;
	MirrorLone();
	if (!bGiven)
	{
		++LoneGiftsRefused;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: a gift pressed while Lone was refused at %.2f s; still Lone."), Fight.time());
	}
}

void UFTArenaRunner::LogProbeCheck(const ftcore::GiftResult& Result)
{
	// The slot's NotHurt refusal is the proof the lead's health reached it before the press: at full health a Heal
	// press must be refused NotHurt, and a hurt lead's must not be.
	const bool bFull = ProbeLeadHealth >= 1.f;
	const bool bNotHurt = !Result.given && Result.error == ftcore::GiftError::NotHurt;
	const bool bOk = ProbeLeadHealth >= 0.f && bFull == bNotHurt;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: probe self-check %s: setLeadHealth %.2f before the press; the press %s (error %d)."),
		bOk ? TEXT("ok") : TEXT("FAILED"), ProbeLeadHealth, Result.given ? TEXT("given") : TEXT("refused"), static_cast<int32>(Result.error));
}

bool UFTArenaRunner::IsLoneBargainDue() const
{
	return LoneBargainAt >= 0.0 && static_cast<double>(Fight.time()) - LoneBargainAt < kLoneBargainShow;
}

ftcore::LoneBargain UFTArenaRunner::GetLoneBargain() const
{
	return Session ? Session->Get().loneBargain() : ftcore::LoneBargain{};
}

ftcore::LoneRoll UFTArenaRunner::GetLoneRoll() const
{
	return Session ? Session->Get().loneRoll(LoneSaved, LonePool) : ftcore::loneRoll(LoneSaved, 0u, 0u, LonePool);
}

void UFTArenaRunner::ApplyGift(const ftcore::GiftResult& Result)
{
	if (!Result.given)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: a gift press refused (gift %d, error %d)."), Fight.time(),
			static_cast<int32>(Result.gift), static_cast<int32>(Result.error));
		return;
	}
	if (Result.gift == ftcore::Gift::Heal)
	{
		const int32 Healed = Fight.hostHeal(FMath::RoundToInt(Result.healShare * static_cast<float>(Fight.def().fighter.hp)));
		const int32 Moved = Result.healLoad > 0 ? Fight.spendLoad(Result.healLoad) : 0;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: gift Heal: +%d hp, %d Load off the lead, %d Heals left."), Fight.time(), Healed,
			Moved, Session ? Session->Get().giftHealsLeft() : 0);
	}
	else if (Result.gift == ftcore::Gift::Guard)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: gift Guard: x%.2f on the lead's blows until %.2f (session clock)."), Fight.time(),
			Result.guardScale, Result.guardUntil);
	}
}

int32 UFTArenaRunner::GetGiftHealsLeft() const
{
	return Session ? Session->Get().giftHealsLeft() : 0;
}

double UFTArenaRunner::GetGiftHealReadyIn() const
{
	return Session ? Session->Get().giftHealReadyIn() : 0.0;
}

double UFTArenaRunner::GetGiftGuardReadyIn() const
{
	return Session ? Session->Get().giftGuardReadyIn() : 0.0;
}

bool UFTArenaRunner::IsRefusedTooEarly() const
{
	return HallMode != 0 && HallTooEarlyAt >= 0.0 && static_cast<double>(Fight.time()) - HallTooEarlyAt < kRefusedTooEarlyShow;
}

bool UFTArenaRunner::IsEscrowWaiting() const
{
	return HallMode != 0 && Hall.escrowWaiting();
}

float UFTArenaRunner::GetEscrowWaitLeft() const
{
	return HallMode != 0 ? Hall.escrowWaitLeft() : 0.f;
}

float UFTArenaRunner::GetWeakPointLockFor() const
{
	return HallMode != 0 ? Hall.weakPointLockFor() : 0.f;
}

// Why the Lone Carry ended, for the log.
static const TCHAR* LoneEndWord(ftcore::LoneEnd Why)
{
	switch (Why)
	{
	case ftcore::LoneEnd::Gift: return TEXT("a gift given");
	case ftcore::LoneEnd::PartnerCalled: return TEXT("the partner called");
	case ftcore::LoneEnd::FightEnded: return TEXT("the fight's end");
	default: return TEXT("none");
	}
}

void UFTArenaRunner::MirrorLone()
{
	if (!Fight.lone() || !Session || Session->Get().lone())
	{
		return;
	}
	Fight.setLone(false, LoneTuning, Session->Get().blowCap()); // a Release begun Lone keeps its x1.3 under the cap
	LoneEndedAt = Fight.time();
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: %s ended the Lone Carry at %.2f s (Load %d/%d)."), LoneEndWord(Session->Get().loneEndedBy()),
		Fight.time(), Fight.load(), Fight.loadCap());
}

// Before the step, from the step's input (the player's, or the autopilot's in an unattended run): its gift and
// count presses, and the guard's edges (a refusal hold; the session's prompt ignores a guard already down when it
// is offered). The fight takes the same presses as its own heal and pair until those actions move over.
void UFTArenaRunner::PressSession(const ftcore::ArenaInput& In)
{
	if (!Session)
	{
		return;
	}
	// Probe: one gift press at -ArenaLoneGiftAt's fight time (the heal's own press otherwise).
	const bool bProbeGift = LoneGiftAt >= 0.f && !bLoneGiftDone && Fight.time() >= LoneGiftAt;
	if (bProbeGift)
	{
		bLoneGiftDone = true;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: probe gift press at %.2f s (-ArenaLoneGiftAt)."), Fight.time());
	}
	// A gift key's press: its gift, selected now (a press held past a Lone step gives the same gift) and applied. The
	// lead's own heal while Lone, and the probe, press the slot's Heal unapplied, as before the gift keys: a gift given
	// ends the Lone Carry. Outside Lone the heal never presses the slot. Nothing picks a gift for the player.
	const ftcore::Gift Pick = GiftPick;
	GiftPick = ftcore::Gift::None;
	const bool bKeyGift = Pick != ftcore::Gift::None;
	if (bKeyGift || (In.heal && Fight.lone()) || bProbeGift)
	{
		Session->Get().selectGift(bKeyGift ? Pick : ftcore::Gift::Heal);
		// The slot refuses a Heal at a lead at full health (NotHurt); it reads the lead's health as of this press.
		Session->Get().setLeadHealth(SessionLead, static_cast<float>(Fight.hp()) / static_cast<float>(FMath::Max(Fight.def().fighter.hp, 1)));
		if (bProbeGift)
		{
			ProbeLeadHealth = static_cast<float>(Fight.hp()) / static_cast<float>(FMath::Max(Fight.def().fighter.hp, 1));
		}
		if (LoneGift.press(Fight.lone())) // else pressed after the step (PressPendingLoneGift)
		{
			const ftcore::GiftResult Result = Session->Press(EFTSharedAction::Gift, ftcore::kThirdSlot, SessionLead).gift;
			if (bProbeGift)
			{
				LogProbeCheck(Result);
			}
			if (bKeyGift)
			{
				ApplyGift(Result);
			}
		}
		else
		{
			bLoneGiftApply = bLoneGiftApply || bKeyGift;
			bProbeAwaiting = bProbeAwaiting || bProbeGift;
		}
	}
	if (In.pair || bSharedCount) // under -ArenaSharedCount every step (refused while a count is live or cooling)
	{
		Session->Press(EFTSharedAction::Count, SessionLead);
	}
	if (In.guard != bSessionGuard)
	{
		bSessionGuard = In.guard;
		Session->Press(In.guard ? EFTSharedAction::RefuseDown : EFTSharedAction::RefuseUp, SessionLead);
	}
}

// The session judges each blow the player lands on an enemy (a Hit from the player; the partner's counters are other
// blow types) inside the fight's blow hook, after the sub-hosts' ruling: judgeHeldBlow on the damage they leave. A
// final outcome scales the blow (x1 alone); a Held one lands at base and its settle's bonus comes after a later tick.
ftcore::ArenaBlowRuling UFTArenaRunner::RuleSessionBlow(void* User, const ftcore::ArenaBlow& Blow)
{
	UFTArenaRunner* Self = static_cast<UFTArenaRunner*>(User);
	ftcore::ArenaBlowRuling Ruling;
	if (Self->SessionInnerHook)
	{
		Ruling = Self->SessionInnerHook(Self->SessionInnerUser, Blow);
	}
	if (Ruling.veto || !Self->Session || Blow.type != ftcore::ArenaEventType::Hit || Blow.source != ftcore::kArenaPlayer ||
		Blow.target == ftcore::kArenaPlayer)
	{
		return Ruling;
	}
	const float Scale = Ruling.damage > 0.f ? FMath::Min(Ruling.damage, 1.0e6f) : 0.f; // NaN counts as 0, as the fight reads it
	// With a blow cap (-ArenaCountLift) a held blow's bonus comes from its damage before the crit, and the whole blow
	// (crit, count, Lift, mark) stays under the cap. Without one, as before: the base is the damage after the crit.
	ftcore::FightSession& S = Self->Session->Get();
	const bool bCapped = S.blowCap() > 0.f;
	const int Base = static_cast<int>(FMath::RoundToFloat(static_cast<float>(Blow.damage) * (bCapped ? 1.f : Scale)));
	Self->FeedSessionLoads();
	const ftcore::CountHit Hit = S.judgeHeldBlow(Self->SessionLead, Blow.target, Base, Blow.move, bCapped ? Scale : 1.f);
	const float Want = Hit.outcome == ftcore::CountOutcome::Held ? Scale : Scale * Hit.multiplier;
	if (Hit.outcome == ftcore::CountOutcome::Held)
	{
		++Self->SessionHeldBlows;
		Self->bSessionPartnerDue = Self->bSharedCount;
	}
	if (bCapped)
	{
		const float Kept = S.capBlow(Want);
		Self->CappedBlows += Kept < Want ? 1 : 0;
		Ruling.damage = Kept;
	}
	else if (Hit.outcome != ftcore::CountOutcome::Held)
	{
		Ruling.damage = Want;
	}
	// A boss's break value takes the count's joined and Lift multipliers only (a held blow's bonus comes as a hostBlow).
	if (Hit.outcome != ftcore::CountOutcome::Held)
	{
		Ruling.breakScale *= Hit.multiplier;
	}
	return Ruling;
}

// Chains the session's ruling after whatever hook the sub-hosts set at this begin (none, kind crits, the kite or the
// stone bearer). On a restart where none of them set one, the hook is still the session's: its inner hook stays.
void UFTArenaRunner::InstallSessionHook()
{
	if (!Session)
	{
		return;
	}
	if (Fight.blowHook() != &UFTArenaRunner::RuleSessionBlow)
	{
		SessionInnerHook = Fight.blowHook();
		SessionInnerUser = Fight.blowHookUser();
	}
	Fight.setBlowHook(&UFTArenaRunner::RuleSessionBlow, this);
}

// After the step: under -ArenaSharedCount the scripted partner joins the window a lead's blow waits in; then the
// session moves by what the held clock moved (0 through a hit pause, so hit-stop is never subtracted twice), and each
// held blow that settled above x1 adds its extra damage through ruledHostBlow (reported with the next step).
void UFTArenaRunner::FeedSession()
{
	if (!Session)
	{
		return;
	}
	if (bSessionPartnerDue)
	{
		bSessionPartnerDue = false;
		const uint8 Partner = SessionLead == ftcore::kLeadA ? ftcore::kLeadB : ftcore::kLeadA;
		FeedSessionLoads();
		if (ftcore::sharedOutcome(Session->Get().judgeBlow(Partner).outcome))
		{
			LoneBreaches += Fight.lone() ? 1 : 0;
			++SessionJoins;
		}
	}
	const float Held = Fight.heldTime();
	const ftcore::FightTick& Tick = Session->Tick(FMath::Max(0.f, Held - SessionHeld));
	SessionHeld = Held;
	if (Tick.loneCarryBegan)
	{
		LoneBargainAt = static_cast<double>(Fight.time());
		const ftcore::LoneBargain Bargain = Session->Get().loneBargain();
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: the Lone Carry began: Release x%.2f, Load gain x%.2f, cap %d, taken x%.2f."), Bargain.release,
			Bargain.loadGain, Bargain.loadCap, Bargain.damageTaken);
	}
	ApplyCountLift(Tick);
	for (const ftcore::HeldBonus& Bonus : Tick.bonuses)
	{
		// Ruled as a blow by the sub-hosts' hook (a boss gone invulnerable or a vetoing phase takes none), not by the
		// session's own judge, which already judged it.
		if (Fight.ruledHostBlow(Bonus.enemy, Bonus.extraDamage, Bonus.move, SessionInnerHook, SessionInnerUser))
		{
			++SessionBonuses;
			SessionBonusDamage += Bonus.extraDamage;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: a held blow on the count adds %d damage to %s (move %d)."), Bonus.extraDamage,
				UTF8_TO_TCHAR(SlotKindId(Bonus.enemy)), Bonus.move);
		}
	}
}

void UFTArenaRunner::FeedSessionLoads()
{
	if (!bCountLift || !Session)
	{
		return;
	}
	const uint8 Partner = SessionLead == ftcore::kLeadA ? ftcore::kLeadB : ftcore::kLeadA;
	Session->Get().setLoad(SessionLead, Fight.load());
	if (!Session->Get().keepsLoad(Partner))
	{
		Session->Get().setLoad(Partner, PartnerLoad); // a probe's fixed Load (the session keeps the partner's otherwise)
	}
}

void UFTArenaRunner::ApplyCountLift(const ftcore::FightTick& Tick)
{
	if (!bCountLift || !Session)
	{
		return;
	}
	const ftcore::FightSession& S = Session->Get();
	const ftcore::CountJoin& Join = S.strikeCount().lastJoin();
	bool bNewLift = false;
	if (Join.at != LastJoinAt && Join.first != ftcore::kNoFighter)
	{
		LastJoinAt = Join.at;
		bNewLift = Join.lift;
		MaxJoinLoad = FMath::Max(MaxJoinLoad, static_cast<int32>(Join.loadSum));
		if (Join.lift)
		{
			++Lifts;
			LiftTime = Now;
			LiftMultiplier = S.strikeCount().tuning().onCountMultiplier * S.strikeCount().liftTuning().liftMultiplier;
		}
	}
	int32 Spent[2] = {0, 0}; // the player's, the partner's
	for (const ftcore::CountLoadChange& C : Tick.loads)
	{
		if (C.kept)
		{
			// The partner's gauge: the session applied it already.
			PartnerLoadTime = Now;
			if (C.kind == ftcore::CountLoadKind::LiftSpend)
			{
				Spent[1] += C.amount;
				LiftSpent += C.amount;
			}
			else
			{
				PartnerLoadStored += C.amount;
				const TCHAR* Source = C.kind == ftcore::CountLoadKind::ParryStore ? TEXT("parry")
					: (C.kind == ftcore::CountLoadKind::BlockStore ? TEXT("block") : TEXT("count"));
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: partner stored %d Load (%s), %d/%d."), C.amount, Source, S.keptLoad(), S.keptCap());
			}
			continue;
		}
		if (C.lead != SessionLead)
		{
			continue; // a probe's fixed partner Load: nothing to apply
		}
		if (C.kind == ftcore::CountLoadKind::CountStore)
		{
			const int Stored = Fight.giveLoad(C.amount);
			CountLoadStored += Stored;
			++CountLoadStores;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: count stored %d Load for lead %d."), Stored, static_cast<int32>(C.lead));
		}
		else
		{
			const int Gone = Fight.spendLoad(C.amount);
			LiftSpent += Gone;
			Spent[0] += Gone;
		}
	}
	if (bNewLift)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: count Lift at %.2f s: x%.2f, spent %d+%d."), Fight.time(), LiftMultiplier, Spent[0], Spent[1]);
	}
	// The kite's locked seam opens with the session's lock (a joined count, a take half) for the lock's open time.
	if (Kite.IsActive() && Kite.IsLocked() && S.weakPointOpen())
	{
		const double Until = S.lock().until();
		if (Until != KiteLockUntil)
		{
			KiteLockUntil = Until;
			++KiteUnlocks;
			Kite.OnUnlocked(static_cast<double>(Fight.time()), Until - S.now());
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: the kite's seam unlocked at %.2f s for %.2f s."), Fight.time(), Until - S.now());
		}
	}
}

int32 UFTArenaRunner::GetPartnerLoad() const
{
	return Session ? Session->Get().keptLoad() : 0;
}

int32 UFTArenaRunner::GetPartnerLoadCap() const
{
	return Session ? Session->Get().keptCap() : 0;
}

float UFTArenaRunner::GetEnemyMarkBonus(int32 Slot) const
{
	if (!Session || Slot < 0 || Slot >= static_cast<int32>(ftcore::kMaxMarkedEnemies))
	{
		return 0.f;
	}
	return Session->Get().mark(static_cast<uint8>(Slot)).bonus;
}

double UFTArenaRunner::GetEnemyMarkUntil(int32 Slot) const
{
	if (!Session || Slot < 0 || Slot >= static_cast<int32>(ftcore::kMaxMarkedEnemies))
	{
		return -1e9;
	}
	const ftcore::EnemyMark M = Session->Get().mark(static_cast<uint8>(Slot));
	return M.bonus > 0.f ? M.until : -1e9;
}

void UFTArenaRunner::StepFight()
{
	ftcore::ArenaInput In;
	bool bIntercept = false;
	if (bAutoPlay)
	{
		In = Fight.autopilot(); // the lead's own heal; the autopilot never presses a gift
		// Attacks walk and turn by the same rules as the player's, so an unattended check runs what a player gets.
		ftcore::attackMoveInput(AttackMoveTable, Fight, In);
		if (bNoLock)
		{
			In.lock = false; // never pressed: nothing is ever locked, so attacks take soft targets
		}
		// -ArenaGlide=pulse: starts, stops and reversals over and over, for the speeding-up and braking buckets.
		int32 GlideNow = GlideStick;
		if (GlideStick == 4 && !In.dodge)
		{
			constexpr double kPulseGo = 0.8, kPulseRest = 0.6;
			const double Phase = FMath::Fmod(Now, 2.0 * (kPulseGo + kPulseRest));
			GlideNow = Phase < kPulseGo ? 0 : Phase < kPulseGo + kPulseRest ? -1 : Phase < 2.0 * kPulseGo + kPulseRest ? 2 : -1;
			In.move = ftcore::Vec2{0.f, 0.f};
		}
		if (GlideStick == 5 && !In.dodge)
		{
			// -ArenaGlide=orbitguard: round the target with the guard held, no attacks (the guard cone against the body).
			GlideNow = 1;
			In.move = ftcore::Vec2{0.f, 0.f};
			In.guard = true;
			In.light = In.heavy = In.release = false;
		}
		const bool bWasRolling = GlideRolling;
		GlideRolling = Fight.output().player.action == ftcore::ArenaAction::Dodge;
		if (GlideStick == 6 && bWasRolling && !GlideRolling)
		{
			// -ArenaGlide=rollstrike: a light attack the moment a roll ends, while the drawn body may still be turning.
			In.light = true;
			In.dodge = In.guard = false;
		}
		if (GlideStick == 7 && In.dodge)
		{
			// -ArenaGlide=backstep: every roll pressed with the stick let go, so the rules backstep (the
			// drawn body rolls the way it goes, then turns back to the lock).
			In.move = ftcore::Vec2{0.f, 0.f};
		}
		if (GlideStick == 8 && In.dodge && !GlideRolling)
		{
			// -ArenaGlide=sidestep: every roll pressed with the stick straight across the lock, to the target's right
			// then its left in turn (side dashes where the body has the clips, rolls where it has not).
			const std::uint8_t Lock = Fight.lock();
			if (Lock != ftcore::kArenaNobody && Lock < MaxEnemies)
			{
				const ftcore::Vec2 D{Bodies.enemies[Lock].x - Bodies.player.x, Bodies.enemies[Lock].y - Bodies.player.y};
				const float L = ArenaLength(D);
				if (L > 1e-3f)
				{
					const float S = (GlideSideRolls++ & 1) ? -1.f : 1.f; // +: the ground plane's right (y right of x)
					In.move = ftcore::Vec2{-S * D.y / L, S * D.x / L};
				}
			}
		}
		if (GlideStick >= 9 && GlideStick <= 11)
		{
			// -ArenaGlide=jump / jumpmove / jumpstrike: jumps only (no attack, roll, guard or heal), one from the first
			// free frame kGlideJumpEvery after the last frame on the jump's clock; let go (jump), held across the target
			// (jumpmove), or let go with a light at kGlideAirLightAt on the air clock (jumpstrike: the air light).
			constexpr double kGlideJumpEvery = 1.2;
			constexpr float kGlideAirLightAt = 0.22f;
			const ftcore::ArenaPlayerOut& P = Fight.output().player;
			In.light = In.heavy = In.release = In.dodge = In.guard = In.heal = In.pair = In.takeHalf = false;
			In.move = ftcore::Vec2{0.f, 0.f};
			GlideNow = GlideStick == 10 ? 1 : -1;
			if (P.jumpTime >= 0.f)
			{
				GlideJumpLast = Now;
			}
			const bool bFree = P.action == ftcore::ArenaAction::Idle || P.action == ftcore::ArenaAction::Move;
			In.jump = bFree && Now - GlideJumpLast >= kGlideJumpEvery;
			// Only in the air: once the air light hands back to the jump, a press in the landing would start a ground light.
			const ftcore::ArenaFighter& Kit = Fight.def().fighter;
			In.light = GlideStick == 11 && P.action == ftcore::ArenaAction::Jump && P.jumpTime >= kGlideAirLightAt
				&& P.jumpTime < Kit.jumpStartup + Kit.jumpAir;
		}
		if (GlideNow >= 0 && GlideNow < 3 && !In.dodge && In.move.x * In.move.x + In.move.y * In.move.y < 1e-4f)
		{
			// -ArenaGlide: the stick held toward, across or away from the target (the lock, else the nearest standing).
			const std::uint8_t Lock = Fight.lock();
			const int32 Count = FMath::Min(static_cast<int32>(Fight.def().enemyCount), static_cast<int32>(MaxEnemies));
			int32 Target = Lock != ftcore::kArenaNobody && Lock < Count && Fight.enemyHp(Lock) > 0 ? static_cast<int32>(Lock) : -1;
			float Best = TNumericLimits<float>::Max();
			for (int32 I = 0; Target < 0 && I < Count; ++I)
			{
				const ftcore::Vec2 D{Bodies.enemies[I].x - Bodies.player.x, Bodies.enemies[I].y - Bodies.player.y};
				if (Fight.enemyHp(static_cast<std::size_t>(I)) > 0 && ArenaLength(D) < Best)
				{
					Best = ArenaLength(D);
					Target = I;
				}
			}
			if (Target >= 0)
			{
				const ftcore::Vec2 D{Bodies.enemies[Target].x - Bodies.player.x, Bodies.enemies[Target].y - Bodies.player.y};
				const float L = ArenaLength(D);
				if (L > 1e-3f)
				{
					const ftcore::Vec2 U{D.x / L, D.y / L};
					In.move = GlideNow == 0 ? U : GlideNow == 1 ? ftcore::Vec2{-U.y, U.x} : ftcore::Vec2{-U.x, -U.y};
				}
			}
		}
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
		if (bTableKey)
		{
			// The pilot stops attacking and releasing while it means to set the table, so the setter comes free and stays so.
			if (TableKey.holding() || TableCouldBegin())
			{
				In.light = In.heavy = In.release = false;
			}
			const bool bStart = !TableKey.holding() && TableCanBegin();
			TableKeyPresses += bStart ? 1 : 0;
			bReleaseDown = (TableKeyPresses % 2) == 1;
			In.release = In.release || bStart;
			StepTableKey(In);
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
#if !UE_BUILD_SHIPPING
		StepAimCheck(In);
#endif
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
		StepTableKey(In);
	}
	const bool bBoss = StoneBearer.IsActive();
	if (bBoss)
	{
		StoneBearer.Buttons(bIntercept, In.guard, Bodies);
		StoneBearer.BeforeTick();
		LogStoneMoveMask();
		StoneBearerTurnBeforeTick();
	}
	PrevBodies = Bodies;
	PrevHeldTime = static_cast<double>(Fight.heldTime());
	HoldPausedEnemies();
	if (bAllyOn)
	{
		if (bAutoPlay)
		{
			In.takeHalf = bTakeHalfAuto && Fight.takeHalfDue(0.1f);
			// -ArenaAutoBlock: the guard held from the start of any enemy wind-up (so it blocks, never parries) in place
			// of the autopilot's parry, roll or swing, for a capture of block Load.
			if (bAutoBlock)
			{
				const ftcore::ArenaOutput& Was = Fight.output();
				for (size_t I = 0; I < Fight.enemyCount(); ++I)
				{
					if (Fight.enemyHp(I) > 0 && (Was.enemies[I].part == ftcore::ArenaPart::Startup
						|| Was.enemies[I].part == ftcore::ArenaPart::Active))
					{
						In.guard = true;
						In.dodge = In.light = In.heavy = In.release = false;
						break;
					}
				}
			}
		}
		MoveAlly(); // after PrevBodies: the ally is drawn from where it stood
	}
	if (HallMode != 0)
	{
		StepHallucination(In);
	}
	PressSession(In);
	if (Session)
	{
		// The gift Guard scales every blow the lead takes (1 unguarded).
		In.takenScale = Session->Get().giftSlot().guardScaleFor(SessionLead, Session->Get().now());
	}
	const ftcore::ArenaOutput& Out = Fight.tick(StepSeconds, In, Bodies);
	ftcore::arenaStep(Fight, Bodies, StepSeconds, Fit.zone);
	if (HallMode != 0)
	{
		AfterHallucinationStep(Out);
	}
	// In a hit pause the figure's clips stand still (its clock stopped), so what drives its legs holds too: the body
	// standing for the pause is not a stop, and easing the gait, the strike's walk share or the legs' turn under a
	// frozen clip would move the feet with nothing else moving.
	const bool bLeadPaused = Out.player.stopLeft > 0.f && PlayerStepAt >= 0;
	if (!bLeadPaused)
	{
		const float DX = Bodies.player.x - PrevBodies.player.x, DY = Bodies.player.y - PrevBodies.player.y;
		float StepNow = FMath::Sqrt(DX * DX + DY * DY) / StepSeconds;
		// A step faster than any the body makes is a teleport: the legs start over from a stand. A facing turned
		// further than a running turn in one step (a snap) starts the history over too.
		const ftcore::Vec2 DrawnWay = PlayerDrawFacing(Out.player);
		const float Facing = FMath::Atan2(DrawnWay.y, DrawnWay.x);
		const bool bSnapped = PlayerStepAt >= 0 && FMath::Abs(FMath::FindDeltaAngleRadians(PlayerStepFacing, Facing)) > kStepSnapTurn;
		PlayerStepFacing = Facing;
		if (!(StepNow <= kMaxStepSpeed))
		{
			StepNow = 0.f;
			PlayerStepAt = -1;
			if (PlayerFigure)
			{
				PlayerFigure->ResetFeet(); // pushed or put elsewhere: nothing held stays behind
			}
		}
		if (bSnapped)
		{
			PlayerStepAt = -1;
		}
		// After a start, a teleport or a snap the history is filled with this step.
		if (PlayerStepAt < 0)
		{
			PlayerSteps[0] = PlayerSteps[1] = PlayerSteps[2] = StepNow;
			PlayerStepAt = 0;
		}
		PlayerSteps[PlayerStepAt] = StepNow;
		PlayerStepAt = (PlayerStepAt + 1) % 3;
		const float A = PlayerSteps[0], B = PlayerSteps[1], C = PlayerSteps[2];
		PlayerStepSpeed = FMath::Max(FMath::Min(A, B), FMath::Min(FMath::Max(A, B), C));
	}
	if (bBoss && StoneBearer.AfterTick(Out.events, Bodies))
	{
		// Onto its plinth: put there, not walked there.
		PrevBodies.enemies[StoneBearer.GetSlot()] = Bodies.enemies[StoneBearer.GetSlot()];
	}
	if (bBoss)
	{
		LogStoneMoveMask(); // a blow inside the tick can change it
		StoneBearerWalk(Out);
		StoneBearerTurn(Out);
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
	// The lead's strike clips run on the held clock, so they stand still through its hit pauses as its action does.
	LeadWeapons.Feed(Out.events, static_cast<double>(Fight.heldTime()));
	FeedSession();
	// The events (this step's Released pooled), then a gift held while Lone: the order is Core's (LoneGiftHold::stepEnd).
	LoneGift.stepEnd([this, &Out] { HandleEvents(Out.events); }, [this] { PressPendingLoneGift(); });
	TickTable(Out);
	TrackParts(Out);
	// The lead's aim: the locked enemy, else the current attack's soft target, else the nearest one standing, on the near
	// side of its body at its chest; with nobody, a metre ahead at the lead's chest.
	{
		const FVector LeadChest = PlayerFigure ? PlayerFigure->GetChest() : ToWorld(Bodies.player) + FVector(0.0, 0.0, 130.0);
		int32 Target = -1;
		const std::uint8_t Lock = Fight.lock();
		const int32 Count = FMath::Min(static_cast<int32>(Fight.def().enemyCount), static_cast<int32>(MaxEnemies));
		if (Lock != ftcore::kArenaNobody && Lock < Count && Fight.enemyHp(Lock) > 0)
		{
			Target = Lock;
		}
		else if (Out.player.soft != ftcore::kArenaNobody && Out.player.soft < Count && Fight.enemyHp(Out.player.soft) > 0)
		{
			Target = Out.player.soft;
		}
		float Best = TNumericLimits<float>::Max();
		for (int32 I = 0; Target < 0 && I < Count; ++I)
		{
			if (Fight.enemyHp(static_cast<std::size_t>(I)) <= 0)
			{
				continue;
			}
			const ftcore::Vec2 D{Bodies.enemies[I].x - Bodies.player.x, Bodies.enemies[I].y - Bodies.player.y};
			Best = FMath::Min(Best, D.x * D.x + D.y * D.y);
		}
		for (int32 I = 0; Target < 0 && Best < TNumericLimits<float>::Max() && I < Count; ++I)
		{
			const ftcore::Vec2 D{Bodies.enemies[I].x - Bodies.player.x, Bodies.enemies[I].y - Bodies.player.y};
			if (Fight.enemyHp(static_cast<std::size_t>(I)) > 0 && D.x * D.x + D.y * D.y <= Best)
			{
				Target = I;
			}
		}
		if (Target >= 0)
		{
			const ftcore::Vec2 Hit = ftcore::fieldHitPoint(Bodies.player, Bodies.enemies[Target], Fight.def().enemies[Target].bodyRadius);
			const AFTArenaFighter* Enemy = EnemyFigures[Target].Get();
			const FVector At = ToWorld(Hit);
			const ftcore::Vec2 To{Bodies.enemies[Target].x - Bodies.player.x, Bodies.enemies[Target].y - Bodies.player.y};
			const float Len = FMath::Sqrt(To.x * To.x + To.y * To.y);
			bLeadAimDir = Len > 1e-3f;
			if (bLeadAimDir)
			{
				LeadAimDir = ftcore::Vec2{To.x / Len, To.y / Len};
			}
			LeadWeapons.SetAim(FVector(At.X, At.Y, Enemy ? Enemy->GetChest().Z : LeadChest.Z), true);
		}
		else
		{
			bLeadAimDir = false;
			// The rules' facing, not the drawn one: the shot goes where the rules aim it, so the arm may lead the body by a
			// few frames of its turn.
			LeadWeapons.SetAim(LeadChest + FVector(Out.player.facing.x, Out.player.facing.y, 0.0) * 100.0, false);
		}
		// Aimed with nothing locked: the shot, the bolt and the raised arm go at the reticle point.
		if (ArenaAim.alpha() > 0.f && Lock == ftcore::kArenaNobody)
		{
			LeadWeapons.SetAim(ReticlePoint, true);
		}
		LeadWeapons.SetAimHold(ArenaAim.alpha());
		// The share from the walk the body makes: its measured speed less the attack's lunge while it lunges (startup
		// and active), so a strike's own lunge keeps the whole-body clip rather than putting the legs on the walk.
		float Walk = PlayerStepSpeed;
		const bool bAttacking = Out.player.action == ftcore::ArenaAction::Light || Out.player.action == ftcore::ArenaAction::Heavy
			|| Out.player.action == ftcore::ArenaAction::Riposte || Out.player.action == ftcore::ArenaAction::Release;
		if (const ftcore::ArenaAttack* Swing = bAttacking ? ftcore::arenaPlayerAttack(Fight.def().fighter, Out.player.move) : nullptr)
		{
			const float Span = Swing->startup + Swing->active;
			if (Span > 0.f && Out.player.actionTime <= Span)
			{
				Walk = FMath::Max(0.f, Walk - FMath::Abs(Swing->lunge) / Span);
			}
		}
		const float Want = ftcore::attackUpperShare(AttackMoveTable, Walk);
		// The air light has no clip of its own yet: the strike plays on the upper body only, over the jump's legs.
		const bool bAirStrike = bAttacking && Out.player.move == ftcore::kArenaAirMove;
		if (bAirStrike)
		{
			LeadUpperShare = 1.f;
		}
		else if (!bLeadPaused)
		{
			LeadUpperShare = FMath::Clamp(Want, LeadUpperShare - kUpperShareRate * StepSeconds, LeadUpperShare + kUpperShareRate * StepSeconds);
		}
		LeadWeapons.SetActionUpper(LeadUpperShare);
		// A kick in the air (the guns kit's heavy clip, until its own air clip exists): the kicking leg, hip down, keeps
		// the ground kick over the fall's other leg and pelvis.
		const ftcore::FightClip AirClip = ftcore::arenaHeavyStrike(Fight.def().fighter);
		LeadWeapons.SetActionLeg(!bAirStrike ? -1
			: AirClip == ftcore::FightClip::KickRight ? 1 : AirClip == ftcore::FightClip::KickLeft ? 0 : -1);
		// The legs toward the way the body went, where they walk: on the locomotion (no strike, or by the strike's
		// upper share). Standing, dodging or under a whole-body clip they ease back under the facing.
		if (!bLeadPaused)
		{
			const float DX = Bodies.player.x - PrevBodies.player.x, DY = Bodies.player.y - PrevBodies.player.y;
			const ftcore::ArenaAction Act = Out.player.action;
			const float Legs = (Act == ftcore::ArenaAction::Idle || Act == ftcore::ArenaAction::Move || Act == ftcore::ArenaAction::Guard
				|| Act == ftcore::ArenaAction::Heal) ? 1.f : (bAttacking ? LeadUpperShare : 0.f);
			float LegTarget = 0.f;
			if (Legs > 0.f && PlayerStepSpeed > 0.3f)
			{
				const ftcore::Vec2 Dir{DX, DY};
				LegTarget = Legs * FMath::Clamp(FMath::FindDeltaAngleDegrees(ArenaYaw(PlayerDrawFacing(Out.player)), ArenaYaw(Dir)), -kLegYawMax, kLegYawMax);
			}
			const float Step = kLegYawRate * StepSeconds;
			LeadLegYaw = FMath::Clamp(LegTarget, LeadLegYaw - Step, LeadLegYaw + Step);
		}
	}
	LeadWeapons.Track(Out.player, static_cast<double>(Fight.heldTime()));
	LeadWeapons.HitPause(Out.player.stopLeft > 0.f);
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
	// The street crowd hears the fight (nothing when the map has no crowd).
	if (UFTCrowdSubsystem* Crowd = UFTCrowdSubsystem::Get(GetWorld()))
	{
		Crowd->FeedArena(Events);
	}
	for (const ftcore::ArenaEvent& E : Events)
	{
		KickJolt(E);
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
				const bool bHeavy = E.move == ftcore::kArenaHeavyMove || E.move == ftcore::kArenaRiposteMove || E.move == ftcore::kArenaReleaseMove;
				const double HitWas = HitSince[E.target];
				const bool bFlinches = Flinches(E.target);
				StartFlinch(E.target, bHeavy);
				if (bHeavy && static_cast<int32>(E.target) == StoneBearerSlot)
				{
					// Logged for the heavy reaction checks: whether the heavy reaction is what shows, else what holds it.
					const ftcore::ArenaBrain Brain = Fight.output().enemies[E.target].brain;
					const TCHAR* Shows = !bFlinches ? (Fight.enemyBreakGuard(static_cast<size_t>(E.target)) ? TEXT("no: its break move's windup or active part")
							: TEXT("no: on its knees or getting up"))
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
				BossHitVfx(E);
				EnemyHitTime[E.target] = Now;
				StartFlinch(E.target, true);
			}
			break;
		case ftcore::ArenaEventType::AllyHit:
			AllyHitTime = Now;
			break;
		case ftcore::ArenaEventType::TableShare:
			// A set table passed part of a blow on: the body that took it flashes as hit, and it joins the popups.
			if (E.target == ftcore::kArenaPlayer)
			{
				PlayerHitTime = Now;
			}
			else if (E.target == ftcore::kArenaAlly)
			{
				AllyHitTime = Now;
			}
			else if (E.target < MaxEnemies)
			{
				EnemyHitTime[E.target] = Now;
			}
			// Under -ArenaCountLift a table share is a shared blow: offered to the lock (the kite's seam) as take half is.
			if (bCountLift && Session)
			{
				const ftcore::UnlockResult Unlock = Session->Get().offerKey(ftcore::kUnlockSharedBlow);
				++CarrySharedOffers;
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: a table share offered a shared blow: unlock result %d."), static_cast<int32>(Unlock));
			}
			break;
		case ftcore::ArenaEventType::AllyDowned:
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: the ally is down at %.2f s."), Fight.time());
			// The partner down: its kept gauge stores and spends nothing for the rest of the fight.
			if (Session && Session->Get().keepsLoad(Session->Get().keptLead()))
			{
				Session->Get().setKeptDown(true);
			}
			break;
		case ftcore::ArenaEventType::TookHalf:
			// Taking half is a shared input: offered to the boss script's point (or the session's lock) as a shared blow.
			if (Session)
			{
				const ftcore::UnlockResult Unlock = Session->Get().offerKey(ftcore::kUnlockSharedBlow);
				++CarrySharedOffers;
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: take half offered a shared blow: unlock result %d."), static_cast<int32>(Unlock));
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: took half of enemy %d's blow (%d hp) at %.2f s."), static_cast<int32>(E.source),
				E.amount, Fight.time());
			break;
		case ftcore::ArenaEventType::Released:
			ReleaseTime = Now;
			ReleaseSpent = E.amount;
			if (Session && Fight.lone())
			{
				const bool bPooled = Session->Get().poolRelease(SessionLead, E.amount);
				LoneReleases += bPooled ? 1 : 0;
				LoneBreaches += bPooled ? 0 : 1;
			}
			// A releaseMark chair: the Release marks its target (the lock target) for the next joined count on it.
			if (bCountLift && Session && CarryChairs.releaseMark() > 0.f)
			{
				const std::uint8_t Target = Fight.lock();
				if (Target < MaxEnemies && Session->Get().markEnemy(Target, CarryChairs.releaseMark(), CarryChairs.releaseMarkSeconds()))
				{
					++Marks;
					UE_LOG(LogForgottenThird, Display, TEXT("Arena: Release marked enemy %d (+%.2f until %.2f s)."), static_cast<int32>(Target),
						CarryChairs.releaseMark(), Session->Get().mark(Target).until);
				}
			}
			continue; // a banner, not a popup
		case ftcore::ArenaEventType::Overloaded:
			OverloadTime = Now;
			continue; // a banner, not a popup
		case ftcore::ArenaEventType::ReadCounter:
			if (E.target < MaxEnemies)
			{
				BossHitVfx(E);
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
		case ftcore::ArenaEventType::ReloadStarted:
		case ftcore::ArenaEventType::Reloaded:
			OnLeadReload(E.type == ftcore::ArenaEventType::ReloadStarted, E.amount);
			PlayGunCue(E);
			continue; // no popup
		case ftcore::ArenaEventType::DryPress:
			OnLeadDryPress(E.amount);
			PlayGunCue(E);
			continue; // no popup
		case ftcore::ArenaEventType::SteppedBack: // drawn as a backward step (DriveSkin); no popup
			if (E.source < ftcore::kMaxArenaEnemies)
			{
				StepBackSpeed[E.source] = static_cast<float>(E.amount) / 100.f / ftcore::kArenaStepBackTime;
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.3f: enemy %d steps back %.2f m from the lead over %.2f s (a break-out with no token or out of sight)."),
				Fight.time(), static_cast<int32>(E.source), static_cast<float>(E.amount) / 100.f, ftcore::kArenaStepBackTime);
			continue;
		case ftcore::ArenaEventType::BreakTell: // the boss's own break move is its tell; no HUD of its own
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d's break value tells (%.2f of %.2f)."), Fight.time(),
				static_cast<int32>(E.source), static_cast<float>(E.amount) / 100.f,
				E.source < Fight.enemyCount() ? Fight.enemyBreakAt(E.source) : 0.f);
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
		const FVector Tell = ToWorld(StoneTellAt);
		BossVfx.Play(FFTArenaBossVfx::EEffect::Impact, Tell + FVector(0.0, 0.0, Lift), StoneTellShapeMul, TEXT("blow lands"));
		const uint8 BossMove = StoneBearer.BossMoveOf(Out.move);
		const ftcore::StoneBearerDef& D = StoneBearer.GetDef();
		FVector Part;
		const char* Socket = BossMove < D.boss.moveCount && D.boss.moves[BossMove].hasContact ? D.boss.moves[BossMove].contactSocketChars.data() : nullptr;
		// The contact's socket is a label, not always a bone: the carried piece's striking edge rides the piece's bone.
		if (Socket && FCStringAnsi::Strcmp(Socket, "slab_edge") == 0)
		{
			Socket = "slab";
		}
		else if (Socket && bStoneMoveLeft && FCStringAnsi::Strcmp(Socket, "hand_r") == 0)
		{
			Socket = "hand_l"; // its left-hand clip strikes with the other hand
		}
		if (Socket && Figure && Figure->GetSkinPoint(FName(Socket), Part))
		{
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: boss move %d tell-to-impact %.2f m (%s, dust 0.00 m from the tell)."), Fight.time(),
				static_cast<int32>(Out.move), FVector::Dist2D(Part, Tell) / 100.0, ANSI_TO_TCHAR(Socket));
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
		// A blow whose startup drew no tell: its dust where it lands now.
		BossVfx.Play(FFTArenaBossVfx::EEffect::Impact, At, ShapeMul, TEXT("blow lands"));
		StoneTellMove = 255;
		return;
	}
	StoneTellAt = S.To;
	StoneTellMove = Out.move;
	StoneTellShapeMul = ShapeMul;
	StoneTellFacing = Out.facing;
	StoneTellFigurePos = Figure ? Figure->GetActorLocation() : FVector::ZeroVector;
	StoneTellFigureYaw = Figure ? Figure->GetActorRotation().Yaw : 0.f;
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
	// On its knees the body is the clip's, not the capsule's (no root motion yet): out from the pelvis, at its height.
	FVector Pelvis;
	const AFTArenaFighter* Figure = EnemyFigures[StoneBearerSlot].Get();
	if (StoneKneelShows() && Figure && Figure->GetSkinPoint(FName(TEXT("pelvis")), Pelvis))
	{
		BossVfx.Play(FFTArenaBossVfx::EEffect::Chips, Pelvis + FVector(Dir.x * Out * 100.0, Dir.y * Out * 100.0, 0.0), 1.f, TEXT("hit on its knees"));
		return;
	}
	FVector At = ToWorld(ftcore::Vec2{Boss.x + Dir.x * Out, Boss.y + Dir.y * Out});
	At.Z = PlayerFigure ? PlayerFigure->GetChest().Z : At.Z + 100.0;
	BossVfx.Play(FFTArenaBossVfx::EEffect::Chips, At, 1.f, TEXT("hit"));
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
	DrawnTime = FMath::Max(0.0, FMath::Lerp(PrevHeldTime, static_cast<double>(Fight.heldTime()), static_cast<double>(Alpha)));

	if (PlayerFigure)
	{
		const ftcore::ArenaPlayerOut& P = Out.player;
		FFTArenaPose Pose;
		Pose.Feet = ToWorld(ArenaLerp(PrevBodies.player, Bodies.player, Alpha));
		Pose.Yaw = ArenaYaw(PlayerDrawFacing(P)); // the drawn facing: a lock's snap is drawn as a turn
#if !UE_BUILD_SHIPPING
		const bool bDodging = P.action == ftcore::ArenaAction::Dodge;
		if (bDodging && !bWasDodging)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: roll starts at %.2f s"), Fight.time());
		}
		bWasDodging = bDodging;
		// The jump's take-off and its end: the first frame drawn off the jump's clock, and the action then (free when
		// Idle or Move; a blow in the air ends it in Hitstun).
		if (P.jumpTime >= 0.f && JumpStartedAt < 0.0)
		{
			JumpStartedAt = Fight.time() - P.jumpTime;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: jump takes off at %.2f s"), JumpStartedAt);
		}
		else if (P.jumpTime < 0.f && JumpStartedAt >= 0.0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: jump ends at %.2f s, %.2f s after take-off, into %s"), Fight.time(),
				Fight.time() - JumpStartedAt, UTF8_TO_TCHAR(ftcore::arenaActionName(P.action)));
			JumpStartedAt = -1.0;
		}
#endif
		// The legs at the pace the body is drawn moving (the last steps' median), not the sim's wish (which the
		// bodies' push may have cut): a cycle faster than the ground goes by is a glide.
		Pose.Speed = PlayerStepSpeed;
		Pose.LegYaw = LeadLegYaw;
		// Where the walk heads (the stick's share of the pace; unknown in an attack or a roll) and how far the body
		// would slide at the brake (Core's). A hit pause keeps the last (Core has none there), so a start or a stop under
		// way isn't sent back to the gaits by the pause.
		if (P.stopLeft <= 0.f)
		{
			LeadWantSpeed = P.wantSpeed;
			LeadStopDistance = P.stopDistance;
		}
		Pose.WantSpeed = LeadWantSpeed;
		Pose.StopDistance = LeadStopDistance;
		// The feet held while a strike's clip has the legs (its walk share under half) and through a hit pause: the
		// body's lunge goes over planted feet, which step when it has gone too far.
		{
			const ftcore::ArenaAction Act = P.action;
			const bool bStrike = Act == ftcore::ArenaAction::Light || Act == ftcore::ArenaAction::Heavy || Act == ftcore::ArenaAction::Riposte
				|| Act == ftcore::ArenaAction::Release;
			Pose.bFootLock = (bStrike && LeadUpperShare < 0.5f) || (P.stopLeft > 0.f && Act != ftcore::ArenaAction::Dodge);
		}
		Pose.Tint = PlayerFigure->GetBaseColor();
		const ftcore::ArenaAttack* Attack = nullptr;
		switch (P.action)
		{
		case ftcore::ArenaAction::Light:
			// A gun kit's aimed round is a light with its own move id; its numbers are the kit's shot.
			if (P.move == ftcore::kArenaShotMove)
			{
				Attack = ftcore::arenaPlayerAttack(D.fighter, P.move);
				Pose.Swing = EFTArenaSwing::Across;
				break;
			}
			// The air light: its own move id and numbers (the kit's jumpLight), a downward swing.
			if (P.move == ftcore::kArenaAirMove)
			{
				Attack = ftcore::arenaPlayerAttack(D.fighter, P.move);
				Pose.Swing = EFTArenaSwing::Overhead;
				break;
			}
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
			const ftcore::Vec2 F = PlayerDrawFacing(P); // the way the body is drawn facing
			const ftcore::Vec2& Dir = P.dodgeDir;
			Pose.DodgeLocal = FVector2D(Dir.x * F.x + Dir.y * F.y, Dir.y * F.x - Dir.x * F.y);
			Pose.DodgeFraction = D.fighter.dodgeTime > 0.f ? P.actionTime / D.fighter.dodgeTime : 1.f;
			Pose.DodgeTime = D.fighter.dodgeTime;
			Pose.bDodgeBack = P.dodgeBack;
			Pose.DodgeSide = P.dodgeSide;
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
			// The lead turns to its aim target while it attacks (the first half of the startup), holds it
			// through the active part and turns back to Core's facing over the recovery.
			if (bLeadAimDir)
			{
				const float S = FMath::Max(Attack->startup, 1e-3f);
				const float T = P.actionTime;
				float W = FMath::SmoothStep(0.f, 1.f, T / (0.5f * S));
				if (T > S + Attack->active)
				{
					W = 1.f - FMath::SmoothStep(0.f, 1.f, (T - S - Attack->active) / FMath::Max(Attack->recovery, 1e-3f));
				}
				const float Delta = FMath::FindDeltaAngleDegrees(Pose.Yaw, ArenaYaw(LeadAimDir));
				Pose.Yaw += W * Delta;
				// The legs keep the way they went: what the body turns, they turn back.
				Pose.LegYaw = FMath::Clamp(Pose.LegYaw - W * Delta, -kLegYawMax, kLegYawMax);
			}
		}
		// The jump, on Core's air clock (an air light's too): the body at the arc's height at the time drawn (a step
		// behind, Alpha of the way to the last; a hit pause holds it), the clips told it is in the air from take-off to
		// touch-down (rising while the arc climbs) and no foot held. The landing is on the ground: the land clip plays.
		if (P.jumpTime >= 0.f)
		{
			const ftcore::ArenaFighter& Kit = D.fighter;
			const float Behind = P.stopLeft > 0.f ? 0.f : (1.f - Alpha) * StepSeconds;
			const float AirT = FMath::Max(0.f, P.jumpTime - Behind) - Kit.jumpStartup;
			const float Air = FMath::Max(Kit.jumpAir, 1e-3f);
			const float U = FMath::Clamp(AirT / Air, 0.f, 1.f);
			Pose.bAir = AirT < Kit.jumpAir;
			Pose.AirHeight = AirT > 0.f ? 100.f * Kit.jumpHeight * 4.f * U * (1.f - U) : 0.f;
			Pose.AirRise = 100.f * Kit.jumpHeight * 4.f * (1.f - 2.f * U) / Air;
			Pose.AirFraction = AirT > 0.f && Pose.bAir ? U : -1.f;
			Pose.bFootLock = false;
			Pose.JumpClock = P.jumpTime;
		}
		if (P.invulnerable)
		{
			Pose.Tint = FMath::Lerp(Pose.Tint, FLinearColor(0.85f, 0.95f, 1.f), 0.45f);
		}
		Pose.Tint = FMath::Lerp(Pose.Tint, FLinearColor(1.f, 0.2f, 0.15f), ArenaModePulse(Now, PlayerHitTime, kArenaHitTint));
		PlayerFigure->ApplyPose(Pose);
		// The strike clips at the fight time drawn (the bodies are drawn a step behind, Alpha of the way to the last), and
		// the gun's reload overlay by the fight's own progress.
		LeadWeapons.SetReload(Fight.reloading(), Fight.reloadProgress());
		LeadWeapons.Paint(DrawnTime);
	}
	PlaceAlly(Alpha);
	PaintTable(Alpha);

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
		if (I == StoneBearerSlot && StoneTurnSign != 0)
		{
			// Mid-turn, the turn's own yaw at the frame's time, in step with its clip (Core's facing is the last step's).
			Pose.Yaw = ArenaYaw(StoneTurnFacing(static_cast<double>(Fight.time()) + static_cast<double>(Alpha) * StepSeconds - StoneTurnStart));
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
				ftcore::ArenaAttack Timed = Move.attack;
				Timed.startup += E.unseen ? ftcore::kArenaUnseenWindup : 0.f; // an unseen start's longer windup
				Pose.PartFraction = ArenaPartFraction(Timed, E.part, E.stateTime);
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
		case ftcore::ArenaBrain::Flinch:
			// A placeholder until flinch clips exist: a small lean back, easing out over the flinch (it is pushed, not walking).
			Pose.LeanBack = 7.f * FMath::Clamp(1.f - E.stateTime / FMath::Max(Kind.flinchTime, 1e-3f), 0.f, 1.f);
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

void UFTArenaRunner::TickAim(float DeltaSeconds)
{
	if (!bAimRulesTried)
	{
		bAimRulesTried = true;
		ftcore::AimRules Rules;
		TArray<uint8> Bytes;
		std::string Error;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/aim.json");
		if (!(FFileHelper::LoadFileToArray(Bytes, *File) && ftcore::importAimRules(FT::BytesAsText(Bytes), Rules, &Error)))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Battle/aim.json; the aim uses its built-in rules.\n%s"), *FT::ToFString(Error));
			Rules = ftcore::AimRules();
		}
		ArenaAim.setRules(Rules);
		ftcore::AttackMoveRules Moves;
		const FString MoveFile = FPaths::ProjectDir() / TEXT("Data/Battle/attack_move.json");
		Bytes.Reset();
		if (!(FFileHelper::LoadFileToArray(Bytes, *MoveFile) && ftcore::importAttackMoveRules(FT::BytesAsText(Bytes), Moves, &Error)))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Battle/attack_move.json; attacks use built-in move rules: %s"), *FT::ToFString(Error));
			Moves = ftcore::AttackMoveRules();
		}
		AttackMoveTable = Moves;
	}
#if !UE_BUILD_SHIPPING
	if (bCoverCheck && !Fight.over() && Fight.time() >= 1.f)
	{
		StepCoverCheck();
	}
	// -ArenaAimCheck holds aim while it aims and fires, and through the assist sweep until that lets aim go.
	const bool bProbeAim = AimCheckPhase == 2 || AimCheckPhase == 3 || (AimCheckPhase == 5 && !bAssistAimOff);
#else
	const bool bProbeAim = false;
#endif
	ArenaAim.tick(DeltaSeconds, ((bAimHeld && !bAutoPlay && !bIdle) || bProbeAim) && !Fight.over(), ftcore::FieldBody::Free);
	// The reticle: the camera's line to the first thing it meets within reticleRange, the lead's figure not counted.
	const APlayerController* PC = GetPC();
	if (ArenaAim.alpha() > 0.f && PC && PC->PlayerCameraManager)
	{
		const FVector From = PC->PlayerCameraManager->GetCameraLocation();
		const FRotator View = ViewRotation(*PC->PlayerCameraManager);
		const FVector To = From + View.Vector() * (double(ArenaAim.rules().reticleRange) * 100.0);
		FHitResult Hit;
		double BodyT = 1.0;
		ReticleEnemy = PickReticle(From, To, Hit, bReticleHit, BodyT);
		ReticlePoint = ReticleEnemy >= 0 ? FMath::Lerp(From, To, BodyT) : (bReticleHit ? FVector(Hit.ImpactPoint) : To);
		bReticleHit = bReticleHit || ReticleEnemy >= 0;
		ReticleHeadingPoint = ReticlePoint;
		if (const AFTArenaFighter* OnBody = ReticleEnemy >= 0 ? GetEnemyFigure(ReticleEnemy) : nullptr)
		{
			// On a body: head for the middle of the stretch of the crosshair's line inside the body (from where it
			// enters to where it leaves), a point inside it, so the round from the lead's muzzle goes into the body, not
			// past its edge or under a hanging or wide one, as a point near its axis could at a steep look.
			const FVector At = OnBody->GetActorLocation();
			const ftcore::ArenaBodyCylinder Cyl = ftcore::arenaBodyCylinder(Fight.def().enemies[static_cast<size_t>(ReticleEnemy)],
				ftcore::Vec2{static_cast<float>(At.X / 100.0), static_cast<float>(At.Y / 100.0)}, static_cast<float>(At.Z / 100.0));
			const ftcore::ArenaPoint3 A{static_cast<float>(From.X / 100.0), static_cast<float>(From.Y / 100.0), static_cast<float>(From.Z / 100.0)};
			const ftcore::ArenaPoint3 B{static_cast<float>(To.X / 100.0), static_cast<float>(To.Y / 100.0), static_cast<float>(To.Z / 100.0)};
			float Back = 0.f;
			if (ftcore::arenaRayEntersBody(B, A, Cyl, Back))
			{
				ReticleHeadingPoint = FMath::Lerp(From, To, 0.5 * (BodyT + FMath::Clamp(1.0 - static_cast<double>(Back), BodyT, 1.0)));
			}
			else
			{
				// The line's far end lies inside the body (or the line only grazes it, where the way back in is float
				// error): the body's centre line at the entry's height, a point inside it whatever the edge does.
				const FVector Entry = FMath::Lerp(From, To, BodyT);
				ReticleHeadingPoint = FVector(At.X, At.Y, FMath::Clamp(Entry.Z / 100.0, double(Cyl.base), double(Cyl.top)) * 100.0);
			}
		}
		if (!bReticleHit)
		{
			const FVector Flat = View.Vector().GetSafeNormal2D();
			ReticleHeadingPoint = From + Flat * (double(ArenaAim.rules().reticleRange) * 100.0);
		}
		// A guns kit's round from the lead along the same heading and climb would reach no body: the crosshair is on
		// one past the shot's range (another body in the way still takes the round, so that is not out of reach).
		// Another body on the muzzle's line before the one under the crosshair (off the camera's line) takes the round:
		// the crosshair shows it blocked.
		bReticleOutOfReach = false;
		bReticleTakes = false;
		float Pitch = 0.f;
		if (ReticleEnemy != BlockedFor)
		{
			bReticleBlocked = false; // a new body under the crosshair starts clear
			BlockedRun = 0;
			BlockedFor = ReticleEnemy;
		}
		// Locked on, the round goes at the lock, not the crosshair, so the crosshair says nothing about its reach.
		if (Fight.lock() == ftcore::kArenaNobody && Fight.def().fighter.weapon == ftcore::ArenaWeapon::Guns && ReticlePitch(Pitch))
		{
			const float Yaw = FMath::DegreesToRadians(ViewYaw());
			const ftcore::Vec2 Dir = ReticleAimDir(ftcore::Vec2{FMath::Cos(Yaw), FMath::Sin(Yaw)});
			// The round's own rule: the crosshair's body wins within kArenaShotGraze of another met first (a graze where
			// the two bodies' edges cross on the muzzle's line), so blocked here is blocked for the round.
			const uint8 Meets = Fight.shotMeets(Dir, Pitch, nullptr, 0u, ReticleAimBody());
			bReticleOutOfReach = ReticleEnemy >= 0 && Meets == ftcore::kArenaNobody;
			const bool bBlockedNow = ReticleEnemy >= 0 && Meets != ftcore::kArenaNobody && static_cast<int32>(Meets) != ReticleEnemy;
			// Raw, not held: aim assist slows only while the round would take the crosshair's body this frame.
			bReticleTakes = ReticleEnemy >= 0 && static_cast<int32>(Meets) == ReticleEnemy;
			// Shown blocked or clear only once the new state has held kBlockHoldFrames frames, so a crosshair on the
			// edge does not flicker.
			BlockedRun = bBlockedNow != bReticleBlocked ? BlockedRun + 1 : 0;
			if (BlockedRun >= kBlockHoldFrames)
			{
				bReticleBlocked = bBlockedNow;
				BlockedRun = 0;
			}
		}
		else
		{
			bReticleBlocked = false;
			BlockedRun = 0;
		}
#if !UE_BUILD_SHIPPING
		if (AimCheckPhase == 2 && Now - AimCheckSince >= 1.0)
		{
			// The view has settled on the top: what the crosshair meets, then one aimed shot.
			const AFTArenaFighter* Boss = GetEnemyFigure(AimCheckSlot);
			const AFTArenaFighter* Lead = PlayerFigure.Get();
			const double Floor = Lead ? Lead->GetActorLocation().Z : 0.0;
			const ftcore::ArenaPitchSplit Split = ftcore::arenaSplitPitch(ftcore::kArenaLook, CameraPitch);
			const bool bOnBoss = Boss && ReticleEnemy == AimCheckSlot;
			float ShotPitch = 0.f;
			const bool bPitched = ReticlePitch(ShotPitch);
			AimCheckLine = FString::Printf(TEXT("view pitch %.1f deg (limit %.0f; boom %.1f, lens %.1f), lens %.2f m over the floor and %.2f m from the lead's middle, crosshair on %s at %.1f m up, %.1f m from the lead; round %s"),
				CameraPitch, ftcore::kArenaLook.viewMax, Split.boom, Split.tilt, (From.Z - Floor) / 100.0,
				Lead ? FVector::Dist(From, Lead->GetActorLocation() + FVector(0.0, 0.0, 90.0)) / 100.0 : 0.0,
				bOnBoss ? TEXT("the boss") : (ReticleEnemy >= 0 ? TEXT("another enemy") : (bReticleHit && Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("nothing"))),
				(ReticlePoint.Z - Floor) / 100.0,
				Lead ? FVector::Dist2D(ReticlePoint, Lead->GetActorLocation()) / 100.0 : 0.0,
				*(bPitched ? FString::Printf(TEXT("pitched %.1f deg%s"), FMath::RadiansToDegrees(ShotPitch), bReticleOutOfReach ? TEXT(", out of reach") : (bReticleBlocked ? TEXT(", blocked") : TEXT(""))) : FString(TEXT("flat"))));
			bAimCheckOnBoss = bOnBoss;
			// The pitch the camera rendered (boom lag and lens tilt included, the jolt not), not the one asked for.
			AimCheckRendered = static_cast<float>(FRotator::NormalizeAxis(View.Pitch));
			bAimCheckPitchOk = FMath::Abs(AimCheckRendered - AimCheckPitch) < 1.5f;
			AimCheckHpBefore = static_cast<float>(Fight.enemyHp(static_cast<size_t>(AimCheckSlot)));
			AimCheckShotsBefore = Fight.stats().shots;
			AimCheckHitsBefore = Fight.stats().shotHits;
			AimCheckPhase = 3;
			AimCheckSince = Now;
			bAimCheckFire = true;
		}
#endif
	}
	else
	{
		bReticleBlocked = false; // the aim let go: a new aim starts clear
		bReticleTakes = false;
		BlockedRun = 0;
		BlockedFor = -1;
	}
}

int32 UFTArenaRunner::ReticleBody(const FVector& From, const FVector& To, double& T) const
{
	int32 Found = -1;
	const ftcore::ArenaPoint3 A{static_cast<float>(From.X / 100.0), static_cast<float>(From.Y / 100.0), static_cast<float>(From.Z / 100.0)};
	const ftcore::ArenaPoint3 B{static_cast<float>(To.X / 100.0), static_cast<float>(To.Y / 100.0), static_cast<float>(To.Z / 100.0)};
	const int32 Count = FMath::Min(static_cast<int32>(Fight.enemyCount()), static_cast<int32>(MaxEnemies));
	for (int32 I = 0; I < Count; ++I)
	{
		const AFTArenaFighter* Body = GetEnemyFigure(I);
		if (!Body || !Fight.enemyAlive(static_cast<size_t>(I)))
		{
			continue;
		}
		const FVector At = Body->GetActorLocation();
		const ftcore::ArenaBodyCylinder Cyl = ftcore::arenaBodyCylinder(Fight.def().enemies[static_cast<size_t>(I)],
			ftcore::Vec2{static_cast<float>(At.X / 100.0), static_cast<float>(At.Y / 100.0)}, static_cast<float>(At.Z / 100.0));
		float Enter = 0.f;
		if (ftcore::arenaRayEntersBody(A, B, Cyl, Enter) && static_cast<double>(Enter) < T)
		{
			T = static_cast<double>(Enter);
			Found = I;
		}
	}
	return Found;
}

int32 UFTArenaRunner::PickReticle(const FVector& From, const FVector& To, FHitResult& Hit, bool& bWorldHit, double& T) const
{
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(FTArenaAimReticle), false, PlayerFigure.Get());
	bWorldHit = GetWorld()->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Params);
	T = bWorldHit ? Hit.Time : 1.0;
	return ReticleBody(From, To, T); // only a body entered before the world's hit
}

#if !UE_BUILD_SHIPPING
void UFTArenaRunner::StepCoverCheck()
{
	bCoverCheck = false; // once
	int32 Slot = -1;
	const int32 Count = FMath::Min(static_cast<int32>(Fight.enemyCount()), static_cast<int32>(MaxEnemies));
	for (int32 I = 0; I < Count && Slot < 0; ++I)
	{
		Slot = GetEnemyFigure(I) && Fight.enemyAlive(static_cast<size_t>(I)) ? I : -1;
	}
	const AFTArenaFighter* Body = GetEnemyFigure(Slot);
	const ftcore::ArenaZone& Zone = Fit.zone;
	int32 Drum = -1;
	double Nearest = 0.0;
	for (int32 O = 0; Body && O < static_cast<int32>(Zone.obstacleCount); ++O)
	{
		const ftcore::ZoneObstacle& Piece = Zone.obstacles[static_cast<size_t>(O)];
		const double Dist = FVector::Dist2D(FVector(Piece.at.x * 100.0, Piece.at.y * 100.0, 0.0), Body->GetActorLocation());
		if (Piece.authored && !Piece.box && (Drum < 0 || Dist < Nearest))
		{
			Drum = O;
			Nearest = Dist;
		}
	}
	bool bPass = false;
	FString Line = TEXT("n/a (no standing enemy, or no cover drum in this zone)");
	if (Drum >= 0)
	{
		// From 0.5 m over the floor (the drums stand 1 m), 1.5 m past the drum's far side, over its centre to the body's
		// middle (as ReticleBody sizes it), carried on to the reticle's range: the line meets the drum low on its near face.
		const ftcore::ZoneObstacle& Piece = Zone.obstacles[static_cast<size_t>(Drum)];
		const FVector At = Body->GetActorLocation();
		const ftcore::ArenaBodyCylinder Cyl = ftcore::arenaBodyCylinder(Fight.def().enemies[static_cast<size_t>(Slot)],
			ftcore::Vec2{static_cast<float>(At.X / 100.0), static_cast<float>(At.Y / 100.0)}, static_cast<float>(At.Z / 100.0));
		const double Low = ZoneZ + 50.0;
		const FVector Centre(Piece.at.x * 100.0, Piece.at.y * 100.0, Low);
		const FVector Axis(At.X, At.Y, 50.0 * double(Cyl.base + Cyl.top));
		const FVector Way = (FVector(Axis.X, Axis.Y, Low) - Centre).GetSafeNormal2D();
		const FVector From = Centre - Way * (double(Piece.radius) * 100.0 + 150.0);
		const FVector To = From + (Axis - From).GetSafeNormal() * (double(ArenaAim.rules().reticleRange) * 100.0);
		const double Len = FVector::Dist(From, To) / 100.0;
		FHitResult Hit;
		bool bWorld = false;
		double T = 1.0;
		const int32 Pick = PickReticle(From, To, Hit, bWorld, T);
		double OpenT = 1.0;
		const int32 Open = ReticleBody(From, To, OpenT); // the same line with the world left out: the control
		// No body picked: aim assist has no body to slow over, and the round's aim body (its graze preference) is none.
		const uint8 AimBody = Pick >= 0 && Pick < static_cast<int32>(ftcore::kArenaNobody) ? static_cast<uint8>(Pick) : ftcore::kArenaNobody;
		bPass = bWorld && Open == Slot && Hit.Time < OpenT && Pick < 0 && AimBody == ftcore::kArenaNobody;
		Line = FString::Printf(TEXT("%s (enemy %d %.1f m from drum %d; a line from %.2f m over the floor %.1f m past the drum's far side to the body's middle %.2f m up: the world hit %s at %.2f m, the body at %s with the world left out; picked %s, so aim assist x%.2f and the round's aim body %s; a synthetic line, not the camera's)"),
			bPass ? TEXT("PASS") : TEXT("FAIL"), Slot, Nearest / 100.0, Drum, (Low - ZoneZ) / 100.0, 1.5, (Axis.Z - ZoneZ) / 100.0,
			bWorld && Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("nothing"), bWorld ? Hit.Time * Len : 0.0,
			*(Open >= 0 ? FString::Printf(TEXT("%.2f m (enemy %d)"), OpenT * Len, Open) : FString(TEXT("none (missed)"))),
			*(Pick >= 0 ? FString::Printf(TEXT("enemy %d"), Pick) : FString(TEXT("no body"))),
			ftcore::aimAssistTurn(true, true, Pick >= 0), AimBody == ftcore::kArenaNobody ? TEXT("none") : TEXT("set"));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: reticle behind cover %s."), *Line);
	if (bQuitOnFinish)
	{
		FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(bPass ? 0 : 1), TEXT("Arena"));
	}
}
#endif

FRotator UFTArenaRunner::ViewRotation(const APlayerCameraManager& Camera) const
{
	const FRotator Rendered = Camera.GetCameraRotation();
	return JoltModifier ? JoltModifier->Unjolted(Rendered) : Rendered;
}

uint8 UFTArenaRunner::ReticleAimBody() const
{
	return ReticleEnemy >= 0 && ReticleEnemy < static_cast<int32>(ftcore::kArenaNobody) ? static_cast<uint8>(ReticleEnemy) : ftcore::kArenaNobody;
}

ftcore::Vec2 UFTArenaRunner::ReticleAimDir(const ftcore::Vec2& Fallback) const
{
	const ftcore::Vec2 D{float(ReticleHeadingPoint.X / 100.0) - Bodies.player.x, float(ReticleHeadingPoint.Y / 100.0) - Bodies.player.y};
	// No reticle traced yet (the aim not blended in): the caller's own way.
	if (ArenaAim.alpha() <= 0.f || D.x * D.x + D.y * D.y <= 0.01f)
	{
		return Fallback;
	}
	const float L = FMath::Sqrt(D.x * D.x + D.y * D.y);
	return ftcore::Vec2{D.x / L, D.y / L};
}

bool UFTArenaRunner::ReticlePitch(float& OutPitch) const
{
	OutPitch = 0.f;
	const AFTArenaFighter* Lead = PlayerFigure.Get();
	if (!Lead || ArenaAim.alpha() <= 0.f)
	{
		return false;
	}
	const FVector Muzzle = Lead->GetActorLocation() + FVector(0.0, 0.0, double(ftcore::kArenaShotHeight) * 100.0);
	const double Flat = FVector::Dist2D(ReticleHeadingPoint, Muzzle);
	OutPitch = static_cast<float>(FMath::Atan2(ReticleHeadingPoint.Z - Muzzle.Z, FMath::Max(Flat, 1.0)));
	return true;
}

bool UFTArenaRunner::BodyPitch(int32 Slot, float& OutPitch) const
{
	OutPitch = 0.f;
	if (Slot < 0 || Slot >= static_cast<int32>(Fight.enemyCount()) || Slot >= static_cast<int32>(MaxEnemies) || !Fight.enemyAlive(static_cast<size_t>(Slot)))
	{
		return false;
	}
	const ftcore::ArenaBodyCylinder Body = ftcore::arenaBodyCylinder(Fight.def().enemies[static_cast<size_t>(Slot)], Bodies.enemies[static_cast<size_t>(Slot)], 0.f);
	const float DX = Body.at.x - Bodies.player.x, DY = Body.at.y - Bodies.player.y;
	OutPitch = FMath::Atan2((Body.base + Body.top) * 0.5f - ftcore::kArenaShotHeight, FMath::Max(FMath::Sqrt(DX * DX + DY * DY), 0.01f));
	return true;
}

int32 UFTArenaRunner::TallestStanding(float& OutTop) const
{
	int32 Best = -1;
	OutTop = 0.f;
	const int32 Count = FMath::Min(static_cast<int32>(Fight.enemyCount()), static_cast<int32>(MaxEnemies));
	for (int32 I = 0; I < Count; ++I)
	{
		if (!Fight.enemyAlive(static_cast<size_t>(I)))
		{
			continue;
		}
		const ftcore::ArenaEnemyKind& Foe = Fight.def().enemies[static_cast<size_t>(I)];
		const float Top = ftcore::arenaEnemyHeight(Foe) * (Foe.look.skinned() ? Foe.look.scale : 1.f) + (Foe.look.skinned() ? Foe.look.hover : 0.f);
		if (Top > OutTop)
		{
			OutTop = Top;
			Best = I;
		}
	}
	return Best;
}

bool UFTArenaRunner::AssistOverBody() const
{
	return ArenaAim.aiming() && Fight.lock() == ftcore::kArenaNobody && Fight.def().fighter.weapon == ftcore::ArenaWeapon::Guns
		&& ReticleEnemy >= 0 && Fight.enemyAlive(static_cast<size_t>(ReticleEnemy)) && bReticleTakes;
}

float UFTArenaRunner::TurnLook(float MouseX, float MouseY, float LookX, float LookY, float DeltaSeconds, bool bAssist)
{
	const ftcore::ArenaLookTurn Turn = ftcore::arenaLookStep(CameraYaw, CameraPitch, MouseX, MouseY, LookX, LookY, DeltaSeconds, bAssist,
		AssistOverBody());
	CameraYaw = Turn.yaw;
	CameraPitch = Turn.pitch;
	return Turn.stickKeep;
}

#if !UE_BUILD_SHIPPING
void UFTArenaRunner::StepAssistCheck(float DeltaSeconds)
{
	static const TCHAR* const StepNames[] = {TEXT("the sweep off the body and back"), TEXT("lock on and off"), TEXT("aim let go and held again"),
		TEXT("the target killed")};
	if (AssistStep > 3)
	{
		return;
	}
	const int32 Slot = AimCheckSlot;
	const AFTArenaFighter* Body = GetEnemyFigure(Slot);
	const APlayerController* PC = GetPC();
	if (!Body || !PC || !PC->PlayerCameraManager || Now - AssistStepAt > 6.0)
	{
		AssistFail = FString::Printf(TEXT("step %d, %s"), AssistStep, StepNames[AssistStep]);
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %.2f: aim assist check: %s ran out of time (part %d)."), Fight.time(), *AssistFail, AssistSub);
		AimCheckPhase = 4;
		AssistStep = 4;
		return;
	}
	// The view's pitch, and from step 1 its yaw, kept on the body's middle so the moving body stays under the crosshair;
	// only the sweep's own yaw is measured.
	const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
	const FVector At = Body->GetActorLocation();
	const ftcore::ArenaBodyCylinder Cyl = ftcore::arenaBodyCylinder(Fight.def().enemies[static_cast<size_t>(Slot)],
		ftcore::Vec2{static_cast<float>(At.X / 100.0), static_cast<float>(At.Y / 100.0)}, static_cast<float>(At.Z / 100.0));
	const FVector ToMid = FVector(At.X, At.Y, 50.0 * double(Cyl.base + Cyl.top)) - Eye;
	const float WantPitch = FMath::Clamp(static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(ToMid.Z, ToMid.Size2D()))),
		ftcore::kArenaLook.viewMin, ftcore::kArenaLook.viewMax);
	CameraPitch = FMath::FInterpConstantTo(CameraPitch, WantPitch, DeltaSeconds, 90.f);
	if (AssistStep > 0)
	{
		const float Off = FMath::FindDeltaAngleDegrees(CameraYaw, static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(ToMid.Y, ToMid.X))));
		CameraYaw += FMath::Clamp(Off, -180.f * DeltaSeconds, 180.f * DeltaSeconds);
	}
	const ftcore::Settings* Prefs = GetSettings();
	const bool bAssist = !Prefs || Prefs->aimAssist;
	const bool bLocked = Fight.lock() != ftcore::kArenaNobody;
	const auto PressLock = [this](const TCHAR* Why)
	{
		bAssistLockPress = true;
		AssistMarkAt = Now;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: aim assist check presses lock (%s)."), Fight.time(), Why);
	};
	// The share as the look takes it this frame (locked too, where the look does not turn the view).
	const float Keep = ftcore::aimAssistTurn(bAssist, true, AssistOverBody());
	if (AssistStep == 0 && !bLocked)
	{
		const float Before = CameraYaw;
		TurnLook(0.f, 0.f, 0.5f * AssistSweep, 0.f, DeltaSeconds, bAssist);
		const int32 Over = Keep < 1.f ? 1 : 0;
		AssistTime[Over] += DeltaSeconds;
		AssistTurned[Over] += FMath::Abs(CameraYaw - Before);
	}
	else if (AssistStep == 0 && Now - AssistMarkAt > 0.3)
	{
		PressLock(TEXT("to let go of the lock from before the shot: the view does not turn while one is on"));
	}
	if (Keep != AssistKeep)
	{
		const TCHAR* Why = !bAssist ? TEXT("aim assist off")
			: !ArenaAim.aiming() ? TEXT("aim let go")
			: bLocked ? TEXT("locked on")
			: ReticleEnemy < 0 ? TEXT("the crosshair off every body")
			: !Fight.enemyAlive(static_cast<size_t>(ReticleEnemy)) ? TEXT("its body dead")
			: !bReticleTakes ? TEXT("the round would not take its body") : TEXT("over a body the round would take");
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: aim assist look scale %.2f -> %.2f (%s; crosshair %s, aim %.2f, lock %d, step %d)."),
			Fight.time(), AssistKeep, Keep, Why, *(ReticleEnemy >= 0 ? FString::Printf(TEXT("on enemy %d"), ReticleEnemy) : FString(TEXT("off every body"))),
			ArenaAim.alpha(), static_cast<int32>(Fight.lock()), AssistStep);
		if (AssistStep == 0)
		{
			AssistEnters += Keep < 1.f ? 1 : 0;
			AssistExits += Keep < 1.f ? 0 : 1;
		}
		AssistKeep = Keep;
		AssistSince = Now;
	}
	const bool bOnIt = ReticleEnemy == Slot;
	const bool bHeld = Keep < 1.f && Now - AssistSince >= 0.3; // over the body, the round taking it, for 0.3 s
	const auto Next = [this](int32 Step)
	{
		AssistStep = Step;
		AssistSub = 0;
		AssistStepAt = Now;
	};
	switch (AssistStep)
	{
	case 0:
		// Out across the body until off it 0.5 s, then back until over it again.
		if (Keep == 1.f && ReticleEnemy < 0 && AssistExits >= 1 && (AssistReturns & 1) == 0)
		{
			AssistReturns |= 1;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: aim assist check: the crosshair left the body, look scale %.2f."), Fight.time(), Keep);
		}
		if (AssistSweep > 0.f && (AssistReturns & 1) != 0 && Keep == 1.f && Now - AssistSince > 0.5)
		{
			AssistSweep = -1.f;
		}
		else if (AssistSweep < 0.f && Keep < 1.f)
		{
			Next(1);
		}
		break;
	case 1:
		if (AssistSub == 0 && !bLocked && bHeld)
		{
			PressLock(TEXT("to lock on, the crosshair on the body"));
			AssistSub = 1;
		}
		else if (AssistSub == 1 && bLocked)
		{
			AssistReturns |= Keep == 1.f && bOnIt ? 2 : 0;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: aim assist check: lock-on began (enemy %d), look scale %.2f, the crosshair %s."), Fight.time(),
				static_cast<int32>(Fight.lock()), Keep, bOnIt ? TEXT("still on the body") : TEXT("off the body"));
			AssistSub = 2;
			AssistMarkAt = Now;
		}
		else if (AssistSub >= 2 && bLocked && Now - AssistMarkAt > 0.5)
		{
			PressLock(TEXT("to let go of the lock"));
			AssistSub = 3;
		}
		else if (AssistSub == 3 && !bLocked && bHeld)
		{
			Next(2);
		}
		break;
	case 2:
		if (AssistSub == 0 && bHeld)
		{
			bAssistAimOff = true;
			AssistMarkAt = Now;
			AssistSub = 1;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: aim assist check lets go of aim, the crosshair on the body."), Fight.time());
		}
		else if (AssistSub == 1 && !ArenaAim.aiming())
		{
			AssistReturns |= Keep == 1.f && bOnIt ? 4 : 0;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: aim assist check: aim let go (aim %.2f, blending out), look scale %.2f, the crosshair %s."), Fight.time(),
				ArenaAim.alpha(), Keep, bOnIt ? TEXT("still on the body") : TEXT("off the body"));
			AssistSub = 2;
		}
		else if (AssistSub == 2 && Now - AssistMarkAt > 0.6)
		{
			bAssistAimOff = false;
			AssistSub = 3;
		}
		else if (AssistSub == 3 && bHeld)
		{
			Next(3);
		}
		break;
	default:
		if (bHeld)
		{
			// The target dies under the crosshair (a host blow of its hp): the share must be 1 in this same frame, the
			// fight's Victory may give no other.
			const int32 Hp = Fight.enemyHp(static_cast<size_t>(Slot));
			const bool bDealt = Fight.hostBlow(static_cast<size_t>(Slot), Hp, ftcore::kArenaShotMove);
			const bool bDead = !Fight.enemyAlive(static_cast<size_t>(Slot));
			const float After = ftcore::aimAssistTurn(bAssist, true, AssistOverBody());
			AssistReturns |= bDealt && bDead && After == 1.f && bOnIt ? 8 : 0;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: aim assist look scale %.2f -> %.2f (the target killed by a host blow of %d hp: dealt %d, dead %d; the crosshair %s, step 3)."),
				Fight.time(), Keep, After, Hp, bDealt ? 1 : 0, bDead ? 1 : 0, bOnIt ? TEXT("still on its body") : TEXT("off its body"));
			AssistKeep = After;
			AssistSince = Now;
			AssistStep = 4;
			AimCheckPhase = 4;
		}
		break;
	}
}

void UFTArenaRunner::StepAimCheck(ftcore::ArenaInput& In)
{
	if (AimCheckPhase == 5)
	{
		// The assist check: stand, aim along the view (unless step 2 let it go), and press lock when StepAssistCheck asks.
		const float Yaw = FMath::DegreesToRadians(ViewYaw());
		const ftcore::Vec2 View{FMath::Cos(Yaw), FMath::Sin(Yaw)};
		In = ftcore::ArenaInput();
		In.view = View;
		In.aim = !bAssistAimOff;
		if (In.aim)
		{
			In.aimDir = ReticleAimDir(View);
			In.aimPitched = ReticlePitch(In.aimPitch);
			In.aimTurn = ArenaAim.rules().turnRate;
			In.aimSpeed = ArenaAim.speedScale();
		}
		In.lock = bAssistLockPress; // StepAssistCheck's press (a toggle: on, or let go)
		bAssistLockPress = false;
		return;
	}
	if (AimCheckPhase == 1 && Fight.time() >= 3.f)
	{
		float Top = 0.f;
		AimCheckSlot = TallestStanding(Top);
		if (AimCheckSlot >= 0)
		{
			AimCheckPhase = 2;
			AimCheckSince = Now;
		}
	}
	if (AimCheckPhase != 2 && AimCheckPhase != 3)
	{
		return;
	}
	if (AimCheckSlot < 0 || !Fight.enemyAlive(static_cast<size_t>(AimCheckSlot)))
	{
		AimCheckLine += TEXT(", the target fell before the shot");
		bAimCheckNa = true;
		AimCheckPhase = 4;
		return;
	}
	// Stand still and aim at it; nothing else is pressed.
	const ftcore::Vec2 E = Bodies.enemies[static_cast<size_t>(AimCheckSlot)];
	const ftcore::Vec2 D{E.x - Bodies.player.x, E.y - Bodies.player.y};
	const float L = FMath::Max(ArenaLength(D), 1e-3f);
	const ftcore::Vec2 To{D.x / L, D.y / L};
	In = ftcore::ArenaInput();
	In.view = To;
	In.aim = true;
	In.aimDir = ReticleAimDir(To);
	In.aimPitched = ReticlePitch(In.aimPitch);
	In.aimTurn = ArenaAim.rules().turnRate;
	In.aimSpeed = ArenaAim.speedScale();
	if (AimCheckPhase == 2 && Now - AimCheckSince > 4.0)
	{
		AimCheckLine = TEXT("the view never settled (no camera manager)");
		bAimCheckNa = true;
		AimCheckPhase = 4;
		return;
	}
	if (AimCheckPhase != 3)
	{
		return;
	}
	const ftcore::ArenaStats& S = Fight.stats();
	if (bAimCheckFire)
	{
		In.light = true;
		bAimCheckFire = false;
		++AimCheckPresses;
		AimCheckSince = Now;
		return;
	}
	if (Now - AimCheckSince < 1.2)
	{
		return;
	}
	if (S.shots > AimCheckShotsBefore)
	{
		// PASS only with all three: the view reached the solved pitch, the crosshair's line met this target, and the
		// shot took hp off this target (a nearer enemy's hit does not count).
		const bool bTargetHit = S.shotHits > AimCheckHitsBefore
			&& static_cast<float>(Fight.enemyHp(static_cast<size_t>(AimCheckSlot))) < AimCheckHpBefore;
		bAimCheckPass = bAimCheckPitchOk && bAimCheckOnBoss && bTargetHit;
		const float Off = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(In.aimDir.x * To.x + In.aimDir.y * To.y, -1.f, 1.f)));
		AimCheckLine += FString::Printf(TEXT(", the shot's heading %.1f deg off the boss's middle; pitch reached %s (rendered %.1f), crosshair on the boss %s, the boss hit %s"),
			Off, bAimCheckPitchOk ? TEXT("yes") : TEXT("no"), AimCheckRendered, bAimCheckOnBoss ? TEXT("yes") : TEXT("no"), bTargetHit ? TEXT("yes") : TEXT("no"));
		AimCheckPhase = bAssistCheck ? 5 : 4;
		AssistStepAt = AssistSince = Now;
	}
	else if (AimCheckPresses < 4)
	{
		bAimCheckFire = true; // the press did not start a shot (stunned, busy): again
	}
	else
	{
		AimCheckLine += TEXT(", no shot started in 4 presses");
		AimCheckPhase = 4;
	}
}
#endif

#if !UE_BUILD_SHIPPING
void UFTArenaRunner::SampleFrame(int32 Target, float Want, float DeltaSeconds)
{
	APlayerController* PC = GetPC();
	if (Target < 0 || Target >= static_cast<int32>(Fight.enemyCount()) || Fight.over() || !PC || !PC->PlayerCameraManager)
	{
		FramePrev = FrameTurn; // a release folds the turn into CameraYaw: the view holds, so it is no rate
		return;
	}
	const float Rate = DeltaSeconds > 0.f ? FMath::Abs(FrameTurn - FramePrev) / DeltaSeconds : 0.f;
	FramePrev = FrameTurn;
	const APlayerCameraManager& Cam = *PC->PlayerCameraManager;
	const FVector Eye = Cam.GetCameraLocation();
	const FRotator View = ViewRotation(Cam);
	const FVector Ahead = View.Vector();
	const FVector Right = FRotationMatrix(View).GetScaledAxis(EAxis::Y);
	const double Tan = FMath::Tan(FMath::DegreesToRadians(static_cast<double>(Cam.GetFOVAngle()) * 0.5));
	// A point's place across the rendered screen (-1 the left edge, +1 the right), `Side` cm along the view's right of it.
	const auto Across = [&](const FVector& At, double Side) {
		const FVector D = At + Right * Side - Eye;
		return static_cast<float>(FVector::DotProduct(D, Right) / FMath::Max(FVector::DotProduct(D, Ahead), 1.0) / Tan);
	};
	const ftcore::Vec2& E = Bodies.enemies[static_cast<size_t>(Target)];
	const ftcore::Vec2 To{E.x - Bodies.player.x, E.y - Bodies.player.y};
	const ftcore::ArenaEnemyKind& Foe = Fight.def().enemies[static_cast<size_t>(Target)];
	const float Scale = Foe.look.skinned() ? Foe.look.scale : 1.f;
	const float Tall = ftcore::arenaEnemyHeight(Foe) * Scale + (Foe.look.skinned() ? Foe.look.hover : 0.f);
	const float Mid = Tall >= kArenaTallBoss ? Tall * kArenaFaceHeight : Tall * 0.5f;
	const FVector Lead(Bodies.player.x * 100.0, Bodies.player.y * 100.0, ZoneZ + 100.0);
	const FVector Body(E.x * 100.0, E.y * 100.0, ZoneZ + Mid * 100.0);
	const float LeadX = Across(Lead, 0.0);
	const float FoeX = Across(Body, 0.0);
	// The target's near edge less the lead's far one (a 0.4 m body): below 0 the lead covers part of the target.
	const float Gap = Across(Body, -Foe.bodyRadius * Scale * 100.0) - Across(Lead, 40.0);
	++FrameFrames;
	FrameTurnMost = FMath::Max(FrameTurnMost, FMath::Abs(FrameTurn));
	FrameRateMost = FMath::Max(FrameRateMost, Rate);
	const FVector Pivot(Lead.X, Lead.Y, ZoneZ + Fit.camera.pivot * 100.0);
	FrameLensMost = FMath::Max(FrameLensMost, static_cast<float>(FVector::Dist(Eye, Pivot) / 100.0));
	if (const AFTArenaFighter* Figure = PlayerFigure.Get(); Figure && Figure->GetBoom())
	{
		FrameArmMost = FMath::Max(FrameArmMost, Figure->GetBoom()->TargetArmLength / 100.f);
	}
	// Settled: the lock's line within 5 degrees of the target and the turn within 1 of its want.
	if (FMath::Abs(FMath::FindDeltaAngleDegrees(CameraYaw, ArenaYaw(To))) > 5.f || FMath::Abs(FrameTurn - Want) > 1.f)
	{
		return;
	}
	++FrameSettled;
	FrameInThird += FMath::Abs(LeadX) <= 1.f / 3.f && FMath::Abs(FoeX) <= 1.f / 3.f ? 1 : 0;
	FrameLeadWorst = FMath::Max(FrameLeadWorst, FMath::Abs(LeadX));
	FrameFoeWorst = FMath::Max(FrameFoeWorst, FMath::Abs(FoeX));
	const float Dist = ArenaLength(To);
	const int32 Band = Dist < 3.f ? 0 : (Dist < 6.f ? 1 : (Dist < 10.f ? 2 : 3));
	++FrameBandN[Band];
	FrameBandFoe[Band] += FoeX;
	FrameBandGap[Band] = FMath::Min(FrameBandGap[Band], Gap);
}
#endif

void UFTArenaRunner::ApplyAimCamera()
{
	AFTArenaFighter* Figure = PlayerFigure.Get();
	const float A = ArenaAim.alpha();
	if (!Figure || (A <= 0.f && !bAimCamera))
	{
		return;
	}
	bAimCamera = A > 0.f;
	// The fight's camera pulled in over the right shoulder by the aim stance (ftcore::arenaCameraPose's rig).
	const ftcore::ArenaCameraPose Pose = ftcore::arenaCameraPose(Fit.camera, ArenaAim, ftcore::ArenaFrameYaw{CameraYaw, FrameTurn}, CameraPitch);
	Figure->SetCameraRig(Pose.arm, Pose.pivot, Pose.side, Pose.up, Pose.fov);
}

void UFTArenaRunner::SteerCamera(float DeltaSeconds)
{
	APlayerController* PC = GetPC();
	if (!PC || !bBegun)
	{
		return;
	}
	// The player's hit pause holds the camera where it is (no turn, and never a pull-out: the arm is not touched).
	if (Fight.hitStopLeft() > 0.f)
	{
		ftcore::ArenaFrameScene Held;
		Held.held = true;
		const ftcore::ArenaFrameYaw Kept = ftcore::arenaFrameYawStep(FrameRules(), ftcore::ArenaFrameYaw{CameraYaw, FrameTurn}, Held, DeltaSeconds);
		CameraYaw = Kept.line;
		FrameTurn = Kept.turn;
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
#if !UE_BUILD_SHIPPING
	if (AimCheckPhase == 5)
	{
		StepAssistCheck(DeltaSeconds); // the sweep turns the view itself: nothing is followed
		Target = -1;
	}
#endif
	const AFTArenaFighter* Figure = PlayerFigure.Get();
	// The boom's length now (the aim pulls it in), for the look-up solve.
	const float ArmNow = Figure && Figure->GetBoom() ? Figure->GetBoom()->TargetArmLength / 100.f : Fit.camera.arm;
#if !UE_BUILD_SHIPPING
	if (AimCheckPhase == 2 || AimCheckPhase == 3)
	{
		// -ArenaAimCheck: straight at the tallest standing enemy's top, from where the boom puts the lens.
		Target = AimCheckSlot;
	}
#endif
	// The yaw: the lock's line (CameraYaw, eased onto the target) and the framing's orbit on it (FrameTurn), both stepped
	// by ftcore::arenaFrameYawStep. The view is drawn at their sum (ViewYaw), and the stick, the crosshair and the fight's
	// view read that, so the lead goes where the stick points on the screen, and a release (the turn folded into the line)
	// leaves the stick's basis where it was.
	ftcore::ArenaFrameScene Scene;
	const ftcore::Settings* Prefs = GetSettings();
	Scene.framing = !Prefs || Prefs->lockFraming;
	Scene.aiming = ArenaAim.aiming(); // the aim camera frames the shot over the shoulder
	Scene.side = Fit.camera.side;
	Scene.lineRate = kArenaLockTurn;
#if !UE_BUILD_SHIPPING
	if (bNoFrame || AimCheckPhase == 2 || AimCheckPhase == 3)
	{
		Scene.framing = false;
		FrameTurn = AimCheckPhase == 2 || AimCheckPhase == 3 ? 0.f : FrameTurn; // -ArenaAimCheck measures the plain lock view
	}
#endif
	const bool bFollowed = Target >= 0 && Target < static_cast<int32>(Fight.enemyCount());
	ftcore::Vec2 To{};
	if (bFollowed)
	{
		const ftcore::Vec2& E = Bodies.enemies[static_cast<size_t>(Target)];
		To = ftcore::Vec2{E.x - Bodies.player.x, E.y - Bodies.player.y};
		Scene.followed = true;
		Scene.hasLine = ArenaLength(To) > 0.3f;
		Scene.lineWant = ArenaYaw(To);
		Scene.distance = ArenaLength(To);
	}
	const ftcore::ArenaFrameYaw Yaw = ftcore::arenaFrameYawStep(FrameRules(), ftcore::ArenaFrameYaw{CameraYaw, FrameTurn}, Scene, DeltaSeconds);
	CameraYaw = Yaw.line;
	FrameTurn = Yaw.turn;
#if !UE_BUILD_SHIPPING
	const float FrameWant = bFollowed && Scene.framing && !Scene.aiming ? ftcore::arenaFrameTurn(FrameRules(), Scene.side, Scene.distance) : 0.f;
#endif
	if (bFollowed)
	{
		// A tall boss (a hanging one, hover included) is looked up at: the pitch that sends the view through its face from
		// where the arm puts the camera, the target as far ahead as it stands along the turned view
		// (ftcore::arenaFrameLockPitch), no higher than the free camera may look.
		const ftcore::ArenaEnemyKind& Foe = Fight.def().enemies[static_cast<size_t>(Target)];
		const float Hover = Foe.look.skinned() ? Foe.look.hover : 0.f;
		const float Tall = ftcore::arenaEnemyHeight(Foe) * (Foe.look.skinned() ? Foe.look.scale : 1.f) + Hover;
		const ftcore::ZoneCamera& Rig = Fit.camera;
		float WantPitch = ftcore::arenaFrameLockPitch(ftcore::kArenaLook, Rig.pivot, ArmNow, Rig.up, Rig.lockPitch, ArenaLength(To), FrameTurn, Tall);
#if !UE_BUILD_SHIPPING
		if (AimCheckPhase == 2 || AimCheckPhase == 3)
		{
			// The top itself, less a little so the line meets the body rather than grazing over it (the turn is 0 here).
			AimCheckPitch = ftcore::arenaLookUpPitch(ftcore::kArenaLook, Rig.pivot, ArmNow, Rig.up, ArenaLength(To), Tall * 0.95f, Rig.lockPitch);
			WantPitch = AimCheckPitch;
		}
		const float PitchRate = AimCheckPhase == 2 ? 12.f : 4.f; // -ArenaAimCheck settles faster
#else
		const float PitchRate = 4.f;
#endif
		CameraPitch = FMath::FInterpTo(CameraPitch, WantPitch, DeltaSeconds, PitchRate);
	}
	CameraYaw = FMath::UnwindDegrees(CameraYaw);
	// The boom swings up to its limit, the lens tilts the rest: a steep look keeps the lens over the floor and the arm
	// its length.
	// The pose (ftcore::arenaCameraPose): the drawn yaw, the boom and the lens tilt; the rig is the zone's and the aim's.
	CameraPitch = ftcore::arenaClampViewPitch(ftcore::kArenaLook, CameraPitch);
	const ftcore::ArenaCameraPose Pose = ftcore::arenaCameraPose(Fit.camera, ArenaAim, ftcore::ArenaFrameYaw{CameraYaw, FrameTurn}, CameraPitch);
	PC->SetControlRotation(FRotator(Pose.boomPitch, Pose.yaw, 0.0));
#if !UE_BUILD_SHIPPING
	if (bFrameCheck)
	{
		SampleFrame(Target, FrameWant, DeltaSeconds);
	}
#endif
	if (AFTArenaFighter* Lead = PlayerFigure.Get())
	{
		Lead->SetCameraTilt(Pose.tilt);
	}
}

void UFTArenaRunner::Finish()
{
	bResultLogged = true;
	ResultTime = Now;
	if (PlayerFigure)
	{
		PlayerFigure->ResetFeet(); // handed back (the story, the explore pawn, a result screen): nothing held carries over
	}
	const bool bWon = Fight.phase() == ftcore::ArenaPhase::Victory;
	if (Session && !bWon)
	{
		// A loss: a Lone Carry still on ends here (loneEndedBy reads FightEnded), nothing folds, and the next begin rolls
		// the gift tally back. A story step that keeps the loss (its loss branch) calls finish(true) again after this.
		Session->Get().finish(false);
	}
	if (bWon && Session)
	{
		Session->Get().finish(true); // the gift tally carries over a win; a retry's begin rolls it back
		const bool bFolded = Session->Get().foldLone(LoneSaved);
		if (bFolded && Session->Get().loneReleases() > 0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: the win folds %u lone Releases (%u Load) into the pool: %u Releases, %u Load (not persisted)."),
				Session->Get().loneReleases(), Session->Get().lonePooled(), LoneSaved.releases, LoneSaved.pooled);
		}
	}
	LogStoneWalkCounts();
	LogResult();
	if (bQuitOnFinish && !bShots)
	{
		FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(bWon ? 0 : 1), TEXT("Arena"));
	}
	// The autopilot's wait before it fights again: long enough for the bosses' defeat clips (all dead on a victory) to play out.
	float LongestDefeat = 0.f;
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
		}
	}
	AutoAgainWait = FMath::Max(kArenaAutoAgain, static_cast<double>(LongestDefeat) + kArenaDefeatTail);
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
	LogFeatureChecks();
	LogCarry();
	LogTable();
	LogBrief();
}

void UFTArenaRunner::LogFeatureChecks() const
{
	Glide.Log();
	const ftcore::ArenaStats& S = Fight.stats();
	const ftcore::ArenaFeatureChecks C = ftcore::arenaFeatureChecks(S);
	const auto Text = [](ftcore::ArenaCheck Check) { return FT::ToFString(ftcore::arenaCheckText(Check)); };
	const auto Verdict = [](bool bShown, bool bCan) { return !bCan ? TEXT("n/a") : (bShown ? TEXT("PASS") : TEXT("FAIL")); };
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: lights free %s (%d lights, %.1f stamina)."), *Text(C.lightsFreeCheck), S.lights,
		S.lightStamina);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: cancels %s (%d of %d chances)."), *Text(C.cancelsCheck), S.cancels,
		S.cancelChances);
	// The jump: only in runs that can jump (-ArenaJump or a jump probe), so a default run's lines stay as they were.
	if (JumpBound() || (GlideStick >= 9 && GlideStick <= 11))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: jump %s (%d started, %d landed)."), *Text(C.jumpCheck), S.jumps, S.landings);
	}
	// Flinch (ftcore::arenaFeatureChecks): n/a when no blow could flinch an upright enemy (a kit too light for the
	// kinds' flinchPoise, poise breaking first, or no kind that flinches, as a boss).
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: flinch %s (%d of %d chances)."), *Text(C.flinchCheck), S.flinches,
		S.flinchChances);
	// Break-out (ftcore::arenaFeatureChecks): FAIL when upright blows landed and the run never moved or it was due
	// and free; barred when a mask or the host barred it; n/a when poise always broke first.
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: break-out %s (%d; due %d, barred %d; %d upright blows; best run %d of %d)."),
		*Text(C.breakOutCheck), S.breakOuts, S.breakDue, S.breakBarred, S.breakUpright, S.breakRunBest, S.breakRunNeed);
	// Soft target: n/a when no attack could take one (every one started locked on or aimed: the autopilot locks unless
	// -ArenaNoLock, and an aimed shot takes the aim), or the kit has no softRange.
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: soft target %s (%d of %d chances)."),
		Fight.def().fighter.softRange > 0.f ? *Text(C.softTargetCheck) : TEXT("n/a"), S.softTargets, S.softChances);
	// Shots count only for a guns kit that fired (a player's aim, or the autopilot under -ArenaAutoShoot).
	const bool bGuns = Fight.def().fighter.weapon == ftcore::ArenaWeapon::Guns;
#if !UE_BUILD_SHIPPING
	if (bFrameCheck)
	{
		const ftcore::ZoneCamera& Rig = Fit.camera;
		const float FreeLens = FMath::Sqrt(Rig.arm * Rig.arm + Rig.side * Rig.side + Rig.up * Rig.up);
		// A zone whose framing cap is 0 cannot centre the target: there the check is only that the turn stays 0 and the
		// lock's line eases onto the target (settled frames), not the middle third.
		const bool bNoTurn = FrameRules().maxTurn <= 0.f;
		const bool bPass = FrameSettled > 0 && (bNoTurn || FrameInThird == FrameSettled) && FrameTurnMost <= FrameRules().maxTurn + 0.01f &&
			FrameArmMost <= Rig.arm + 0.001f;
		UE_LOG(LogForgottenThird, Display,
			TEXT("Arena check: framing %s%s (%d frames followed, %d settled, %d with both bodies in the middle third; worst |x| lead %.3f, target %.3f; turn most %.1f deg (cap %.0f), its rate most %.0f deg/s; boom arm most %.2f m (rig %.2f); lens to pivot most %.2f m (rig %.2f, camera lag aside))."),
			bPass ? TEXT("PASS") : TEXT("FAIL"), bNoFrame ? TEXT(" [framing off]") : (bNoTurn ? TEXT(" [zone cap 0: turn and line only]") : TEXT("")), FrameFrames, FrameSettled, FrameInThird,
			FrameLeadWorst, FrameFoeWorst, FrameTurnMost, FrameRules().maxTurn, FrameRateMost, FrameArmMost, Rig.arm, FrameLensMost,
			FreeLens);
		const TCHAR* Bands[4] = {TEXT("under 3"), TEXT("3-6"), TEXT("6-10"), TEXT("10 and over")};
		for (int32 I = 0; I < 4; ++I)
		{
			if (FrameBandN[I] > 0)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Arena check: framing at %s m: %d settled, target x mean %+.3f, edge gap least %+.3f."), Bands[I],
					FrameBandN[I], FrameBandFoe[I] / static_cast<float>(FrameBandN[I]), FrameBandGap[I]);
			}
		}
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("ArenaAimCheck")))
	{
		float Top = 0.f;
		const int32 Tallest = AimCheckSlot >= 0 ? AimCheckSlot : TallestStanding(Top);
		if (AimCheckSlot >= 0)
		{
			const ftcore::ArenaEnemyKind& Foe = Fight.def().enemies[static_cast<size_t>(AimCheckSlot)];
			Top = ftcore::arenaEnemyHeight(Foe) * (Foe.look.skinned() ? Foe.look.scale : 1.f) + (Foe.look.skinned() ? Foe.look.hover : 0.f);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: aim up %s (enemy %d, top %.1f m up, aimed at %.1f deg: %s)."),
			(AimCheckPhase == 4 || AimCheckPhase == 5) && !bAimCheckNa ? (bAimCheckPass ? TEXT("PASS") : TEXT("FAIL")) : TEXT("n/a"), Tallest, Top, AimCheckPitch,
			AimCheckLine.IsEmpty() ? TEXT("not run") : *AimCheckLine);
	}
	if (bAssistCheck)
	{
		const float OverRate = AssistTime[1] > 0.f ? AssistTurned[1] / AssistTime[1] : 0.f;
		const float OffRate = AssistTime[0] > 0.f ? AssistTurned[0] / AssistTime[0] : 0.f;
		const bool bRan = AssistEnters >= 1 && AssistExits >= 1 && OverRate > 0.f && OffRate > 0.f;
		const float Ratio = bRan ? OverRate / OffRate : 0.f;
		const bool bPass = bRan && FMath::Abs(Ratio - ftcore::kAimAssistPad) < 0.05f && AssistReturns == 15;
		const auto Back = [this](uint8 Bit) { return (AssistReturns & Bit) != 0 ? TEXT("yes") : TEXT("no"); };
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: aim assist %s (stick share over a body x%.2f, off it x1.00: swept %.1f deg/s over a body for %.2f s and %.1f deg/s off it for %.2f s, ratio %.2f; %d entries, %d exits; back to x1.00 as the crosshair left %s, on lock-on %s, as aim let go %s, as the target died %s%s)."),
			!bRan && AssistFail.IsEmpty() ? TEXT("n/a") : (bPass ? TEXT("PASS") : TEXT("FAIL")), ftcore::kAimAssistPad, OverRate, AssistTime[1], OffRate,
			AssistTime[0], Ratio, AssistEnters, AssistExits, Back(1), Back(2), Back(4), Back(8),
			*(AssistFail.IsEmpty() ? FString() : FString::Printf(TEXT("; ran out of time at %s"), *AssistFail)));
	}
#endif
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: shots %s (%d fired, %d hit)."), Verdict(C.shots, bGuns && (bAutoShoot || S.shots > 0)),
		S.shots, S.shotHits);
	// Two leads on one count (the partner scripted under -ArenaSharedCount): a held blow's bonus landed.
	// n/a for a fight that ends Lone: nobody joins a Lone lead.
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: shared count bonus %s (%d held, %d joined, %d bonuses, +%d damage)."),
		Verdict(SessionBonuses > 0, bSharedCount && Session != nullptr && !Fight.lone()), SessionHeldBlows, SessionJoins, SessionBonuses, SessionBonusDamage);
	if (bCountLift)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: COUNT stores %d stored %d lifts %d liftSpent %d marks %d kiteUnlocks %d maxJoinLoad %d partnerStored %d partnerLoad %d/%d blowCap %.1f capped %d."),
			CountLoadStores, CountLoadStored, Lifts, LiftSpent, Marks, KiteUnlocks, MaxJoinLoad, PartnerLoadStored,
			GetPartnerLoad(), GetPartnerLoadCap(), Session ? Session->Get().blowCap() : 0.f, CappedBlows);
	}
	if (LoneWanted())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: LONE start %d end %d endedAt %.2f endedBy %s releases %d pooled %u giftPresses %d refused %d slot %d (pool exposed, not persisted)."),
			bLoneAtStart ? 1 : 0, Fight.lone() ? 1 : 0, LoneEndedAt, Session ? LoneEndWord(Session->Get().loneEndedBy()) : TEXT("none"), LoneReleases,
			Session ? Session->Get().lonePooled() : 0u, LoneGiftPresses, LoneGiftsRefused, static_cast<int32>(LoneSlot));
		// The glue: while Lone nobody joined, no table began and every Release pooled (a refused gift is on the LONE line).
		const bool bLoneOk = LoneBreaches == 0;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: lone %s (%d breaches, %d of %d gift presses refused)."),
			Verdict(bLoneOk, bLoneAtStart), LoneBreaches, LoneGiftsRefused, LoneGiftPresses);
	}
	if (HallMode != 0)
	{
		using EV = ftcore::HallucinationEventType;
		const auto N = [this](EV E) { return HallSeen[static_cast<int32>(E)]; };
		const bool bOk = (HallMode == 1 ? N(EV::Held) > 0 && N(EV::PulledBack) > 0
			: HallMode == 2 ? N(EV::Refused) > 0 && N(EV::Held) == 0
			: HallMode == 3 ? N(EV::Expired) > 0 && N(EV::Held) == 0
			: N(EV::Held) > 0 && N(EV::Refused) > 0 && N(EV::PulledBack) == 0 && HallGuardHeld > 0 && HallGuardKept > 0)
			&& HallBlocksHeld == 0;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: hallucination %s (mode %d: %d started, %d held, %d refused, %d pulled back, %d expired, %d torn; max hold %.2f; %d steps held, %.2f m drift, %d enemy attacks begun meanwhile; guard down %d held steps and %d after, %d blocks or parries in them)."),
			Verdict(bOk, true), HallMode, N(EV::Started), N(EV::Held), N(EV::Refused), N(EV::PulledBack), N(EV::Expired), N(EV::Torn),
			HallMaxHold, HallHeldSteps, HallHeldDrift, HallEnemyActs, HallGuardHeld, HallGuardKept, HallBlocksHeld);
	}
	// Impact (ftcore/ArenaImpact.h): numbers only, no verdict. The jolt is rotation only (the arm and the field of view
	// are never touched); the marker is the off-frame tell and the stinger only doubles it.
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: jolt (%d blows asked; peak %.2f deg handed to the camera; camera shake %.2f)."),
		JoltAsks, JoltApplied, ShakeScale());
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: off-frame tells (%d windups off frame; %d stingers%s)."), OffFrameWindups, Stingers,
		(Stingers > 0 && !StingerAudio) ? TEXT(", no audio device: the marker alone") : TEXT(""));
	// Pace: time-to-kill per slot and the punish windows used, waiting for a play report (no pass mark).
	char Pace[256];
	ftcore::arenaPaceText(S, Fight.enemyCount(), Pace, sizeof Pace);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: %s."), UTF8_TO_TCHAR(Pace));
}

const ftcore::Settings* UFTArenaRunner::GetSettings() const
{
	const UWorld* World = GetWorld();
	const UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	const UFTSettingsSubsystem* Sub = GI ? GI->GetSubsystem<UFTSettingsSubsystem>() : nullptr;
	return Sub ? &Sub->Get() : nullptr;
}

float UFTArenaRunner::ShakeScale() const
{
	if (ShakeOverride >= 0.f)
	{
		return ShakeOverride;
	}
	const ftcore::Settings* Settings = GetSettings();
	return Settings ? Settings->cameraShake : 1.f;
}

void UFTArenaRunner::PlayGunCue(const ftcore::ArenaEvent& E)
{
	// The sfx cue (Data/Audio/cues.json): silent, logged once, until its sound is imported.
	const char* Cue = ftcore::arenaGunCue(E);
	UWorld* World = GetWorld();
	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	if (UFTAudioSubsystem* Audio = Cue && GI ? GI->GetSubsystem<UFTAudioSubsystem>() : nullptr)
	{
		Audio->HandleCue(FString(UTF8_TO_TCHAR(Cue)), EFTCueAction::Play);
	}
}

void UFTArenaRunner::OnLeadReload(bool bStarted, int32 Amount)
{
	// The reload clip (lead_reload) is posed each frame by the fight's reload progress (FFTLeadWeapons::SetReload), so
	// it spans Amount ms whatever its length; its sound is the cue PlayGunCue fires.
	if (bStarted)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: reload begins (%d ms)."), Fight.time(), Amount);
	}
	else
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: reloaded, %d rounds."), Fight.time(), Amount);
	}
}

void UFTArenaRunner::OnLeadDryPress(int32 Presses)
{
	// The flinch on the gun arm (lead_dry_flinch); the dry click is the cue PlayGunCue fires.
	LeadWeapons.DryPressed(static_cast<double>(Fight.heldTime()));
	UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: dry press (%d so far)."), Fight.time(), Presses);
}

void UFTArenaRunner::KickJolt(const ftcore::ArenaEvent& E)
{
	const float Degrees = ftcore::arenaJoltOf(Fight.def(), E);
	if (Degrees <= 0.f)
	{
		return;
	}
	++JoltAsks;
	// The yaw swings first toward the enemy in the exchange (the one hit, or the one hitting).
	const uint8 Other = E.source == ftcore::kArenaPlayer ? E.target : E.source;
	float Side = 1.f;
	if (Other < Fight.enemyCount())
	{
		Side = ftcore::arenaBearing(CameraView.eye, CameraView.forward, Bodies.enemies[Other]) < 0.f ? -1.f : 1.f;
	}
	Jolt.kick(Degrees, ShakeScale(), Side);
}

void UFTArenaRunner::TickImpact(float DeltaSeconds)
{
	APlayerController* PC = GetPC();
	APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager.Get() : nullptr;

	// The jolt, on the frame's clock.
	Jolt.step(DeltaSeconds);
	const float Pitch = Jolt.pitch();
	const float Yaw = Jolt.yaw();
	JoltApplied = FMath::Max(JoltApplied, FMath::Max(FMath::Abs(Pitch), FMath::Abs(Yaw)));
	if (Camera && !JoltModifier)
	{
		JoltModifier = Cast<UFTArenaJoltModifier>(Camera->AddNewCameraModifier(UFTArenaJoltModifier::StaticClass()));
	}
	if (JoltModifier)
	{
		JoltModifier->SetOffsets(Pitch, Yaw);
	}

	// The off-frame tells, against the view the camera last rendered (its place, facing and horizontal field of view).
	if (Camera)
	{
		const FVector Eye = Camera->GetCameraLocation();
		const FRotator View = ViewRotation(*Camera);
		const FVector Forward = View.Vector();
		CameraView.eye = ftcore::Vec2{static_cast<float>(Eye.X / 100.0), static_cast<float>(Eye.Y / 100.0)};
		const FVector2D Flat = FVector2D(Forward.X, Forward.Y).GetSafeNormal();
		if (!Flat.IsNearlyZero())
		{
			CameraView.forward = ftcore::Vec2{static_cast<float>(Flat.X), static_cast<float>(Flat.Y)};
		}
		CameraView.halfAngle = FMath::Clamp(Camera->GetFOVAngle() * 0.5f, 5.f, 85.f);
		// The windup arc's camera half (an enemy outside it circles round before it strikes): the real camera's
		// horizontal half-angle, handed to the fight each time it changes (aiming, a close shot).
		if (CameraView.halfAngle != FightCameraHalf)
		{
			FightCameraHalf = CameraView.halfAngle;
			Fight.setCameraHalfAngle(FightCameraHalf);
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: the windup arc's camera half-angle is %.1f degrees."), Fight.time(), FightCameraHalf);
		}
		// The vertical half-angle from the horizontal one and the screen's shape, so a body wholly under or over the
		// frame (a ground enemy while aiming high) is told too.
		int32 SizeX = 0, SizeY = 0;
		PC->GetViewportSize(SizeX, SizeY);
		const float Aspect = SizeX > 0 && SizeY > 0 ? static_cast<float>(SizeX) / static_cast<float>(SizeY) : 16.f / 9.f;
		CameraView.vHalfAngle = FMath::RadiansToDegrees(FMath::Atan(FMath::Tan(FMath::DegreesToRadians(CameraView.halfAngle)) / Aspect));
		CameraView.eyeZ = static_cast<float>(Eye.Z / 100.0);
		CameraView.pitch = static_cast<float>(FRotator::NormalizeAxis(View.Pitch));
	}
	ftcore::ArenaTellBody TellBodies[ftcore::kMaxArenaEnemies];
	const size_t Count = FMath::Min(static_cast<size_t>(Fight.enemyCount()), ftcore::kMaxArenaEnemies);
	for (size_t I = 0; I < Count; ++I)
	{
		TellBodies[I] = ftcore::arenaTellBody(Fight, I, Bodies.enemies[I]);
		if (const AFTArenaFighter* Figure = GetEnemyFigure(static_cast<int32>(I)))
		{
			const ftcore::ArenaBodyCylinder Cyl = ftcore::arenaBodyCylinder(Fight.def().enemies[I], Bodies.enemies[I],
				static_cast<float>(Figure->GetActorLocation().Z / 100.0));
			TellBodies[I].low = Cyl.base;
			TellBodies[I].high = Cyl.top;
		}
	}
	const ftcore::ArenaOffFrameTells Tells =
		(Camera && !Fight.over()) ? ftcore::arenaOffFrameTells(CameraView, TellBodies, Count) : ftcore::ArenaOffFrameTells();
	bool bNow[ftcore::kMaxArenaEnemies] = {};
	float BearingOf[ftcore::kMaxArenaEnemies] = {};
	for (size_t K = 0; K < Tells.count; ++K)
	{
		bNow[Tells.items[K].enemy] = true;
		BearingOf[Tells.items[K].enemy] = Tells.items[K].bearing;
	}
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		const size_t Slot = static_cast<size_t>(I);
		if (Slot >= Count || !TellBodies[Slot].winding)
		{
			bWindupTold[I] = false; // the next windup is told afresh
		}
		bOffFrame[I] = bNow[Slot];
		if (!bNow[Slot] || bWindupTold[I])
		{
			continue;
		}
		// Once per windup: the marker's onset pulse starts with it (OffFrameSince), and the stinger doubles it, no closer
		// than kStingerGap to the last one so two windups at once sound once.
		bWindupTold[I] = true;
		OffFrameSince[I] = Now; // once per windup: a body hovering at the view's edge doesn't ring again
		++OffFrameWindups;
		constexpr double kStingerGap = 0.25;
		const bool bSting = Now - LastStinger >= kStingerGap;
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: enemy %d winds up off frame (bearing %.0f deg)%s."), Fight.time(), I,
			BearingOf[Slot], bSting ? TEXT(": stinger") : TEXT(""));
		if (!bSting)
		{
			continue;
		}
		LastStinger = Now;
		++Stingers;
		// Made on the first stinger; the sound is asked for again while there is none (no audio device yet, or -nosound).
		if (!Stinger)
		{
			Stinger = NewObject<UFTBeatBossMetronome>(this);
		}
		if (!StingerAudio)
		{
			StingerAudio = UGameplayStatics::SpawnSound2D(this, Stinger, 1.f, 1.f, 0.f, nullptr, false, false);
		}
		if (StingerAudio && Stinger)
		{
			const ftcore::Settings* Settings = GetSettings();
			StingerAudio->SetVolumeMultiplier(Settings ? Settings->master * Settings->sfx : 1.f);
			Stinger->Trigger(UFTBeatBossMetronome::EVoice::Chime);
		}
	}
}

void UFTArenaRunner::EndImpact()
{
	if (JoltModifier)
	{
		if (APlayerController* PC = GetPC(); PC && PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->RemoveCameraModifier(JoltModifier);
		}
		JoltModifier = nullptr;
	}
	if (StingerAudio)
	{
		StingerAudio->Stop();
		StingerAudio = nullptr;
	}
	Stinger = nullptr;
	Jolt = ftcore::ArenaJolt();
}

void UFTArenaRunner::BeginHallucination()
{
	HallPool = ftcore::TragedyPool(HallTable);
	for (int32 K = 0; K < static_cast<int32>(HallTable.keys.size()); ++K)
	{
		HallPool.unlock(K); // the probe has lived every key
	}
	Hall = ftcore::Hallucination(HallTuning);
	HallRng = 7u;
	HallShownAt = 0.0;
	FMemory::Memzero(HallSeen);
	HallHeldSteps = HallEnemyActs = HallGuardHeld = HallGuardKept = HallBlocksHeld = 0;
	HallHeldAt = HallRefusedAt = -1.0;
	bHallKeptStep = false;
	HallHeldDrift = HallMaxHold = 0.f;
	bHallHeldStep = false;
	HallOpenLeft = 0.f;
	HallBlowsTaken = 0;
	HallTooEarlyAt = -1.0;
	bHallNoWeakPointLogged = false;
}

// The refusal's weak point on every weak-point host the fight has (the kite, the stone bearer, a boss kind's crit):
// shut while Hall.weakPointShutFor() > 0 (Held shuts it; an open one closes), and opened for weakPointOpenFor() on
// the step that window rises (a refusal completed; Core opens none through the lock). The boss's own openings are
// left as they are outside the shut.
void UFTArenaRunner::HallWeakPoint()
{
	const float Open = Hall.weakPointOpenFor();
	const bool bShut = Hall.weakPointShutFor() > 0.f;
	const bool bRise = Open > HallOpenLeft + 1e-4f;
	HallOpenLeft = Open;
	const double T = static_cast<double>(Fight.time());
	bool bAny = false;
	if (Kite.IsActive())
	{
		bAny = true;
		Kite.SetShut(bShut);
		if (bRise)
		{
			Kite.OpenFor(T, Open);
		}
	}
	if (StoneBearer.IsActive())
	{
		bAny = true;
		StoneBearer.SetWeakPointShut(bShut);
		if (bRise)
		{
			StoneBearer.OpenWeakPointFor(Open);
		}
	}
	for (int32 I = 0; I < static_cast<int32>(Fight.enemyCount()) && I < MaxEnemies; ++I)
	{
		if (bKindCritSlot[I] && Fight.def().enemies[static_cast<size_t>(I)].boss)
		{
			bAny = true;
			KindCrits[static_cast<size_t>(I)].setShut(bShut);
			if (bRise)
			{
				KindCrits[static_cast<size_t>(I)].openFor(T, static_cast<double>(Open));
			}
		}
	}
	if (bRise && bAny)
	{
		if (bShut)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the refusal's opening (%.2f s) is held shut (%.2f s left)."), Fight.time(), Open,
				Hall.weakPointShutFor());
		}
		else
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the refusal opens the weak point for %.2f s."), Fight.time(), Open);
		}
	}
	if (!bAny && (bRise || bShut) && !bHallNoWeakPointLogged)
	{
		bHallNoWeakPointLogged = true;
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: the refusal's weak-point window has no host in this fight (no kite, stone bearer or boss kind crit)."));
	}
}

void UFTArenaRunner::StepHallucination(ftcore::ArenaInput& In)
{
	static const TCHAR* const Names[] = {TEXT("started"), TEXT("hold began"), TEXT("held"), TEXT("refused"), TEXT("pulled back"),
		TEXT("expired"), TEXT("torn")};
	const auto Count = [this]()
	{
		for (int32 I = 0; I < Hall.eventCount(); ++I)
		{
			const ftcore::HallucinationEvent& E = Hall.events()[I];
			const int32 T = static_cast<int32>(E.type);
			++HallSeen[T];
			HallHeldAt = E.type == ftcore::HallucinationEventType::Held ? Hall.now() : HallHeldAt;
			HallRefusedAt = E.type == ftcore::HallucinationEventType::Refused ? Hall.now() : HallRefusedAt;
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: hallucination %s (key %d)."), Fight.time(), T < 7 ? Names[T] : TEXT("?"), E.key);
		}
	};
	ftcore::HallucinationInput H;
	H.lead = SessionLead;
	H.inFight = true;
	H.partnerPresent = Session != nullptr && !Fight.lone(); // alone, only the refusal ends a hold
	H.accepting = (HallMode == 1 || HallMode == 4) && Hall.active();
	H.brief = LeadBrief;
	H.partnerBrief = ftcore::BriefId::Count; // the partner's kit takes no Brief yet
	H.partnerAble = H.partnerPresent && (Fight.allyMaxHp() == 0 || Fight.allyHp() > 0);
	H.blowsTaken = HallBlowsTaken; // the last step's (AfterHallucinationStep)
	HallBlowsTaken = 0;
	// The refusal is the guard button. The autoplay bot guards for its own reasons, so there the probe presses it
	// instead: mode 2 as a bare refusal, mode 4 by holding the guard itself.
	const bool bProbeGuard = HallMode == 4
		&& ((Hall.held() && HallHeldAt >= 0.0 && Hall.now() - HallHeldAt >= 1.0)
			|| (HallRefusedAt >= 0.0 && Hall.now() - HallRefusedAt < 1.0));
	In.guard = In.guard || bProbeGuard;
	H.refuse = !bAutoPlay ? In.guard
		: HallMode == 2 ? Hall.active() && Hall.now() - HallShownAt >= 1.0 : bProbeGuard;
	Hall.tick(static_cast<double>(StepSeconds), H);
	Count();
	// Held's one cost: a share of the lead's max health on the step Held began (hostHurt never kills: 1 hp stays).
	if (Hall.heldCost() > 0.f)
	{
		const int32 Cost = FMath::RoundToInt(Hall.heldCost() * static_cast<float>(Fight.def().fighter.hp));
		const int32 Taken = Fight.hostHurt(Cost); // never inside the log: its arguments go unevaluated when it is off
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: Held costs the lead %d hp (%d asked)."), Fight.time(), Taken, Cost);
	}
	if (Hall.refusedTooEarly())
	{
		HallTooEarlyAt = static_cast<double>(Fight.time());
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: refused too early: no opening."), Fight.time());
	}
	HallWeakPoint();
	// The weighted pick (newest x3, caused x2; the lone key x2 once this fight's lone Releases pool lonePoolFloor).
	// Unscripted, so no scare flag: the arena has no authored moment and no ScarePacer.
	ftcore::HallucinationStartIn Offer;
	Offer.band = ftcore::intensityBand(ftcore::IntensityBand::Low, 0, HallTuning);
	Offer.loneLoadPool = Session ? Session->Get().lonePooled() : 0u;
	if (!Hall.active() && Fight.time() >= HallAt && Hall.start(HallPool, HallRng, Offer) == ftcore::HallucinationStart::Started)
	{
		HallShownAt = Hall.now();
		Count();
	}
	const float Hold = Hall.holdLevel();
	HallMaxHold = FMath::Max(HallMaxHold, Hold);
	bHallHeldStep = Hall.held();
	bHallKeptStep = !bHallHeldStep && Hall.refuseKept();
	if (bHallHeldStep)
	{
		++HallHeldSteps;
		HallHeldFrom = Bodies.player;
		HallGuardHeld += In.guard ? 1 : 0;
	}
	HallGuardKept += bHallKeptStep && In.guard ? 1 : 0;
	// Held: still and blank (the guard went to the refusal) until pulled back or refused. Holding: the stick slows.
	In = ftcore::hallucinationArenaInput(Hall, In);
}

void UFTArenaRunner::AfterHallucinationStep(const ftcore::ArenaOutput& Out)
{
	for (const ftcore::ArenaEvent& E : Out.events)
	{
		HallBlowsTaken += (E.type == ftcore::ArenaEventType::Hit && E.target == ftcore::kArenaPlayer) ? 1 : 0;
	}
	if (!bHallHeldStep && !bHallKeptStep)
	{
		return;
	}
	if (bHallHeldStep)
	{
		HallHeldDrift += ArenaLength(ftcore::Vec2{Bodies.player.x - HallHeldFrom.x, Bodies.player.y - HallHeldFrom.y});
	}
	for (const ftcore::ArenaEvent& E : Out.events)
	{
		HallEnemyActs += (bHallHeldStep && E.type == ftcore::ArenaEventType::AttackStarted && E.source < ftcore::kMaxArenaEnemies) ? 1 : 0;
		HallBlocksHeld += (E.type == ftcore::ArenaEventType::Blocked || E.type == ftcore::ArenaEventType::Parried) ? 1 : 0;
	}
}

void UFTArenaRunner::Restart()
{
	Fight.setLone(LoneAccepted(SessionLead), LoneTuning); // a restart is Lone again (and leaves the heal charges out)
	LoneGift = {}; // a press held from the last attempt is not this one's
	bLoneGiftApply = false;
	GiftPick = ftcore::Gift::None;
	if (HallMode != 0)
	{
		BeginHallucination();
	}
	Fight.restart();
	if (bStoryFight)
	{
		// The story's fight again from its start, bound or not: the last attempt's story writes roll back to the
		// step's mark and it is marked again (one retry counted), or the new attempt is refused its binding
		// (BeginSession) and counts nothing.
		const UWorld* World = GetWorld();
		UFTGameInstance* Instance = World ? World->GetGameInstance<UFTGameInstance>() : nullptr;
		UFTStoryStateSubsystem* Story = Instance ? Instance->GetSubsystem<UFTStoryStateSubsystem>() : nullptr;
		if (!Story || !Story->RestartStep())
		{
			// The step's mark is gone (or the state is): the new attempt can't roll back, and BeginSession refuses
			// its binding rather than count on top of the last attempt.
			UE_LOG(LogForgottenThird, Error, TEXT("Arena: Restart of the story's fight found no open step to roll back; the new attempt plays unbound."));
		}
	}
	bRestarting = true; // BeginSession: a later attempt never marks a missing step
	BeginSession();
	bRestarting = false;
	ResetTable();
	LeadWeapons.Restart();
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
	InstallSessionHook();
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
	std::string BreakError;
	if (!StoneBearer.IsLoaded() || !StoneBearer.BuildKind(Def.enemies[static_cast<size_t>(StoneBearerSlot)], Def.fighter, &Error, &BreakError))
	{
		if (!Error.empty())
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %s"), *FT::ToFString(Error));
		}
		StoneBearerSlot = -1;
	}
	// Once a boss kind a play session (the runner's life): every fight's prepare builds the same kind again.
	const FName BreakKind(*FT::ToFString(FFTStoneBearerHost::KindId));
	if (!BreakError.empty() && !BreakErrorLogged.Contains(BreakKind))
	{
		BreakErrorLogged.Add(BreakKind);
		UE_LOG(LogForgottenThird, Error, TEXT("Arena: %s"), *FT::ToFString(BreakError));
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
	bool bWalk = bWas;
	if (!bBetween || bOwedUp || bHitPlays)
	{
		bWalk = false;
	}
	else if (!bWas)
	{
		bWalk = Wanted > kArenaWalkStart && bAhead && StoneTurnSign == 0; // a turn under way ends at its cycle's end first
	}
	else if (Walked >= kStoneWalkHold && (Wanted < kArenaWalkStop || !bAhead))
	{
		bWalk = false;
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
			StoneHeldTurn += !bOwedUp && !bHitPlays && bAhead ? 1 : 0;
		}
	}
	bStoneWanted = bWants;
	const float Speed = static_cast<float>((*Skin.Clips)[static_cast<size_t>(Skin.Walk)].speed);
	if (bWalk && !bWas)
	{
		StoneWalkSince = Now;
		++StoneWalksBegun;
		// Logged for the walk checks in -nullrhi runs.
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d walks at %.2f m/s (rate 1.00, wanted %.2f m/s)."), Fight.time(), I, Speed, Wanted);
	}
	else if (!bWalk && bWas)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d stops walking after %.2f s (rate 1.00)."), Fight.time(), I, Walked);
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
	UE_LOG(LogForgottenThird, Log, TEXT("Arena: stone_bearer walk: wanted over %.2f m/s on %d steps (Approach %d, Strafe %d) in %d runs; %d walks begun; starts held by the cone %d, a get-up %d, a hit %d steps."),
		kArenaWalkStart, StoneWantApproach + StoneWantStrafe, StoneWantApproach, StoneWantStrafe, StoneWantRuns, StoneWalksBegun,
		StoneHeldCone, StoneHeldUp, StoneHeldHit);
	if (Skins[StoneBearerSlot].TurnL >= 0)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: stone_bearer turns: %d begun, %d steps, %d cancelled; walk starts held by a turn %d steps."),
			StoneTurnsBegun, StoneTurnSteps, StoneTurnsCancelled, StoneHeldTurn);
	}
}

bool UFTArenaRunner::StoneTurns() const
{
	if (StoneBearerSlot < 0 || StoneBearerSlot >= MaxEnemies || static_cast<size_t>(StoneBearerSlot) >= Fight.enemyCount())
	{
		return false;
	}
	const FFTArenaSkin& Skin = Skins[StoneBearerSlot];
	const AFTArenaFighter* Figure = EnemyFigures[StoneBearerSlot].Get();
	return Figure && Figure->IsSkinned() && Skin.IsLoaded() && Skin.TurnL >= 0 && Skin.TurnR >= 0;
}

ftcore::Vec2 UFTArenaRunner::StoneTurnFacing(double T) const
{
	// Its whole cycles, then the smootherstep of the one under way (held once it has settled), never past the cycles it is
	// committed to; scaled while its clip fades in by the crossfade's weight (which the anim instance raises linearly).
	const double At = FMath::Max(T, 0.0);
	const double K = FMath::Floor(At / kStoneTurnCycle);
	const double U = FMath::Min((At - K * kStoneTurnCycle) / kStoneTurnPivot, 1.0);
	const double Eased = U * U * U * (10.0 - 15.0 * U + 6.0 * U * U);
	const double Weight = StoneTurnBlend > 0.0 ? FMath::Min(1.0, (At + static_cast<double>(FrameDelta)) / StoneTurnBlend) : 1.0;
	const double Angle = FMath::DegreesToRadians(static_cast<double>(kStoneTurnDeg)) * FMath::Min(K + Eased, static_cast<double>(StoneTurnCycles))
		* Weight * static_cast<double>(StoneTurnSign);
	const double C = FMath::Cos(Angle);
	const double S = FMath::Sin(Angle);
	return ftcore::Vec2{static_cast<float>(StoneTurnFrom.x * C - StoneTurnFrom.y * S), static_cast<float>(StoneTurnFrom.x * S + StoneTurnFrom.y * C)};
}

void UFTArenaRunner::StoneBearerTurnBeforeTick()
{
	if (!StoneTurns())
	{
		return;
	}
	const size_t Slot = static_cast<size_t>(StoneBearerSlot);
	// Standing, only the turn turns it; walking, Core turns it as it goes.
	Fight.setEnemyTurnHeld(Slot, !bWalking[StoneBearerSlot]);
	if (StoneTurnSign != 0)
	{
		Fight.setEnemyFacing(Slot, StoneTurnFacing(static_cast<double>(Fight.time()) + StepSeconds - StoneTurnStart));
	}
}

void UFTArenaRunner::StoneBearerTurn(const ftcore::ArenaOutput& Out)
{
	if (!StoneTurns())
	{
		return;
	}
	const int32 I = StoneBearerSlot;
	const size_t Slot = static_cast<size_t>(I);
	const FFTArenaSkin& Skin = Skins[I];
	const AFTArenaFighter* Figure = EnemyFigures[I].Get();
	const ftcore::ArenaEnemyOut& E = Out.enemies[Slot];
	const int32 Hit = bHitHeavy[I] ? Skin.HitHeavy : Skin.HitLight;
	const bool bHitPlays = Hit >= 0 && Now - HitSince[I] < static_cast<double>(Skin.Sequences[Hit]->GetPlayLength());
	const bool bBetween = E.brain == ftcore::ArenaBrain::Approach || E.brain == ftcore::ArenaBrain::Strafe;
	const bool bOwedUp = Now < UpOwedUntil[I];
	const bool bFree = bBetween && !bWalking[I] && !bHitPlays && !bOwedUp && !StoneBearer.OnPlinth() && Fight.enemyAlive(Slot);
	// How far the lead is off its facing (degrees): + to its right (the yaw rising), - to its left.
	const ftcore::Vec2 To{Bodies.player.x - Bodies.enemies[Slot].x, Bodies.player.y - Bodies.enemies[Slot].y};
	const float Off = FMath::RadiansToDegrees(FMath::Atan2(E.facing.x * To.y - E.facing.y * To.x, E.facing.x * To.x + E.facing.y * To.y));
	if (StoneTurnSign != 0)
	{
		const TCHAR* Way = StoneTurnSign > 0 ? TEXT("right") : TEXT("left");
		const TCHAR* PivotName = StoneTurnSign > 0 ? TEXT("foot_r") : TEXT("foot_l"); // turn_r pivots on its right foot
		const double T = static_cast<double>(Fight.time()) - StoneTurnStart;
		FVector Foot;
		if (!bStoneTurnChecked && bFree && T >= kStoneTurnPivot)
		{
			// The sign check, logged for the -nullrhi runs: the pivot foot as drawn once the first cycle's yaw has settled,
			// against where it stood as the turn began (it is planted until the catch-up step, 0.43 s in). A turn whose yaw
			// runs against its clip moves it 0.2-0.4 m.
			bStoneTurnChecked = true;
			if (bStoneTurnFoot && Figure && Figure->GetSkinPoint(FName(PivotName), Foot))
			{
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d's turn (%s) has settled its first step: its pivot %s moved %.1f cm."), Fight.time(), I,
					Way, PivotName, FVector::Dist2D(Foot, StoneTurnFoot));
			}
		}
		if (!bFree)
		{
			// A move, a reaction or a hold takes it mid-turn: the facing stays where the turn had it. Logged with how far the
			// pivot foot is from where it stood as the cycle began (the crossfade out of a cycle under way slides it).
			const float Turned = FMath::RadiansToDegrees(FMath::Atan2(StoneTurnFrom.x * E.facing.y - StoneTurnFrom.y * E.facing.x,
				StoneTurnFrom.x * E.facing.x + StoneTurnFrom.y * E.facing.y));
			const TCHAR* Why = E.brain == ftcore::ArenaBrain::Attack ? TEXT("a move starts")
				: (E.brain == ftcore::ArenaBrain::Staggered ? TEXT("a stagger")
				: (!bBetween ? TEXT("it leaves its steps") : (bHitPlays ? TEXT("a hit reaction") : (bWalking[I] ? TEXT("a walk") : TEXT("a hold")))));
			const double Moved = bStoneTurnFoot && Figure && Figure->GetSkinPoint(FName(PivotName), Foot) ? FVector::Dist2D(Foot, StoneTurnFoot) / 100.0 : -1.0;
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d's turn (%s) is cancelled at %.1f deg, %.2f s into step %d (%s): its pivot %s is %.2f m from where it stood."),
				Fight.time(), I, Way, FMath::Abs(Turned), T - (StoneTurnCycles - 1) * kStoneTurnCycle, StoneTurnCycles, Why, PivotName, Moved);
			++StoneTurnsCancelled;
			StoneTurnSign = 0;
			return;
		}
		if (T + 1e-6 < StoneTurnCycles * kStoneTurnCycle)
		{
			return;
		}
		// A cycle's end: its facing is exactly its cycles round (StoneTurnFacing holds it there). Another step while the lead
		// is still that way past the residual and the walk doesn't want to start; else it ends, and the walk may start next.
		const float Wanted = ArenaLength(E.velocity);
		const float Cone = E.brain == ftcore::ArenaBrain::Approach ? kStoneApproachCone : kStoneWalkCone;
		const bool bWalkWants = Wanted > kArenaWalkStart && E.velocity.x * E.facing.x + E.velocity.y * E.facing.y >= Cone * Wanted;
		if (static_cast<float>(StoneTurnSign) * Off > kStoneTurnResidual && !bWalkWants)
		{
			++StoneTurnCycles;
			++StoneTurnSteps;
			bStoneTurnFoot = Figure && Figure->GetSkinPoint(FName(PivotName), StoneTurnFoot);
			return;
		}
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d's turn (%s) ends after %d steps (%.0f deg): the lead is %.1f deg off%s."), Fight.time(), I, Way,
			StoneTurnCycles, StoneTurnCycles * kStoneTurnDeg, Off, bWalkWants ? TEXT(", and it walks") : TEXT(""));
		StoneTurnSign = 0;
		return;
	}
	if (!bFree || FMath::Abs(Off) < kStoneTurnStart)
	{
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
	StoneTurnSign = Off > 0.f ? 1 : -1;
	StoneTurnStart = static_cast<double>(Fight.time());
	StoneTurnFrom = E.facing;
	StoneTurnCycles = 1;
	const ftcore::BeatClip& Row = (*Skin.Clips)[static_cast<size_t>(StoneTurnSign > 0 ? Skin.TurnR : Skin.TurnL)];
	StoneTurnBlend = Row.blend > 0.0 ? Row.blend : Skin.Clips->blend;
	bStoneTurnFoot = Figure && Figure->GetSkinPoint(FName(StoneTurnSign > 0 ? TEXT("foot_r") : TEXT("foot_l")), StoneTurnFoot);
	bStoneTurnChecked = false;
	++StoneTurnsBegun;
	++StoneTurnSteps;
	UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d turns %s: the lead is %.1f deg off (about %d steps)."), Fight.time(), I,
		StoneTurnSign > 0 ? TEXT("right") : TEXT("left"), Off, FMath::Max(1, FMath::RoundToInt(FMath::Abs(Off) / kStoneTurnDeg)));
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
	Kite.SetLock(bCountLift && Session ? &Session->Get() : nullptr);
	if (Kite.IsLocked())
	{
		// A seam behind a shared-blow lock in a fight with nothing to offer one has no crits at all: say so.
		ftcore::SharedBlowSources Sources;
		// While Lone nobody joins, and take half and the table are refused.
		Sources.sharedCount = bSharedCount && !Fight.lone();
		Sources.takeHalf = (bAutoPlay ? bTakeHalfAuto : bCarry) && !Fight.lone();
		Sources.tableShare = bTable && !Fight.lone();
		if (!ftcore::lockCanOpen(Kite.GetWeakPoints().lock, Sources))
		{
			UE_LOG(LogForgottenThird, Warning,
				TEXT("Arena: the kite's seam is locked and nothing in this fight can open it (no shared count, take half or table): no crits on the kite."));
		}
	}
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

// ---- Carrying: Load by chairs, block Load, the ally and take half (off unless a switch asks) ----

namespace
{
constexpr int32 kArenaAllyHp = 135;           // the ally's hp in a carrying run
constexpr float kArenaAllyBack = 0.5f;        // m the ally stands behind the player... (beside it, so a blow that
constexpr float kArenaAllySide = 2.f;         // ... and to its left                     reaches the player can reach it)
constexpr float kArenaAllyPace = 0.8f;        // of the fighter's moveSpeed: how fast the ally follows
constexpr float kArenaAllySettle = 0.15f;     // m: closer than this to its place, the ally stands
}

void UFTArenaRunner::BeginCarry(const FFTLeadLook* AllyLookIn)
{
	// Off by default, in every build; only the dev switches below turn carrying on (a shipping build keeps it off).
	bCarry = bAllyOn = bTakeHalfAuto = bAutoBlock = false;
	CarryChairKeys.Reset();
	CarryChairs = ftcore::ArenaChairs();
	CarryCap = ftcore::kMaxLoad;
	AllyLook = AllyLookIn;
	CarrySharedOffers = 0;
#if !UE_BUILD_SHIPPING
	const TCHAR* Cmd = FCommandLine::Get();
	FString Keys;
	const bool bChairs = FParse::Value(Cmd, TEXT("ArenaChairs="), Keys, false);
	int32 CapArg = 0;
	const bool bCapArg = FParse::Value(Cmd, TEXT("ArenaCap="), CapArg);
	bTakeHalfAuto = FParse::Param(Cmd, TEXT("ArenaTakeHalfAuto"));
	bAutoBlock = FParse::Param(Cmd, TEXT("ArenaAutoBlock"));
	bCarry = bChairs || bCapArg || bTakeHalfAuto || FParse::Param(Cmd, TEXT("ArenaAlly")) || FParse::Param(Cmd, TEXT("ArenaTable"))
		|| FParse::Param(Cmd, TEXT("ArenaTableAuto")) || FParse::Param(Cmd, TEXT("ArenaTableFill"))
		|| FParse::Param(Cmd, TEXT("ArenaTableKey"));
	if (!bCarry)
	{
		return;
	}
	bAllyOn = true;

	// The named places: four seats, one Named per chair key (its row's person), the rest never laid. "none" names
	// none, so the cap is the base carry alone.
	ftcore::PlaceTable Places; // none named without -ArenaChairs: no table can be set
	Places.reset(4);
	if (bChairs)
	{
		CarryChairKeys = Keys;
		if (Keys != TEXT("none"))
		{
			ftcore::ChairTable ChairRows;
			TArray<uint8> Bytes;
			std::string Error;
			const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/chairs.json");
			if (!(FFileHelper::LoadFileToArray(Bytes, *File) && ftcore::importChairTable(FT::BytesAsText(Bytes), ChairRows, &Error)))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Battle/chairs.json; no chairs are named.\n%s"), *FT::ToFString(Error));
				ChairRows = ftcore::ChairTable();
			}
			TArray<FString> Parts;
			Keys.ParseIntoArray(Parts, TEXT(","), true);
			for (const FString& Part : Parts)
			{
				const std::string Key(TCHAR_TO_UTF8(*Part));
				const ftcore::ChairRow* Row = ChairRows.findKey(Key);
				if (!Row)
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaChairs: no chair %s in Data/Battle/chairs.json."), *Part);
					continue;
				}
				if (Places.layPlace(Row->person) != ftcore::PlaceError::None)
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaChairs: chair %s could not be named (the seats are full or it is named already)."), *Part);
				}
			}
			CarryChairs = ftcore::chairsFor(Places, ChairRows);
		}
		CarryCap = Places.carryCap();
	}
	if (bCapArg)
	{
		CarryCap = FMath::Clamp(CapArg, 1, ftcore::kMaxLoad);
	}

	Fight.setLoadCap(CarryCap);
	Fight.setChairs(CarryChairs);
	Fight.setCarry(true);
	Fight.setAlly(kArenaAllyHp, Def.fighter.bodyRadius);
	BeginTable(Places, bCapArg);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: carrying: chairs %s (%d named), Load cap %d, blocks store Load, ally %d hp%s%s."),
		CarryChairKeys.IsEmpty() ? TEXT("-") : *CarryChairKeys, static_cast<int32>(CarryChairs.count), CarryCap, kArenaAllyHp,
		bTakeHalfAuto ? TEXT(", the autopilot takes half") : TEXT(""), bAutoBlock ? TEXT(", the autopilot blocks") : TEXT(""));
#endif
}

void UFTArenaRunner::SpawnAlly(UWorld* World)
{
	if (!bAllyOn || !World || AllyFigure)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AllyFigure = World->SpawnActor<AFTArenaFighter>(AFTArenaFighter::StaticClass(), FTransform::Identity, Params);
	if (AllyFigure)
	{
		// A lead's look is worn only by a player-side figure; the view stays on the player's figure.
		AllyFigure->Setup(true, Def.fighter.bodyRadius, kArenaPlayerColor, 0.f, AllyLook);
		AllyFigure->ResetFeet();
	}
}

ftcore::Vec2 UFTArenaRunner::AllyHome(const ftcore::Vec2& Player, const ftcore::Vec2& Facing) const
{
	// Behind (against the facing) and to the left (the facing turned a quarter anticlockwise), kept in the ring.
	const ftcore::Vec2 Home{Player.x - Facing.x * kArenaAllyBack - Facing.y * kArenaAllySide,
		Player.y - Facing.y * kArenaAllyBack + Facing.x * kArenaAllySide};
	return Fit.ring.clamp(Home, Def.fighter.bodyRadius + 0.5f);
}

void UFTArenaRunner::MoveAlly()
{
	if (!bAllyOn || !Fight.allyAlive())
	{
		return;
	}
	const ftcore::ArenaPlayerOut& P = Fight.output().player;
	const ftcore::Vec2 Home = AllyHome(Bodies.player, P.facing);
	const ftcore::Vec2 To{Home.x - Bodies.ally.x, Home.y - Bodies.ally.y};
	const float Far = ArenaLength(To);
	if (Far <= kArenaAllySettle)
	{
		return;
	}
	const float Step = FMath::Min(Far, Def.fighter.moveSpeed * kArenaAllyPace * StepSeconds);
	Bodies.ally.x += To.x / Far * Step;
	Bodies.ally.y += To.y / Far * Step;
}

void UFTArenaRunner::PlaceAlly(float Alpha)
{
	if (!AllyFigure || !bAllyOn)
	{
		return;
	}
	const ftcore::Vec2 Was = PrevBodies.ally;
	const ftcore::Vec2 Is = Bodies.ally;
	const ftcore::Vec2 Moved{Is.x - Was.x, Is.y - Was.y};
	const float Speed = ArenaLength(Moved) / StepSeconds;
	FFTArenaPose Pose;
	Pose.Feet = ToWorld(ArenaLerp(Was, Is, Alpha));
	// Walking: the way it goes; standing: the player's drawn facing.
	Pose.Yaw = ArenaYaw(Speed > 0.05f ? Moved : PlayerDrawFacing(Fight.output().player));
	Pose.Speed = Speed;
	Pose.bLying = !Fight.allyAlive();
	Pose.Tint = AllyFigure->GetBaseColor();
	if (Now - AllyHitTime < kArenaHitTint)
	{
		Pose.Tint = FLinearColor(1.f, 0.2f, 0.2f);
	}
	AllyFigure->ApplyPose(Pose);
}

void UFTArenaRunner::BeginBrief()
{
	BriefId.Reset();
	LeadBrief = ftcore::BriefId::Count;
	BriefStats.Reset();
	BriefRating = 0;
#if !UE_BUILD_SHIPPING
	FString RatingText;
	const bool bRating = FParse::Value(FCommandLine::Get(), TEXT("ArenaRating="), RatingText, false);
	FString Want;
	if (!FParse::Value(FCommandLine::Get(), TEXT("ArenaBrief="), Want, false) || Want.TrimStartAndEnd().IsEmpty())
	{
		if (bRating)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("Arena: -ArenaRating needs -ArenaBrief (a Rating is a Brief's); the run stops."));
			FTRequestExit(1, TEXT("ArenaRating"));
		}
		return;
	}
	if (bRating)
	{
		RatingText.TrimStartAndEndInline();
		// Digits only (IsNumeric takes signs, points and exponents): "1e3" or "+5" is refused, not read as 1 or 5.
		int32 Rating = 0;
		bool bDigits = RatingText.Len() >= 1 && RatingText.Len() <= 2;
		for (const TCHAR C : RatingText)
		{
			bDigits = bDigits && C >= TEXT('0') && C <= TEXT('9');
		}
		if (bDigits)
		{
			for (const TCHAR C : RatingText)
			{
				Rating = Rating * 10 + (C - TEXT('0'));
			}
		}
		if (Rating < 1 || Rating > ftcore::kMaxRating)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("Arena: -ArenaRating=%s: a Rating is a whole number 1..%d; the run stops."), *RatingText, ftcore::kMaxRating);
			FTRequestExit(1, TEXT("ArenaRating"));
			return;
		}
		BriefRating = Rating;
	}
	Want = Want.TrimStartAndEnd().ToLower();
	ftcore::BriefId Id = ftcore::BriefId::Count;
	if (!ftcore::briefFromName(FT::ToUtf8(Want), Id))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("Arena: -ArenaBrief: no Brief %s (counsel, bailiff, notary, proxy, escrow); the run stops."), *Want);
		FTRequestExit(1, TEXT("ArenaBrief"));
		return;
	}
	if (KitId != TEXT("duy"))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("Arena: -ArenaBrief: only the guns lead's own kit takes a Brief (this kit is %s); the run stops."), *KitId);
		FTRequestExit(1, TEXT("ArenaBrief"));
		return;
	}
	FString Text;
	const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/briefs.json");
	ftcore::BriefTable Briefs;
	std::string Error;
	if (!FFileHelper::LoadFileToString(Text, *File) || !ftcore::importBriefTable(FT::ToUtf8(Text), Def.fighter, Briefs, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("Arena: -ArenaBrief: no usable Data/Battle/briefs.json; the run stops. %s"), *FT::ToFString(Error));
		FTRequestExit(1, TEXT("ArenaBrief"));
		return;
	}
	const ftcore::BriefDef* Brief = Briefs.find(Id);
	if (!Brief)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("Arena: -ArenaBrief: %s is not in Data/Battle/briefs.json yet; the run stops."), *Want);
		FTRequestExit(1, TEXT("ArenaBrief"));
		return;
	}
	// Without -ArenaRating every stat is 10: the Brief's kit changes on the lead's own kit (slice 1's kit).
	const ftcore::LicenceStats Stats = BriefRating > 0 ? ftcore::LicenceRating{}.statsFor(*Brief, BriefRating) : ftcore::LicenceStats{};
	Def.fighter = ftcore::buildBriefKit(*Brief, Stats).fighter;
	BriefId = Want;
	LeadBrief = Id;
	BriefStats = FString::Printf(TEXT("%d/%d/%d/%d/%d"), Stats.v[0], Stats.v[1], Stats.v[2], Stats.v[3], Stats.v[4]);
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena: Brief %s on the %s kit (-ArenaBrief): Rating %d, stats %s, hp %d, rounds %d, reload %.2f s, shot %d damage %.0f m startup %.2f s, kicks %d/%d/%d, heavy %d, poise %.2f, guard absorb %.4f, parry %.4f."),
		*BriefId, *KitId, BriefRating, *BriefStats, Def.fighter.hp, static_cast<int32>(Def.fighter.rounds), Def.fighter.reload, Def.fighter.shot.damage,
		Def.fighter.shot.range, Def.fighter.shot.startup, Def.fighter.light[0].damage, Def.fighter.light[1].damage, Def.fighter.light[2].damage,
		Def.fighter.heavy.damage, Def.fighter.poise, Def.fighter.guardAbsorb, Def.fighter.parryWindow);
#endif
}

void UFTArenaRunner::LogBrief() const
{
	if (BriefId.IsEmpty())
	{
		return;
	}
	const ftcore::ArenaStats& S = Fight.stats();
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena: BRIEF %s rating %d stats %s rounds %d/%d reload %.2f shots %d shotHits %d reloads %d dryPresses %d kicks %d."),
		*BriefId, BriefRating, *BriefStats, static_cast<int32>(Fight.rounds()), static_cast<int32>(Fight.roundsMax()), Def.fighter.reload, S.shots,
		S.shotHits, S.reloads, S.dryPresses, S.lights);
}

void UFTArenaRunner::LogCarry() const
{
	if (!bCarry)
	{
		return;
	}
	const ftcore::ArenaStats& S = Fight.stats();
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena: CARRY chairs %s cap %d blockLoad %d takeHalfLoad %d takeHalves %d sharedOffers %d ally %d/%d."),
		CarryChairKeys.IsEmpty() ? TEXT("-") : *CarryChairKeys, Fight.loadCap(), S.blockLoad, S.takeHalfLoad, S.takeHalves,
		CarrySharedOffers, Fight.allyHp(), Fight.allyMaxHp());
}

// ---- Setting the table (-ArenaTable) ----

namespace
{
constexpr double kArenaTableCooldown = 20.0; // s after a table clears, at cap 100 (tableCooldownForCap stretches it)
constexpr int32 kArenaTableMinBodies = 3;    // a set table with fewer bodies gutters
constexpr int32 kArenaTableRingParts = 96;   // segments of the ground ring
constexpr int32 kArenaTableArcParts = 72;    // segments of the countdown arc
constexpr float kArenaTableRingWidth = 0.1f;  // m
constexpr float kArenaTableArcWidth = 0.05f;  // m, just inside the ring
constexpr float kArenaTableArcInset = 0.16f;  // m
constexpr float kArenaTableLineWidth = 0.025f; // m
constexpr float kArenaTableThick = 0.012f;     // m: how tall every part stands off the ground

// The world accent (Data/UI/style.json's Solhaven accent, the built-in style's when the file is missing or refused),
// sRGB to linear as the UI does, so the palette carries over to the ring.
FLinearColor TableAccent()
{
	ftcore::UiStyle Style = ftcore::defaultUiStyle();
	FString Text;
	if (FFileHelper::LoadFileToString(Text, *(FPaths::ProjectDir() / TEXT("Data/UI/style.json"))))
	{
		std::string Error;
		if (!ftcore::importUiStyle(FT::ToUtf8(Text), Style, &Error))
		{
			Style = ftcore::defaultUiStyle();
		}
	}
	const ftcore::UiColor C = ftcore::uiColor(Style, ftcore::UiWorld::Solhaven, ftcore::UiPalette::Default, ftcore::UiToken::Accent);
	const auto To8 = [](float V) { return static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(V, 0.f, 1.f) * 255.f)); };
	return FLinearColor(FColor(To8(C.r), To8(C.g), To8(C.b), 255));
}

const TCHAR* TableErrorName(ftcore::TableError E)
{
	switch (E)
	{
	case ftcore::TableError::None: return TEXT("none");
	case ftcore::TableError::NotFull: return TEXT("notFull");
	case ftcore::TableError::TooFewSeats: return TEXT("tooFewSeats");
	case ftcore::TableError::Busy: return TEXT("busy");
	case ftcore::TableError::Cooling: return TEXT("cooling");
	case ftcore::TableError::BadSetter: return TEXT("badSetter");
	case ftcore::TableError::Occupied: return TEXT("occupied");
	case ftcore::TableError::Lone: return TEXT("lone");
	}
	return TEXT("?");
}

const TCHAR* TableStateName(ftcore::TableState S)
{
	switch (S)
	{
	case ftcore::TableState::Ready: return TEXT("ready");
	case ftcore::TableState::Setting: return TEXT("setting");
	case ftcore::TableState::Set: return TEXT("set");
	case ftcore::TableState::Cooling: return TEXT("cooling");
	}
	return TEXT("?");
}
}

void UFTArenaRunner::BeginTable(const ftcore::PlaceTable& Places, bool bCapArg)
{
	bTable = bTableAuto = bTableFill = bTableKey = false;
	TablePlaces = Places;
#if !UE_BUILD_SHIPPING
	const TCHAR* Cmd = FCommandLine::Get();
	bTableAuto = FParse::Param(Cmd, TEXT("ArenaTableAuto"));
	bTableFill = FParse::Param(Cmd, TEXT("ArenaTableFill"));
	bTableKey = FParse::Param(Cmd, TEXT("ArenaTableKey"));
	bTable = FParse::Param(Cmd, TEXT("ArenaTable")) || bTableAuto || bTableFill || bTableKey;
	if (!bTable)
	{
		return;
	}
	const int32 MinSeats = ftcore::TableTuning().minSeats;
	if (bCapArg || Places.tableSeats() < MinSeats)
	{
		UE_LOG(LogForgottenThird, Error,
			TEXT("Arena: -ArenaTable needs -ArenaChairs naming %d or more chairs (it names %d) and no -ArenaCap (the table counts the chairs' cap); the run stops."),
			MinSeats, Places.tableSeats());
		bTable = false;
		FTRequestExit(1, TEXT("ArenaTable"));
		return;
	}
	ResetTable();
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: table: %d seats, at least %d bodies, ring %.1f m, cooldown %.1f s%s%s."),
		Places.tableSeats(), kArenaTableMinBodies, TableSeats.tuning().radius, TableClock.tuning().cooldown,
		bTableAuto ? TEXT(", the autopilot sets it at full Load") : TEXT(""), bTableFill ? TEXT(", the Load filled while it is ready") : TEXT(""));
#endif
}

void UFTArenaRunner::ResetTable()
{
	if (!bTable)
	{
		return;
	}
	ftcore::TableTuning Tuning;
	Tuning.minBodies = kArenaTableMinBodies;
	Tuning.cooldown = ftcore::tableCooldownForCap(kArenaTableCooldown, CarryCap);
	TableClock.reset(Tuning, static_cast<double>(Fight.heldTime()));
	TableSeats.reset(ftcore::TableRingTuning());
	Fight.setTableSeats(ftcore::ArenaTableSeats());
	TableLastRefusal = ftcore::TableError::None;
	TableBegins = TableRefusals = TableSets = TableMostBodies = TableStands = TableLeaves = 0;
	TableGutters = TableFails = TableClears = TableShares = TableShareHp = 0;
	TableKey.reset();
	TableKeyBegins = TableKeyTaps = TableKeyCancels = TableKeyPresses = TableKeyDrops = 0;
	TableSetSeconds = 0.0;
	TableLastTick = -1.0;
	if (TableActor)
	{
		TableActor->SetActorHiddenInGame(true);
	}
}

bool UFTArenaRunner::BeginTableNow(double T)
{
	ftcore::Load Held;
	Held.value = Fight.load();
	const ftcore::Vec2 At = Bodies.player;
	const ftcore::TableSpot Spot{At.x, At.y, TableSeats.tuning().radius};
	// While Lone the table is refused, whichever path asks (the key, -ArenaTableAuto).
	const ftcore::TableError E = Fight.lone() ? ftcore::TableError::Lone
		: TableClock.beginAt(ftcore::kLeadA, Held, TablePlaces, 0, Spot, nullptr, 0, T);
	if (E != ftcore::TableError::None)
	{
		++TableRefusals;
		if (E != TableLastRefusal)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: table refused at %.2f s: %s."), T, TableErrorName(E));
		}
		TableLastRefusal = E;
		return false;
	}
	TableLastRefusal = ftcore::TableError::None;
	const int32 Spent = Fight.spendLoad(Fight.load());
	TableSeats.place(At);
	++TableBegins;
	LoneBreaches += Fight.lone() ? 1 : 0;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: table setting at %.2f s at (%.2f, %.2f): %d Load spent, %d would sit."),
		T, At.x, At.y, Spent, TableSeats.wouldSit(Fight, Bodies, At));
	return true;
}

bool UFTArenaRunner::TableCanBegin() const
{
	return TableSetterFree() && TableCouldBegin();
}

bool UFTArenaRunner::TableCouldBegin() const
{
	ftcore::Load Held;
	Held.value = Fight.load();
	return !Fight.lone() && TableClock.state() == ftcore::TableState::Ready && Fight.hp() > 0 && Fight.load() >= Fight.loadCap()
		&& TablePlaces.full(Held) && TablePlaces.tableSeats() >= TableClock.tuning().minSeats
		&& TableSeats.wouldSit(Fight, Bodies, Bodies.player) >= kArenaTableMinBodies;
}

bool UFTArenaRunner::TableSetterFree() const
{
	return ftcore::tableSetterFree(Fight.output().player.action);
}

void UFTArenaRunner::StepTableKey(ftcore::ArenaInput& In)
{
	if (!bTable)
	{
		return;
	}
	const double T = static_cast<double>(Fight.heldTime());
	const bool bFree = TableSetterFree();
	const bool bPossible = TableCouldBegin();
	const ftcore::TableKeyStep S = TableKey.step(In.release, bReleaseDown, bFree, bPossible, T);
	In.release = S.release;
	if (S.why != ftcore::TableKeyDrop::None)
	{
		const TCHAR* Why = S.why == ftcore::TableKeyDrop::Stale ? TEXT("steps were missed (stale)")
			: S.why == ftcore::TableKeyDrop::Busy ? TEXT("the setter can't act (press swallowed)")
			: TEXT("the table stopped being possible (passed on as a Release)");
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: table key hold dropped at %.2f s: %s."), T, Why);
		if (S.why == ftcore::TableKeyDrop::Lost)
		{
			++TableKeyCancels;
		}
		else
		{
			++TableKeyDrops;
		}
	}
	switch (S.result)
	{
	case ftcore::TableHoldResult::Begin:
		// A refusal the checks above missed: the press goes to the fight as the Release it was.
		if (BeginTableNow(T))
		{
			++TableKeyBegins;
		}
		else
		{
			In.release = true;
			++TableKeyTaps;
		}
		break;
	case ftcore::TableHoldResult::PassRelease:
		++TableKeyTaps;
		break;
	default:
		break;
	}
}

ftcore::ArenaTableView UFTArenaRunner::GetTableView() const
{
	if (!bTable)
	{
		return ftcore::ArenaTableView();
	}
	return ftcore::tableView(TableClock, TableSeats, TableKey.hold(), static_cast<double>(Fight.heldTime()));
}

void UFTArenaRunner::TickTable(const ftcore::ArenaOutput& Out)
{
	if (!bTable)
	{
		return;
	}
	const double T = static_cast<double>(Fight.heldTime());
	if (TableClock.state() == ftcore::TableState::Set && TableLastTick >= 0.0)
	{
		TableSetSeconds += FMath::Max(0.0, T - TableLastTick);
	}
	TableLastTick = T;
	for (const ftcore::ArenaEvent& E : Out.events)
	{
		if (E.type == ftcore::ArenaEventType::TableShare)
		{
			++TableShares;
			TableShareHp += E.amount;
		}
	}
	// The setter staggered or downed while it sets: the table fails.
	if (TableClock.state() == ftcore::TableState::Setting)
	{
		const ftcore::ArenaAction Act = Out.player.action;
		if (Act == ftcore::ArenaAction::Hitstun || Act == ftcore::ArenaAction::GuardBreak || Act == ftcore::ArenaAction::Downed)
		{
			TableClock.interrupt(T);
		}
	}
	// The autopilot: at full Load, with enough bodies in reach (the setter counted).
	if (bAutoPlay && TableClock.state() == ftcore::TableState::Ready && Fight.hp() > 0)
	{
		if (bTableFill && Fight.load() < Fight.loadCap())
		{
			Fight.giveLoad(Fight.loadCap() - Fight.load());
		}
		if (bTableAuto && TableSetterFree() && Fight.load() >= Fight.loadCap()
			&& TableSeats.wouldSit(Fight, Bodies, Bodies.player) >= kArenaTableMinBodies)
		{
			BeginTableNow(T);
		}
	}
	const auto Drain = [&]()
	{
		for (const ftcore::TableEvent& E : TableClock.update(T))
		{
			switch (E.type)
			{
			case ftcore::TableEventType::Set:
			{
				const int32 Sat = TableSeats.sit(Fight, Bodies);
				++TableSets;
				TableMostBodies = FMath::Max(TableMostBodies, Sat);
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: table set at %.2f s: %d bodies (seats 0x%x, waiting 0x%x)."),
					E.at, Sat, static_cast<uint32>(TableSeats.seated()), static_cast<uint32>(TableSeats.waiting()));
				TableClock.checkBodies(Sat, T);
				break;
			}
			case ftcore::TableEventType::Guttered:
				++TableGutters;
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: table guttered at %.2f s: %d bodies."), E.at, TableSeats.bodies());
				TableSeats.clear();
				break;
			case ftcore::TableEventType::Failed:
				++TableFails;
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: table failed at %.2f s: the setter was struck."), E.at);
				TableSeats.clear();
				break;
			case ftcore::TableEventType::Cleared:
				++TableClears;
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: table cleared at %.2f s."), E.at);
				TableSeats.clear();
				break;
			default:
				break;
			}
		}
	};
	Drain();
	if (TableClock.set())
	{
		const ftcore::TableRingChange Change = TableSeats.update(Fight, Bodies);
		if (Change.any())
		{
			TableStands += Change.stood;
			TableLeaves += Change.left;
			TableMostBodies = FMath::Max(TableMostBodies, TableSeats.bodies());
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: table at %.2f s: %d stood, %d left, %d sat: %d bodies."),
				T, Change.stood, Change.left, Change.sat, TableSeats.bodies());
			TableClock.checkBodies(TableSeats.bodies(), T);
			Drain();
		}
	}
	Fight.setTableSeats(TableClock.set() && TableSeats.placed()
		? TableSeats.seats(Fight, TableClock.tuning().enemyShare, TablePlaces.tableSeats())
		: ftcore::ArenaTableSeats());
	if (TableClock.state() == ftcore::TableState::Setting || TableClock.set())
	{
		SpawnTable();
	}
}

void UFTArenaRunner::SpawnTable()
{
	UWorld* World = GetWorld();
	if (TableActor || !World)
	{
		return;
	}
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Cube || !Base)
	{
		return;
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	TableActor = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	if (!TableActor)
	{
		return;
	}
	UStaticMeshComponent* Root = TableActor->GetStaticMeshComponent();
	Root->SetMobility(EComponentMobility::Movable);
	Root->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Root->SetCanEverAffectNavigation(false);
	const FLinearColor Accent = TableAccent();
	UMaterialInstanceDynamic* Ring = UMaterialInstanceDynamic::Create(Base, TableActor);
	Ring->SetVectorParameterValue(TEXT("Color"), Accent);
	UMaterialInstanceDynamic* Line = UMaterialInstanceDynamic::Create(Base, TableActor);
	Line->SetVectorParameterValue(TEXT("Color"), Accent);
	const auto Part = [&](UMaterialInstanceDynamic* Paint) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(TableActor);
		C->SetMobility(EComponentMobility::Movable);
		C->SetStaticMesh(Cube);
		C->SetMaterial(0, Paint);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCanEverAffectNavigation(false);
		C->SetCastShadow(false);
		C->SetupAttachment(Root);
		C->RegisterComponent();
		TableActor->AddInstanceComponent(C);
		return C;
	};
	// A segment of a circle of radius R round the actor: the cube is 1 m on a side, centred.
	const auto Circle = [&](TArray<TObjectPtr<UStaticMeshComponent>>& Parts, int32 Count, float Radius, float Width)
	{
		const float Chord = 2.f * Radius * FMath::Sin(PI / static_cast<float>(Count)) + Width * 0.5f;
		for (int32 I = 0; I < Count; ++I)
		{
			const float A = (static_cast<float>(I) + 0.5f) / static_cast<float>(Count) * 2.f * PI;
			UStaticMeshComponent* C = Part(Ring);
			C->SetRelativeLocation(FVector(FMath::Cos(A) * Radius * 100.0, FMath::Sin(A) * Radius * 100.0, kArenaTableThick * 50.0 + 1.0));
			C->SetRelativeRotation(FRotator(0.0, FMath::RadiansToDegrees(A) + 90.0, 0.0));
			C->SetRelativeScale3D(FVector(Chord, Width, kArenaTableThick));
			Parts.Add(C);
		}
	};
	const float Radius = TableSeats.tuning().radius;
	Circle(TableRingParts, kArenaTableRingParts, Radius, kArenaTableRingWidth);
	Circle(TableArcParts, kArenaTableArcParts, Radius - kArenaTableArcInset, kArenaTableArcWidth);
	for (int32 I = 0; I < 1 + MaxEnemies; ++I)
	{
		UStaticMeshComponent* C = Part(Line);
		C->SetUsingAbsoluteLocation(true);
		C->SetUsingAbsoluteRotation(true);
		C->SetUsingAbsoluteScale(true);
		C->SetVisibility(false);
		TableLineParts.Add(C);
	}
	TableActor->SetActorHiddenInGame(true);
}

void UFTArenaRunner::PaintTable(float Alpha)
{
	if (!bTable || !TableActor)
	{
		return;
	}
	const ftcore::TableState State = TableClock.state();
	const bool bShow = (State == ftcore::TableState::Setting || State == ftcore::TableState::Set) && TableSeats.placed();
	TableActor->SetActorHiddenInGame(!bShow);
	if (!bShow)
	{
		return;
	}
	TableActor->SetActorLocation(ToWorld(TableSeats.centre()));
	// The arc: filling while it sets, then the time left while set (clockwise from the front).
	const double T = static_cast<double>(Fight.heldTime());
	const ftcore::TableTuning& Tuning = TableClock.tuning();
	float Share = 0.f;
	if (State == ftcore::TableState::Setting)
	{
		Share = Tuning.setTime > 0.0 ? 1.f - static_cast<float>(FMath::Clamp((TableClock.until() - T) / Tuning.setTime, 0.0, 1.0)) : 1.f;
	}
	else
	{
		Share = static_cast<float>(FMath::Clamp((TableClock.until() - T) / Tuning.setDuration, 0.0, 1.0));
	}
	const int32 Lit = FMath::CeilToInt32(Share * static_cast<float>(TableArcParts.Num()));
	for (int32 I = 0; I < TableArcParts.Num(); ++I)
	{
		TableArcParts[I]->SetVisibility(I < Lit);
	}
	// A thin line from each seated body to the setter, on the ground.
	const ftcore::Vec2 Setter = ArenaLerp(PrevBodies.player, Bodies.player, Alpha);
	const auto Draw = [&](int32 Slot, bool bOn, const ftcore::Vec2& From)
	{
		if (!TableLineParts.IsValidIndex(Slot))
		{
			return;
		}
		UStaticMeshComponent* C = TableLineParts[Slot];
		const ftcore::Vec2 Span{Setter.x - From.x, Setter.y - From.y};
		const float Length = ArenaLength(Span);
		C->SetVisibility(bOn && State == ftcore::TableState::Set && Length > 0.05f);
		if (!C->IsVisible())
		{
			return;
		}
		const ftcore::Vec2 Mid{(Setter.x + From.x) * 0.5f, (Setter.y + From.y) * 0.5f};
		C->SetWorldLocation(ToWorld(Mid) + FVector(0.0, 0.0, kArenaTableThick * 50.0 + 1.5));
		C->SetWorldRotation(FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(Span.y, Span.x)), 0.0));
		C->SetWorldScale3D(FVector(Length, kArenaTableLineWidth, kArenaTableThick));
	};
	const uint16 Seated = TableSeats.seated();
	Draw(0, (Seated & ftcore::kTableAllyBit) != 0, ArenaLerp(PrevBodies.ally, Bodies.ally, Alpha));
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		const size_t E = static_cast<size_t>(I);
		const bool bOn = E < Fight.enemyCount() && (Seated & ftcore::tableEnemyBit(E)) != 0;
		Draw(1 + I, bOn, ArenaLerp(PrevBodies.enemies[E], Bodies.enemies[E], Alpha));
	}
}

void UFTArenaRunner::LogTable() const
{
	if (!bTable)
	{
		return;
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena: TABLE begins %d refusals %d sets %d mostBodies %d stood %d left %d guttered %d failed %d cleared %d shares %d shareHp %d keyBegins %d keyTaps %d keyCancels %d keyDrops %d setSeconds %.1f of %.1f state %s."),
		TableBegins, TableRefusals, TableSets, TableMostBodies, TableStands, TableLeaves, TableGutters, TableFails, TableClears,
		TableShares, TableShareHp, TableKeyBegins, TableKeyTaps, TableKeyCancels, TableKeyDrops, TableSetSeconds,
		static_cast<double>(Fight.heldTime()), TableStateName(TableClock.state()));
}

void UFTArenaRunner::EndTable()
{
	TableRingParts.Reset();
	TableArcParts.Reset();
	TableLineParts.Reset();
	if (TableActor)
	{
		TableActor->Destroy();
		TableActor = nullptr;
	}
}
