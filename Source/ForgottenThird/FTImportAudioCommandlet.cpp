#include "FTImportAudioCommandlet.h"

#include "ForgottenThird.h"
#include "FTAudioSubsystem.h"

#if WITH_EDITOR
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "FileHelpers.h"
#include "FTText.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Sound/SoundWave.h"
#include "ftcore/Face.h"
#include "ftcore/Loudness.h"
#include "ftcore/Wav.h"

#include <string>
#include <vector>

namespace
{
const TCHAR* const GTag = TEXT("FTImportAudio");

void LogIssues(const FString& Id, const std::vector<ftcore::AssetIssue>& Issues)
{
	for (const ftcore::AssetIssue& Issue : Issues)
	{
		if (Issue.severity == ftcore::AssetSeverity::Error)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: %s: %s"), GTag, *Id, *FT::ToFString(Issue.code), *FT::ToFString(Issue.message));
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: %s: %s: %s"), GTag, *Id, *FT::ToFString(Issue.code), *FT::ToFString(Issue.message));
		}
	}
}

// Decodes, measures and checks one file (Cue is null for a voice line). False when it can't be
// read or breaks a rule. OutSeconds gets the clip's length.
bool CheckClipFile(const FString& Path, const FString& Id, ftcore::AudioBus Bus, const ftcore::AudioCue* Cue, double& OutSeconds)
{
	OutSeconds = 0.0;
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: can't read %s."), GTag, *Id, *Path);
		return false;
	}
	ftcore::WavInfo Info;
	std::vector<float> Samples;
	std::string Error;
	const std::string_view View(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
	if (!ftcore::readWav(View, Info, &Samples, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: %s"), GTag, *Id, *FT::ToFString(Error));
		return false;
	}
	OutSeconds = Info.durationSeconds();
	const ftcore::Loudness Loud = ftcore::measureLoudness(Samples.data(), Info.frames, Info.channels, Info.sampleRate);
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: %u Hz, %u ch, %u-bit%s, %.2f s, %s LUFS, peak %.1f dBFS, true peak %.1f dBTP%s."),
		GTag, *Id, Info.sampleRate, Info.channels, Info.bitsPerSample, Info.isFloat ? TEXT(" float") : TEXT(""),
		Info.durationSeconds(), Loud.hasIntegrated ? *FString::Printf(TEXT("%.1f"), Loud.integratedLufs) : TEXT("n/a"),
		Loud.samplePeakDb, Loud.truePeakDb, Info.hasLoop ? TEXT(", smpl loop") : TEXT(""));

	const std::vector<ftcore::AssetIssue> Issues = ftcore::checkClip(Info, Loud, Bus, Cue);
	LogIssues(Id, Issues);
	return ftcore::countErrors(Issues) == 0;
}

// The line's lip-sync track, when there is one: it must parse and match the audio's length (a
// mismatch is a warning). False when it doesn't parse. bOutFound says whether the file is there.
bool CheckLipSyncFile(const FString& Id, double AudioSeconds, bool& bOutFound)
{
	const FString Path = UFTAudioSubsystem::VoiceTrackFile(Id);
	bOutFound = !Path.IsEmpty();
	if (!bOutFound)
	{
		return true;
	}
	TArray<uint8> Bytes;
	ftcore::FaceTrack Track;
	std::string Error;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		Error = "can't read the file";
	}
	if (!Error.empty() ||
		!ftcore::parseFaceTrack(std::span<const std::uint8_t>(Bytes.GetData(), static_cast<size_t>(Bytes.Num())), Track, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: bad lip-sync track: %s"), GTag, *Id, *FT::ToFString(Error));
		return false;
	}
	std::vector<ftcore::AssetIssue> Issues;
	ftcore::checkLipSyncLength(AudioSeconds, Track.duration(), Issues);
	LogIssues(Id, Issues);
	return true;
}

