#include "FTAudioSubsystem.h"

#include "Components/AudioComponent.h"
#include "ForgottenThird.h"
#include "FTSettingsSubsystem.h"
#include "FTText.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"

namespace
{
bool IsSilenceCue(const FString& CueId)
{
	return CueId.EndsWith(TEXT("-NONE"));
}
} // namespace

void UFTAudioSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UFTSettingsSubsystem>();
	UFTStorySubsystem* Story = Collection.InitializeDependency<UFTStorySubsystem>();

	FString Error;
	bManifestLoaded = LoadCueTable(Cues, Error);
	if (bManifestLoaded)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Audio: %d cues in the manifest."), static_cast<int32>(Cues.cues.size()));
	}
	else
	{
		// Music is optional for every screen: report it and carry on silent.
		UE_LOG(LogForgottenThird, Display, TEXT("Audio: no cue manifest (%s); the story plays silent."), *Error);
	}
	if (Story)
	{
		Story->OnCue.AddDynamic(this, &UFTAudioSubsystem::HandleStoryCue);
		Story->OnBeatChanged.AddDynamic(this, &UFTAudioSubsystem::HandleBeat);
	}
}

void UFTAudioSubsystem::Deinitialize()
{
	if (UFTStorySubsystem* Story = GetGameInstance()->GetSubsystem<UFTStorySubsystem>())
	{
		Story->OnCue.RemoveDynamic(this, &UFTAudioSubsystem::HandleStoryCue);
		Story->OnBeatChanged.RemoveDynamic(this, &UFTAudioSubsystem::HandleBeat);
	}
	StopVoice();
	StopMusic(0.f);
	for (const TPair<FString, TObjectPtr<UAudioComponent>>& Pair : Ambience)
	{
		if (Pair.Value)
		{
			Pair.Value->Stop();
		}
	}
	Ambience.Reset();
	Super::Deinitialize();
}

bool UFTAudioSubsystem::LoadCueTable(ftcore::AudioCueTable& Out, FString& OutError)
{
	const FString Path = FPaths::ProjectDir() / TEXT("Data/Audio/cues.json");
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
	{
		OutError = FString::Printf(TEXT("can't read %s"), *Path);
		return false;
	}
	std::string Error;
	if (!ftcore::importAudioCues(FT::BytesAsText(Bytes), Out, &Error))
	{
		OutError = FT::ToFString(Error).TrimEnd();
		return false;
	}
	return true;
}

FString UFTAudioSubsystem::AssetFolderFor(const ftcore::AudioCue& Cue)
{
	return FString(TEXT("/Game/Audio/")) + ANSI_TO_TCHAR(ftcore::audioBusFolder(Cue.bus));
}

FString UFTAudioSubsystem::AssetPathFor(const ftcore::AudioCue& Cue)
{
	const FString Id = FT::ToFString(Cue.id);
	return AssetFolderFor(Cue) / Id + TEXT(".") + Id;
}

FString UFTAudioSubsystem::VoiceAssetPath(const FString& LineId)
{
	return FString(TEXT("/Game/Voice/")) + LineId + TEXT(".") + LineId;
}

FString UFTAudioSubsystem::VoiceTrackFile(const FString& LineId)
{
	const FString Path = FPaths::ProjectDir() / TEXT("Data/Voice") / LineId + TEXT(".nface");
	return FPaths::FileExists(Path) ? Path : FString();
}

bool UFTAudioSubsystem::IsVoicePlaying() const
{
	return Voice && Voice->IsPlaying();
}

void UFTAudioSubsystem::HandleBeat(EFTStoryState State)
{
	StopVoice();
	if (State != EFTStoryState::Line)
	{
		return;
	}
	const UFTStorySubsystem* Story = GetGameInstance()->GetSubsystem<UFTStorySubsystem>();
	const FString LineId = Story ? Story->GetLineId() : FString();
	if (LineId.IsEmpty())
	{
		return;
	}
	USoundBase* Sound = LoadObject<USoundBase>(nullptr, *VoiceAssetPath(LineId), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Sound)
	{
		if (!bLoggedNoVoice)
		{
			bLoggedNoVoice = true;
			UE_LOG(LogForgottenThird, Display, TEXT("Audio: line %s has no imported voice; lines without one show text only."), *LineId);
		}
		return;
	}
	float Volume = 1.f;
	if (const UFTSettingsSubsystem* Settings = GetGameInstance()->GetSubsystem<UFTSettingsSubsystem>())
	{
		Volume = Settings->Get().master * Settings->Get().voice;
	}
	// A line belongs to its beat: it doesn't persist across a map change.
	Voice = UGameplayStatics::CreateSound2D(GetGameInstance(), Sound, Volume, 1.f, 0.f, nullptr, false, true);
	if (Voice)
	{
		Voice->Play();
	}
	VoiceLineId = LineId;
	OnVoiceLine.Broadcast(LineId, Sound->GetDuration());
}

