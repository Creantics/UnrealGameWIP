#include "FTArenaRunner.h"

#include "FTCrowdSubsystem.h"
#include "FTCrowdSubsystemV2.h"

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
#include "FTArenaDevTiming.h"
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
#include "FTNewUi.h"
#include "FTPauseSubsystem.h"
#include "FTStoryStateSubsystem.h"
#include "FTText.h"
#include "FTVfxComponent.h"
#include "NiagaraComponent.h"
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
#include "ftcore/LeadSwitch.h"
#include "ftcore/ArenaCloseFrame.h"

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
// The health share (0..1 of the body's hp) the held-out partner brings when it joins a fight begun Lone. There is
// no carried-health source yet (party health between fights is owed to the save lane), so the partner joins at full
// health. This is the one place the value comes from.
float PartnerJoinHealth()
{
	return 1.f;
}

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
constexpr float kArenaAllyTurnRate = 540.f; // deg/s: the drawn ally turns no faster (no one-frame snap into a string)
// The floor tell's flash after a move goes active (s).
constexpr double kArenaTellFlash = 0.3;
// A boss at least this tall (m) is looked up at: the camera's pitch rises toward its face, at this share of its height
// (hover included), by ftcore::arenaLookUpPitch (the boom held at its limit, the lens tilting the rest).
const TCHAR* PartnerActWord(ftcore::PartnerAct A)
{
	switch (A)
	{
	case ftcore::PartnerAct::Wait: return TEXT("Wait");
	case ftcore::PartnerAct::MoveTo: return TEXT("MoveTo");
	case ftcore::PartnerAct::Attack: return TEXT("Attack");
	case ftcore::PartnerAct::Guard: return TEXT("Guard");
	case ftcore::PartnerAct::StepBack: return TEXT("StepBack");
	}
	return TEXT("?");
}

const TCHAR* PartnerWhyWord(ftcore::PartnerWhy W)
{
	static const TCHAR* const Words[] = {TEXT("NotBegun"), TEXT("Down"), TEXT("CannotAct"), TEXT("Guard"), TEXT("Leash"),
		TEXT("String"), TEXT("Pause"), TEXT("NoTarget"), TEXT("Approach"), TEXT("HeldOff"), TEXT("Start"), TEXT("Budget"),
		TEXT("Committed"), TEXT("Reposition"), TEXT("NoKit")};
	const size_t I = static_cast<size_t>(W);
	return I < UE_ARRAY_COUNT(Words) ? Words[I] : TEXT("?");
}

// The brain's own word for why the partner is not acting (ftcore::PartnerIdle).
const TCHAR* PartnerIdleWord(ftcore::PartnerIdle W)
{
	static const TCHAR* const Words[] = {TEXT("None"), TEXT("NoBudget"), TEXT("NoTarget"), TEXT("Moving"), TEXT("Staggered"),
		TEXT("Pause"), TEXT("Other"), TEXT("NoKit")};
	static_assert(UE_ARRAY_COUNT(Words) == ftcore::kPartnerIdleCount, "one word per idle reason");
	const size_t I = static_cast<size_t>(W);
	return I < UE_ARRAY_COUNT(Words) ? Words[I] : TEXT("?");
}

constexpr float kArenaTallBoss = ftcore::kArenaLookTallBoss;
// The low lens's shoulder crosses to its other side at this speed (m/s) when ftcore::arenaLowSide flips it.
constexpr float kArenaLowSideRate = 2.4f;
// The partner's soft turn eases at this rate (1/s) and stays off this long (s) after the player turns the view.
constexpr float kPartnerYawRate = 3.f;
constexpr float kPartnerYawQuiet = 1.5f;
// "Arena check: partner in play": an act counts once it lasted this long (s) and was carried out.
constexpr float kPartnerActMin = 0.25f;
// Its rate bar is judged over engaged steps alone (ruled 11-16): those with the lead's last attack (a light, heavy,
// riposte or release) at most this long ago (s), since the partner's attack time is capped by the lead's.
constexpr float kPartnerEngagedWindow = 5.f;
// Under this many engaged seconds the rate bar is not covered (the line says so and judges the screen share alone).
constexpr float kPartnerEngagedMinSeconds = 5.f;
#if !UE_BUILD_SHIPPING
// -ArenaStandOffStrike: the presses of one light string and the time between them (s).
constexpr int32 kStandOffStringPresses = 3;
constexpr float kStandOffPressGap = 0.45f;
#endif
// The close lens's boom lag held to this far (m): the lead sits about 0.05 of the width inside his third, which at the close
// arm is about 0.15 m across, and the boom's own lag left the lens up to 0.6 m behind a running lead.
constexpr float kArenaCloseLagMost = 0.10f;
// A pose with its arm, shoulder and height scaled by `Share` (the boom's wall probe shortens all three alike).
ftcore::ArenaCameraPose WallScaled(ftcore::ArenaCameraPose Pose, float Share)
{
	Pose.arm *= Share;
	Pose.side *= Share;
	Pose.up *= Share;
	return Pose;
}

constexpr float kArenaFaceHeight = ftcore::kArenaLookFaceShare;
#if !UE_BUILD_SHIPPING
// The close framing check's aim point on a target with no weak point: its chest, this share of its height up.
constexpr float kArenaChestShare = 0.72f;
#endif
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
// The camera half-angle's log line: only a change over this many degrees, at most once in this many seconds.
constexpr float kCameraHalfLogStep = 1.f;
constexpr double kCameraHalfLogEvery = 1.0;
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
	// The lead's kit must be one the player may drive (ftcore::arenaPlayable): a party member only never starts a fight
	// as the lead. Refused: no fight begins, one error line (the host checks first; this is the runner's own guard).
	if (!ftcore::arenaPlayable(Setup.Def.fighter))
	{
		UE_CLOG(!bLeadRefused, LogForgottenThird, Error, TEXT("Arena: the fight is refused: the lead's kit (%s) is not playable; no fight begins."),
			*Setup.KitId);
		bLeadRefused = true;
		return;
	}
	if (Setup.Table)
	{
		Table = *Setup.Table;
	}
	Def = Setup.Def;
	// Per-lead kits: the partner fights with the kit of the lead it is (its look's arena.json entry). No fallback:
	// with no table, no ally or no entry for it, it is NoKit (no attacks, not playable), never the lead's kit, so a
	// later change to Def.fighter (the Brief's kit) leaves nothing stale here.
	const ftcore::ArenaFighter* AllyKit = Setup.Table && Setup.Ally ? Table.findFighter(FT::ToUtf8(Setup.Ally->Name.ToLower())) : nullptr;
	Def.partnerKit = AllyKit ? *AllyKit : ftcore::arenaNoKit();
	if (!AllyKit && Setup.Ally)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("Arena: the partner %s has no kit (%s): it is NoKit (no attacks, not playable), never the lead's kit."),
			*Setup.Ally->Name, Setup.Table ? TEXT("no entry in Data/Battle/arena.json") : TEXT("no arena table"));
	}
	Fit = Setup.Fit;
	// The camera preset: put on this copy of the fit's camera after the zone's fit was made and checked with the zone's
	// own rig (the close lens's arm and pitch are outside that check's ranges, ftcore::checkArenaZone); classic leaves
	// the fit's camera untouched. The ring, the starts and the sizing are the fit's own; what the fight reads from the
	// drawn camera (the view yaw, the windup arc's half-angle, the partner's stationing camera) follows the lens, as
	// ftcore/CameraPreset.h lists. Fit.cameraBlocked stays the zone rig's share; the close lens's own is not measured yet.
	PickCameraPreset();
	Fit.camera = ftcore::applyCameraPreset(Fit.camera, CameraPresetTable, CameraPresetNow);
	// The close lens is shaped each frame from the preset as picked (ShapeCloseLens); classic is never shaped.
	bCloseShape = CameraPresetNow == ftcore::CameraPresetId::Close;
	CloseBase = CameraLens;
#if !UE_BUILD_SHIPPING
	if (bCloseShape && CloseArmSwitch > 0.f)
	{
		// -ArenaCameraArm=<m>: the close lens's arm for this run (a review pair), read by PickCameraPreset.
		Fit.camera.arm = CloseArmSwitch;
		CloseBase.rig.arm = CameraLens.rig.arm = CloseArmSwitch;
	}
	{
		// -ArenaQuitAfter=<s>: the run's end held this long after the result (0.5 to 30 s); see the header.
		QuitAfterFinish = 0.f;
		float QuitAfter = 0.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaQuitAfter="), QuitAfter) && FMath::IsFinite(QuitAfter) && QuitAfter > 0.f)
		{
			QuitAfterFinish = FMath::Clamp(QuitAfter, 0.5f, 30.f);
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaQuitAfter: the run ends %.2f s after the result."), QuitAfterFinish);
		}
		// -ArenaStandOff=<m>: the autopilot circles its target at this range, guarding, no attacks or dodges (stand-off
		// stills at a set range). 0 or missing: off.
		StandOffAt = 0.f;
		float Stand = 0.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaStandOff="), Stand) && FMath::IsFinite(Stand) && Stand > 0.f)
		{
			StandOffAt = FMath::Clamp(Stand, 1.f, 20.f);
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaStandOff: the autopilot circles its target at %.2f m."), StandOffAt);
		}
		// -ArenaStandOffStrike=<s>: with it, a light string from that range every <s> s (1.5 to 10), the guard let go over
		// it, so a stand-off run's lead fights and the partner's budget (capped by the lead's attack time) is a fair one.
		StandOffStrikeEvery = 0.f;
		float Strike = 0.f;
		if (StandOffAt > 0.f && FParse::Value(FCommandLine::Get(), TEXT("ArenaStandOffStrike="), Strike) && FMath::IsFinite(Strike) && Strike > 0.f)
		{
			StandOffStrikeEvery = FMath::Clamp(Strike, 1.5f, 10.f);
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaStandOffStrike: a light string of %d presses every %.1f s."),
				kStandOffStringPresses, StandOffStrikeEvery);
		}
	}
	{
		// -ArenaRollAt=<s,s,...>: the autopilot presses dodge at these fight times (a capture's dodge strip, the same
		// roll time under every lens). Read once here; up to MaxRollAt, in rising order.
		RollAtCount = 0;
		FString RollList;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaRollAt="), RollList, false))
		{
			TArray<FString> Parts;
			RollList.ParseIntoArray(Parts, TEXT(","));
			for (const FString& Part : Parts)
			{
				const float At = FCString::Atof(*Part);
				if (RollAtCount < MaxRollAt && FMath::IsFinite(At) && At >= 0.f && (RollAtCount == 0 || At > RollAt[RollAtCount - 1]))
				{
					RollAt[RollAtCount++] = At;
				}
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaRollAt: %d dodge presses for the autopilot."), RollAtCount);
		}
	}
#endif
	LeadSeat = Setup.LeadSeat;
	AllySeat = Setup.AllySeat;
	LeadName = Setup.Lead ? Setup.Lead->Name : FString();
	AllyName = Setup.Ally ? Setup.Ally->Name : FString();
#if !UE_BUILD_SHIPPING
	// Review stills: start the player 1.2 m from the zone's wall, beside it, so the wall's cue shows.
	if (FParse::Param(FCommandLine::Get(), TEXT("ArenaNearWall")))
	{
		const ftcore::Vec2 Side{Fit.playerFacing.y, -Fit.playerFacing.x};
		ftcore::Vec2 P = Fit.playerStart;
		for (int32 I = 0; I < 400 && Fit.zone.wallDistance(P) > 1.2f; ++I)
		{
			P = ftcore::Vec2{P.x + Side.x * 0.1f, P.y + Side.y * 0.1f};
		}
		Fit.playerStart = P;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: the player starts %.1f m from the wall (-ArenaNearWall)."), Fit.zone.wallDistance(P));
	}
#endif
	ZoneId = Setup.ZoneId;
	ZoneZ = Setup.ZoneZ;
	ZoneStyle = Setup.ZoneStyle;
	KitId = Setup.KitId;
	Seed = Setup.Seed;
	ZoneYaw = Setup.ZoneYaw;
	bAutoPlay = Setup.bAutoPlay;
	bIdle = Setup.bIdle;
	bPartyScreenOn = FT::NewUiEnabled(); // read once, as the HUD reads it at BeginPlay
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
	// Every strike's aim line on a capture run (-ArenaShot) or on asking; else one summary at the fight's end.
	LeadWeapons.SetLogEachAim(bShots || FParse::Param(FCommandLine::Get(), TEXT("ArenaAimEach")));
	AllyWeapons.SetLogEachAim(bShots || FParse::Param(FCommandLine::Get(), TEXT("ArenaAimEach")));
	ReadSpecialTable(); // the special moves' shots (once) and their test switches
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
	CloseFrameOk = CloseFrameLead = CloseFrameFoe = CloseFrameMate = 0;
	for (int32 I = 0; I < 2; ++I)
	{
		CloseMissWall[I] = CloseMissLag[I] = CloseMissCap[I] = CloseMissMateTurn[I] = CloseMissFast[I] = CloseMissNear[I] = 0;
	}
	CloseMissFoeLow = CloseMissFoeHigh = CloseMissFoeAtLens = 0;
	CloseMissFoeSum = 0.f;
	FrameShotSkipped = CloseFarSkipped = 0;
	ResetLeadFrameChecks();
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
	bNoLoneCall = bLone && FParse::Param(FCommandLine::Get(), TEXT("ArenaNoLoneCall"));
	LoneGiftAt = -1.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaLoneGiftAt="), LoneGiftAt) && !(LoneGiftAt >= 0.f))
	{
		LoneGiftAt = -1.f;
	}
#else
	bLone = false;
	bNoLoneCall = false;
#endif
	// The third slot: the story carry's while it is a fight the story handed off and not yet settled (Companion with no
	// carry), then -ArenaThirdSlot outside Shipping.
	ThirdSlot = static_cast<uint8>(ftcore::SlotState::Companion);
	{
		const UWorld* SlotWorld = GetWorld();
		const UFTGameInstance* SlotInstance = SlotWorld ? SlotWorld->GetGameInstance<UFTGameInstance>() : nullptr;
		if (SlotInstance && SlotInstance->StoryCarry.bValid && !SlotInstance->StoryCarry.bDone && SlotInstance->StoryCarry.ThirdSlot <= static_cast<uint8>(ftcore::SlotState::Hostile))
		{
			ThirdSlot = SlotInstance->StoryCarry.ThirdSlot;
		}
	}
#if !UE_BUILD_SHIPPING
	{
		FString Slot;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaThirdSlot="), Slot))
		{
			if (Slot.Equals(TEXT("Companion"), ESearchCase::IgnoreCase)) ThirdSlot = static_cast<uint8>(ftcore::SlotState::Companion);
			else if (Slot.Equals(TEXT("Vacant"), ESearchCase::IgnoreCase)) ThirdSlot = static_cast<uint8>(ftcore::SlotState::Vacant);
			else if (Slot.Equals(TEXT("Hostile"), ESearchCase::IgnoreCase)) ThirdSlot = static_cast<uint8>(ftcore::SlotState::Hostile);
		}
	}
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
	PlayerLook = Setup.Lead;
	BeginCarry(Setup.Ally);
	BeginPartner();
	PrepareStoneBearer();
	PrepareKite();
#if !UE_BUILD_SHIPPING
	// The partner's proof runs: -ArenaPartnerFoeHp=N multiplies every enemy's hp (1-10) so an autopilot fight lasts
	// long enough for several 20 s budget windows. With -ArenaPartner only.
	int32 FoeHpScale = 1;
	if (bPartner && FParse::Value(FCommandLine::Get(), TEXT("ArenaPartnerFoeHp="), FoeHpScale) && FoeHpScale > 1)
	{
		FoeHpScale = FMath::Min(FoeHpScale, 10);
		for (size_t I = 0; I < Def.enemyCount; ++I)
		{
			Def.enemies[I].hp *= FoeHpScale;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaPartnerFoeHp: every enemy's hp x%d for this run."), FoeHpScale);
	}
#endif
	Fight.begin(Def, Fit.ring, Fit.playerStart, Fit.playerFacing, Seed);
	GiveSpecials(); // begin clears them
	SessionLead = SessionLeadOf(Setup.Lead);
	StartLead = SessionLead;
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
	if (!bBegun || bLeadRefused)
	{
		return;
	}
	if (!bHold)
	{
		Now += DeltaSeconds;
	}
	HandoffFrameDt = DeltaSeconds;
	// A view holding the fight's clock holds the lead's own clips too (its locomotion runs on the frame's time), so the
	// views after it show the pose at their fight time.
	if (USkeletalMeshComponent* LeadSkin = PlayerFigure ? PlayerFigure->GetLeadSkin() : nullptr)
	{
		LeadSkin->GlobalAnimRateScale = bHold ? 0.f : 1.f;
	}
	// The pose drawn last frame at last frame's placement, before anything moves.
	Glide.Sample(PlayerFigure ? PlayerFigure->GetLeadSkin() : nullptr, DeltaSeconds);
	// A special move: Core freezes itself and keeps stepping (its own clock only); no input but the skip is read while it plays, and
	// the shots' tail past Core's cut back holds the fight until the camera is back (FTArenaRunnerSpecial.cpp).
	const bool bCoreHold = bHold || (bSpecialOn && !Fight.frozen());
	if (!bSpecialOn && !Fight.frozen())
	{
		PollInput(DeltaSeconds);
		TickAim(DeltaSeconds);
	}
	else if (!bHold)
	{
		PollSpecialSkip(); // a press while it plays: the skip (FTArenaRunnerSpecial.cpp)
	}

	const float HeldBefore = Fight.heldTime();
	for (float& Paused : PausedThisFrame)
	{
		Paused = 0.f;
	}
	if (!Fight.over() && !bCoreHold)
	{
		Accumulator = FMath::Min(Accumulator + static_cast<double>(DeltaSeconds), static_cast<double>(StepSeconds * MaxStepsPerFrame));
		while (Accumulator >= StepSeconds && !Fight.over())
		{
			StepFight();
			Accumulator -= StepSeconds;
		}
	}
	HeldDelta = FMath::Max(0.f, Fight.heldTime() - HeldBefore);
	// Over with a partner shot's or cast's impact still waiting: the held clock stands, so its wait runs on the frame's.
	if (Fight.over() && !bHold && PartnerImpactDue >= 0.0)
	{
		PartnerImpactDue -= static_cast<double>(DeltaSeconds);
		PlayWaitingPartnerImpact(static_cast<double>(Fight.heldTime()));
	}
	// Frozen, the fight's clock stands while the accumulator cycles: drawn at the last step, nothing saws.
	PlaceFigures(Fight.over() || Fight.frozen() ? 1.f : static_cast<float>(Accumulator / StepSeconds));
	if (bPartner && !Fight.frozen()) // no lead switch mid-special (Core refuses it)
	{
		CheckSwapInput(); // the figures as drawn this frame
		TickHandoff();
		TrackSwitchPartner();
	}
	// The legs' speed as the figure hands it to the locomotion (none while dodging or down).
	{
		const ftcore::ArenaAction Act = Fight.output().player.action;
		Glide.Drawn(Fight.output().player, Now, Act == ftcore::ArenaAction::Dodge || Act == ftcore::ArenaAction::Downed ? 0.f : PlayerStepSpeed);
	}
	SteerCamera(DeltaSeconds);
	ApplyAimCamera();
	TickSpecial(bHold ? 0.f : DeltaSeconds);
	TickSpecialCost(DeltaSeconds, bHold);
	TickImpact(bCoreHold || bSpecialOn ? 0.f : DeltaSeconds);

	if (Fight.over())
	{
		if (!bResultLogged)
		{
			Finish();
		}
		else if (bQuitOnFinish && !bShots && QuitAfterFinish > 0.f && GetResultAge() >= QuitAfterFinish)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaQuitAfter: the run ends %.2f s after the result."), GetResultAge());
			bQuitOnFinish = false; // once: the exit lands a frame or more later
			FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(QuitStatus), TEXT("Arena"));
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

#if !UE_BUILD_SHIPPING
	// -ArenaStandOff: the lead never attacks, so the fight would last to the watchdog; the run ends after its stills.
	if ((bQuitOnFinish || bShots) && StandOffAt > 0.f && Fight.time() > StandOffRunSeconds)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaStandOff run ends at %.2f s of fight time."), Fight.time());
		LogResult();
		if (bPartner)
		{
			LogPartnerChecks(false, false); // the per-step partner lines (in play, on screen, clear of the lead) for the run so far
		}
		bQuitOnFinish = false;
		StandOffAt = 0.f; // once: the exit lands a frame or more later
		FPlatformMisc::RequestExitWithStatus(false, 0, TEXT("Arena"));
		return;
	}
#endif
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
	EndSpecial(TEXT("the fight ended"));
	if (bArenaSpecials && bBegun)
	{
		Fight.storeSpecialGauges(SpecialGauges); // kept for the next fight
	}
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
	AllyWeapons.Reset(); // and these the ally's
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
		PlayerFigure->SetCameraLag(CameraLag, bCloseShape ? kArenaCloseLagMost : 0.f); // the preset's boom lag (classic: the boom's own)
		// The kit's weapon on the lead's body: its strike clips, props and effects.
		if (Lead)
		{
			LeadWeapons.Begin(PlayerFigure, *Lead, Def.fighter);
			PlayerFigure->SetJumpTiming(Def.fighter.jumpStartup, Def.fighter.jumpAir, Def.fighter.jumpLandLock);
			LoadSpecialClip(); // its special's clip, for the body it wears
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
	FTArenaDevTiming::ApplyClips(*Skin.Clips, TablePath, I); // -ArenaMoveTiming, a capture switch; nothing without it
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
	Skin.Run = Skin.Walk >= 0 ? Find("run") : -1;
	Skin.Run = Skin.Run >= 0 && (*Skin.Clips)[static_cast<size_t>(Skin.Run)].speed > (*Skin.Clips)[static_cast<size_t>(Skin.Walk)].speed ? Skin.Run : -1;
	Skin.Trot = Skin.Walk >= 0 ? Find("trot") : -1;
	if (Skin.Trot >= 0)
	{
		const double TrotSpeed = (*Skin.Clips)[static_cast<size_t>(Skin.Trot)].speed;
		const bool bBetween = TrotSpeed > (*Skin.Clips)[static_cast<size_t>(Skin.Walk)].speed &&
			(Skin.Run < 0 || TrotSpeed < (*Skin.Clips)[static_cast<size_t>(Skin.Run)].speed);
		Skin.Trot = bBetween ? Skin.Trot : -1;
	}
	// A gait the table names but the skin can't play (no walk, its asset missing, or its speed out of order) says so.
	const std::pair<const char*, int32> Bound[] = {{"walk", Skin.Walk}, {"trot", Skin.Trot}, {"run", Skin.Run}};
	for (const std::pair<const char*, int32>& B : Bound)
	{
		if (B.second < 0 && Skin.Clips->find(B.first) >= 0)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: enemy %d's %hs clip is not played (no walk, no asset, or its speed_mps out of order): %s."),
				I, B.first, *TablePath);
		}
	}
	{
		// The gaits, slowest first, and the band they play in (from the speed the walk starts at; between that and the
		// stop speed, a walk settling to a halt, the walk holds the band's lowest rate).
		double Speeds[ftcore::kMaxGaits] = {};
		int32 Count = 0;
		for (const int32 Gait : {Skin.Walk, Skin.Trot, Skin.Run})
		{
			if (Gait >= 0)
			{
				Skin.GaitClip[Count] = Gait;
				Speeds[Count++] = (*Skin.Clips)[static_cast<size_t>(Gait)].speed;
			}
		}
		Skin.Gaits = ftcore::planGaitBand(Speeds, Count, kArenaWalkStart);
		if (Count > 1)
		{
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: enemy %d gaits: %d, rates %.2f-%.2f%s."), I, Skin.Gaits.count,
				Skin.Gaits.minRate, Skin.Gaits.maxRate, Skin.Gaits.covers ? TEXT("") : TEXT(" (too far apart: the feet slide near a switch)"));
		}
	}
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
	float GaitRate = 0.f; // the walk's or run's playing rate while it walks, else 0
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
		// The gait by speed (walk, trot, run: those bound), each inside the skin's rate band, so the feet don't slide.
		const int32 Was = GaitNow[I];
		GaitNow[I] = bWalking[I] ? ftcore::pickGait(Skin.Gaits, bWas ? Was : 0, WalkSpeed[I]) : 0;
		const int32 Gait = Skin.GaitClip[FMath::Clamp(GaitNow[I], 0, ftcore::kMaxGaits - 1)];
		bRunning[I] = bWalking[I] && Skin.Run >= 0 && Gait == Skin.Run;
		if (bWalking[I] && Gait >= 0)
		{
			const float Rate = static_cast<float>(ftcore::gaitRate(Skin.Gaits, GaitNow[I], WalkSpeed[I]));
			GaitRate = Rate;
			if (bWas && Was != GaitNow[I])
			{
				// The gait changes: carry the stride's phase across, as a share of each cycle, so the feet don't jump.
				const int32 FromClip = Skin.GaitClip[FMath::Clamp(Was, 0, ftcore::kMaxGaits - 1)];
				const double FromLen = FromClip >= 0 ? static_cast<double>(Skin.Sequences[FromClip]->GetPlayLength()) : 0.0;
				const double ToLen = static_cast<double>(Skin.Sequences[Gait]->GetPlayLength());
				WalkPhase[I] = FromLen > 0.0 ? FMath::Fmod(WalkPhase[I], FromLen) / FromLen * ToLen : 0.0;
				const TCHAR* Word = Gait == Skin.Run ? TEXT("runs") : (Gait == Skin.Trot ? TEXT("trots") : TEXT("walks again"));
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: enemy %d %s at %.2f m/s (rate %.2f)."), Now, I, Word, WalkSpeed[I], Rate);
			}
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
		// lead, so the blow it dodges is seen to miss. A sidestep (no room behind it at the ring's edge) plays the stepping
		// turn to that side when the skin has one. No walk clip: the idle.
		if (StepBackWay[I] != 0 && Skin.TurnL >= 0 && Skin.TurnR >= 0 && !bStone)
		{
			Clip = StepBackWay[I] == 1 ? Skin.TurnL : Skin.TurnR;
			Time = static_cast<double>(Out.stateTime) + static_cast<double>(Alpha) * StepSeconds;
			bPhased = true;
		}
		else if (Skin.Walk >= 0 && !bStone)
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
			const int32 Gait = Skin.GaitClip[FMath::Clamp(GaitNow[I], 0, ftcore::kMaxGaits - 1)];
			Clip = Gait >= 0 ? Gait : Skin.Walk;
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
	SkinAt[I] = At;
	SkinRate[I] = MoveRate > 0.f ? MoveRate : ((Clip == Skin.Walk || Clip == Skin.Trot || Clip == Skin.Run) && GaitRate > 0.f ? GaitRate : 1.f);
	Figure->PlayClip(Sequence, At, bLoop, ClipDelta, bHoldRoot, static_cast<float>(Blend), Row.additiveIn);
}

UFTArenaRunner::FEnemyClipNow UFTArenaRunner::GetEnemyClipNow(int32 I) const
{
	FEnemyClipNow Now_;
	if (I < 0 || I >= MaxEnemies)
	{
		return Now_;
	}
	const FFTArenaSkin& Skin = Skins[I];
	Now_.Sequence = Skin.IsLoaded() && Skin.Sequences.IsValidIndex(SkinClip[I]) ? Skin.Sequences[SkinClip[I]].Get() : nullptr;
	Now_.At = SkinAt[I];
	Now_.Rate = SkinRate[I];
	Now_.Ground = WalkSpeed[I];
	Now_.bWalk = bWalking[I];
	Now_.bRun = bRunning[I];
	return Now_;
}

