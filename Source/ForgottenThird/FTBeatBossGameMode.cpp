#include "FTBeatBossGameMode.h"

#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Camera/CameraActor.h"
#include "Components/AudioComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTActionButtons.h"
#include "FTBeatBossHUD.h"
#include "FTBeatBossMetronome.h"
#include "FTConductorComponent.h"
#include "FTGameInstance.h"
#include "FTPauseSubsystem.h"
#include "FTSceneLook.h"
#include "FTSettingsSubsystem.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include <string>
#include <vector>

namespace
{
// The stage, centimetres, x from the lead towards the boss, z up (the floor's top at 0).
constexpr double kBossX = 640.0;
const FVector kBossViewer(-850.0, 320.0, 430.0);
const FVector kBossViewTarget(300.0, -40.0, 110.0);
// The HUD hangs the boss's name plate (its hp) this far over its body's top.
constexpr double kBossAnchorLift = 70.0;

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
constexpr int32 kBossShotViews = 5;
// -BeatBossMissPhase adds a sixth, the retry prompt.
constexpr int32 kBossShotViewsMiss = 6;

const TCHAR* const kBossHumanMesh = TEXT("/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_human.test_human");
const TCHAR* const kBossHumanIdle = TEXT("/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_humanidle.test_humanidle");

// The boss's body colour at rest, wound up, struck and staggered.
const FLinearColor kBossRest(0.16f, 0.15f, 0.21f);
const FLinearColor kBossWound(0.95f, 0.07f, 0.03f);
const FLinearColor kBossStruck(2.2f, 2.2f, 2.2f);
const FLinearColor kBossDazed(0.75f, 0.6f, 0.1f);

// Retry: Enter, Space or the pad's A; give up: Q, Backspace or the pad's B.
bool BossYesPressed(const APlayerController& PC)
{
	return PC.WasInputKeyJustPressed(EKeys::Enter) || PC.WasInputKeyJustPressed(EKeys::SpaceBar)
		|| PC.WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Bottom);
}

bool BossNoPressed(const APlayerController& PC)
{
	return PC.WasInputKeyJustPressed(EKeys::Q) || PC.WasInputKeyJustPressed(EKeys::BackSpace)
		|| PC.WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Right);
}

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

// 0 -> 1 -> 0 over Length seconds from Since (0 outside it).
float BossBump(double Now, double Since, double Length)
{
	const double T = Now - Since;
	if (T < 0.0 || T > Length)
	{
		return 0.f;
	}
	return static_cast<float>(FMath::Sin(PI * T / Length));
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

FVector AFTBeatBossGameMode::BossLocation()
{
	return FVector(kBossX, 0.0, BossHeight * 0.5);
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

FVector AFTBeatBossGameMode::GetBossAnchor() const
{
	return BossHome + FVector(0.0, 0.0, BossHeight * 0.5 + kBossAnchorLift);
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
	bIdle = FParse::Param(Cmd, TEXT("BeatBossIdle"));
	int32 Miss = 0;
	if (!bIdle && FParse::Value(Cmd, TEXT("BeatBossMissPhase="), Miss) && Miss > 0)
	{
		MissPhase = Miss;
	}
	bAutoPlay = !bIdle && (FParse::Param(Cmd, TEXT("BeatBossAutoPlay")) || !ShotPrefix.IsEmpty() || MissPhase > 0);
	bQuitOnFinish = FParse::Param(Cmd, TEXT("BeatBossQuitOnFinish"));

	FString Id = TEXT("test_track");
	FParse::Value(Cmd, TEXT("BeatBossTrack="), Id);
	if (!LoadTrack(Id))
	{
		Stage = EStage::Done;
		if (bQuitOnFinish || !ShotPrefix.IsEmpty())
		{
			Quit(3);
		}
		return;
	}

	Assist = ftcore::QteAssist();
	if (const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>())
	{
		if (const UFTSettingsSubsystem* SettingsSub = GI->GetSubsystem<UFTSettingsSubsystem>())
		{
			Assist.autoComplete = SettingsSub->Get().qteAutoComplete;
			Assist.holdToggle = SettingsSub->Get().holdToggle;
		}
	}
	if (bAutoPlay)
	{
		Assist.autoComplete = true;
	}
	if (bIdle)
	{
		Assist.autoComplete = false;
	}
	if (MissPhase > 0)
	{
		// The host drives the autopilot itself (Pilot), so it can leave a phase unanswered.
		MissPhase = FMath::Min(MissPhase, static_cast<int32>(Track.def.phaseCount));
		Assist.autoComplete = false;
	}

	FindStage();

	// Look through the map's camera rather than the pawn.
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		for (TActorIterator<ACameraActor> It(GetWorld()); It; ++It)
		{
			PC->SetViewTarget(*It);
			break;
		}
	}

	// The stand-in music: one procedural sound for the whole fight. No audio device (-NullRHI runs may have
	// none) leaves the component null and the triggers go nowhere.
	Metronome = NewObject<UFTBeatBossMetronome>(this);
	MetronomeAudio = UGameplayStatics::SpawnSound2D(this, Metronome, 1.f, 1.f, 0.f, nullptr, false, false);
	if (Track.music.empty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: track %s on the wall clock with the metronome."), *Id);
	}
	else
	{
		// No music cue is played by this demo yet; the clicks and stems stand in for it.
		UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: track %s on the wall clock with the metronome standing in for music cue %s."), *Id,
			*FT::ToFString(Track.music));
	}

	// The fight reads the conductor's clock, so this tick comes after the conductor has taken its frame's.
	AddTickPrerequisiteComponent(Conductor);
}

void AFTBeatBossGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (MetronomeAudio)
	{
		MetronomeAudio->Stop();
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
	return true;
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

	if (AActor* B = Boss.Get())
	{
		BossHome = B->GetActorLocation();
		BossScale = B->GetActorScale3D();
		// One material for the body and its parts (not the visor), so the wind-up reddens all of it.
		if (UMaterialInterface* Surface = FTScene::LoadSurface())
		{
			BossMaterial = UMaterialInstanceDynamic::Create(Surface, this);
			BossMaterial->SetVectorParameterValue(TEXT("Color"), kBossRest);
			TArray<AActor*> Parts;
			B->GetAttachedActors(Parts, true, true);
			Parts.Add(B);
			for (AActor* Part : Parts)
			{
				if (!Part || Part->ActorHasTag(TEXT("BeatBossVisor")))
				{
					continue;
				}
				TArray<UStaticMeshComponent*> Meshes;
				Part->GetComponents(Meshes);
				for (UStaticMeshComponent* Mesh : Meshes)
				{
					Mesh->SetMaterial(0, BossMaterial);
				}
			}
		}
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: the map has no boss (an actor tagged %s; FTBuildBeatBossWorld builds it)."), *BossTag().ToString());
	}

	if (AActor* L = Lead.Get())
	{
		LeadHome = L->GetActorLocation();
		USkeletalMeshComponent* Skin = L->FindComponentByClass<USkeletalMeshComponent>();
		LeadMesh = Skin;
		if (Skin)
		{
			if (!Skin->GetSkeletalMeshAsset())
			{
				if (USkeletalMesh* Human = LoadObject<USkeletalMesh>(nullptr, kBossHumanMesh, nullptr, LOAD_NoWarn | LOAD_Quiet))
				{
					Skin->SetSkeletalMesh(Human);
				}
			}
			if (UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, kBossHumanIdle, nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				Skin->PlayAnimation(Idle, true);
			}
		}
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: the map has no lead (an actor tagged %s; FTBuildBeatBossWorld builds it)."), *LeadTag().ToString());
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
	if ((bQuitOnFinish || !ShotPrefix.IsEmpty()) && Wall - StartWall > kBossWatchdog)
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
	const bool bHold = Shots();
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
		Animate();
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
			Handle(Fight.update(Now));
		}
		Fight.beatLine(Line);
		Sound();
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
	Animate();
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
			Handle(Fight.press(Button, EventTime));
		}
		if (bReleased || (!bNowDown && bDown[D]))
		{
			Handle(Fight.release(Button, EventTime));
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
	const bool bYes = BossYesPressed(*PC);
	const bool bNo = BossNoPressed(*PC);
	switch (Stage)
	{
	case EStage::RetryPrompt:
		if (bYes)
		{
			Retry();
		}
		else if (bNo)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: given up after %d retries."), Retries);
			bGaveUp = true;
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
	Fight.begin(Track.def, Tuning, Now, Assist);
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
	BossBlowTime = -1e9;
	SoundBars = -1;
	SoundPlayhead = 0.0;
	SoundCueCount = 0;
	SoundCueId = 0;
	SoundTrack = Fight.trackTime();
	UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: begin (%d phases, boss %d hp, lead %d hp)%s%s; losing phase %d (0: none)."),
		static_cast<int32>(Track.def.phaseCount), Track.def.bossHp, Track.def.leadHp, bAutoPlay ? TEXT(", autoplay") : TEXT(""),
		bIdle ? TEXT(", idle") : TEXT(""), MissPhase);
}

