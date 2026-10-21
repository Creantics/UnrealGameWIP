#pragma once

// Plays the story's music and sound cues. It listens to UFTStorySubsystem::OnCue and looks each cue
// id up in Data/Audio/cues.json (ftcore::AudioCueTable), then plays the imported sound at
// /Game/Audio/<Bus>/<CueId> (FTImportAudio puts it there):
//
//   music loop      Play cross-fades from the current music to this one (the cue's fadeOut / fadeIn);
//                   Stop, or a cue id ending in -NONE, fades the current music out.
//   music stinger / oneshot, sfx oneshot   plays once over everything (Stop does nothing).
//   ambience loop   held until a Stop for its id; several can run at once.
//
// Volume is master x the bus's setting (UFTSettingsSubsystem; sfx and ambience share the sfx
// slider) x the cue's gainDb; music and ambience follow the sliders as they move. Sounds persist across
// map changes (story <-> battle <-> field), so the music carries over a hand-off.
//
// Voice: on every story line it looks for /Game/Voice/<LineId> (FTImportAudio -Voice imports
// Data/Voice/<LineId>.wav there) and plays it on the voice slider, stopping the previous line's voice
// when the beat moves on. OnVoiceLine then fires with the line id and the clip's length, and
// VoiceTrackFile gives the line's lip-sync track (Data/Voice/<LineId>.nface) for UFTFaceComponent::
// PlayTrack. Voice files speak the story, so Data/Voice and Content/Voice stay out of git.
//
// Until the audio is delivered nearly every cue has no sound: a cue missing from the manifest or
// without an imported asset is silent and logged once at Display (never a warning, so headless runs
// stay clean). Without an audio device (-nosound) nothing plays but the bookkeeping still runs.
// Logs carry cue and line ids only.

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"

#include "ftcore/AudioCues.h"

#include "FTStorySubsystem.h"

#include "FTAudioSubsystem.generated.h"

class UAudioComponent;
class USoundBase;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFTVoiceLineEvent, const FString&, LineId, float, Seconds);

UCLASS()
class FORGOTTENTHIRD_API UFTAudioSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Data/Audio/cues.json, read into Out. False (and OutError) when it is missing or invalid.
	static bool LoadCueTable(ftcore::AudioCueTable& Out, FString& OutError);
	// "/Game/Audio/<Bus>"
	static FString AssetFolderFor(const ftcore::AudioCue& Cue);
	// "/Game/Audio/<Bus>/<Id>.<Id>"
	static FString AssetPathFor(const ftcore::AudioCue& Cue);

	// Plays, stops or fires a cue as the story would. Unknown ids are ignored (logged once).
	void HandleCue(const FString& CueId, EFTCueAction Action);

	// "/Game/Voice/<LineId>.<LineId>"
	static FString VoiceAssetPath(const FString& LineId);
	// Data/Voice/<LineId>.nface when that file exists, else "".
	static FString VoiceTrackFile(const FString& LineId);

	// Fires when a line's voice starts.
	UPROPERTY(BlueprintAssignable, Category = "Audio")
	FFTVoiceLineEvent OnVoiceLine;
	// The line whose voice is playing ("" when none).
	const FString& GetVoiceLine() const { return VoiceLineId; }
	bool IsVoicePlaying() const;

	// The music cue playing now ("" when none).
	const FString& GetCurrentMusic() const { return CurrentMusicId; }
	bool IsManifestLoaded() const { return bManifestLoaded; }

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override;

private:
	UFUNCTION()
	void HandleStoryCue(const FString& CueId, const FString& Kind, EFTCueAction Action);
	UFUNCTION()
	void HandleBeat(EFTStoryState State);
	void StopVoice();

	// The cue's sound, or null (logged once) when it isn't imported.
	USoundBase* FindSound(const ftcore::AudioCue& Cue);
	// master x bus x gain for the cue
	float VolumeFor(const ftcore::AudioCue& Cue) const;
	UAudioComponent* Spawn(const ftcore::AudioCue& Cue, USoundBase* Sound, bool bAutoDestroy);
	void StopMusic(float FadeSeconds);
	void LogOnce(const FString& CueId, const TCHAR* Why);

	ftcore::AudioCueTable Cues;
	bool bManifestLoaded = false;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<USoundBase>> Sounds;
	TSet<FString> Logged;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> Music;
	FString CurrentMusicId;
	float CurrentMusicFadeOut = 0.f;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UAudioComponent>> Ambience;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> Voice;
	FString VoiceLineId;
	bool bLoggedNoVoice = false;

	uint32 SettingsRevision = MAX_uint32;
};