void UFTArenaRunner::PutBodiesAtStart()
{
	Bodies = ftcore::ArenaBodies();
	Bodies.player = Fight.playerStart();
	// Each fight starts on the right shoulder (reset again below with the rest of the lens), before the partner's close
	// start reads it: a restart after a left-shoulder fight would place the partner on the wrong flank otherwise.
	CloseShoulder = CloseShoulderWant = 1.f;
	if (bAllyOn)
	{
		Bodies.ally = bCloseShape && bPartner ? PartnerCloseStart(Bodies.player, Fight.playerStartFacing())
			: AllyHome(Bodies.player, Fight.playerStartFacing());
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
		bRunning[I] = false;
		GaitNow[I] = 0;
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
	LastHitFrom = FVector2D::ZeroVector;
	LastHitAt = -1e9;
	// The lock camera's eased state starts each fight at rest: no partner turn, no low lens.
	PartnerYawNow = PartnerYawGoal = 0.f;
	CloseMelee = CloseTall = 0.f;
	CloseRadiusNow = CloseContact = CloseContactWant = ContactYawNow = CloseCutTall = CloseContactLow = StandOffPitchNow = 0.f;
	ClearSideNow = ClearPivotNow = 0.f;
	ClearHeld = ftcore::ArenaCloseClear();
	CloseWallCount = 0;
	CloseWallAge = 1e9f;
	bStandOffWeak = false;
	bContactPicked = false;
	CloseShoulder = CloseShoulderWant = 1.f;
	CloseShoulderFor = CloseTargetNow = -1;
	CloseNextPickAt = -1e9f;
	CloseDistanceNow = -1.f;
	CloseHeightNow = 0.f;
#if !UE_BUILD_SHIPPING
	CloseSwaps = CloseYawFrames = CloseYawOn = 0;
	CloseWallFrames = CloseWallLow = CloseWallHeld = 0;
	CloseContactFrames = CloseContactSwaps = 0;
	CloseClearFrames = CloseClearSeen = 0;
	CloseClearSideMost = CloseClearPivotMost = 0.f;
	CloseContactYawMost = 0.f;
	CloseTallFrames = CloseTallMateFrames = CloseTallMateIn = 0;
	CloseTallHeadMost = CloseTallPitchMost = 0.f;
	CloseTallWaistLeast = 1.f;
	CloseYawSum = CloseYawMost = 0.f;
	CloseRangedFrames = CloseRangedWide = 0;
	CloseRangedYawMost = 0.f;
	MateFrames = MateIn = MateBehind = MateLeft = MateRight = MateOffY = 0;
	MateOutAhead = MateOutRight = 0.f;
	MateCastSteps = MateCastCoreOff = 0;
	MateCastCoreWorst = 0.f;
	MateDrawFrames = MateDrawOff = MateDrawTurning = 0;
	MateDrawWorst = 0.f;
	MateHeadFrames = MateHeadOff = 0;
	MateHeadSum = MateHeadWorst = 0.f;
	MateStartsWalk = MateStartsPaused = MateStartsStill = MateStartsWide = 0;
	MateStandSum = 0.f;
	MateWalkEnd = -1.0;
	bMateWasStringing = false;
	RollAtNext = 0;
#endif
	AllyDrawYawAt = -1.0;
	LockLow = 0.f;
	LowSideNow = ftcore::kArenaLow.side;
	bLowRig = false;
	bShortArmLogged = false;
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
	PartyScreen = ftcore::UiPartyScreen{};
}

void UFTArenaRunner::PollInput(float DeltaSeconds)
{
	APlayerController* PC = GetPC();
	// Paused or just resumed: every press state clears, so a key let go meanwhile fires nothing later. The autopilot
	// reads no keys. Out of focus only the held attack clears (below); the toggled latches stay.
	if (!PC || bAutoPlay || bIdle || UFTPauseSubsystem::IsBlocking(this) || UFTPauseSubsystem::IsJustResumed(this))
	{
		// The HUD's Guard press count: the key's state taken as it is, so a key pressed in the pause and held after it
		// counts no press.
		bGuardWasDown = PC && FTInputBindings::IsDown(*PC, ftcore::InputAction::Guard);
		bGuardHeld = false;
		GuardInput.reset();
		bInterceptHeld = false;
		InterceptInput.reset();
		bAimHeld = false;
		AimInput.reset();
		AttackHold.reset();
		Stick = ftcore::Vec2();
		// Input taken away: shut, no pick; under the autopilot the -ArenaSwitchAt probe may still drive it.
		StepPartyScreen(ftcore::UiPartyInput{}, DeltaSeconds);
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
		bAimHeld && Fight.def().fighter.weapon == ftcore::ArenaWeapon::Guns); // the controlled lead's kit
	bPressLight |= Press.light;
	bPressHeavy |= Press.heavy;
	// The HUD's press counts (GetPromptPresses), by ftcore::UiPrompt: 0 HEAL, 1 GIFT HEAL, 2 GIFT GUARD, 3 PARTNER, 4 ATTACK,
	// 5 GUARD, 6 DODGE, 7 JUMP.
	if (FTInputBindings::WasJustPressed(*PC, InputAction::Light))
	{
		++PromptPresses[4];
	}
	const bool bDodgeNow = FTInputBindings::WasJustPressed(*PC, InputAction::Roll);
	bPressDodge |= bDodgeNow;
	PromptPresses[6] += bDodgeNow ? 1u : 0u;
	// The jump: its key does nothing in a fight unless the jump is bound (-ArenaJump, until the user has tried it).
	if (JumpBound())
	{
		const bool bJumpNow = FTInputBindings::WasJustPressed(*PC, InputAction::Jump);
		PromptPresses[7] += bJumpNow ? 1u : 0u;
		bPressJump |= bJumpNow;
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
	const bool bPairNow = Pressed(InputAction::Count);
	bPressPair |= bPairNow;
	PromptPresses[3] += bPairNow ? 1u : 0u;
	// B (keyboard only until the pad's layout is ruled): the partner's order, STAY CLOSE and SPREAD in turn.
	if (bPartner && Pressed(InputAction::PartnerOrder))
	{
		bPartnerSpread = !bPartnerSpread;
		++PartnerOrders;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the partner's order: %s."), Fight.time(), bPartnerSpread ? TEXT("SPREAD") : TEXT("STAY CLOSE"));
	}
	// Guard: held, or pressed once to toggle (a refusal is a held Guard, so the toggle saves holding it).
	const bool bGuardDown = FTInputBindings::IsDown(*PC, InputAction::Guard);
	PromptPresses[5] += bGuardDown && !bGuardWasDown ? 1u : 0u;
	bGuardWasDown = bGuardDown;
	bGuardHeld = GuardInput.tick(bGuardDown, FTInputBindings::Toggles(World, InputAction::Guard));
	// Three presses, each its own key with no modifier: the lead's own heal (Gift's row), and the third slot's Gift
	// Heal and Gift Guard (no pad button yet: those two are keyboard only until the pad's layout is ruled).
	const bool bHealNow = Pressed(InputAction::Gift);
	bPressHeal |= bHealNow;
	PromptPresses[0] += bHealNow ? 1u : 0u;
	if (Pressed(InputAction::GiftHeal))
	{
		GiftPick = ftcore::Gift::Heal;
		++PromptPresses[1];
	}
	else if (Pressed(InputAction::GiftGuard))
	{
		GiftPick = ftcore::Gift::Guard;
		++PromptPresses[2];
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
	// The special: its key, and on a pad with none bound, the shoulders' chord (route "shoulders_chord": the pad's Guard
	// and Read buttons, the second within kInputShoulderChord s of the first). Core refuses it when not ready.
	if (Pressed(InputAction::Special))
	{
		RequestSpecial();
	}
	{
		const FKey& PadSpecial = FTInputBindings::Key(InputAction::Special, ftcore::InputDevice::Pad);
		const FKey& PadGuard = FTInputBindings::Key(InputAction::Guard, ftcore::InputDevice::Pad);
		const FKey& PadRead = FTInputBindings::Key(InputAction::Read, ftcore::InputDevice::Pad);
		if (!PadSpecial.IsValid() && PadGuard.IsValid() && PadRead.IsValid())
		{
			const double Clock = World ? World->GetRealTimeSeconds() : 0.0;
			const bool bGuardNow = PC->WasInputKeyJustPressed(PadGuard);
			const bool bReadNow = PC->WasInputKeyJustPressed(PadRead);
			const double Chord = static_cast<double>(ftcore::kInputShoulderChord);
			if ((bGuardNow && (bReadNow || Clock - PadReadPressedAt <= Chord)) || (bReadNow && Clock - PadGuardPressedAt <= Chord))
			{
				RequestSpecial();
				PadGuardPressedAt = PadReadPressedAt = -1.0e9;
			}
			else
			{
				PadGuardPressedAt = bGuardNow ? Clock : PadGuardPressedAt;
				PadReadPressedAt = bReadNow ? Clock : PadReadPressedAt;
			}
		}
	}
	// A called-blow boss's Intercept (its Guard is the guard above), held or toggled.
	// Fight only: the pad's stick click is Crouch outside a fight (one button, two places).
	bInterceptHeld = InterceptInput.tick(ftcore::inputActionLive(InputAction::Intercept, true) && FTInputBindings::IsDown(*PC, InputAction::Intercept),
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
	// Through a close shoulder swap a held stick keeps the basis it had until it is let go, and held past the blend's
	// end eases onto the view over 0.25 s (ftcore::arenaCloseStickBasis), so a held direction never turns under the
	// player in one frame; with no swap behind it the basis is the view's yaw exactly (classic never swaps). The close
	// contact swing's blend counts as a swap.
	const bool bSwapping = bCloseShape && (ftcore::arenaCloseSwapping(CloseShoulder, CloseShoulderWant)
		|| ftcore::arenaCloseContactBlending(CloseContact, CloseContactWant));
	StickBasis = ftcore::arenaCloseStickBasis(ftcore::kArenaClose, StickBasis, ViewYaw(), bSwapping, Forward != 0.f || Right != 0.f, DeltaSeconds);
	Stick = ftcore::arenaStickToWorld(StickBasis.yaw, Forward, Right);

	// The view: the mouse and the right stick turn the camera unless something is locked; locked, a flick of
	// the right stick switches the target.
	const float LookX = PC->GetInputAnalogKeyState(EKeys::Gamepad_RightX);
	const float LookY = PC->GetInputAnalogKeyState(EKeys::Gamepad_RightY);
	float MouseX = 0.f;
	float MouseY = 0.f;
	PC->GetInputMouseDelta(MouseX, MouseY);
	if (FMath::Abs(LookX) > 0.2f || FMath::Abs(LookY) > 0.2f || MouseX != 0.f || MouseY != 0.f)
	{
		LastLookInputAt = Fight.time(); // the partner's soft turn stays off a while after (SteerCamera)
	}

	// The party screen (the new UI, FT::NewUiEnabled): Party's key, or on the pad the map key (Party's route there),
	// opens it; a vertical flick of the right stick or the mouse moves its cursor, and letting go confirms
	// (ftcore::uiPartyScreenStep). While it stands the look and the target flick are held; the left stick still moves
	// the lead. Tab (SwitchLead, the keyboard's only) asks the same switch in one press. Under -FTOldUi neither is
	// read, as before the party screen.
	if (bPartyScreenOn)
	{
		ftcore::UiPartyInput PartyIn;
		PartyIn.live = !FTInputBindings::LostFocus() && !Fight.over();
		PartyIn.partyDown = FTInputBindings::IsDown(*PC, InputAction::Party);
		const FKey& PadMap = FTInputBindings::Key(InputAction::Map, ftcore::InputDevice::Pad);
		PartyIn.mapDown = PadMap.IsValid() && PC->IsInputKeyDown(PadMap);
		PartyIn.toggle = FTInputBindings::PartyToggles(World);
		PartyIn.stickY = LookY;
		PartyIn.mouseY = MouseY;
		StepPartyScreen(PartyIn, DeltaSeconds);
	}
	{
		const FKey& Tab = FTInputBindings::Key(InputAction::SwitchLead, ftcore::InputDevice::Keys);
		if (bPartyScreenOn && !PartyScreen.open && Tab.IsValid() && PC->WasInputKeyJustPressed(Tab))
		{
			AskLeadSwitch(TEXT("SwitchLead"));
		}
	}
	if (PartyScreen.open)
	{
		bStickFlicked = true; // no target flick from a stick or a mouse still pushed as the screen shuts
		bMouseFlicked = true;
		return;
	}

	if (Fight.lock() != ftcore::kArenaNobody)
	{
		// The mouse: a sharp sideways move switches, and it re-arms once the mouse slows.
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
	LoneReleases = LoneGiftPresses = LoneGiftsRefused = LoneBreaches = LonePartnerCalls = LonePartnerEnds = 0;
	SessionTakeovers = SessionRises = 0;
	bSessionDefeated = false;
	PartnerPrevTime = PartnerWinStart = static_cast<double>(Fight.time());
	PartnerWinLead = PartnerWinAlly = 0.0;
	PartnerWinDamage0 = PartnerWinLeadDamage0 = PartnerWinBoss0 = PartnerWinAimed0 = 0;
	PartnerWinLeadFalls = PartnerWinAllyFalls = PartnerWinTakeovers = PartnerWinRises = 0;
	PartnerWindows = PartnerWindowsHeld = 0;
	PartnerWorstOver = -1e9;
	PartnerLeadFalls = PartnerAllyFalls = PartnerTakeovers = PartnerRises = 0;
	PartnerSessionTookOver = PartnerLeadMismatch = 0;
	PartnerHeldLeaks = PartnerBlowsRuled = PartnerHitEvents = PartnerJoins = 0;
	PartnerJoinAsks = PartnerJoinsLanded = PartnerJoinsRefused = PartnerJoinDisagree = 0;
	PartnerOnSteps = PartnerStationedSteps = PartnerCameraSteps = 0;
	PartnerSeenSteps = PartnerFrustumSteps = PartnerActions = PartnerClearSteps = PartnerLeadClearSteps = 0;
	PartnerClearFlatSteps = PartnerLeadClearFlatSteps = 0;
	PartnerNearSteps = PartnerNearOkSteps = 0;
	PartnerClipsStarted = PartnerClipsWhole = 0;
	LeadWeapons.ResetAimSummary();
	AllyWeapons.ResetAimSummary();
	PartnerPlaySeconds = 0.f;
	PartnerEngagedSeconds = 0.f;
	LeadAttackAt = -1e9f;
	PartnerEngagedActions = 0;
	PartnerActEngaged = false;
#if !UE_BUILD_SHIPPING
	StandOffStrikeNext = 1.f;
	StandOffStrikePresses = 0;
#endif
	PartnerStillFor = PartnerStillLongest = 0.f;
	bPartnerStillAt = false;
	PartnerLastAct = PartnerIdleWhy = 0xFF;
	PartnerActFrom = PartnerActTo = -1.f;
	PartnerActDone = false;
	PartnerSwapsBase = Fight.stats().partnerSwaps; // the brain's swaps start over with a restart (its begin)
	PartnerJoinedAt = -1.0;
	bSwitchAsked = bSwitchAskOpen = bSwitchAskedThisStep = bSwitchBackAsked = false;
	SwitchAsks = SwitchesSeen = SwitchesDone = SwitchesRefused = SwitchAsksRefused = SwitchAsksSame = 0;
	bSwapCheckArmed = bSwapCheckPlaced = false;
	SwapMoveSteps = SwapSteps = SwapChecks = SwapChecksPassed = 0;
	SwapWorstNew = 1e9;
	SwapWorstOld = 0.0;
	SwapWorstAlign = 1.0;
	SwapSwitchChecks = 0;
	bLoneGiftDone = false;
	LoneGift = {};
	PartnerLoadTime = -1e9;
	LiftTime = LastJoinAt = KiteLockUntil = -1e9;
	LiftMultiplier = 1.f;
	if (Session)
	{
		ftcore::FightSessionDef SessionDef;
		// The lead switch's cooldown from Data/Battle/lead_switch.json (the default, 1 s, if it is refused).
		{
			ftcore::LeadSwitchTuning SwitchTuning;
			TArray<uint8> Bytes;
			std::string Error;
			const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/lead_switch.json");
			if (!(FFileHelper::LoadFileToArray(Bytes, *File) && ftcore::importLeadSwitchTuning(FT::BytesAsText(Bytes), SwitchTuning, &Error)))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Battle/lead_switch.json; the lead switch keeps its defaults.\n%s"), *FT::ToFString(Error));
				SwitchTuning = ftcore::LeadSwitchTuning();
			}
			SessionDef.leadSwitchCooldownSeconds = SwitchTuning.cooldownSeconds;
		}
		// Only the controlled lead stands in this ring (no partner body): the session's fall rules then make its 0 the
		// fight's loss, as before them; no takeover to a body that is not there, no stand-up after 30 s. With the
		// partner's body (-ArenaPartner, not Lone) both leads are in it: a fall with the other up is a takeover.
		SessionDef.inFight = {SessionLead == ftcore::kLeadA, SessionLead == ftcore::kLeadB};
		if (PartnerBody())
		{
			SessionDef.inFight = {true, true};
		}
		// Which lead the player may drive: the kits' own data flag (ftcore::arenaPlayable), the one the fight reads, so
		// the session and the fight make one rule (no takeover to, no switch to, a party member only). At a begin or a
		// restart the session's lead (StartLead) holds Def.fighter and the other Def.partnerKit.
		SessionDef.playable[SessionLead] = ftcore::arenaPlayable(Def.fighter);
		SessionDef.playable[OtherLead()] = ftcore::arenaPlayable(Def.partnerKit);
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
		SessionDef.thirdSlot = static_cast<ftcore::SlotState>(ThirdSlot);
		if (!Session->Begin(SessionDef, static_cast<double>(SessionHeld)) && !Session->GetLastCheck().playable)
		{
			// Core's refusal: no lead in the fight may be driven. The fight stops here (TickFight runs nothing more).
			UE_CLOG(!bLeadRefused, LogForgottenThird, Error, TEXT("Arena: the fight is refused: no lead in it is playable; it stops."));
			bLeadRefused = true;
		}
		if (PartnerBody())
		{
			// A begin keeps the last fight's controlled lead, so ask for this one's. That is a request: the first tick's
			// close resolves it (activeLead moves there, reported as tick.switched), before anything reads the lead.
			Session->Get().setActiveLead(SessionLead);
		}
		// Per-lead Load and heal charges (a partner run): the begin stores 0 for every value, so both leads take the
		// fight's charges, and each lead the session does not keep its Load (the controlled lead's gauge, a probe's
		// fixed partner Load); the kept lead's is the kept gauge, 0 at the begin. A retry is a begin (Restart), so it
		// seeds the same way: the arena runner takes no session checkpoint, so it has no host part to write.
		bPerLead = bPartner;
		FedLead = SessionLead;
		OwnLoad[0] = OwnLoad[1] = OwnHeals[0] = OwnHeals[1] = -1;
		bOwnDeparted[0] = bOwnDeparted[1] = false;
		LeadMoves = LeadMovesChecked = LeadReturnsChecked = LeadValueMisses = 0;
		bLeadStepOpen = false; // the begin stored every value: the next step opens with its own feed
		LeadLoadSpends = LeadHealSpends = LateSetsSeen = 0;
		if (bPerLead)
		{
			ftcore::FightSession& S = Session->Get();
			const uint8 Other = OtherLead();
			S.setLeadHeals(SessionLead, Fight.healCharges());
			S.setLeadHeals(Other, Fight.healCharges());
			if (!S.keepsLoad(SessionLead))
			{
				S.setLoad(SessionLead, Fight.load());
			}
			if (!S.keepsLoad(Other))
			{
				S.setLoad(Other, FMath::Max(PartnerLoad, 0));
			}
			OwnLoad[Other] = S.leadLoad(Other);
			OwnHeals[Other] = S.leadHeals(Other);
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

ftcore::SlotState UFTArenaRunner::GetThirdSlot() const
{
	return Session ? Session->Get().thirdSlot() : static_cast<ftcore::SlotState>(ThirdSlot);
}

ftcore::GiftError UFTArenaRunner::GetThirdSlotRefusal() const
{
	return Session ? Session->Get().thirdSlotRefusal() : ftcore::GiftError::None;
}

ftcore::UiPartyWhy UFTArenaRunner::GetLeadSwitchReason(const uint8 Lead) const
{
	if (!Session)
	{
		return ftcore::UiPartyWhy::Unknown;
	}
	// THE one host-side condition: a lead with no body in this ring cannot be controlled (NOT HERE). The rest is
	// ftcore::FightSession::leadSwitchReason(Lead), the close's own predicate, as the row's word.
	const bool bBody = Lead == SessionLead || PartnerBody();
	return ftcore::uiPartyLeadWhy(Session->Get().leadSwitchReason(Lead), bBody);
}

void UFTArenaRunner::StepPartyScreen(ftcore::UiPartyInput In, const float DeltaSeconds)
{
	In.partnerHere = ftcore::uiPartyPartnerHere(GetLeadSwitchReason(OtherLead()));
	In.lead = static_cast<int>(SessionLead); // a lead change under the open screen puts the cursor on Lead
	const bool bProbe = ProbePartyInput(In);
	const ftcore::UiPartyOut Out = ftcore::uiPartyScreenStep(PartyScreen, In, DeltaSeconds);
	if (Out.picked() && Out.pick == ftcore::UiPartyRow::Partner)
	{
		bSwitchAsked = bSwitchAsked || bProbe;
		AskLeadSwitch(bProbe ? TEXT("-ArenaSwitchAt, on the party screen") : TEXT("the party screen"));
	}
}

bool UFTArenaRunner::ProbePartyInput(ftcore::UiPartyInput& In) const
{
	if (SwitchAt < 0.f || !bPartyScreenOn || !PartnerBody() || Fight.over())
	{
		return false;
	}
	// Core's script (live even where a capture run's window has no focus): keys up, hold, flick, let go, reopen.
	return ftcore::uiPartyProbeInput(Fight.time() - SwitchAt, In);
}

bool UFTArenaRunner::AskLeadSwitch(const TCHAR* From)
{
	const uint8 Partner = OtherLead();
	// One ask per step: the session resolves one request at the close, so a second press in the same step (Tab and
	// the screen's release in one frame) sends, counts and calls nothing.
	if (bSwitchAskedThisStep)
	{
		UE_LOG(LogForgottenThird, Verbose, TEXT("Arena: %.2f: a second lead switch asked this step (%s); one ask per step."), Fight.time(), From);
		return false;
	}
	bSwitchAskedThisStep = true;
	// No partner body in this fight (ordinary play, a Lone Carry): nothing to switch to, so nothing is sent; the call
	// reads SWITCH: NOT HERE (the party screen's row never takes the cursor then, so only Tab gets here).
	if (!PartnerBody())
	{
		++SwitchRefusedSerial;
		SwitchRefusedWhy = ftcore::UiPartyWhy::NotHere;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: a lead switch asked (%s); not sent: lead %d has no body in this fight."),
			Fight.time(), From, static_cast<int32>(Partner));
		return false;
	}
	// A party member only (the partner kit not playable, ftcore::ArenaFighter::playable): never sent to the session.
	// The fight is asked so it names and counts the refusal (ArenaSwitchRefusal::NotPlayable; nothing else changes);
	// the call reads NOT HERE, as the session's own word for the third slot.
	if (!Fight.def().partnerKit.playable)
	{
		Fight.switchLeads();
		++SwitchRefusedSerial;
		SwitchRefusedWhy = ftcore::UiPartyWhy::NotHere;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: a lead switch asked (%s); not sent: the partner is a party member only, not playable (%d refused)."),
			Fight.time(), From, Fight.stats().leadSwitchesNotPlayable);
		return false;
	}
	// The session takes the request (the step's close resolves it: tick.switched swaps the bodies, or
	// tick.switchRefused with its word). Not taken only for a lead not in its fight: the call is raised here.
	if (!Session || !Session->Get().setActiveLead(Partner))
	{
		++SwitchRefusedSerial;
		SwitchRefusedWhy = GetLeadSwitchReason(Partner);
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %.2f: a lead switch asked (%s); not taken: lead %d is not in the session's fight."),
			Fight.time(), From, static_cast<int32>(Partner));
		return false;
	}
	++SwitchAsks;
	bSwitchAskOpen = true;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: a lead switch to lead %d asked (%s); the step's close resolves it."), Fight.time(),
		static_cast<int32>(Partner), From);
	return true;
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
		SpendLeadLoadNow(Moved);
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

ftcore::PressReason UFTArenaRunner::GetGiftReason(const ftcore::Gift G) const
{
	if (!Session)
	{
		return ftcore::PressReason::Absent;
	}
	const float Health = static_cast<float>(Fight.hp()) / static_cast<float>(FMath::Max(Fight.def().fighter.hp, 1));
	return Session->Get().giftReason(G, SessionLead, Health);
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
	FeedLeadHealths(); // with the partner's body: both leads' health before any press
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
		// The Count press is the lead calling the partner in: accepted while Lone, it ends the Lone Carry (PartnerCalled),
		// here and nowhere else. A refused press leaves it Lone.
		const bool bWasLone = Session->Get().lone();
		const ftcore::SharedReply Reply = Session->Press(EFTSharedAction::Count, SessionLead);
		if (bWasLone && Reply.accepted)
		{
			++LonePartnerCalls;
			LonePartnerEnds += Session->Get().endLone(ftcore::LoneEnd::PartnerCalled) ? 1 : 0;
			MirrorLone();
			// The partner held out of a fight begun Lone comes in on this call (and on no other end): the session's
			// join resolves at the step's close and FeedSession brings it into the fight on tick.joined, at
			// PartnerJoinHealth() (full: no carried-health source yet).
			if (bPartnerHeldOut && Session->Get().loneEndedBy() == ftcore::LoneEnd::PartnerCalled
				&& Session->Get().joinFight(OtherLead(), PartnerJoinHealth()))
			{
				++PartnerJoinAsks;
			}
		}
	}
	if (SwitchAt >= 0.f && !bSwitchAsked && !bPartyScreenOn && PartnerBody() && Fight.time() >= SwitchAt)
	{
		// The probe's switch without the party screen (-FTOldUi; with it the probe asks on the screen, ProbePartyInput):
		// the same send path, a request the step's close resolves (tick.switched).
		bSwitchAsked = true;
		AskLeadSwitch(TEXT("-ArenaSwitchAt"));
	}
	if (SwitchBackAt >= 0.f && bSwitchAsked && !bSwitchBackAsked && PartnerBody() && SessionLead != StartLead && Fight.time() >= SwitchBackAt)
	{
		// Back to the first lead (either UI: the one send path), for the per-lead Load check's return.
		bSwitchBackAsked = true;
		AskLeadSwitch(TEXT("-ArenaSwitchBackAt"));
	}
	if (In.guard != bSessionGuard)
	{
		bSessionGuard = In.guard;
		Session->Press(In.guard ? EFTSharedAction::RefuseDown : EFTSharedAction::RefuseUp, SessionLead);
	}
}

// The session judges each blow a lead's body lands on an enemy inside the fight's blow hook, after the sub-hosts'
// ruling. The player's (a Hit from kArenaPlayer; the partner's counters are other blow types): judgeHeldBlow on the
// damage they leave; a final outcome scales the blow (x1 alone), a Held one lands at base and its settle's bonus comes
// after a later tick. The partner's body's (a Hit from kArenaAlly, -ArenaPartner): the session's partner path,
// judgeBlow(the other lead), once per blow; a final outcome scales it, a Held one lands at base with no wait. Never
// the controlled lead's held records, Loads or the scripted partner's join (bSessionPartnerDue).
ftcore::ArenaBlowRuling UFTArenaRunner::RuleSessionBlow(void* User, const ftcore::ArenaBlow& Blow)
{
	UFTArenaRunner* Self = static_cast<UFTArenaRunner*>(User);
	ftcore::ArenaBlowRuling Ruling;
	if (Self->SessionInnerHook)
	{
		Ruling = Self->SessionInnerHook(Self->SessionInnerUser, Blow);
	}
	if (Ruling.veto || !Self->Session || Blow.type != ftcore::ArenaEventType::Hit || Blow.target == ftcore::kArenaPlayer)
	{
		return Ruling;
	}
	const bool bPartnerBlow = Blow.source == ftcore::kArenaAlly;
	if (bPartnerBlow ? !Self->PartnerBody() : Blow.source != ftcore::kArenaPlayer)
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
	const ftcore::CountHit Hit = bPartnerBlow ? S.judgeBlow(Self->OtherLead())
		: S.judgeHeldBlow(Self->SessionLead, Blow.target, Base, Blow.move, bCapped ? Scale : 1.f);
	const float Want = Hit.outcome == ftcore::CountOutcome::Held ? Scale : Scale * Hit.multiplier;
	if (bPartnerBlow)
	{
		++Self->PartnerBlowsRuled;
		Self->PartnerJoins += ftcore::sharedOutcome(Hit.outcome) ? 1 : 0;
	}
	else if (Hit.outcome == ftcore::CountOutcome::Held)
	{
		++Self->SessionHeldBlows;
		// The scripted partner (-ArenaSharedCount) joins only with no partner body: that body's own blows join.
		Self->bSessionPartnerDue = Self->bSharedCount && !Self->PartnerBody();
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
	FeedLeadHealths(); // with the partner's body: both leads' health before the partner's press and the tick
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
	// FedLead's Load and heal charges as the fight holds them now (the step's blows already fed them in the hook).
	int32 FedLoad = -1, FedHeals = -1;
	if (bPerLead)
	{
		// The step's feed was at its open and its spends went through spendLeadLoad / spendLeadHeal: no set now. A
		// lead that loses control on this tick leaves with that feed less those spends (the session's own values).
		if (!bLeadStepOpen)
		{
			OpenLeadStep(); // no open before this close (a begin on the step): feed now, before the tick
		}
		FedLoad = FMath::Max(StepOpenLoad - StepLoadSpent, 0);
		FedHeals = FMath::Max(StepOpenHeals - StepHealsSpent, 0);
	}
	// The lead's health every step: the session's fall feed (0 is down) and the gift Heal's NotHurt.
	Session->Get().setLeadHealth(SessionLead, static_cast<float>(Fight.hp()) / static_cast<float>(FMath::Max(Fight.def().fighter.hp, 1)));
	const ftcore::FightTick& Tick = Session->Tick(FMath::Max(0.f, Held - SessionHeld));
	SessionHeld = Held;
	if (bPerLead)
	{
		bLeadStepOpen = false; // the tick closed the step: the next one opens with its own feed
		const int32 Late = Session->Get().lateSets();
		ensureMsgf(Late == 0, TEXT("Arena: the session refused %d late per-lead set(s): a set after a spend on its step."), Late);
		LateSetsSeen = FMath::Max(LateSetsSeen, Late);
		LateSetsRun = FMath::Max(LateSetsRun, Late);
	}
	SessionTakeovers += Tick.tookOver != ftcore::kNoFighter ? 1 : 0;
	if (bPartnerHeldOut && Tick.joined == OtherLead())
	{
		BringPartnerIn(); // the session took the partner in at this close: the fight takes it before its next step
	}
	PartnerJoinsRefused += bPartner && Tick.joinRefused != ftcore::kNoFighter ? 1 : 0;
	if (PartnerBody() && Tick.switched == OtherLead())
	{
		SwitchLeadBodies(); // a switch with both up resolved at this close (the begin's own request names SessionLead)
	}
	if (PartnerBody())
	{
		// The fight swapped the bodies on its TookOver (TakeOverBodies) and the session moves its control the same
		// step: both name the same lead, step by step, until a defeat leaves the session's where it was.
		PartnerSessionTookOver += Tick.tookOver != ftcore::kNoFighter ? 1 : 0;
		// Read after the tick (the switch resolves at its close). A refused switch the host asked (AskLeadSwitch: the
		// player's, the probe's) is the rules' answer; any other (the begin's own request) counts.
		PartnerLeadMismatch += (!Session->Get().defeated() && Session->Get().activeLead() != SessionLead) ? 1 : 0;
		PartnerLeadMismatch += Tick.switchRefused != ftcore::kNoFighter && !bSwitchAskOpen ? 1 : 0;
		SwitchAsksRefused += Tick.switchRefused != ftcore::kNoFighter && bSwitchAskOpen ? 1 : 0;
		// An ask the close resolved as Same (a takeover the same step already gave that lead control) reports neither.
		SwitchAsksSame += bSwitchAskOpen && Tick.switched == ftcore::kNoFighter && Tick.switchRefused == ftcore::kNoFighter ? 1 : 0;
	}
	bSwitchAskOpen = false; // this close resolved any request waiting
	bSwitchAskedThisStep = false;
	SessionRises += Tick.leadsRose != 0 ? 1 : 0;
	bSessionDefeated = bSessionDefeated || Tick.defeated;
	if (Tick.switched != ftcore::kNoFighter)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the session gave control to lead %d."), Fight.time(), static_cast<int32>(Tick.switched));
	}
	// A refused switch is the HUD's SWITCH: <WORD> call; never on a takeover's step (the takeover wins, no message).
	if (Tick.switchRefused != ftcore::kNoFighter && Tick.tookOver == ftcore::kNoFighter)
	{
		++SwitchRefusedSerial;
		SwitchRefusedWhy = ftcore::uiPartyWhyOf(Tick.switchRefusal);
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the session refused the switch to lead %d (refusal %d)."), Fight.time(),
			static_cast<int32>(Tick.switchRefused), static_cast<int32>(Tick.switchRefusal));
		// A party member only (a switch waiting in a restored checkpoint): the fight is asked too, so it names and
		// counts the same refusal (ArenaSwitchRefusal::NotPlayable, leadSwitchesNotPlayable) as an asked one.
		if (Tick.switchRefusal == ftcore::LeadSwitch::NotPlayable && !Fight.def().partnerKit.playable && !Fight.switchLeads()) // never a swap: refused by the same flag
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the fight's refusal %d (%d refused as not playable)."), Fight.time(),
				static_cast<int32>(Fight.switchRefusal()), Fight.stats().leadSwitchesNotPlayable);
		}
	}
	if (Tick.loneCarryBegan)
	{
		LoneBargainAt = static_cast<double>(Fight.time());
		const ftcore::LoneBargain Bargain = Session->Get().loneBargain();
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: the Lone Carry began: Release x%.2f, Load gain x%.2f, cap %d, taken x%.2f."), Bargain.release,
			Bargain.loadGain, Bargain.loadCap, Bargain.damageTaken);
	}
	if (bPerLead)
	{
		ApplyLeadValues(Tick, FedLoad, FedHeals); // before the tick's Load changes and the fight's next step
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
	if (bPerLead)
	{
		// The step's feed was at its open: none again before its tick, so no set follows a spend reported on it.
		// Outside a step (a held blow's bonus ruled after the tick) the feed is the lead's values as they are now.
		if (!bLeadStepOpen)
		{
			FeedLeadValues(); // the lead the fight's gauge is for; the away lead's (a probe's fixed Load among them) stays the session's
		}
		return;
	}
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

void UFTArenaRunner::FeedLeadValues()
{
	if (!Session)
	{
		return;
	}
	ftcore::FightSession& S = Session->Get();
	if (!S.keepsLoad(FedLead))
	{
		S.setLoad(FedLead, Fight.load());
	}
	S.setLeadHeals(FedLead, Fight.healCharges());
}

void UFTArenaRunner::OpenLeadStep()
{
	if (!bPerLead || !Session)
	{
		return;
	}
	FeedLeadValues(); // 1. the controlled lead's gauge and charges as the step opens
	StepOpenLoad = Fight.load();
	StepOpenHeals = Fight.healCharges();
	StepLoadSpent = StepHealsSpent = 0;
	bLeadStepOpen = true;
}

void UFTArenaRunner::SpendLeadLoadNow(int32 Amount)
{
	if (!bLeadStepOpen || Amount <= 0)
	{
		return; // outside the step's window: the next open's feed carries it
	}
	Session->Get().spendLeadLoad(FedLead, Amount);
	StepLoadSpent += Amount;
	++LeadLoadSpends;
}

void UFTArenaRunner::ReportFightSpends(const ftcore::ArenaOutput& Out)
{
	if (!bLeadStepOpen)
	{
		return;
	}
	for (const ftcore::ArenaEvent& E : Out.events)
	{
		if (E.type == ftcore::ArenaEventType::Released && E.source == ftcore::kArenaPlayer)
		{
			SpendLeadLoadNow(E.amount); // the Release's opening spend
		}
	}
	// The charges the fight's own heal used this step (each heal begun spends one).
	const int32 Used = StepOpenHeals - StepHealsSpent - static_cast<int32>(Fight.healCharges());
	for (int32 K = 0; K < Used; ++K)
	{
		if (Session->Get().spendLeadHeal(FedLead))
		{
			++StepHealsSpent;
			++LeadHealSpends;
		}
	}
}

void UFTArenaRunner::ApplyLeadValues(const ftcore::FightTick& Tick, int32 FedLoad, int32 FedHeals)
{
	const ftcore::FightSession& S = Session->Get();
	const bool bReported = Tick.leadLoadNow >= 0 || Tick.leadHealsNow >= 0;
	const uint8 From = FedLead;
	const uint8 To = S.activeLead();
	const bool bToLead = To == ftcore::kLeadA || To == ftcore::kLeadB;
	const bool bMoved = bReported && bToLead && To != From;
	if (bMoved)
	{
		// The lead losing the fight's gauge keeps its last feed (the session's own rule), plus the fold below.
		OwnLoad[From] = FedLoad;
		OwnHeals[From] = FedHeals;
		bOwnDeparted[From] = true;
	}
	// The kept changes this step: a store or a Lift spend on the away lead's kept gauge (before a swap, so in the
	// value it brings back) and the fold of the losing lead's changes at a swap. The controlled lead has no ledger.
	for (const ftcore::CountLoadChange& C : Tick.loads)
	{
		if (!C.kept || (C.lead != ftcore::kLeadA && C.lead != ftcore::kLeadB) || OwnLoad[C.lead] < 0)
		{
			continue;
		}
		const int32 Signed = C.kind == ftcore::CountLoadKind::LiftSpend ? -C.amount : C.amount;
		OwnLoad[C.lead] = FMath::Clamp(OwnLoad[C.lead] + Signed, 0, ftcore::kMaxLoad);
	}
	if (Tick.leadLoadNow >= 0)
	{
		Fight.setLoad(Tick.leadLoadNow); // clamped to the fight's own cap; overloaded() judged from it
	}
	if (Tick.leadHealsNow >= 0)
	{
		Fight.setHealCharges(Tick.leadHealsNow);
	}
	if (bReported && bToLead)
	{
		FedLead = To;
	}
	if (!bMoved)
	{
		return;
	}
	++LeadMoves;
	if (OwnLoad[To] >= 0)
	{
		++LeadMovesChecked;
		LeadReturnsChecked += bOwnDeparted[To] ? 1 : 0;
		const bool bOk = Tick.leadLoadNow == OwnLoad[To] && Tick.leadHealsNow == OwnHeals[To] && Fight.load() == Tick.leadLoadNow
			&& static_cast<int32>(Fight.healCharges()) == Tick.leadHealsNow;
		LeadValueMisses += bOk ? 0 : 1;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: control to lead %d with its own Load %d (expected %d, the gauge %d) and heal charges %d (expected %d, the fight %d); lead %d left with %d and %d%s."),
			Fight.time(), static_cast<int32>(To), Tick.leadLoadNow, OwnLoad[To], Fight.load(), Tick.leadHealsNow, OwnHeals[To],
			static_cast<int32>(Fight.healCharges()), static_cast<int32>(From), FedLoad, FedHeals, bOwnDeparted[To] ? TEXT(", a return") : TEXT(""));
	}
	OwnLoad[To] = OwnHeals[To] = -1;
	bOwnDeparted[To] = false;
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
	// The kept gauge follows control (keptLead), so it is the partner's: leadLoad(OtherLead()) reads the same.
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
	if (Fight.frozen() && StepFrozen())
	{
		return; // a special's frozen step: nothing but its clock moves (FTArenaRunnerSpecial.cpp)
	}
	OpenLeadStep(); // per-lead: the controlled lead's values before any press, blow or spend this step
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
		// -ArenaNoLoneCall: none while Lone, so a fight begun Lone with -ArenaPartner keeps the partner out.
		In.pair = bPartnerDue && !(bNoLoneCall && Fight.lone());
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
		if (bTableOpen && TableClock.state() == ftcore::TableState::Set)
		{
			// -ArenaTableOpen: the autopilot stands still and open while the table is set, so a blow lands on a seated
			// lead and the share shows (a review switch for the HUD's SHARED popup, not a way to play).
			In.move = ftcore::Vec2{};
			In.light = In.heavy = In.dodge = In.guard = In.release = In.pair = In.jump = In.heal = false;
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
		if (RollAtNext < RollAtCount && Fight.time() >= RollAt[RollAtNext])
		{
			// -ArenaRollAt: a dodge press now, along the stick the autopilot holds (nothing else pressed with it).
			++RollAtNext;
			In.dodge = true;
			In.light = In.heavy = In.release = false;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaRollAt dodge pressed at %.2f s."), Fight.time());
		}
		if (StandOffAt > 0.f)
		{
			// -ArenaStandOff: the lead circles the target the camera follows (else the lock) at a slow walk, stepping in or
			// out to hold the range, guarding (a capture mode: stand-off stills at a set range).
			const int32 T = CloseTargetNow >= 0 ? CloseTargetNow : (Fight.lock() != ftcore::kArenaNobody ? static_cast<int32>(Fight.lock()) : -1);
			In.light = In.heavy = In.release = In.dodge = false;
			In.guard = true;
			In.move = ftcore::Vec2{0.f, 0.f};
			if (T >= 0 && T < static_cast<int32>(Fight.enemyCount()))
			{
				const ftcore::Vec2& E = Bodies.enemies[static_cast<size_t>(T)];
				const ftcore::Vec2 To{E.x - Bodies.player.x, E.y - Bodies.player.y};
				const float D = ArenaLength(To);
				const float Err = D - StandOffAt;
				if (D > 0.01f)
				{
					const float Push = (FMath::Abs(Err) > 0.15f ? FMath::Clamp(Err * 1.5f, -1.f, 1.f) : 0.f) / D;
					const float Round = 0.4f / D; // the tangent, counter-clockwise round the target
					ftcore::Vec2 M{To.x * Push - To.y * Round, To.y * Push + To.x * Round};
					const float Len = ArenaLength(M);
					if (Len > 1.f)
					{
						M = ftcore::Vec2{M.x / Len, M.y / Len};
					}
					In.move = M;
				}
			}
			if (StandOffStrikeEvery > 0.f)
			{
				// -ArenaStandOffStrike: the string's presses kStandOffPressGap s apart, the next string <s> s after the last
				// one began; no guard while one runs.
				if (Fight.time() >= StandOffStrikeNext)
				{
					In.light = true;
					++StandOffStrikePresses;
					StandOffStrikeNext += StandOffStrikePresses % kStandOffStringPresses != 0 ? kStandOffPressGap
						: StandOffStrikeEvery - kStandOffPressGap * static_cast<float>(kStandOffStringPresses - 1);
				}
				const ftcore::ArenaAction LeadActNow = Fight.output().player.action;
				if (In.light || StandOffStrikePresses % kStandOffStringPresses != 0 || LeadActNow == ftcore::ArenaAction::Light
					|| LeadActNow == ftcore::ArenaAction::Heavy)
				{
					In.guard = false;
				}
			}
		}
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
	// The first kTakeoverHoldSeconds of a takeover (the fight's clock, as the camera's move): the camera holds on the
	// fallen lead and no press is taken (the camera's own forward stays, so the lock still picks from the view).
	// Cleared here and again after the last assignment (the boss's buttons, autoplay's take-half and guard, the
	// hallucination) so nothing set on the way leaks into the step.
	const bool bTakeoverHold = TakeoverInputUntil >= 0.0 && static_cast<double>(Fight.time()) < TakeoverInputUntil;
	const auto HoldPresses = [&In, &bIntercept]()
	{
		In.move = ftcore::Vec2{};
		In.light = In.heavy = In.dodge = In.jump = In.heal = In.guard = In.lock = In.release = In.pair = In.takeHalf = false;
		In.lockSwitch = 0;
		In.read = -1;
		bIntercept = false;
	};
	if (bTakeoverHold)
	{
		HoldPresses();
		++TakeoverInputsIgnored;
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
	if (bTakeoverHold)
	{
		HoldPresses();
	}
	PressSession(In);
	if (Session)
	{
		// The gift Guard scales every blow the lead takes (1 unguarded).
		In.takenScale = Session->Get().giftSlot().guardScaleFor(SessionLead, Session->Get().now());
	}
	if (bPartner)
	{
		In.partnerOrder = bPartnerSpread ? 1 : 0;
		// The partner's stationing (rules 8-12) from the play camera as the player has it: the active lead's body,
		// the pivot, the arm and the control rotation (ftcore::arenaCameraLens of the pose last drawn), on the ground
		// in m; its forward is cameraFacing, else view. Not the view target or a boom: a capture's side camera, a
		// switch's blend or the boom's lag is not where the player looks from. No pose drawn yet: no stationing.
		if (bCameraPoseNow)
		{
			const ftcore::ArenaLensAt Lens = ftcore::arenaCameraLens(bFedCamera ? CameraLensNow : CameraPoseNow);
			In.cameraOn = true;
			In.cameraPos = ftcore::Vec2{Bodies.player.x + Lens.x, Bodies.player.y + Lens.y};
			// ... and that camera in full, the lens over the floor (the brain's frame rules 13-16, the in-play count).
			In.partnerCamera = bFedCamera ? FedCamera : ftcore::arenaPoseCamera(CameraPoseNow, Bodies.player, LockAspect(), PartnerHeight());
			// Under the close preset the partner stands ahead of the lead on a flank of its target, inside that same
			// camera (ftcore::PartnerCloseView, rule 19); classic gives none, as before.
			if (bCloseShape && bPartnerStation)
			{
				RefreshCloseWalls(StepSeconds);
				In.partnerClose = CloseStationView(In.partnerCamera);
			}
		}
	}
	if (bPartner)
	{
		TrackSwapCheck(In); // the input this step takes, after a takeover or a switch
	}
	const bool bAllyWasUp = Fight.allyHp() > 0; // the brain stations only a body up before and after the step
	PressSpecial(In);
	const ftcore::ArenaOutput& Out = Fight.tick(StepSeconds, In, Bodies);
	// The press tick or the cut back: the performer put where Core says, and no step (no separation push) this tick.
	const bool bSpecialPut = AfterSpecialTick(Out);
	ReportFightSpends(Out);
	if (bPartner && Session)
	{
		// Who is in the fight, step by step: the session's word as of its last close against the fight's this step
		// (a join lands at a close and the fight takes the partner before its next step).
		PartnerJoinDisagree += Out.ally.partnerOn != Session->Get().inFight(OtherLead()) ? 1 : 0;
	}
	if (bPartner && Out.ally.partnerOn && bAllyWasUp && Fight.allyHp() > 0)
	{
		++PartnerOnSteps;
		PartnerCameraSteps += In.cameraOn ? 1 : 0;
		PartnerStationedSteps += Out.ally.stationed ? 1 : 0;
		// In play: the partner in the frame the brain itself tests (at ftcore::kPartnerFrustumMargin on the camera it was
		// fed: its feet, middle and head, ftcore::partnerInFrame; under the close camera its middle and head alone,
		// ftcore::partnerInFrameClose), and its actions that lasted and were carried out.
		if (ftcore::partnerCameraWhole(In.partnerCamera))
		{
			++PartnerFrustumSteps;
			if (bCloseShape)
			{
				const ftcore::PartnerCameraView& Cam = In.partnerCamera;
				++PartnerNearSteps;
				PartnerNearOkSteps += ftcore::partnerCloseNearOk(ftcore::Vec2{Cam.pos.x, Cam.pos.y}, ftcore::Vec2{Cam.forward.x, Cam.forward.y},
					Bodies.player, Bodies.ally) ? 1 : 0;
			}
			const bool bSeen = bCloseShape ? ftcore::partnerInFrameClose(In.partnerCamera, ftcore::kPartnerFrustumMargin, Bodies.ally)
			                               : ftcore::partnerInFrame(In.partnerCamera, ftcore::kPartnerFrustumMargin, Bodies.ally);
			PartnerSeenSteps += bSeen ? 1 : 0;
			// On screen and not behind the locked target: hidden when the lines from the lens to its middle and to its
			// head both pass through the target's drawn cylinder.
			bool bHidden = false;
			const std::uint8_t Locked = Fight.lock();
			if (bSeen && Locked < Fight.def().enemies.size() && Locked < Bodies.enemies.size())
			{
				const ftcore::ArenaEnemyKind& Foe = Fight.def().enemies[Locked];
				const float Scale = Foe.look.skinned() ? Foe.look.scale : 1.f;
				const float Base = In.partnerCamera.groundZ + (Foe.look.skinned() ? Foe.look.hover : 0.f);
				const float Top = Base + ftcore::arenaEnemyHeight(Foe) * Scale;
				const ftcore::PartnerVec3& Eye = In.partnerCamera.pos;
				const ftcore::ArenaPoint3 From{Eye.x, Eye.y, Eye.z};
				const float Feet = In.partnerCamera.groundZ;
				const float Tall = In.partnerCamera.bodyHeight;
				const auto Behind = [&](float Z)
				{
					return ftcore::arenaHiddenBehind(From, {Bodies.ally.x, Bodies.ally.y, Feet + Z}, Bodies.enemies[Locked],
						Foe.bodyRadius * Scale, Base, Top);
				};
				bHidden = Behind(Tall * 0.55f) && Behind(Tall * 0.95f);
				PartnerClearFlatSteps +=
					ftcore::partnerCloseHiddenBehind(ftcore::Vec2{Eye.x, Eye.y}, Bodies.ally, Bodies.enemies[Locked], Foe.bodyRadius * Scale) ? 0 : 1;
			}
			else if (bSeen)
			{
				++PartnerClearFlatSteps; // nothing locked: nothing to hide behind
			}
			PartnerClearSteps += bSeen && !bHidden ? 1 : 0;
			// ... and not behind the lead: the same two lines through the lead's body (its radius, up to its head).
			if (bSeen)
			{
				const ftcore::ArenaLeadBody Lb = LeadBody();
				const ftcore::PartnerVec3& LeadEye = In.partnerCamera.pos;
				const float Ground = In.partnerCamera.groundZ;
				const float Height = In.partnerCamera.bodyHeight;
				const auto BehindLead = [&](float Z)
				{
					return ftcore::arenaHiddenBehind({LeadEye.x, LeadEye.y, LeadEye.z}, {Bodies.ally.x, Bodies.ally.y, Ground + Z}, Bodies.player,
						Lb.radius, Ground, Ground + Lb.head);
				};
				PartnerLeadClearSteps += BehindLead(Height * 0.55f) && BehindLead(Height * 0.95f) ? 0 : 1;
				PartnerLeadClearFlatSteps += ftcore::partnerCloseOverlaps(ftcore::Vec2{LeadEye.x, LeadEye.y}, Bodies.ally, 0.4f,
					Bodies.player, 0.4f) ? 0 : 1;
			}
		}
		PartnerPlaySeconds += StepSeconds;
		// Engaged: the lead attacked (a light, heavy, riposte or release) within kPartnerEngagedWindow s.
		const ftcore::ArenaAction LeadAct = Out.player.action;
		if (LeadAct == ftcore::ArenaAction::Light || LeadAct == ftcore::ArenaAction::Heavy || LeadAct == ftcore::ArenaAction::Riposte
			|| LeadAct == ftcore::ArenaAction::Release)
		{
			LeadAttackAt = Fight.time();
		}
		const bool bEngaged = Fight.time() - LeadAttackAt <= kPartnerEngagedWindow;
		PartnerEngagedSeconds += bEngaged ? StepSeconds : 0.f;
		// Standing still: not swinging or guarding and moved under 1 cm since the last step; the longest such run.
		const bool bStill = bPartnerStillAt && !Out.ally.attacking && !Out.ally.guarding &&
			ArenaLength(ftcore::Vec2{Bodies.ally.x - PartnerStillAt.x, Bodies.ally.y - PartnerStillAt.y}) < 0.01f;
		PartnerStillFor = bStill ? PartnerStillFor + StepSeconds : 0.f;
		PartnerStillLongest = FMath::Max(PartnerStillLongest, PartnerStillFor);
		PartnerStillAt = Bodies.ally;
		bPartnerStillAt = true;
		const ftcore::PartnerIntent& Intent = AllyBrain.intent();
		const uint8 Act = static_cast<uint8>(Intent.act);
		if (Act != PartnerLastAct || (Intent.act == ftcore::PartnerAct::Attack && Intent.newString))
		{
			FlushPartnerAction(); // a new act (or a new string) ends the one before
			PartnerActFrom = Fight.time();
			PartnerActAt = Bodies.ally;
			PartnerActDone = false;
		}
		PartnerLastAct = Act;
		PartnerActEngaged = PartnerActEngaged || bEngaged; // an act counts as engaged when any of its steps was
		// Carried out: a blow reached its active part, the guard went up, a move covered half a metre. A Wait never
		// counts.
		switch (Intent.act)
		{
		case ftcore::PartnerAct::Attack: PartnerActDone = PartnerActDone || (Out.ally.attacking && Out.ally.part == ftcore::ArenaPart::Active); break;
		case ftcore::PartnerAct::Guard: PartnerActDone = PartnerActDone || Out.ally.guarding; break;
		case ftcore::PartnerAct::MoveTo:
		case ftcore::PartnerAct::StepBack:
			PartnerActDone = PartnerActDone || ArenaLength(ftcore::Vec2{Bodies.ally.x - PartnerActAt.x, Bodies.ally.y - PartnerActAt.y}) >= 0.5f;
			break;
		default: break;
		}
		PartnerActTo = Fight.time() + StepSeconds;
		// Its idle reason, logged each time it changes (the brain's own report, ftcore::PartnerIntent::idle).
		const uint8 Why = static_cast<uint8>(Intent.idle);
		if (Why != PartnerIdleWhy && Intent.idle != ftcore::PartnerIdle::None)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: partner idle at %.2f s (%s)."), Fight.time(), PartnerIdleWord(Intent.idle));
		}
		PartnerIdleWhy = Why;
		if (Intent.offFrame && Intent.newString)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: partner string begun off the frame at %.2f s (no station in frame)."), Fight.time());
		}
	}
	else
	{
		// Down or out: a stand-still run ends, so a partner that rises where it fell starts a new one.
		PartnerStillFor = 0.f;
		bPartnerStillAt = false;
	}
	if (bPartner)
	{
		TakeOverBodies(Out); // before the step: the bodies are the host's (ArenaFight::setPartner)
	}
	if (!bSpecialPut)
	{
		const uint32 CapsBefore = Bodies.settleCaps;
		ftcore::arenaStep(Fight, Bodies, StepSeconds, Fit.zone);
		if (Bodies.settleCaps != CapsBefore)
		{
			// The settle stopped at its pass cap (walls kept, a pair perhaps left past 0.1 mm): a capture counts these.
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: body settle hit its pass cap (%u so far)."), Fight.time(),
				   Bodies.settleCaps);
		}
	}
	if (bPartner)
	{
		WalkPartner(Out);
	}
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
		// The puff at the height the lead's blow lands (last step's aim: a fist's contact point, a shot's chest point).
		const FVector LeadAt = PlayerFigure ? PlayerFigure->GetChest() : ToWorld(Bodies.player);
		KiteFx.Feed(Out.events, Kite, Fight, ToWorld(Bodies.enemies[static_cast<size_t>(KiteSlot)]),
			FVector(LeadAt.X, LeadAt.Y, PlayerFigure ? LeadWeapons.GetAimPoint().Z : LeadAt.Z));
	}
	if (bKindCritHook)
	{
		FeedKindCrits(Out.events);
	}
	// The lead's strike clips run on the held clock, so they stand still through its hit pauses as its action does.
	LeadWeapons.Feed(Out.events, static_cast<double>(Fight.heldTime()));
	TrackAllyWeapons(Out);
	FeedSession();
	TrackPartner(Out);
	// The events (this step's Released pooled), then a gift held while Lone: the order is Core's (LoneGiftHold::stepEnd).
	LoneGift.stepEnd([this, &Out] { HandleEvents(Out.events); }, [this] { PressPendingLoneGift(); });
	ReactLeadBodies(Out.events);
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
			// A melee blow at the point of the body its fist or foot can reach; a shot or a cast at the chest.
			const FVector Ranged(At.X, At.Y, Enemy ? Enemy->GetChest().Z : LeadChest.Z);
			if (Enemy)
			{
				const double Feet = Enemy->GetActorLocation().Z;
				LeadWeapons.SetAimAtBody(At, Feet, Feet + Enemy->GetHeight(), Ranged);
			}
			else
			{
				LeadWeapons.SetAim(Ranged, true);
			}
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
	else if (UFTCrowdSubsystemV2* CrowdV2 = UFTCrowdSubsystemV2::Get(GetWorld()))
	{
		CrowdV2->FeedArena(Events); // -CrowdV2
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
				if (E.source < MaxEnemies)
				{
					// The blow's side for the hit flash: lead to striker, in the drawn camera's ground axes.
					const ftcore::Vec2& By = Bodies.enemies[E.source];
					const FVector2D D(By.x - Bodies.player.x, By.y - Bodies.player.y);
					const double Yaw = FMath::DegreesToRadians(static_cast<double>(bCameraPoseNow ? CameraPoseNow.yaw : ViewYaw()));
					const FVector2D Ahead(FMath::Cos(Yaw), FMath::Sin(Yaw));
					const FVector2D RightAxis(-FMath::Sin(Yaw), FMath::Cos(Yaw));
					const FVector2D Local(FVector2D::DotProduct(D, RightAxis), FVector2D::DotProduct(D, Ahead));
					LastHitFrom = Local.SizeSquared() > 1e-6 ? Local.GetSafeNormal() : FVector2D(0.0, 1.0);
				}
				else
				{
					LastHitFrom = FVector2D::ZeroVector; // a blow with no enemy behind it (a hazard): no side
				}
				LastHitAt = Now;
			}
			else if (E.target < MaxEnemies)
			{
				// The partner's blow is a Hit too (source kArenaAlly, move = its blow's index, never a heavy's).
				PartnerHitEvents += E.source == ftcore::kArenaAlly ? 1 : 0;
				if (E.source == ftcore::kArenaAlly)
				{
					// A shot's impact as it arrives (its tracer has just left), a cast's and a blow's now (the bolt left
					// as the startup began: FFTLeadWeapons::FlightOf, 0 unless the startup is under the bolt's floor).
					// The partner's own kit: its blow is that kit's light (ArenaDef::partnerKit).
					const ftcore::ArenaFighter& Kit = Fight.def().partnerKit;
					const int32 Lights = FMath::Max(1, static_cast<int32>(Kit.lightCount));
					const double Flight = FFTLeadWeapons::FlightOf(Kit, static_cast<uint8>(static_cast<int32>(E.move) % Lights));
					if (Flight > 0.0)
					{
						PlayWaitingPartnerImpact(1e300); // one waiting already plays now
						PartnerImpactWaiting = E;
						PartnerImpactDue = static_cast<double>(Fight.heldTime()) + Flight;
					}
					else
					{
						PartnerImpact(E);
					}
				}
				else if (E.source == ftcore::kArenaPlayer && E.amount > 0 && PlayerFigure)
				{
					ImpactSoundHook(E.source, PlayerFigure->GetChest(), 1.f);
				}
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
				StepBackWay[E.source] = static_cast<uint8>(E.move);
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.3f: enemy %d steps %s %.2f m over %.2f s (a break-out with no token or out of sight)."),
				Fight.time(), static_cast<int32>(E.source), E.move == 1 ? TEXT("to its left") : E.move == 2 ? TEXT("to its right") : TEXT("back from the lead"),
				static_cast<float>(E.amount) / 100.f, ftcore::kArenaStepBackTime);
			continue;
		case ftcore::ArenaEventType::BreakTell: // the boss's own break move is its tell; no HUD of its own
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.3f: enemy %d's break value tells (%.2f of %.2f)."), Fight.time(),
				static_cast<int32>(E.source), static_cast<float>(E.amount) / 100.f,
				E.source < Fight.enemyCount() ? Fight.enemyBreakAt(E.source) : 0.f);
			continue; // no popup
		case ftcore::ArenaEventType::LockChanged:
