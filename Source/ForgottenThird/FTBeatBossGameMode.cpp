#include "FTBeatBossGameMode.h"

#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTActionButtons.h"
#include "FTBeatBossHUD.h"
#include "FTBeatFighterAnimInstance.h"
#include "FTBeatBossMetronome.h"
#include "FTBeatBossMusic.h"
#include "FTConductorComponent.h"
#include "FTGameInstance.h"
#include "FTMeshFacing.h"
#include "FTPauseSubsystem.h"
#include "FTSceneLook.h"
#include "FTSettingsSubsystem.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#include <string>
#include <vector>

namespace
{
// The stage, centimetres, x from the lead towards the boss, z up (the floor's top at 0).
// The first shot, before the choreography takes the camera: the wide one, from the side.
const FVector kBossViewer(AFTBeatBossGameMode::FightGap * 0.5 - 30.0, 470.0, 160.0);
const FVector kBossViewTarget(AFTBeatBossGameMode::FightGap * 0.5, 0.0, 120.0);

// A -BeatBossQuitOnFinish or -BeatBossShot run quits with code 2 after this many wall seconds.
constexpr double kBossWatchdog = 300.0;
// A defeat shows this long (the fight's seconds) before the retry prompt (or an autoplay's own retry).
constexpr double kBossDefeatShow = 1.4;
constexpr double kBossAutoDefeatShow = 0.5;
// An autoplay run confirms the retry prompt once it has shown this long (the fight's seconds), for a lost
// phase this many times at most, then ends lost.
constexpr double kBossAutoConfirm = 1.0;
constexpr int32 kBossAutoRetries = 3;
// A won fight with -BeatBossQuitOnFinish quits after showing its result this long.
constexpr double kBossVictoryShow = 1.0;
// Each -BeatBossShot view holds the fight still this long (wall seconds) while the shot is written.
constexpr double kBossShotHold = 1.0;

// The views -BeatBossShot renders (the file name's number and name), in the order they are looked for. A view the
// track never gives is left out, with a warning; the run ends after the result. Every one of the eight shots, a prompt
// of every answer, the hit-stop, the silence's roar, the stagger, the finisher and the victory's freeze and ramp.
enum EBossView : int32
{
	ViewCountIn,
	ViewShot0, // + the shot's number (wide, over_lead, over_boss, close_lead, close_boss, low_hero, top_down, finisher_orbit)
	ViewAnswer0 = ViewShot0 + 8, // + the answer's number (attack, guard, parry, dodge, jump, hold, paired, release)
	ViewStrike = ViewAnswer0 + 8,
	ViewSilence,
	ViewStagger,
	ViewFinisher,
	ViewFreeze,
	ViewRamp1,
	ViewRamp2,
	ViewRetry,
	ViewResult,
	ViewCount
};
static_assert(ViewCount <= 32, "the views are a bit each in a 32-bit mask");
const TCHAR* const kBossViewNames[ViewCount] = {
	TEXT("01_countin"),
	TEXT("10_shot_wide"), TEXT("11_shot_over_lead"), TEXT("12_shot_over_boss"), TEXT("13_shot_close_lead"), TEXT("14_shot_close_boss"),
	TEXT("15_shot_low_hero"), TEXT("16_shot_top_down"), TEXT("17_shot_finisher_orbit"),
	TEXT("20_prompt_attack"), TEXT("21_prompt_guard"), TEXT("22_prompt_parry"), TEXT("23_prompt_dodge"), TEXT("24_prompt_jump"),
	TEXT("25_prompt_hold"), TEXT("26_prompt_paired"), TEXT("27_prompt_release"),
	TEXT("30_hitstop"), TEXT("31_silence_roar"), TEXT("32_stagger"), TEXT("33_finisher"),
	TEXT("40_victory_freeze"), TEXT("41_victory_ramp_a"), TEXT("42_victory_ramp_b"), TEXT("43_retry"), TEXT("44_result")};
// Seconds of the fight's clock between two frames of -BeatBossSeq (10 frames a second).
constexpr double kBossSeqStep = 0.1;
// The camera's shake dies away at this rate (trauma per second).
constexpr float kBossTraumaDecay = 1.9f;

const FName kBoneHead(TEXT("head"));
const FName kBoneChest(TEXT("spine_03"));
const FName kBoneHandR(TEXT("hand_r"));
const FName kBoneHandL(TEXT("hand_l"));

const TCHAR* const kBossHumanMesh = TEXT("/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_human.test_human");
const TCHAR* const kBossHumanIdle = TEXT("/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_humanidle.test_humanidle");

// The boss's hide, cloth and hair; white while a blow lands and amber while it is staggered (mixed into the hide).
const FLinearColor kBossHide(0.22f, 0.03f, 0.03f);
const FLinearColor kBossCloth(0.07f, 0.012f, 0.012f);
const FLinearColor kBossHair(0.015f, 0.01f, 0.01f);
const FLinearColor kBossStruck(0.55f, 0.48f, 0.42f);
const FLinearColor kBossDazed(0.7f, 0.3f, 0.04f);

// A shot: where the camera stands, what it looks at, its lens (focal length, millimetres, on a 24.576 mm sensor), the
// aperture its depth of field uses (f-stop; the focus is the target's distance), and how bright each light is for it.
struct FBossShot
{
	FVector Pos = FVector::ZeroVector;
	FVector Target = FVector::ZeroVector;
	float Mm = 28.f;
	float Fstop = 8.f;
	float Key = 1.f;
	float RimBoss = 1.f;
	float RimLead = 1.f;
	float Kicker = 1.f;
};

// The horizontal field of view of a lens on the 24.576 mm sensor, degrees.
float BossFov(float Mm)
{
	return FMath::RadiansToDegrees(2.f * FMath::Atan(12.288f / FMath::Max(1.f, Mm)));
}

float BossSmooth(float X)
{
	const float T = FMath::Clamp(X, 0.f, 1.f);
	return T * T * (3.f - 2.f * T);
}

// A bump that rises to 1 over Rise seconds and falls back to 0 over Fall: the age of an effect in, its strength out.
float BossBump(double Age, float Rise, float Fall)
{
	if (Age < 0.0)
	{
		return 0.f;
	}
	if (Age < Rise)
	{
		return static_cast<float>(Age) / Rise;
	}
	const float T = static_cast<float>(Age - Rise) / Fall;
	return T >= 1.f ? 0.f : 1.f - T;
}

// Retry: Enter, Space or the pad's right shoulder; give up: Q, Backspace or the pad's left shoulder. The pad's face
// buttons are the fight's (Jump and Dodge among them), so a player still pressing them never retries or gives up.
bool BossYesPressed(const APlayerController& PC)
{
	return PC.WasInputKeyJustPressed(EKeys::Enter) || PC.WasInputKeyJustPressed(EKeys::SpaceBar)
		|| PC.WasInputKeyJustPressed(EKeys::Gamepad_RightShoulder);
}

bool BossNoPressed(const APlayerController& PC)
{
	return PC.WasInputKeyJustPressed(EKeys::Q) || PC.WasInputKeyJustPressed(EKeys::BackSpace)
		|| PC.WasInputKeyJustPressed(EKeys::Gamepad_LeftShoulder);
}

// None of the result's keys is held down.
bool BossResultKeysUp(const APlayerController& PC)
{
	return !PC.IsInputKeyDown(EKeys::Enter) && !PC.IsInputKeyDown(EKeys::SpaceBar) && !PC.IsInputKeyDown(EKeys::Gamepad_RightShoulder)
		&& !PC.IsInputKeyDown(EKeys::Q) && !PC.IsInputKeyDown(EKeys::BackSpace) && !PC.IsInputKeyDown(EKeys::Gamepad_LeftShoulder);
}
// The result's keys are read once it has shown this long and every one has been let go; a Give up asks twice within this long.
constexpr double kBossResultArm = 0.5;
constexpr double kBossGiveUpWindow = 4.0;

// A track id is a file name: letters, digits and underscores only.
bool BossValidId(const FString& Id)
{
	if (Id.IsEmpty() || Id.Len() > 64)
	{
		return false;
	}
	for (const TCHAR C : Id)
	{
		if (!FChar::IsAlnum(C) && C != TEXT('_'))
		{
			return false;
		}
	}
	return true;
}

} // namespace

AFTBeatBossGameMode::AFTBeatBossGameMode()
{
	HUDClass = AFTBeatBossHUD::StaticClass();
	bHostsStory = false;
	PrimaryActorTick.bCanEverTick = true;

	// The fight's clock: the pause-aware wall clock (no music is bound; the metronome follows the clock).
	Conductor = CreateDefaultSubobject<UFTConductorComponent>(TEXT("Conductor"));
	Conductor->bWallClockWithoutAudio = true;

	for (double& P : Pulses)
	{
		P = -1e9;
	}
}

FName AFTBeatBossGameMode::BossTag()
{
	return FName(TEXT("BeatBoss"));
}

FName AFTBeatBossGameMode::LeadTag()
{
	return FName(TEXT("BeatBossLead"));
}

FName AFTBeatBossGameMode::KeyTag()
{
	return FName(TEXT("BeatKey"));
}

FName AFTBeatBossGameMode::RimBossTag()
{
	return FName(TEXT("BeatRimBoss"));
}

FName AFTBeatBossGameMode::RimLeadTag()
{
	return FName(TEXT("BeatRimLead"));
}

FName AFTBeatBossGameMode::KickerTag()
{
	return FName(TEXT("BeatKicker"));
}

float AFTBeatBossGameMode::StageExposure()
{
	// The manual exposure's bias (absolute: in manual mode it scales a fixed EV100 of 0), found by eye on the wide shot with
	// -BeatBossExposure: the stage is dark, and a lens must never change how bright it is.
	return 0.0f;
}

FVector AFTBeatBossGameMode::BossLocation()
{
	return FVector(FightGap, 0.0, 0.0);
}

FVector AFTBeatBossGameMode::LeadLocation()
{
	return FVector::ZeroVector;
}

FVector AFTBeatBossGameMode::ViewerLocation()
{
	return kBossViewer;
}

FRotator AFTBeatBossGameMode::ViewerRotation()
{
	return (kBossViewTarget - kBossViewer).Rotation();
}

void AFTBeatBossGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as the other demo modes do: the story must not start here.
	AGameModeBase::StartPlay();

	StartWall = UFTPauseSubsystem::WallSeconds();
	const TCHAR* Cmd = FCommandLine::Get();
	FString Shot;
	if (FParse::Value(Cmd, TEXT("BeatBossShot="), Shot) && !Shot.IsEmpty())
	{
		ShotPrefix = Shot;
		float Delay = ShotDelay;
		if (FParse::Value(Cmd, TEXT("BeatBossShotDelay="), Delay))
		{
			ShotDelay = FMath::Clamp(Delay, 0.f, 60.f);
		}
	}
	FString Seq;
	if (FParse::Value(Cmd, TEXT("BeatBossSeq="), Seq) && !Seq.IsEmpty() && ShotPrefix.IsEmpty())
	{
		SeqPrefix = Seq;
		int32 Frames = SeqCount;
		if (FParse::Value(Cmd, TEXT("BeatBossSeqCount="), Frames))
		{
			SeqCount = FMath::Clamp(Frames, 1, 120);
		}
		float Delay = ShotDelay;
		if (FParse::Value(Cmd, TEXT("BeatBossShotDelay="), Delay))
		{
			ShotDelay = FMath::Clamp(Delay, 0.f, 60.f);
		}
	}
	bRootMotion = FParse::Param(Cmd, TEXT("BeatBossRootMotion"));
	bIdle = FParse::Param(Cmd, TEXT("BeatBossIdle"));
	int32 Miss = 0;
	if (!bIdle && FParse::Value(Cmd, TEXT("BeatBossMissPhase="), Miss) && Miss > 0)
	{
		MissPhase = Miss;
	}
	bAutoPlay = !bIdle && (FParse::Param(Cmd, TEXT("BeatBossAutoPlay")) || !ShotPrefix.IsEmpty() || !SeqPrefix.IsEmpty() || MissPhase > 0);
	bQuitOnFinish = FParse::Param(Cmd, TEXT("BeatBossQuitOnFinish"));

	FString Id = TEXT("cine_track");
	FParse::Value(Cmd, TEXT("BeatBossTrack="), Id);
	LoadAccess();
	ExposureEv = StageExposure();
	FParse::Value(Cmd, TEXT("BeatBossExposure="), ExposureEv);
	if (!LoadTrack(Id))
	{
		Stage = EStage::Done;
		if (bQuitOnFinish || !ShotPrefix.IsEmpty() || !SeqPrefix.IsEmpty())
		{
			Quit(3);
		}
		return;
	}
	LoadChoreo(Id);

	if (MissPhase > 0)
	{
		// The host drives the autopilot itself (Pilot), so it can leave a phase unanswered.
		MissPhase = FMath::Min(MissPhase, static_cast<int32>(Track.def.phaseCount));
	}
	ApplyAccess(false);
	UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: access %s"), *FT::ToFString(ftcore::encodeBeatAccess(Access)));

	FindStage();

	// Look through the map's camera rather than the pawn.
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		for (TActorIterator<ACameraActor> It(GetWorld()); It; ++It)
		{
			Camera = *It;
			PC->SetViewTarget(*It);
			break;
		}
	}

	// The music: one procedural sound for the whole fight. No audio device (-NullRHI runs may have none) leaves
	// the audio component null and nothing sounds. -BeatBossMetronome brings back the old click-and-stems stand-in.
	if (FParse::Param(Cmd, TEXT("BeatBossMetronome")))
	{
		Metronome = NewObject<UFTBeatBossMetronome>(this);
		MetronomeAudio = UGameplayStatics::SpawnSound2D(this, Metronome, 1.f, 1.f, 0.f, nullptr, false, false);
		if (Track.music.empty())
		{
			UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: track %s on the wall clock with the metronome."), *Id);
		}
		else
		{
			UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: track %s on the wall clock with the metronome standing in for music cue %s."), *Id,
				*FT::ToFString(Track.music));
		}
	}
	else
	{
		Music = NewObject<UFTBeatBossMusic>(this);
		MusicAudio = UGameplayStatics::SpawnSound2D(this, Music, 1.f, 1.f, 0.f, nullptr, false, false);
		if (Track.music.empty())
		{
			UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: track %s on the wall clock with the procedural music."), *Id);
		}
		else
		{
			UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: track %s on the wall clock with the procedural music standing in for music cue %s."), *Id,
				*FT::ToFString(Track.music));
		}
	}

	// The fight reads the conductor's clock, so this tick comes after the conductor has taken its frame's.
	AddTickPrerequisiteComponent(Conductor);
}

void AFTBeatBossGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UNiagaraSystem* Rooted = PuffSystem.Get())
	{
		Rooted->RemoveFromRoot();
	}
	if (MetronomeAudio)
	{
		MetronomeAudio->Stop();
	}
	if (MusicAudio)
	{
		MusicAudio->Stop();
	}
	Super::EndPlay(EndPlayReason);
}

bool AFTBeatBossGameMode::LoadTrack(const FString& Id)
{
	if (!BossValidId(Id))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: '%s' is not a track id (letters, digits and underscores)."), *Id);
		return false;
	}
	const FString Rel = FString::Printf(TEXT("Data/Battle/beat_boss/%s.json"), *Id);
	const FString File = FPaths::ProjectDir() / Rel;
	TArray<uint8> Bytes;
	if (!IFileManager::Get().FileExists(*File) || !FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: couldn't read %s."), *Rel);
		return false;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ftcore::importBeatBossTrack(FT::BytesAsText(Bytes), Track, &Error, &Warnings))
	{
		// The loader's errors name paths and keys only.
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: %s refused:\n%s"), *Rel, *FT::ToFString(Error));
		return false;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: %s: %s"), *Rel, *FT::ToFString(W));
	}
	if (Access.tempoScale < 1.f)
	{
		// The practice tempo: every bpm and every time scaled together (never a phase under 40 bpm).
		Track.def = ftcore::rescaleBeatBossDef(Track.def, static_cast<double>(Access.tempoScale));
		UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: practice tempo %.2f."), static_cast<double>(Access.tempoScale));
	}
	return true;
}

bool AFTBeatBossGameMode::LoadChoreo(const FString& Id)
{
	TArray<uint8> Bytes;
	const FString ClipsFile = FPaths::ProjectDir() / TEXT("Data/Anim/beat_clips.json");
	if (!IFileManager::Get().FileExists(*ClipsFile) || !FFileHelper::LoadFileToArray(Bytes, *ClipsFile))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: couldn't read Data/Anim/beat_clips.json; the fighters will stand."));
		return false;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ftcore::importBeatClips(FT::BytesAsText(Bytes), Clips, &Error, &Warnings))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: Data/Anim/beat_clips.json refused:\n%s"), *FT::ToFString(Error));
		return false;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: Data/Anim/beat_clips.json: %s"), *FT::ToFString(W));
	}
	// The clips themselves, by catalogue index.
	ClipAssets.Reset();
	ClipAssets.SetNum(static_cast<int32>(Clips.count));
	int32 Loaded = 0;
	for (size_t I = 0; I < Clips.count; ++I)
	{
		const FString Package = FT::ToFString(std::string(Clips.asset(I)));
		if (Package.IsEmpty())
		{
			continue;
		}
		const FString Path = Package + TEXT(".") + FPackageName::GetShortName(Package);
		ClipAssets[static_cast<int32>(I)] = LoadObject<UAnimSequence>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (ClipAssets[static_cast<int32>(I)])
		{
			++Loaded;
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: clip %s is missing (%s)."), *FT::ToFString(std::string(Clips.id(I))), *Package);
		}
	}
	StanceLeadClip = Clips.find("lead_stance");
	StanceBossClip = Clips.find("boss_stance");

	const FString Rel = FString::Printf(TEXT("Data/Battle/beat_boss/%s.choreo.json"), *Id);
	const FString File = FPaths::ProjectDir() / Rel;
	if (!IFileManager::Get().FileExists(*File) || !FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: %s is missing; the fighters will stand and the camera stay wide."), *Rel);
		return false;
	}
	if (!ftcore::importBeatChoreo(FT::BytesAsText(Bytes), Track, Clips, ChoreoDef, &Error, &Warnings))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: %s refused:\n%s"), *Rel, *FT::ToFString(Error));
		return false;
	}
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: %s: %s"), *Rel, *FT::ToFString(W));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: %d of %d clips loaded; choreography %s."), Loaded, static_cast<int32>(Clips.count), *Rel);
	BossAttackClip.Init(false, static_cast<int32>(Clips.count));
	BossClipArm.Init(0, static_cast<int32>(Clips.count));
	for (size_t P = 0; P < ChoreoDef.phaseCount; ++P)
	{
		const ftcore::ChoreoPhase& Phase = ChoreoDef.phases[P];
		for (size_t K = 0; K < Phase.moveCount; ++K)
		{
			if (Phase.moves[K].boss >= 0)
			{
				BossAttackClip[Phase.moves[K].boss] = true;
				if (Phase.moves[K].anchor == ftcore::ChoreoAnchor::BossFistR)
				{
					BossClipArm[Phase.moves[K].boss] |= 1;
				}
				else if (Phase.moves[K].anchor == ftcore::ChoreoAnchor::BossFistL)
				{
					BossClipArm[Phase.moves[K].boss] |= 2;
				}
			}
		}
		for (size_t K = 0; K < Phase.finisherCount; ++K)
		{
			if (Phase.finisher[K].boss >= 0)
			{
				BossAttackClip[Phase.finisher[K].boss] = true;
			}
		}
	}
	Choreo.begin(ChoreoDef, Clips);
	return true;
}

void AFTBeatBossGameMode::PlaceFighter(AActor* Actor, const FVector& Location, double Yaw, double Scale)
{
	if (Actor)
	{
		Actor->SetActorLocationAndRotation(Location, FRotator(0.0, Yaw, 0.0));
		Actor->SetActorScale3D(FVector(Scale));
	}
}

void AFTBeatBossGameMode::SpawnFighters()
{
	UWorld* World = GetWorld();
	USkeletalMesh* Human = LoadObject<USkeletalMesh>(nullptr, kBossHumanMesh, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!World || !Human)
	{
		return;
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// A map built before the fighters were skeletal (or by hand) lacks one: the test human is made here.
	const auto Make = [&](TWeakObjectPtr<AActor>& Slot, FName Tag, const TCHAR* Label)
	{
		if (Slot.IsValid())
		{
			return;
		}
		ASkeletalMeshActor* Actor = World->SpawnActor<ASkeletalMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
		if (!Actor)
		{
			return;
		}
		USkeletalMeshComponent* Skin = Actor->GetSkeletalMeshComponent();
		Skin->SetMobility(EComponentMobility::Movable);
		Skin->SetSkeletalMesh(Human);
		Skin->SetRelativeRotation(FRotator(0.0, FTMeshFacing::FacingYaw(*Human), 0.0));
		Actor->Tags.Add(Tag);
#if WITH_EDITOR
		Actor->SetActorLabel(Label);
#endif
		Slot = Actor;
	};
	Make(Lead, LeadTag(), TEXT("Lead"));
	Make(Boss, BossTag(), TEXT("Boss"));
}

void AFTBeatBossGameMode::FindStage()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(BossTag()))
		{
			Boss = *It;
		}
		else if (It->ActorHasTag(LeadTag()))
		{
			Lead = *It;
		}
	}
	SpawnFighters();

	// Both fighters stand where the stage puts them (the map may have been built with other numbers), the boss
	// facing back at the lead and 1.35 times the size.
	BossHome = BossLocation();
	LeadHome = LeadLocation();
	PlaceFighter(Lead.Get(), LeadHome, 0.0, 1.0);
	PlaceFighter(Boss.Get(), BossHome, 180.0, BossScaleFactor);

	// Each plays through UFTBeatFighterAnimInstance: the clip the choreography names, at the time it names.
	const auto Setup = [this](AActor* Actor, TWeakObjectPtr<USkeletalMeshComponent>& Slot)
	{
		USkeletalMeshComponent* Skin = Actor ? Actor->FindComponentByClass<USkeletalMeshComponent>() : nullptr;
		Slot = Skin;
		if (!Skin)
		{
			return;
		}
		if (!Skin->GetSkeletalMeshAsset())
		{
			if (USkeletalMesh* Human = LoadObject<USkeletalMesh>(nullptr, kBossHumanMesh, nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				Skin->SetSkeletalMesh(Human);
			}
		}
		Skin->SetAnimationMode(EAnimationMode::AnimationCustomMode);
		Skin->SetAnimInstanceClass(UFTBeatFighterAnimInstance::StaticClass());
		// Bones are read for the prompts and the camera even where the mesh is off screen or unrendered.
		Skin->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		// The pose is set in this actor's tick: the mesh follows it.
		Skin->AddTickPrerequisiteActor(this);
		if (UFTBeatFighterAnimInstance* Anim = Cast<UFTBeatFighterAnimInstance>(Skin->GetAnimInstance()))
		{
			Anim->SetHoldRoot(!bRootMotion);
		}
	};
	Setup(Lead.Get(), LeadMesh);
	Setup(Boss.Get(), BossMesh);

	// The map's lights, by tag, with what they were built at: each shot scales them apart. A glow on the boss's fist
	// that rises across a tell is made here (a map has no such light).
	StageLights.Reset();
	StageLightBase.Reset();
	StageLightTags.Reset();
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		for (const FName& Tag : {KeyTag(), RimBossTag(), RimLeadTag(), KickerTag()})
		{
			if (It->ActorHasTag(Tag))
			{
				if (UPointLightComponent* Lamp = It->FindComponentByClass<UPointLightComponent>())
				{
					StageLights.Add(Lamp);
					StageLightBase.Add(Lamp->Intensity);
					StageLightTags.Add(Tag);
				}
			}
		}
	}
	if (AActor* BossActor = Boss.Get(); BossActor && BossMesh.IsValid() && BossMesh->GetBoneIndex(FName(TEXT("hand_r"))) != INDEX_NONE)
	{
		UPointLightComponent* Glow = NewObject<UPointLightComponent>(BossActor, TEXT("FistGlow"));
		Glow->SetMobility(EComponentMobility::Movable);
		Glow->SetIntensity(0.f);
		Glow->SetLightColor(FLinearColor(1.f, 0.35f, 0.12f));
		Glow->SetAttenuationRadius(260.f);
		Glow->SetCastShadows(false);
		Glow->RegisterComponent();
		Glow->AttachToComponent(BossMesh.Get(), FAttachmentTransformRules::SnapToTargetNotIncludingScale, FName(TEXT("hand_r")));
		HandLight = Glow;
		HandLightBone = FName(TEXT("hand_r"));
	}
	if (!Lead.IsValid())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: the map has no lead (an actor tagged %s; FTBuildBeatBossWorld builds it)."), *LeadTag().ToString());
	}
	if (!Boss.IsValid())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: the map has no boss (an actor tagged %s; FTBuildBeatBossWorld builds it)."), *BossTag().ToString());
	}

	// The boss is the same body in a dark red hide: each of the body's materials (the character material, its
	// BaseColor and Emissive) is made over as an instance of its own tinted for the boss, its eyes burning.
	if (USkeletalMeshComponent* Skin = BossMesh.Get())
	{
		BossMaterials.Reset();
		BossBase.Reset();
		BossEmissive.Reset();
		for (int32 I = 0; I < Skin->GetNumMaterials(); ++I)
		{
			UMaterialInterface* Source = Skin->GetMaterial(I);
			if (!Source)
			{
				continue;
			}
			const FString Name = Source->GetName();
			FLinearColor Base = kBossHide;
			FLinearColor Glow = FLinearColor::Black;
			if (Name.Contains(TEXT("eye")))
			{
				Base = FLinearColor(0.02f, 0.01f, 0.01f);
				Glow = FLinearColor(3.2f, 0.9f, 0.12f);
			}
			else if (Name.Contains(TEXT("hair")))
			{
				Base = kBossHair;
			}
			else if (Name.Contains(TEXT("shirt")) || Name.Contains(TEXT("shorts")) || Name.Contains(TEXT("shoe")))
			{
				Base = kBossCloth;
			}
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Source, this);
			Mid->SetVectorParameterValue(TEXT("BaseColor"), Base);
			Mid->SetVectorParameterValue(TEXT("Emissive"), Glow);
			Skin->SetMaterial(I, Mid);
			BossMaterials.Add(Mid);
			BossBase.Add(Base);
			BossEmissive.Add(Glow);
		}
	}
}

void AFTBeatBossGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bQuitting || !Conductor || Track.def.phaseCount == 0)
	{
		return;
	}
	const double Wall = UFTPauseSubsystem::WallSeconds();
	if ((bQuitOnFinish || !ShotPrefix.IsEmpty() || !SeqPrefix.IsEmpty()) && Wall - StartWall > kBossWatchdog)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: the run didn't finish within %.0f s; quitting."), kBossWatchdog);
		LogResult();
		Quit(2);
		return;
	}
	if (!Conductor->IsClockValid())
	{
		return;
	}

	PollAccess(Wall);
	const double Song = Conductor->GetSongTime();
	if (Stage == EStage::Waiting)
	{
		LastSong = Song;
		Now = Song - HeldSeconds;
		// A shot run waits for the shaders to settle, so the count-in isn't drawn over a grey world.
		if (!ShotPrefix.IsEmpty() && Wall - StartWall < ShotDelay)
		{
			return;
		}
		Begin();
	}

	// A view being written holds the fight's clock still.
	const bool bHold = Shots() || Sequence();
	if (bQuitting)
	{
		return;
	}
	if (bHold)
	{
		HeldSeconds += Song - LastSong;
	}
	LastSong = Song;
	Now = Song - HeldSeconds;
	if (bHold)
	{
		Scene(DeltaSeconds);
		return;
	}

	switch (Stage)
	{
	case EStage::Playing:
		// Presses first: they are stamped with the clock as it reads in this frame, then the fight takes the frame.
		if (MissPhase > 0)
		{
			Pilot();
		}
		else if (!bAutoPlay && !bIdle)
		{
			PollKeys(Conductor->GetInputSongTime() - HeldSeconds);
		}
		if (Stage == EStage::Playing && !Fight.over())
		{
			Handle(Fight.update(Now), Now);
		}
		Fight.beatLine(Line);
		if (Music)
		{
			MusicFrame();
		}
		else
		{
			Sound();
		}
		if (Fight.over())
		{
			Finish();
		}
		break;

	case EStage::Result:
		if (Fight.state() == ftcore::BeatBossState::Victory)
		{
			if (bQuitOnFinish && ShotPrefix.IsEmpty() && GetResultAge() >= kBossVictoryShow)
			{
				Quit(0);
				return;
			}
			PollChoice();
			break;
		}
		if (GetResultAge() < (bAutoPlay ? kBossAutoDefeatShow : kBossDefeatShow))
		{
			break;
		}
		if (bIdle)
		{
			Stage = EStage::Done;
			if (bQuitOnFinish)
			{
				Quit(1);
				return;
			}
		}
		else if (bAutoPlay)
		{
			if (AutoRetries < kBossAutoRetries)
			{
				// The prompt shows as it would for a player, then is confirmed on its own.
				Stage = EStage::RetryPrompt;
				PromptTime = Now;
			}
			else
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: autoplay lost the fight and its %d retries; it ends lost."), AutoRetries);
				bGaveUp = true;
				Stage = EStage::Done;
				if (bQuitOnFinish)
				{
					Quit(1);
					return;
				}
			}
		}
		else
		{
			Stage = EStage::RetryPrompt;
			ResultShownAt = Now;
			bResultArmed = false;
			bGiveUpAsked = false;
		}
		break;

	case EStage::RetryPrompt:
		if (bAutoPlay)
		{
			if (Now - PromptTime >= kBossAutoConfirm)
			{
				++AutoRetries;
				Retry();
			}
			break;
		}
		PollChoice();
		break;

	case EStage::Done:
		PollChoice();
		break;

	default:
		break;
	}
	Scene(DeltaSeconds);
}

void AFTBeatBossGameMode::PollKeys(double EventTime)
{
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	// The frame after the pause menu closed, a key still held is not a press of the fight's own: it counts
	// once it is let go and pressed again.
	const bool bResumed = UFTPauseSubsystem::IsJustResumed(this);
	for (int32 D = 0; D < FTActionButtons::Count; ++D)
	{
		bool bPressed = false;
		bool bReleased = false;
		bool bNowDown = false;
		for (int32 K = 0; K < FTActionButtons::KeysPerButton; ++K)
		{
			const FKey& Key = FTActionButtons::Key(D, K);
			bPressed |= PC->WasInputKeyJustPressed(Key);
			bReleased |= PC->WasInputKeyJustReleased(Key);
			bNowDown |= PC->IsInputKeyDown(Key);
		}
		const ftcore::ActionButton Button = FTActionButtons::Button(D);
		if (!bResumed && (bPressed || (bNowDown && !bDown[D])))
		{
			Handle(Fight.press(Button, EventTime), EventTime);
		}
		if (bReleased || (!bNowDown && bDown[D]))
		{
			Handle(Fight.release(Button, EventTime), EventTime);
		}
		bDown[D] = bNowDown;
	}
}

void AFTBeatBossGameMode::PollChoice()
{
	if (bAutoPlay || bIdle)
	{
		return;
	}
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || UFTPauseSubsystem::IsJustResumed(this))
	{
		return;
	}
	// A key still held from the fight (or a mash that was meant for it) must not answer the result: the screen is read
	// once it has shown half a second and every key of its own has been let go.
	if (!bResultArmed)
	{
		bResultArmed = Now - ResultShownAt >= kBossResultArm && BossResultKeysUp(*PC);
		return;
	}
	const bool bYes = BossYesPressed(*PC);
	const bool bNo = BossNoPressed(*PC);
	if (bGiveUpAsked && Now - GiveUpAskedAt > kBossGiveUpWindow)
	{
		bGiveUpAsked = false;
	}
	switch (Stage)
	{
	case EStage::RetryPrompt:
		if (bYes)
		{
			bGiveUpAsked = false;
			Retry();
		}
		else if (bNo)
		{
			// Giving up is asked twice: a stray press must not end the run.
			if (!bGiveUpAsked)
			{
				bGiveUpAsked = true;
				GiveUpAskedAt = Now;
				break;
			}
			UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: given up after %d retries."), Retries);
			bGaveUp = true;
			bGiveUpAsked = false;
			Stage = EStage::Done;
			ResultTime = Now;
			if (bQuitOnFinish)
			{
				Quit(1);
			}
		}
		break;
	case EStage::Result:
	case EStage::Done:
		// A won fight, or one given up: the track again from the top.
		if (bYes)
		{
			Begin();
		}
		break;
	default:
		break;
	}
}

void AFTBeatBossGameMode::Begin()
{
	LoadAccess();
	ApplyAccess(false);
	Fight.begin(Track.def, Tuning, Now, Assist);
	AppliedAssist = Assist;
	PendingHitCount = 0;
	bResultArmed = false;
	bGiveUpAsked = false;
	CameraShot = ftcore::ChoreoShot::Wide;
	LastCutNow = -1e9;
	JoltStart = -1e9;
	Fight.beatLine(Line);
	Stage = EStage::Playing;
	BeginTime = Now;
	ResultTime = Now;
	Retries = 0;
	AutoRetries = 0;
	bMissing = false;
	bGaveUp = false;
	bResultLogged = false;
	PopupTotal = 0;
	for (double& P : Pulses)
	{
		P = -1e9;
	}
	LockoutUntil = -1e9;
	SilenceSeconds = 0.0;
	CountKind = Track.def.leadInCount;
	LeadMoveTime = -1e9;
	Trauma = 0.f;
	PromptViewCount = 0;
	for (FAnchorMemo& M : AnchorMemos)
	{
		M = FAnchorMemo{};
	}
	if (Choreo.begun())
	{
		Choreo.begin(ChoreoDef, Clips);
		LastHitContacts = Choreo.hitContacts();
	}
	SoundBars = -1;
	SoundPlayhead = 0.0;
	SoundCueCount = 0;
	SoundCueId = 0;
	SoundTrack = Fight.trackTime();
	MusicReset();
	UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: begin (%d phases, boss %d hp, lead %d hp)%s%s; losing phase %d (0: none)."),
		static_cast<int32>(Track.def.phaseCount), Track.def.bossHp, Track.def.leadHp, bAutoPlay ? TEXT(", autoplay") : TEXT(""),
		bIdle ? TEXT(", idle") : TEXT(""), MissPhase);
}

void AFTBeatBossGameMode::Retry()
{
	const int32 FromBar = Fight.stats().bars;
	// The options are read again, and Core takes them with the retry.
	LoadAccess();
	ApplyAccess(true);
	AppliedAssist = Assist;
	PendingHitCount = 0;
	bResultArmed = false;
	bGiveUpAsked = false;
	Fight.retry(Now);
	if (Choreo.begun())
	{
		Choreo.retry(Fight);
	}
	Fight.beatLine(Line);
	++Retries;
	bMissing = false;
	Stage = EStage::Playing;
	bResultLogged = false;
	LockoutUntil = -1e9;
	const int32 Phase = FMath::Clamp(Fight.phase(), 0, static_cast<int32>(Track.def.phaseCount) - 1);
	CountKind = Track.def.phases[static_cast<size_t>(Phase)].count;
	SoundBars = -1;
	SoundCueCount = 0;
	SoundTrack = Fight.trackTime();
	MusicReset();
	Pulse(EPulse::Phase, Now, Phase);
	UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: retry %d from phase %d's checkpoint (boss %d hp, lead %d hp), lost after %d bars."), Retries,
		Phase + 1, Fight.bossHp(), Fight.hp(), FromBar);
}

