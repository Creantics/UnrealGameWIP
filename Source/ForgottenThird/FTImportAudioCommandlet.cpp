#include "FTImportAudioCommandlet.h"

#include "ForgottenThird.h"
#include "FTAudioSubsystem.h"

#if WITH_EDITOR
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "FileHelpers.h"
#include "FTText.h"
#include "IAssetTools.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Sound/SoundWave.h"
#include "ftcore/Loudness.h"
#include "ftcore/Wav.h"

#include <string>
#include <vector>

namespace
{
const TCHAR* const GTag = TEXT("FTImportAudio");

// Decodes, measures and checks one file. False when it can't be read or breaks a rule.
bool CheckClipFile(const FString& Path, const ftcore::AudioCue& Cue)
{
	const FString Id = FT::ToFString(Cue.id);
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
	const ftcore::Loudness Loud = ftcore::measureLoudness(Samples.data(), Info.frames, Info.channels, Info.sampleRate);
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: %u Hz, %u ch, %u-bit%s, %.2f s, %s LUFS, peak %.1f dBFS, true peak %.1f dBTP%s."),
		GTag, *Id, Info.sampleRate, Info.channels, Info.bitsPerSample, Info.isFloat ? TEXT(" float") : TEXT(""),
		Info.durationSeconds(), Loud.hasIntegrated ? *FString::Printf(TEXT("%.1f"), Loud.integratedLufs) : TEXT("n/a"),
		Loud.samplePeakDb, Loud.truePeakDb, Info.hasLoop ? TEXT(", smpl loop") : TEXT(""));

	const std::vector<ftcore::AssetIssue> Issues = ftcore::checkClip(Info, Loud, Cue.bus, &Cue);
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
	return ftcore::countErrors(Issues) == 0;
}

// Imports one checked file as /Game/Audio/<Bus>/<Id> and sets its looping. False on failure.
bool ImportClip(const FString& Path, const ftcore::AudioCue& Cue)
{
	const FString Id = FT::ToFString(Cue.id);
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = Path;
	Task->DestinationPath = UFTAudioSubsystem::AssetFolderFor(Cue);
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
	Wave->bLooping = Cue.playback == ftcore::CuePlayback::Loop;
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
		if (!CheckClipFile(Path, Cue))
		{
			++Failed;
			continue;
		}
		if (!bCheckOnly)
		{
			if (ImportClip(Path, Cue))
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