#if !UE_BUILD_SHIPPING
			if (E.target != ftcore::kArenaNobody && E.target < Fight.enemyCount())
			{
				// The lock pick against the drawn view (the close lens's shoulder and partner turn move it).
				const ftcore::Vec2& Foe = Bodies.enemies[E.target];
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: lock to %d at %.2f s: %.2f m away, drawn yaw %.1f, camera %s, shoulder %+.2f, melee %.2f."),
					static_cast<int32>(E.target), Fight.time(), ArenaLength(ftcore::Vec2{Foe.x - Bodies.player.x, Foe.y - Bodies.player.y}), ViewYaw(),
					UTF8_TO_TCHAR(ftcore::cameraPresetName(CameraPresetNow)), CloseShoulder, CloseMelee);
			}
#endif
			continue; // no popup
		case ftcore::ArenaEventType::CardsDealt: // the cards themselves are the display
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
	// On the boss's surface toward the striker (the lead, or the partner's body), at the lead's chest height.
	const size_t Slot = static_cast<size_t>(StoneBearerSlot);
	const ftcore::Vec2& Boss = Bodies.enemies[Slot];
	const ftcore::Vec2& By = E.source == ftcore::kArenaAlly ? Bodies.ally : Bodies.player;
	ftcore::Vec2 Dir{By.x - Boss.x, By.y - Boss.y};
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