void AFTBeatBossGameMode::Pilot()
{
	if (!bMissing && Retries == 0 && Fight.phase() + 1 == MissPhase && !Fight.counting())
	{
		bMissing = true;
		UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: losing phase %d on purpose (boss %d hp)."), MissPhase, Fight.bossHp());
	}
	if (bMissing)
	{
		return;
	}
	// As the tests' pilot does: every action due by now, each at its own time (several may fall in a frame).
	for (int32 Guard = 0; Guard < 64; ++Guard)
	{
		const ftcore::QteAction A = Fight.autopilot(Now);
		if (A.kind == ftcore::QteAction::Kind::None)
		{
			break;
		}
		Handle(A.kind == ftcore::QteAction::Kind::Press ? Fight.press(A.input, A.time) : Fight.release(A.input, A.time), A.time);
	}
}

void AFTBeatBossGameMode::Finish()
{
	Stage = EStage::Result;
	ResultTime = Now;
	ResultShownAt = Now;
	bResultArmed = false;
	bGiveUpAsked = false;
	LogResult();
}

void AFTBeatBossGameMode::LogResult() const
{
	const ftcore::BeatBossStats& S = Fight.stats();
	const TCHAR* Outcome = Fight.state() == ftcore::BeatBossState::Victory ? TEXT("Victory")
		: Fight.state() == ftcore::BeatBossState::Defeat                   ? TEXT("Defeat")
																			: TEXT("Unfinished");
	// Counts only; never text.
	UE_LOG(LogForgottenThird, Display,
		TEXT("BEATBOSS RESULT: %s in %d bars, %.1f s, %d perfect, %d good, %d miss, best streak %d, %d retries, boss %d/%d hp, lead %d/%d hp, ")
			TEXT("%d whiffs, %d strays, %d finishers, %d releases, %d staggers, %d silences, phase %d/%d%s%s."),
		Outcome, S.bars, Now - BeginTime, S.perfects, S.goods, S.misses, S.maxStreak, S.retries, Fight.bossHp(), Fight.bossMaxHp(), Fight.hp(),
		Fight.maxHp(), S.whiffs, S.strays, S.finishers, S.releases, S.staggers, S.silences, Fight.phase() + 1,
		static_cast<int32>(Track.def.phaseCount), bAutoPlay ? TEXT(", autoplay") : TEXT(""), bIdle ? TEXT(", idle") : TEXT(""));
}

void AFTBeatBossGameMode::Quit(int32 Code)
{
	if (bQuitting)
	{
		return;
	}
	bQuitting = true;
	Stage = EStage::Done;
	if (Music)
	{
		// Whether the stream ran at all (a run with no audio device never does), and how many sounds it started late.
		UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: music %s, %u late, %u dropped, %u revoked."), Music->IsRunning() ? TEXT("ran") : TEXT("did not run"),
			Music->GetLateCount(), Music->GetDroppedCount(), MusicDriver.revoked());
	}
	UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: quitting (%d)."), Code);
	FTRequestExit(Code, TEXT("BeatBoss"));
}

void AFTBeatBossGameMode::Pulse(EPulse Which, double Time, int32 Amount)
{
	Pulses[static_cast<int32>(Which)] = Time;
	PulseAmounts[static_cast<int32>(Which)] = Amount;
}

void AFTBeatBossGameMode::AddPopup(const ftcore::BeatBossEvent& E)
{
	FPopup& P = Popups[PopupTotal % MaxPopups];
	P.Type = E.type;
	P.Grade = E.grade;
	P.Answer = E.answer;
	P.Amount = E.amount;
	P.Time = E.time;
	// A judgement pops where its prompt was; a press that answered nothing, where the next prompt is.
	P.bAnchored = false;
	if (E.prompt != 0)
	{
		for (const FAnchorMemo& M : AnchorMemos)
		{
			if (M.Id == E.prompt)
			{
				P.Anchor = M.World;
				P.bAnchored = true;
			}
		}
	}
	if (!P.bAnchored && PromptViewCount > 0)
	{
		P.Anchor = PromptViews[0].World;
		P.bAnchored = true;
	}
	++PopupTotal;
}

void AFTBeatBossGameMode::Observe(const ftcore::BeatBossEvents& Events, double Clock)
{
	if (Choreo.begun())
	{
		Choreo.observe(Fight, Events, Clock);
	}
}

void AFTBeatBossGameMode::LoadAccess()
{
	ftcore::BeatAccessOptions Loaded;
	const FString File = FPaths::ProjectSavedDir() / TEXT("BeatAccess.json");
	TArray<uint8> Bytes;
	if (IFileManager::Get().FileExists(*File) && FFileHelper::LoadFileToArray(Bytes, *File))
	{
		std::string Error;
		if (!ftcore::decodeBeatAccess(FT::BytesAsText(Bytes), Loaded, &Error))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: Saved/BeatAccess.json refused: %s"), *FT::ToFString(Error));
		}
		AccessStamp = IFileManager::Get().GetTimeStamp(*File);
	}
	else
	{
		AccessStamp = FDateTime(0);
	}
	// The flags win over the file.
	const TCHAR* Cmd = FCommandLine::Get();
	float F = 0.f;
	int32 I = 0;
	if (FParse::Value(Cmd, TEXT("BeatWindowScale="), F)) Loaded.windowScale = F;
	if (FParse::Value(Cmd, TEXT("BeatWhiffMs="), F)) Loaded.whiffLockoutMs = F;
	if (FParse::Value(Cmd, TEXT("BeatPairMode="), I)) Loaded.pairMode = I;
	if (FParse::Value(Cmd, TEXT("BeatUiScale="), F)) Loaded.uiScale = F;
	if (FParse::Value(Cmd, TEXT("BeatCuts="), I)) Loaded.cutsReduced = I;
	if (FParse::Value(Cmd, TEXT("BeatShake="), F)) Loaded.shakeScale = F;
	if (FParse::Value(Cmd, TEXT("BeatFlash="), I)) Loaded.flashLevel = I;
	if (FParse::Value(Cmd, TEXT("BeatTempo="), F)) Loaded.tempoScale = F;
	ftcore::clampBeatAccess(Loaded);
	Access = Loaded;
	FlashLevel = Access.flashLevel;
	if (const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
	{
		if (const UFTSettingsSubsystem* SettingsSub = GI->GetSubsystem<UFTSettingsSubsystem>())
		{
			bReduceFlashesSeen = SettingsSub->Get().reduceFlashes;
			if (bReduceFlashesSeen)
			{
				FlashLevel = FMath::Max(FlashLevel, 1);
			}
		}
	}
}

// The assists (the settings', the flags') and the tuning the options make. Before the fight begins it only sets them;
// in a fight, Core takes them at the next bar line (and from a retry).
void AFTBeatBossGameMode::ApplyAccess(bool bRetry)
{
	ftcore::QteAssist Wanted;
	if (const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
	{
		if (const UFTSettingsSubsystem* SettingsSub = GI->GetSubsystem<UFTSettingsSubsystem>())
		{
			Wanted.autoComplete = SettingsSub->Get().qteAutoComplete;
			Wanted.holdToggle = SettingsSub->Get().holdToggle;
		}
	}
	if (bAutoPlay)
	{
		Wanted.autoComplete = true;
	}
	if (bIdle || MissPhase > 0)
	{
		// Idle plays nothing; the miss phase's pilot is the host's own.
		Wanted.autoComplete = false;
	}
	// A track whose windows would overlap at the scale asked for is held to the widest it can take.
	ftcore::BeatAccessOptions Used = Access;
	Used.windowScale = ftcore::maxBeatWindowScale(Track.def, ftcore::BeatBossTuning{}, Access.windowScale);
	if (Used.windowScale < Access.windowScale)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: window scale %.2f would overlap two prompts of one button on this track; held to %.2f."),
			static_cast<double>(Access.windowScale), static_cast<double>(Used.windowScale));
	}
	Tuning = ftcore::beatAccessTuning(ftcore::BeatBossTuning{}, Used);
	Assist = Wanted;
	AppliedAssist = Wanted;
	if (Fight.state() != ftcore::BeatBossState::NotStarted)
	{
		Fight.requestAccess(Tuning, Wanted);
	}
	(void)bRetry;
}