// Imports one checked file as <Folder>/<Name> and sets its looping. False on failure.
bool ImportClip(const FString& Path, const FString& Folder, const FString& Id, bool bLoop)
{
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = Path;
	Task->DestinationPath = Folder;
	Task->DestinationName = Id;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bSave = false;
	Task->bAsync = false;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.ImportAssetTasks({ Task });

	USoundWave* Wave = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		if (!Wave)
		{
			Wave = Cast<USoundWave>(Object);
		}
	}
	if (!Wave)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: no sound wave came out of %s."), GTag, *Id, *Path);
		return false;
	}
	Wave->bLooping = bLoop;
	Wave->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages({ Wave->GetPackage() }, false))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: couldn't save %s."), GTag, *Id, *Wave->GetPathName());
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: imported %s (%s)."), GTag, *Id, *Wave->GetPathName(),
		Wave->bLooping ? TEXT("looping") : TEXT("once"));
	return true;
}
// -Voice: every Data/Voice/<LineId>.wav (or just Only) onto /Game/Voice/<LineId>.
int32 ImportVoice(const FString& Only, bool bCheckOnly)
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data/Voice"));
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.wav")), true, false);
	Files.Sort();
	int32 Found = 0, Imported = 0, Failed = 0, NoTrack = 0;
	for (const FString& File : Files)
	{
		const FString Id = FPaths::GetBaseFilename(File);
		if (!Only.IsEmpty() && Id != Only)
		{
			continue;
		}
		++Found;
		const FString Path = Dir / File;
		double Seconds = 0.0;
		bool bTrack = false;
		const bool bClipOk = CheckClipFile(Path, Id, ftcore::AudioBus::Voice, nullptr, Seconds);
		const bool bTrackOk = CheckLipSyncFile(Id, Seconds, bTrack);
		NoTrack += bTrack ? 0 : 1;
		if (!bClipOk || !bTrackOk)
		{
			++Failed;
			continue;
		}
		if (!bCheckOnly)
		{
			if (ImportClip(Path, TEXT("/Game/Voice"), Id, false))
			{
				++Imported;
			}
			else
			{
				++Failed;
			}
		}
	}
	if (!Only.IsEmpty() && Found == 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: no Data/Voice/%s.wav."), GTag, *Only);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %d voice lines found, %d imported, %d without a lip-sync track, %d failed%s."), GTag,
		Found, Imported, NoTrack, Failed, bCheckOnly ? TEXT(" (check only)") : TEXT(""));
	return Failed == 0 ? 0 : 1;
}
} // namespace
#endif

UFTImportAudioCommandlet::UFTImportAudioCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTImportAudioCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString Only;
	FParse::Value(*Params, TEXT("Cue="), Only);
	const bool bCheckOnly = FParse::Param(*Params, TEXT("Check"));
	if (FParse::Param(*Params, TEXT("Voice")))
	{
		return ImportVoice(Only, bCheckOnly);
	}

	ftcore::AudioCueTable Cues;
	FString Error;
	if (!UFTAudioSubsystem::LoadCueTable(Cues, Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s"), GTag, *Error);
		return 1;
	}
	if (!Only.IsEmpty() && !Cues.find(FT::ToUtf8(Only)))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: cue %s isn't in Data/Audio/cues.json."), GTag, *Only);
		return 1;
	}

	const FString Root = FPaths::ProjectDir() / TEXT("Data/Audio");
	int32 Found = 0, Imported = 0, Failed = 0;
	TArray<FString> Missing;
	for (const ftcore::AudioCue& Cue : Cues.cues)
	{
		const FString Id = FT::ToFString(Cue.id);
		if (!Only.IsEmpty() && Id != Only)
		{
			continue;
		}
		const FString Path = FPaths::ConvertRelativePathToFull(Root / FT::ToFString(Cue.file));
		if (!FPaths::FileExists(Path))
		{
			Missing.Add(Id);
			continue;
		}
		++Found;
		double Seconds = 0.0;
		if (!CheckClipFile(Path, Id, Cue.bus, &Cue, Seconds))
		{
			++Failed;
			continue;
		}
		if (!bCheckOnly)
		{
			if (ImportClip(Path, UFTAudioSubsystem::AssetFolderFor(Cue), Id, Cue.playback == ftcore::CuePlayback::Loop))
			{
				++Imported;
			}
			else
			{
				++Failed;
			}
		}
	}
	if (!Missing.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s: no file yet for %d cues: %s"), GTag, Missing.Num(), *FString::Join(Missing, TEXT(" ")));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %d cues, %d files found, %d imported, %d missing, %d failed%s."), GTag,
		static_cast<int32>(Cues.cues.size()), Found, Imported, Missing.Num(), Failed, bCheckOnly ? TEXT(" (check only)") : TEXT(""));
	return Failed == 0 ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTImportAudio needs an editor build."));
	return 1;
#endif
}