bool UFTArenaRunner::GetBossWeakPoint(int32 Slot, FVector& Out) const
{
	if (Slot < 0 || Slot >= MaxEnemies)
	{
		return false;
	}
	if (Slot == StoneBearerSlot)
	{
		Out = BossWeakPoint();
		return true;
	}
	const AFTArenaFighter* Figure = EnemyFigures[Slot].Get();
	if (Slot != KiteSlot || !Figure)
	{
		return false;
	}
	static const FName Socket(FFTKiteHost::WeakPointSocket);
	if (!Figure->GetSkinPoint(Socket, Out))
	{
		Out = Figure->GetChest();
	}
	return true;
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
		// The ready stance while locked on and standing or stepping in a live fight.
		Pose.bReady = !Fight.over() && Fight.lock() != ftcore::kArenaNobody && !Pose.bAir
			&& (P.action == ftcore::ArenaAction::Idle || P.action == ftcore::ArenaAction::Move);
		Pose.ReadyStance = ReadyStanceFor(0, PlayerLook);
		PlayerFigure->ApplyPose(Pose);
		// The strike clips at the fight time drawn (the bodies are drawn a step behind, Alpha of the way to the last), and
		// the gun's reload overlay by the fight's own progress.
		LeadWeapons.SetReload(Fight.reloading(), Fight.reloadProgress());
		LeadWeapons.Paint(DrawnTime);
	}
	PlaceAlly(Alpha);
	AllyWeapons.Paint(DrawnTime);
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
	if (bCloseShape && CloseContact > 0.f)
	{
		FramePrev = FrameTurn; // the close contact swing frames the flank, not the thirds: counted on its own check line
		return;
	}
	const float Rate = DeltaSeconds > 0.f ? FMath::Abs(FrameTurn - FramePrev) / DeltaSeconds : 0.f;
	FramePrev = FrameTurn;
	const APlayerCameraManager& Cam = *PC->PlayerCameraManager;
	const FVector Eye = Cam.GetCameraLocation();
	// Only the lead's play camera is judged (ruled 11-16): a capture's shot or side view, or a lens further from the pivot
	// than the arm and a metre, is no play frame and is left out of every count here.
	if (PC->GetViewTarget() != PlayerFigure.Get())
	{
		++FrameShotSkipped;
		return;
	}
	if (bCloseShape &&
		FVector::Dist(Eye, FVector(Bodies.player.x * 100.0, Bodies.player.y * 100.0, ZoneZ + Fit.camera.pivot * 100.0)) > (Fit.camera.arm + 1.f) * 100.0)
	{
		++FrameShotSkipped;
		++CloseFarSkipped; // reported on the miss line: a lens that far off is a lag or cut fault the split must show
		return;
	}
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
	if (bCloseShape)
	{
		// The close camera's bar (ruled 11-16): the lead in its near-side third, the target's weak point (else its chest)
		// in its band of the width, the partner anywhere in the frame (ftcore::arenaCloseFrameCheck). Shares of the width.
		FVector Aim(E.x * 100.0, E.y * 100.0, ZoneZ + Tall * kArenaChestShare * 100.0);
		FVector Weak;
		if (GetBossWeakPoint(Target, Weak))
		{
			Aim = Weak;
		}
		// The vertical half-angle's tangent from the drawn fov (horizontal) and the sampled view's own aspect (its
		// camera's when constrained, else its player's viewport); y as arenaFramePoint gives it (0 the bottom, 1 the top).
		const FVector Up = FRotationMatrix(View).GetScaledAxis(EAxis::Z);
		const double TanUp = Tan / FMath::Max(static_cast<double>(ViewAspect(*PC)), 0.1);
		ftcore::ArenaFramePoint Mate;
		const bool bMateOn = bPartner && Fight.output().ally.partnerOn && Fight.allyHp() > 0;
		if (bMateOn)
		{
			const FVector M = FVector(Bodies.ally.x * 100.0, Bodies.ally.y * 100.0, ZoneZ + 100.0) - Eye;
			const double Depth = FVector::DotProduct(M, Ahead);
			Mate.ahead = Depth > 1.0;
			Mate.x = 0.5f + 0.5f * static_cast<float>(FVector::DotProduct(M, Right) / FMath::Max(Depth, 1.0) / Tan);
			Mate.y = 0.5f + 0.5f * static_cast<float>(FVector::DotProduct(M, Up) / FMath::Max(Depth, 1.0) / TanUp);
		}
		const float AimX = 0.5f + 0.5f * Across(Aim, 0.0);
		const ftcore::ArenaCloseFrameCheck C = ftcore::arenaCloseFrameCheck(ftcore::kArenaClose, CloseShoulder, 0.5f + 0.5f * LeadX,
			AimX, ArenaLength(To), bMateOn, Mate);
		CloseFrameOk += C.all ? 1 : 0;
		CloseFrameLead += C.lead ? 1 : 0;
		CloseFrameFoe += C.target ? 1 : 0;
		CloseFrameMate += C.partner ? 1 : 0;
		// What each miss had (the lens as the boom has it against the rig's own, the turn, the partner's turn, the lead's pace).
		const USpringArmComponent* MissBoom = PlayerFigure.Get() ? PlayerFigure->GetBoom() : nullptr;
		const float RigArm = MissBoom ? MissBoom->TargetArmLength / 100.f : Fit.camera.arm;
		const float RigLens = FMath::Sqrt(RigArm * RigArm + Fit.camera.side * Fit.camera.side);
		const bool bMissWall = BoomWallShare() < 0.98f;
		const bool bMissLag = static_cast<float>(FVector::Dist(Eye, Pivot) / 100.0) > RigLens + 0.15f;
		const bool bMissCap = FMath::Abs(FrameTurn) >= FrameRules().maxTurn - 0.5f;
		const bool bMissMateTurn = FMath::Abs(PartnerYawNow) > 1.f;
		const ftcore::Vec2 LeadMoved{Bodies.player.x - PrevBodies.player.x, Bodies.player.y - PrevBodies.player.y};
		const bool bMissFast = ArenaLength(LeadMoved) > 3.f * StepSeconds;
		const bool bMissNear = ArenaLength(To) < 3.f;
		for (int32 K = 0; K < 2; ++K)
		{
			if (K == 0 ? C.target : C.lead)
			{
				continue;
			}
			CloseMissWall[K] += bMissWall ? 1 : 0;
			CloseMissLag[K] += bMissLag ? 1 : 0;
			CloseMissCap[K] += bMissCap ? 1 : 0;
			CloseMissMateTurn[K] += bMissMateTurn ? 1 : 0;
			CloseMissFast[K] += bMissFast ? 1 : 0;
			CloseMissNear[K] += bMissNear ? 1 : 0;
		}
		if (!C.target)
		{
			const float Mirrored = CloseShoulder < 0.f ? 1.f - AimX : AimX;
			CloseMissFoeLow += Mirrored < ftcore::kArenaClose.targetMax ? 1 : 0;
			CloseMissFoeHigh += Mirrored >= ftcore::kArenaClose.targetMax ? 1 : 0;
			CloseMissFoeSum += Mirrored;
			CloseMissFoeAtLens += FVector::DotProduct(Aim - Eye, Ahead) < 100.0 ? 1 : 0;
		}
	}
	FrameLeadWorst = FMath::Max(FrameLeadWorst, FMath::Abs(LeadX));
	FrameFoeWorst = FMath::Max(FrameFoeWorst, FMath::Abs(FoeX));
	const float Dist = ArenaLength(To);
	const int32 Band = Dist < 3.f ? 0 : (Dist < 6.f ? 1 : (Dist < 10.f ? 2 : 3));
	++FrameBandN[Band];
	FrameBandFoe[Band] += FoeX;
	FrameBandGap[Band] = FMath::Min(FrameBandGap[Band], Gap);
}
#endif

ftcore::ArenaCameraPose UFTArenaRunner::LockRig() const
{
	return ftcore::arenaCameraPose(Fit.camera, ArenaAim, ftcore::ArenaFrameYaw{CameraYaw, FrameTurn}, 0.f);
}

float UFTArenaRunner::BoomWallShare() const
{
	// The spring arm's probe pulls the whole offset (the arm and the socket's shoulder and height) in along the line
	// from its origin: the share left is the probed lens's distance over the unprobed one's.
	const AFTArenaFighter* Lead = PlayerFigure.Get();
	const USpringArmComponent* Boom = Lead ? Lead->GetBoom() : nullptr;
	if (!Boom || !Boom->IsCollisionFixApplied())
	{
		return 1.f;
	}
	const FVector Origin = Boom->GetComponentLocation() + Boom->TargetOffset;
	const double Full = FVector::Dist(Boom->GetUnfixedCameraPosition(), Origin);
	const double Used = FVector::Dist(Boom->GetSocketLocation(USpringArmComponent::SocketName), Origin);
	return Full > 1.0 ? FMath::Clamp(static_cast<float>(Used / Full), 0.05f, 1.f) : 1.f;
}

float UFTArenaRunner::PartnerHeight() const
{
	if (const AFTArenaFighter* Mate = AllyFigure.Get())
	{
		// The anchor hangs 45 cm over the head (AFTArenaFighter::GetAnchor).
		return FMath::Max(0.5f, static_cast<float>(Mate->GetAnchor().Z - Mate->GetActorLocation().Z - 45.0) / 100.f);
	}
	return 1.8f;
}

void UFTArenaRunner::FlushPartnerAction()
{
	if (PartnerActFrom >= 0.f && PartnerActDone && PartnerActTo - PartnerActFrom >= kPartnerActMin - 1e-4f)
	{
		++PartnerActions;
		PartnerEngagedActions += PartnerActEngaged ? 1 : 0;
	}
	PartnerActFrom = PartnerActTo = -1.f;
	PartnerActDone = false;
	PartnerActEngaged = false;
}

float UFTArenaRunner::LockAspect() const
{
	FVector2D Size(0.0, 0.0);
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->GetViewportSize(Size);
	}
	return Size.X > 1.0 && Size.Y > 1.0 ? static_cast<float>(Size.X / Size.Y) : 16.f / 9.f;
}

float UFTArenaRunner::ViewAspect(const APlayerController& PC) const
{
	// The aspect the player's view is drawn at: its camera's own when the camera constrains it, else the player's
	// viewport (the lock's game viewport when the player has none yet).
	if (PC.PlayerCameraManager)
	{
		const FMinimalViewInfo& View = PC.PlayerCameraManager->GetCameraCacheView();
		if (View.bConstrainAspectRatio && View.AspectRatio > 0.1f)
		{
			return View.AspectRatio;
		}
	}
	int32 X = 0, Y = 0;
	PC.GetViewportSize(X, Y);
	return X > 1 && Y > 1 ? static_cast<float>(X) / static_cast<float>(Y) : LockAspect();
}

void UFTArenaRunner::RefreshCloseWalls(float Dt)
{
	CloseWallAge += Dt;
	if (CloseWallAge < CloseWallSeconds)
	{
		return;
	}
	CloseWallAge = 0.f;
	CloseWallCount = 0;
	const AFTArenaFighter* Lead = PlayerFigure.Get();
	const USpringArmComponent* Boom = Lead ? Lead->GetBoom() : nullptr;
	UWorld* World = GetWorld();
	if (!Lead || !Boom || !World)
	{
		return;
	}
	// The boom's own probe (its channel and sphere), swept flat from the lead at 1.2 m; the fighters are not walls.
	FCollisionQueryParams& Params = CloseWallParams;
	Params.StatId = SCENE_QUERY_STAT_ONLY(ArenaCloseWalls);
	Params.bTraceComplex = false;
	Params.ClearIgnoredSourceObjects(); // keeps its room: no allocation after the first sweep
	Params.AddIgnoredActor(Lead);
	if (AllyFigure)
	{
		Params.AddIgnoredActor(AllyFigure);
	}
	for (int32 I = 0; I < MaxEnemies; ++I)
	{
		if (const AFTArenaFighter* Foe = EnemyFigures[I].Get())
		{
			Params.AddIgnoredActor(Foe);
		}
	}
	const FVector From = ToWorld(Bodies.player) + FVector(0.0, 0.0, 120.0);
	for (int32 K = 0; K < CloseWallRays; ++K)
	{
		const double A = 2.0 * UE_DOUBLE_PI * K / CloseWallRays;
		const FVector To = From + FVector(FMath::Cos(A), FMath::Sin(A), 0.0) * (CloseWallReach * 100.0);
		FHitResult Hit;
		if (!World->SweepSingleByChannel(Hit, From, To, FQuat::Identity, Boom->ProbeChannel, FCollisionShape::MakeSphere(Boom->ProbeSize), Params))
		{
			continue;
		}
		const FVector2D N = FVector2D(Hit.ImpactNormal.X, Hit.ImpactNormal.Y).GetSafeNormal();
		if (N.IsNearlyZero())
		{
			continue; // a floor or a ceiling, not a wall
		}
		// 1 m along the surface (square to its normal), centred on the hit.
		const ftcore::Vec2 C{static_cast<float>(Hit.ImpactPoint.X / 100.0), static_cast<float>(Hit.ImpactPoint.Y / 100.0)};
		const ftcore::Vec2 T{static_cast<float>(-N.Y) * 0.5f, static_cast<float>(N.X) * 0.5f};
		ftcore::PartnerWall& W = CloseWalls[CloseWallCount++];
		W.a = ftcore::Vec2{C.x - T.x, C.y - T.y};
		W.b = ftcore::Vec2{C.x + T.x, C.y + T.y};
		W.halfThickness = 0.1f;
	}
}

ftcore::PartnerCloseView UFTArenaRunner::CloseStationView(const ftcore::PartnerCameraView& Camera) const
{
	ftcore::PartnerCloseView V;
	V.given = true;
	// The lead's side of the screen: the lens over the right shoulder puts the lead left of centre (0 mid-swap).
	V.leadSide = static_cast<std::int8_t>(FMath::Abs(CloseShoulder) < 0.5f ? 0 : (CloseShoulder > 0.f ? -1 : 1));
	// The kit band: a magic partner keeps the caster's range, the others the melee one.
	const ftcore::ArenaFighter& Kit = Fight.def().partnerKit;
	const bool bCaster = Kit.weapon == ftcore::ArenaWeapon::Magic;
	V.standMin = bCaster ? ftcore::kPartnerCloseCasterMin : ftcore::kPartnerCloseMeleeMin;
	V.standMax = bCaster ? ftcore::kPartnerCloseCasterMax : ftcore::kPartnerCloseMeleeMax;
	V.bodyRadius = FMath::Clamp(Kit.bodyRadius, 0.1f, 1.5f);
	V.walls = std::span<const ftcore::PartnerWall>(CloseWalls, static_cast<size_t>(CloseWallCount));
	// The lens as the brain's camera has it: its half-angles, its pitch and its height over the partner's ground.
	if (ftcore::partnerCameraWhole(Camera))
	{
		const float Vertical = FMath::DegreesToRadians(0.5f * Camera.verticalFovDegrees);
		V.verticalHalfDegrees = FMath::RadiansToDegrees(Vertical);
		V.halfAngleDegrees = FMath::RadiansToDegrees(FMath::Atan(FMath::Tan(Vertical) * Camera.aspect));
		V.pitchDegrees = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Camera.forward.z, -1.f, 1.f)));
		V.lensHeight = Camera.pos.z - Camera.groundZ;
	}
	else
	{
		V.halfAngleDegrees = 0.5f * CameraPoseNow.fov;
	}
	return V;
}

ftcore::ArenaLeadBody UFTArenaRunner::LeadBody() const
{
	ftcore::ArenaLeadBody Body;
	if (const AFTArenaFighter* Lead = PlayerFigure.Get())
	{
		// The anchor hangs 45 cm over the head (AFTArenaFighter::GetAnchor).
		Body.head = FMath::Max(0.5f, static_cast<float>(Lead->GetAnchor().Z - Lead->GetActorLocation().Z - 45.0) / 100.f);
	}
	// The height the lens keeps in the frame (classic: 0, the feet; close: the shaped cut, raised on a tall target).
	// In tall contact the cut comes back down to the waist (ftcore::arenaCloseContactLowCut): the low lens keeps the lead
	// from there up.
	Body.cut = bCloseShape
		? ftcore::arenaCloseContactLowCut(ftcore::kArenaClose,
			ftcore::arenaCloseShape(ftcore::kArenaClose, CloseBase, CloseMelee, CloseCutTall, CloseShoulder, Body.head).cutAt, Body.head, CloseContactLow)
		: ftcore::cameraPresetCut(CameraLens, Body.head);
	return Body;
}

void UFTArenaRunner::ShapeCloseLens(int32 Target, float DeltaSeconds)
{
	const ftcore::ArenaCloseRules& R = ftcore::kArenaClose;
	const bool bOn = Target >= 0 && Target < static_cast<int32>(Fight.enemyCount());
	CloseTargetNow = bOn ? Target : -1;
	CloseDistanceNow = -1.f;
	if (!bOn)
	{
		CloseShoulderFor = -1; // nothing followed: the next lock picks again, the same target included
	}
	if (bOn)
	{
		const ftcore::Vec2& E = Bodies.enemies[static_cast<size_t>(Target)];
		CloseDistanceNow = ArenaLength(ftcore::Vec2{E.x - Bodies.player.x, E.y - Bodies.player.y});
		const ftcore::ArenaEnemyKind& Foe = Fight.def().enemies[static_cast<size_t>(Target)];
		CloseHeightNow = ftcore::arenaEnemyHeight(Foe) * (Foe.look.skinned() ? Foe.look.scale : 1.f) + (Foe.look.skinned() ? Foe.look.hover : 0.f);
		CloseTall = ftcore::arenaCloseTallness(R, CloseHeightNow);
		CloseRadiusNow = Foe.bodyRadius * (Foe.look.skinned() ? Foe.look.scale : 1.f);
	}
	// The melee share from the range, held still for a dodge's first moment (the hit pause holds the whole camera
	// before SteerCamera gets here). Nothing followed eases it back to the far shape.
	const ftcore::ArenaAction Act = Fight.output().player.action;
	const bool bDodgeHold = Act == ftcore::ArenaAction::Dodge && ftcore::arenaCloseDodgeHolds(R, Fight.output().player.actionTime);
	CloseMelee = ftcore::arenaCloseMeleeStep(R, CloseMelee, ftcore::arenaCloseMeleeWant(R, CloseDistanceNow), DeltaSeconds, bDodgeHold);
	// The shoulder: picked when the followed target changes, or when a wall leaves the arm under wallRoom of its length
	// (asked again no sooner than every 0.5 s), never mid-attack (the pick waits for the attack to end).
	const bool bMidAttack = Act == ftcore::ArenaAction::Light || Act == ftcore::ArenaAction::Heavy || Act == ftcore::ArenaAction::Riposte
		|| Act == ftcore::ArenaAction::Release;
	// Contact (a tall or wide target within about 3 m of its body; none while aiming): the share blends over 0.5 s,
	// held still mid-attack and through a dodge's first moment like the swap. Nothing followed: the swing folds into the
	// lock's yaw at once (the view and the stick's basis hold, as a release folds the framing turn) and the share is 0.
	CloseContactWant = bOn && !ArenaAim.aiming() ? ftcore::arenaCloseContactWant(R, CloseDistanceNow, CloseHeightNow, CloseRadiusNow) : 0.f;
	if (!bOn)
	{
		CameraYaw += ContactYawNow;
		ContactYawNow = CloseContact = 0.f;
	}
	CloseContact = ftcore::arenaCloseContactStep(R, CloseContact, CloseContactWant, bMidAttack || bDodgeHold ? 0.f : DeltaSeconds);
	CloseCutTall = ftcore::arenaCloseCutTallness(R, CloseHeightNow, CloseContact);
	// On a target 4 m tall or more the contact share also drops the lens low (0 on a shorter one: its framing unchanged).
	CloseContactLow = ftcore::arenaCloseContactLow(R, CloseContact, CloseHeightNow);
	// Entering contact asks for the shoulder again, each side tried at its swing (the open side).
	bContactPicked = bContactPicked && CloseContactWant > 0.f;
	const bool bContactPick = bOn && CloseContactWant > 0.f && !bContactPicked;
	const bool bDue = bOn && (Target != CloseShoulderFor || bContactPick || (BoomWallShare() < R.wallRoom && Fight.time() >= CloseNextPickAt));
	if (bDue && !bMidAttack)
	{
		PickCloseShoulder(Target);
		bContactPicked = CloseContactWant > 0.f;
	}
	// A swap's blend holds still through an attack: frozen where it is until the attack ends.
	CloseShoulder = ftcore::arenaCloseShoulderStep(R, CloseShoulder, CloseShoulderWant, bMidAttack ? 0.f : DeltaSeconds);
	// The cut is LeadBody's (it knows the lead's height); the rest goes on the rig and the preset the frame reads.
	const ftcore::ArenaCloseShape Shape = ftcore::arenaCloseShape(R, CloseBase, CloseMelee, CloseCutTall, CloseShoulder, 0.f);
	// Stand-off clearance: off contact, a target whose head sits behind the lead on the screen (the lead's box widened by
	// 5% of the frame) takes the lens further out on its shoulder (to 0.85 m in all) and the pivot up (to 0.3 m), by the
	// least of the two that clears it, eased in and out over 0.25 s. Asked of the last drawn view with this frame's shape
	// (the clearance taken off), the arm as the wall leaves it; scaled down as contact blends in. The last ask is held
	// while it still clears the head (by 2% of the frame less margin) and the bare view does not (by 2% more), so a head
	// swaying at the edge of a move does not flip it.
	ftcore::ArenaCloseClear ClearWant;
	if (bOn && bCameraPoseNow && CloseContact < 1.f && !ArenaAim.aiming())
	{
		ftcore::ArenaCameraPose View = CameraPoseNow;
		View.pivot = Shape.pivot;
		View.side = Shape.side;
		View.fov = Shape.fov;
		const ftcore::Vec2& E = Bodies.enemies[static_cast<size_t>(Target)];
		const ftcore::ArenaPoint3 Head{E.x - Bodies.player.x, E.y - Bodies.player.y, CloseHeadHeight()};
		ClearWant = ftcore::arenaCloseStandOffClear(R, WallScaled(View, BoomWallShare()), LockAspect(), LeadBody(), Head, ClearHeld);
		ClearHeld = ClearWant;
		ClearWant.side *= 1.f - CloseContact;
		ClearWant.pivot *= 1.f - CloseContact;
	}
	else
	{
		ClearHeld = ftcore::ArenaCloseClear();
	}
	// The side is eased unsigned (ClearWant.side carries the shape's own sign) and drawn on this frame's shoulder.
	ClearSideNow = ftcore::arenaCloseClearStep(R, ClearSideNow, FMath::Abs(ClearWant.side), DeltaSeconds);
	ClearPivotNow = ftcore::arenaCloseClearStep(R, ClearPivotNow, ClearWant.pivot, DeltaSeconds);
	Fit.camera.pivot = Shape.pivot + ClearPivotNow;
	Fit.camera.side = Shape.side + (Shape.side < 0.f ? -ClearSideNow : ClearSideNow);
	Fit.camera.fov = Shape.fov;
	CameraLens.targetX = Shape.screenX;
	CameraLens.cutMin = Shape.cutMin;
}

void UFTArenaRunner::PickCloseShoulder(int32 Target)
{
	const ftcore::ArenaCloseRules& R = ftcore::kArenaClose;
	const bool bNewTarget = Target != CloseShoulderFor;
	CloseShoulderFor = Target;
	CloseNextPickAt = Fight.time() + 0.5f;
	// Each side at the view the lock settles to: the line to the target, the range's shape and that side's own framing
	// turn (ftcore::arenaFrameTurnAt, the target's share of the width mirrored with the side).
	const ftcore::Vec2& E = Bodies.enemies[static_cast<size_t>(Target)];
	const ftcore::Vec2 ToFoe{E.x - Bodies.player.x, E.y - Bodies.player.y};
	const float Dist = ArenaLength(ToFoe);
	const float Line = Dist > 0.3f ? ArenaYaw(ToFoe) : CameraYaw;
	const float Settle = ftcore::arenaCloseMeleeWant(R, Dist);
	const ftcore::Settings* Prefs = GetSettings();
	const bool bFraming = (!Prefs || Prefs->lockFraming) && !ArenaAim.aiming();
	float Room[2] = {1.f, 1.f}; // the share of the line from the pivot to the lens that is clear: right, left
	bool bMate[2] = {false, false};
	const AFTArenaFighter* Lead = PlayerFigure.Get();
	const USpringArmComponent* Boom = Lead ? Lead->GetBoom() : nullptr;
	UWorld* World = GetWorld();
	const bool bMateOn = bPartner && Fight.output().ally.partnerOn && Fight.allyHp() > 0;
	const ftcore::ArenaPoint3 Mate{Bodies.ally.x - Bodies.player.x, Bodies.ally.y - Bodies.player.y, 1.f};
	const float Aspect = LockAspect();
	const float ArmNow = Boom ? Boom->TargetArmLength / 100.f : Fit.camera.arm;
	const float Back = ftcore::arenaEyeBack(ArmNow, Fit.camera.up, ftcore::arenaSplitPitch(ftcore::kArenaLook, CameraPitch).boom);
	// In contact range each side is tried where its lens would be at that side's swing.
	const float Contact = ArenaAim.aiming() ? 0.f : ftcore::arenaCloseContactWant(R, Dist, CloseHeightNow, CloseRadiusNow);
	for (int32 K = 0; K < 2; ++K)
	{
		const float S = K == 0 ? 1.f : -1.f;
		const ftcore::ArenaCloseShape Shape = ftcore::arenaCloseShape(R, CloseBase, Settle, CloseTall, S, 0.f);
		ftcore::ZoneCamera Rig = Fit.camera;
		Rig.pivot = Shape.pivot;
		Rig.side = Shape.side;
		Rig.fov = Shape.fov;
		float Turn = bFraming ? ftcore::arenaFrameTurnAt(FrameRules(), Shape.side, Dist, Shape.screenX, Shape.fov, Back) : 0.f;
		Turn += ftcore::arenaCloseContactYaw(Turn, ftcore::arenaCloseContactSwing(R, S, Shape.side, Dist, CloseRadiusNow, Shape.fov, Back), Contact);
		const ftcore::ArenaCameraPose P = ftcore::arenaCameraPose(Rig, ArenaAim, ftcore::ArenaFrameYaw{Line, Turn}, CameraPitch);
		if (Boom && World)
		{
			// The boom's own probe, swept from its origin (at that pivot) to where that side's lens would be.
			const ftcore::ArenaLensAt At = ftcore::arenaCameraLens(P);
			const FVector From = Boom->GetComponentLocation() + Boom->TargetOffset + FVector(0.0, 0.0, (Shape.pivot - Fit.camera.pivot) * 100.0);
			const FVector LensAt = From + FVector(At.x, At.y, At.z) * 100.0;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(ArenaCloseShoulder), false, Lead);
			FHitResult Hit;
			if (World->SweepSingleByChannel(Hit, From, LensAt, FQuat::Identity, Boom->ProbeChannel, FCollisionShape::MakeSphere(Boom->ProbeSize), Params))
			{
				Room[K] = Hit.Time;
			}
		}
		if (bMateOn)
		{
			const ftcore::ArenaFramePoint F = ftcore::arenaFramePoint(P, Aspect, Mate);
			bMate[K] = F.ahead && F.x >= ftcore::kPartnerFrustumMargin && F.x <= 1.f - ftcore::kPartnerFrustumMargin;
		}
	}
	// A wall's re-pick reads the room alone: with both sides open or both blocked the side stays (no swap on the
	// partner's place that no wall forced).
	const float Want = ftcore::arenaCloseShoulderPick(R, Room[0], Room[1], bNewTarget && bMate[0], bNewTarget && bMate[1], CloseShoulderWant);
#if !UE_BUILD_SHIPPING
	if (bNewTarget || Want != CloseShoulderWant)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: close shoulder %s at %.2f s for target %d (%s; room right %.2f, left %.2f; partner in frame right %d, left %d)."),
			Want < 0.f ? TEXT("left") : TEXT("right"), Fight.time(), Target, bNewTarget ? TEXT("lock") : TEXT("wall"), Room[0], Room[1],
			bMate[0] ? 1 : 0, bMate[1] ? 1 : 0);
	}
	CloseSwaps += Want != CloseShoulderWant ? 1 : 0;
	CloseContactSwaps += Want != CloseShoulderWant && CloseContact > 0.5f ? 1 : 0;
#endif
	CloseShoulderWant = Want;
}

#if !UE_BUILD_SHIPPING
void UFTArenaRunner::LogCloseFrameShot(const TCHAR* Shot) const
{
	if (!bCloseShape || !bCameraPoseNow)
	{
		return;
	}
	// What the pass bar reads, from the drawn lens (the partner's turn included): the lead's spine, the target's centre
	// and how much of the target's width the lead covers, and the partner with its place in the lens's ground frame.
	const ftcore::ArenaCameraPose& Lens = CameraLensNow;
	const float Aspect = LockAspect();
	const ftcore::ArenaLeadShares S = ftcore::arenaLeadShares(Lens, Aspect, LeadBody());
	float TargetX = -1.f, Overlap = -1.f;
	ftcore::ArenaBodySpan Span;
	if (CloseTargetNow >= 0 && CloseTargetNow < static_cast<int32>(Fight.enemyCount()))
	{
		const size_t T = static_cast<size_t>(CloseTargetNow);
		const ftcore::Vec2& E = Bodies.enemies[T];
		const ftcore::ArenaEnemyKind& Foe = Fight.def().enemies[T];
		const float X = E.x - Bodies.player.x, Y = E.y - Bodies.player.y, Z = CloseHeightNow * 0.5f;
		const float Rad = Foe.bodyRadius * (Foe.look.skinned() ? Foe.look.scale : 1.f);
		const float Yaw = FMath::DegreesToRadians(Lens.yaw);
		const float Rx = -FMath::Sin(Yaw) * Rad, Ry = FMath::Cos(Yaw) * Rad; // the view's right, the body's radius long
		const ftcore::ArenaFramePoint C = ftcore::arenaFramePoint(Lens, Aspect, ftcore::ArenaPoint3{X, Y, Z});
		const ftcore::ArenaFramePoint L = ftcore::arenaFramePoint(Lens, Aspect, ftcore::ArenaPoint3{X - Rx, Y - Ry, Z});
		const ftcore::ArenaFramePoint Rt = ftcore::arenaFramePoint(Lens, Aspect, ftcore::ArenaPoint3{X + Rx, Y + Ry, Z});
		if (C.ahead && L.ahead && Rt.ahead)
		{
			TargetX = C.x;
			const float Lo = FMath::Min(L.x, Rt.x), Hi = FMath::Max(L.x, Rt.x);
			Overlap = FMath::Max(0.f, FMath::Min(S.right, Hi) - FMath::Max(S.left, Lo)) / FMath::Max(1e-4f, Hi - Lo);
		}
		Span = ftcore::arenaBodySpan(Lens, Aspect, ftcore::ArenaPoint3{X, Y, Z}, Rad); // its outline from the lens
	}
	const bool bMateOn = bPartner && Fight.output().ally.partnerOn && Fight.allyHp() > 0;
	const ftcore::ArenaPoint3 Mate{Bodies.ally.x - Bodies.player.x, Bodies.ally.y - Bodies.player.y, 1.f};
	const ftcore::ArenaFramePoint F = ftcore::arenaFramePoint(Lens, Aspect, Mate);
	const ftcore::ArenaLensGround G = ftcore::arenaLensGround(Lens, Mate);
	// In frame under the close lens: the partner's head and waist both inside the frame (its feet are not tested).
	const auto Inside = [](const ftcore::ArenaFramePoint& P) { return P.ahead && P.x >= 0.f && P.x <= 1.f && P.y >= 0.f && P.y <= 1.f; };
	const ftcore::ArenaFramePoint MateHead = ftcore::arenaFramePoint(Lens, Aspect, ftcore::ArenaPoint3{Mate.x, Mate.y, CloseMateHead});
	const ftcore::ArenaFramePoint MateWaist = ftcore::arenaFramePoint(Lens, Aspect, ftcore::ArenaPoint3{Mate.x, Mate.y, CloseMateWaist});
	const bool bMateIn = bMateOn && Inside(MateHead) && Inside(MateWaist);
	UE_LOG(LogForgottenThird, Display,
		TEXT("Arena: close frame at view %s: distance %.2f m, melee %.2f, shoulder %+.2f, fov %.1f; lead spine x %.3f (left %.3f, right %.3f); target x %.3f, overlap %.3f of its width; partner %s x %.3f (%.2f m ahead of the lens, %.2f m right), head y %.3f, waist y %.3f, turn %.1f (-1: not measured); contact %.2f, swing %.1f, pitch %.1f (stand-off %.1f on its %s), arm %.2f; target body x %.3f to %.3f (%s)."),
		Shot, CloseDistanceNow, CloseMelee, CloseShoulder, Lens.fov, 0.5f * (S.left + S.right), S.left, S.right, TargetX, Overlap,
		!bMateOn ? TEXT("off") : (bMateIn ? TEXT("in frame") : TEXT("out of frame")), F.ahead ? F.x : -1.f, G.ahead, G.right,
		MateHead.ahead ? MateHead.y : -1.f, MateWaist.ahead ? MateWaist.y : -1.f, PartnerYawNow,
		CloseContact, ContactYawNow, CameraPitch, StandOffPitchNow, bStandOffWeak ? TEXT("weak point") : TEXT("top"), Lens.arm,
		Span.left, Span.right, Span.ahead ? TEXT("ahead") : TEXT("not ahead"));
}
#endif

float UFTArenaRunner::LockAimHeight(int32 Target, float Base, float Top) const
{
	const AFTArenaFighter* Foe = GetEnemyFigure(Target);
	const AFTArenaFighter* Lead = PlayerFigure.Get();
	const double Floor = Lead ? Lead->GetActorLocation().Z : 0.0;
	float Tell = -1.f, Weak = -1.f;
	const ftcore::ArenaEnemyOut& E = Fight.output().enemies[static_cast<size_t>(Target)];
	if (Foe && (E.part == ftcore::ArenaPart::Startup || E.part == ftcore::ArenaPart::Active))
	{
		Tell = static_cast<float>(Foe->GetAnchor().Z - Floor) / 100.f; // the telegraph's bar (the HUD's DrawTell)
	}
	FVector At;
	if (GetBossWeakPoint(Target, At))
	{
		Weak = static_cast<float>(At.Z - Floor) / 100.f;
	}
	return ftcore::arenaLockAimHeight(Base, Top, Tell, Weak);
}