// Once in a while: a changed assist in the settings, or a changed options file, is handed to the fight (it lands at the
// next bar line).
void AFTBeatBossGameMode::PollAccess(double Wall)
{
	if (Wall - AccessCheckWall < 0.5)
	{
		return;
	}
	AccessCheckWall = Wall;
	bool bChanged = false;
	if (const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
	{
		if (const UFTSettingsSubsystem* SettingsSub = GI->GetSubsystem<UFTSettingsSubsystem>())
		{
			const bool bAuto = SettingsSub->Get().qteAutoComplete && !bIdle && MissPhase == 0 && !bAutoPlay;
			const bool bToggle = SettingsSub->Get().holdToggle;
			bChanged |= SettingsSub->Get().reduceFlashes != bReduceFlashesSeen;
			if (!bAutoPlay && !bIdle && MissPhase == 0)
			{
				bChanged |= bAuto != AppliedAssist.autoComplete;
			}
			bChanged |= bToggle != AppliedAssist.holdToggle;
		}
	}
	const FString File = FPaths::ProjectSavedDir() / TEXT("BeatAccess.json");
	const FDateTime Stamp = IFileManager::Get().FileExists(*File) ? IFileManager::Get().GetTimeStamp(*File) : FDateTime(0);
	bChanged |= Stamp != AccessStamp;
	if (bChanged)
	{
		LoadAccess();
		ApplyAccess(false);
		UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: access options changed; they land at the next bar line."));
	}
}

void AFTBeatBossGameMode::LandHit(const ftcore::BeatBossEvent& E, double Time)
{
	Pulse(EPulse::BossHit, Time, E.amount);
	// A heavy blow shakes the camera more: a counter's, a hold's or a multiplied strike.
	Shake(0.28f + FMath::Min(0.4f, static_cast<float>(E.amount) * 0.008f));
	SpawnHitPuff(AnchorWorld(ftcore::ChoreoAnchor::BossChest), E.amount >= 20 ? 1.2f : 0.8f);
}

// A blow the lead's follow-up has not landed yet waits for its contact: the popup, the flash and the shake come with
// the blow on screen, not with the verdict.
void AFTBeatBossGameMode::PendHit(const ftcore::BeatBossEvent& E)
{
	double Wait = 0.0;
	if (Choreo.begun() && Choreo.hitContacts() != LastHitContacts)
	{
		LastHitContacts = Choreo.hitContacts();
		if (Fight.trackRunning())
		{
			Wait = FMath::Clamp(Choreo.hitContact() - Fight.trackTime(), 0.0, 1.5);
		}
	}
	if (Wait < 0.02 || PendingHitCount >= MaxPendingHits)
	{
		LandHit(E, E.time);
		return;
	}
	PendingHits[PendingHitCount++] = FPendingHit{Now + Wait, E};
}

void AFTBeatBossGameMode::FlushHits()
{
	for (int32 I = 0; I < PendingHitCount;)
	{
		if (PendingHits[I].Due <= Now + 1e-9)
		{
			LandHit(PendingHits[I].Event, PendingHits[I].Due);
			PendingHits[I] = PendingHits[--PendingHitCount];
		}
		else
		{
			++I;
		}
	}
}

// Dust and a puff of cloth where a blow lands (sparks are for metal, and nothing here is). The system is built by
// FTBuildVfx from Data/Vfx/dust_puff.json; without it nothing shows. Live ones follow the visuals' clock, so a hit-stop
// holds them and the slow motion slows them.
void AFTBeatBossGameMode::SpawnHitPuff(const FVector& At, float Scale)
{
	if (FlashLevel >= 2 || !GetWorld() || Stage == EStage::Waiting)
	{
		return;
	}
	if (!PuffSystem.IsValid() && !bPuffTried)
	{
		bPuffTried = true;
		if (UNiagaraSystem* Loaded = LoadObject<UNiagaraSystem>(nullptr, TEXT("/Game/VFX/NS_dust_puff.NS_dust_puff"), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Loaded->AddToRoot();
			PuffSystem = Loaded;
		}
	}
	if (!PuffSystem.IsValid())
	{
		return;
	}
	if (UNiagaraComponent* Puff = UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(), PuffSystem.Get(), At, FRotator::ZeroRotator, FVector(Scale)))
	{
		if (Puffs.Num() >= 8)
		{
			Puffs.RemoveAt(0, EAllowShrinking::No);
		}
		Puffs.Add(Puff);
	}
}

void AFTBeatBossGameMode::UpdateLights(float Key, float RimBoss, float RimLead, float Kicker)
{
	for (int32 I = 0; I < StageLights.Num(); ++I)
	{
		UPointLightComponent* Lamp = StageLights[I].Get();
		if (!Lamp)
		{
			continue;
		}
		const FName Tag = StageLightTags[I];
		const float Scale = Tag == KeyTag() ? Key : Tag == RimBossTag() ? RimBoss : Tag == RimLeadTag() ? RimLead : Kicker;
		const float Intensity = StageLightBase[I] * Scale;
		if (!FMath::IsNearlyEqual(Lamp->Intensity, Intensity, 1.f))
		{
			Lamp->SetIntensity(Intensity);
		}
	}
	// The fist about to strike glows: a light on its hand that rises across the tell.
	UPointLightComponent* Glow = HandLight.Get();
	USkeletalMeshComponent* BossSkin = BossMesh.Get();
	if (Glow && BossSkin)
	{
		float Level = 0.f;
		FName Bone = HandLightBone;
		for (int32 I = 0; I < PromptViewCount; ++I)
		{
			const FPromptView& V = PromptViews[I];
			if (V.Anchor == ftcore::ChoreoAnchor::BossFistR || V.Anchor == ftcore::ChoreoAnchor::BossFistL)
			{
				Level = FMath::Max(Level, V.Progress);
				Bone = V.Anchor == ftcore::ChoreoAnchor::BossFistR ? FName(TEXT("hand_r")) : FName(TEXT("hand_l"));
			}
		}
		if (Bone != HandLightBone && BossSkin->GetBoneIndex(Bone) != INDEX_NONE)
		{
			HandLightBone = Bone;
			Glow->AttachToComponent(BossSkin, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Bone);
		}
		const float Intensity = FlashLevel >= 2 ? 0.f : 2400.f * Level * Level;
		if (!FMath::IsNearlyEqual(Glow->Intensity, Intensity, 1.f))
		{
			Glow->SetIntensity(Intensity);
		}
	}
}

// The boss's body for this frame: its build (set once), its lean (a swing into its blow, a recoil when it is staggered),
// its knockback and, in its defeat, the root's fall.
void AFTBeatBossGameMode::UpdateBossBody(float DeltaSeconds)
{
	UFTBeatFighterAnimInstance* BossAnim = BossMesh.IsValid() ? Cast<UFTBeatFighterAnimInstance>(BossMesh->GetAnimInstance()) : nullptr;
	UFTBeatFighterAnimInstance* LeadAnim = LeadMesh.IsValid() ? Cast<UFTBeatFighterAnimInstance>(LeadMesh->GetAnimInstance()) : nullptr;
	const int32 Clip = Sample.boss.clip;
	const float Time = static_cast<float>(Sample.boss.time);
	float Lean = 0.f;
	float Drop = 0.f;
	int32 Arm = 0;
	double Back = 0.0;
	if (Choreo.begun() && Clip >= 0 && Clip < static_cast<int32>(Clips.count))
	{
		const ftcore::BeatClip& C = Clips[static_cast<size_t>(Clip)];
		if (Clip == ChoreoDef.stagger.boss)
		{
			// The stagger is a big moment: the spine snaps back 18 degrees and recovers, and the body goes 35 cm back
			// over 250 ms and returns over a second.
			Lean = -18.f * FMath::Min(1.f, Time / 0.15f) * FMath::Exp(-FMath::Max(0.f, Time - 0.15f) * 2.5f);
			Back = 35.0 * (Time < 0.25f ? BossSmooth(Time / 0.25f) : 1.f - FMath::Clamp((Time - 0.25f) / 1.0f, 0.f, 1.f));
		}
		else if (Clip < BossAttackClip.Num() && BossAttackClip[Clip] && C.impact > 0.0)
		{
			// The swing leans into the blow, so a fist at the boss's height comes down onto the lead instead of over him.
			const float Impact = static_cast<float>(C.impact);
			Lean = 16.f * (Time <= Impact ? BossSmooth(Time / Impact) : 1.f - BossSmooth((Time - Impact) / 0.5f));
			// And the striking arm comes down 32 degrees through the swing, so the fist ends at the lead's head, not above it.
			Drop = 32.f * BossSmooth((Time - 0.4f * Impact) / (0.6f * Impact)) * (1.f - BossSmooth((Time - Impact - 0.1f) / 0.4f));
			Arm = Clip < BossClipArm.Num() ? BossClipArm[Clip] : 0;
		}
	}
	if (BossAnim)
	{
		BossAnim->SetBuild(1.15f, 0.85f, 1.25f);
		BossAnim->SetLean(Lean);
		BossAnim->SetArmDrop((Arm & 1) != 0 ? Drop : 0.f, (Arm & 2) != 0 ? Drop : 0.f);
		// A defeat falls: the root's travel is let go for that clip, held for every other.
		const bool bFalls = Choreo.begun() && ChoreoDef.victory.boss != ftcore::kNoClip && Clip == ChoreoDef.victory.boss;
		BossAnim->SetHoldRoot(!bRootMotion && !bFalls);
	}
	if (LeadAnim)
	{
		const bool bFalls = Choreo.begun() && ChoreoDef.defeat.lead != ftcore::kNoClip && Sample.lead.clip == ChoreoDef.defeat.lead;
		LeadAnim->SetHoldRoot(!bRootMotion && !bFalls);
		// His stance has the hands up at the face: while he stands in it they come down to a guard at the chest, and go
		// back as soon as he plays anything else (which has its own hands).
		const bool bStance = Sample.lead.clip < 0 || Sample.lead.clip == StanceLeadClip;
		const float Target = bStance ? 1.f : 0.f;
		LeadGuardDown += (Target - LeadGuardDown) * FMath::Min(1.f, DeltaSeconds * 10.f);
		LeadAnim->SetGuardDown(LeadGuardDown);
	}
	if (AActor* BossActor = Boss.Get())
	{
		BossActor->SetActorLocation(BossHome + FVector(Back, 0.0, 0.0));
	}
}

void AFTBeatBossGameMode::Shake(float Amount)
{
	Trauma = FMath::Max(Trauma, FMath::Clamp(Amount, 0.f, 1.f));
}

void AFTBeatBossGameMode::Handle(const ftcore::BeatBossEvents& Events, double Clock)
{
	using ftcore::BeatBossEventType;
	// The choreography sees every call's events, each with the clock it was made at.
	Observe(Events, Clock);
	for (const ftcore::BeatBossEvent& E : Events)
	{
		if (Music)
		{
			MusicDriver.onEvent(E);
		}
		switch (E.type)
		{
		case BeatBossEventType::Judged:
			AddPopup(E);
			Pulse(EPulse::Judged, E.time, static_cast<int32>(E.grade));
			if ((E.grade == ftcore::QteGrade::Perfect || E.grade == ftcore::QteGrade::Good) && Fight.inFinisher())
			{
				Pulse(EPulse::FinisherStrike, E.time);
			}
			if (E.grade == ftcore::QteGrade::Miss)
			{
				Voice(static_cast<uint8>(UFTBeatBossMetronome::EVoice::Miss));
			}
			else
			{
				LeadMove = E.answer;
				LeadMoveTime = E.time;
				if (E.grade == ftcore::QteGrade::Perfect)
				{
					Voice(static_cast<uint8>(UFTBeatBossMetronome::EVoice::Good));
				}
			}
			if (E.grade == ftcore::QteGrade::Perfect && E.answer == ftcore::BeatBossAnswer::Guard)
			{
				Shake(0.4f);
			}
			break;
		case BeatBossEventType::BossHit:
			// The popup, the flash and the shake come with the blow on screen: a counter's follow-up lands a beat later.
			PendHit(E);
			break;
		case BeatBossEventType::LeadHit:
			Pulse(EPulse::LeadHit, E.time, E.amount);
			Shake(0.7f);
			break;
		case BeatBossEventType::Whiff:
			// Counters are locked out for E.amount milliseconds from the press.
			LockoutUntil = E.time + static_cast<double>(E.amount) * 0.001;
			Pulse(EPulse::Whiff, E.time, E.amount);
			AddPopup(E);
			break;
		case BeatBossEventType::LockedOut:
			AddPopup(E);
			break;
		case BeatBossEventType::Multiplier:
			Pulse(EPulse::Multiplier, E.time, E.amount);
			break;
		case BeatBossEventType::Reward:
			Pulse(EPulse::Reward, E.time, E.amount);
			AddPopup(E);
			break;
		case BeatBossEventType::LoadStored:
			Pulse(EPulse::LoadStored, E.time, E.amount);
			AddPopup(E);
			break;
		case BeatBossEventType::Released:
			Pulse(EPulse::Released, E.time, E.amount);
			break;
		case BeatBossEventType::FinisherStart:
			Pulse(EPulse::Finisher, E.time);
			break;
		case BeatBossEventType::FinisherBroken:
			Pulse(EPulse::FinisherBroken, E.time);
			break;
		case BeatBossEventType::PhaseStart:
			Pulse(EPulse::Phase, E.time, E.amount);
			UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: phase %d starts on its bar %d (boss %d hp, %d bars played)."), E.amount + 1,
				Fight.bar(), Fight.bossHp(), Fight.stats().bars);
			break;
		case BeatBossEventType::Count:
			Pulse(EPulse::Count, E.time, E.amount);
			if (CountKind == ftcore::BeatCountKind::Click)
			{
				Voice(static_cast<uint8>(UFTBeatBossMetronome::EVoice::Count));
			}
			else if (CountKind == ftcore::BeatCountKind::Voice)
			{
				Voice(static_cast<uint8>(UFTBeatBossMetronome::EVoice::Voice));
			}
			break;
		case BeatBossEventType::SilenceStart:
		{
			Pulse(EPulse::Silence, E.time, E.amount);
			SilenceSeconds = static_cast<double>(E.amount) * 0.001;
			// The count back is the silence's own: the phase's silence of this length.
			const int32 Phase = FMath::Clamp(Fight.phase(), 0, static_cast<int32>(Track.def.phaseCount) - 1);
			const ftcore::BarMap& Map = Track.def.phases[static_cast<size_t>(Phase)];
			for (int32 I = 0; I < Map.silenceCount; ++I)
			{
				if (FMath::Abs(Map.silences[static_cast<size_t>(I)].seconds * 1000.0 - static_cast<double>(E.amount)) <= 1.0)
				{
					CountKind = Map.silences[static_cast<size_t>(I)].count;
					break;
				}
			}
			break;
		}
		case BeatBossEventType::Resumed:
			Pulse(EPulse::Resumed, E.time, E.amount);
			break;
		case BeatBossEventType::RestOpened:
			Pulse(EPulse::RestOpened, E.time, E.amount);
			break;
		case BeatBossEventType::Stagger:
			Pulse(EPulse::Stagger, E.time);
			Shake(0.85f);
			break;
		case BeatBossEventType::Victory:
			Shake(0.9f);
			break;
		case BeatBossEventType::Defeat:
			Shake(0.6f);
			break;
		default:
			break;
		}
	}
}