void UFTAudioSubsystem::StopVoice()
{
	if (Voice)
	{
		Voice->Stop();
	}
	Voice = nullptr;
	if (!VoiceLineId.IsEmpty())
	{
		VoiceLineId.Reset();
		OnVoiceStopped.Broadcast();
	}
}

void UFTAudioSubsystem::HandleStoryCue(const FString& CueId, const FString& Kind, EFTCueAction Action)
{
	HandleCue(CueId, Action);
}

void UFTAudioSubsystem::HandleCue(const FString& CueId, EFTCueAction Action)
{
	if (IsSilenceCue(CueId))
	{
		StopMusic(CurrentMusicFadeOut);
		return;
	}
	const ftcore::AudioCue* Cue = Cues.find(FT::ToUtf8(CueId));
	if (!Cue)
	{
		LogOnce(CueId, TEXT("isn't in Data/Audio/cues.json"));
		return;
	}

	const bool bHeld = Cue->playback == ftcore::CuePlayback::Loop;
	if (Action == EFTCueAction::Stop)
	{
		if (!bHeld)
		{
			return;
		}
		if (Cue->bus == ftcore::AudioBus::Music)
		{
			if (CurrentMusicId == CueId)
			{
				StopMusic(Cue->fadeOutSeconds);
			}
		}
		else if (TObjectPtr<UAudioComponent>* Held = Ambience.Find(CueId))
		{
			if (*Held)
			{
				(*Held)->bAutoDestroy = true;
				(*Held)->FadeOut(Cue->fadeOutSeconds, 0.f);
			}
			Ambience.Remove(CueId);
		}
		return;
	}

	if (bHeld && Cue->bus == ftcore::AudioBus::Music)
	{
		if (CurrentMusicId == CueId)
		{
			return; // already playing
		}
		StopMusic(CurrentMusicFadeOut);
		CurrentMusicId = CueId;
		CurrentMusicFadeOut = Cue->fadeOutSeconds;
		if (USoundBase* Sound = FindSound(*Cue))
		{
			Music = Spawn(*Cue, Sound, false);
		}
		MusicLoop = ftcore::loopRegionFor(*Cue);
		MusicCursor = -1.0;
		if (Music)
		{
			Music->OnAudioPlaybackPercentNative.AddUObject(this, &UFTAudioSubsystem::HandleMusicPercent);
		}
		return;
	}
	if (bHeld)
	{
		if (Ambience.Contains(CueId))
		{
			return;
		}
		USoundBase* Sound = FindSound(*Cue);
		UAudioComponent* Component = Sound ? Spawn(*Cue, Sound, false) : nullptr;
		Ambience.Add(CueId, Component);
		return;
	}
	if (USoundBase* Sound = FindSound(*Cue))
	{
		Spawn(*Cue, Sound, true);
	}
}

USoundBase* UFTAudioSubsystem::FindSound(const ftcore::AudioCue& Cue)
{
	const FString Id = FT::ToFString(Cue.id);
	if (const TObjectPtr<USoundBase>* Found = Sounds.Find(Id))
	{
		return *Found;
	}
	USoundBase* Sound = LoadObject<USoundBase>(nullptr, *AssetPathFor(Cue), nullptr, LOAD_NoWarn | LOAD_Quiet);
	Sounds.Add(Id, Sound); // a miss is cached too, so it's looked up once
	if (!Sound)
	{
		LogOnce(Id, TEXT("has no imported sound yet"));
	}
	return Sound;
}

float UFTAudioSubsystem::VolumeFor(const ftcore::AudioCue& Cue) const
{
	float Bus = 1.f, Master = 1.f;
	if (const UFTSettingsSubsystem* Settings = GetGameInstance()->GetSubsystem<UFTSettingsSubsystem>())
	{
		const ftcore::Settings& S = Settings->Get();
		Master = S.master;
		switch (Cue.bus)
		{
		case ftcore::AudioBus::Music: Bus = S.music; break;
		case ftcore::AudioBus::Voice: Bus = S.voice; break;
		default: Bus = S.sfx; break;
		}
	}
	return Master * Bus * FMath::Pow(10.f, Cue.gainDb / 20.f);
}