void UFTArenaRunner::LogLeadFrameShot(const TCHAR* Shot) const
{
#if !UE_BUILD_SHIPPING
	LogCloseFrameShot(Shot);
	if (!bLeadShotSampled)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: lead frame at view %s: not held (no lock on a tall target)."), Shot);
		return;
	}
	const ftcore::ArenaLeadShares& S = LeadShotShares;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: lead frame at view %s: %s (feet %.3f, head %.3f, left %.3f, right %.3f, ahead %d, visible %d)."),
		Shot, ftcore::arenaLeadInFrame(LeadFrameRules(), S) ? TEXT("kept") : TEXT("BROKE the rule"), S.feet, S.head, S.left, S.right,
		S.ahead ? 1 : 0, ftcore::arenaLeadVisible(S) ? 1 : 0);
#else
	(void)Shot;
#endif
}

#if !UE_BUILD_SHIPPING
void UFTArenaRunner::ResetLeadFrameChecks()
{
	LeadFrameFrames = LeadFrameKept = LeadFrameVisible = TellFrames = TellInPlace = TellPinned = TellEdge = 0;
	LeadFeetLeast = 1.f;
	LeadHeadMost = LeadSideWorst = 0.f;
	for (int32 I = 0; I < 3; ++I)
	{
		LeadMissWorst[I] = 0.f;
		LeadMissAt[I] = -1.f;
	}
}

void UFTArenaRunner::SampleLeadFrame()
{
	const APlayerController* PC = GetPC();
	const AFTArenaFighter* Lead = PlayerFigure.Get();
	if (!PC || !PC->PlayerCameraManager || !Lead)
	{
		return;
	}
	const APlayerCameraManager* Cam = PC->PlayerCameraManager;
	const FVector Eye = Cam->GetCameraLocation();
	const FRotator Rot = Cam->GetCameraRotation();
	const double TanH = FMath::Tan(FMath::DegreesToRadians(static_cast<double>(Cam->GetFOVAngle()) * 0.5));
	const double TanV = TanH / static_cast<double>(LockAspect());
	const FVector Right = FRotationMatrix(Rot).GetUnitAxis(EAxis::Y) * static_cast<double>(LeadBody().radius) * 100.0;
	// The low points at the preset's cut, as ftcore::arenaLeadShares takes them (classic: the feet themselves).
	const FVector Feet = Lead->GetActorLocation() + FVector(0.0, 0.0, static_cast<double>(LeadBody().cut) * 100.0);
	const FVector Head = Lead->GetAnchor() - FVector(0.0, 0.0, 45.0);
	const FVector Points[6] = {Feet, Head, Feet + Right, Feet - Right, Head + Right, Head - Right};
	ftcore::ArenaLeadShares S;
	S.ahead = true;
	S.feet = S.left = 1e9f;
	S.head = S.right = -1e9f;
	for (const FVector& P : Points)
	{
		const FVector V = Rot.UnrotateVector(P - Eye);
		if (V.X <= 1.0)
		{
			S.ahead = false;
			break;
		}
		const float X = static_cast<float>(0.5 * (1.0 + V.Y / (V.X * TanH)));
		const float Y = static_cast<float>(0.5 * (1.0 + V.Z / (V.X * TanV)));
		S.feet = FMath::Min(S.feet, Y);
		S.head = FMath::Max(S.head, Y);
		S.left = FMath::Min(S.left, X);
		S.right = FMath::Max(S.right, X);
	}
	bLeadShotSampled = LockTarget >= 0;
	LeadShotShares = S;
	if (LockTarget < 0)
	{
		return; // only a tall target's hold is the check's (a short one keeps the lead by the framing already)
	}
	++LeadFrameFrames;
	const ftcore::ArenaLeadFrameRules LeadRules = LeadFrameRules();
	LeadFrameKept += ftcore::arenaLeadInFrame(LeadRules, S) ? 1 : 0;
	LeadFrameVisible += ftcore::arenaLeadVisible(S) ? 1 : 0;
	{
		// How far past the rule (0: kept), into the three worst.
		const float Edge = 0.5f * (1.f - LeadRules.widthMid);
		const float Miss = !S.ahead ? 1.f + FMath::Max(0.f, 1.f - S.feet)
			: FMath::Max(FMath::Max(LeadRules.feetMin - S.feet, S.head - LeadRules.headMax),
				FMath::Max(Edge - S.left, S.right - (1.f - Edge)));
		for (int32 I = 0; Miss > 0.f && I < 3; ++I)
		{
			if (Miss > LeadMissWorst[I])
			{
				for (int32 J = 2; J > I; --J)
				{
					LeadMissWorst[J] = LeadMissWorst[J - 1];
					LeadMissAt[J] = LeadMissAt[J - 1];
				}
				LeadMissWorst[I] = Miss;
				LeadMissAt[I] = Fight.time();
				break;
			}
		}
	}
	if (S.ahead)
	{
		LeadFeetLeast = FMath::Min(LeadFeetLeast, S.feet);
		LeadHeadMost = FMath::Max(LeadHeadMost, S.head);
		LeadSideWorst = FMath::Max(LeadSideWorst, FMath::Max(0.5f - S.left, S.right - 0.5f));
	}
	// The telegraph through a windup: its bar over the body, pinned under the top edge (the HUD's DrawTell), or the
	// edge marker when the body is out of the frame.
	const ftcore::ArenaEnemyOut& E = Fight.output().enemies[static_cast<size_t>(LockTarget)];
	const AFTArenaFighter* Foe = GetEnemyFigure(LockTarget);
	if (Foe && E.part == ftcore::ArenaPart::Startup)
	{
		++TellFrames;
		const FVector C = Rot.UnrotateVector(Foe->GetChest() - Eye);
		const FVector O = Rot.UnrotateVector(Foe->GetAnchor() - Eye);
		const bool bChestIn = C.X > 1.0 && FMath::Abs(C.Y / (C.X * TanH)) <= 1.0 && FMath::Abs(C.Z / (C.X * TanV)) <= 1.0;
		if (!bChestIn || IsOffFrame(LockTarget))
		{
			++TellEdge;
		}
		else if (O.X > 1.0 && O.Z / (O.X * TanV) < 0.8)
		{
			++TellInPlace;
		}
		else
		{
			++TellPinned;
		}
	}
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
	if (LockLow > 0.f || bLowRig)
	{
		return; // SteerCamera gives the boom the low rig, blended with the stance's, while the low lens is on
	}
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
#if !UE_BUILD_SHIPPING
	if (bLeadHeld)
	{
		SampleLeadFrame(); // the frame the last step's hold drew, as the rendered camera shows it
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
	if (bCloseShape)
	{
		ShapeCloseLens(Target, DeltaSeconds); // the close rig at this range and shoulder, before anything below reads it
	}
	// The yaw: the lock's line (CameraYaw, eased onto the target) and the framing's orbit on it (FrameTurn), both stepped
	// by ftcore::arenaFrameYawStep. The view is drawn at their sum (ViewYaw), and the stick, the crosshair and the fight's
	// view read that, so the lead goes where the stick points on the screen, and a release (the turn folded into the line)
	// leaves the stick's basis where it was.
	ftcore::ArenaFrameScene Scene;
	const ftcore::Settings* Prefs = GetSettings();
	Scene.framing = !Prefs || Prefs->lockFraming;
	Scene.aiming = ArenaAim.aiming(); // the aim camera frames the shot over the shoulder
	// The shoulder and the distance back as the boom really has them: its wall probe shortens the arm, the shoulder and the
	// height alike (BoomWallShare), and a shorter shoulder sends the target further across, so the turn is solved on that.
	const float TurnWallShare = BoomWallShare();
	Scene.side = Fit.camera.side * TurnWallShare;
	Scene.lineRate = kArenaLockTurn;
	// The preset's share of the width the turn puts the target at (classic: 0.5, the centre line, and then the lens and
	// its distance back are not read), for the lens as the boom has it now.
	Scene.screenX = CameraLens.targetX;
	Scene.fov = Fit.camera.fov;
	Scene.back = ftcore::arenaEyeBack(ArmNow * TurnWallShare, Fit.camera.up * TurnWallShare,
		ftcore::arenaSplitPitch(ftcore::kArenaLook, CameraPitch).boom);
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
	if (bCloseShape && bFollowed)
	{
		// Contact: the swing round the lead toward the open side (ftcore::arenaCloseContactSwing), added to the drawn view
		// like the partner's turn and blended from the framing turn by the contact share (0 exactly off contact).
		const float Swing = ftcore::arenaCloseContactSwing(ftcore::kArenaClose, CloseShoulder, Scene.side, Scene.distance, CloseRadiusNow,
			Fit.camera.fov, Scene.back);
		ContactYawNow = ftcore::arenaCloseContactYaw(FrameTurn, Swing, CloseContact);
	}
#if !UE_BUILD_SHIPPING
	const float FrameWant = bFollowed && Scene.framing && !Scene.aiming
		? ftcore::arenaFrameTurnAt(FrameRules(), Scene.side, Scene.distance, Scene.screenX, Scene.fov, Scene.back) : 0.f;
#endif
	// The lens as the boom really has it: its wall probe shortens the arm, the shoulder and the height alike, by this
	// share (measured on the boom's last update). The lead's rule and the fed camera are solved on that lens; the boom
	// is given its own lengths (its probe shortens them again).
	const float WallShare = BoomWallShare();
	const float Aspect = LockAspect();
	const ftcore::ArenaLeadBody Body = LeadBody();
	const ftcore::ArenaCameraPose LensRig = WallScaled(LockRig(), WallShare);
	ftcore::ArenaLowRules SideRules = ftcore::kArenaLow; // the low shoulder's size at this arm (its side picked below)
	SideRules.side = ftcore::kArenaLow.side * WallShare;
	bool bTallLock = false; // a tall target followed outside the aim stance: the lens goes low, the aim is its read point
	float AimHeight = 0.f;
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
		bTallLock = Tall >= kArenaTallBoss && ArenaAim.alpha() <= 0.f;
		if (bTallLock)
		{
			// The lock on a tall target: the look through what the player must read (the telegraph, else the weak point,
			// else the chest; ftcore::arenaLockAimPitch) from the low lens, held to the lead's rule below. The low
			// shoulder goes to the side that keeps that point clear of the lead (ftcore::arenaLowSide, with hysteresis),
			// and the low share is no more than the arm allows (ftcore::arenaLowFit, worked out again only when the arm,
			// the aspect or the lead changes).
			AimHeight = LockAimHeight(Target, Hover, Tall);
			const ftcore::ArenaPoint3 Aim{To.x, To.y, AimHeight};
			if (LensRig.arm < ftcore::kArenaHeadKeptArm && !bShortArmLogged)
			{
				// Under ftcore::kArenaHeadKeptArm no pitch keeps the whole head (the lock frame's known limit): said once a fight.
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: camera arm %.2f m at %.2f s, under %.2f m: the lead's head top can leave the frame."),
					LensRig.arm, Fight.time(), ftcore::kArenaHeadKeptArm);
				bShortArmLogged = true;
			}
			LowSideSign = ftcore::arenaLowSide(ftcore::kArenaLook, LensRig, CameraPitch, Aspect, Body, Aim, LowSideSign, ftcore::kArenaLowSide, SideRules);
			if (FMath::Abs(LensRig.arm - LowFitArm) > 0.05f || Aspect != LowFitAspect || Body.head != LowFitHead)
			{
				// No more than the preset allows (CameraPreset::lowMax: classic 1, which every fit is under; close 0).
				LowFit = ftcore::cameraPresetLowCap(CameraLens, ftcore::arenaLowFit(ftcore::kArenaLook, LeadFrameRules(), LensRig, Aspect, Body, SideRules));
				LowFitArm = LensRig.arm;
				LowFitAspect = Aspect;
				LowFitHead = Body.head;
			}
			ftcore::ArenaLowRules WantRules = SideRules;
			WantRules.side = LowSideNow * WallShare;
			WantPitch = ftcore::arenaLockAimPitch(ftcore::kArenaLook, LensRig, Aspect, Aim, Rig.lockPitch, LockLow, WantRules);
		}
		if (bCloseShape)
		{
			// Stand-off: the least look-up from the preset's own pitch that keeps the target's top (its weak point when it
			// has one) under the frame's top margin, never over +12 (ftcore::arenaCloseStandOffPitch, on the lens as drawn).
			// In melee range the lens looks up toward the target's head from there (ftcore::arenaCloseMeleePitch). In
			// contact the lock's own look-up (the aim pitch on a tall target) is held to 15-25 degrees, blended in by the
			// contact share (ftcore::arenaCloseContactPitch), and the cut is raised to allow it. The lead's rule holds it below.
			ftcore::ArenaPoint3 Top{To.x, To.y, Tall};
			FVector Weak;
			const AFTArenaFighter* Lead = PlayerFigure.Get();
			bStandOffWeak = Lead && GetBossWeakPoint(Target, Weak);
			if (bStandOffWeak)
			{
				const FVector Rel = (Weak - Lead->GetActorLocation()) / 100.0;
				Top = ftcore::ArenaPoint3{static_cast<float>(Rel.X), static_cast<float>(Rel.Y), static_cast<float>(Rel.Z)};
			}
			StandOffPitchNow = ftcore::arenaCloseStandOffPitch(ftcore::kArenaClose, ftcore::kArenaLook, LensRig, Aspect, Top, Rig.lockPitch);
			const float Off = ftcore::arenaCloseMeleePitch(ftcore::kArenaClose, ftcore::kArenaLook, Rig.pivot, ArmNow, Rig.up, ArenaLength(To), Tall,
				CloseMelee, StandOffPitchNow);
			const float Held = ftcore::arenaCloseMeleePitch(ftcore::kArenaClose, ftcore::kArenaLook, Rig.pivot, ArmNow, Rig.up, ArenaLength(To), Tall,
				CloseMelee, WantPitch);
			WantPitch = ftcore::arenaCloseContactPitch(ftcore::kArenaClose, Off, Held, CloseContact);
			// Tall contact looks up from the low lens instead, at most 15 degrees (ftcore::arenaCloseContactLowPitch).
			WantPitch = ftcore::arenaCloseContactLowPitch(ftcore::kArenaClose, WantPitch, CloseContactLow);
		}
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
	// The lead's rule (ftcore::arenaLeadPitch): while a target is followed outside the aim stance, the pitch is held
	// where the controlled lead's whole body stays in the frame (feet over 6% of its height, head under 55%, the body in
	// the middle 70% of its width), from the lens as the boom has it. A tall target brings the lens low and to the
	// side (ftcore::kArenaLow, eased): it looks up from there, never from further back. The aim stance and the free
	// look (nothing followed) are outside the rule. On an ordinary lock with the lead in frame it changes nothing.
	bool bHold = bFollowed && ArenaAim.alpha() <= 0.f;
#if !UE_BUILD_SHIPPING
	bHold = bHold && AimCheckPhase != 2 && AimCheckPhase != 3 && AimCheckPhase != 5; // the checks measure the plain lock view
#endif
	// Never more than the arm allows this frame: a wall that shortens the arm cuts the share at once, it never eases down.
	LockLow = FMath::Min(LockLow, LowFit);
	LockLow = FMath::FInterpConstantTo(LockLow, bHold && bTallLock ? LowFit : 0.f, DeltaSeconds, ftcore::kArenaLow.rate);
	// The low shoulder crosses over when its side flips (eased), and snaps while the low lens is off (nothing to see).
	const float LowSideWant = LowSideSign * ftcore::kArenaLow.side;
	LowSideNow = LockLow > 0.f ? FMath::FInterpConstantTo(LowSideNow, LowSideWant, DeltaSeconds, kArenaLowSideRate) : LowSideWant;
	ftcore::ArenaLowRules LowRules = SideRules;
	LowRules.side = LowSideNow * WallShare;
	// Tall contact on the close lens: the lens drops 0.9 m over the floor on the shoulder's own side (the share eased with
	// the contact share). The close preset's lock low lens is off (lowMax 0), so the two never mix.
	float LowNow = LockLow;
	if (bCloseShape && CloseContactLow > LockLow)
	{
		LowNow = CloseContactLow;
		LowRules = ftcore::arenaCloseContactLowRules(ftcore::kArenaClose, LensRig.side);
	}
	ftcore::ArenaCameraPose Pose = ftcore::arenaCameraPose(Fit.camera, ArenaAim, ftcore::ArenaFrameYaw{CameraYaw, FrameTurn}, CameraPitch);
	ftcore::ArenaCameraPose Lens = WallScaled(Pose, WallShare);
	if (bHold || LowNow > 0.f)
	{
		if (bHold)
		{
			const float Asked = CameraPitch;
			CameraPitch = ftcore::arenaLeadPitch(ftcore::kArenaLook, LeadFrameRules(), LensRig, Aspect, Body, CameraPitch, LowNow, LowRules).pitch;
			if (bCloseShape)
			{
				// A wall-shortened close lens keeps the cut only by diving (ftcore::arenaCloseWallPitch): held at -6 or over,
				// the cut gives way instead.
				const float Held = ftcore::arenaCloseWallPitch(ftcore::kArenaClose, CameraPitch, Asked);
#if !UE_BUILD_SHIPPING
				CloseWallHeld += Held != CameraPitch ? 1 : 0;
#endif
				CameraPitch = Held;
			}
		}
		Lens = ftcore::arenaPoseAtPitch(ftcore::kArenaLook, LensRig, CameraPitch, LowNow, LowRules);
		Pose = WallScaled(Lens, 1.f / WallShare); // the boom's own lengths: its probe shortens them to the lens solved
	}
	// The contact swing (0 off contact), before the partner's turn is solved on the lens it turns.
	Pose.yaw += ContactYawNow;
	Lens.yaw += ContactYawNow;
	// The partner as a soft term (ftcore::arenaPartnerYaw): a turn of at most kArenaPartnerYaw.maxYaw (5) degrees that
	// brings it into the frame while the lead keeps its rule, eased at kPartnerYawRate per second with a half-degree
	// deadband so it does not hunt, and off (eased back to 0) for kPartnerYawQuiet s after the player turns the view.
	float YawWant = 0.f;
	const bool bLookedLately = Fight.time() - LastLookInputAt < kPartnerYawQuiet;
	// No partner turn in contact: the swing has the target outside the band of the width the turn keeps it in.
	if (bHold && !bLookedLately && bPartner && Fight.output().ally.partnerOn && Fight.allyHp() > 0 && !(CloseContact > 0.f))
	{
		const ftcore::ArenaPoint3 Mate{Bodies.ally.x - Bodies.player.x, Bodies.ally.y - Bodies.player.y, 1.f};
		// The close lens turns up to ftcore::kArenaClose.partnerYawMax (12) degrees, the target kept in its band of the width;
		// for a ranged partner (its kit's own range handed to the brain with the close station) up to partnerYawRangedMax
		// (12 as ruled; the switch may widen it), never growing during the lead's attack or a dodge's first moment
		// (ftcore::arenaClosePartnerYawCap).
		if (bCloseShape)
		{
			const ftcore::ArenaAction Act = Fight.output().player.action;
			const bool bLeadBusy = Act == ftcore::ArenaAction::Light || Act == ftcore::ArenaAction::Heavy
				|| Act == ftcore::ArenaAction::Riposte || Act == ftcore::ArenaAction::Release
				|| (Act == ftcore::ArenaAction::Dodge && ftcore::arenaCloseDodgeHolds(ftcore::kArenaClose, Fight.output().player.actionTime));
			ftcore::ArenaCloseRules Rules = ftcore::kArenaClose;
#if !UE_BUILD_SHIPPING
			if (PartnerYawCapSwitch > 0.f)
			{
				Rules.partnerYawRangedMax = PartnerYawCapSwitch;
			}
#endif
			Rules.partnerYawMax = ftcore::arenaClosePartnerYawCap(Rules, RangedPartner(), bLeadBusy, PartnerYawNow);
			YawWant = ftcore::arenaClosePartnerYaw(Rules, LeadFrameRules(), Lens, Aspect, Body, Mate,
				ftcore::ArenaPoint3{To.x, To.y, FMath::Clamp(CloseHeightNow * 0.5f, 0.5f, 2.f)}, CloseShoulder);
		}
		else
		{
			YawWant = ftcore::arenaPartnerYaw(ftcore::kArenaPartnerYaw, LeadFrameRules(), Lens, Aspect, Body, Mate);
		}
	}
	if (FMath::Abs(YawWant - PartnerYawGoal) > 0.5f || YawWant == 0.f)
	{
		PartnerYawGoal = YawWant;
	}
	PartnerYawNow = FMath::FInterpTo(PartnerYawNow, PartnerYawGoal, DeltaSeconds, kPartnerYawRate);
	Pose.yaw += PartnerYawNow;
	Lens.yaw += PartnerYawNow;
#if !UE_BUILD_SHIPPING
	if (bHold && bPartner && Fight.output().ally.partnerOn && Fight.allyHp() > 0)
	{
		const ftcore::ArenaPoint3 Mate{Bodies.ally.x - Bodies.player.x, Bodies.ally.y - Bodies.player.y, 1.f};
		const ftcore::ArenaFramePoint F = ftcore::arenaFramePoint(Lens, Aspect, Mate);
		const ftcore::ArenaLensGround G = ftcore::arenaLensGround(Lens, Mate);
		++MateFrames;
		const bool bMateIn = F.ahead && F.x >= 0.f && F.x <= 1.f && F.y >= 0.f && F.y <= 1.f;
		if (bCloseShape && CloseContactLow > 0.5f)
		{
			++CloseTallMateFrames;
			CloseTallMateIn += bMateIn ? 1 : 0;
		}
		if (bMateIn)
		{
			++MateIn;
		}
		else
		{
			MateBehind += !F.ahead ? 1 : 0;
			MateLeft += F.ahead && F.x < 0.f ? 1 : 0;
			MateRight += F.ahead && F.x > 1.f ? 1 : 0;
			MateOffY += F.ahead && F.x >= 0.f && F.x <= 1.f ? 1 : 0;
			MateOutAhead += G.ahead;
			MateOutRight += G.right;
		}
	}
	if (bCloseShape && bHold)
	{
		++CloseYawFrames;
		CloseYawOn += FMath::Abs(PartnerYawNow) > 0.25f ? 1 : 0;
		CloseYawSum += FMath::Abs(PartnerYawNow);
		CloseYawMost = FMath::Max(CloseYawMost, FMath::Abs(PartnerYawNow));
		if (RangedPartner() && bPartner && Fight.output().ally.partnerOn && Fight.allyHp() > 0)
		{
			++CloseRangedFrames;
			CloseRangedWide += FMath::Abs(PartnerYawNow) > ftcore::kArenaClose.partnerYawMax + 0.25f ? 1 : 0;
			CloseRangedYawMost = FMath::Max(CloseRangedYawMost, FMath::Abs(PartnerYawNow));
		}
		if (CloseContact > 0.f)
		{
			++CloseContactFrames;
			CloseContactYawMost = FMath::Max(CloseContactYawMost, FMath::Abs(ContactYawNow));
		}
		else if (CloseTargetNow >= 0)
		{
			// Stand-off: the target's head in the drawn lens, clear of the lead's box (at the clearance's 0.45 m, no margin).
			++CloseClearFrames;
			CloseClearSeen += ftcore::arenaCloseHeadHidden(Lens, Aspect, ftcore::arenaCloseClearBody(ftcore::kArenaClose, Body),
				ftcore::ArenaPoint3{To.x, To.y, CloseHeadHeight()}, 0.f) ? 0 : 1;
			CloseClearSideMost = FMath::Max(CloseClearSideMost, FMath::Abs(ClearSideNow));
			CloseClearPivotMost = FMath::Max(CloseClearPivotMost, ClearPivotNow);
		}
		if (CloseContactLow > 0.5f)
		{
			const ftcore::ArenaLeadShares Sh = ftcore::arenaLeadShares(Lens, Aspect, Body);
			++CloseTallFrames;
			CloseTallHeadMost = FMath::Max(CloseTallHeadMost, Sh.ahead ? Sh.head : 1.f);
			CloseTallWaistLeast = FMath::Min(CloseTallWaistLeast, Sh.ahead ? Sh.feet : 0.f);
			CloseTallPitchMost = FMath::Max(CloseTallPitchMost, CameraPitch);
		}
		if (WallShare < 0.98f)
		{
			++CloseWallFrames;
			const ftcore::ArenaLeadShares Sh = ftcore::arenaLeadShares(Lens, Aspect, Body);
			const float Spine = 0.5f * (Sh.left + Sh.right);
			CloseWallLow += Sh.ahead && (CloseShoulder < 0.f ? 1.f - Spine : Spine) < 0.28f ? 1 : 0;
		}
	}
#endif
	LockTarget = bHold && bTallLock ? Target : -1;
	bLeadHeld = bHold;
	// The aim point above the frame: the UI's edge marker (GetLockAimAbove), where it leaves the top edge.
	bLockAimAbove = false;
	LockAimX = 0.5f;
	if (bHold && bTallLock)
	{
		const ftcore::ArenaFramePoint AimAt = ftcore::arenaFramePoint(Lens, Aspect, ftcore::ArenaPoint3{To.x, To.y, AimHeight});
		bLockAimAbove = !AimAt.ahead || AimAt.y > 1.f;
		LockAimX = AimAt.ahead ? FMath::Clamp(AimAt.x, 0.f, 1.f) : 0.5f;
	}
	// The low rig is given to the boom here, the aim stance's blend included (ApplyAimCamera stands aside while the low
	// lens is on), so the stance starting on a low lens eases out of it rather than jumping.
	if (AFTArenaFighter* LowLead = PlayerFigure.Get(); LowLead && (LowNow > 0.f || bLowRig))
	{
		LowLead->SetCameraRig(Pose.arm, Pose.pivot, Pose.side, Pose.up, Pose.fov);
		bLowRig = LowNow > 0.f;
	}
	else if (AFTArenaFighter* CloseLead = PlayerFigure.Get(); CloseLead && bCloseShape && ArenaAim.alpha() <= 0.f)
	{
		// The close lens's shaped rig (the arm is the preset's; the pivot, the shoulder and the fov move with the range and
		// the side). The aim stance blends from the same shaped rig in ApplyAimCamera.
		CloseLead->SetCameraRig(Pose.arm, Pose.pivot, Pose.side, Pose.up, Pose.fov);
	}
	PC->SetControlRotation(FRotator(Pose.boomPitch, Pose.yaw, 0.0));
#if !UE_BUILD_SHIPPING
	if (bFrameCheck)
	{
		SampleFrame(Target, FrameWant, DeltaSeconds);
	}
#endif
	CameraPoseNow = Pose;
	bCameraPoseNow = true;
	// What the fight's next step is fed (the partner's stationing and its in-play count): this frame's solved lens, the
	// arm it really has, over the lead where it stands now.
	CameraLensNow = Lens;
	FedCamera = ftcore::arenaPoseCamera(Lens, Bodies.player, Aspect, PartnerHeight());
	bFedCamera = true;
	if (AFTArenaFighter* Lead = PlayerFigure.Get())
	{
		Lead->SetCameraTilt(Pose.tilt);
		// After a switch the lead in view is not the possessed figure: its boom takes the control rotation here.
		Lead->SetBoomRotation(FRotator(Pose.boomPitch, Pose.yaw, 0.0));
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
	if (Session)
	{
		// With no partner body the lead's 0 is the old loss: the session calls it a defeat too, never a takeover or a
		// stand-up. n/a for a win.
		const bool bLost = Fight.phase() == ftcore::ArenaPhase::Defeat;
		const bool bFateOk = SessionTakeovers == 0 && SessionRises == 0 && bSessionDefeated == bLost;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: lone lead down %s (fight %s, session defeat %d, takeovers %d, rises %d)."),
			!bLost || PartnerBody() ? TEXT("n/a") : (bFateOk ? TEXT("PASS") : TEXT("FAIL")), bLost ? TEXT("lost") : (bWon ? TEXT("won") : TEXT("ended")),
			bSessionDefeated ? 1 : 0, SessionTakeovers, SessionRises);
	}
	if (bPartner)
	{
		LogPartnerChecks(bWon, Fight.phase() == ftcore::ArenaPhase::Defeat);
	}
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
	if (bPartner && !bWon && bQuitOnFinish && !bShots)
	{
		CheckPartnerRetry(); // a loss: the retry from the fight's start, checked before the run ends
	}
	QuitStatus = bWon ? 0 : 1;
	if (bQuitOnFinish && !bShots && QuitAfterFinish <= 0.f)
	{
		FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(QuitStatus), TEXT("Arena"));
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
	LeadWeapons.LogAimSummary();
	AllyWeapons.LogAimSummary();
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
	// kinds' flinchPoise, poise breaking first, or no kind that flinches, as a boss). The chances are counted after
	// the break guard, chain cap and active/armoured-attack rules; the heavy-enough blows before them are shown too.
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: flinch %s (%d of %d chances; %d blows heavy enough before the guard, chain-cap and armour rules)."),
		*Text(C.flinchCheck), S.flinches, S.flinchChances, S.flinchBlows);
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
	if (LeadFrameFrames > 0)
	{
		UE_LOG(LogForgottenThird, Display,
			TEXT("Arena check: lead in frame %s (%d frames on a tall lock, %d kept the rule (feet over %.0f%%, head under %.0f%%, the middle %.0f%%), %d whole body seen; feet least %.3f, head most %.3f, side most %.3f from the middle; telegraph frames %d: %d bar in place, %d pinned under the top edge, %d edge marker; the aim stance and the free look are outside the rule)."),
			LeadFrameKept == LeadFrameFrames ? TEXT("PASS") : TEXT("FAIL"), LeadFrameFrames, LeadFrameKept, LeadFrameRules().feetMin * 100.f,
			LeadFrameRules().headMax * 100.f, LeadFrameRules().widthMid * 100.f, LeadFrameVisible, LeadFeetLeast, LeadHeadMost,
			LeadSideWorst, TellFrames, TellInPlace, TellPinned, TellEdge);
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: lead in frame worst samples (past the rule by, at fight time): %.3f at %.2f s, %.3f at %.2f s, %.3f at %.2f s (-1 s: none)."),
			LeadMissWorst[0], LeadMissAt[0], LeadMissWorst[1], LeadMissAt[1], LeadMissWorst[2], LeadMissAt[2]);
	}
	if (MateFrames > 0)
	{
		const int32 Out = MateFrames - MateIn;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner from the drawn lens on %d held frames: %d in frame, %d behind the lens, %d off the left, %d off the right, %d over or under; out of frame its mean place %.2f m ahead of the lens, %.2f m right."),
			MateFrames, MateIn, MateBehind, MateLeft, MateRight, MateOffY, Out > 0 ? MateOutAhead / static_cast<float>(Out) : 0.f,
			Out > 0 ? MateOutRight / static_cast<float>(Out) : 0.f);
	}
	if (MateCastSteps > 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner facing in its strings on %d steps: Core's facing off its target by over 30 degrees on %d (worst %.0f); drawn after placing on %d frames, off Core's by over 30 once the blow is out on %d (worst %.0f), in its startup on %d (turning in at %.0f deg/s at most, logged only)."),
			MateCastSteps, MateCastCoreOff, MateCastCoreWorst, MateDrawFrames, MateDrawOff, MateDrawWorst, MateDrawTurning, kArenaAllyTurnRate);
	}
	if (MateHeadFrames > 0 || MateStartsWalk + MateStartsStill > 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: close partner drawn at Core's heading on %d walking frames out of its strings and guards: off it by over 30 degrees on %d, mean %.1f, worst %.1f; string starts off a walk %d, with a visible pause (stood over 0.1 s) %d (mean stand %.2f s), from a standstill %d; Core's heading over 60 degrees off its target at a start %d."),
			MateHeadFrames, MateHeadOff, MateHeadFrames > 0 ? MateHeadSum / static_cast<float>(MateHeadFrames) : 0.f, MateHeadWorst,
			MateStartsWalk, MateStartsPaused, MateStartsPaused > 0 ? MateStandSum / static_cast<float>(MateStartsPaused) : 0.f, MateStartsStill, MateStartsWide);
	}
	if (bCloseShape)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: close lens partner turn on %d of %d held frames, mean %.2f, most %.2f degrees; shoulder swaps %d, ends %+.2f."),
			CloseYawOn, CloseYawFrames, CloseYawFrames > 0 ? CloseYawSum / static_cast<float>(CloseYawFrames) : 0.f, CloseYawMost, CloseSwaps, CloseShoulder);
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: close lens ranged partner (its kit's own range on) on %d held frames, turn most %.1f (cap %.1f), %d of them turned past the melee %.0f degrees."),
			CloseRangedFrames, CloseRangedYawMost, PartnerYawCapSwitch > 0.f ? PartnerYawCapSwitch : ftcore::kArenaClose.partnerYawRangedMax,
			CloseRangedWide, ftcore::kArenaClose.partnerYawMax);
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: close lens wall-shortened (boom under 98%%) on %d of %d held frames, %d of them with the lead's spine under 28%% of the width; the lead's rule held off a dive under -6 degrees on %d frames."),
			CloseWallFrames, CloseYawFrames, CloseWallLow, CloseWallHeld);
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: close lens contact on %d of %d held frames (left out of the frame check), swing most %.1f degrees, %d shoulder swaps in contact."),
			CloseContactFrames, CloseYawFrames, CloseContactYawMost, CloseContactSwaps);
		const float ClearShare = CloseClearFrames > 0 ? 100.f * static_cast<float>(CloseClearSeen) / static_cast<float>(CloseClearFrames) : 100.f;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: close lens target head clear of the lead %s on %d of %d stand-off frames (%.1f%%, bar 95%%), the clearance at most %.2f m side, %.2f m pivot."),
			ClearShare >= 95.f ? TEXT("PASS") : TEXT("FAIL"), CloseClearSeen, CloseClearFrames, ClearShare, CloseClearSideMost, CloseClearPivotMost);
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: close lens tall contact (low lens over half) on %d held frames: lead's head at most %.0f%% of the height, waist at least %.0f%%, pitch at most %.1f degrees; partner in the frame on %d of %d of them (exempt, logged only)."),
			CloseTallFrames, CloseTallHeadMost * 100.f, CloseTallWaistLeast * 100.f, CloseTallPitchMost, CloseTallMateIn, CloseTallMateFrames);
	}
	if (bFrameCheck)
	{
		const ftcore::ZoneCamera& Rig = Fit.camera;
		const float FreeLens = FMath::Sqrt(Rig.arm * Rig.arm + Rig.side * Rig.side + Rig.up * Rig.up);
		// A zone whose framing cap is 0 cannot centre the target: there the check is only that the turn stays 0 and the
		// lock's line eases onto the target (settled frames), not the middle third.
		const bool bNoTurn = FrameRules().maxTurn <= 0.f;
		// The middle-third bar is the classic camera's; the close camera is judged by its own (ruled 11-16), at framePass.
		const float CloseShare = FrameSettled > 0 ? static_cast<float>(CloseFrameOk) / static_cast<float>(FrameSettled) : 0.f;
		const bool bFramed = bCloseShape ? CloseShare >= ftcore::kArenaClose.framePass : (bNoTurn || FrameInThird == FrameSettled);
		const bool bPass = FrameSettled > 0 && bFramed && FrameTurnMost <= FrameRules().maxTurn + 0.01f && FrameArmMost <= Rig.arm + 0.001f;
		UE_LOG(LogForgottenThird, Display,
			TEXT("Arena check: framing %s%s (%d frames followed, %d settled, %d with both bodies in the middle third; worst |x| lead %.3f, target %.3f; turn most %.1f deg (cap %.0f), its rate most %.0f deg/s; boom arm most %.2f m (rig %.2f); lens to pivot most %.2f m (rig %.2f, camera lag aside))."),
			bPass ? TEXT("PASS") : TEXT("FAIL"), bNoFrame ? TEXT(" [framing off]") : (bCloseShape ? TEXT(" [close camera: its own bar, the middle third logged only]") : (bNoTurn ? TEXT(" [zone cap 0: turn and line only]") : TEXT(""))), FrameFrames, FrameSettled, FrameInThird,
			FrameLeadWorst, FrameFoeWorst, FrameTurnMost, FrameRules().maxTurn, FrameRateMost, FrameArmMost, Rig.arm, FrameLensMost,
			FreeLens);
		if (bCloseShape)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena check: close framing %s on %d of %d settled frames (%.1f%%, bar %.0f%%): lead in its near-side third %d, target's weak point or chest in its band (from %.2f under %.0f m) %d, partner in the frame %d; %d shot-view or far-lens frames left out."),
				CloseShare >= ftcore::kArenaClose.framePass ? TEXT("PASS") : TEXT("FAIL"), CloseFrameOk, FrameSettled, CloseShare * 100.f,
				ftcore::kArenaClose.framePass * 100.f, CloseFrameLead, ftcore::kArenaClose.frameNearTargetMin, ftcore::kArenaClose.frameNearDistance,
				CloseFrameFoe, CloseFrameMate, FrameShotSkipped);
			const int32 FoeMisses = FrameSettled - CloseFrameFoe;
			const int32 LeadMisses = FrameSettled - CloseFrameLead;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena check: close framing misses: target %d (below its band %d, above %d, mean share %.3f, aim point under 1 m in front of the lens %d; with the boom wall-shortened %d, the lens lagging over 0.15 m %d, the turn at its cap %d, the partner's turn on %d, the lead over 3 m/s %d, under 3 m %d); lead out of its third %d (wall-shortened %d, lagging %d, turn at its cap %d, partner's turn %d, over 3 m/s %d, under 3 m %d); lens over the arm and 1 m from the pivot (left out) %d."),
				FoeMisses, CloseMissFoeLow, CloseMissFoeHigh, FoeMisses > 0 ? CloseMissFoeSum / static_cast<float>(FoeMisses) : 0.f, CloseMissFoeAtLens,
				CloseMissWall[0], CloseMissLag[0], CloseMissCap[0], CloseMissMateTurn[0], CloseMissFast[0], CloseMissNear[0],
				LeadMisses, CloseMissWall[1], CloseMissLag[1], CloseMissCap[1], CloseMissMateTurn[1], CloseMissFast[1], CloseMissNear[1],
				CloseFarSkipped);
		}
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
			Session ? Session->Get().lonePooled() : 0u, LoneGiftPresses, LoneGiftsRefused, static_cast<int32>(ThirdSlot));
		// The glue: while Lone nobody joined, no table began and every Release pooled (a refused gift is on the LONE line).
		const bool bLoneOk = LoneBreaches == 0;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: lone %s (%d breaches, %d of %d gift presses refused)."),
			Verdict(bLoneOk, bLoneAtStart), LoneBreaches, LoneGiftsRefused, LoneGiftPresses);
		// The partner's call: an accepted Count press while Lone ended it, once, with PartnerCalled. n/a with no such press.
		const bool bCallOk = LonePartnerCalls <= 1 && LonePartnerEnds == LonePartnerCalls &&
			(LonePartnerCalls == 0 || (Session && Session->Get().loneEndedBy() == ftcore::LoneEnd::PartnerCalled));
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: lone partner call %s (%d accepted calls while Lone, %d ended it; ended by %s)."),
			Verdict(bCallOk, bLoneAtStart && LonePartnerCalls > 0), LonePartnerCalls, LonePartnerEnds,
			Session ? LoneEndWord(Session->Get().loneEndedBy()) : TEXT("none"));
	}
	// The third slot: the session holds the fight's (carry or -ArenaThirdSlot) and its refusal matches it.
	if (Session)
	{
		const ftcore::SlotState Want = static_cast<ftcore::SlotState>(ThirdSlot);
		const ftcore::GiftError Why = GetThirdSlotRefusal();
		const ftcore::GiftError WantWhy = Want == ftcore::SlotState::Vacant ? ftcore::GiftError::Vacant
			: Want == ftcore::SlotState::Hostile ? ftcore::GiftError::Hostile : ftcore::GiftError::None;
		const bool bSlotOk = GetThirdSlot() == Want && Why == WantWhy;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: third slot %s (fight %d, session %d, refusal %d)."),
			bSlotOk ? TEXT("PASS") : TEXT("FAIL"), static_cast<int32>(Want), static_cast<int32>(GetThirdSlot()), static_cast<int32>(Why));
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