void AFTBeatBossGameMode::Retry()
{
	const int32 FromBar = Fight.stats().bars;
	Fight.retry(Now);
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
		Handle(A.kind == ftcore::QteAction::Kind::Press ? Fight.press(A.input, A.time) : Fight.release(A.input, A.time));
	}
}

void AFTBeatBossGameMode::Finish()
{
	Stage = EStage::Result;
	ResultTime = Now;
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
	++PopupTotal;
}

void AFTBeatBossGameMode::Handle(const ftcore::BeatBossEvents& Events)
{
	using ftcore::BeatBossEventType;
	for (const ftcore::BeatBossEvent& E : Events)
	{
		switch (E.type)
		{
		case BeatBossEventType::Judged:
			AddPopup(E);
			Pulse(EPulse::Judged, E.time, static_cast<int32>(E.grade));
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
			if (ftcore::isBossCounter(E.answer))
			{
				BossBlowTime = E.time;
			}
			break;
		case BeatBossEventType::BossHit:
			Pulse(EPulse::BossHit, E.time, E.amount);
			break;
		case BeatBossEventType::LeadHit:
			Pulse(EPulse::LeadHit, E.time, E.amount);
			BossBlowTime = E.time;
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

void AFTBeatBossGameMode::Animate()
{
	// The boss winds up as the nearest counter's marker closes in on the hit line.
	float Wind = 0.f;
	if (Stage == EStage::Playing)
	{
		for (size_t I = 0; I < Fight.promptCount(); ++I)
		{
			const ftcore::BeatBossPrompt& P = Fight.prompt(I);
			if (!ftcore::isBossCounter(P.answer))
			{
				continue;
			}
			const double A = Fight.approach(P);
			if (A >= 0.0)
			{
				Wind = FMath::Max(Wind, static_cast<float>(1.0 - A));
			}
		}
	}
	WindUp = Wind;

	if (AActor* B = Boss.Get())
	{
		const float Lunge = BossBump(Now, BossBlowTime, 0.32);
		// Staggered: it sags for the stagger's length, easing in and out.
		const double Since = Now - GetPulse(EPulse::Stagger);
		float Sag = 0.f;
		if (Since >= 0.0 && Since <= 1.6)
		{
			Sag = static_cast<float>(FMath::Min3(1.0, Since / 0.2, (1.6 - Since) / 0.3));
		}
		const bool bBeaten = Stage != EStage::Playing && Fight.state() == ftcore::BeatBossState::Victory;
		if (bBeaten)
		{
			Sag = 1.f;
		}
		const float Wound = Wind * (1.f - Lunge);
		const FVector Location = BossHome + FVector(-150.0 * Lunge, 0.0, -35.0 * Sag - (bBeaten ? 60.0 : 0.0));
		const FRotator Rotation(16.f * Wound - 12.f * Lunge - 18.f * Sag, 180.f, 0.f);
		B->SetActorLocationAndRotation(Location, Rotation);
		B->SetActorScale3D(BossScale * (1.0 + 0.16 * Wind));

		if (BossMaterial)
		{
			FLinearColor Color = FMath::Lerp(kBossRest, kBossWound, Wind);
			Color = FMath::Lerp(Color, kBossDazed, Sag);
			const double Struck = Now - GetPulse(EPulse::BossHit);
			if (Struck >= 0.0 && Struck < 0.16)
			{
				Color = FMath::Lerp(kBossStruck, Color, static_cast<float>(Struck / 0.16));
			}
			BossMaterial->SetVectorParameterValue(TEXT("Color"), Color);
		}
	}

	if (AActor* L = Lead.Get())
	{
		// The lead's move with its last answer, and a recoil when hit.
		const float Move = BossBump(Now, LeadMoveTime, 0.28);
		FVector Offset = FVector::ZeroVector;
		switch (LeadMove)
		{
		case ftcore::BeatBossAnswer::Dodge:
			Offset = FVector(-10.0, -110.0, 0.0) * Move;
			break;
		case ftcore::BeatBossAnswer::Jump:
			Offset = FVector(0.0, 0.0, 70.0) * Move;
			break;
		case ftcore::BeatBossAnswer::Guard:
		case ftcore::BeatBossAnswer::Parry:
			Offset = FVector(-25.0, 0.0, 0.0) * Move;
			break;
		default:
			Offset = FVector(90.0, 0.0, 0.0) * Move;
			break;
		}
		Offset += FVector(-60.0, 0.0, 0.0) * BossBump(Now, GetPulse(EPulse::LeadHit), 0.3);
		L->SetActorLocation(LeadHome + Offset);
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
	const int32 Views = MissPhase > 0 ? kBossShotViewsMiss : kBossShotViews;
	const uint32 All = (1u << Views) - 1u;
	if (ShotsTaken == All || (ShotsTaken & (1u << 4)) != 0)
	{
		if (ShotsTaken != All)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("BeatBoss: only %d of %d views came (mask %u); quitting."), ShotCount, Views, ShotsTaken);
		}
		else
		{
			UE_LOG(LogForgottenThird, Display, TEXT("BeatBoss: views taken; quitting."));
		}
		ShotPrefix.Reset();
		Quit(0);
		return true;
	}

	const auto Want = [this](int32 Index) { return (ShotsTaken & (1u << Index)) == 0; };
	if (Stage == EStage::Playing)
	{
		// The lead-in's count, with a numeral or two shown.
		if (Want(0) && Fight.counting() && Fight.stats().bars == 0 && Now - BeginTime > 0.9)
		{
			TakeShot(0, TEXT("1_countin"));
			return true;
		}
		if (Fight.trackRunning())
		{
			for (size_t I = 0; I < Fight.promptCount(); ++I)
			{
				const ftcore::BeatBossPrompt& P = Fight.prompt(I);
				const double A = Fight.approach(P);
				// A counter's marker on its way in, the boss winding up.
				if (Want(1) && ftcore::isBossCounter(P.answer) && A >= 0.3 && A <= 0.6)
				{
					TakeShot(1, TEXT("2_counter"));
					return true;
				}
				// The Release's marker, open while there is Load.
				if (Want(3) && P.answer == ftcore::BeatBossAnswer::Release && A >= 0.2 && A <= 0.7)
				{
					TakeShot(3, TEXT("4_release"));
					return true;
				}
			}
		}
		// A scripted silence, half a second in.
		if (Want(2) && Fight.silent() && Now - GetPulse(EPulse::Silence) >= 0.5)
		{
			TakeShot(2, TEXT("3_silence"));
			return true;
		}
	}
	// -BeatBossMissPhase: the retry prompt after the lost phase.
	if (Want(5) && MissPhase > 0 && Stage == EStage::RetryPrompt && Now - PromptTime >= 0.6)
	{
		TakeShot(5, TEXT("6_retry"));
		return true;
	}
	if (Want(4) && Stage == EStage::Result && Fight.state() == ftcore::BeatBossState::Victory && GetResultAge() >= 0.6)
	{
		TakeShot(4, TEXT("5_result"));
		return true;
	}
	return false;
}

void AFTBeatBossGameMode::TakeShot(int32 Index, const TCHAR* Name)
{
	const FString File = FString::Printf(TEXT("%s_%s.png"), *ShotPrefix, Name);
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