void AFTBeatBossGameMode::Voice(uint8 Which)
{
	if (Metronome)
	{
		Metronome->Trigger(static_cast<UFTBeatBossMetronome::EVoice>(Which));
	}
	// The procedural music sounds everything itself: the count, the beats, the moves' ticks, pickups and accents and the
	// verdicts all come from the driver, placed on the impact, so nothing is triggered from here.
}

void AFTBeatBossGameMode::MusicReset()
{
	if (!Music)
	{
		return;
	}
	MusicDriver.reset(Now, MusicOut);
	Music->Post(MusicOut, Now);
}

void AFTBeatBossGameMode::MusicFrame()
{
	// A pause or a held view leaves the clock behind the wall clock: the mapping to the audio frames starts over.
	if (Music->Sync(Now))
	{
		MusicDriver.reset(Now, MusicOut);
		Music->Post(MusicOut, Now);
	}
	MusicDriver.update(Fight, Line, Now, CountKind, MusicOut);
	Music->Post(MusicOut, Now);
}

void AFTBeatBossGameMode::Sound()
{
	const double TrackNow = Fight.trackTime();

	// The beat: a click on each beat of the bar as the playhead crosses it, accented on the bar line, none on
	// an empty beat. The count bars are the count's own (its events), and a silence is silent.
	if (Fight.stats().bars != SoundBars)
	{
		SoundBars = Fight.stats().bars;
		SoundPlayhead = -1e-6;
	}
	if (Fight.trackRunning() && !Line.frozen && !Line.counting)
	{
		for (size_t I = 0; I < Line.tickCount; ++I)
		{
			const ftcore::BeatLineTick& T = Line.ticks[I];
			if (T.bar != 0 || T.at <= SoundPlayhead || T.at > Line.playhead)
			{
				continue;
			}
			if (T.kind == ftcore::BeatTickKind::BarLine)
			{
				Voice(static_cast<uint8>(UFTBeatBossMetronome::EVoice::Accent));
			}
			else if (T.kind == ftcore::BeatTickKind::Beat)
			{
				Voice(static_cast<uint8>(UFTBeatBossMetronome::EVoice::Click));
			}
		}
		SoundPlayhead = Line.playhead;
	}

	// Each move's stem on its impact, answered or not: the host lists the prompts as they come.
	const uint32 Listed = SoundCueId;
	for (size_t I = 0; I < Fight.promptCount(); ++I)
	{
		const ftcore::BeatBossPrompt& P = Fight.prompt(I);
		if (P.id <= Listed || SoundCueCount >= MaxSoundCues)
		{
			continue;
		}
		FSoundCue& C = SoundCues[SoundCueCount++];
		C.Id = P.id;
		C.Impact = P.impact;
		C.Stem = P.stem;
		SoundCueId = FMath::Max(SoundCueId, P.id);
	}
	for (int32 I = SoundCueCount - 1; I >= 0; --I)
	{
		const FSoundCue& C = SoundCues[I];
		if (C.Impact > TrackNow)
		{
			continue;
		}
		// One crossed this frame sounds; one long past (a time jump) is dropped quietly.
		if (C.Impact > SoundTrack - 0.25)
		{
			Voice(static_cast<uint8>(static_cast<int32>(UFTBeatBossMetronome::EVoice::Bass) + C.Stem % 3));
		}
		SoundCues[I] = SoundCues[SoundCueCount - 1];
		--SoundCueCount;
	}
	SoundTrack = TrackNow;
}

ftcore::ActionButton AFTBeatBossGameMode::ButtonFor(ftcore::BeatBossAnswer Answer, ftcore::ActionButton Own)
{
	switch (Answer)
	{
	case ftcore::BeatBossAnswer::Guard:
	case ftcore::BeatBossAnswer::Parry:
		return ftcore::ActionButton::Guard;
	case ftcore::BeatBossAnswer::Dodge:
		return ftcore::ActionButton::Dodge;
	case ftcore::BeatBossAnswer::Jump:
		return ftcore::ActionButton::Jump;
	case ftcore::BeatBossAnswer::Attack:
		return ftcore::ActionButton::Attack;
	default:
		return Own; // a hold, a release or a pair names its own button
	}
}

void AFTBeatBossGameMode::CalibrateFacing(USkeletalMeshComponent* Mesh, double TargetYaw)
{
	if (!Mesh || Mesh->GetBoneIndex(FName(TEXT("thigh_l"))) == INDEX_NONE || Mesh->GetBoneIndex(FName(TEXT("thigh_r"))) == INDEX_NONE)
	{
		return;
	}
	// Which way the body faces, read off the animated pose: forward is the left-to-right line of the hips turned a
	// quarter (right x up). The meshes, the clips and their root bones don't agree on a facing, so the mesh is
	// turned on its actor until the stance faces where it should.
	const FVector Across = Mesh->GetBoneLocation(FName(TEXT("thigh_r")), EBoneSpaces::WorldSpace) - Mesh->GetBoneLocation(FName(TEXT("thigh_l")), EBoneSpaces::WorldSpace);
	const FVector Forward = FVector::CrossProduct(FVector(Across.X, Across.Y, 0.0), FVector::UpVector);
	if (Forward.SizeSquared2D() < 1.0)
	{
		return;
	}
	const double Yaw = FMath::RadiansToDegrees(FMath::Atan2(Forward.Y, Forward.X));
	const double Delta = FMath::UnwindDegrees(TargetYaw - Yaw);
	Mesh->AddRelativeRotation(FRotator(0.0, Delta, 0.0));
	UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: %s faced %.0f degrees, turned by %.0f to face %.0f."), *Mesh->GetOwner()->GetName(), Yaw, Delta, TargetYaw);
}

FVector AFTBeatBossGameMode::BoneWorld(USkeletalMeshComponent* Mesh, const TCHAR* Bone, const FVector& Fallback) const
{
	if (!Mesh)
	{
		return Fallback;
	}
	const FName Name(Bone);
	return Mesh->GetBoneIndex(Name) != INDEX_NONE ? Mesh->GetBoneLocation(Name, EBoneSpaces::WorldSpace) : Fallback;
}

FVector AFTBeatBossGameMode::AnchorWorld(ftcore::ChoreoAnchor Anchor) const
{
	USkeletalMeshComponent* LeadSkin = LeadMesh.Get();
	USkeletalMeshComponent* BossSkin = BossMesh.Get();
	const FVector Middle = (LeadHome + BossHome) * 0.5 + FVector(0.0, 0.0, 130.0);
	switch (Anchor)
	{
	case ftcore::ChoreoAnchor::BossHead:
		return BoneWorld(BossSkin, TEXT("head"), BossHome + FVector(0.0, 0.0, 225.0)) + FVector(0.0, 0.0, 14.0);
	case ftcore::ChoreoAnchor::BossFistR:
		return BoneWorld(BossSkin, TEXT("hand_r"), BossHome + FVector(-40.0, 40.0, 140.0));
	case ftcore::ChoreoAnchor::BossFistL:
		return BoneWorld(BossSkin, TEXT("hand_l"), BossHome + FVector(-40.0, -40.0, 140.0));
	case ftcore::ChoreoAnchor::BossChest:
		return BoneWorld(BossSkin, TEXT("spine_03"), BossHome + FVector(0.0, 0.0, 165.0));
	case ftcore::ChoreoAnchor::LeadHead:
		return BoneWorld(LeadSkin, TEXT("head"), LeadHome + FVector(0.0, 0.0, 165.0));
	case ftcore::ChoreoAnchor::LeadChest:
		return BoneWorld(LeadSkin, TEXT("spine_03"), LeadHome + FVector(0.0, 0.0, 122.0));
	case ftcore::ChoreoAnchor::ScreenCenter:
	default:
		return Middle;
	}
}

void AFTBeatBossGameMode::UpdatePrompts()
{
	PromptViewCount = 0;
	const int32 Count = FMath::Min<int32>(static_cast<int32>(Sample.promptCount), MaxPromptViews);
	for (int32 I = 0; I < Count; ++I)
	{
		const ftcore::ChoreoPromptView& P = Sample.prompts[static_cast<size_t>(I)];
		FPromptView& V = PromptViews[PromptViewCount++];
		V.Id = P.id;
		V.World = AnchorWorld(P.anchor);
		V.Progress = P.progress;
		V.Answer = P.answer;
		V.bFinisher = P.finisher;
		V.bOnScreen = P.anchor == ftcore::ChoreoAnchor::ScreenCenter;
		V.Anchor = P.anchor;
		V.Button = ButtonFor(P.answer, ftcore::ActionButton::Attack);
		V.Second = ftcore::ActionButton::Attack;
		V.Span = 1.f;
		V.Held = -1.f;
		V.Left = 0;
		for (size_t K = 0; K < Fight.promptCount(); ++K)
		{
			const ftcore::BeatBossPrompt& Open = Fight.prompt(K);
			if (Open.id == P.id)
			{
				V.Button = ButtonFor(P.answer, Open.button);
				V.Second = Open.second;
				V.Span = static_cast<float>(FMath::Max(0.05, Open.impact - Open.tell));
				const double TrackNow = Fight.trackTime();
				if (Open.pressA >= 0.0)
				{
					if (Open.answer == ftcore::BeatBossAnswer::Guard)
					{
						V.Held = static_cast<float>(FMath::Clamp((TrackNow - Open.pressA) / FMath::Max(0.01, Open.impact - Open.pressA), 0.0, 1.0));
					}
					else if (ftcore::isBossHold(Open.answer))
					{
						V.Held = static_cast<float>(FMath::Clamp((TrackNow - Open.impact) / FMath::Max(0.01, Open.end - Open.impact), 0.0, 1.0));
					}
				}
				else if (ftcore::isBossHold(Open.answer))
				{
					V.Held = 0.f; // the tail to its end, drained as it is held
				}
			}
			if (P.finisher && Open.finisher && Open.id >= P.id)
			{
				++V.Left;
			}
		}
		// Where it stood, for the judgement that pops there once it is answered (the choreography forgets it then).
		FAnchorMemo* Memo = nullptr;
		for (FAnchorMemo& M : AnchorMemos)
		{
			if (M.Id == P.id)
			{
				Memo = &M;
			}
		}
		if (!Memo)
		{
			Memo = &AnchorMemos[AnchorMemoNext];
			AnchorMemoNext = (AnchorMemoNext + 1) % MaxAnchorMemos;
			Memo->Id = P.id;
		}
		Memo->World = V.World;
	}
}