void UFTArenaRunner::PickCameraPreset()
{
	if (!bCameraPresetsTried)
	{
		bCameraPresetsTried = true;
		TArray<uint8> Bytes;
		std::string Error;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/camera_presets.json");
		ftcore::CameraPresets Read;
		if (FFileHelper::LoadFileToArray(Bytes, *File) && ftcore::importCameraPresets(FT::BytesAsText(Bytes), Read, &Error))
		{
			CameraPresetTable = Read;
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Battle/camera_presets.json; the built-in camera presets.\n%s"),
				*FT::ToFString(Error));
			CameraPresetTable = ftcore::CameraPresets();
		}
	}
	// -ArenaCamera=close|classic wins over the settings' Camera row, in every build (a review run picks its lens).
	const ftcore::Settings* Prefs = GetSettings();
	ftcore::CameraPresetId Id = Prefs ? Prefs->cameraPreset : ftcore::CameraPresetId::Classic;
	const TCHAR* From = Prefs ? TEXT("settings") : TEXT("default");
	FString Word;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaCamera="), Word))
	{
		if (Word == TEXT("close") || Word == TEXT("classic"))
		{
			Id = Word == TEXT("close") ? ftcore::CameraPresetId::Close : ftcore::CameraPresetId::Classic;
			From = TEXT("-ArenaCamera");
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaCamera=%s is no camera preset (close or classic); the %s one stands."), *Word, From);
		}
	}
	CameraPresetNow = Id;
	CameraLens = ftcore::cameraPreset(CameraPresetTable, Id);
	CameraLag = CameraLens.lagSeconds;
#if !UE_BUILD_SHIPPING
	// -ArenaCameraLag=<seconds>: the boom's lag for this run over the preset's (0 the boom's own, -1 off).
	float Lag = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaCameraLag="), Lag))
	{
		CameraLag = FMath::IsFinite(Lag) ? Lag : -1.f;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: camera lag %.3f s for this run (-ArenaCameraLag; 0 the boom's own, under 0 off)."), CameraLag);
	}
	// -ArenaPartnerYawCap=<deg>: a ranged partner's close yaw most for this run (default 12); classic ignores it.
	PartnerYawCapSwitch = 0.f;
	float YawCap = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaPartnerYawCap="), YawCap) && Id == ftcore::CameraPresetId::Close && FMath::IsFinite(YawCap))
	{
		PartnerYawCapSwitch = FMath::Clamp(YawCap, ftcore::kArenaClose.partnerYawMax, 40.f);
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: close lens ranged partner turn most %.1f degrees for this run (-ArenaPartnerYawCap, held to %.0f-40)."),
			PartnerYawCapSwitch, ftcore::kArenaClose.partnerYawMax);
	}
	// -ArenaCameraArm=<m>: the close lens's arm for this run (a review pair), held to 1.5-3.0 m; classic ignores it.
	CloseArmSwitch = 0.f;
	float Arm = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaCameraArm="), Arm) && Id == ftcore::CameraPresetId::Close && FMath::IsFinite(Arm))
	{
		CloseArmSwitch = FMath::Clamp(Arm, 1.5f, 3.f);
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: close lens arm %.2f m for this run (-ArenaCameraArm, held to 1.5-3.0 m)."), CloseArmSwitch);
	}
	// -ArenaPartnerStation=0: the close preset hands the brain no close station this run (a review's before; the in-frame
	// count still tests the close way).
	bPartnerStation = true;
	int32 Station = 1;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaPartnerStation="), Station) && Station == 0)
	{
		bPartnerStation = false;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: partner close station off for this run (-ArenaPartnerStation=0)."));
	}
#endif
	UE_LOG(LogForgottenThird, Display, TEXT("Arena camera preset: %s (from %s)"), UTF8_TO_TCHAR(ftcore::cameraPresetName(Id)), From);
}

bool UFTArenaRunner::RangedPartner() const
{
	return bCloseShape && bPartnerStation && bPartner && Fight.def().partnerKit.partnerReach > 0.f;
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
			CameraHalfChangedAt = Now;
			Fight.setCameraHalfAngle(FightCameraHalf);
			// Logged when it moved more than kCameraHalfLogStep from the last line, at most once every kCameraHalfLogEvery.
			if (FMath::Abs(FightCameraHalf - LoggedCameraHalf) > kCameraHalfLogStep && Now - CameraHalfLoggedAt >= kCameraHalfLogEvery)
			{
				LoggedCameraHalf = FightCameraHalf;
				CameraHalfLoggedAt = Now;
				UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: the windup arc's camera half-angle is %.1f degrees."), Fight.time(), FightCameraHalf);
			}
		}
		// A change the throttle kept back (more than kCameraHalfLogStep from the last line), then held
		// kCameraHalfLogEvery: one more line with where it settled.
		if (FMath::Abs(FightCameraHalf - LoggedCameraHalf) > kCameraHalfLogStep && Now - CameraHalfChangedAt >= kCameraHalfLogEvery &&
			Now - CameraHalfLoggedAt >= kCameraHalfLogEvery)
		{
			LoggedCameraHalf = FightCameraHalf;
			CameraHalfLoggedAt = Now;
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: the windup arc's camera half-angle settled at %.1f degrees."), Fight.time(), FightCameraHalf);
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
	H.partnerPresent = PartnerInSession(); // the session's own word (inFight); alone, only the refusal ends a hold
	H.accepting = (HallMode == 1 || HallMode == 4) && Hall.active();
	H.brief = LeadBrief;
	H.partnerBrief = ftcore::BriefId::Count; // the partner's kit takes no Brief yet
	H.partnerAble = H.partnerPresent && !Session->Get().down(OtherLead());
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
	EndSpecial(TEXT("the fight began again"));
	if (bPartner)
	{
		// The fight as it began: each lead in its own figure, the first lead controlled (Fight.restart clears swapped()).
		DropPendingHandoff(TEXT("a restart"));
		if (bFiguresSwapped)
		{
			SwapLeadFigures();
		}
		TakeoverInputUntil = -1.0;
		SwitchPartnerAt = -1.0;
		bNamesSwapped = bFiguresSwapped;
		SessionLead = StartLead;
	}
	RecoverLeadBodies(true); // the fight as it began: both leads standing
	Fight.setLone(LoneAccepted(SessionLead), LoneTuning); // a restart is Lone again (and leaves the heal charges out)
	if (bPartnerLoneStart)
	{
		// Begun Lone: Lone again, the partner is out again (a join on the last attempt is not this one's).
		if (Fight.lone() && !bPartnerHeldOut)
		{
			PutPartnerOut();
		}
		else if (!Fight.lone() && bPartnerHeldOut)
		{
			BringPartnerIn(); // not Lone this time: both leads from the start, as a fight begun with both
		}
	}
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
	AllyWeapons.Restart();
	bAllyStruck = bAllyStrikeCut = false;
	AllyTargetDeadLast = -1;
	PartnerImpactDue = -1.0;
#if !UE_BUILD_SHIPPING
	ResetLeadFrameChecks(); // its line is printed per attempt (Finish), its fight times this attempt's
#endif
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
constexpr float kArenaAllySide = 2.f;         // ... and to its right (+y of a +x facing) reaches the player can reach it)
// The partner's start under the close camera (PartnerCloseStart): m ahead of the lead, and to the side away from the
// shoulder.
constexpr float kArenaPartnerStartAhead = 3.f;
constexpr float kArenaPartnerStartFlank = 1.5f;
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
	Fight.setAlly(kArenaAllyHp, Def.partnerKit.bodyRadius);
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
	AllyDrawYawAt = -1.0; // a new figure takes its wanted yaw at once
	if (AllyFigure)
	{
		// A lead's look is worn only by a player-side figure; the view stays on the player's figure.
		AllyFigure->Setup(true, Def.partnerKit.bodyRadius, kArenaPlayerColor, 0.f, AllyLook);
		AllyFigure->ResetFeet();
	}
}

ftcore::Vec2 UFTArenaRunner::AllyHome(const ftcore::Vec2& Player, const ftcore::Vec2& Facing) const
{
	// Behind (against the facing) and to the side (+y of a +x facing: the world's right, a quarter clockwise seen from
	// above), kept in the ring.
	const ftcore::Vec2 Home{Player.x - Facing.x * kArenaAllyBack - Facing.y * kArenaAllySide,
		Player.y - Facing.y * kArenaAllyBack + Facing.x * kArenaAllySide};
	return Fit.ring.clamp(Home, Fight.def().partnerKit.bodyRadius + 0.5f);
}

ftcore::Vec2 UFTArenaRunner::PartnerCloseStart(const ftcore::Vec2& Player, const ftcore::Vec2& Facing) const
{
	// Under the close camera the home beside the lead is beside the lens on a right shoulder (+y is the world's right
	// of a +x facing): the partner starts ahead of the lead on the far flank instead, the side away from the shoulder,
	// in the frame and off the line to the target (ruled 11-16). The brain walks it to its station from there.
	const float Away = CloseShoulderWant < 0.f ? 1.f : -1.f; // +1: the facing's right
	const ftcore::Vec2 Start{Player.x + Facing.x * kArenaPartnerStartAhead - Facing.y * Away * kArenaPartnerStartFlank,
		Player.y + Facing.y * kArenaPartnerStartAhead + Facing.x * Away * kArenaPartnerStartFlank};
	return Fit.ring.clamp(Start, Fight.def().partnerKit.bodyRadius + 0.5f);
}

void UFTArenaRunner::MoveAlly()
{
	if (!bAllyOn || bPartner || !Fight.allyAlive()) // the partner walks to its own goal (WalkPartner)
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
	const float Step = FMath::Min(Far, Fight.def().partnerKit.moveSpeed * kArenaAllyPace * StepSeconds);
	Bodies.ally.x += To.x / Far * Step;
	Bodies.ally.y += To.y / Far * Step;
}

void UFTArenaRunner::PlaceAlly(float Alpha)
{
	if (!AllyFigure || !bAllyOn)
	{
		AllyDrawYawAt = -1.0; // placed again later: it takes its wanted yaw at once, not a sweep from where it was
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
	if (bPartner)
	{
		// The partner's own facing while it swings, guards or stands; its light string drawn as the lead's lights are.
		const ftcore::ArenaAllyOut& A = Fight.output().ally;
		if (A.attacking || A.guarding || Speed <= 0.05f)
		{
			Pose.Yaw = ArenaYaw(A.facing);
		}
		Pose.bGuard = A.guarding;
		if (A.attacking)
		{
			const ftcore::ArenaFighter& F = Fight.def().fighter;
			const int32 Lights = FMath::Max(1, static_cast<int32>(F.lightCount));
			const int32 Blow = static_cast<int32>(A.blow) % Lights;
			Pose.bAttacking = true;
			Pose.Part = static_cast<uint8>(A.part);
			Pose.PartFraction = ArenaPartFraction(F.light[static_cast<size_t>(Blow)], A.part, A.stateTime);
			Pose.Swing = Blow + 1 >= Lights ? EFTArenaSwing::Overhead : ((Blow % 2) == 0 ? EFTArenaSwing::Across : EFTArenaSwing::Backhand);
		}
	}
	// The close camera's partner is drawn at Core's heading (ArenaAllyOut::heading): toward its walk, over a walk's
	// last metre (or 0.3 s) toward its target, standing toward its facing, turned at 540 deg/s at most in Core. Not in
	// a string or a guard: there its body is pushed or recoils, and the heading follows that move (half a turn off the
	// target on a step back); its facing is drawn, as before.
	if (bPartner && bCloseShape && Fight.output().ally.partnerOn && !Fight.output().ally.attacking && !Fight.output().ally.guarding)
	{
		Pose.Yaw = ArenaYaw(Fight.output().ally.heading);
	}
	// No one-frame snap for the close camera's partner: the drawn figure turns toward the wanted yaw at kArenaAllyTurnRate
	// at most (a safety: Core's heading already turns no faster). The first placement of a session or of a new figure
	// takes the wanted yaw at once; a held clock holds it. Classic and the plain follower are drawn at the wanted yaw.
	if (AllyDrawYawAt < 0.0 || !(bPartner && bCloseShape))
	{
		AllyDrawYaw = Pose.Yaw;
	}
	else
	{
		const float Dt = FMath::Clamp(static_cast<float>(Now - AllyDrawYawAt), 0.f, 0.1f);
		const float Most = kArenaAllyTurnRate * Dt;
		AllyDrawYaw = FRotator::NormalizeAxis(AllyDrawYaw + FMath::Clamp(FMath::FindDeltaAngleDegrees(AllyDrawYaw, Pose.Yaw), -Most, Most));
	}
	AllyDrawYawAt = Now;
	Pose.Yaw = AllyDrawYaw;
#if !UE_BUILD_SHIPPING
	// The facing check on the figure as drawn this frame, against Core's facing for its string.
	if (bPartner && Fight.output().ally.attacking)
	{
		const ftcore::ArenaAllyOut& Mate = Fight.output().ally;
		const float DrawOff = FMath::Abs(FMath::FindDeltaAngleDegrees(AllyDrawYaw, ArenaYaw(Mate.facing)));
		const bool bOut = Mate.part != ftcore::ArenaPart::Startup;
		++MateDrawFrames;
		MateDrawOff += bOut && DrawOff > 30.f ? 1 : 0;
		MateDrawTurning += !bOut && DrawOff > 30.f ? 1 : 0;
		MateDrawWorst = bOut ? FMath::Max(MateDrawWorst, DrawOff) : MateDrawWorst;
	}
	if (bPartner && bCloseShape && Fight.output().ally.partnerOn)
	{
		const ftcore::ArenaAllyOut& Mate = Fight.output().ally;
		if (Speed > 0.05f && !Mate.attacking && !Mate.guarding)
		{
			const float HeadOff = FMath::Abs(FMath::FindDeltaAngleDegrees(AllyDrawYaw, ArenaYaw(Mate.heading)));
			++MateHeadFrames;
			MateHeadOff += HeadOff > 30.f ? 1 : 0;
			MateHeadSum += HeadOff;
			MateHeadWorst = FMath::Max(MateHeadWorst, HeadOff);
			// A walk to a station only: a blow's lunge or a push is not a walk a string starts off.
			if (Mate.act == static_cast<uint8>(ftcore::PartnerAct::MoveTo))
			{
				MateWalkEnd = Now;
			}
		}
		if (Mate.attacking && !bMateWasStringing && Mate.blow == 0)
		{
			const double Stood = MateWalkEnd < 0.0 ? 1e9 : Now - MateWalkEnd;
			if (Stood < 1.0)
			{
				++MateStartsWalk;
				MateStartsPaused += Stood > 0.1 ? 1 : 0;
				MateStandSum += Stood > 0.1 ? static_cast<float>(Stood) : 0.f;
			}
			else
			{
				++MateStartsStill;
			}
			const int32 T = Mate.target;
			if (T >= 0 && T < static_cast<int32>(Fight.output().enemies.size()))
			{
				const ftcore::Vec2 To{Bodies.enemies[static_cast<size_t>(T)].x - Is.x, Bodies.enemies[static_cast<size_t>(T)].y - Is.y};
				MateStartsWide += FMath::Abs(FMath::FindDeltaAngleDegrees(ArenaYaw(Mate.heading), ArenaYaw(To))) > 60.f ? 1 : 0;
			}
		}
		bMateWasStringing = Mate.attacking;
	}
#endif
	Pose.Tint = AllyFigure->GetBaseColor();
	if (Now - AllyHitTime < kArenaHitTint)
	{
		Pose.Tint = FLinearColor(1.f, 0.2f, 0.2f);
	}
	// The ready stance whenever the fight is live and it is not swinging or guarding.
	Pose.bReady = !Fight.over() && !Pose.bLying && !Pose.bAttacking && !Pose.bGuard;
	Pose.ReadyStance = ReadyStanceFor(1, AllyLook);
	AllyFigure->ApplyPose(Pose);
}

void UFTArenaRunner::FreezePartnerImpact(double T)
{
	UFTVfxComponent* Vfx = PartnerImpactFx.Get();
	UNiagaraComponent* Niagara = Vfx ? Vfx->GetNiagara() : nullptr;
	// T is the drawn clock, up to a step behind the held clock it was played on: one played this step is at age 0.
	const double Age = FMath::Max(0.0, T - PartnerImpactPlayedAt);
	if (bPartnerImpactFrozen || !Niagara || !Niagara->IsActive() || PartnerImpactPlayedAt < 0.0 || T - PartnerImpactPlayedAt < -0.1)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: view frame %.3f: partner impact not running (last played at %.3f)."), T, PartnerImpactPlayedAt);
		return;
	}
	// From its start again, then on to its age at T (a paused system doesn't advance), as FFTLeadWeapons::Freeze.
	const bool bRerun = Age > 0.0 && Age <= FFTLeadWeapons::kFreezeRerun;
	if (bRerun)
	{
		Vfx->Play();
		const int32 Ticks = FMath::Max(1, FMath::CeilToInt32(static_cast<float>(Age) / FFTLeadWeapons::kFreezeTick));
		Niagara->AdvanceSimulation(Ticks, static_cast<float>(Age) / static_cast<float>(Ticks));
	}
	Niagara->SetPaused(true);
	bPartnerImpactFrozen = true;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: view frame %.3f: partner impact %s at age %.3f (%s), %s."), T, *Vfx->Effect.ToString(), Age,
		bRerun ? TEXT("run again from its start") : TEXT("as it was"), Niagara->IsPaused() ? TEXT("paused") : TEXT("NOT paused"));
}

void UFTArenaRunner::ThawPartnerImpact()
{
	UFTVfxComponent* Vfx = PartnerImpactFx.Get();
	if (UNiagaraComponent* Niagara = Vfx ? Vfx->GetNiagara() : nullptr; Niagara && bPartnerImpactFrozen)
	{
		Niagara->SetPaused(false);
	}
	bPartnerImpactFrozen = false;
}

bool UFTArenaRunner::GetPartnerImpactAt(FVector& OutAt) const
{
	const UFTVfxComponent* Vfx = PartnerImpactFx.Get();
	if (!bPartnerImpactFrozen || !Vfx)
	{
		return false;
	}
	OutAt = Vfx->GetComponentLocation();
	return true;
}

void UFTArenaRunner::PlayWaitingPartnerImpact(double T)
{
	if (PartnerImpactDue >= 0.0 && T + 1e-6 >= PartnerImpactDue)
	{
		PartnerImpactDue = -1.0;
		PartnerImpact(PartnerImpactWaiting);
	}
}

void UFTArenaRunner::TrackAllyWeapons(const ftcore::ArenaOutput& Out)
{
	PlayWaitingPartnerImpact(static_cast<double>(Fight.heldTime())); // a shot's or cast's, on its arrival
	AFTArenaFighter* Ally = AllyFigure.Get();
	if (!bPartner || !bAllyOn || !Ally || !AllyLook)
	{
		bAllyStruck = false;
		if (AllyWeapons.IsOn())
		{
			AllyWeapons.Track(ftcore::ArenaPlayerOut{}, static_cast<double>(Fight.heldTime())); // out of play: a clip fades
		}
		return;
	}
	if (!AllyWeapons.IsOn())
	{
		if (AllyWeaponsFailed.Get() == Ally)
		{
			return; // it could not begin on this figure (no skeletal body): not every step again
		}
		AllyWeapons.SetSource(ftcore::kArenaAlly);
		AllyWeapons.SetImpacts(false); // PartnerImpact plays its blows' impact
		AllyWeapons.Begin(Ally, *AllyLook, Fight.def().partnerKit); // its own kit; again on the same figure: nothing
		if (!AllyWeapons.IsOn())
		{
			AllyWeaponsFailed = Ally;
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: the partner's figure takes no lead weapons (its strike clips and effects); not tried again on it."));
			return;
		}
	}
	const double T = static_cast<double>(Fight.heldTime());
	AllyWeapons.Feed(Out.events, T); // its Hit sparks (source kArenaAlly); Core gives its blows no AttackStarted
	// The ally's light string as the lead's light moves (its blow's index), so the strike clip each lead plays for that
	// light plays on its body: a new blow is a new index, or the same index begun again (stateTime back).
	const ftcore::ArenaAllyOut& A = Out.ally;
#if !UE_BUILD_SHIPPING
	if (A.attacking && A.target >= 0 && A.target < static_cast<int32>(Fight.enemyCount()))
	{
		// Whose a cast facing away is: Core's facing against the line to its target, the drawn yaw against Core's.
		const ftcore::Vec2& Foe = Bodies.enemies[static_cast<size_t>(A.target)];
		const ftcore::Vec2 To{Foe.x - Bodies.ally.x, Foe.y - Bodies.ally.y};
		const float CoreOff = ArenaLength(To) > 0.05f ? FMath::Abs(FMath::FindDeltaAngleDegrees(ArenaYaw(A.facing), ArenaYaw(To))) : 0.f;
		++MateCastSteps;
		MateCastCoreOff += CoreOff > 30.f ? 1 : 0;
		MateCastCoreWorst = FMath::Max(MateCastCoreWorst, CoreOff);
	}
#endif
	const int32 Lights = FMath::Max(1, static_cast<int32>(Fight.def().partnerKit.lightCount));
	const uint8 Move = static_cast<uint8>(static_cast<int32>(A.blow) % Lights);
	const bool bNewBlow = A.attacking && (!bAllyStruck || Move != AllyStruckBlow || A.stateTime + 1e-4f < AllyStruckTime);
	if (bNewBlow)
	{
		bAllyStrikeCut = false;
		AllyWeapons.StartStrike(Move, T - static_cast<double>(A.stateTime));
	}
	// Cut: a blow that lands on it (its hitstun) or its target down ends the clip (it fades); Core lets the blow itself
	// run out.
	if (A.attacking && !bAllyStrikeCut)
	{
		for (const ftcore::ArenaEvent& E : Out.events)
		{
			if ((E.type == ftcore::ArenaEventType::AllyHit && E.amount > 0) || E.type == ftcore::ArenaEventType::AllyDowned)
			{
				bAllyStrikeCut = true;
			}
		}
		// Its target down: cut only when it was down the step before too (the killing blow plays on: Core marks the
		// target Dead on the step the blow lands).
		if (A.target >= 0 && A.target == AllyTargetDeadLast)
		{
			bAllyStrikeCut = true;
		}
	}
	AllyTargetDeadLast = A.target >= 0 && A.target < MaxEnemies && Out.enemies[static_cast<size_t>(A.target)].brain == ftcore::ArenaBrain::Dead
		? A.target : -1;
	const bool bSwinging = A.attacking && !bAllyStrikeCut;
	bAllyStruck = A.attacking;
	AllyStruckBlow = Move;
	AllyStruckTime = A.stateTime;
	// Its aim: a melee blow at the point of the target's body it can reach, a shot or a cast at the chest; else
	// straight ahead.
	const FVector Chest = Ally->GetActorLocation() + FVector(0.0, 0.0, 40.0);
	const AFTArenaFighter* Target = A.target >= 0 && A.target < MaxEnemies ? EnemyFigures[A.target].Get() : nullptr;
	if (Target)
	{
		const size_t Slot = static_cast<size_t>(A.target);
		const FVector Surface = ToWorld(ftcore::fieldHitPoint(Bodies.ally, Bodies.enemies[Slot], Fight.def().enemies[Slot].bodyRadius));
		const double Feet = Target->GetActorLocation().Z;
		AllyWeapons.SetAimAtBody(Surface, Feet, Feet + Target->GetHeight(), Target->GetChest());
	}
	else
	{
		AllyWeapons.SetAim(Chest + FVector(A.facing.x, A.facing.y, 0.0) * 100.0, false);
	}
	ftcore::ArenaPlayerOut P;
	P.action = bSwinging ? ftcore::ArenaAction::Light : A.guarding ? ftcore::ArenaAction::Guard : ftcore::ArenaAction::Idle;
	P.part = bSwinging ? A.part : ftcore::ArenaPart::None;
	P.move = Move;
	P.facing = A.facing;
	AllyWeapons.Track(P, T);
}

