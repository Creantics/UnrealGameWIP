#include "FTBeatBossGameMode.h"

#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
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
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
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
// track never gives is left out, with a warning; the run ends after the result.
enum EBossView : int32
{
	ViewCountIn,
	ViewCounter,
	ViewStrike,
	ViewClose,
	ViewOrbit,
	ViewSilence,
	ViewRelease,
	ViewRetry,
	ViewResult,
	ViewCount
};
const TCHAR* const kBossViewNames[ViewCount] = {
	TEXT("1_countin"), TEXT("2_counter"), TEXT("3_strike"), TEXT("4_close"), TEXT("5_orbit"), TEXT("6_silence"), TEXT("7_release"),
	TEXT("8_retry"), TEXT("9_result")};
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
	Trauma = 0.f;
	PromptViewCount = 0;
	for (FAnchorMemo& M : AnchorMemos)
	{
		M = FAnchorMemo{};
	}
	if (Choreo.begun())
	{
		Choreo.begin(ChoreoDef, Clips);
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
			Pulse(EPulse::BossHit, E.time, E.amount);
			// A heavy blow shakes the camera more: a counter's, a hold's or a multiplied strike.
			Shake(0.28f + FMath::Min(0.4f, static_cast<float>(E.amount) * 0.008f));
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
	else if (Music)
	{
		// The music's own sounds (the count, the beats, the moves' accents) come from the driver; the answers' cues from here.
		switch (static_cast<UFTBeatBossMetronome::EVoice>(Which))
		{
		case UFTBeatBossMetronome::EVoice::Good:
			Music->Trigger(ftcore::MusicSound::Good);
			break;
		case UFTBeatBossMetronome::EVoice::Miss:
			Music->Trigger(ftcore::MusicSound::Miss);
			break;
		default:
			break;
		}
	}
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
		for (size_t K = 0; K < Fight.promptCount(); ++K)
		{
			if (Fight.prompt(K).id == P.id)
			{
				V.Button = ButtonFor(P.answer, Fight.prompt(K).button);
				V.Second = Fight.prompt(K).second;
				break;
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
	if (!Cam)
	{
		return;
	}
	const ftcore::ChoreoShot Shot = Sample.shot;
	if (Shot != CameraShot)
	{
		CameraShot = Shot;
		bCameraCut = true;
		if (Shot == ftcore::ChoreoShot::FinisherOrbit)
		{
			OrbitAngle = 0.0;
		}
	}
	// The orbit turns with the animation: the finisher's slow motion slows it, a hit-stop holds it.
	if (Shot == ftcore::ChoreoShot::FinisherOrbit)
	{
		OrbitAngle += 24.0 * static_cast<double>(DeltaSeconds) * static_cast<double>(Sample.visualRate);
	}

	const FVector L = LeadHome;
	const FVector B = BossHome;
	const FVector Mid = (L + B) * 0.5 + FVector(0.0, 0.0, 125.0);
	FVector Pos = Mid;
	FVector Target = Mid;
	float Fov = 60.f;
	switch (Shot)
	{
	case ftcore::ChoreoShot::Wide:
		// Square on to the fight from the side: the lead on the left, the boss on the right.
		Pos = Mid + FVector(-20.0, 430.0, 20.0);
		Target = Mid + FVector(0.0, 0.0, -30.0);
		Fov = 60.f;
		break;
	case ftcore::ChoreoShot::OverLead:
		// Over the lead's shoulder at the boss.
		Pos = L + FVector(-290.0, 105.0, 215.0);
		Target = B + FVector(-15.0, 0.0, 160.0);
		Fov = 50.f;
		break;
	case ftcore::ChoreoShot::OverBoss:
		// Over the boss's shoulder at the lead, from high enough to see past its bulk.
		Pos = B + FVector(280.0, -120.0, 300.0);
		Target = L + FVector(0.0, 0.0, 140.0);
		Fov = 48.f;
		break;
	case ftcore::ChoreoShot::CloseLead:
		Pos = L + FVector(95.0, 125.0, 170.0);
		Target = L + FVector(0.0, 0.0, 150.0);
		Fov = 36.f;
		break;
	case ftcore::ChoreoShot::CloseBoss:
		Pos = B + FVector(-150.0, 140.0, 235.0);
		Target = B + FVector(0.0, 0.0, 200.0);
		Fov = 40.f;
		break;
	case ftcore::ChoreoShot::LowHero:
		// On the floor by the lead's feet, looking up past him at the boss.
		Pos = L + FVector(-110.0, 85.0, 60.0);
		Target = Mid + FVector(30.0, 0.0, 105.0);
		Fov = 62.f;
		break;
	case ftcore::ChoreoShot::TopDown:
		Pos = Mid + FVector(-150.0, 0.0, 640.0);
		Target = Mid + FVector(0.0, 0.0, -125.0);
		Fov = 52.f;
		break;
	case ftcore::ChoreoShot::FinisherOrbit:
	default:
	{
		const double A = FMath::DegreesToRadians(215.0 + OrbitAngle);
		const double R = 520.0 - 8.0 * FMath::Min(OrbitAngle, 30.0);
		Pos = Mid + FVector(FMath::Cos(A) * R, FMath::Sin(A) * R, 110.0);
		Target = Mid + FVector(0.0, 0.0, 20.0);
		Fov = 44.f;
		break;
	}
	}
	// A slow push in over each shot, so a held one is never still.
	const double Push = 1.0 - 0.07 * FMath::Clamp(Sample.sinceCut / 3.0, 0.0, 1.0);
	Pos = Target + (Pos - Target) * Push;

	// The shake of a heavy blow: a short, fast tremor that dies away.
	FRotator Rotation = (Target - Pos).Rotation();
	const double T = SceneWall;
	const double Q = static_cast<double>(Trauma * Trauma);
	if (Q > 0.0)
	{
		Pos += FVector(FMath::Sin(T * 47.0), FMath::Sin(T * 61.0 + 1.3), FMath::Sin(T * 53.0 + 2.1)) * (Q * 7.0);
		Rotation += FRotator(FMath::Sin(T * 57.0) * Q * 1.4, FMath::Sin(T * 43.0 + 0.7) * Q * 1.8, FMath::Sin(T * 67.0 + 2.0) * Q * 1.2);
	}
	Trauma = FMath::Max(0.f, Trauma - kBossTraumaDecay * DeltaSeconds);

	Cam->SetActorLocationAndRotation(Pos, Rotation);
	if (UCameraComponent* Lens = Cam->GetCameraComponent())
	{
		Lens->SetFieldOfView(Fov);
	}
	bCameraCut = false;
}

void AFTBeatBossGameMode::Scene(float DeltaSeconds)
{
	SceneWall = UFTPauseSubsystem::WallSeconds();
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
	UpdateCamera(DeltaSeconds);

	// The boss's hide: dark red; white for a moment when a blow lands, amber while it is staggered.
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
		if (Age >= 0.0 && Age < 0.14)
		{
			Struck = 1.f - static_cast<float>(Age / 0.14);
		}
		if (FMath::Abs(Dazed - BossDazedLast) > 0.004f || FMath::Abs(Struck - BossStruckLast) > 0.004f)
		{
			BossDazedLast = Dazed;
			BossStruckLast = Struck;
			for (int32 I = 0; I < BossMaterials.Num(); ++I)
			{
				const bool bEye = BossEmissive[I].R > 0.f;
				FLinearColor Base = FMath::Lerp(BossBase[I], kBossDazed, bEye ? 0.f : Dazed);
				FLinearColor Glow = BossEmissive[I] + (bEye ? FLinearColor::Black : kBossStruck * Struck);
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
			for (int32 I = 0; I < PromptViewCount; ++I)
			{
				const FPromptView& V = PromptViews[I];
				// A counter's ring on its way in, on the boss's fist.
				if (Want(ViewCounter) && ftcore::isBossCounter(V.Answer) && V.Progress >= 0.55f && V.Progress <= 0.85f
					&& (V.Anchor == ftcore::ChoreoAnchor::BossFistR || V.Anchor == ftcore::ChoreoAnchor::BossFistL))
				{
					TakeShot(ViewCounter);
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
			// A close shot, once its cut has settled.
			if (Want(ViewClose) && (Sample.shot == ftcore::ChoreoShot::CloseBoss || Sample.shot == ftcore::ChoreoShot::CloseLead)
				&& Sample.sinceCut > 0.55 && Sample.promptCount > 0)
			{
				TakeShot(ViewClose);
				return true;
			}
			// The finisher's orbit, a little way round.
			if (Want(ViewOrbit) && Sample.shot == ftcore::ChoreoShot::FinisherOrbit && Sample.sinceCut > 0.9)
			{
				TakeShot(ViewOrbit);
				return true;
			}
			for (size_t I = 0; I < Fight.promptCount(); ++I)
			{
				const ftcore::BeatBossPrompt& P = Fight.prompt(I);
				const double A = Fight.approach(P);
				// The Release's marker, open while there is Load.
				if (Want(ViewRelease) && P.answer == ftcore::BeatBossAnswer::Release && A >= 0.2 && A <= 0.7)
				{
					TakeShot(ViewRelease);
					return true;
				}
			}
		}
		// A scripted silence, half a second in.
		if (Want(ViewSilence) && Fight.silent() && Now - GetPulse(EPulse::Silence) >= 0.5)
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
	if (Want(ViewResult) && Stage == EStage::Result && Fight.state() == ftcore::BeatBossState::Victory && GetResultAge() >= 0.6)
	{
		TakeShot(ViewResult);
		return true;
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