void AFTBeatBossGameMode::UpdateCamera(float DeltaSeconds)
{
	ACameraActor* Cam = Camera.Get();
	UCameraComponent* Lens = Cam ? Cam->GetCameraComponent() : nullptr;
	if (!Cam || !Lens)
	{
		return;
	}

	// Which shot is on: the choreography's, unless the reduced-cuts option holds one longer (1: a shot stays 2.5 s at
	// least and no cut falls on an impact beat) or keeps one for the phase (2).
	RequestedShot = Sample.shot;
	ftcore::ChoreoShot Shot = CameraShot;
	if (RequestedShot != CameraShot)
	{
		bool bAllow = true;
		if (Access.cutsReduced >= 2)
		{
			// One shot for the whole phase: only a phase's first bar line (the choreography's cut to its opening shot) changes it.
			bAllow = Now - GetPulse(EPulse::Phase) < 0.05;
		}
		else if (Access.cutsReduced == 1)
		{
			bAllow = Now - LastCutNow >= 2.5;
			for (int32 I = 0; I < PromptViewCount && bAllow; ++I)
			{
				bAllow = PromptViews[I].Progress < 0.85f; // an impact is coming: the cut waits
			}
			bAllow = bAllow && Now - GetPulse(EPulse::Judged) > 0.25;
		}
		if (bAllow || Stage == EStage::Waiting)
		{
			Shot = RequestedShot;
		}
	}
	if (Shot != CameraShot)
	{
		CameraShot = Shot;
		bCameraCut = true;
		LastCutNow = Now;
		CutKick = 0.0;
		if (Shot == ftcore::ChoreoShot::FinisherOrbit)
		{
			OrbitAngle = 0.0;
		}
		// The renderer is told the picture jumped: its temporal history (anti-aliasing, global illumination, motion
		// blur) would otherwise smear the old shot over the new one.
		if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
		{
			if (PC->PlayerCameraManager)
			{
				PC->PlayerCameraManager->SetGameCameraCutThisFrame();
			}
		}
	}
	CutKick += static_cast<double>(DeltaSeconds);
	const float VisualRate = Sample.visualRate;
	// The orbit turns with the animation: the finisher's slow motion slows it, a hit-stop holds it.
	if (Shot == ftcore::ChoreoShot::FinisherOrbit)
	{
		OrbitAngle += 24.0 * static_cast<double>(DeltaSeconds) * static_cast<double>(VisualRate);
	}
	ShakePhase += static_cast<double>(DeltaSeconds) * static_cast<double>(VisualRate);

	// Every shot stands on the +y side of the line from the lead to the boss, so a cut never turns the screen round.
	const FVector L = LeadHome;
	const FVector B = BossHome;
	const FVector Mid = (L + B) * 0.5;
	const double Hb = 230.0; // the boss's height, centimetres
	FBossShot S;
	switch (Shot)
	{
	case ftcore::ChoreoShot::Wide:
		// Square on to the fight from the side, level: the lead on the left, the boss on the right.
		S.Pos = Mid + FVector(0.0, 2.7 * Hb, 105.0);
		S.Target = Mid + FVector(0.0, 0.0, 110.0);
		S.Mm = 28.f;
		S.Fstop = 8.f;
		break;
	case ftcore::ChoreoShot::OverLead:
		// Over the lead's shoulder at the boss, who looms: the boss's rim at full, the lead's half.
		S.Pos = L + FVector(-1.4 * Hb, 70.0, 175.0);
		S.Target = B + FVector(0.0, 0.0, 175.0);
		S.Mm = 32.f;
		S.Fstop = 2.8f;
		S.Key = 0.9f;
		S.RimLead = 0.5f;
		break;
	case ftcore::ChoreoShot::OverBoss:
		// Over the boss's shoulder at the lead, from the same side as the others, the lead's rim up so he reads against
		// the boss's dark back.
		S.Pos = B + FVector(1.6 * Hb, 200.0, 300.0);
		S.Target = L + FVector(20.0, 0.0, 145.0);
		S.Mm = 35.f;
		S.Fstop = 2.8f;
		S.Key = 0.8f;
		S.RimBoss = 0.4f;
		S.RimLead = 1.2f;
		break;
	case ftcore::ChoreoShot::CloseLead:
		// A three-quarter view of his face from the +y side, in front of him and short of the boss (the lens at 35 degrees
		// off the axis would stand in the boss).
		S.Pos = L + FVector(65.0, 150.0, 160.0);
		S.Target = L + FVector(0.0, 0.0, 155.0);
		S.Mm = 50.f;
		S.Fstop = 2.f;
		S.Key = 1.1f;
		S.RimBoss = 0.6f;
		S.RimLead = 0.6f;
		break;
	case ftcore::ChoreoShot::CloseBoss:
		// From below: the boss looms; its rim up.
		S.Pos = B + FVector(-185.0, 125.0, 150.0);
		S.Target = B + FVector(0.0, 0.0, 216.0);
		S.Mm = 50.f;
		S.Fstop = 2.8f;
		S.RimBoss = 1.4f;
		S.RimLead = 0.6f;
		break;
	case ftcore::ChoreoShot::LowHero:
		// On the floor by the lead's feet, looking up past him at the boss.
		S.Pos = L + FVector(-110.0, 85.0, 35.0);
		S.Target = Mid + FVector(30.0, 0.0, 120.0);
		S.Mm = 21.f;
		S.Fstop = 8.f;
		S.Key = 0.9f;
		S.RimBoss = 1.2f;
		break;
	case ftcore::ChoreoShot::TopDown:
		// Straight down, a little off the vertical; the rims are horizontal and give way to the key's pool.
		S.Pos = Mid + FVector(-107.0, 0.0, 3.3 * Hb);
		S.Target = Mid + FVector(0.0, 0.0, 0.0);
		S.Mm = 35.f;
		S.Fstop = 8.f;
		S.RimBoss = 0.3f;
		S.RimLead = 0.3f;
		break;
	case ftcore::ChoreoShot::FinisherOrbit:
	default:
	{
		// Starts wide and low, and between the three strikes dollies in and lengthens the lens, so the last blow is the
		// tightest frame. t runs on the visuals' clock: the slow motion's dips and the hit-stops hold it.
		const float T = BossSmooth(static_cast<float>(FMath::Min(Sample.sinceCut / 4.4, 1.0)));
		const double A = FMath::DegreesToRadians(100.0 + OrbitAngle);
		const double R = FMath::Lerp(2.3 * Hb, 1.2 * Hb, static_cast<double>(T));
		S.Pos = Mid + FVector(FMath::Cos(A) * R, FMath::Sin(A) * R, FMath::Lerp(0.5 * Hb, 0.7 * Hb, static_cast<double>(T)));
		S.Target = Mid + FVector(0.0, 0.0, FMath::Lerp(100.0, 140.0, static_cast<double>(T)));
		S.Mm = FMath::Lerp(28.f, 50.f, T);
		S.Fstop = 4.f;
		S.Key = 0.8f;
		S.RimBoss = 1.4f;
		S.RimLead = 1.2f;
		break;
	}
	}
	float Fov = BossFov(S.Mm);
	FVector Pos = S.Pos;
	FVector Target = S.Target;
	// A slow push in over each shot, so a held one is never still.
	const double Push = 1.0 - 0.07 * FMath::Clamp(Sample.sinceCut / 3.0, 0.0, 1.0);
	Pos = Target + (Pos - Target) * Push;

	// The cut's kick: the new shot starts 3 % tighter and settles over 120 ms, so each cut has an attack on the beat.
	Fov *= 1.f - 0.03f * (1.f - BossSmooth(static_cast<float>(CutKick / 0.12)));
	// (The look spec's lens punch on a hit-stop, and more motion blur in the slow motion, are left out: the accessibility
	// spec asks for no added zoom or blur in a hit-stop or the slow motion, and it wins.)

	// The shake of a heavy blow: a short, fast tremor that dies away, on the visuals' clock (it holds in a hit-stop and
	// breaks on its release) and scaled by the player's option.
	FRotator Rotation = (Target - Pos).Rotation();
	const double T = ShakePhase;
	const float Scale = Access.shakeScale;
	const double Q = static_cast<double>(Trauma * Trauma) * static_cast<double>(Scale);
	if (Q > 0.0)
	{
		Pos += FVector(FMath::Sin(T * 47.0), FMath::Sin(T * 61.0 + 1.3), FMath::Sin(T * 53.0 + 2.1)) * (Q * 7.0);
		Rotation += FRotator(FMath::Sin(T * 57.0) * Q * 1.4, FMath::Sin(T * 43.0 + 0.7) * Q * 1.8, 0.0);
	}
	Trauma = FMath::Max(0.f, Trauma - kBossTraumaDecay * DeltaSeconds);
	// The jolt: when the hit-stop lets go of a blow, the frozen frame springs 1.5 cm along the fight's axis and settles over 90 ms.
	{
		const double Age = Now - JoltStart;
		if (Age >= 0.0 && Age < 0.09)
		{
			Pos.X += 1.5 * static_cast<double>(Scale) * FMath::Sin(Age / 0.09 * UE_PI);
		}
	}

	// The camera's own finish: manual exposure (so a lens never changes the brightness), the depth of field of the shot,
	// and the effects of the moment: a fringe on the hits, a vignette that punches on a hit on the lead, a grade that
	// deepens in the finisher's slow motion.
	FPostProcessSettings& Post = Lens->PostProcessSettings;
	Post.bOverride_AutoExposureMethod = true;
	Post.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	Post.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Post.AutoExposureApplyPhysicalCameraExposure = false;
	Post.bOverride_AutoExposureBias = true;
	Post.AutoExposureBias = ExposureEv;
	Post.bOverride_DepthOfFieldSensorWidth = true;
	Post.DepthOfFieldSensorWidth = 24.576f;
	Post.bOverride_DepthOfFieldFstop = true;
	Post.DepthOfFieldFstop = S.Fstop;
	Post.bOverride_DepthOfFieldFocalDistance = true;
	Post.DepthOfFieldFocalDistance = static_cast<float>(FVector::Dist(Pos, Target));
	const float Dip = FMath::Clamp((1.f - Sample.slowMo) / 0.6f, 0.f, 1.f);
	const bool bFx = FlashLevel < 2;
	float Fringe = 0.f;
	float Vignette = 0.4f;
	if (bFx)
	{
		Fringe = FMath::Max(Fringe, 0.8f * BossBump(Now - GetPulse(EPulse::LeadHit), 0.02f, 0.28f));
		Fringe = FMath::Max(Fringe, 0.5f * BossBump(Now - GetPulse(EPulse::Stagger), 0.02f, 0.48f));
		Fringe = FMath::Max(Fringe, 0.4f * BossBump(Now - GetPulse(EPulse::FinisherStrike), 0.02f, 0.13f));
		Fringe = FMath::Max(Fringe, 0.2f * BossBump(Now - GetPulse(EPulse::Whiff), 0.02f, 0.23f));
		Vignette += 0.35f * BossBump(Now - GetPulse(EPulse::LeadHit), 0.12f, 0.23f);
	}
	Vignette += 0.15f * Dip;
	Post.bOverride_SceneFringeIntensity = true;
	Post.SceneFringeIntensity = Fringe;
	Post.bOverride_VignetteIntensity = true;
	Post.VignetteIntensity = Vignette;
	Post.bOverride_ColorSaturation = true;
	Post.ColorSaturation = FVector4(1.0 - 0.15 * Dip, 1.0 - 0.15 * Dip, 1.0 - 0.15 * Dip, 1.0);
	Post.bOverride_ColorContrast = true;
	Post.ColorContrast = FVector4(1.0 + 0.1 * Dip, 1.0 + 0.1 * Dip, 1.0 + 0.1 * Dip, 1.0);
	Post.bOverride_MotionBlurAmount = true;
	Post.MotionBlurAmount = 0.5f;
	Post.bOverride_MotionBlurMax = true;
	Post.MotionBlurMax = 5.f;

	Cam->SetActorLocationAndRotation(Pos, Rotation);
	Lens->SetFieldOfView(Fov);
	Lens->PostProcessBlendWeight = 1.f;
	UpdateLights(S.Key, S.RimBoss, S.RimLead, S.Kicker);
	bCameraCut = false;
}