const ftcore::ReadyStance* UFTArenaRunner::ReadyStanceFor(int32 Figure, const FFTLeadLook* Look)
{
	if (!bReadyStancesRead)
	{
		bReadyStancesRead = true;
		ReadyStances = ftcore::readyStanceDefaults();
		ftcore::ReadyStanceSet Read = ReadyStances;
		TArray<uint8> Bytes;
		std::string Error;
		const FString File = FPaths::ProjectDir() / TEXT("Data/Anim/ready_stance.json");
		if (FFileHelper::LoadFileToArray(Bytes, *File) && ftcore::importReadyStances(FT::BytesAsText(Bytes), Read, &Error))
		{
			ReadyStances = Read;
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Anim/ready_stance.json; the ready stance uses its built-in tables.\n%s"),
				*FT::ToFString(Error));
		}
	}
	const int32 I = Figure == 0 ? 0 : 1;
	if (!ReadyOf[I] || ReadyLookOf[I] != Look)
	{
		ReadyLookOf[I] = Look;
		const std::string Id = Look ? FT::ToUtf8(Look->Name) : std::string();
		ReadyOf[I] = ftcore::readyStanceFor(ReadyStances, Id);
		bool bMapped = false;
		for (size_t L = 0; L < ReadyStances.leadCount; ++L)
		{
			bMapped |= FT::ToFString(ReadyStances.leads[L].id).Equals(Look ? Look->Name : FString(), ESearchCase::IgnoreCase);
		}
		for (size_t K = 0; ReadyOf[I] && K < ReadyStances.stanceCount; ++K)
		{
			if (&ReadyStances.stances[K].stance == ReadyOf[I])
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: ready stance for the %s's look '%s': '%s'%s."), I == 0 ? TEXT("lead") : TEXT("partner"),
					Look ? *Look->Name : TEXT("-"), *FT::ToFString(ReadyStances.stances[K].id), bMapped ? TEXT("") : TEXT(" (NOT mapped: the fallback)"));
			}
		}
	}
	return ReadyOf[I];
}

// ---- The partner (-ArenaPartner): Core's PartnerBrain drives the other lead's body ----

namespace
{
// s: a window's partner attack time is measured here step by step from the ally's output, the brain's own budget by
// the lengths of the blows it starts, so the two can part by about a step at each end of a blow.
constexpr double kPartnerBudgetSlack = 0.1;
constexpr float kPartnerShareLow = 0.10f;  // the partner's damage over a fight: this share of the boss's hp ...
constexpr float kPartnerShareHigh = 0.25f; // ... to this (a tuning target, reported, not a check)
}

void UFTArenaRunner::BeginPartner()
{
	bPartner = false;
	bPartnerSpread = true;
	bFiguresSwapped = false;
	bNamesSwapped = false;
	HandoffMoveAt = TakeoverInputUntil = SwitchPartnerAt = SwitchCamCheckAt = -1.0;
	HandoffBegun = HandoffDropped = HandoffTakeovers = HandoffMovesOnTime = HandoffSwitches = TakeoverInputsIgnored = 0;
	HandoffWorstLate = SwitchCamWorst = 0.0;
	SwitchCamChecks = BenchMorphChecks = BenchMorphUnchecked = 0;
	BenchMorphWorst = 0.f;
	bCameraPoseNow = false; // the first frame's step, before any camera is drawn, feeds no stationing
	bFedCamera = false;
	LastLookInputAt = -1e9f;
	LowFitArm = -1.f;
	LowSideSign = Fit.camera.side < 0.f ? -1.f : 1.f;
	bLockAimAbove = false;

	PartnerOrders = 0;
#if !UE_BUILD_SHIPPING
	bPartner = FParse::Param(FCommandLine::Get(), TEXT("ArenaPartner"));
	SwitchAt = -1.f;
	FParse::Value(FCommandLine::Get(), TEXT("ArenaSwitchAt="), SwitchAt);
	SwitchBackAt = -1.f;
	FParse::Value(FCommandLine::Get(), TEXT("ArenaSwitchBackAt="), SwitchBackAt);
#endif
	if (!bPartner)
	{
		bPartnerHeldOut = false;
		return;
	}
	if (!AllyLook)
	{
		// No look for the other lead (the setup names no ally): no partner body to spawn, so no partner at all.
		bPartner = false;
		bPartnerHeldOut = false;
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaPartner, but the setup gives the other lead no look: the partner is not spawned."));
		return;
	}
	TArray<uint8> Bytes;
	std::string Error;
	const FString File = FPaths::ProjectDir() / TEXT("Data/Battle/partner_brain.json");
	if (!(FFileHelper::LoadFileToArray(Bytes, *File) && ftcore::importPartnerBrain(FT::BytesAsText(Bytes), AllyTuning, &Error)))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: no usable Data/Battle/partner_brain.json; the partner keeps its defaults.\n%s"), *FT::ToFString(Error));
		AllyTuning = ftcore::PartnerTuning();
	}
	// A fight begun Lone: the partner is out (no partner brain or body yet, one lead in the session) until the lead's
	// partner call ends the carry and the session's join brings it in (BringPartnerIn); any other end keeps it out.
	bPartnerLoneStart = bPartnerHeldOut = Fight.lone();
	if (bPartnerHeldOut)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaPartner in a fight begun Lone: the partner is out until the partner call ends the carry (a Release keeps it out)."));
		return;
	}
	bAllyOn = true;
	// The partner's body has its own kit's max hp (ArenaDef::partnerKit).
	Fight.setAlly(Def.partnerKit.hp, Def.partnerKit.bodyRadius);
	Fight.setPartner(&AllyBrain, &AllyTuning);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: the partner (-ArenaPartner): the other lead's body, %d hp, driven by the partner brain; order SPREAD (%s: STAY CLOSE / SPREAD); attack budget %.1f x the lead's + %.1f s in any %.0f s; down %.0f s, then up at %.0f%%."),
		Def.partnerKit.hp, *FTInputBindings::Key(ftcore::InputAction::PartnerOrder, ftcore::InputDevice::Keys).ToString(), AllyTuning.budgetShare, AllyTuning.budgetAllowanceSeconds, AllyTuning.budgetWindowSeconds, AllyTuning.downSeconds,
		AllyTuning.riseHealth * 100.f);
}

void UFTArenaRunner::BringPartnerIn()
{
	bPartnerHeldOut = false;
	++PartnerJoinsLanded;
	PartnerJoinedAt = static_cast<double>(Fight.time());
	bAllyOn = true;
	// Whole: the health fed to joinFight (1.0). setPartner begins the brain in a running fight. Its own kit's max
	// (after a switch, partnerKit.hp is the partner body's max).
	Fight.setAlly(Fight.def().partnerKit.hp, Fight.def().partnerKit.bodyRadius);
	Fight.setPartner(&AllyBrain, &AllyTuning);
	// Under the close camera it joins ahead on the far flank, not beside the lens (PartnerCloseStart).
	Bodies.ally = PrevBodies.ally = bCloseShape ? PartnerCloseStart(Bodies.player, Fight.output().player.facing)
		: AllyHome(Bodies.player, Fight.output().player.facing);
	SpawnAlly(GetWorld());
	if (AllyFigure)
	{
		AllyFigure->SetActorHiddenInGame(false);
	}
	// The budget's windows start at the join: the partner had no time in the fight before it.
	const ftcore::ArenaStats& S = Fight.stats();
	PartnerPrevTime = PartnerWinStart = PartnerJoinedAt;
	PartnerWinLead = PartnerWinAlly = 0.0;
	PartnerWinDamage0 = S.partnerDamage;
	PartnerWinLeadDamage0 = S.damageDealt;
	PartnerWinBoss0 = S.bossAttacks;
	PartnerWinAimed0 = S.bossAimedPartner;
	PartnerWinLeadFalls = PartnerWinAllyFalls = PartnerWinTakeovers = PartnerWinRises = 0;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the partner call brought lead %d into the fight (the session's join): its body at %d hp, driven by the partner brain."),
		Fight.time(), static_cast<int32>(OtherLead()), Fight.allyHp());
}

void UFTArenaRunner::SwitchLeadBodies()
{
	++SwitchesSeen;
	if (!Fight.switchLeads())
	{
		// The session switched and the fight could not (a body down, the fight over): the lead check counts it.
		++SwitchesRefused;
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: %.2f: the session switched to lead %d, the fight refused the switch."), Fight.time(),
			static_cast<int32>(OtherLead()));
		return;
	}
	// As on a takeover: the bodies, the figures (weapons, side dashes, the camera's view target) and the lead swap;
	// the input stays on the controller and moves the new body (ArmSwapCheck proves it).
	std::swap(Bodies.player, Bodies.ally);
	std::swap(PrevBodies.player, PrevBodies.ally);
	SwapLeadFigures(EHandoff::Switch);
	SessionLead = OtherLead();
	++SwitchesDone;
	// The left body's brain intents from the next step: when it first moves and when it leaves the new lead's view.
	SwitchPartnerAt = static_cast<double>(Fight.time());
	SwitchPartnerMoved = SwitchPartnerOut = -1.0;
	SwitchPartnerLogs = 0;
	SwitchPartnerFrom = AllyFigure ? AllyFigure->GetActorLocation() : FVector::ZeroVector;
	ArmSwapCheck(true);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the lead switch: control passes to lead %d at %d hp; the other lead's body at %d hp, driven by the partner brain."),
		Fight.time(), static_cast<int32>(SessionLead), Fight.hp(), Fight.allyHp());
}

void UFTArenaRunner::PutPartnerOut()
{
	// A retry of a fight begun Lone: the partner is out again on the fight's side (the session's begin has one lead).
	bPartnerHeldOut = true;
	bAllyOn = false;
	Fight.setPartner(nullptr, nullptr);
	Fight.setAlly(0, Fight.def().partnerKit.bodyRadius);
	if (AllyFigure)
	{
		AllyFigure->SetActorHiddenInGame(true);
	}
}

void UFTArenaRunner::TakeOverBodies(const ftcore::ArenaOutput& Out)
{
	for (const ftcore::ArenaEvent& E : Out.events)
	{
		if (E.type != ftcore::ArenaEventType::TookOver)
		{
			continue;
		}
		// The player stands in the partner's place and the fallen lead lies where it fell: each figure keeps its body.
		// Input needs no rerouting: the controller keeps possessing what it did (it only sets the view target); the
		// runner reads the input from it (FTInputBindings on the PlayerController) into the fight's player, which moves
		// Bodies.player, and PlayerFigure (the partner's actor after the swap) is drawn there. ArmSwapCheck proves it.
		std::swap(Bodies.player, Bodies.ally);
		std::swap(PrevBodies.player, PrevBodies.ally);
		SwapLeadFigures(EHandoff::Takeover);
		SessionLead = OtherLead();
		++PartnerTakeovers;
		ArmSwapCheck();
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the lead fell with the partner up: control passes to lead %d at %d hp; the fallen lead's body is down (up again after %.0f s)."),
			Fight.time(), static_cast<int32>(SessionLead), E.amount, AllyTuning.downSeconds);
	}
}

void UFTArenaRunner::SwapLeadFigures(EHandoff Handoff)
{
	DropPendingHandoff(TEXT("another body change in the hold"));
	if (LeadWeapons.GauntletMorphNames().Num() > 0)
	{
		BenchMorphs = LeadWeapons.GauntletMorphNames(); // on a body change only
		BenchMorphOwner = PlayerFigure;
	}
	LeadWeapons.End();
	PartnerClipsStarted += AllyWeapons.GetClipsStarted();
	PartnerClipsWhole += AllyWeapons.GetClipsCompleted();
	AllyWeapons.End(); // begun again on the new ally figure by TrackAllyWeapons
	bAllyStruck = bAllyStrikeCut = false;
	AllyTargetDeadLast = -1;
	PartnerImpactDue = -1.0; // the shot left from the other figure
	Swap(PlayerFigure, AllyFigure);
	Swap(PlayerLook, AllyLook);
	AllyDrawYawAt = -1.0; // the other body is the ally now: it takes its wanted yaw at once, not a sweep from the old one's
	// The HUD's rows follow the figures: the controlled lead's seat and name are the lead row's (the rows trade places
	// on the seat change, ftcore::uiRowSwapStep), the other's the partner row's.
	Swap(LeadSeat, AllySeat);
	Swap(LeadName, AllyName);
	bFiguresSwapped = !bFiguresSwapped;
	if (PlayerFigure && PlayerLook)
	{
		// The weapon set and jump timing for the figure now controlled: its lead's kit, from the kits as given (a
		// restart swaps the figures back before the fight puts its kits back).
		const ftcore::ArenaFighter& Kit = bFiguresSwapped ? Def.partnerKit : Def.fighter;
		LeadWeapons.Begin(PlayerFigure, *PlayerLook, Kit);
		PlayerFigure->SetJumpTiming(Kit.jumpStartup, Kit.jumpAir, Kit.jumpLandLock);
		LoadSpecialClip(); // the lead now controlled: its special's clip
	}
	if (PlayerFigure)
	{
		PlayerFigure->ResetFeet();
		// Side dashes need the controlled figure's own side clips: read again for the new one.
		Fight.setSideDashes(PlayerFigure->CanSideDash());
	}
	if (AllyFigure)
	{
		AllyFigure->ResetFeet();
		// The benched figure keeps none of the look's gauntlet morphs (End set them back to 0).
		const bool bGauntletBenched = BenchMorphs.Num() > 0 && BenchMorphOwner.Get() == AllyFigure;
		const USkeletalMeshComponent* Benched = bGauntletBenched ? AllyFigure->GetLeadSkin() : nullptr;
		BenchMorphUnchecked += bGauntletBenched && !Benched ? 1 : 0; // benched, but no skin to read
		for (const FName& Morph : BenchMorphs)
		{
			if (Benched)
			{
				BenchMorphWorst = FMath::Max(BenchMorphWorst, FMath::Abs(Benched->GetMorphTarget(Morph)));
				++BenchMorphChecks;
			}
		}
	}
	if (PlayerFigure)
	{
		PlayerFigure->SetCameraLag(CameraLag, bCloseShape ? kArenaCloseLagMost : 0.f); // the preset's boom lag on the new lead's boom too
	}
	if (PlayerFigure && bCameraPoseNow)
	{
		// The play camera's rig on the new lead's boom (its own is the setup default); SteerCamera turns it.
		PlayerFigure->SetCameraRig(CameraPoseNow.arm, CameraPoseNow.pivot, CameraPoseNow.side, CameraPoseNow.up, CameraPoseNow.fov);
		PlayerFigure->SetCameraTilt(CameraPoseNow.tilt);
		PlayerFigure->SetBoomRotation(FRotator(CameraPoseNow.boomPitch, CameraPoseNow.yaw, 0.0));
	}
	PlayerStepAt = -1; // the legs start over from a stand
	if (Handoff == EHandoff::Takeover)
	{
		// No cut: the view stays on the fallen lead's figure (AllyFigure now), the HUD keeps its names and no input is
		// taken for kTakeoverHoldSeconds; then TickHandoff moves the camera to the new lead over kTakeoverMoveSeconds,
		// the fallen body in frame at the move's start, and the HUD names the new lead. One clock: the fight's.
		HandoffFellAt = static_cast<double>(Fight.time());
		HandoffMoveAt = TakeoverInputUntil = HandoffFellAt + kTakeoverHoldSeconds;
		++HandoffBegun;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the takeover's hand-off: the camera holds on the fallen lead %.2f s, then moves to the new lead over %.2f s; no input to %.2f."),
			Fight.time(), kTakeoverHoldSeconds, kTakeoverMoveSeconds, TakeoverInputUntil);
		return;
	}
	bNamesSwapped = bFiguresSwapped;
	TakeoverInputUntil = -1.0;
	if (APlayerController* PC = GetPC())
	{
		if (PlayerFigure && Handoff == EHandoff::Switch)
		{
			// A switch eases to the new lead (no cut).
			PC->SetViewTargetWithBlend(PlayerFigure, kSwitchEaseSeconds, EViewTargetBlendFunction::VTBlend_EaseInOut, 2.f);
			++HandoffSwitches;
			SwitchCamCheckAt = Now + kSwitchEaseSeconds + 0.5; // the ease done and the boom's lag settled
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the switch's hand-off: the camera eases to the new lead over %.2f s; the HUD names it now."),
				Fight.time(), kSwitchEaseSeconds);
		}
		else if (PlayerFigure)
		{
			PC->SetViewTarget(PlayerFigure);
		}
	}
}

void UFTArenaRunner::DropPendingHandoff(const TCHAR* Why)
{
	if (HandoffMoveAt < 0.0)
	{
		return;
	}
	HandoffMoveAt = -1.0;
	TakeoverInputUntil = -1.0;
	bNamesSwapped = bFiguresSwapped;
	++HandoffDropped;
	// The view to the lead now controlled (a body change after this sets its own), only from a fighter's play camera
	// and while the fight goes on: a capture's camera and the result's view stay as they are.
	APlayerController* PC = GetPC();
	const bool bMoveView = PC && PlayerFigure && !Fight.over() && Cast<AFTArenaFighter>(PC->GetViewTarget()) != nullptr;
	if (bMoveView)
	{
		PC->SetViewTargetWithBlend(PlayerFigure, kTakeoverMoveSeconds, EViewTargetBlendFunction::VTBlend_EaseInOut, 2.f);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: a takeover's hand-off dropped in its hold (%s): %s."), Fight.time(), Why,
		bMoveView ? TEXT("the view eases to the lead now controlled") : TEXT("the view stays"));
}

void UFTArenaRunner::TickHandoff()
{
	if (SwitchCamCheckAt >= 0.0 && Now >= SwitchCamCheckAt)
	{
		// The stationing's camera (the pose's lens over the lead) against the drawn camera's, on the ground. Measured,
		// not a pass or fail: the boom's collision probe may pull the drawn one in and its lag trails a moving lead.
		SwitchCamCheckAt = -1.0;
		const APlayerController* PC = GetPC();
		if (PC && PC->PlayerCameraManager && bCameraPoseNow && PC->GetViewTarget() != PlayerFigure.Get())
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: after the switch the view is not the lead's play camera (a capture's): no camera measure."), Fight.time());
		}
		else if (PC && PC->PlayerCameraManager && bCameraPoseNow)
		{
			const ftcore::ArenaLensAt Lens = ftcore::arenaCameraLens(CameraPoseNow);
			const FVector Drawn = PC->PlayerCameraManager->GetCameraLocation();
			const double Gap = FVector2D::Distance(FVector2D(Drawn.X, Drawn.Y),
				FVector2D((Bodies.player.x + Lens.x) * 100.0, (Bodies.player.y + Lens.y) * 100.0));
			SwitchCamWorst = FMath::Max(SwitchCamWorst, Gap);
			++SwitchCamChecks;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: after the switch the stationing's camera is %.1f cm from the drawn camera (on the ground)."),
				Fight.time(), Gap);
		}
	}
	const double T = static_cast<double>(Fight.time());
	if (HandoffMoveAt < 0.0)
	{
		return;
	}
	if (T + 1e-6 < HandoffMoveAt)
	{
		if (Fight.over())
		{
			DropPendingHandoff(TEXT("the fight ended"));
		}
		return;
	}
	const double Late = T - HandoffMoveAt;
	HandoffMoveAt = -1.0;
	++HandoffTakeovers;
	bNamesSwapped = bFiguresSwapped; // the new lead's name row at the move's start
	if (APlayerController* PC = GetPC(); PC && PlayerFigure)
	{
		PC->SetViewTargetWithBlend(PlayerFigure, kTakeoverMoveSeconds, EViewTargetBlendFunction::VTBlend_EaseInOut, 2.f);
	}
	HandoffWorstLate = FMath::Max(HandoffWorstLate, Late);
	// On time: within the frame that crossed the hold's end (a headless run steps coarser frames than a drawn one).
	HandoffMovesOnTime += Late <= FMath::Max(0.05, static_cast<double>(HandoffFrameDt) + 0.005) ? 1 : 0;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the takeover's camera move starts %.2f s after the fall (%.0f ms late), over %.2f s; the HUD names the new lead."),
		Fight.time(), T - HandoffFellAt, Late * 1000.0, kTakeoverMoveSeconds);
}

namespace
{
// Whether a world point is inside the player's camera frame (16:9, the camera manager's horizontal FOV).
bool InCameraView(const APlayerController* PC, const FVector& P)
{
	const APlayerCameraManager* Cam = PC ? PC->PlayerCameraManager.Get() : nullptr;
	if (!Cam)
	{
		return false;
	}
	const FVector L = Cam->GetCameraRotation().UnrotateVector(P - Cam->GetCameraLocation());
	if (L.X <= 1.0)
	{
		return false;
	}
	const double TanH = FMath::Tan(FMath::DegreesToRadians(static_cast<double>(Cam->GetFOVAngle()) * 0.5));
	return FMath::Abs(L.Y / L.X) <= TanH && FMath::Abs(L.Z / L.X) <= TanH * 9.0 / 16.0;
}
}

void UFTArenaRunner::TrackSwitchPartner()
{
	if (SwitchPartnerAt < 0.0 || !AllyFigure)
	{
		return;
	}
	constexpr double kEvery = 0.25, kFor = 2.0, kOutWanted = 1.0;
	const double Since = static_cast<double>(Fight.time()) - SwitchPartnerAt;
	const ftcore::PartnerIntent& Intent = AllyBrain.intent();
	const FVector At = AllyFigure->GetActorLocation();
	const double Moved = FVector::Dist2D(At, SwitchPartnerFrom);
	if (SwitchPartnerMoved < 0.0 && Moved > 5.0)
	{
		SwitchPartnerMoved = Since;
	}
	// Out of view: neither its chest nor its feet in the new lead's frame.
	const bool bSeen = InCameraView(GetPC(), AllyFigure->GetChest()) || InCameraView(GetPC(), At);
	if (SwitchPartnerOut < 0.0 && !bSeen)
	{
		SwitchPartnerOut = Since;
	}
	if (Since + 1e-6 >= kEvery * SwitchPartnerLogs && SwitchPartnerLogs <= static_cast<int32>(kFor / kEvery))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: switch +%.2f s: the left body's brain %s (%s), %.0f cm from where the switch left it, %s the new lead's view."),
			Since, PartnerActWord(Intent.act), PartnerWhyWord(Intent.why), Moved, bSeen ? TEXT("in") : TEXT("out of"));
		++SwitchPartnerLogs;
	}
	if (Since >= kFor || Fight.over())
	{
		++SwitchPartnerChecks;
		SwitchWorstMoved = FMath::Max(SwitchWorstMoved, SwitchPartnerMoved < 0.0 ? kFor : SwitchPartnerMoved);
		SwitchWorstOut = FMath::Max(SwitchWorstOut, SwitchPartnerOut < 0.0 ? kFor : SwitchPartnerOut);
		const FString MovedWord = SwitchPartnerMoved < 0.0 ? FString(TEXT("never in 2 s")) : FString::Printf(TEXT("at +%.2f s"), SwitchPartnerMoved);
		const FString OutWord = SwitchPartnerOut < 0.0 ? FString(TEXT("never in 2 s")) : FString::Printf(TEXT("at +%.2f s"), SwitchPartnerOut);
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: switch partner: the left body first moved %s, left the new lead's view %s (wanted within %.2f s)."),
			*MovedWord, *OutWord, kOutWanted);
		SwitchPartnerAt = -1.0;
	}
}

void UFTArenaRunner::LogHandoffChecks() const
{
	const bool bNames = bNamesSwapped == bFiguresSwapped;
	if (HandoffBegun > 0)
	{
		// Each takeover's hold either started its camera move (on time: within the frame that crossed it) or was
		// dropped (the fight ended, another body change, a restart); the fight's clock for both.
		const bool bOk = HandoffMovesOnTime == HandoffTakeovers && HandoffTakeovers + HandoffDropped == HandoffBegun && bNames;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: takeover hand-off %s (%d takeovers: %d camera moves, %d on time (at most %.0f ms late) after a %.2f s hold, over %.2f s, %d dropped in the hold; %d steps of input ignored; HUD names %s)."),
			bOk ? TEXT("PASS") : TEXT("FAIL"), HandoffBegun, HandoffTakeovers, HandoffMovesOnTime, HandoffWorstLate * 1000.0, kTakeoverHoldSeconds,
			kTakeoverMoveSeconds, HandoffDropped, TakeoverInputsIgnored, bNames ? TEXT("with the figures") : TEXT("NOT with the figures"));
	}
	if (HandoffSwitches > 0 || HandoffBegun > 0)
	{
		// At each body change, and again now when the gauntlet lead's figure is the benched one.
		int32 Checks = BenchMorphChecks;
		int32 Unchecked = BenchMorphUnchecked;
		float Worst = BenchMorphWorst;
		const AFTArenaFighter* Owner = BenchMorphOwner.Get();
		// A benched figure the partner's driver wears its look on (its gauntlet morphs on) is not benched bare.
		const bool bDriven = AllyWeapons.IsOn();
		const bool bBenchedNow = Owner && BenchMorphs.Num() > 0 && Owner == AllyFigure.Get() && !bDriven;
		if (const USkeletalMeshComponent* Benched = bBenchedNow ? Owner->GetLeadSkin() : nullptr)
		{
			for (const FName& Morph : BenchMorphs)
			{
				Worst = FMath::Max(Worst, FMath::Abs(Benched->GetMorphTarget(Morph)));
				++Checks;
			}
		}
		else
		{
			Unchecked += bBenchedNow ? 1 : 0;
		}
		// A benched gauntlet figure with no skin to read is UNCHECKED, never a pass.
		const TCHAR* Verdict = Worst > 1e-4f ? TEXT("FAIL") : (Unchecked > 0 ? TEXT("UNCHECKED") : (Checks == 0 ? TEXT("n/a, the gauntlet lead never benched") : TEXT("PASS")));
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: benched figure morphs %s (%d checked, %d benched with no skin to read, after %d body changes, worst weight %.3f; the gauntlet lead benched at the end %s)."),
			Verdict, Checks, Unchecked, HandoffSwitches + HandoffBegun, Worst,
			bBenchedNow ? TEXT("yes") : (bDriven && Owner && Owner == AllyFigure.Get() ? TEXT("as the driven partner, exempt") : TEXT("no")));
	}
	if (HandoffSwitches > 0 || SwitchPartnerChecks > 0)
	{
		// The camera and the names are this host's; when the left body leaves the view is the partner brain's
		// stationing (measured here, not yet a pass or fail).
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: switch hand-off %s (%d switches eased over %.2f s; HUD names %s; the left body first moved within %.2f s, out of view within %.2f s, %d measured; the stationing's camera at most %.1f cm from the drawn one, %d measured)."),
			bNames ? TEXT("PASS") : TEXT("FAIL"), HandoffSwitches, kSwitchEaseSeconds, bNames ? TEXT("with the figures") : TEXT("NOT with the figures"),
			SwitchWorstMoved, SwitchWorstOut, SwitchPartnerChecks, SwitchCamWorst, SwitchCamChecks);
	}
}

void UFTArenaRunner::PartnerImpact(const ftcore::ArenaEvent& E)
{
	if (E.amount <= 0 || E.target >= MaxEnemies || !AllyFigure)
	{
		return;
	}
	// The blow point: on the struck body's surface toward the partner, at the partner's chest height.
	const size_t T = static_cast<size_t>(E.target);
	const ftcore::Vec2& Foe = Bodies.enemies[T];
	ftcore::Vec2 Dir{Bodies.ally.x - Foe.x, Bodies.ally.y - Foe.y};
	const float Length = ArenaLength(Dir);
	Dir = Length > 1e-4f ? ftcore::Vec2{Dir.x / Length, Dir.y / Length} : Fight.output().enemies[T].facing;
	const float Out = Fight.def().enemies[T].bodyRadius;
	FVector At = ToWorld(ftcore::Vec2{Foe.x + Dir.x * Out, Foe.y + Dir.y * Out});
	At.Z = AllyFigure->GetChest().Z;
	// The lead's own impact effect (both leads play the one fighter kit here), on the partner's figure, at 70%.
	UFTVfxComponent* Vfx = PartnerImpactFx.Get();
	if (Vfx && Vfx->GetOwner() != AllyFigure)
	{
		Vfx->DestroyComponent(); // the figures swapped: the effect goes with the partner's figure
		Vfx = nullptr;
	}
	if (!Vfx)
	{
		Vfx = NewObject<UFTVfxComponent>(AllyFigure);
		Vfx->Effect = FName(FFTLeadWeapons::ImpactEffectOf(Fight.def().partnerKit.weapon)); // remade on a swap
		Vfx->bPlayOnBeginPlay = false;
		Vfx->SetupAttachment(AllyFigure->GetRootComponent());
		Vfx->SetUsingAbsoluteLocation(true);
		Vfx->SetUsingAbsoluteRotation(true);
		Vfx->SetUsingAbsoluteScale(true);
		Vfx->RegisterComponent();
		Vfx->SetWorldScale3D(FVector(kPartnerImpactScale));
		Vfx->SetParameters(kPartnerImpactScale, kPartnerImpactScale);
		PartnerImpactFx = Vfx;
	}
	// A spell's ring square to the blow, facing the partner (the others as they were made).
	Vfx->SetWorldLocationAndRotation(At, Fight.def().partnerKit.weapon == ftcore::ArenaWeapon::Magic
		? FFTLeadWeapons::ImpactFacing(AllyFigure->GetActorLocation(), At) : FRotator::ZeroRotator);
	const bool bPlayed = Vfx->Play();
	PartnerImpactPlayedAt = static_cast<double>(Fight.heldTime());
	UE_LOG(LogForgottenThird, Verbose, TEXT("Arena: %.2f: partner impact %s at %.0f%% on enemy %d (%d hp), %.0f cm from the partner's chest%s."),
		Fight.time(), *Vfx->Effect.ToString(), kPartnerImpactScale * 100.f, static_cast<int32>(E.target), E.amount,
		FVector::Dist(At, AllyFigure->GetChest()), bPlayed ? TEXT("") : TEXT(" (no effect asset: not drawn)"));
	ImpactSoundHook(E.source, At, kPartnerImpactScale);
}