UAudioComponent* UFTAudioSubsystem::Spawn(const ftcore::AudioCue& Cue, USoundBase* Sound, bool bAutoDestroy)
{
	// Persist across level transitions: the story, battle and field screens are separate maps.
	UAudioComponent* Component = UGameplayStatics::CreateSound2D(GetGameInstance(), Sound, VolumeFor(Cue), 1.f, 0.f,
		nullptr, true, bAutoDestroy);
	if (!Component)
	{
		return nullptr; // no audio device (-nosound)
	}
	// From the top: offset is the lead-in before beat 0, for the beat grid, not a start time.
	if (Cue.fadeInSeconds > 0.f)
	{
		Component->FadeIn(Cue.fadeInSeconds, 1.f);
	}
	else
	{
		Component->Play();
	}
	return Component;
}

void UFTAudioSubsystem::StopMusic(float FadeSeconds)
{
	if (Music)
	{
		Music->OnAudioPlaybackPercentNative.RemoveAll(this);
		Music->bAutoDestroy = true;
		if (FadeSeconds > 0.f)
		{
			Music->FadeOut(FadeSeconds, 0.f);
		}
		else
		{
			Music->Stop();
		}
	}
	Music = nullptr;
	CurrentMusicId.Reset();
	CurrentMusicFadeOut = 0.f;
	MusicLoop = ftcore::LoopRegion();
	MusicCursor = -1.0;
}

const ftcore::AudioCue* UFTAudioSubsystem::GetMusicCue() const
{
	return Music && !CurrentMusicId.IsEmpty() ? Cues.find(FT::ToUtf8(CurrentMusicId)) : nullptr;
}

void UFTAudioSubsystem::HandleMusicPercent(const UAudioComponent* /*Audio*/, const USoundWave* Wave, const float Percent)
{
	if (Wave)
	{
		MusicCursor = static_cast<double>(Percent) * static_cast<double>(Wave->Duration);
	}
}

void UFTAudioSubsystem::TickMusicLoop()
{
	if (!Music || MusicCursor < 0.0 || !ftcore::loopShouldRestart(MusicLoop, MusicCursor, 0.0))
	{
		return;
	}
	// The cursor is checked once a frame, so the jump lands up to a frame late; landing as far past
	// loopStart as the cursor ran past loopEnd keeps the beat phase.
	Music->Play(ftcore::loopRestartPosition(MusicLoop, MusicCursor));
	MusicCursor = -1.0;
}

void UFTAudioSubsystem::LogOnce(const FString& CueId, const TCHAR* Why)
{
	bool bAlready = false;
	Logged.Add(CueId, &bAlready);
	if (!bAlready)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Audio: cue %s %s; silent."), *CueId, Why);
	}
}

void UFTAudioSubsystem::Tick(float DeltaTime)
{
	TickMusicLoop();
	const UFTSettingsSubsystem* Settings = GetGameInstance()->GetSubsystem<UFTSettingsSubsystem>();
	if (!Settings || Settings->GetRevision() == SettingsRevision)
	{
		return;
	}
	SettingsRevision = Settings->GetRevision();
	// Held sounds follow the sliders; one-shots are short and keep the volume they started with.
	if (Music)
	{
		if (const ftcore::AudioCue* Cue = Cues.find(FT::ToUtf8(CurrentMusicId)))
		{
			Music->SetVolumeMultiplier(VolumeFor(*Cue));
		}
	}
	if (Voice)
	{
		Voice->SetVolumeMultiplier(Settings->Get().master * Settings->Get().voice);
	}
	for (const TPair<FString, TObjectPtr<UAudioComponent>>& Pair : Ambience)
	{
		const ftcore::AudioCue* Cue = Cues.find(FT::ToUtf8(Pair.Key));
		if (Pair.Value && Cue)
		{
			Pair.Value->SetVolumeMultiplier(VolumeFor(*Cue));
		}
	}
}

TStatId UFTAudioSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTAudioSubsystem, STATGROUP_Tickables);
}

ETickableTickType UFTAudioSubsystem::GetTickableTickType() const
{
	return IsTemplate() ? ETickableTickType::Never : ETickableTickType::Always;
}