void AFTBeatBossGameMode::Scene(float DeltaSeconds)
{
	SceneWall = UFTPauseSubsystem::WallSeconds();
	FlushHits();
	// Where the beat is, for the prompts' pulse.
	if (Fight.trackRunning() && Track.def.phaseCount > 0)
	{
		const double Beat = Track.def.phases[static_cast<size_t>(FMath::Clamp(Fight.phase(), 0, static_cast<int32>(Track.def.phaseCount) - 1))].beatSeconds();
		BeatPhase = static_cast<float>(FMath::Fmod(Line.playhead, Beat) / Beat);
	}
	else
	{
		BeatPhase = 0.5f;
	}
	const bool bScene = Choreo.begun() && Stage != EStage::Waiting;
	if (bScene)
	{
		if (Fight.over())
		{
			// After the end the scene keeps time for the end poses.
			const ftcore::BeatBossEvents None;
			Choreo.observe(Fight, None, Now);
		}
		Choreo.sampleNow(Sample);
	}
	else
	{
		Sample = ftcore::ChoreoSample{};
	}

	// The two fighters play what the choreography says, or their stance when it says nothing.
	const auto Pose = [&](USkeletalMeshComponent* Mesh, const ftcore::ChoreoPose& P, int32 StanceClip)
	{
		UFTBeatFighterAnimInstance* Anim = Mesh ? Cast<UFTBeatFighterAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
		if (!Anim)
		{
			return;
		}
		int32 Clip = bScene ? static_cast<int32>(P.clip) : -1;
		float Time = static_cast<float>(P.time);
		if (Clip < 0)
		{
			Clip = StanceClip;
			const float Length = Clip >= 0 ? static_cast<float>(Clips[static_cast<size_t>(Clip)].length) : 1.f;
			Time = FMath::Fmod(static_cast<float>(SceneWall), FMath::Max(0.1f, Length));
		}
		if (Clip < 0 || Clip >= ClipAssets.Num() || !ClipAssets[Clip])
		{
			return;
		}
		Anim->SetPose(ClipAssets[Clip], Time, Clips[static_cast<size_t>(Clip)].loop, DeltaSeconds);
	};
	Pose(LeadMesh.Get(), Sample.lead, StanceLeadClip);
	Pose(BossMesh.Get(), Sample.boss, StanceBossClip);

	// A few frames in, once the stance has posed the bodies: turn each to face the other.
	if (!bFaced && ++SceneFrames >= 6)
	{
		bFaced = true;
		CalibrateFacing(LeadMesh.Get(), 0.0);
		CalibrateFacing(BossMesh.Get(), 180.0);
	}
	BossHeadWorld = AnchorWorld(ftcore::ChoreoAnchor::BossHead) + FVector(0.0, 0.0, 30.0);
	UpdatePrompts();
	UpdateBossBody(DeltaSeconds);
	// The frozen frame of a hit-stop springs when it lets go of a blow.
	if (bStopLast && !Sample.hitStop && (Now - GetPulse(EPulse::BossHit) < 0.4 || Now - GetPulse(EPulse::LeadHit) < 0.4))
	{
		JoltStart = Now;
	}
	bStopLast = Sample.hitStop;
	for (int32 I = Puffs.Num() - 1; I >= 0; --I)
	{
		if (UNiagaraComponent* Puff = Puffs[I].Get(); Puff && Puff->IsActive())
		{
			Puff->SetCustomTimeDilation(FMath::Max(0.001f, Sample.visualRate));
		}
		else
		{
			Puffs.RemoveAtSwap(I, EAllowShrinking::No);
		}
	}
	UpdateCamera(DeltaSeconds);

	// The boss's hide: dark red; a faint lift for a moment when a blow lands (less, and slower, under the flash options),
	// amber while it is staggered; its eyes burn up as its next blow comes.
	if (BossMaterials.Num() > 0)
	{
		float Dazed = 0.f;
		const double Since = Now - GetPulse(EPulse::Stagger);
		if (Since >= 0.0 && Since <= 1.6)
		{
			Dazed = static_cast<float>(FMath::Min3(1.0, Since / 0.2, (1.6 - Since) / 0.3)) * 0.8f;
		}
		float Struck = 0.f;
		const double Age = Now - GetPulse(EPulse::BossHit);
		const float Peak = FlashLevel >= 2 ? 0.f : FlashLevel == 1 ? 0.05f : 0.12f;
		const float Fall = FlashLevel >= 1 ? 0.3f : 0.14f;
		if (Age >= 0.0 && Age < Fall + 0.05)
		{
			Struck = Peak * (Age < 0.05 ? 1.f : 1.f - static_cast<float>((Age - 0.05) / Fall));
		}
		float Eyes = 1.f;
		for (int32 I = 0; I < PromptViewCount; ++I)
		{
			if (PromptViews[I].Anchor != ftcore::ChoreoAnchor::LeadHead && PromptViews[I].Anchor != ftcore::ChoreoAnchor::LeadChest && PromptViews[I].Answer != ftcore::BeatBossAnswer::Attack)
			{
				Eyes = FMath::Max(Eyes, 1.f + 1.5f * PromptViews[I].Progress);
			}
		}
		if (FMath::Abs(Dazed - BossDazedLast) > 0.004f || FMath::Abs(Struck - BossStruckLast) > 0.004f || FMath::Abs(Eyes - BossEyesLast) > 0.01f)
		{
			BossDazedLast = Dazed;
			BossStruckLast = Struck;
			BossEyesLast = Eyes;
			for (int32 I = 0; I < BossMaterials.Num(); ++I)
			{
				const bool bEye = BossEmissive[I].R > 0.f;
				FLinearColor Base = FMath::Lerp(BossBase[I], kBossDazed, bEye ? 0.f : Dazed);
				FLinearColor Glow = bEye ? BossEmissive[I] * Eyes : BossEmissive[I] + kBossStruck * (Struck * 0.5f);
				Base = FMath::Lerp(Base, kBossStruck, Struck * 0.4f);
				BossMaterials[I]->SetVectorParameterValue(TEXT("BaseColor"), Base);
				BossMaterials[I]->SetVectorParameterValue(TEXT("Emissive"), Glow);
			}
		}
	}
}

bool AFTBeatBossGameMode::Shots()
{
	if (ShotPrefix.IsEmpty() || Stage == EStage::Waiting)
	{
		return false;
	}
	const double Wall = UFTPauseSubsystem::WallSeconds();
	if (Wall < ShotHoldUntil)
	{
		return true;
	}
	if ((ShotsTaken & (1u << ViewResult)) != 0)
	{
		// The result is the last view. Views the track never gave are named, not waited for.
		UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: %d views taken (mask %u); quitting."), ShotCount, ShotsTaken);
		ShotPrefix.Reset();
		Quit(0);
		return true;
	}

	const auto Want = [this](int32 Index) { return (ShotsTaken & (1u << Index)) == 0; };
	if (Stage == EStage::Playing)
	{
		// The lead-in's count, with a numeral or two shown.
		if (Want(ViewCountIn) && Fight.counting() && Fight.stats().bars == 0 && Now - BeginTime > 0.9)
		{
			TakeShot(ViewCountIn);
			return true;
		}
		if (Fight.trackRunning())
		{
			// Each of the eight shots, once its cut has settled (with a prompt on it, but the orbit's).
			for (int32 K = 0; K < 8; ++K)
			{
				const ftcore::ChoreoShot Which = static_cast<ftcore::ChoreoShot>(K);
				if (Want(ViewShot0 + K) && Sample.shot == Which && CameraShot == Which && Sample.sinceCut > 0.55 && Sample.sinceCut < 1.8
					&& (Which == ftcore::ChoreoShot::FinisherOrbit || Sample.promptCount > 0))
				{
					TakeShot(ViewShot0 + K);
					return true;
				}
			}
			// A prompt of each answer, its ring part of the way in.
			for (int32 I = 0; I < PromptViewCount; ++I)
			{
				const FPromptView& V = PromptViews[I];
				const int32 A = static_cast<int32>(V.Answer);
				if (!V.bFinisher && A >= 0 && A < 8 && Want(ViewAnswer0 + A) && V.Progress >= 0.5f && V.Progress <= 0.72f)
				{
					TakeShot(ViewAnswer0 + A);
					return true;
				}
			}
			// The lead's strike landing: the hit-stop is on the blow, and the boss has been struck.
			if (Want(ViewStrike) && Sample.hitStop && Now - GetPulse(EPulse::BossHit) < 0.4 && Now - GetPulse(EPulse::BossHit) >= 0.0
				&& (LeadMove == ftcore::BeatBossAnswer::Attack || LeadMove == ftcore::BeatBossAnswer::Hold))
			{
				TakeShot(ViewStrike);
				return true;
			}
			// The boss's stagger, in its recoil.
			if (Want(ViewStagger) && Now - GetPulse(EPulse::Stagger) >= 0.3 && Now - GetPulse(EPulse::Stagger) < 0.8)
			{
				TakeShot(ViewStagger);
				return true;
			}
			// The finisher: three strikes told at once, the orbit a little way round.
			if (Want(ViewFinisher) && Sample.shot == ftcore::ChoreoShot::FinisherOrbit && Sample.sinceCut > 0.9 && PromptViewCount >= 2)
			{
				TakeShot(ViewFinisher);
				return true;
			}
		}
		// A scripted silence, the boss roaring in it.
		if (Want(ViewSilence) && Fight.silent() && Now - GetPulse(EPulse::Silence) >= 0.6)
		{
			TakeShot(ViewSilence);
			return true;
		}
	}
	// -BeatBossMissPhase: the retry prompt after the lost phase.
	if (Want(ViewRetry) && MissPhase > 0 && Stage == EStage::RetryPrompt && Now - PromptTime >= 0.6)
	{
		TakeShot(ViewRetry);
		return true;
	}
	// The win: the final blow frozen, then the ramp up, then the result.
	if (Stage == EStage::Result && Fight.state() == ftcore::BeatBossState::Victory)
	{
		if (Want(ViewFreeze) && GetResultAge() >= 0.1)
		{
			TakeShot(ViewFreeze);
			return true;
		}
		if (Want(ViewRamp1) && GetResultAge() >= 0.65)
		{
			TakeShot(ViewRamp1);
			return true;
		}
		if (Want(ViewRamp2) && GetResultAge() >= 1.4)
		{
			TakeShot(ViewRamp2);
			return true;
		}
		if (Want(ViewResult) && GetResultAge() >= 2.4)
		{
			TakeShot(ViewResult);
			return true;
		}
	}
	return false;
}

void AFTBeatBossGameMode::TakeShot(int32 Index)
{
	const FString File = FString::Printf(TEXT("%s_%s.png"), *ShotPrefix, kBossViewNames[Index]);
	// The game viewport handles HighResShot; the engine's own Exec doesn't.
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *File), *GLog);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: view %s."), *File);
	ShotsTaken |= 1u << Index;
	++ShotCount;
	ShotHoldUntil = UFTPauseSubsystem::WallSeconds() + kBossShotHold;
}

bool AFTBeatBossGameMode::Sequence()
{
	if (SeqPrefix.IsEmpty() || Stage == EStage::Waiting)
	{
		return false;
	}
	const double Wall = UFTPauseSubsystem::WallSeconds();
	if (Wall < SeqHoldUntil)
	{
		return true;
	}
	if (SeqTaken >= SeqCount)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: %d sequence frames taken; quitting."), SeqTaken);
		SeqPrefix.Reset();
		Quit(0);
		return true;
	}
	if (Stage != EStage::Playing || !Fight.trackRunning())
	{
		return false;
	}
	if (!bSeqRunning)
	{
		// From the first parry's tell: the lead meets the boss's blow, then answers it.
		for (int32 I = 0; I < PromptViewCount; ++I)
		{
			if (PromptViews[I].Answer == ftcore::BeatBossAnswer::Parry && PromptViews[I].Progress < 0.2f)
			{
				bSeqRunning = true;
				SeqNextClock = Now;
			}
		}
	}
	if (bSeqRunning && Now >= SeqNextClock - 1e-9)
	{
		const FString File = FString::Printf(TEXT("%s_%04d.png"), *SeqPrefix, SeqTaken);
		if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
		{
			Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot 1280x720 filename=\"%s\""), *File), *GLog);
		}
		++SeqTaken;
		SeqNextClock += kBossSeqStep;
		SeqHoldUntil = Wall + 0.3;
		return true;
	}
	return false;
}