void UFTArenaRunner::ImpactSoundHook(uint8 Source, const FVector& At, float Scale)
{
	// The one place a landed blow's impact sound plays (the lead's at 1, the partner's at kPartnerImpactScale). No
	// impact sound is in Data/Audio/cues.json yet: logged once per source until one is.
	const int32 I = Source == ftcore::kArenaAlly ? 1 : 0;
	if (!bImpactHookLogged[I])
	{
		bImpactHookLogged[I] = true;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: impact sound hook: the %s's blows at %.0f%% (first at %s); no impact cue yet, silent."),
			I == 1 ? TEXT("partner") : TEXT("lead"), Scale * 100.f, *At.ToCompactString());
	}
}

void UFTArenaRunner::WalkPartner(const ftcore::ArenaOutput& Out)
{
	if (!Out.ally.partnerOn)
	{
		return;
	}
	// To the fight's goal at its own kit's moveSpeed, off the living enemies' bodies, inside the ring.
	const ftcore::Vec2 To{Out.ally.goal.x - Bodies.ally.x, Out.ally.goal.y - Bodies.ally.y};
	const float Far = ArenaLength(To);
	const float Most = Fight.def().partnerKit.moveSpeed * StepSeconds;
	if (Far > Most)
	{
		Bodies.ally.x += To.x * Most / Far;
		Bodies.ally.y += To.y * Most / Far;
	}
	else
	{
		Bodies.ally = Out.ally.goal;
	}
	const float Radius = Fight.allyRadius();
	for (size_t I = 0; I < Fight.enemyCount(); ++I)
	{
		if (Fight.enemyHp(I) <= 0)
		{
			continue;
		}
		const float Least = Radius + Fight.def().enemies[I].bodyRadius;
		const ftcore::Vec2 Off{Bodies.ally.x - Bodies.enemies[I].x, Bodies.ally.y - Bodies.enemies[I].y};
		const float Apart = ArenaLength(Off);
		if (Apart >= Least)
		{
			continue;
		}
		const ftcore::Vec2 Dir = Apart > 1e-4f ? ftcore::Vec2{Off.x / Apart, Off.y / Apart} : ftcore::Vec2{1.f, 0.f};
		Bodies.ally = ftcore::Vec2{Bodies.enemies[I].x + Dir.x * Least, Bodies.enemies[I].y + Dir.y * Least};
	}
	Bodies.ally = Fit.ring.clamp(Bodies.ally, Radius);
}

void UFTArenaRunner::FeedLeadHealths()
{
	if (!Session || !PartnerBody())
	{
		return;
	}
	// The session's fall feed for both leads: the player's body is the controlled lead's, the ally's the other's.
	Session->Get().setLeadHealth(SessionLead, static_cast<float>(Fight.hp()) / static_cast<float>(FMath::Max(Fight.def().fighter.hp, 1)));
	Session->Get().setLeadHealth(OtherLead(), static_cast<float>(Fight.allyHp()) / static_cast<float>(FMath::Max(Fight.allyMaxHp(), 1)));
}

void UFTArenaRunner::TrackPartner(const ftcore::ArenaOutput& Out)
{
	if (bPartnerHeldOut)
	{
		PartnerHeldLeaks += Out.ally.partnerOn ? 1 : 0; // held out: Core's partner never acts
		return;
	}
	if (!PartnerBody())
	{
		return;
	}
	const double T = static_cast<double>(Fight.time());
	const double Dt = FMath::Max(0.0, T - PartnerPrevTime);
	PartnerPrevTime = T;
	// The lead attacks as the brain reads it (ArenaFight's leadAttacking): in a light or a heavy, startup to recovery.
	if (Out.player.action == ftcore::ArenaAction::Light || Out.player.action == ftcore::ArenaAction::Heavy)
	{
		PartnerWinLead += Dt;
	}
	if (Out.ally.attacking)
	{
		PartnerWinAlly += Dt;
	}
	bool bTook = false;
	for (const ftcore::ArenaEvent& E : Out.events)
	{
		bTook = bTook || E.type == ftcore::ArenaEventType::TookOver;
	}
	for (const ftcore::ArenaEvent& E : Out.events)
	{
		if (E.type == ftcore::ArenaEventType::Downed && E.target == ftcore::kArenaPlayer)
		{
			++PartnerLeadFalls;
			++PartnerWinLeadFalls;
		}
		else if (E.type == ftcore::ArenaEventType::AllyDowned && !bTook) // a takeover's AllyDowned is the lead's fall
		{
			++PartnerAllyFalls;
			++PartnerWinAllyFalls;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the partner is down (up again after %.0f s)."), T, AllyTuning.downSeconds);
		}
		else if (E.type == ftcore::ArenaEventType::TookOver)
		{
			++PartnerWinTakeovers;
		}
		else if (E.type == ftcore::ArenaEventType::AllyRose)
		{
			++PartnerRises;
			++PartnerWinRises;
			if (AllyFigure)
			{
				AllyFigure->RecoverBody();
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: the partner stands again at %d hp."), T, E.amount);
		}
	}
	if (T - PartnerWinStart >= static_cast<double>(AllyTuning.budgetWindowSeconds) - 1e-6)
	{
		LogPartnerWindow();
	}
}

void UFTArenaRunner::LogPartnerWindow()
{
	const ftcore::ArenaStats& S = Fight.stats();
	const double T = static_cast<double>(Fight.time());
	const double Limit = static_cast<double>(AllyTuning.budgetShare) * PartnerWinLead + static_cast<double>(AllyTuning.budgetAllowanceSeconds);
	const bool bHeld = PartnerWinAlly <= Limit + kPartnerBudgetSlack;
	++PartnerWindows;
	PartnerWindowsHeld += bHeld ? 1 : 0;
	PartnerWorstOver = FMath::Max(PartnerWorstOver, PartnerWinAlly - Limit);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: partner window %d (%.1f-%.1f s): attack time partner %.2f s, lead %.2f s: budget %s (at most %.2f s); damage partner %d, lead %d; boss strings %d, %d at the partner; falls lead %d, partner %d; takeovers %d; rises %d; order %s."),
		PartnerWindows, PartnerWinStart, T, PartnerWinAlly, PartnerWinLead, bHeld ? TEXT("held") : TEXT("OVER"), Limit,
		S.partnerDamage - PartnerWinDamage0, S.damageDealt - PartnerWinLeadDamage0, S.bossAttacks - PartnerWinBoss0,
		S.bossAimedPartner - PartnerWinAimed0, PartnerWinLeadFalls, PartnerWinAllyFalls, PartnerWinTakeovers, PartnerWinRises,
		bPartnerSpread ? TEXT("SPREAD") : TEXT("STAY CLOSE"));
	PartnerWinStart = T;
	PartnerWinLead = PartnerWinAlly = 0.0;
	PartnerWinDamage0 = S.partnerDamage;
	PartnerWinLeadDamage0 = S.damageDealt;
	PartnerWinBoss0 = S.bossAttacks;
	PartnerWinAimed0 = S.bossAimedPartner;
	PartnerWinLeadFalls = PartnerWinAllyFalls = PartnerWinTakeovers = PartnerWinRises = 0;
}

void UFTArenaRunner::LogPartnerChecks(bool bWon, bool bLost)
{
	if (bPartner)
	{
		// Who is in the fight: a fight begun Lone takes the partner on the partner call only (one join asked and
		// landed), else never; the session and the fight agree on it at every step.
		const bool bCalled = Session && Session->Get().loneEndedBy() == ftcore::LoneEnd::PartnerCalled;
		const bool bIn = Session && Session->Get().inFight(OtherLead());
		const bool bWant = !bPartnerLoneStart || bCalled;
		const bool bJoinOk = PartnerJoinDisagree == 0 && PartnerJoinsRefused == 0 && bIn == bWant && bPartnerHeldOut == !bWant
			&& PartnerJoinAsks == (bPartnerLoneStart && bCalled ? 1 : 0) && PartnerJoinsLanded == PartnerJoinAsks;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner join %s (begun %s, the carry ended by %s: joins asked %d, landed %d at %.2f s, refused %d; the partner in the session %d, held out %d; steps the session and the fight disagreed %d)."),
			bJoinOk ? TEXT("PASS") : TEXT("FAIL"), bPartnerLoneStart ? TEXT("Lone") : TEXT("with both"),
			Session ? LoneEndWord(Session->Get().loneEndedBy()) : TEXT("none"), PartnerJoinAsks, PartnerJoinsLanded, PartnerJoinedAt,
			PartnerJoinsRefused, bIn ? 1 : 0, bPartnerHeldOut ? 1 : 0, PartnerJoinDisagree);
	}
	if (bPartner && (SwitchAsks > 0 || SwitchesSeen > 0))
	{
		// A switch with both up: every one the session made, the fight made (bodies, figures, lead), none refused; every
		// ask switched, was refused by the session's rules (a player's ask in the cooldown or on a body down) or found
		// control already there (Same). Asks are one per step (AskLeadSwitch), so Tab and a screen release count once.
		const bool bSwitchOk = SwitchesRefused == 0 && SwitchesDone == SwitchesSeen &&
							   SwitchesSeen + SwitchAsksRefused + SwitchAsksSame >= SwitchAsks;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner switch %s (asked %d, the session switched %d, refused by its rules %d, already there %d, the fight swapped %d, refused %d; leadSwitches %d)."),
			bSwitchOk ? TEXT("PASS") : TEXT("FAIL"), SwitchAsks, SwitchesSeen, SwitchAsksRefused, SwitchAsksSame, SwitchesDone, SwitchesRefused,
			Fight.stats().leadSwitches);
	}
	if (bPerLead)
	{
		// Per-lead Load: every control move brought the new lead its own Load and heal charges exactly (as it left
		// them, plus the kept changes since; the begin's for the partner), and the fight's gauge and charges took them.
		// Covered only with a switch and a switch back (-ArenaSwitchAt with -ArenaSwitchBackAt, or two takeovers).
		const bool bCovered = LeadMovesChecked >= 2 && LeadReturnsChecked >= 1;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: per-lead Load %s (%d control moves, %d checked, %d returns, %d differed)."),
			LeadValueMisses > 0 ? TEXT("FAIL") : (bCovered ? TEXT("PASS") : TEXT("not covered")), LeadMoves, LeadMovesChecked,
			LeadReturnsChecked, LeadValueMisses);
	}
	if (bPerLead)
	{
		// The host's order: every set before the step's spends, so the session refused none (lateSets 0) in any fight
		// of the run (LateSetsRun: a retry's begin does not wipe an earlier fight's late sets).
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: per-lead host order %s (late sets %d in the run, %d this fight; %d Load spends and %d heal charges reported)."),
			LateSetsRun > 0 ? TEXT("FAIL") : (LeadLoadSpends + LeadHealSpends > 0 ? TEXT("PASS") : TEXT("not covered")),
			LateSetsRun, LateSetsSeen, LeadLoadSpends, LeadHealSpends);
	}
	if (bPartner && PartnerOnSteps > 0)
	{
		// Stationing: every step the partner stood up with the camera fed ran it, and its brain was told of every
		// body change (a switch or a takeover) exactly once.
		const ftcore::ArenaStats& St = Fight.stats();
		// The brain's own count of its resets, the fight's count of the resets it asked, and the host's count of the
		// switches and takeovers it drew: three counters kept in three places.
		const int32 FightSwaps = St.partnerSwaps - PartnerSwapsBase;
		const bool bSwapsOk = AllyBrain.swaps() == static_cast<std::uint32_t>(FightSwaps) && FightSwaps == SwitchesDone + PartnerTakeovers;
		const bool bStationOk = PartnerCameraSteps > 0 && PartnerStationedSteps == PartnerCameraSteps;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner stationing %s (%d steps up, %d with the camera, %d stationed; brain resets %u, fight %d, host %d switches and %d takeovers)."),
			bSwapsOk && bStationOk ? TEXT("PASS") : TEXT("FAIL"), PartnerOnSteps, PartnerCameraSteps, PartnerStationedSteps,
			static_cast<uint32>(AllyBrain.swaps()), FightSwaps, SwitchesDone, PartnerTakeovers);
	}
	if (bPartner)
	{
		// In play: the partner on the screen, and not behind the locked target (ftcore::arenaHiddenBehind), at least 70% of
		// the steps it was up (ftcore::partnerInFrame at ftcore::kPartnerFrustumMargin) and at least four actions per 20 s
		// of the engaged steps (the lead attacked within kPartnerEngagedWindow s; ruled 11-16), each lasting kPartnerActMin
		// s or more and carried out; the whole run's rate is printed beside it. Under kPartnerEngagedMinSeconds engaged the
		// rate is not covered and the screen share alone is judged. Under kPartnerPlayMinSteps measured steps: not covered. Then the brain's own report:
		// the seconds it spent on each idle reason (None is the time it acted) and the strings it began off the frame.
		FlushPartnerAction();
		constexpr int32 kPartnerPlayMinSteps = 200;
		FString Brain;
		for (size_t I = 0; I < ftcore::kPartnerIdleCount; ++I)
		{
			const ftcore::PartnerIdle W = static_cast<ftcore::PartnerIdle>(I);
			Brain += FString::Printf(TEXT("%s%s %.1f s"), I ? TEXT(", ") : TEXT(""), PartnerIdleWord(W), AllyBrain.idleSeconds(W));
		}
		Brain += FString::Printf(TEXT("; strings begun off the frame %u; longest stand-still %.1f s"),
			static_cast<uint32>(AllyBrain.offFrameStarts()), PartnerStillLongest);
		// A partner kit with no attacks (no kit): its budget reads spent, so this time is inside noBudget's.
		Brain += FString::Printf(TEXT("; no kit %.1f s"), Fight.stats().partnerNoKitSeconds);
		if (PartnerFrustumSteps < kPartnerPlayMinSteps)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner in play not covered (%d steps measured in the play camera's frame of %d up, wanted %d; brain: %s)."),
				PartnerFrustumSteps, PartnerOnSteps, kPartnerPlayMinSteps, *Brain);
		}
		else
		{
			// The pass is on the share on screen and not behind the locked target; the plain on-screen share is printed too.
			const float Seen = static_cast<float>(PartnerSeenSteps) / static_cast<float>(PartnerFrustumSteps);
			const float Clear = static_cast<float>(PartnerClearSteps) / static_cast<float>(PartnerFrustumSteps);
			const float LeadClear = static_cast<float>(PartnerLeadClearSteps) / static_cast<float>(PartnerFrustumSteps);
			const float ClearFlat = static_cast<float>(PartnerClearFlatSteps) / static_cast<float>(PartnerFrustumSteps);
			const float LeadClearFlat = static_cast<float>(PartnerLeadClearFlatSteps) / static_cast<float>(PartnerFrustumSteps);
			const float Per20 = PartnerPlaySeconds > 0.f ? static_cast<float>(PartnerActions) * 20.f / PartnerPlaySeconds : 0.f;
			const bool bRateCovered = PartnerEngagedSeconds >= kPartnerEngagedMinSeconds;
			const float Engaged20 = bRateCovered ? static_cast<float>(PartnerEngagedActions) * 20.f / PartnerEngagedSeconds : 0.f;
			const FString Rate = bRateCovered ? FString::Printf(TEXT("%d in %.1f s engaged, %.1f per 20 s, wanted 4"), PartnerEngagedActions,
				PartnerEngagedSeconds, Engaged20)
				: FString::Printf(TEXT("not covered (%.1f s engaged, wanted %.0f)"), PartnerEngagedSeconds, kPartnerEngagedMinSeconds);
			const int32 ClipsStarted = PartnerClipsStarted + AllyWeapons.GetClipsStarted();
			const int32 ClipsWhole = PartnerClipsWhole + AllyWeapons.GetClipsCompleted();
			UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner in play %s (on screen and not behind the locked target %.0f%% of %d steps, wanted 70%%; on screen %.0f%%; on screen and not behind the lead %.0f%%; on the floor plane (the station's measure) not behind the target %.0f%%, not overlapping the lead %.0f%%; close station %s; actions with the lead attacking within %.0f s: %s; over the whole run %d in %.1f s, %.1f per 20 s; blows with a strike clip %d, played whole %d; brain: %s)."),
				Clear >= 0.7f && (!bRateCovered || Engaged20 >= 4.f) ? TEXT("PASS") : TEXT("FAIL"), Clear * 100.f, PartnerFrustumSteps, Seen * 100.f, LeadClear * 100.f,
				ClearFlat * 100.f, LeadClearFlat * 100.f, !bCloseShape ? TEXT("none (classic)") : (bPartnerStation ? TEXT("on") : TEXT("off")),
				kPartnerEngagedWindow, *Rate, PartnerActions, PartnerPlaySeconds, Per20, ClipsStarted, ClipsWhole, *Brain);
			if (PartnerNearSteps > 0)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner rule 1 share %.0f%% (%d of %d close-camera steps no shallower than the lead's depth less 0.5 m and at least 2.5 m from the lens; logged only)."),
					100.f * static_cast<float>(PartnerNearOkSteps) / static_cast<float>(PartnerNearSteps), PartnerNearOkSteps, PartnerNearSteps);
			}
		}
	}
	if (bPartnerHeldOut)
	{
		// Begun Lone and not called in: out of the fight to its end on both sides (no partner, one lead in the session).
		const bool bOneLead = !Session || !Session->Get().inFight(OtherLead());
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner held out %s (begun Lone; steps the partner acted %d, the session's other lead in the fight %d, Lone now %d)."),
			PartnerHeldLeaks == 0 && bOneLead && !Fight.output().ally.partnerOn ? TEXT("PASS") : TEXT("FAIL"), PartnerHeldLeaks, bOneLead ? 0 : 1, Fight.lone() ? 1 : 0);
		return;
	}
	if (!PartnerBody())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner n/a (the fight is Lone: the partner never acts and only the lead is in it)."));
		return;
	}
	if (static_cast<double>(Fight.time()) - PartnerWinStart > 0.5)
	{
		LogPartnerWindow(); // the last, short window
	}
	const ftcore::ArenaStats& S = Fight.stats();
	const ftcore::ArenaDef& D = Fight.def();
	int32 BossHp = 0;
	int32 AllHp = 0;
	for (size_t I = 0; I < D.enemyCount; ++I)
	{
		AllHp += D.enemies[I].hp;
		BossHp += D.enemies[I].boss ? D.enemies[I].hp : 0;
	}
	const int32 Pool = BossHp > 0 ? BossHp : AllHp;
	const float ShareBoss = Pool > 0 ? static_cast<float>(S.partnerDamage) / static_cast<float>(Pool) : 0.f;
	const int32 Dealt = S.partnerDamage + S.damageDealt;
	const float ShareAll = Dealt > 0 ? static_cast<float>(S.partnerDamage) / static_cast<float>(Dealt) : 0.f;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner budget %s (%d windows, %d held; the most a window stood over its limit %+.2f s)."),
		PartnerWindows > 0 && PartnerWindowsHeld == PartnerWindows ? TEXT("PASS") : TEXT("FAIL"), PartnerWindows, PartnerWindowsHeld, PartnerWorstOver);
	const float AimCap = static_cast<float>(ftcore::kArenaBossAimShareTenths) / 10.f;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner aim %s (%d of %d boss strings at the partner, the highest share %.3f; at most %.1f)."),
		S.bossAimShareMax <= AimCap + 1e-4f ? TEXT("PASS") : TEXT("FAIL"), S.bossAimedPartner, S.bossAttacks, S.bossAimShareMax, AimCap);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: partner share %s (the partner's %d damage: %.3f of the %s %d hp, %.3f of all %d damage dealt; target %.2f-%.2f of the boss's)."),
		!bWon ? TEXT("n/a, not won") : (ShareBoss >= kPartnerShareLow && ShareBoss <= kPartnerShareHigh ? TEXT("in target") : TEXT("out of target")),
		S.partnerDamage, ShareBoss, BossHp > 0 ? TEXT("boss's") : TEXT("enemies'"), Pool, ShareAll, Dealt, kPartnerShareLow, kPartnerShareHigh);
	const bool bTakeoverOk = PartnerTakeovers == S.takeovers && PartnerSessionTookOver == PartnerTakeovers && PartnerLeadMismatch == 0
		&& bFiguresSwapped == Fight.swapped() && (SessionLead != StartLead) == Fight.swapped();
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner takeover %s (fight %d, host %d, session %d; steps the session's lead differed %d; swapped fight %d, figures %d)."),
		bTakeoverOk ? TEXT("PASS") : TEXT("FAIL"), S.takeovers, PartnerTakeovers, PartnerSessionTookOver, PartnerLeadMismatch,
		Fight.swapped() ? 1 : 0, bFiguresSwapped ? 1 : 0);
	// Each partner blow: one Hit event, and the session's partner path ruled it once (the player's path never).
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner blows ruled once %s (fight %d blows, %d Hit events, the session ruled %d, %d joined)."),
		!Session ? TEXT("n/a, no session") : (PartnerBlowsRuled == S.partnerBlows && PartnerHitEvents == S.partnerBlows ? TEXT("PASS") : TEXT("FAIL")),
		S.partnerBlows, PartnerHitEvents, PartnerBlowsRuled, PartnerJoins);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner swap input %s (%d takeovers, %d switches, %d checked (%d after a switch), %d passed; the new body moved at least %.0f cm with a move input, the fallen one at most %.0f cm, a switched one along the input at least %.2f)."),
		SwapChecks == 0 ? TEXT("n/a") : (SwapChecksPassed == SwapChecks ? TEXT("PASS") : TEXT("FAIL")), PartnerTakeovers, SwitchesDone, SwapChecks,
		SwapSwitchChecks, SwapChecksPassed, SwapChecks > 0 ? SwapWorstNew : 0.0, SwapWorstOld, SwapSwitchChecks > 0 ? SwapWorstAlign : 0.0);
	DropPendingHandoff(TEXT("the fight ended")); // a fight over inside a takeover's hold
	LogHandoffChecks();
	const bool bBothDown = Session && Session->Get().down(ftcore::kLeadA) && Session->Get().down(ftcore::kLeadB);
	// A party member only as the ally (its kit not playable): the lead's fall is the defeat, with no takeover.
	const bool bAllyNotPlayable = !ftcore::arenaPlayable(Fight.def().partnerKit);
	const bool bLoneFall = bAllyNotPlayable && S.takeovers == 0 && PartnerTakeovers == 0 && Session && Session->Get().down(SessionLead);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner defeat %s (session defeat %d, both leads down %d; ally playable %d, the lead down with no takeover %d)."),
		!bLost ? TEXT("n/a") : (bSessionDefeated && (bAllyNotPlayable ? bLoneFall : bBothDown) ? TEXT("PASS") : TEXT("FAIL")),
		bSessionDefeated ? 1 : 0, bBothDown ? 1 : 0, bAllyNotPlayable ? 0 : 1, bLoneFall ? 1 : 0);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: partner fight: falls lead %d, partner %d; takeovers %d; rises %d (fight %d); partner blows %d, strings %d, guards %d; order presses %d."),
		PartnerLeadFalls, PartnerAllyFalls, PartnerTakeovers, PartnerRises, S.partnerRises, S.partnerBlows, S.partnerStrings, S.partnerGuards, PartnerOrders);
}

namespace
{
constexpr int32 kSwapCheckMoves = 20;   // steps with a move input after a takeover before the figures are compared
constexpr int32 kSwapCheckSteps = 600;  // steps after it with too few move inputs: that takeover goes unchecked
constexpr double kSwapNewMoved = 20.0;  // cm: the controlled figure moves at least this ...
constexpr double kSwapOldStill = 5.0;   // ... and, after a takeover, the fallen one (lying, down) at most this
constexpr double kSwapAlign = 0.7;      // after a switch: the cosine between the new body's way and the summed input
}

void UFTArenaRunner::ArmSwapCheck(bool bSwitch)
{
	bSwapCheckArmed = true;
	bSwapCheckPlaced = false; // the places are read once the figures are drawn after the swap
	bSwapIsSwitch = bSwitch;
	SwapMoveSteps = SwapSteps = 0;
	SwapMoveSum = FVector2D::ZeroVector;
}

void UFTArenaRunner::TrackSwapCheck(const ftcore::ArenaInput& In)
{
	if (!bSwapCheckArmed || !bSwapCheckPlaced)
	{
		return;
	}
	++SwapSteps;
	if (In.move.x * In.move.x + In.move.y * In.move.y > 0.04f)
	{
		++SwapMoveSteps;
		SwapMoveSum += FVector2D(In.move.x, In.move.y); // ToWorld maps the fight's x, y onto the world's X, Y
	}
	if (SwapSteps > kSwapCheckSteps)
	{
		bSwapCheckArmed = false; // the lead stood: nothing to compare
	}
}

void UFTArenaRunner::CheckSwapInput()
{
	if (!bSwapCheckArmed || !PlayerFigure || !AllyFigure)
	{
		return;
	}
	if (!bSwapCheckPlaced)
	{
		bSwapCheckPlaced = true;
		SwapNewAt = PlayerFigure->GetActorLocation();
		SwapOldAt = AllyFigure->GetActorLocation();
		return;
	}
	if (SwapMoveSteps < kSwapCheckMoves)
	{
		return;
	}
	bSwapCheckArmed = false;
	// After a takeover the fallen body stays down for downSeconds, longer than the moves counted here, so it lies
	// still throughout. After a switch the partner brain walks the old body, so the new body's way must follow the
	// input: the cosine between its move and the summed move input.
	const FVector NewDelta = PlayerFigure->GetActorLocation() - SwapNewAt;
	const double NewMoved = FVector2D(NewDelta.X, NewDelta.Y).Size();
	const double OldMoved = FVector::Dist2D(AllyFigure->GetActorLocation(), SwapOldAt);
	const double SumLen = SwapMoveSum.Size();
	const double Align = NewMoved > 0.0 && SumLen > 0.0 ? (NewDelta.X * SwapMoveSum.X + NewDelta.Y * SwapMoveSum.Y) / (NewMoved * SumLen) : -1.0;
	const bool bOk = NewMoved >= kSwapNewMoved && (bSwapIsSwitch ? Align >= kSwapAlign : OldMoved <= kSwapOldStill);
	++SwapChecks;
	SwapChecksPassed += bOk ? 1 : 0;
	SwapWorstNew = FMath::Min(SwapWorstNew, NewMoved);
	if (bSwapIsSwitch)
	{
		++SwapSwitchChecks;
		SwapWorstAlign = FMath::Min(SwapWorstAlign, Align);
	}
	else
	{
		SwapWorstOld = FMath::Max(SwapWorstOld, OldMoved);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: %.2f: after the %s, %d steps with a move input: the controlled figure moved %.0f cm (along the input %.2f), the other one %.0f cm (%s)."),
		Fight.time(), bSwapIsSwitch ? TEXT("switch") : TEXT("takeover"), SwapMoveSteps, NewMoved, Align, OldMoved,
		bOk ? TEXT("input on the new body") : TEXT("NOT on the new body"));
}

void UFTArenaRunner::CheckPartnerRetry()
{
	const int32 WasSwapped = Fight.swapped() ? 1 : 0;
	const int32 Takeovers = PartnerTakeovers;
	Restart(); // the game mode's and the result screen's retry: the fight from its start
	// No tick has run since the retry's begin, so the switch back to the first lead may still wait for the first close.
	const bool bFirstLead = Session && (Session->Get().switchRequested() == StartLead
		|| (Session->Get().switchRequested() == ftcore::kNoFighter && Session->Get().activeLead() == StartLead));
	const bool bSessionOk = !Session || (bFirstLead && !Session->Get().down(ftcore::kLeadA)
		&& !Session->Get().down(ftcore::kLeadB) && !Session->Get().defeated());
	// Begun Lone: Lone again, with one lead (the partner out on both sides, its body hidden) until another call.
	const bool bOneLeadAgain = !bPartnerLoneStart || !Fight.lone()
		|| (bPartnerHeldOut && !bAllyOn && Fight.allyMaxHp() == 0 && Session && !Session->Get().inFight(OtherLead()));
	const bool bOk = !Fight.over() && !Fight.swapped() && !bFiguresSwapped && SessionLead == StartLead
		&& Fight.hp() == Fight.def().fighter.hp && Fight.allyHp() == Fight.allyMaxHp() && bSessionOk && bOneLeadAgain;
	UE_LOG(LogForgottenThird, Display, TEXT("Arena check: partner retry %s (from the fight's start after %d takeovers: swapped %d -> %d, lead %d, hp %d/%d, partner %d/%d, session %s)."),
		bOk ? TEXT("PASS") : TEXT("FAIL"), Takeovers, WasSwapped, Fight.swapped() ? 1 : 0, static_cast<int32>(SessionLead), Fight.hp(),
		Fight.def().fighter.hp, Fight.allyHp(), Fight.allyMaxHp(), bSessionOk ? TEXT("both up, first lead controlled") : TEXT("not reset"));
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
	bTable = bTableAuto = bTableFill = bTableKey = bTableOpen = false;
	TablePlaces = Places;
#if !UE_BUILD_SHIPPING
	const TCHAR* Cmd = FCommandLine::Get();
	bTableAuto = FParse::Param(Cmd, TEXT("ArenaTableAuto"));
	bTableFill = FParse::Param(Cmd, TEXT("ArenaTableFill"));
	bTableKey = FParse::Param(Cmd, TEXT("ArenaTableKey"));
	bTableOpen = FParse::Param(Cmd, TEXT("ArenaTableOpen"));
	bTable = FParse::Param(Cmd, TEXT("ArenaTable")) || bTableAuto || bTableFill || bTableKey || bTableOpen;
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
	SpendLeadLoadNow(Spent);
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
